"""Verify persistent effect settings and preset isolation without GTK or Resolve."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("cadence_preferences", Path(__file__).resolve().parents[1] / "scripts/cadence_preferences.py")
preferences = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preferences)


class PresetStorage(unittest.TestCase):
    """Keep last settings durable while presets never replace source metadata."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "settings.json"

    def store(self):
        return preferences.PreferenceStore(
            {"fps_min": (16, 1, 60, False), "fps_max": (20, 1, 60, False),
             "variation": (0.7, 0, 1, False), "source_fps": (25, 1, 120, False),
             "samples": (9, 1, 17, True), "seed": (1925, 0, 2147483647, True)},
            {"source_fps_mode": ("timeline", ("timeline", "source", "custom")),
             "exposure_mode": ("bake", ("bake", "ofx")),
             "exposure_location": ("color", ("color", "fusion")),
             "blur_mode": ("flow_integrated", ("flow_integrated", "simple"))},
            {"auto_import": True, "use_raw_source": True}, self.path,
            texts={"output_folder": "/tmp/SilentFilm"})

    def test_exposure_mode_survives_preset_and_factory_reset(self):
        store = self.store()
        self.assertEqual(store.current["exposure_mode"], "bake")
        store.save_preset("Via OFX", dict(store.current, exposure_mode="ofx"))
        self.assertEqual(self.store().current["exposure_mode"], "ofx")
        store.reset()
        self.assertEqual(store.current["exposure_mode"], "bake")
        self.assertEqual(store.preset("Via OFX")["exposure_mode"], "ofx")

    def test_exposure_location_survives_preset_and_factory_reset(self):
        store = self.store()
        self.assertEqual(store.current["exposure_location"], "color")
        store.save_preset("Fusion", dict(store.current, exposure_location="fusion"))
        self.assertEqual(self.store().current["exposure_location"], "fusion")
        store.reset()
        self.assertEqual(store.current["exposure_location"], "color")
        self.assertEqual(store.preset("Fusion")["exposure_location"], "fusion")

    def test_output_folder_survives_restart_and_preset_selection(self):
        store = self.store()
        name = store.save_preset("External SSD", dict(store.current, output_folder="/media/SSD/Кино"))
        self.assertEqual(self.store().current["output_folder"], "/media/SSD/Кино")
        store.reset()
        self.assertEqual(store.preset(name)["output_folder"], "/media/SSD/Кино")
        for path in ("", "\n/tmp/test", "/tmp/test\0other"):
            with self.assertRaises(ValueError):
                store.set_current(dict(store.current, output_folder=path))

    def test_raw_source_defaults_on_and_preset_can_restore_either_choice(self):
        store = self.store()
        self.assertTrue(store.current["use_raw_source"])
        store.save_preset("Processed", dict(store.current, use_raw_source=False))
        again = self.store()
        self.assertFalse(again.current["use_raw_source"])
        self.assertFalse(again.preset("Processed")["use_raw_source"])
        again.reset()
        self.assertTrue(self.store().current["use_raw_source"])

    def test_last_effect_values_and_custom_fps_survive_reopening(self):
        store = self.store()
        store.set_current(dict(store.current, fps_min=10, fps_max=30, variation=1,
                               source_fps_mode="custom", source_fps=36, samples=17))
        again = self.store()
        self.assertEqual(again.current, store.current)
        self.assertEqual(again.current["source_fps"], 36)
        self.assertEqual(again.current["fps_min"], 10)

    def test_clip_geometry_range_paths_and_playback_fps_are_never_saved(self):
        store = self.store()
        name = store.save_preset("Film", dict(store.current, output_fps=50, encoded_source_fps=24,
                                              width=1920, height=1080, start_frame=140, frame_count=100,
                                              input="/private/source.mov", source_fps_mode="source"))
        for forbidden in ("output_fps", "encoded_source_fps", "width", "height", "start_frame", "frame_count", "input"):
            self.assertNotIn(forbidden, store.current)
            self.assertNotIn(forbidden, store.preset(name))
        self.assertEqual(store.preset(name)["source_fps_mode"], "source")

    def test_factory_reset_keeps_named_presets(self):
        store = self.store()
        name = store.save_preset("Камера 1920", dict(store.current, fps_min=12, source_fps_mode="custom", source_fps=48))
        store.reset()
        again = self.store()
        self.assertEqual(again.current, again.factory)
        self.assertEqual(again.preset(name)["source_fps"], 48)
        self.assertEqual(again.preset(name)["fps_min"], 12)
        self.assertIsNone(again.selected)

    def test_unicode_names_are_data_and_overwriting_requires_explicit_replace(self):
        store = self.store()
        name = store.save_preset("Тёплый / 1920", store.current)
        with self.assertRaisesRegex(ValueError, "существует"):
            store.save_preset("ТЁПЛЫЙ / 1920", dict(store.current, samples=5))
        self.assertEqual(store.preset(name)["samples"], 9)
        store.save_preset("ТЁПЛЫЙ / 1920", dict(store.current, samples=5), replace=True)
        self.assertEqual(self.store().names(), [name])
        self.assertEqual(self.store().preset(name)["samples"], 5)
        self.assertEqual([file.name for file in self.path.parent.iterdir()], ["settings.json"])

    def test_invalid_edit_keeps_the_previous_file(self):
        store = self.store()
        store.set_current(store.current)
        original = self.path.read_bytes()
        for invalid in ({"samples": 18}, {"fps_min": 30, "fps_max": 20},
                        {"source_fps": float("nan")}, {"auto_import": 1}):
            with self.assertRaises(ValueError):
                store.set_current(dict(store.current, **invalid))
            self.assertEqual(self.path.read_bytes(), original)

    def test_failed_atomic_replacement_keeps_the_last_good_state(self):
        store = self.store()
        store.set_current(store.current)
        original = self.path.read_bytes()
        with patch.object(preferences.os, "replace", side_effect=OSError("disk unavailable")):
            with self.assertRaises(OSError):
                store.set_current(dict(store.current, samples=5))
        self.assertEqual(self.path.read_bytes(), original)
        self.assertEqual(store.current["samples"], 9)
        self.assertEqual(list(self.path.parent.glob("settings.json.*")), [])

    def test_corrupted_file_is_preserved_before_the_next_save(self):
        self.path.write_text("{broken", encoding="utf-8")
        store = self.store()
        self.assertIsNotNone(store.warning)
        store.set_current(store.factory)
        copies = list(self.path.parent.glob("settings.json.invalid-*"))
        self.assertEqual(len(copies), 1)
        self.assertEqual(copies[0].read_text(encoding="utf-8"), "{broken")
        self.assertEqual(json.loads(self.path.read_text(encoding="utf-8"))["current"], store.factory)


if __name__ == "__main__":
    unittest.main()
