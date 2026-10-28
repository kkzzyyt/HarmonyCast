"""Reject known Server 2012 startup incompatibilities in a deployed x64 app.

Requires pefile (see requirements-build.txt). This checks the shipped binaries,
not the build machine's DLL search path; it does not replace testing on Server 2012.
"""

import argparse
from pathlib import Path
import sys

import pefile


REQUIRED_FILES = (
    "HarmonyCast.exe", "d3d12.dll", "Qt5Core.dll", "Qt5Gui.dll", "Qt5Widgets.dll",
    "Qt5Network.dll", "Qt5Svg.dll", "Qt5WebSockets.dll", "platforms/qwindows.dll",
    "avcodec-60.dll", "avutil-58.dll", "swscale-7.dll",
    "msvcp140.dll", "vcruntime140.dll", "ucrtbase.dll",
)
WINDOWS10_IMPORTS = {
    "GetDpiForWindow", "GetDpiForSystem", "GetSystemDpiForProcess",
    "GetThreadDpiAwarenessContext", "GetAwarenessFromDpiAwarenessContext",
    "SetThreadDpiAwarenessContext", "SetThreadDescription", "GetThreadDescription",
}


def check_directory(root: Path) -> list:
    errors = [f"Missing runtime file: {name}" for name in REQUIRED_FILES if not (root / name).is_file()]
    for binary in sorted(root.rglob("*")):
        if not binary.is_file() or binary.suffix.lower() not in {".dll", ".exe"}:
            continue
        label = str(binary.relative_to(root))
        try:
            with pefile.PE(str(binary), fast_load=True) as image:
                if image.FILE_HEADER.Machine != 0x8664:
                    errors.append(f"{label}: expected an x64 binary")
                subsystem = (image.OPTIONAL_HEADER.MajorSubsystemVersion, image.OPTIONAL_HEADER.MinorSubsystemVersion)
                if subsystem > (6, 2):
                    errors.append(f"{label}: subsystem {subsystem} requires a newer Windows version")
                image.parse_data_directories(directories=[
                    pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
                    pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"],
                ])
                for attribute in ("DIRECTORY_ENTRY_IMPORT", "DIRECTORY_ENTRY_DELAY_IMPORT"):
                    for dependency in getattr(image, attribute, []):
                        dll = dependency.dll.decode("ascii", errors="replace")
                        if dll.lower() == "d3d12.dll" or dll.lower().startswith("qt6"):
                            errors.append(f"{label}: incompatible dependency {dll}")
                        if attribute == "DIRECTORY_ENTRY_IMPORT":
                            for symbol in dependency.imports:
                                name = (symbol.name or b"").decode("ascii", errors="replace")
                                if name in WINDOWS10_IMPORTS:
                                    errors.append(f"{label}: Windows 10 entry point {dll}!{name}")
                if binary.name.lower() == "d3d12.dll":
                    image.parse_data_directories(directories=[
                        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
                    ])
                    if hasattr(image, "DIRECTORY_ENTRY_EXPORT") and image.DIRECTORY_ENTRY_EXPORT.symbols:
                        export_symbols = {
                            exp.name.decode("ascii", errors="replace")
                            for exp in image.DIRECTORY_ENTRY_EXPORT.symbols if exp.name
                        }
                        required_exports = {"D3D12CreateDevice", "D3D12GetDebugInterface"}
                        missing = required_exports - export_symbols
                        if missing:
                            errors.append(f"{label}: stub is missing expected exports: {', '.join(sorted(missing))}")
        except (pefile.PEFormatError, OSError) as error:
            errors.append(f"{label}: cannot inspect PE image: {error}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Deployed Server 2012 app directory")
    args = parser.parse_args()
    errors = check_directory(args.directory)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Server 2012 runtime gate passed (Qt 5, x64, subsystem <= 6.2, d3d12 stub present, no D3D12 imports).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
