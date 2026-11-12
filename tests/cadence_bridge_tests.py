"""Test import isolation and rollback without loading or connecting to Resolve."""

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("cadence_bridge", Path(__file__).resolve().parents[1] / "scripts/cadence_resolve_bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class LocalModuleDiscovery(unittest.TestCase):
    """Resolve sibling modules even when the host interpreter omits the script directory."""

    def test_bridge_bootstraps_its_own_directory_for_cache_import(self):
        directory = Path(__file__).resolve().parents[1] / "scripts"
        isolated = [entry for entry in sys.path if Path(entry or ".").resolve() != directory]
        with patch.object(sys, "path", isolated), patch.dict(sys.modules):
            sys.modules.pop("cadence_cache", None)
            local_spec = importlib.util.spec_from_file_location("isolated_cadence_bridge", directory / "cadence_resolve_bridge.py")
            local_module = importlib.util.module_from_spec(local_spec)
            local_spec.loader.exec_module(local_module)
            cache = importlib.import_module("cadence_cache")
            self.assertEqual(Path(cache.__file__).resolve(), directory / "cadence_cache.py")


class ImportIsolation(unittest.TestCase):
    """Protect original edits and avoid duplicate imports when reopening completed jobs."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.context = {"project_id": "project-A", "project_name": "Original", "source_name": "source"}
        (self.directory / "app_context.json").write_text(json.dumps(self.context), encoding="utf-8")
        (self.directory / "cadence.mov").touch()
        (self.directory / "result.ini").write_text(
            "schema=1\nstate=complete\nframe_count=115\noutput_fps=25\nformat=prores\nprocessing=capture_rgb\n"
            f"media_path={self.directory / 'cadence.mov'}\n", encoding="utf-8")
        self.clip = Mock()
        self.clip.GetClipProperty.return_value = "25"
        self.timeline = Mock()
        self.timeline.GetSettings.return_value = {"timelineFrameRate": "25"}
        self.timeline.GetUniqueId.return_value = "generated-timeline"
        self.timeline.GetName.return_value = "Generated"
        self.item = Mock()
        self.item.GetDuration.return_value = 115
        self.pool = Mock()
        self.pool.ImportMedia.return_value = [self.clip]
        self.pool.CreateEmptyTimeline.return_value = self.timeline
        self.pool.AppendToTimeline.return_value = [self.item]
        self.project = Mock()
        self.project.GetMediaPool.return_value = self.pool
        self.project.GetTimelineCount.return_value = 1
        self.project.GetTimelineByIndex.return_value = self.timeline
        self.old_timeline = self.project.GetCurrentTimeline.return_value
        self.context_patch = patch.object(bridge, "project_context", return_value=self.context)
        self.context_patch.start()
        self.addCleanup(self.context_patch.stop)

    def run_import(self):
        """Import the immutable fixture through a host double."""
        return bridge.import_result(self.project, {"job": str(self.directory)})

    def test_project_switch_blocks_import(self):
        """A background job cannot import into a different open project."""
        self.context_patch.stop()
        with patch.object(bridge, "project_context", return_value={"project_id": "project-B"}):
            with self.assertRaisesRegex(RuntimeError, "исходный проект"):
                self.run_import()
        self.pool.ImportMedia.assert_not_called()

    def test_duration_failure_rolls_back_new_timeline(self):
        """A host FPS mismatch preserves the old edit and the rendered source file."""
        self.item.GetDuration.return_value = 230
        with self.assertRaisesRegex(RuntimeError, "Монтажная длина"):
            self.run_import()
        self.project.SetCurrentTimeline.assert_called_with(self.old_timeline)
        self.pool.DeleteTimelines.assert_called_once_with([self.timeline])
        self.assertTrue((self.directory / "cadence.mov").exists())
        self.assertFalse((self.directory / "resolve_import.json").exists())

    def test_reopened_job_does_not_duplicate_timeline(self):
        """A persistent marker makes a repeated import harmless."""
        self.run_import()
        again = self.run_import()
        self.assertIn("Уже импортировано", again["message"])
        self.pool.ImportMedia.assert_called_once()
        self.pool.CreateEmptyTimeline.assert_called_once()

    def test_movie_import_uses_simple_path(self):
        """Use the path form already exercised by the legacy MOV import."""
        self.run_import()
        self.pool.ImportMedia.assert_called_once_with([str(self.directory / "cadence.mov")])

    def test_movie_import_falls_back_to_structured_form(self):
        """Hosts without the legacy overload can use the documented clipInfo form."""
        self.pool.ImportMedia.side_effect = [[], [self.clip]]
        self.pool.GetCurrentFolder.return_value.GetClipList.return_value = []
        self.run_import()
        self.assertEqual(self.pool.ImportMedia.call_count, 2)
        self.pool.ImportMedia.assert_called_with([{"FilePath": str(self.directory / "cadence.mov")}])

    def test_movie_added_without_return_handle_is_reused(self):
        """Do not import again when Resolve added the exact file but returned no handle."""
        self.pool.ImportMedia.return_value = []
        self.pool.GetCurrentFolder.return_value.GetClipList.return_value = [self.clip]
        self.clip.GetClipProperty.side_effect = lambda key: str(self.directory / "cadence.mov") if key == "File Path" else "25"
        self.run_import()
        self.pool.ImportMedia.assert_called_once()

    def test_movie_failure_reports_actual_format_without_tiff_advice(self):
        """An import failure must preserve the render and identify the actual media."""
        self.pool.ImportMedia.return_value = []
        self.pool.GetCurrentFolder.return_value.GetClipList.return_value = []
        with self.assertRaises(RuntimeError) as error:
            self.run_import()
        self.assertIn("prores", str(error.exception))
        self.assertNotIn("Show Individual Frames", str(error.exception))
        self.pool.AppendToTimeline.assert_not_called()
        self.assertTrue((self.directory / "import_diagnostics.json").exists())

    def test_cadence_names_keep_labels_and_safe_suffix(self):
        """Rendered input names must not replace the montage clip's own label."""
        self.assertEqual(bridge.cadence_name("Scene_01.mp4"), "Scene_01_cadence")
        self.assertEqual(bridge.cadence_name("Сцена.01"), "Сцена.01_cadence")
        self.assertEqual(bridge.cadence_name("Сцена 1.mov"), "Сцена 1_cadence")
        self.assertEqual(bridge.cadence_name("Scene_01_cadence.mov"), "Scene_01_cadence")
        name = bridge.cadence_name("../" + "ы" * 300 + "%?.mp4")
        self.assertTrue(name.endswith("_cadence"))
        self.assertLessEqual(len(name.encode("utf-8")), 198)
        self.assertFalse(any(character in name for character in "/\\%?"))


class TimelineSelection(unittest.TestCase):
    """Verify trimmed timeline selection and replacement without ripple or a live host."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.media_path = self.directory / "source.mov"
        self.media_path.touch()
        self.media = Mock()
        self.media.GetUniqueId.return_value = "same-file"
        self.media.GetClipProperty.side_effect = lambda name: str(self.media_path) if name == "File Path" else "25"
        self.original = self.make_item("selected", 100, 200, 40, 139)
        self.following = self.make_item("following", 200, 300, 140, 239)
        self.video = [self.original, self.following]
        self.audio = self.make_item("audio", 100, 200, 40, 139)
        self.audio.GetType.return_value = "audio"
        self.original.GetLinkedItems.return_value = [self.audio]
        self.timeline = Mock()
        self.timeline.GetUniqueId.return_value = "edit-timeline"
        self.timeline.GetName.return_value = "Edit"
        self.timeline.GetSettings.return_value = {"timelineFrameRate": "25"}
        self.timeline.GetSelectedClips.return_value = [self.original, self.audio]
        self.timeline.GetTrackCount.side_effect = lambda kind: 1 if kind in ("video", "audio") else 0
        self.timeline.GetItemListInTrack.side_effect = lambda kind, track: self.video if kind == "video" else [self.audio]
        self.timeline.GetIsTrackLocked.return_value = False
        self.backup = Mock()
        self.backup.GetUniqueId.return_value = "backup-id"
        self.backup.GetName.return_value = "Backup"
        self.timeline.DuplicateTimeline.return_value = self.backup
        self.project = Mock()
        self.project.GetCurrentTimeline.return_value = self.timeline
        self.project.GetUniqueId.return_value = "project-A"
        self.project.GetName.return_value = "Project"
        self.pool = Mock()
        self.context = bridge.timeline_source(self.project)
        self.pool.DeleteTimelines.return_value = True
        self.target = self.context["target"]
        def delete(items, ripple):
            self.assertFalse(ripple)
            for item in items:
                self.video.remove(item)
            return True
        self.timeline.DeleteClips.side_effect = delete
        def append(infos):
            info = infos[0]
            self.assertEqual(info["mediaType"], 1)
            self.assertEqual(info["trackIndex"], 1)
            self.replacement = self.make_item("prepared", info["recordFrame"], info["recordFrame"] + 80, 0, 79)
            self.video.append(self.replacement)
            return [self.replacement]
        self.pool.AppendToTimeline.side_effect = append

    def make_item(self, identity, start, end, source_start, source_end):
        """Create a clip with independently tracked source and timeline coordinates."""
        item = Mock()
        item.GetUniqueId.return_value = identity
        item.GetName.return_value = identity
        item.GetMediaPoolItem.return_value = self.media
        item.GetType.return_value = "video"
        item.GetTrackTypeAndIndex.return_value = ["video", 1]
        item.GetSpeed.return_value = {"Percentage": 100}
        item.GetStart.return_value = start
        item.GetEnd.return_value = end
        item.GetDuration.return_value = end - start
        item.GetSourceStartFrame.return_value = source_start
        item.GetSourceEndFrame.return_value = source_end
        item.GetLinkedItems.return_value = []
        return item

    def test_batch_selection_keeps_distinct_edits_and_ignores_linked_audio(self):
        self.timeline.GetSelectedClips.return_value = [self.following, self.audio, self.original, self.original]
        items = bridge.timeline_sources(self.project)["items"]
        self.assertEqual([item["target"]["item_id"] for item in items], ["selected", "following"])
        self.assertEqual([item["target"]["source_start"] for item in items], [40, 140])
        self.assertEqual([item["source"]["marks"] for item in items], [{"in": 0, "out": 99}] * 2)
        self.assertEqual(items[0]["source"]["clip_id"], items[1]["source"]["clip_id"])
        with self.assertRaisesRegex(RuntimeError, "ровно один"):
            bridge.timeline_source(self.project)

    def test_batch_selection_orders_tracks_at_the_same_start(self):
        upper = self.make_item("upper", 100, 150, 5, 55)
        upper.GetTrackTypeAndIndex.return_value = ["video", 2]
        self.timeline.GetSelectedClips.return_value = [upper, self.original]
        items = bridge.timeline_sources(self.project)["items"]
        self.assertEqual([item["target"]["item_id"] for item in items], ["selected", "upper"])

    def test_batch_selection_rejects_audio_only(self):
        self.timeline.GetSelectedClips.return_value = [self.audio]
        with self.assertRaisesRegex(RuntimeError, "видеоклип"):
            bridge.timeline_sources(self.project)

    def test_raw_range_uses_source_frames_not_timeline_duration(self):
        """Raw 24-fps input must not inherit a longer 25-fps timeline frame count."""
        target = dict(self.target, source_start=12, source_end=252, fps=24, start=100, end=351)
        source = bridge.raw_source_range(target)
        self.assertEqual(source["start_frame"], 12)
        self.assertEqual(source["frame_count"], 240)
        self.assertEqual(source["expected_source_fps"], 24)

    def test_raw_range_rejects_empty_interval(self):
        """An invalid source range must not silently render the complete file."""
        with self.assertRaisesRegex(RuntimeError, "In/Out"):
            bridge.raw_source_range(dict(self.target, source_start=40, source_end=40))

    def test_png_memory_transport_does_not_write_files(self):
        """Encoded export must pass the same PNG bytes directly to the encoder."""
        import base64
        payload = b"\x89PNG\r\n\x1a\n" + b"frame"
        exporter = Mock()
        exporter.ExportFrame.return_value = {"success": True, "data": base64.b64encode(payload).decode("ascii")}
        path = self.directory / "frame.png"
        data, transport = bridge.export_png_bytes(exporter, 12, path)
        self.assertEqual((data, transport), (payload, "encoded"))
        self.assertFalse(path.exists())
        exporter.ExportFrame.assert_called_once_with(12, {"outputType": "encodedString", "encodedImageFormat": "png", "maxDimension": 0})

    def test_png_file_fallback_is_explicit_and_reused(self):
        """Unsupported encoded export is probed once, then file transport is retained."""
        payload = b"\x89PNG\r\n\x1a\n" + b"frame"
        exporter = Mock()
        def export(index, options):
            if options["outputType"] == "encodedString":
                return {"success": False}
            Path(options["filePath"]).write_bytes(payload)
            return {"success": True}
        exporter.ExportFrame.side_effect = export
        path = self.directory / "frame.png"
        data, transport = bridge.export_png_bytes(exporter, 12, path)
        self.assertEqual((data, transport), (payload, "file"))
        bridge.export_png_bytes(exporter, 13, path, transport)
        self.assertEqual(exporter.ExportFrame.call_count, 3)
        self.assertFalse(path.exists())

    def test_raw_frames_preserve_timeline_count_and_trim(self):
        """97 source frames must fill the same 101 timeline frames as the processed path."""
        target = dict(self.target, source_start=140, source_end=237, fps=24, start=146, end=247)
        frames = [bridge.raw_timeline_frame(target, offset) for offset in range(101)]
        self.assertEqual(len(frames), 101)
        self.assertEqual((frames[0], frames[-1]), (140, 236))
        self.assertEqual(set(frames), set(range(140, 237)))
        self.assertEqual(frames, sorted(frames))

    def test_native_raw_request_keeps_both_clocks_separate(self):
        """Native decode validates encoded FPS while cadence receives the unchanged timeline grid."""
        target = dict(self.target, source_start=140, source_end=237, fps=24, start=146, end=247)
        request = bridge.native_raw_request(target, 25)
        self.assertEqual((request["start_frame"], request["frame_count"]), (0,101))
        self.assertEqual((request["raw_start_frame"], request["raw_frame_count"]), (140,97))
        self.assertEqual(request["timeline_input_fps"],25)
        self.assertEqual(request["expected_source_fps"],24)

    def test_encoder_queue_preserves_order_and_flushes_before_finish(self):
        """The producer can overlap encoding without reordering or dropping PNG bytes."""
        class Pipe:
            def __init__(self): self.data=bytearray(); self.closed=False
            def write(self, value):
                self.data.extend(value[:2])
                return min(2,len(value))
            def close(self): self.closed=True
        pipe=Pipe()
        writer=bridge.PngPipeWriter(pipe, lambda:False)
        try:
            for frame in (b"first",b"second",b"third"):
                writer.put(frame)
            writer.finish()
            self.assertEqual(pipe.data,b"firstsecondthird")
            self.assertTrue(pipe.closed)
            self.assertEqual(writer.pending.maxsize,2)
        finally:
            writer.abort()

    def test_encoder_queue_propagates_write_failure(self):
        """A stopped encoder must not leave a full producer queue waiting forever."""
        pipe=Mock()
        pipe.write.side_effect=BrokenPipeError("stopped")
        writer=bridge.PngPipeWriter(pipe, lambda:False)
        try:
            with self.assertRaisesRegex(RuntimeError,"FFmpeg"):
                writer.put(b"frame")
                writer.finish()
        finally:
            writer.abort()

    def test_selection_uses_trim_of_instance_not_whole_file(self):
        """Two edits must retain distinct source/record ranges and bake from frame zero."""
        first = self.context["source"]
        self.assertEqual(first["marks"], {"in": 0, "out": 99})
        self.assertTrue(first["requires_bake"])
        self.assertEqual(self.target["source_start"], 40)
        self.assertEqual(self.target["start"], 100)
        self.timeline.GetSelectedClips.return_value = [self.following]
        context = bridge.timeline_source(self.project)
        second = context["source"]
        self.assertEqual(second["path"], first["path"])
        self.assertEqual(second["marks"], {"in": 0, "out": 99})
        self.assertEqual(context["target"]["source_start"], 140)
        self.assertEqual(context["target"]["start"], 200)
        self.project.StartRendering.assert_not_called()

    def test_shorter_replacement_preserves_following_and_audio(self):
        """A shorter capture leaves a gap without moving audio or the next clip."""
        result = bridge.replace_result(self.project, self.pool, Mock(), self.target, 80, 25, self.directory)
        self.assertEqual(result["mode"], "replace")
        self.assertEqual(self.replacement.GetStart(), 100)
        self.assertEqual(self.replacement.GetEnd(), 180)
        self.assertEqual(self.following.GetStart(), 200)
        self.assertEqual(self.audio.GetStart(), 100)
        self.timeline.SetClipsLinked.assert_called_with([self.replacement, self.audio], True)
        self.assertTrue((self.directory / "replace_recovery.json").exists())
        self.pool.DeleteTimelines.assert_called_once_with([self.backup])
        self.assertFalse(result["backup_retained"])
        self.assertEqual(json.loads((self.directory / "replace_recovery.json").read_text())["backup_state"], "deleted")

    def test_backup_cleanup_failure_does_not_repeat_successful_edit(self):
        self.pool.DeleteTimelines.return_value = False
        result = bridge.replace_result(self.project, self.pool, Mock(), self.target, 80, 25, self.directory)
        self.assertEqual(result["mode"], "replace")
        self.assertTrue(result["backup_retained"])
        self.assertIn("удерживать кэш", result["message"])
        self.pool.AppendToTimeline.assert_called_once()
        self.assertEqual(json.loads((self.directory / "replace_recovery.json").read_text())["backup_state"], "retained")

    def test_longer_replacement_cannot_overwrite_next_clip(self):
        """Insufficient space must stop before backup, unlink or deletion."""
        with self.assertRaisesRegex(RuntimeError, "свободного места"):
            bridge.replace_result(self.project, self.pool, Mock(), self.target, 101, 25, self.directory)
        self.timeline.DuplicateTimeline.assert_not_called()
        self.timeline.DeleteClips.assert_not_called()
        self.pool.AppendToTimeline.assert_not_called()

    def test_changed_trim_blocks_replacement(self):
        """Editing a source trim while the renderer runs invalidates the saved target."""
        self.original.GetSourceStartFrame.return_value = 41
        with self.assertRaisesRegex(RuntimeError, "изменился"):
            bridge.replace_result(self.project, self.pool, Mock(), self.target, 80, 25, self.directory)
        self.timeline.DeleteClips.assert_not_called()

    def test_failed_insert_opens_backup(self):
        """A failed host edit must expose the recovery timeline, not report success."""
        self.pool.AppendToTimeline.side_effect = lambda infos: []
        with self.assertRaisesRegex(RuntimeError, "резервная копия"):
            bridge.replace_result(self.project, self.pool, Mock(), self.target, 80, 25, self.directory)
        self.project.SetCurrentTimeline.assert_called_with(self.backup)
        self.pool.DeleteTimelines.assert_not_called()

    def test_add_above_retains_original_and_allows_longer_result(self):
        """A new top track must not delete the source or be constrained by its next cut."""
        top = []
        tracks = {"video": 1, "audio": 1, "subtitle": 0}
        self.timeline.GetTrackCount.side_effect = lambda kind: tracks[kind]
        self.timeline.GetItemListInTrack.side_effect = lambda kind, index: (self.video if index == 1 else top) if kind == "video" else [self.audio]
        def add_track(kind):
            tracks[kind] += 1
            return True
        self.timeline.AddTrack.side_effect = add_track
        def append(infos):
            info = infos[0]
            self.assertEqual(info["recordFrame"], 100)
            self.assertEqual(info["trackIndex"], 2)
            item = self.make_item("above", 100, 240, 0, 139)
            item.GetTrackTypeAndIndex.return_value = ["video", 2]
            top.append(item)
            return [item]
        self.pool.AppendToTimeline.side_effect = append
        result = bridge.add_above_result(self.project, self.pool, Mock(), self.target, 140, 25, self.directory)
        self.assertEqual(result["mode"], "above")
        self.assertIn(self.original, self.video)
        self.assertEqual(self.following.GetStart(), 200)
        self.timeline.DeleteClips.assert_not_called()
        self.timeline.SetClipsLinked.assert_not_called()
        self.timeline.AddTrack.assert_called_once_with("video")
        self.pool.DeleteTimelines.assert_called_once_with([self.backup])
        self.assertFalse(result["backup_retained"])

    def test_add_above_reuses_nearest_track_with_space(self):
        """Reuse a partially occupied track when its clips are outside the entire output interval."""
        before = self.make_item("before", 0, 100, 0, 99)
        after = self.make_item("after", 240, 300, 0, 59)
        tracks = {1: self.video, 2: [before, after], 3: []}
        self.timeline.GetTrackCount.side_effect = lambda kind: 3 if kind == "video" else 1 if kind == "audio" else 0
        self.timeline.GetIsTrackEnabled.side_effect = lambda kind, track: track != 2
        self.timeline.GetItemListInTrack.side_effect = lambda kind, index: tracks[index] if kind == "video" else [self.audio]
        def append(infos):
            info = infos[0]
            self.assertEqual(info["trackIndex"], 2)
            item = self.make_item("reused", 100, 240, 0, 139)
            item.GetTrackTypeAndIndex.return_value = ["video", 2]
            tracks[2].append(item)
            return [item]
        self.pool.AppendToTimeline.side_effect = append
        result = bridge.add_above_result(self.project, self.pool, Mock(), self.target, 140, 25, self.directory)
        self.assertEqual(result["track_index"], 2)
        self.assertFalse(result["created_track"])
        self.timeline.AddTrack.assert_not_called()
        self.timeline.DeleteClips.assert_not_called()
        self.timeline.SetTrackEnable.assert_not_called()

    def test_above_track_search_checks_locks_and_full_output_duration(self):
        """A gap fitting the old source duration may still be too short for the prepared result."""
        occupied = self.make_item("occupied", 220, 300, 0, 79)
        self.timeline.GetTrackCount.return_value = 4
        self.timeline.GetTrackCount.side_effect = None
        self.timeline.GetIsTrackLocked.side_effect = lambda kind, track: track == 2
        self.timeline.GetItemListInTrack.side_effect = lambda kind, track: [occupied] if track == 3 else []
        self.assertEqual(bridge.available_above_track(self.timeline, self.target, 140), 4)
        self.timeline.GetIsTrackEnabled.side_effect = lambda kind, track: track != 4
        self.assertEqual(bridge.available_above_track(self.timeline, self.target, 140), 4)
        self.timeline.AddTrack.assert_not_called()


class ProcessedBake(unittest.TestCase):
    """Exercise isolated exports and job ownership without starting Resolve or a native render."""

    make_item = TimelineSelection.make_item

    def setUp(self):
        TimelineSelection.setUp(self)
        self.helper = self.directory / "fake-helper"
        self.helper.touch()
        self.project.GetSettings.return_value = {"colorScienceMode": "davinciYRGB", "colorSpaceTimeline": "Rec.709",
                                                "colorSpaceTimelineGamma": "Gamma 2.4", "timelineResolutionWidth": "1920",
                                                "timelineResolutionHeight": "1080", "timelineFrameRate": "25"}
        self.temporary_timeline = Mock()
        self.temporary_timeline.GetUniqueId.return_value = "temporary"
        self.temporary_timeline.GetName.return_value = "Temporary"
        self.temporary_timeline.GetTrackCount.return_value = 2
        self.clone = self.make_item("cloned-item", 100, 200, 40, 139)
        self.temporary_timeline.GetItemListInTrack.return_value = [self.clone]
        self.graph = self.temporary_timeline.GetNodeGraph.return_value
        self.graph.GetNumNodes.return_value = 2
        self.timeline.DuplicateTimeline.return_value = self.temporary_timeline
        self.active_timeline = self.timeline
        self.project.GetCurrentTimeline.side_effect = lambda: self.active_timeline
        def select(timeline):
            self.active_timeline = timeline
            return True
        self.project.SetCurrentTimeline.side_effect = select
        self.project.GetTimelineCount.return_value = 2
        self.project.GetTimelineByIndex.side_effect = lambda index: [self.timeline, self.temporary_timeline][index - 1]
        self.project.IsRenderingInProgress.return_value = False
        self.project.GetCurrentRenderMode.return_value = 0
        self.project.GetCurrentRenderFormatAndCodec.return_value = {"format": "mp4", "codec": "h264"}
        self.project.GetRenderFormats.return_value = {"QuickTime": "mov"}
        self.project.GetRenderCodecs.return_value = {"DNxHR 444": "dnxhr444"}
        self.project.AddRenderJob.return_value = "owned-job"
        self.project.GetRenderJobList.return_value = [{"JobId": "owned-job", "MarkIn": 100, "MarkOut": 199, "ExportAlpha": True}]
        self.project.GetRenderJobStatus.return_value = {"JobStatus": "Rendering", "CompletionPercentage": 40}

    def start_bake(self):
        """Simulate pressing Calculate after selection has already populated the UI."""
        return bridge.bake_start(self.project, {"job": str(self.directory), "target": self.target, "helper": str(self.helper)})

    def test_selection_does_not_start_render(self):
        """Opening the app may query metadata but must not render or duplicate timelines."""
        bridge.timeline_source(self.project)
        self.timeline.DuplicateTimeline.assert_not_called()
        self.project.StartRendering.assert_not_called()
        self.project.AddRenderJob.assert_not_called()

    def test_direct_start_never_enqueues_a_render_job(self):
        """Starting the item-frame pipeline must leave Resolve's render queue untouched."""
        with patch.object(bridge.subprocess, "Popen") as worker:
            worker.return_value.pid = 1234
            result = bridge.direct_start(self.project, {"job": str(self.directory), "target": self.target,
                                                        "helper": str(self.helper), "require_alpha": False})
        self.assertEqual(result["method"], "frame_provider")
        self.assertEqual(result["frame_count"], 100)
        self.project.AddRenderJob.assert_not_called()
        self.project.StartRendering.assert_not_called()
        self.timeline.DuplicateTimeline.assert_not_called()

    def test_range_falls_back_when_select_all_frames_is_rejected(self):
        """Some hosts reject SelectAllFrames; the queued marks must still be exact."""
        self.project.SetRenderSettings.side_effect = lambda values: "SelectAllFrames" not in values
        state = self.start_bake()
        self.assertEqual(state["codec_attempts"][0]["range_settings"], "marks_only")
        self.project.SetRenderSettings.assert_any_call({"MarkIn": 100, "MarkOut": 199})
        self.project.StartRendering.assert_called_once_with(["owned-job"], False)

    def test_range_fallback_rejects_wrong_queued_marks(self):
        """Never render the full timeline when the host did not accept the requested marks."""
        self.project.SetRenderSettings.side_effect = lambda values: "SelectAllFrames" not in values
        self.project.GetRenderJobList.return_value[0]["MarkOut"] = 299
        with self.assertRaisesRegex(RuntimeError, "Диапазон экспорта"):
            self.start_bake()
        self.project.StartRendering.assert_not_called()

    def test_range_fallback_rejects_reported_select_all(self):
        """Reported full-timeline mode must block rendering even if marks appear correct."""
        self.project.SetRenderSettings.side_effect = lambda values: "SelectAllFrames" not in values
        self.project.GetRenderJobList.return_value[0]["SelectAllFrames"] = True
        with self.assertRaisesRegex(RuntimeError, "всего таймлайна"):
            self.start_bake()
        self.project.StartRendering.assert_not_called()

    def test_pixel_aspect_is_inherited_from_timeline(self):
        """Square timeline pixels are inherited without forcing an unsupported render option."""
        self.project.SetRenderSettings.side_effect = lambda values: "PixelAspectRatio" not in values
        self.project.GetSettings.return_value["timelinePixelAspectRatio"] = "square"
        self.project.GetRenderJobList.return_value[0]["PixelAspectRatio"] = 1.0
        state = self.start_bake()
        self.assertEqual(state["state"], "rendering")
        self.assertEqual(state["timeline_pixel_aspect"], "square")
        self.assertTrue(all("PixelAspectRatio" not in call.args[0] for call in self.project.SetRenderSettings.call_args_list))

    def test_non_square_timeline_is_accepted(self):
        """Resolve labels do not identify the exact rational pixel aspect of the render."""
        self.timeline.GetSettings.return_value["timelinePixelAspectRatio"] = "4_3"
        self.assertEqual(self.start_bake()["timeline_pixel_aspect"], "4_3")

    def test_unrecognized_timeline_aspect_is_checked_at_render(self):
        """Unknown host labels must not be claimed to mean non-square pixels."""
        self.timeline.GetSettings.return_value["timelinePixelAspectRatio"] = "host-specific label"
        self.project.GetRenderJobList.return_value[0]["PixelAspectRatio"] = 1.0
        state = self.start_bake()
        self.assertEqual(state["timeline_pixel_aspect"], "host-specific label")

    def test_timeline_resolution_overrides_project(self):
        """Use the edited timeline grid rather than source or project dimensions."""
        self.timeline.GetSettings.return_value.update(timelineResolutionWidth="1280", timelineResolutionHeight="720")
        state = self.start_bake()
        self.assertEqual((state["width"], state["height"]), (1280, 720))
        self.project.SetRenderSettings.assert_any_call({"FormatWidth": 1280})
        self.project.SetRenderSettings.assert_any_call({"FormatHeight": 720})

    def test_queued_resolution_mismatch_stops_render(self):
        """A different host output size must not be silently substituted."""
        self.project.GetRenderJobList.return_value[0]["FormatWidth"] = 1280
        with self.assertRaisesRegex(RuntimeError, "FormatWidth"):
            self.start_bake()
        self.project.StartRendering.assert_not_called()

    def test_non_square_queued_pixels_are_recorded(self):
        """A non-square render is accepted and its reported aspect remains visible."""
        self.project.SetRenderSettings.side_effect = lambda values: "PixelAspectRatio" not in values
        self.project.GetRenderJobList.return_value[0]["PixelAspectRatio"] = 2.0
        state = self.start_bake()
        self.assertEqual(state["codec_attempts"][0]["reported_pixel_aspect"], 2.0)

    def test_setup_reports_all_rejected_required_fields(self):
        """Report multiple failures at once to avoid one retry per rejected setting."""
        rejected = {"ExportAudio", "ExportSubtitle", "DataBurnIn"}
        self.project.SetRenderSettings.side_effect = lambda values: not rejected.intersection(values)
        with self.assertRaises(RuntimeError) as error:
            self.start_bake()
        for name in rejected:
            self.assertIn(name, str(error.exception))
        self.project.AddRenderJob.assert_not_called()

    def test_preexisting_retime_uses_processed_timeline_grid(self):
        """Existing retime is baked by Resolve, not approximated with raw source extents."""
        self.original.GetSpeed.return_value = {"Percentage": 150}
        self.original.GetSourceEndFrame.return_value = 189
        context = bridge.timeline_source(self.project)
        self.assertEqual(context["source"]["marks"], {"in": 0, "out": 99})
        self.assertEqual(context["source"]["fps"], 25)
        self.assertEqual(context["target"]["speed"], 150)
        self.project.StartRendering.assert_not_called()

    def test_bake_isolated_clip_and_exact_record_range(self):
        """Calculate queues only the selected processed clip and restores Deliver before render."""
        state = self.start_bake()
        self.assertEqual(state["state"], "rendering")
        self.project.StartRendering.assert_called_once_with(["owned-job"], False)
        self.temporary_timeline.SetTrackEnable.assert_any_call("video", 1, True)
        self.temporary_timeline.SetTrackEnable.assert_any_call("video", 2, False)
        self.timeline.SetTrackEnable.assert_not_called()
        self.timeline.GetNodeGraph.assert_not_called()
        self.graph.SetNodeEnabled.assert_any_call(1, False)
        settings = {}
        for call in self.project.SetRenderSettings.call_args_list:
            settings.update(call.args[0])
        self.assertEqual((settings["MarkIn"], settings["MarkOut"]), (100, 199))
        self.assertFalse(settings["SelectAllFrames"])
        self.assertFalse(settings["ExportSubtitle"])
        self.assertEqual(settings["AddFrameHandles"], 0)
        self.assertTrue(settings["ExportAlpha"])
        self.assertEqual(state["frame_count"], 100)
        self.assertEqual(state["input_transfer"], "gamma24")
        self.project.SetCurrentRenderFormatAndCodec.assert_called_with("mp4", "h264")
        self.project.SetCurrentRenderMode.assert_called_with(0)
        self.assertIs(self.active_timeline, self.timeline)
        self.timeline.DeleteClips.assert_not_called()

    def test_complete_bake_publishes_only_finished_input(self):
        """An incomplete export cannot be used as cadence input."""
        self.start_bake()
        running = bridge.bake_poll(self.project, {"job": str(self.directory)})
        self.assertNotIn("input", running)
        media = self.directory / "resolve_source/processed.mov"
        media.write_bytes(b"finished fake render")
        self.project.GetRenderJobStatus.return_value = {"JobStatus": "Complete", "CompletionPercentage": 100}
        with patch.object(bridge, "inspect_baked_media", return_value={"has_alpha": True}):
            ready = bridge.bake_poll(self.project, {"job": str(self.directory)})
        self.assertEqual(ready["state"], "complete")
        self.assertEqual(ready["input"], str(media))
        self.project.DeleteRenderJob.assert_called_once_with("owned-job")
        self.project.GetMediaPool().DeleteTimelines.assert_called_once_with([self.temporary_timeline])

    def test_foreign_render_is_not_cancelled(self):
        """Cancellation cannot stop a render owned by a different queue operation."""
        self.start_bake()
        self.project.GetRenderJobList.return_value = [{"JobId": "owned-job"}, {"JobId": "foreign-job"}]
        with self.assertRaisesRegex(RuntimeError, "другой рендер"):
            bridge.bake_poll(self.project, {"job": str(self.directory)}, cancel=True)
        self.project.StopRendering.assert_not_called()

    def test_setup_failure_restores_original_state(self):
        """A rejected render setup must not edit the source or leave it selected on a scratch timeline."""
        self.project.SetRenderSettings.return_value = False
        with self.assertRaisesRegex(RuntimeError, "codec"):
            self.start_bake()
        self.assertIs(self.active_timeline, self.timeline)
        self.project.LoadRenderPreset.assert_called()
        self.project.StartRendering.assert_not_called()
        self.timeline.DeleteClips.assert_not_called()
        state = json.loads((self.directory / "bake_state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["state"], "error")

    def test_unsupported_color_contract_stops_before_mutation(self):
        """Managed/HDR signals cannot be silently interpreted as native SDR sRGB."""
        self.project.GetSettings.return_value["colorScienceMode"] = "davinciYRGBColorManagedv2"
        with self.assertRaisesRegex(RuntimeError, "Color Managed"):
            self.start_bake()
        self.timeline.DuplicateTimeline.assert_not_called()
        self.project.SaveAsNewRenderPreset.assert_not_called()

    def test_combined_working_space_label(self):
        """Both combined and separate Resolve color-setting representations are accepted explicitly."""
        settings = self.project.GetSettings.return_value
        settings.update(colorSpaceTimeline="Rec.709 Gamma 2.4", colorSpaceTimelineGamma="", separateColorSpaceAndGamma="0")
        self.assertEqual(bridge.bake_profile(self.project, self.timeline)["input_transfer"], "gamma24")

    def test_alpha_mode_is_set_after_export_alpha(self):
        """The dependent alpha mode setting must be applied after alpha is enabled."""
        seen = []
        def setting(values):
            key = next(iter(values))
            if key == "AlphaMode":
                self.assertIn("ExportAlpha", seen)
            seen.append(key)
            return True
        self.project.SetRenderSettings.side_effect = setting
        self.project.GetRenderJobList.return_value[0]["ExportAlpha"] = "1"
        self.assertEqual(self.start_bake()["state"], "rendering")

    def test_absent_alpha_metadata_is_verified_in_file(self):
        """A missing queue field must not be mistaken for an explicit alpha rejection."""
        del self.project.GetRenderJobList.return_value[0]["ExportAlpha"]
        self.assertEqual(self.start_bake()["state"], "rendering")

    def test_actual_missing_alpha_blocks_native_input(self):
        """Requested transparency cannot be silently discarded after export."""
        self.start_bake()
        (self.directory / "resolve_source/processed.mov").write_bytes(b"finished")
        self.project.GetRenderJobStatus.return_value = {"JobStatus": "Complete"}
        with patch.object(bridge, "inspect_baked_media", side_effect=RuntimeError("No alpha")):
            result = bridge.bake_poll(self.project, {"job": str(self.directory)})
        self.assertEqual(result["state"], "error")
        self.assertNotIn("input", result)

    def test_decoded_alpha_requirement_can_only_be_disabled_explicitly(self):
        """Decoded pixel format must satisfy the requested transparency contract."""
        state = {"helper": str(self.helper), "width": 1920, "height": 1080, "fps": 25}
        output = Mock(returncode=0, stdout=json.dumps({"width": 1920, "height": 1080, "fps": 25,
                                                     "has_alpha": False}), stderr="")
        with patch.object(bridge.subprocess, "run", return_value=output) as run:
            with self.assertRaisesRegex(RuntimeError, "нет alpha"):
                bridge.inspect_baked_media(state, self.directory / "processed.mov")
            state["require_alpha"] = False
            self.assertFalse(bridge.inspect_baked_media(state, self.directory / "processed.mov")["has_alpha"])
            self.assertEqual(run.call_args.args[0][1], "--inspect")

    def test_resolution_and_lut_options_are_not_active_global_luts(self):
        """Resolution and interpolation controls must not be interpreted as assigned LUT paths."""
        self.project.GetSettings.return_value.update(timelineOutputResolutionWidth="3840",
                                                     timelineOutputResolutionHeight="2160",
                                                     timelineLUTInterpolation="tetrahedral")
        profile = bridge.bake_profile(self.project, self.timeline)
        self.assertEqual((profile["width"], profile["height"]), (1920, 1080))

    def test_assigned_global_lut_still_blocks_bake(self):
        """Narrowing field detection must retain protection against double output grading."""
        settings = self.project.GetSettings.return_value
        for key in ("colorOutputLUT", "colorTimelineLUT", "output_lut_path"):
            with self.subTest(key=key):
                settings[key] = "Looks/Example.cube"
                with self.assertRaisesRegex(RuntimeError, "Глобальный output/timeline LUT"):
                    bridge.bake_profile(self.project, self.timeline)
                del settings[key]
        self.project.SaveAsNewRenderPreset.assert_not_called()

    def test_unassigned_global_lut_is_allowed(self):
        """Empty LUT selections are valid neutral settings."""
        self.project.GetSettings.return_value.update(colorOutputLUT="None", colorTimelineLUT="")
        self.assertEqual(bridge.bake_profile(self.project, self.timeline)["input_transfer"], "gamma24")

    def test_empty_timeline_gamma_preserves_project_gamma(self):
        """An empty optional timeline field must not erase a known inherited project gamma."""
        self.timeline.GetSettings.return_value.update(colorSpaceTimelineGamma="")
        self.assertEqual(bridge.bake_profile(self.project, self.timeline)["input_transfer"], "gamma24")

    def test_missing_gamma_can_be_read_from_legacy_getter(self):
        """Support host versions whose bulk settings omit a separately queryable color field."""
        self.project.GetSettings.return_value["colorSpaceTimelineGamma"] = ""
        self.timeline.GetSetting.side_effect = lambda key: "Gamma 2.4" if key == "colorSpaceTimelineGamma" else None
        self.assertEqual(bridge.bake_profile(self.project, self.timeline)["input_transfer"], "gamma24")

    def test_combined_legacy_getter_can_supply_missing_gamma(self):
        """A documented combined setting can resolve missing split gamma metadata."""
        self.project.GetSettings.return_value["colorSpaceTimelineGamma"] = ""
        self.timeline.GetSetting.side_effect = lambda key: "Rec.709 Gamma 2.4" if key == "colorSpaceTimeline" else None
        self.assertEqual(bridge.bake_profile(self.project, self.timeline)["input_transfer"], "gamma24")

    def test_unknown_gamma_requires_explicit_choice(self):
        """An absent field is not silently treated as Gamma 2.4."""
        self.project.GetSettings.return_value["colorSpaceTimelineGamma"] = None
        with self.assertRaisesRegex(RuntimeError, "Гамма входа"):
            bridge.bake_profile(self.project, self.timeline)
        self.assertEqual(bridge.bake_profile(self.project, self.timeline, "gamma24")["input_transfer"], "gamma24")

    def test_manual_gamma_does_not_override_known_signal(self):
        """A manual label cannot pretend to convert an existing gamma or Log signal."""
        self.project.GetSettings.return_value["colorSpaceTimelineGamma"] = "sRGB"
        with self.assertRaisesRegex(RuntimeError, "не совпадает"):
            bridge.bake_profile(self.project, self.timeline, "gamma24")
        self.project.GetSettings.return_value["colorSpaceTimelineGamma"] = "LogC"
        with self.assertRaisesRegex(RuntimeError, "без CST"):
            bridge.bake_profile(self.project, self.timeline, "gamma24")


class SavedJobUiState(unittest.TestCase):
    """Exercise saved-job transitions without starting GTK or connecting to Resolve."""

    def test_opened_job_cannot_start_raw_source_with_timeline_range(self):
        """Opening a result must clear stale input fields and block a new raw-file render."""
        app_spec = importlib.util.spec_from_file_location("cadence_app_test", Path(__file__).resolve().parents[1] / "scripts/cadence_app.py")
        app_module = importlib.util.module_from_spec(app_spec)
        with patch.dict("sys.modules", {"gi": Mock(), "gi.repository": Mock(), "cadence_resolve_bridge": bridge}):
            app_spec.loader.exec_module(app_module)
        app = app_module.Application.__new__(app_module.Application)
        app.running_job = app.future = None
        for name in ("placement", "job", "timeline_label", "status", "numeric_request", "choose_path"):
            setattr(app, name, Mock())
        app.values = {}
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            (directory / "app_context.json").write_text(json.dumps({"target": {"item_id": "saved"}, "source_name": "clip"}), encoding="utf-8")
            app.open_job()
            callback = app.choose_path.call_args.args[2]
            callback(folder)
            self.assertIsNone(app.source_snapshot)
            self.assertEqual(app.values, {})
            with self.assertRaisesRegex(RuntimeError, "Import"):
                app.start()
            app.numeric_request.assert_not_called()


class CameraParameterMapping(unittest.TestCase):
    """Keep capture timing modes and grouped UI controls independent from playback FPS."""

    def setUp(self):
        app_spec = importlib.util.spec_from_file_location("cadence_parameter_test", Path(__file__).resolve().parents[1] / "scripts/cadence_app.py")
        self.app = importlib.util.module_from_spec(app_spec)
        with patch.dict("sys.modules", {"gi": Mock(), "gi.repository": Mock(), "cadence_resolve_bridge": bridge}):
            app_spec.loader.exec_module(self.app)

    def test_fps_modes_use_the_selected_clock(self):
        resolve = self.app.source_interpretation_fps
        context = {"timeline_fps": 25, "output_fps": 30}
        snapshot = {"fps": 25, "target": {"fps": 24}}
        self.assertEqual(resolve("timeline", 36, context, snapshot), 25)
        self.assertEqual(resolve("source", 36, context, snapshot), 24)
        self.assertEqual(resolve("custom", 36, context, snapshot), 36)
        self.assertEqual(resolve("source", 36, {}, None), 0)
        with self.assertRaisesRegex(RuntimeError, "таймлайн"):
            resolve("timeline", 36, {"output_fps": 30}, snapshot)

    def test_loading_existing_preset_does_not_replace_dropdown_model(self):
        """Loading after Factory Reset must not enqueue new selection notifications."""
        app = self.app.Application.__new__(self.app.Application)
        app.preferences = Mock()
        app.preferences.names.return_value = ["My preset"]
        app._preset_names = ["My preset"]
        app._preferences_loading = False
        app.preset_list = Mock()
        app.preset_list.get_selected.return_value = 1
        app.preset_name = Mock()
        app.refresh_presets("My preset")
        app.preset_list.set_model.assert_not_called()
        app.preset_list.set_selected.assert_not_called()
        self.assertFalse(app._preferences_loading)

    def test_repeated_preset_notification_is_idempotent(self):
        """GTK may deliver a pending notification after the preset is already applied."""
        app = self.app.Application.__new__(self.app.Application)
        app.running_job = app.future = app.resume_job = None
        app.preferences = Mock()
        app.preferences.names.return_value = ["My preset"]
        app.preferences.selected = "My preset"
        app._preference_dirty = False
        app.preset_list = Mock()
        app.preset_list.get_selected.return_value = 1
        app.apply_preferences = Mock()
        app.select_preset()
        app.preferences.set_current.assert_not_called()
        app.apply_preferences.assert_not_called()

    def test_groups_cover_each_numeric_control_once(self):
        fields = [item[0] for item in self.app.Application.FIELDS]
        grouped = [key for _, keys in self.app.Application.FIELD_GROUPS for key in keys]
        self.assertEqual(sorted(fields), sorted(grouped))
        self.assertEqual(len(grouped), len(set(grouped)))

    def test_calculate_refreshes_actual_timeline_selection_before_render(self):
        app = self.app.Application.__new__(self.app.Application)
        app.running_job = app.future = app.opened_job = app.resume_job = None
        app.load_timeline_source = Mock()
        app.start()
        app.load_timeline_source.assert_called_once_with(after=app.start_selection)

    def test_clip_bounds_and_output_rate_are_not_editable_controls(self):
        fields = {item[0] for item in self.app.Application.FIELDS}
        self.assertTrue(fields.isdisjoint({"start_frame", "frame_count", "output_fps"}))
        self.assertIn("source_fps", self.app.Application.PRESET_NUMBERS)

    def test_ranges_require_timeline_selection(self):
        interval = self.app.capture_range
        self.assertEqual(interval({"target": {"start": 100, "end": 148},
                                   "marks": {"in": 12, "out": 200}}),
                         {"start_frame": 0, "frame_count": 48})
        snapshot = {"target": {"start": 146, "end": 247, "source_start": 140, "source_end": 237, "fps": 24}}
        self.assertEqual(interval(snapshot, raw=True), interval(snapshot, raw=False))
        self.assertEqual(interval(snapshot, raw=True), {"start_frame": 0, "frame_count": 101})
        with self.assertRaises(ValueError):
            interval({"marks": {"in": 12, "out": 47}})
        with self.assertRaises(ValueError):
            interval(None)
        with self.assertRaises(ValueError):
            interval({"marks": {"in": 20, "out": 10}})

    def test_automatic_display_does_not_replace_saved_custom_fps(self):
        app = self.app.Application.__new__(self.app.Application)
        app.preference_numbers = {"source_fps": (25, 1, 120, False)}
        app.source_fps_mode = Mock()
        app.source_fps_mode.get_selected.return_value = 0
        app._draft_preferences = {"source_fps": 36}
        app.values = {"source_fps": Mock()}
        app.values["source_fps"].get_text.return_value = "25"
        self.assertEqual(app.read_preference("source_fps"), 36)
        app.source_fps_mode.get_selected.return_value = 2
        app.values["source_fps"].get_text.return_value = "48"
        self.assertEqual(app.read_preference("source_fps"), 48)


class BatchProcessing(unittest.TestCase):
    """Protect sequential imports, frozen trims and cancellation between clip jobs."""

    def setUp(self):
        from cadence_batch import BatchQueue
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.items = [{"project_id": "P", "target": {"item_id": key, "source_start": start},
                       "source": {"name": key}} for key, start in (("A", 12), ("B", 90), ("C", 150))]
        self.queue = BatchQueue(self.items, {"seed": 17}, self.directory)

    def test_snapshots_survive_selection_changes_and_same_media_trims(self):
        self.items[0]["target"]["source_start"] = 999
        self.items.clear()
        self.queue.begin()
        self.assertEqual(self.queue.current["snapshot"]["target"]["source_start"], 12)
        self.queue.complete(self.directory / "A", "imported")
        self.assertEqual(self.queue.current["snapshot"]["target"]["source_start"], 90)
        report = json.loads(self.queue.path.read_text(encoding="utf-8"))
        self.assertEqual(report["index"], 1)
        self.assertEqual(report["settings"], {"seed": 17})
        self.assertEqual([entry["state"] for entry in report["clips"]], ["imported", "pending", "pending"])

    def test_failure_preserves_completed_results_and_skips_remaining_clips(self):
        self.queue.begin()
        self.queue.complete("A", "imported")
        self.queue.begin()
        self.queue.stop("Ошибка импорта")
        self.assertFalse(self.queue.active)
        self.assertEqual([entry["state"] for entry in self.queue.entries], ["imported", "error", "skipped"])
        self.queue.complete("B", "imported")
        self.assertEqual(self.queue.index, 1)
        with self.assertRaises(RuntimeError):
            self.queue.begin()

    def make_app(self):
        app_spec = importlib.util.spec_from_file_location("cadence_batch_ui_test", Path(__file__).resolve().parents[1] / "scripts/cadence_app.py")
        module = importlib.util.module_from_spec(app_spec)
        with patch.dict(sys.modules, {"gi": Mock(), "gi.repository": Mock(), "cadence_resolve_bridge": bridge}):
            app_spec.loader.exec_module(module)
        app = module.Application.__new__(module.Application)
        app.batch = self.queue
        for name in ("job", "progress", "batch_progress", "batch_label", "status", "submit",
                     "update_source_fps_display", "update_source_mode", "guarded"):
            setattr(app, name, Mock())
        app.job.get_text.return_value = str(self.directory)
        app.running_job = app.process = app.future = app.baking_job = app.imported_job = None
        app.job_target = {"item_id": "A"}
        app.auto_pending = True
        app.active_import_mode = "above"
        return module, app

    def test_next_clip_waits_for_import_and_old_result_cannot_finish_next_clip(self):
        module, app = self.make_app()
        self.queue.begin()
        (self.directory / "status.ini").write_text("state=complete\n", encoding="utf-8")
        (self.directory / "result.ini").write_text("state=complete\n", encoding="utf-8")
        app.update_job()
        app.update_job()
        self.assertEqual(self.queue.index, 0)
        self.assertEqual(self.queue.current["state"], "importing")
        app.submit.assert_called_once()
        app.submit.call_args.args[1]({"message": "Импортировано"})
        self.assertEqual(self.queue.index, 1)
        module.GLib.idle_add.assert_called_once_with(app.next_batch_item)
        app.update_job()
        self.assertEqual(self.queue.current["state"], "pending")
        self.assertEqual(self.queue.index, 1)
        app.next_batch_item()
        app.job.set_text.assert_called_with("")
        self.assertEqual(app.source_snapshot["target"]["item_id"], "B")
        app.guarded.assert_called_once_with(app.start_selected)

    def test_auto_import_off_advances_after_render_without_resolve_import(self):
        module, app = self.make_app()
        self.queue.begin()
        app.auto_pending = False
        (self.directory / "status.ini").write_text("state=complete\n", encoding="utf-8")
        (self.directory / "result.ini").write_text("state=complete\n", encoding="utf-8")
        app.update_job()
        self.assertEqual(self.queue.entries[0]["state"], "rendered")
        app.submit.assert_not_called()
        module.GLib.idle_add.assert_called_once_with(app.next_batch_item)

    def test_cancel_before_next_launch_keeps_remaining_clips_untouched(self):
        module, app = self.make_app()
        self.queue.begin()
        app.cancel()
        self.assertFalse(self.queue.active)
        self.assertEqual([entry["state"] for entry in self.queue.entries], ["cancelled", "skipped", "skipped"])
        app.next_batch_item()
        app.guarded.assert_not_called()
        app.submit.assert_not_called()

    def test_failed_native_launch_releases_busy_state_and_cancels_provider(self):
        module, app = self.make_app()
        app.args = Mock(helper="/missing/silentfilm_cadence")
        app.running_job = app.baking_job = self.directory
        with patch.object(module.subprocess, "Popen", side_effect=OSError("Нет исполняемого файла")):
            with self.assertRaisesRegex(OSError, "исполняемого"):
                app.launch_native(self.directory, {"seed": 17})
        self.assertIsNone(app.running_job)
        self.assertIsNone(app.baking_job)
        self.assertFalse(app.auto_pending)
        self.assertTrue((self.directory / "cancel.request").exists())


if __name__ == "__main__":
    unittest.main()
