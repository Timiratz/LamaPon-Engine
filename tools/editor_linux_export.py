#!/usr/bin/env python3
"""Build a Linux game from the Windows editor through an existing WSL distro."""
from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import subprocess
import sys

from editor_web_export import validate_output
from export_web import ExportError, configure_cli_output


def wsl_command(executable: str, distribution: str) -> list[str]:
    command = [executable]
    if distribution:
        if any(character in distribution for character in "\0\r\n"):
            raise ExportError("WSLディストリビューション名に改行や制御文字は使用できません。")
        command += ["--distribution", distribution]
    return command


def to_wsl_path(executable: str, distribution: str, path: Path, *, must_exist: bool) -> str:
    path = path.resolve(strict=must_exist)
    command = wsl_command(executable, distribution) + ["--exec", "wslpath", "-u", "-a", str(path)]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                            timeout=30, check=False)
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise ExportError("WSLパスを解決できません。ディストリビューションとドライブ共有を確認してください。"
                          + ("\n" + detail if detail else ""))
    converted = result.stdout.strip()
    if not converted.startswith("/") or "\0" in converted or "\n" in converted or "\r" in converted:
        raise ExportError("WSLから絶対Linuxパスを取得できませんでした。")
    return PurePosixPath(converted).as_posix()


def windows_output_path(output: Path, output_wsl: str, artifact_wsl: str) -> Path:
    output_root = PurePosixPath(output_wsl)
    artifact = PurePosixPath(artifact_wsl)
    try:
        relative = artifact.relative_to(output_root)
    except ValueError as error:
        raise ExportError("Linux実行ファイルが選択した出力先の外にあります。") from error
    if not relative.parts or any(part in {"", ".", ".."} for part in relative.parts):
        raise ExportError("Linux実行ファイルの出力パスが不正です。")
    return output / Path(*PureWindowsPath(*relative.parts).parts)


def run_export(project: Path, output: Path, result_path: Path, engine_root: Path,
               distribution: str, sdl_source_directory: Path, wsl_executable: str | None = None) -> dict:
    project = project.resolve(strict=True)
    engine_root = engine_root.resolve(strict=True)
    sdl_source_directory = sdl_source_directory.resolve(strict=True)
    if not project.is_file() or not (engine_root / "tools/editor_linux_build.py").is_file():
        raise ExportError("LamaPonプロジェクトまたはLinux出力ツールが見つかりません。")
    if not (sdl_source_directory / "CMakeLists.txt").is_file():
        raise ExportError("SDL3 3.4以降のソースフォルダーを指定してください。")

    output = validate_output(project, output, engine_root)
    wsl = wsl_executable or shutil.which("wsl.exe") or shutil.which("wsl")
    if not wsl:
        raise ExportError("WSLが見つかりません。既存のLinuxビルド環境を用意するか、ビルド設定生成を選んでください。")

    linux_project = to_wsl_path(wsl, distribution, project, must_exist=True)
    linux_output = to_wsl_path(wsl, distribution, output, must_exist=False)
    linux_result = to_wsl_path(wsl, distribution, result_path, must_exist=False)
    linux_engine = to_wsl_path(wsl, distribution, engine_root, must_exist=True)
    linux_sdl = to_wsl_path(wsl, distribution, sdl_source_directory, must_exist=True)
    linux_script = PurePosixPath(linux_engine) / "tools/editor_linux_build.py"
    command = wsl_command(wsl, distribution) + [
        "--exec", "python3", "-B", linux_script.as_posix(),
        "--project", linux_project,
        "--output", linux_output,
        "--result", linux_result,
        "--engine-root", linux_engine,
        "--sdl-source-directory", linux_sdl,
    ]
    completed = subprocess.run(command, check=False)
    if not result_path.is_file():
        raise ExportError("WSLビルド結果を取得できません。ビルドログを確認してください。")
    try:
        result = json.loads(result_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        raise ExportError("WSLビルド結果のJSONを読み取れません。") from error
    if not isinstance(result, dict):
        raise ExportError("WSLビルド結果の形式が不正です。")
    if completed.returncode or result.get("ok") is not True:
        message = result.get("message")
        raise ExportError(message if isinstance(message, str) and message else "WSLでLinuxゲームをビルドできませんでした。")
    if result.get("platform") != "linux" or result.get("built") is not True \
            or result.get("artifactChecksPassed") is not True:
        raise ExportError("WSLビルドでLinux配布物の検査が完了しませんでした。")

    artifact = windows_output_path(output, linux_output, result.get("artifactPath", ""))
    if not artifact.is_file() or artifact.stat().st_size == 0:
        raise ExportError("Linux実行ファイルがWindows側の出力先に見つかりません。")
    with artifact.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            raise ExportError("出力されたゲームはLinux ELF実行ファイルではありません。")
    result["buildProjectPath"] = str(output)
    result["artifactPath"] = str(artifact.resolve())
    result["message"] = ("Linuxゲームをビルドし、ELF・アセット・ライセンス・共有ライブラリ依存を検査しました。"
                          "Linux／Steam Deckでの起動確認は別途必要です。")
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    configure_cli_output()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux",), required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--engine-root", type=Path, required=True)
    parser.add_argument("--sdl-source-directory", type=Path, required=True)
    parser.add_argument("--distribution", default="", help="Existing WSL distribution; empty uses the default")
    parser.add_argument("--wsl-executable", help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        result = run_export(args.project, args.output, args.result, args.engine_root,
                            args.distribution, args.sdl_source_directory, args.wsl_executable)
        print(result["message"], flush=True)
        return 0
    except (OSError, ValueError, ExportError, subprocess.TimeoutExpired) as error:
        result = {"ok": False, "platform": "linux", "message": str(error)}
        args.result.parent.mkdir(parents=True, exist_ok=True)
        args.result.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(result["message"], file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
