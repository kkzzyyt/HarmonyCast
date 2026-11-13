"""Exercise both Windows package gates with real, minimal PE images."""

import importlib.util
from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/windows/check_server2012_runtime.py"
spec = importlib.util.spec_from_file_location("check_server2012_runtime", SCRIPT)
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)

GENERIC_SCRIPT = Path(__file__).resolve().parents[1] / "scripts/windows/check_windows_runtime.py"
generic_spec = importlib.util.spec_from_file_location("check_windows_runtime", GENERIC_SCRIPT)
generic_runtime = importlib.util.module_from_spec(generic_spec)
generic_spec.loader.exec_module(generic_runtime)


def pe_image(imports=(), exports=(), subsystem=(6, 2)):
    """Build a PE32+ image with optional import and export tables."""
    data = bytearray(8192)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x8664, 1, 0, 0, 0, 240, 0x2022)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<Q", data, optional + 24, 0x140000000)
    struct.pack_into("<II", data, optional + 32, 0x1000, 0x200)
    struct.pack_into("<HH", data, optional + 48, *subsystem)
    struct.pack_into("<II", data, optional + 56, 0x3000, 0x200)
    struct.pack_into("<H", data, optional + 68, 2)
    struct.pack_into("<I", data, optional + 108, 16)
    section = optional + 240
    data[section:section + 8] = b".rdata\0\0"
    struct.pack_into("<IIII", data, section + 8, 0x1E00, 0x1000, 0x1E00, 0x200)
    struct.pack_into("<I", data, section + 36, 0x40000040)

    cursor = 0x200

    def append(value):
        nonlocal cursor
        rva = cursor - 0x200 + 0x1000
        data[cursor:cursor + len(value)] = value
        cursor += len(value)
        return rva

    if imports:
        import_desc_size = (len(imports) + 1) * 20
        import_desc_offset = cursor
        cursor += import_desc_size
        struct.pack_into("<II", data, optional + 112 + 8, import_desc_offset - 0x200 + 0x1000, import_desc_size)
        for index, (dll, function) in enumerate(imports):
            name = append(dll.encode("ascii") + b"\0")
            hint = append(b"\0\0" + function.encode("ascii") + b"\0")
            thunk = append(struct.pack("<QQ", hint, 0))
            struct.pack_into("<IIIII", data, import_desc_offset + index * 20, thunk, 0, 0, name, thunk)

    if exports:
        export_dir_offset = cursor
        export_dir_rva = cursor - 0x200 + 0x1000
        cursor += 40
        dll_name_rva = append(b"d3d12.dll\0")
        func_rvas = [0x1000 for _ in exports]
        name_rvas = [append(exp.encode("ascii") + b"\0") for exp in exports]
        ordinals = list(range(len(exports)))

        functions_rva = append(b"".join(struct.pack("<I", r) for r in func_rvas))
        names_rva = append(b"".join(struct.pack("<I", r) for r in name_rvas))
        ordinals_rva = append(b"".join(struct.pack("<H", o) for o in ordinals))

        export_size = cursor - export_dir_offset
        struct.pack_into("<II", data, optional + 112, export_dir_rva, export_size)
        struct.pack_into("<IIHHIIIIIII", data, export_dir_offset,
                         0, 0, 0, 0, dll_name_rva, 1,
                         len(exports), len(exports), functions_rva, names_rva, ordinals_rva)

    return bytes(data)


