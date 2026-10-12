#!/usr/bin/env python3
"""Generate and build a Linux game in an existing Linux environment."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import build_native
import export_native


def build_linux(project: Path, output: Path, engine_root: Path,
                sdl_source_directory: Path) -> dict:
    if sys.version_info < (3, 11):
        raise build_native.ExportError("Linux export requires Python 3.11 or newer in WSL")
    project = project.resolve(strict=True)
    engine_root = engine_root.resolve(strict=True)
    sdl_source_directory = sdl_source_directory.resolve(strict=True)
    if not project.is_file() or not (sdl_source_directory / "CMakeLists.txt").is_file():
        raise build_native.ExportError("WSL requires the existing project file and SDL3 source directory")
    if engine_root != export_native.portable.ENGINE_ROOT.resolve(strict=True):
        raise build_native.ExportError("Linux export tools and engine source must come from the same LamaPon folder")

    output = output.resolve()
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise build_native.ExportError("Linux output must be a new or empty directory")
    report, context = export_native.inspect_project(project, "linux")
    if not report["canGenerateBuildProject"]:
        messages = [item["message"] + "\n" + item["action"] for item in report["findings"]
                    if item["level"] == "reject"]
        raise build_native.ExportError("\n\n".join(messages))
    export_native.generate_build_project(output, "linux", report, context)

    options = SimpleNamespace(
        project_directory=output,
        build_directory=output / "build",
        engine_root=engine_root,
        game_root=context["root"],
        configuration="Release",
        cmake="cmake",
        generator=None,
        sdl_source_directory=sdl_source_directory,
        sdl_cmake_package=None,
        sdl_license_file=None,
        android_sdk=None,
        java_home=None,
        gradle_home=None,
        allow_downloads=False,
    )
    result = build_native.build(options)
    artifact = Path(result["artifactPath"]).resolve(strict=True)
    try:
        artifact.relative_to(options.build_directory.resolve(strict=True))
    except ValueError as error:
        raise build_native.ExportError("Linux executable escaped the build output directory") from error
    if result.get("platform") != "linux" or result.get("built") is not True \
            or result.get("artifactChecksPassed") is not True or not artifact.is_file():
        raise build_native.ExportError("Linux build or package dependency inspection did not succeed")
    readme = output / "README.txt"
    readme.write_text(
        "This folder contains a Linux game binary built with the existing WSL toolchain.\n"
        f"Executable: {artifact.relative_to(output).as_posix()}\n"
        "The ELF, staged assets, licenses and shared-library dependencies passed package checks.\n"
        "The game has not been launched on Linux or Steam Deck; test it on each target system.\n"
        "The generated CMake build project and native-build-result.json are retained for review.\n",
        encoding="utf-8",
    )
    return {
        "ok": True,
        "platform": "linux",
        "built": True,
        "artifactChecksPassed": True,
        "buildProjectPath": str(output),
        "artifactPath": str(artifact),
        "message": "Linuxゲームをビルドし、ELF・アセット・ライセンス・共有ライブラリ依存を検査しました。Linux／Steam Deckでの起動確認は別途必要です。",
    }


def main() -> int:
    export_native.portable.configure_cli_output()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--engine-root", type=Path, required=True)
    parser.add_argument("--sdl-source-directory", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = build_linux(args.project, args.output, args.engine_root, args.sdl_source_directory)
        code = 0
    except (OSError, ValueError, KeyError, build_native.ExportError) as error:
        result = {"ok": False, "platform": "linux", "message": str(error)}
        code = 1
    args.result.parent.mkdir(parents=True, exist_ok=True)
    args.result.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(result["message"], flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
