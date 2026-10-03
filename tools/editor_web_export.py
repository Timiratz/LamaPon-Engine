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


# print_status(message: 表示する状態文)はUTF-8対応で状態を出力します。
def print_status(message: str) -> None:
    """ローカルのコードページにない文字もUTF-8で確実に出力します。"""
    # 現在の標準出力設定で出力を試す
    try:
        print(message, flush=True)
    # ローカル文字コードで表現できない場合にUTF-8へ切り替える
    except UnicodeEncodeError:
        # Windowsの英語環境などでは標準出力がcp1252になる場合があります。
        # エディターとCIはいずれもUTF-8を扱うため、再設定して再送します。
        # 標準出力をUTF-8に再設定できるか確認する
        reconfigure = getattr(sys.stdout, "reconfigure", None)
        # 再設定できる場合はUTF-8で再送する
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")
            print(message, flush=True)
            return
        # 既定の出力文字コード
        encoding = getattr(sys.stdout, "encoding", None) or "ascii"
        # 表現できない文字をエスケープした状態文
        escaped = message.encode(
            encoding, errors="backslashreplace").decode(encoding)
        print(escaped, flush=True)


# validate_output(project: プロジェクト設定, output: 出力先, engine: エンジンルート)は安全な配置先を返します。
def validate_output(project: Path, output: Path, engine: Path) -> Path:
    """プロジェクトやSDK、その親フォルダーを上書きする出力先を拒否します。"""
    # 絶対化した出力先
    output = output.resolve()
    # プロジェクトのルートディレクトリ
    root = project.parent.parent if project.parent.name == ".lamapon" else project.parent
    # 絶対化したプロジェクトルート
    root = root.resolve()
    # 絶対化したエンジンルート
    engine = engine.resolve()
    # 上書き禁止にするプロジェクト領域
    protected = [root / "assets", root / ".lamapon", root / ".git"]
    # 同一ルートにあるエンジンのソース領域を保護する
    if engine == root or engine in root.parents:
        # name: エンジン保護対象名; protected: 上書き禁止領域
        protected += [engine / name for name in ("src", "tools", "third_party", "assets", "cmake")]
    # 別ルートのエンジン全体を保護する
    else:
        protected.append(engine)
    # 出力先がプロジェクトまたはその親にならないことを確認する
    if output == root or output in root.parents or output == Path(output.anchor):
        raise ValueError("プロジェクトやその親フォルダーは出力先にできません。dist配下などを指定してください。")
    # 保護対象と重なる出力先を拒否する
    for source in protected:
        # 出力先と保護領域の重なりを確認する
        if output == source or source in output.parents or output in source.parents:
            raise ValueError("アセット・プロジェクト設定・エンジンSDKを含む場所には出力できません。")
    # 既存の出力先がディレクトリであることを確認する
    if output.exists() and not output.is_dir():
        raise ValueError("出力先にはファイルではなくフォルダーを指定してください。")
    # 既存ディレクトリの内容が本ツールのパッケージか確認する
    if output.is_dir() and any(output.iterdir()):
        # Webパッケージの識別マニフェスト
        manifest = output / "web-export-manifest.json"
        if not manifest.is_file() or json.loads(manifest.read_text(encoding="utf-8")).get("format") != "lamapon.web-export-manifest":
            raise ValueError("出力先にWebパッケージ以外のファイルがあります。空のフォルダーか新しい出力先を指定してください。")
    return output


