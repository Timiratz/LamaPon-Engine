"""分離したCLIコマンドを、公開引数・JSON・終了コードから検査する。"""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


class SceneCommandTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="シーン CLI ", dir=WORK_DIR)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.write(".lamapon/project.json", {"startupScene": "scenes/main.scene"})
        self.document = {
            "format": "LamaPonScene", "version": 1, "mainCamera": None,
            "objects": [{"id": 1, "name": "編集前", "parent": None,
                         "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
                         "components": [{"type": "Rigidbody", "mass": 1.0}]}]}
        self.scene = self.write("assets/scenes/main.scene", self.document)
        self.prefab = self.write("assets/prefabs/main.prefab", {
            **self.document, "format": "LamaPonPrefab", "root": 1})

    def write(self, name, document):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
        return path

    def cli(self, *arguments, code=0):
        process = subprocess.run([str(EXECUTABLE), *map(str, arguments)],
                                 capture_output=True, encoding="utf-8", timeout=30)
        self.assertEqual(process.returncode, code, process.stdout + process.stderr)
        # json.loadsは余分な進行文や2個目のJSONも拒否します。
        report = json.loads(process.stdout)
        self.assertEqual(report["ok"], code == 0, report)
        return report

    def scene_command(self, command, *arguments, code=0):
        return self.cli(command, "--project", self.root, "--scene", "scenes/main.scene",
                        *arguments, code=code)

    def test_component_catalog_and_errors(self):
        report = self.cli("component", "list", "--category", "UI")
        self.assertEqual(report["command"], "component list")
        self.assertEqual(report["count"], len(report["components"]))
        self.assertTrue(all(item["category"] == "UI" for item in report["components"]))
        schema = self.cli("component", "schema", "--type", "Rigidbody")["schema"]
        self.assertEqual(next(f for f in schema["fields"] if f["name"] == "mass")["type"], "number")
        report = self.cli("component", "schema", "--type", "DoesNotExist", code=1)
        self.assertIn("Unknown component schema", report["error"])

    def test_scene_inspect_validate_and_failed_assertions(self):
        report = self.scene_command("inspect")
        self.assertEqual(report["document"]["objects"][0]["name"], "編集前")
        self.assertEqual((report["objectCount"], report["componentCount"]), (1, 1))
        self.assertEqual(self.scene_command("validate")["problems"], [])
        self.write("assertions.json", [{"kind": "object-count", "expected": 1}])
        report = self.scene_command("test", "--spec", "assertions.json")
        self.assertEqual(report["passed"], 1)
        self.write("assertions.json", [{"kind": "object-count", "expected": 2}])
        self.assertEqual(self.scene_command("test", "--spec", "assertions.json", code=1)["failed"], 1)

    def test_patch_preview_commit_and_invalid_type_leave_original(self):
        before = self.scene.read_bytes()
        self.write("operations.json", [{"op": "rename", "target": 1, "name": "編集後"}])
        preview = self.scene_command("patch", "--operations", "operations.json", "--dry-run")
        self.assertEqual(preview["operationsApplied"], 1)
        self.assertEqual(self.scene.read_bytes(), before)
        self.scene_command("patch", "--operations", "operations.json")
        self.assertEqual(json.loads(self.scene.read_text(encoding="utf-8"))["objects"][0]["name"], "編集後")
        committed = self.scene.read_bytes()
        self.write("operations.json", [
            {"op": "rename", "target": 1, "name": "保存されない名前"},
            {"op": "set-component", "target": 1, "type": "Rigidbody", "path": "mass", "value": "invalid"}])
        report = self.scene_command("patch", "--operations", "operations.json", code=1)
        self.assertIn("wrong type", report["error"])
        self.assertEqual(self.scene.read_bytes(), committed)

    def test_prefab_commands(self):
        args = ("--project", self.root, "--path", "prefabs/main.prefab")
        self.assertEqual(self.cli("prefab", "inspect", *args)["objectCount"], 1)
        self.cli("prefab", "validate", *args)
        self.write("operations.json", [{"op": "rename", "target": 1, "name": "Prefab編集後"}])
        self.cli("prefab", "patch", *args, "--operations", "operations.json")
        self.assertEqual(json.loads(self.prefab.read_text(encoding="utf-8"))["objects"][0]["name"], "Prefab編集後")

    def test_invalid_hierarchy_and_outside_output(self):
        self.document["objects"][0]["parent"] = 1
        self.write("assets/scenes/main.scene", self.document)
        report = self.scene_command("validate", code=1)
        self.assertIn("parent-cycle", {p["kind"] for p in report["problems"]})
        self.document["objects"][0]["parent"] = None
        self.write("assets/scenes/main.scene", self.document)
        before = self.scene.read_bytes()
        self.write("operations.json", [{"op": "rename", "target": 1, "name": "保存不可"}])
        report = self.scene_command("patch", "--operations", "operations.json", "--out", "../outside.scene", code=1)
        self.assertIn("inside the project", report["error"])
        self.assertEqual(self.scene.read_bytes(), before)
        self.assertFalse((self.root.parent / "outside.scene").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    arguments, remaining = parser.parse_known_args()
    EXECUTABLE = arguments.executable.resolve()
    WORK_DIR = arguments.work_dir.resolve()
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *remaining])
