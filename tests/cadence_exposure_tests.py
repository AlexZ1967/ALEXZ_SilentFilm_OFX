"""Verify Fusion map binding and rollback without loading or connecting to Resolve."""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cadence_exposure as exposure


class Output:
    def __init__(self, owner):
        self.owner = owner

    def GetTool(self):
        return self.owner


class Input:
    def __init__(self, identifier, name):
        self.identifier, self.name, self.output = identifier, name, None

    def GetAttrs(self):
        return {"INPS_ID": self.identifier, "INPS_Name": self.name, "INPS_DataType": "Text"}

    def ConnectTo(self, output):
        self.output = output
        return True

    def GetConnectedOutput(self):
        return self.output


class Tool:
    def __init__(self, identifier, name, inputs=()):
        self.attrs = {"TOOLS_RegID": identifier, "TOOLS_Name": name, "TOOLB_PassThrough": False}
        self.inputs = [Input(*entry) for entry in inputs]
        self.values, self.ignored = {}, set()
        self.image_input, self.image_output = Input("image", "Image"), Output(self)

    def GetAttrs(self):
        return dict(self.attrs)

    def SetAttrs(self, attrs):
        self.attrs.update(attrs)

    def GetInputList(self):
        return dict(enumerate(self.inputs, 1))

    def SetInput(self, identifier, value):
        if identifier not in self.ignored:
            self.values[identifier] = value
        # Документированный SetInput может возвращать None; проверяется GetInput.

    def GetInput(self, identifier):
        return self.values.get(identifier)

    def FindMainInput(self, index):
        return self.image_input if index == 1 else None

    def FindMainOutput(self, index):
        return self.image_output if index == 1 else None


class Binding(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'Кино "A"' / "exposure.sfexposure"
        self.source = Tool("MediaIn", "MediaIn1")
        self.sink = Tool("MediaOut", "MediaOut1")
        self.tool = Tool("runtime.registry.id", "SilentFilmCrankExposure1",
                         [("exposure_map", "Exposure Map"), ("blend", "Blend"),
                          ("Blend", "Blend"), ("frame_offset", "Frame Offset")])
        self.comp = Mock()
        self.comp.CT_Tool = 1
        self.comp.GetToolList.return_value = {1: self.source, 2: self.sink}
        self.comp.AddTool.return_value = self.tool
        self.item = Mock()
        self.names = []
        self.item.GetFusionCompCount.side_effect = lambda: len(self.names)
        self.item.GetFusionCompNameList.side_effect = lambda: list(self.names)
        def add():
            self.names.append("Composition 1")
            return self.comp
        def remove(name):
            self.names.remove(name)
            return True
        self.item.AddFusionComp.side_effect = add
        self.item.DeleteFusionCompByName.side_effect = remove
        self.resolve = Mock()
        self.resolve.Fusion.return_value.GetRegList.return_value = {
            1: {"REGS_ID": "runtime.registry.id", "REGS_Name": "SilentFilm Crank Exposure"}}
        self.diagnostics = {}

    def bind(self):
        return exposure.configure_fusion_exposure(self.item, self.resolve, str(self.path), self.diagnostics)

    def test_assigns_exact_path_and_plugin_blend_then_connects_graph(self):
        result = self.bind()
        self.assertEqual(result["exposure_status"], "configured_fusion")
        self.assertTrue(result["exposure_effect_enabled"])
        self.assertEqual(self.tool.values, {"exposure_map": str(self.path), "blend": 1, "frame_offset": 0})
        self.assertNotIn("Blend", self.tool.values)
        self.assertIs(self.tool.image_input.output.owner, self.source)
        self.assertIs(self.sink.image_input.output.owner, self.tool)
        self.assertFalse(self.tool.attrs["TOOLB_PassThrough"])
        self.comp.Lock.assert_called_once()
        self.comp.Unlock.assert_called_once()
        self.item.DeleteFusionCompByName.assert_not_called()

    def test_real_input_ids_can_be_discovered_from_unambiguous_labels(self):
        self.tool.inputs = [Input("PathInput7", "Exposure Map"), Input("CrankMix", "Blend"), Input("OffsetInput", "Frame Offset")]
        self.bind()
        self.assertEqual(self.tool.values["PathInput7"], str(self.path))
        self.assertEqual(self.tool.values["CrankMix"], 1)

    def test_registry_objects_are_supported_without_guessing_id_prefixes(self):
        self.tool.attrs["TOOLS_RegID"] = "real.ofx.id"
        entry = Mock()
        entry.GetAttrs.return_value = {"REGS_ID": "real.ofx.id", "REGS_Name": "SilentFilm Crank Exposure"}
        self.resolve.Fusion.return_value.GetRegList.return_value = {1: entry}
        self.bind()
        self.comp.AddTool.assert_called_once_with("real.ofx.id", False, 1, 0)

    def test_existing_composition_is_not_modified(self):
        self.names.append("User grade")
        with self.assertRaises(exposure.FusionExposureError) as caught:
            self.bind()
        self.assertFalse(caught.exception.requires_recovery)
        self.item.AddFusionComp.assert_not_called()
        self.item.DeleteFusionCompByName.assert_not_called()

    def test_unconfirmed_path_assignment_rolls_back_owned_composition(self):
        self.tool.ignored.add("exposure_map")
        with self.assertRaises(exposure.FusionExposureError) as caught:
            self.bind()
        self.assertFalse(caught.exception.requires_recovery)
        self.assertEqual(self.names, [])
        self.comp.Unlock.assert_called_once()

    def test_ambiguous_parameter_names_are_rejected(self):
        self.tool.inputs = [Input("map1", "Exposure Map"), Input("map2", "Exposure Map")]
        with self.assertRaises(exposure.FusionExposureError):
            self.bind()
        self.assertFalse(self.tool.values)
        self.assertEqual(self.names, [])

    def test_missing_registry_tool_rolls_back(self):
        self.resolve.Fusion.return_value.GetRegList.return_value = []
        with self.assertRaises(exposure.FusionExposureError):
            self.bind()
        self.assertEqual(self.names, [])
        self.comp.Unlock.assert_called_once()

    def test_failed_cleanup_requires_recovery_instead_of_another_effect(self):
        self.tool.ignored.add("exposure_map")
        self.item.DeleteFusionCompByName.side_effect = None
        self.item.DeleteFusionCompByName.return_value = False
        with self.assertRaises(exposure.FusionExposureError) as caught:
            self.bind()
        self.assertTrue(caught.exception.requires_recovery)
        self.assertIn("rollback_error", self.diagnostics)


if __name__ == "__main__":
    unittest.main()
