//
// obs-teleport. OBS Studio plugin.
// Copyright (C) 2021-2026 Florian Zwoch <fzwoch@gmail.com>
//
// This file is part of obs-teleport.
//
// obs-teleport is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// obs-teleport is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with obs-teleport. If not, see <http://www.gnu.org/licenses/>.
//

package main

//
// #include <obs-module.h>
// #include <util/platform.h>
//
// extern void* teleport_effect_state_create(void);
// extern void teleport_effect_state_destroy(void *state);
// extern void* teleport_effect_render(void *state, obs_source_t *source, gs_effect_t *effect, int capture, uint32_t *width_out, uint32_t *height_out);
//
import "C"
import (
	"bytes"
	"image"
	"runtime/cgo"
	"time"
	"unsafe"
)

//export filter_effect_get_name
func filter_effect_get_name(type_data C.uintptr_t) *C.char {
	return frontend_effect_str
}

//export filter_effect_create
func filter_effect_create(settings *C.obs_data_t, source *C.obs_source_t) C.uintptr_t {
	h := &teleportFilter{
		done:        make(chan any),
		filter:      source,
		pool:        NewPool(10),
		effectState: unsafe.Pointer(C.teleport_effect_state_create()),
	}

	h.Add(1)
	go filter_loop(h)

	return C.uintptr_t(cgo.NewHandle(h))
}

//export filter_effect_destroy
func filter_effect_destroy(data C.uintptr_t) {
	h := cgo.Handle(data).Value().(*teleportFilter)

	h.done <- nil
	h.Wait()

	close(h.done)

	C.teleport_effect_state_destroy(h.effectState)

	cgo.Handle(data).Delete()
}

//export filter_effect_video_render
func filter_effect_video_render(data C.uintptr_t, effect *C.gs_effect_t) {
	h := cgo.Handle(data).Value().(*teleportFilter)

	capture := C.int(0)
	if h.SenderGetNumConns() > 0 {
		capture = 1
	}

	var width, height C.uint32_t
	buf := C.teleport_effect_render(h.effectState, h.filter, effect, capture, &width, &height)

	if buf == nil {
		return
	}
	defer C.bfree(buf)

	p := &Packet{
		Header: Header{
			Timestamp: uint64(C.os_gettime_ns()),
		},
		ImageBuffer: h.pool.Get().(*bytes.Buffer),
	}

	settings := C.obs_source_get_settings(h.filter)
	p.Quality = int(C.obs_data_get_int(settings, quality_str))
	C.obs_data_release(settings)

	var planes [C.MAX_AV_PLANES]*C.uint8_t
	planes[0] = (*C.uint8_t)(buf)

	p.ToImage(width, height, C.VIDEO_FORMAT_BGRA, planes)
	if p.Image == nil {
		return
	}

	switch p.Image.(type) {
	case *image.RGBA:
		C.video_format_get_parameters(C.VIDEO_CS_SRGB, C.VIDEO_RANGE_FULL, (*C.float)(unsafe.Pointer(&p.ImageHeader.ColorMatrix[0])), (*C.float)(unsafe.Pointer(&p.ImageHeader.ColorRangeMin[0])), (*C.float)(unsafe.Pointer(&p.ImageHeader.ColorRangeMax[0])))
	}

	h.Lock()
	h.queue = append(h.queue, p)

	queueSize := time.Duration(h.queue[len(h.queue)-1].Header.Timestamp - h.queue[0].Header.Timestamp)

	if queueSize > time.Second {
		blog(C.LOG_WARNING, "encoder queue exceeded: "+queueSize.String())
	}
	h.Unlock()

	h.Add(1)
	go func(p *Packet) {
		defer h.Done()

		p.ToJPEG(h.pool)

		h.Lock()
		defer h.Unlock()

		p.DoneProcessing = true

		for len(h.queue) > 0 && h.queue[0].DoneProcessing {
			h.SenderSend(h.queue[0].Buffer)
			h.pool.Put(h.queue[0].ImageBuffer)

			h.queue[0] = nil
			h.queue = h.queue[1:]
		}
	}(p)
}
