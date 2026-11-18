# Bundled HDC binaries

This directory contains prebuilt HDC (OpenHarmony Device Connector) and libusb binaries for out-of-the-box, zero-dependency client execution across platforms:

- **macOS (Darwin ARM64 + Intel x86_64)**:
  - `hdc` (executable)
  - `libusb_shared.dylib` (shared library)
  - Both files are universal Mach-O binaries. The existing ARM64 slices are retained; the Intel slices come from the official OpenHarmony 5.0.0.71 public SDK (API 12), `toolchains-darwin-x64-5.0.0.71-Release.zip`.
  - SDK source: https://repo.huaweicloud.com/openharmony/os/5.0.0-Release/ohos-sdk-mac-public.tar.gz
  - The complete SDK archive was verified against its published SHA-256 `863abc8e47acd760a42921a96bd1e36a857593801ce6f90ed3044a77fdbab6c6` before extracting the Intel tools. Derived bundle checksums are retained in `checksums.json`.
- **Windows (x64 / Windows Server 2012 / Win 8 / 10 / 11)**:
  - `hdc.exe` (PE32+ console executable, Subsystem 6.0)
  - `libusb_shared.dll` (dynamic library)

The client automatically discovers and prioritizes these bundled binaries before falling back to system PATH or DevEco Studio SDK installations.

HDC upstream: https://github.com/openharmony/developtools_hdc (Apache-2.0)
libusb upstream: https://github.com/libusb/libusb (LGPL-2.1)
See `THIRD_PARTY_NOTICES.md` for license notices and redistribution notes.
