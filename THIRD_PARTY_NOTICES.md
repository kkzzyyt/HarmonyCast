# Third-party components

HarmonyCast first-party code is distributed under GPL-3.0-or-later; see `LICENSE`
and `NOTICE`. The files below retain their original licenses and attribution.

## OpenHarmony_Scrcpy v2.3

- Source: https://gitcode.com/OpenHarmony_Tools/OpenHarmony_Scrcpy
- Snapshot: 85890e693fb5b3e4b3ef50a16af688787ca85070
- Copyright (c) 2026 luodh0157.
- License: Apache License, Version 2.0; included in `licenses/OpenHarmony_Scrcpy-LICENSE.txt`.
- Reused files: `Client/video/decoder.py`, `Client/video/config.py`, and `Client/core/constants.py`, copied into `harmonycast/upstream/`. Decoder imports are adapted for the local package and application logger. The decoder uses complete access-unit packets directly and slice threading to avoid waiting for later frames. Original copyright headers are retained.
- Reused assets: Darwin ARM64 HDC and libusb, ARM32 RK3568 and ARM64 HarmonyOS server binaries, and `ohscrcpy_server.cfg`.
- The wrapper fixes macOS HDC argument handling and implements its own bounded packet receiver, process ownership, compatibility preview, log tools, and Qt user interface.
- `native/capture_server.c` and the RISC-V startup entry are new HarmonyCast components. They use the device's public OpenHarmony C APIs for continuous capture, H.264 surface encoding, and RGBA fallback. Device system libraries are used for linking and are not redistributed.

## Optional local HDC and libusb build inputs

- The original local `resources/hdc/hdc` is the Darwin ARM64 HDC distributed with the upstream
  OpenHarmony_Scrcpy snapshot above. HDC source is maintained at
  https://github.com/openharmony/developtools_hdc under Apache-2.0; the Apache
  license text is included in `licenses/OpenHarmony_Scrcpy-LICENSE.txt`.
- The original local `resources/hdc/libusb_shared.dylib` is the accompanying dynamic USB library.
  libusb is licensed under LGPL-2.1-or-later; its license is included in
  `licenses/libusb-LGPL-2.1.txt`. The source project is
  https://github.com/libusb/libusb and the bundled copy comes from the same
  OpenHarmony_Scrcpy distribution. The precise libusb source revision/build flags
  of that prebuilt distribution have not been independently established here.
- These prebuilt HDC/libusb files are excluded from this Git repository. Users
  can select their own SDK-installed HDC executable in Settings. Binary app releases
  require checking source availability and the redistribution obligations for
  the exact versions of all bundled dependencies.

## Runtime dependencies

- PyQt5 5.15.11 — Riverbank Computing Limited, GPL v3. https://www.riverbankcomputing.com/software/pyqt/
- Qt 5.15.19 — LGPL v3. https://www.qt.io/licensing/open-source-lgpl-obligations
- PyAV 16.1.0 — BSD 3-Clause, see `licenses/PyAV-LICENSE.txt`. https://github.com/PyAV-Org/PyAV
- NumPy 2.5.4 — BSD 3-Clause and bundled notices, see `licenses/NumPy-LICENSE.txt`. https://numpy.org/
- Python 3.12 — PSF License. https://docs.python.org/3/license.html

Python distribution metadata is preserved by PyInstaller. Build scripts and
wrapper source are supplied in this repository. This initial publication is a
source release; the original workspace's app bundles are not published here.
Source code is required when redistributing a combined application using the GPL
edition of PyQt5; review component licenses for redistribution terms.