# build_environment(emsdk: 任意のSDK位置)はビルド環境とemcmakeを返します。
def build_environment(emsdk: Path | None) -> tuple[dict[str, str], Path]:
    # 子プロセスに渡す環境
    env = os.environ.copy()
    env["PYTHONUTF8"] = "1"
    env["PYTHONUNBUFFERED"] = "1"
    env["EMSDK_PYTHON"] = sys.executable
    # SDKを明示指定した場合の実行パスを組み立てる
    if emsdk:
        # 絶対化したEmscripten SDKルート
        emsdk = emsdk.resolve()
        # emcmake起動スクリプト
        emcmake = emsdk / "upstream" / "emscripten" / "emcmake"
        # Pythonエントリーがあればそれを使う
        if emcmake.with_suffix(".py").is_file():
            # 同梱のPython起動スクリプト
            emcmake = emcmake.with_suffix(".py")
        # SDK設定ファイル
        config = emsdk / ".emscripten"
        # SDKの起動に必要なファイルを確認する
        if not emcmake.is_file() or not config.is_file():
            raise ValueError("Emscripten SDKが未設定です。emsdk install / activateを済ませたフォルダーを指定してください。")
        env["EMSDK"] = str(emsdk)
        env["EM_CONFIG"] = str(config)
        env["PATH"] = str(emcmake.parent) + os.pathsep + env.get("PATH", "")
    # 環境変数からemcmakeを検索する
    else:
        # PATH上で検出したemcmake
        found = shutil.which("emcmake")
        # SDKが見つからない場合は案内する
        if not found:
            raise ValueError("Emscripten SDKが見つかりません。「Webビルド環境」でSDKフォルダーを指定してください。")
        # 検出した起動スクリプト
        emcmake = Path(found)
        # .batをシェル経由で起動せず、同梱のPythonエントリーを使います。
        # Pythonエントリーがあればシェルを介さず使用する
        if emcmake.with_suffix(".py").is_file():
            # 同梱のPython起動スクリプト
            emcmake = emcmake.with_suffix(".py")
        # バッチファイルの場合はシェル起動用の拡張子を外す
        elif emcmake.suffix.lower() in {".bat", ".cmd"}:
            # シェル起動用拡張子を除いた実行名
            emcmake = emcmake.with_suffix("")
        # 起動スクリプトの存在を確認する
        if not emcmake.is_file():
            raise ValueError("Emscriptenのemcmakeスクリプトが見つかりません。SDKを確認してください。")
    # CMakeが環境にあることを確認する
    if not shutil.which("cmake", path=env.get("PATH")):
        raise ValueError("CMakeが見つかりません。CMakeをインストールし、エディターを起動し直してください。")
    return env, emcmake


# publish_package(stage: 完成パッケージ, output: 公開先)は完成品を安全に入れ替えます。
def publish_package(stage: Path, output: Path) -> None:
    """同じボリュームの完成品と入れ替え、失敗時には旧パッケージへ戻します。"""
    # 既存出力の退避先
    backup = output.with_name(output.name + ".previous-" + uuid.uuid4().hex)
    # 既存出力があるか
    replaced = output.exists()
    # 既存パッケージを一時退避する
    if replaced:
        output.rename(backup)
    # 完成品への入れ替えを試す
    try:
        stage.rename(output)
    # 入れ替え失敗時は既存パッケージを復元する
    except OSError:
        # 退避済みの旧パッケージを元に戻す
        if replaced:
            backup.rename(output)
        raise
    # 成功後に旧パッケージを削除する
    if replaced:
        # 公開後のバックアップ削除失敗で、完成品を失敗扱いにはしません。
        # 旧出力の削除を試す
        try:
            shutil.rmtree(backup)
        # 旧出力を残して公開成功を維持する
        except OSError as error:
            print_status(f"以前の出力を保持しました: {backup}: {error}")


