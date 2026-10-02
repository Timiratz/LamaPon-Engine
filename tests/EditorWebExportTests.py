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

# テスト対象のWeb出力スクリプト
TOOL = Path(__file__).resolve().parents[1] / "tools" / "editor_web_export.py"
# インポート用のPythonモジュール仕様
SPEC = importlib.util.spec_from_file_location("editor_web_export", TOOL)
# テスト対象モジュール
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


class EditorWebExportTests(unittest.TestCase):
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
