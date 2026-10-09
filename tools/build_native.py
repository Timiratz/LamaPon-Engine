#!/usr/bin/env python3
"""Build a generated native project with existing tools; never installs SDKs.

The caller authorizes the chosen build directory. Gradle dependency retrieval
requires --allow-downloads; SDK/NDK components must already exist even then.
Build success does not claim runtime, device or Steam Deck verification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import sys
import zipfile

from export_web import ExportError, configure_cli_output, is_within
import native_android
import native_windows
import native_linux


def load_project(directory: Path) -> dict:
    description = json.loads((directory / "native-build-project.json").read_text(encoding="utf-8"))
    if not isinstance(description, dict) or description.get("format") != "lamapon.native-build-project" \
            or type(description.get("version")) is not int or description["version"] != 2:
        raise ExportError("Regenerate the native build project with the current exporter")
    if description.get("platform") not in {"windows", "linux", "android"} \
            or not isinstance(description.get("target"), str) \
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.+-]*", description["target"]):
        raise ExportError("Native build target is invalid")
    if not (directory / "CMakeLists.txt").is_file():
        raise ExportError("Generated CMakeLists.txt is missing")
    scene_asset(description)
    asset_directory = description.get("assetDirectory")
    includes = description.get("assetIncludePaths")
    if not safe_asset_path(asset_directory) or not isinstance(includes, list) or not includes \
            or any(not safe_asset_path(value, allow_root=True) for value in includes):
        raise ExportError("Native asset selection is invalid; regenerate the build project")
    dependencies = description.get("packageDependencies", {})
    if description["platform"] == "android":
        android = description.get("android")
        abis = android.get("abis") if isinstance(android, dict) else None
        if not isinstance(abis, list) or not abis or any(not isinstance(abi, str) or abi not in ("arm64-v8a", "x86_64") for abi in abis) \
                or len(set(abis)) != len(abis):
            raise ExportError("Native Android ABI list is invalid; regenerate the build project")
        allowed_variants = {"android-" + abi for abi in abis}
    else:
        allowed_variants = {description["platform"] + "-x86_64"}
    if not isinstance(dependencies, dict) or set(dependencies) - allowed_variants:
        raise ExportError("Native package dependency targets are invalid; regenerate the build project")
    for variant, entries in dependencies.items():
        if not isinstance(entries, list):
            raise ExportError("Native package dependency list is invalid; regenerate the build project")
        for entry in entries:
            if not isinstance(entry, dict) or entry.get("variant") != variant \
                    or not isinstance(entry.get("package"), str) or not entry["package"]:
                raise ExportError("Native package dependency metadata is invalid; regenerate the build project")
            for field in ("includeDirectories", "sources", "libraries", "runtimeFiles", "defines"):
                values = entry.get(field)
                if not isinstance(values, list) or any(not isinstance(value, str) or not value for value in values):
                    raise ExportError("Native package dependency fields are invalid; regenerate the build project")
                if field != "defines" and any(not safe_asset_path(value) for value in values):
                    raise ExportError("Native package paths are unsafe; regenerate the build project")
                if field == "sources" and any(PurePosixPath(value).suffix.lower() not in {".c", ".cc", ".cpp", ".cxx"}
                                               for value in values):
                    raise ExportError("Native package sources must be C/C++ files; regenerate the build project")
            licenses = entry.get("licenseFiles")
            if not isinstance(licenses, list) or any(not isinstance(item, dict)
                    or not safe_asset_path(item.get("source"))
                    or not safe_asset_path(item.get("destination")) for item in licenses):
                raise ExportError("Native package license metadata is invalid; regenerate the build project")
    return description


def safe_asset_path(value, allow_root=False) -> bool:
    return isinstance(value, str) and (allow_root and value == "." or bool(value)
        and not value.startswith("/") and "\\" not in value and ":" not in value
        and all(part not in {"", ".", ".."} for part in value.split("/")))


def expected_assets(description: dict, project: Path, engine: Path, game: Path) -> dict[str, Path]:
    root = (game / description["assetDirectory"]).resolve(strict=True)
    if not is_within(root, game):
        raise ExportError("Native asset directory escapes the game project")
    expected = {}
    for relative in description["assetIncludePaths"]:
        included = root / relative
        if not included.exists() or not is_within(included.resolve(), root):
            raise ExportError("Native asset selection is missing or escapes its directory")
        for source in included.rglob("*") if included.is_dir() else (included,):
            if source.is_file():
                if not is_within(source.resolve(), root):
                    raise ExportError("Native asset link escapes its directory")
                expected[source.relative_to(root).as_posix()] = source
    expected["lamapon-default-font.ttf"] = engine / "third_party/imgui/misc/fonts/ProggyClean.ttf"
    expected["lamapon-input-actions.json"] = project / "lamapon-input-actions.json"
    return expected


def check_desktop_assets(staged: Path, description: dict, project: Path, engine: Path, game: Path) -> None:
    expected = expected_assets(description, project, engine, game)
    actual = {path.relative_to(staged).as_posix(): path for path in staged.rglob("*") if path.is_file()}
    if actual.keys() != expected.keys():
        raise ExportError("Staged game assets differ from the current selection; use a new empty build directory")
    for relative, source in expected.items():
        destination = actual[relative]
        if not is_within(destination.resolve(), staged.resolve()):
            raise ExportError("Staged asset link escapes the game output")
        with source.open("rb") as original, destination.open("rb") as packaged:
            if hashlib.file_digest(original, "sha256").digest() != hashlib.file_digest(packaged, "sha256").digest():
                raise ExportError("Staged game asset content is outdated: " + relative)


def scene_asset(description: dict) -> str:
    scene = description.get("scenePath")
    if not isinstance(scene, str) or not scene.startswith("/assets/") or "\\" in scene \
            or any(part in {"", ".", ".."} for part in scene[1:].split("/")) \
            or PurePosixPath(scene).suffix != ".json":
        raise ExportError("Native startup scene must be a path within /assets")
    return scene[1:]


def prepare_directory(directory: Path, project: Path, engine: Path, game: Path) -> None:
    if directory in {project, engine, game} or directory in engine.parents or directory in game.parents:
        raise ExportError("Build directory cannot replace the project, game or engine")
    for root in (engine / "src", engine / "tools", engine / "cmake", engine / "third_party",
                 game / "assets", game / ".lamapon", game / ".git"):
        if is_within(directory, root) or is_within(root, directory):
            raise ExportError("Build directory overlaps source or project settings")
    owner = directory / "lamapon-native-build-owner.json"
    expected = {"format": "lamapon.native-build-owner", "version": 1, "project": str(project)}
    if directory.exists():
        if not directory.is_dir():
            raise ExportError("Build directory must be a directory")
        if any(directory.iterdir()) and (not owner.is_file()
                or json.loads(owner.read_text(encoding="utf-8")) != expected):
            raise ExportError("Build directory must be empty or owned by this generated project")
    directory.mkdir(parents=True, exist_ok=True)
    owner.write_text(json.dumps(expected, indent=2) + "\n", encoding="utf-8")


def executable(value: str) -> str:
    path = Path(value)
    resolved = str(path.resolve()) if path.is_file() else shutil.which(value)
    if not resolved or Path(resolved).suffix.lower() in {".bat", ".cmd", ".ps1"}:
        raise ExportError("Specify an existing native executable: " + value)
    return resolved


def run(command: list[str], cwd: Path, environment: dict, log: Path) -> None:
    with log.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(command, ensure_ascii=False) + "\n")
        stream.flush()
        result = subprocess.run(command, cwd=cwd, env=environment, stdout=stream, stderr=subprocess.STDOUT,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0) if sys.platform == "win32" else 0)
    if result.returncode:
        raise ExportError(f"Build command failed ({result.returncode}); inspect {log}")


def gradle_jvm_property(name: str, path: Path) -> str:
    # Gradle's ArgumentsSplitter supports quoted sections, but no backslash
    # escaping. Choose a quote absent from the path so spaces remain literal.
    value = f"-D{name}={path.as_posix()}"
    for quote in ('"', "'"):
        if quote not in value:
            return quote + value + quote
    raise ExportError("Android build path cannot contain both single and double quotes")


def desktop_artifact(directory: Path, description: dict, configuration: str,
                     project: Path, engine: Path, game: Path) -> Path:
    target = description["target"] + (".exe" if description["platform"] == "windows" else "")
    candidates = [directory / configuration / target, directory / target]
    artifacts = [path for path in candidates if path.is_file()]
    if len(artifacts) != 1 or artifacts[0].stat().st_size == 0:
        raise ExportError("Build did not produce the expected game executable")
    artifact = artifacts[0]
    with artifact.open("rb") as stream:
        magic = stream.read(4)
    if (description["platform"] == "windows" and not magic.startswith(b"MZ")) \
            or (description["platform"] == "linux" and magic != b"\x7fELF"):
        raise ExportError("Game output is not an executable for the target platform")
    for relative in (scene_asset(description), "assets/lamapon-default-font.ttf", "assets/lamapon-input-actions.json",
                     "licenses/LamaPon.txt", "licenses/SDL3.txt", "licenses/ProggyClean.txt", "licenses/imgui.txt",
                     "licenses/nlohmann.txt", "licenses/cgltf.txt", "licenses/stb.txt"):
        staged = artifact.parent / relative
        if not staged.is_file() or staged.stat().st_size == 0:
            raise ExportError("Game dependency or license was not staged: " + relative)
    check_desktop_assets(artifact.parent / "assets", description, project, engine, game)
    return artifact


def android_command(args, description: dict, directory: Path, engine: Path, game: Path) -> tuple[list[str], dict]:
    if not args.android_sdk or not args.java_home or not args.gradle_home or not args.sdl_source_directory:
        raise ExportError("Android requires existing --android-sdk, --java-home, --gradle-home and --sdl-source-directory")
    config = native_android.android_settings({"project": {"export": {"native": {"android": description["android"]}}},
                                             "root": game})
    sdk = args.android_sdk.resolve(strict=True)
    jdk = args.java_home.resolve(strict=True)
    gradle = args.gradle_home.resolve(strict=True)
    suffix = ".exe" if sys.platform == "win32" else ""
    java = executable(str(jdk / "bin" / ("java" + suffix)))
    ndk = sdk / "ndk" / native_android.ANDROID_NDK_VERSION
    cmake = sdk / "cmake" / native_android.ANDROID_CMAKE_VERSION
    build_tools = sdk / "build-tools" / native_android.ANDROID_BUILD_TOOLS_VERSION
    required = [
        (f"platforms;android-{config['compileSdk']}", sdk / "platforms" / f"android-{config['compileSdk']}" / "android.jar"),
        ("JDK javac", jdk / "bin" / ("javac" + suffix)),
        (f"ndk;{native_android.ANDROID_NDK_VERSION}", ndk / "build/cmake/android.toolchain.cmake"),
        (f"ndk;{native_android.ANDROID_NDK_VERSION}", ndk / "NOTICE"),
        (f"ndk;{native_android.ANDROID_NDK_VERSION}", ndk / "NOTICE.toolchain"),
        (f"cmake;{native_android.ANDROID_CMAKE_VERSION}", cmake / "bin" / ("cmake" + suffix)),
        (f"cmake;{native_android.ANDROID_CMAKE_VERSION}", cmake / "bin" / ("ninja" + suffix)),
        (f"build-tools;{native_android.ANDROID_BUILD_TOOLS_VERSION}", build_tools / ("aapt2" + suffix)),
        (f"build-tools;{native_android.ANDROID_BUILD_TOOLS_VERSION}", build_tools / ("zipalign" + suffix)),
        (f"build-tools;{native_android.ANDROID_BUILD_TOOLS_VERSION}", build_tools / "lib/apksigner.jar"),
        ("SDL3 Java sources", args.sdl_source_directory / "android-project/app/src/main/java/org/libsdl/app/SDLActivity.java"),
    ]
    missing = [(component, path) for component, path in required if not path.is_file()]
    if missing:
        sdk_packages = sorted({component for component, _ in missing if ";" in component})
        other_files = [f"{component}: {path}" for component, path in missing if ";" not in component]
        details = ["Android build prerequisites are missing."]
        if sdk_packages:
            details.append("Install or repair SDK Manager packages: " + ", ".join(sdk_packages))
        if other_files:
            details.extend(["Check these existing tool paths:", *(" - " + item for item in other_files)])
        raise ExportError("\n".join(details))
    launchers = list((gradle / "lib").glob("gradle-gradle-cli-main-*.jar"))
    if len(launchers) != 1:
        raise ExportError("Gradle distribution is missing its CLI launcher JAR")
    version = re.fullmatch(r"gradle-gradle-cli-main-(\d+)\.(\d+)\.(\d+)\.jar", launchers[0].name)
    if not version or tuple(map(int, version.groups())) < native_android.ANDROID_MIN_GRADLE_VERSION:
        raise ExportError("Android requires Gradle 9.6.0 or newer")
    environment = {"JAVA_HOME": str(jdk), "ANDROID_HOME": str(sdk), "ANDROID_SDK_ROOT": str(sdk),
                   "ANDROID_USER_HOME": str(directory / "android-user-home"),
                   "GRADLE_USER_HOME": str(directory / "gradle-user-home")}
    command = [java, f"-Duser.home={directory / 'user-home'}", f"-Djava.io.tmpdir={directory / 'tmp'}",
               "-cp", str(launchers[0]), "org.gradle.launcher.GradleMain", "--no-daemon", "--console=plain",
               "--max-workers=2", f"-Dorg.gradle.java.home={jdk}",
               "-Pandroid.builder.sdkDownload=false",
               "-Dorg.gradle.jvmargs=-Xmx2048m -Dfile.encoding=UTF-8 "
               + gradle_jvm_property("user.home", directory / "user-home") + " "
               + gradle_jvm_property("java.io.tmpdir", directory / "tmp"),
               "--project-cache-dir", str(directory / "gradle-project-cache"),
               f"-PlamaponBuildRoot={directory}", f"-PlamaponEngineRoot={engine}", f"-PlamaponProjectRoot={game}",
               f"-PlamaponSdlRoot={args.sdl_source_directory.resolve(strict=True)}", ":app:assembleDebug"]
    if not args.allow_downloads:
        command.append("--offline")
    return command, environment


def check_android_elf(stream, size: int, abi: str, name: str) -> dict:
    """Read ELF headers in place; never extract APK contents into temporary files."""
    def reject(reason):
        raise ExportError(f"APK library {name}: {reason}")

    header = stream.read(64)
    if len(header) != 64:
        reject("truncated ELF header")
    ident, kind, machine, version, entry, program_offset, sections, flags, header_size, \
        program_size, program_count, section_size, section_count, section_names = struct.unpack("<16sHHIQQQIHHHHHH", header)
    if ident[:7] != b"\x7fELF\x02\x01\x01" or kind != 3 or version != 1 \
            or machine != {"arm64-v8a": 183, "x86_64": 62}.get(abi):
        reject("expected a little-endian ELF64 shared library for " + abi)
    if header_size != 64 or program_size != 56 or not 0 < program_count < 65535 \
            or program_offset < 64 or program_offset + program_size * program_count > size:
        reject("invalid ELF program header table")
    load_alignments = []
    relro_count = 0
    for index in range(program_count):
        stream.seek(program_offset + index * program_size)
        record = stream.read(program_size)
        if len(record) != program_size:
            reject("truncated ELF program header")
        segment_type, segment_flags, offset, address, physical, file_size, memory_size, alignment = struct.unpack("<IIQQQQQQ", record)
        if segment_type == 1:  # PT_LOAD: runtime-mapped segment.
            if file_size > memory_size or offset + file_size > size:
                reject("load segment exceeds the library size")
            if alignment < 16384 or alignment & (alignment - 1) or address % alignment != offset % alignment:
                reject("load segment is not compatible with 16KB pages")
            load_alignments.append(alignment)
        elif segment_type == 0x6474E552:  # PT_GNU_RELRO: read-only range after relocations.
            if file_size > memory_size or offset + file_size > size:
                reject("RELRO segment exceeds the library size")
            if (address + memory_size) % 16384:
                reject("RELRO range end is not aligned to 16KB")
            relro_count += 1
    if not load_alignments:
        reject("no loadable ELF segments")
    return {"path": name, "abi": abi, "loadAlignments": load_alignments, "relroSegments": relro_count}


def apk_asset_name(entry: zipfile.ZipInfo, expected_names: set[str]) -> str:
    """Resolve unflagged UTF-8 asset names emitted by Android's APK packager."""
    name = entry.filename
    if entry.flag_bits & 0x800:
        return name
    try:
        utf8_name = name.encode("cp437").decode("utf-8")
    except (UnicodeEncodeError, UnicodeDecodeError):
        return name
    return utf8_name if utf8_name in expected_names else name