# export(project: プロジェクト設定, output: 出力先, result_path: 実行結果, emsdk: 任意のSDK位置)はWeb出力結果を返します。
def export(project: Path, output: Path, result_path: Path, emsdk: Path | None) -> dict:
    # LamaPonエンジンルート
    engine = Path(__file__).resolve().parent.parent
    # 絶対化したプロジェクト設定
    project = project.resolve()
    # プロジェクト設定が存在することを確認する
    if not project.is_file():
        raise ValueError("プロジェクト設定が見つかりません。先にプロジェクトを保存してください。")
    # 安全性を検証した出力先
    output = validate_output(project, output, engine)
    # 出力プロセスの環境とemcmake
    env, emcmake = build_environment(emsdk)
    output.parent.mkdir(parents=True, exist_ok=True)
    # 出力と同じ親に用意するビルドステージ
    stage = Path(tempfile.mkdtemp(prefix=f".{output.name}-web-", dir=output.parent))
    # ステージを使ってWebパッケージをビルドする
    try:
        # Web出力スクリプトの実行引数
        command = [sys.executable, "-X", "utf8", "-u", str(engine / "tools" / "export_web.py"),
                   str(project), "--output", str(stage), "--emcmake", str(emcmake)]
        # Ninjaがあればビルドジェネレーターに指定する
        if shutil.which("ninja", path=env.get("PATH")):
            command.extend(["--generator", "Ninja"])
        print_status("Web互換性の検査とHTMLビルドを実行しています。")
        # Webビルドの終了状態
        completed = subprocess.run(command, env=env, check=False)
        # 互換性レポートのパス
        report = stage / "web-compatibility-report.json"
        # 互換性レポートの内容
        document = {}
        # レポートがあれば結果先へ複製して読み込む
        if report.is_file():
            shutil.copy2(report, result_path.parent / report.name)
            # 読み込んだ互換性レポート
            document = json.loads(report.read_text(encoding="utf-8"))
        # Webビルド失敗を利用者向けエラーにする
        if completed.returncode:
            # 既定の失敗メッセージ
            message = "Web出力に失敗しました。ログの内容を確認してください。"
            # 互換性レポートの拒否項目から原因を抽出する
            if report.is_file():
                # item: 検査結果; rejected: 拒否レベルの結果
                rejected = [item for item in document.get("findings", []) if item.get("level") == "reject"]
                # 拒否理由があれば詳細を表示する
                if rejected:
                    # item: 拒否結果; details: 表示する理由
                    details = [item.get("message", item.get("reason", str(item))) for item in rejected[:8]]
                    # 拒否理由をまとめた案内文
                    message = "このプロジェクトには現在のWeb出力で未対応の機能があります。\n" + "\n".join(details)
            raise ValueError(message)
        # 完成パッケージのマニフェスト
        manifest_path = stage / "web-export-manifest.json"
        # マニフェストの内容
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        # ビルド出力のHTML一覧
        html_files = sorted(stage.glob("LamaPonWebGL-*.html"))
        # HTMLが1つだけ生成され内容があることを確認する
        if len(html_files) != 1 or not html_files[0].stat().st_size:
            raise ValueError("完成したHTMLを確認できませんでした。既存の出力は保持しています。")
        # 公開するHTMLファイル名
        html_name = html_files[0].name
        publish_package(stage, output)
        # item: 互換性検査結果; warnings: 警告件数
        warnings = sum(item.get("level") == "warning" for item in document.get("findings", []))
        # 成功時の案内文
        message = "Web（HTML）形式での出力が完了しました。"
        # 警告がある場合は件数を追記する
        if warnings:
            # 警告件数を含めた案内文
            message += f"互換性の注意事項が{warnings}件あります。ビルドログを確認してください。"
        return {"ok": True, "outputDirectory": str(output), "htmlPath": str(output / html_name),
                "message": message, "manifest": manifest_path.name,
                "singleFile": manifest.get("target", {}).get("singleFile", False), "warningCount": warnings}
    # 失敗時もビルドステージを片付ける
    finally:
        # ステージが残っていれば削除する
        if stage.exists():
            shutil.rmtree(stage)


# main()はWeb出力を実行して結果ファイルを作成する。
def main() -> int:
    # CLI引数の定義
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--result", type=Path, required=True)
    parser.add_argument("--emsdk", type=Path)
    # CLI引数
    args = parser.parse_args()
    args.result.parent.mkdir(parents=True, exist_ok=True)
    # Web出力に失敗した場合も結果を記録する
    try:
        # Web出力の結果
        result = export(args.project, args.output, args.result, args.emsdk)
    # UIへエラー内容を返す
    except Exception as error:
        # 失敗結果
        result = {"ok": False, "message": str(error)}
    # UIは終了コードと、この実行で生成した結果ファイルを検査します。
    # 一時保存する結果ファイル
    temporary = args.result.with_suffix(".tmp")
    # JSON結果を一時保存する
    temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    # 完成した結果ファイルを公開する
    temporary.replace(args.result)
    print_status(result["message"])
    # 成功状態を終了コードで返す
    return 0 if result["ok"] else 1


# スクリプトとして実行した場合だけCLIを起動する
if __name__ == "__main__":
    raise SystemExit(main())
