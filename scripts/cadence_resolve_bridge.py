"""Exchange JSON with Resolve from its bundled external Python interpreter."""

import datetime
import base64
import json
import math
import os
from pathlib import Path
import re
import sys
import subprocess
import traceback
import time
import queue
import threading


# ResolvePython может не добавлять папку запущенного скрипта в пути импорта.
_BRIDGE_DIRECTORY = str(Path(__file__).resolve().parent)
if _BRIDGE_DIRECTORY not in sys.path:
    sys.path.insert(0, _BRIDGE_DIRECTORY)


def check(value, message):
    """Raise a readable error when a host operation fails."""
    if not value:
        raise RuntimeError(message)
    return value


def sequence(value):
    """Normalize lists and integer-keyed host tables."""
    return list(value.values()) if isinstance(value, dict) else list(value or [])


def decimal(value):
    """Read a host frame rate with optional suffixes."""
    match = re.search(r"\d+(?:[.,]\d+)?", str(value))
    return float(match[0].replace(",", ".")) if match else 0.0


def host_fps(fps):
    """Use Resolve's accepted display labels for fractional NTSC frame rates."""
    for nominal, label in ((24000 / 1001, "23.976"), (30000 / 1001, "29.97"),
                           (48000 / 1001, "47.952"), (60000 / 1001, "59.94"),
                           (120000 / 1001, "119.88")):
        if abs(fps - nominal) < 0.0005:
            return label
    return f"{fps:g}"


def cadence_name(source_name):
    """Build a portable Unicode basename from a clip label while retaining its identity."""
    name = str(source_name).strip()
    if Path(name).suffix.lower() in (".mov", ".mp4", ".mkv", ".mxf", ".avi", ".webm", ".m4v", ".mts", ".m2ts", ".tif", ".tiff"):
        name = name[: -len(Path(name).suffix)]
    name = re.sub(r'[<>:"/\\|?*%\x00-\x1f\x7f]', "_", name).strip(". ") or "clip"
    if name.endswith("_cadence"):
        name = name[:-8]
    name = name.encode("utf-8")[:190].decode("utf-8", errors="ignore").rstrip(". ") or "clip"
    return name + "_cadence"


def read_ini(path):
    """Read native flat key/value metadata without interpolation."""
    result = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            check(key.strip() not in result, "Повторяющийся ключ паспорта")
            result[key.strip()] = value.strip()
    return result


def project_context(project):
    """Capture project identity and current playback rate."""
    timeline = project.GetCurrentTimeline()
    settings = timeline.GetSettings() if timeline else project.GetSettings()
    return {"project_id": project.GetUniqueId(), "project_name": project.GetName(),
            "timeline_id": timeline.GetUniqueId() if timeline else None,
            "timeline_name": timeline.GetName() if timeline else None,
            "timeline_fps": (decimal(settings.get("timelineFrameRate", "")) or None) if timeline else None,
            "output_fps": decimal(settings.get("timelineFrameRate", "25")) or 25}


def item_snapshot(item):
    """Capture source extents and edit identity without retaining host handles."""
    media = check(item.GetMediaPoolItem(), "Нужен обычный файловый видеоклип, не генератор/compound")
    path = media.GetClipProperty("File Path")
    check(path and Path(path).is_file(), "У выбранного клипа нет доступного исходного файла")
    track = sequence(item.GetTrackTypeAndIndex())
    check(len(track) == 2 and track[0] == "video" and item.GetType() == "video", "Выбери исходный видеоклип")
    speed = item.GetSpeed() or {}
    start, end = float(item.GetStart()), float(item.GetEnd())
    source_start, source_end = int(item.GetSourceStartFrame()), int(item.GetSourceEndFrame())
    check(end > start and start.is_integer() and end.is_integer() and source_start >= 0 and source_end >= 0,
          "Resolve вернул некорректный диапазон выбранного клипа")
    return {"item_id": item.GetUniqueId(), "media_id": media.GetUniqueId(), "path": str(Path(path).resolve()),
            "name": item.GetName(), "track_index": int(track[1]), "start": int(start), "end": int(end),
            "source_start": source_start, "source_end": source_end,
            "fps": decimal(media.GetClipProperty("FPS")), "speed": speed.get("Percentage")}


def timeline_sources(project):
    """Snapshot selected video instances in deterministic timeline order, ignoring linked audio."""
    timeline = check(project.GetCurrentTimeline(), "Открой таймлайн и выдели видеоклипы")
    selected = [item for item in sequence(timeline.GetSelectedClips()) if item.GetType() == "video"]
    check(selected, "Выдели один или несколько видеоклипов на таймлайне")
    context = project_context(project)
    items, seen = [], set()
    for item in selected:
        if item.GetUniqueId() in seen:
            continue
        seen.add(item.GetUniqueId())
        target = item_snapshot(item)
        target.update(timeline_id=timeline.GetUniqueId(), timeline_name=timeline.GetName())
        count = target["end"] - target["start"]
        source = {"path": target["path"], "name": target["name"], "fps": context["output_fps"],
                  "clip_id": target["media_id"], "marks": {"in": 0, "out": count - 1}, "requires_bake": True}
        items.append(dict(context, target=target, source=source))
    items.sort(key=lambda value: (value["target"]["start"], value["target"]["track_index"], value["target"]["item_id"]))
    return {"items": items}


def timeline_source(project):
    """Retain the single-item API for historical callers."""
    items = timeline_sources(project)["items"]
    check(len(items) == 1, "Выдели ровно один видеоклип на таймлайне")
    return items[0]


def raw_source_range(target):
    """Return the unprocessed source interval using source-frame coordinates, end exclusive."""
    start, end, fps = int(target["source_start"]), int(target["source_end"]), float(target["fps"])
    check(start >= 0 and end > start and math.isfinite(fps) and fps > 0,
          "Resolve не сообщил корректный In/Out исходного файла")
    return {"input": target["path"], "start_frame": start, "frame_count": end - start,
            "expected_source_fps": fps}


