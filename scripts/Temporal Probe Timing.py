"""Export timing for the selected Resolve timeline clip."""

import math
from pathlib import Path
from tempfile import NamedTemporaryFile


def resolve_app():
    """Get Resolve from the internal scripting environment."""
    app = globals().get("resolve")
    if app:
        return app
    bridge = globals().get("bmd")
    if bridge:
        return bridge.scriptapp("Resolve")
    import DaVinciResolveScript
    return DaVinciResolveScript.scriptapp("Resolve")


def frame_count(value, label):
    """Validate an integer frame count from the scripting API."""
    try:
        number = float(value)
    except (TypeError, ValueError):
        raise ValueError(f"Resolve не сообщил {label}") from None
    if not number.is_integer() or not 2 <= number <= 1_000_000_000:
        raise ValueError(f"Некорректное число кадров ({label}): {value}")
    return int(number)


def export_timing():
    """Write a unique timing file for one right-trimmed video item."""
    app = resolve_app()
    project = app.GetProjectManager().GetCurrentProject() if app else None
    timeline = project.GetCurrentTimeline() if project else None
    if not timeline:
        raise ValueError("Откройте таймлайн")
    selected = timeline.GetSelectedClips() or []
    if isinstance(selected, dict):
        selected = selected.values()
    videos = [item for item in selected if item.GetType() == "video"]
    if len(videos) != 1:
        raise ValueError("На странице Edit выделите ровно один видеоклип")
    item = videos[0]
    media = item.GetMediaPoolItem()
    if not media or not media.GetClipProperty("File Path"):
        raise ValueError("Нужен клип из исходного видеофайла")
    full = frame_count(media.GetClipProperty("Frames"), "исходная длина")
    trimmed = frame_count(item.GetDuration(True), "длина после подрезки")
    source_rate = float(media.GetClipProperty("FPS") or 0)
    timeline_rate = float((timeline.GetSettings() or {}).get("timelineFrameRate") or 0)
    percentage = float((item.GetSpeed() or {}).get("Percentage", 0))
    left_offset = float(item.GetLeftOffset(True))
    if not (source_rate > 0 and math.isclose(source_rate, timeline_rate, abs_tol=0.002)
            and math.isclose(percentage, 100, abs_tol=0.001)
            and math.isclose(left_offset, 0, abs_tol=0.001)):
        raise ValueError("Используйте клип с подрезкой только справа, скоростью 100% и FPS таймлайна")
    if trimmed >= full:
        raise ValueError(f"Клип не укорочен справа ({full} → {trimmed} кадров)")

    directory = Path.home() / ".local/share/ALEXZSilentFilm/TemporalProbe"
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    with NamedTemporaryFile(mode="w", encoding="ascii", dir=directory,
                            prefix="Timing-", suffix=".sftiming", delete=False) as file:
        file.write(f"TemporalProbeTiming 1\n{full} {trimmed}\n")
    print(f"[Temporal Probe] {item.GetName()}: {full} → {trimmed} кадров")
    print(f"[Temporal Probe] Выберите в Timeline Timing File: {file.name}")


try:
    export_timing()
except (AttributeError, OSError, TypeError, ValueError, ImportError) as error:
    print(f"[Temporal Probe] {error}")
