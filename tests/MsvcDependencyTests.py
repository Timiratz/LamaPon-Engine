"""MSVC/Ninjaがヘッダー変更を検出することを実コンパイルで検査する。"""

import argparse
import hashlib
from pathlib import Path
import subprocess


def run(*command: str) -> str:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=60, check=False)
    output = result.stdout.decode("utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command}\n{output}")
    return output


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--engine-root", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()

    source = args.work_dir.resolve() / "source"
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
    header = source / "設定 value.h"
    header.write_text("#define TEST_VALUE 17\n", encoding="utf-8")
    (source / "main.cpp").write_text(
        '#include "設定 value.h"\nint GetValue() { return TEST_VALUE; }\n', encoding="utf-8")
    run(args.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
        f"-DENGINE_ROOT={args.engine_root.resolve().as_posix()}",
        f"-DCMAKE_MAKE_PROGRAM={args.ninja}", f"-DCMAKE_CXX_COMPILER={args.compiler}",
        "-DREPAIR_DEPENDENCIES=OFF")
    run(args.cmake, "--build", str(build))
    objects = list((build / "CMakeFiles" / "DependencyRegression.dir").rglob("*.obj"))
    if len(objects) != 1:
        raise AssertionError(f"Expected one object: {objects}")
    obj = objects[0]
    # ヘッダーとソースを変更せずに再構成し、依存情報を
    # 採取し直せることを検証します。
    run(args.cmake, "-S", str(source), "-B", str(build), "-DREPAIR_DEPENDENCIES=ON")
    run(args.cmake, "--build", str(build))
    before_hash = hashlib.sha256(obj.read_bytes()).digest()
    dependencies = run(args.ninja, "-C", str(build), "-t", "deps")
    if "value.h" not in dependencies:
        raise AssertionError(f"Ninja did not record the header:\n{dependencies}")

    # ソース・CMakeは触らず、ヘッダー内の値だけを変えて再コンパイルさせる。
    header.write_text("#define TEST_VALUE 29\n", encoding="utf-8")
    run(args.cmake, "--build", str(build))
    if hashlib.sha256(obj.read_bytes()).digest() == before_hash:
        raise AssertionError("Header edit did not change the compiled object")
    unchanged_time = obj.stat().st_mtime_ns
    run(args.cmake, "--build", str(build))
    if obj.stat().st_mtime_ns != unchanged_time:
        raise AssertionError("An unchanged build recompiled the object")
    print("Header edit rebuilt the object; unchanged build reused it.")


if __name__ == "__main__":
    main()