def check_apk(artifact: Path, description: dict, asset_sources: dict[str, Path] | None = None) -> list[dict]:
    with zipfile.ZipFile(artifact) as archive:
        entries = archive.infolist()
        names = {entry.filename for entry in entries}
        if len(names) != len(entries):
            raise ExportError("APK contains duplicate file entries")
        required = {"AndroidManifest.xml", "classes.dex", "assets/" + scene_asset(description),
                    "assets/assets/lamapon-default-font.ttf", "assets/assets/lamapon-input-actions.json"}
        required.update("assets/licenses/" + name for name in
                        ("LamaPon.txt", "AndroidNDK.txt", "AndroidNDK-toolchain.txt", "SDL3.txt",
                         "ProggyClean.LICENSE.txt", "imgui.txt", "nlohmann.txt", "cgltf.txt", "stb.txt"))
        expected_libraries = set()
        for abi in description["android"]["abis"]:
            expected_libraries.update(f"lib/{abi}/{name}" for name in
                                      ("libmain.so", "libSDL3.so", "libc++_shared.so"))
            for dependency in description.get("packageDependencies", {}).get("android-" + abi, []):
                for relative in dependency["libraries"] + dependency["runtimeFiles"]:
                    if relative.lower().endswith(".so"):
                        expected_libraries.add(f"lib/{abi}/{Path(relative).name}")
                required.update("assets/licenses/" + item["destination"]
                                for item in dependency["licenseFiles"])
        required.update(expected_libraries)
        if not required <= names:
            raise ExportError("APK is missing Activity, native libraries, font or licenses: " + ", ".join(sorted(required - names)))
        actual_libraries = {entry.filename for entry in entries
                            if entry.filename.startswith("lib/") and not entry.is_dir()}
        if actual_libraries != expected_libraries:
            unexpected = sorted(actual_libraries - expected_libraries)
            raise ExportError("APK contains undeclared native library files: " + ", ".join(unexpected))
        if any(archive.getinfo(name).file_size == 0 for name in required):
            raise ExportError("APK contains an empty required game file")
        if asset_sources is not None:
            expected = {"assets/assets/" + name: source for name, source in asset_sources.items()}
            expected_names = set(expected)
            actual = {}
            for entry in entries:
                if not entry.filename.startswith("assets/assets/") or entry.is_dir():
                    continue
                name = apk_asset_name(entry, expected_names)
                if name in actual:
                    raise ExportError("APK contains duplicate game asset paths: " + name)
                actual[name] = entry
            if actual.keys() != expected.keys():
                missing = sorted(expected.keys() - actual.keys())
                unexpected = sorted(actual.keys() - expected.keys())
                raise ExportError(
                    "APK game assets differ from the current selection"
                    + " (missing: " + ", ".join(missing)
                    + "; unexpected: " + ", ".join(unexpected) + ")")
            for name, source in expected.items():
                with source.open("rb") as original, archive.open(actual[name]) as packaged:
                    if hashlib.file_digest(original, "sha256").digest() != hashlib.file_digest(packaged, "sha256").digest():
                        raise ExportError("APK game asset content is outdated: " + name)
        libraries = []
        for entry in entries:
            if not entry.filename.startswith("lib/") or not entry.filename.endswith(".so"):
                continue
            parts = entry.filename.split("/")
            if len(parts) != 3 or parts[1] not in description["android"]["abis"]:
                raise ExportError("APK library has an unexpected ABI or path: " + entry.filename)
            if entry.compress_type != zipfile.ZIP_STORED:
                raise ExportError("APK native libraries must be uncompressed: " + entry.filename)
            with archive.open(entry) as stream:
                libraries.append(check_android_elf(stream, entry.file_size, parts[1], entry.filename))
        return libraries


