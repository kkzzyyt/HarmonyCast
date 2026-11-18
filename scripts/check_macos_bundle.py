"""Verify the architecture and runtime closure of a deployed macOS app."""
import argparse
from pathlib import Path
import plistlib
import re
import subprocess
import struct
from collections import deque

MACHO_MAGIC = {bytes.fromhex(value) for value in (
    "feedface", "cefaedfe", "feedfacf", "cffaedfe", "cafebabe", "bebafeca", "cafebabf", "bfbafeca"
)}
REQUIRED_BINARIES = (
    "Contents/MacOS/HarmonyCast", "Contents/PlugIns/platforms/libqcocoa.dylib",
    "Contents/PlugIns/imageformats/libqjpeg.dylib", "Contents/PlugIns/tls/libqsecuretransportbackend.dylib",
    "Contents/Resources/hdc/hdc", "Contents/Resources/hdc/libusb_shared.dylib",
    "Contents/Resources/adb/macos/adb",
)
REQUIRED = REQUIRED_BINARIES + ("Contents/Resources/licenses/miniz-LICENSE.txt",)


def macho(path):
    with path.open("rb") as stream:
        return stream.read(4) in MACHO_MAGIC


def version(value):
    parts = tuple(int(part) for part in value.split("."))
    return (parts + (0, 0, 0))[:3]


def file_type(path, architecture):
    cpu = {"arm64": 0x100000C, "x86_64": 0x1000007}[architecture]
    with path.open("rb") as stream:
        header = stream.read(8)
        magic = header[:4]
        if magic in (bytes.fromhex("cafebabe"), bytes.fromhex("cafebabf"), bytes.fromhex("bebafeca"), bytes.fromhex("bfbafeca")):
            endian = ">" if magic in (bytes.fromhex("cafebabe"), bytes.fromhex("cafebabf")) else "<"
            count = struct.unpack(endian + "I", header[4:8])[0]
            if count > 64:
                raise ValueError("too many Mach-O slices")
            wide = magic in (bytes.fromhex("cafebabf"), bytes.fromhex("bfbafeca"))
            for _ in range(count):
                entry = stream.read(32 if wide else 20)
                if len(entry) != (32 if wide else 20):
                    raise ValueError("truncated Mach-O slice table")
                if struct.unpack_from(endian + "I", entry)[0] == cpu:
                    offset = struct.unpack_from(endian + ("Q" if wide else "I"), entry, 8)[0]
                    stream.seek(offset)
                    break
            else:
                raise ValueError("required Mach-O slice missing")
        else:
            stream.seek(0)
        header = stream.read(16)
        if len(header) != 16:
            raise ValueError("truncated Mach-O header")
        endian = "<" if header[:4] in (bytes.fromhex("cefaedfe"), bytes.fromhex("cffaedfe")) else ">"
        return struct.unpack_from(endian + "I", header, 12)[0]


def load_commands(text):
    dependencies, rpaths = [], []
    for block in re.split(r"(?m)^\s*Load command \d+\s*$", text):
        match = re.search(r"(?m)^\s*cmd (LC_\w+)", block)
        if not match:
            continue
        command = match.group(1)
        if command == "LC_RPATH":
            value = re.search(r"(?m)^\s*path (.+?) \(offset \d+\)", block)
            if value:
                rpaths.append(value.group(1))
        elif command in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB"):
            value = re.search(r"(?m)^\s*name (.+?) \(offset \d+\)", block)
            if value:
                dependencies.append(value.group(1))
        # LC_ID_DYLIB is the library's own install name, not a dependency.
    return dependencies, rpaths


