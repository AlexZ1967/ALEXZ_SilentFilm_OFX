"""Run the native physical-capture pipeline from a standalone GTK 4 application."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path
import subprocess
import time
import sys

from cadence_resolve_bridge import cadence_name

try:
    import gi
    gi.require_version("Gtk", "4.0")
    gi.require_version("Pango", "1.0")
    from gi.repository import GLib, Gtk, Pango
except (ImportError, ValueError) as error:
    raise SystemExit(
        "Для окна SilentFilm нужны GTK 4 / PyGObject / Pango.\n"
        "Debian/Ubuntu: sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-4.0\n"
        f"Подробности: {error}"
    ) from None


def read_ini(path):
    """Read a native metadata snapshot; atomic replacement keeps it consistent."""
    values = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            if key.strip() in values:
                raise ValueError("Повторяющийся ключ паспорта")
            values[key.strip()] = value.strip()
    return values


def write_ini(path, values):
    """Write flat native settings with exact path values and no shell interpolation."""
    rows = []
    for key, value in values.items():
        if any(char in str(value) for char in "\r\n"):
            raise ValueError("Путь или значение содержит перенос строки")
        rows.append(f"{key}={value}\n")
    Path(path).write_text("".join(rows), encoding="utf-8")


def child_environment():
    """Keep Conda and host-library overrides out of native child processes."""
    environment = os.environ.copy()
    for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONHOME", "PYTHONPATH"):
        environment.pop(name, None)
    return environment


def require_flow_support(helper, mode, raw=False, exposure_mode="bake"):
    """Validate helper parameter support and flow before exporting processed source frames."""
    try:
        result = subprocess.run([str(helper), "--capabilities"], capture_output=True, text=True,
                                timeout=10, env=child_environment())
        if result.returncode != 0:
            raise ValueError("Unsupported capabilities request")
        capabilities = json.loads(result.stdout)
        if capabilities.get("exposure_ofx") is not True:
            raise ValueError("Native helper не поддерживает выбор Bake/OFX")
        if capabilities.get("camera_parameter_schema", 0) < 2 or capabilities.get("max_blur_samples", 0) < 17:
            raise ValueError("Outdated camera parameter support")
        supported = capabilities["optical_flow"] is True
        if raw and capabilities.get("native_timeline_source") is not True:
            raise ValueError("Outdated native timeline source support")
        if not raw and capabilities.get("resolve_frame_provider") is not True:
            raise ValueError("Outdated direct Resolve frame provider support")
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        raise RuntimeError("Нужна актуальная сборка native helper Cadence с поддержкой Bake/OFX, временной сетки таймлайна, source_fps и 17 отсчётов. Пересобери проект. " + str(error)) from error
    if mode == "flow_integrated" and not supported:
        raise RuntimeError("Эта сборка Cadence не поддерживает оптический поток. Установи libopencv-dev, повтори конфигурацию CMake и сборку. Затем переоткрой окно Cadence.")


def source_interpretation_fps(mode, custom, context, snapshot):
    """Resolve capture timing independently from encoded-media validation and output playback FPS."""
    if mode == "timeline":
        value = context.get("timeline_fps")
        if value is None:
            raise RuntimeError("Для source_fps_mode=timeline открой таймлайн Resolve или выбери source/custom.")
    elif mode == "source":
        snapshot = snapshot or {}
        value = (snapshot.get("target") or {}).get("fps", snapshot.get("fps"))
        if value is None:
            return 0  # Native helper прочитает FPS выбранного файла.
    elif mode == "custom":
        value = custom
    else:
        raise ValueError("Неизвестный source_fps_mode")
    value = float(value)
    if not math.isfinite(value) or not 1 <= value <= 120:
        raise ValueError("source_fps: допустимо 1…120")
    return value


def capture_range(snapshot, raw=False):
    """Derive the input interval solely from the selected timeline item."""
    snapshot = snapshot or {}
    target = snapshot.get("target")
    if target:
        count = int(target["end"] - target["start"])
        if count <= 0:
            raise ValueError("Пустой диапазон выбранного клипа таймлайна")
        return {"start_frame": 0, "frame_count": count}
    raise ValueError("Выдели один видеоклип на таймлайне Resolve")


class Application:
    """Own UI state while native rendering and Resolve communication stay external."""

    FIELDS = [
        ("source_fps", "source_fps", 25, 1, 120, False,
         "Частота интерпретации входных кадров. timeline — из текущего таймлайна; source — из исходного клипа; custom — ручной ввод. Больше FPS — короче виртуальная сцена и меньше снятых кадров. Это не output_fps."),
        ("fps_min", "target_fps_min", 16, 1, 60, False,
         "Медленная ручка ускоряет действие при проекции. Начни с 16–20 fps; 10–30 — выраженная проверка."),
        ("fps_max", "target_fps_max", 20, 1, 60, False,
         "Быстрая ручка замедляет действие при проекции. Значение должно быть не меньше минимального FPS."),
        ("variation", "fps_drift_strength", 0.7, 0, 1, False,
         "0 — ровная ручка; 0,6–0,8 — естественная неравномерность; 1 — полный заданный размах."),
        ("irregularity", "crank_irregularity", 0.15, 0, 1, False,
         "Неповторяющиеся отклонения от оборота к обороту. Начни с 0,1–0,25."),
        ("seed", "seed", 1925, 0, 2147483647, True,
         "Фиксирует расписание ручки и связанное движение камеры. Один оборот соответствует восьми кадрам."),
        ("shutter", "shutter_fraction", 0.65, 0, 1.5, False,
         "0,5 — половина периода снятого кадра. Медленнее ручка — длиннее выдержка. Действует вместе с motion_blur_strength; при ненулевой силе смаза есть минимальная ширина выдержки, как в Comfy."),
        ("motion_blur", "motion_blur_strength", 0.8, 0, 2, False,
         "0 выключает временную интеграцию; 0,8 — начальное значение. Оптический поток применяется к дробным позициям каждого отсчёта выдержки в выбранном режиме интерполяции."),
        ("samples", "blur_samples", 9, 1, 17, True,
         "9 — как в Comfy; 5 — для более быстрого расчёта. При 3 треугольные веса почти полностью оставляют центральный отсчёт: это не полноценный смаз, дефекты flow могут стать заметнее. При 1 также берётся центр; flow дробного центра сохраняется."),
        ("exposure_link", "exposure_variation", 1, 0, 1, False,
         "0 — постоянная экспозиция; 1 — полный закон выдержки. Bake Exposure запекает её, Exposure via OFX сохраняет точную карту для отдельной ноды с Blend. При постоянной скорости ручки пульсации нет."),
        ("shake", "camera_shake_strength", 0.25, 0, 1, False,
         "Движение камеры внутри выдержки, связанное с оборотом ручки. Начни с 0,25; 0 выключает."),
    ]
    FIELD_GROUPS = [
        ("Source Timing", ("source_fps",)),
        ("Hand Crank", ("fps_min", "fps_max", "variation", "irregularity", "seed")),
        ("Exposure", ("shutter", "motion_blur", "samples", "exposure_link")),
        ("Camera Motion", ("shake",)),
    ]
    PRESET_NUMBERS = ("source_fps", "fps_min", "fps_max", "variation", "irregularity", "seed",
                      "shutter", "motion_blur", "samples", "exposure_link", "shake")

    def __init__(self, window, args):
        self.window, self.args = window, args
        self.executor = ThreadPoolExecutor(max_workers=1)
        self.future = None
        self.next_source_refresh = 0
        self.context = {}
        self.source_snapshot = None
        self.selection_items = []
        self.batch = None
        self.job_target = None
        self.process = None
        self.running_job = None
        self.baking_job = None
        self.resume_job = None
        self.opened_job = None
        self.next_bake_poll = 0
        self.bake_cancel_requested = False
        self.last_bake_error = None
        self.auto_pending = False
        self.active_import_mode = "above"
        self.imported_job = None
        self.dialogs = set()
        self.values = {}
        self.closed = False
        from cadence_preferences import PreferenceStore
        self.preference_numbers = {key: (default, low, high, integer)
                                   for key, label, default, low, high, integer, hint in self.FIELDS
                                   if key in self.PRESET_NUMBERS}
        self.preference_choices = {
            "exposure_mode": ("bake", ("bake", "ofx")),
            "blur_mode": ("flow_integrated", ("flow_integrated", "simple")),
            "compute_device": (args.backend, ("cuda", "cpu")),
            "format": ("prores", ("prores", "tiff")),
            "input_transfer": ("auto", ("auto", "srgb", "gamma24", "linear")),
            "placement": ("above", ("replace", "above")),
            "source_fps_mode": ("timeline", ("timeline", "source", "custom")),
        }
        self.preferences = PreferenceStore(self.preference_numbers, self.preference_choices,
                                           {"auto_import": True, "preserve_alpha": False, "use_raw_source": True},
                                           texts={"output_folder": args.root})
        self._preferences_loading = True
        self._preference_timer = None
        self._draft_preferences = self.preferences.current
        self.build_ui()
        self.apply_preferences(self._draft_preferences)
        self._preferences_loading = False
        self.connect_preferences()
        if self.preferences.warning:
            self.preference_status.set_text(self.preferences.warning)
        previous = Path(self.root.get_text()).expanduser() / "last_app_job.json"
        try:
            self.job.set_text(json.loads(previous.read_text(encoding="utf-8"))["directory"])
        except (OSError, ValueError, KeyError):
            pass
        window.connect("close-request", self.close)
        self.poll_source = GLib.timeout_add(500, self.poll)

    def guarded(self, action):
        """Keep recoverable failures visible inside the application."""
        try:
            action()
        except Exception as error:
            self.stop_batch(str(error))
            self.status.set_text("Ошибка: " + str(error))
            self.message(str(error), Gtk.MessageType.ERROR)

    @staticmethod
    def label(text, wrap=True):
        """Use Pango Unicode wrapping and the desktop font without fixed pixel metrics."""
        widget = Gtk.Label(label=text, xalign=0)
        if wrap:
            widget.set_wrap(True)
            widget.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
            widget.set_max_width_chars(90)
            widget.set_hexpand(True)
        return widget

    @staticmethod
    def entry(text="", editable=True):
        """Create a selectable text field, including long Unicode file paths."""
        widget = Gtk.Entry(text=str(text), hexpand=True)
        widget.set_editable(editable)
        return widget

    def button(self, text, action):
        """Create a guarded GTK command button."""
        widget = Gtk.Button(label=text)
        widget.connect("clicked", lambda button: self.guarded(action))
        return widget

    def message(self, text, kind=Gtk.MessageType.INFO, confirm=None):
        """Show a nonblocking message or close confirmation on GTK 4.0 and later."""
        dialog = Gtk.MessageDialog(transient_for=self.window, modal=True,
                                   message_type=kind,
                                   buttons=Gtk.ButtonsType.OK_CANCEL if confirm else Gtk.ButtonsType.OK,
                                   text="SilentFilm Cadence", secondary_text=text)
        self.dialogs.add(dialog)
        def response(dialog, code):
            dialog.destroy()
            self.dialogs.discard(dialog)
            if confirm and code == Gtk.ResponseType.OK:
                self.guarded(confirm)
        dialog.connect("response", response)
        dialog.present()

    def choose_path(self, title, action, callback):
        """Keep a native chooser alive until its asynchronous response arrives."""
        dialog = Gtk.FileChooserNative.new(title, self.window, action, "Select", "Cancel")
        dialog.set_modal(True)
        self.dialogs.add(dialog)
        def response(dialog, code):
            selected = dialog.get_file() if code == Gtk.ResponseType.ACCEPT else None
            path = selected.get_path() if selected else None
            dialog.destroy()
            self.dialogs.discard(dialog)
            if path:
                self.guarded(lambda: callback(path))
        dialog.connect("response", response)
        dialog.show()

    def build_ui(self):
        """Build a scrollable GTK form with Pango labels and persistent progress controls."""
        self.window.set_title("SilentFilm — Camera Cadence")
        self.window.set_default_size(940, 850)
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        for setter in (outer.set_margin_top, outer.set_margin_bottom, outer.set_margin_start, outer.set_margin_end):
            setter(16)
        self.window.set_child(outer)
        title = self.label("Hand-Crank Camera Cadence")
        title.add_css_class("title-2")
        outer.append(title)
        self.batch_label = self.label("Можно выделить несколько видеоклипов для последовательной обработки.")
        outer.append(self.batch_label)
        self.batch_progress = Gtk.ProgressBar(show_text=True)
        outer.append(self.batch_progress)
        outer.append(self.label("Одна экспозиция → один кадр показа. Экспозицию ручки можно запечь или применять позже в SilentFilm Crank Exposure."))
        self.ignore_processing = Gtk.CheckButton(label="Use Raw Source (In/Out Only)", active=True)
        self.ignore_processing.set_tooltip_text("Не учитывать обработки клипа. Кадры исходника приводятся к монтажному диапазону и FPS таймлайна; длительность входа одинакова в обоих режимах. Частоту интерпретации задаёт source_fps_mode. Гамму входа выбери по исходнику. По умолчанию включено; сохраняется в настройках и пресетах.")
        self.ignore_processing.connect("toggled", lambda widget: self.guarded(self.update_source_mode))
        outer.append(self.ignore_processing)
        scroll = Gtk.ScrolledWindow(vexpand=True)
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        outer.append(scroll)
        form = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        scroll.set_child(form)
        presets = Gtk.Grid(column_spacing=10, row_spacing=8)
        form.append(presets)
        self._preset_names = self.preferences.names()
        self.preset_list = Gtk.DropDown.new_from_strings(["Custom"] + self._preset_names)
        self.preset_list.set_hexpand(True)
        self.preset_list.set_enable_search(True)
        self.preset_list.set_tooltip_text("Пресет содержит параметры съёмки и режим source_fps. В timeline/source FPS читается заново из текущего источника, в custom восстанавливается ручное значение. Размеры и In/Out не меняются.")
        self.preset_name = self.entry()
        self.preset_name.set_placeholder_text("Preset Name")
        self.preset_name.set_tooltip_text("Имя собственного пресета. При совпадении имени приложение предложит подтвердить перезапись.")
        self.save_preset_button = self.button("Save Preset", self.save_preset)
        self.reset_button = self.button("Factory Reset", self.factory_reset)
        self.reset_button.set_tooltip_text("Сбросить настройки эффекта на заводские, сохранив именованные пресеты, клип, размеры и диапазон. source_fps_mode вернётся к timeline, частота показа останется из таймлайна.")
        presets.attach(self.label("Preset", False), 0, 0, 1, 1)
        presets.attach(self.preset_list, 1, 0, 1, 1)
        presets.attach(self.reset_button, 2, 0, 1, 1)
        presets.attach(self.label("Preset Name", False), 0, 1, 1, 1)
        presets.attach(self.preset_name, 1, 1, 1, 1)
        presets.attach(self.save_preset_button, 2, 1, 1, 1)
        self.preference_status = self.label("Настройки эффекта и Output Folder сохраняются автоматически; размеры, диапазон и путь исходника не сохраняются.")
        form.append(self.preference_status)
        source = Gtk.Grid(column_spacing=10, row_spacing=8)
        form.append(source)
        source.attach(self.label("Selected Timeline Clip", False), 0, 0, 1, 1)
        self.timeline_label = self.label("Выдели видеоклипы на Edit; приложение загрузит их монтажные диапазоны.")
        source.attach(self.timeline_label, 1, 0, 2, 1)
        self.root = self.entry(self.args.root)
        self.root.set_tooltip_text("Корень хранения результатов SilentFilm. Внутри создаются папка проекта и папки клипов с cadence и уникальным идентификатором. Сохраняется в настройках и пресетах.")
        source.attach(self.label("Output Folder", False), 0, 1, 1, 1)
        source.attach(self.root, 1, 1, 1, 1)
        self.root_browse = self.button("Browse…", self.select_root)
        source.attach(self.root_browse, 2, 1, 1, 1)
        self.clean_button = self.button("Clean Project Cache", self.clean_cache)
        self.clean_button.set_tooltip_text("Показать завершённые неиспользуемые результаты только текущего проекта. Проверяются все его таймлайны; удаление выполняется после подтверждения списка.")
        self.clean_timeline_button = self.button("Clean Timeline Cache", lambda: self.clean_cache("timeline"))
        self.clean_timeline_button.set_tooltip_text("Очистить неиспользуемые генерации, созданные для текущего таймлайна. Результаты, используемые на любом другом таймлайне проекта, сохраняются. Удаляются и неиспользуемые записи Media Pool.")
        source.attach(self.clean_timeline_button, 1, 2, 1, 1)
        source.attach(self.clean_button, 2, 2, 1, 1)
        heading = self.label("Processing")
        heading.add_css_class("heading")
        form.append(heading)
        options = Gtk.Grid(column_spacing=12, row_spacing=6)
        form.append(options)
        self.exposure_mode = Gtk.DropDown.new_from_strings(["Bake Exposure", "Exposure via OFX"])
        self.exposure_mode.set_tooltip_text("Bake — экспозиция в готовом видео. OFX — отдельная карта и попытка автоматически настроить SilentFilm Crank Exposure в Fusion: путь карты, Blend1, Frame Offset0. Blend меняется без пересчёта Cadence на странице Fusion. Если API откажет, появится инструкция ручной настройки. Выбор сохраняется в пресетах.")
        options.attach(self.label("Crank Exposure", False), 0, 2, 1, 1)
        options.attach(self.exposure_mode, 1, 2, 2, 1)
        self.backend = Gtk.DropDown.new_from_strings(["cuda", "cpu"])
        self.backend.set_selected(0 if self.args.backend == "cuda" else 1)
        self.backend.set_tooltip_text("cpu — нативная обработка на CPU; cuda — интеграция выдержки, shake и запекаемая экспозиция на GPU. Farneback оценивается на CPU OpenCV, flow-деформации и смешивание выполняются на CUDA. Exposure via OFX переносит применение экспозиции в отдельную ноду. Автоматический fallback при OOM не реализован.")
        self.format = Gtk.DropDown.new_from_strings(["ProRes 4444", "TIFF RGBA16"])
        self.transfer = Gtk.DropDown.new_from_strings(["Auto (Resolve)", "sRGB", "Rec.709 Gamma 2.4", "Linear Rec.709"])
        self.transfer.set_selected(0)
        self.transfer.set_tooltip_text("Для таймлайна начни с Авто. Если Resolve не сообщает гамму, выбери фактическую рабочую гамму из Project Settings. Это не CST и не преобразование цвета.")
        for column, text, widget in ((0, "compute_device", self.backend), (1, "Output Format", self.format), (2, "Input Transfer", self.transfer)):
            options.attach(self.label(text, False), column, 0, 1, 1)
            options.attach(widget, column, 1, 1, 1)
        self.interpolation = Gtk.DropDown.new_from_strings(["flow_integrated", "simple"])
        self.interpolation.set_selected(0)
        self.interpolation.set_tooltip_text("flow_integrated согласует движение соседних кадров с защитой перекрытий и контуров, как в Comfy. simple смешивает кадры без компенсации движения.")
        self.source_fps_mode = Gtk.DropDown.new_from_strings(["timeline", "source", "custom"])
        self.source_fps_mode.set_selected(0)
        self.source_fps_mode.set_tooltip_text("timeline — FPS текущего таймлайна Resolve; source — FPS оригинального исходного клипа; custom — ручная интерпретация кадров. Не меняет output_fps.")
        self.timeline_fps_label = self.label("timeline_fps: Auto")
        self.timeline_fps_label.set_tooltip_text("FPS показа результата всегда берётся из текущего таймлайна перед расчётом; не сохраняется в пресетах.")
        form.append(self.timeline_fps_label)
        self.range_label = self.label("Source Range: No Selection")
        self.range_label.set_tooltip_text("Диапазон берётся только из выбранного клипа таймлайна. Ручного ввода нет.")
        self.hint = self.label("Наведи указатель на параметр — здесь появится рекомендация.")
        self.hint.add_css_class("dim-label")
        specs = {field[0]: field for field in self.FIELDS}
        for group_name, keys in self.FIELD_GROUPS:
            heading = self.label(group_name)
            heading.add_css_class("heading")
            form.append(heading)
            parameters = Gtk.Grid(column_spacing=14, row_spacing=8)
            form.append(parameters)
            row = 0
            if group_name == "Source Timing":
                parameters.attach(self.label("source_fps_mode"), 0, row, 1, 1)
                parameters.attach(self.source_fps_mode, 1, row, 1, 1)
                row += 1
            if group_name == "Exposure":
                parameters.attach(self.label("blur_mode"), 0, row, 1, 1)
                parameters.attach(self.interpolation, 1, row, 1, 1)
                row += 1
            for key in keys:
                key, text, default, low, high, integer, hint = specs[key]
                label = self.label(text)
                label.set_max_width_chars(28)
                entry = self.entry(default)
                entry.set_width_chars(10)
                self.values[key] = entry
                parameters.attach(label, 0, row, 1, 1)
                parameters.attach(entry, 1, row, 1, 1)
                row += 1
                label.set_mnemonic_widget(entry)
                for widget in (label, entry):
                    widget.set_tooltip_text(hint)
                    motion = Gtk.EventControllerMotion()
                    motion.connect("enter", lambda controller, x, y, text=hint: self.hint.set_text(text))
                    widget.add_controller(motion)
                    focus = Gtk.EventControllerFocus()
                    focus.connect("enter", lambda controller, text=hint: self.hint.set_text(text))
                    widget.add_controller(focus)
            if group_name == "Source Timing":
                parameters.attach(self.range_label, 0, row, 2, 1)
        self.source_fps_mode.connect("notify::selected", lambda *args: self.source_fps_mode_changed())
        self.update_source_fps_display()
        form.append(self.hint)
        form.append(self.label("CFR SDR Rec.709, без звука. ProRes — с потерями; TIFF — без потерь. Выход: цветной sRGB до оптики."))
        self.auto_import = Gtk.CheckButton(label="Auto Import", active=True)
        form.append(self.auto_import)
        form.append(self.label("Output Placement"))
        self.placement = Gtk.DropDown.new_from_strings(["Replace Selected Clip", "Add Above Source"])
        self.placement.set_tooltip_text("Добавление использует ближайшую существующую свободную видеодорожку выше исходной, доступную на всю длину результата. Новая верхняя дорожка создаётся только при отсутствии места. Оригинал сохраняется.")
        self.placement.set_selected(1)
        self.placement.set_sensitive(False)
        form.append(self.placement)
        self.preserve_alpha = Gtk.CheckButton(label="Preserve Alpha", active=False)
        self.preserve_alpha.set_tooltip_text("Для непрозрачного видео можно выключить. Без alpha прозрачные области запекутся на чёрном фоне.")
        form.append(self.preserve_alpha)
        form.append(self.label("При сокращении останется промежуток. Длинному результату нужно свободное место. Перед заменой создаётся копия таймлайна."))
        form.append(self.label("Кадры с обработками запрашиваются у Resolve по мере расчёта каденса. Во время расчёта не меняй обработки выбранного клипа."))
        actions = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE, column_spacing=8, row_spacing=8)
        actions.set_max_children_per_line(5)
        outer.append(actions)
        self.start_button = self.button("Calculate", self.start)
        self.start_button.add_css_class("suggested-action")
        self.import_button = self.button("Import", self.import_job)
        for widget in (self.start_button, self.button("Cancel", self.cancel), self.import_button,
                       self.button("Open Job…", self.open_job), self.button("Show Folder", self.show_directory)):
            actions.insert(widget, -1)
        self.progress = Gtk.ProgressBar(show_text=True)
        outer.append(self.progress)
        self.status = self.label("Выдели один или несколько видеоклипов на таймлайне. В Resolve включи External scripting → Local.")
        self.status.set_selectable(True)
        outer.append(self.status)
        paths = Gtk.Grid(column_spacing=10, row_spacing=8)
        outer.append(paths)
        self.job = self.entry(editable=False)
        paths.attach(self.label("Job", False), 0, 0, 1, 1)
        paths.attach(self.job, 1, 0, 2, 1)
        self.exposure_map_path = self.entry(editable=False)
        self.exposure_map_path.set_placeholder_text("Exposure Map — появится после расчёта")
        self.exposure_map_path.set_tooltip_text("Путь можно скопировать и выбрать в Exposure Map ноды SilentFilm Crank Exposure. Для Bake дополнительная нода не нужна.")
        outer.append(self.exposure_map_path)
        outer.append(self.label("Exposure via OFX: карта назначается автоматически в Fusion → SilentFilm Crank Exposure. Затем Camera Optics → Negative → Print → Projector. При отказе API появится инструкция ручной настройки."))

    def preference_widgets(self):
        """Map stable choices to controls while excluding clip paths and ranges."""
        return {"blur_mode": self.interpolation, "compute_device": self.backend,
                "exposure_mode": self.exposure_mode,
                "format": self.format, "input_transfer": self.transfer, "placement": self.placement,
                "source_fps_mode": self.source_fps_mode,
                "auto_import": self.auto_import, "preserve_alpha": self.preserve_alpha,
                "use_raw_source": self.ignore_processing}

    def read_preference(self, key):
        """Read one effect control without requiring a source clip or valid timing metadata."""
        if key == "output_folder":
            return self.root.get_text()
        if key in self.preference_numbers:
            if key == "source_fps" and self.source_fps_mode.get_selected() != 2:
                return self._draft_preferences["source_fps"]
            value = float(self.values[key].get_text().replace(",", "."))
            _, low, high, integer = self.preference_numbers[key]
            if not math.isfinite(value) or not low <= value <= high or (integer and value != int(value)):
                raise ValueError("Недопустимое значение: " + key)
            return int(value) if integer else value
        widget = self.preference_widgets()[key]
        if key in self.preference_choices:
            return self.preference_choices[key][1][widget.get_selected()]
        return widget.get_active()

    def snapshot_preferences(self):
        """Capture only allowed controls for an explicitly saved preset."""
        return self.preferences.validate({key: self.read_preference(key) for key in self.preferences.factory})

    def context_change(self, widget, method, value):
        """Apply source/job metadata without treating it as an edited global preference."""
        previous = getattr(self, "_preferences_loading", False)
        self._preferences_loading = True
        try:
            getattr(widget, method)(value)
        finally:
            self._preferences_loading = previous

    def refresh_presets(self, selected=None):
        """Refresh Unicode preset names without triggering another selection action."""
        names = self.preferences.names()
        previous = self._preferences_loading
        self._preferences_loading = True
        try:
            # Замена модели из notify::selected повторно уведомляет GTK о выборе
            # после выхода из обработчика. При загрузке значений список не меняется.
            if names != self._preset_names:
                self._preset_names = list(names)
                self.preset_list.set_model(Gtk.StringList.new(["Custom"] + names))
            index = names.index(selected) + 1 if selected in names else 0
            if self.preset_list.get_selected() != index:
                self.preset_list.set_selected(index)
            if selected:
                self.preset_name.set_text(selected)
        finally:
            self._preferences_loading = previous

    def apply_preferences(self, values):
        """Apply effect values and FPS interpretation choices without changing clip bounds or sizes."""
        values = self.preferences.validate(values)
        if self._preference_timer is not None:
            GLib.source_remove(self._preference_timer)
            self._preference_timer = None
        previous = self._preferences_loading
        self._preferences_loading = True
        try:
            self._draft_preferences = values
            self.root.set_text(values["output_folder"])
            for key in self.preference_numbers:
                self.values[key].set_text(str(values[key]))
            for key, widget in self.preference_widgets().items():
                if key in self.preference_choices:
                    widget.set_selected(self.preference_choices[key][1].index(values[key]))
                else:
                    widget.set_active(values[key])
            self._preference_dirty = False
            self.refresh_presets(self.preferences.selected)
            self.update_source_mode()
            self.update_source_fps_display()
        finally:
            self._preferences_loading = previous

    def connect_preferences(self):
        """Debounce user edits while ignoring automatic FPS/range updates."""
        self.root.connect("changed", lambda widget: self.preference_changed("output_folder"))
        for key in self.preference_numbers:
            self.values[key].connect("changed", lambda widget, key=key: self.preference_changed(key))
        for key, widget in self.preference_widgets().items():
            if key in self.preference_choices:
                widget.connect("notify::selected", lambda widget, prop, key=key: self.preference_changed(key))
            else:
                widget.connect("toggled", lambda widget, key=key: self.preference_changed(key))
        self.preset_list.connect("notify::selected", lambda *args: None if self._preferences_loading else self.guarded(self.select_preset))

    def preference_changed(self, key):
        """Keep invalid intermediate typing out of the on-disk settings document."""
        if self._preferences_loading or self.closed:
            return
        try:
            self._draft_preferences[key] = self.read_preference(key)
        except (ValueError, IndexError):
            return
        self._preference_dirty = True
        self.context_change(self.preset_list, "set_selected", 0)
        if self._preference_timer is not None:
            GLib.source_remove(self._preference_timer)
        self._preference_timer = GLib.timeout_add(600, self.autosave_preferences)

    def flush_preferences(self):
        """Commit the latest valid edited values before closing or switching presets."""
        if self._preference_timer is not None:
            GLib.source_remove(self._preference_timer)
            self._preference_timer = None
        if self._preference_dirty:
            self.preferences.set_current(self._draft_preferences)
            self._preference_dirty = False

    def autosave_preferences(self):
        """Report storage failures without interrupting rendering or discarding the last good file."""
        self._preference_timer = None
        try:
            self.flush_preferences()
            self.preference_status.set_text("Настройки эффекта сохранены автоматически.")
        except (OSError, ValueError) as error:
            self.preference_status.set_text("Настройки не сохранены: " + str(error))
        return GLib.SOURCE_REMOVE

    def select_preset(self):
        """Apply a named effect preset immediately, leaving all clip timing metadata intact."""
        if self.running_job or self.future or self.resume_job or self.batch_active():
            raise RuntimeError("Дождись завершения задания перед выбором пресета")
        index = self.preset_list.get_selected()
        names = self.preferences.names()
        if not 0 <= index <= len(names):
            return
        if index == 0:
            self.preferences.set_current(self._draft_preferences)
            return
        name = names[index - 1]
        if self.preferences.selected == name and not self._preference_dirty:
            return
        self.preferences.set_current(self.preferences.preset(name), selected=name)
        self.apply_preferences(self.preferences.current)
        self.preference_status.set_text(f"Пресет «{name}» применён. Размеры и диапазон клипа не менялись; автоматические FPS берутся из текущего источника.")

    def save_preset(self):
        """Save a named preset, asking before replacing an existing name."""
        if self.running_job or self.future or self.resume_job or self.batch_active():
            raise RuntimeError("Дождись завершения задания перед сохранением пресета")
        name = self.preferences.validate_name(self.preset_name.get_text())
        values = self.snapshot_preferences()
        def save():
            actual = self.preferences.save_preset(name, values, replace=True)
            self.apply_preferences(self.preferences.current)
            self.preference_status.set_text(f"Пресет «{actual}» сохранён.")
        if self.preferences.existing_name(name):
            self.message(f"Пресет «{name}» уже существует. Перезаписать его текущими настройками?", confirm=save)
        else:
            save()

    def factory_reset(self):
        """Reset only persistent effect settings, preserving named presets and source metadata."""
        if self.running_job or self.future or self.resume_job or self.batch_active():
            raise RuntimeError("Дождись завершения задания перед сбросом настроек")
        self.preferences.reset()
        self.apply_preferences(self.preferences.current)
        self.preset_name.set_text("")
        self.preference_status.set_text("Заводские настройки эффекта восстановлены. Именованные пресеты, клип и диапазон сохранены.")

    def bridge(self, request):
        """Run one Resolve RPC in its matching bundled interpreter."""
        interpreter = Path(self.args.resolve_python)
        if not interpreter.is_file():
            raise RuntimeError("Не найден внешний Python Resolve 21.1+: " + str(interpreter))
        result = subprocess.run([str(interpreter), str(Path(__file__).with_name("cadence_resolve_bridge.py"))],
                                input=json.dumps(request, ensure_ascii=False), text=True, capture_output=True,
                                encoding="utf-8", errors="replace", env=child_environment(),
                                timeout=600 if request["action"] == "cache_clean" else 60)
        replies = [line[len("SILENTFILM_JSON:"):] for line in result.stdout.splitlines()
                   if line.startswith("SILENTFILM_JSON:")]
        reply = json.loads(replies[-1]) if replies else None
        if not reply or not reply.get("ok"):
            try:
                directory = Path(request.get("job", self.args.root))
                directory.mkdir(parents=True, exist_ok=True)
                with (directory / "resolve_bridge.log").open("a", encoding="utf-8") as log:
                    log.write("\n" + request["action"] + ":\n" + result.stderr + "\n" + result.stdout + "\n")
            except OSError:
                pass
        if not replies:
            raise RuntimeError("Resolve API не ответил: " + (result.stderr or result.stdout)[-1800:])
        if not reply["ok"]:
            raise RuntimeError(reply["error"])
        return reply["result"]

    def submit(self, request, callback, error_callback=None):
        """Dispatch serialized Resolve work without blocking the UI thread."""
        if self.future:
            raise RuntimeError("Дождись завершения текущей операции Resolve")
        self.future = (self.executor.submit(self.bridge, request), callback, error_callback)

    def load_timeline_source(self, quiet=False, after=None):
        """Read all selected video instances without changing the running queue."""
        if self.running_job or self.future or self.batch_active():
            if quiet:
                return
            raise RuntimeError("Дождись завершения текущей операции")
        def received(result):
            items = result.get("items", [result])
            if quiet and self.source_snapshot and self.selection_items == items:
                return
            self.selection_items = items
            result = items[0]
            source = result["source"]
            self.context = {key: value for key, value in result.items() if key != "source"}
            self.source_snapshot = dict(source, **self.context)
            self.job_target = self.resume_job = self.opened_job = None
            self.placement.set_sensitive(True)
            target = result["target"]
            self.timeline_label.set_text(f'{result["project_name"]} / {target["timeline_name"]}: {source["name"]}, V{target["track_index"]}, кадры {target["start"]}–{target["end"] - 1}')
            self.batch_label.set_text(f"Выбрано видеоклипов: {len(items)}. Общие настройки применяются ко всем.")
            self.status.set_text("Нажми Calculate для последовательной обработки выделенных клипов.")
            self.update_source_mode()
            if after:
                after()
        def failed(error):
            self.source_snapshot = None
            self.selection_items = []
            self.timeline_label.set_text("Выдели один или несколько видеоклипов на таймлайне.")
            self.range_label.set_text("Source Range: No Selection")
            self.status.set_text(str(error))
        self.next_source_refresh = time.monotonic() + 5
        self.submit({"action": "timeline_sources"}, received, failed if quiet else None)

    def batch_active(self):
        """Include gaps between individual jobs in the busy state."""
        return bool(getattr(self, "batch", None) and self.batch.active)

    def start_selection(self):
        """Freeze the selected instances once before starting a sequential queue."""
        self.batch = None
        if len(self.selection_items) <= 1:
            self.start_selected()
            return
        from cadence_batch import BatchQueue
        self.batch = BatchQueue(self.selection_items, self.snapshot_preferences(), self.root.get_text())
        self.next_batch_item()

    def next_batch_item(self):
        """Start one saved item without consulting the changing UI selection."""
        if not self.batch_active():
            return False
        self.batch.begin()
        result = self.batch.current["snapshot"]
        self.context = {key: value for key, value in result.items() if key != "source"}
        self.source_snapshot = dict(result["source"], **self.context)
        self.job_target = self.resume_job = self.opened_job = None
        self.process = None
        self.job.set_text("")
        self.progress.set_fraction(0.0)
        self.batch_label.set_text(f'Клип {self.batch.index + 1}/{len(self.batch.entries)}: {result["source"]["name"]}')
        self.update_source_mode()
        self.guarded(self.start_selected)
        return False

    def finish_batch_item(self, outcome, message=""):
        """Advance only after rendering and the requested import have finished."""
        if not self.batch_active():
            return
        self.batch.complete(self.job.get_text(), outcome, message)
        if self.batch_active():
            GLib.idle_add(self.next_batch_item)
        else:
            self.batch_progress.set_fraction(1.0)
            self.batch_progress.set_text(f"{len(self.batch.entries)}/{len(self.batch.entries)} завершено")
            self.batch_label.set_text(f"Очередь завершена: {len(self.batch.entries)} клипов. Отчёт: {self.batch.path}")

    def stop_batch(self, message, state="error"):
        """Preserve completed results and leave remaining clips untouched."""
        if self.batch_active():
            self.batch.stop(message, state)
            self.auto_pending = False
            self.batch_label.set_text(f"Очередь остановлена: {message}. Отчёт: {self.batch.path}")

    def update_source_mode(self):
        """Refresh read-only source-range metadata for the selected processing mode."""
        target = (self.source_snapshot or {}).get("target")
        if not target:
            return
        if self.ignore_processing.get_active():
            capture_range(self.source_snapshot, raw=True)
            if self.transfer.get_selected() == 0:
                self.context_change(self.transfer, "set_selected", 1)
            self.status.set_text("Исходные кадры без обработок будут приведены к In/Out и FPS таймлайна. Длительность входа не изменится.")
        self.update_range_display()

    def select_root(self):
        """Choose an output directory."""
        self.choose_path("Output Folder", Gtk.FileChooserAction.SELECT_FOLDER, self.root.set_text)

    def clean_cache(self, scope="project"):
        """Preview current-project garbage and require confirmation of the concrete deletion list."""
        if self.running_job or self.future or self.resume_job or self.batch_active():
            raise RuntimeError("Дождись завершения задания перед очисткой кэша")
        root = str(Path(self.preferences.validate({"output_folder": self.root.get_text()})["output_folder"]).expanduser().resolve())
        def planned(plan):
            entries = plan["entries"]
            if not entries:
                self.message("В выбранной Output Folder нет завершённых неиспользуемых результатов для выбранной области очистки.")
                return
            names = "\n".join(Path(item["path"]).name for item in entries)
            def cleaned(result):
                deleted = set(result["deleted"])
                pointer = Path(root) / "last_app_job.json"
                try:
                    if json.loads(pointer.read_text(encoding="utf-8")).get("directory") in deleted:
                        pointer.unlink()
                except (OSError, ValueError):
                    pass
                if self.job.get_text() in deleted:
                    self.job.set_text("")
                    self.opened_job = self.job_target = None
                    self.imported_job = None
                self.status.set_text(f'Кэш проекта очищен: удалено {len(deleted)}, пропущено {result["skipped"]}, ошибок {len(result["errors"])}.')
                if result["errors"]:
                    self.message("\n".join(result["errors"]), Gtk.MessageType.ERROR)
            def execute():
                if self.running_job or self.future or self.resume_job:
                    raise RuntimeError("Дождись завершения текущей операции")
                self.submit({"action": "cache_clean", "root": root, "project_id": plan["project_id"],
                             "scope": scope, "timeline_id": plan.get("timeline_id"), "entries": entries}, cleaned)
            area = f'Таймлайн: {plan["timeline_name"]}' if scope == "timeline" else "Все таймлайны проекта"
            self.message(f'Проект: {plan["project_name"]}\n{area}\nУдалить {len(entries)} результатов ({plan["bytes"] / (1024 ** 2):.1f} MiB)?\n\n{names}\n\nОни не используются на таймлайнах этого проекта. Файлы будут удалены вместе с их неиспользуемыми записями Media Pool. Undo монтажа не восстановит удалённые файлы. Папки других проектов не затрагиваются.', confirm=execute)
        def context_received(context):
            self.submit({"action": "cache_plan", "root": root, "project_id": context["project_id"],
                         "scope": scope, "timeline_id": context.get("timeline_id")}, planned)
        self.submit({"action": "context"}, context_received)

    def update_range_display(self):
        """Show source/timeline bounds without providing editable range controls."""
        snapshot = self.source_snapshot
        if self.opened_job:
            self.range_label.set_text("Source Range: Saved Job")
            return
        if not snapshot:
            self.range_label.set_text("Source Range: No Selection")
            return
        raw = bool(snapshot.get("target") and self.ignore_processing.get_active())
        try:
            interval = capture_range(snapshot, raw)
        except (ValueError, RuntimeError, KeyError, TypeError):
            self.range_label.set_text("Source Range: Unavailable")
            return
        if snapshot.get("target"):
            target = snapshot["target"]
            self.range_label.set_text(f'Timeline In: {target["start"]} | Out: {target["end"] - 1} | Frames: {interval["frame_count"]}')
        else:
            start, count = interval["start_frame"], interval["frame_count"]
            end = str(start + count - 1) if count else "Auto"
            self.range_label.set_text(f"Source In: {start} | Out: {end}" + (f" | Frames: {count}" if count else ""))

    def source_fps_mode_changed(self):
        """Restore the saved custom value while automatic modes always use current metadata."""
        if self.source_fps_mode.get_selected() == 2:
            self.context_change(self.values["source_fps"], "set_text", str(self._draft_preferences["source_fps"]))
        self.update_source_fps_display()

    def update_source_fps_display(self):
        """Refresh automatic FPS values without persisting the previous clip's metadata."""
        mode = ("timeline", "source", "custom")[self.source_fps_mode.get_selected()]
        widget = self.values["source_fps"]
        busy = bool(self.running_job or self.future or self.resume_job or self.batch_active())
        self.source_fps_mode.set_sensitive(not busy)
        widget.set_editable(mode == "custom" and not busy)
        metadata = self.source_snapshot or self.context
        rate = metadata.get("timeline_fps")
        self.timeline_fps_label.set_text(f"timeline_fps: {rate:g}" if isinstance(rate, (int, float)) and rate > 0 else "timeline_fps: Auto")
        self.update_range_display()
        if mode == "custom":
            return
        snapshot = self.source_snapshot or {}
        context = snapshot or self.context
        try:
            value = source_interpretation_fps(mode, 0, context, snapshot)
            self.context_change(widget, "set_text", f"{value:g}" if value else "Auto")
        except (ValueError, TypeError, RuntimeError):
            self.context_change(widget, "set_text", "Auto")

    def numeric_request(self):
        """Validate all capture controls before creating a job."""
        request = {}
        for key, label, default, low, high, integer, hint in self.FIELDS:
            if key == "source_fps" and self.source_fps_mode.get_selected() != 2:
                request[key] = 0
                continue
            try:
                value = float(self.values[key].get_text().replace(",", "."))
            except ValueError:
                raise ValueError("Некорректное число: " + label) from None
            if not math.isfinite(value) or not low <= value <= high or (integer and value != int(value)):
                raise ValueError(f"{label}: допустимо {low}…{high}" + (", целое число" if integer else ""))
            request[key] = int(value) if integer else value
        if request["fps_min"] > request["fps_max"]:
            raise ValueError("Минимальный FPS должен быть не больше максимального")
        snapshot = self.source_snapshot
        if not snapshot or not snapshot.get("target"):
            raise ValueError("Выдели один видеоклип на таймлайне Resolve")
        path = Path(snapshot["path"]).expanduser()
        if not path.is_file():
            raise ValueError("Выбери доступный исходный видеофайл")
        choice = self.transfer.get_selected()
        if choice == 0 and self.ignore_processing.get_active():
            raise ValueError("Для исходника без обработок укажи фактическую гамму входного файла")
        request.update(capture_range(snapshot, bool(snapshot and snapshot.get("target") and self.ignore_processing.get_active())))
        request.update(schema=1, processing="capture_rgb", input=str(path.resolve()),
                       exposure_mode=("bake", "ofx")[self.exposure_mode.get_selected()],
                       blur_mode=("flow_integrated", "simple")[self.interpolation.get_selected()],
                       device=("cuda", "cpu")[self.backend.get_selected()], format="prores" if self.format.get_selected() == 0 else "tiff",
                       input_transfer=("srgb", "srgb", "gamma24", "linear")[choice])
        if self.source_snapshot and request["input"] == str(Path(self.source_snapshot["path"]).resolve()):
            if self.source_snapshot["fps"] > 0:
                request["expected_source_fps"] = self.source_snapshot["fps"]
        return request

    def start(self):
        """Refresh the actual timeline selection immediately before creating a new capture."""
        if self.running_job or self.future or self.batch_active():
            raise RuntimeError("Дождись завершения текущей операции")
        if self.opened_job or self.resume_job:
            return self.start_selected()
        self.load_timeline_source(after=self.start_selection)

    def start_selected(self):
        """Create an isolated request and start the native renderer without a shell."""
        if self.process and self.process.poll() is None:
            raise RuntimeError("Предыдущее задание ещё выполняется")
        if self.running_job:
            raise RuntimeError("Дождись обновления статуса предыдущего задания")
        if self.resume_job:
            directory = self.resume_job
            self.resume_job = None
            self.running_job = self.baking_job = directory
            self.process = None
            self.auto_pending = self.auto_import.get_active() and bool(self.job_target)
            context = json.loads((directory / "app_context.json").read_text(encoding="utf-8"))
            self.active_import_mode = context.get("import_mode", "above")
            self.bake_cancel_requested = False
            self.status.set_text("Возобновление задания: ожидание обработанного клипа Resolve…")
            return
        if self.opened_job:
            raise RuntimeError("Открыто сохранённое задание. Для готового результата нажми «Import». "
                               "Для нового расчёта выдели клип таймлайна и заново открой окно из меню Resolve.")
        request = self.numeric_request()
        self.exposure_map_path.set_text("")
        fps_mode = ("timeline", "source", "custom")[self.source_fps_mode.get_selected()]
        if not Path(self.args.helper).is_file():
            raise RuntimeError("Сначала собери native helper: " + self.args.helper)
        require_flow_support(self.args.helper, request["blur_mode"], raw=self.ignore_processing.get_active(), exposure_mode=request["exposure_mode"])
        snapshot = self.source_snapshot
        if self.job_target and not snapshot:
            raise RuntimeError("Для нового расчёта заново выбери клип таймлайна. Готовое задание можно импортировать без пересчёта.")
        mode = ("replace", "above")[self.placement.get_selected()]
        target = snapshot["target"]
        ignore_processing = self.ignore_processing.get_active()
        transfer_override = (None, "srgb", "gamma24", "linear")[self.transfer.get_selected()]
        if not ignore_processing:
            expected_count = target["end"] - target["start"]
            if request["start_frame"] != 0 or request["frame_count"] != expected_count:
                raise RuntimeError("Для выбранного монтажного клипа используется ровно его подрезанный диапазон; загрузи клип заново")
        def launch(context):
            if getattr(self, "batch", None) and self.batch.stopped:
                return
            if snapshot and snapshot.get("project_id") != context.get("project_id"):
                raise RuntimeError("Проект изменился. Обнови источники из Resolve")
            timeline_fps = source_interpretation_fps("timeline", 0, context, snapshot)
            if abs(snapshot["output_fps"] - timeline_fps) >= 0.002:
                raise RuntimeError("FPS таймлайна изменился; загрузи выбранный клип заново")
            request["output_fps"] = timeline_fps
            self.context.update(context)
            self.timeline_fps_label.set_text(f"timeline_fps: {timeline_fps:g}")
            if ignore_processing:
                request.update(context["raw_source"])
            request["source_fps"] = source_interpretation_fps(fps_mode, request["source_fps"], context, snapshot)
            context["source_fps_mode"] = fps_mode
            context["capture_source_fps"] = request["source_fps"]
            root_text = self.preferences.validate({"output_folder": self.root.get_text()})["output_folder"]
            root = Path(root_text).expanduser().resolve()
            context["source_name"] = snapshot["name"]
            context["target"] = target
            from cadence_cache import create_job
            directory = create_job(root, context, context["source_name"])
            if self.batch_active():
                self.batch.current["job"] = str(directory)
                self.batch.save()
            request["output_name"] = cadence_name(context["source_name"])
            context["import_mode"] = mode
            if target:
                context["target"] = target
                context["source_processing"] = "native_raw_timeline" if ignore_processing else "resolve_frame_provider"
            (directory / "app_context.json").write_text(json.dumps(context, ensure_ascii=False), encoding="utf-8")
            self.running_job = directory
            self.job_target = target
            self.active_import_mode = context["import_mode"]
            self.job.set_text(str(directory))
            self.imported_job = None
            self.auto_pending = self.auto_import.get_active() and bool(self.job_target)
            self.bake_cancel_requested = False
            (root / "last_app_job.json").write_text(json.dumps({"directory": str(directory)}), encoding="utf-8")
            if target and not ignore_processing:
                (directory / "app_context.json").write_text(json.dumps(context, ensure_ascii=False), encoding="utf-8")
                (directory / "pending_native.json").write_text(json.dumps(request, ensure_ascii=False), encoding="utf-8")
                self.baking_job = directory
                self.process = None
                self.submit({"action": "direct_start", "job": str(directory), "target": target,
                             "transfer_override": transfer_override, "require_alpha": self.preserve_alpha.get_active(),
                             "ignore_processing": ignore_processing, "helper": self.args.helper},
                            self.receive_bake, self.bake_error)
                self.status.set_text("Подключение к источнику кадров Resolve…")
            else:
                self.launch_native(directory, request)
        validation = {"action": "validate_target", "target": target, "ignore_processing": ignore_processing}
        self.submit(validation, launch)
        self.status.set_text("Проверка проекта и FPS таймлайна перед запуском…")

    def launch_native(self, directory, request):
        """Start C++/CUDA only after the processed input is complete."""
        try:
            write_ini(directory / "request.ini", request)
            with (directory / "render.log").open("wb") as log:
                self.process = subprocess.Popen([self.args.helper, "--job", str(directory / "request.ini")],
                                                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                                env=child_environment(), start_new_session=True)
        except Exception:
            # Ошибка запуска не должна оставлять окно в состоянии вечного расчёта.
            self.running_job = self.baking_job = None
            self.auto_pending = False
            (directory / "cancel.request").write_text("cancel\n", encoding="utf-8")
            raise
        stage = "Каденс с кадрами Resolve по запросу" if request.get("source_mode") == "resolve_frames" else ("2/2: каденс" if self.baking_job else "Каденс")
        self.baking_job = None
        self.progress.set_fraction(0.0)
        self.status.set_text(stage + " — запущен нативный расчёт; прогресс обновляется автоматически.")

    def bake_error(self, error):
        """Keep render ownership recoverable after an RPC or project-switch error."""
        self.auto_pending = False
        self.stop_batch(str(error))
        self.next_bake_poll = time.monotonic() + 5
        state_file = self.baking_job / "bake_state.json" if self.baking_job else None
        state = json.loads(state_file.read_text(encoding="utf-8")) if state_file and state_file.exists() else {}
        if state.get("state") != "rendering":
            self.baking_job = self.running_job = None
        self.status.set_text("Ошибка подготовки Resolve: " + str(error))
        if not self.bake_cancel_requested and str(error) != self.last_bake_error:
            self.message(str(error), Gtk.MessageType.ERROR)
        self.last_bake_error = str(error)

    def receive_bake(self, state):
        """Advance the Resolve-to-native pipeline without blocking GTK's main loop."""
        phase = state.get("state")
        self.last_bake_error = None
        if phase == "rendering":
            self.progress.set_fraction(max(0.0, min(0.99, float(state.get("progress", 0)) / 100)))
            self.status.set_text("Подключение источника кадров Resolve…" if state.get("method") == "frame_provider" else f'1/2: Resolve — обработка выбранного клипа, {state.get("progress", 0)}%.')
            self.next_bake_poll = time.monotonic() + 1
            return
        directory = self.baking_job
        if phase == "complete" and not self.bake_cancel_requested and not (directory / "cancel.request").exists():
            request = json.loads((directory / "pending_native.json").read_text(encoding="utf-8"))
            request.update(input=state["input"], start_frame=0, frame_count=state["frame_count"],
                           input_transfer=state["input_transfer"], expected_source_fps=state["fps"])
            if state.get("source_mode") == "resolve_frames":
                request["source_mode"] = "resolve_frames"
            context_file = directory / "app_context.json"
            context = json.loads(context_file.read_text(encoding="utf-8"))
            context["baked_input"] = state["input"]
            context_file.write_text(json.dumps(context, ensure_ascii=False), encoding="utf-8")
            try:
                self.launch_native(directory, request)
            except Exception:
                self.resume_job = directory
                self.baking_job = self.running_job = None
                self.auto_pending = False
                raise
            if state.get("cleanup_warnings"):
                self.message("Промежуточный файл готов; служебные объекты нужно проверить: " + "; ".join(state["cleanup_warnings"]))
            return
        self.baking_job = self.running_job = None
        self.auto_pending = False
        self.status.set_text("Подготовка Resolve остановлена: " + state.get("error", phase or "неизвестный статус"))
        self.stop_batch(state.get("error", phase or "Подготовка остановлена"))

    def cancel(self):
        """Request cooperative cancellation without killing a Resolve process."""
        if self.batch_active():
            self.stop_batch("Отменено пользователем", "cancelled")
            if not self.running_job:
                self.status.set_text("Очередь остановлена; текущая операция импорта может завершиться.")
                return
        if self.baking_job:
            self.bake_cancel_requested = True
            self.auto_pending = False
            (self.baking_job / "cancel.request").write_text("cancel\n", encoding="utf-8")
            self.status.set_text("Запрошена отмена подготовки кадров Resolve…")
            return
        if self.future and not self.running_job:
            raise RuntimeError("Подготовка задания ещё не завершена")
        directory = self.running_job or Path(self.job.get_text())
        status = read_ini(directory / "status.ini")
        if status.get("state") not in ("starting", "decoding", "rendering", "finishing"):
            raise RuntimeError("Нет активного задания")
        (directory / "cancel.request").write_text("cancel\n", encoding="utf-8")
        self.auto_pending = False
        self.status.set_text("Запрошена отмена. Ожидание завершения записи…")

    def open_job(self):
        """Reopen persistent status after the app was closed."""
        if self.running_job or self.future or self.batch_active():
            raise RuntimeError("Сначала дождись текущего задания")
        def selected(path):
            self.batch = None
            context_file = Path(path) / "app_context.json"
            context = json.loads(context_file.read_text(encoding="utf-8")) if context_file.exists() else {}
            self.job_target = context.get("target")
            self.context_change(self.placement, "set_selected", 1 if context.get("import_mode", "above") == "above" else 0)
            self.job.set_text(path)
            self.opened_job = Path(path)
            result_file = self.opened_job / "result.ini"
            if result_file.exists():
                saved = read_ini(result_file)
                self.context_change(self.exposure_mode, "set_selected", 1 if saved.get("exposure_mode") == "ofx" else 0)
            self.auto_pending = False
            self.process = None
            self.source_snapshot = None
            self.resume_job = None
            self.baking_job = None
            self.timeline_label.set_text("Открыто сохранённое задание: " + context.get("source_name", Path(path).name))
            self.status.set_text("Задание открыто. Для готового результата нажми «Import»; для нового расчёта выбери источник заново.")
            bake_file = Path(path) / "bake_state.json"
            native_file = Path(path) / "status.ini"
            if bake_file.exists() and not native_file.exists() and not (Path(path) / "cancel.request").exists():
                state = json.loads(bake_file.read_text(encoding="utf-8"))
                if state.get("state") in ("rendering", "complete"):
                    self.resume_job = Path(path)
                    pending = json.loads((Path(path) / "pending_native.json").read_text(encoding="utf-8"))
                    for key, widget in self.values.items():
                        if key in pending:
                            self.context_change(widget, "set_text", str(pending[key]))
                    self.status.set_text("Задание открыто. Нажми «Calculate», чтобы продолжить подготовку/каденс.")
            elif native_file.exists() and read_ini(native_file).get("state") in ("starting", "decoding", "rendering", "finishing"):
                self.running_job = Path(path)
        self.choose_path("Cadence Job Folder", Gtk.FileChooserAction.SELECT_FOLDER, selected)

    def import_job(self, mode=None):
        """Import only a completed immutable native result."""
        directory = Path(self.job.get_text())
        result = read_ini(directory / "result.ini")
        if result.get("state") != "complete":
            raise RuntimeError("Задание ещё не завершено")
        if self.imported_job == str(directory) and result.get("exposure_mode") != "ofx":
            self.status.set_text("Это задание уже импортировано.")
            return
        if not self.job_target:
            raise RuntimeError("В задании нет выбранного клипа таймлайна. Результат сохранён в файл; новый таймлайн не создаётся.")
        if mode and mode not in ("replace", "above"):
            mode = None
        def imported(result):
            self.imported_job = str(directory)
            if result.get("exposure_map_path"):
                self.exposure_map_path.set_text(result["exposure_map_path"])
            self.status.set_text(result["message"])
            if self.batch_active() and self.batch.current["state"] == "importing":
                self.finish_batch_item("imported", result["message"])
            elif getattr(self, "batch", None) and self.batch.stopped and self.batch.current and self.batch.current.get("job") == str(directory):
                self.batch.current.update(state="imported", message=result["message"])
                self.batch.save()
        mode = mode or ("replace", "above")[self.placement.get_selected()]
        self.submit({"action": "import", "job": str(directory.resolve()), "mode": mode}, imported)
        self.status.set_text("Замена выбранного клипа…" if mode == "replace" else "Добавление над исходным клипом…")

    def show_directory(self):
        """Open the job folder in the desktop file manager."""
        directory = Path(self.job.get_text())
        if not self.job.get_text() or not directory.is_dir():
            raise RuntimeError("Сначала выбери задание")
        subprocess.Popen(["xdg-open", str(directory.resolve())], env=child_environment(),
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def update_job(self):
        """Observe atomically published progress and detect native launch failures."""
        self.update_source_fps_display()
        if not self.job.get_text():
            return
        directory = Path(self.job.get_text())
        if self.baking_job:
            if not self.future and time.monotonic() >= self.next_bake_poll:
                state_file = self.baking_job / "bake_state.json"
                state = json.loads(state_file.read_text(encoding="utf-8")) if state_file.exists() else {}
                action = "direct_poll" if state.get("method") in ("item_frames", "frame_provider") else ("bake_cancel" if self.bake_cancel_requested else "bake_poll")
                self.submit({"action": action, "job": str(self.baking_job)}, self.receive_bake, self.bake_error)
            return
        stopped = self.process and self.process.poll() is not None
        status_file = directory / "status.ini"
        status = read_ini(status_file) if status_file.exists() else {}
        if status.get("state") == "complete" and (directory / "result.ini").exists():
            result = read_ini(directory / "result.ini")
            map_path = result.get("exposure_map_path", "") if result.get("exposure_mode") == "ofx" else ""
            if self.exposure_map_path.get_text() != map_path:
                self.exposure_map_path.set_text(map_path)
            self.progress.set_fraction(1.0)
            if self.running_job:
                exposure = "Карта сохранена для Crank Exposure OFX." if result.get("exposure_mode") == "ofx" else "Экспозиция запечена в результат."
                self.status.set_text(f'Готово: {result["frame_count"]} кадров, {float(result["output_duration"]):.3f} с. {exposure}')
                self.running_job = None
            if self.batch_active():
                if not self.future and self.batch.current["state"] == "running":
                    if self.auto_pending:
                        self.auto_pending = False
                        self.batch.current["state"] = "importing"
                        self.batch.save()
                        self.import_job(mode=self.active_import_mode)
                    else:
                        self.finish_batch_item("rendered", "Автоимпорт выключен; результат сохранён в файл")
                return
            if self.auto_pending and not self.future:
                self.auto_pending = False
                self.import_job(mode=self.active_import_mode)
            return
        if self.running_job:
            duration = float(status.get("source_duration_hint", 0))
            position = float(status.get("source_position", 0))
            self.progress.set_fraction(max(0.0, min(0.99, position / duration)) if duration > 0 else 0.0)
            self.status.set_text(f'Каденс: {status.get("message", "Запуск…")}; записано кадров: {status.get("frames_written", "0")}')
            if status.get("state") in ("error", "cancelled") or stopped:
                self.running_job = None
                self.auto_pending = False
                self.stop_batch(status.get("message", "Расчёт остановлен"), status.get("state", "error"))
                self.status.set_text("Расчёт остановлен: " + status.get("message", "процесс завершился без результата") +
                                ". Подробности — render.log в папке задания.")
        elif status.get("state") in ("starting", "decoding", "rendering", "finishing"):
            duration = float(status.get("source_duration_hint", 0))
            position = float(status.get("source_position", 0))
            self.progress.set_fraction(max(0.0, min(0.99, position / duration)) if duration > 0 else 0.0)
            self.status.set_text(status.get("message", "Задание выполняется") + "; кадров: " + status.get("frames_written", "0"))
        elif status.get("state") in ("error", "cancelled"):
            self.status.set_text("Задание остановлено: " + status.get("message", status["state"]) + ". Подробности — render.log.")

    def poll(self):
        """Deliver background replies on the GTK thread and refresh progress."""
        if self.future and self.future[0].done():
            future, callback, error_callback = self.future
            self.future = None
            if future.exception() and error_callback:
                error_callback(future.exception())
            else:
                self.guarded(lambda: callback(future.result()))
        try:
            self.update_job()
        except Exception as error:
            self.auto_pending = False
            self.stop_batch(str(error))
            self.status.set_text("Ошибка чтения задания: " + str(error))
        busy = bool(self.future or self.running_job or self.batch_active())
        if self.batch_active():
            fraction = (self.batch.index + self.progress.get_fraction()) / len(self.batch.entries)
            self.batch_progress.set_fraction(fraction)
            self.batch_progress.set_text(f"{self.batch.index}/{len(self.batch.entries)} завершено")
        self.start_button.set_sensitive(not busy and (not self.opened_job or bool(self.resume_job)))
        has_target = self.job_target or (self.source_snapshot or {}).get("target")
        self.import_button.set_sensitive(not busy and bool(self.job_target))
        self.placement.set_sensitive(not busy and bool(has_target))
        self.preserve_alpha.set_sensitive(not busy and not self.resume_job and not self.ignore_processing.get_active())
        self.ignore_processing.set_sensitive(not busy and not self.opened_job)
        self.root.set_editable(not busy and not self.resume_job)
        self.root_browse.set_sensitive(not busy and not self.resume_job)
        self.clean_button.set_sensitive(not busy and not self.resume_job)
        self.clean_timeline_button.set_sensitive(not busy and not self.resume_job)
        for key, widget in self.values.items():
            widget.set_editable(not busy and not self.resume_job)
        self.backend.set_sensitive(not busy and not self.resume_job)
        self.exposure_mode.set_sensitive(not busy and not self.resume_job and not self.opened_job)
        self.source_fps_mode.set_sensitive(not busy and not self.resume_job)
        self.interpolation.set_sensitive(not busy and not self.resume_job)
        self.format.set_sensitive(not busy and not self.resume_job)
        self.auto_import.set_sensitive(not busy)
        self.transfer.set_sensitive(not busy and not self.resume_job)
        self.progress.set_text(f"{self.progress.get_fraction():.0%}")
        for widget in (self.preset_list, self.preset_name, self.save_preset_button, self.reset_button):
            widget.set_sensitive(not busy and not self.resume_job)
        self.update_source_fps_display()
        if not busy and not self.opened_job and not self.resume_job and not self.dialogs and time.monotonic() >= self.next_source_refresh:
            self.load_timeline_source(quiet=True)
        return GLib.SOURCE_CONTINUE

    def close(self, window):
        """Ask before leaving a render running; never block GTK's main loop."""
        if self.future:
            self.message("Дождись завершения операции Resolve перед закрытием окна.")
            return True
        if self.running_job:
            self.message("Текущий рендер продолжится после закрытия. Следующий этап и автоимпорт требуют открытого окна; открой задание позже и нажми «Calculate» для продолжения. Закрыть окно?",
                         confirm=self.finish_close)
            return True
        self.finish_close()
        return True

    def finish_close(self):
        """Release GUI resources while leaving detached native rendering alive."""
        if self.closed:
            return
        try:
            self.flush_preferences()
        except ValueError:
            pass  # Незавершённый/некорректный ввод не заменяет последние корректные настройки.
        except OSError as error:
            self.message(f"Не удалось сохранить настройки: {error}. Закрыть окно без сохранения последних изменений?",
                         confirm=self.destroy_window)
            return
        self.destroy_window()

    def destroy_window(self):
        """Close after settings have been saved or the user accepted a storage failure."""
        if self.closed:
            return
        self.closed = True
        self.stop_batch("Окно закрыто; текущий процесс может продолжаться отдельно", "detached")
        if self._preference_timer is not None:
            GLib.source_remove(self._preference_timer)
            self._preference_timer = None
        GLib.source_remove(self.poll_source)
        for dialog in list(self.dialogs):
            dialog.destroy()
        self.dialogs.clear()
        self.executor.shutdown(wait=False)
        self.window.destroy()


def main():
    """Start with explicit native/helper locations supplied by the generated launcher."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--backend", choices=("cuda", "cpu"), default="cuda")
    parser.add_argument("--resolve-python", default="/opt/resolve/bin/ResolvePython")
    args = parser.parse_args()
    application = Gtk.Application(application_id="com.alexz.silentfilm.cadence")
    def activate(application):
        if application.get_active_window():
            application.get_active_window().present()
            application.controller.load_timeline_source(quiet=True)
            return
        window = Gtk.ApplicationWindow(application=application)
        application.controller = Application(window, args)
        window.present()
        application.controller.load_timeline_source(quiet=True)
    application.connect("activate", activate)
    return application.run([])


if __name__ == "__main__":
    sys.exit(main())
