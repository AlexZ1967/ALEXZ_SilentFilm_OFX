"""Persist validated Cadence effect settings and named presets without clip metadata."""

from copy import deepcopy
import json
import math
import os
from pathlib import Path
import shutil
import tempfile
import uuid


class PreferenceStore:
    """Atomically store only explicitly allowed settings, preserving user presets on reset."""

    def __init__(self, numbers, choices, flags, path=None, texts=None):
        self.numbers, self.choices, self.flags = numbers, choices, flags
        self.texts = texts or {}
        self.factory = {key: spec[0] for key, spec in numbers.items()}
        self.factory.update({key: spec[0] for key, spec in choices.items()})
        self.factory.update(flags)
        self.factory.update(self.texts)
        root = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
        self.path = Path(path) if path is not None else root / "ALEXZ" / "SilentFilm" / "cadence_settings.json"
        self.data = {"schema": 1, "current": deepcopy(self.factory), "presets": {}, "selected": None}
        self.warning = None
        self.needs_backup = False
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
            if not isinstance(raw, dict) or raw.get("schema") != 1 or not isinstance(raw.get("presets"), dict):
                raise ValueError("Неизвестный формат настроек")
            self.data["current"] = self.validate(raw.get("current", {}))
            for name, settings in raw["presets"].items():
                name = self.validate_name(name)
                if any(item.casefold() == name.casefold() for item in self.data["presets"]):
                    raise ValueError("Повторяющееся имя пресета")
                self.data["presets"][name] = self.validate(settings)
            selected = raw.get("selected")
            self.data["selected"] = selected if selected in self.data["presets"] else None
        except FileNotFoundError:
            pass
        except (OSError, ValueError, TypeError) as error:
            self.data = {"schema": 1, "current": deepcopy(self.factory), "presets": {}, "selected": None}
            self.warning = f"Настройки не прочитаны: {error}. При следующем сохранении исходный файл будет сохранён отдельно."
            self.needs_backup = self.path.exists()

    @property
    def current(self):
        return deepcopy(self.data["current"])

    @property
    def selected(self):
        return self.data["selected"]

    def names(self):
        return sorted(self.data["presets"], key=str.casefold)

    def existing_name(self, name):
        name = self.validate_name(name)
        return next((item for item in self.names() if item.casefold() == name.casefold()), None)

    @staticmethod
    def validate_name(name):
        """Keep names as Unicode data, never as filenames or paths."""
        if not isinstance(name, str):
            raise ValueError("Укажи имя пресета")
        name = name.strip()
        if not name or len(name) > 80 or any(ord(char) < 32 or ord(char) == 127 for char in name):
            raise ValueError("Имя пресета: от 1 до 80 символов без переводов строк")
        if name.casefold() == "custom":
            raise ValueError("Имя Custom зарезервировано для изменённых настроек")
        return name

    def validate(self, values):
        """Filter unknown fields using the explicit effect-setting schema."""
        if not isinstance(values, dict):
            raise ValueError("Некорректные значения настроек")
        result = deepcopy(self.factory)
        for key, value in values.items():
            if key in self.numbers:
                _, low, high, integer = self.numbers[key]
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) \
                        or not low <= value <= high or (integer and value != int(value)):
                    raise ValueError("Недопустимое значение: " + key)
                result[key] = int(value) if integer else value
            elif key in self.choices:
                if value not in self.choices[key][1]:
                    raise ValueError("Недопустимый выбор: " + key)
                result[key] = value
            elif key in self.flags:
                if type(value) is not bool:
                    raise ValueError("Недопустимый флаг: " + key)
                result[key] = value
            elif key in self.texts:
                if not isinstance(value, str) or not value.strip() or any(char in value for char in "\0\r\n"):
                    raise ValueError("Недопустимое текстовое значение: " + key)
                result[key] = value
        if "fps_min" in result and result["fps_min"] > result["fps_max"]:
            raise ValueError("target_fps_min должен быть не больше target_fps_max")
        return result

    def _commit(self, data):
        """Replace the settings file only after a complete UTF-8 document has been flushed."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.needs_backup and self.path.exists():
            backup = self.path.with_name(self.path.name + ".invalid-" + uuid.uuid4().hex)
            shutil.copy2(self.path, backup)
            self.needs_backup = False
        descriptor, temporary = tempfile.mkstemp(prefix=self.path.name + ".", dir=self.path.parent)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                json.dump(data, output, ensure_ascii=False, indent=2, allow_nan=False)
                output.write("\n")
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, self.path)
        finally:
            Path(temporary).unlink(missing_ok=True)
        self.data = data

    def set_current(self, values, selected=None):
        data = deepcopy(self.data)
        data["current"] = self.validate(values)
        data["selected"] = selected if selected in data["presets"] else None
        self._commit(data)

    def reset(self):
        self.set_current(self.factory)

    def preset(self, name):
        return deepcopy(self.data["presets"][name])

    def save_preset(self, name, values, replace=False):
        name = self.validate_name(name)
        existing = self.existing_name(name)
        if existing and not replace:
            raise ValueError("Пресет с таким именем уже существует")
        name = existing or name
        data = deepcopy(self.data)
        settings = self.validate(values)
        data["presets"][name] = settings
        data["current"] = deepcopy(settings)
        data["selected"] = name
        self._commit(data)
        return name
