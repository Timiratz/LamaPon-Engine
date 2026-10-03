"""MSVC/Ninjaがヘッダー変更を検出することを実コンパイルで検査する。"""

import argparse
import hashlib
from pathlib import Path
import subprocess


# run(command: 実行するプロセスと引数)
def run(*command: str) -> str:
    """commandを実行し、失敗時は出力付き例外にします。"""
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=60, check=False)
    # 標準出力と標準エラーをまとめたUTF-8文字列
    output = result.stdout.decode("utf-8", errors="replace")
    # 外部コマンドの失敗を呼び出し元へ伝える
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command}\n{output}")
    return output


def main() -> None:
    """依存ファイルの変更時に再コンパイルされることを検証します。"""
    # コマンドライン引数の定義
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--engine-root", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    # 検証対象の引数
    args = parser.parse_args()

    # 一時検証用のソースとビルド配置
    source = args.work_dir.resolve() / "source"
    # 検証プロジェクトのビルド配置
    build = args.work_dir.resolve() / "build"
    source.mkdir(parents=True, exist_ok=True)
    (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.25)
project(DependencyRegression LANGUAGES CXX)
if(REPAIR_DEPENDENCIES)
    include("${ENGINE_ROOT}/cmake/LamaPonMsvcDependencies.cmake")
    lamapon_configure_msvc_dependencies()
else()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "Incorrect prefix: ")
endif()
add_library(DependencyRegression OBJECT main.cpp)
target_compile_options(DependencyRegression PRIVATE /utf-8)
''', encoding="utf-8")
    # 日本語と空白のあるヘッダーでも、依存パスが欠落しないことを確認。
    # 日本語ファイル名の依存ヘッダー
    header = source / "設定 value.h"
    header.write_text("#define TEST_VALUE 17\n", encoding="utf-8")
    (source / "main.cpp").write_text(
        '#include "設定 value.h"\nint GetValue() { return TEST_VALUE; }\n', encoding="utf-8")
    # Ninja構成と初回コンパイル
    run(args.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
        f"-DENGINE_ROOT={args.engine_root.resolve().as_posix()}",
        f"-DCMAKE_MAKE_PROGRAM={args.ninja}", f"-DCMAKE_CXX_COMPILER={args.compiler}",
        "-DREPAIR_DEPENDENCIES=OFF")
    run(args.cmake, "--build", str(build))
    # 検証用ターゲットのオブジェクト一覧
    objects = list((build / "CMakeFiles" / "DependencyRegression.dir").rglob("*.obj"))
    # ターゲットは単一オブジェクトを生成する
    if len(objects) != 1:
        raise AssertionError(f"Expected one object: {objects}")
    # 再コンパイルを比較するオブジェクト
    obj = objects[0]
    # ヘッダーとソースを変更せずに再構成し、依存情報を採取し直せることを検証します。
    # 修正済み依存設定へ再構成して再ビルド
    run(args.cmake, "-S", str(source), "-B", str(build), "-DREPAIR_DEPENDENCIES=ON")
    run(args.cmake, "--build", str(build))
    # 再構成後オブジェクトの内容ハッシュ
    before_hash = hashlib.sha256(obj.read_bytes()).digest()
    # Ninjaが記録した依存ファイル一覧
    dependencies = run(args.ninja, "-C", str(build), "-t", "deps")
    # 依存ヘッダーが記録されていることを確認
    if "value.h" not in dependencies:
        raise AssertionError(f"Ninja did not record the header:\n{dependencies}")

    # ソース・CMakeは触らず、ヘッダー内の値だけを変えて再コンパイルさせる。
    header.write_text("#define TEST_VALUE 29\n", encoding="utf-8")
    run(args.cmake, "--build", str(build))
    # ヘッダー変更後にオブジェクトが更新されたことを確認
    if hashlib.sha256(obj.read_bytes()).digest() == before_hash:
        raise AssertionError("Header edit did not change the compiled object")
    # 変更後オブジェクトの更新時刻
    unchanged_time = obj.stat().st_mtime_ns
    run(args.cmake, "--build", str(build))
    # 入力不変なら再コンパイルしないことを確認
    if obj.stat().st_mtime_ns != unchanged_time:
        raise AssertionError("An unchanged build recompiled the object")
    print("Header edit rebuilt the object; unchanged build reused it.")


# 直接実行時だけ回帰検証を開始
if __name__ == "__main__":
    main()