def inspect_bundle(root, architecture, minimum_os=None):
    root = root.resolve()
    errors = [f"Missing required file: {name}" for name in REQUIRED if not (root / name).is_file()]
    for name in REQUIRED_BINARIES:
        path = root / name
        if path.is_file() and path.resolve().is_relative_to(root) and not macho(path):
            errors.append(f"{name}: required host binary is not Mach-O")
    files = set()
    for path in root.rglob("*"):
        resolved = path.resolve()
        if not resolved.is_relative_to(root):
            errors.append(f"{path.relative_to(root)}: symlink escapes application bundle")
        elif path.is_file():
            files.add(resolved)
    floor = (0, 0, 0)
    nodes = {}
    for path in sorted(files):
        if not macho(path):
            continue
        label = path.relative_to(root).as_posix()
        try:
            architectures = subprocess.check_output(["lipo", "-archs", str(path)], text=True).split()
            if architecture not in architectures:
                errors.append(f"{label}: missing {architecture} slice ({', '.join(architectures)})")
                continue
            commands = subprocess.check_output(["otool", "-arch", architecture, "-l", str(path)], text=True)
            dependencies, rpaths = load_commands(commands)
            nodes[path] = (file_type(path, architecture), dependencies, rpaths)
            versions = re.findall(r"\bminos\s+([0-9.]+)", commands)
            versions += re.findall(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([0-9.]+)", commands)
            if not versions:
                errors.append(f"{label}: no minimum macOS load command")
            for value in versions:
                floor = max(floor, version(value))
                if minimum_os and version(value) > version(minimum_os):
                    errors.append(f"{label}: requires macOS {value}, exceeds target {minimum_os}")
        except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as error:
            errors.append(f"{label}: Mach-O inspection failed: {error}")
    if not nodes:
        errors.append("No Mach-O binaries found in bundle")

    def expand(value, loader, executable):
        if value.startswith("@loader_path/"):
            return (loader.parent / value[len("@loader_path/"):]).resolve()
        if value.startswith("@executable_path/"):
            return (executable.parent / value[len("@executable_path/"):]).resolve()
        return Path(value).resolve() if Path(value).is_absolute() else None

    main = (root / "Contents/MacOS/HarmonyCast").resolve()
    main_paths = []
    if main in nodes:
        main_paths = [expand(value, main, main) for value in nodes[main][2]]
        main_paths = [value for value in main_paths if value is not None]
    # Validate dynamic plugins in the app context, and each auxiliary executable
    # in its own context. Follow dependencies with their inherited LC_RPATH stack.
    pending = deque((path, path, ()) for path, node in nodes.items() if node[0] == 2)
    unvisited = set(nodes)
    visited = set()
    while pending or unvisited:
        if not pending:
            # Libraries not reached from an executable are dynamically loaded
            # plugins. Validate each with the main executable's run-path stack.
            pending.append((min(unvisited), main, tuple(main_paths)))
        path, executable, inherited = pending.popleft()
        _, dependencies, raw_paths = nodes[path]
        paths = [expand(value, path, executable) for value in raw_paths]
        paths = tuple(dict.fromkeys(value for value in paths + list(inherited) if value is not None))
        key = (path, executable, paths)
        if key in visited:
            continue
        visited.add(key)
        unvisited.discard(path)
        for dependency in dependencies:
            if dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            if dependency.startswith("/"):
                errors.append(f"{path.relative_to(root)}: external dependency {dependency}")
                continue
            if dependency.startswith("@rpath/"):
                suffix = dependency[len("@rpath/"):]
                candidates = [(directory / suffix).resolve() for directory in paths]
                resolved = next((candidate for candidate in candidates if candidate.is_file()), None)
            else:
                resolved = expand(dependency, path, executable)
            if resolved is None or not resolved.is_relative_to(root) or not resolved.is_file():
                errors.append(f"{path.relative_to(root)}: unresolved or external dependency {dependency}")
            elif resolved not in nodes:
                errors.append(f"{path.relative_to(root)}: dependency is not a verified Mach-O: {dependency}")
            else:
                pending.append((resolved, executable, tuple(paths)))
    return errors, floor, len(nodes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--arch", required=True, choices=("arm64", "x86_64"))
    parser.add_argument("--minimum-os")
    parser.add_argument("--write-minimum", action="store_true")
    args = parser.parse_args()
    if args.minimum_os and not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", args.minimum_os):
        parser.error("--minimum-os must be a numeric macOS version")
    errors, floor, count = inspect_bundle(args.bundle, args.arch, args.minimum_os)
    if errors:
        print("\n".join(errors))
        return 1
    minimum = args.minimum_os or ".".join(map(str, floor[:2]))
    if args.write_minimum:
        path = args.bundle / "Contents/Info.plist"
        with path.open("rb") as stream:
            info = plistlib.load(stream)
        info["LSMinimumSystemVersion"] = minimum
        with path.open("wb") as stream:
            plistlib.dump(info, stream)
    print(f"macOS bundle gate passed: {count} Mach-O binaries support {args.arch}, minimum macOS {minimum}, dependencies bundled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