class Server2012RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in runtime.REQUIRED_FILES:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(pe_image())

    def test_accepts_compatible_runtime(self):
        (self.root / "HarmonyCast.exe").write_bytes(pe_image([("Qt5Core.dll", "qVersion")]))
        self.assertEqual(runtime.check_directory(self.root), [])

    def test_accepts_d3d12_stub_exports(self):
        (self.root / "d3d12.dll").write_bytes(pe_image(exports=["D3D12CreateDevice", "D3D12GetDebugInterface"]))
        self.assertEqual(runtime.check_directory(self.root), [])

    def test_rejects_d3d12_stub_missing_device_export(self):
        (self.root / "d3d12.dll").write_bytes(pe_image(exports=["SomeOtherFunction"]))
        errors = runtime.check_directory(self.root)
        self.assertTrue(any("stub is missing expected exports" in error and "D3D12CreateDevice" in error for error in errors))

    def test_rejects_missing_d3d12_stub(self):
        (self.root / "d3d12.dll").unlink()
        errors = runtime.check_directory(self.root)
        self.assertTrue(any("Missing runtime file: d3d12.dll" in error for error in errors))

    def test_rejects_d3d12_in_transitive_plugin(self):
        (self.root / "platforms/qwindows.dll").write_bytes(pe_image([("D3D12.dll", "D3D12CreateDevice")]))
        self.assertTrue(any("D3D12.dll" in error for error in runtime.check_directory(self.root)))

    def test_rejects_qt6_dependency(self):
        (self.root / "HarmonyCast.exe").write_bytes(pe_image([("Qt6Gui.dll", "qVersion")]))
        self.assertTrue(any("Qt6Gui.dll" in error for error in runtime.check_directory(self.root)))

    def test_rejects_newer_subsystem(self):
        (self.root / "HarmonyCast.exe").write_bytes(pe_image(subsystem=(10, 0)))
        self.assertTrue(any("subsystem" in error for error in runtime.check_directory(self.root)))

    def test_accepts_api_set_forwarder_newer_subsystem(self):
        (self.root / "api-ms-win-crt-runtime-l1-1-0.dll").write_bytes(pe_image(subsystem=(10, 0)))
        self.assertEqual(runtime.check_directory(self.root), [])

    def test_rejects_windows10_entry_point(self):
        (self.root / "Qt5Gui.dll").write_bytes(pe_image([("user32.dll", "GetDpiForWindow")]))
        self.assertTrue(any("GetDpiForWindow" in error for error in runtime.check_directory(self.root)))

    def test_rejects_missing_bundled_hdc(self):
        (self.root / "resources/hdc/hdc.exe").unlink()
        errors = runtime.check_directory(self.root)
        self.assertTrue(any("Missing runtime file: resources/hdc/hdc.exe" in error for error in errors))

    def test_rejects_missing_jpeg_plugin(self):
        (self.root / "imageformats/qjpeg.dll").unlink()
        errors = runtime.check_directory(self.root)
        self.assertIn("Missing runtime file: imageformats/qjpeg.dll", errors)

    def test_rejects_wrong_architecture_jpeg_plugin(self):
        image = bytearray(pe_image())
        struct.pack_into("<H", image, 0x84, 0x14C)
        (self.root / "imageformats/qjpeg.dll").write_bytes(image)
        errors = runtime.check_directory(self.root)
        self.assertTrue(
            any("imageformats/qjpeg.dll: expected an x64 binary" in error for error in errors)
        )

    def test_rejects_incomplete_package(self):
        (self.root / "platforms/qwindows.dll").unlink()
        self.assertTrue(any("qwindows.dll" in error for error in runtime.check_directory(self.root)))

    def test_rejects_corrupt_binary(self):
        (self.root / "HarmonyCast.exe").write_bytes(b"not a PE")
        self.assertTrue(runtime.check_directory(self.root))

    def test_rejects_empty_directory(self):
        self.assertTrue(runtime.check_directory(self.root / "absent"))

    def test_rejects_wrong_architecture(self):
        image = bytearray(pe_image())
        struct.pack_into("<H", image, 0x84, 0x14C)
        (self.root / "HarmonyCast.exe").write_bytes(image)
        self.assertTrue(any("x64" in error for error in runtime.check_directory(self.root)))

    def test_cli_succeeds_for_complete_package(self):
        with patch("sys.argv", [str(SCRIPT), str(self.root)]), redirect_stdout(io.StringIO()):
            self.assertEqual(runtime.main(), 0)

    def test_cli_fails_for_incomplete_package(self):
        output = io.StringIO()
        with patch("sys.argv", [str(SCRIPT), str(self.root / "absent")]), redirect_stderr(output):
            self.assertEqual(runtime.main(), 1)
        self.assertIn("Missing runtime file", output.getvalue())


