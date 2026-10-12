"""Inspect Portable native output, or generate a build project for an existing toolchain.

Inspection is read-only. Generation writes only the requested new output folder;
it does not clone the engine/project, download SDKs, or claim a verified binary.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys

import export_web as portable
import native_android


NATIVE_MODULES = ["core", "input", "renderer2d", "renderer3d", "physics3d", "audio", "particles3d"]
NATIVE_PLATFORMS = ("windows", "linux", "android")
ASSET_EXTENSIONS = {".json", ".glb", ".gltf", ".bin", ".png", ".jpg", ".jpeg", ".bmp", ".ttf", ".wav", ".ogg"}


def exact_relative_path(root: Path, relative: str) -> Path | None:
    """Check directory entries without relying on the host's case sensitivity."""
    path = PurePosixPath(relative.replace("\\", "/"))
    if path.is_absolute() or any(":" in part for part in path.parts):
        return None
    parts = []
    for part in path.parts:
        if part == "..":
            if not parts:
                return None
            parts.pop()
        else:
            parts.append(part)
    current = root
    for part in parts:
        if not current.is_dir() or part not in {entry.name for entry in current.iterdir()}:
            return None
        current = current / part
    return current if current.exists() else None


def json_asset_references(value, field=None):
    if isinstance(value, str):
        normalized = value.replace("\\", "/")
        if normalized.startswith("/assets/"):
            yield normalized[8:]
        elif normalized and field in {"model", "texture", "audio", "clip", "fontAsset",
                                       "albedoTexture", "normalTexture", "roughnessTexture", "metallicTexture",
                                       "occlusionTexture", "emissiveTexture"}:
            yield normalized
        elif normalized.casefold().startswith(("textures/", "audio/", "scenes/", "models/", "fonts/",
                                               "materials/", "animations/", "shaders/")):
            yield normalized
    elif isinstance(value, list):
        for child in value:
            yield from json_asset_references(child)
    elif isinstance(value, dict):
        for key, child in value.items():
            yield from json_asset_references(child, key)


model_references = portable.model_references


