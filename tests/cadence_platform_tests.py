"""Check Windows process isolation and real TCP provider lifecycle without GTK or Resolve."""

import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cadence_platform as platform
import cadence_frame_provider as provider


class Environments(unittest.TestCase):
    def test_windows_child_does_not_inherit_gtk_libraries(self):
        original = {"PATH": "gtk;native", "SILENTFILM_NATIVE_PATH": "native", "PYTHONHOME": "wrong",
                    "GI_TYPELIB_PATH": "gtk", "PYGI_DLL_PATH": "gtk", "GSETTINGS_SCHEMA_DIR": "gtk"}
        with patch.object(platform, "WINDOWS", True), patch.dict(os.environ, original, clear=True):
            child = platform.child_environment()
            self.assertEqual(child["PATH"], "native")
            self.assertEqual(child["PYTHONIOENCODING"], "utf-8")
            for key in ("PYTHONHOME", "GI_TYPELIB_PATH", "PYGI_DLL_PATH", "GSETTINGS_SCHEMA_DIR"):
                self.assertNotIn(key, child)
            self.assertEqual(os.environ["PATH"], "gtk;native")

    def test_windows_runtime_paths_are_separate(self):
        with patch.object(platform, "WINDOWS", True), patch.dict(os.environ, {"LOCALAPPDATA": "C:/Users/A/AppData/Local"}, clear=True):
            self.assertEqual(platform.settings_root(), Path("C:/Users/A/AppData/Local"))
            self.assertTrue(platform.resolve_python().replace("\\", "/").endswith("ResolvePython/ResolvePython.exe"))

    def test_detach_is_platform_specific(self):
        with patch.object(platform, "WINDOWS", False):
            self.assertEqual(platform.detached_options(), {"start_new_session": True})
        with patch.object(platform, "WINDOWS", True), patch.object(platform.subprocess, "CREATE_NEW_PROCESS_GROUP", 512, create=True), patch.object(platform.subprocess, "CREATE_NO_WINDOW", 134217728, create=True):
            self.assertEqual(platform.detached_options(), {"creationflags": 134218240})

    def test_atomic_replace_retries_only_transient_windows_errors(self):
        conflict = OSError("sharing violation")
        conflict.winerror = 32
        with patch.object(platform, "WINDOWS", True), patch.object(platform.os, "replace", side_effect=[conflict, None]) as replace, patch.object(platform.time, "sleep"):
            platform.atomic_replace("source", "destination")
            self.assertEqual(replace.call_count, 2)
        with patch.object(platform, "WINDOWS", False), patch.object(platform.os, "replace", side_effect=conflict), patch.object(platform.time, "sleep") as sleep:
            with self.assertRaises(OSError):
                platform.atomic_replace("source", "destination")
            sleep.assert_not_called()


class TcpProvider(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.errors = []
        self.state = {"width": 7, "height": 5, "fps": 25, "frame_count": 3}
        self.png = b"\x89PNG\r\n\x1a\n" + b"\0" * 8 + struct.pack(">II", 7, 5) + b"\x10\x02"
        self.exports = []
        def export(index):
            self.exports.append(index)
            return self.png
        def serve():
            try:
                provider.serve_frames(self.directory, self.state, export, transport="tcp")
            except Exception as error:
                self.errors.append(error)
        self.worker = threading.Thread(target=serve, daemon=True)
        self.worker.start()
        self.addCleanup(self.stop)
        deadline = time.monotonic() + 5
        metadata = self.directory / "frame_source.ini"
        while not metadata.exists():
            if self.errors or time.monotonic() > deadline:
                self.fail("TCP provider did not publish metadata: " + str(self.errors))
            time.sleep(0.01)
        self.metadata = dict(line.split("=", 1) for line in metadata.read_text(encoding="utf-8").splitlines() if "=" in line)

    def stop(self):
        (self.directory / "cancel.request").write_text("cancel\n")
        self.worker.join(timeout=3)
        self.assertFalse(self.worker.is_alive(), "Provider ignored cancellation")

    @staticmethod
    def receive(connection, count):
        data = b""
        while len(data) < count:
            chunk = connection.recv(count - len(data))
            if not chunk:
                raise RuntimeError("Unexpected provider disconnect")
            data += chunk
        return data

    def connect(self, token=None):
        self.assertEqual(self.metadata["socket_host"], "127.0.0.1")
        connection = socket.create_connection(("127.0.0.1", int(self.metadata["socket_port"])), timeout=3)
        self.addCleanup(connection.close)
        connection.sendall((token or self.metadata["token"]).encode("ascii"))
        return connection

    def test_tcp_auth_cache_and_frame_error(self):
        connection = self.connect()
        self.assertEqual(self.receive(connection, 4), b"SF01")
        for index in (1, 1, 2):
            connection.sendall(struct.pack("<Q", index))
            status, count = struct.unpack("<BQ", self.receive(connection, 9))
            self.assertEqual(status, 0)
            self.assertEqual(self.receive(connection, count), self.png)
        self.assertEqual(self.exports, [1, 2])
        connection.sendall(struct.pack("<Q", 3))
        status, count = struct.unpack("<BQ", self.receive(connection, 9))
        self.assertEqual(status, 1)
        self.assertTrue(self.receive(connection, count))
        connection.close()
        self.worker.join(timeout=3)
        self.assertFalse(self.worker.is_alive())
        self.assertEqual(self.state["state"], "error")

    def test_reject_wrong_token(self):
        connection = self.connect("0" * 64)
        self.assertEqual(connection.recv(4), b"")
        self.worker.join(timeout=3)
        self.assertTrue(self.errors)
        self.assertEqual(self.exports, [])

    def test_cancel_while_waiting_for_client(self):
        self.stop()
        self.assertEqual(self.state["state"], "cancelled")
        self.assertFalse(json.loads((self.directory / "bake_state.json").read_text(encoding="utf-8"))["provider_active"])


if __name__ == "__main__":
    unittest.main()
