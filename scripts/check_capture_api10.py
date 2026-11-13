"""Validate shipped API 10 ELF files without installing an OpenHarmony SDK."""
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
API10_IMPORTS = {
    "OH_AVScreenCapture_Create", "OH_AVScreenCapture_SetCallback", "OH_AVScreenCapture_Init",
    "OH_AVScreenCapture_SetMicrophoneEnabled", "OH_AVScreenCapture_StartScreenCapture",
    "OH_AVScreenCapture_StopScreenCapture", "OH_AVScreenCapture_Release",
    "OH_AVScreenCapture_AcquireVideoBuffer", "OH_AVScreenCapture_ReleaseVideoBuffer",
    "OH_NativeBuffer_GetConfig", "OH_NativeBuffer_Map", "OH_NativeBuffer_Unmap",
}


def validate(path: Path, bits: int, machine: int):
    data = path.read_bytes()
    if data[:6] != b"\x7fELF" + bytes((2 if bits == 64 else 1, 1)):
        raise ValueError(f"{path.name}: unexpected ELF format")
    if struct.unpack_from("<H", data, 18)[0] != machine:
        raise ValueError(f"{path.name}: incorrect CPU architecture")
    offset = struct.unpack_from("<Q" if bits == 64 else "<I", data, 40 if bits == 64 else 32)[0]
    entry_size, count = struct.unpack_from("<HH", data, 58 if bits == 64 else 46)
    section_format = "<IIQQQQIIQQ" if bits == 64 else "<10I"
    sections = [struct.unpack_from(section_format, data, offset + i * entry_size) for i in range(count)]

    def contents(section):
        return data[section[4]:section[4] + section[5]]

    def string(table, start):
        end = table.index(b"\0", start)
        return table[start:end].decode("ascii")

    imports, libraries = set(), set()
    for section in sections:
        if section[1] not in (6, 11):
            continue
        table = contents(sections[section[6]])
        raw = contents(section)
        for index in range(0, len(raw), section[9]):
            if section[1] == 6:
                tag, value = struct.unpack_from("<qQ" if bits == 64 else "<iI", raw, index)
                if tag == 1:
                    libraries.add(string(table, value))
            else:
                symbol = struct.unpack_from("<IBBHQQ" if bits == 64 else "<IIIBBH", raw, index)
                undefined = symbol[3] == 0 if bits == 64 else symbol[5] == 0
                if undefined and symbol[0]:
                    imports.add(string(table, symbol[0]))
    if libraries != {"libnative_avscreen_capture.so", "libnative_buffer.so", "libc.so"}:
        raise ValueError(f"{path.name}: unsupported dependencies: {sorted(libraries)}")
    native_imports = {symbol for symbol in imports if symbol.startswith("OH_")}
    if native_imports != API10_IMPORTS:
        raise ValueError(f"{path.name}: unexpected native imports: {sorted(native_imports ^ API10_IMPORTS)}")
    return len(native_imports)


def main():
    for arch, bits, machine in (("arm64", 64, 183), ("armv7", 32, 40)):
        path = ROOT / "resources/capture" / f"harmonycast_capture_api10_{arch}"
        count = validate(path, bits, machine)
        print(f"{path.name}: {bits}-bit ELF, {count} API 9/10 imports, no API 12 dependencies")


if __name__ == "__main__":
    main()
