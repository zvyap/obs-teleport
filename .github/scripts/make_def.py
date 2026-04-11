"""Generate a MinGW-compatible .def file from a Windows DLL's export table."""
import pefile
import sys


def make_def(dll_path, def_path):
    pe = pefile.PE(dll_path)
    with open(def_path, "w") as f:
        f.write("EXPORTS\n")
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
                if exp.name:
                    f.write("  " + exp.name.decode() + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <dll> <def>", file=sys.stderr)
        sys.exit(1)
    make_def(sys.argv[1], sys.argv[2])