class TestWindowsGenericRuntimeGate(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        (self.root / "platforms").mkdir(parents=True)
        (self.root / "resources/hdc").mkdir(parents=True)

        for name in (
            "HarmonyCast.exe", "platforms/qwindows.dll",
            "imageformats/qjpeg.dll",
            "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll",
            "Qt6Svg.dll", "Qt6SvgWidgets.dll", "Qt6WebSockets.dll",
            "avcodec-60.dll", "avutil-58.dll", "swscale-7.dll",
            "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll",
            "opengl32sw.dll", "D3Dcompiler_47.dll",
            "resources/hdc/hdc.exe", "resources/hdc/libusb_shared.dll",
            "resources/capture/harmonycast_capture_api10_arm64",
            "resources/capture/harmonycast_capture_api10_armv7",
            "resources/adb/windows-x64/adb.exe", "resources/adb/windows-x64/AdbWinApi.dll",
            "resources/adb/windows-x64/AdbWinUsbApi.dll", "resources/adb/windows-x64/libwinpthread-1.dll",
            "resources/servers/android/scrcpy-server-v5.0.1",
        ):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            image = bytearray(pe_image())
            if name.startswith("resources/adb/"):
                struct.pack_into("<H", image, 0x84, 0x14C)
            target.write_bytes(image)

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_accepts_complete_generic_package(self):
        self.assertEqual(generic_runtime.check_directory(self.root), [])

    def test_rejects_missing_vcruntime(self):
        (self.root / "vcruntime140.dll").unlink()
        errors = generic_runtime.check_directory(self.root)
        self.assertTrue(any("vcruntime140.dll" in e for e in errors))

    def test_rejects_missing_msvcp(self):
        (self.root / "msvcp140.dll").unlink()
        errors = generic_runtime.check_directory(self.root)
        self.assertTrue(any("msvcp140.dll" in e for e in errors))

    def test_rejects_missing_support_binaries(self):
        for name in (
            "imageformats/qjpeg.dll",
            "vcruntime140_1.dll",
            "resources/hdc/libusb_shared.dll",
            "resources/capture/harmonycast_capture_api10_arm64",
            "resources/capture/harmonycast_capture_api10_armv7",
        ):
            with self.subTest(name=name):
                path = self.root / name
                path.unlink()
                try:
                    errors = generic_runtime.check_directory(self.root)
                    self.assertIn(f"Missing required runtime file: {name}", errors)
                finally:
                    path.write_bytes(pe_image())

    def test_rejects_missing_qt_modules(self):
        for module in (
            "Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Network",
            "Qt6Svg", "Qt6WebSockets",
        ):
            with self.subTest(module=module):
                path = self.root / f"{module}.dll"
                path.unlink()
                try:
                    errors = generic_runtime.check_directory(self.root)
                    self.assertIn(
                        f"Missing required dependency matching prefix: {module}*.dll",
                        errors,
                    )
                finally:
                    path.write_bytes(pe_image())

    def test_accepts_without_unreferenced_svg_widgets(self):
        # Release imports use QSvgRenderer, not QSvgWidget; windeployqt omits it.
        (self.root / "Qt6SvgWidgets.dll").unlink()
        self.assertEqual(generic_runtime.check_directory(self.root), [])

    def test_rejects_non_dll_prefix_matches(self):
        for dependency in ("avcodec-60", "Qt6Svg"):
            with self.subTest(dependency=dependency):
                (self.root / f"{dependency}.dll").unlink()
                (self.root / f"{dependency}.lib").write_bytes(b"library metadata")
                (self.root / f"{dependency}.dll.txt").write_text("not a DLL")
        errors = generic_runtime.check_directory(self.root)
        self.assertIn("Missing required dependency matching prefix: avcodec*.dll", errors)
        self.assertIn("Missing required dependency matching prefix: Qt6Svg*.dll", errors)

    def test_rejects_wrong_architecture_jpeg_plugin(self):
        image = bytearray(pe_image())
        struct.pack_into("<H", image, 0x84, 0x14C)
        (self.root / "imageformats/qjpeg.dll").write_bytes(image)
        errors = generic_runtime.check_directory(self.root)
        self.assertTrue(
            any("imageformats/qjpeg.dll: expected x64 binary" in error for error in errors)
        )

    def test_allows_only_named_standalone_adb_process_files(self):
        path = self.root / "resources/adb/windows-x64/extra.dll"
        image = bytearray(pe_image())
        struct.pack_into("<H", image, 0x84, 0x14C)
        path.write_bytes(image)
        errors = generic_runtime.check_directory(self.root)
        self.assertTrue(any("extra.dll: expected x64" in error for error in errors))

    def test_rejects_corrupt_hdc_dependency(self):
        (self.root / "resources/hdc/libusb_shared.dll").write_bytes(b"not a PE image")
        errors = generic_runtime.check_directory(self.root)
        self.assertTrue(
            any(
                "resources/hdc/libusb_shared.dll: failed to inspect PE image" in error
                for error in errors
            )
        )

    def test_rejects_missing_pe_inspector(self):
        with patch.object(generic_runtime, "pefile", None):
            errors = generic_runtime.check_directory(self.root)
        self.assertTrue(any("Cannot inspect PE binaries" in error for error in errors))

    def test_accepts_microsoft_redist_bootstrapper(self):
        image = bytearray(pe_image())
        struct.pack_into("<H", image, 0x84, 0x14C)
        (self.root / "vc_redist.x64.exe").write_bytes(image)
        self.assertEqual(generic_runtime.check_directory(self.root), [])

    def test_rejects_corrupt_dll_with_redist_prefix(self):
        (self.root / "vc_redist_extra.dll").write_bytes(b"not a PE image")
        errors = generic_runtime.check_directory(self.root)
        self.assertTrue(
            any("vc_redist_extra.dll: failed to inspect PE image" in error for error in errors)
        )

    def test_accepts_uppercase_versioned_dependency(self):
        (self.root / "avcodec-60.dll").rename(self.root / "AVCODEC-61.DLL")
        self.assertEqual(generic_runtime.check_directory(self.root), [])

    def test_cli_succeeds_for_complete_package(self):
        with (
            patch("sys.argv", [str(GENERIC_SCRIPT), str(self.root)]),
            redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(generic_runtime.main(), 0)

    def test_cli_fails_for_absent_directory(self):
        output = io.StringIO()
        with (
            patch("sys.argv", [str(GENERIC_SCRIPT), str(self.root / "absent")]),
            redirect_stderr(output),
        ):
            self.assertEqual(generic_runtime.main(), 1)
        self.assertIn("Deployment directory does not exist", output.getvalue())

    def test_rejects_missing_ffmpeg(self):
        (self.root / "avcodec-60.dll").unlink()
        errors = generic_runtime.check_directory(self.root)
        self.assertTrue(any("avcodec" in e for e in errors))

    def test_rejects_missing_rasterizer(self):
        (self.root / "opengl32sw.dll").unlink()
        errors = generic_runtime.check_directory(self.root)
        self.assertTrue(any("opengl32sw.dll" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
