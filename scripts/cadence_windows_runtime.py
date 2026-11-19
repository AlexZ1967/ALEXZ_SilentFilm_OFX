"""Bootstrap a relocated UCRT64 GTK runtime before importing the GUI."""

import argparse
import base64
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile


def configure(prefix, cache):
    """Regenerate path-sensitive loader metadata at the installed location."""
    os.environ["GTK_EXE_PREFIX"] = str(prefix)
    os.environ["GTK_DATA_PREFIX"] = str(prefix)
    fonts = prefix / "etc/fonts/fonts.conf"
    if fonts.is_file():
        os.environ["FONTCONFIG_FILE"] = str(fonts)
        os.environ["FONTCONFIG_PATH"] = str(fonts.parent)
    loaders = sorted((prefix / "lib/gdk-pixbuf-2.0").glob("*/loaders"))
    if loaders:
        if len(loaders) != 1:
            raise RuntimeError("Ambiguous bundled gdk-pixbuf loader directories")
        os.environ["GDK_PIXBUF_MODULEDIR"] = str(loaders[0])
        query = prefix / "bin/gdk-pixbuf-query-loaders.exe"
        result = subprocess.run([str(query)], check=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=60)
        metadata = cache / "loaders.cache"
        metadata.write_bytes(result.stdout)
        os.environ["GDK_PIXBUF_MODULE_FILE"] = str(metadata)


def check_runtime(prefix):
    """Check isolated Python, GTK, fonts and image decoding after relocation."""
    if Path(sys.prefix).resolve() != prefix.resolve():
        raise RuntimeError(f"Python escaped the bundled prefix: {sys.prefix}")
    for entry in sys.path:
        if entry and not Path(entry).resolve().is_relative_to(prefix.resolve()):
            raise RuntimeError(f"Python retained an external import path: {entry}")
    import gi
    gi.require_version("Gtk", "4.0")
    gi.require_version("Pango", "1.0")
    gi.require_version("PangoCairo", "1.0")
    gi.require_version("GdkPixbuf", "2.0")
    gi.require_version("Gio", "2.0")
    from gi.repository import Gtk, Pango, PangoCairo, GdkPixbuf, GLib, Gio
    Gtk.init()
    families = PangoCairo.FontMap.get_default().list_families()
    if not families:
        raise RuntimeError("Bundled Pango cannot find any fonts")
    png = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=")
    image = GdkPixbuf.Pixbuf.new_from_stream(Gio.MemoryInputStream.new_from_bytes(GLib.Bytes.new(png)), None)
    if (image.get_width(), image.get_height()) != (1, 1):
        raise RuntimeError("Bundled image decoder returned an invalid image")
    window = Gtk.Window()
    window.destroy()
    print(f"Runtime OK: Python {sys.version.split()[0]}, GTK {Gtk.get_major_version()}.{Gtk.get_minor_version()}, "
          f"Pango {Pango.version_string()}, fonts={len(families)}, prefix={prefix}")


def main():
    """Keep runtime caches alive for the lifetime of the GUI process."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--app-script", type=Path)
    args, remaining = parser.parse_known_args()
    prefix = args.prefix.resolve()
    dll_directory = os.add_dll_directory(str(prefix / "bin")) if hasattr(os, "add_dll_directory") else None
    cache_root = Path(os.environ["LOCALAPPDATA"]) / "ALEXZSilentFilm/cadence/runtime-cache"
    cache_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gtk-", dir=cache_root) as directory:
        configure(prefix, Path(directory))
        if args.check:
            check_runtime(prefix)
        else:
            if args.app_script is None:
                parser.error("--app-script is required without --check")
            sys.path.insert(0, str(args.app_script.resolve().parent))
            sys.argv = [str(args.app_script), *remaining]
            runpy.run_path(str(args.app_script), run_name="__main__")
    if dll_directory is not None:
        dll_directory.close()


if __name__ == "__main__":
    main()
