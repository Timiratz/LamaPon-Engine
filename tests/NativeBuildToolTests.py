"""Validate build failure and package gates in memory; no test files are created."""
import io
import json
from pathlib import Path
import sys
import struct
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_native as BUILD


class NativeBuildToolTests(unittest.TestCase):
    def test_android_emulator_smoke_completes_first_run_setup_before_launch(self):
        emulator_smoke = (ROOT / "tests/AndroidEmulatorSmoke.py").read_text(encoding="utf-8")
        self.assertIn("def prepare_headless_emulator():", emulator_smoke)
        self.assertIn('"global", "device_provisioned", "1"', emulator_smoke)
        self.assertIn('"secure", "user_setup_complete", "1"', emulator_smoke)
        prepare_call = emulator_smoke.index(
            "    prepare_headless_emulator()", emulator_smoke.index("def main():"))
        install_call = emulator_smoke.index('    adb("install", "-r", str(APK))')
        self.assertLess(prepare_call, install_call)

    def test_steam_deck_smoke_builds_and_runs_inside_the_official_runtime_sdk(self):
        workflow = (ROOT / ".github/workflows/platform-core-ci.yml").read_text(encoding="utf-8")
        self.assertIn("Install Android SDK packages required by APK export", workflow)
        self.assertIn("steamrt4-game-smoke:", workflow)
        self.assertIn("registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk:latest", workflow)
        self.assertIn("Configure and build the real game inside Steam Runtime 4", workflow)
        self.assertIn("xvfb-run -a python3 -B tests/RunNativeTests.py", workflow)
        self.assertIn("Verify relocated shared SDL distribution in Steam Runtime 4", workflow)
        self.assertIn("Generate a real Linux game output project", workflow)
        self.assertIn("tools/export_native.py --project", workflow)
        self.assertIn("tools/build_native.py", workflow)
        self.assertIn("Launch the generated Linux game and verify persistent saves", workflow)
        self.assertLess(workflow.index("Configure and build the real game inside Steam Runtime 4"),
                        workflow.index("Build and inspect generated Linux game package in Steam Runtime 4"))

    def test_android_ci_installs_every_sdk_package_required_by_apk_export(self):
        workflow = (ROOT / ".github/workflows/platform-core-ci.yml").read_text(encoding="utf-8")
        emulator_smoke = (ROOT / "tests/AndroidEmulatorSmoke.py").read_text(encoding="utf-8")
        minimum_gradle = ".".join(map(str, BUILD.native_android.ANDROID_MIN_GRADLE_VERSION))
        self.assertIn(f"NDK_VERSION: {BUILD.native_android.ANDROID_NDK_VERSION}", workflow)
        self.assertIn(f'CMAKE_VERSION: "{BUILD.native_android.ANDROID_CMAKE_VERSION}"', workflow)
        self.assertIn('python-version: "3.12"', workflow)
        setup_android = "android-actions/setup-android@be39fa834029ff78f1a44aa3bb0819b8fc2bd8fd"
        self.assertIn(setup_android, workflow)
        self.assertIn("accept-android-sdk-licenses: true", workflow)
        self.assertLess(workflow.index(setup_android),
                        workflow.index("Install Android SDK packages required by APK export"))
        self.assertIn('sdk_root = os.environ["ANDROID_HOME"]', workflow)
        self.assertIn('input="y\\n" * 128', workflow)
        self.assertIn('run_sdkmanager("--licenses")', workflow)
        install_packages = 'run_sdkmanager(\n              "--install",'
        self.assertIn(install_packages, workflow)
        self.assertLess(workflow.index('run_sdkmanager("--licenses")'),
                        workflow.index(install_packages))
        self.assertLess(workflow.rindex('run_sdkmanager("--licenses")'),
                        workflow.index('test -d "$ANDROID_HOME/platforms/android-36"'))
        self.assertIn("actions/setup-java@de7274f081f381c8f8158605e0321c36c376e2e6", workflow)
        self.assertIn('distribution: temurin\n          java-version: "17"', workflow)
        self.assertIn("gradle/actions/setup-gradle@3f5f9adaf7d9fecd50b5935e54106014257a94e6", workflow)
        self.assertIn(f'gradle-version: "{minimum_gradle}"', workflow)
        self.assertIn(f'"platforms;android-{BUILD.native_android.ANDROID_COMPILE_SDK_VERSION}"', workflow)
        self.assertIn(f'"build-tools;{BUILD.native_android.ANDROID_BUILD_TOOLS_VERSION}"', workflow)
        self.assertIn('"ndk;" + os.environ["NDK_VERSION"]', workflow)
        self.assertIn('"cmake;" + os.environ["CMAKE_VERSION"]', workflow)
        self.assertIn('test -d "$ANDROID_HOME/platforms/android-36"', workflow)
        self.assertIn('test -d "$ANDROID_HOME/build-tools/36.0.0"', workflow)
        self.assertIn('--java-home "$JAVA_HOME"', workflow)
        self.assertNotIn('JAVA_HOME_17_X64', workflow)
        self.assertIn('tools/export_native.py --project "$game/.lamapon/project.json"', workflow)
        self.assertIn("tools/build_native.py", workflow)
        self.assertIn("reactivecircus/android-emulator-runner@a421e43855164a8197daf9d8d40fe71c6996bb0d", workflow)
        self.assertIn("Show Android Gradle failure context", workflow)
        self.assertIn("tail -n 160", workflow)
        self.assertIn("target: google_apis_ps16k", workflow)
        self.assertIn("emulator-options: -no-window -gpu software -no-snapshot", workflow)
        self.assertNotIn("-gpu swiftshader_indirect", workflow)
        self.assertIn("script: python3 -B tests/AndroidEmulatorSmoke.py", workflow)
        self.assertIn("tests/AndroidEmulatorSmoke.py", workflow)
        self.assertIn("getconf", emulator_smoke)
        self.assertIn("KEYCODE_HOME", emulator_smoke)
        self.assertIn("def wait_for_resume(initial_count, initial_pid):", emulator_smoke)
        self.assertIn(
            "if resumed_count != initial_count or resumed_pid != initial_pid:",
            emulator_smoke)
        self.assertIn("run-as", emulator_smoke)

    def description(self, platform="android"):
        return {"format": "lamapon.native-build-project", "version": 2,
                "platform": platform, "target": "Game", "scenePath": "/assets/scenes/Main.scene.json",
                "engineRoot": str(ROOT), "projectRoot": str(ROOT / "tests/native"),
                "assetDirectory": "assets", "assetIncludePaths": ["scenes/Main.scene.json"],
                "android": {"abis": ["arm64-v8a", "x86_64"]}}

    def test_desktop_package_runtime_libraries_are_selected_for_the_target(self):
        description = self.description()
        description["packageDependencies"] = {
            "windows-x86_64": [{"libraries": ["assets/packages/foo/libfoo.lib"],
                                "runtimeFiles": ["assets/packages/foo/foo.dll"]}],
            "linux-x86_64": [{"libraries": ["assets/packages/foo/libfoo.so.1"],
                              "runtimeFiles": ["assets/packages/foo/libplugin.so.2"]}],
            "android-arm64-v8a": [{"libraries": ["assets/packages/foo/libfoo.so"],
                                   "runtimeFiles": []}],
        }
        self.assertEqual(BUILD.package_runtime_library_names(description, "windows-x86_64"), ["foo.dll"])
        self.assertEqual(BUILD.package_runtime_library_names(description, "linux-x86_64"),
                         ["libfoo.so.1", "libplugin.so.2"])

    def test_desktop_package_rejects_missing_extra_and_outdated_assets(self):
        staged = ROOT / "test-output/platform-core/not-created-asset-probe"
        game = ROOT / "tests/native"
        names = {"scenes/Main.scene.json", "lamapon-default-font.ttf", "lamapon-input-actions.json"}
        rglob, is_file = Path.rglob, Path.is_file
        for problem in (None, "missing", "extra", "outdated"):
            contents = {staged / name: b"current bytes" for name in names}
            if problem == "missing":
                del contents[staged / "scenes/Main.scene.json"]
            elif problem == "extra":
                contents[staged / "removed-secret.txt"] = b"old bytes"
            elif problem == "outdated":
                contents[staged / "scenes/Main.scene.json"] = b"old bytes"
            with self.subTest(problem=problem), \
                    mock.patch.object(Path, "rglob", lambda path, pattern: iter(contents) if path == staged else rglob(path, pattern)), \
                    mock.patch.object(Path, "is_file", lambda path: True if path in contents else is_file(path)), \
                    mock.patch.object(Path, "open", lambda path, *args, **kwargs: io.BytesIO(contents.get(path, b"current bytes"))):
                if problem is None:
                    BUILD.check_desktop_assets(staged, self.description("windows"), ROOT, ROOT, game)
                else:
                    with self.assertRaises(BUILD.ExportError):
                        BUILD.check_desktop_assets(staged, self.description("windows"), ROOT, ROOT, game)

    def test_asset_selection_rejects_absolute_and_parent_paths(self):
        for value in (None, "", "/assets", "C:/assets", "../assets", "scenes/../assets", "scenes\\Main.json", "scenes//Main.json"):
            self.assertFalse(BUILD.safe_asset_path(value, allow_root=True))
        self.assertTrue(BUILD.safe_asset_path(".", allow_root=True))
        self.assertFalse(BUILD.safe_asset_path("."))
        self.assertTrue(BUILD.safe_asset_path("scenes/Main.scene.json"))

    def elf(self, abi="arm64-v8a", load_alignment=16384, relro_end=16384):
        # A synthetic ELF header fixture for structural checks, not executable code.
        data = bytearray(16384)
        ident = b"\x7fELF\x02\x01\x01" + bytes(9)
        struct.pack_into("<16sHHIQQQIHHHHHH", data, 0, ident, 3,
                         183 if abi == "arm64-v8a" else 62, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
        struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0, 0, len(data), len(data), load_alignment)
        struct.pack_into("<IIQQQQQQ", data, 120, 0x6474E552, 4,
                         relro_end - 4096, relro_end - 4096, 0, 4096, 4096, 1)
        return bytes(data)

    def apk(self, omit=None, empty=None, overrides=None, compressed=None, duplicate=None, extra_names=()):
        names = {"AndroidManifest.xml", "classes.dex", "assets/assets/scenes/Main.scene.json",
                 "assets/assets/lamapon-default-font.ttf", "assets/assets/lamapon-input-actions.json"}
        names.update("assets/licenses/" + name for name in
                     ("LamaPon.txt", "AndroidNDK.txt", "AndroidNDK-toolchain.txt", "SDL3.txt",
                      "ProggyClean.LICENSE.txt", "imgui.txt", "nlohmann.txt", "cgltf.txt", "stb.txt"))
        names.update(f"lib/{abi}/{name}" for abi in ("arm64-v8a", "x86_64")
                     for name in ("libmain.so", "libSDL3.so", "libc++_shared.so"))
        names.update(extra_names)
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            for name in names - {omit}:
                content = self.elf(name.split("/")[1]) if name.endswith(".so") else b"test content"
                if name == empty:
                    content = b""
                if overrides and name in overrides:
                    content = overrides[name]
                archive.writestr(name, content, compress_type=zipfile.ZIP_DEFLATED if name == compressed else zipfile.ZIP_STORED)
            if duplicate:
                # ZipFile warns at construction; malformed APKs must still be rejected.
                import warnings
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    archive.writestr(duplicate, b"duplicate")
        buffer.seek(0)
        return zipfile.ZipFile(buffer)

    def test_apk_requires_every_requested_abi_scene_and_license(self):
        for omitted in ("lib/x86_64/libSDL3.so", "assets/assets/scenes/Main.scene.json",
                        "assets/licenses/stb.txt", "assets/licenses/LamaPon.txt", "assets/licenses/AndroidNDK-toolchain.txt",
                        "assets/assets/lamapon-input-actions.json"):
            with self.subTest(omitted=omitted), mock.patch.object(BUILD.zipfile, "ZipFile", return_value=self.apk(omit=omitted)):
                with self.assertRaises(BUILD.ExportError):
                    BUILD.check_apk(Path("unused.apk"), self.description())

    def test_empty_library_does_not_pass_package_check(self):
        with mock.patch.object(BUILD.zipfile, "ZipFile", return_value=self.apk(empty="lib/arm64-v8a/libmain.so")):
            with self.assertRaises(BUILD.ExportError):
                BUILD.check_apk(Path("unused.apk"), self.description())

    def test_complete_package_passes_content_gate_only(self):
        with mock.patch.object(BUILD.zipfile, "ZipFile", return_value=self.apk()):
            libraries = BUILD.check_apk(Path("unused.apk"), self.description())
        self.assertEqual(len(libraries), 6)
        self.assertEqual({item["abi"] for item in libraries}, {"arm64-v8a", "x86_64"})

    def test_apk_requires_package_shared_libraries_and_license_notices(self):
        description = self.description()
        description["packageDependencies"] = {
            "android-arm64-v8a": [{"variant": "android-arm64-v8a", "package": "fixture",
                "includeDirectories": [], "libraries": ["assets/packages/fixture/arm64/libfixture.so"],
                "runtimeFiles": [], "defines": [],
                "licenseFiles": [{"source": "assets/packages/fixture/LICENSE.txt",
                                   "destination": "packages/fixture/LICENSE.txt"}]}],
            "android-x86_64": [],
        }
        needed = {"lib/arm64-v8a/libfixture.so", "assets/licenses/packages/fixture/LICENSE.txt"}
        for omit in (None, "lib/arm64-v8a/libfixture.so", "assets/licenses/packages/fixture/LICENSE.txt"):
            with self.subTest(omit=omit), mock.patch.object(BUILD.zipfile, "ZipFile",
                    return_value=self.apk(omit=omit, extra_names=needed)):
                if omit is None:
                    self.assertEqual(len(BUILD.check_apk(Path("unused.apk"), description)), 7)
                else:
                    with self.assertRaises(BUILD.ExportError):
                        BUILD.check_apk(Path("unused.apk"), description)

    def test_apk_asset_selection_and_content_match_original_files(self):
        sources = {name: Path(name) for name in
                   ("scenes/Main.scene.json", "lamapon-default-font.ttf", "lamapon-input-actions.json")}
        for problem in (None, "missing", "extra", "outdated"):
            selected = dict(sources)
            overrides = None
            if problem == "missing":
                selected["scenes/Missing.scene.json"] = Path("missing")
            elif problem == "extra":
                del selected["scenes/Main.scene.json"]
            elif problem == "outdated":
                overrides = {"assets/assets/scenes/Main.scene.json": b"previous scene"}
            with self.subTest(problem=problem), \
                    mock.patch.object(BUILD.zipfile, "ZipFile", return_value=self.apk(overrides=overrides)), \
                    mock.patch.object(Path, "open", lambda *args, **kwargs: io.BytesIO(b"test content")):
                if problem is None:
                    self.assertEqual(len(BUILD.check_apk(Path("unused.apk"), self.description(), selected)), 6)
                else:
                    with self.assertRaises(BUILD.ExportError):
                        BUILD.check_apk(Path("unused.apk"), self.description(), selected)

    def test_apk_asset_name_matches_unflagged_utf8_from_android_packager(self):
        expected = "assets/assets/scenes/白画像.bmp"
        legacy_name = expected.encode("utf-8").decode("cp437")
        entry = zipfile.ZipInfo(legacy_name)
        self.assertEqual(BUILD.apk_asset_name(entry, {expected}), expected)
        ascii_name = "assets/assets/scenes/Main.scene.json"
        self.assertEqual(BUILD.apk_asset_name(zipfile.ZipInfo(ascii_name), {expected}), ascii_name)

    def test_apk_rejects_library_of_wrong_architecture(self):
        name = "lib/arm64-v8a/libmain.so"
        with mock.patch.object(BUILD.zipfile, "ZipFile", return_value=self.apk(overrides={name: self.elf("x86_64")})):
            with self.assertRaisesRegex(BUILD.ExportError, "ELF64 shared library"):
                BUILD.check_apk(Path("unused.apk"), self.description())

    def test_apk_rejects_4kb_load_and_relro_alignment(self):
        name = "lib/x86_64/libc++_shared.so"
        for content in (self.elf("x86_64", load_alignment=4096), self.elf("x86_64", relro_end=12288)):
            with mock.patch.object(BUILD.zipfile, "ZipFile", return_value=self.apk(overrides={name: content})):
                with self.assertRaisesRegex(BUILD.ExportError, "16KB"):
                    BUILD.check_apk(Path("unused.apk"), self.description())

    def test_apk_rejects_duplicate_entries_and_compressed_libraries(self):
        for archive in (self.apk(duplicate="classes.dex"), self.apk(compressed="lib/x86_64/libmain.so")):
            with mock.patch.object(BUILD.zipfile, "ZipFile", return_value=archive), self.assertRaises(BUILD.ExportError):
                BUILD.check_apk(Path("unused.apk"), self.description())

    def test_apk_rejects_undeclared_native_libraries(self):
        unexpected = "lib/arm64-v8a/libundeclared.so"
        with mock.patch.object(BUILD.zipfile, "ZipFile", return_value=self.apk(extra_names={unexpected})):
            with self.assertRaisesRegex(BUILD.ExportError, "undeclared native library"):
                BUILD.check_apk(Path("unused.apk"), self.description())

    def test_elf_rejects_truncated_tables_and_invalid_load_ranges(self):
        good = self.elf()
        bad_table = bytearray(good)
        struct.pack_into("<Q", bad_table, 32, len(good))
        bad_load = bytearray(good)
        struct.pack_into("<Q", bad_load, 64 + 32, len(good) + 1)
        bad_load_offset = bytearray(good)
        struct.pack_into("<Q", bad_load_offset, 64 + 8, 1)
        bad_relro_offset = bytearray(good)
        struct.pack_into("<Q", bad_relro_offset, 120 + 8, len(good) - 4095)
        bad_relro_size = bytearray(good)
        struct.pack_into("<Q", bad_relro_size, 120 + 8, 0)
        struct.pack_into("<Q", bad_relro_size, 120 + 32, 8192)
        bad_magic = bytearray(good)
        bad_magic[4] = 1  # ELF32 in a 64-bit ABI folder.
        no_load = bytearray(good)
        struct.pack_into("<I", no_load, 64, 0)
        for content in (good[:40], good[:100], bad_table, bad_load, bad_load_offset,
                        bad_relro_offset, bad_relro_size, bad_magic, no_load):
            with self.subTest(size=len(content)), self.assertRaises(BUILD.ExportError):
                BUILD.check_android_elf(io.BytesIO(content), len(content), "arm64-v8a", "fixture.so")

    def test_zip_alignment_failure_cannot_record_android_build_success(self):
        args = SimpleNamespace(project_directory=ROOT, engine_root=None, game_root=None,
                               build_directory=ROOT / "test-output/platform-core/unused", android_sdk=ROOT)
        writes, commands = [], []
        def capture(path, content, **kwargs):
            writes.append(json.loads(content))
        def invoke(command, *args):
            commands.append(command)
            if command[0] == "zipalign":
                raise BUILD.ExportError("bad zip alignment")
        with mock.patch.object(BUILD, "load_project", return_value=self.description()), \
                mock.patch.object(BUILD, "android_command", return_value=(["java"], {})), \
                mock.patch.object(BUILD, "prepare_directory"), mock.patch.object(Path, "mkdir"), \
                mock.patch.object(Path, "write_text", capture), mock.patch.object(BUILD, "run", side_effect=invoke), \
                mock.patch.object(BUILD, "check_apk", return_value=[]), \
                mock.patch.object(BUILD, "executable", return_value="zipalign"):
            with self.assertRaisesRegex(BUILD.ExportError, "bad zip alignment"):
                BUILD.build(args)
        self.assertEqual(writes, [{"built": False, "platform": "android"}])
        self.assertEqual(commands[-1][1:7], ["-c", "-P", "16", "-v", "4", str(args.build_directory / "app/outputs/apk/debug/app-debug.apk")])

    def test_scene_paths_cannot_escape_package(self):
        for scene in ("/assets/../secret.json", "/assets//scene.json", "/other/scene.json",
                      "/assets/scenes\\main.json", "/assets/scenes/main.txt", None):
            with self.subTest(scene=scene), self.assertRaises(BUILD.ExportError):
                BUILD.scene_asset(dict(self.description(), scenePath=scene))

    def test_android_launch_keeps_caches_and_forked_jvm_in_build_directory(self):
        directory = ROOT / "test-output/platform-core/日本語 build & cache"
        args = SimpleNamespace(android_sdk=ROOT, java_home=ROOT, gradle_home=ROOT,
                               sdl_source_directory=ROOT, allow_downloads=False)
        with mock.patch.object(Path, "is_file", return_value=True), \
                mock.patch.object(Path, "glob", return_value=[ROOT / "lib/gradle-gradle-cli-main-9.6.0.jar"]), \
                mock.patch.object(BUILD, "executable", return_value="java"):
            command, environment = BUILD.android_command(args, self.description(), directory, ROOT, ROOT / "tests/native")
        self.assertIn("--offline", command)
        self.assertIn("--no-daemon", command)
        self.assertIn("--max-workers=2", command)
        self.assertIn("-Pandroid.builder.sdkDownload=false", command)
        self.assertIn(f"-PlamaponBuildRoot={directory}", command)
        self.assertEqual(environment["GRADLE_USER_HOME"], str(directory / "gradle-user-home"))
        self.assertEqual(environment["ANDROID_USER_HOME"], str(directory / "android-user-home"))
        jvm = next(value for value in command if value.startswith("-Dorg.gradle.jvmargs="))
        self.assertIn(f'"-Duser.home={(directory / "user-home").as_posix()}"', jvm)
        self.assertIn(f'"-Djava.io.tmpdir={(directory / "tmp").as_posix()}"', jvm)

    def test_android_preflight_names_missing_sdk_packages_before_build_setup(self):
        directory = ROOT / "test-output/platform-core/android-missing-components-probe"
        args = SimpleNamespace(android_sdk=ROOT, java_home=ROOT, gradle_home=ROOT,
                               sdl_source_directory=ROOT, allow_downloads=False)
        with mock.patch.object(Path, "is_file", return_value=False), \
                mock.patch.object(BUILD, "executable", return_value="java"):
            with self.assertRaises(BUILD.ExportError) as raised:
                BUILD.android_command(args, self.description(), directory, ROOT, ROOT / "tests/native")
        message = str(raised.exception)
        for package in ("platforms;android-36", f"ndk;{BUILD.native_android.ANDROID_NDK_VERSION}",
                        "cmake;3.31.6", "build-tools;36.0.0"):
            with self.subTest(package=package):
                self.assertIn(package, message)
        self.assertIn("JDK javac", message)
        self.assertIn("SDL3 Java sources", message)
        self.assertFalse(directory.exists())

    def test_gradle_unsupported_quote_path_rejected(self):
        with self.assertRaises(BUILD.ExportError):
            BUILD.gradle_jvm_property("user.home", Path('a\'b"c'))

    def test_protected_source_build_directories_rejected_before_write(self):
        with mock.patch.object(Path, "mkdir") as mkdir:
            for directory in (ROOT, ROOT.parent, ROOT / "tools/build", ROOT / "src/build",
                              ROOT / "tests/native/assets/build"):
                with self.subTest(directory=directory), self.assertRaises(BUILD.ExportError):
                    BUILD.prepare_directory(directory, ROOT / "generated", ROOT, ROOT / "tests/native")
            mkdir.assert_not_called()

    def test_existing_unowned_directory_is_preserved(self):
        with mock.patch.object(Path, "mkdir") as mkdir, mock.patch.object(Path, "write_text") as write:
            with self.assertRaises(BUILD.ExportError):
                BUILD.prepare_directory(ROOT / "docs", ROOT / "generated", ROOT, ROOT / "tests/native")
            mkdir.assert_not_called()
            write.assert_not_called()

    def test_cross_os_build_fails_before_directory_creation(self):
        args = SimpleNamespace(project_directory=ROOT)
        platform = "linux" if sys.platform == "win32" else "windows"
        with mock.patch.object(BUILD, "load_project", return_value=self.description(platform)), \
                mock.patch.object(BUILD, "prepare_directory") as prepare:
            with self.assertRaises(BUILD.ExportError):
                BUILD.build(args)
            prepare.assert_not_called()

    def test_failed_command_clears_previous_success_before_running(self):
        platform = "windows" if sys.platform == "win32" else "linux"
        args = SimpleNamespace(project_directory=ROOT, engine_root=None, game_root=None,
                               build_directory=ROOT / "test-output/platform-core/unused",
                               cmake=sys.executable, configuration="Release", generator=None,
                               sdl_source_directory=None, sdl_cmake_package=None, sdl_license_file=None)
        writes = []
        def capture(path, content, **kwargs):
            writes.append((path.name, json.loads(content)))
        def fail(*args):
            self.assertEqual(writes[-1][1], {"built": False, "platform": platform})
            raise BUILD.ExportError("compiler failure")
        with mock.patch.object(BUILD, "load_project", return_value=self.description(platform)), \
                mock.patch.object(BUILD, "prepare_directory"), mock.patch.object(Path, "mkdir"), \
                mock.patch.object(Path, "write_text", capture), mock.patch.object(BUILD, "run", side_effect=fail):
            with self.assertRaisesRegex(BUILD.ExportError, "compiler failure"):
                BUILD.build(args)
        self.assertEqual(len(writes), 1)

    def test_linux_dependency_failure_cannot_record_build_success(self):
        args = SimpleNamespace(project_directory=ROOT, engine_root=None, game_root=None,
                               build_directory=ROOT / "test-output/platform-core/not-created-linux-build",
                               cmake=sys.executable, configuration="Release", generator=None,
                               sdl_source_directory=None, sdl_cmake_package=None, sdl_license_file=None)
        checks = {"machine": 62, "bundledLibraries": ["libSDL3.so.0"],
                  "abiCompatibilityVerified": False, "cleanMachineVerified": False}
        for fails in (False, True):
            writes = []
            def capture(path, content, **kwargs):
                writes.append(json.loads(content))
            with self.subTest(fails=fails), mock.patch.object(sys, "platform", "linux"), \
                    mock.patch.object(BUILD, "load_project", return_value=self.description("linux")), \
                    mock.patch.object(BUILD, "prepare_directory"), mock.patch.object(Path, "mkdir"), \
                    mock.patch.object(Path, "write_text", capture), mock.patch.object(BUILD, "run"), \
                    mock.patch.object(BUILD, "desktop_artifact", return_value=args.build_directory / "Game"), \
                    mock.patch.object(BUILD.native_linux, "check_package", return_value=checks,
                                      side_effect=BUILD.ExportError("missing Linux dependency") if fails else None):
                if fails:
                    with self.assertRaisesRegex(BUILD.ExportError, "missing Linux dependency"):
                        BUILD.build(args)
                    self.assertEqual(writes, [{"built": False, "platform": "linux"}])
                else:
                    result = BUILD.build(args)
                    self.assertEqual(result["linuxChecks"], checks)
                    self.assertTrue(result["artifactChecksPassed"])
                    self.assertFalse(result["executed"])
                    self.assertTrue(writes[-1]["built"])

    def test_linux_build_checks_declared_package_runtime_libraries(self):
        args = SimpleNamespace(project_directory=ROOT, engine_root=None, game_root=None,
                               build_directory=ROOT / "test-output/platform-core/not-created-linux-runtime-build",
                               cmake=sys.executable, configuration="Release", generator=None,
                               sdl_source_directory=None, sdl_cmake_package=None, sdl_license_file=None)
        description = self.description("linux")
        description["packageDependencies"] = {"linux-x86_64": [{
            "libraries": ["assets/packages/fixture/libfixture.so.1"],
            "runtimeFiles": ["assets/packages/fixture/libplugin.so"],
        }]}
        writes = []
        checks = {"machine": 62, "bundledLibraries": ["libfixture.so.1", "libplugin.so"],
                  "abiCompatibilityVerified": False, "cleanMachineVerified": False}
        with mock.patch.object(sys, "platform", "linux"), \
                mock.patch.object(BUILD, "load_project", return_value=description), \
                mock.patch.object(BUILD, "prepare_directory"), mock.patch.object(Path, "mkdir"), \
                mock.patch.object(Path, "write_text", lambda path, content, **kwargs: writes.append(json.loads(content))), \
                mock.patch.object(BUILD, "run"), \
                mock.patch.object(BUILD, "desktop_artifact", return_value=args.build_directory / "Game"), \
                mock.patch.object(BUILD.native_linux, "check_package", return_value=checks) as check_package:
            result = BUILD.build(args)

        check_package.assert_called_once_with(args.build_directory / "Game", ["libfixture.so.1", "libplugin.so"])
        self.assertEqual(result["linuxChecks"], checks)
        self.assertTrue(writes[-1]["built"])

    def test_windows_build_checks_declared_package_runtime_dlls(self):
        args = SimpleNamespace(project_directory=ROOT, engine_root=None, game_root=None,
                               build_directory=ROOT / "test-output/platform-core/not-created-windows-runtime-build",
                               cmake=sys.executable, configuration="Release", generator=None,
                               sdl_source_directory=None, sdl_cmake_package=None, sdl_license_file=None)
        description = self.description("windows")
        description["packageDependencies"] = {"windows-x86_64": [{
            "libraries": ["assets/packages/fixture/libfixture.lib"],
            "runtimeFiles": ["assets/packages/fixture/plugin.dll"],
        }]}
        writes = []
        checks = {"machine": 0x8664, "bundledLibraries": ["plugin.dll"],
                  "runtimeNoticeIncluded": False, "cleanMachineVerified": False,
                  "systemLibraries": []}
        with mock.patch.object(sys, "platform", "win32"), \
                mock.patch.object(BUILD, "load_project", return_value=description), \
                mock.patch.object(BUILD, "prepare_directory"), mock.patch.object(Path, "mkdir"), \
                mock.patch.object(Path, "write_text", lambda path, content, **kwargs: writes.append(json.loads(content))), \
                mock.patch.object(BUILD, "run"), \
                mock.patch.object(BUILD, "desktop_artifact", return_value=args.build_directory / "Game.exe"), \
                mock.patch.object(BUILD.native_windows, "check_package", return_value=checks) as check_package:
            result = BUILD.build(args)

        check_package.assert_called_once_with(args.build_directory / "Game.exe", ["plugin.dll"])
        self.assertEqual(result["windowsChecks"], checks)
        self.assertTrue(writes[-1]["built"])

    def test_linux_arm64_binary_cannot_pass_x86_64_export_gate(self):
        args = SimpleNamespace(project_directory=ROOT, engine_root=None, game_root=None,
                               build_directory=ROOT / "test-output/platform-core/not-created-linux-arm64-build",
                               cmake=sys.executable, configuration="Release", generator=None,
                               sdl_source_directory=None, sdl_cmake_package=None, sdl_license_file=None)
        checks = {"machine": 183, "bundledLibraries": [],
                  "abiCompatibilityVerified": False, "cleanMachineVerified": False}
        writes = []
        with mock.patch.object(sys, "platform", "linux"), \
                mock.patch.object(BUILD, "load_project", return_value=self.description("linux")), \
                mock.patch.object(BUILD, "prepare_directory"), mock.patch.object(Path, "mkdir"), \
                mock.patch.object(Path, "write_text", lambda path, content, **kwargs: writes.append(json.loads(content))), \
                mock.patch.object(BUILD, "run"), \
                mock.patch.object(BUILD, "desktop_artifact", return_value=args.build_directory / "Game"), \
                mock.patch.object(BUILD.native_linux, "check_package", return_value=checks):
            with self.assertRaisesRegex(BUILD.ExportError, "must target x86_64"):
                BUILD.build(args)
        self.assertEqual(writes, [{"built": False, "platform": "linux"}])


if __name__ == "__main__":
    unittest.main()