def native_package_dependencies(asset_root: Path, platform: str, context: dict) -> tuple[list[dict], dict[str, list[dict]]]:
    """Resolve package-native inputs for the selected desktop target or Android ABIs."""
    packages_root = asset_root / "packages"
    if not packages_root.is_dir():
        return [], {}
    if not portable.is_within(packages_root.resolve(), asset_root.resolve()):
        return [portable.finding("reject", "native-package-path-escape",
            "Package directory resolves outside the project's asset folder.",
            "Keep package folders inside assets/packages before exporting.")], {}

    findings = []
    variants_by_target = {}
    allowed_targets = {"windows-x86_64", "linux-x86_64", "android-arm64-v8a", "android-x86_64"}
    targets = (["android-" + abi for abi in native_android.android_settings(context)["abis"]]
               if platform == "android" else [platform + "-x86_64"])
    try:
        package_directories = sorted((path for path in packages_root.iterdir() if path.is_dir()),
                                     key=lambda path: path.name.casefold())
    except OSError as error:
        return [portable.finding("reject", "native-package-scan-failed",
            f"Cannot inspect native package dependencies: {error}",
            "Check access to the project's assets/packages directory.")], {}

    runtime_names = {target: {} for target in targets}
    license_names = {target: set() for target in targets}

    def add_finding(code, message, action):
        findings.append(portable.finding("reject", code, message, action))

    def safe_package_define(value: str) -> bool:
        if len(value) > 128:
            return False
        name, separator, assigned = value.partition("=")
        return (1 <= len(name) <= 64 and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) is not None
                and (not separator or len(assigned) <= 64
                     and re.fullmatch(r"[A-Za-z0-9_.+-]*", assigned) is not None))

    def parse_variant(raw, package_name, package_directory, target, legacy=False):
        before = len(findings)
        fields = {"includeDirectories", "libraries", "runtimeFiles", "defines"}
        if not legacy:
            fields.update({"licenseFiles", "sources"})
        if not isinstance(raw, dict) or set(raw) - fields:
            add_finding("invalid-native-package-manifest",
                f"Invalid native settings for {package_name} ({target}).",
                "Use includeDirectories, libraries, runtimeFiles and defines; variants may also use licenseFiles.")
            return None
        values = {key: raw.get(key, []) for key in fields}
        if any(not isinstance(items, list) or any(not isinstance(item, str) or not item for item in items)
               for items in values.values()):
            add_finding("invalid-native-package-manifest",
                f"Native package fields must be arrays of nonempty strings: {package_name} ({target}).",
                "Repair the package manifest and use relative paths inside the package directory.")
            return None
        if any(not safe_package_define(define) for define in values["defines"]):
            add_finding("invalid-native-package-manifest",
                f"Native package defines contain an invalid value: {package_name} ({target}).",
                "Use simple macro definitions such as FEATURE_ENABLED or LEVEL=2.")
            return None
        entry = {"package": package_name, "variant": target, "includeDirectories": [],
                 "sources": [], "libraries": [], "runtimeFiles": [], "defines": list(values["defines"]),
                 "licenseFiles": []}
        for field in ("includeDirectories", "sources", "libraries", "runtimeFiles", "licenseFiles"):
            for relative in values.get(field, []):
                relative_path = PurePosixPath(relative)
                if relative_path.is_absolute() or "\\" in relative or ":" in relative \
                        or any(part in {"", ".", ".."} for part in relative_path.parts) or relative.endswith("/") \
                        or any(character in relative for character in (";", "\x00", "\n", "\r")) \
                        or "$<" in relative:
                    add_finding("invalid-native-package-path",
                        f"Native package paths must be normalized relative paths: {package_name}/{relative}.",
                        "Use forward-slash paths without '.', '..', drive letters or absolute prefixes.")
                    continue
                path = exact_relative_path(package_directory, relative)
                if path is None or not portable.is_within(path.resolve(), package_directory.resolve()):
                    add_finding("invalid-native-package-path",
                        f"Native package path is missing, case-mismatched or outside its package: "
                        f"assets/packages/{package_name}/{relative} ({target}).",
                        "Use existing, correctly cased relative paths contained in the package directory.")
                    continue
                is_directory = field == "includeDirectories"
                if path.is_dir() != is_directory or (not is_directory and not path.is_file()):
                    add_finding("invalid-native-package-path",
                        f"Native package {field} entry has the wrong file type: {package_name}/{relative}.",
                        "Use directories for includeDirectories and files for the other native package fields.")
                    continue
                suffix = path.name.lower()
                valid_library = (suffix.endswith(".lib") if target.startswith("windows-") else
                                 suffix.endswith(".a") or (suffix.endswith(".so") if target.startswith("android-")
                                 else re.search(r"\.so(?:\.\d+)*$", suffix) is not None))
                if field == "libraries" and not valid_library:
                    add_finding("invalid-native-package-library",
                        f"Library format does not match {target}: assets/packages/{package_name}/{relative}.",
                        "Use .lib for Windows and .a or .so for Linux/Android.")
                    continue
                if field == "runtimeFiles" and not (
                        suffix.endswith(".dll") if target.startswith("windows-") else
                        suffix.endswith(".so") if target.startswith("android-") else
                        re.search(r"\.so(?:\.\d+)*$", suffix)):
                    add_finding("invalid-native-package-runtime",
                        f"Runtime library format does not match {target}: assets/packages/{package_name}/{relative}.",
                        "Use .dll for Windows and .so for Linux/Android runtime libraries.")
                    continue
                if field == "sources" and path.suffix.lower() not in {".c", ".cc", ".cpp", ".cxx"}:
                    add_finding("invalid-native-package-source",
                        f"Package source must be a C/C++ translation unit: assets/packages/{package_name}/{relative}.",
                        "Use .c, .cc, .cpp or .cxx package implementation files.")
                    continue
                if field in {"runtimeFiles", "libraries"}:
                    reserved_runtime = (suffix in {"libmain.so", "libsdl3.so", "libc++_shared.so"}
                                        if target.startswith("android-") else
                                        suffix == "sdl3.dll" if target.startswith("windows-") else
                                        suffix.startswith("libsdl3.so"))
                    if reserved_runtime:
                        add_finding("reserved-native-package-runtime",
                            f"Package library conflicts with the {target} runtime: {package_name}/{relative}.",
                            "Rename or remove package libraries that replace SDL or an Android runtime library.")
                        continue
                project_relative = path.relative_to(context["root"]).as_posix()
                if field == "licenseFiles":
                    destination = f"packages/{package_name}/{relative}"
                    folded = destination.casefold()
                    if folded in license_names[target]:
                        add_finding("duplicate-native-package-license",
                            f"Package license destination collides in {target}: {destination}.",
                            "Use unique package directories and license filenames.")
                        continue
                    license_names[target].add(folded)
                    entry[field].append({"source": project_relative, "destination": destination})
                else:
                    entry[field].append(project_relative)
                if field in {"runtimeFiles", "libraries"} and (
                        suffix.endswith(".dll") or re.search(r"\.so(?:\.\d+)*$", suffix)):
                    folded = path.name.casefold()
                    prior = runtime_names[target].get(folded)
                    if prior is not None and prior != str(path.resolve()):
                        add_finding("duplicate-native-package-runtime",
                            f"Native package runtime filename collides in {target}: {path.name}.",
                            "Rename one runtime library so every packaged library has a unique filename.")
                    runtime_names[target][folded] = str(path.resolve())
        return None if len(findings) != before else entry

    for package_directory in package_directories:
        package_name = package_directory.name
        manifest_path = package_directory / "package.json"
        if not manifest_path.is_file():
            continue
        resolved_package_directory = package_directory.resolve()
        if (not portable.is_within(resolved_package_directory, packages_root.resolve())
                or not portable.is_within(manifest_path.resolve(), resolved_package_directory)):
            add_finding("native-package-path-escape", f"Package manifest escapes assets/packages: {package_name}",
                        "Keep the package manifest inside its own package directory.")
            continue
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
        except (OSError, UnicodeError, ValueError) as error:
            add_finding("invalid-native-package-manifest", f"Cannot read {package_name}/package.json: {error}",
                        "Repair the package.json file or remove the package from this project.")
            continue
        if not isinstance(manifest, dict):
            add_finding("invalid-native-package-manifest", f"Package manifest must be a JSON object: {package_name}.",
                        "Repair the package.json file or remove the package from this project.")
            continue
        native = manifest.get("native", {})
        native_variants = manifest.get("nativeVariants", {})
        if not isinstance(native, dict):
            add_finding("invalid-native-package-manifest", f"Package native settings must be an object: {package_name}.",
                        "Repair the package.json file or remove the package from this project.")
            continue
        if not isinstance(native_variants, dict) or set(native_variants) - allowed_targets:
            add_finding("invalid-native-package-manifest", f"Package nativeVariants contains unsupported target names: {package_name}.",
                        "Supported targets: windows-x86_64, linux-x86_64, android-arm64-v8a, android-x86_64.")
            continue
        selected = {}
        if (native or native_variants) and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", package_name):
            add_finding("invalid-native-package-manifest",
                f"Native package folder name is unsafe for generated output paths: {package_name}.",
                "Use a package folder name containing only letters, digits, dots, underscores and hyphens.")
            continue
        for target in targets:
            raw = native_variants.get(target)
            legacy = False
            if raw is None and target == "windows-x86_64" and native:
                raw, legacy = native, True
            if raw is not None:
                parsed = parse_variant(raw, package_name, package_directory, target, legacy)
                if parsed is not None:
                    selected[target] = parsed
            elif native or native_variants:
                add_finding("unresolved-native-package-dependency",
                    f"Native package {package_name} has no dependency variant for {target}.",
                    f"Add nativeVariants.{target} or disable the package for {platform}.")
        for target, entry in selected.items():
            variants_by_target.setdefault(target, []).append(entry)
    return findings, variants_by_target