def raw_timeline_frame(target, offset):
    """Map a timeline-frame center into the trimmed source interval without changing duration."""
    source = raw_source_range(target)
    count = int(target["end"] - target["start"])
    check(count > 0 and 0 <= offset < count, "Кадр вне монтажного диапазона")
    # SourceEndFrame is exclusive. Integer arithmetic avoids rounding drift.
    return source["start_frame"] + min(source["frame_count"] - 1,
                                      ((2 * offset + 1) * source["frame_count"]) // (2 * count))


def native_raw_request(target, timeline_fps):
    """Read raw media directly while exposing exactly the selected timeline frame grid."""
    source = raw_source_range(target)
    frames = int(target["end"] - target["start"])
    check(frames > 0 and math.isfinite(timeline_fps) and timeline_fps > 0, "Некорректная временная сетка таймлайна")
    return {"input": source["input"], "start_frame": 0, "frame_count": frames,
            "expected_source_fps": source["expected_source_fps"], "raw_start_frame": source["start_frame"],
            "raw_frame_count": source["frame_count"], "timeline_input_fps": timeline_fps}


def timeline_positions(timeline, exclude=None):
    """Record all edit positions so replacing one item cannot silently move the rest."""
    positions = {}
    for kind in ("video", "audio", "subtitle"):
        for track in range(1, int(timeline.GetTrackCount(kind)) + 1):
            for item in sequence(timeline.GetItemListInTrack(kind, track)):
                identity = item.GetUniqueId()
                if identity != exclude:
                    positions[identity] = [kind, track, float(item.GetStart(True)), float(item.GetEnd(True))]
    return positions


def replacement_target(project, target, count, fps, check_space=True):
    """Validate target identity, available space and playback FPS before making any edit."""
    timeline = check(project.GetCurrentTimeline(), "Открой исходный таймлайн")
    check(timeline.GetUniqueId() == target["timeline_id"], "Открой исходный таймлайн: " + target["timeline_name"])
    check(abs(decimal(timeline.GetSettings().get("timelineFrameRate")) - fps) < 0.002,
          "Для замены FPS показа должен совпадать с FPS исходного таймлайна")
    check(not check_space or not timeline.GetIsTrackLocked("video", target["track_index"]), "Дорожка выбранного клипа заблокирована")
    items = sequence(timeline.GetItemListInTrack("video", target["track_index"]))
    matches = [item for item in items if item.GetUniqueId() == target["item_id"]]
    check(len(matches) == 1, "Исходный клип удалён или перемещён на другую дорожку; выбери его заново")
    original = matches[0]
    current = item_snapshot(original)
    for key in ("item_id", "media_id", "path", "track_index", "start", "end", "source_start", "source_end", "fps", "speed"):
        check(current[key] == target[key], "Исходный клип изменился после выбора; выбери его заново")
    for other in items:
        if other.GetUniqueId() == target["item_id"]:
            continue
        start, end = float(other.GetStart()), float(other.GetEnd())
        check(not (start < target["end"] and end > target["start"]),
              "У исходного клипа есть переход или перекрытие; убери его перед заменой")
        check(not check_space or not (start < target["start"] + count and end > target["start"]),
              "Результат длиннее свободного места до следующего клипа. Позиции монтажа сохранены; замена не выполнена")
    return timeline, original


def atomic_json(path, data):
    """Publish durable state without exposing half-written JSON to GUI polling."""
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    temporary.replace(path)


def find_timeline(project, identity):
    """Locate a saved host object by UUID after a bridge process has ended."""
    for index in range(1, int(project.GetTimelineCount()) + 1):
        timeline = project.GetTimelineByIndex(index)
        if timeline.GetUniqueId() == identity:
            return timeline
    return None


def bake_profile(project, timeline, transfer_override=None):
    """Require a supported SDR working signal rather than guessing Log/HDR conversion."""
    settings = dict(project.GetSettings())
    timeline_settings = dict(timeline.GetSettings())
    inherited_fields = {"colorScienceMode", "colorSpaceTimeline", "colorSpaceTimelineGamma",
                        "separateColorSpaceAndGamma", "timelineResolutionWidth", "timelineResolutionHeight",
                        "timelinePixelAspectRatio", "timelineFrameRate"}
    for key, value in timeline_settings.items():
        if value not in (None, "") or key not in inherited_fields:
            settings[key] = value
    # Некоторые версии SDK возвращают часть color metadata только через прежний getter.
    def direct_color(obj, key):
        getter = getattr(obj, "GetSetting", None)
        if callable(getter):
            try:
                value = getter(key)
                if isinstance(value, str) and value.strip():
                    return value
            except Exception:
                pass
        return ""
    if not settings.get("colorSpaceTimelineGamma"):
        direct_gamma = direct_color(timeline, "colorSpaceTimelineGamma") or direct_color(project, "colorSpaceTimelineGamma")
        if direct_gamma:
            settings["colorSpaceTimelineGamma"] = direct_gamma
    check(settings.get("colorScienceMode") == "davinciYRGB",
          "Автоматический bake пока поддерживает DaVinci YRGB SDR Rec.709. Color Managed/ACES требуют отдельного цветового контракта")
    space = str(settings.get("colorSpaceTimeline") or "").lower().replace(" ", "")
    gamma = str(settings.get("colorSpaceTimelineGamma") or "").lower().replace(" ", "")
    combined = {"rec.709gamma2.4": "gamma24", "rec709gamma2.4": "gamma24", "srgb": "srgb",
                "rec.709srgb": "srgb", "rec709srgb": "srgb", "rec.709linear": "linear", "rec709linear": "linear"}
    transfer = combined.get(space)
    if transfer:
        if str(settings.get("separateColorSpaceAndGamma")) == "1" and gamma:
            check({"gamma2.4": "gamma24", "srgb": "srgb", "linear": "linear"}.get(gamma) == transfer,
                  "Противоречивые настройки working space/gamma; проверь настройки цвета")
    else:
        check(space in ("rec.709", "rec709", "rec.709(scene)"), "Для bake нужен SDR Rec.709 timeline working space")
        transfer = {"gamma2.4": "gamma24", "srgb": "srgb", "linear": "linear"}.get(gamma)
    if not transfer and not gamma:
        for obj in (timeline, project):
            direct_space = direct_color(obj, "colorSpaceTimeline").lower().replace(" ", "")
            if direct_space in combined:
                transfer = combined[direct_space]
                break
            check(not direct_space or direct_space in ("rec.709", "rec709", "rec.709(scene)"),
                  "Resolve сообщил неподдержанную рабочую гамму/пространство: " + direct_space + ". Выбор другой гаммы не заменяет CST")
    if transfer_override:
        check(transfer_override in ("srgb", "gamma24", "linear"), "Неподдержанная явно заданная гамма bake")
        check(not transfer or transfer == transfer_override,
              "Выбранная гамма не совпадает с настройками Resolve. Выбор гаммы не является преобразованием CST")
        if not transfer:
            check(not gamma, "Гамма Resolve не поддержана: " + gamma + ". Её нельзя заменить указанием другой гаммы без CST")
            transfer = transfer_override
    check(transfer, "Resolve не сообщил поддержанную гамму bake: " +
          repr({"space": settings.get("colorSpaceTimeline"), "gamma": settings.get("colorSpaceTimelineGamma"),
                "separate": settings.get("separateColorSpaceAndGamma")}) +
          ". В поле «Гамма входа» выбери фактическую рабочую гамму из настроек Resolve: Gamma 2.4 / sRGB / Linear Rec.709")
    # Подстрока "lut" есть и в Resolution: проверяем имена полей,
    # а не совпадения внутри названий разрешения/режимов интерполяции.
    global_lut_keys = {prefix + suffix
                       for prefix in ("coloroutputlut", "colortimelinelut", "outputlut", "timelinelut")
                       for suffix in ("", "name", "path")}
    for key, value in settings.items():
        normalized_key = re.sub(r"[^a-z0-9]", "", key.lower())
        if normalized_key in global_lut_keys:
            check(str(value).lower() in ("", "none", "no lut selected", "0"),
                  "Глобальный output/timeline LUT нельзя запечь повторно; автоматический clip-only bake с ним пока не поддержан")
    aspect = str(settings.get("timelinePixelAspectRatio") or "").strip().lower()
    # Имя настройки хранится для диагностики; фактическое численное SAR берём из готового файла.
    return {"input_transfer": transfer, "width": int(settings["timelineResolutionWidth"]),
            "height": int(settings["timelineResolutionHeight"]),
            "fps": decimal(settings["timelineFrameRate"]), "timeline_pixel_aspect": aspect or None}


def bake_cleanup(project, state):
    """Remove only this operation's render job and temporary timeline."""
    warnings = []
    if state.get("render_job_id"):
        try:
            if not project.DeleteRenderJob(state["render_job_id"]):
                warnings.append("Не удалено служебное задание " + state["render_job_id"])
        except Exception as error:
            warnings.append(str(error))
    try:
        temporary = find_timeline(project, state.get("temporary_id")) if state.get("temporary_id") else None
        if temporary:
            current = project.GetCurrentTimeline()
            if current and current.GetUniqueId() == temporary.GetUniqueId():
                original = find_timeline(project, state["original_id"])
                check(original and project.SetCurrentTimeline(original), "Не удалось выйти из временного таймлайна")
            if not project.GetMediaPool().DeleteTimelines([temporary]):
                warnings.append("Не удалён служебный таймлайн " + temporary.GetName())
    except Exception as error:
        warnings.append(str(error))
    state["cleanup_warnings"] = warnings


def restore_deliver(project, state):
    """Restore the saved preset, codec and render mode before starting the owned job."""
    check(project.LoadRenderPreset(state["restore_preset"]),
          "Не удалось восстановить настройки Deliver: " + state["restore_preset"])
    codec = state["old_codec"]
    if codec.get("format") and codec.get("codec"):
        check(project.SetCurrentRenderFormatAndCodec(codec["format"], codec["codec"]), "Не удалось восстановить исходный codec Deliver")
    check(project.SetCurrentRenderMode(state["old_mode"]), "Не удалось восстановить render mode")


def reported_bool(value):
    """Normalize explicit SDK flags without treating missing metadata as False."""
    if isinstance(value, bool):
        return value
    if value in (0, 1):
        return bool(value)
    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in ("true", "1", "yes", "on"):
            return True
        if normalized in ("false", "0", "no", "off"):
            return False
    return None


def inspect_baked_media(state, path):
    """Verify the decoded input format with the same FFmpeg reader used for cadence."""
    environment = os.environ.copy()
    for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONHOME", "PYTHONPATH"):
        environment.pop(name, None)
    result = subprocess.run([state["helper"], "--inspect", str(path)], capture_output=True,
                            text=True, encoding="utf-8", errors="replace", env=environment, timeout=60)
    check(result.returncode == 0, "Не удалось проверить intermediate. Пересобери silentfilm_cadence. " + result.stderr[-1200:])
    info = json.loads(result.stdout)
    check(info["width"] == state["width"] and info["height"] == state["height"]
          and abs(float(info["fps"]) - state["fps"]) < 0.002, "Параметры отрендеренного файла не совпали с заданием")
    check(not state.get("require_alpha", True) or info.get("has_alpha") is True,
          "В декодированном intermediate нет alpha. Каденс не запущен. Для непрозрачного видео можно явно отключить «Сохранять прозрачность»")
    return info


def direct_start(project, request):
    """Start a detached provider of processed frames requested by the native renderer."""
    directory = Path(request["job"]).resolve()
    check(directory.is_dir() and not (directory / "bake_state.json").exists(), "Для подготовки нужно новое задание")
    helper = Path(request.get("helper", ""))
    check(helper.is_file(), "Не найден native helper; собери приложение")
    target = request["target"]
    fps = project_context(project)["output_fps"]
    timeline, item = replacement_target(project, target, target["end"] - target["start"], fps, check_space=False)
    check(callable(getattr(item, "ExportFrame", None)), "Resolve не предоставляет TimelineItem.ExportFrame")
    profile = bake_profile(project, timeline, request.get("transfer_override"))
    raw = bool(request.get("ignore_processing", False))
    check(not raw, "Use Raw Source должен использовать нативное чтение исходника")
    state = {"method": "frame_provider", "state": "rendering", "project_id": project.GetUniqueId(),
             "ignore_processing": raw,
             "target": target, "helper": str(helper.resolve()), "require_alpha": bool(request.get("require_alpha", False)),
             "frame_count": target["end"] - target["start"], "progress": 0, **profile}
    atomic_json(directory / "bake_state.json", state)
    try:
        with (directory / "resolve_frames.log").open("ab") as log:
            process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--provider-worker", str(directory)],
                                       stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
    except Exception as error:
        state.update(state="error", error=str(error))
        atomic_json(directory / "bake_state.json", state)
        raise
    state["worker_pid"] = process.pid
    # Worker owns the state file after it starts; do not overwrite its progress.
    return state


def provider_worker(directory):
    """Serve item frames on demand while keeping Resolve objects inside its Python interpreter."""
    state_path = directory / "bake_state.json"
    state = json.loads(state_path.read_text(encoding="utf-8"))
    temporary = directory / "resolve_frame.png"
    try:
        import DaVinciResolveScript
        from cadence_frame_provider import serve_frames
        resolve = check(DaVinciResolveScript.scriptapp("Resolve"), "Нет подключения к Resolve")
        project = check(resolve.GetProjectManager().GetCurrentProject(), "Открой исходный проект")
        check(project.GetUniqueId() == state["project_id"], "Открой исходный проект")
        _, item = replacement_target(project, state["target"], state["frame_count"], state["fps"], check_space=False)
        transport, exports = None, 0
        def export(index):
            nonlocal item, transport, exports
            check(not (directory / "cancel.request").exists(), "Получение кадров отменено")
            if exports % 25 == 0:
                current = resolve.GetProjectManager().GetCurrentProject()
                check(current and current.GetUniqueId() == state["project_id"], "Проект Resolve изменился во время расчёта")
                _, item = replacement_target(project, state["target"], state["frame_count"], state["fps"], check_space=False)
            payload, transport = export_png_bytes(item, index, temporary, transport)
            exports += 1
            state["frame_transport"] = transport
            return payload
        serve_frames(directory, state, export)
    except Exception as error:
        traceback.print_exc(file=sys.stderr)
        state.update(state="cancelled" if (directory / "cancel.request").exists() else "error", error=str(error), provider_active=False)
        atomic_json(state_path, state)
    finally:
        temporary.unlink(missing_ok=True)


def export_png_bytes(exporter, index, path, transport=None):
    """Prefer in-memory PNG export; retain file export for hosts lacking encoded output."""
    if transport != "file":
        try:
            reply = exporter.ExportFrame(index, {"outputType": "encodedString", "encodedImageFormat": "png", "maxDimension": 0})
        except Exception:
            if transport == "encoded":
                raise
            reply = None
        if isinstance(reply, dict) and reply.get("success") and isinstance(reply.get("data"), str):
            payload = base64.b64decode(reply["data"], validate=True)
            check(payload[:8] == b"\x89PNG\r\n\x1a\n", "Resolve вернул некорректный PNG")
            return payload, "encoded"
        check(transport != "encoded", f"Resolve прервал получение кадра {index}: {reply}")
    path.unlink(missing_ok=True)
    reply = exporter.ExportFrame(index, {"outputType": "file", "filePath": str(path), "maxDimension": 0})
    check(isinstance(reply, dict) and reply.get("success") and path.is_file(), f"Resolve не экспортировал кадр {index}: {reply}")
    payload = path.read_bytes()
    path.unlink()
    check(payload[:8] == b"\x89PNG\r\n\x1a\n", "Resolve вернул некорректный PNG")
    return payload, "file"


class PngPipeWriter:
    """Overlap serialized Resolve export with encoder writes using a bounded two-frame queue."""

    def __init__(self, pipe, cancelled):
        self.pipe, self.cancelled = pipe, cancelled
        self.pending = queue.Queue(maxsize=2)
        self.error = None
        self.stopped = threading.Event()
        self.worker = threading.Thread(target=self._write, daemon=True)
        self.worker.start()

    def _write(self):
        try:
            while not self.stopped.is_set():
                try:
                    payload = self.pending.get(timeout=0.1)
                except queue.Empty:
                    continue
                if payload is None:
                    self.pipe.close()
                    return
                view = memoryview(payload)
                while view:
                    written = self.pipe.write(view)
                    if not written:
                        raise BrokenPipeError("Encoder stopped accepting frames")
                    view = view[written:]
        except Exception as error:
            self.error = error
        finally:
            self.stopped.set()

    def put(self, payload):
        while True:
            check(not self.cancelled(), "Подготовка отменена")
            if self.error:
                raise RuntimeError("FFmpeg прервал приём кадров; подробности в ffmpeg_frames.log") from self.error
            check(not self.stopped.is_set(), "Поток записи кадров завершился раньше времени")
            try:
                self.pending.put(payload, timeout=0.1)
                return
            except queue.Full:
                pass

    def finish(self):
        self.put(None)
        while self.worker.is_alive():
            check(not self.cancelled(), "Подготовка отменена")
            self.worker.join(timeout=0.1)
        if self.error:
            raise RuntimeError("FFmpeg прервал приём кадров; подробности в ffmpeg_frames.log") from self.error

    def abort(self):
        self.stopped.set()
        self.worker.join(timeout=5)


def direct_worker(directory):
    """Read processed item frames through Resolve and stream PNGs to lossless FFV1."""
    state_path = directory / "bake_state.json"
    state = json.loads(state_path.read_text(encoding="utf-8"))
    encoder = None
    pipe_writer = None
    started = time.monotonic()
    frame_path = directory / "resolve_frame.png"
    try:
        import DaVinciResolveScript
        resolve = check(DaVinciResolveScript.scriptapp("Resolve"), "Нет подключения к Resolve")
        project = check(resolve.GetProjectManager().GetCurrentProject(), "Открой исходный проект")
        check(project.GetUniqueId() == state["project_id"], "Открой исходный проект")
        target = state["target"]
        _, item = replacement_target(project, target, state["frame_count"], state["fps"], check_space=False)
        output = directory / "resolve_processed.mkv"
        environment = os.environ.copy()
        for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONHOME", "PYTHONPATH"):
            environment.pop(key, None)
        pixel_format = "gbrap16le" if state["require_alpha"] else "gbrp16le"
        exporter = item.GetMediaPoolItem() if state.get("ignore_processing") else item
        previous_index, previous_payload, transport = None, None, None
        export_seconds = pipe_wait_seconds = 0.0
        last_progress = time.monotonic()
        with (directory / "ffmpeg_frames.log").open("wb") as log:
            encoder = subprocess.Popen(["/usr/bin/ffmpeg", "-nostdin", "-v", "error", "-f", "image2pipe",
                                        "-framerate", str(state["fps"]), "-vcodec", "png", "-i", "pipe:0",
                                        "-an", "-c:v", "ffv1", "-level", "3", "-threads", "4", "-slices", "4", "-slicecrc", "0", "-pix_fmt", pixel_format,
                                        "-y", str(output)], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                       stderr=log, env=environment)
            pipe_writer = PngPipeWriter(encoder.stdin, lambda: (directory / "cancel.request").exists())
            for offset in range(state["frame_count"]):
                check(not (directory / "cancel.request").exists(), "Подготовка отменена")
                if offset and offset % 25 == 0:
                    check(resolve.GetProjectManager().GetCurrentProject().GetUniqueId() == state["project_id"],
                          "Во время подготовки изменён проект Resolve")
                    _, item = replacement_target(project, target, state["frame_count"], state["fps"], check_space=False)
                    exporter = item.GetMediaPoolItem() if state.get("ignore_processing") else item
                frame_index = raw_timeline_frame(target, offset) if state.get("ignore_processing") else offset
                if frame_index == previous_index:
                    payload = previous_payload
                else:
                    before = time.monotonic()
                    payload, transport = export_png_bytes(exporter, frame_index, frame_path, transport)
                    export_seconds += time.monotonic() - before
                    previous_index, previous_payload = frame_index, payload
                state["frame_transport"] = transport
                if offset == 0 and state.get("ignore_processing"):
                    # Raw keeps the original raster; timing always comes from the timeline.
                    header = payload[:24]
                    check(header[:8] == b"\x89PNG\r\n\x1a\n" and len(header) == 24, "Resolve не создал корректный PNG")
                    state["width"] = int.from_bytes(header[16:20], "big")
                    state["height"] = int.from_bytes(header[20:24], "big")
                if state["require_alpha"]:
                    header = payload[:26]
                    check(len(header) == 26 and header[:8] == b"\x89PNG\r\n\x1a\n" and header[25] in (4, 6),
                          "Resolve не предоставил PNG с прозрачностью; отключи alpha для непрозрачного клипа")
                before = time.monotonic()
                pipe_writer.put(payload)
                pipe_wait_seconds += time.monotonic() - before
                state["progress"] = int(95 * (offset + 1) / state["frame_count"])
                if time.monotonic() - last_progress >= 0.5:
                    state.update(export_seconds=round(export_seconds, 3), pipe_wait_seconds=round(pipe_wait_seconds, 3))
                    atomic_json(state_path, state)
                    last_progress = time.monotonic()
            pipe_writer.finish()
            state.update(export_seconds=round(export_seconds, 3), pipe_wait_seconds=round(pipe_wait_seconds, 3))
            while encoder.poll() is None:
                if (directory / "cancel.request").exists():
                    encoder.terminate()
                    raise RuntimeError("Подготовка отменена")
                try:
                    encoder.wait(timeout=0.5)
                except subprocess.TimeoutExpired:
                    pass
            check(encoder.returncode == 0 and output.is_file() and output.stat().st_size,
                  "Не удалось собрать кадры в FFV1; подробности в ffmpeg_frames.log")
        state["decoded_input"] = inspect_baked_media(state, output)
        check(not (directory / "cancel.request").exists(), "Подготовка отменена")
        state.update(state="complete", input=str(output), progress=100)
    except Exception as error:
        traceback.print_exc(file=sys.stderr)
        if encoder and encoder.poll() is None:
            encoder.terminate()
            try:
                encoder.wait(timeout=5)
            except subprocess.TimeoutExpired:
                encoder.kill()
                encoder.wait()
        state.update(state="cancelled" if (directory / "cancel.request").exists() else "error", error=str(error))
    finally:
        if pipe_writer:
            pipe_writer.abort()
        frame_path.unlink(missing_ok=True)
        state["preparation_seconds"] = round(time.monotonic() - started, 3)
        atomic_json(state_path, state)


def bake_start(project, request):
    """Queue a processed clip export only after the user presses Calculate in the GUI."""
    directory = Path(request["job"]).resolve()
    check(directory.is_dir() and not (directory / "bake_state.json").exists(), "Для bake нужно новое задание")
    check(not (directory / "cancel.request").exists(), "Подготовка отменена")
    helper = Path(request.get("helper", ""))
    check(helper.is_file(), "Не найден native helper для проверки intermediate; пересобери приложение")
    require_alpha = request.get("require_alpha", True)
    check(isinstance(require_alpha, bool), "Некорректный флаг прозрачности")
    check(not project.IsRenderingInProgress(), "В Resolve уже идёт рендер. Дождись его завершения")
    target = request["target"]
    fps = project_context(project)["output_fps"]
    timeline, original = replacement_target(project, target, target["end"] - target["start"], fps)
    profile = bake_profile(project, timeline, request.get("transfer_override"))
    formats = project.GetRenderFormats() or {}
    check("mov" in formats.values(), "В Resolve недоступен QuickTime MOV для промежуточного файла")
    codecs = project.GetRenderCodecs("mov") or {}
    candidates = []
    for name, identity in codecs.items():
        normalized = re.sub(r"[^a-z0-9]", "", (name + str(identity)).lower())
        priority = 0 if "prores4444" in normalized else 1 if "dnxhr444" in normalized else 2
        if priority < 2 or ("uncompressed" in normalized and "rgb" in normalized) or (not require_alpha and "dnxhr" in normalized):
            candidates.append((priority, name, identity))
    check(candidates, "Нужен доступный encoder ProRes 4444, DNxHR 444 или Uncompressed RGB")
    source_dir = directory / "resolve_source"
    source_dir.mkdir()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    preset = "SilentFilm restore " + stamp
    state = {"project_id": project.GetUniqueId(), "original_id": timeline.GetUniqueId(),
             "state": "preparing", "render_job_id": "", "restore_preset": preset,
             "old_mode": project.GetCurrentRenderMode(), "old_codec": project.GetCurrentRenderFormatAndCodec(),
             "source_directory": str(source_dir), "frame_count": target["end"] - target["start"],
             "helper": str(helper.resolve()), "require_alpha": require_alpha, "codec_attempts": [], **profile}
    check(project.SaveAsNewRenderPreset(preset), "Не удалось сохранить настройки Deliver; bake не запускался")
    atomic_json(directory / "bake_state.json", state)
    restored = False
    try:
        temporary = check(timeline.DuplicateTimeline("SilentFilm source — " + stamp), "Не удалось скопировать таймлайн для bake")
        state["temporary_id"] = temporary.GetUniqueId()
        atomic_json(directory / "bake_state.json", state)
        check(project.SetCurrentTimeline(temporary), "Не удалось выбрать служебный таймлайн")
        for track in range(1, int(temporary.GetTrackCount("video")) + 1):
            check(temporary.SetTrackEnable("video", track, track == target["track_index"]), "Не удалось изолировать видеодорожку")
        matches = [item for item in sequence(temporary.GetItemListInTrack("video", target["track_index"]))
                   if float(item.GetStart()) == target["start"] and float(item.GetEnd()) == target["end"]
                   and item.GetMediaPoolItem() and item.GetMediaPoolItem().GetUniqueId() == target["media_id"]]
        check(len(matches) == 1, "Не найден монтажный экземпляр в служебной копии")
        check(matches[0].SetClipEnabled(True), "Не удалось включить выбранный клип в служебной копии")
        # Только собственная обработка клипа/группы. Timeline grade останется на исходном timeline и применится один раз.
        graph = temporary.GetNodeGraph()
        check(graph, "Resolve не предоставил graph таймлайна для clip-only bake")
        for index in range(1, int(graph.GetNumNodes()) + 1):
            check(graph.SetNodeEnabled(index, False), "Не удалось исключить timeline grade из clip-only bake")
        check(project.SetCurrentRenderMode(1), "Не назначен Single Clip render")
        for priority, name, codec in sorted(candidates):
            attempt = {"codec": name, "id": str(codec)}
            state["codec_attempts"].append(attempt)
            if not project.SetCurrentRenderFormatAndCodec("mov", codec):
                attempt["rejected"] = "SetCurrentRenderFormatAndCodec"
                continue
            if not project.SetCurrentRenderMode(1):
                attempt["rejected"] = "SetCurrentRenderMode"
                continue
            render_range = {"MarkIn": target["start"], "MarkOut": target["end"] - 1}
            # Resolve может отклонить SelectAllFrames отдельно от границ. Пробуем
            # передать диапазон целиком, затем только границы; очередь проверяется ниже.
            if project.SetRenderSettings({"SelectAllFrames": 0, **render_range}):
                attempt["range_settings"] = "select_and_marks"
            elif project.SetRenderSettings(render_range):
                attempt["range_settings"] = "marks_only"
            else:
                attempt["rejected_settings"] = ["MarkIn/MarkOut"]
                continue
            settings = {"TargetDir": str(source_dir), "CustomName": "processed", "UseUniqueFilenames": False,
                        "ExportVideo": True, "ExportAudio": False, "FormatWidth": profile["width"],
                        "FormatHeight": profile["height"], "FrameRate": profile["fps"],
                        "ExportAlpha": require_alpha, "UseFullExtents": False, "AddFrameHandles": 0,
                        "ExportSubtitle": False, "DataBurnIn": "None", "ReplaceExistingFilesInPlace": False,
                        "ColorSpaceTag": "Same as Project", "GammaTag": "Same as Project"}
            if require_alpha:
                # AlphaMode принимается только после включения ExportAlpha.
                ordered = {}
                for key, value in settings.items():
                    ordered[key] = value
                    if key == "ExportAlpha":
                        ordered["AlphaMode"] = 1
                settings = ordered
            optional = {"UseUniqueFilenames", "ReplaceExistingFilesInPlace", "ColorSpaceTag", "GammaTag",
                        "UseFullExtents", "AddFrameHandles"}
            failures = []
            for key, value in settings.items():
                if not project.SetRenderSettings({key: value}):
                    failures.append(key)
            attempt["rejected_settings"] = failures
            if any(key not in optional for key in failures):
                continue
            identity = project.AddRenderJob()
            if not identity:
                attempt["rejected"] = "AddRenderJob"
                continue
            state["render_job_id"] = identity
            atomic_json(directory / "bake_state.json", state)
            jobs = [job for job in sequence(project.GetRenderJobList()) if job.get("JobId") == identity]
            check(len(jobs) == 1, "Не найдено служебное задание в очереди Resolve")
            info = jobs[0]
            attempt["reported_alpha"] = info.get("ExportAlpha")
            attempt["reported_alpha_mode"] = info.get("AlphaMode")
            if require_alpha and reported_bool(info.get("ExportAlpha")) is False:
                attempt["rejected"] = "ExportAlpha=False"
                check(project.DeleteRenderJob(identity), "Не удалось удалить неподходящее служебное задание")
                state["render_job_id"] = ""
                continue
            check(info.get("MarkIn") == target["start"] and info.get("MarkOut") == target["end"] - 1,
                  f"Диапазон экспорта Resolve={info.get('MarkIn')}…{info.get('MarkOut')}, ожидается={target['start']}…{target['end'] - 1}")
            check(reported_bool(info.get("SelectAllFrames")) is not True,
                  "Resolve подтвердил экспорт всего таймлайна вместо выбранного диапазона")
            if info.get("FrameRate") is not None:
                check(abs(decimal(info["FrameRate"]) - profile["fps"]) < 0.002, "Resolve назначил другой FPS промежуточному файлу")
            for key, expected in (("FormatWidth", profile["width"]), ("FormatHeight", profile["height"])):
                if info.get(key) is not None:
                    check(int(info[key]) == expected, f"Resolve назначил {key}={info[key]}, ожидается {expected}")
            attempt["reported_pixel_aspect"] = info.get("PixelAspectRatio")
            if require_alpha and info.get("AlphaMode") is not None:
                check(int(info["AlphaMode"]) == 1, "Resolve не подтвердил straight alpha")
            if isinstance(info.get("RenderMode"), str):
                check(info["RenderMode"].lower() == "single clip", "Resolve не назначил Single Clip render")
            state["codec"] = name
            break
        check(state["render_job_id"], "Не удалось настроить промежуточный codec: " + json.dumps(state["codec_attempts"], ensure_ascii=False))
        restore_deliver(project, state)
        check(project.SetCurrentTimeline(timeline), "Не удалось вернуться к исходному таймлайну")
        restored = True
        check(project.DeleteRenderPreset(preset), "Не удалось удалить служебный preset")
        check(not (directory / "cancel.request").exists(), "Подготовка отменена")
        check(project.StartRendering([state["render_job_id"]], False), "Resolve не запустил служебный рендер")
        state["state"] = "rendering"
        atomic_json(directory / "bake_state.json", state)
        return state
    except Exception as error:
        if not restored:
            try:
                restore_deliver(project, state)
                restored = True
                project.SetCurrentTimeline(timeline)
                if restored:
                    project.DeleteRenderPreset(preset)
            except Exception:
                restored = False
        if not project.IsRenderingInProgress():
            bake_cleanup(project, state)
        state.update(state="cancelled" if (directory / "cancel.request").exists() else "error",
                     error=str(error), deliver_restored=restored)
        atomic_json(directory / "bake_state.json", state)
        raise


def bake_poll(project, request, cancel=False):
    """Poll only this job and publish a validated, completed intermediate for native processing."""
    directory = Path(request["job"]).resolve()
    state = json.loads((directory / "bake_state.json").read_text(encoding="utf-8"))
    check(project.GetUniqueId() == state["project_id"], "Открой исходный проект для продолжения bake")
    if state["state"] != "rendering":
        return state
    status = project.GetRenderJobStatus(state["render_job_id"]) or {}
    phase = status.get("JobStatus", "")
    if cancel and phase == "Rendering":
        # StartRendering запущен с единственным ID; чужую очередь не останавливаем.
        active = [job for job in sequence(project.GetRenderJobList())
                  if (project.GetRenderJobStatus(job["JobId"]) or {}).get("JobStatus") == "Rendering"]
        check(all(job["JobId"] == state["render_job_id"] for job in active), "В Resolve идёт другой рендер; отмена не выполнена")
        project.StopRendering()
        state["cancel_requested"] = True
        atomic_json(directory / "bake_state.json", state)
        return state
    if phase == "Complete":
        if cancel or state.get("cancel_requested"):
            state["state"] = "cancelled"
        else:
            files = [path for path in Path(state["source_directory"]).iterdir() if path.suffix.lower() == ".mov" and path.is_file()]
            check(len(files) == 1 and files[0].stat().st_size > 0, "Resolve не создал один завершённый MOV промежуточного файла")
            try:
                state["decoded_input"] = inspect_baked_media(state, files[0])
                state.update(state="complete", input=str(files[0].resolve()))
            except Exception as error:
                state.update(state="error", error=str(error))
    elif phase in ("Failed", "Cancelled", "Background Render Cancelled", "Remote Render Cancelled"):
        state.update(state="error" if phase == "Failed" else "cancelled", error=status.get("Error", phase))
    elif cancel and phase != "Rendering":
        state["state"] = "cancelled"
    elif phase not in ("Ready", "Rendering"):
        check(False, "Неизвестный статус служебного рендера: " + repr(status))
    if state["state"] != "rendering":
        bake_cleanup(project, state)
    state["progress"] = status.get("CompletionPercentage", 0)
    atomic_json(directory / "bake_state.json", state)
    return state


def finish_recovery_backup(pool, backup, directory):
    """Discard a recovery copy only after a verified edit; report cleanup failure without repeating the edit."""
    retained, warning = False, ""
    try:
        check(pool.DeleteTimelines([backup]), "Resolve не удалил служебную копию")
    except Exception as error:
        retained = True
        warning = f"Служебная копия «{backup.GetName()}» осталась: {error}. Её ссылки могут удерживать кэш."
    try:
        path = directory / "replace_recovery.json"
        recovery = json.loads(path.read_text(encoding="utf-8"))
        recovery["backup_state"] = "retained" if retained else "deleted"
        atomic_json(path, recovery)
    except (OSError, ValueError) as error:
        warning += " Не удалось обновить паспорт резервной копии: " + str(error)
    return {"backup_name": backup.GetName(), "backup_retained": retained, "backup_warning": warning}


def replace_result(project, pool, clip, target, count, fps, directory):
    """Replace only the target video and retain its recovery timeline only when the edit fails."""
    timeline, original = replacement_target(project, target, count, fps)
    before = timeline_positions(timeline, target["item_id"])
    linked = [item for item in sequence(original.GetLinkedItems()) if item.GetUniqueId() != target["item_id"]]
    backup_name = "SilentFilm backup — " + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    backup = check(timeline.DuplicateTimeline(backup_name), "Не удалось создать резервную копию таймлайна; замена не выполнялась")
    recovery = {"backup_id": backup.GetUniqueId(), "backup_name": backup.GetName(),
                "original_timeline_id": timeline.GetUniqueId()}
    # Записываем точку восстановления до первого изменения исходного монтажа.
    (directory / "replace_recovery.json").write_text(json.dumps(recovery, ensure_ascii=False), encoding="utf-8")
    try:
        check(project.SetCurrentTimeline(timeline), "Не удалось выбрать исходный таймлайн")
        if linked:
            check(timeline.SetClipsLinked([original] + linked, False), "Не удалось отделить видео от связанных клипов")
        check(timeline.DeleteClips([original], False), "Resolve не удалил выбранный видеоклип")
        check(timeline_positions(timeline) == before, "Resolve изменил другие клипы при удалении; используй резервную копию")
        clip.ClearMarkInOut("all")
        added = sequence(pool.AppendToTimeline([{"mediaPoolItem": clip, "mediaType": 1,
                                                 "trackIndex": target["track_index"], "recordFrame": target["start"]}]))
        check(len(added) == 1, "Resolve не вставил результат на место клипа")
        item = added[0]
        check(abs(float(item.GetStart()) - target["start"]) < 0.01
              and abs(float(item.GetDuration()) - count) < 0.01
              and sequence(item.GetTrackTypeAndIndex()) == ["video", target["track_index"]],
              "Resolve вставил результат с неверной позицией, дорожкой или длительностью")
        check(timeline_positions(timeline, item.GetUniqueId()) == before, "Resolve изменил остальные монтажные позиции")
        if linked:
            check(timeline.SetClipsLinked([item] + linked, True), "Не удалось восстановить связи с исходным аудио")
        check(timeline_positions(timeline, item.GetUniqueId()) == before, "Монтажные позиции изменились при восстановлении связей")
    except Exception as error:
        try:
            opened = bool(project.SetCurrentTimeline(backup))
        except Exception:
            opened = False
        recovery_hint = ("Открыта" if opened else "Открой вручную:") + f" резервная копия «{backup.GetName()}»"
        raise RuntimeError(f"Замена не завершена: {error}. {recovery_hint}. "
                           "Исходный таймлайн может быть частично изменён; резервная копия и готовый файл сохранены") from error
    recovery_status = finish_recovery_backup(pool, backup, directory)
    return {"project_id": project.GetUniqueId(), "timeline_id": timeline.GetUniqueId(), "item_id": item.GetUniqueId(),
            "timeline": timeline.GetName(), "mode": "replace", **recovery_status,
            "message": f"Клип заменён: {count} кадров. Позиции остальных клипов сохранены; при сокращении оставлен промежуток. "
                       "Добавь Camera Optics при необходимости. " + recovery_status["backup_warning"]}


def track_has_room(timeline, track, start, count):
    """Require an unlocked video track with room, regardless of its visibility."""
    if timeline.GetIsTrackLocked("video", track):
        return False
    end = start + count
    return all(float(item.GetEnd()) <= start or float(item.GetStart()) >= end
               for item in sequence(timeline.GetItemListInTrack("video", track)))


def available_above_track(timeline, target, count):
    """Choose the nearest available existing track above the source, or one new top track."""
    tracks = int(timeline.GetTrackCount("video"))
    for track in range(target["track_index"] + 1, tracks + 1):
        if track_has_room(timeline, track, target["start"], count):
            return track
    return tracks + 1


def add_above_result(project, pool, clip, target, count, fps, directory):
    """Reuse the nearest suitable video track; append a track only when existing tracks cannot fit."""
    timeline, original = replacement_target(project, target, count, fps, check_space=False)
    before = timeline_positions(timeline)
    previous_tracks = int(timeline.GetTrackCount("video"))
    track = available_above_track(timeline, target, count)
    created_track = False
    backup = check(timeline.DuplicateTimeline("SilentFilm backup — " + datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")),
                   "Не удалось создать резервную копию таймлайна")
    atomic_json(directory / "replace_recovery.json", {"backup_id": backup.GetUniqueId(), "backup_name": backup.GetName(),
                                                      "original_timeline_id": timeline.GetUniqueId()})
    try:
        check(project.SetCurrentTimeline(timeline), "Не удалось выбрать исходный таймлайн")
        if track > previous_tracks:
            check(int(timeline.GetTrackCount("video")) == previous_tracks, "Состав дорожек изменился; повтори импорт")
            added_track = timeline.AddTrack("video")
            created_track = bool(added_track) and int(timeline.GetTrackCount("video")) == track
            check(created_track, "Resolve не создал видеодорожку для результата")
        check(track_has_room(timeline, track, target["start"], count), "Место над исходным клипом уже занято или дорожка недоступна")
        clip.ClearMarkInOut("all")
        added = sequence(pool.AppendToTimeline([{"mediaPoolItem": clip, "mediaType": 1,
                                                 "trackIndex": track, "recordFrame": target["start"]}]))
        check(len(added) == 1, "Не удалось добавить результат над исходным клипом")
        item = added[0]
        check(abs(float(item.GetStart()) - target["start"]) < 0.01 and abs(float(item.GetDuration()) - count) < 0.01
              and sequence(item.GetTrackTypeAndIndex()) == ["video", track], "Неверные позиция/длина/дорожка результата")
        check(timeline_positions(timeline, item.GetUniqueId()) == before, "Resolve изменил исходный монтаж при добавлении результата")
    except Exception as error:
        if created_track:
            try:
                if int(timeline.GetTrackCount("video")) == track and not sequence(timeline.GetItemListInTrack("video", track)):
                    timeline.DeleteTrack("video", track)
            except Exception:
                pass  # Резервная копия остаётся основной точкой восстановления.
        project.SetCurrentTimeline(backup)
        raise RuntimeError(f"Добавление не завершено: {error}. Резервная копия: «{backup.GetName()}». Файл результата сохранён") from error
    recovery_status = finish_recovery_backup(pool, backup, directory)
    return {"project_id": project.GetUniqueId(), "timeline_id": timeline.GetUniqueId(), "item_id": item.GetUniqueId(),
            "timeline": timeline.GetName(), "mode": "above", **recovery_status, "track_index": track, "created_track": created_track,
            "message": f"Результат добавлен на V{track} над исходным клипом. Оригинал и позиции остальных клипов сохранены. "
                       + recovery_status["backup_warning"]}


def import_prepared_media(pool, result, count, directory):
    """Import a movie or image sequence using host-compatible argument forms."""
    path = result["media_path"]
    is_sequence = result["format"] == "tiff"
    diagnostics = {"format": result["format"], "path": path, "attempts": []}
    if is_sequence:
        arguments = [[{"FilePath": path, "StartIndex": 0, "EndIndex": count - 1}]]
    else:
        check(Path(path).is_file(), "Подготовленное видео отсутствует: " + path)
        # Обычный путь использовался в ранее проверенном Lua-импорте MOV.
        arguments = [[path], [{"FilePath": path}]]
    for argument in arguments:
        attempt = {"argument": argument}
        try:
            response = pool.ImportMedia(argument)
            imported = sequence(response) if isinstance(response, (list, tuple, dict)) else []
            attempt.update(return_type=type(response).__name__, count=len(imported))
        except Exception as error:
            imported = []
            attempt.update(error=str(error), count=0)
        diagnostics["attempts"].append(attempt)
        atomic_json(directory / "import_diagnostics.json", diagnostics)
        if len(imported) == 1:
            return imported[0]
        if imported:
            break
        if not is_sequence:
            # Некоторые версии хоста добавляют файл, но не возвращают handle.
            # Проверяем текущую папку перед повторной попыткой, чтобы не плодить копии.
            folder = pool.GetCurrentFolder()
            existing = sequence(folder.GetClipList()) if folder else []
            for clip in existing:
                media_path = clip.GetClipProperty("File Path")
                if isinstance(media_path, str) and media_path and Path(media_path).resolve() == Path(path).resolve():
                    diagnostics["recovered_from_current_folder"] = True
                    atomic_json(directory / "import_diagnostics.json", diagnostics)
                    return clip
    detail = ("Для TIFF отключи Show Individual Frames и повтори импорт последовательности."
              if is_sequence else "Попробуй перетащить этот MOV в Media Pool вручную: это покажет, принимает ли Resolve файл.")
    raise RuntimeError(f"Resolve не вернул один клип при импорте {result['format']}: {path}. {detail} "
                       "Результат сохранён, повторный расчёт не нужен. Диагностика: import_diagnostics.json")


def import_result(project, request):
    """Import a complete capture into a verified new timeline or replace its saved target without ripple."""
    directory = Path(request["job"]).resolve()
    result = read_ini(directory / "result.ini")
    check(result.get("schema") == "1" and result.get("state") == "complete", "Задание ещё не завершено")
    context_file = directory / "app_context.json"
    context = json.loads(context_file.read_text(encoding="utf-8")) if context_file.exists() else {}
    current = project_context(project)
    check(not context.get("project_id") or context["project_id"] == current["project_id"],
          "Открой исходный проект: " + context.get("project_name", ""))
    marker = directory / "resolve_import.json"
    if marker.exists():
        previous = json.loads(marker.read_text(encoding="utf-8"))
        if previous.get("project_id") == current["project_id"]:
            for index in range(1, int(project.GetTimelineCount()) + 1):
                timeline = project.GetTimelineByIndex(index)
                if timeline.GetUniqueId() == previous.get("timeline_id"):
                    return {"message": "Уже импортировано: " + timeline.GetName(), "timeline": timeline.GetName()}
    count = int(result["frame_count"])
    fps = float(result["output_fps"])
    check(count > 0 and math.isfinite(fps) and fps > 0, "Некорректная длина результата")
    mode = request.get("mode", context.get("import_mode", "new_timeline"))
    check(mode in ("replace", "above", "new_timeline"), "Неизвестный режим импорта")
    if mode in ("replace", "above"):
        check(context.get("target"), "В задании нет привязки к выделенному клипу таймлайна")
        processing = context.get("source_processing")
        check(processing in ("raw_source_trim", "native_raw_timeline") or (processing in ("resolve_clip_bake", "resolve_item_frames", "resolve_raw_timeline_frames", "resolve_frame_provider") and context.get("baked_input")),
              "В задании не указан обработанный вход или явный режим исходника без обработок")
        if processing == "raw_source_trim":
            source = raw_source_range(context["target"])
            check(Path(result.get("input", "")).resolve() == Path(source["input"]).resolve()
                  and int(result.get("source_start_frame", -1)) == source["start_frame"]
                  and int(result.get("source_frame_count", -1)) == source["frame_count"],
                  "Результат не соответствует In/Out выбранного исходника")
        if processing == "native_raw_timeline":
            source = native_raw_request(context["target"], context["output_fps"])
            check(Path(result.get("input", "")).resolve() == Path(source["input"]).resolve()
                  and int(result.get("raw_start_frame", -1)) == source["raw_start_frame"]
                  and int(result.get("raw_frame_count", -1)) == source["raw_frame_count"]
                  and int(result.get("source_frame_count", -1)) == source["frame_count"]
                  and abs(float(result.get("timeline_input_fps", 0)) - source["timeline_input_fps"]) < 0.002,
                  "Результат не соответствует исходной подрезке и временной сетке таймлайна")
        if processing == "resolve_frame_provider":
            check(result.get("source_mode") == "resolve_frames"
                  and Path(result.get("input", "")).resolve() == Path(context["baked_input"]).resolve()
                  and int(result.get("source_frame_count", -1)) == context["target"]["end"]-context["target"]["start"],
                  "Результат не соответствует прямому источнику кадров монтажного клипа")
        replacement_target(project, context["target"], count, fps, check_space=mode == "replace")
    pool = project.GetMediaPool()
    clip = import_prepared_media(pool, result, count, directory)
    if result.get("clip_name"):
        check(clip.SetName(result["clip_name"]), "Не удалось назначить имя клипа результата")
    clip.SetClipProperty("FPS", host_fps(fps))
    check(abs(decimal(clip.GetClipProperty("FPS")) - fps) < 0.002,
          "FPS результата не назначен; файл оставлен в Media Pool")
    if mode in ("replace", "above"):
        operation = replace_result if mode == "replace" else add_above_result
        outcome = operation(project, pool, clip, context["target"], count, fps, directory)
        temporary = marker.with_suffix(".tmp")
        temporary.write_text(json.dumps(outcome, ensure_ascii=False), encoding="utf-8")
        temporary.replace(marker)
        return outcome
    old_timeline = project.GetCurrentTimeline()
    name = "SilentFilm Cadence — " + context.get("source_name", "Result") + datetime.datetime.now().strftime(" — %H%M%S")
    timeline = check(pool.CreateEmptyTimeline(name), "Не удалось создать таймлайн")
    try:
        check(timeline.SetSettings({"useCustomSettings": "1"}), "Не включены отдельные настройки таймлайна")
        check(timeline.SetSettings({"timelineFrameRate": host_fps(fps)}), "Не назначен FPS таймлайна")
        check(abs(decimal(timeline.GetSettings().get("timelineFrameRate")) - fps) < 0.002,
              "FPS таймлайна не совпал с результатом")
        check(timeline.SetStartTimecode("00:00:00:00"), "Не назначен нулевой старт таймлайна")
        check(project.SetCurrentTimeline(timeline), "Не удалось выбрать таймлайн")
        clip.ClearMarkInOut("all")
        # Современная форма без ручного source trim: все M кадров.
        added = sequence(pool.AppendToTimeline([{"mediaPoolItem": clip}]))
        actual = float(added[0].GetDuration()) if len(added) == 1 else None
        check(actual is not None and abs(actual - count) < 0.01,
              f"Монтажная длина Resolve={actual}, ожидается={count}; "
              f"Timeline FPS={timeline.GetSettings().get('timelineFrameRate')}, "
              f"Media Pool FPS={clip.GetClipProperty('FPS')}, Frames={clip.GetClipProperty('Frames')}. "
              "Подготовленный файл сохранён; повторный расчёт не требуется")
    except Exception:
        if old_timeline:
            project.SetCurrentTimeline(old_timeline)
        pool.DeleteTimelines([timeline])
        raise
    outcome = {"project_id": current["project_id"], "timeline_id": timeline.GetUniqueId(), "timeline": name,
               "message": f"Импортировано {count} кадров, {count / fps:.3f} с при {fps:g} fps. Далее добавь Camera Optics → Negative → Print → Projector."}
    temporary = marker.with_suffix(".tmp")
    temporary.write_text(json.dumps(outcome, ensure_ascii=False), encoding="utf-8")
    temporary.replace(marker)
    return outcome


def cache_references(project):
    """Collect every project timeline reference and pool usage without changing the active timeline."""
    used = set()
    count = project.GetTimelineCount()
    check(isinstance(count, (int, float)) and count >= 0, "Не удалось получить все таймлайны проекта")
    for index in range(1, int(count) + 1):
        timeline = check(project.GetTimelineByIndex(index), "Не удалось проверить таймлайн проекта")
        for kind in ("video", "audio"):
            tracks = timeline.GetTrackCount(kind)
            check(isinstance(tracks, (int, float)) and tracks >= 0, "Не удалось проверить дорожки проекта")
            for track in range(1, int(tracks) + 1):
                for item in sequence(timeline.GetItemListInTrack(kind, track)):
                    clip = item.GetMediaPoolItem()
                    if clip:
                        path = clip.GetClipProperty("File Path")
                        check(isinstance(path, str), "Не удалось проверить путь клипа таймлайна")
                        if path:
                            used.add(path)
    media = []
    seen = set()
    def visit(folder):
        check(folder, "Не удалось проверить Media Pool")
        identity = folder.GetUniqueId()
        check(isinstance(identity, str) and identity and identity not in seen, "Не удалось обойти папки Media Pool")
        seen.add(identity)
        for clip in sequence(folder.GetClipList()):
            path = clip.GetClipProperty("File Path")
            check(isinstance(path, str), "Не удалось проверить путь клипа Media Pool")
            if path:
                usage = str(clip.GetClipProperty("Usage")).strip()
                media.append({"path": path, "usage": int(usage) if usage.isdigit() else None, "clip": clip})
        for child in sequence(folder.GetSubFolderList()):
            visit(child)
    visit(project.GetMediaPool().GetRootFolder())
    return used, media


def clean_project_cache(project, request):
    """Preview or delete only explicitly confirmed, revalidated unused jobs of the current project."""
    from cadence_cache import beneath, candidates, remove_generation
    context = project_context(project)
    check(context["project_id"] == request["project_id"], "Текущий проект изменился; повтори очистку")
    scope = request.get("scope", "project")
    check(scope in ("timeline", "project"), "Неизвестная область очистки")
    timeline_id = request.get("timeline_id") if scope == "timeline" else None
    if scope == "timeline":
        check(timeline_id and context.get("timeline_id") == timeline_id, "Текущий таймлайн изменился; повтори очистку")
    used, media = cache_references(project)
    approved_paths = None if request["action"] == "cache_plan" else {entry["path"] for entry in request["entries"]}
    entries = candidates(request["root"], context["project_id"], used, media, timeline_id, approved_paths)
    if request["action"] == "cache_plan":
        return dict(context, scope=scope, entries=entries, bytes=sum(item["bytes"] for item in entries))
    approved = {(entry["path"], entry["generation_id"]) for entry in request["entries"]}
    deleted, errors = [], []
    for entry in entries:
        if (entry["path"], entry["generation_id"]) not in approved:
            continue
        try:
            used, media = cache_references(project)
            fresh = candidates(request["root"], context["project_id"], used, media, timeline_id, {entry["path"]})
            check(any(item["path"] == entry["path"] and item["generation_id"] == entry["generation_id"] for item in fresh),
                  "Результат снова используется или его состояние изменилось")
            linked = [item["clip"] for item in media if beneath(item["path"], Path(entry["path"]))]
            if linked:
                check(project.GetMediaPool().DeleteClips(linked), "Resolve не удалил неиспользуемый результат из Media Pool")
            remove_generation(entry)
            deleted.append(entry["path"])
        except (OSError, ValueError, RuntimeError) as error:
            errors.append(entry["path"] + ": " + str(error))
    return {"deleted": deleted, "errors": errors, "skipped": len(approved) - len(deleted) - len(errors)}


def main():
    """Handle one request so the GUI never loads Resolve libraries into its process."""
    try:
        request = json.load(sys.stdin)
        import DaVinciResolveScript
        resolve = check(DaVinciResolveScript.scriptapp("Resolve"),
                        "Нет подключения к Resolve. Открой Studio и включи External scripting → Local в Preferences → System → General")
        project = check(resolve.GetProjectManager().GetCurrentProject(), "Открой проект в Resolve")
        action = request["action"]
        if action == "timeline_source":
            result = timeline_source(project)
        elif action == "timeline_sources":
            result = timeline_sources(project)
        elif action == "context":
            result = project_context(project)
        elif action == "validate_target":
            target = request["target"]
            context = project_context(project)
            replacement_target(project, target, target["end"] - target["start"], context["output_fps"])
            if request.get("ignore_processing"):
                context["raw_source"] = native_raw_request(target, context["output_fps"])
            result = context
        elif action == "bake_start":
            result = bake_start(project, request)
        elif action == "direct_start":
            result = direct_start(project, request)
        elif action == "direct_poll":
            result = json.loads((Path(request["job"]).resolve() / "bake_state.json").read_text(encoding="utf-8"))
        elif action in ("bake_poll", "bake_cancel"):
            result = bake_poll(project, request, cancel=action == "bake_cancel")
        elif action == "import":
            result = import_result(project, request)
        elif action in ("cache_plan", "cache_clean"):
            result = clean_project_cache(project, request)
        else:
            raise ValueError("Неизвестная команда приложения")
        reply = {"ok": True, "result": result}
    except Exception as error:
        traceback.print_exc(file=sys.stderr)
        reply = {"ok": False, "error": str(error)}
    print("SILENTFILM_JSON:" + json.dumps(reply, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--provider-worker":
        provider_worker(Path(sys.argv[2]).resolve())
    elif len(sys.argv) == 3 and sys.argv[1] == "--direct-worker":
        direct_worker(Path(sys.argv[2]).resolve())
    else:
        main()
