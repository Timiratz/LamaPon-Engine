from __future__ import annotations

import importlib.util
import json
import re
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock


TOOL_PATH = Path(__file__).resolve().parents[1] / "tools" / "export_web.py"
SPEC = importlib.util.spec_from_file_location("lamapon_export_web", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
EXPORT_WEB = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT_WEB)


class WebExportToolTests(unittest.TestCase):
    @staticmethod
    def _write_glb(path: Path, document: dict, binary: bytes = b"") -> None:
        encoded = json.dumps(document, separators=(",", ":")).encode("utf-8")
        encoded += b" " * ((4 - len(encoded) % 4) % 4)
        chunks = [(0x4E4F534A, encoded)]
        if binary:
            binary += b"\x00" * ((4 - len(binary) % 4) % 4)
            chunks.append((0x004E4942, binary))
        total = 12 + sum(8 + len(payload) for _, payload in chunks)
        result = bytearray(struct.pack("<4sII", b"glTF", 2, total))
        for chunk_type, payload in chunks:
            result.extend(struct.pack("<II", len(payload), chunk_type))
            result.extend(payload)
        path.write_bytes(result)

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
        if scene is None:
            scene = (
                '{"format":"LamaPonScene","mainCamera":1,"objects":['
                '{"id":1,"name":"Camera","parent":null,"components":['
                '{"type":"Camera","enabled":true}],"enabled":true}]}'
            )
        (root / "assets" / "scenes" / "Main.scene.json").write_text(
            scene + "\n",
            encoding="utf-8",
        )
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

    def test_web_asset_conversion_catalog_is_complete(self):
        expected_images = {
            ".bmp", ".dds", ".gif", ".jpeg", ".jpg", ".png",
            ".tga", ".tif", ".tiff",
        }
        expected_audio = {
            ".aac", ".flac", ".m4a", ".mp3", ".ogg", ".wma",
        }
        expected_models = {
            ".3ds", ".3mf", ".ac", ".blend", ".dae", ".dxf",
            ".fbx", ".glb", ".gltf", ".ifc", ".lwo", ".md2",
            ".md3", ".md5mesh", ".ms3d", ".obj", ".off", ".ogex",
            ".ply", ".pmx", ".smd", ".step", ".stl", ".stp",
            ".vrm", ".x", ".x3d",
        }
        actual = EXPORT_WEB.WEB_ASSET_CONVERSIONS
        self.assertEqual(
            {extension for extension, rule in actual.items()
             if rule == ("image", "webp")},
            expected_images,
        )
        self.assertEqual(
            {extension for extension, rule in actual.items()
             if rule == ("audio", "wav")},
            expected_audio,
        )
        self.assertEqual(
            {extension for extension, rule in actual.items()
             if rule == ("model", "glb")},
            expected_models,
        )

    def test_single_file_package_verification_accepts_expanded_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            html = Path(directory) / "Game.html"
            html.write_text(
                "<!doctype html><canvas></canvas><script>start()</script>",
                encoding="utf-8",
            )

            checks = EXPORT_WEB.verify_web_artifacts([html], True)

            self.assertEqual(
                [item["code"] for item in checks],
                [
                    "non-empty-artifacts",
                    "html-entrypoint",
                    "self-contained-html",
                ],
            )

    def test_every_native_scene_component_is_classified_for_web(self):
        native_components: set[str] = set()
        component_root = TOOL_PATH.parents[1] / "src" / "LamaPon" / "Components"
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

    def test_package_verification_rejects_unexpanded_shell(self):
        with tempfile.TemporaryDirectory() as directory:
            html = Path(directory) / "Game.html"
            html.write_text(
                "<canvas></canvas><script></script>{{{ SCRIPT }}}",
                encoding="utf-8",
            )

            with self.assertRaises(EXPORT_WEB.ExportError):
                EXPORT_WEB.verify_web_artifacts([html], True)

    def test_package_verification_enforces_standard_output_name(self):
        with tempfile.TemporaryDirectory() as directory:
            html = Path(directory) / "WrongName.html"
            html.write_text(
                "<!doctype html><canvas></canvas><script>start()</script>",
                encoding="utf-8",
            )

            with self.assertRaises(EXPORT_WEB.ExportError):
                EXPORT_WEB.verify_web_artifacts(
                    [html],
                    True,
                    "LamaPonWebGL-Game",
                )

    def test_compatibility_report_contains_strict_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            project = {
                "name": "PortableGame",
                "export": {"web": {"portableGame": True}},
            }
            findings = [
                EXPORT_WEB.finding("auto", "converted", "done", "none"),
                EXPORT_WEB.finding("warning", "visual", "compare", "preview"),
            ]

            report = EXPORT_WEB.write_compatibility_report(
                output,
                project,
                "lamapon-web-target",
                "webgl2-basic-3d",
                "ready",
                findings,
            )
            payload = json.loads(report.read_text(encoding="utf-8"))

            self.assertEqual(payload["version"], 2)
            self.assertTrue(payload["strictPortableContract"])
            self.assertEqual(
                payload["summary"],
                {"auto": 1, "info": 0, "warning": 1, "reject": 0},
            )

    def test_generated_target_uses_declared_sources_and_modules(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.cpp").write_text("int main() { return 0; }\n")
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

            generated = EXPORT_WEB.generate_lamapon_web_target(
                root,
                project,
                "GeneratedGame",
                ["core", "input"],
                "webgl2-basic-2d",
                root / ".lamapon" / "generated",
            )
            cmake = (generated / "CMakeLists.txt").read_text(encoding="utf-8")

            self.assertIn("lamapon_add_web_game(GeneratedGame", cmake)
            self.assertIn(str(root / "main.cpp"), cmake)
            self.assertIn("        core", cmake)
            self.assertIn("        input", cmake)
            self.assertIn("SINGLE_FILE", cmake)
            self.assertNotIn("ASSET_DIRECTORY", cmake)
            self.assertIn(
                "OUTPUT_NAME [==[LamaPonWebGL-GeneratedGame]==]",
                cmake,
            )

    def test_normal_lamapon_project_gets_generated_portable_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project_path = root / ".lamapon" / "project.json"
            project_path.parent.mkdir()
            scripts = root / "assets" / "scripts"
            scenes = root / "assets" / "scenes"
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
            source_document = {
                "format": "LamaPonProject",
                "gameName": "Beginner Game",
                "startupScene": "scenes/Main.scene.json",
            }
            project_path.write_text(
                json.dumps(source_document),
                encoding="utf-8",
            )
            _, project = EXPORT_WEB.load_project(project_path)
            project_root = EXPORT_WEB.lamapon_project_root(project_path, project)

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
            findings = EXPORT_WEB.validate_web_compatibility(
                source,
                project,
                profile,
                project_kind,
            )
            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )
            self.assertIn(
                "generated-web-runtime-target",
                {item["code"] for item in findings},
            )
            self.assertEqual(
                json.loads(project_path.read_text(encoding="utf-8")),
                source_document,
            )

    def test_portable_target_compiles_original_sources_and_stages_assets(self):
        with tempfile.TemporaryDirectory() as directory:
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
            project = {
                "name": "PortableGame",
                "projectName": "CarGame",
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

            findings = EXPORT_WEB.validate_web_compatibility(
                root,
                project,
                "webgl2-basic-3d",
                "lamapon-web-target",
            )
            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )

            generated = EXPORT_WEB.generate_lamapon_web_target(
                root,
                project,
                "PortableGame",
                ["core", "input", "renderer3d", "particles3d"],
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )
            cmake = (generated / "CMakeLists.txt").read_text(encoding="utf-8")
            staged_scene = root / ".lamapon" / "web-generated-assets" / (
                "scenes/Main.scene.json"
            )

            self.assertIn("PORTABLE_GAME", cmake)
            self.assertIn(
                "OUTPUT_NAME [==[LamaPonWebGL-CarGame]==]", cmake
            )
            self.assertIn("GAME_NAME [==[Portable Game]==]", cmake)
            self.assertIn(
                "SCENE_PATH [==[/assets/scenes/Main.scene.json]==]", cmake
            )
            self.assertIn("        particles3d", cmake)
            self.assertTrue(staged_scene.is_file())

    def test_web_artifact_name_is_forced_from_project_name(self):
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

        output_name = EXPORT_WEB.web_artifact_prefix(
            Path("/tmp/source"),
            project,
            "lamapon-web-target",
        )

        self.assertEqual(output_name, "LamaPonWebGL-ドライブゲーム")

    def test_web_project_name_rejects_filename_separators(self):
        with self.assertRaises(EXPORT_WEB.ExportError):
            EXPORT_WEB.web_artifact_prefix(
                Path("/tmp/source"),
                {"projectName": "Folder/Game"},
                "lamapon-web-target",
            )

    def test_strict_check_rejects_unknown_api_and_missing_module(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                '#include "LamaPon/LamaPon.h"\n'
                "LamaPon::AudioSourceComponent* audio{};\n"
                "LamaPon::FutureRendererComponent* future{};\n",
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            codes = {item["code"] for item in findings}

            self.assertIn("missing-required-module", codes)
            self.assertIn("unsupported-portable-api", codes)

    def test_strict_check_rejects_scene_contract_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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
            project = self._portable_fixture(root, scene=scene)

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            codes = {item["code"] for item in findings}

            self.assertIn("missing-scene-asset", codes)
            self.assertIn("unsupported-scene-component", codes)
            self.assertIn("unregistered-scene-script", codes)
            self.assertIn("cyclic-scene-hierarchy", codes)

    def test_strict_check_accepts_ui_rect_scene_component(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer2d", "renderer3d"],
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )

    def test_strict_check_rejects_corrupt_selected_asset(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                asset_include_paths=["scenes", "textures"],
            )
            (root / "assets" / "textures").mkdir()
            (root / "assets" / "textures" / "broken.png").write_bytes(
                b"not a png"
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertIn(
                "corrupt-png-asset",
                {item["code"] for item in findings},
            )

    def test_strict_check_rejects_unknown_input_action(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                '#include "LamaPon/LamaPon.h"\n'
                'float value = input.Value("Teleport");\n',
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertIn(
                "unsupported-input-action",
                {item["code"] for item in findings},
            )

    def test_project_input_action_is_generated_without_engine_edit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            self.assertNotIn(
                "unsupported-input-action",
                {item["code"] for item in findings},
            )
            self.assertIn(
                "project-input-action-map",
                {item["code"] for item in findings},
            )

            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                project["export"]["web"],
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )
            input_map = json.loads(
                (staged / "lamapon-input-actions.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                input_map["actions"]["ToggleView"][1]["control"],
                "GamePadY",
            )

            # The Editor can now hot-reload an externally changed
            # project.json. Export must likewise read the latest on-disk
            # bindings instead of reusing a map cached by an earlier preview.
            (root / ".lamapon" / "project.json").write_text(json.dumps({
                "inputActions": [{
                    "name": "ToggleView",
                    "bindings": [
                        {"control": "KeyboardV", "scale": 1.0},
                        {"control": "GamePadX", "scale": 1.0},
                    ],
                }],
            }), encoding="utf-8")
            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                project["export"]["web"],
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )
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

    def test_strict_check_accepts_supported_2d_only_scene(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                scene='{"format":"LamaPonScene","objects":[]}',
                modules=["core", "input", "renderer2d"],
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-2d", "lamapon-web-target"
            )

            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )

    def test_strict_check_rejects_convertible_asset_without_tool(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                asset_include_paths=["scenes", "textures"],
            )
            (root / "assets" / "textures").mkdir()
            (root / "assets" / "textures" / "road.dds").write_bytes(
                b"DDS " + bytes(124)
            )

            with mock.patch.object(
                EXPORT_WEB.shutil,
                "which",
                return_value=None,
            ):
                findings = EXPORT_WEB.validate_web_compatibility(
                    root,
                    project,
                    "webgl2-basic-3d",
                    "lamapon-web-target",
                )

            codes = {item["code"] for item in findings}
            self.assertIn("missing-asset-converter", codes)
            self.assertNotIn("unsupported-asset-format", codes)

    def test_png_is_always_staged_as_lossless_webp_at_original_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            textures = assets / "textures"
            textures.mkdir(parents=True)
            original = textures / "road.png"
            original.write_bytes(b"\x89PNG\r\n\x1a\noriginal-project-data")
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
            web = {
                "assetDirectory": "assets",
                "assetIncludePaths": ["textures"],
                "converterTools": {"imageMagick": str(converter)},
            }

            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                web,
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )

            self.assertIsNotNone(staged)
            converted = staged / "textures" / "road.png"
            self.assertEqual(converted.read_bytes()[0:4], b"RIFF")
            self.assertEqual(converted.read_bytes()[8:12], b"WEBP")
            self.assertEqual(
                original.read_bytes(),
                b"\x89PNG\r\n\x1a\noriginal-project-data",
            )
            manifest = json.loads(
                (staged / "lamapon-asset-conversions.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(manifest["assets"][0]["path"], "textures/road.png")
            self.assertEqual(manifest["assets"][0]["sourceFormat"], "png")
            self.assertEqual(manifest["assets"][0]["runtimeFormat"], "webp")

    def test_gif_conversion_uses_native_single_texture_first_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            textures = root / "assets" / "textures"
            textures.mkdir(parents=True)
            original = textures / "animated.gif"
            original.write_bytes(b"GIF89a-original-project-data")
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
            web = {
                "assetDirectory": "assets",
                "assetIncludePaths": ["textures"],
                "converterTools": {"imageMagick": str(converter)},
            }

            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                web,
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )

            converted = staged / "textures" / "animated.gif"
            self.assertEqual(converted.read_bytes()[8:12], b"WEBP")
            arguments = converter.with_suffix(".args").read_text(
                encoding="utf-8"
            ).splitlines()
            self.assertEqual(arguments[0], f"{original.resolve()}[0]")
            self.assertIn("webp:lossless=true", arguments)

    def test_asset_converter_wrong_signature_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.png"
            destination = root / "destination.png"
            source.write_bytes(b"\x89PNG\r\n\x1a\nsource")
            converter = root / "fake-magick"
            converter.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                "destination = sys.argv[-1].split(':', 1)[-1]\n"
                "pathlib.Path(destination).write_bytes(b'not-a-webp')\n",
                encoding="utf-8",
            )
            converter.chmod(0o755)

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

    def test_audio_conversion_stages_wav_bytes_at_original_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            audio = root / "assets" / "audio"
            audio.mkdir(parents=True)
            original = audio / "engine.ogg"
            original.write_bytes(b"OggS-original-project-data")
            converter = root / "fake-ffmpeg"
            converter.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                "pathlib.Path(sys.argv[-1]).write_bytes("
                "b'RIFF\\x00\\x00\\x00\\x00WAVEconverted')\n",
                encoding="utf-8",
            )
            converter.chmod(0o755)
            web = {
                "assetDirectory": "assets",
                "assetIncludePaths": ["audio"],
                "converterTools": {"ffmpeg": str(converter)},
            }

            staged = EXPORT_WEB.stage_portable_web_assets(
                root,
                web,
                "webgl2-basic-3d",
                root / ".lamapon" / "generated",
            )

            converted = staged / "audio" / "engine.ogg"
            self.assertEqual(converted.read_bytes()[0:4], b"RIFF")
            self.assertEqual(original.read_bytes(), b"OggS-original-project-data")
            manifest = json.loads(
                (staged / "lamapon-asset-conversions.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(manifest["assets"][0]["path"], "audio/engine.ogg")
            self.assertEqual(manifest["assets"][0]["runtimeFormat"], "wav")

    def test_portable_glb_without_images_needs_no_image_converter(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "triangle.glb"
            self._write_glb(model, {
                "asset": {"version": "2.0"},
                "accessors": [{"count": 3}],
                "meshes": [{"primitives": [{
                    "mode": 4,
                    "attributes": {"POSITION": 0},
                }]}],
            })

            generated = EXPORT_WEB.externalize_glb_images(model, None)
            details = EXPORT_WEB.validate_portable_glb(model)

            self.assertEqual(generated, [])
            self.assertEqual(details["primitives"], 1)
            self.assertEqual(details["images"], 0)

    def test_external_model_texture_is_rejected_instead_of_omitted(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "external.glb"
            self._write_glb(model, {
                "asset": {"version": "2.0"},
                "images": [{"uri": "missing.png"}],
            })

            with self.assertRaises(EXPORT_WEB.ExportError):
                EXPORT_WEB.externalize_glb_images(model, None)

    def test_embedded_model_texture_is_externalized_as_webp(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            model = root / "textured.glb"
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

            generated = EXPORT_WEB.externalize_glb_images(model, converter)

            self.assertEqual(len(generated), 1)
            self.assertEqual(generated[0].read_bytes()[8:12], b"WEBP")
            payload = model.read_bytes()
            json_length = struct.unpack_from("<I", payload, 12)[0]
            document = json.loads(payload[20:20 + json_length].decode("utf-8"))
            self.assertEqual(
                document["images"][0]["uri"],
                "textured.glb.image-0.webp",
            )
            self.assertNotIn("bufferView", document["images"][0])

    def test_portable_glb_rejects_material_features_it_would_lose(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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

            with self.assertRaises(EXPORT_WEB.ExportError):
                EXPORT_WEB.validate_portable_glb(model)

    def test_portable_glb_reports_supported_unlit_material(self):
        with tempfile.TemporaryDirectory() as directory:
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

            details = EXPORT_WEB.validate_portable_glb(model)

            self.assertEqual(details["materials"], 1)
            self.assertEqual(details["unlitMaterials"], 1)

    def test_portable_glb_accepts_uint32_sized_meshes(self):
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "large.glb"
            self._write_glb(model, {
                "asset": {"version": "2.0"},
                "accessors": [{"count": 70000}],
                "meshes": [{"primitives": [{
                    "mode": 4,
                    "attributes": {"POSITION": 0},
                }]}],
            })

            details = EXPORT_WEB.validate_portable_glb(model)

            self.assertEqual(details["vertices"], 70000)

    def test_scene_accepts_safe_cross_platform_utility_components(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer2d", "renderer3d"],
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )

            self.assertFalse(
                any(item["level"] == "reject" for item in findings),
                findings,
            )

    def test_scene_rejects_renderer_state_that_would_be_silently_lost(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer2d", "renderer3d"],
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            codes = {item["code"] for item in findings}

            self.assertIn("unsupported-camera-render-texture", codes)
            self.assertIn("unsupported-mesh-custom-shader", codes)
            self.assertIn("unsupported-world-overlay", codes)
            self.assertIn("unsupported-sprite-render-texture", codes)

    def test_scene_feature_contract_rejects_silent_model_and_physics_loss(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
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
            project = self._portable_fixture(
                root,
                scene=scene,
                modules=["core", "input", "renderer3d", "physics3d"],
            )

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            codes = {item["code"] for item in findings}

            self.assertIn("unsupported-model-wireframe", codes)
            self.assertIn("unsupported-animation-controller", codes)
            self.assertIn("unsupported-root-motion", codes)
            self.assertIn("unsupported-advanced-rigidbody", codes)

    def test_material_contract_checks_nested_assets_and_custom_shader(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = self._portable_fixture(
                root,
                asset_include_paths=["scenes", "materials"],
            )
            materials = root / "assets" / "materials"
            materials.mkdir()
            (materials / "car.material.json").write_text(json.dumps({
                "type": "LamaPonLitMaterial",
                "version": 2,
                "albedoTexture": "textures/missing.png",
                "shader": "shaders/CustomCar.hlsl",
            }), encoding="utf-8")

            findings = EXPORT_WEB.validate_web_compatibility(
                root, project, "webgl2-basic-3d", "lamapon-web-target"
            )
            codes = {item["code"] for item in findings}

            self.assertIn("missing-material-asset", codes)
            self.assertIn("unsupported-custom-material-shader", codes)

    def test_package_removes_previous_manifest_artifact_after_rename(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            output = root / "output"
            build.mkdir()
            output.mkdir()
            old_html = output / "OldName.html"
            old_html.write_text("old", encoding="utf-8")
            (output / "web-export-manifest.json").write_text(
                '{"artifacts":[{"path":"OldName.html"}]}',
                encoding="utf-8",
            )
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


if __name__ == "__main__":
    unittest.main()
