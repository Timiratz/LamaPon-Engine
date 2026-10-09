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
    def test_ascii_embedding_preserves_unicode_virtual_names_and_refreshes_payload(self):
        spec = importlib.util.spec_from_file_location("lamapon_embed_assets", TOOL_PATH.parent / "embed_web_assets.py")
        embed = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(embed)
        with tempfile.TemporaryDirectory(prefix="lamapon-web-embed-") as directory:
            root = Path(directory)
            source, output = root / "assets", root / "embedded"
            source.mkdir()
            name = "白画像 🚀 &.png"
            (source / name).write_bytes(b"original bytes")
            (source / "old.txt").write_bytes(b"old")
            embed.prepare(source, output)
            script = (output / "aliases.js").read_text(encoding="ascii")
            entries = json.loads(script.split("var entries = ", 1)[1].split(";\n", 1)[0])
            mapping = {destination: origin.rsplit("/", 1)[1] for origin, destination in entries}
            self.assertEqual((output / "payload" / mapping["/assets/" + name]).read_bytes(), b"original bytes")
            self.assertTrue(all(path.name.isascii() for path in (output / "payload").iterdir()))
            (source / "old.txt").unlink()
            (source / name).write_bytes(b"changed bytes")
            embed.prepare(source, output)
            self.assertEqual((output / "payload" / mapping["/assets/" + name]).read_bytes(), b"changed bytes")
            self.assertFalse((output / "payload" / mapping["/assets/old.txt"]).exists())
            foreign = root / "foreign"
            foreign.mkdir()
            marker = foreign / "keep.txt"
            marker.write_text("keep", encoding="utf-8")
            with self.assertRaises(ValueError):
                embed.prepare(source, foreign)
            self.assertEqual(marker.read_text(encoding="utf-8"), "keep")
            with self.assertRaises(ValueError):
                embed.prepare(source, source / "overlap")
            self.assertFalse((source / "overlap").exists())

    def test_default_and_explicit_startup_scene_infer_3d_modules(self):
        source = TOOL_PATH.parents[1] / "tests/native"
        for web in ({}, {"scenePath": "/assets/scenes/Main.scene.json"}):
            with self.subTest(web=web), mock.patch.object(Path, "rglob", return_value=[]):
                modules = EXPORT_WEB.infer_lamapon_modules(source, {"export": {"web": web}})
            self.assertIn("renderer3d", modules)

    def test_portable_gltf_and_glb_are_staged_without_model_conversion(self):
        fixtures = TOOL_PATH.parents[1] / "tests/fixtures/models"
        with tempfile.TemporaryDirectory(prefix="lamapon-web-model-") as directory:
            root = Path(directory)
            models = root / "assets/models"
            models.mkdir(parents=True)
            originals = {}
            for name in ("embedded-data.gltf", "embedded-buffer.glb"):
                originals[name] = (fixtures / name).read_bytes()
                (models / name).write_bytes(originals[name])
            web = {"portableGame": True, "assetDirectory": "assets", "assetIncludePaths": ["models"]}
            with mock.patch.object(EXPORT_WEB, "run_asset_conversion", side_effect=AssertionError("Unexpected conversion")):
                staged = EXPORT_WEB.stage_portable_web_assets(root, web, "webgl2-basic-3d", root / "build/generated")
            for name, original in originals.items():
                self.assertEqual((staged / "models" / name).read_bytes(), original)
            model = models / "embedded-data.gltf"
            document = json.loads(model.read_text(encoding="utf-8"))
            document["images"][0]["uri"] = "Missing.png"
            model.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(EXPORT_WEB.ExportError, "missing or not packaged"):
                EXPORT_WEB.validate_direct_portable_model(model, root / "assets", [models])
            document["images"][0]["uri"] = "White.png"
            model.write_text(json.dumps(document), encoding="utf-8")
            (models / "white.png").write_bytes(b"placeholder")
            with self.assertRaises(EXPORT_WEB.ExportError):
                EXPORT_WEB.validate_direct_portable_model(model, root / "assets", [models])

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

    def test_web_ui_and_local_lights_accept_supported_settings(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            objects = [{"id": i + 1, "name": kind, "components": [{"type": kind}]} for i, kind in enumerate(
                ["Camera", "UICanvas", "UIImage", "UIButton", "PointLight", "SpotLight"])]
            scene = json.dumps({"format": "LamaPonScene", "mainCamera": 1, "objects": objects})
            project = self._portable_fixture(root, scene=scene, modules=["core", "input", "renderer2d", "renderer3d"])
            findings = EXPORT_WEB.validate_web_compatibility(root, project, "webgl2-basic-3d", "lamapon-web-target")
            self.assertFalse(any(item["level"] == "reject" for item in findings), findings)
            extra = [{"id": i + 7, "name": "Extra light", "components": [{"type": "PointLight"}]} for i in range(7)]
            (root / "assets/scenes/Main.scene.json").write_text(json.dumps(
                {"format": "LamaPonScene", "mainCamera": 1, "objects": objects + extra}), encoding="utf-8")
            findings = EXPORT_WEB.validate_web_compatibility(root, project, "webgl2-basic-3d", "lamapon-web-target")
            self.assertTrue(any(item["code"] == "portable-local-light-limit" for item in findings))
            objects[2]["components"][0]["border"] = [1, 1, 1, 1]
            objects[3]["components"][0]["loadTargetAdditive"] = True
            (root / "assets/scenes/Main.scene.json").write_text(json.dumps(
                {"format": "LamaPonScene", "mainCamera": 1, "objects": objects}), encoding="utf-8")
            findings = EXPORT_WEB.validate_web_compatibility(root, project, "webgl2-basic-3d", "lamapon-web-target")
            codes = {item["code"] for item in findings if item["level"] == "reject"}
            self.assertIn("unsupported-ui-image-feature", codes)
            self.assertIn("unsupported-ui-button-action", codes)

    def test_serialized_ui_button_scene_targets_are_checked_recursively(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-ui-scenes-") as directory:
            root = Path(directory)
            scenes = root / "assets" / "scenes"
            main_scene = {
                "format": "LamaPonScene",
                "mainCamera": 1,
                "objects": [
                    {"id": 1, "name": "Camera",
                     "components": [{"type": "Camera"}]},
                    {"id": 2, "name": "Next button",
                     "components": [{"type": "UIButton",
                                     "clickEvent": "GoToNextScene",
                                     "targetScene": "scenes/Next.scene.json"}]},
                    {"id": 3, "name": "Reload button",
                     "components": [{"type": "UIButton",
                                     "reloadCurrentScene": True}]},
                ],
            }
            next_scene = {
                "format": "LamaPonScene",
                "mainCamera": 10,
                "objects": [
                    {"id": 10, "name": "Next camera",
                     "components": [{"type": "Camera"}]},
                    {"id": 11, "name": "Final button",
                     "components": [{"type": "UIButton",
                                     "targetScene": "scenes/Final.scene.json"}]},
                ],
            }
            final_scene = {
                "format": "LamaPonScene",
                "mainCamera": 20,
                "objects": [
                    {"id": 20, "name": "Final camera",
                     "components": [{"type": "Camera"}]},
                    {"id": 21, "name": "Unsupported shape",
                     "components": [{"type": "MeshRenderer", "shape": "Torus"}]},
                ],
            }
            project = self._portable_fixture(
                root,
                scene=json.dumps(main_scene),
                modules=["core", "input", "renderer2d", "renderer3d"],
            )
            (scenes / "Next.scene.json").write_text(json.dumps(next_scene), encoding="utf-8")
            (scenes / "Final.scene.json").write_text(json.dumps(final_scene), encoding="utf-8")

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            codes = {item["code"] for item in findings}
            self.assertIn("unsupported-primitive-shape", codes)
            self.assertNotIn("unsupported-ui-button-action", codes)
            self.assertNotIn("invalid-ui-button-scene", codes)

            main_scene["objects"][1]["components"][0]["targetScene"] = "../outside.scene.json"
            (scenes / "Main.scene.json").write_text(json.dumps(main_scene), encoding="utf-8")
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            self.assertIn("invalid-ui-button-scene", {item["code"] for item in findings})

            main_scene["objects"][1]["components"][0]["targetScene"] = "scenes/Missing.scene.json"
            (scenes / "Main.scene.json").write_text(json.dumps(main_scene), encoding="utf-8")
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            self.assertIn("missing-scene-asset", {item["code"] for item in findings})

    def test_conversion_cache_invalidates_inputs_tools_and_corrupt_output(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            original = assets / "image.png"
            original.write_bytes(b"original")
            spare = assets / "spare.json"
            spare.write_text("{}", encoding="utf-8")
            converter = root / "fake-magick"
            converter.write_bytes(b"converter-v1")
            web = {"assetDirectory": "assets", "converterTools": {"imageMagick": str(converter)}}
            generated = root / ".lamapon/generated"
            def convert(source, destination, *args):
                destination.write_bytes(b"RIFFconverted-webp")
            with mock.patch.object(EXPORT_WEB, "run_asset_conversion", side_effect=convert) as conversion:
                staged = EXPORT_WEB.stage_portable_web_assets(root, web, "webgl2-basic-3d", generated)
                output = staged / "image.png"
                original_time = output.stat().st_mtime_ns
                EXPORT_WEB.stage_portable_web_assets(root, web, "webgl2-basic-3d", generated)
                self.assertEqual(conversion.call_count, 1)
                self.assertEqual(output.stat().st_mtime_ns, original_time)
                original.write_bytes(b"changed-source")
                (staged / "lamapon-asset-conversions.json").write_bytes(b"\xff")
                EXPORT_WEB.stage_portable_web_assets(root, web, "webgl2-basic-3d", generated)
                self.assertEqual(conversion.call_count, 2)
                converter.write_bytes(b"converter-v2")
                EXPORT_WEB.stage_portable_web_assets(root, web, "webgl2-basic-3d", generated)
                self.assertEqual(conversion.call_count, 3)
                output.write_bytes(b"corrupt")
                EXPORT_WEB.stage_portable_web_assets(root, web, "webgl2-basic-3d", generated)
                self.assertEqual(conversion.call_count, 4)
                original.unlink()
                EXPORT_WEB.stage_portable_web_assets(root, web, "webgl2-basic-3d", generated)
                self.assertFalse(output.exists())
                self.assertFalse((staged / "lamapon-asset-conversions.json").exists())
                cache = json.loads((generated.parent / "web-asset-cache.json").read_text(encoding="utf-8"))
                self.assertEqual(cache, {})

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

    def test_portable_scene_reports_environment_effects_it_does_not_render(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            environment = {
                key: {"enabled": True}
                for key in EXPORT_WEB.PORTABLE_UNSUPPORTED_ENVIRONMENT_EFFECTS
            }
            environment["fog"] = {"enabled": True, "density": 0.4}
            environment["sky"] = {
                "enabled": True,
                "cubemap": "textures/Sky.dds",
                "groundColor": [0.2, 0.3, 0.4],
                "intensity": 2.0,
                "iblIntensity": 0.5,
                "sunDriven": True,
            }
            scene = json.dumps({
                "format": "LamaPonScene",
                "mainCamera": 1,
                "environment": environment,
                "objects": [{
                    "id": 1,
                    "name": "Camera",
                    "components": [{"type": "Camera"}],
                }],
            })
            project = self._portable_fixture(root, scene=scene)

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")

            effect_warnings = [
                item for item in findings
                if item["code"] == "unsupported-environment-effect"
            ]
            self.assertEqual(len(effect_warnings), len(
                EXPORT_WEB.PORTABLE_UNSUPPORTED_ENVIRONMENT_EFFECTS), findings)
            self.assertTrue(all("Portable" in item["message"] for item in effect_warnings))
            self.assertTrue(any(item["code"] == "unsupported-fog-density" for item in findings))
            self.assertTrue(any(item["code"] == "unsupported-sky-settings" for item in findings))

            environment = {key: {"enabled": False} for key in environment}
            environment["fog"] = {"enabled": False, "density": 0.0}
            environment["sky"] = {"enabled": False}
            scene = json.dumps({
                "format": "LamaPonScene",
                "mainCamera": 1,
                "environment": environment,
                "objects": [{
                    "id": 1,
                    "name": "Camera",
                    "components": [{"type": "Camera"}],
                }],
            })
            (root / "assets/scenes/Main.scene.json").write_text(scene, encoding="utf-8")
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            self.assertFalse(any(item["code"] in {
                "unsupported-environment-effect", "unsupported-fog-density",
                "unsupported-sky-settings",
            } for item in findings), findings)

    def test_every_serialized_scene_environment_setting_is_classified(self):
        serializer = (TOOL_PATH.parents[1] / "src" / "LamaPon" / "Scene"
                      / "SceneSerialization.cpp").read_text(encoding="utf-8")
        environment_start = serializer.index('{ "environment", {')
        physics_start = serializer.index('{ "physics", {', environment_start)
        environment_block = serializer[environment_start:physics_start]
        serialized_settings = set(re.findall(
            r'(?m)^ {16}\{\s*"([A-Za-z_]\w*)"\s*,', environment_block))
        classified_settings = (
            EXPORT_WEB.PORTABLE_HANDLED_ENVIRONMENT_SETTINGS
            | set(EXPORT_WEB.PORTABLE_UNSUPPORTED_ENVIRONMENT_EFFECTS)
        )
        self.assertEqual(serialized_settings, classified_settings,
                         sorted(serialized_settings ^ classified_settings))

    def test_portable_scene_rejects_malformed_environment_json_before_runtime(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(root)
            malformed_environments = [
                "invalid",
                {"fog": False},
                {"sky": {"topColor": [1.0, "blue", 0.5]}},
                {"sky": {"enabled": 1}},
                {"ambientIntensity": "high"},
                {"sky": {"intensity": 10 ** 1000}},
            ]
            for environment in malformed_environments:
                with self.subTest(environment=environment):
                    scene = json.dumps({
                        "format": "LamaPonScene",
                        "mainCamera": 1,
                        "environment": environment,
                        "objects": [{
                            "id": 1,
                            "name": "Camera",
                            "components": [{"type": "Camera"}],
                        }],
                    })
                    (root / "assets/scenes/Main.scene.json").write_text(
                        scene, encoding="utf-8")
                    findings = EXPORT_WEB.validate_web_compatibility(
                        root, project, "webgl2-basic-3d", "lamapon-web-target")
                    self.assertTrue(any(
                        item["level"] == "reject"
                        and item["code"] == "invalid-scene-environment-setting"
                        for item in findings
                    ), findings)

    def test_portable_scene_rejects_malformed_runtime_component_values(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(root)
            malformed_components = [
                ({"type": "Camera", "nearPlane": "close"}, "nearPlane"),
                ({"type": "Camera", "enabled": 1}, "enabled"),
                ({"type": "Camera", "verticalFieldOfView": 10 ** 1000},
                 "verticalFieldOfView"),
                ({"type": "Camera", "targetTextureWidth": 1 << 31},
                 "targetTextureWidth"),
                ({"type": "BoxCollider3D", "layer": 1 << 32}, "layer"),
                ({"type": "SpriteRenderer", "sortOrder": 1 << 31}, "sortOrder"),
                ({"type": "ParallaxLayer", "referenceId": -1}, "referenceId"),
                ({"type": "UIImage", "border": 5}, "border"),
                ({"type": "Rigidbody", "constraints": {"positionX": "yes"}},
                 "constraints"),
                ({"type": "AudioSource", "streaming": "false"}, "streaming"),
                ({"type": "MeshRenderer", "customParameters": ["bad"]},
                 "customParameters"),
                ({"type": "SpriteRenderer", "shaderKeywords": "keyword"},
                 "shaderKeywords"),
                ({"type": "InputMover", "horizontalAction": []},
                 "horizontalAction"),
                ({"type": "ModelRenderer", "baseColor": [1, "blue", 1, 1]},
                 "baseColor"),
                ({"type": "SpriteAnimator", "clips": [{"name": "Idle",
                  "startFrame": "zero"}]}, "startFrame"),
            ]
            for component, field in malformed_components:
                with self.subTest(component=component):
                    scene = json.dumps({
                        "format": "LamaPonScene",
                        "mainCamera": 1,
                        "objects": [{
                            "id": 1,
                            "name": "Camera",
                            "transform": {"position": [0, "bad", 0]},
                            "components": [{"type": "Camera"}, component],
                        }],
                    })
                    (root / "assets/scenes/Main.scene.json").write_text(
                        scene, encoding="utf-8")
                    findings = EXPORT_WEB.validate_web_compatibility(
                        root, project, "webgl2-basic-3d", "lamapon-web-target")
                    invalid_settings = [item for item in findings
                                        if item["code"] == "invalid-scene-setting"]
                    self.assertTrue(any(
                        f".{field}" in item["message"]
                        for item in invalid_settings
                    ), findings)
                    self.assertTrue(any(
                        "transform.position" in item["message"]
                        for item in invalid_settings
                    ), findings)

    def test_portable_scene_rejects_unrepresentable_ids_without_crashing(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(root)
            malformed_scenes = [
                {"format": "LamaPonScene", "mainCamera": [], "objects": [{
                    "id": 1, "components": [{"type": "Camera"}],
                }]},
                {"format": "LamaPonScene", "mainCamera": 1, "objects": [{
                    "id": True, "parent": [], "components": [{"type": "Camera"}],
                }]},
                {"format": "LamaPonScene", "mainCamera": 1, "objects": [{
                    "id": 1 << 63, "components": [{"type": "Camera"}],
                }]},
            ]
            for scene in malformed_scenes:
                with self.subTest(scene=scene):
                    (root / "assets/scenes/Main.scene.json").write_text(
                        json.dumps(scene), encoding="utf-8")
                    findings = EXPORT_WEB.validate_web_compatibility(
                        root, project, "webgl2-basic-3d", "lamapon-web-target")
                    codes = {item["code"] for item in findings}
                    self.assertTrue(
                        "invalid-scene-setting" in codes
                        or "invalid-scene-object-id" in codes,
                        findings,
                    )

    def test_portable_component_type_schema_matches_runtime_loader(self):
        loader_source = (TOOL_PATH.parents[1] / "src" / "LamaPon" / "Portable"
                         / "PortableRuntime.cpp").read_text(encoding="utf-8")
        loader_source = loader_source[loader_source.index("bool Scene::Load("):]
        document_fields = set(re.findall(
            r'document\.value\(\s*"([A-Za-z_]\w*)"', loader_source))
        self.assertEqual(document_fields, {
            "environment", "format", "mainCamera", "objects", "root", "version",
        })
        object_fields = set(re.findall(
            r'objectJson\.value\(\s*"([A-Za-z_]\w*)"', loader_source))
        self.assertEqual(object_fields, {
            "alwaysVisible", "components", "cullingMargin", "enabled",
            "id", "name", "parent", "tag", "transform",
        })
        transform_fields = set(re.findall(
            r'transform\.value\(\s*"([A-Za-z_]\w*)"', loader_source))
        self.assertEqual(transform_fields, {"position", "rotation", "scale"})
        component_value_fields = set(re.findall(
            r'component\.value\(\s*"([A-Za-z_]\w*)"', loader_source))
        component_field_types = (
            EXPORT_WEB.PORTABLE_SCENE_COMPONENT_FIELDS
            - EXPORT_WEB.PORTABLE_SCENE_COMPONENT_ARRAY_FIELDS
        )
        self.assertLessEqual(
            component_value_fields,
            component_field_types,
            sorted(component_value_fields - component_field_types),
        )
        component_array_fields = set(re.findall(
            r'component\.at\(\s*"([A-Za-z_]\w*)"', loader_source))
        self.assertLessEqual(
            component_array_fields,
            EXPORT_WEB.PORTABLE_SCENE_COMPONENT_ARRAY_FIELDS,
            sorted(component_array_fields
                   - EXPORT_WEB.PORTABLE_SCENE_COMPONENT_ARRAY_FIELDS),
        )
        portable_loader_fields = set(re.findall(
            r'(?:component|material)\.(?:value|at|contains|find)\(\s*"([A-Za-z_]\w*)"',
            loader_source,
        ))
        character_rig_loader = (TOOL_PATH.parents[1] / "src" / "LamaPon"
                                / "Portable" / "PortableCharacterRig2D.cpp")
        portable_loader_fields.update(re.findall(
            r'component\.(?:value|at|contains|find)\(\s*"([A-Za-z_]\w*)"',
            character_rig_loader.read_text(encoding="utf-8"),
        ))
        clip_value_fields = set(re.findall(
            r'clip\.value\(\s*"([A-Za-z_]\w*)"', loader_source))
        self.assertEqual(
            clip_value_fields,
            EXPORT_WEB.PORTABLE_SCENE_CLIP_FIELDS,
            sorted(clip_value_fields ^ EXPORT_WEB.PORTABLE_SCENE_CLIP_FIELDS),
        )
        self.assertEqual(
            EXPORT_WEB.PORTABLE_SCENE_COMPONENT_INTEGER_FIELD_TYPES.keys(),
            EXPORT_WEB.PORTABLE_SCENE_COMPONENT_INTEGER_FIELDS,
        )
        self.assertTrue({"clickEvent", "targetScene", "reloadCurrentScene"} <=
                        EXPORT_WEB.PORTABLE_SCENE_COMPONENT_RUNTIME_FIELDS)
        self.assertEqual(
            EXPORT_WEB.PORTABLE_SCENE_COMPONENT_UNSUPPORTED_FIELDS["UIButton"],
            {"loadTargetAdditive"},
        )
        self.assertEqual(
            EXPORT_WEB.PORTABLE_SCENE_CLIP_INTEGER_FIELD_TYPES.keys(),
            EXPORT_WEB.PORTABLE_SCENE_CLIP_INTEGER_FIELDS,
        )
        material_value_fields = set(re.findall(
            r'material\.value\(\s*"([A-Za-z_]\w*)"', loader_source))
        self.assertLessEqual(
            material_value_fields,
            (EXPORT_WEB.PORTABLE_MATERIAL_STRING_FIELDS
             | EXPORT_WEB.PORTABLE_MATERIAL_NUMBER_FIELDS
             | EXPORT_WEB.PORTABLE_MATERIAL_ARRAY_FIELDS),
            sorted(material_value_fields - (
                EXPORT_WEB.PORTABLE_MATERIAL_STRING_FIELDS
                | EXPORT_WEB.PORTABLE_MATERIAL_NUMBER_FIELDS
                | EXPORT_WEB.PORTABLE_MATERIAL_ARRAY_FIELDS
            )),
        )
        material_array_fields = set(re.findall(
            r'material\.at\(\s*"([A-Za-z_]\w*)"', loader_source))
        self.assertLessEqual(
            material_array_fields,
            EXPORT_WEB.PORTABLE_MATERIAL_ARRAY_FIELDS,
            sorted(material_array_fields
                   - EXPORT_WEB.PORTABLE_MATERIAL_ARRAY_FIELDS),
        )
        loaded_material_fields = set(re.findall(
            r'loadedMaterial\.value\(\s*"([A-Za-z_]\w*)"', loader_source))
        self.assertEqual(loaded_material_fields, {"type"})

        serializer_source = (TOOL_PATH.parents[1] / "src" / "LamaPon" / "Scene"
                             / "SceneSerialization.cpp").read_text(encoding="utf-8")
        serializer_source = serializer_source[
            serializer_source.index("Json SerializeComponent("):
        ]
        serializer_branches = list(re.finditer(
            r'dynamic_cast\s*<\s*const\s+LamaPon::\s*(\w+)\s*\s*\*>',
            serializer_source,
        ))
        serialized_runtime_fields = set()
        for branch_index, branch in enumerate(serializer_branches):
            component_type = branch.group(1).removesuffix("Component")
            if component_type not in EXPORT_WEB.PORTABLE_SCENE_COMPONENTS:
                continue
            branch_end = (serializer_branches[branch_index + 1].start()
                          if branch_index + 1 < len(serializer_branches)
                          else serializer_source.find("\n    }", branch.start()))
            branch_source = serializer_source[branch.start():branch_end]
            serialized_fields = set(re.findall(
                r'result\["([A-Za-z_]\w*)"\]', branch_source))
            serialized_fields.update(re.findall(
                r'SerializeAssetReference\(\s*result\s*,\s*"([A-Za-z_]\w*)"',
                branch_source,
                re.S,
            ))
            serialized_runtime_fields.update(
                serialized_fields
                & EXPORT_WEB.PORTABLE_SCENE_COMPONENT_RUNTIME_FIELDS
            )
            if component_type in EXPORT_WEB.PORTABLE_SCENE_ALWAYS_REJECT_COMPONENTS:
                continue
            classified_fields = (
                EXPORT_WEB.PORTABLE_SCENE_COMPONENT_RUNTIME_FIELDS
                | EXPORT_WEB.PORTABLE_SCENE_COMPONENT_UNSUPPORTED_FIELDS.get(
                    component_type, set())
                | EXPORT_WEB.PORTABLE_SCENE_COMPONENT_EDITOR_ONLY_FIELDS.get(
                    component_type, set())
                | {"type", "enabled"}
            )
            self.assertLessEqual(
                serialized_fields,
                classified_fields,
                f"{component_type}: "
                f"{sorted(serialized_fields - classified_fields)}",
            )
        self.assertLessEqual(
            serialized_runtime_fields,
            portable_loader_fields,
            sorted(serialized_runtime_fields - portable_loader_fields),
        )
        self.assertTrue(
            EXPORT_WEB.PORTABLE_SCENE_ALWAYS_REJECT_COMPONENTS
            <= EXPORT_WEB.PORTABLE_SCENE_COMPONENTS.keys()
        )
        for component_type, fields in EXPORT_WEB.PORTABLE_SCENE_COMPONENT_UNSUPPORTED_FIELDS.items():
            self.assertTrue(
                fields <= EXPORT_WEB.PORTABLE_SCENE_COMPONENT_FIELDS,
                (component_type, sorted(fields - EXPORT_WEB.PORTABLE_SCENE_COMPONENT_FIELDS)),
            )
        self.assertEqual(
            EXPORT_WEB.PORTABLE_SCENE_COMPONENT_EDITOR_ONLY_FIELDS,
            {"ParticleSystem": {"previewInEditor"}},
        )
        self.assertTrue(
            set().union(*EXPORT_WEB.PORTABLE_SCENE_COMPONENT_EDITOR_ONLY_FIELDS.values())
            <= EXPORT_WEB.PORTABLE_SCENE_COMPONENT_FIELDS
        )
        self.assertTrue(
            EXPORT_WEB.PORTABLE_MATERIAL_UNSUPPORTED_FIELDS
            <= (EXPORT_WEB.PORTABLE_MATERIAL_STRING_FIELDS
                | set(EXPORT_WEB.PORTABLE_MATERIAL_LIST_FIELD_TYPES))
        )

    def test_portable_scene_object_field_differences_are_diagnosed(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(root)
            scene = {
                "format": "LamaPonScene", "mainCamera": 1,
                "objects": [{
                    "id": 1, "name": "Camera", "tag": "probe",
                    "persistent": True, "persistenceKey": "camera",
                    "alwaysVisible": True, "cullingMargin": 4.0,
                    "prefabAsset": "prefabs/Camera.prefab.json",
                    "prefabAssetGuid": "asset-camera",
                    "components": [{"type": "Camera"}],
                }],
            }
            (root / "assets/scenes/Main.scene.json").write_text(
                json.dumps(scene), encoding="utf-8")
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            codes = {item["code"] for item in findings}
            self.assertIn("portable-object-persistence-ignored", codes)
            self.assertIn("portable-legacy-render-culling-ignored", codes)
            self.assertNotIn("invalid-scene-setting", codes)

            malformed_fields = [
                ("tag", 7), ("persistent", "true"),
                ("persistenceKey", []), ("alwaysVisible", 1),
                ("cullingMargin", 10 ** 1000),
                ("prefabAsset", {}), ("prefabAssetGuid", False),
            ]
            for field, value in malformed_fields:
                with self.subTest(field=field):
                    malformed = {
                        "format": "LamaPonScene", "mainCamera": 1,
                        "objects": [{
                            "id": 1, "name": "Camera", field: value,
                            "components": [{"type": "Camera"}],
                        }],
                    }
                    (root / "assets/scenes/Main.scene.json").write_text(
                        json.dumps(malformed), encoding="utf-8")
                    findings = EXPORT_WEB.validate_web_compatibility(
                        root, project, "webgl2-basic-3d", "lamapon-web-target")
                    self.assertTrue(any(
                        item["code"] == "invalid-scene-setting"
                        and f".{field}" in item["message"]
                        for item in findings
                    ), findings)

    def test_portable_project_warns_when_global_physics_settings_are_ignored(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(root)
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            self.assertFalse(any(
                item["code"] == "portable-project-physics-approximation"
                for item in findings), findings)

            project["physics"] = {
                "gravity": {"x": 2.0, "y": -3.0, "z": 1.0},
                "fixedTimeStep": 1.0 / 120.0,
                "collisionOff": [[0, 1]],
                "clampDiscreteSpeed": True,
            }
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            warning = next(item for item in findings
                           if item["code"] == "portable-project-physics-approximation")
            self.assertEqual(warning["level"], "warning")
            for setting in ("gravity", "fixedTimeStep", "collision layer matrix",
                            "speed clamp"):
                self.assertIn(setting, warning["message"])

    def test_every_top_level_portable_header_type_has_a_module(self):
        # header はゲームスクリプトがincludeするPortable公開ヘッダー。
        header = (TOOL_PATH.parents[1] / "src" / "LamaPon" / "Portable"
                  / "include" / "LamaPon" / "LamaPon.h").read_text(
                      encoding="utf-8")
        namespace_start = header.index("namespace LamaPon\n{")
        # C++ tokenはコメント・文字列中の型名を無視するための検査用テキスト。
        code = EXPORT_WEB.mask_cpp_non_code(header[namespace_start:])
        declaration = re.compile(
            r"\{|\}|\b(?:class|struct|enum\s+class|enum)\s+([A-Za-z_]\w*)"
        )
        # top_level_types はLamaPon namespace直下に宣言された公開型。
        top_level_types: set[str] = set()
        depth = 0
        for match in declaration.finditer(code):
            if match.group(1) and depth == 1:
                top_level_types.add(match.group(1))
            if match.group() == "{":
                depth += 1
            elif match.group() == "}":
                depth -= 1

        self.assertTrue(top_level_types)
        self.assertLessEqual(
            top_level_types,
            set(EXPORT_WEB.PORTABLE_API_MODULES),
            sorted(top_level_types - set(EXPORT_WEB.PORTABLE_API_MODULES)),
        )

    def test_reactive_event_types_are_recognized_as_core_portable_apis(self):
        source = (
            '#include "LamaPon/LamaPon.h"\n'
            'using namespace LamaPon;\n'
            'void ObserveEvents(Scene& scene) {\n'
            '    Observable<EventArgs> stream = scene.Events().Observe("Game.Test");\n'
            '    Subscription subscription = stream.Subscribe([](const EventArgs&) {});\n'
            '    CompositeSubscription subscriptions;\n'
            '    subscriptions.Add(std::move(subscription));\n'
            '}\n'
        )
        with tempfile.TemporaryDirectory(prefix="lamapon-web-reactive-api-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=source,
                scene='{"format":"LamaPonScene","objects":[]}',
                modules=["core", "input", "renderer2d"],
            )

            inferred = EXPORT_WEB.infer_lamapon_modules(root, project)
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-2d", "lamapon-web-target"
            )

        self.assertEqual(inferred, ["core", "input", "renderer2d"])
        self.assertFalse(any(
            item["code"] in {"unsupported-portable-api", "missing-required-module"}
            for item in findings
        ), findings)

    def test_portable_scene_loader_gaps_are_explicitly_rejected(self):
        # loader_source はPortable Sceneのcomponent読込実装。
        runtime = (TOOL_PATH.parents[1] / "src" / "LamaPon" / "Portable"
                   / "PortableRuntime.cpp").read_text(encoding="utf-8")
        load_start = runtime.index("bool Scene::Load(")
        parent_resolution = runtime.index("for (const PendingParent& pending", load_start)
        loader_source = runtime[load_start:parent_resolution]
        # handled はScene::Loadに実際の復元分岐があるcomponent名。
        handled = set(re.findall(r'type\s*==\s*"([A-Za-z0-9_]+)"', loader_source))
        # 複数型を共有する復元分岐も列挙します。
        for pair in re.findall(
            r'type\s*==\s*"([A-Za-z0-9_]+)"\s*\|\|\s*type\s*==\s*"([A-Za-z0-9_]+)"',
            loader_source,
        ):
            handled.update(pair)
        if "CharacterRig2DRuntime::LoadComponent" in loader_source:
            handled.update({
                "Blink2D", "Keyform2D", "Rig2D", "SpriteSkin2D", "Sway2D",
            })

        self.assertEqual(
            set(EXPORT_WEB.PORTABLE_SCENE_COMPONENTS) - handled,
            {"MeshCollider3D"},
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scene = json.dumps({
                "format": "LamaPonScene",
                "mainCamera": 1,
                "objects": [{
                    "id": 1,
                    "name": "Camera",
                    "components": [{"type": "Camera"}],
                }, {
                    "id": 2,
                    "name": "Terrain",
                    "components": [{"type": "MeshCollider3D"}],
                }],
            })
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer3d", "physics3d"],
            )
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            mesh_collider = next(
                item for item in findings
                if item["code"] == "unsupported-scene-mesh-collider"
            )
            self.assertEqual(mesh_collider["level"], "reject")

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

    def test_namespace_import_does_not_hide_portable_api_module_requirements(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            source = ('#include "LamaPon/LamaPon.h"\n'
                      'using namespace LamaPon;\n'
                      'MeshRendererComponent* mesh{};\n')
            project = self._portable_fixture(
                root,
                source_text=source,
                scene='{"format":"LamaPonScene","objects":[]}',
                modules=["core", "input", "renderer2d"],
            )

            inferred = EXPORT_WEB.infer_lamapon_modules(root, project)
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertIn("renderer3d", inferred)
            self.assertTrue(any(
                item["code"] == "missing-required-module"
                and "LamaPon::MeshRendererComponent" in item["message"]
                and "renderer3d" in item["message"]
                for item in findings
            ), findings)

    def test_single_symbol_import_tracks_portable_api_module_requirements(self):
        cases = (
            'using LamaPon::MeshRendererComponent;\n',
            'using ::LamaPon :: MeshRendererComponent;\n',
            'namespace LP = :: LamaPon; using LP :: MeshRendererComponent;\n',
            'using namespace :: LamaPon;\n',
        )
        for import_line in cases:
            with self.subTest(import_line=import_line), \
                    tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
                root = Path(directory)
                source = ('#include "LamaPon/LamaPon.h"\n' + import_line
                          + 'MeshRendererComponent* mesh{};\n')
                project = self._portable_fixture(
                    root,
                    source_text=source,
                    scene='{"format":"LamaPonScene","objects":[]}',
                    modules=["core", "input", "renderer2d"],
                )
                inferred = EXPORT_WEB.infer_lamapon_modules(root, project)
                findings = EXPORT_WEB.validate_web_compatibility(
                    root, project, "webgl2-basic-3d", "lamapon-web-target"
                )
                self.assertIn("renderer3d", inferred)
                self.assertTrue(any(
                    item["code"] == "missing-required-module"
                    and "LamaPon::MeshRendererComponent" in item["message"]
                    and "renderer3d" in item["message"]
                    for item in findings
                ), findings)

    def test_single_symbol_import_rejects_unknown_portable_api(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            source = ('#include "LamaPon/LamaPon.h"\n'
                      'using LamaPon::FutureRendererComponent;\n'
                      'FutureRendererComponent* future{};\n')
            project = self._portable_fixture(root, source_text=source)
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            self.assertTrue(any(
                item["code"] == "unsupported-portable-api"
                and "LamaPon::FutureRendererComponent" in item["message"]
                for item in findings
            ), findings)

    def test_namespace_alias_unknown_portable_type_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            source = ('#include "LamaPon/LamaPon.h"\n'
                      'namespace LP = LamaPon;\n'
                      'LP::FutureRendererComponent* future{};\n')
            project = self._portable_fixture(root, source_text=source)

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertTrue(any(
                item["code"] == "unsupported-portable-api"
                and "LamaPon::FutureRendererComponent" in item["message"]
                for item in findings
            ), findings)

    def test_imported_namespace_alias_tracks_unqualified_portable_types(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            source = ('#include "LamaPon/LamaPon.h"\n'
                      'namespace LP = LamaPon;\n'
                      'using namespace LP;\n'
                      'MeshRendererComponent* mesh{};\n')
            project = self._portable_fixture(
                root,
                source_text=source,
                scene='{"format":"LamaPonScene","objects":[]}',
                modules=["core", "input", "renderer2d"],
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertTrue(any(
                item["code"] == "missing-required-module"
                and "LamaPon::MeshRendererComponent" in item["message"]
                and "renderer3d" in item["message"]
                for item in findings
            ), findings)

    def test_scene_methods_infer_and_require_their_runtime_modules(self):
        cases = (
            ("WebAudio", "audio", 'void Use(LamaPon::Scene& scene) { scene.WebAudio(); }'),
            ("Raycast", "physics3d", 'auto use = &LamaPon::Scene::Raycast;'),
            ("Raycast", "physics3d", 'auto use = &LamaPon :: Scene :: Raycast;'),
            ("WebAudio", "audio", 'auto use = &:: LamaPon :: Scene :: WebAudio;'),
        )
        for method, module, source_line in cases:
            with self.subTest(method=method), \
                    tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
                root = Path(directory)
                source = '#include "LamaPon/LamaPon.h"\n' + source_line + "\n"
                project = self._portable_fixture(
                    root,
                    source_text=source,
                    scene='{"format":"LamaPonScene","objects":[]}',
                    modules=["core", "input", "renderer2d"],
                )

                inferred = EXPORT_WEB.infer_lamapon_modules(root, project)
                findings = EXPORT_WEB.validate_web_compatibility(
                    root, project, "webgl2-basic-3d", "lamapon-web-target"
                )

                self.assertIn(module, inferred)
                self.assertTrue(any(
                    item["code"] == "missing-required-module"
                    and f"LamaPon::Scene::{method}" in item["message"]
                    and module in item["message"]
                    for item in findings
                ), findings)

    def test_portable_contract_rejects_unimplemented_scene_physics_queries(self):
        methods = (
            "RaycastAll", "SphereCast", "BoxCast", "CapsuleCast",
            "OverlapBox", "OverlapSphere", "OverlapCapsule",
        )
        source = (
            '#include "LamaPon/LamaPon.h"\n'
            'void Query(LamaPon::Scene& scene) {\n'
            + "\n".join(f"    scene.{method}();" for method in methods)
            + "\n}\n"
        )
        with tempfile.TemporaryDirectory(prefix="lamapon-web-physics-api-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=source,
                scene='{"format":"LamaPonScene","objects":[]}',
            )
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

        unsupported = [item for item in findings
                       if item["code"] == "unsupported-portable-scene-api"]
        self.assertEqual(len(unsupported), len(methods), findings)
        self.assertEqual(
            {method for method in methods
             if any(f"{method}はPortable" in item["message"] for item in unsupported)},
            set(methods), findings,
        )

    def test_portable_contract_rejects_unimplemented_scene_configuration_apis(self):
        methods = (
            "SetMainCamera", "SetSkySettings", "LoadDataAsset",
            "SaveToFile", "PhysicsStats",
        )
        source = (
            '#include "LamaPon/LamaPon.h"\n'
            'void Configure(LamaPon::Scene& scene) {\n'
            + "\n".join(f"    (void)scene.{method}();" for method in methods)
            + "\n}\n"
        )
        with tempfile.TemporaryDirectory(prefix="lamapon-web-scene-api-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=source,
                scene='{"format":"LamaPonScene","objects":[]}',
            )
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

        unsupported = [item for item in findings
                       if item["code"] == "unsupported-portable-scene-api"]
        self.assertEqual(len(unsupported), len(methods), findings)
        self.assertEqual(
            {method for method in methods
             if any(f"{method}はPortable" in item["message"] for item in unsupported)},
            set(methods), findings,
        )

    def test_portable_scene_clear_is_rejected_without_rejecting_event_bus_clear(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-scene-clear-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=(
                    '#include "LamaPon/LamaPon.h"\n'
                    'void ClearScene(LamaPon::Scene& scene, LamaPon::EventBus& events) {\n'
                    '    scene.Clear();\n'
                    '    events.Clear();\n'
                    '}\n'
                ),
            )
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

        unsupported = [item for item in findings
                       if item["code"] == "unsupported-portable-scene-api"]
        self.assertEqual(len(unsupported), 1, findings)
        self.assertIn("ClearはPortable", unsupported[0]["message"])

    def test_portable_contract_rejects_qualified_unimplemented_scene_query(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-physics-qualified-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=(
                    '#include "LamaPon/LamaPon.h"\n'
                    'auto query = &LamaPon::Scene::RaycastAll;\n'
                ),
            )
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

        self.assertTrue(any(
            item["code"] == "unsupported-portable-scene-api"
            and "RaycastAllはPortable" in item["message"]
            for item in findings
        ), findings)

    def test_portable_contract_rejects_windows_only_script_methods(self):
        methods = (
            "StartCoroutine", "SetWindowSize", "LoadDataAsset",
            "SignInWithDiscord", "Network", "SaveNumber", "CreateGameObject",
        )
        source = (
            '#include "LamaPon/LamaPon.h"\n'
            'namespace LP = LamaPon;\n'
            'class Probe final : public LP::Script {\n'
            '    void Update(float) override {\n'
            + "\n".join(f"        (void)this->{method}();" for method in methods)
            + "\n    }\n};\n"
        )
        with tempfile.TemporaryDirectory(prefix="lamapon-web-script-api-") as directory:
            root = Path(directory)
            project = self._portable_fixture(root, source_text=source)
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

        unsupported = [item for item in findings
                       if item["code"] == "unsupported-portable-script-api"]
        self.assertEqual(len(unsupported), len(methods), findings)
        self.assertEqual(
            {method for method in methods
             if any(f"Script::{method}はPortable" in item["message"] for item in unsupported)},
            set(methods), findings,
        )

    def test_portable_script_api_scan_ignores_non_script_calls_and_local_helpers(self):
        source = (
            '#include "LamaPon/LamaPon.h"\n'
            'class Probe final : public LamaPon::Script {\n'
            '    void StartCoroutine() {}\n'
            '    void Update(float) override { StartCoroutine(); }\n'
            '};\n'
            'class Worker { void Update() { StartCoroutine(); } };\n'
            '// class Fake : public LamaPon::Script { void Update(){ Network(); } };\n'
            'const char* note = "StartCoroutine() Network()";\n'
        )
        with tempfile.TemporaryDirectory(prefix="lamapon-web-script-api-safe-") as directory:
            root = Path(directory)
            project = self._portable_fixture(root, source_text=source)
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

        self.assertFalse(any(
            item["code"] == "unsupported-portable-script-api"
            for item in findings
        ), findings)

    def test_cpp_comments_and_literals_do_not_require_portable_modules(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            source = (
                '#include "LamaPon/LamaPon.h"\n'
                'using namespace LamaPon;\n'
                'const char* label = "MeshRendererComponent Scene::WebAudio LamaPon::FutureType";\n'
                'const char* raw = R"quoted(MeshRendererComponent)quoted";\n'
                'const char* raw8 = u8R"json(Scene::WebAudio LamaPon::FutureType)json";\n'
                '// MeshRendererComponent and scene.WebAudio() are examples only.\n'
                '/* LamaPon::FutureType scene.Raycast(ray, 2, hit); */\n'
                '// object.AddComponent<NativeScriptComponent>("Game.Missing");\n'
            )
            project = self._portable_fixture(
                root,
                source_text=source,
                scene='{"format":"LamaPonScene","objects":[]}',
                modules=["core", "input", "renderer2d"],
            )

            inferred = EXPORT_WEB.infer_lamapon_modules(root, project)
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-2d", "lamapon-web-target"
            )

            self.assertNotIn("renderer3d", inferred)
            self.assertNotIn("audio", inferred)
            self.assertNotIn("physics3d", inferred)
            self.assertFalse(any(
                item["code"] in {
                    "missing-required-module", "unsupported-portable-api",
                    "unregistered-dynamic-script",
                }
                for item in findings
            ), findings)

    def test_dynamic_prefab_instantiation_is_checked_and_packaged(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            source = (
                '#include "LamaPon/LamaPon.h"\n'
                "class Spawner : public LamaPon::Script {\n"
                "    void Update(float) override {\n"
                '        Instantiate("prefabs/Enemy.prefab.json");\n'
                '        GetScene().InstantiatePrefab("prefabs/Enemy.prefab.json");\n'
                "    }\n"
                "};\n"
            )
            project = self._portable_fixture(
                root,
                source_text=source,
                modules=["core", "input", "renderer2d", "renderer3d"],
                asset_include_paths=["scenes", "prefabs"],
            )
            prefabs = root / "assets" / "prefabs"
            prefabs.mkdir()
            (prefabs / "Enemy.prefab.json").write_text(json.dumps({
                "format": "LamaPonPrefab", "version": 1, "root": 7,
                "objects": [{"id": 7, "name": "Enemy", "parent": None,
                             "components": [{"type": "SpriteRenderer"}]}],
            }), encoding="utf-8")

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            self.assertFalse(any(item["level"] == "reject" for item in findings), findings)
            self.assertTrue(any(item["code"] == "portable-contract-complete"
                                for item in findings), findings)

    def test_dynamic_prefab_instantiation_requires_a_static_packaged_asset(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=(
                    "class Spawner : public LamaPon::Script {\n"
                    "    void Update(float) override { Instantiate(prefabPath); }\n"
                    "};\n"
                ),
                asset_include_paths=["scenes"],
            )
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            self.assertTrue(any(item["code"] == "invalid-portable-prefab-reference"
                                for item in findings), findings)

    def test_namespace_alias_script_prefab_instantiation_requires_static_path(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-prefab-alias-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=(
                    'namespace LP = LamaPon;\n'
                    'class Spawner : public LP::Script {\n'
                    '    void Update(float) override { this->Instantiate(prefabPath); }\n'
                    '};\n'
                ),
            )
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

        self.assertTrue(any(
            item["code"] == "invalid-portable-prefab-reference"
            for item in findings
        ), findings)

    def test_multiple_inheritance_script_prefab_instantiation_is_checked(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-prefab-multiple-bases-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=(
                    'namespace LP = LamaPon;\n'
                    'struct Marker {};\n'
                    'class Spawner : public Marker, public LP::Script {\n'
                    '    void Update(float) override { this->Instantiate(prefabPath); }\n'
                    '};\n'
                ),
            )
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

        self.assertTrue(any(
            item["code"] == "invalid-portable-prefab-reference"
            for item in findings
        ), findings)

    def test_dynamic_prefab_instantiation_rejects_cyclic_hierarchies(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=(
                    "class Spawner : public LamaPon::Script {\n"
                    '    void Update(float) override { Instantiate("prefabs/Enemy.prefab.json"); }\n'
                    "};\n"
                ),
                asset_include_paths=["scenes", "prefabs"],
            )
            prefabs = root / "assets" / "prefabs"
            prefabs.mkdir()
            (prefabs / "Enemy.prefab.json").write_text(json.dumps({
                "format": "LamaPonPrefab", "version": 1, "root": 7,
                "objects": [
                    {"id": 7, "name": "Enemy", "parent": None,
                     "components": []},
                    {"id": 8, "name": "Cycle A", "parent": 9,
                     "components": []},
                    {"id": 9, "name": "Cycle B", "parent": 8,
                     "components": []},
                ],
            }), encoding="utf-8")

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertTrue(any(
                item["code"] == "invalid-portable-prefab-hierarchy"
                and item["level"] == "reject"
                for item in findings
            ), findings)

    def test_unimplemented_scene_manager_methods_are_rejected_before_build(self):
        methods = (
            "RequestLoadAsync", "RequestLoadAdditive", "IsInputBlocked",
            "LoadStatus", "PrefetchFailureCount", "PrefetchedAssetBytes",
            "PrefetchedAssetCount", "ResetTransition", "ResolveScenePath",
            "SetCurrentScenePath", "TransitionFrame",
        )
        calls = "\n".join(
            f'        GetScene().Scenes().{method}("scenes/next.scene.json");'
            if method in {"RequestLoadAsync", "RequestLoadAdditive"}
            else f"        GetScene().Scenes().{method}();"
            for method in methods
        )
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=(
                    "class Spawner : public LamaPon::Script {\n"
                    "    void Update(float) override {\n"
                    + calls
                    + "\n    }\n"
                    "};\n"
                ),
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            rejected_methods = [
                item["message"] for item in findings
                if item["code"] == "unsupported-portable-scene-api"
            ]
            self.assertEqual(len(rejected_methods), len(methods), findings)
            self.assertEqual(
                {method for method in methods
                 if any(f"{method}はPortable" in item for item in rejected_methods)},
                set(methods), findings,
            )

    def test_request_load_requires_a_static_packaged_scene_path(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=(
                    "class Spawner : public LamaPon::Script {\n"
                    "    void Update(float) override { GetScene().Scenes().RequestLoad(nextScene); }\n"
                    "};\n"
                ),
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertTrue(any(
                item["code"] == "invalid-portable-scene-reference"
                for item in findings
            ), findings)

    def test_request_load_checks_target_scene_scripts_and_hierarchy(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                source_text=(
                    '#include "LamaPon/LamaPon.h"\n'
                    'LAMAPON_SCRIPT_NAMED(Spawner, "Test.Spawner", "Spawner");\n'
                    "class Spawner : public LamaPon::Script {\n"
                    '    void Update(float) override { GetScene().Scenes().RequestLoad("scenes/Next.scene.json"); }\n'
                    "};\n"
                ),
            )
            (root / "assets" / "scenes" / "Next.scene.json").write_text(
                json.dumps({
                    "format": "LamaPonScene",
                    "objects": [{
                        "id": 1,
                        "name": "Unregistered target script",
                        "parent": None,
                        "components": [{
                            "type": "NativeScript",
                            "script": "Test.Missing",
                        }],
                    }],
                }),
                encoding="utf-8",
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertTrue(any(
                item["code"] == "unregistered-scene-script"
                and "Test.Missing" in item["message"]
                for item in findings
            ), findings)

    def test_unregistered_dynamic_scripts_are_rejected_across_namespace_spellings(self):
        cases = (
            ("using namespace LamaPon;", "NativeScriptComponent"),
            ("namespace LP = LamaPon;", "LP::NativeScriptComponent"),
            ("namespace LP = LamaPon; using namespace LP;", "NativeScriptComponent"),
            ("using LamaPon::NativeScriptComponent;", "NativeScriptComponent"),
            ("using namespace :: LamaPon;", "NativeScriptComponent"),
            ("namespace LP = :: LamaPon;", "LP :: NativeScriptComponent"),
            ("using :: LamaPon :: NativeScriptComponent;", "NativeScriptComponent"),
        )
        for namespace_setup, component_type in cases:
            with self.subTest(namespace_setup=namespace_setup), \
                    tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
                root = Path(directory)
                source = (
                    '#include "LamaPon/LamaPon.h"\n'
                    + namespace_setup + "\n"
                    'LAMAPON_SCRIPT_NAMED(Probe, "Game.Probe", "Probe");\n'
                    'void Spawn(LamaPon::GameObject& object) {\n'
                    f'    object.AddComponent<{component_type}>("Game.Missing");\n'
                    '}\n'
                )
                project = self._portable_fixture(root, source_text=source)

                findings = EXPORT_WEB.validate_web_compatibility(
                    root, project, "webgl2-basic-3d", "lamapon-web-target"
                )

                self.assertTrue(any(
                    item["code"] == "unregistered-dynamic-script"
                    and "Game.Missing" in item["message"]
                    for item in findings
                ), findings)

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

    def test_default_script_registration_macro_matches_scene_and_dynamic_ids(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            source = (
                '#include "LamaPon/LamaPon.h"\n'
                'class ProbeScript final : public LamaPon::Script {};\n'
                'LAMAPON_SCRIPT(ProbeScript);\n'
                'void Spawn(LamaPon::GameObject& object) {\n'
                '    object.AddComponent<LamaPon::NativeScriptComponent>("Game.ProbeScript");\n'
                '}\n'
            )
            scene = (
                '{"format":"LamaPonScene","mainCamera":1,"objects":[{'
                '"id":1,"name":"Camera","components":['
                '{"type":"Camera"},'
                '{"type":"NativeScript","script":"Game.ProbeScript","properties":{}}'
                ']}]}'
            )
            project = self._portable_fixture(root, source_text=source, scene=scene)
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            codes = {item["code"] for item in findings}
            self.assertNotIn("unregistered-scene-script", codes, findings)
            self.assertNotIn("unregistered-dynamic-script", codes, findings)
            self.assertFalse(any(item["level"] == "reject" for item in findings), findings)

    def test_default_script_macro_examples_in_comments_and_strings_do_not_register(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            source = (
                '#include "LamaPon/LamaPon.h"\n'
                '// LAMAPON_SCRIPT(FakeScript);\n'
                'const char* example = "LAMAPON_SCRIPT(FakeScript);";\n'
            )
            scene = (
                '{"format":"LamaPonScene","mainCamera":1,"objects":[{'
                '"id":1,"name":"Camera","components":['
                '{"type":"Camera"},'
                '{"type":"NativeScript","script":"Game.FakeScript","properties":{}}'
                ']}]}'
            )
            project = self._portable_fixture(root, source_text=source, scene=scene)
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            self.assertIn("unregistered-scene-script", {item["code"] for item in findings})

    def test_strict_check_rejects_non_finite_scene_numbers(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            for value in ("NaN", "Infinity", "1e999"):
                with self.subTest(value=value):
                    case_root = root / value
                    scene = (
                        '{"format":"LamaPonScene","mainCamera":1,"objects":['
                        '{"id":1,"name":"Camera","components":['
                        '{"type":"Camera","field":' + value + '}]}]}'
                    )
                    project = self._portable_fixture(case_root, scene=scene)

                    findings = EXPORT_WEB.validate_web_compatibility(
                        case_root, project, "webgl2-basic-3d", "lamapon-web-target"
                    )

                    diagnostic = next(
                        item for item in findings
                        if item["code"] == "invalid-scene-number"
                    )
                    self.assertIn("objects[0].components[0].field", diagnostic["message"])

    def test_strict_check_reports_invalid_utf8_startup_scene(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            project = self._portable_fixture(root)
            (root / "assets/scenes/Main.scene.json").write_bytes(b"\xff\xfe")

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            diagnostic = next(
                item for item in findings if item["code"] == "invalid-scene-json"
            )
            self.assertIn("UTF-8 byte 0", diagnostic["message"])

    def test_strict_check_rejects_invalid_material_and_non_finite_asset_numbers(self):
        cases = (
            ("broken-material", "materials/Broken.material.json", "{ invalid",
             "invalid-portable-material"),
            ("non-finite-material", "materials/NonFinite.material.json",
             '{"type":"LamaPonLitMaterial","version":1,"baseColor":[NaN,1,1,1]}',
             "invalid-portable-material-number"),
            ("wrong-material-field", "materials/WrongField.material.json",
             '{"type":"LamaPonLitMaterial","version":1,"roughness":"high"}',
             "invalid-portable-material-setting"),
            ("wrong-material-version", "materials/WrongVersion.material.json",
             '{"type":"LamaPonLitMaterial","version":true}',
             "invalid-portable-material"),
            ("non-finite-animation", "animations/NonFinite.animation.json",
             '{"format":"LamaPonAnimationClip","version":1,"duration":1e999,'
             '"keyframes":[{"time":1e999,"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}]}',
             "invalid-portable-animation"),
        )
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            for name, relative, contents, expected_code in cases:
                with self.subTest(name=name):
                    case_root = root / name
                    project = self._portable_fixture(
                        case_root,
                        asset_include_paths=["scenes", "materials", "animations"],
                    )
                    for asset_directory in ("materials", "animations"):
                        (case_root / "assets" / asset_directory).mkdir(parents=True)
                    asset = case_root / "assets" / relative
                    asset.write_text(contents, encoding="utf-8")

                    findings = EXPORT_WEB.validate_web_compatibility(
                        case_root, project, "webgl2-basic-3d", "lamapon-web-target"
                    )

                    self.assertIn(expected_code, {item["code"] for item in findings})

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

    # test_scene_accepts_2d_character_rig_components(self: テストケース): 2Dキャラクター部品を含むシーンを受け入れる。
    def test_scene_accepts_2d_character_rig_components(self):
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
                        "name": "Character",
                        "parent": None,
                        "enabled": True,
                        "components": [{
                            "type": "Rig2D",
                            "parameters": [{"name": "AngleX", "minimum": -30, "maximum": 30}],
                        }],
                    },
                    {
                        "id": 3,
                        "name": "Hair",
                        "parent": 2,
                        "enabled": True,
                        "components": [
                            {"type": "SpriteRenderer", "meshColumns": 1, "meshRows": 4},
                            {"type": "SpriteSkin2D", "bones": [4]},
                            {"type": "Keyform2D", "channels": [{
                                "parameter": "AngleX",
                                "keys": [{"value": 30, "position": [4, 0]}],
                            }]},
                            {"type": "Sway2D", "tipOffset": [0, 100]},
                            {"type": "Blink2D", "columns": 3},
                        ],
                    },
                    {
                        "id": 4,
                        "name": "Bone",
                        "parent": 3,
                        "enabled": True,
                        "components": [],
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

    def test_portable_serialized_feature_differences_have_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            components = [
                {"type": "Camera", "targetTexture": "textures/target.png"},
                {"type": "DirectionalLight", "castsShadows": True},
                {"type": "PointLight", "castsShadows": True},
                {"type": "SpotLight", "castsShadows": True},
                {"type": "MeshRenderer", "shape": "Torus",
                 "shader": "shaders/custom.hlsl", "worldOverlay": True,
                 "shaderKeywords": ["CUSTOM"], "customParameters": [[1, 0, 0, 0]],
                 "customTexture0": "textures/custom.png"},
                {"type": "SpriteRenderer", "renderTexture": "textures/target.png",
                 "shader": "shaders/sprite.hlsl", "customParameters": [[1, 0, 0, 0]]},
                {"type": "UIButton", "targetScene": "scenes/Other.scene.json",
                 "clickEvent": "Start", "reloadCurrentScene": True,
                 "loadTargetAdditive": True},
                {"type": "UIImage", "border": [1, 1, 1, 1],
                 "renderTexture": "textures/target.png"},
                {"type": "AudioSource", "bus": 1, "streaming": True,
                 "spatial": True},
                {"type": "ModelRenderer", "wireframe": True,
                 "animationController": "animations/model.controller.json",
                 "applyRootMotion": True, "rootMotionNode": "Root",
                 "useLegacyShading": True, "preserveEmbeddedMaterialColor": True,
                 "shader": "shaders/model.hlsl", "shaderKeywords": ["CUSTOM"],
                 "customParameters": [[1, 0, 0, 0]],
                 "customTexture0": "textures/custom.png"},
                {"type": "ParticleSystem", "shape": "Mesh",
                 "renderMode": "StretchedBillboard", "shader": "shaders/particle.hlsl",
                 "auxiliaryTexture": "textures/aux.png",
                 "customParameters": [[1, 0, 0, 0]]},
                {"type": "BoxCollider3D", "friction": 0.9, "restitution": 0.8},
                {"type": "MeshCollider3D"},
                {"type": "Rigidbody", "mass": 5.0, "interpolate": True},
            ]
            scene = json.dumps({
                "format": "LamaPonScene",
                "mainCamera": 1,
                "objects": [{"id": index + 1, "name": component["type"],
                             "components": [component]}
                            for index, component in enumerate(components)],
            })
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer2d", "renderer3d",
                         "audio", "physics3d", "particles3d"],
            )
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            codes = {item["code"] for item in findings}
            expected_codes = {
                "unsupported-camera-render-texture", "unsupported-portable-shadows",
                "unsupported-primitive-shape", "unsupported-mesh-custom-shader",
                "unsupported-mesh-custom-bindings", "unsupported-world-overlay",
                "unsupported-sprite-render-texture", "unsupported-sprite-custom-shader",
                "unsupported-ui-button-action", "unsupported-ui-image-feature",
                "portable-audio-bus-approximation", "portable-audio-buffered-stream",
                "web-spatial-audio-approximation", "unsupported-model-wireframe",
                "unsupported-animation-controller", "unsupported-root-motion",
                "unsupported-legacy-model-shading", "unsupported-preserve-material-color",
                "unsupported-model-custom-shader", "unsupported-model-shader-keywords",
                "unsupported-model-custom-bindings", "web-particle-shape-approximation",
                "unsupported-particle-render-mode", "unsupported-particle-custom-shader",
                "portable-basic-box-physics", "unsupported-scene-mesh-collider",
                "unsupported-advanced-rigidbody", "portable-rigidbody-interpolation-ignored",
            }
            self.assertTrue(expected_codes <= codes, sorted(expected_codes - codes))

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

    # test_audio_bus_contract_reports_portable_mixer_limits(self: テストケース): 個別音声バスの近似と不正値を検査する。
    def test_audio_bus_contract_reports_portable_mixer_limits(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            scene = json.dumps({
                "format": "LamaPonScene",
                "mainCamera": 1,
                "objects": [{
                    "id": 1,
                    "name": "Camera",
                    "components": [{"type": "Camera"}],
                }, {
                    "id": 2,
                    "name": "Music",
                    "components": [{"type": "AudioSource", "bus": 1}],
                }, {
                    "id": 3,
                    "name": "Effects",
                    "components": [{"type": "AudioSource", "bus": 2}],
                }],
            })
            project = self._portable_fixture(
                root, scene=scene, modules=["core", "audio", "renderer3d"])

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            bus_warnings = [item for item in findings
                            if item["code"] == "portable-audio-bus-approximation"]
            self.assertEqual(len(bus_warnings), 1, findings)
            self.assertIn("Music", bus_warnings[0]["message"])
            self.assertEqual(bus_warnings[0]["level"], "warning")

            for invalid_bus in (True, -1, 4, "music"):
                with self.subTest(invalid_bus=invalid_bus):
                    invalid_scene = json.loads(scene)
                    invalid_scene["objects"][1]["components"][0]["bus"] = invalid_bus
                    (root / "assets/scenes/Main.scene.json").write_text(
                        json.dumps(invalid_scene), encoding="utf-8")
                    invalid_findings = EXPORT_WEB.validate_web_compatibility(
                        root, project, "webgl2-basic-3d", "lamapon-web-target")
                    self.assertTrue(any(
                        item["code"] == "invalid-scene-audio-bus"
                        and item["level"] == "reject"
                        for item in invalid_findings
                    ), invalid_findings)

    # test_rigidbody_interpolation_is_reported_when_portable_renderer_ignores_it(self: テストケース): Rigidbody描画補間の差を診断する。
    def test_rigidbody_interpolation_is_reported_when_portable_renderer_ignores_it(self):
        with tempfile.TemporaryDirectory(prefix="lamapon-web-") as directory:
            root = Path(directory)
            scene = {
                "format": "LamaPonScene",
                "mainCamera": 1,
                "objects": [{
                    "id": 1,
                    "name": "Camera",
                    "components": [{"type": "Camera"}],
                }, {
                    "id": 2,
                    "name": "Moving body",
                    "components": [{"type": "Rigidbody", "interpolate": True}],
                }],
            }
            project = self._portable_fixture(
                root, scene=json.dumps(scene), modules=["core", "physics3d", "renderer3d"])

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            warning = next(item for item in findings
                           if item["code"] == "portable-rigidbody-interpolation-ignored")
            self.assertEqual(warning["level"], "warning")
            self.assertIn("Moving body", warning["message"])

            scene["objects"][1]["components"][0]["interpolate"] = False
            (root / "assets/scenes/Main.scene.json").write_text(
                json.dumps(scene), encoding="utf-8")
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            self.assertNotIn(
                "portable-rigidbody-interpolation-ignored",
                {item["code"] for item in findings})

            scene["objects"][1]["components"][0]["interpolate"] = "false"
            (root / "assets/scenes/Main.scene.json").write_text(
                json.dumps(scene), encoding="utf-8")
            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target")
            self.assertTrue(any(
                item["code"] == "invalid-rigidbody-interpolation"
                and item["level"] == "reject"
                for item in findings), findings)

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
