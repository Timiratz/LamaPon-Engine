from __future__ import annotations

import importlib.util
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

# テスト対象のWeb出力スクリプト
TOOL = Path(__file__).resolve().parents[1] / "tools" / "editor_web_export.py"
# インポート用のPythonモジュール仕様
SPEC = importlib.util.spec_from_file_location("editor_web_export", TOOL)
# テスト対象モジュール
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


class EditorWebExportTests(unittest.TestCase):
    def configure_adapter(self):
        script = self.root / "tools" / "ExportWeb.py"
        script.parent.mkdir(parents=True, exist_ok=True)
        script.write_text("# explicit adapter", encoding="utf-8")
        self.project.write_text(json.dumps({"format": "LamaPonProject", "export": {"web": {
            "buildSystem": "project-script", "buildScript": "tools/ExportWeb.py"}}}), encoding="utf-8")
        return script

    def test_project_adapter_is_explicit_and_stays_inside_tools(self):
        script = self.configure_adapter()
        self.assertEqual(EXPORT.project_build_script(self.project), script)
        for name in ("../outside.py", "assets/untrusted.py", str(script.resolve()), "tools/missing.py"):
            document = json.loads(self.project.read_text(encoding="utf-8"))
            document["export"]["web"]["buildScript"] = name
            self.project.write_text(json.dumps(document), encoding="utf-8")
            with self.subTest(name=name), self.assertRaises(ValueError):
                EXPORT.project_build_script(self.project)
        self.project.write_text('{"format":"LamaPonProject"}', encoding="utf-8")
        self.assertIsNone(EXPORT.project_build_script(self.project))

    def test_project_adapter_publishes_verified_output(self):
        script = self.configure_adapter()
        self.previous_output()
        def build(command, **kwargs):
            self.assertEqual(command[4], "-u")
            self.assertEqual(Path(command[5]), script)
            self.assertIn("--engine", command)
            stage = Path(command[command.index("--output") + 1])
            page = stage / "LamaPonWebGL-Game.html"
            content = b"<canvas></canvas>"
            page.write_bytes(content)
            (stage / "web-export-manifest.json").write_text(json.dumps({
                "format": "lamapon.web-export-manifest", "html": page.name,
                "bytes": len(content), "sha256": hashlib.sha256(content).hexdigest(),
                "maximumHtmlBytes": 20_000_000}), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(EXPORT, "build_environment", return_value=(os.environ.copy(), Path("emcmake"))), \
             mock.patch.object(EXPORT.subprocess, "run", side_effect=build):
            result = EXPORT.export(self.project, self.output, self.result, None)
        self.assertTrue(result["ok"])
        self.assertTrue(Path(result["htmlPath"]).is_file())
        self.assertFalse((self.output / "game.html").exists())

    def test_adapter_failed_or_corrupt_output_preserves_old_package(self):
        self.configure_adapter()
        self.previous_output()
        for code in (1, 0):
            def build(command, **kwargs):
                stage = Path(command[command.index("--output") + 1])
                (stage / "LamaPonWebGL-Game.html").write_text("<canvas></canvas>", encoding="utf-8")
                (stage / "web-export-manifest.json").write_text(json.dumps({
                    "format":"lamapon.web-export-manifest", "html":"LamaPonWebGL-Game.html",
                    "bytes": 1, "sha256": "invalid"}), encoding="utf-8")
                return subprocess.CompletedProcess(command, code)
            with self.subTest(code=code), \
                 mock.patch.object(EXPORT, "build_environment", return_value=(os.environ.copy(), Path("emcmake"))), \
                 mock.patch.object(EXPORT.subprocess, "run", side_effect=build), self.assertRaises(ValueError):
                EXPORT.export(self.project, self.output, self.result, None)
            self.assertEqual((self.output / "game.html").read_text(), "previous")

    def test_adapter_package_rejects_path_escape_and_size_overflow(self):
        stage = self.root / "stage"
        stage.mkdir()
        content = b"<canvas></canvas>"
        (stage / "LamaPonWebGL-Game.html").write_bytes(content)
        valid = {"format":"lamapon.web-export-manifest", "html":"LamaPonWebGL-Game.html",
                 "bytes":len(content), "sha256":hashlib.sha256(content).hexdigest()}
        for change in ({"html":"../LamaPonWebGL-Game.html"}, {"maximumHtmlBytes":1},
                       {"maximumHtmlBytes":True}, {"sha256":"bad"}):
            (stage / "web-export-manifest.json").write_text(json.dumps(valid | change), encoding="utf-8")
            with self.subTest(change=change), self.assertRaises(ValueError):
                EXPORT.validate_project_package(stage)

    # self: テストケース
    # UTF-8パスを含む一時プロジェクトを用意します。
    def setUp(self):
        # テスト専用一時領域
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        # 日本語と空白を含むプロジェクトルート
        self.root = Path(self.temporary.name) / "日本語 & project"
        # エクスポーターが要求するプロジェクト設定
        self.project = self.root / ".lamapon" / "project.json"
        self.project.parent.mkdir(parents=True)
        self.project.write_text('{"format":"LamaPonProject"}', encoding="utf-8")
        # 既存パッケージの置換先
        self.output = self.root / "dist" / "Web"
        # エクスポート結果JSONの保存先
        self.result = self.root / ".lamapon" / "result.json"

    # self: テストケース
    # 置換前のWeb成果物一式を作成します。
    def previous_output(self):
        self.output.mkdir(parents=True)
        (self.output / "game.html").write_text("previous", encoding="utf-8")
        (self.output / "web-export-manifest.json").write_text(
            '{"format":"lamapon.web-export-manifest"}', encoding="utf-8")

    # self: テストケース
    # ソース・SDK・既存ファイルを出力先にできないことを検査します。
    def test_protects_sources_sdk_and_unrelated_files(self):
        # エンジンSDKのテスト位置
        engine = Path(self.temporary.name) / "sdk"
        # path: 出力先として拒否すべき既存領域
        for path in (self.root, self.root.parent, self.root / "assets" / "nested",
                     self.project.parent, self.root / ".git" / "exports", engine, engine / "tools"):
            # 各禁止パスが検証エラーになることを確認
            with self.subTest(path=path), self.assertRaises(ValueError):
                EXPORT.validate_output(self.project, path, engine)
        self.output.mkdir(parents=True)
        # 既存データを保護する出力先ファイル
        (self.output / "important.txt").write_text("keep", encoding="utf-8")
        # 既存データがある出力先も拒否
        with self.assertRaises(ValueError):
            EXPORT.validate_output(self.project, self.output, engine)
        self.assertEqual((self.output / "important.txt").read_text(), "keep")

    # self: テストケース
    # emcmake.pyとEM_CONFIGをSDKから解決することを検査します。
    def test_sdk_uses_python_entry_without_shell_activation(self):
        # 空白と日本語を含むEmscripten SDK
        sdk = Path(self.temporary.name) / "SDK & 日本語"
        # Python経由で起動するemcmakeエントリー
        entry = sdk / "upstream" / "emscripten" / "emcmake.py"
        entry.parent.mkdir(parents=True)
        entry.touch()
        (sdk / ".emscripten").touch()
        # cmake以外の実行ファイル探索を抑えて環境を構築
        with mock.patch.object(EXPORT.shutil, "which", return_value="cmake"):
            # env: 子プロセス環境, found: SDK内のemcmake.py
            env, found = EXPORT.build_environment(sdk)
        self.assertTrue(found.samefile(entry))
        self.assertTrue(Path(env["EM_CONFIG"]).samefile(sdk / ".emscripten"))
        self.assertEqual(env["PYTHONUTF8"], "1")

    def make_sdk(self, directory):
        entry = directory / "upstream" / "emscripten" / "emcmake.py"
        entry.parent.mkdir(parents=True)
        entry.touch()
        (directory / ".emscripten").touch()
        return entry

    def test_managed_tools_work_without_path_or_sdk_environment(self):
        sdk = self.root / "managed SDK"
        entry = self.make_sdk(sdk)
        cmake = self.root / "cmake" / "cmake.exe"
        ninja = self.root / "ninja" / "ninja.exe"
        for path in (cmake, ninja):
            path.parent.mkdir(); path.touch()
        manifest = self.root / "LamaPon" / "toolchains" / "web" / "web-support.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps({"format":"lamapon.web-support", "ready":True,
            "emsdk":str(sdk), "cmake":str(cmake), "ninja":str(ninja)}), encoding="utf-8")
        def which(name, **kwargs):
            if name == "cmake":
                self.assertIn(str(cmake.parent), kwargs["path"])
                self.assertIn(str(ninja.parent), kwargs["path"])
                return str(cmake)
            return None
        with mock.patch.dict(os.environ, {"LOCALAPPDATA":str(self.root)}, clear=True), \
             mock.patch.object(EXPORT.shutil, "which", side_effect=which):
            env, found = EXPORT.build_environment(None)
            self.assertEqual(found, entry)
            self.assertEqual(env["EMSDK"], str(sdk))
            self.assertNotIn("PATH", os.environ)

    def test_sdk_auto_detection_without_environment_or_path(self):
        sdk = self.root / "Tools" / "emsdk"
        entry = self.make_sdk(sdk)
        with mock.patch.dict(os.environ, {}, clear=True), \
             mock.patch.object(EXPORT.Path, "home", return_value=self.root), \
             mock.patch.object(EXPORT.shutil, "which", side_effect=lambda name, **kw: "cmake" if name == "cmake" else None):
            env, found = EXPORT.build_environment(None)
        self.assertEqual(found, entry)
        self.assertEqual(Path(env["EMSDK"]), sdk)
        self.assertEqual(Path(env["EM_CONFIG"]), sdk / ".emscripten")

    def test_sdk_auto_detection_from_path_and_config(self):
        sdk = self.root / "custom sdk"
        entry = self.make_sdk(sdk)
        for environment, on_path in (({}, str(entry)), ({"EM_CONFIG":str(sdk / ".emscripten")}, None)):
            with self.subTest(environment=environment), mock.patch.dict(os.environ, environment, clear=True), \
                 mock.patch.object(EXPORT.Path, "home", return_value=self.root), \
                 mock.patch.object(EXPORT.shutil, "which", side_effect=lambda name, **kw: on_path if name == "emcmake" else "cmake"):
                env, found = EXPORT.build_environment(None)
                self.assertEqual(Path(env["EMSDK"]), sdk)
                self.assertEqual(found, entry)

    def test_stale_environment_falls_back_but_explicit_invalid_sdk_is_rejected(self):
        sdk = self.root / "Tools" / "emsdk"
        self.make_sdk(sdk)
        with mock.patch.dict(os.environ, {"EMSDK":str(self.root / "gone")}, clear=True), \
             mock.patch.object(EXPORT.Path, "home", return_value=self.root), \
             mock.patch.object(EXPORT.shutil, "which", return_value=None):
            self.assertEqual(EXPORT.find_emsdk(None), sdk)
            with self.assertRaises(ValueError):
                EXPORT.find_emsdk(self.root / "explicit missing")

    # self: テストケース
    # UTF-8表示に失敗する端末で出力を復旧することを検査します。
    def test_status_output_falls_back_to_utf8(self):
        # UTF-8へ変換後の出力バッファ
        output = io.BytesIO()
        # CP1252端末を模擬するテキストストリーム
        stream = io.TextIOWrapper(output, encoding="cp1252")
        # stdoutを端末エンコーディングへ差し替え
        with mock.patch.object(EXPORT.sys, "stdout", stream):
            EXPORT.print_status("Web出力を開始します。")
        stream.flush()
        self.assertEqual(
            output.getvalue().decode("utf-8").splitlines(),
            ["Web出力を開始します。"],
        )

    # self: テストケース
    # SDK欠落時に説明可能なエラーを返すことを検査します。
    def test_missing_sdk_has_actionable_error(self):
        # SDK不足時の例外契約
        with self.assertRaisesRegex(ValueError, "Emscripten SDK"):
            EXPORT.build_environment(self.root / "missing")

    # self: テストケース
    # ビルド拒否時に既存出力を維持し診断を保存することを検査します。
    def test_rejected_build_keeps_old_output_and_report(self):
        self.previous_output()
        # reject(command: 起動引数, kwargs: subprocess追加オプション)
        def reject(command, **kwargs):
            # 出力引数から取得する一時生成先
            stage = Path(command[command.index("--output") + 1])
            (stage / "web-compatibility-report.json").write_text(json.dumps({
                "findings": [{"level": "reject", "message": "Unsupported native API"}]}), encoding="utf-8")
            return subprocess.CompletedProcess(command, 2)
        # SDK検出と子プロセスを拒否応答へ置き換える
        with mock.patch.object(EXPORT, "build_environment", return_value=(os.environ.copy(), Path("emcmake"))), \
             mock.patch.object(EXPORT.subprocess, "run", side_effect=reject), \
             self.assertRaisesRegex(ValueError, "Unsupported native API"):
            EXPORT.export(self.project, self.output, self.result, None)
        self.assertEqual((self.output / "game.html").read_text(), "previous")
        self.assertTrue((self.result.parent / "web-compatibility-report.json").is_file())
        self.assertFalse(list(self.output.parent.glob(".Web-web-*")))

    # self: テストケース
    # 成功時に成果物を置換し、空白を含むパスを保つことを検査します。
    def test_success_replaces_package_and_keeps_literal_paths(self):
        self.previous_output()
        # build(command: 起動引数, kwargs: subprocess追加オプション)
        def build(command, **kwargs):
            self.assertIn(str(self.project.resolve()), command)
            self.assertNotIn("shell", kwargs)
            # 出力引数から取得する一時生成先
            stage = Path(command[command.index("--output") + 1])
            (stage / "LamaPonWebGL-Game.html").write_text("<canvas></canvas>", encoding="utf-8")
            (stage / "web-export-manifest.json").write_text(json.dumps({
                "format": "lamapon.web-export-manifest", "target": {"singleFile": True}}), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        # SDK検出と子プロセスを成功応答へ置き換える
        with mock.patch.object(EXPORT, "build_environment", return_value=(os.environ.copy(), Path("emcmake"))), \
             mock.patch.object(EXPORT.subprocess, "run", side_effect=build):
            # Web出力の正常応答
            result = EXPORT.export(self.project, self.output, self.result, None)
        self.assertTrue(result["ok"] and result["singleFile"])
        self.assertTrue(Path(result["htmlPath"]).is_file())
        self.assertFalse((self.output / "game.html").exists())
        self.assertFalse(list(self.output.parent.glob("Web.previous-*")))

    # self: テストケース
    # 成果物公開失敗時に置換前パッケージを復元することを検査します。
    def test_publish_failure_restores_previous_package(self):
        self.previous_output()
        # 公開元となるstageディレクトリ
        stage = self.output.parent / "stage"
        stage.mkdir()
        # 元のPath.rename実装
        rename = Path.rename
        # fail_stage(path: 失敗させる元パス, target: 置換先)
        def fail_stage(path, target):
            # stage公開だけを失敗させる
            if path == stage:
                raise OSError("publication failed")
            return rename(path, target)
        # rename失敗を注入して復旧動作を確認
        with mock.patch.object(Path, "rename", fail_stage), self.assertRaises(OSError):
            EXPORT.publish_package(stage, self.output)
        self.assertEqual((self.output / "game.html").read_text(), "previous")


# 直接実行時だけ回帰テストを開始
if __name__ == "__main__":
    unittest.main()
