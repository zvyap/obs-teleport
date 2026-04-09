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

#include <obs-module.h>

struct teleport_effect_state {
	gs_texrender_t *texrender;
	gs_stagesurface_t *stagesurface;
	uint32_t staged_width;
	uint32_t staged_height;
	bool has_staged_data;
};

void *teleport_effect_state_create(void)
{
	return bzalloc(sizeof(struct teleport_effect_state));
}

void teleport_effect_state_destroy(void *state)
{
	struct teleport_effect_state *s = state;
	obs_enter_graphics();
	if (s->texrender)
		gs_texrender_destroy(s->texrender);
	if (s->stagesurface)
		gs_stagesurface_destroy(s->stagesurface);
	obs_leave_graphics();
	bfree(s);
}

// Called on the render thread from filter_effect_video_render.
//
// When capture is non-zero:
//   - Renders the upstream target into an internal texrender (for capture),
//     stages the resulting texture for CPU readback next frame, and draws the
//     captured texture to the current render target (transparent passthrough).
//   - If pixel data from the previously staged frame is available it is
//     returned as a newly bmalloc-allocated BGRA buffer (width*height*4 bytes)
//     that the caller must bfree().  width_out and height_out are set to the
//     dimensions of the returned buffer.
//
// When capture is zero:
//   - obs_source_skip_video_filter is called for a zero-cost passthrough and
//     NULL is returned.
void *teleport_effect_render(void *state, obs_source_t *source,
			     gs_effect_t *effect, int capture,
			     uint32_t *width_out, uint32_t *height_out)
{
	struct teleport_effect_state *s = state;

	obs_source_t *target = obs_filter_get_target(source);
	if (!target) {
		obs_source_skip_video_filter(source);
		return NULL;
	}

	uint32_t width = obs_source_get_width(target);
	uint32_t height = obs_source_get_height(target);

	if (width == 0 || height == 0) {
		obs_source_skip_video_filter(source);
		return NULL;
	}

	if (!capture) {
		obs_source_skip_video_filter(source);
		return NULL;
	}

	// Recreate GPU resources when the source dimensions change.
	if (s->staged_width != width || s->staged_height != height) {
		s->has_staged_data = false;
		if (s->texrender) {
			gs_texrender_destroy(s->texrender);
			s->texrender = NULL;
		}
		if (s->stagesurface) {
			gs_stagesurface_destroy(s->stagesurface);
			s->stagesurface = NULL;
		}
		s->staged_width = width;
		s->staged_height = height;
	}

	// Collect the CPU-accessible pixel data staged during the previous frame
	// (ping-pong: stage in frame N, map in frame N+1 to avoid GPU stalls).
	void *result = NULL;
	if (s->has_staged_data && s->stagesurface) {
		uint8_t *mapped;
		uint32_t linesize;
		if (gs_stagesurface_map(s->stagesurface, &mapped, &linesize)) {
			uint32_t row_bytes = width * 4;
			uint8_t *buf = bmalloc((size_t)row_bytes * height);
			for (uint32_t row = 0; row < height; row++) {
				memcpy(buf + (size_t)row * row_bytes,
				       mapped + (size_t)row * linesize,
				       row_bytes);
			}
			gs_stagesurface_unmap(s->stagesurface);
			*width_out = width;
			*height_out = height;
			result = buf;
		}
		s->has_staged_data = false;
	}

	// Create GPU resources on first use (or after dimension change).
	if (!s->texrender)
		s->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!s->stagesurface)
		s->stagesurface =
			gs_stagesurface_create(width, height, GS_BGRA);

	// Render the upstream source into our capture texrender.
	gs_texrender_reset(s->texrender);
	if (gs_texrender_begin(s->texrender, width, height)) {
		struct vec4 black = {0};
		gs_clear(GS_CLEAR_COLOR, &black, 1.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f,
			 100.0f);
		obs_source_video_render(target);
		gs_texrender_end(s->texrender);

		// Stage the captured texture for CPU readback next frame.
		gs_stage_texture(s->stagesurface,
				 gs_texrender_get_texture(s->texrender));
		s->has_staged_data = true;
	}

	// Passthrough: draw the captured texrender to the current render target.
	gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
	if (tex) {
		gs_effect_t *default_effect =
			obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(
			gs_effect_get_param_by_name(default_effect, "image"),
			tex);
		while (gs_effect_loop(default_effect, "Draw"))
			gs_draw_sprite(tex, 0, width, height);
	}

	return result;
}
