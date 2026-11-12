"""Manage explicitly owned project directories without depending on Resolve or GTK."""

import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
import uuid


PROJECT_MARKER = ".silentfilm-project.json"
JOB_MARKER = ".silentfilm-cache.json"


def component(name):
    """Make a bounded Unicode filename component while retaining recognizable names."""
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f\x7f]', "_", str(name)).strip(". ")
    return name.encode("utf-8")[:160].decode("utf-8", errors="ignore").rstrip(". ") or "Untitled"


def metadata(path):
    """Read an ownership marker without following marker symlinks."""
    if path.is_symlink():
        raise ValueError("Символическая ссылка вместо паспорта кэша")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict) or value.get("schema") != 1:
        raise ValueError("Неизвестный паспорт кэша")
    return value


def project_directories(root, project_id):
    """Find only direct child directories explicitly owned by the requested project."""
    root = Path(root).expanduser().resolve()
    if not root.exists():
        return []
    result = []
    for path in sorted(root.iterdir()):
        if path.is_symlink() or not path.is_dir():
            continue
        try:
            marker = metadata(path / PROJECT_MARKER)
        except (OSError, ValueError):
            continue
        if marker.get("project_id") == project_id:
            result.append(path)
    return result


def create_job(root, context, clip_name):
    """Create a unique generation beneath a stable, explicitly owned project directory."""
    identity = context.get("project_id")
    if not isinstance(identity, str) or not identity:
        raise ValueError("Resolve не сообщил идентификатор проекта")
    root = Path(root).expanduser().resolve()
    root.mkdir(parents=True, exist_ok=True)
    existing = project_directories(root, identity)
    if existing:
        parent = existing[0]
    else:
        suffix = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:12]
        parent = root / (component(context["project_name"]) + "_" + suffix)
        parent.mkdir(exist_ok=True)
        if parent.is_symlink():
            raise ValueError("Папка проекта не должна быть символической ссылкой")
        marker_path = parent / PROJECT_MARKER
        try:
            with marker_path.open("x", encoding="utf-8") as output:
                json.dump({"schema": 1, "project_id": identity, "project_name": context["project_name"]}, output, ensure_ascii=False)
        except FileExistsError:
            if metadata(marker_path).get("project_id") != identity:
                raise ValueError("Папка уже принадлежит другому проекту")
    name = component(clip_name)
    if not name.endswith("_cadence"):
        name += "_cadence"
    directory = Path(tempfile.mkdtemp(prefix=name + "_", dir=parent))
    (directory / JOB_MARKER).write_text(json.dumps({"schema": 1, "project_id": identity,
                                                  "generation_id": uuid.uuid4().hex,
                                                  "timeline_id": (context.get("target") or {}).get("timeline_id", context.get("timeline_id")),
                                                  "clip_name": clip_name}, ensure_ascii=False), encoding="utf-8")
    return directory


def beneath(path, directory):
    """Match video files, sequence patterns and individual frames inside one generation."""
    try:
        Path(path).expanduser().resolve().relative_to(directory)
        return True
    except (ValueError, TypeError):
        return False


def terminal_job(directory):
    """Allow cleanup only after native rendering has reached a terminal state."""
    provider = directory / "bake_state.json"
    if provider.exists():
        try:
            state = json.loads(provider.read_text(encoding="utf-8"))
            if state.get("method") == "frame_provider" and state.get("provider_active"):
                return False
        except (OSError, ValueError, TypeError, AttributeError):
            return False
    path = directory / "status.ini"
    if path.is_symlink() or not path.is_file():
        return False
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values.get("state") in ("complete", "cancelled", "error")


def candidates(root, project_id, used_paths, media, timeline_id=None, approved_paths=None):
    """List owned, terminal, unused generations; unknown host usage always preserves data."""
    found = []
    for parent in project_directories(root, project_id):
        for directory in sorted(parent.iterdir()):
            if approved_paths is not None and str(directory) not in approved_paths:
                continue
            if directory.is_symlink() or not directory.is_dir():
                continue
            try:
                marker = metadata(directory / JOB_MARKER)
                if marker.get("project_id") != project_id or not marker.get("generation_id") or not terminal_job(directory):
                    continue
                if timeline_id is not None and marker.get("timeline_id") != timeline_id:
                    continue
                # API хоста не показывает произвольные OFX file parameters: карту могли
                # привязать к другой копии клипа. Такие задания автоматически не удаляем.
                result_file = directory / "result.ini"
                if result_file.is_file() and any(line.strip() == "exposure_mode=ofx"
                                                for line in result_file.read_text(encoding="utf-8").splitlines()):
                    continue
                if any(beneath(path, directory) for path in used_paths):
                    continue
                linked = [item for item in media if beneath(item["path"], directory)]
                if any(item["usage"] != 0 for item in linked):
                    continue
                size = sum(path.stat().st_size for path in directory.rglob("*") if path.is_file() and not path.is_symlink())
                found.append({"path": str(directory), "generation_id": marker["generation_id"], "bytes": size})
            except (OSError, ValueError):
                continue
    return found


def remove_generation(entry):
    """Delete only the revalidated generation supplied by the host cleanup operation."""
    directory = Path(entry["path"])
    if directory.is_symlink() or metadata(directory / JOB_MARKER).get("generation_id") != entry["generation_id"]:
        raise ValueError("Папка кэша изменилась после проверки")
    shutil.rmtree(directory)
