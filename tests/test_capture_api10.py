"""Exercise the real API 10 capture service against an API 10-only runtime."""
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from check_capture_api10 import validate


class CaptureBinaryTests(unittest.TestCase):
    def test_api10_imports_and_architectures(self):
        for arch, bits, machine in (("arm64", 64, 183), ("armv7", 32, 40)):
            with self.subTest(arch=arch):
                self.assertEqual(validate(ROOT / "resources/capture" / f"harmonycast_capture_api10_{arch}", bits, machine), 12)

    def test_reject_new_native_imports_and_libraries(self):
        original = (ROOT / "resources/capture/harmonycast_capture_api10_arm64").read_bytes()
        old = b"OH_AVScreenCapture_SetCallback"
        newer = b"OH_API12_" + b"x" * (len(old) - len(b"OH_API12_"))
        with tempfile.TemporaryDirectory(prefix="hc-api10-gate-") as folder:
            path = Path(folder) / "capture"
            for data in (original.replace(old, newer), original.replace(b"libc.so\0", b"oops.so\0")):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    validate(path, 64, 183)


@unittest.skipIf(os.name == "nt", "POSIX device service; Windows verifies the bundled ELF imports")
class CaptureRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get("HC_CAPTURE_TEST_CC") or shutil.which("clang") or shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler unavailable")
        cls.temporary = tempfile.TemporaryDirectory(prefix="hc-api10-test-")
        cls.binary = Path(cls.temporary.name) / "capture"
        import shlex
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DHC_CAPTURE_API10_TEST",
                        "-I" + str(ROOT / "tests/fixtures"), str(ROOT / "native/capture_server_api10.c"),
                        str(ROOT / "tests/fixtures/capture_api10_stubs.c"), "-o", str(cls.binary),
                        *shlex.split(os.environ.get("HC_CAPTURE_TEST_CFLAGS", ""))], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_capture(self, mode, extra=(), terminate=False):
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        environment = dict(os.environ, HC_API10_MODE=mode)
        process = subprocess.Popen([str(self.binary), "-p", str(port), "-w", "720", "-h", "1280", "-f", "60", *extra],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
        try:
            deadline = time.monotonic() + 3
            connection = None
            while connection is None:
                try:
                    connection = socket.create_connection(("127.0.0.1", port), timeout=0.8)
                except OSError:
                    if time.monotonic() > deadline or process.poll() is not None:
                        self.fail("capture listener did not start: " + process.communicate(timeout=2)[1].decode())
                    time.sleep(0.01)
            with connection:
                result = bytearray()
                try:
                    while len(result) < 48:
                        chunk = connection.recv(48 - len(result))
                        if not chunk:
                            break
                        result.extend(chunk)
                except (socket.timeout, TimeoutError, ConnectionResetError):
                    pass
                if terminate:
                    process.terminate()
            _, diagnostics = process.communicate(timeout=4)
            self.assertIn(process.returncode, (0, 6), diagnostics.decode())
            return bytes(result), diagnostics.decode()
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_rgba_and_stride(self):
        data, log = self.run_capture("rgba")
        self.assertEqual(struct.unpack("!8I", data[:32]), (0x48435232, 2, 2, 8, 16, 720, 1280, 0))
        self.assertEqual(data[32:], bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]))
        self.assertIn("leases=1/1 maps=1/1", log)

    def test_bgra_and_rgbx(self):
        for mode in ("bgra", "rgbx"):
            with self.subTest(mode=mode):
                data, _ = self.run_capture(mode)
                self.assertEqual(data[32:36], bytes([255, 0, 0, 255]))

    def test_resize_preserves_source_coordinates(self):
        data, _ = self.run_capture("rgba", ("-m", "1"))
        self.assertEqual(struct.unpack("!8I", data[:32]), (0x48435232, 1, 1, 4, 4, 720, 1280, 0))

    def test_fences_and_frame_rate_release_all_buffers(self):
        for mode in ("fence-ready", "rate-limit"):
            with self.subTest(mode=mode):
                data, log = self.run_capture(mode)
                self.assertEqual(len(data), 48)
                self.assertIn("maps=1/1", log)
        _, log = self.run_capture("rate-limit")
        self.assertIn("leases=2/2", log)

    def test_invalid_buffers_never_reach_the_screen(self):
        for mode in ("bad-format", "bad-stride", "oversize", "map-fail", "map-null", "fence-timeout", "no-buffer"):
            with self.subTest(mode=mode):
                data, _ = self.run_capture(mode)
                self.assertEqual(data, b"")

    def test_capture_setup_failures(self):
        for mode in ("create-fail", "callback-fail", "init-fail", "mic-fail", "start-fail", "error-callback"):
            with self.subTest(mode=mode):
                data, _ = self.run_capture(mode)
                self.assertEqual(data, b"")

    def test_invalid_arguments(self):
        for args in (("-p", "1"), ("-p", "1234", "-w", "oops"), ("-p", "1234", "-w")):
            self.assertEqual(subprocess.run([str(self.binary), *args], capture_output=True).returncode, 2)

    def test_stop_during_live_capture(self):
        data, log = self.run_capture("rgba", terminate=True)
        self.assertEqual(len(data), 48)
        self.assertIn("leases=1/1 maps=1/1", log)


if __name__ == "__main__":
    unittest.main()
