"""Inspect existing fixtures and exercise generated text in memory; no test files are created."""
import importlib.util
import io
import json
from pathlib import Path
import re
import sys
import unittest
from xml.etree import ElementTree
from unittest import mock
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("lamapon_export_native", ROOT / "tools/export_native.py")
NATIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(NATIVE)
import editor_native_export as EDITOR_NATIVE


def public_class_methods(path, class_name):
    source = path.read_text(encoding="utf-8")
    code = NATIVE.portable.mask_cpp_non_code(source)
    declaration = re.search(
        r"\bclass\s+" + re.escape(class_name) + r"\b[^{};]*\{", code
    )
    if declaration is None:
        raise AssertionError(f"Missing {class_name} definition in {path}")
    opening = code.index("{", declaration.start())
    depth = 0
    closing = None
    for offset in range(opening, len(code)):
        if code[offset] == "{":
            depth += 1
        elif code[offset] == "}":
            depth -= 1
            if depth == 0:
                closing = offset
                break
    if closing is None:
        raise AssertionError(f"Unclosed {class_name} definition in {path}")
    body = code[opening + 1:closing].split("\n    private:", 1)[0]
    names = set()
    index = 0
    brace_depth = 0
    while index < len(body):
        if body[index] == "{":
            brace_depth += 1
            index += 1
            continue
        if body[index] == "}":
            brace_depth -= 1
            index += 1
            continue
        if brace_depth == 0 and body[index] == "(":
            prefix = re.search(r"(~?[A-Za-z_]\w*)\s*$", body[:index])
            if prefix:
                paren_depth = 1
                end = index + 1
                while end < len(body) and paren_depth:
                    if body[end] == "(":
                        paren_depth += 1
                    elif body[end] == ")":
                        paren_depth -= 1
                    end += 1
                terminator = end
                while terminator < len(body) and body[terminator] not in "{;":
                    terminator += 1
                if terminator < len(body):
                    names.add(prefix.group(1))
                    if body[terminator] == "{":
                        nested_depth = 1
                        terminator += 1
                        while terminator < len(body) and nested_depth:
                            if body[terminator] == "{":
                                nested_depth += 1
                            elif body[terminator] == "}":
                                nested_depth -= 1
                            terminator += 1
                    index = terminator + 1
                    continue
        index += 1
    return names - {class_name, "~" + class_name}


def class_string_return(path, class_name, method_name):
    source = path.read_text(encoding="utf-8")
    code = NATIVE.portable.mask_cpp_non_code(source, mask_literals=False)
    declaration = re.search(
        r"\bclass\s+" + re.escape(class_name) + r"\b[^{};]*\{", code
    )
    if declaration is None:
        return None
    opening = code.index("{", declaration.start())
    depth = 0
    closing = None
    for offset in range(opening, len(code)):
        if code[offset] == "{":
            depth += 1
        elif code[offset] == "}":
            depth -= 1
            if depth == 0:
                closing = offset
                break
    if closing is None:
        raise AssertionError(f"Unclosed {class_name} definition in {path}")
    body = code[opening + 1:closing]
    result = re.search(
        r"\b" + re.escape(method_name)
        + r"\s*\(\s*\)[^{;]*\{[^{}]*return\s+\"([^\"]+)\"",
        body,
        re.DOTALL,
    )
    return result.group(1) if result else None


