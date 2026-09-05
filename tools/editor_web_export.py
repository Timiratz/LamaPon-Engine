#!/usr/bin/env python3
"""エディターのWeb出力を準備し、成功したパッケージだけを公開します。"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid


def validate_output(project: Path, output: Path, engine: Path) -> Path:
    """プロジェクトやSDK、その親フォルダーを上書きする出力先を拒否します。"""
    output = output.resolve()
    root = project.parent.parent if project.parent.name == ".lamapon" else project.parent
    root = root.resolve()
    engine = engine.resolve()
    protected = [root / "assets", root / ".lamapon", root / ".git"]
    if engine == root or engine in root.parents:
        protected += [engine / name for name in ("src", "tools", "third_party", "assets", "cmake")]
    else:
        protected.append(engine)
    if output == root or output in root.parents or output == Path(output.anchor):
        raise ValueError("プロジェクトやその親フォルダーは出力先にできません。dist配下などを指定してください。")
    for source in protected:
        if output == source or source in output.parents or output in source.parents:
            raise ValueError("アセット・プロジェクト設定・エンジンSDKを含む場所には出力できません。")
    if output.exists() and not output.is_dir():
        raise ValueError("出力先にはファイルではなくフォルダーを指定してください。")
    if output.is_dir() and any(output.iterdir()):
        manifest = output / "web-export-manifest.json"
        if not manifest.is_file() or json.loads(manifest.read_text(encoding="utf-8")).get("format") != "lamapon.web-export-manifest":
            raise ValueError("出力先にWebパッケージ以外のファイルがあります。空のフォルダーか新しい出力先を指定してください。")
    return output


def build_environment(emsdk: Path | None) -> tuple[dict[str, str], Path]:
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    env["PYTHONUNBUFFERED"] = "1"
    env["EMSDK_PYTHON"] = sys.executable
    if emsdk:
        emsdk = emsdk.resolve()
        emcmake = emsdk / "upstream" / "emscripten" / "emcmake"
        if emcmake.with_suffix(".py").is_file():
            emcmake = emcmake.with_suffix(".py")
        config = emsdk / ".emscripten"
        if not emcmake.is_file() or not config.is_file():
            raise ValueError("Emscripten SDKが未設定です。emsdk install / activateを済ませたフォルダーを指定してください。")
        env["EMSDK"] = str(emsdk)
        env["EM_CONFIG"] = str(config)
        env["PATH"] = str(emcmake.parent) + os.pathsep + env.get("PATH", "")
    else:
        found = shutil.which("emcmake")
        if not found:
            raise ValueError("Emscripten SDKが見つかりません。「Webビルド環境」でSDKフォルダーを指定してください。")
        emcmake = Path(found)
        # .batをシェル経由で起動せず、同梱のPythonエントリーを使います。
        if emcmake.with_suffix(".py").is_file():
            emcmake = emcmake.with_suffix(".py")
        elif emcmake.suffix.lower() in {".bat", ".cmd"}:
            emcmake = emcmake.with_suffix("")
        if not emcmake.is_file():
            raise ValueError("Emscriptenのemcmakeスクリプトが見つかりません。SDKを確認してください。")
    if not shutil.which("cmake", path=env.get("PATH")):
        raise ValueError("CMakeが見つかりません。CMakeをインストールし、エディターを起動し直してください。")
    return env, emcmake


def publish_package(stage: Path, output: Path) -> None:
    """同じボリュームの完成品と入れ替え、失敗時には旧パッケージへ戻します。"""
    backup = output.with_name(output.name + ".previous-" + uuid.uuid4().hex)
    replaced = output.exists()
    if replaced:
        output.rename(backup)
    try:
        stage.rename(output)
    except OSError:
        if replaced:
            backup.rename(output)
        raise
    if replaced:
        # 公開後のバックアップ削除失敗で、完成品を失敗扱いにはしません。
        try:
            shutil.rmtree(backup)
        except OSError as error:
            print(f"以前の出力を保持しました: {backup}: {error}", flush=True)


def export(project: Path, output: Path, result_path: Path, emsdk: Path | None) -> dict:
    engine = Path(__file__).resolve().parent.parent
    project = project.resolve()
    if not project.is_file():
        raise ValueError("プロジェクト設定が見つかりません。先にプロジェクトを保存してください。")
    output = validate_output(project, output, engine)
    env, emcmake = build_environment(emsdk)
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{output.name}-web-", dir=output.parent))
    try:
        command = [sys.executable, "-X", "utf8", "-u", str(engine / "tools" / "export_web.py"),
                   str(project), "--output", str(stage), "--emcmake", str(emcmake)]
        if shutil.which("ninja", path=env.get("PATH")):
            command.extend(["--generator", "Ninja"])
        print("Web互換性の検査とHTMLビルドを実行しています。", flush=True)
        completed = subprocess.run(command, env=env, check=False)
        report = stage / "web-compatibility-report.json"
        document = {}
        if report.is_file():
            shutil.copy2(report, result_path.parent / report.name)
            document = json.loads(report.read_text(encoding="utf-8"))
        if completed.returncode:
            message = "Web出力に失敗しました。ログの内容を確認してください。"
            if report.is_file():
                rejected = [item for item in document.get("findings", []) if item.get("level") == "reject"]
                if rejected:
                    details = [item.get("message", item.get("reason", str(item))) for item in rejected[:8]]
                    message = "このプロジェクトには現在のWeb出力で未対応の機能があります。\n" + "\n".join(details)
            raise ValueError(message)
        manifest_path = stage / "web-export-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        html_files = sorted(stage.glob("LamaPonWebGL-*.html"))
        if len(html_files) != 1 or not html_files[0].stat().st_size:
            raise ValueError("完成したHTMLを確認できませんでした。既存の出力は保持しています。")
        html_name = html_files[0].name
        publish_package(stage, output)
        warnings = sum(item.get("level") == "warning" for item in document.get("findings", []))
        message = "Web（HTML）形式での出力が完了しました。"
        if warnings:
            message += f"互換性の注意事項が{warnings}件あります。ビルドログを確認してください。"
        return {"ok": True, "outputDirectory": str(output), "htmlPath": str(output / html_name),
                "message": message, "manifest": manifest_path.name,
                "singleFile": manifest.get("target", {}).get("singleFile", False), "warningCount": warnings}
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--emsdk", type=Path)
    args = parser.parse_args()
    args.result.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = export(args.project, args.output, args.result, args.emsdk)
    except Exception as error:
        result = {"ok": False, "message": str(error)}
    # UIは終了コードと、この実行で生成した結果ファイルを検査します。
    temporary = args.result.with_suffix(".tmp")
    temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(args.result)
    print(result["message"], flush=True)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
