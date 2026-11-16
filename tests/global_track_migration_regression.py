#!/usr/bin/env python3
"""Check the one-time preset migration, backups, conflicts, and repeat runs."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/migrate_global_tracks.py"
spec = importlib.util.spec_from_file_location("migration", SCRIPT)
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)


class MigrationTests(unittest.TestCase):
    def source(self):
        return {"timelines": [{"tracks": [{
            "isProjectTrack": True, "name": "Project Track", "id": "track_1",
            "clips": [{"name": "Project", "projectKeyframes": [{
                "beat": 4.25, "ease": {"inflection": 0.3, "steepness": 2.0},
                "values": {"Project Track": "projectKeyframes", "Text": "Project"}
            }]}]
        }]}]}

    def test_nested_content_and_custom_names(self):
        data = self.source()
        original = copy.deepcopy(data)
        self.assertTrue(migration.migrate(data))
        track = data["timelines"][0]["tracks"][0]
        old = original["timelines"][0]["tracks"][0]
        self.assertTrue(track["isGlobalTrack"])
        self.assertNotIn("isProjectTrack", track)
        self.assertEqual(track["name"], "Global Track")
        self.assertEqual(track["clips"][0]["name"], "Global")
        self.assertEqual(track["clips"][0]["globalKeyframes"], old["clips"][0]["projectKeyframes"])
        self.assertFalse(migration.migrate(data))
        old["name"] = "My scenes"
        old["clips"][0]["name"] = "Intro"
        migration.migrate(original)
        self.assertEqual(old["name"], "My scenes")
        self.assertEqual(old["clips"][0]["name"], "Intro")
        self.assertEqual(migration.default_name("Project 12", "Project", "Global"), "Global 12")

    def test_unrelated_content(self):
        data = {"name": "Project Track", "values": {"Project": "Project Track"}}
        before = copy.deepcopy(data)
        self.assertFalse(migration.migrate(data))
        self.assertEqual(data, before)

    def test_conflict(self):
        data = self.source()
        data["timelines"][0]["tracks"][0]["isGlobalTrack"] = False
        with self.assertRaises(ValueError):
            migration.migrate(data)

    def test_preview_apply_and_backup(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "timeline.json"
            original = json.dumps(self.source(), indent=4).encode()
            path.write_bytes(original)
            command = [sys.executable, str(SCRIPT), folder]
            subprocess.run(command, check=True, capture_output=True)
            self.assertEqual(path.read_bytes(), original)
            subprocess.run(command + ["--apply"], check=True, capture_output=True)
            self.assertEqual(path.with_name(path.name + ".before-global-track").read_bytes(), original)
            expected = self.source()
            migration.migrate(expected)
            self.assertEqual(json.loads(path.read_bytes()), expected)
            result = subprocess.run(command + ["--apply"], check=True, capture_output=True, text=True)
            self.assertIn("0 JSON file(s) updated", result.stdout)


if __name__ == "__main__":
    unittest.main()
