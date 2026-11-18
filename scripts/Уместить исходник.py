"""Fit the full source of a right-trimmed clip into its current timeline slot."""

import math
from typing import NamedTuple


class ClipState(NamedTuple):
    start: int
    end: int
    source_start: int
    source_end: int
    right_offset: float
    speed: float


def resolve_app():
    """Get Resolve from the internal Workspace scripting environment."""
    app = globals().get("resolve")
    if app:
        return app
    bridge = globals().get("bmd")
    if bridge:
        return bridge.scriptapp("Resolve")
    import DaVinciResolveScript
    return DaVinciResolveScript.scriptapp("Resolve")


def number(value, label):
    """Read a finite number returned by Resolve."""
    try:
        result = float(value)
    except (TypeError, ValueError):
        raise ValueError(f"Resolve не сообщил {label}") from None
    if not math.isfinite(result):
        raise ValueError(f"Resolve вернул неверное значение ({label}): {value}")
    return result


def frame(value, label):
    """Read a whole frame without truncating subframe edits."""
    result = number(value, label)
    if not result.is_integer():
        raise ValueError(f"Дробная позиция ({label}): {result}")
    return int(result)


def snapshot(item):
    """Read the timeline slot, source interval, and speed of an item."""
    speed = item.GetSpeed() or {}
    return ClipState(
        frame(item.GetStart(True), "начало на таймлайне"),
        frame(item.GetEnd(True), "конец на таймлайне"),
        frame(item.GetSourceStartFrame(), "начало исходного отрезка"),
        frame(item.GetSourceEndFrame(), "конец исходного отрезка"),
        number(item.GetRightOffset(True), "правую подрезку"),
        number(speed.get("Percentage"), "скорость"),
    )


def speed_matches(actual, expected):
    """Allow the host to round a percentage to two decimal places."""
    return math.isclose(actual, expected, rel_tol=0, abs_tol=0.005001)


def set_speed(item, percentage):
    """Change speed while preserving the placement of neighboring clips."""
    return item.SetSpeed({
        "Percentage": percentage,
        "RippleTimeline": False,
        "StretchKeyframesToFit": False,
    })


def restore(item, original):
    """Try to recover the original edit after a partial change."""
    try:
        current = snapshot(item)
        if not speed_matches(current.speed, original.speed):
            set_speed(item, original.speed)
            current = snapshot(item)
        if (current.start, current.end, current.source_start, current.source_end,
                current.right_offset) != original[:5]:
            item.SetRightOffset(original.right_offset, True, False)
        return snapshot(item) == original
    except Exception:
        return False


def selected_video(timeline):
    """Return one selected, independent video item on an editable track."""
    selection = timeline.GetSelectedClips() or []
    if isinstance(selection, dict):
        selection = selection.values()
    videos = [item for item in selection if item.GetType() == "video"]
    if len(videos) != 1:
        raise ValueError("На странице Edit выделите ровно один видеоклип")
    item = videos[0]
    if any(link.GetUniqueId() != item.GetUniqueId() for link in (item.GetLinkedItems() or [])):
        raise ValueError("Первая версия работает с отдельным видеоклипом без связанных дорожек")
    track_type, track_index = item.GetTrackTypeAndIndex()
    if timeline.GetIsTrackLocked(track_type, track_index):
        raise ValueError("Видеодорожка заблокирована")
    return item


def fit_source():
    """Apply a constant speed and verify the original slot and full source."""
    app = resolve_app()
    project = app.GetProjectManager().GetCurrentProject() if app else None
    timeline = project.GetCurrentTimeline() if project else None
    if not timeline:
        raise ValueError("Откройте таймлайн")
    item = selected_video(timeline)
    media = item.GetMediaPoolItem()
    if not media or not media.GetClipProperty("File Path"):
        raise ValueError("Нужен клип из исходного видеофайла")

    original = snapshot(item)
    trimmed = original.end - original.start
    full = frame(media.GetClipProperty("Frames"), "длина исходника")
    source_fps = number(media.GetClipProperty("FPS"), "FPS исходника")
    timeline_fps = number((timeline.GetSettings() or {}).get("timelineFrameRate"), "FPS таймлайна")
    if source_fps <= 0 or not math.isclose(source_fps, timeline_fps, abs_tol=0.002):
        raise ValueError("FPS исходника должен совпадать с FPS таймлайна")
    if not speed_matches(original.speed, 100):
        raise ValueError("Исходная скорость выбранного клипа должна быть 100%")
    if abs(number(item.GetLeftOffset(True), "левую подрезку")) > 0.001:
        raise ValueError("Подрежьте клип только справа")
    if trimmed < 2 or trimmed >= full or original.right_offset <= 0:
        raise ValueError(f"Нужна подрезка справа с остатком хотя бы в два кадра ({full} → {trimmed})")
    if frame(item.GetDuration(True), "длина на таймлайне") != trimmed:
        raise ValueError("Границы клипа расходятся с его длительностью")
    # Обе конвенции SourceEnd сохраняются: Resolve может считать конец включительно или исключительно.
    if original.source_end - original.source_start not in (trimmed - 1, trimmed):
        raise ValueError("Исходные границы Resolve не соответствуют клипу на скорости 100%")
    expected_source_end = original.source_end + full - trimmed
    percentage = 100.0 * full / trimmed

    try:
        if not set_speed(item, percentage):
            raise ValueError(f"Resolve отклонил скорость {percentage:.4f}%")
        changed = snapshot(item)
        if changed.start != original.start or changed.source_start != original.source_start:
            raise ValueError("При смене скорости Resolve сдвинул начало клипа")
        if abs(changed.right_offset) > 0.001 and not item.SetRightOffset(0, True, False):
            raise ValueError("Resolve не дал открыть исходник до последнего кадра")
        result = snapshot(item)
        if (result.start, result.end, result.source_start, result.source_end) != (
                original.start, original.end, original.source_start, expected_source_end
        ) or abs(result.right_offset) > 0.001 or not speed_matches(result.speed, percentage):
            raise ValueError(
                f"Resolve вернул длину {result.end - result.start} вместо {trimmed} кадров "
                f"и конец исходника {result.source_end} вместо {expected_source_end}"
            )
    except Exception as error:
        recovered = restore(item, original)
        restoration = "Прежние параметры клипа восстановлены." if recovered else (
            "Прежние параметры не удалось подтвердить. Проверьте клип и историю Undo."
        )
        raise RuntimeError(f"{error}. {restoration}") from error

    print(f"[Уместить исходник] {item.GetName()}: {full} → {trimmed} кадров, {result.speed:.4f}%.")


try:
    fit_source()
except Exception as error:
    print(f"[Уместить исходник] {error}")