def android_package_license_findings(package_dependencies: dict[str, list[dict]]) -> list[dict]:
    """Reject per-ABI licenses that collide when Gradle merges the APK assets."""
    destinations = {}
    findings = []
    for target, entries in package_dependencies.items():
        if not target.startswith("android-"):
            continue
        for entry in entries:
            for license_file in entry["licenseFiles"]:
                destination = license_file["destination"].casefold()
                source = license_file["source"]
                prior = destinations.get(destination)
                if prior is not None and prior != source:
                    findings.append(portable.finding(
                        "reject", "duplicate-native-package-license",
                        f"Android package licenses from multiple ABIs collide at {license_file['destination']}.",
                        "Use the same license file for each ABI, or give distinct licenses unique package paths.",
                    ))
                else:
                    destinations[destination] = source
    return findings


def inspect_project(project_file: Path, platform: str) -> tuple[dict, dict]:
    if platform not in NATIVE_PLATFORMS:
        raise portable.ExportError(
            "Native output supports only: " + ", ".join(NATIVE_PLATFORMS)
        )
    project_file = project_file.resolve(strict=True)
    project = json.loads(project_file.read_text(encoding="utf-8-sig"))
    if not isinstance(project, dict) or not portable.is_lamapon_project(project_file, project):
        raise portable.ExportError("Native output requires a LamaPonProject document")
    root = portable.lamapon_project_root(project_file, project)
    exports = project.get("export", {})
    if not isinstance(exports, dict) or not isinstance(exports.get("native", {}), dict):
        raise portable.ExportError("export.native must be an object")
    normalized = copy.deepcopy(project)
    normalized["export"] = {"web": copy.deepcopy(exports.get("native", {}))}
    settings = portable.prepare_normal_lamapon_web_configuration(root, normalized)
    settings["portableGame"] = True
    sources = [portable.require_relative_file(root, value, "export.native.sources")
               for value in settings["sources"]]
    if any(path.suffix.lower() not in {".c", ".cc", ".cpp", ".cxx"} for path in sources):
        raise portable.ExportError("Native sources must be C/C++ translation units")
    roots = portable.web_asset_roots(root, settings)
    if roots is None:
        raise portable.ExportError("Native asset directory does not exist")
    asset_root, included = roots
    if not portable.is_within(asset_root, root):
        raise portable.ExportError("Native assets must stay inside the project")
    findings = portable.validate_portable_contract(root, normalized, "lamapon-project", settings, NATIVE_MODULES)
    # The shared validator describes its original Web target. Native diagnostics
    # must identify the output the user actually selected.
    native_wording = (
        ("メインCanvas", "メイン表示領域"),
        ("ポータブルWeb", "Portable"),
        ("Web出力", "ネイティブ出力"),
        ("WebGL", "OpenGL／OpenGL ES"),
        ("Web Audio", "Portable音声バックエンド"),
        ("基本Web", "基本Portable"),
        ("Webプロファイル", "Portableプロファイル"),
        ("Web版", "Portable版"),
        ("Web未対応", "Portableランタイム未対応"),
        ("Webでは", "Portableでは"),
        ("Web入力", "Portable入力"),
        ("ブラウザー用", "Portable用"),
        ("ブラウザーバックエンド", "Portable入力バックエンド"),
        ("ブラウザー間", "OS間"),
        ("ブラウザー", "Portable実行環境"),
        ("export.web.", "export.native."),
    )
    for item in findings:
        for key in ("message", "action"):
            for old, new in native_wording:
                item[key] = item[key].replace(old, new)
        if item["code"] == "web-spatial-audio-approximation":
            item["code"] = "native-spatial-audio-approximation"
            item["message"] = (
                "ネイティブ出力の3D音声は距離減衰とステレオ定位で近似し、"
                "Web AudioのHRTF定位は使用しません。"
            )
            item["action"] = (
                "対象OSの実スピーカーまたはヘッドホンで定位と減衰を確認してください。"
            )
        if item["code"] == "portable-contract-complete":
            item["message"] = item["message"].replace("Web互換性チェック", "Portable互換性チェック")
            item["action"] = (
                "Android SDK・NDKを使ってAPKをビルドし、Android端末で動作確認してください。"
                if platform == "android" else
                f"対象OS（{platform}）でCMakeによるゲームのビルドと動作確認を行ってください。"
            )
    try:
        package_findings, package_dependencies = native_package_dependencies(
            asset_root, platform, {"project": project, "root": root})
    except portable.ExportError as error:
        package_findings, package_dependencies = [portable.finding(
            "reject", "invalid-android-settings", str(error),
            "Correct export.native.android before resolving package ABI variants.")], {}
    findings.extend(package_findings)
    if platform == "android":
        findings.extend(android_package_license_findings(package_dependencies))
    case_sensitive = platform in {"linux", "android"}
    references = set()
    if case_sensitive:
        for relative in settings["sources"]:
            if exact_relative_path(root, relative) is None:
                findings.append(portable.finding("reject", "case-sensitive-source-path",
                    f"Game source path does not match the actual filename: {relative}",
                    "Match every directory and filename's uppercase and lowercase characters."))
        startup = settings.get("scenePath", "")
        if isinstance(startup, str) and startup.startswith("/assets/"):
            references.add((startup[8:], "startup scene"))
    actions = portable.portable_project_input_actions(root)
    for name, bindings in actions.items():
        if any(binding.get("control") not in portable.PORTABLE_INPUT_CONTROLS
               or not math.isfinite(binding.get("scale", 1.0)) for binding in bindings):
            findings.append(portable.finding(
                "reject", "unsupported-native-input-binding", f"Unsupported input binding in action {name}",
                "Use a supported Portable control and a finite scale.",
            ))
    for path in sorted(asset_root.rglob("*")):
        if not path.is_file() or not portable.asset_is_selected(path, included):
            continue
        if not portable.is_within(path.resolve(), asset_root):
            raise portable.ExportError("Native asset link escapes the asset directory")
        first = path.relative_to(asset_root).parts[0]
        reserved = {"lamapon-default-font.ttf", "lamapon-input-actions.json"}
        if (first.casefold() if platform == "windows" else first) in reserved:
            findings.append(portable.finding(
                "reject", "reserved-native-asset", f"Asset conflicts with an engine-generated file: {path.relative_to(root)}",
                "Rename this asset or exclude it from export.native.assetIncludePaths. "
                "Custom fonts can use another filename and be referenced with fontAsset.",
            ))
            continue
        if case_sensitive and path.suffix.lower() == ".json":
            try:
                document = json.loads(path.read_text(encoding="utf-8-sig"))
                references.update((reference, str(path.relative_to(root)))
                                  for reference in json_asset_references(document))
            except (UnicodeError, ValueError):
                pass  # Scene/material syntax diagnostics belong to the shared validator.
        if path.suffix.lower() not in ASSET_EXTENSIONS:
            findings.append(portable.finding(
                "reject", "unsupported-native-asset", f"Native asset format is unsupported: {path.relative_to(root)}",
                "Use PNG/JPEG/BMP, TTF, WAV/Ogg Vorbis, glTF/GLB or Portable JSON; no automatic native conversion is available.",
            ))
        elif path.suffix.lower() in {".glb", ".gltf"}:
            try:
                document = (portable.read_portable_glb_document(path) if path.suffix.lower() == ".glb"
                            else json.loads(path.read_text(encoding="utf-8")))
                portable.validate_portable_model_document(document, path)
                for reference in model_references(document):
                    model_relative = (path.parent.relative_to(asset_root) / reference).as_posix()
                    if case_sensitive and exact_relative_path(asset_root, model_relative) is None:
                        raise portable.ExportError(f"Native model reference has missing or mismatched filename case: {reference}")
                    referenced = (path.parent / reference).resolve()
                    if not portable.is_within(referenced, asset_root) or not referenced.is_file() \
                            or not portable.asset_is_selected(referenced, included):
                        raise portable.ExportError(f"Native model reference is missing or not packaged: {reference}")
            except (ValueError, portable.ExportError) as error:
                findings.append(portable.finding("reject", "unsupported-native-model", str(error),
                                                 "Prepare a Portable model with supported PNG/JPEG images and valid references."))
    game_source_paths = set(portable.selected_web_source_files(root, settings))
    package_source_paths = {root / source for entries in package_dependencies.values()
                            for entry in entries for source in entry["sources"]}
    source_paths = sorted(game_source_paths | package_source_paths)
    for path in source_paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        if case_sensitive:
            token = re.compile(portable.ASSET_PATH_TOKEN.pattern, portable.ASSET_PATH_TOKEN.flags | re.IGNORECASE)
            references.update((reference, str(path.relative_to(root))) for reference in token.findall(text))
        if path in game_source_paths and any(token in text for token in (
                "<windows.h>", "<Windows.h>", "<emscripten", "<d3d11", "<d3d12")):
            findings.append(portable.finding(
                "reject", "platform-specific-game-source", f"Platform-specific dependency in {path.relative_to(root)}",
                "Keep game scripts on the Portable LamaPon API or provide an explicit native implementation.",
            ))
    for reference, location in sorted(references):
        target = exact_relative_path(asset_root, reference)
        if target is None or not target.is_file() or not portable.asset_is_selected(target.resolve(), included):
            findings.append(portable.finding("reject", "case-sensitive-asset-reference",
                f"{location}: asset is missing, not packaged, or filename case differs: {reference}",
                "Match every directory and filename exactly and include the referenced asset in the output."))
    if platform == "android":
        try:
            native_android.android_settings({"project": project, "root": root})
        except portable.ExportError as error:
            findings.append(portable.finding("reject", "invalid-android-settings", str(error),
                                             "Correct export.native.android before generating the Android project."))
    report = {
        "format": "lamapon.native-output-inspection", "version": 1,
        "platform": platform, "runtime": "portable-sdl", "verified": False,
        "findings": findings, "canGenerateBuildProject": not any(f["level"] == "reject" for f in findings),
        "limitations": ["This project's native build and execution have not been verified by inspection.",
                        "Android Gradle project generation is available; APK build and device execution are not verified.",
                        "Steam Deck requires Linux build and device verification.",
                        "Portable API restrictions remain; Windows runtime feature parity is incomplete."],
    }
    context = {"root": root, "project": project, "settings": settings, "sources": sources,
               "asset_root": asset_root, "included": included, "actions": actions,
               "package_dependencies": package_dependencies}
    return report, context


