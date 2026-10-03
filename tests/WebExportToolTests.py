from __future__ import annotations

import importlib.util
import html
import json
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


# TOOL_PATH はWeb出力ツールのパス。
TOOL_PATH = Path(__file__).resolve().parents[1] / "tools" / "export_web.py"
# SPEC はツールの読込仕様。
SPEC = importlib.util.spec_from_file_location("lamapon_export_web", TOOL_PATH)
# ツール読込に必要な仕様を確認する。
assert SPEC is not None and SPEC.loader is not None
# EXPORT_WEB は読込済みWeb出力ツール。
EXPORT_WEB = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT_WEB)


# Web出力ツールの互換性と素材変換を検証する。
class WebExportToolTests(unittest.TestCase):
    # setUp(self: テストケース): Windowsでも擬似変換器を起動できるよう実行を差し替える。
    def setUp(self):
        # run は元のサブプロセス実行関数。
        run = subprocess.run

        # run_converter(command: コマンド, args: 追加引数, kwargs: 実行設定): 擬似変換器はPythonで起動し、それ以外は元の実行関数へ渡す。
        def run_converter(command, *args, **kwargs):
            # 擬似変換器だけPython経由で起動する。
            if (isinstance(command, list) and command
                    and Path(command[0]).name in {"fake-magick", "fake-ffmpeg"}):
                # command は実行するコマンド。
                command = [sys.executable, *command]
            # 元のサブプロセス実行関数へ処理を委譲する。
            return run(command, *args, **kwargs)

        # patcher は実行差し替えパッチ。
        patcher = mock.patch.object(EXPORT_WEB.subprocess, "run", side_effect=run_converter)
        patcher.start()
        self.addCleanup(patcher.stop)

    # _write_glb(path: 保存先, document: glTF文書, binary: BINデータ): JSONとBINをパディングしてGLBを保存する。
    @staticmethod
    def _write_glb(path: Path, document: dict, binary: bytes = b"") -> None:
        # encoded はGLBのJSONチャンク。
        encoded = json.dumps(document, separators=(",", ":")).encode("utf-8")
        encoded += b" " * ((4 - len(encoded) % 4) % 4)
        # chunks はGLBチャンク一覧。
        chunks = [(0x4E4F534A, encoded)]
        # BINチャンクがある場合だけGLBへ追加する。
        if binary:
            binary += b"\x00" * ((4 - len(binary) % 4) % 4)
            chunks.append((0x004E4942, binary))
        # total はGLB全体のバイト数（payload は内容、_ は種別の読み捨て）。
        total = 12 + sum(8 + len(payload) for _, payload in chunks)
        # result は生成するGLBバイト列。
        result = bytearray(struct.pack("<4sII", b"glTF", 2, total))
        # chunk_type（種別）と payload（内容）をGLBへ追加する。
        for chunk_type, payload in chunks:
            result.extend(struct.pack("<II", len(payload), chunk_type))
            result.extend(payload)
        path.write_bytes(result)

    # _portable_fixture(self: テストケース, root: 一時プロジェクト, source_text: C++コード, scene: シーンJSON, modules: 使用モジュール, asset_include_paths: 対象アセット): 最小のWeb互換プロジェクトを作る。
    def _portable_fixture(
        self,
        root: Path,
        source_text: str = '#include "LamaPon/LamaPon.h"\n',
        scene: str | None = None,
        modules: list[str] | None = None,
        asset_include_paths: list[str] | None = None,
    ):
        (root / "assets" / "scripts").mkdir(parents=True)
        (root / "assets" / "scenes").mkdir(parents=True)
        (root / "assets" / "scripts" / "Game.cpp").write_text(
            source_text,
            encoding="utf-8",
        )
        # シーン省略時は既定シーンを使う。
        if scene is None:
            # scene はWeb検証用シーン定義。
            scene = (
                '{"format":"LamaPonScene","mainCamera":1,"objects":['
                '{"id":1,"name":"Camera","parent":null,"components":['
                '{"type":"Camera","enabled":true}],"enabled":true}]}'
            )
        (root / "assets" / "scenes" / "Main.scene.json").write_text(
            scene + "\n",
            encoding="utf-8",
        )
        # Web検証用の最小プロジェクト設定を返す。
        return {
            "name": "PortableGame",
            "projectName": "PortableGame",
            "export": {
                "targets": ["web"],
                "modules": modules or ["core", "input", "renderer3d"],
                "web": {
                    "buildSystem": "lamapon",
                    "portableGame": True,
                    "sources": ["assets/scripts/Game.cpp"],
                    "assetDirectory": "assets",
                    "assetIncludePaths": asset_include_paths or ["scenes"],
                    "scenePath": "/assets/scenes/Main.scene.json",
                },
            },
        }

    # test_web_asset_conversion_catalog_is_complete(self: テストケース): Web変換対象の画像・音声・モデル形式を照合する。
    def test_web_asset_conversion_catalog_is_complete(self):
        # expected_images は対応画像形式一覧。
        expected_images = {
            ".bmp", ".dds", ".gif", ".jpeg", ".jpg", ".png",
            ".tga", ".tif", ".tiff",
        }
        # expected_audio は対応音声形式一覧。
        expected_audio = {
            ".aac", ".flac", ".m4a", ".mp3", ".ogg", ".wma",
        }
        # expected_models は対応モデル形式一覧。
        expected_models = {
            ".3ds", ".3mf", ".ac", ".blend", ".dae", ".dxf",
            ".fbx", ".glb", ".gltf", ".ifc", ".lwo", ".md2",
            ".md3", ".md5mesh", ".ms3d", ".obj", ".off", ".ogex",
            ".ply", ".pmx", ".smd", ".step", ".stl", ".stp",
            ".vrm", ".x", ".x3d",
        }
        # actual は実装済み変換形式一覧。
        actual = EXPORT_WEB.WEB_ASSET_CONVERSIONS
        # extension（形式）と rule（変換規則）で画像対象を照合する。
        self.assertEqual(
            {extension for extension, rule in actual.items()
             if rule == ("image", "webp")},
            expected_images,
        )
        # extension（形式）と rule（変換規則）で音声対象を照合する。
        self.assertEqual(
            {extension for extension, rule in actual.items()
             if rule == ("audio", "wav")},
            expected_audio,
        )
        # extension（形式）と rule（変換規則）でモデル対象を照合する。
        self.assertEqual(
            {extension for extension, rule in actual.items()
             if rule == ("model", "glb")},
            expected_models,
        )

    # test_single_file_package_verification_accepts_expanded_runtime(self: テストケース): 自己完結したHTMLを単一ファイル出力として受け入れる。
    def test_single_file_package_verification_accepts_expanded_runtime(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # html は検証対象HTML。
            html = Path(directory) / "Game.html"
            html.write_text(
                "<!doctype html><canvas></canvas><script>start()</script>",
                encoding="utf-8",
            )

            # checks は成果物検証結果。
            checks = EXPORT_WEB.verify_web_artifacts([html], True)

            # item は成果物検証項目。規定の検証コード一覧と照合する。
            self.assertEqual(
                [item["code"] for item in checks],
                [
                    "non-empty-artifacts",
                    "html-entrypoint",
                    "self-contained-html",
                ],
            )

    # test_every_native_scene_component_is_classified_for_web(self: テストケース): 全ネイティブコンポーネントがWeb分類済みか確認する。
    def test_every_native_scene_component_is_classified_for_web(self):
        # native_components はネイティブ要素名一覧。
        native_components: set[str] = set()
        # component_root は要素ヘッダーの配置先。
        component_root = TOOL_PATH.parents[1] / "src" / "LamaPon" / "Components"
        # header（定義ファイル）からネイティブ要素名を収集する。
        for header in component_root.glob("*Component.h"):
            native_components.update(re.findall(
                r'return\s+"([A-Za-z0-9_]+)"',
                header.read_text(encoding="utf-8"),
            ))

        self.assertEqual(
            native_components,
            EXPORT_WEB.KNOWN_NATIVE_SCENE_COMPONENTS,
        )
        self.assertLessEqual(
            set(EXPORT_WEB.PORTABLE_SCENE_COMPONENTS),
            native_components,
        )

    # test_package_verification_rejects_unexpanded_shell(self: テストケース): 未展開テンプレートを含むHTMLを拒否する。
    def test_package_verification_rejects_unexpanded_shell(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # html は検証対象HTML。
            html = Path(directory) / "Game.html"
            html.write_text(
                "<canvas></canvas><script></script>{{{ SCRIPT }}}",
                encoding="utf-8",
            )

            # 不正な入力が出力処理で拒否されることを確認する。
            with self.assertRaises(EXPORT_WEB.ExportError):
                EXPORT_WEB.verify_web_artifacts([html], True)

    # test_package_verification_enforces_standard_output_name(self: テストケース): 規定外のHTML名を持つ出力を拒否する。
    def test_package_verification_enforces_standard_output_name(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # html は検証対象HTML。
            html = Path(directory) / "WrongName.html"
            html.write_text(
                "<!doctype html><canvas></canvas><script>start()</script>",
                encoding="utf-8",
            )

            # 不正な入力が出力処理で拒否されることを確認する。
            with self.assertRaises(EXPORT_WEB.ExportError):
                EXPORT_WEB.verify_web_artifacts(
                    [html],
                    True,
                    "LamaPonWebGL-Game",
                )

    # test_compatibility_report_contains_strict_summary(self: テストケース): 互換性レポートの厳格契約と診断集計を確認する。
    def test_compatibility_report_contains_strict_summary(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # output はWeb出力先。
            output = Path(directory)
            # project はWeb出力用プロジェクト設定。
            project = {
                "name": "PortableGame",
                "export": {"web": {"portableGame": True}},
            }
            # findings は互換性診断結果。
            findings = [
                EXPORT_WEB.finding("auto", "converted", "done", "none"),
                EXPORT_WEB.finding("warning", "visual", "compare", "preview"),
            ]

            # report は互換性レポートのパス。
            report = EXPORT_WEB.write_compatibility_report(
                output,
                project,
                "lamapon-web-target",
                "webgl2-basic-3d",
                "ready",
                findings,
            )
            # payload は互換性レポートのJSON本文。
            payload = json.loads(report.read_text(encoding="utf-8"))

            self.assertEqual(payload["version"], 2)
            self.assertTrue(payload["strictPortableContract"])
            self.assertEqual(
                payload["summary"],
                {"auto": 1, "info": 0, "warning": 1, "reject": 0},
            )

    # test_generated_target_uses_declared_sources_and_modules(self: テストケース): 生成CMakeターゲットが指定ソースとモジュールを使うか確認する。
    def test_generated_target_uses_declared_sources_and_modules(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            (root / "main.cpp").write_text("int main() { return 0; }\n")
            # project はWeb出力用プロジェクト設定。
            project = {
                "name": "GeneratedGame",
                "export": {
                    "targets": ["web"],
                    "modules": ["core", "input"],
                    "web": {
                        "buildSystem": "lamapon",
                        "sources": ["main.cpp"],
                        "singleFile": True,
                    },
                },
            }

            # generated は生成された出力。
            generated = EXPORT_WEB.generate_lamapon_web_target(
                root,
                project,
                "GeneratedGame",
                ["core", "input"],
                "webgl2-basic-2d",
                root / ".lamapon" / "generated",
            )
            # cmake は生成CMake設定。
            cmake = (generated / "CMakeLists.txt").read_text(encoding="utf-8")

            self.assertIn("lamapon_add_web_game(GeneratedGame", cmake)
            self.assertIn((root / "main.cpp").resolve().as_posix(), cmake)
            self.assertIn("        core", cmake)
            self.assertIn("        input", cmake)
            self.assertIn("SINGLE_FILE", cmake)
            self.assertNotIn("ASSET_DIRECTORY", cmake)
            self.assertIn(
                "OUTPUT_NAME [==[LamaPonWebGL-GeneratedGame]==]",
                cmake,
            )

    # test_normal_lamapon_project_gets_generated_portable_target(self: テストケース): 通常のLamaPonプロジェクトから移植用Webターゲットを生成する。
    def test_normal_lamapon_project_gets_generated_portable_target(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # project_path はプロジェクト設定ファイル。
            project_path = root / ".lamapon" / "project.json"
            project_path.parent.mkdir()
            # scripts はゲームコードの配置先。
            scripts = root / "assets" / "scripts"
            # scenes はシーン素材の配置先。
            scenes = root / "assets" / "scenes"
            # textures は画像素材の配置先。
            textures = root / "assets" / "textures"
            scripts.mkdir(parents=True)
            scenes.mkdir(parents=True)
            textures.mkdir(parents=True)
            (scripts / "Game.cpp").write_text(
                '#include "LamaPon/LamaPon.h"\n'
                "LamaPon::SpriteRendererComponent* sprite = nullptr;\n",
                encoding="utf-8",
            )
            (scenes / "Main.scene.json").write_text(
                '{"format":"LamaPonScene","objects":[]}',
                encoding="utf-8",
            )
            # source_document は元プロジェクト文書。
            source_document = {
                "format": "LamaPonProject",
                "gameName": "Beginner Game",
                "startupScene": "scenes/Main.scene.json",
            }
            project_path.write_text(
                json.dumps(source_document),
                encoding="utf-8",
            )
            # _ は使用しない戻り値、project はWeb出力用プロジェクト設定。
            _, project = EXPORT_WEB.load_project(project_path)
            # project_root は解決済みプロジェクトルート。
            project_root = EXPORT_WEB.lamapon_project_root(project_path, project)

            # profile は互換性プロファイル、project_kind はプロジェクト方式、renderer はWeb描画器名、source はWeb出力元のパス、target は生成ターゲット名。
            source, target, renderer, profile, project_kind = (
                EXPORT_WEB.require_web_configuration(
                    project_path,
                    project_root,
                    project,
                )
            )

            self.assertEqual(source, root.resolve())
            self.assertEqual(target, f"LamaPonWeb_{root.name}")
            self.assertEqual(renderer, "webgl2")
            self.assertEqual(profile, "webgl2-basic-2d")
            self.assertEqual(project_kind, "lamapon-project")
            # web はWeb出力設定。
            web = project["export"]["web"]
            self.assertTrue(web["portableGame"])
            self.assertTrue(web["singleFile"])
            self.assertEqual(web["sources"], ["assets/scripts/Game.cpp"])
            self.assertEqual(
                web["scenePath"],
                "/assets/scenes/Main.scene.json",
            )

            self.assertEqual(
                web["assetIncludePaths"],
                ["scenes", "textures"],
            )
            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                source,
                project,
                profile,
                project_kind,
            )
            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )
            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertIn(
                "generated-web-runtime-target",
                {item["code"] for item in findings},
            )
            self.assertEqual(
                json.loads(project_path.read_text(encoding="utf-8")),
                source_document,
            )

    # test_lamapon_web_export_validates_online_settings_before_output(self: テストケース): オンライン設定の不備でWeb出力前に失敗するか確認する。
    def test_lamapon_web_export_validates_online_settings_before_output(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # project_path はプロジェクト設定ファイル。
            project_path = root / ".lamapon" / "project.json"
            project_path.parent.mkdir()
            # valid は有効なオンライン設定。
            valid = {
                "enabled": True,
                "serviceBaseUrl": "https://online.example.test",
                "gameId": "com.example.web-test",
                "environmentId": "production",
                "allowInsecureLoopback": False,
                "openAuthorizationBrowser": True,
            }
            # invalid_cases は不正設定のテスト一覧。
            invalid_cases = [
                ("not-object", None),
                ("enabled-type", {**valid, "enabled": 1}),
                ("url-type", {**valid, "serviceBaseUrl": False}),
                ("game-type", {**valid, "gameId": 7}),
                ("environment-type", {**valid, "environmentId": []}),
                ("insecure-type", {
                    **valid, "allowInsecureLoopback": "true",
                }),
                ("browser-type", {
                    **valid, "openAuthorizationBrowser": 1,
                }),
                ("missing-url", {**valid, "serviceBaseUrl": ""}),
                ("missing-game", {**valid, "gameId": ""}),
                ("missing-environment", {
                    **valid, "environmentId": "",
                }),
                ("http-remote", {
                    **valid,
                    "serviceBaseUrl": "http://example.test",
                }),
                ("http-loopback", {
                    **valid,
                    "serviceBaseUrl": "http://localhost:8080",
                    "allowInsecureLoopback": True,
                }),
                ("https-development-switch", {
                    **valid,
                    "allowInsecureLoopback": True,
                }),
                ("disabled-http-loopback", {
                    **valid,
                    "enabled": False,
                    "serviceBaseUrl": "http://127.0.0.1:8080",
                    "allowInsecureLoopback": True,
                }),
                ("url-userinfo", {
                    **valid,
                    "serviceBaseUrl": "https://user@example.test",
                }),
                ("url-query", {
                    **valid,
                    "serviceBaseUrl": "https://example.test/api?mode=1",
                }),
                ("url-fragment", {
                    **valid,
                    "serviceBaseUrl": "https://example.test/api#part",
                }),
                ("url-control", {
                    **valid,
                    "serviceBaseUrl": "https://example.test/\rheader",
                }),
                ("url-empty-authority", {
                    **valid,
                    "serviceBaseUrl": "https:///api",
                }),
                ("url-too-long", {
                    **valid,
                    "serviceBaseUrl": "https://" + "a" * 2041,
                }),
                ("game-characters", {**valid, "gameId": "bad/game"}),
                ("game-non-ascii", {**valid, "gameId": "ゲーム"}),
                ("game-too-long", {**valid, "gameId": "g" * 129}),
                ("environment-characters", {
                    **valid, "environmentId": "bad environment",
                }),
                ("environment-too-long", {
                    **valid, "environmentId": "e" * 65,
                }),
            ]

            # name（ケース名）と online（設定値）を使い各入力を検証する。
            for name, online in invalid_cases:
                # 各設定不備を独立したケースとして実行する。
                with self.subTest(name=name):
                    project_path.write_text(
                        json.dumps({
                            "format": "LamaPonProject",
                            "online": online,
                        }),
                        encoding="utf-8",
                    )
                    # output はWeb出力先。
                    output = root / "outputs" / name
                    # Web出力引数をケースごとに差し替える。
                    with mock.patch.object(sys, "argv", [
                        str(TOOL_PATH),
                        str(project_path),
                        "--output",
                        str(output),
                        "--dry-run",
                    ]):
                        # 不正な入力が出力処理で拒否されることを確認する。
                        with self.assertRaises(EXPORT_WEB.ExportError):
                            EXPORT_WEB.main()
                    self.assertFalse(
                        output.exists(),
                        "invalid online settings created Web output",
                    )

    # test_lamapon_web_online_settings_drop_secrets_and_normalize_url(self: テストケース): 秘密値を除外しURLを正規化する。
    def test_lamapon_web_online_settings_drop_secrets_and_normalize_url(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # project_path はプロジェクト設定ファイル。
            project_path = Path(directory) / ".lamapon" / "project.json"
            project_path.parent.mkdir()
            # online はケースごとのオンライン設定。
            online = {
                "enabled": True,
                "serviceBaseUrl": "https://online.example.test/api///",
                "gameId": "com.example.web-test",
                "environmentId": "staging_2",
                "allowInsecureLoopback": False,
                "openAuthorizationBrowser": False,
                "client_secret": "must-not-ship",
                "accessToken": "must-not-ship",
                "refreshToken": "must-not-ship",
                # Rich PresenceはWindows専用のためWeb設定から除外する。
                "discordPresence": {
                    "enabled": True,
                    "applicationId": "123456789012345678",
                },
            }
            project_path.write_text(
                json.dumps({
                    "format": "LamaPonProject",
                    "online": online,
                }),
                encoding="utf-8",
            )

            # _ は使用しない戻り値、loaded は読込済み設定。
            _, loaded = EXPORT_WEB.load_project(project_path)

            self.assertEqual(
                set(loaded["online"]),
                {
                    "enabled",
                    "serviceBaseUrl",
                    "gameId",
                    "environmentId",
                    "allowInsecureLoopback",
                    "openAuthorizationBrowser",
                },
            )
            self.assertEqual(
                loaded["online"]["serviceBaseUrl"],
                "https://online.example.test/api",
            )

    # test_portable_target_compiles_original_sources_and_stages_assets(self: テストケース): 元ソースを使うターゲットとステージ済みシーンを確認する。
    def test_portable_target_compiles_original_sources_and_stages_assets(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            (root / "assets" / "scripts").mkdir(parents=True)
            (root / "assets" / "scenes").mkdir(parents=True)
            (root / "assets" / "scripts" / "Game.cpp").write_text(
                '#include "LamaPon/LamaPon.h"\n'
                "DirectX::XMFLOAT3 position{};\n",
                encoding="utf-8",
            )
            (root / "assets" / "scenes" / "Main.scene.json").write_text(
                '{"format":"LamaPonScene","mainCamera":1,"objects":['
                '{"id":1,"name":"Camera","parent":null,"components":['
                '{"type":"Camera","enabled":true}],"enabled":true}]}'
                "\n",
                encoding="utf-8",
            )
            # project はWeb出力用プロジェクト設定。
            project = {
                "name": "PortableGame",
                "projectName": "SampleGame",
                "gameName": "Portable Game",
                "export": {
                    "targets": ["web"],
                    "modules": ["core", "input", "renderer3d", "particles3d"],
                    "web": {
                        "buildSystem": "lamapon",
                        "portableGame": True,
                        "sources": ["assets/scripts/Game.cpp"],
                        "assetDirectory": "assets",
                        "assetIncludePaths": ["scenes"],
                        "scenePath": "/assets/scenes/Main.scene.json",
                    },
                },
            }

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root,
                project,
                "webgl2-basic-3d",
                "lamapon-web-target",
            )
            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )

            # generated は生成された出力。
            generated = EXPORT_WEB.generate_lamapon_web_target(
                root,
                project,
                "PortableGame",
                ["core", "input", "renderer3d", "particles3d"],
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )
            # cmake は生成CMake設定。
            cmake = (generated / "CMakeLists.txt").read_text(encoding="utf-8")
            # staged_scene は生成済みシーンのパス。
            staged_scene = root / ".lamapon" / "web-generated-assets" / (
                "scenes/Main.scene.json"
            )

            self.assertIn("PORTABLE_GAME", cmake)
            self.assertIn(
                "OUTPUT_NAME [==[LamaPonWebGL-SampleGame]==]", cmake
            )
            self.assertIn("GAME_NAME [==[Portable Game]==]", cmake)
            self.assertIn(
                "SCENE_PATH [==[/assets/scenes/Main.scene.json]==]", cmake
            )
            self.assertIn("        particles3d", cmake)
            self.assertTrue(staged_scene.is_file())

    # test_web_artifact_name_is_forced_from_project_name(self: テストケース): Web成果物名がプロジェクト名から決まることを確認する。
    def test_web_artifact_name_is_forced_from_project_name(self):
        # project はWeb出力用プロジェクト設定。
        project = {
            "name": "BuildTarget",
            "projectName": "ドライブゲーム",
            "gameName": "Display Title",
            "export": {
                "web": {
                    "artifactPrefix": "IgnoredCustomName",
                }
            },
        }

        # output_name はoutput_nameの値。
        output_name = EXPORT_WEB.web_artifact_prefix(
            Path("/tmp/source"),
            project,
            "lamapon-web-target",
        )

        self.assertEqual(output_name, "LamaPonWebGL-ドライブゲーム")

    # test_web_project_name_rejects_filename_separators(self: テストケース): ファイル名区切りを含むプロジェクト名を拒否する。
    def test_web_project_name_rejects_filename_separators(self):
        # 不正な入力が出力処理で拒否されることを確認する。
        with self.assertRaises(EXPORT_WEB.ExportError):
            EXPORT_WEB.web_artifact_prefix(
                Path("/tmp/source"),
                {"projectName": "Folder/Game"},
                "lamapon-web-target",
            )

    # test_strict_check_rejects_unknown_api_and_missing_module(self: テストケース): 未対応APIと必須モジュール不足を診断する。
    def test_strict_check_rejects_unknown_api_and_missing_module(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                '#include "LamaPon/LamaPon.h"\n'
                "LamaPon::AudioSourceComponent* audio{};\n"
                "LamaPon::FutureRendererComponent* future{};\n",
            )

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            # item は互換性診断、codes は診断コード一覧。
            codes = {item["code"] for item in findings}

            self.assertIn("missing-required-module", codes)
            self.assertIn("unsupported-portable-api", codes)

    # test_strict_check_rejects_scene_contract_failures(self: テストケース): 欠落素材や循環階層などのシーン違反を診断する。
    def test_strict_check_rejects_scene_contract_failures(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # scene はWeb検証用シーン定義。
            scene = (
                '{"format":"LamaPonScene","mainCamera":1,"objects":['
                '{"id":1,"name":"Camera","parent":2,"components":['
                '{"type":"Camera","enabled":true},'
                '{"type":"NativeScript","script":"Game.Missing"}],'
                '"enabled":true},'
                '{"id":2,"name":"Unknown","parent":1,"components":['
                '{"type":"FutureComponent","texture":"textures/missing.png"}]'
                ',"enabled":true}]}'
            )
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(root, scene=scene)

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            # item は互換性診断、codes は診断コード一覧。
            codes = {item["code"] for item in findings}

            self.assertIn("missing-scene-asset", codes)
            self.assertIn("unsupported-scene-component", codes)
            self.assertIn("unregistered-scene-script", codes)
            self.assertIn("cyclic-scene-hierarchy", codes)

    # test_strict_check_accepts_ui_rect_scene_component(self: テストケース): 対応済みUIシーン要素を受け入れる。
    def test_strict_check_accepts_ui_rect_scene_component(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # scene はWeb検証用シーン定義。
            scene = json.dumps({
                "format": "LamaPonScene",
                "mainCamera": 1,
                "objects": [{
                    "id": 1,
                    "name": "Camera",
                    "parent": None,
                    "enabled": True,
                    "components": [{"type": "Camera", "enabled": True}],
                }, {
                    "id": 2,
                    "name": "HUD Label",
                    "parent": None,
                    "enabled": True,
                    "components": [{
                        "type": "UIRectTransform",
                        "enabled": True,
                        "anchorMin": [0.5, 0.0],
                        "anchorMax": [0.5, 0.0],
                        "pivot": [0.5, 0.0],
                        "anchoredPosition": [0.0, 24.0],
                        "sizeDelta": [320.0, 64.0],
                    }, {
                        "type": "TextRenderer",
                        "enabled": True,
                        "text": "READY",
                    }],
                }],
            })
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer2d", "renderer3d"],
            )

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )

    # test_strict_check_rejects_corrupt_selected_asset(self: テストケース): 壊れた選択済みPNGを診断する。
    def test_strict_check_rejects_corrupt_selected_asset(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                asset_include_paths=["scenes", "textures"],
            )
            (root / "assets" / "textures").mkdir()
            (root / "assets" / "textures" / "broken.png").write_bytes(
                b"not a png"
            )

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertIn(
                "corrupt-png-asset",
                {item["code"] for item in findings},
            )

    # test_strict_check_rejects_unknown_input_action(self: テストケース): 未登録の入力アクションを拒否する。
    def test_strict_check_rejects_unknown_input_action(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                '#include "LamaPon/LamaPon.h"\n'
                'float value = input.Value("Teleport");\n',
            )

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertIn(
                "unsupported-input-action",
                {item["code"] for item in findings},
            )

    # test_project_input_action_is_generated_without_engine_edit(self: テストケース): プロジェクト設定から入力マップを生成して再読込する。
    def test_project_input_action_is_generated_without_engine_edit(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                '#include "LamaPon/LamaPon.h"\n'
                'bool toggle = input.WasPressed("ToggleView");\n',
            )
            (root / ".lamapon").mkdir()
            (root / ".lamapon" / "project.json").write_text(json.dumps({
                "inputActions": [{
                    "name": "ToggleView",
                    "bindings": [
                        {"control": "KeyboardC", "scale": 1.0},
                        {"control": "GamePadY", "scale": 1.0},
                    ],
                }],
            }), encoding="utf-8")

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertNotIn(
                "unsupported-input-action",
                {item["code"] for item in findings},
            )
            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertIn(
                "project-input-action-map",
                {item["code"] for item in findings},
            )

            # staged は素材のステージ先。
            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                project["export"]["web"],
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )
            # input_map は生成された入力マップ。
            input_map = json.loads(
                (staged / "lamapon-input-actions.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                input_map["actions"]["ToggleView"][1]["control"],
                "GamePadY",
            )

            # 出力時に設定を再読込し、最新の入力割当を反映する。
            (root / ".lamapon" / "project.json").write_text(json.dumps({
                "inputActions": [{
                    "name": "ToggleView",
                    "bindings": [
                        {"control": "KeyboardV", "scale": 1.0},
                        {"control": "GamePadX", "scale": 1.0},
                    ],
                }],
            }), encoding="utf-8")
            # staged は素材のステージ先。
            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                project["export"]["web"],
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )
            # refreshed_input_map は再読込後の入力マップ。
            refreshed_input_map = json.loads(
                (staged / "lamapon-input-actions.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                refreshed_input_map["actions"]["ToggleView"],
                [
                    {"control": "KeyboardV", "scale": 1.0},
                    {"control": "GamePadX", "scale": 1.0},
                ],
            )

    # test_strict_check_accepts_supported_2d_only_scene(self: テストケース): 対応する2D専用シーンを受け入れる。
    def test_strict_check_accepts_supported_2d_only_scene(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                scene='{"format":"LamaPonScene","objects":[]}',
                modules=["core", "input", "renderer2d"],
            )

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-2d", "lamapon-web-target"
            )

            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )

    # test_strict_check_rejects_convertible_asset_without_tool(self: テストケース): 変換ツールがない素材を診断する。
    def test_strict_check_rejects_convertible_asset_without_tool(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                asset_include_paths=["scenes", "textures"],
            )
            (root / "assets" / "textures").mkdir()
            (root / "assets" / "textures" / "road.dds").write_bytes(
                b"DDS " + bytes(124)
            )

            # 外部コマンドの検出結果を一時的に差し替える。
            with mock.patch.object(
                EXPORT_WEB.shutil,
                "which",
                return_value=None,
            ):
                # findings は互換性診断結果。
                findings = EXPORT_WEB.validate_web_compatibility(
                    root,
                    project,
                    "webgl2-basic-3d",
                    "lamapon-web-target",
                )

            # item は互換性診断、codes は診断コード一覧。
            codes = {item["code"] for item in findings}
            self.assertIn("missing-asset-converter", codes)
            self.assertNotIn("unsupported-asset-format", codes)

    # test_png_is_always_staged_as_lossless_webp_at_original_path(self: テストケース): PNGを元の相対パスでロスレスWebP化する。
    def test_png_is_always_staged_as_lossless_webp_at_original_path(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # assets は素材ディレクトリ。
            assets = root / "assets"
            # textures は画像素材の配置先。
            textures = assets / "textures"
            textures.mkdir(parents=True)
            # original は変換前の素材ファイル。
            original = textures / "road.png"
            original.write_bytes(b"\x89PNG\r\n\x1a\noriginal-project-data")
            # converter はテスト用変換プログラム。
            converter = root / "fake-magick"
            converter.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                "destination = sys.argv[-1].split(':', 1)[-1]\n"
                "pathlib.Path(destination).write_bytes("
                "b'RIFF\\x10\\x00\\x00\\x00WEBPVP8Lconverted')\n",
                encoding="utf-8",
            )
            converter.chmod(0o755)
            # web はWeb出力設定。
            web = {
                "assetDirectory": "assets",
                "assetIncludePaths": ["textures"],
                "converterTools": {"imageMagick": str(converter)},
            }

            # staged は素材のステージ先。
            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                web,
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )

            self.assertIsNotNone(staged)
            # converted は変換後の素材ファイル。
            converted = staged / "textures" / "road.png"
            self.assertEqual(converted.read_bytes()[0:4], b"RIFF")
            self.assertEqual(converted.read_bytes()[8:12], b"WEBP")
            self.assertEqual(
                original.read_bytes(),
                b"\x89PNG\r\n\x1a\noriginal-project-data",
            )
            # manifest は素材変換マニフェスト。
            manifest = json.loads(
                (staged / "lamapon-asset-conversions.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(manifest["assets"][0]["path"], "textures/road.png")
            self.assertEqual(manifest["assets"][0]["sourceFormat"], "png")
            self.assertEqual(manifest["assets"][0]["runtimeFormat"], "webp")

    # test_gif_conversion_uses_native_single_texture_first_frame(self: テストケース): GIFの先頭フレームを単一WebPとして変換する。
    def test_gif_conversion_uses_native_single_texture_first_frame(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # textures は画像素材の配置先。
            textures = root / "assets" / "textures"
            textures.mkdir(parents=True)
            # original は変換前の素材ファイル。
            original = textures / "animated.gif"
            original.write_bytes(b"GIF89a-original-project-data")
            # converter はテスト用変換プログラム。
            converter = root / "fake-magick"
            converter.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                "pathlib.Path(__file__).with_suffix('.args').write_text("
                "'\\n'.join(sys.argv[1:]))\n"
                "destination = sys.argv[-1].split(':', 1)[-1]\n"
                "pathlib.Path(destination).write_bytes("
                "b'RIFF\\x10\\x00\\x00\\x00WEBPVP8Lconverted')\n",
                encoding="utf-8",
            )
            converter.chmod(0o755)
            # web はWeb出力設定。
            web = {
                "assetDirectory": "assets",
                "assetIncludePaths": ["textures"],
                "converterTools": {"imageMagick": str(converter)},
            }

            # staged は素材のステージ先。
            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                web,
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )

            # converted は変換後の素材ファイル。
            converted = staged / "textures" / "animated.gif"
            self.assertEqual(converted.read_bytes()[8:12], b"WEBP")
            # arguments は変換器に渡した引数。
            arguments = converter.with_suffix(".args").read_text(
                encoding="utf-8"
            ).splitlines()
            self.assertEqual(arguments[0], f"{original.resolve()}[0]")
            self.assertIn("webp:lossless=true", arguments)

    # test_asset_converter_wrong_signature_is_rejected(self: テストケース): WebP署名が不正な変換結果を拒否する。
    def test_asset_converter_wrong_signature_is_rejected(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # source は変換前のPNGファイル。
            source = root / "source.png"
            # destination はdestinationの値。
            destination = root / "destination.png"
            source.write_bytes(b"\x89PNG\r\n\x1a\nsource")
            # converter はテスト用変換プログラム。
            converter = root / "fake-magick"
            converter.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                "destination = sys.argv[-1].split(':', 1)[-1]\n"
                "pathlib.Path(destination).write_bytes(b'not-a-webp')\n",
                encoding="utf-8",
            )
            converter.chmod(0o755)

            # 変換結果の形式違反とエラー内容を確認する。
            with self.assertRaisesRegex(
                EXPORT_WEB.ExportError,
                "valid WebP bytes",
            ):
                EXPORT_WEB.run_asset_conversion(
                    source,
                    destination,
                    "image",
                    "webp",
                    converter,
                )

    # test_audio_conversion_stages_wav_bytes_at_original_path(self: テストケース): OGGを元の相対パスでWAV化する。
    def test_audio_conversion_stages_wav_bytes_at_original_path(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # audio は音声素材ディレクトリ。
            audio = root / "assets" / "audio"
            audio.mkdir(parents=True)
            # original は変換前の素材ファイル。
            original = audio / "engine.ogg"
            original.write_bytes(b"OggS-original-project-data")
            # converter はテスト用変換プログラム。
            converter = root / "fake-ffmpeg"
            converter.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                "pathlib.Path(sys.argv[-1]).write_bytes("
                "b'RIFF\\x00\\x00\\x00\\x00WAVEconverted')\n",
                encoding="utf-8",
            )
            converter.chmod(0o755)
            # web はWeb出力設定。
            web = {
                "assetDirectory": "assets",
                "assetIncludePaths": ["audio"],
                "converterTools": {"ffmpeg": str(converter)},
            }

            # staged は素材のステージ先。
            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                web,
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )

            # converted は変換後の素材ファイル。
            converted = staged / "audio" / "engine.ogg"
            self.assertEqual(converted.read_bytes()[0:4], b"RIFF")
            self.assertEqual(original.read_bytes(), b"OggS-original-project-data")
            # manifest は素材変換マニフェスト。
            manifest = json.loads(
                (staged / "lamapon-asset-conversions.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(manifest["assets"][0]["path"], "audio/engine.ogg")
            self.assertEqual(manifest["assets"][0]["runtimeFormat"], "wav")

    # test_portable_glb_without_images_needs_no_image_converter(self: テストケース): 画像のないGLBは画像変換器なしで検証する。
    def test_portable_glb_without_images_needs_no_image_converter(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # model は検証対象GLBモデル。
            model = Path(directory) / "triangle.glb"
            self._write_glb(model, {
                "asset": {"version": "2.0"},
                "accessors": [{"count": 3}],
                "meshes": [{"primitives": [{
                    "mode": 4,
                    "attributes": {"POSITION": 0},
                }]}],
            })

            # generated は生成された出力。
            generated = EXPORT_WEB.externalize_glb_images(model, None)
            # details はGLB互換性の詳細。
            details = EXPORT_WEB.validate_portable_glb(model)

            self.assertEqual(generated, [])
            self.assertEqual(details["primitives"], 1)
            self.assertEqual(details["images"], 0)

    # test_external_model_texture_is_rejected_instead_of_omitted(self: テストケース): 外部テクスチャを黙って除外せず拒否する。
    def test_external_model_texture_is_rejected_instead_of_omitted(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # model は検証対象GLBモデル。
            model = Path(directory) / "external.glb"
            self._write_glb(model, {
                "asset": {"version": "2.0"},
                "images": [{"uri": "missing.png"}],
            })

            # 不正な入力が出力処理で拒否されることを確認する。
            with self.assertRaises(EXPORT_WEB.ExportError):
                EXPORT_WEB.externalize_glb_images(model, None)

    # test_embedded_model_texture_is_externalized_as_webp(self: テストケース): GLB内PNGをWebP素材として外部化する。
    def test_embedded_model_texture_is_externalized_as_webp(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # model は検証対象GLBモデル。
            model = root / "textured.glb"
            # image_bytes はGLB内の画像データ。
            image_bytes = b"\x89PNG\r\n\x1a\nmodel-texture"
            self._write_glb(model, {
                "asset": {"version": "2.0"},
                "buffers": [{"byteLength": len(image_bytes)}],
                "bufferViews": [{
                    "buffer": 0,
                    "byteOffset": 0,
                    "byteLength": len(image_bytes),
                }],
                "images": [{"bufferView": 0, "mimeType": "image/png"}],
            }, image_bytes)
            # converter はテスト用変換プログラム。
            converter = root / "fake-magick"
            converter.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                "destination = sys.argv[-1].split(':', 1)[-1]\n"
                "pathlib.Path(destination).write_bytes("
                "b'RIFF\\x10\\x00\\x00\\x00WEBPVP8Lmodel')\n",
                encoding="utf-8",
            )
            converter.chmod(0o755)

            # generated は生成された出力。
            generated = EXPORT_WEB.externalize_glb_images(model, converter)

            self.assertEqual(len(generated), 1)
            self.assertEqual(generated[0].read_bytes()[8:12], b"WEBP")
            # payload はGLBファイル全体のバイト列。
            payload = model.read_bytes()
            # json_length はGLB JSONチャンク長。
            json_length = struct.unpack_from("<I", payload, 12)[0]
            # document はGLB内のJSON文書。
            document = json.loads(payload[20:20 + json_length].decode("utf-8"))
            self.assertEqual(
                document["images"][0]["uri"],
                "textured.glb.image-0.webp",
            )
            self.assertNotIn("bufferView", document["images"][0])

    # test_portable_glb_rejects_material_features_it_would_lose(self: テストケース): 保持できないマテリアル機能を拒否する。
    def test_portable_glb_rejects_material_features_it_would_lose(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # model は検証対象GLBモデル。
            model = root / "advanced.glb"
            self._write_glb(model, {
                "asset": {"version": "2.0"},
                "accessors": [{"count": 3}],
                "meshes": [{"primitives": [{
                    "mode": 4,
                    "attributes": {"POSITION": 0},
                }]}],
                "materials": [{
                    "extensions": {
                        "KHR_materials_transmission": {
                            "transmissionFactor": 0.8,
                        },
                    },
                }],
            })

            # 不正な入力が出力処理で拒否されることを確認する。
            with self.assertRaises(EXPORT_WEB.ExportError):
                EXPORT_WEB.validate_portable_glb(model)

    # test_portable_glb_reports_supported_unlit_material(self: テストケース): 対応するUnlitマテリアルを報告する。
    def test_portable_glb_reports_supported_unlit_material(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # model は検証対象GLBモデル。
            model = Path(directory) / "unlit.glb"
            self._write_glb(model, {
                "asset": {"version": "2.0"},
                "accessors": [{"count": 3}],
                "meshes": [{"primitives": [{
                    "mode": 4,
                    "attributes": {"POSITION": 0},
                }]}],
                "materials": [{
                    "extensions": {"KHR_materials_unlit": {}},
                }],
            })

            # details はGLB互換性の詳細。
            details = EXPORT_WEB.validate_portable_glb(model)

            self.assertEqual(details["materials"], 1)
            self.assertEqual(details["unlitMaterials"], 1)

    # test_portable_glb_accepts_uint32_sized_meshes(self: テストケース): 32ビット頂点数のGLBメッシュを受け入れる。
    def test_portable_glb_accepts_uint32_sized_meshes(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # model は検証対象GLBモデル。
            model = Path(directory) / "large.glb"
            self._write_glb(model, {
                "asset": {"version": "2.0"},
                "accessors": [{"count": 70000}],
                "meshes": [{"primitives": [{
                    "mode": 4,
                    "attributes": {"POSITION": 0},
                }]}],
            })

            # details はGLB互換性の詳細。
            details = EXPORT_WEB.validate_portable_glb(model)

            self.assertEqual(details["vertices"], 70000)

    # test_scene_accepts_safe_cross_platform_utility_components(self: テストケース): 安全な共通ユーティリティ要素を含むシーンを受け入れる。
    def test_scene_accepts_safe_cross_platform_utility_components(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # scene はWeb検証用シーン定義。
            scene = json.dumps({
                "format": "LamaPonScene",
                "mainCamera": 1,
                "objects": [
                    {
                        "id": 1,
                        "name": "Camera",
                        "parent": None,
                        "enabled": True,
                        "components": [{"type": "Camera", "enabled": True}],
                    },
                    {
                        "id": 2,
                        "name": "Animated sprite",
                        "parent": None,
                        "enabled": True,
                        "components": [
                            {"type": "SpriteRenderer"},
                            {"type": "Rotator", "angularVelocity": [0, 0, 1]},
                            {
                                "type": "InputMover",
                                "horizontalAction": "MoveHorizontal",
                                "verticalAction": "MoveVertical",
                            },
                            {
                                "type": "SpriteAnimator",
                                "columns": 4,
                                "rows": 2,
                                "clips": [{
                                    "name": "Run",
                                    "startFrame": 0,
                                    "frameCount": 8,
                                    "framesPerSecond": 12,
                                }],
                            },
                            {"type": "ParallaxLayer", "referenceId": 1},
                            {"type": "RenderCulling", "alwaysVisible": True},
                        ],
                    },
                ],
            })
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer2d", "renderer3d"],
            )

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            # item は互換性診断。期待するコードやレベルを照合する。
            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )

    # test_scene_rejects_renderer_state_that_would_be_silently_lost(self: テストケース): Webで失われる描画状態を診断する。
    def test_scene_rejects_renderer_state_that_would_be_silently_lost(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # scene はWeb検証用シーン定義。
            scene = json.dumps({
                "format": "LamaPonScene",
                "mainCamera": 1,
                "objects": [{
                    "id": 1,
                    "name": "Camera",
                    "parent": None,
                    "enabled": True,
                    "components": [{
                        "type": "Camera",
                        "enabled": True,
                        "targetTexture": "ui/minimap",
                    }],
                }, {
                    "id": 2,
                    "name": "Custom",
                    "parent": None,
                    "enabled": True,
                    "components": [{
                        "type": "MeshRenderer",
                        "shader": "shaders/custom.hlsl",
                        "worldOverlay": True,
                    }, {
                        "type": "SpriteRenderer",
                        "renderTexture": "ui/minimap",
                    }],
                }],
            })
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer2d", "renderer3d"],
            )

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            # item は互換性診断、codes は診断コード一覧。
            codes = {item["code"] for item in findings}

            self.assertIn("unsupported-camera-render-texture", codes)
            self.assertIn("unsupported-mesh-custom-shader", codes)
            self.assertIn("unsupported-world-overlay", codes)
            self.assertIn("unsupported-sprite-render-texture", codes)

    # test_scene_feature_contract_rejects_silent_model_and_physics_loss(self: テストケース): 消失するモデル・物理機能を診断する。
    def test_scene_feature_contract_rejects_silent_model_and_physics_loss(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # scene はWeb検証用シーン定義。
            scene = json.dumps({
                "format": "LamaPonScene",
                "mainCamera": 1,
                "objects": [
                    {
                        "id": 1,
                        "name": "Camera",
                        "parent": None,
                        "enabled": True,
                        "components": [{"type": "Camera", "enabled": True}],
                    },
                    {
                        "id": 2,
                        "name": "Animated car",
                        "parent": None,
                        "enabled": True,
                        "components": [{
                            "type": "ModelRenderer",
                            "wireframe": True,
                            "animationController": "animations/car.controller.json",
                            "applyRootMotion": True,
                        }, {
                            "type": "Rigidbody",
                            "mass": 1200.0,
                        }],
                    },
                ],
            })
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer3d", "physics3d"],
            )

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            # item は互換性診断、codes は診断コード一覧。
            codes = {item["code"] for item in findings}

            self.assertIn("unsupported-model-wireframe", codes)
            self.assertIn("unsupported-animation-controller", codes)
            self.assertIn("unsupported-root-motion", codes)
            self.assertIn("unsupported-advanced-rigidbody", codes)

    # test_material_contract_checks_nested_assets_and_custom_shader(self: テストケース): 材料の参照素材と独自シェーダーを診断する。
    def test_material_contract_checks_nested_assets_and_custom_shader(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # project はWeb出力用プロジェクト設定。
            project = self._portable_fixture(
                root,
                asset_include_paths=["scenes", "materials"],
            )
            # materials はマテリアル素材の配置先。
            materials = root / "assets" / "materials"
            materials.mkdir()
            (materials / "car.material.json").write_text(json.dumps({
                "type": "LamaPonLitMaterial",
                "version": 2,
                "albedoTexture": "textures/missing.png",
                "shader": "shaders/CustomCar.hlsl",
            }), encoding="utf-8")

            # findings は互換性診断結果。
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            # item は互換性診断、codes は診断コード一覧。
            codes = {item["code"] for item in findings}

            self.assertIn("missing-material-asset", codes)
            self.assertIn("unsupported-custom-material-shader", codes)

    # test_package_removes_previous_manifest_artifact_after_rename(self: テストケース): 名前変更後に古い成果物を削除する。
    def test_package_removes_previous_manifest_artifact_after_rename(self):
        # directory は検証用の一時領域。処理と出力を隔離する。
        with tempfile.TemporaryDirectory() as directory:
            # root は一時プロジェクトのルート。
            root = Path(directory)
            # build はWebビルド成果物の場所。
            build = root / "build"
            # output はWeb出力先。
            output = root / "output"
            build.mkdir()
            output.mkdir()
            # old_html は改名前のHTML成果物。
            old_html = output / "OldName.html"
            old_html.write_text("old", encoding="utf-8")
            (output / "web-export-manifest.json").write_text(
                '{"artifacts":[{"path":"OldName.html"}]}',
                encoding="utf-8",
            )
            # new_html は改名後のHTML成果物。
            new_html = build / "LamaPonWebGL-NewName.html"
            new_html.write_text(
                "<!doctype html><canvas></canvas><script>start()</script>",
                encoding="utf-8",
            )

            EXPORT_WEB.copy_web_package(
                build,
                output,
                "LamaPonWebGL-NewName",
                "webgl2-basic-2d",
                [],
                {"projectName": "NewName"},
                "lamapon-web-target",
                True,
            )

            self.assertFalse(old_html.exists())
            self.assertTrue((output / new_html.name).is_file())
            # packaged は梱包済みHTML本文。
            packaged = (output / new_html.name).read_text(encoding="utf-8")
            self.assertIn('<pre id="lamapon-licenses" hidden>', packaged)
            # 単体HTMLからライセンス本文を復元できる。
            self.assertIn(EXPORT_WEB.web_license_bundle(), html.unescape(packaged))


# 直接実行時のみテストスイートを開始する。
if __name__ == "__main__":
    unittest.main()