def package_runtime_library_names(description: dict, target: str) -> list[str]:
    """Return target-specific package libraries staged beside a desktop game."""
    names = set()
    for dependency in description.get("packageDependencies", {}).get(target, []):
        for relative in dependency["libraries"] + dependency["runtimeFiles"]:
            lower = relative.lower()
            if (target.startswith("windows-") and lower.endswith(".dll")) \
                    or (target.startswith("linux-") and re.search(r"\.so(?:\.[0-9]+)*$", lower)):
                names.add(PurePosixPath(relative).name)
    return sorted(names)


def build(args) -> dict:
    project = args.project_directory.resolve(strict=True)
    description = load_project(project)
    platform = description["platform"]
    if platform != "android" and platform != {"win32": "windows", "linux": "linux"}.get(sys.platform):
        raise ExportError("Desktop output must be built on its target OS; use the generated source settings there")
    engine = (args.engine_root or Path(description["engineRoot"])).resolve(strict=True)
    game = (args.game_root or Path(description["projectRoot"])).resolve(strict=True)
    directory = args.build_directory.resolve()
    if platform == "android":
        command, overrides = android_command(args, description, directory, engine, game)
    else:
        cmake = executable(args.cmake)
        overrides = {}
        command = [cmake, "-S", str(project), "-B", str(directory), f"-DCMAKE_BUILD_TYPE={args.configuration}",
                   f"-DLAMAPON_ENGINE_ROOT={engine}", f"-DLAMAPON_PROJECT_ROOT={game}", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"]
        generator = args.generator or ("Visual Studio 17 2022" if platform == "windows" else None)
        if generator:
            command += ["-G", generator]
            if platform == "windows" and generator.startswith("Visual Studio"):
                command += ["-A", "x64"]
        for option, path in (("LAMAPON_SDL_SOURCE_DIRECTORY", args.sdl_source_directory), ("SDL3_DIR", args.sdl_cmake_package),
                             ("LAMAPON_SDL_LICENSE_FILE", args.sdl_license_file)):
            if path:
                command.append(f"-D{option}={path.resolve(strict=True)}")
    prepare_directory(directory, project, engine, game)
    (directory / "tmp").mkdir(exist_ok=True)
    environment = dict(os.environ, **overrides, TEMP=str(directory / "tmp"), TMP=str(directory / "tmp"), TMPDIR=str(directory / "tmp"))
    result_path = directory / "native-build-result.json"
    result_path.write_text(json.dumps({"built": False, "platform": platform}) + "\n", encoding="utf-8")
    log = directory / "native-build.log"
    run(command, project / "android" if platform == "android" else project, environment, log)
    if platform == "android":
        artifact = directory / "app/outputs/apk/debug/app-debug.apk"
        libraries = check_apk(artifact, description, expected_assets(description, project, engine, game))
        sdk_tools = args.android_sdk.resolve() / "build-tools" / native_android.ANDROID_BUILD_TOOLS_VERSION
        zipalign = executable(str(sdk_tools / ("zipalign.exe" if sys.platform == "win32" else "zipalign")))
        run([zipalign, "-c", "-P", "16", "-v", "4", str(artifact)], project, environment, log)
        java = command[0]
        run([java, "-jar", str(sdk_tools / "lib/apksigner.jar"), "verify", str(artifact)],
            project, environment, log)
    else:
        run([cmake, "--build", str(directory), "--config", args.configuration, "--parallel", "2"], project, environment, log)
        artifact = desktop_artifact(directory, description, args.configuration, project, engine, game)
        if platform == "windows":
            windows_checks = native_windows.check_package(
                artifact, package_runtime_library_names(description, "windows-x86_64"))
            if windows_checks["machine"] != 0x8664:
                raise ExportError("Windows native output must target x86_64")
        elif platform == "linux":
            linux_checks = native_linux.check_package(
                artifact, package_runtime_library_names(description, "linux-x86_64"))
            if linux_checks["machine"] != 62:
                raise ExportError("Linux native output must target x86_64")
    result = {"format": "lamapon.native-build-result", "version": 1, "platform": platform,
              "built": True, "artifactChecksPassed": True, "executed": False,
              "artifactPath": str(artifact), "configuration": "Debug" if platform == "android" else args.configuration,
              "logPath": str(log)}
    if platform == "android":
        result["androidChecks"] = {"libraries": libraries, "zipPageAlignment": 16384,
                                   "signatureVerified": True, "pageSizeRuntimeVerified": False}
    elif platform == "windows":
        result["windowsChecks"] = windows_checks
    elif platform == "linux":
        result["linuxChecks"] = linux_checks
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    configure_cli_output()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-directory", type=Path, required=True)
    parser.add_argument("--build-directory", type=Path, required=True)
    for name in ("engine-root", "game-root", "sdl-source-directory", "sdl-cmake-package", "sdl-license-file",
                 "android-sdk", "java-home", "gradle-home"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator")
    parser.add_argument("--configuration", choices=("Release", "Debug"), default="Release")
    parser.add_argument("--allow-downloads", action="store_true", help="Allow Gradle dependencies; SDK components must already exist")
    args = parser.parse_args()
    try:
        print(json.dumps(build(args), indent=2))
        return 0
    except (OSError, ValueError, KeyError, ExportError, zipfile.BadZipFile) as error:
        print("Native build failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
