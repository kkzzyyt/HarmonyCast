# Bundled HDC binaries

This directory contains prebuilt HDC (OpenHarmony Device Connector) and libusb binaries for out-of-the-box, zero-dependency client execution across platforms:

- **macOS (Darwin ARM64)**:
  - `hdc` (executable)
  - `libusb_shared.dylib` (shared library)
- **Windows (x64 / Windows Server 2012 / Win 8 / 10 / 11)**:
  - `hdc.exe` (PE32+ console executable, Subsystem 6.0)
  - `libusb_shared.dll` (dynamic library)

The client automatically discovers and prioritizes these bundled binaries before falling back to system PATH or DevEco Studio SDK installations.

HDC upstream: https://github.com/openharmony/developtools_hdc (Apache-2.0)
libusb upstream: https://github.com/libusb/libusb (LGPL-2.1)
See `THIRD_PARTY_NOTICES.md` for license notices and redistribution notes.
