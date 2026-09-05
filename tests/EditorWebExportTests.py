from __future__ import annotations

import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

TOOL = Path(__file__).resolve().parents[1] / "tools" / "editor_web_export.py"
SPEC = importlib.util.spec_from_file_location("editor_web_export", TOOL)
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


class EditorWebExportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "日本語 & project"
        self.project = self.root / ".lamapon" / "project.json"
        self.project.parent.mkdir(parents=True)
        self.project.write_text('{"format":"LamaPonProject"}', encoding="utf-8")
        self.output = self.root / "dist" / "Web"
        self.result = self.root / ".lamapon" / "result.json"

    def previous_output(self):
        self.output.mkdir(parents=True)
        (self.output / "game.html").write_text("previous", encoding="utf-8")
        (self.output / "web-export-manifest.json").write_text(
            '{"format":"lamapon.web-export-manifest"}', encoding="utf-8")

    def test_protects_sources_sdk_and_unrelated_files(self):
        engine = Path(self.temporary.name) / "sdk"
        for path in (self.root, self.root.parent, self.root / "assets" / "nested",
                     self.project.parent, self.root / ".git" / "exports", engine, engine / "tools"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                EXPORT.validate_output(self.project, path, engine)
        self.output.mkdir(parents=True)
        (self.output / "important.txt").write_text("keep", encoding="utf-8")
        with self.assertRaises(ValueError):
            EXPORT.validate_output(self.project, self.output, engine)
        self.assertEqual((self.output / "important.txt").read_text(), "keep")

    def test_sdk_uses_python_entry_without_shell_activation(self):
        sdk = Path(self.temporary.name) / "SDK & 日本語"
        entry = sdk / "upstream" / "emscripten" / "emcmake.py"
        entry.parent.mkdir(parents=True)
        entry.touch()
        (sdk / ".emscripten").touch()
        with mock.patch.object(EXPORT.shutil, "which", return_value="cmake"):
            env, found = EXPORT.build_environment(sdk)
        self.assertTrue(found.samefile(entry))
        self.assertTrue(Path(env["EM_CONFIG"]).samefile(sdk / ".emscripten"))
        self.assertEqual(env["PYTHONUTF8"], "1")

    def test_status_output_falls_back_to_utf8(self):
        output = io.BytesIO()
        stream = io.TextIOWrapper(output, encoding="cp1252")
        with mock.patch.object(EXPORT.sys, "stdout", stream):
            EXPORT.print_status("Web出力を開始します。")
        stream.flush()
        self.assertEqual(
            output.getvalue().decode("utf-8").splitlines(),
            ["Web出力を開始します。"],
        )

    def test_missing_sdk_has_actionable_error(self):
        with self.assertRaisesRegex(ValueError, "Emscripten SDK"):
            EXPORT.build_environment(self.root / "missing")

    def test_rejected_build_keeps_old_output_and_report(self):
        self.previous_output()
        def reject(command, **kwargs):
            stage = Path(command[command.index("--output") + 1])
            (stage / "web-compatibility-report.json").write_text(json.dumps({
                "findings": [{"level": "reject", "message": "Unsupported native API"}]}), encoding="utf-8")
            return subprocess.CompletedProcess(command, 2)
        with mock.patch.object(EXPORT, "build_environment", return_value=(os.environ.copy(), Path("emcmake"))), \
             mock.patch.object(EXPORT.subprocess, "run", side_effect=reject), \
             self.assertRaisesRegex(ValueError, "Unsupported native API"):
            EXPORT.export(self.project, self.output, self.result, None)
        self.assertEqual((self.output / "game.html").read_text(), "previous")
        self.assertTrue((self.result.parent / "web-compatibility-report.json").is_file())
        self.assertFalse(list(self.output.parent.glob(".Web-web-*")))

    def test_success_replaces_package_and_keeps_literal_paths(self):
        self.previous_output()
        def build(command, **kwargs):
            self.assertIn(str(self.project.resolve()), command)
            self.assertNotIn("shell", kwargs)
            stage = Path(command[command.index("--output") + 1])
            (stage / "LamaPonWebGL-Game.html").write_text("<canvas></canvas>", encoding="utf-8")
            (stage / "web-export-manifest.json").write_text(json.dumps({
                "format": "lamapon.web-export-manifest", "target": {"singleFile": True}}), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(EXPORT, "build_environment", return_value=(os.environ.copy(), Path("emcmake"))), \
             mock.patch.object(EXPORT.subprocess, "run", side_effect=build):
            result = EXPORT.export(self.project, self.output, self.result, None)
        self.assertTrue(result["ok"] and result["singleFile"])
        self.assertTrue(Path(result["htmlPath"]).is_file())
        self.assertFalse((self.output / "game.html").exists())
        self.assertFalse(list(self.output.parent.glob("Web.previous-*")))

    def test_publish_failure_restores_previous_package(self):
        self.previous_output()
        stage = self.output.parent / "stage"
        stage.mkdir()
        rename = Path.rename
        def fail_stage(path, target):
            if path == stage:
                raise OSError("publication failed")
            return rename(path, target)
        with mock.patch.object(Path, "rename", fail_stage), self.assertRaises(OSError):
            EXPORT.publish_package(stage, self.output)
        self.assertEqual((self.output / "game.html").read_text(), "previous")


if __name__ == "__main__":
    unittest.main()
