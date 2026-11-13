"""Exercise the exported DRX schema, unique file bindings and fresh-result Color application without Resolve."""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cadence_color_preset as color


class ColorPreset(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.template = color.template_path()
        self.document = self.template.read_text(encoding="utf-8")
        self.original = bytes.fromhex(color.body_text(self.document))
        self.map = self.root / 'Кино "A"' / "exposure.sfexposure"
        self.map.parent.mkdir()
        self.map.write_text("SILENTFILM_CRANK_EXPOSURE 1\nofx 25 3\n0 1\n1 0.5\n2 2\n", encoding="utf-8")

    def test_real_template_has_one_map_and_two_nodes(self):
        metadata = color.patch_body(self.original)
        self.assertEqual(metadata["node_count"], 2)
        self.assertTrue(metadata["map"].endswith("exposure.sfexposure"))

    def test_unicode_path_preserves_all_non_map_data(self):
        output = self.root / "result.drx"
        result = color.generate_preset(self.template, self.map, output)
        document = output.read_text(encoding="utf-8")
        body = bytes.fromhex(color.body_text(document))
        self.assertEqual(color.patch_body(body)["map"], str(self.map))
        old_map = color.patch_body(self.original)["map"]
        restored = color.patch_body(body, old_map)
        self.assertEqual(color.codec().decode(restored[1:]), color.codec().decode(self.original[1:]))
        self.assertEqual(document.replace(color.body_text(document), "BODY"),
                         self.document.replace(color.body_text(self.document), "BODY"))
        self.assertEqual(result["preset"], str(output))
        self.assertFalse(output.with_suffix(".drx.tmp").exists())

    def test_each_job_retains_its_own_unique_map(self):
        second = self.root / "second.sfexposure"
        second.write_bytes(self.map.read_bytes())
        first_output, second_output = self.root / "first.drx", self.root / "second.drx"
        color.generate_preset(self.template, self.map, first_output)
        color.generate_preset(self.template, second, second_output)
        for output, expected in ((first_output, self.map), (second_output, second)):
            body = bytes.fromhex(color.body_text(output.read_text()))
            self.assertEqual(color.patch_body(body)["map"], str(expected))

    def test_wrong_plugin_and_corrupt_payload_are_rejected(self):
        decoded = color.codec().decode(self.original[1:])
        other = decoded.replace(color.PLUGIN_ID, b"x" * len(color.PLUGIN_ID))
        with self.assertRaises(ValueError):
            color.patch_body(b"\x81" + color.codec().encode(other))
        for blob in (b"", b"bad", self.original[:-12]):
            with self.assertRaises(ValueError):
                color.patch_body(blob)
        with self.assertRaises(ValueError):
            color.unpack(b"\x0a\x20\x00")

    def fixture(self):
        item, old, applied = Mock(), Mock(), Mock()
        item.GetStart.return_value = 100
        item.GetDuration.return_value = 3
        item.GetFusionCompCount.return_value = 0
        item.GetNodeGraph.side_effect = [old, applied]
        old.ApplyGradeFromDRX.return_value = True
        applied.GetNumNodes.return_value = 2
        applied.GetNodeLabel.side_effect = lambda index: "SilentFilm Crank Exposure" if index == 2 else ""
        applied.GetToolsInNode.side_effect = lambda index: ["OpenFX"] if index == 2 else ["Corrector"]
        current = {"versionName": "Original", "versionType": 1}
        names = []
        item.GetCurrentVersion.side_effect = lambda: dict(current)
        item.GetVersionNameList.side_effect = lambda kind: list(names)
        def add(name, kind):
            self.assertEqual(kind, 0)
            names.append(name)
            return True
        def load(name, kind):
            current.update(versionName=name, versionType=kind)
            return True
        def delete(name, kind):
            names.remove(name)
            return True
        item.AddVersion.side_effect = add
        item.LoadVersionByName.side_effect = load
        item.DeleteVersionByName.side_effect = delete
        return item, old, applied

    def test_apply_uses_local_version_and_reacquires_invalidated_graph(self):
        item, old, applied = self.fixture()
        diagnostics = {}
        outcome = color.configure_color_exposure(item, self.map, diagnostics, fresh=True)
        self.assertEqual(outcome["exposure_status"], "configured_color")
        self.assertEqual(item.GetNodeGraph.call_count, 2)
        old.ApplyGradeFromDRX.assert_called_once_with(str(self.map.parent / "crank_exposure.drx"), 0)
        old.GetNumNodes.assert_not_called()
        self.assertEqual(item.GetCurrentVersion()["versionType"], 0)
        self.assertEqual(item.GetCurrentVersion()["versionName"], outcome["exposure_color_version"])
        item.DeleteVersionByName.assert_not_called()

    def test_existing_result_is_never_overwritten(self):
        item, old, applied = self.fixture()
        with self.assertRaisesRegex(ValueError, "новому результату"):
            color.configure_color_exposure(item, self.map, {})
        item.AddVersion.assert_not_called()
        item.GetNodeGraph.assert_not_called()

    def test_preset_uses_exact_variant_clip_name_beside_map(self):
        item, old, applied = self.fixture()
        name = "Damy_na_skachkah_2_cadence_a84y7i6z"
        outcome = color.configure_color_exposure(item, self.map, {}, fresh=True, clip_name=name)
        preset = self.map.parent / (name + ".drx")
        old.ApplyGradeFromDRX.assert_called_once_with(str(preset), 0)
        self.assertTrue(preset.is_file())
        self.assertEqual(outcome["exposure_color_preset"], str(preset))
        self.assertFalse((self.map.parent / "crank_exposure.drx").exists())

    def test_preset_name_cannot_escape_clip_directory(self):
        item, old, applied = self.fixture()
        with self.assertRaisesRegex(ValueError, "имя Color"):
            color.configure_color_exposure(item, self.map, {}, fresh=True, clip_name="../outside")
        old.ApplyGradeFromDRX.assert_not_called()

    def test_rejected_or_unconfirmed_grade_restores_previous_version(self):
        for failure in ("apply", "nodes"):
            item, old, applied = self.fixture()
            if failure == "apply":
                old.ApplyGradeFromDRX.return_value = False
            else:
                applied.GetNumNodes.return_value = 4
            diagnostics = {}
            with self.assertRaises(ValueError):
                color.configure_color_exposure(item, self.map, diagnostics, fresh=True)
            self.assertEqual(item.GetCurrentVersion(), {"versionName": "Original", "versionType": 1})
            self.assertEqual(item.GetVersionNameList(0), [])
            self.assertTrue(diagnostics["apply_attempted"])
            self.assertNotIn("rollback_error", diagnostics)


if __name__ == "__main__":
    unittest.main()
