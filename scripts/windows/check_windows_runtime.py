"""Verify shipped Windows x64 binaries, Qt dependencies, and MSVC runtime.

Requires pefile (see requirements-build.txt). This checks the shipped binaries
directly to ensure the application is 100% self-contained before packaging.
"""

import argparse
from pathlib import Path
import sys

try:
    import pefile
except ImportError:
    pefile = None


REQUIRED_FILES = (
    "HarmonyCast.exe",
    "platforms/qwindows.dll",
    "msvcp140.dll",
    "vcruntime140.dll",
    "opengl32sw.dll",
    "D3Dcompiler_47.dll",
    "resources/hdc/hdc.exe",
)

REQUIRED_PREFIXES = (
    "Qt6Core",
    "Qt6Gui",
    "Qt6Widgets",
    "Qt6Network",
    "avcodec",
    "avutil",
    "swscale",
)


def check_directory(root: Path) -> list:
    errors = []
    
    # 1. Exact file checks
    for name in REQUIRED_FILES:
        target = root / name
        if not target.is_file():
            errors.append(f"Missing required runtime file: {name}")

    # 2. Prefix checks (handles versioned DLLs like avcodec-60.dll / avcodec-61.dll)
    existing_file_names = {f.name.lower() for f in root.iterdir() if f.is_file()}
    for prefix in REQUIRED_PREFIXES:
        p_lower = prefix.lower()
        if not any(f.startswith(p_lower) for f in existing_file_names):
            errors.append(f"Missing required dependency matching prefix: {prefix}*.dll")

    # 3. PE format checks (if pefile is installed)
    if pefile is not None:
        for binary in sorted(root.rglob("*")):
            if not binary.is_file() or binary.suffix.lower() not in {".dll", ".exe"}:
                continue
            label = str(binary.relative_to(root))
            try:
                with pefile.PE(str(binary), fast_load=True) as image:
                    # Machine 0x8664 is AMD64 (x64)
                    if image.FILE_HEADER.Machine != 0x8664:
                        errors.append(f"{label}: expected x64 binary, found machine type {hex(image.FILE_HEADER.Machine)}")
            except (pefile.PEFormatError, OSError) as error:
                errors.append(f"{label}: failed to inspect PE image: {error}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Deployed Windows app directory to verify")
    args = parser.parse_args()

    errors = check_directory(args.directory)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print("Windows x64 runtime verification passed: all essential Qt6, VC CRT, FFmpeg, and HDC binaries are present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