def cmake_literal(value: str | Path) -> str:
    text = value.as_posix() if isinstance(value, Path) else value
    if ";" in text or "$<" in text or "\x00" in text or "\n" in text or "\r" in text:
        raise portable.ExportError("Native build settings cannot contain semicolons, generator expressions or control characters")
    delimiter = "="
    while "]" + delimiter + "]" in text:
        delimiter += "="
    return "[" + delimiter + "[" + text + "]" + delimiter + "]"


def build_project_description(platform: str, context: dict) -> dict:
    package_dependencies = {}
    for target, entries in context.get("package_dependencies", {}).items():
        package_dependencies[target] = [{key: value for key, value in entry.items()}
                                        for entry in entries]
    description = {"format": "lamapon.native-build-project", "version": 2, "platform": platform,
                   "target": portable.safe_cmake_target("LamaPonNative_" + context["root"].name),
                   "engineRoot": str(portable.ENGINE_ROOT), "projectRoot": str(context["root"]),
                   "scenePath": context["settings"]["scenePath"],
                   "assetDirectory": context["asset_root"].relative_to(context["root"]).as_posix(),
                   "assetIncludePaths": [path.relative_to(context["asset_root"]).as_posix()
                                         for path in context["included"]],
                   "packageDependencies": package_dependencies}
    if platform == "android":
        description["android"] = native_android.android_settings(context)
    return description


