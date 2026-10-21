# Contributing to HarmonyCast

Bug reports, documentation improvements and focused pull requests are welcome.
Please use Chinese or English.

## Development

Use Python 3.12 on macOS. Run commands from the repository root:

```bash
python3.12 -m venv .venv
.venv/bin/python -m pip install -r requirements-dev.txt
QT_QPA_PLATFORM=offscreen .venv/bin/python -m pytest -q --cov --cov-report=term-missing
```

The tests use fake HDC processes, local TCP streams, real H.264 encoding/decoding
and offscreen Qt windows. They do not require a connected device. Hardware smoke
scripts in `scripts/` are separate and run only when explicitly invoked.

Describe the problem and expected behavior in a pull request. Add a regression
test before changing behavior. Keep changes focused and retain upstream notices
in `harmonycast/upstream/`.

## Device issues

Include the host OS, device architecture, system/API version, HDC version and
steps to reproduce. State which mirroring mode is selected. Remove serial
numbers, addresses, application logs and screenshots that you do not intend to
make public.

The project contains board-specific ICU/watchdog extensions and an application
namespace preset inherited from the original local workbench. Changes to those
extensions need hardware-specific validation; unit tests alone do not establish
device compatibility.

## Source layout

- `harmonycast/core.py`: HDC execution, device/application parsing and log storage.
- `harmonycast/backend.py`: mirroring sessions, HiLog and device tools.
- `harmonycast/protocol.py`: bounded stream packet parsing.
- `harmonycast/ui.py`: Qt desktop interface.
- `harmonycast/upstream/`: attributed OpenHarmony_Scrcpy components.
- `native/`: RISC-V capture service source and startup code.
- `resources/`: desktop assets and prebuilt device tools/services.
- `tests/`: automated tests.

Contributions to first-party code are accepted under GPL-3.0-or-later. Third-party
components keep their existing licenses.
