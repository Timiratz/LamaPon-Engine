"""分離したCLIコマンドを、公開引数・JSON・終了コードから検査する。"""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


class SceneCommandTests(unittest.TestCase):
    # self: シーンCLI検証ケースを初期化
    # シーンとprefabの編集可能な一時入力を用意します。
    def setUp(self):
        # テストごとの一時プロジェクト領域
        self.temporary = tempfile.TemporaryDirectory(prefix="シーン CLI ", dir=WORK_DIR)
        self.addCleanup(self.temporary.cleanup)
        # CLIへ渡す一時プロジェクトルート
        self.root = Path(self.temporary.name)
        self.write(".lamapon/project.json", {"startupScene": "scenes/main.scene"})
        # sceneとprefabで共有する元データ
        self.document = {
            "format": "LamaPonScene", "version": 1, "mainCamera": None,
            "objects": [{"id": 1, "name": "編集前", "parent": None,
                         "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
                         "components": [{"type": "Rigidbody", "mass": 1.0}]}]}
        # シーン文書の初期ファイル
        self.scene = self.write("assets/scenes/main.scene", self.document)
        # prefab文書の初期ファイル
        self.prefab = self.write("assets/prefabs/main.prefab", {
            **self.document, "format": "LamaPonPrefab", "root": 1})

    # write(self: 検証ケース, name: 相対パス, document: JSON文書)
    # JSON文書を一時プロジェクト内へ書き込みます。
    def write(self, name, document):
        # プロジェクトルートからの出力パス
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
        return path

    # cli(self: 検証ケース, arguments: CLI引数, code: 期待終了コード)
    # CLIを実行し、JSON応答と終了状態を検査します。
    def cli(self, *arguments, code=0):
        # CLIのプロセス結果
        process = subprocess.run([str(EXECUTABLE), *map(str, arguments)],
                                 capture_output=True, encoding="utf-8", timeout=30)
        self.assertEqual(process.returncode, code, process.stdout + process.stderr)
        # json.loadsは余分な進行文や2個目のJSONも拒否します。
        report = json.loads(process.stdout)
        self.assertEqual(report["ok"], code == 0, report)
        return report

    # scene_command(self: 検証ケース, command: 操作, arguments: 追加引数, code: 期待終了コード)
    # 共通のproject・scene指定を加えてCLIを呼びます。
    def scene_command(self, command, *arguments, code=0):
        return self.cli(command, "--project", self.root, "--scene", "scenes/main.scene",
                        *arguments, code=code)

    # self: 検証ケース
    # 部品一覧・スキーマ取得と未知型エラーを検査します。
    def test_component_catalog_and_errors(self):
        # UI分類を指定した一覧応答
        report = self.cli("component", "list", "--category", "UI")
        self.assertEqual(report["command"], "component list")
        self.assertEqual(report["count"], len(report["components"]))
        # item: 一覧内の各部品
        self.assertTrue(all(item["category"] == "UI" for item in report["components"]))
        # Rigidbodyの部品スキーマ
        schema = self.cli("component", "schema", "--type", "Rigidbody")["schema"]
        # f: 質量項目のスキーマ定義
        self.assertEqual(next(f for f in schema["fields"] if f["name"] == "mass")["type"], "number")
        # 未知型を指定したエラー応答
        report = self.cli("component", "schema", "--type", "DoesNotExist", code=1)
        self.assertIn("Unknown component schema", report["error"])

    # self: 検証ケース
    # シーン確認・検証と成功／失敗するassertionを検査します。
    def test_scene_inspect_validate_and_failed_assertions(self):
        # inspectコマンドのシーン情報
        report = self.scene_command("inspect")
        self.assertEqual(report["document"]["objects"][0]["name"], "編集前")
        self.assertEqual((report["objectCount"], report["componentCount"]), (1, 1))
        self.assertEqual(self.scene_command("validate")["problems"], [])
        # 成功するobject-count assertionの仕様
        self.write("assertions.json", [{"kind": "object-count", "expected": 1}])
        # assertion実行結果
        report = self.scene_command("test", "--spec", "assertions.json")
        self.assertEqual(report["passed"], 1)
        self.write("assertions.json", [{"kind": "object-count", "expected": 2}])
        self.assertEqual(self.scene_command("test", "--spec", "assertions.json", code=1)["failed"], 1)

    # self: 検証ケース
    # dry-run、commit、型エラー時の原本保持を検査します。
    def test_patch_preview_commit_and_invalid_type_leave_original(self):
        # patch前の原本バイト列
        before = self.scene.read_bytes()
        self.write("operations.json", [{"op": "rename", "target": 1, "name": "編集後"}])
        # dry-runのpatch応答
        preview = self.scene_command("patch", "--operations", "operations.json", "--dry-run")
        self.assertEqual(preview["operationsApplied"], 1)
        self.assertEqual(self.scene.read_bytes(), before)
        self.scene_command("patch", "--operations", "operations.json")
        self.assertEqual(json.loads(self.scene.read_text(encoding="utf-8"))["objects"][0]["name"], "編集後")
        # 正常patch後の原本バイト列
        committed = self.scene.read_bytes()
        self.write("operations.json", [
            {"op": "rename", "target": 1, "name": "保存されない名前"},
            {"op": "set-component", "target": 1, "type": "Rigidbody", "path": "mass", "value": "invalid"}])
        # 型不一致を返すpatch応答
        report = self.scene_command("patch", "--operations", "operations.json", code=1)
        self.assertIn("wrong type", report["error"])
        self.assertEqual(self.scene.read_bytes(), committed)

    # self: 検証ケース
    # prefabの確認・検証・patch操作を検査します。
    def test_prefab_commands(self):
        # CLI共通のprefab引数
        args = ("--project", self.root, "--path", "prefabs/main.prefab")
        self.assertEqual(self.cli("prefab", "inspect", *args)["objectCount"], 1)
        self.cli("prefab", "validate", *args)
        self.write("operations.json", [{"op": "rename", "target": 1, "name": "Prefab編集後"}])
        self.cli("prefab", "patch", *args, "--operations", "operations.json")
        self.assertEqual(json.loads(self.prefab.read_text(encoding="utf-8"))["objects"][0]["name"], "Prefab編集後")

    # self: 検証ケース
    # 循環階層とプロジェクト外出力の拒否を検査します。
    def test_invalid_hierarchy_and_outside_output(self):
        # 自分自身を親とする循環データ
        self.document["objects"][0]["parent"] = 1
        self.write("assets/scenes/main.scene", self.document)
        # 循環階層の検証応答
        report = self.scene_command("validate", code=1)
        # p: 問題一覧内の検証項目
        self.assertIn("parent-cycle", {p["kind"] for p in report["problems"]})
        self.document["objects"][0]["parent"] = None
        self.write("assets/scenes/main.scene", self.document)
        # 外部出力patch前の原本バイト列
        before = self.scene.read_bytes()
        self.write("operations.json", [{"op": "rename", "target": 1, "name": "保存不可"}])
        # プロジェクト外出力を拒否する応答
        report = self.scene_command("patch", "--operations", "operations.json", "--out", "../outside.scene", code=1)
        self.assertIn("inside the project", report["error"])
        self.assertEqual(self.scene.read_bytes(), before)
        self.assertFalse((self.root.parent / "outside.scene").exists())


# 直接実行時だけテストCLIの引数解析を行う
if __name__ == "__main__":
    # CLI引数から検証実行ファイルと作業領域を取得
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    # arguments: 検証用, remaining: unittest用
    arguments, remaining = parser.parse_known_args()
    # テスト対象のCLI実行ファイル
    EXECUTABLE = arguments.executable.resolve()
    # テスト用プロジェクトの親ディレクトリ
    WORK_DIR = arguments.work_dir.resolve()
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *remaining])
