# Optional local HDC bundle

Prebuilt HDC and libusb files from the original local workspace are excluded
from Git. Install HDC using the official HarmonyOS/OpenHarmony SDK, then select
the executable in HarmonyCast Settings or add it to PATH. The macOS application
also looks for common DevEco Studio installations.

For a local self-contained build, place the compatible Darwin ARM64 files here:

```text
hdc
libusb_shared.dylib
```

`HarmonyCast.spec` bundles these files only when both are present. Otherwise it
builds an application that requires an externally installed HDC. Check the exact
components' redistribution and corresponding-source requirements before
publishing a bundled binary application. See `THIRD_PARTY_NOTICES.md`.

HDC: https://github.com/openharmony/developtools_hdc

libusb: https://github.com/libusb/libusb