class NativeExportToolTests(unittest.TestCase):
    def test_native_inspection_rejects_macos_and_non_platform_targets(self):
        for platform in ("macos", "darwin", "steam", "steam-deck"):
            with self.subTest(platform=platform), self.assertRaisesRegex(
                NATIVE.portable.ExportError,
                "Native output supports only: windows, linux, android",
            ):
                NATIVE.inspect_project(Path("missing-project.json"), platform)

    def test_wsl_linux_export_helpers_are_in_the_installed_sdk_tool_list(self):
        distribution = (ROOT / "cmake/LamaPonDistribution.cmake").read_text(encoding="utf-8")
        self.assertIn("tools/editor_linux_export.py", distribution)
        self.assertIn("tools/editor_linux_build.py", distribution)

    def test_generated_target_names_are_accepted_by_build_metadata_reader(self):
        import build_native
        directory = ROOT / "unused-native-output"
        description = {"format": "lamapon.native-build-project", "version": 2,
                       "platform": "windows", "scenePath": "/assets/scenes/Main.scene.json",
                       "assetDirectory": "assets", "assetIncludePaths": ["scenes"]}
        for name in ("sdk-export-fixture", "game.v2+test", "日本語ゲーム"):
            description["target"] = NATIVE.portable.safe_cmake_target("LamaPonNative_" + name)
            with self.subTest(name=name), mock.patch.object(Path, "is_file", return_value=True), \
                    mock.patch.object(Path, "read_text", return_value=json.dumps(description)):
                self.assertEqual(build_native.load_project(directory)["target"], description["target"])
        for target in ("../game", "game;command", "game/part", "game\\part", "game:part", "-game"):
            description["target"] = target
            with self.subTest(target=target), \
                    mock.patch.object(Path, "read_text", return_value=json.dumps(description)):
                with self.assertRaises(NATIVE.portable.ExportError):
                    build_native.load_project(directory)

    def test_cli_json_and_errors_are_utf8_under_windows_code_pages(self):
        message = "保存先 / ゲーム 🚀"
        for encoding in ("cp932", "cp1252"):
            output, errors = io.BytesIO(), io.BytesIO()
            stdout = io.TextIOWrapper(output, encoding=encoding)
            stderr = io.TextIOWrapper(errors, encoding=encoding)
            with self.subTest(encoding=encoding), \
                    mock.patch.object(sys, "stdout", stdout), mock.patch.object(sys, "stderr", stderr):
                NATIVE.portable.configure_cli_output()
                print(json.dumps({"message": message}, ensure_ascii=False), flush=True)
                print(message, file=sys.stderr, flush=True)
                self.assertEqual(json.loads(output.getvalue().decode("utf-8"))["message"], message)
                self.assertEqual(errors.getvalue().decode("utf-8").strip(), message)

    def test_reserved_generated_assets_are_rejected_before_build(self):
        asset_root = ROOT / "tests/native/assets"
        original_rglob, original_is_file = Path.rglob, Path.is_file
        for platform in ("windows", "linux", "android"):
            for name in ("lamapon-default-font.ttf", "lamapon-input-actions.json",
                         "lamapon-default-font.ttf/nested.json"):
                conflict = asset_root / name
                def files(path, pattern):
                    result = list(original_rglob(path, pattern))
                    return iter(result + [conflict]) if path == asset_root else iter(result)
                with self.subTest(platform=platform, name=name), \
                        mock.patch.object(Path, "rglob", files), \
                        mock.patch.object(Path, "is_file", lambda path: path == conflict or original_is_file(path)), \
                        mock.patch.object(NATIVE.portable, "web_asset_roots", return_value=(asset_root, [asset_root])), \
                        mock.patch.object(NATIVE.portable, "validate_portable_contract", return_value=[]):
                    report, _ = self.inspect_fixture(platform=platform)
                    self.assertFalse(report["canGenerateBuildProject"])
                    self.assertTrue(any(item["code"] == "reserved-native-asset" for item in report["findings"]))

    def test_unselected_reserved_asset_does_not_block_export(self):
        asset_root = ROOT / "tests/native/assets"
        conflict = asset_root / "lamapon-input-actions.json"
        original_rglob, original_is_file = Path.rglob, Path.is_file
        original_roots = NATIVE.portable.web_asset_roots
        def files(path, pattern):
            result = list(original_rglob(path, pattern))
            return iter(result + [conflict]) if path == asset_root else iter(result)
        def selection(root, settings):
            base, _ = original_roots(root, settings)
            return base, [base / "scenes", base / "audio"]
        with mock.patch.object(Path, "rglob", files), \
                mock.patch.object(Path, "is_file", lambda path: path == conflict or original_is_file(path)), \
                mock.patch.object(NATIVE.portable, "web_asset_roots", selection), \
                mock.patch.object(NATIVE.portable, "validate_portable_contract", return_value=[]):
            report, _ = self.inspect_fixture(platform="android")
            self.assertTrue(report["canGenerateBuildProject"], report["findings"])

    def test_legacy_windows_package_dependencies_are_only_accepted_for_windows_target(self):
        manifest = json.dumps({
            "name": "discord-presence-sdk",
            "native": {
                "libraries": ["sdk/lib/discord_partner_sdk.lib"],
                "runtimeFiles": ["sdk/bin/discord_partner_sdk.dll"],
            },
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture_with_package_manifest(platform, manifest)
                self.assertFalse(report["canGenerateBuildProject"])
                expected_code = "invalid-native-package-path" if platform == "windows" else "unresolved-native-package-dependency"
                dependency = next(item for item in report["findings"] if item["code"] == expected_code)
                if platform != "windows":
                    self.assertIn("nativeVariants." + platform, dependency["action"])

    def test_native_package_target_variants_resolve_for_all_selected_platforms(self):
        manifest = json.dumps({
            "name": "fixture-package",
            "nativeVariants": {
                "windows-x86_64": {"includeDirectories": ["windows/include"],
                    "libraries": ["windows/lib/fixture.lib"], "runtimeFiles": ["windows/bin/fixture.dll"],
                    "defines": ["FIXTURE_ENABLED=1"], "licenseFiles": ["windows/LICENSE.txt"]},
                "linux-x86_64": {"libraries": ["linux/lib/libfixture.so"],
                    "licenseFiles": ["linux/LICENSE.txt"]},
                "android-arm64-v8a": {"libraries": ["android/arm64/libfixture.so"],
                    "licenseFiles": ["android/LICENSE.txt"]},
                "android-x86_64": {"libraries": ["android/x64/libfixture.so"],
                    "licenseFiles": ["android/LICENSE.txt"]},
            },
        })
        paths = ["windows/include", "windows/lib/fixture.lib", "windows/bin/fixture.dll", "windows/LICENSE.txt",
                 "linux/lib/libfixture.so", "linux/LICENSE.txt", "android/arm64/libfixture.so",
                 "android/x64/libfixture.so", "android/LICENSE.txt"]
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, context = self.inspect_fixture_with_package_manifest(platform, manifest, paths)
                self.assertTrue(report["canGenerateBuildProject"], report["findings"])
                target = {"windows": "windows-x86_64", "linux": "linux-x86_64"}.get(platform)
                if target:
                    dependency = context["package_dependencies"][target][0]
                    self.assertEqual(dependency["package"], "fixture-package")
                    self.assertTrue(dependency["licenseFiles"])
                else:
                    self.assertEqual(set(context["package_dependencies"]),
                                     {"android-arm64-v8a", "android-x86_64"})

    def test_empty_native_package_settings_do_not_block_generation(self):
        report, _ = self.inspect_fixture_with_package_manifest(
            "linux", json.dumps({"name": "assets-only", "native": {}}))
        self.assertTrue(report["canGenerateBuildProject"], report["findings"])

    def test_malformed_native_package_settings_are_rejected(self):
        report, _ = self.inspect_fixture_with_package_manifest(
            "android", json.dumps({"name": "broken-package", "native": None}))
        self.assertFalse(report["canGenerateBuildProject"])
        self.assertTrue(any(item["code"] == "invalid-native-package-manifest"
                            for item in report["findings"]))

    def test_native_package_variant_rejects_path_escape_and_wrong_library_format(self):
        for relative, package_paths, expected in (
                ("../sibling/libfixture.so", [], "invalid-native-package-path"),
                ("linux/libfixture.dll", ["linux/libfixture.dll"], "invalid-native-package-library")):
            manifest = json.dumps({"nativeVariants": {"linux-x86_64": {"libraries": [relative]}}})
            with self.subTest(path=relative):
                report, _ = self.inspect_fixture_with_package_manifest("linux", manifest, package_paths)
                self.assertFalse(report["canGenerateBuildProject"])
                self.assertIn(expected, [finding["code"] for finding in report["findings"]])

    def test_package_manifest_must_resolve_inside_its_own_package_directory(self):
        manifest_path = ROOT / "tests/native/assets/packages/fixture-package/package.json"
        escaped_manifest = ROOT / "tests/native/assets/packages/another-package/package.json"
        original_resolve = Path.resolve

        def redirect_manifest(path, strict=False):
            if path == manifest_path:
                return escaped_manifest
            return original_resolve(path, strict=strict)

        with mock.patch.object(Path, "resolve", redirect_manifest):
            report, _ = self.inspect_fixture_with_package_manifest(
                "linux", json.dumps({"nativeVariants": {"linux-x86_64": {}}}))

        self.assertFalse(report["canGenerateBuildProject"])
        self.assertTrue(any(item["code"] == "native-package-path-escape" for item in report["findings"]))

    def test_android_package_licenses_are_collision_checked_across_abis(self):
        dependencies = {
            "android-arm64-v8a": [{"sources": [], "licenseFiles": [{
                "source": "assets/packages/Foo/android/arm64/LICENSE.txt",
                "destination": "packages/Foo/LICENSE.txt"}]}],
            "android-x86_64": [{"sources": [], "licenseFiles": [{
                "source": "assets/packages/foo/android/x64/LICENSE.txt",
                "destination": "packages/foo/LICENSE.txt"}]}],
        }
        with mock.patch.object(NATIVE, "native_package_dependencies", return_value=([], dependencies)):
            report, _ = self.inspect_fixture(platform="android")
        self.assertFalse(report["canGenerateBuildProject"])
        self.assertTrue(any(finding["code"] == "duplicate-native-package-license"
                            for finding in report["findings"]))

        shared_license = "assets/packages/fixture-package/LICENSE.txt"
        dependencies["android-x86_64"][0]["licenseFiles"][0]["source"] = shared_license
        dependencies["android-arm64-v8a"][0]["licenseFiles"][0]["source"] = shared_license
        self.assertEqual(NATIVE.android_package_license_findings(dependencies), [])

    def test_android_package_requires_each_configured_abi_variant(self):
        manifest = json.dumps({"nativeVariants": {
            "android-arm64-v8a": {"libraries": ["android/arm64/libfixture.so"]}}})
        report, _ = self.inspect_fixture_with_package_manifest(
            "android", manifest, ["android/arm64/libfixture.so"])
        self.assertFalse(report["canGenerateBuildProject"])
        self.assertTrue(any("android-x86_64" in item["message"]
                            for item in report["findings"]
                            if item["code"] == "unresolved-native-package-dependency"))

    def test_native_output_allows_checked_literal_dynamic_prefab_instantiation(self):
        source = (
            'LAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");\n'
            'LAMAPON_SCRIPT_NAMED(PrefabProbe, "Test.PortablePrefabProbe", "Prefab probe");\n'
            "class Spawner : public LamaPon::Script {\n"
            '    void Update(float) override { Instantiate("prefabs/PortableProbe.prefab.json"); }\n'
            "};\n"
        )
        report, _ = self.inspect_fixture(platform="linux", text=source)
        self.assertTrue(report["canGenerateBuildProject"], report["findings"])

    def test_native_output_rejects_nonliteral_dynamic_prefab_paths(self):
        source = (
            'LAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");\n'
            "class Spawner : public LamaPon::Script {\n"
            "    void Update(float) override { Instantiate(prefabPath); }\n"
            "};\n"
        )
        report, _ = self.inspect_fixture(platform="android", text=source)
        self.assertFalse(report["canGenerateBuildProject"])
        self.assertTrue(any(item["code"] == "invalid-portable-prefab-reference"
                            for item in report["findings"]))

    def test_native_output_rejects_unimplemented_scene_manager_methods(self):
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
        source = (
            'LAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");\n'
            "class Spawner : public LamaPon::Script {\n"
            "    void Update(float) override {\n"
            + calls
            + "\n    }\n};\n"
        )
        report, _ = self.inspect_fixture(platform="linux", text=source)
        self.assertFalse(report["canGenerateBuildProject"])
        findings = [item for item in report["findings"]
                    if item["code"] == "unsupported-portable-scene-api"]
        self.assertEqual(len(findings), len(methods), report["findings"])
        self.assertEqual(
            {method for method in methods
             if any(f"{method}はPortable" in item["message"] for item in findings)},
            set(methods), report["findings"],
        )

    def test_all_native_targets_report_ignored_scene_object_features(self):
        scene = json.dumps({
            "format": "LamaPonScene",
            "mainCamera": 1,
            "objects": [{
                "id": 1,
                "name": "Camera",
                "tag": "camera",
                "persistent": True,
                "persistenceKey": "main-camera",
                "alwaysVisible": True,
                "cullingMargin": 2.0,
                "components": [{"type": "Camera"}],
            }],
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                codes = {finding["code"] for finding in report["findings"]}
                self.assertIn("portable-object-persistence-ignored", codes)
                self.assertIn("portable-legacy-render-culling-ignored", codes)

    def test_native_outputs_reject_model_backed_mesh_collider_scenes(self):
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
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                self.assertFalse(report["canGenerateBuildProject"], report["findings"])
                collider = next(
                    item for item in report["findings"]
                    if item["code"] == "unsupported-scene-mesh-collider"
                )
                self.assertEqual(collider["level"], "reject")

    def test_native_targets_share_portable_environment_compatibility_warnings(self):
        scene = json.dumps({
            "format": "LamaPonScene",
            "mainCamera": 1,
            "environment": {"colorGrading": {"enabled": True}},
            "objects": [{
                "id": 1,
                "name": "Camera",
                "components": [{"type": "Camera"}],
            }],
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                warning = next(item for item in report["findings"]
                               if item["code"] == "unsupported-environment-effect")
                self.assertTrue(report["canGenerateBuildProject"], report["findings"])
                self.assertEqual(warning["level"], "warning")
                self.assertIn("Portable", warning["message"])

    def test_native_targets_reject_malformed_portable_scene_values(self):
        scene = json.dumps({
            "format": "LamaPonScene",
            "mainCamera": 1,
            "objects": [{
                "id": 1,
                "name": "Camera",
                "transform": {"position": [0, "invalid", 0]},
                "components": [{
                    "type": "Camera",
                    "nearPlane": "close",
                }],
            }],
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                self.assertFalse(report["canGenerateBuildProject"], report["findings"])
                invalid = [item for item in report["findings"]
                           if item["code"] == "invalid-scene-setting"]
                self.assertTrue(any(
                    "nearPlane" in item["message"] for item in invalid
                ), report["findings"])
                self.assertTrue(any(
                    "transform.position" in item["message"] for item in invalid
                ), report["findings"])

    def test_native_targets_explain_basic_box_physics_limits(self):
        scene = json.dumps({
            "format": "LamaPonScene",
            "mainCamera": 1,
            "objects": [{
                "id": 1,
                "name": "Camera",
                "components": [{"type": "Camera"}],
            }, {
                "id": 2,
                "name": "Floor",
                "components": [{
                    "type": "BoxCollider3D",
                    "friction": 0.9,
                    "restitution": 0.8,
                    "frictionCombine": 3,
                    "restitutionCombine": 2,
                }],
            }],
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                warning = next(item for item in report["findings"]
                               if item["code"] == "portable-basic-box-physics")
                self.assertTrue(report["canGenerateBuildProject"], report["findings"])
                self.assertEqual(warning["level"], "warning")
                self.assertIn("摩擦", warning["action"])
                self.assertIn("反発", warning["action"])

    def test_native_targets_report_ignored_rigidbody_interpolation(self):
        scene = json.dumps({
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
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                warning = next(item for item in report["findings"]
                               if item["code"] == "portable-rigidbody-interpolation-ignored")
                self.assertTrue(report["canGenerateBuildProject"], report["findings"])
                self.assertEqual(warning["level"], "warning")
                self.assertIn("Transform", warning["message"])
                self.assertIn("Portable出力", warning["action"])

        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform, invalid="string"):
                invalid_scene = json.loads(scene)
                invalid_scene["objects"][1]["components"][0]["interpolate"] = "false"
                report, _ = self.inspect_fixture(
                    platform=platform, scene=json.dumps(invalid_scene))
                self.assertFalse(report["canGenerateBuildProject"], report["findings"])
                self.assertTrue(any(
                    item["code"] == "invalid-rigidbody-interpolation"
                    and item["level"] == "reject"
                    for item in report["findings"]), report["findings"])

    def test_native_compatibility_findings_use_native_or_portable_wording(self):
        scene = json.dumps({
            "format": "LamaPonScene",
            "mainCamera": 1,
            "objects": [{
                "id": 1,
                "name": "Camera",
                "components": [{"type": "Camera", "targetTexture": "render-target"}],
            }, {
                "id": 2,
                "name": "UI",
                "components": [
                    {"type": "UIImage", "border": [1, 1, 1, 1]},
                    {"type": "UIButton", "targetScene": "other"},
                ],
            }, {
                "id": 3,
                "name": "Body",
                "parent": 1,
                "components": [
                    {"type": "MeshCollider3D"},
                    {"type": "Rigidbody", "mass": 4.0},
                ],
            }, {
                "id": 4,
                "name": "Particles",
                "components": [{"type": "ParticleSystem", "renderMode": "Trail"}],
            }, {
                "id": 5,
                "name": "Model",
                "components": [{"type": "ModelRenderer", "wireframe": True}],
            }, {
                "id": 6,
                "name": "Text",
                "components": [{"type": "TextRenderer", "fontFamily": "Custom Sans"}],
            }, {
                "id": 7,
                "name": "Sprite",
                "components": [{"type": "SpriteRenderer", "shader": "custom.hlsl"}],
            }, {
                "id": 8,
                "name": "Animation",
                "components": [{"type": "TransformAnimator", "controller": "controller.json"}],
            }, {
                "id": 9,
                "name": "Mover",
                "components": [{"type": "InputMover", "horizontalAction": "Unknown"}],
            }],
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                messages = "\n".join(
                    item[key] for item in report["findings"] for key in ("message", "action"))
                self.assertNotIn("Web", messages)
                self.assertNotIn("ブラウザー", messages)

    def test_native_targets_report_ignored_global_physics_settings(self):
        physics = {
            "gravity": {"x": 1.0, "y": 0.0, "z": -2.0},
            "fixedTimeStep": 1.0 / 120.0,
            "collisionOff": [[2, 4]],
            "clampDiscreteSpeed": True,
        }
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(
                    platform=platform, project_settings={"physics": physics})
                warning = next(item for item in report["findings"]
                               if item["code"] == "portable-project-physics-approximation")
                self.assertTrue(report["canGenerateBuildProject"], report["findings"])
                self.assertEqual(warning["level"], "warning")
                self.assertIn("gravity", warning["message"])

    def test_native_targets_share_portable_local_light_limit_diagnostics(self):
        objects = [{
            "id": 1,
            "name": "Camera",
            "components": [{"type": "Camera"}],
        }]
        objects.extend({
            "id": index,
            "name": f"Light {index}",
            "components": [{"type": "PointLight"}],
        } for index in range(2, 11))
        scene = json.dumps({
            "format": "LamaPonScene",
            "mainCamera": 1,
            "objects": objects,
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                warning = next(item for item in report["findings"]
                               if item["code"] == "portable-local-light-limit")
                self.assertTrue(report["canGenerateBuildProject"], report["findings"])
                self.assertIn("Portable renderer", warning["message"])
                self.assertNotIn("Web版", warning["message"])

    def test_native_targets_use_their_own_spatial_audio_diagnostic(self):
        scene = json.dumps({
            "format": "LamaPonScene",
            "mainCamera": 1,
            "objects": [{
                "id": 1,
                "name": "Camera",
                "components": [{"type": "Camera"}],
            }, {
                "id": 2,
                "name": "Ambient",
                "components": [{
                    "type": "AudioSource",
                    "spatial": True,
                    "streaming": True,
                }],
            }],
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                by_code = {item["code"]: item for item in report["findings"]}
                spatial = by_code["native-spatial-audio-approximation"]
                buffered = by_code["portable-audio-buffered-stream"]
                self.assertEqual(spatial["level"], "warning")
                self.assertIn("HRTF", spatial["message"])
                self.assertIn("Portable", buffered["message"])
                self.assertNotIn("browser", buffered["message"].lower())

    def test_native_targets_warn_when_audio_bus_routing_is_ignored(self):
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
            }],
        })
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(platform=platform, scene=scene)
                warning = next(item for item in report["findings"]
                               if item["code"] == "portable-audio-bus-approximation")
                self.assertEqual(warning["level"], "warning")
                self.assertIn("個別バス", warning["message"])
                self.assertNotIn("Web版", warning["message"])


    def test_reserved_names_follow_target_filesystem_case_rules(self):
        asset_root = ROOT / "tests/native/assets"
        conflict = asset_root / "LAMAPON-DEFAULT-FONT.TTF"
        original_rglob, original_is_file = Path.rglob, Path.is_file
        def files(path, pattern):
            result = list(original_rglob(path, pattern))
            return iter(result + [conflict]) if path == asset_root else iter(result)
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform), mock.patch.object(Path, "rglob", files), \
                    mock.patch.object(Path, "is_file", lambda path: path == conflict or original_is_file(path)), \
                    mock.patch.object(NATIVE.portable, "web_asset_roots", return_value=(asset_root, [asset_root])), \
                    mock.patch.object(NATIVE.portable, "validate_portable_contract", return_value=[]):
                report, _ = self.inspect_fixture(platform=platform)
                collision = any(item["code"] == "reserved-native-asset" for item in report["findings"])
                self.assertEqual(collision, platform == "windows")

    def inspect_fixture(self, text='LAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");', platform="linux", source="NativeSmoke.cpp", scene=None, project_settings=None):
        # Read the real Scene; project settings and game source content stay in memory.
        project_file = ROOT / "tests/native/CMakeLists.txt"
        source_file = ROOT / "tests/native" / source
        scene_file = ROOT / "tests/native/assets/scenes/Main.scene.json"
        read = Path.read_text
        project = {"format": "LamaPonProject", "name": "Native fixture",
                   "export": {"native": {"sources": [source]}},
                   **(project_settings or {})}

        def replacement(path, *args, **kwargs):
            if path == project_file:
                return json.dumps(project)
            if path == source_file and text is not None:
                return text
            if path == scene_file and scene is not None:
                return scene
            return read(path, *args, **kwargs)

        with mock.patch.object(Path, "read_text", replacement):
            return NATIVE.inspect_project(project_file, platform)

    def inspect_fixture_with_package_manifest(self, platform, manifest_text, package_paths=()):
        assets = ROOT / "tests/native/assets"
        packages_root = assets / "packages"
        package_directory = packages_root / "fixture-package"
        manifest_path = package_directory / "package.json"
        original_iterdir = Path.iterdir
        original_is_dir = Path.is_dir
        original_is_file = Path.is_file
        original_read_text = Path.read_text
        original_exact_relative_path = NATIVE.exact_relative_path

        def iterdir(path):
            if path == packages_root:
                return iter([package_directory])
            return original_iterdir(path)

        def is_dir(path):
            if path in (packages_root, package_directory):
                return True
            return original_is_dir(path)

        def is_file(path):
            if path == manifest_path:
                return True
            return original_is_file(path)

        def read_text(path, *args, **kwargs):
            if path == manifest_path:
                return manifest_text
            if path in virtual_paths and path.suffix.lower() in {".cc", ".cpp", ".cxx"}:
                return 'LAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");'
            return original_read_text(path, *args, **kwargs)

        virtual_paths = {package_directory / value for value in package_paths}
        virtual_directories = {path for path in virtual_paths if path.suffix == ""}

        def is_virtual_dir(path):
            if path in virtual_directories or path in (packages_root, package_directory):
                return True
            return original_is_dir(path)

        def is_virtual_file(path):
            if path in virtual_paths and path not in virtual_directories:
                return True
            return original_is_file(path)

        def exact_path(root, relative):
            candidate = root / relative
            if root == package_directory:
                return candidate if candidate in virtual_paths else None
            return original_exact_relative_path(root, relative)

        with mock.patch.object(Path, "iterdir", iterdir), \
             mock.patch.object(Path, "is_dir", is_virtual_dir), \
             mock.patch.object(Path, "is_file", lambda path: path == manifest_path or is_virtual_file(path) or original_is_file(path)), \
             mock.patch.object(Path, "read_text", read_text), \
             mock.patch.object(NATIVE, "exact_relative_path", exact_path):
            return self.inspect_fixture(platform=platform)

    def test_portable_scene_can_prepare_build_metadata_without_claiming_verification(self):
        report, context = self.inspect_fixture()
        self.assertTrue(report["canGenerateBuildProject"], report["findings"])
        self.assertFalse(report["verified"])
        self.assertEqual(context["settings"]["scenePath"], "/assets/scenes/Main.scene.json")

    def test_real_export_game_source_passes_shared_portable_contract(self):
        for platform in ("windows", "linux", "android"):
            report, context = self.inspect_fixture(text=None, platform=platform, source="ExportSmoke.cpp")
            self.assertTrue(report["canGenerateBuildProject"], report["findings"])
            self.assertEqual(context["sources"], [ROOT / "tests/native/ExportSmoke.cpp"])
            complete = next(item for item in report["findings"] if item["code"] == "portable-contract-complete")
            self.assertIn("Portable互換性チェック", complete["message"])
            self.assertNotIn("Emscripten", complete["action"])
            self.assertIn("Android SDK・NDK" if platform == "android" else platform, complete["action"])
            self.assertFalse(report["verified"])

    def test_native_export_recognizes_default_script_registration_macro(self):
        source = (
            '#include "LamaPon/LamaPon.h"\n'
            'class ProbeScript final : public LamaPon::Script {};\n'
            'LAMAPON_SCRIPT(ProbeScript);\n'
            'void Spawn(LamaPon::GameObject& object) {\n'
            '    object.AddComponent<LamaPon::NativeScriptComponent>("Game.ProbeScript");\n'
            '}\n'
        )
        scene_file = ROOT / "tests/native/assets/scenes/Main.scene.json"
        scene = json.loads(scene_file.read_text(encoding="utf-8"))
        native_script = next(
            component for component in scene["objects"][0]["components"]
            if component["type"] == "NativeScript"
        )
        native_script["script"] = "Game.ProbeScript"
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(
                    text=source, scene=json.dumps(scene, ensure_ascii=False), platform=platform
                )
                self.assertTrue(report["canGenerateBuildProject"], report["findings"])
                self.assertFalse(any(item["code"] in {
                    "unregistered-scene-script", "unregistered-dynamic-script"
                } for item in report["findings"]), report["findings"])

    def test_public_portable_value_and_base_types_are_recognized(self):
        public_types = {
            "GameObjectId": "core",
            "CompositeSubscription": "core",
            "EventArgs": "core",
            "EventBus": "core",
            "Observable": "core",
            "PortableKeyboardState": "input",
            "PortableLocalLightComponent": "renderer3d",
            "PortableUIVisualComponent": "renderer2d",
            "RuntimeState": "core",
            "SceneCollection": "core",
            "ScriptFactory": "core",
            "Subscription": "core",
            "Transform": "core",
            "UIRect": "renderer2d",
        }
        self.assertEqual(
            {name: NATIVE.portable.PORTABLE_API_MODULES.get(name) for name in public_types},
            public_types,
        )
        source = ('LAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");\n' +
                  "\n".join(f"LamaPon::{name}* portable_{name.lower()}{{}};" for name in public_types))
        report, _ = self.inspect_fixture(text=source)
        self.assertTrue(report["canGenerateBuildProject"], report["findings"])
        self.assertFalse(any(item["code"] == "unsupported-portable-api" for item in report["findings"]))

    def test_windows_only_script_methods_are_all_classified_for_portable_exports(self):
        native_header = ROOT / "src/LamaPon/Scripting/Script.h"
        portable_header = ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h"
        native_methods = public_class_methods(native_header, "Script")
        portable_methods = public_class_methods(portable_header, "Script")
        unsupported = set(NATIVE.portable.PORTABLE_UNSUPPORTED_SCRIPT_METHODS)
        self.assertFalse(native_methods - portable_methods - unsupported)
        self.assertFalse(portable_methods & unsupported)

    def test_windows_only_scene_methods_are_all_classified_for_portable_exports(self):
        native_header = ROOT / "src/LamaPon/Scene/Scene.h"
        portable_header = ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h"
        native_methods = public_class_methods(native_header, "Scene")
        portable_methods = public_class_methods(portable_header, "Scene")
        unsupported = set(NATIVE.portable.PORTABLE_UNSUPPORTED_SCENE_METHODS)
        typed_only = set(NATIVE.portable.PORTABLE_UNSUPPORTED_SCENE_TYPED_METHODS)
        self.assertFalse(native_methods - portable_methods - unsupported - typed_only)
        self.assertFalse(portable_methods & unsupported)
        self.assertFalse(portable_methods & typed_only)

        native_manager = public_class_methods(
            ROOT / "src/LamaPon/Scene/SceneManager.h", "SceneManager"
        )
        portable_collection = public_class_methods(portable_header, "SceneCollection")
        self.assertFalse(native_manager - portable_collection - unsupported)
        self.assertFalse(portable_collection & unsupported)

    def test_windows_gameobject_methods_are_all_classified_for_portable_exports(self):
        native_header = ROOT / "src/LamaPon/Scene/GameObject.h"
        portable_header = ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h"
        native_methods = public_class_methods(native_header, "GameObject")
        portable_methods = public_class_methods(portable_header, "GameObject")
        unsupported = set(
            NATIVE.portable.PORTABLE_UNSUPPORTED_GAMEOBJECT_METHODS
        )
        self.assertFalse(native_methods - portable_methods - unsupported)
        self.assertFalse(portable_methods & unsupported)

    def test_text_renderer_script_api_matches_the_windows_contract(self):
        native_header = ROOT / "src/LamaPon/Components/TextRendererComponent.h"
        portable_header = ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h"
        native_methods = public_class_methods(
            native_header, "TextRendererComponent"
        )
        portable_methods = public_class_methods(
            portable_header, "TextRendererComponent"
        )
        script_api = {
            "SetText", "Text", "SetFontFamily", "FontFamily",
            "SetFontSize", "FontSize", "SetColor", "Color",
            "SetLayoutSize", "LayoutSize", "SetWordWrap", "WordWrap",
            "SetHorizontalAlignment", "HorizontalAlignment",
            "SetVerticalAlignment", "VerticalAlignment",
            "SetSortOrder", "SortOrder",
        }
        self.assertFalse(script_api - native_methods)
        self.assertFalse(script_api - portable_methods)

    def test_common_component_accessors_match_the_windows_contract(self):
        native_header = ROOT / "src/LamaPon/Scene/Component.h"
        portable_header = ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h"
        native_methods = public_class_methods(native_header, "Component")
        portable_methods = public_class_methods(portable_header, "Component")
        shared_api = {
            "Owner", "GetTransform", "IsEnabled", "SetEnabled",
            "IsActiveAndEnabled", "TypeName", "ScriptInstance",
        }
        self.assertFalse(shared_api - native_methods)
        self.assertFalse(shared_api - portable_methods)

    def test_basic_physics_component_apis_match_the_windows_contract(self):
        portable_header = ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h"
        box_api = {
            "Size", "SetSize", "Offset", "SetOffset", "IsTrigger",
            "SetTrigger", "Layer", "SetLayer", "CollisionMask",
            "SetCollisionMask", "TypeName",
        }
        box_native = public_class_methods(
            ROOT / "src/LamaPon/Components/BoxCollider3DComponent.h",
            "BoxCollider3DComponent",
        )
        box_portable = public_class_methods(
            portable_header, "BoxCollider3DComponent"
        )
        self.assertFalse(box_api - box_native)
        self.assertFalse(box_api - box_portable)

        rigidbody_api = {
            "IsKinematic", "SetKinematic", "UsesGravity", "SetUseGravity",
            "Velocity", "SetVelocity", "TypeName",
        }
        body_native = public_class_methods(
            ROOT / "src/LamaPon/Components/RigidbodyComponent.h",
            "RigidbodyComponent",
        )
        body_portable = public_class_methods(portable_header, "RigidbodyComponent")
        self.assertFalse(rigidbody_api - body_native)
        self.assertFalse(rigidbody_api - body_portable)

        unsupported = NATIVE.portable.PORTABLE_UNSUPPORTED_COMPONENT_METHODS
        hook_methods = {"OnRenderDebug3D"}
        for name, header in (
            ("AudioSourceComponent", "Components/AudioSourceComponent.h"),
            ("BoxCollider3DComponent", "Components/BoxCollider3DComponent.h"),
            ("RigidbodyComponent", "Components/RigidbodyComponent.h"),
        ):
            native = public_class_methods(
                ROOT / "src/LamaPon" / header, name
            )
            portable = public_class_methods(portable_header, name)
            classified = set(unsupported[name])
            excluded_hooks = hook_methods | {"OnInitialize", "OnUpdate"}
            self.assertFalse(native - portable - classified - excluded_hooks)
            self.assertFalse(portable & classified)

        audio_api = {
            "AudioPath", "SetAudioPath", "Volume", "SetVolume", "Pitch",
            "SetPitch", "Pan", "SetPan", "Loop", "SetLoop", "PlayOnStart",
            "SetPlayOnStart", "IsSpatial", "SetSpatial", "MinimumDistance",
            "SetMinimumDistance", "MaximumDistance", "SetMaximumDistance",
            "Bus", "SetBus", "Play", "PlayOneShot", "Stop", "TypeName",
        }
        audio_native = public_class_methods(
            ROOT / "src/LamaPon/Components/AudioSourceComponent.h",
            "AudioSourceComponent",
        )
        audio_portable = public_class_methods(
            portable_header, "AudioSourceComponent"
        )
        self.assertFalse(audio_api - audio_native)
        self.assertFalse(audio_api - audio_portable)

    def test_all_portable_component_api_gaps_are_classified(self):
        portable_header = ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h"
        portable_source = portable_header.read_text(encoding="utf-8")
        unsupported = NATIVE.portable.PORTABLE_UNSUPPORTED_COMPONENT_METHODS
        lifecycle_hooks = {
            "OnInitialize", "OnUpdate", "OnLateUpdate", "OnFixedUpdate",
            "OnRender2D", "OnRender3D", "OnPreRender3D",
            "OnRenderDebug3D", "OnActiveStateChanged", "OnCollisionEnter",
            "OnCollisionStay", "OnCollisionExit", "OnTriggerEnter",
            "OnTriggerStay", "OnTriggerExit",
        }
        native_component_types = set()
        for header in (ROOT / "src/LamaPon/Components").glob("*.h"):
            source = header.read_text(encoding="utf-8")
            component_types = re.findall(
                r"\bclass\s+(\w+Component)\b[^;{]*:\s*public\s+Component",
                source,
            )
            for component_type in component_types:
                native_component_types.add(component_type)
                declaration = re.search(
                    r"\bclass\s+" + re.escape(component_type)
                    + r"\b([^{};]*)\{",
                    portable_source,
                )
                if declaration is None:
                    continue
                inherited_methods = set()
                for base in re.findall(
                    r"\bpublic\s+([A-Za-z_][A-Za-z0-9_]*)",
                    declaration.group(1),
                ):
                    if re.search(
                        r"\bclass\s+" + re.escape(base) + r"\b[^{};]*\{",
                        portable_source,
                    ):
                        inherited_methods |= public_class_methods(
                            portable_header, base
                        )
                missing = (
                    public_class_methods(header, component_type)
                    - public_class_methods(portable_header, component_type)
                    - inherited_methods
                    - lifecycle_hooks
                )
                self.assertEqual(
                    missing,
                    set(unsupported.get(component_type, ())),
                    f"{component_type} methods missing from Portable need explicit classification",
                )
        self.assertTrue(set(unsupported) <= native_component_types)

    def test_portable_component_type_names_match_the_windows_contract(self):
        portable_header = ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h"
        portable_source = portable_header.read_text(encoding="utf-8")
        for header in (ROOT / "src/LamaPon/Components").glob("*.h"):
            source = header.read_text(encoding="utf-8")
            component_types = re.findall(
                r"\bclass\s+(\w+Component)\b[^;{]*:\s*public\s+Component",
                source,
            )
            for component_type in component_types:
                if not re.search(
                    r"\bclass\s+" + re.escape(component_type)
                    + r"\b[^{};]*\{",
                    portable_source,
                ):
                    continue
                with self.subTest(component_type=component_type):
                    expected = class_string_return(
                        header, component_type, "TypeName"
                    )
                    actual = class_string_return(
                        portable_header, component_type, "TypeName"
                    )
                    self.assertIsNotNone(expected)
                    self.assertEqual(actual, expected)

    def test_every_classified_component_api_gap_is_rejected(self):
        unsupported = NATIVE.portable.PORTABLE_UNSUPPORTED_COMPONENT_METHODS
        lines = [
            "#include \"LamaPon/LamaPon.h\"",
            "class Probe final : public LamaPon::Script {",
            "    void Start() override {",
        ]
        expected = set()
        for component_index, (component_type, methods) in enumerate(
            sorted(unsupported.items())
        ):
            receiver = f"component{component_index}"
            lines.append(
                f"        auto* {receiver} = GetComponent<"
                f"LamaPon::{component_type}>();"
            )
            for method in methods:
                lines.append(f"        if ({receiver}) {receiver}->{method}();")
                expected.add((component_type, method))
        lines.extend(("    }", "};"))
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(
                    text="\n".join(lines), platform=platform
                )
                findings = [
                    item for item in report["findings"]
                    if item["code"] == "unsupported-portable-component-api"
                ]
                actual = {
                    (component_type, method)
                    for component_type, methods in unsupported.items()
                    for method in methods
                    if any(
                        f"{component_type}::{method}" in item["message"]
                        for item in findings
                    )
                }
                self.assertEqual(actual, expected, findings)
                self.assertEqual(len(findings), len(expected), findings)

    def test_unimplemented_physics_component_methods_are_rejected_for_all_native_targets(self):
        source = r'''#include "LamaPon/LamaPon.h"
class Probe final : public LamaPon::Script {
    void Start() override {
        auto* box = GetComponent<LamaPon::BoxCollider3DComponent>();
        if (box) (void)box->WorldBox();
        auto* body = Owner().GetComponent<LamaPon::RigidbodyComponent>();
        if (body) body->AddForce({1.0f, 0.0f, 0.0f});
        (void)GetComponent<LamaPon::RigidbodyComponent>()->Mass();
        auto* audio = GetComponent<LamaPon::AudioSourceComponent>();
        if (audio) audio->Pause();
        (void)GetComponent<LamaPon::AudioSourceComponent>()->SetStreaming(true);
        auto* multilineBody = Owner().GetComponent<
            LamaPon::RigidbodyComponent>();
        if (multilineBody) multilineBody->WakeUp();
        // body->Sleep() is only a comment.
        const char* example = "body->WakeUp()";
        static_cast<void>(example);
    }
};
LAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");
'''
        expected = {
            ("BoxCollider3DComponent", "WorldBox"),
            ("RigidbodyComponent", "AddForce"),
            ("RigidbodyComponent", "Mass"),
            ("AudioSourceComponent", "Pause"),
            ("AudioSourceComponent", "SetStreaming"),
            ("RigidbodyComponent", "WakeUp"),
        }
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(text=source, platform=platform)
                findings = [
                    item for item in report["findings"]
                    if item["code"] == "unsupported-portable-component-api"
                ]
                actual = {
                    (component, method)
                    for component, methods in NATIVE.portable
                        .PORTABLE_UNSUPPORTED_COMPONENT_METHODS.items()
                    for method in methods
                    if any(
                        f"{component}::{method}" in item["message"]
                        for item in findings
                    )
                }
                self.assertEqual(actual, expected, findings)

    def test_camera_component_accessors_match_the_windows_contract(self):
        native_header = ROOT / "src/LamaPon/Components/CameraComponent.h"
        portable_header = ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h"
        native_methods = public_class_methods(native_header, "CameraComponent")
        portable_methods = public_class_methods(portable_header, "CameraComponent")
        shared_api = {
            "VerticalFieldOfView", "SetVerticalFieldOfView", "NearPlane",
            "SetNearPlane", "FarPlane", "SetFarPlane",
        }
        self.assertFalse(shared_api - native_methods)
        self.assertFalse(shared_api - portable_methods)

    def test_native_export_rejects_unimplemented_gameobject_methods(self):
        source = (
            '#include "LamaPon/LamaPon.h"\n'
            'class Probe final : public LamaPon::Script {\n'
            '    void Start() override {\n'
            '        Owner().TranslateWorld({1.0f, 0.0f, 0.0f});\n'
            '        auto& owner = Owner();\n'
            '        (void)owner.IsPersistent();\n'
            '    }\n'
            '    void CheckFindResult() {\n'
            '        auto* child = Find("Child");\n'
            '        if (child) (void)child->PersistenceKey();\n'
            '        auto* nested = Owner().FindChild("Child/Nested");\n'
            '        if (nested) (void)nested->PrefabAssetPath();\n'
            '    }\n'
            '};\n'
            'void Configure(LamaPon::GameObject* object) {\n'
            '    object->SetPrefabAssetPath({});\n'
            '}\n'
            'namespace LP = LamaPon;\n'
            'void ConfigureAlias(LP::GameObject& object) {\n'
            '    (void)object.IsPrefabInstanceRoot();\n'
            '}\n'
            'auto render = &LamaPon::GameObject::Render3D;\n'
        )
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(text=source, platform=platform)
                findings = [item for item in report["findings"]
                            if item["code"]
                            == "unsupported-portable-gameobject-api"]
                self.assertEqual(len(findings), 7, report["findings"])
                self.assertEqual(
                    {method for method in (
                        "TranslateWorld", "IsPersistent",
                        "SetPrefabAssetPath", "IsPrefabInstanceRoot",
                        "PersistenceKey", "PrefabAssetPath", "Render3D",
                    ) if any(f"GameObject::{method}" in item["message"]
                             for item in findings)},
                    {"TranslateWorld", "IsPersistent",
                     "SetPrefabAssetPath", "IsPrefabInstanceRoot",
                     "PersistenceKey", "PrefabAssetPath", "Render3D"},
                    report["findings"],
                )

    def test_native_scene_clear_is_rejected_without_rejecting_event_bus_clear(self):
        report, _ = self.inspect_fixture(
            '#include "LamaPon/LamaPon.h"\n'
            'void ClearScene(LamaPon::Scene& scene, LamaPon::EventBus& events) {\n'
            '    scene.Clear();\n'
            '    events.Clear();\n'
            '}\n'
        )
        findings = [item for item in report["findings"]
                    if item["code"] == "unsupported-portable-scene-api"]
        self.assertEqual(len(findings), 1, report["findings"])
        self.assertIn("ClearはPortable", findings[0]["message"])

    def test_native_export_rejects_unknown_type_through_portable_namespace_alias(self):
        report, _ = self.inspect_fixture(
            '#include "LamaPon/LamaPon.h"\n'
            'namespace LP = LamaPon;\n'
            'LP::FutureRendererComponent* future{};\n'
        )

        self.assertFalse(report["canGenerateBuildProject"])
        self.assertTrue(any(
            item["code"] == "unsupported-portable-api"
            and "LamaPon::FutureRendererComponent" in item["message"]
            for item in report["findings"]
        ), report["findings"])

    def test_native_export_rejects_unregistered_script_through_namespace_import(self):
        report, _ = self.inspect_fixture(
            '#include "LamaPon/LamaPon.h"\n'
            'using namespace LamaPon;\n'
            'LAMAPON_SCRIPT_NAMED(Probe, "Game.Probe", "Probe");\n'
            'void Spawn(LamaPon::GameObject& object) {\n'
            '    object.AddComponent<NativeScriptComponent>("Game.Missing");\n'
            '}\n'
        )

        self.assertFalse(report["canGenerateBuildProject"])
        self.assertTrue(any(
            item["code"] == "unregistered-dynamic-script"
            and "Game.Missing" in item["message"]
            for item in report["findings"]
        ), report["findings"])

    def test_native_export_rejects_unsupported_scene_physics_queries(self):
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
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(text=source, platform=platform)
                findings = [item for item in report["findings"]
                            if item["code"] == "unsupported-portable-scene-api"]
                self.assertEqual(len(findings), len(methods), report["findings"])
                self.assertEqual(
                    {method for method in methods
                     if any(f"{method}はPortable" in item["message"] for item in findings)},
                    set(methods), report["findings"],
                )

    def test_native_export_rejects_unimplemented_scene_configuration_apis(self):
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
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(text=source, platform=platform)
                findings = [item for item in report["findings"]
                            if item["code"] == "unsupported-portable-scene-api"]
                self.assertEqual(len(findings), len(methods), report["findings"])
                self.assertEqual(
                    {method for method in methods
                     if any(f"{method}はPortable" in item["message"] for item in findings)},
                    set(methods), report["findings"],
                )

    def test_native_export_rejects_qualified_unsupported_scene_physics_query(self):
        report, _ = self.inspect_fixture(
            '#include "LamaPon/LamaPon.h"\n'
            'auto query = &LamaPon::Scene::RaycastAll;\n'
        )
        self.assertTrue(any(
            item["code"] == "unsupported-portable-scene-api"
            and "RaycastAllはPortable" in item["message"]
            for item in report["findings"]
        ), report["findings"])

    def test_native_export_rejects_windows_only_script_methods(self):
        methods = (
            "StartCoroutine", "SetWindowSize", "LoadDataAsset",
            "SignInWithDiscord", "Network", "SaveNumber", "CreateGameObject",
        )
        source = (
            '#include "LamaPon/LamaPon.h"\n'
            'class Probe final : public LamaPon::Script {\n'
            '    void Update(float) override {\n'
            + "\n".join(f"        (void){method}();" for method in methods)
            + "\n    }\n};\n"
        )
        for platform in ("windows", "linux", "android"):
            with self.subTest(platform=platform):
                report, _ = self.inspect_fixture(text=source, platform=platform)
                findings = [item for item in report["findings"]
                            if item["code"] == "unsupported-portable-script-api"]
                self.assertEqual(len(findings), len(methods), report["findings"])
                self.assertEqual(
                    {method for method in methods
                     if any(f"Script::{method}はPortable" in item["message"] for item in findings)},
                    set(methods), report["findings"],
                )

    def test_native_script_api_scan_handles_script_as_a_later_base(self):
        report, _ = self.inspect_fixture(
            '#include "LamaPon/LamaPon.h"\n'
            'namespace LP = LamaPon;\n'
            'struct Marker {};\n'
            'class Probe : public Marker, public LP::Script {\n'
            '    void Update(float) override { this->StartCoroutine(); }\n'
            '};\n'
        )
        self.assertTrue(any(
            item["code"] == "unsupported-portable-script-api"
            and "Script::StartCoroutine" in item["message"]
            for item in report["findings"]
        ), report["findings"])

    def test_portable_header_types_are_classified_for_cross_platform_export(self):
        header = (ROOT / "src/LamaPon/Portable/include/LamaPon/LamaPon.h").read_text(encoding="utf-8")
        declared = set(re.findall(
            r"(?m)^    (?:class|struct|enum class|using)\s+([A-Za-z_]\w*)", header))
        internal = {"Renderer3D", "WebAudioRuntime", "WebInput", "XMFLOAT2", "XMFLOAT3",
                    "XMFLOAT4", "XMFLOAT4X4", "XMMATRIX"}
        self.assertEqual(declared - set(NATIVE.portable.PORTABLE_API_MODULES), internal)

    def test_default_ui_navigation_actions_are_exportable_without_project_bindings(self):
        for action in ("UIUp", "UIDown", "UILeft", "UIRight", "UINext", "UIPrevious"):
            with self.subTest(action=action):
                report, _ = self.inspect_fixture('LAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");\n'
                                                 + f'Input().WasPressed("{action}");')
                self.assertTrue(report["canGenerateBuildProject"], report["findings"])

    def test_editor_native_generation_reports_metadata_instead_of_a_game_binary(self):
        report, context = self.inspect_fixture(text=None, platform="linux", source="ExportSmoke.cpp")
        output = ROOT / "test-output/platform-core/not-created-editor-output"
        with mock.patch.object(EDITOR_NATIVE.export_native, "inspect_project", return_value=(report, context)), \
             mock.patch.object(EDITOR_NATIVE.export_native, "generate_build_project") as generate:
            result = EDITOR_NATIVE.export_project(ROOT / "tests/native/CMakeLists.txt", output, "linux")
        self.assertTrue(result["ok"])
        self.assertEqual(result["buildProjectPath"], str(output))
        self.assertNotIn("htmlPath", result)
        self.assertNotIn("apkPath", result)
        generate.assert_called_once_with(output, "linux", report, context)

    def test_editor_rejection_keeps_output_untouched_and_explains_compatibility(self):
        report, context = self.inspect_fixture('#include <windows.h>')
        with mock.patch.object(EDITOR_NATIVE.export_native, "inspect_project", return_value=(report, context)), \
             mock.patch.object(EDITOR_NATIVE.export_native, "generate_build_project") as generate:
            result = EDITOR_NATIVE.export_project(ROOT / "tests/native/CMakeLists.txt",
                                                 ROOT / "test-output/platform-core/not-created-editor-output", "linux")
        self.assertFalse(result["ok"])
        self.assertTrue(result["message"])
        generate.assert_not_called()

    def test_editor_protects_existing_source_directory_before_generation(self):
        with mock.patch.object(EDITOR_NATIVE.export_native, "generate_build_project") as generate:
            with self.assertRaises(ValueError):
                EDITOR_NATIVE.export_project(ROOT / "tests/native/CMakeLists.txt", ROOT / "tests/native", "linux")
        generate.assert_not_called()

    def test_case_sensitive_paths_are_checked_independently_of_the_host(self):
        root = ROOT / "tests/native"
        self.assertEqual(NATIVE.exact_relative_path(root, "assets/scenes/Main.scene.json"),
                         root / "assets/scenes/Main.scene.json")

        self.assertIsNone(NATIVE.exact_relative_path(root, "assets/scenes/main.scene.json"))
        self.assertIsNone(NATIVE.exact_relative_path(root, "Assets/scenes/Main.scene.json"))
        self.assertIsNone(NATIVE.exact_relative_path(root, "../native/assets/scenes/Main.scene.json"))
        self.assertEqual(NATIVE.exact_relative_path(root, "assets/scenes/../scenes/Main.scene.json"),
                         root / "assets/scenes/Main.scene.json")

    def test_scene_asset_fields_support_custom_directories(self):
        self.assertEqual(list(NATIVE.json_asset_references(
            {"objects": [{"components": [{"texture": "sprites/Logo.png", "fontAsset": "custom/Font.ttf"}]}]})),
            ["sprites/Logo.png", "custom/Font.ttf"])

    def test_linux_source_reference_case_mismatch_is_rejected(self):
        report, _ = self.inspect_fixture('LAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");\n'
                                         'const char* path = "scenes/main.scene.json";')
        self.assertFalse(report["canGenerateBuildProject"])
        self.assertTrue(any(item["code"] == "case-sensitive-asset-reference" for item in report["findings"]))

    def test_android_scene_reference_case_mismatch_is_rejected(self):
        scene_path = ROOT / "tests/native/assets/scenes/Main.scene.json"
        original = Path.read_text
        scene = json.loads(original(scene_path, encoding="utf-8"))
        native_script = next(
            component for component in scene["objects"][0]["components"]
            if component["type"] == "NativeScript"
        )
        native_script["properties"]["asset"] = "scenes/main.scene.json"
        def read(path, *args, **kwargs):
            return json.dumps(scene) if path == scene_path else original(path, *args, **kwargs)
        with mock.patch.object(Path, "read_text", read):
            report, _ = self.inspect_fixture(platform="android")
        self.assertFalse(report["canGenerateBuildProject"])
        self.assertTrue(any(item["code"] == "case-sensitive-asset-reference" for item in report["findings"]))

    def test_platform_specific_script_is_rejected_before_generation(self):
        report, context = self.inspect_fixture('#include <windows.h>\nLAMAPON_SCRIPT_NAMED(Probe, "Test.NativeStartup", "Probe");')
        self.assertFalse(report["canGenerateBuildProject"])
        with mock.patch.object(Path, "mkdir") as mkdir:
            with self.assertRaises(NATIVE.portable.ExportError):
                NATIVE.generate_build_project(ROOT / "unused-output", "linux", report, context)
            mkdir.assert_not_called()

    def test_cmake_literals_cannot_close_their_delimiter_or_create_lists(self):
        self.assertEqual(NATIVE.cmake_literal("a]=]b"), "[==[a]=]b]==]")
        for text in ("a;b", "a\nb", "a\x00b", "$<CONFIG>"):
            with self.assertRaises(NATIVE.portable.ExportError):
                NATIVE.cmake_literal(text)

    def test_invalid_input_controls_are_rejected_during_readonly_inspection(self):
        with mock.patch.object(NATIVE.portable, "portable_project_input_actions",
                               return_value={"Jump": [{"control": "Unknown", "scale": 1.0}]}):
            report, _ = self.inspect_fixture()
        self.assertFalse(report["canGenerateBuildProject"])
        self.assertIn("unsupported-native-input-binding", [f["code"] for f in report["findings"]])

    def test_model_embedded_images_and_remote_references_are_reported(self):
        for document in ({"images": [{"bufferView": 0}]},
                         {"images": [{"uri": "data:image/png;base64,AAAA"}]},
                         {"images": [{"uri": "https://example.invalid/image.png"}]}):
            with self.assertRaises(NATIVE.portable.ExportError):
                NATIVE.model_references(document)
        self.assertEqual(NATIVE.model_references({"images": [{"uri": "images/green%20leaf.png"}],
                                                  "buffers": [{"uri": "data:application/octet-stream;base64,AAAA"}]}),
                         ["images/green leaf.png"])

    def test_valid_embedded_png_model_images_need_no_external_image_files(self):
        for name in ("embedded-data.gltf", "embedded-buffer.glb"):
            path = ROOT / "tests/fixtures/models" / name
            document = (NATIVE.portable.read_portable_glb_document(path) if path.suffix == ".glb"
                        else json.loads(path.read_text(encoding="utf-8")))
            self.assertEqual(NATIVE.model_references(document), [])

    def test_embedded_image_buffer_metadata_cannot_escape_the_declared_buffer(self):
        for image, views in (({"bufferView": True, "mimeType": "image/png"}, []),
                             ({"bufferView": 0, "mimeType": "image/webp"}, []),
                             ({"bufferView": 0, "mimeType": "image/png"}, [{"buffer": 0, "byteOffset": 10, "byteLength": 4}]),
                             ({"bufferView": 0, "uri": "a.png", "mimeType": "image/png"}, [])):
            with self.subTest(image=image), self.assertRaises(NATIVE.portable.ExportError):
                NATIVE.model_references({"images": [image], "bufferViews": views, "buffers": [{"byteLength": 12}]})

    def test_generated_build_project_can_remap_source_locations(self):
        report, context = self.inspect_fixture()
        written = {}
        with mock.patch.object(Path, "exists", return_value=False), \
             mock.patch.object(Path, "mkdir"), \
             mock.patch.object(Path, "write_text", autospec=True,
                               side_effect=lambda path, text, **kwargs: written.setdefault(path.name, text)):
            NATIVE.generate_build_project(ROOT / "unused-native-output", "linux", report, context)
        self.assertIn('LAMAPON_PROJECT_ROOT', written["CMakeLists.txt"])
        self.assertIn('CMAKE_SYSTEM_NAME STREQUAL "Linux"', written["CMakeLists.txt"])
        self.assertIn("CMAKE_SYSTEM_PROCESSOR MATCHES", written["CMakeLists.txt"])
        self.assertIn('INPUT_ACTIONS_FILE', written["CMakeLists.txt"])
        self.assertFalse(json.loads(written["native-inspection.json"])["verified"])
        self.assertEqual(json.loads(written["lamapon-input-actions.json"])["actions"], {})
        description = json.loads(written["native-build-project.json"])
        self.assertEqual(description["version"], 2)
        self.assertEqual(description["assetDirectory"], "assets")
        self.assertEqual(description["assetIncludePaths"], ["audio", "prefabs", "scenes"])

    def test_android_configuration_connects_activity_native_libraries_and_assets(self):
        _, context = self.inspect_fixture()
        game_name = 'A & B\'s "Game" \\ Path'
        files = NATIVE.native_android.project_files(context, ROOT, "NativeGame", game_name)
        manifest = ElementTree.fromstring(files["android/app/src/main/AndroidManifest.xml"])
        android = "{http://schemas.android.com/apk/res/android}"
        self.assertEqual(manifest.find("uses-feature").get(android + "glEsVersion"), "0x00030000")
        self.assertEqual(manifest.find("application/activity").get(android + "name"), "com.lamapon.runtime.GameActivity")
        strings = ElementTree.fromstring(files["android/app/src/main/res/values/strings.xml"])
        self.assertEqual(strings.find("string").text, game_name)
        with self.assertRaises(NATIVE.portable.ExportError):
            NATIVE.native_android.project_files(context, ROOT, "NativeGame", "Bad\x00Name")
        activity = files["android/app/src/main/java/com/lamapon/runtime/GameActivity.java"]
        self.assertIn("getFilesDir().getAbsolutePath()", activity)
        self.assertIn('"--data-dir"', activity)
        self.assertIn('"--cache-dir"', activity)
        self.assertIn("getCacheDir().getAbsolutePath()", activity)
        gradle = files["android/app/build.gradle"]
        self.assertIn("'arm64-v8a', 'x86_64'", gradle)
        self.assertIn(f"ndkVersion '{NATIVE.native_android.ANDROID_NDK_VERSION}'", gradle)
        self.assertIn(f"version '{NATIVE.native_android.ANDROID_CMAKE_VERSION}'", gradle)
        self.assertIn("'SDL3-shared'", gradle)
        self.assertIn("assets.srcDir layout.buildDirectory.dir('lamaponAssets').get().asFile", gradle)
        self.assertIn("jniLibs.srcDir layout.buildDirectory.dir('lamaponRuntimeLibs').get().asFile", gradle)
        self.assertNotIn("srcDir layout.buildDirectory.dir('lamaponAssets')\n", gradle)
        self.assertIn("into 'assets'; include 'audio/**', 'prefabs/**', 'scenes/**'", gradle)
        self.assertIn("dependsOn stageAssets", gradle)
        self.assertIn("android.builder.sdkDownload=false", files["android/gradle.properties"])
        self.assertIn("androidComponents.sdkComponents.ndkDirectory", gradle)
        self.assertIn("it.file('NOTICE.toolchain')", gradle)
        self.assertIn("licenses/LamaPon.txt", gradle)

    def test_package_dependencies_flow_into_desktop_cmake_and_android_packaging(self):
        manifest = json.dumps({"name": "fixture-package", "nativeVariants": {
            "linux-x86_64": {"sources": ["linux/Fixture.cpp"],
                              "libraries": ["linux/lib/libfixture.so"],
                              "licenseFiles": ["linux/LICENSE.txt"]},
            "android-arm64-v8a": {"sources": ["android/arm64/Fixture.cpp"],
                                   "libraries": ["android/arm64/libfixture.so"],
                                   "licenseFiles": ["android/LICENSE.txt"]},
            "android-x86_64": {"sources": ["android/x64/Fixture.cpp"],
                                "libraries": ["android/x64/libfixture.so"],
                                "licenseFiles": ["android/LICENSE.txt"]},
        }})
        paths = ["linux/Fixture.cpp", "linux/lib/libfixture.so", "linux/LICENSE.txt",
                 "android/arm64/Fixture.cpp", "android/arm64/libfixture.so",
                 "android/x64/Fixture.cpp", "android/x64/libfixture.so", "android/LICENSE.txt"]
        linux_report, linux_context = self.inspect_fixture_with_package_manifest("linux", manifest, paths)
        written = {}
        with mock.patch.object(Path, "exists", return_value=False), \
             mock.patch.object(Path, "mkdir"), \
             mock.patch.object(Path, "write_text", autospec=True,
                               side_effect=lambda path, text, **kwargs: written.setdefault(path.name, text)):
            NATIVE.generate_build_project(ROOT / "unused-native-output", "linux", linux_report, linux_context)
        generated = written["CMakeLists.txt"]
        self.assertIn("package_libraries", generated)
        self.assertIn("PACKAGE_SOURCES ${package_sources}", generated)
        self.assertIn("PACKAGE_LICENSE_FILES ${package_license_files}", generated)
        self.assertIn("libfixture.so", generated)
        description = json.loads(written["native-build-project.json"])
        self.assertIn("linux-x86_64", description["packageDependencies"])

        android_report, android_context = self.inspect_fixture_with_package_manifest("android", manifest, paths)
        gradle = NATIVE.native_android.project_files(android_context, ROOT, "NativeGame", "Game")["android/app/build.gradle"]
        self.assertIn("jniLibs.srcDir layout.buildDirectory.dir('lamaponRuntimeLibs').get().asFile", gradle)
        self.assertIn("lamaponRuntimeLibs", gradle)
        self.assertIn("into 'licenses/packages/fixture-package/android'", gradle)
        self.assertIn("rename { 'LICENSE.txt' }", gradle)
        self.assertTrue(android_report["canGenerateBuildProject"], android_report["findings"])
        android_written = {}
        with mock.patch.object(Path, "exists", return_value=False), \
             mock.patch.object(Path, "mkdir"), \
             mock.patch.object(Path, "write_text", autospec=True,
                               side_effect=lambda path, text, **kwargs: android_written.setdefault(path.name, text)):
            NATIVE.generate_build_project(ROOT / "unused-android-native-output", "android", android_report, android_context)
        generated_android = android_written["CMakeLists.txt"]
        self.assertIn("CMAKE_ANDROID_ARCH_ABI STREQUAL", generated_android)
        self.assertIn("assets/packages/fixture-package/android/arm64/libfixture.so", generated_android)
        self.assertIn("assets/packages/fixture-package/android/arm64/Fixture.cpp", generated_android)
        self.assertIn("ANDROID_PACKAGE_LIBS_DIRECTORY", generated_android)

    def test_android_invalid_identifiers_sdk_ranges_and_abis_fail_before_writing(self):
        _, context = self.inspect_fixture()
        for config in ({"applicationId": "bad;code"}, {"minSdk": 25}, {"targetSdk": 99},
                       {"abis": ["arm64-v8a", "arm64-v8a"]}, {"versionCode": True}):
            context["project"]["export"]["native"]["android"] = config
            with self.assertRaises(NATIVE.portable.ExportError):
                NATIVE.native_android.project_files(context, ROOT, "NativeGame", "Game")

    def test_android_project_generation_writes_metadata_without_claiming_an_apk(self):
        report, context = self.inspect_fixture(platform="android")
        written = {}
        with mock.patch.object(Path, "exists", return_value=False), \
             mock.patch.object(Path, "mkdir"), \
             mock.patch.object(Path, "write_text", autospec=True,
                               side_effect=lambda path, text, **kwargs: written.setdefault(path.as_posix(), text)):
            NATIVE.generate_build_project(ROOT / "unused-android-output", "android", report, context)
        self.assertTrue(any(key.endswith("android/app/build.gradle") for key in written))
        self.assertFalse(any(key.endswith(".apk") for key in written))
        inspection = next(text for path, text in written.items() if path.endswith("native-inspection.json"))
        self.assertFalse(json.loads(inspection)["verified"])

    def test_editor_apk_missing_sdk_fails_before_output_generation(self):
        report, context = self.inspect_fixture(platform="android", text=None, source="ExportSmoke.cpp")
        options = SimpleNamespace(android_sdk=None, java_home=None, gradle_home=None,
                                  sdl_source_directory=None, allow_downloads=False)
        with mock.patch.object(EDITOR_NATIVE.export_native, "inspect_project", return_value=(report, context)), \
                mock.patch.object(EDITOR_NATIVE.export_native, "generate_build_project") as generate:
            with self.assertRaises(EDITOR_NATIVE.build_native.ExportError):
                EDITOR_NATIVE.export_project(ROOT / "tests/native/CMakeLists.txt", ROOT / "unused-apk-output", "android", options)
            generate.assert_not_called()

    def test_editor_apk_never_accepts_unchecked_or_missing_artifact(self):
        report, context = self.inspect_fixture(platform="android", text=None, source="ExportSmoke.cpp")
        output = ROOT / "unused-apk-output"
        options = SimpleNamespace()
        for build_result in ({"built": False, "platform": "android", "artifactChecksPassed": True},
                             {"built": True, "platform": "android", "artifactChecksPassed": False},
                             {"built": True, "platform": "android", "artifactChecksPassed": True,
                              "artifactPath": str(output / "build/app/outputs/apk/debug/app-debug.apk")}):
            with mock.patch.object(EDITOR_NATIVE.export_native, "inspect_project", return_value=(report, context)), \
                    mock.patch.object(EDITOR_NATIVE.export_native, "generate_build_project"), \
                    mock.patch.object(EDITOR_NATIVE.build_native, "android_command"), \
                    mock.patch.object(EDITOR_NATIVE.build_native, "build", return_value=build_result):
                with self.assertRaises(ValueError):
                    EDITOR_NATIVE.export_project(ROOT / "tests/native/CMakeLists.txt", output, "android", options)


if __name__ == "__main__":
    unittest.main()
