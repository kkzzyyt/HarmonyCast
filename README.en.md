# HarmonyCast

[简体中文](README.md) · [Contributing](CONTRIBUTING.md) · [Third-party notices](THIRD_PARTY_NOTICES.md)

An OpenHarmony desktop debugging workbench for **macOS 14+ on Apple Silicon**.
It combines screen mirroring, touch controls, HiLog filtering, device information
and an HDC command panel.

This is the first public source snapshot of the existing HarmonyCast 1.4.1
workspace. Video protocol/decoder components and some device services come from
[OpenHarmony_Scrcpy](https://gitcode.com/OpenHarmony_Tools/OpenHarmony_Scrcpy),
with the original attribution and Apache-2.0 license retained. HarmonyCast
maintains its Qt workbench, log workflows and the RISC-V capture service in
`native/`.

## Run from source

Install Python 3.12+ and HDC from the official HarmonyOS/OpenHarmony SDK or DevEco
Studio. Enable USB debugging on your device and authorize the host.

```bash
git clone https://github.com/kkzzyyt/HarmonyCast.git
cd HarmonyCast
python3.12 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -m harmonycast
```

Select a device and an appropriate mirroring mode. The Logs page starts/stops
HiLog collection automatically as you enter/leave it. F11 or Control+Command+F
enters fullscreen; Esc returns. The original `com.yarward.*` application filter
is retained: select **全部应用** (all applications) or enter your own package or
namespace. The current UI is primarily Chinese.

HDC is bundled directly within the macOS and Windows release packages for zero-dependency plug-and-play execution. It can also be discovered in common DevEco Studio locations or PATH, or manually selected in Settings; see
[resources/hdc/README.md](resources/hdc/README.md).

## Compatibility

- RISC-V capture uses OpenHarmony C APIs and device-side H.264 encoding, with an
  RGBA fallback. It requires matching device libraries and capture permissions.
- ARM mirroring uses upstream RK3568 ARM32 or HarmonyOS 5.0.1/6.0/6.1 ARM64 service
  binaries, subject to device ABI and permission compatibility.
- Screenshot fallback requires `snapshot_display` and has a lower refresh rate.
- Board-specific ICU/watchdog extensions are available only on matching hardware.
  Commercial firmware compatibility is not guaranteed.

Deploying an upstream ARM service writes to device system directories. RISC-V
capture uses a temporary service in `/data/local/tmp/`. Use development devices
whose debugging permissions you control. No hardware compatibility or performance
retest is claimed for this first public source publication.

## Test and build

```bash
.venv/bin/python -m pip install -r requirements-dev.txt
QT_QPA_PLATFORM=offscreen .venv/bin/python -m pytest -q --cov --cov-report=term-missing
./scripts/build_macos.sh
```

Tests cover HDC execution, log filtering/export, packet parsing, real H.264/local
TCP integration and offscreen Qt flows without a device. Coverage reporting
excludes UI and upstream code; UI behavior is still tested. Hardware smoke and
benchmark scripts are manual and create ignored local artifacts.

The local Apple Silicon build generates `dist/HarmonyCast.app`, a ZIP and SHA-256
checksums. It uses ad-hoc signing, without Developer ID signing or notarization.
This first publication ships source, not the old workspace's app bundles or logs.
See the Chinese README for RISC-V build requirements.

## License

HarmonyCast first-party code is licensed under **GPL-3.0-or-later**. See
[LICENSE](LICENSE), [NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Third-party files retain their own licenses.
