"""Track a sequential clip queue and publish a durable per-clip report."""

from copy import deepcopy
import json
from pathlib import Path
import uuid


class BatchQueue:
    """Keep original edit snapshots stable while each result is rendered and optionally imported."""

    def __init__(self, items, settings, root):
        if not items:
            raise ValueError("Пустая очередь клипов")
        self.entries = [{"snapshot": deepcopy(item), "state": "pending", "job": None} for item in items]
        self.settings = deepcopy(settings)
        self.index, self.stopped = 0, False
        folder = Path(root).expanduser().resolve() / "batch_reports"
        folder.mkdir(parents=True, exist_ok=True)
        self.path = folder / ("batch_" + uuid.uuid4().hex + ".json")
        self.save()

    @property
    def active(self):
        return not self.stopped and self.index < len(self.entries)

    @property
    def current(self):
        return self.entries[self.index] if self.index < len(self.entries) else None

    def begin(self):
        if not self.active or self.current["state"] != "pending":
            raise RuntimeError("Некорректное состояние очереди")
        self.current["state"] = "running"
        self.save()

    def complete(self, job, state, message=""):
        if not self.active:
            return
        self.current.update(state=state, job=str(job), message=message)
        self.index += 1
        self.save()

    def stop(self, message, state="error"):
        if not self.active:
            return
        self.current.update(state=state, message=str(message))
        for entry in self.entries[self.index + 1:]:
            entry.update(state="skipped", message="Очередь остановлена")
        self.stopped = True
        self.save()

    def save(self):
        temporary = self.path.with_suffix(".tmp")
        temporary.write_text(json.dumps({"schema": 1, "index": self.index, "stopped": self.stopped,
                                          "settings": self.settings, "clips": self.entries}, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(self.path)
