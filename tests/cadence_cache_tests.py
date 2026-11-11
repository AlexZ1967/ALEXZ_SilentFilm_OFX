"""Exercise managed storage and destructive cleanup isolation without GTK or Resolve."""

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cadence_cache as cache
import cadence_resolve_bridge as bridge


class CacheFixture(unittest.TestCase):
    """Protect project boundaries and active or referenced generations."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.context = {"project_id": "project-A", "project_name": "Фильм / 1920", "timeline_id": "timeline-A"}

    def job(self, context=None, name="Scene 01", state="complete"):
        directory = cache.create_job(self.root, context or self.context, name)
        (directory / "status.ini").write_text("state=" + state + "\n", encoding="utf-8")
        (directory / "Scene_01_cadence.mov").write_bytes(b"prepared frames")
        return directory

    def candidates(self, used=(), media=()):
        return cache.candidates(self.root, "project-A", used, media)


class ManagedStorage(CacheFixture):
    """Protect project boundaries and active or referenced generations."""

    def test_project_identity_separates_equal_names_and_rename_reuses_directory(self):
        first = self.job()
        second = self.job(dict(self.context, project_id="project-B"))
        renamed = self.job(dict(self.context, project_name="Renamed"))
        self.assertNotEqual(first.parent, second.parent)
        self.assertEqual(first.parent, renamed.parent)
        self.assertTrue(first.parent.name.startswith("Фильм _ 1920_"))
        self.assertTrue(first.name.startswith("Scene 01_cadence_"))
        self.assertNotEqual(first.name, renamed.name)
        self.assertEqual({entry["path"] for entry in self.candidates()}, {str(first), str(renamed)})

    def test_tiff_frame_or_pattern_reference_preserves_whole_generation(self):
        directory = self.job()
        for path in (directory / "frames" / "Scene_01_cadence_00000009.tif",
                     directory / "frames" / "Scene_01_cadence_%08d.tif"):
            self.assertEqual(self.candidates([str(path)]), [])

    def test_unknown_or_nonzero_media_pool_usage_preserves_data(self):
        directory = self.job()
        for usage in (None, 1, 3):
            self.assertEqual(self.candidates(media=[{"path": str(directory / "output.mov"), "usage": usage}]), [])
        self.assertEqual(len(self.candidates(media=[{"path": str(directory / "output.mov"), "usage": 0}])), 1)

    def test_active_unmarked_and_foreign_directories_are_not_candidates(self):
        self.job(state="rendering")
        foreign = self.job(dict(self.context, project_id="project-B"))
        unmarked = foreign.parent.parent / "Old_Cadence"
        unmarked.mkdir()
        (unmarked / "status.ini").write_text("state=complete\n", encoding="utf-8")
        self.assertEqual(self.candidates(), [])
        self.assertTrue(foreign.exists())
        self.assertTrue(unmarked.exists())

    def test_symlinked_generation_and_project_are_not_followed(self):
        directory = self.job()
        (directory.parent / "alias").symlink_to(directory, target_is_directory=True)
        (self.root / "project-alias").symlink_to(directory.parent, target_is_directory=True)
        self.assertEqual(len(self.candidates()), 1)
        marker = directory / cache.JOB_MARKER
        marker.unlink()
        real = self.root / "external-marker.json"
        real.write_text(json.dumps({"schema": 1, "project_id": "project-A", "generation_id": "foreign"}), encoding="utf-8")
        marker.symlink_to(real)
        self.assertEqual(self.candidates(), [])


class HostCleanup(CacheFixture):
    """Recheck every timeline and Media Pool immediately before deleting approved data."""

    def setUp(self):
        super().setUp()
        self.directory = self.job()
        self.clip = Mock()
        self.clip.GetClipProperty.side_effect = {"File Path": str(self.directory / "Scene_01_cadence.mov"), "Usage": "0"}.get
        self.folder = Mock()
        self.folder.GetUniqueId.return_value = "root-bin"
        self.folder.GetClipList.return_value = [self.clip]
        self.folder.GetSubFolderList.return_value = []
        self.pool = Mock()
        self.pool.GetRootFolder.return_value = self.folder
        self.pool.DeleteClips.return_value = True
        self.timelines = []
        for identity in ("timeline-A", "timeline-B"):
            timeline = Mock()
            timeline.GetUniqueId.return_value = identity
            timeline.GetName.return_value = identity
            timeline.GetSettings.return_value = {"timelineFrameRate": "25"}
            timeline.GetTrackCount.side_effect = {"video": 1, "audio": 0}.get
            timeline.GetItemListInTrack.return_value = []
            self.timelines.append(timeline)
        self.project = Mock()
        self.project.GetUniqueId.return_value = "project-A"
        self.project.GetName.return_value = "Фильм / 1920"
        self.project.GetCurrentTimeline.return_value = self.timelines[0]
        self.project.GetTimelineCount.return_value = 2
        self.project.GetTimelineByIndex.side_effect = lambda index: self.timelines[index - 1]
        self.project.GetMediaPool.return_value = self.pool

    def plan(self):
        return bridge.clean_project_cache(self.project, {"action": "cache_plan", "root": str(self.root), "project_id": "project-A"})

    def clean(self, entries):
        return bridge.clean_project_cache(self.project, {"action": "cache_clean", "root": str(self.root), "project_id": "project-A", "entries": entries})

    def test_only_approved_current_project_jobs_and_unused_pool_entries_are_deleted(self):
        plan = self.plan()
        newer = self.job(name="Later")
        foreign = self.job(dict(self.context, project_id="project-B"))
        result = self.clean(plan["entries"])
        self.assertEqual(result["deleted"], [str(self.directory)])
        self.assertFalse(self.directory.exists())
        self.assertTrue(newer.exists())
        self.assertTrue(foreign.exists())
        self.pool.DeleteClips.assert_called_once_with([self.clip])

    def test_new_reference_on_other_timeline_after_preview_prevents_deletion(self):
        plan = self.plan()
        item = Mock()
        item.GetMediaPoolItem.return_value = self.clip
        self.timelines[1].GetItemListInTrack.return_value = [item]
        result = self.clean(plan["entries"])
        self.assertEqual(result["deleted"], [])
        self.assertEqual(result["skipped"], 1)
        self.assertTrue(self.directory.exists())
        self.pool.DeleteClips.assert_not_called()

    def test_pool_delete_failure_preserves_physical_files(self):
        plan = self.plan()
        self.pool.DeleteClips.return_value = False
        result = self.clean(plan["entries"])
        self.assertEqual(len(result["errors"]), 1)
        self.assertTrue(self.directory.exists())

    def test_project_switch_or_incomplete_timeline_scan_prevents_cleanup(self):
        plan = self.plan()
        self.project.GetUniqueId.return_value = "project-B"
        with self.assertRaisesRegex(RuntimeError, "проект изменился"):
            self.clean(plan["entries"])
        self.project.GetUniqueId.return_value = "project-A"
        self.project.GetTimelineByIndex.side_effect = lambda index: None
        with self.assertRaisesRegex(RuntimeError, "таймлайн"):
            self.clean(plan["entries"])
        self.assertTrue(self.directory.exists())

    def test_timeline_scope_keeps_other_timelines_generations_and_shared_results(self):
        other = self.job(dict(self.context, timeline_id="timeline-B"), name="Other")
        request = {"action": "cache_plan", "root": str(self.root), "project_id": "project-A",
                   "scope": "timeline", "timeline_id": "timeline-A"}
        plan = bridge.clean_project_cache(self.project, request)
        self.assertEqual([entry["path"] for entry in plan["entries"]], [str(self.directory)])
        item = Mock()
        item.GetMediaPoolItem.return_value = self.clip
        self.timelines[1].GetItemListInTrack.return_value = [item]
        self.assertEqual(bridge.clean_project_cache(self.project, request)["entries"], [])
        self.timelines[1].GetItemListInTrack.return_value = []
        outcome = bridge.clean_project_cache(self.project, dict(request, action="cache_clean", entries=plan["entries"]))
        self.assertEqual(outcome["deleted"], [str(self.directory)])
        self.assertTrue(other.exists())

    def test_timeline_switch_after_confirmation_prevents_deletion(self):
        request = {"action": "cache_plan", "root": str(self.root), "project_id": "project-A",
                   "scope": "timeline", "timeline_id": "timeline-A"}
        plan = bridge.clean_project_cache(self.project, request)
        self.project.GetCurrentTimeline.return_value = self.timelines[1]
        with self.assertRaisesRegex(RuntimeError, "таймлайн изменился"):
            bridge.clean_project_cache(self.project, dict(request, action="cache_clean", entries=plan["entries"]))
        self.assertTrue(self.directory.exists())


if __name__ == "__main__":
    unittest.main()
