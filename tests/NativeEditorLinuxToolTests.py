"""Test the WSL/Linux editor boundary without launching WSL or creating fixtures."""
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import editor_linux_export as LINUX_EDITOR
import editor_linux_build as LINUX_BUILD


class NativeEditorLinuxToolTests(unittest.TestCase):
    def test_wsl_command_keeps_distribution_as_a_literal_argument(self):
        self.assertEqual(LINUX_EDITOR.wsl_command("wsl.exe", "Ubuntu 24.04"),
                         ["wsl.exe", "--distribution", "Ubuntu 24.04"])
        self.assertEqual(LINUX_EDITOR.wsl_command("wsl.exe", ""), ["wsl.exe"])
        with self.assertRaises(LINUX_EDITOR.ExportError):
            LINUX_EDITOR.wsl_command("wsl.exe", "Ubuntu\n--exec rm")

    def test_windows_path_conversion_uses_wslpath_without_a_shell(self):
        path = ROOT / "パス with spaces & symbols"
        response = SimpleNamespace(returncode=0, stdout="/mnt/c/ゲーム/パス with spaces & symbols\n", stderr="")
        with mock.patch.object(LINUX_EDITOR.subprocess, "run", return_value=response) as run:
            result = LINUX_EDITOR.to_wsl_path("wsl.exe", "Ubuntu-24.04", path, must_exist=False)
        self.assertEqual(result, "/mnt/c/ゲーム/パス with spaces & symbols")
        command = run.call_args.args[0]
        self.assertEqual(command[:5], ["wsl.exe", "--distribution", "Ubuntu-24.04", "--exec", "wslpath"])
        self.assertEqual(command[5:8], ["-u", "-a", str(path.resolve())])
        self.assertNotIn("shell", run.call_args.kwargs)

    def test_wsl_path_failure_explains_distribution_and_drive_mount(self):
        response = SimpleNamespace(returncode=1, stdout="", stderr="WSL distribution not found")
        with mock.patch.object(LINUX_EDITOR.subprocess, "run", return_value=response):
            with self.assertRaisesRegex(LINUX_EDITOR.ExportError, "ディストリビューションとドライブ共有"):
                LINUX_EDITOR.to_wsl_path("wsl.exe", "missing", ROOT / "tests/native", must_exist=True)

    def test_linux_artifact_mapping_rejects_paths_outside_the_selected_output(self):
        output = Path("C:/project/dist/Linux")
        self.assertEqual(LINUX_EDITOR.windows_output_path(output, "/mnt/c/project/dist/Linux",
                                                          "/mnt/c/project/dist/Linux/build/Game"),
                         output / "build" / "Game")
        for artifact in ("/mnt/c/project/outside/Game", "/mnt/c/project/dist/Linux/../outside/Game"):
            with self.subTest(artifact=artifact), self.assertRaises(LINUX_EDITOR.ExportError):
                LINUX_EDITOR.windows_output_path(output, "/mnt/c/project/dist/Linux", artifact)

    def test_run_export_verifies_and_converts_the_linux_artifact_result(self):
        project = ROOT / "tests/native/CMakeLists.txt"
        engine = ROOT
        sdl = ROOT / "tests/native"
        output = ROOT / "test-output/platform-core/not-created-linux-export"
        result_path = ROOT / "test-output/platform-core/not-created-linux-result.json"
        inner = {"ok": True, "platform": "linux", "built": True, "artifactChecksPassed": True,
                 "buildProjectPath": "/mnt/c/game/dist/Linux", "artifactPath": "/mnt/c/game/dist/Linux/build/Game"}
        converted = iter(("/mnt/c/game/.lamapon/project.json", "/mnt/c/game/dist/Linux",
                         "/mnt/c/game/jobs/result.json", "/mnt/c/engine", "/mnt/c/dependencies/SDL"))
        original_is_file = Path.is_file
        original_read_text = Path.read_text
        original_open = Path.open
        original_stat = Path.stat
        writes = []
        def is_file(path):
            return path == result_path or path == output / "build" / "Game" or original_is_file(path)
        def read_text(path, *args, **kwargs):
            if path == result_path:
                return json.dumps(inner)
            return original_read_text(path, *args, **kwargs)
        def open_file(path, *args, **kwargs):
            if path == output / "build" / "Game":
                return io.BytesIO(b"\x7fELFfake linux executable")
            return original_open(path, *args, **kwargs)
        def stat(path, *args, **kwargs):
            if path == output / "build" / "Game":
                return SimpleNamespace(st_size=24)
            return original_stat(path, *args, **kwargs)
        with mock.patch.object(LINUX_EDITOR, "validate_output", return_value=output), \
                mock.patch.object(LINUX_EDITOR.shutil, "which", return_value="wsl.exe"), \
                mock.patch.object(LINUX_EDITOR, "to_wsl_path", side_effect=lambda *a, **k: next(converted)), \
                mock.patch.object(LINUX_EDITOR.subprocess, "run", return_value=SimpleNamespace(returncode=0)) as wsl_run, \
                mock.patch.object(Path, "is_file", is_file), \
                mock.patch.object(Path, "read_text", read_text), \
                mock.patch.object(Path, "open", open_file), \
                mock.patch.object(Path, "stat", stat), \
                mock.patch.object(Path, "write_text", lambda path, value, **kwargs: writes.append((path, value))):
            result = LINUX_EDITOR.run_export(project, output, result_path, engine, "Ubuntu-24.04", sdl)
        self.assertTrue(result["built"] and result["artifactChecksPassed"])
        self.assertEqual(result["artifactPath"], str(output / "build" / "Game"))
        self.assertIn("Steam Deck", result["message"])
        self.assertEqual(len(writes), 1)
        command = wsl_run.call_args.args[0]
        self.assertEqual(command[:7], ["wsl.exe", "--distribution", "Ubuntu-24.04", "--exec",
                                       "python3", "-B", "/mnt/c/engine/tools/editor_linux_build.py"])
        inner_arguments = dict(zip(command[7::2], command[8::2]))
        self.assertEqual(inner_arguments["--project"], "/mnt/c/game/.lamapon/project.json")
        self.assertEqual(inner_arguments["--sdl-source-directory"], "/mnt/c/dependencies/SDL")
        self.assertNotIn("shell", wsl_run.call_args.kwargs)

    def test_wsl_build_stages_a_verified_package_without_claiming_runtime_execution(self):
        project = ROOT / "tests/native/CMakeLists.txt"
        output = ROOT / "test-output/platform-core/not-created-inner-linux-output"
        sdl = ROOT / "tests/native"
        build_directory = output / "build"
        artifact = build_directory / "LamaPonGame"
        context = {"root": ROOT / "tests/native"}
        report = {"canGenerateBuildProject": True, "findings": []}
        writes = []
        original_resolve = Path.resolve
        original_is_file = Path.is_file
        original_exists = Path.exists

        def resolve(path, strict=False):
            if path in {output, build_directory, artifact}:
                return path
            return original_resolve(path, strict=strict)

        def exists(path):
            if path == output:
                return False
            return original_exists(path)

        def is_file(path):
            if path == artifact:
                return True
            return original_is_file(path)

        built = {"platform": "linux", "built": True, "artifactChecksPassed": True,
                 "artifactPath": str(artifact)}
        with mock.patch.object(LINUX_BUILD.export_native, "inspect_project", return_value=(report, context)), \
                mock.patch.object(LINUX_BUILD.export_native, "generate_build_project") as generate, \
                mock.patch.object(LINUX_BUILD.build_native, "build", return_value=built) as build, \
                mock.patch.object(Path, "resolve", resolve), \
                mock.patch.object(Path, "exists", exists), \
                mock.patch.object(Path, "is_file", is_file), \
                mock.patch.object(Path, "write_text", lambda path, text, **kwargs: writes.append((path, text))):
            result = LINUX_BUILD.build_linux(project, output, ROOT, sdl)
        self.assertTrue(result["built"] and result["artifactChecksPassed"])
        self.assertIn("Steam Deck", result["message"])
        self.assertEqual(result["artifactPath"], str(artifact))
        generate.assert_called_once_with(output, "linux", report, context)
        self.assertEqual(build.call_args.args[0].build_directory, build_directory)
        self.assertEqual(build.call_args.args[0].sdl_source_directory, sdl)
        self.assertIn("has not been launched", writes[0][1])


if __name__ == "__main__":
    unittest.main()
