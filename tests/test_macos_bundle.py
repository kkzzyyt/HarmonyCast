import importlib.util
from pathlib import Path
import tempfile
import unittest
import struct
import os
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/check_macos_bundle.py"
SPEC = importlib.util.spec_from_file_location("macos_bundle", SCRIPT)
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class MacBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in runtime.REQUIRED:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            executable = path.name in ("HarmonyCast", "hdc", "adb")
            path.write_bytes(struct.pack("<4I", 0xFEEDFACF, 0x1000007, 3, 2 if executable else 6) if path.suffix != ".txt" else b"license")
        self.library = self.root / "Contents/Frameworks/libavcodec.60.dylib"
        self.library.parent.mkdir(parents=True)
        self.library.write_bytes(struct.pack("<4I", 0xFEEDFACF, 0x1000007, 3, 6))

    def output(self, args, **kwargs):
        if args[0] == "lipo":
            return "x86_64 arm64\n"
        return ("Load command 0\n cmd LC_BUILD_VERSION\n cmdsize 32\n platform 1\n minos 12.0\n"
                "Load command 1\n cmd LC_LOAD_DYLIB\n name /usr/lib/libSystem.B.dylib (offset 24)\n"
                "Load command 2\n cmd LC_RPATH\n path @executable_path/../Frameworks (offset 12)\n")

    def test_both_architectures(self):
        with patch.object(runtime.subprocess, "check_output", side_effect=self.output):
            for architecture in ("arm64", "x86_64"):
                errors, floor, count = runtime.inspect_bundle(self.root, architecture, "12.0")
                self.assertEqual(errors, [])
                self.assertEqual(floor, (12, 0, 0))
                self.assertGreater(count, 1)

    def test_rejects_wrong_architecture_in_helper(self):
        def output(args, **kwargs):
            if args[0] == "lipo" and Path(args[-1]).name == "hdc":
                return "arm64\n"
            return self.output(args, **kwargs)
        with patch.object(runtime.subprocess, "check_output", side_effect=output):
            errors, _, _ = runtime.inspect_bundle(self.root, "x86_64")
        self.assertTrue(any("hdc: missing x86_64" in error for error in errors))

    def test_rejects_external_or_missing_dependency(self):
        for dependency in ("/usr/local/opt/ffmpeg/lib/libavcodec.60.dylib", "@rpath/missing.dylib"):
            def output(args, **kwargs):
                return self.output(args, **kwargs).replace("/usr/lib/libSystem.B.dylib", dependency)
            with patch.object(runtime.subprocess, "check_output", side_effect=output):
                self.assertTrue(runtime.inspect_bundle(self.root, "x86_64")[0])

    def test_rejects_newer_runtime(self):
        with patch.object(runtime.subprocess, "check_output", side_effect=self.output):
            errors, _, _ = runtime.inspect_bundle(self.root, "x86_64", "11.0")
        self.assertTrue(any("exceeds target" in error for error in errors))

    def test_rejects_missing_required_resources(self):
        (self.root / "Contents/Resources/hdc/hdc").unlink()
        with patch.object(runtime.subprocess, "check_output", side_effect=self.output):
            errors, _, _ = runtime.inspect_bundle(self.root, "x86_64")
        self.assertTrue(any("Missing required file" in error for error in errors))

    def test_rejects_corrupt_required_helper(self):
        (self.root / "Contents/Resources/hdc/hdc").write_bytes(b"not a Mach-O executable")
        with patch.object(runtime.subprocess, "check_output", side_effect=self.output):
            errors, _, _ = runtime.inspect_bundle(self.root, "x86_64")
        self.assertTrue(any("required host binary is not Mach-O" in error for error in errors))

    def test_loader_path_is_not_a_global_filename_search(self):
        elsewhere = self.root / "Contents/Elsewhere/libmissing.dylib"
        elsewhere.parent.mkdir(); elsewhere.write_bytes(self.library.read_bytes())
        def output(args, **kwargs):
            text = self.output(args, **kwargs)
            return text.replace("/usr/lib/libSystem.B.dylib", "@loader_path/libmissing.dylib") if args[-1].endswith("HarmonyCast") else text
        with patch.object(runtime.subprocess, "check_output", side_effect=output):
            self.assertTrue(runtime.inspect_bundle(self.root, "x86_64")[0])

    @unittest.skipIf(os.name == "nt", "bundle symlink validation runs on macOS; Windows may lack symlink privileges")
    def test_rejects_required_symlink_outside_bundle(self):
        with tempfile.TemporaryDirectory() as folder:
            outside = Path(folder) / "hdc"; outside.write_bytes(self.library.read_bytes())
            helper = self.root / "Contents/Resources/hdc/hdc"; helper.unlink(); helper.symlink_to(outside)
            with patch.object(runtime.subprocess, "check_output", side_effect=self.output):
                self.assertTrue(any("symlink escapes" in error for error in runtime.inspect_bundle(self.root, "x86_64")[0]))

    def test_resolves_rpath_and_ignores_library_install_id(self):
        def output(args, **kwargs):
            text = self.output(args, **kwargs)
            if args[0] == "lipo": return text
            if args[-1].endswith("HarmonyCast"):
                return text.replace("/usr/lib/libSystem.B.dylib", "@rpath/libavcodec.60.dylib")
            return text + "Load command 3\n cmd LC_ID_DYLIB\n name @rpath/unused-own-name.dylib (offset 24)\n"
        with patch.object(runtime.subprocess, "check_output", side_effect=output):
            self.assertEqual(runtime.inspect_bundle(self.root, "x86_64")[0], [])

    def test_auxiliary_executable_has_its_own_directory(self):
        def output(args, **kwargs):
            text = self.output(args, **kwargs)
            if args[0] != "lipo" and Path(args[-1]).name == "hdc":
                return text.replace("/usr/lib/libSystem.B.dylib", "@executable_path/libusb_shared.dylib")
            return text
        with patch.object(runtime.subprocess, "check_output", side_effect=output):
            self.assertEqual(runtime.inspect_bundle(self.root, "x86_64")[0], [])

    def test_inherits_rpath_through_dependency_chain(self):
        folder = self.library.parent / "Extras"; folder.mkdir()
        for name in ("libB.dylib", "libC.dylib"):
            (folder / name).write_bytes(self.library.read_bytes())
        def output(args, **kwargs):
            text = self.output(args, **kwargs)
            if args[0] == "lipo": return text
            name = Path(args[-1]).name
            dependency = {"HarmonyCast": "@rpath/libavcodec.60.dylib", "libavcodec.60.dylib": "@loader_path/Extras/libB.dylib", "libB.dylib": "@rpath/libC.dylib"}.get(name)
            if dependency: text = text.replace("/usr/lib/libSystem.B.dylib", dependency)
            if name == "libavcodec.60.dylib": text += "Load command 4\n cmd LC_RPATH\n path @loader_path/Extras (offset 12)\n"
            return text
        with patch.object(runtime.subprocess, "check_output", side_effect=output):
            self.assertEqual(runtime.inspect_bundle(self.root, "x86_64")[0], [])

    def test_inherited_external_rpath_cannot_hide_behind_local_library(self):
        for name in ("libA.dylib", "libB.dylib", "libC.dylib"):
            (self.library.parent / name).write_bytes(self.library.read_bytes())
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "libC.dylib").write_bytes(self.library.read_bytes())
            def output(args, **kwargs):
                text = self.output(args, **kwargs)
                if args[0] == "lipo": return text
                name = Path(args[-1]).name
                dependency = {"HarmonyCast": "@rpath/libA.dylib", "libA.dylib": "@loader_path/libB.dylib", "libB.dylib": "@rpath/libC.dylib"}.get(name)
                if dependency: text = text.replace("/usr/lib/libSystem.B.dylib", dependency)
                if name == "libB.dylib": text = text.replace("Load command 2\n cmd LC_RPATH\n path @executable_path/../Frameworks (offset 12)\n", "")
                if name == "libA.dylib": text = text.replace("@executable_path/../Frameworks", folder)
                return text
            with patch.object(runtime.subprocess, "check_output", side_effect=output):
                self.assertTrue(runtime.inspect_bundle(self.root, "x86_64")[0])


if __name__ == "__main__":
    unittest.main()