def generate_build_project(output: Path, platform: str, report: dict, context: dict) -> None:
    if report.get("platform") != platform:
        raise portable.ExportError("Build target must match the inspected platform")
    if not report["canGenerateBuildProject"]:
        raise portable.ExportError("Native compatibility findings must be resolved before generation")
    output = output.resolve()
    root = context["root"]
    if portable.is_within(output, context["asset_root"]) or output == root:
        raise portable.ExportError("Build project output must be separate from project assets and project root")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise portable.ExportError("Build project output must be a new or empty directory")
    target = portable.safe_cmake_target("LamaPonNative_" + root.name)
    settings = context["settings"]
    name = context["project"].get("gameName", context["project"].get("name", root.name))
    if not isinstance(name, str) or not name:
        raise portable.ExportError("Native game name must be a nonempty string")
    lines = ["cmake_minimum_required(VERSION 3.25)", f"project({target} LANGUAGES C CXX)",
             f"set(LAMAPON_ENGINE_ROOT {cmake_literal(portable.ENGINE_ROOT)} CACHE PATH \"Existing LamaPon engine source\")",
             f"set(LAMAPON_PROJECT_ROOT {cmake_literal(root)} CACHE PATH \"Existing game project\")",
             'set(LAMAPON_SDL_SOURCE_DIRECTORY "" CACHE PATH "Existing SDL 3.4+ source directory")',
             'set(LAMAPON_SDL_LICENSE_FILE "" CACHE FILEPATH "SDL license for redistribution")',
             f'if(NOT CMAKE_SYSTEM_NAME STREQUAL "{dict(windows="Windows", linux="Linux", android="Android")[platform]}")',
             f'  message(FATAL_ERROR "This build project targets {platform}; choose the matching toolchain")', "endif()",
             'include("${LAMAPON_ENGINE_ROOT}/cmake/LamaPonNative.cmake")']
    if platform == "linux":
        lines.extend(['if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")',
                      '  message(FATAL_ERROR "This Linux output targets x86_64")', "endif()"])
    elif platform == "windows":
        lines.extend(["if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)",
                      '  message(FATAL_ERROR "This Windows output targets x86_64")', "endif()"])
    lines.append("set(game_sources")
    lines.extend("  " + cmake_literal(path.relative_to(root)) for path in context["sources"])
    lines += [")", 'list(TRANSFORM game_sources PREPEND "${LAMAPON_PROJECT_ROOT}/")',
              "set(asset_relative " + cmake_literal(context["asset_root"].relative_to(root)) + ")",
              "set(package_include_directories)", "set(package_sources)", "set(package_libraries)", "set(package_defines)",
              "set(package_runtime_files)", "set(package_license_files)"]
    package_dependencies = context.get("package_dependencies", {})
    package_path_index = 0

    def append_package_path(relative: str, variable: str, indent: str = "", destination: str | None = None):
        nonlocal package_path_index
        path_variable = f"_lamapon_package_path_{package_path_index}"
        package_path_index += 1
        lines.append(indent + f"set({path_variable} {cmake_literal(relative)})")
        line = indent + f'list(APPEND {variable} "${{LAMAPON_PROJECT_ROOT}}/${{{path_variable}}}"'
        if destination is not None:
            line += " " + cmake_literal(destination)
        lines.append(line + ")")

    if platform == "android":
        android = native_android.android_settings(context)
        lines += ["set(package_variant_found FALSE)"]
        for abi in android["abis"]:
            target_variant = "android-" + abi
            entries = package_dependencies.get(target_variant, [])
            lines += [f'if(CMAKE_ANDROID_ARCH_ABI STREQUAL {cmake_literal(abi)})',
                      "  set(package_variant_found TRUE)"]
            for entry in entries:
                for field, variable in (("includeDirectories", "package_include_directories"),
                                        ("sources", "package_sources"),
                                        ("libraries", "package_libraries"), ("defines", "package_defines"),
                                        ("runtimeFiles", "package_runtime_files")):
                    for value in entry[field]:
                        if field == "defines":
                            lines.append(f"  list(APPEND {variable} {cmake_literal(value)})")
                        else:
                            append_package_path(value, variable, "  ")
                for license_file in entry["licenseFiles"]:
                    append_package_path(license_file["source"], "package_license_files", "  ",
                                        license_file["destination"])
            lines.append("endif()")
        lines += ["if(NOT package_variant_found)",
                  '  message(FATAL_ERROR "Unsupported Android ABI for this package configuration")', "endif()"]
    else:
        for entry in package_dependencies.get(platform + "-x86_64", []):
            for field, variable in (("includeDirectories", "package_include_directories"),
                                    ("sources", "package_sources"),
                                    ("libraries", "package_libraries"), ("defines", "package_defines"),
                                    ("runtimeFiles", "package_runtime_files")):
                for value in entry[field]:
                    if field == "defines":
                        lines.append(f"list(APPEND {variable} {cmake_literal(value)})")
                    else:
                        append_package_path(value, variable)
            for license_file in entry["licenseFiles"]:
                append_package_path(license_file["source"], "package_license_files", "",
                                    license_file["destination"])
    lines += [f"lamapon_add_native_game({target}", "  SOURCES ${game_sources}",
              '  ASSET_DIRECTORY "${LAMAPON_PROJECT_ROOT}/${asset_relative}"', "  ASSET_INCLUDE_PATHS"]
    lines.extend("    " + cmake_literal(path.relative_to(context["asset_root"])) for path in context["included"])
    lines += ["  GAME_NAME " + cmake_literal(name), "  SCENE_PATH " + cmake_literal(settings["scenePath"]),
              '  SDL_SOURCE_DIRECTORY "${LAMAPON_SDL_SOURCE_DIRECTORY}"',
              '  SDL_LICENSE_FILE "${LAMAPON_SDL_LICENSE_FILE}"',
              '  INPUT_ACTIONS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/lamapon-input-actions.json"',
              "  PACKAGE_INCLUDE_DIRECTORIES ${package_include_directories}",
              "  PACKAGE_SOURCES ${package_sources}",
              "  PACKAGE_LIBRARIES ${package_libraries}", "  PACKAGE_DEFINES ${package_defines}",
              "  PACKAGE_RUNTIME_FILES ${package_runtime_files}",
              "  PACKAGE_LICENSE_FILES ${package_license_files}",
              '  ANDROID_PACKAGE_LIBS_DIRECTORY "${LAMAPON_ANDROID_PACKAGE_LIBS_DIRECTORY}"', ")", ""]
    actions = context["actions"]
    android_files = native_android.project_files(context, portable.ENGINE_ROOT, target, name) if platform == "android" else {}
    output.mkdir(parents=True, exist_ok=True)
    (output / "CMakeLists.txt").write_text("\n".join(lines), encoding="utf-8")
    (output / "lamapon-input-actions.json").write_text(json.dumps(
        {"format": "lamapon.web-input-actions", "version": 1, "actions": actions}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "native-inspection.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "native-build-project.json").write_text(json.dumps(
        build_project_description(platform, context), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "README.txt").write_text(
        "This is an unverified build project, not a game binary or standalone source bundle.\n"
        "Build with existing tools and an authorized output directory:\n"
        "python -B <engine>/tools/build_native.py --project-directory . --build-directory <build> "
        "--sdl-source-directory <existing SDL sources>\n"
        "Android also requires --android-sdk, --java-home and --gradle-home; Gradle dependencies are offline "
        "unless --allow-downloads is explicitly supplied. No SDK is installed by this command.\n"
        "native-build-result.json records artifact checks separately from execution. Test the game on its target OS.\n"
        "Windows output targets Windows 10 or later. MSVC release runtime DLLs and shared SDL DLLs "
        "are staged beside the executable. PE import checks require non-system dependencies in that directory. "
        "MSVC Debug runtimes are not supported for distributable output; use Release. "
        "See licenses/WindowsRuntime.txt for deployment information.\n"
        "Portable package dependencies are selected from package.json nativeVariants by target: Windows x86_64, "
        "Linux x86_64, or each configured Android ABI. Shared package libraries and declared runtime files are "
        "staged with their game; package licenseFiles are included under licenses/packages.\n"
        "Linux output uses $ORIGIN to find bundled shared SDL by its SONAME. ELF dependency checks "
        "reject missing non-system libraries, CPU mismatches and build-machine search paths. "
        "glibc/libstdc++ version compatibility and Steam Deck execution still need separate verification.\n"
        "Keep the existing engine and game sources available. On another OS, remap their locations:\n"
        "cmake -S . -B <build> -DLAMAPON_ENGINE_ROOT=<engine> -DLAMAPON_PROJECT_ROOT=<game> "
        "-DLAMAPON_SDL_SOURCE_DIRECTORY=<existing SDL sources>\n"
        "cmake --build <build> --config Release --parallel 2\n"
        "Android additionally requires the NDK CMake toolchain, ABI and API; see android/README.txt for Gradle APK configuration.\n",
        encoding="utf-8",
    )
    for relative, text in android_files.items():
        path = output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


def main() -> int:
    portable.configure_cli_output()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--platform", choices=NATIVE_PLATFORMS, required=True)
    parser.add_argument("--generate-build-project", type=Path,
                        help="Create build metadata in this new/empty directory; omitted means read-only inspection")
    args = parser.parse_args()
    try:
        report, context = inspect_project(args.project, args.platform)
        if args.generate_build_project:
            generate_build_project(args.generate_build_project, args.platform, report, context)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0 if report["canGenerateBuildProject"] else 2
    except (OSError, ValueError, portable.ExportError) as error:
        print(f"Native output failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
