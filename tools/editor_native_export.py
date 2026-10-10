#!/usr/bin/env python3
"""Generate native settings, optionally building a debug APK with existing tools."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import export_native
import build_native
from editor_web_export import validate_output


def export_project(project: Path, output: Path, platform: str, build_options=None) -> dict:
    if output.is_dir() and any(output.iterdir()):
        raise ValueError("ビルド設定の生成先は、新しいフォルダーまたは空のフォルダーを指定してください。")
    output = validate_output(project, output, export_native.portable.ENGINE_ROOT)
    report, context = export_native.inspect_project(project, platform)
    if not report["canGenerateBuildProject"]:
        messages = [item["message"] + "\n" + item["action"] for item in report["findings"]
                    if item["level"] == "reject"]
        return {"ok": False, "message": "\n\n".join(messages), "platform": platform}
    if build_options is not None:
        if platform != "android":
            raise ValueError("APKビルドの対象はAndroidです。")
        build_options.project_directory = output
        build_options.build_directory = output / "build"
        # Check installed SDK components before generating an output folder.
        build_native.android_command(build_options, export_native.build_project_description(platform, context),
                                     build_options.build_directory, export_native.portable.ENGINE_ROOT, context["root"])
    export_native.generate_build_project(output, platform, report, context)
    if build_options is not None:
        result = build_native.build(build_options)
        artifact = Path(result.get("artifactPath", ""))
        expected = output / "build/app/outputs/apk/debug/app-debug.apk"
        if result.get("built") is not True or result.get("artifactChecksPassed") is not True \
                or result.get("platform") != "android" or artifact.resolve() != expected.resolve() \
                or not artifact.is_file() or artifact.stat().st_size == 0:
            raise ValueError("APKのビルド結果と配布物検査を確認できませんでした。")
        return {"ok": True, "platform": platform, "built": True, "artifactChecksPassed": True,
                "buildProjectPath": str(output), "artifactPath": str(artifact),
                "message": "Android debug APKをビルドし、配布物・署名を検査しました。端末での動作は未確認です。"}
    return {"ok": True, "platform": platform, "buildProjectPath": str(output),
            "message": "ビルド設定を生成しました。ゲーム本体のビルドと動作確認はまだ行っていません。"}


def main() -> int:
    export_native.portable.configure_cli_output()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--platform", choices=("linux", "android"), required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--build-apk", action="store_true")
    for name in ("android-sdk", "java-home", "gradle-home", "sdl-source-directory"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--allow-downloads", action="store_true")
    args = parser.parse_args()
    try:
        args.engine_root = args.game_root = None
        result = export_project(args.project, args.output, args.platform, args if args.build_apk else None)
    except (OSError, ValueError, export_native.portable.ExportError) as error:
        result = {"ok": False, "platform": args.platform, "message": str(error)}
    args.result.parent.mkdir(parents=True, exist_ok=True)
    args.result.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(result["message"], flush=True)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
