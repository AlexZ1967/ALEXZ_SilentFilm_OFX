"""Keep GUI, Resolve and native child processes in separate platform environments."""

import os
from pathlib import Path
import shutil
import subprocess
import time


WINDOWS = os.name == "nt"


def atomic_replace(source, destination):
    """Publish a file, tolerating brief Windows sharing conflicts with status readers."""
    for attempt in range(101):
        try:
            os.replace(source, destination)
            return
        except OSError as error:
            if not WINDOWS or getattr(error, "winerror", None) not in (5, 32, 33) or attempt == 100:
                raise
            time.sleep(0.01)


def child_environment():
    """Remove GUI/runtime overrides before starting ResolvePython or the native helper."""
    environment = os.environ.copy()
    for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONHOME", "PYTHONPATH"):
        environment.pop(name, None)
    if WINDOWS:
        for name in ("PYGI_DLL_PATH", "GI_TYPELIB_PATH", "GTK_PATH", "GTK_DATA_PREFIX",
                     "GTK_EXE_PREFIX", "GSETTINGS_SCHEMA_DIR", "XDG_DATA_DIRS", "GDK_BACKEND", "GSK_RENDERER",
                     "GDK_PIXBUF_MODULEDIR", "GDK_PIXBUF_MODULE_FILE", "FONTCONFIG_FILE", "FONTCONFIG_PATH",
                     "GIO_MODULE_DIR", "GIO_EXTRA_MODULES"):
            environment.pop(name, None)
        native_path = environment.get("SILENTFILM_NATIVE_PATH")
        if native_path is not None:
            environment["PATH"] = native_path
        environment["PYTHONUTF8"] = "1"
        environment["PYTHONIOENCODING"] = "utf-8"
    return environment


def detached_options():
    """Detach workers from the launcher without terminating them when the GUI closes."""
    if WINDOWS:
        return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW}
    return {"start_new_session": True}


def resolve_python():
    """Locate the bundled Resolve 21.1+ interpreter, retaining explicit user overrides."""
    override = os.environ.get("SILENTFILM_RESOLVE_PYTHON")
    if override:
        return override
    if WINDOWS:
        return str(Path(os.environ.get("ProgramW6432") or os.environ.get("ProgramFiles") or "C:/Program Files")
                   / "Blackmagic Design/DaVinci Resolve/ResolvePython/ResolvePython.exe")
    return "/opt/resolve/bin/ResolvePython"


def settings_root():
    """Use LocalAppData on Windows and retain the established XDG location on Linux."""
    if WINDOWS:
        return Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData/Local")
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")


def ffmpeg_executable():
    """Find an explicitly configured encoder or the executable packaged with the app."""
    override = os.environ.get("SILENTFILM_FFMPEG")
    if override:
        return override
    if WINDOWS:
        bundled = Path(__file__).resolve().parents[1] / "bin/ffmpeg.exe"
        if bundled.is_file():
            return str(bundled)
        candidate = shutil.which("ffmpeg.exe", path=child_environment().get("PATH"))
        if not candidate:
            raise RuntimeError("Не найден ffmpeg.exe; используй полную установку Cadence")
        return candidate
    return "/usr/bin/ffmpeg"


def open_directory(directory):
    """Open the file manager without passing a user path to a command shell."""
    path = str(Path(directory).resolve())
    if WINDOWS:
        os.startfile(path)
    else:
        subprocess.Popen(["xdg-open", path], env=child_environment(), stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
