"""Check PE dependency closure and exclusion of development files without Windows."""

from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts/windows"))
from bundle_runtime import pe_imports, RuntimePlan, collect_typelibs


def image(imports=(), delayed=()):
    """Create a small AMD64 PE fixture with distinct virtual and raw addresses."""
    data = bytearray(0x1000)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, 1)
    struct.pack_into("<H", data, 0x94, 240)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<Q", data, optional + 24, 0x180000000)
    struct.pack_into("<I", data, optional + 60, 0x200)
    struct.pack_into("<I", data, optional + 108, 16)
    struct.pack_into("<IIII", data, optional + 240 + 8, 0xE00, 0x1000, 0xE00, 0x200)
    name_offset = 0x900
    for index, raw, stride, names in ((1, 0x200, 20, imports), (13, 0x500, 32, delayed)):
        if not names:
            continue
        struct.pack_into("<II", data, optional + 112 + index * 8, raw + 0xE00, (len(names) + 1) * stride)
        for number, name in enumerate(names):
            encoded = name.encode("ascii") + b"\0"
            data[name_offset:name_offset + len(encoded)] = encoded
            if index == 1:
                struct.pack_into("<I", data, raw + number * stride + 12, name_offset + 0xE00)
            else:
                struct.pack_into("<II", data, raw + number * stride, 1, name_offset + 0xE00)
            name_offset += len(encoded)
    return data


class PeDependencies(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.prefix = self.root / "ucrt64"
        (self.prefix / "bin").mkdir(parents=True)
        self.system = self.root / "System32"
        self.system.mkdir()

    def binary(self, name, imports=(), delayed=()):
        path = self.prefix / "bin" / name
        path.write_bytes(image(imports, delayed))
        return path

    def test_regular_and_delayed_imports_use_section_rvas(self):
        path = self.binary("python.exe", ("python314.dll", "KERNEL32.dll"), ("lazy.dll",))
        self.assertEqual(pe_imports(path), {"python314.dll", "KERNEL32.dll", "lazy.dll"})

    def test_corrupt_or_wrong_architecture_is_rejected(self):
        path = self.binary("wrong.dll")
        content = bytearray(image())
        struct.pack_into("<H", content, 0x84, 0x14C)
        path.write_bytes(content)
        with self.assertRaisesRegex(ValueError, "AMD64"):
            pe_imports(path)
        path.write_bytes(b"MZ")
        with self.assertRaisesRegex(ValueError, "Truncated"):
            pe_imports(path)

    def test_closure_handles_cycles_and_does_not_include_other_tools(self):
        entry = self.binary("python.exe", ("A.dll", "KERNEL32.dll", "api-ms-win-core-file-l1-1-0.dll"))
        self.binary("a.dll", ("b.dll",))
        self.binary("b.dll", (), ("a.dll",))
        self.binary("gcc.exe")
        (self.system / "KERNEL32.dll").write_bytes(b"system")
        plan = RuntimePlan(self.prefix, self.system)
        plan.add(entry, "interpreter")
        plan.close_dependencies()
        self.assertEqual({str(path) for path in plan.files}, {str(Path("bin") / name) for name in ("python.exe", "a.dll", "b.dll")})

    def test_missing_dependency_fails_instead_of_using_unrelated_path(self):
        entry = self.binary("python.exe", ("missing.dll",))
        plan = RuntimePlan(self.prefix, self.system)
        plan.add(entry, "interpreter")
        with self.assertRaisesRegex(RuntimeError, "missing.dll"):
            plan.close_dependencies()

    def test_tree_keeps_python_but_excludes_tests_caches_and_static_libraries(self):
        stdlib = self.prefix / "lib/python3.14"
        for relative in ("json/__init__.py", "test/test_big.py", "__pycache__/os.pyc", "libpython.a", "config-3.14/Makefile"):
            path = stdlib / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture", encoding="utf-8")
        plan = RuntimePlan(self.prefix, self.system)
        plan.tree(stdlib, "stdlib", excluded=("test",))
        self.assertEqual(set(plan.files), {Path("lib/python3.14/json/__init__.py")})

    def test_glib_win32_is_included_without_a_declared_gtk_dependency(self):
        directory = self.prefix / "lib/girepository-1.0"
        directory.mkdir(parents=True)
        (directory / "GLibWin32-2.0.typelib").write_bytes(b"")
        loaded = {}

        def require(namespace, version):
            path = directory / f"{namespace}-{version}.typelib"
            path.touch()
            loaded[namespace] = str(path)

        repository = Mock()
        repository.require.side_effect = require
        repository.get_loaded_namespaces.side_effect = lambda: list(loaded)
        repository.get_typelib_path.side_effect = lambda namespace: loaded[namespace]
        gi = SimpleNamespace(Repository=SimpleNamespace(get_default=lambda: repository))
        plan = RuntimePlan(self.prefix, self.system)
        with patch.dict(sys.modules, {"gi": gi}):
            collect_typelibs(plan)
        repository.require.assert_any_call("GLibWin32", "2.0")
        self.assertIn(Path("lib/girepository-1.0/GLibWin32-2.0.typelib"), plan.files)


if __name__ == "__main__":
    unittest.main()
