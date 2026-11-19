"""Select a relocatable Python/GTK runtime and its transitive PE dependencies."""

import argparse
from collections import defaultdict, deque
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys


def pe_imports(path):
    """Read regular and delay-load DLL names from an AMD64 PE image."""
    data = path.read_bytes()

    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError(f"Truncated PE image: {path}")
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b"MZ":
        raise ValueError(f"Not a PE image: {path}")
    header = unpack("<I", 0x3C)[0]
    if data[header:header + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE signature: {path}")
    machine, count = unpack("<HH", header + 4)
    optional_size = unpack("<H", header + 20)[0]
    optional = header + 24
    if machine != 0x8664 or unpack("<H", optional)[0] != 0x20B or optional_size < 112:
        raise ValueError(f"Expected an AMD64 PE32+ image: {path}")
    image_base = unpack("<Q", optional + 24)[0]
    header_size = unpack("<I", optional + 60)[0]
    directory_count = unpack("<I", optional + 108)[0]
    sections = [unpack("<IIII", optional + optional_size + index * 40 + 8) for index in range(count)]

    def offset(rva):
        if 0 <= rva < min(header_size, len(data)):
            return rva
        for virtual_size, address, raw_size, raw in sections:
            if address <= rva < address + raw_size:
                result = raw + rva - address
                if result < len(data):
                    return result
        raise ValueError(f"Unmapped PE RVA {rva:#x}: {path}")

    def name(rva):
        start = offset(rva)
        end = data.find(b"\0", start, min(len(data), start + 512))
        if end < 0:
            raise ValueError(f"Unterminated DLL name: {path}")
        result = data[start:end].decode("ascii")
        if not result or "/" in result or "\\" in result:
            raise ValueError(f"Invalid imported DLL name: {path}")
        return result

    names = set()
    for index, stride in ((1, 20), (13, 32)):
        if directory_count <= index or 112 + (index + 1) * 8 > optional_size:
            continue
        rva, size = unpack("<II", optional + 112 + index * 8)
        if not rva or not size:
            continue
        for position in range(0, size, stride):
            descriptor = unpack("<" + "I" * (stride // 4), offset(rva + position))
            if not any(descriptor):
                break
            name_rva = descriptor[3] if index == 1 else descriptor[1]
            if index == 13 and not descriptor[0] & 1:
                name_rva -= image_base
            names.add(name(name_rva))
        else:
            raise ValueError(f"Unterminated PE import descriptors: {path}")
    return names


class RuntimePlan:
    """Track explicit resources and close native dependencies before copying."""

    def __init__(self, prefix, system):
        self.prefix = prefix.resolve()
        self.system = system
        self.files = defaultdict(set)
        self.pending = deque()
        self.native = set()
        self.binaries = {path.name.lower(): path for path in (self.prefix / "bin").iterdir() if path.is_file()}

    def add(self, path, reason):
        path = Path(path)
        if not path.is_file() or not path.resolve().is_relative_to(self.prefix):
            raise RuntimeError(f"Missing or external runtime file: {path}")
        relative = path.relative_to(self.prefix)
        if relative not in self.files and path.suffix.lower() in (".exe", ".dll", ".pyd"):
            self.pending.append(path)
        self.files[relative].add(reason)

    def tree(self, path, reason, excluded=()):
        if not path.is_dir():
            return
        for directory, names, files in os.walk(path, followlinks=False):
            names[:] = [name for name in names if name not in excluded and name != "__pycache__" and not name.startswith("config-")]
            for name in files:
                if not name.endswith((".pyc", ".pyo", ".a", ".lib", ".h", ".c", ".pdb")) and not name.startswith(("_test", "_ctypes_test", "xxlimited", "_tkinter")):
                    self.add(Path(directory) / name, reason)

    def dll(self, name, reason):
        key = name.lower()
        if key.startswith(("api-ms-", "ext-ms-")):
            return
        if key in self.binaries:
            self.add(self.binaries[key], reason)
        elif not (self.system / name).is_file():
            raise RuntimeError(f"Unresolved runtime DLL: {name} ({reason})")

    def close_dependencies(self):
        while self.pending:
            binary = self.pending.popleft()
            if binary in self.native:
                continue
            self.native.add(binary)
            for name in sorted(pe_imports(binary)):
                self.dll(name, f"PE:{binary.relative_to(self.prefix).as_posix()}")

    def copy(self, destination):
        sizes = {path: (self.prefix / path).stat().st_size for path in self.files}
        total = sum(sizes.values())
        print(f"Selected runtime: {len(sizes)} files, {total / 1048576:.1f} MiB, {len(self.native)} PE binaries", flush=True)
        for relative in sorted(self.files):
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(self.prefix / relative, target)
        manifest = [{"path": path.as_posix(), "bytes": sizes[path], "reasons": sorted(self.files[path])} for path in sorted(self.files)]
        (destination.parent / "runtime-files.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        print("Runtime files copied; no toolchain, headers, static libraries or unrelated site-packages.", flush=True)


def collect_typelibs(plan):
    """Load only GUI namespaces and use their registered dependency metadata."""
    import gi
    repository = gi.Repository.get_default()
    for namespace, version in (("Gtk", "4.0"), ("PangoCairo", "1.0"), ("GdkPixbuf", "2.0"), ("Gio", "2.0")):
        repository.require(namespace, version)
    for namespace, version in (("GdkWin32", "4.0"), ("GioWin32", "2.0")):
        if (plan.prefix / "lib/girepository-1.0" / f"{namespace}-{version}.typelib").is_file():
            repository.require(namespace, version)
    for namespace in repository.get_loaded_namespaces():
        path = Path(repository.get_typelib_path(namespace))
        plan.add(path, f"typelib:{namespace}")
        # Typelib хранит имена динамически загружаемых библиотек вне PE imports.
        for library in re.findall(rb"[A-Za-z0-9_+.-]+\.dll", path.read_bytes(), re.IGNORECASE):
            plan.dll(library.decode("ascii"), f"typelib:{namespace}")


def main():
    """Build a GUI-only file plan from the existing MSYS2 installation."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    prefix = args.prefix.resolve()
    if os.name != "nt" or Path(sys.prefix).resolve() != prefix:
        parser.error("Run with the source MSYS2 UCRT64 Python interpreter on Windows")
    if args.destination.resolve().is_relative_to(prefix):
        parser.error("The destination must be outside the source UCRT64 installation")
    if args.destination.exists() and any(args.destination.iterdir()):
        parser.error("The runtime destination must be empty")
    dll_directory = os.add_dll_directory(str(prefix / "bin"))
    os.environ["PYGI_DLL_PATH"] = str(prefix / "bin")
    os.environ["GI_TYPELIB_PATH"] = str(prefix / "lib/girepository-1.0")
    plan = RuntimePlan(prefix, Path(os.environ["SystemRoot"]) / "System32")
    plan.add(prefix / "bin/python.exe", "Python interpreter")
    plan.add(prefix / "bin/gdk-pixbuf-query-loaders.exe", "image loader cache")
    stdlib = prefix / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}"
    plan.tree(stdlib, "Python standard library", excluded=("site-packages", "test", "tests", "idlelib", "tkinter", "ensurepip", "venv"))
    for name in ("gi", "cairo"):
        package = stdlib / "site-packages" / name
        if not package.is_dir():
            raise RuntimeError(f"Missing Python GUI package: {package}")
        plan.tree(package, f"Python package:{name}", excluded=("tests",))
    collect_typelibs(plan)
    for loader in (prefix / "lib/gdk-pixbuf-2.0").glob("*/loaders/*.dll"):
        if any(format_name in loader.name.lower() for format_name in ("png", "svg", "jpeg")):
            plan.add(loader, "PNG/SVG/JPEG image loader")
    for relative in ("etc/fonts", "share/fontconfig", "share/glib-2.0/schemas", "share/gtk-4.0",
                     "share/icons/Adwaita", "share/icons/AdwaitaLegacy", "share/icons/hicolor",
                     "share/locale/ru/LC_MESSAGES", "share/licenses"):
        plan.tree(prefix / relative, f"GUI resource:{relative}", excluded=("tests", "demo", "demos"))
    for name in ("mime.cache", "globs", "globs2", "magic", "aliases", "subclasses", "icons", "generic-icons", "types"):
        path = prefix / "share/mime" / name
        if path.is_file():
            plan.add(path, "file chooser MIME metadata")
    plan.close_dependencies()
    plan.copy(args.destination)
    dll_directory.close()


if __name__ == "__main__":
    main()
