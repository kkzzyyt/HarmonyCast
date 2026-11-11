"""Verify required Windows x64 binaries, Qt plugins, and MSVC runtime.

Requires pefile (see requirements-build.txt). This checks the shipped files;
it does not replace testing on the target Windows computer.
"""

import argparse
import sys
from pathlib import Path

try:
    import pefile
except ImportError:
    pefile = None


REQUIRED_FILES = (
    "HarmonyCast.exe",
    "platforms/qwindows.dll",
    "imageformats/qjpeg.dll",
    "msvcp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "opengl32sw.dll",
    "D3Dcompiler_47.dll",
    "resources/hdc/hdc.exe",
    "resources/hdc/libusb_shared.dll",
)

REQUIRED_PREFIXES = (
    "Qt6Core",
    "Qt6Gui",
    "Qt6Widgets",
    "Qt6Network",
    "Qt6Svg",
    "Qt6WebSockets",
    "avcodec",
    "avutil",
    "swscale",
)


def _has_dependency(file_names: set[str], prefix: str) -> bool:
    """Match Qt modules exactly and allow FFmpeg's versioned DLL names."""
    expected = prefix.lower()
    if expected.startswith("qt"):
        return f"{expected}.dll" in file_names
    return any(name.startswith(expected) for name in file_names)


def _check_pe_images(root: Path) -> list[str]:
    errors = []
    for binary in sorted(root.rglob("*")):
        if not binary.is_file() or binary.suffix.lower() not in {".dll", ".exe"}:
            continue
        if (
            binary.suffix.lower() == ".exe"
            and binary.name.lower().startswith("vc_redist")
        ):
            # Microsoft's redistributable installer has a 32-bit bootstrapper.
            continue
        label = binary.relative_to(root).as_posix()
        try:
            with pefile.PE(str(binary), fast_load=True) as image:
                if image.FILE_HEADER.Machine != 0x8664:
                    errors.append(
                        f"{label}: expected x64 binary, found machine type "
                        f"{hex(image.FILE_HEADER.Machine)}"
                    )
        except (pefile.PEFormatError, OSError) as error:
            errors.append(f"{label}: failed to inspect PE image: {error}")
    return errors


def check_directory(root: Path) -> list[str]:
    """Return packaging failures without consulting the host's DLL search path."""
    if not root.is_dir():
        return [f"Deployment directory does not exist: {root}"]
    errors = [
        f"Missing required runtime file: {name}"
        for name in REQUIRED_FILES
        if not (root / name).is_file()
    ]
    file_names = {
        path.name.lower()
        for path in root.iterdir()
        if path.is_file() and path.suffix.lower() == ".dll"
    }
    for prefix in REQUIRED_PREFIXES:
        if not _has_dependency(file_names, prefix):
            errors.append(f"Missing required dependency matching prefix: {prefix}*.dll")
    if pefile is None:
        errors.append(
            "Cannot inspect PE binaries: install scripts/windows/requirements-build.txt"
        )
    else:
        errors.extend(_check_pe_images(root))

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "directory", type=Path, help="Deployed Windows app directory to verify"
    )
    args = parser.parse_args()

    errors = check_directory(args.directory)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print(
        "Windows x64 runtime gate passed: required Qt6 modules, image plugins, "
        "VC CRT, FFmpeg, and HDC binaries are present."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
