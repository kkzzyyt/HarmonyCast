# Build with: .venv/bin/python -m PyInstaller --noconfirm HarmonyCast.spec
from pathlib import Path

root = Path(SPECPATH)
hdc_files = [root / "resources/hdc/hdc", root / "resources/hdc/libusb_shared.dylib"]
hdc_binaries = [(str(path), "resources/hdc") for path in hdc_files] if all(path.is_file() for path in hdc_files) else []
a = Analysis(
    [str(root / "launch.py")],
    pathex=[str(root)],
    binaries=hdc_binaries,
    datas=[(str(root / "resources/servers"), "resources/servers"),
           (str(root / "resources/capture"), "resources/capture"),
           (str(root / "resources/app-icon.svg"), "resources"),
           (str(root / "resources/chevron-down.svg"), "resources"),
           (str(root / "resources/check.svg"), "resources"),
           (str(root / "licenses"), "licenses"),
           (str(root / "README.md"), "."),
           (str(root / "LICENSE"), "."),
           (str(root / "NOTICE"), "."),
           (str(root / "THIRD_PARTY_NOTICES.md"), ".")],
    hiddenimports=["PyQt5.QtSvg", "PyQt5.QtWebSockets"],
    excludes=["tkinter", "cv2", "scipy", "matplotlib", "pytest"],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz, a.scripts, [], exclude_binaries=True, name="HarmonyCast",
    debug=False, bootloader_ignore_signals=False, strip=False, upx=False,
    console=False, argv_emulation=False, target_arch="arm64", codesign_identity=None,
)
collect = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name="HarmonyCast")
app = BUNDLE(
    collect, name="HarmonyCast.app", icon=str(root / "resources/app-icon.icns"),
    bundle_identifier="local.harmonycast.desktop",
    info_plist={
        "CFBundleDisplayName": "鸿蒙投屏",
        "CFBundleShortVersionString": "1.5.0",
        "CFBundleVersion": "8",
        "NSHighResolutionCapable": True,
        "LSMinimumSystemVersion": "14.0",
        "NSHumanReadableCopyright": "HarmonyCast · includes OpenHarmony_Scrcpy (Apache-2.0)",
    },
)
