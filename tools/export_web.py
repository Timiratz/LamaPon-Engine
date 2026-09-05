#!/usr/bin/env python3
"""EmscriptenでLamaPonのWebターゲットをビルドし、Webパッケージを収集します。"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from datetime import UTC, datetime
from pathlib import Path
from typing import Any


ENGINE_ROOT = Path(__file__).resolve().parent.parent


class ExportError(RuntimeError):
    """利用者が対処できるWeb出力設定またはビルドエラーです。"""


WEB_PROFILES: dict[str, dict[str, Any]] = {
    "webgl2-basic-2d": {
        "modules": {
            "core",
            "renderer2d",
            "input",
            "audio",
        },
        "module_reasons": {
            "renderer3d": "Windows/D3D11用の3Dレンダラーは、この2Dプロファイルに含まれません。",
            "physics3d": "標準の物理APIはこの2Dプロファイルに含まれません。ポータブルWeb物理ランタイムは3Dプロファイルで使用できます。",
            "particles3d": "現在のパーティクルレンダラーはD3D11専用です。",
            "postprocess": "現在のポストプロセス処理はD3D11/HLSL専用です。",
            "compute": "このプロファイルはWebGL2のコンピュートシェーダーに対応していません。",
            "custom-hlsl": "WebGL2のシェーダー経路ではHLSLを使用できません。",
            "native-plugin": "ブラウザーではDLL/EXEプラグインを読み込めません。",
            "filesystem-native": "ブラウザー出力では仮想アセットファイルシステムを使用する必要があります。",
            "threads": "この基本プロファイルではpthreadsとSharedArrayBufferを有効にしていません。",
        },
        "asset_extensions": {
            ".json",
            ".jpeg",
            ".jpg",
            ".png",
            ".webp",
            ".wav",
            ".otf",
            ".ttf",
            ".woff",
            ".woff2",
        },
    },
    "webgl2-basic-3d": {
        "modules": {
            "core",
            "renderer2d",
            "renderer3d",
            "input",
            "physics3d",
            "audio",
            "particles3d",
        },
        "module_reasons": {
            "postprocess": "現在のWebポストプロセス処理は、基本3Dランタイムに含まれません。",
            "compute": "このプロファイルはWebGL2のコンピュートシェーダーに対応していません。",
            "custom-hlsl": "WebGL2のシェーダー経路ではHLSLを使用できません。",
            "native-plugin": "ブラウザーではDLL/EXEプラグインを読み込めません。",
            "filesystem-native": "ブラウザー出力では仮想アセットファイルシステムを使用する必要があります。",
            "threads": "この基本プロファイルではpthreadsとSharedArrayBufferを有効にしていません。",
        },
        "asset_extensions": {
            ".json",
            ".jpeg",
            ".jpg",
            ".png",
            ".webp",
            ".wav",
            ".txt",
            ".otf",
            ".ttf",
            ".woff",
            ".woff2",
        },
    },
}

DEFAULT_MODULES = ["core", "renderer2d", "input"]
DEFAULT_PLATFORM_SOURCE_NAMES = {
    "winmain.cpp",
    "windowsmain.cpp",
    "win32main.cpp",
    "windowsplatform.cpp",
    "win32platform.cpp",
}

SOURCE_EXTENSIONS = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
}

FORBIDDEN_SOURCE_TOKENS = {
    "#include <Windows.h>": "Win32ヘッダー",
    '#include "Windows.h"': "Win32ヘッダー",
    "#include <d3d11.h>": "Direct3D 11ヘッダー",
    "#include <dxgi.h>": "DXGIヘッダー",
    "#include <DirectXMath.h>": "DirectXMathヘッダー",
    "#include <xaudio2.h>": "XAudio2ヘッダー",
    "#include <WICTextureLoader.h>": "DirectXTKのテクスチャ読み込み",
    "ID3D11": "Direct3D 11のオブジェクト型",
    "D3D11_": "Direct3D 11の定数",
    "DirectX::": "DirectXMathまたはDirectXTKの型",
    "HWND": "Win32ウィンドウハンドル",
    "XAudio": "XAudio2 API",
}

PORTABLE_DIRECTX_TOKEN = re.compile(
    r"DirectX::(?:XMFLOAT2|XMFLOAT3|XMFLOAT4|XMFLOAT4X4|XMMATRIX|"
    r"XMStoreFloat4x4|XM_PI)"
)

SOURCE_WARNINGS = {
    "std::filesystem::exists": (
        "ファイルの存在確認では、パッケージ内のブラウザー用仮想"
        "ファイルシステムだけが見えます"
    ),
    "std::filesystem::directory_iterator": (
        "フォルダー走査では、パッケージ内のブラウザー用仮想ファイルシステムだけが見えます"
    ),
    "std::filesystem::recursive_directory_iterator": (
        "フォルダー走査では、パッケージ内のブラウザー用仮想ファイルシステムだけが見えます"
    ),
    "std::thread": (
        "スレッドにはSharedArrayBuffer/pthreadsを有効にした配布設定が必要です。"
        "基本プロファイルでは使用できません"
    ),
    "std::async": (
        "基本プロファイルでは、非同期処理がブラウザースレッドで実行される保証はありません"
    ),
}

PORTABLE_API_MODULES: dict[str, str] = {
    "AudioBus": "audio",
    "AudioSourceComponent": "audio",
    "BoxCollider3DComponent": "physics3d",
    "CameraComponent": "renderer3d",
    "CollisionEvent": "physics3d",
    "Component": "core",
    "GameObject": "core",
    "GraphicsDevice": "core",
    "MeshCollider3DComponent": "physics3d",
    "MeshRendererComponent": "renderer3d",
    "ModelRendererComponent": "renderer3d",
    "NativeScriptComponent": "core",
    "ParticleEmitterShape": "particles3d",
    "ParticleRenderMode": "particles3d",
    "ParticleSystemComponent": "particles3d",
    "InputPointerButtonState": "input",
    "InputPointerState": "input",
    "InputMoverComponent": "input",
    "InputSystem": "input",
    "Logger": "core",
    "PhysicsHit": "physics3d",
    "PhysicsQueryFilter": "physics3d",
    "PrimitiveShape": "renderer3d",
    "ProceduralMeshVertex": "renderer3d",
    "PointerButton": "input",
    "Ray": "physics3d",
    "RenderCullingComponent": "renderer3d",
    "RigidbodyComponent": "physics3d",
    "RotatorComponent": "core",
    "Scene": "core",
    "Script": "core",
    "ShaderCullMode": "renderer3d",
    "SpriteMaskComponent": "renderer2d",
    "SpriteMaskInteraction": "renderer2d",
    "SpriteMaskShape": "renderer2d",
    "SpriteAnimationClip": "renderer2d",
    "SpriteAnimatorComponent": "renderer2d",
    "SpriteRendererComponent": "renderer2d",
    "TextHorizontalAlignment": "renderer2d",
    "TextRendererComponent": "renderer2d",
    "TextVerticalAlignment": "renderer2d",
    "UIRectTransformComponent": "renderer2d",
    "TransformAnimatorComponent": "core",
    "ParallaxLayerComponent": "renderer2d",
}

PORTABLE_SCENE_COMPONENTS: dict[str, str] = {
    "NativeScript": "core",
    "Camera": "renderer3d",
    # これらのシーンコンポーネントには、ポータブル版で独立したランタイム処理が
    # ありません。ライティングにはシーン環境の既定値を使い、ブラウザーの
    # AudioContextではAudioListenerオブジェクトを必要としません。
    "AudioListener": "audio",
    "DirectionalLight": "renderer3d",
    "InputMover": "input",
    "AudioSource": "audio",
    "BoxCollider3D": "physics3d",
    "MeshCollider3D": "physics3d",
    "MeshRenderer": "renderer3d",
    "ModelRenderer": "renderer3d",
    "ParticleSystem": "particles3d",
    "Rigidbody": "physics3d",
    "Rotator": "core",
    "RenderCulling": "renderer3d",
    "SpriteAnimator": "renderer2d",
    "SpriteMask": "renderer2d",
    "SpriteRenderer": "renderer2d",
    "TextRenderer": "renderer2d",
    "TransformAnimator": "core",
    "UIRectTransform": "renderer2d",
    "ParallaxLayer": "renderer2d",
}

PORTABLE_NOOP_SCENE_COMPONENTS = {"AudioListener", "RenderCulling"}
PORTABLE_APPROXIMATE_SCENE_COMPONENTS = {"DirectionalLight"}

# ネイティブシーンコンポーネントの全種類を明示します。新しいコンポーネントは
# Web互換を宣言する前に、ここおよびPORTABLE_SCENE_COMPONENTSで分類します。
KNOWN_NATIVE_SCENE_COMPONENTS = {
    "AudioListener", "AudioSource", "Billboard", "BoxCollider2D",
    "BoxCollider3D", "Camera", "CapsuleCollider3D", "CharacterController",
    "CircleCollider2D", "ConvexHullCollider3D", "DirectionalLight",
    "InputMover", "Joint", "LODGroup", "Light2D", "MeshCollider3D",
    "MeshRenderer", "ModelRenderer", "NativeScript", "NavMesh",
    "NavMeshAgent", "ParallaxLayer", "ParticleSystem", "PointLight",
    "PolygonCollider2D", "ReflectionProbe", "RenderCulling", "Rigidbody",
    "Rotator", "SphereCollider3D", "SpotLight", "SpriteAnimator",
    "SpriteMask", "SpriteParticles2D", "SpriteRenderer", "TextRenderer",
    "Tilemap", "TransformAnimator", "UIButton", "UICanvas", "UIImage",
    "UIInputField", "UILayoutGroup", "UIRectTransform", "UIScrollView",
    "UISlider", "UIToggle",
}

LAMAPON_API_TOKEN = re.compile(r"\bLamaPon::([A-Za-z_][A-Za-z0-9_]*)")
ASSET_PATH_TOKEN = re.compile(
    r"(?:std::filesystem::path\s*)?\{?\s*"
    r'"((?:textures|audio|scenes|models|fonts)/[^"\r\n]+)"'
)
SCRIPT_REGISTRATION_TOKEN = re.compile(
    r"LAMAPON_SCRIPT_(?:NAMED|WITH_SCHEMA)\s*\(\s*[^,]+,\s*\"([^\"]+)\"",
    re.MULTILINE,
)
DYNAMIC_SCRIPT_TOKEN = re.compile(
    r"AddComponent\s*<\s*LamaPon::NativeScriptComponent\s*>\s*"
    r"\(\s*\"([^\"]+)\"",
    re.MULTILINE,
)
WEB_UNSUPPORTED_ENVIRONMENT_EFFECTS = {
    "autoExposure": "自動露出",
    "bloom": "ブルーム",
    "depthOfField": "被写界深度",
    "motionBlur": "モーションブラー",
    "screenSpaceLensFlare": "スクリーンスペースレンズフレア",
}
PORTABLE_INPUT_ACTIONS = {
    "MoveHorizontal",
    "MoveVertical",
    "LookHorizontal",
    "LookVertical",
    "Accelerate",
    "Brake",
    "Restart",
    "Jump",
    "Submit",
    "Cancel",
    "Pause",
}
PORTABLE_INPUT_CONTROLS = {
    *(f"Keyboard{letter}" for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"),
    *(f"Keyboard{digit}" for digit in "0123456789"),
    "KeyboardLeft", "KeyboardRight", "KeyboardUp", "KeyboardDown",
    "KeyboardSpace", "KeyboardEnter", "KeyboardEscape", "KeyboardTab",
    "KeyboardLeftShift", "KeyboardRightShift",
    "KeyboardLeftControl", "KeyboardRightControl",
    "MouseLeft", "MouseRight", "MouseMiddle", "MouseX", "MouseY",
    "MouseWheel",
    "GamePadLeftX", "GamePadLeftY", "GamePadRightX", "GamePadRightY",
    "GamePadLeftTrigger", "GamePadRightTrigger",
    "GamePadA", "GamePadB", "GamePadX", "GamePadY",
    "GamePadLeftShoulder", "GamePadRightShoulder",
    "GamePadBack", "GamePadStart", "GamePadLeftStick", "GamePadRightStick",
    "GamePadDPadUp", "GamePadDPadDown", "GamePadDPadLeft",
    "GamePadDPadRight",
}
INPUT_ACTION_TOKEN = re.compile(
    r"\b(?:Value|WasPressed|IsDown|WasReleased)\s*\(\s*\"([^\"]+)\""
)

IGNORED_RUNTIME_ASSET_EXTENSIONS = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".meta",
    # ソースに付属するファイルは、準備中に正規化したGLBへ統合します。
    ".bin",
    ".mtl",
}

# 変換後も仮想アセットパスを変更しません。たとえばtextures/road.pngは同じ
# パスへWebPデータとして配置するため、既存のC++コードやシーン参照を直す必要は
# ありません。ブラウザー側は拡張子ではなくファイルシグネチャで形式を判定します。
WEB_ASSET_CONVERSIONS: dict[str, tuple[str, str]] = {
    ".bmp": ("image", "webp"),
    ".dds": ("image", "webp"),
    ".gif": ("image", "webp"),
    ".jpeg": ("image", "webp"),
    ".jpg": ("image", "webp"),
    ".png": ("image", "webp"),
    ".tga": ("image", "webp"),
    ".tif": ("image", "webp"),
    ".tiff": ("image", "webp"),
    ".aac": ("audio", "wav"),
    ".flac": ("audio", "wav"),
    ".m4a": ("audio", "wav"),
    ".mp3": ("audio", "wav"),
    ".ogg": ("audio", "wav"),
    ".wma": ("audio", "wav"),
    ".3ds": ("model", "glb"),
    ".3mf": ("model", "glb"),
    ".ac": ("model", "glb"),
    ".blend": ("model", "glb"),
    ".dae": ("model", "glb"),
    ".dxf": ("model", "glb"),
    ".fbx": ("model", "glb"),
    ".glb": ("model", "glb"),
    ".gltf": ("model", "glb"),
    ".ifc": ("model", "glb"),
    ".lwo": ("model", "glb"),
    ".md2": ("model", "glb"),
    ".md3": ("model", "glb"),
    ".md5mesh": ("model", "glb"),
    ".ms3d": ("model", "glb"),
    ".obj": ("model", "glb"),
    ".off": ("model", "glb"),
    ".ogex": ("model", "glb"),
    ".ply": ("model", "glb"),
    ".pmx": ("model", "glb"),
    ".smd": ("model", "glb"),
    ".step": ("model", "glb"),
    ".stl": ("model", "glb"),
    ".stp": ("model", "glb"),
    ".vrm": ("model", "glb"),
    ".x": ("model", "glb"),
    ".x3d": ("model", "glb"),
}

WEB_ASSET_CONVERTER_SETTINGS = {
    # Windowsに同名のconvert.exeがあっても、代替ツールとして使用しません。
    "image": ("imageMagick", ("magick",)),
    "audio": ("ffmpeg", ("ffmpeg",)),
    "model": ("assimp", ("assimp",)),
}
WEB_ASSET_KIND_LABELS = {
    "image": "画像",
    "audio": "音声",
    "model": "モデル",
}

INVALID_WEB_PROJECT_NAME = re.compile(r'[<>:"/\\|?*\x00-\x1f]')

DEFAULT_RUNTIME_ASSET_DIRECTORIES = (
    "animations",
    "audio",
    "fonts",
    "materials",
    "models",
    "prefabs",
    "scenes",
    "textures",
)


def portable_project_input_actions(source: Path) -> dict[str, list[dict[str, Any]]]:
    """LamaPonプロジェクトから、ブラウザー用に変換できる入力設定を読み込みます。"""
    project_path = source / ".lamapon" / "project.json"
    if not project_path.is_file():
        return {}
    try:
        document = json.loads(project_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return {}
    actions = document.get("inputActions", []) if isinstance(document, dict) else []
    if not isinstance(actions, list):
        return {}
    result: dict[str, list[dict[str, Any]]] = {}
    for action in actions:
        if not isinstance(action, dict):
            continue
        name = action.get("name")
        bindings = action.get("bindings", [])
        if not isinstance(name, str) or not name or not isinstance(bindings, list):
            continue
        normalized: list[dict[str, Any]] = []
        for binding in bindings:
            if not isinstance(binding, dict):
                continue
            control = binding.get("control")
            scale = binding.get("scale", 1.0)
            if isinstance(control, str) and control \
                    and isinstance(scale, (int, float)):
                normalized.append({"control": control, "scale": float(scale)})
        result[name] = normalized
    return result


def web_project_name(
    source: Path,
    project: dict[str, Any],
    project_kind: str,
) -> str:
    """配布ファイル名に使用する安定したプロジェクト名を返します。"""
    explicit_name = project.get("projectName")
    if explicit_name is not None:
        name = explicit_name
    elif project_kind == "lamapon-project":
        name = source.name
    else:
        name = project.get("name", source.name)
    if not isinstance(name, str) or not name.strip():
        raise ExportError("The Web project name must be a non-empty string.")
    name = name.strip()
    if name in {".", ".."} or INVALID_WEB_PROJECT_NAME.search(name):
        raise ExportError(
            "The Web project name contains characters that cannot be used "
            "in a cross-platform filename."
        )
    return name


def web_artifact_prefix(
    source: Path,
    project: dict[str, Any],
    project_kind: str,
) -> str:
    return f"LamaPonWebGL-{web_project_name(source, project, project_kind)}"


def is_lamapon_project(project_path: Path, project: dict[str, Any]) -> bool:
    return (
        project.get("format") == "LamaPonProject"
        or project_path.parent.name == ".lamapon"
    )


def lamapon_project_root(project_path: Path, project: dict[str, Any]) -> Path:
    if (
        is_lamapon_project(project_path, project)
        and project_path.parent.name == ".lamapon"
    ):
        return project_path.parent.parent.resolve()
    return project_path.parent.resolve()


def load_project(project_path: Path) -> tuple[Path, dict[str, Any]]:
    if not project_path.is_file():
        raise ExportError(f"Project file was not found: {project_path}")
    try:
        project = json.loads(project_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ExportError(
            f"Project JSON is invalid at line {error.lineno}, "
            f"column {error.colno}: {project_path}"
        ) from error
    if not isinstance(project, dict):
        raise ExportError("The project root must be a JSON object.")
    return project_path.parent, project


def safe_cmake_target(name: str) -> str:
    normalized = re.sub(r"[^A-Za-z0-9_.+-]", "_", name.strip())
    if not normalized or not re.match(r"[A-Za-z_]", normalized):
        normalized = f"LamaPon_{normalized}"
    return normalized


def prepare_normal_lamapon_web_configuration(
    source: Path,
    project: dict[str, Any],
) -> dict[str, Any]:
    """通常のLamaPonプロジェクトからメモリ上にポータブルターゲットを作成します。

    元のプロジェクトには何も書き込みません。明示された高度な設定は保持し、
    通常のプロジェクト設定、スクリプト、エディターが生成する起動シーンだけで
    利用できるようにします。
    """
    export = project.get("export", {})
    if export is None:
        export = {}
    if not isinstance(export, dict):
        raise ExportError("Project export must be an object when specified.")
    web_value = export.get("web", project.get("webExport", {}))
    if web_value is None:
        web_value = {}
    if not isinstance(web_value, dict):
        raise ExportError("Project export.web must be an object when specified.")
    web = dict(web_value)

    script_root = source / "assets" / "scripts"
    if "sources" not in web:
        source_files = [
            path for path in script_root.rglob("*")
            if path.is_file()
            and path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}
            and path.name.lower() not in DEFAULT_PLATFORM_SOURCE_NAMES
        ] if script_root.is_dir() else []
        web["sources"] = [
            path.relative_to(source).as_posix()
            for path in sorted(source_files)
        ]

    asset_root = source / "assets"
    if "assetIncludePaths" not in web:
        web["assetIncludePaths"] = [
            name for name in DEFAULT_RUNTIME_ASSET_DIRECTORIES
            if (asset_root / name).exists()
        ]
    web.setdefault("assetDirectory", "assets")
    startup_scene = project.get("startupScene", "scenes/Main.scene.json")
    if not isinstance(startup_scene, str) or not startup_scene:
        raise ExportError("LamaPonProject startupScene must be a path string.")
    web.setdefault("scenePath", f"/assets/{startup_scene.lstrip('/')}")
    web.setdefault("renderer", "webgl2")
    web.setdefault("buildSystem", "lamapon")
    web.setdefault("portableGame", True)
    web.setdefault("singleFile", True)
    web.setdefault(
        "cmakeTarget",
        safe_cmake_target(f"LamaPonWeb_{source.name}"),
    )

    normalized_export = dict(export)
    normalized_export.setdefault("targets", ["web"])
    normalized_export["web"] = web
    project["export"] = normalized_export
    return web


def require_web_configuration(
    project_path: Path,
    project_root: Path,
    project: dict[str, Any],
) -> tuple[Path, str, str, str, str]:
    if is_lamapon_project(project_path, project):
        web = prepare_normal_lamapon_web_configuration(project_root, project)
        renderer = web.get("renderer", "webgl2")
        target = web.get("cmakeTarget")
        inferred_modules = resolved_project_modules(
            project_root,
            project,
            "lamapon-project",
        )
        profile = web.get(
            "compatibilityProfile",
            "webgl2-basic-3d"
            if "renderer3d" in inferred_modules
            else "webgl2-basic-2d",
        )
        if renderer != "webgl2":
            raise ExportError(
                "The first Web backend is webgl2; "
                f"received {renderer!r}."
            )
        if not isinstance(target, str) or not target:
            raise ExportError("Web cmakeTarget must be a non-empty string.")
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.+-]*", target) is None:
            raise ExportError(
                "Web cmakeTarget contains characters that are unsafe for "
                "a generated CMake target."
            )
        if not isinstance(profile, str) or not profile:
            raise ExportError(
                "Web compatibilityProfile must be a non-empty string."
            )
        return project_root, target, renderer, profile, "lamapon-project"

    export = project.get("export")
    if not isinstance(export, dict):
        raise ExportError("Project has no export configuration.")
    targets = export.get("targets")
    if not isinstance(targets, list) or "web" not in targets:
        raise ExportError("Project export.targets does not include 'web'.")
    web = export.get("web", {})
    if web is None:
        web = {}
    if not isinstance(web, dict):
        raise ExportError("Project export.web must be an object when specified.")
    renderer = web.get("renderer", "webgl2")
    if renderer != "webgl2":
        raise ExportError(
            "The first Web backend is webgl2; "
            f"received {renderer!r}."
        )
    source_directory = web.get("sourceDirectory", ".")
    build_system = web.get("buildSystem", "cmake")
    target = web.get("cmakeTarget", project.get("name"))
    profile = web.get("compatibilityProfile", "webgl2-basic-2d")
    if not isinstance(source_directory, str) or not source_directory:
        raise ExportError("export.web.sourceDirectory must be a path string.")
    if not isinstance(target, str) or not target:
        raise ExportError("export.web.cmakeTarget must be a non-empty string.")
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.+-]*", target) is None:
        raise ExportError(
            "export.web.cmakeTarget contains characters that are unsafe for "
            "a generated CMake target."
        )
    if not isinstance(profile, str) or not profile:
        raise ExportError(
            "export.web.compatibilityProfile must be a non-empty string."
        )
    if build_system not in {"cmake", "lamapon"}:
        raise ExportError(
            "export.web.buildSystem must be 'lamapon' or 'cmake'."
        )
    source = (project_root / source_directory).resolve()
    if not source.is_dir():
        raise ExportError(f"Web source directory was not found: {source}")
    if build_system == "cmake" and not (source / "CMakeLists.txt").is_file():
        raise ExportError(f"Web source has no CMakeLists.txt: {source}")
    return source, target, renderer, profile, (
        "lamapon-web-target" if build_system == "lamapon" else "cmake"
    )


def source_is_excluded(path: Path, excluded: list[Path]) -> bool:
    return any(path == item or is_within(path, item) for item in excluded)


def finding(
    level: str,
    code: str,
    message: str,
    action: str,
) -> dict[str, str]:
    return {
        "level": level,
        "code": code,
        "message": message,
        "action": action,
    }


def web_asset_converter(
    web: dict[str, Any],
    kind: str,
) -> Path | None:
    setting_name, commands = WEB_ASSET_CONVERTER_SETTINGS[kind]
    configured = web.get("converterTools", {})
    if configured is None:
        configured = {}
    if not isinstance(configured, dict):
        raise ExportError("export.web.converterTools must be an object.")
    value = configured.get(setting_name)
    if value is not None:
        if not isinstance(value, str) or not value:
            raise ExportError(
                f"export.web.converterTools.{setting_name} must be a path string."
            )
        candidate = Path(value).expanduser()
        resolved = (
            candidate.resolve()
            if candidate.is_absolute() or candidate.parent != Path(".")
            else Path(shutil.which(value) or value).resolve()
        )
        if not resolved.is_file():
            raise ExportError(
                f"Configured Web asset converter was not found: {resolved}"
            )
        return resolved
    for command in commands:
        located = shutil.which(command)
        if located:
            return Path(located).resolve()
    return None


def run_asset_conversion(
    source: Path,
    destination: Path,
    kind: str,
    runtime_format: str,
    converter: Path,
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if kind == "image":
        # LamaPonのネイティブ版ではGIFを動画ではなく単一テクスチャとして扱うため、
        # 先頭フレームを選んで同じ挙動を保ちます。
        image_source = f"{source}[0]" if source.suffix.lower() == ".gif" \
            else str(source)
        command = [str(converter), image_source]
        if runtime_format == "webp":
            # デコード済みの画素とアルファを保持し、追加の非可逆劣化を避けながら
            # Web出力のコンテナーだけを標準化します。
            command.extend((
                "-define", "webp:lossless=true",
                "-quality", "100",
            ))
        command.append(f"{runtime_format}:{destination}")
    elif kind == "audio":
        command = [
            str(converter),
            "-v", "error",
            "-y",
            "-i", str(source),
            "-vn",
            "-acodec", "pcm_s16le",
            "-f", runtime_format,
            str(destination),
        ]
    elif kind == "model":
        command = [
            str(converter),
            "export",
            str(source),
            str(destination),
            f"-f{runtime_format}2",
        ]
    else:
        raise ExportError(f"Unknown Web asset conversion kind: {kind}")
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or not destination.is_file() \
            or destination.stat().st_size == 0:
        detail = (result.stderr or result.stdout).strip()
        if len(detail) > 600:
            detail = detail[:600] + "..."
        raise ExportError(
            f"Failed to convert Web asset '{source}' to {runtime_format.upper()}"
            + (f": {detail}" if detail else ".")
        )
    payload = destination.read_bytes()[:16]
    if runtime_format == "webp" and (
        len(payload) < 12
        or payload[:4] != b"RIFF"
        or payload[8:12] != b"WEBP"
    ):
        raise ExportError(
            f"Image converter did not produce valid WebP bytes: {source}"
        )
    if runtime_format == "wav" and (
        len(payload) < 12
        or payload[:4] != b"RIFF"
        or payload[8:12] != b"WAVE"
    ):
        raise ExportError(
            f"Audio converter did not produce valid WAV bytes: {source}"
        )
    if runtime_format == "glb" and (
        len(payload) < 12
        or payload[:4] != b"glTF"
        or struct.unpack_from("<I", payload, 4)[0] != 2
    ):
        raise ExportError(
            f"Model converter did not produce valid GLB 2.0 bytes: {source}"
        )


def externalize_glb_images(
    glb_path: Path,
    image_converter: Path | None,
) -> list[Path]:
    """GLBの埋め込み画像を可逆WebPのサイドカーファイルへ書き換えます。

    正規化したモデルは元の仮想パスに保ち、マテリアルの画像URIは隣に配置する
    WebPファイルへ変更します。これにより、ブラウザーへ渡すすべての画像に
    WebP限定の規則を適用できます。
    """
    data = glb_path.read_bytes()
    if len(data) < 20 or data[:4] != b"glTF":
        raise ExportError(f"Converted model is not a GLB 2.0 file: {glb_path}")
    magic, version, declared_length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF" or version != 2 or declared_length != len(data):
        raise ExportError(f"Converted model has an invalid GLB header: {glb_path}")
    chunks: list[tuple[int, bytes]] = []
    offset = 12
    while offset + 8 <= len(data):
        length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        end = offset + length
        if end > len(data):
            raise ExportError(f"Converted model has a truncated GLB chunk: {glb_path}")
        chunks.append((chunk_type, data[offset:end]))
        offset = end
    json_chunk = next(
        (payload for chunk_type, payload in chunks if chunk_type == 0x4E4F534A),
        None,
    )
    binary_chunk = next(
        (payload for chunk_type, payload in chunks if chunk_type == 0x004E4942),
        None,
    )
    if json_chunk is None:
        raise ExportError(f"Converted model has no GLB JSON chunk: {glb_path}")
    try:
        document = json.loads(json_chunk.rstrip(b" \t\r\n\x00").decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ExportError(f"Converted model has invalid GLB JSON: {glb_path}") from error
    if not isinstance(document, dict):
        raise ExportError(f"Converted model GLB root is not an object: {glb_path}")
    images = document.get("images", [])
    buffer_views = document.get("bufferViews", [])
    if not isinstance(images, list) or not isinstance(buffer_views, list):
        raise ExportError(f"Converted model has invalid image metadata: {glb_path}")
    if not images:
        return []
    generated: list[Path] = []
    with tempfile.TemporaryDirectory(prefix="lamapon-web-model-") as temporary:
        temporary_root = Path(temporary)
        for index, image in enumerate(images):
            if not isinstance(image, dict) or "bufferView" not in image:
                raise ExportError(
                    f"Converted model image #{index} is external instead of "
                    f"self-contained: {glb_path}"
                )
            if image_converter is None:
                raise ExportError(
                    "The Hub image conversion module is required to normalize "
                    f"embedded model texture #{index}: {glb_path}"
                )
            view_index = image.get("bufferView")
            if (
                not isinstance(view_index, int)
                or view_index < 0
                or view_index >= len(buffer_views)
                or binary_chunk is None
            ):
                raise ExportError(
                    f"Converted model image #{index} has an invalid bufferView: "
                    f"{glb_path}"
                )
            view = buffer_views[view_index]
            if not isinstance(view, dict) or view.get("buffer", 0) != 0:
                raise ExportError(
                    f"Converted model image #{index} uses an unsupported buffer: "
                    f"{glb_path}"
                )
            start = int(view.get("byteOffset", 0))
            length = int(view.get("byteLength", 0))
            if start < 0 or length <= 0 or start + length > len(binary_chunk):
                raise ExportError(
                    f"Converted model image #{index} has invalid byte bounds: "
                    f"{glb_path}"
                )
            mime = str(image.get("mimeType", "image/png")).lower()
            source_extension = {
                "image/jpeg": ".jpg",
                "image/png": ".png",
                "image/webp": ".webp",
            }.get(mime, ".image")
            temporary_image = temporary_root / f"image-{index}{source_extension}"
            temporary_image.write_bytes(binary_chunk[start:start + length])
            sidecar = glb_path.with_name(
                f"{glb_path.name}.image-{index}.webp"
            )
            run_asset_conversion(
                temporary_image,
                sidecar,
                "image",
                "webp",
                image_converter,
            )
            image.pop("bufferView", None)
            image.pop("mimeType", None)
            image["uri"] = sidecar.name
            generated.append(sidecar)
    if not generated:
        return []
    encoded_json = json.dumps(
        document,
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    encoded_json += b" " * ((4 - len(encoded_json) % 4) % 4)
    rebuilt_chunks: list[tuple[int, bytes]] = []
    replaced_json = False
    for chunk_type, payload in chunks:
        if chunk_type == 0x4E4F534A and not replaced_json:
            rebuilt_chunks.append((chunk_type, encoded_json))
            replaced_json = True
        else:
            rebuilt_chunks.append((chunk_type, payload))
    total_length = 12 + sum(8 + len(payload) for _, payload in rebuilt_chunks)
    rebuilt = bytearray(struct.pack("<4sII", b"glTF", 2, total_length))
    for chunk_type, payload in rebuilt_chunks:
        rebuilt.extend(struct.pack("<II", len(payload), chunk_type))
        rebuilt.extend(payload)
    glb_path.write_bytes(rebuilt)
    return generated


def validate_portable_glb(glb_path: Path) -> dict[str, int]:
    """ポータブルランタイムで保持できない正規GLB機能を拒否します。"""
    data = glb_path.read_bytes()
    if len(data) < 20 or data[:4] != b"glTF":
        raise ExportError(f"Converted model is not a GLB 2.0 file: {glb_path}")
    _, version, declared_length = struct.unpack_from("<4sII", data, 0)
    if version != 2 or declared_length != len(data):
        raise ExportError(f"Converted model has an invalid GLB header: {glb_path}")
    offset = 12
    json_chunk: bytes | None = None
    while offset + 8 <= len(data):
        length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        end = offset + length
        if end > len(data):
            raise ExportError(f"Converted model has a truncated GLB chunk: {glb_path}")
        if chunk_type == 0x4E4F534A and json_chunk is None:
            json_chunk = data[offset:end]
        offset = end
    if json_chunk is None:
        raise ExportError(f"Converted model has no GLB JSON chunk: {glb_path}")
    try:
        document = json.loads(json_chunk.rstrip(b" \t\r\n\x00").decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ExportError(f"Converted model has invalid GLB JSON: {glb_path}") from error
    if not isinstance(document, dict):
        raise ExportError(f"Converted model GLB root is not an object: {glb_path}")
    required_extensions = document.get("extensionsRequired", [])
    if not isinstance(required_extensions, list):
        raise ExportError(f"Converted model has invalid required extensions: {glb_path}")
    if required_extensions:
        raise ExportError(
            f"Converted model requires unsupported GLB extension(s) "
            f"{', '.join(map(str, required_extensions))}: {glb_path}"
        )
    # extensionsUsedの列挙だけではエラーにしません。Assimpはマテリアルが使用しない
    # 無害な既定拡張も列挙するため、実データを検査して一般的なインポートを許可し、
    # 失われる描画機能だけを拒否します。
    unsupported_material_extensions = {
        "KHR_materials_anisotropy",
        "KHR_materials_clearcoat",
        "KHR_materials_diffuse_transmission",
        "KHR_materials_dispersion",
        "KHR_materials_iridescence",
        "KHR_materials_sheen",
        "KHR_materials_transmission",
        "KHR_materials_volume",
    }
    materials = document.get("materials", [])
    if not isinstance(materials, list):
        raise ExportError(f"Converted model has invalid material metadata: {glb_path}")
    texture_view_names = {
        "baseColorTexture", "metallicRoughnessTexture", "normalTexture",
        "occlusionTexture", "emissiveTexture",
    }
    unlit_materials = 0
    for material_index, material in enumerate(materials):
        if not isinstance(material, dict):
            continue
        extensions = material.get("extensions", {})
        if not isinstance(extensions, dict):
            raise ExportError(
                f"Converted model material #{material_index} has invalid "
                f"extensions: {glb_path}"
            )
        active_unsupported = sorted(
            extension for extension in unsupported_material_extensions
            if extension in extensions and extensions[extension]
        )
        if active_unsupported:
            raise ExportError(
                f"Converted model material #{material_index} uses advanced "
                f"material extension(s) {', '.join(active_unsupported)} that "
                f"the WebGL material backend cannot reproduce: {glb_path}"
            )
        if "KHR_materials_unlit" in extensions:
            unlit_materials += 1
        specular_extension = extensions.get("KHR_materials_specular", {})
        if isinstance(specular_extension, dict) and (
            specular_extension.get("specularTexture")
            or specular_extension.get("specularColorTexture")
        ):
            raise ExportError(
                f"Converted model material #{material_index} uses KHR "
                f"specular textures; the basic WebGL backend supports its "
                f"factor/color values but not the two additional texture "
                f"slots yet: {glb_path}"
            )
        views: list[dict[str, Any]] = []
        pbr = material.get("pbrMetallicRoughness", {})
        if isinstance(pbr, dict):
            views.extend(
                value for key, value in pbr.items()
                if key in texture_view_names and isinstance(value, dict)
            )
        views.extend(
            value for key, value in material.items()
            if key in texture_view_names and isinstance(value, dict)
        )
        for view in views:
            if int(view.get("texCoord", 0)) != 0:
                raise ExportError(
                    f"Converted model material #{material_index} uses UV set "
                    f"{view.get('texCoord')}; the Portable renderer currently "
                    f"supports TEXCOORD_0 only: {glb_path}"
                )
            view_extensions = view.get("extensions", {})
            if isinstance(view_extensions, dict) \
                    and "KHR_texture_transform" in view_extensions:
                raise ExportError(
                    f"Converted model material #{material_index} uses a "
                    f"texture UV transform that cannot yet be reproduced "
                    f"exactly by the Portable renderer: {glb_path}"
                )
    accessors = document.get("accessors", [])
    meshes = document.get("meshes", [])
    if not isinstance(accessors, list) or not isinstance(meshes, list):
        raise ExportError(f"Converted model has invalid mesh metadata: {glb_path}")
    renderable_primitives = 0
    total_vertices = 0
    for mesh in meshes:
        if not isinstance(mesh, dict):
            continue
        primitives = mesh.get("primitives", [])
        if not isinstance(primitives, list):
            continue
        for primitive in primitives:
            if not isinstance(primitive, dict) or primitive.get("mode", 4) != 4:
                continue
            if primitive.get("targets"):
                raise ExportError(
                    f"Converted model uses morph targets, which are not yet "
                    f"portable without changing the animation: {glb_path}"
                )
            extensions = primitive.get("extensions", {})
            if isinstance(extensions, dict) and (
                "KHR_draco_mesh_compression" in extensions
                or "EXT_meshopt_compression" in extensions
            ):
                raise ExportError(
                    f"Converted model uses compressed geometry unsupported by "
                    f"the current Portable decoder: {glb_path}"
                )
            attributes = primitive.get("attributes", {})
            position_index = attributes.get("POSITION") \
                if isinstance(attributes, dict) else None
            if not isinstance(position_index, int) \
                    or position_index < 0 or position_index >= len(accessors):
                continue
            position = accessors[position_index]
            vertex_count = position.get("count", 0) \
                if isinstance(position, dict) else 0
            if not isinstance(vertex_count, int) or vertex_count <= 0:
                continue
            renderable_primitives += 1
            total_vertices += vertex_count
    if renderable_primitives == 0:
        raise ExportError(
            f"Converted model contains no portable triangle primitives: {glb_path}"
        )
    animations = document.get("animations", [])
    if not isinstance(animations, list):
        raise ExportError(f"Converted model has invalid animation metadata: {glb_path}")
    for animation in animations:
        if not isinstance(animation, dict):
            continue
        for channel in animation.get("channels", []):
            target = channel.get("target", {}) if isinstance(channel, dict) else {}
            if isinstance(target, dict) and target.get("path") == "weights":
                raise ExportError(
                    f"Converted model animates morph weights, which cannot yet "
                    f"be preserved by the Portable runtime: {glb_path}"
                )
    skins = document.get("skins", [])
    images = document.get("images", [])
    return {
        "meshes": len(meshes),
        "primitives": renderable_primitives,
        "vertices": total_vertices,
        "skins": len(skins) if isinstance(skins, list) else 0,
        "animations": len(animations),
        "images": len(images) if isinstance(images, list) else 0,
        "materials": len(materials),
        "unlitMaterials": unlit_materials,
    }


def infer_lamapon_modules(source: Path, project: dict[str, Any]) -> list[str]:
    """通常のLamaPonプロジェクトが使用する最小限のWebモジュールを推定します。"""
    modules = ["core", "input"]
    script_files = [
        path
        for path in source.rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_EXTENSIONS
        and not any(
            part in {".git", ".lamapon", "build", "third_party"}
            for part in path.relative_to(source).parts
        )
    ]
    script_text = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in script_files
    )
    for api in LAMAPON_API_TOKEN.findall(script_text):
        module = PORTABLE_API_MODULES.get(api)
        if module is not None:
            modules.append(module)

    startup_scene = project.get("startupScene")
    if isinstance(startup_scene, str) and startup_scene:
        scene_path = source / "assets" / startup_scene
        try:
            scene = json.loads(scene_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError):
            scene = None
        if isinstance(scene, dict):
            for obj in scene.get("objects", []):
                if not isinstance(obj, dict):
                    continue
                for component in obj.get("components", []):
                    if not isinstance(component, dict):
                        continue
                    component_type = component.get("type")
                    module = PORTABLE_SCENE_COMPONENTS.get(component_type)
                    if module is not None:
                        modules.append(module)

    # 通常のLamaPon 3Dプロジェクトでは、起動シーンが空でもスクリプトが描画
    # オブジェクトを動的に生成できます。その場合はグラフィックス設定から判定します。
    graphics = project.get("graphics", {})
    if not ({"renderer2d", "renderer3d"} & set(modules)):
        modules.append(
            "renderer3d"
            if isinstance(graphics, dict) and graphics.get("renderingPath")
            else "renderer2d"
        )
    return list(dict.fromkeys(modules))


def resolved_project_modules(
    source: Path,
    project: dict[str, Any],
    project_kind: str,
) -> list[str]:
    export = project.get("export", {})
    if not isinstance(export, dict):
        raise ExportError("Project export must be an object when specified.")
    modules = export.get(
        "modules",
        infer_lamapon_modules(source, project)
        if project_kind == "lamapon-project"
        else DEFAULT_MODULES,
    )
    if not isinstance(modules, list) or not modules:
        raise ExportError("Project export.modules must be a string list.")
    if any(not isinstance(module, str) or not module for module in modules):
        raise ExportError("Every project export.modules entry must be a string.")
    return list(dict.fromkeys(modules))


def selected_web_source_files(source: Path, web: dict[str, Any]) -> list[Path]:
    source_values = web.get("sources")
    if source_values is None:
        return [
            path
            for path in source.rglob("*")
            if path.is_file() and path.suffix.lower() in SOURCE_EXTENSIONS
        ]
    if not isinstance(source_values, list) or any(
        not isinstance(value, str) or not value for value in source_values
    ):
        raise ExportError("export.web.sources must be a non-empty string list.")
    selected = [require_relative_file(
        source, value, "export.web.sources entry") for value in source_values]
    headers = [
        path.resolve()
        for path in source.rglob("*")
        if path.is_file() and path.suffix.lower() in {".h", ".hh", ".hpp", ".hxx"}
        and not any(part in {".git", ".lamapon", "build", "third_party"}
                    for part in path.relative_to(source).parts)
    ]
    return list(dict.fromkeys([*selected, *headers]))


def web_asset_roots(
    source: Path,
    web: dict[str, Any],
) -> tuple[Path, list[Path]] | None:
    asset_value = web.get("assetDirectory", "assets")
    if not isinstance(asset_value, str) or not asset_value:
        raise ExportError("export.web.assetDirectory must be a path string.")
    asset_directory = (source / asset_value).resolve()
    if not asset_directory.exists():
        return None
    if not asset_directory.is_dir():
        raise ExportError(f"Web asset path is not a directory: {asset_directory}")
    include_values = web.get("assetIncludePaths", ["."])
    if not isinstance(include_values, list) or not include_values or any(
        not isinstance(item, str) or not item for item in include_values
    ):
        raise ExportError("export.web.assetIncludePaths must be a string list.")
    included_roots = [(asset_directory / item).resolve() for item in include_values]
    if any(not is_within(path, asset_directory) for path in included_roots):
        raise ExportError("export.web.assetIncludePaths must stay inside assets.")
    if any(not path.exists() for path in included_roots):
        missing = next(path for path in included_roots if not path.exists())
        raise ExportError(f"Included Web asset path was not found: {missing}")
    return asset_directory, included_roots


def asset_is_selected(path: Path, roots: list[Path]) -> bool:
    return any(path == root or is_within(path, root) for root in roots)


def validate_asset_integrity(
    path: Path,
    source: Path,
) -> list[dict[str, str]]:
    findings: list[dict[str, str]] = []
    relative = path.relative_to(source)
    extension = path.suffix.lower()
    try:
        data = path.read_bytes()
    except OSError as error:
        return [finding(
            "reject", "unreadable-web-asset",
            f"アセット「{relative}」を読み込めませんでした: {error}",
            "ファイルのアクセス権を修復するか、アセットを置き換えてください。",
        )]
    if not data:
        return [finding(
            "reject", "empty-web-asset",
            f"アセット「{relative}」が空です。",
            "空のアセットを置き換えるか削除してください。",
        )]
    if extension == ".png":
        if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
            findings.append(finding(
                "reject", "corrupt-png-asset",
                f"アセット「{relative}」のPNGヘッダーが正しくありません。",
                "テクスチャを有効なPNGとして再出力してください。",
            ))
        else:
            width, height = struct.unpack(">II", data[16:24])
            if width == 0 or height == 0:
                findings.append(finding(
                    "reject", "invalid-texture-size",
                    f"テクスチャ「{relative}」の幅または高さが0です。",
                    "テクスチャを再出力してください。",
                ))
            elif width > 4096 or height > 4096:
                findings.append(finding(
                    "warning", "large-web-texture",
                    f"テクスチャ「{relative}」のサイズは{width}x{height}です。ブラウザーのメモリを多く消費する可能性があります。",
                    "Web用テクスチャは4096以下を目安に縮小してください。",
                ))
    elif extension in {".jpg", ".jpeg"}:
        if len(data) < 4 or not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
            findings.append(finding(
                "reject", "corrupt-jpeg-asset",
                f"アセット「{relative}」のJPEG境界マーカーが正しくありません。",
                "テクスチャを有効なJPEGとして再出力してください。",
            ))
    elif extension == ".webp":
        if (
            len(data) < 12
            or data[:4] != b"RIFF"
            or data[8:12] != b"WEBP"
        ):
            findings.append(finding(
                "reject", "corrupt-webp-asset",
                f"アセット「{relative}」のWebPヘッダーが正しくありません。",
                "テクスチャを有効なWebPとして再出力してください。",
            ))
    elif extension == ".wav":
        if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
            findings.append(finding(
                "reject", "corrupt-wav-asset",
                f"アセット「{relative}」のRIFF/WAVEヘッダーが正しくありません。",
                "音声を有効なWAVファイルとして再出力してください。",
            ))
    elif extension == ".json":
        try:
            json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            findings.append(finding(
                "reject", "invalid-json-asset",
                f"アセット「{relative}」は有効なUTF-8 JSONではありません。",
                "JSONアセットを修復するか再出力してください。",
            ))
    return findings


def validate_portable_contract(
    source: Path,
    project: dict[str, Any],
    project_kind: str,
    web: dict[str, Any],
    modules: list[str],
) -> list[dict[str, str]]:
    if project_kind not in {"lamapon-web-target", "lamapon-project"} \
            or not web.get("portableGame", False):
        return []
    source = source.resolve()
    findings: list[dict[str, str]] = []
    source_files = selected_web_source_files(source, web)
    configured_input_actions = portable_project_input_actions(source)
    available_input_actions = PORTABLE_INPUT_ACTIONS \
        | set(configured_input_actions)
    source_text: dict[Path, str] = {
        path: path.read_text(encoding="utf-8", errors="replace")
        for path in source_files
    }
    declared_modules = set(modules)
    for required in ("core", "input"):
        if required not in declared_modules:
            findings.append(finding(
                "reject", "missing-portable-base-module",
                f"ポータブルWebゲームには「{required}」モジュールが必要です。",
                f"export.modulesに「{required}」を追加してください。",
            ))
    if not ({"renderer2d", "renderer3d"} & declared_modules):
        findings.append(finding(
            "reject", "missing-renderer-module",
            "ポータブルWebゲームにはrenderer2dまたはrenderer3dが必要です。",
            "プロジェクトが使用するレンダラーを指定してください。",
        ))
    api_locations: dict[str, list[str]] = {}
    asset_references: dict[str, list[str]] = {}
    registered_scripts: set[str] = set()
    dynamic_scripts: dict[str, list[str]] = {}
    input_actions: dict[str, list[str]] = {}
    for path, text in source_text.items():
        relative = path.relative_to(source)
        for line_number, line in enumerate(text.splitlines(), start=1):
            for api in LAMAPON_API_TOKEN.findall(line):
                api_locations.setdefault(api, []).append(
                    f"{relative}:{line_number}"
                )
            for asset in ASSET_PATH_TOKEN.findall(line):
                asset_references.setdefault(asset, []).append(
                    f"{relative}:{line_number}"
                )
            for action in INPUT_ACTION_TOKEN.findall(line):
                input_actions.setdefault(action, []).append(
                    f"{relative}:{line_number}"
                )
        registered_scripts.update(SCRIPT_REGISTRATION_TOKEN.findall(text))
        for script_id in DYNAMIC_SCRIPT_TOKEN.findall(text):
            dynamic_scripts.setdefault(script_id, []).append(str(relative))

    for api, locations in sorted(api_locations.items()):
        required_module = PORTABLE_API_MODULES.get(api)
        location = locations[0]
        if required_module is None:
            findings.append(finding(
                "reject",
                "unsupported-portable-api",
                f"{location}: LamaPon::{api}にはポータブルWeb版の実装がありません。",
                "Web出力を行う前に、このAPIをポータブルランタイムへ実装してテストしてください。",
            ))
        elif required_module not in declared_modules:
            findings.append(finding(
                "reject",
                "missing-required-module",
                f"{location}: LamaPon::{api}には「{required_module}」モジュールが必要ですが、プロジェクトで指定されていません。",
                f"export.modulesに「{required_module}」を追加してください。",
            ))
    for script_id, locations in sorted(dynamic_scripts.items()):
        if script_id not in registered_scripts:
            findings.append(finding(
                "reject", "unregistered-dynamic-script",
                f"{locations[0]}はNativeScript「{script_id}」を作成しますが、選択したWebソースに登録がありません。",
                "LAMAPON_SCRIPT_NAMEDを含むソースを追加するか、スクリプトIDを修正してください。",
            ))
    for action, locations in sorted(input_actions.items()):
        if action not in available_input_actions:
            findings.append(finding(
                "reject", "unsupported-input-action",
                f"{locations[0]}は入力アクション「{action}」を使用しますが、ポータブルWeb入力マップに定義がありません。",
                "Web出力を行う前に、ポータブル入力バックエンドへアクションとブラウザー用バインドを追加してください。",
            ))
            continue
        unsupported_controls = sorted({
            str(binding.get("control"))
            for binding in configured_input_actions.get(action, [])
            if binding.get("control") not in PORTABLE_INPUT_CONTROLS
        })
        if unsupported_controls:
            findings.append(finding(
                "reject", "unsupported-input-control",
                f"入力アクション「{action}」には、ブラウザーバックエンドで割り当てられない操作があります: {', '.join(unsupported_controls)}。",
                "ポータブル入力バックエンドが対応するキーボード、マウス、標準ゲームパッドの操作を使用してください。",
            ))
        elif action not in PORTABLE_INPUT_ACTIONS:
            findings.append(finding(
                "auto", "project-input-action-map",
                f"プロジェクトの入力アクション「{action}」は、出力時にLamaPonプロジェクトのバインドから変換されます。",
                "このアクション名のためにエンジンを変更する必要はありません。",
            ))

    asset_selection = web_asset_roots(source, web)
    asset_directory: Path | None = None
    included_roots: list[Path] = []
    if asset_selection is not None:
        asset_directory, included_roots = asset_selection
    for reference, locations in sorted(asset_references.items()):
        if asset_directory is None:
            findings.append(finding(
                "reject",
                "missing-asset-directory",
                f"{locations[0]}は「{reference}」を参照していますが、アセットフォルダーがありません。",
                "アセットフォルダーを復元するか、参照を削除してください。",
            ))
            continue
        target = (asset_directory / reference).resolve()
        if not is_within(target, asset_directory) or not target.is_file():
            findings.append(finding(
                "reject",
                "missing-referenced-asset",
                f"{locations[0]}が参照するアセット「{reference}」が見つかりません。",
                "アセットを追加するか、ポータブルアセットパスを修正してください。",
            ))
        elif not asset_is_selected(target, included_roots):
            findings.append(finding(
                "reject",
                "unpackaged-referenced-asset",
                f"{locations[0]}が参照する「{reference}」はassetIncludePathsの対象外です。",
                "参照先を含むアセットパスをWebパッケージへ追加してください。",
            ))

    scene_path = web.get("scenePath", "/assets/scenes/Main.scene.json")
    if not isinstance(scene_path, str) or not scene_path.startswith("/assets/"):
        findings.append(finding(
            "reject", "invalid-scene-path",
            "ポータブルシーンのパスは「/assets/」で始める必要があります。",
            "export.web.scenePathに、パッケージへ含めるシーンJSONのパスを指定してください。",
        ))
        return findings
    scene_file = (
        asset_directory / scene_path.removeprefix("/assets/")
        if asset_directory is not None else None
    )
    if scene_file is None or not scene_file.is_file():
        findings.append(finding(
            "reject", "missing-startup-scene",
            f"起動シーンが見つかりません: {scene_path}",
            "シーンをパッケージへ追加するか、export.web.scenePathを修正してください。",
        ))
        return findings
    if not asset_is_selected(scene_file.resolve(), included_roots):
        findings.append(finding(
            "reject", "unpackaged-startup-scene",
            f"起動シーン「{scene_path}」はassetIncludePathsの対象外です。",
            "シーンを含むフォルダーをWebパッケージへ追加してください。",
        ))
    try:
        scene = json.loads(scene_file.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        findings.append(finding(
            "reject", "invalid-scene-json",
            f"{scene_file.relative_to(source)}:{error.lineno}: シーンJSONが正しくありません。",
            "Web出力を行う前にシーンファイルを修正してください。",
        ))
        return findings
    if not isinstance(scene, dict) or scene.get("format") != "LamaPonScene":
        findings.append(finding(
            "reject", "invalid-scene-format",
            f"{scene_file.relative_to(source)}はLamaPonScene形式ではありません。",
            "互換性のあるLamaPon Editorでシーンを開き、保存し直してください。",
        ))
        return findings

    def scene_asset_strings(value: Any) -> list[str]:
        if isinstance(value, str) and value.startswith(
            (
                "textures/", "audio/", "scenes/", "models/", "fonts/",
                "materials/", "animations/", "shaders/",
            )
        ):
            return [value]
        if isinstance(value, list):
            return [item for child in value for item in scene_asset_strings(child)]
        if isinstance(value, dict):
            return [item for child in value.values()
                    for item in scene_asset_strings(child)]
        return []

    for reference in sorted(set(scene_asset_strings(scene))):
        target = (asset_directory / reference).resolve()
        scene_location = str(scene_file.relative_to(source))
        if not is_within(target, asset_directory) or not target.is_file():
            findings.append(finding(
                "reject", "missing-scene-asset",
                f"{scene_location}が参照するアセット「{reference}」が見つかりません。",
                "アセットを追加するか、シーン内の参照を修正してください。",
            ))
        elif not asset_is_selected(target, included_roots):
            findings.append(finding(
                "reject", "unpackaged-scene-asset",
                f"{scene_location}が参照する「{reference}」はassetIncludePathsの対象外です。",
                "アセットをWebパッケージへ追加してください。",
            ))

    material_files: list[Path] = []
    for root in included_roots:
        candidates = [root] if root.is_file() else root.rglob("*.material.json")
        material_files.extend(
            path.resolve() for path in candidates
            if path.is_file() and path.name.lower().endswith(".material.json")
        )
    for material_file in dict.fromkeys(material_files):
        location = str(material_file.relative_to(source))
        try:
            material = json.loads(material_file.read_text(encoding="utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        if (
            not isinstance(material, dict)
            or material.get("type") != "LamaPonLitMaterial"
            or material.get("version") not in {1, 2}
        ):
            findings.append(finding(
                "reject", "invalid-portable-material",
                f"マテリアル「{location}」は対応しているLamaPonLitMaterial形式ではありません。",
                "互換性のあるLamaPon Editorでマテリアルを開き、保存し直してください。",
            ))
            continue
        for field in (
            "albedoTexture", "normalTexture", "roughnessTexture",
            "metallicTexture", "occlusionTexture", "emissiveTexture",
        ):
            reference = material.get(field, "")
            if not isinstance(reference, str) or not reference:
                continue
            target = (asset_directory / reference).resolve()
            if not is_within(target, asset_directory) or not target.is_file():
                findings.append(finding(
                    "reject", "missing-material-asset",
                    f"マテリアル「{location}」が参照する{field}「{reference}」が見つかりません。",
                    "テクスチャを追加するか、マテリアル内の参照を修正してください。",
                ))
            elif not asset_is_selected(target, included_roots):
                findings.append(finding(
                    "reject", "unpackaged-material-asset",
                    f"マテリアル「{location}」が参照する「{reference}」はassetIncludePathsの対象外です。",
                    "テクスチャをWebパッケージへ追加してください。",
                ))
        shader = material.get("shader", "")
        if isinstance(shader, str) and shader:
            if Path(shader).name.lower() == "lamaponlit.hlsl":
                findings.append(finding(
                    "auto", "standard-shader-backend-replacement",
                    f"マテリアル「{location}」にはLamaPonLitが設定されています。WebGLでは対応する組み込みGLSLバックエンドを使用します。",
                    "プロジェクトの変更は不要です。",
                ))
            else:
                findings.append(finding(
                    "reject", "unsupported-custom-material-shader",
                    f"マテリアル「{location}」はカスタムHLSLシェーダー「{shader}」を使用しています。",
                    "ポータブル版のシェーダーグラフまたはGLSLバックエンドを用意するか、Webでは標準のLamaPonLitマテリアルを使用してください。",
                ))
        custom_textures = material.get("customTextures", [])
        if isinstance(custom_textures, list) and any(custom_textures):
            findings.append(finding(
                "reject", "unsupported-custom-material-textures",
                f"マテリアル「{location}」はカスタムシェーダーのテクスチャスロットを使用しています。",
                "カスタムテクスチャスロットには、ポータブル版のカスタムシェーダーバックエンドが必要です。",
            ))
        custom_parameters = material.get("customParameters", [])
        if (
            isinstance(custom_parameters, list)
            and any(
                isinstance(parameter, list)
                and any(value != 0 for value in parameter)
                for parameter in custom_parameters
            )
        ):
            findings.append(finding(
                "reject", "unsupported-custom-material-parameters",
                f"マテリアル「{location}」は既定値以外のカスタムシェーダーパラメーターを使用しています。",
                "カスタムパラメーターには、ポータブル版のカスタムシェーダーバックエンドが必要です。",
            ))

    animation_files: list[Path] = []
    for root in included_roots:
        candidates = [root] if root.is_file() else root.rglob("*.animation.json")
        animation_files.extend(
            path.resolve() for path in candidates
            if path.is_file() and path.name.lower().endswith(".animation.json")
        )
    for animation_file in dict.fromkeys(animation_files):
        location = str(animation_file.relative_to(source))
        try:
            animation = json.loads(animation_file.read_text(encoding="utf-8"))
            keyframes = animation.get("keyframes", [])
            duration = animation.get("duration", 0.0)
            times = [
                keyframe.get("time")
                for keyframe in keyframes
                if isinstance(keyframe, dict)
            ]
            valid_vectors = all(
                isinstance(keyframe, dict)
                and all(
                    isinstance(keyframe.get(field), list)
                    and len(keyframe[field]) == 3
                    and all(isinstance(value, (int, float))
                            for value in keyframe[field])
                    for field in ("position", "rotation", "scale")
                )
                for keyframe in keyframes
            )
            valid = (
                isinstance(animation, dict)
                and animation.get("format") == "LamaPonAnimationClip"
                and animation.get("version") == 1
                and isinstance(keyframes, list)
                and 1 <= len(keyframes) <= 4096
                and len(times) == len(keyframes)
                and all(isinstance(time, (int, float)) and time >= 0.0
                        for time in times)
                and all(times[index] < times[index + 1]
                        for index in range(len(times) - 1))
                and isinstance(duration, (int, float))
                and duration > 0.0
                and duration >= times[-1]
                and valid_vectors
            )
        except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
            valid = False
        if not valid:
            findings.append(finding(
                "reject", "invalid-portable-animation",
                f"アニメーション「{location}」は有効なLamaPonAnimationClip version 1形式ではありません。",
                "互換性のあるLamaPon Editorでクリップを開き、保存し直してください。",
            ))

    objects = scene.get("objects", [])
    if not isinstance(objects, list):
        findings.append(finding(
            "reject", "invalid-scene-objects",
            "シーンの「objects」は配列である必要があります。",
            "シーンを修復するか保存し直してください。",
        ))
        return findings
    object_ids: set[int] = set()
    parent_links: list[tuple[str, Any]] = []
    component_links: list[tuple[str, str, Any]] = []
    scene_scripts: set[str] = set()
    cameras: set[int] = set()
    for index, object_value in enumerate(objects):
        if not isinstance(object_value, dict):
            findings.append(finding(
                "reject", "invalid-scene-object",
                f"シーンオブジェクト#{index}はオブジェクト形式ではありません。",
                "シーンを修復するか保存し直してください。",
            ))
            continue
        object_id = object_value.get("id")
        name = str(object_value.get("name", f"#{index}"))
        if not isinstance(object_id, int) or object_id in object_ids:
            findings.append(finding(
                "reject", "invalid-scene-object-id",
                f"シーンオブジェクト「{name}」の整数IDがないか、ほかのオブジェクトと重複しています。",
                "シーンを修復するか保存し直してください。",
            ))
        else:
            object_ids.add(object_id)
        if object_value.get("parent") is not None:
            parent_links.append((name, object_value.get("parent")))
        components = object_value.get("components", [])
        if not isinstance(components, list):
            findings.append(finding(
                "reject", "invalid-scene-components",
                f"シーンオブジェクト「{name}」のcomponentsは配列である必要があります。",
                "シーンを修復するか保存し直してください。",
            ))
            continue
        for component in components:
            component_type = component.get("type") if isinstance(component, dict) else None
            required_module = PORTABLE_SCENE_COMPONENTS.get(str(component_type))
            if required_module is None:
                known_native = str(component_type) in KNOWN_NATIVE_SCENE_COMPONENTS
                findings.append(finding(
                    "reject",
                    "unsupported-scene-component",
                    f"シーンオブジェクト「{name}」は、{'既知のネイティブ' if known_native else '未登録の'}コンポーネント「{component_type}」を使用していますが、ポータブルランタイムに対応するバックエンドがありません。",
                    "Web版から削除するか、ポータブルランタイムへ実装してテストしてから出力してください。",
                ))
                continue
            if required_module not in declared_modules:
                findings.append(finding(
                    "reject", "missing-scene-module",
                    f"シーンの「{name}」にある{component_type}コンポーネントには「{required_module}」モジュールが必要です。",
                    f"export.modulesに「{required_module}」を追加してください。",
                ))
            if component_type in PORTABLE_NOOP_SCENE_COMPONENTS:
                description = (
                    "コンポーネントとして保持されますが、基本ポータブルレンダラーは"
                    "有効な全オブジェクトを描画するため、カリング処理を行いません"
                    if component_type == "RenderCulling"
                    else "ネイティブオブジェクトを作らずブラウザーバックエンドで処理されます"
                )
                findings.append(finding(
                    "auto", "scene-component-backend-replacement",
                    f"シーンの「{name}」にある{component_type}コンポーネントは、{description}。",
                    "プロジェクトの変更は不要です。",
                ))
            if component_type in PORTABLE_APPROXIMATE_SCENE_COMPONENTS:
                findings.append(finding(
                    "warning", "scene-component-approximation",
                    f"シーンの「{name}」にある{component_type}コンポーネントには、Web版の簡易ライティングを使用します。",
                    "プレビューでライティングを確認してください。このプロファイルでは高度な影やネイティブシェーダーの動作を再現しません。",
                ))
                if component.get("castsShadows", False):
                    findings.append(finding(
                        "warning", "unsupported-web-shadows",
                        f"シーンのライト「{name}」で影が有効ですが、webgl2-basic-3dでは描画できません。",
                        "Web版では影を無効にするか、影に対応したプロファイルを使用してください。",
                    ))
            if component_type == "Camera":
                if component.get("targetTexture"):
                    findings.append(finding(
                        "reject", "unsupported-camera-render-texture",
                        f"「{name}」のカメラはtargetTexture「{component.get('targetTexture')}」へ描画します。",
                        "基本WebプロファイルではメインCanvasのカメラを使用するか、ポータブル版のレンダーテクスチャバックエンドを追加してください。",
                    ))
            if component_type == "MeshRenderer":
                if str(component.get("shape", "Cube")) not in {
                    "Cube", "Sphere", "Cylinder", "Plane",
                }:
                    findings.append(finding(
                        "reject", "unsupported-primitive-shape",
                        f"「{name}」のMeshRendererは、未対応のプリミティブ形状「{component.get('shape')}」を使用しています。",
                        "Cube、Sphere、Cylinder、Plane、またはModelRendererを使用してください。",
                    ))
                shader = component.get("shader", "")
                if (
                    isinstance(shader, str)
                    and shader
                    and Path(shader).name.lower() != "lamaponlit.hlsl"
                ):
                    findings.append(finding(
                        "reject", "unsupported-mesh-custom-shader",
                        f"「{name}」のMeshRendererはカスタムシェーダー「{shader}」を使用しています。",
                        "LamaPonLitを使用するか、対応するポータブルシェーダーを追加してください。",
                    ))
                mesh_custom_textures = [
                    value for key, value in component.items()
                    if key.startswith("customTexture")
                    and isinstance(value, str) and value
                ]
                mesh_custom_parameters = component.get("customParameters", [])
                if (
                    component.get("shaderKeywords")
                    or mesh_custom_textures
                    or (
                        isinstance(mesh_custom_parameters, list)
                        and any(
                            isinstance(parameter, list)
                            and any(value != 0 for value in parameter)
                            for parameter in mesh_custom_parameters
                        )
                    )
                ):
                    findings.append(finding(
                        "reject", "unsupported-mesh-custom-bindings",
                        f"「{name}」のMeshRendererは、カスタムシェーダーのキーワード、テクスチャ、またはパラメーターを使用しています。",
                        "Web版では標準のPBRマテリアルスロットを使用してください。",
                    ))
                if component.get("worldOverlay", False):
                    findings.append(finding(
                        "reject", "unsupported-world-overlay",
                        f"「{name}」のMeshRendererでworldOverlayが有効です。",
                        "worldOverlayを無効にするか、ポータブル版のオーバーレイパスを追加してください。",
                    ))
            if component_type == "ModelRenderer":
                if component.get("wireframe", False):
                    findings.append(finding(
                        "reject", "unsupported-model-wireframe",
                        f"「{name}」のModelRendererでワイヤーフレーム描画が有効です。",
                        "Web版ではワイヤーフレームを無効にするか、ポータブル版のライン描画バックエンドを追加してください。",
                    ))
                controller = component.get("animationController", "")
                if isinstance(controller, str) and controller:
                    findings.append(finding(
                        "reject", "unsupported-animation-controller",
                        f"「{name}」のModelRendererはAnimator Controller「{controller}」を使用していますが、ステートマシンはポータブルランタイムで未対応です。",
                        "モデルに埋め込まれたアニメーション操作を使用するか、ポータブル版のAnimator Controllerランタイムを追加してください。",
                    ))
                if component.get("applyRootMotion", False):
                    findings.append(finding(
                        "reject", "unsupported-root-motion",
                        f"「{name}」のModelRendererでアニメーションのルートモーションが有効です。",
                        "ルートモーションを無効にするか、ポータブル版のルートモーション処理を追加してください。",
                    ))
                shader = component.get("shader", "")
                if (
                    isinstance(shader, str)
                    and shader
                    and Path(shader).name.lower() != "lamaponlit.hlsl"
                ):
                    findings.append(finding(
                        "reject", "unsupported-model-custom-shader",
                        f"「{name}」のModelRendererはカスタムHLSLシェーダー「{shader}」を使用しています。",
                        "標準のLamaPonLitマテリアルを使用するか、ポータブル版のシェーダーバックエンドを追加してください。",
                    ))
                if component.get("shaderKeywords"):
                    findings.append(finding(
                        "reject", "unsupported-model-shader-keywords",
                        f"「{name}」のModelRendererでカスタムシェーダーキーワードが有効です。",
                        "シェーダーバリアントには、ポータブル版のシェーダーバックエンドが必要です。",
                    ))
                if component.get("useLegacyShading", False):
                    findings.append(finding(
                        "reject", "unsupported-legacy-model-shading",
                        f"「{name}」のModelRendererは、Web版のPBRバックエンドで再現できない従来のネイティブシェーディングを使用しています。",
                        "LamaPonLit PBRを使用するか、対応するポータブルバックエンドを追加してください。",
                    ))
                if component.get("preserveEmbeddedMaterialColor", False):
                    findings.append(finding(
                        "reject", "unsupported-preserve-material-color",
                        f"「{name}」のModelRendererでは、オーバーライド時に埋め込みマテリアルの色を保持する設定が有効です。",
                        "Web版へこの設定を実装するまでは、必要なベースカラーをマテリアルへ反映してください。",
                    ))
                custom_textures = [
                    value for key, value in component.items()
                    if key.startswith("customTexture")
                    and isinstance(value, str) and value
                ]
                custom_parameters = component.get("customParameters", [])
                if custom_textures or (
                    isinstance(custom_parameters, list)
                    and any(
                        isinstance(parameter, list)
                        and any(value != 0 for value in parameter)
                        for parameter in custom_parameters
                    )
                ):
                    findings.append(finding(
                        "reject", "unsupported-model-custom-bindings",
                        f"「{name}」のModelRendererは、カスタムシェーダーのテクスチャまたはパラメーターを使用しています。",
                        "標準のPBRスロットを使用するか、ポータブル版のカスタムシェーダーバックエンドを追加してください。",
                    ))
            if component_type == "AudioSource":
                if component.get("spatial", False):
                    findings.append(finding(
                        "warning", "web-spatial-audio-approximation",
                        f"「{name}」のAudioSourceで3D空間音響が有効です。",
                        "Web Audioの距離減衰とパンを使用します。ブラウザーのプレビューで、リスナー位置と減衰範囲を確認してください。",
                    ))
                if component.get("streaming", False):
                    findings.append(finding(
                        "warning", "web-audio-buffered-stream",
                        f"「{name}」のAudioSourceでストリーミング再生が有効です。",
                        "基本Webプロファイルでは、パッケージ内の音声を再生前にメモリへデコードします。",
                    ))
            if component_type == "ParticleSystem":
                shape = str(component.get("shape", "Point"))
                if shape not in {"Point", "Cone", "Sphere", "Box"}:
                    findings.append(finding(
                        "warning", "web-particle-shape-approximation",
                        f"「{name}」のParticleSystemはエミッター形状「{shape}」を使用しています。",
                        "基本WebプロファイルではPointエミッターへ置き換えます。プレビューで効果を確認してください。",
                    ))
                if str(component.get("renderMode", "Billboard")) not in {
                    "Billboard", "Horizontal",
                }:
                    findings.append(finding(
                        "reject", "unsupported-particle-render-mode",
                        f"「{name}」のParticleSystemは、未対応の描画モード「{component.get('renderMode')}」を使用しています。",
                        "BillboardまたはHorizontalを使用してください。",
                    ))
                particle_shader = component.get("shader", "")
                auxiliary_texture = component.get("auxiliaryTexture", "")
                custom_parameters = component.get("customParameters", [])
                if (
                    (isinstance(particle_shader, str) and particle_shader)
                    or (
                        isinstance(auxiliary_texture, str)
                        and auxiliary_texture
                    )
                    or (
                        isinstance(custom_parameters, list)
                        and any(
                            isinstance(parameter, list)
                            and any(value != 0 for value in parameter)
                            for parameter in custom_parameters
                        )
                    )
                ):
                    findings.append(finding(
                        "reject", "unsupported-particle-custom-shader",
                        f"「{name}」のParticleSystemは、カスタムシェーダー、補助テクスチャ、または既定値以外のシェーダーパラメーターを使用しています。",
                        "標準のパーティクルマテリアルを使用するか、ポータブル版のシェーダーバックエンドを追加してください。",
                    ))
            if component_type == "BoxCollider3D":
                findings.append(finding(
                    "warning", "web-basic-box-physics",
                    f"「{name}」のBoxCollider3Dには、決定論的なAABB方式のブラウザー物理バックエンドを使用します。",
                    "回転、摩擦の合成方法、連続衝突は近似されます。プレビューでゲームプレイを確認してください。",
                ))
            if component_type == "MeshCollider3D":
                findings.append(finding(
                    "reject", "unsupported-scene-mesh-collider",
                    f"「{name}」のMeshCollider3Dは、ポータブルシーンローダーでモデルから衝突用三角形をまだ生成できません。",
                    "このWebプロファイルではBoxCollider3Dを使用するか、ポータブル版のメッシュコライダー用デコーダーを追加してください。",
                ))
            if component_type == "Rigidbody":
                advanced_rigidbody = (
                    component.get("collisionDetection", "discrete") != "discrete"
                    or component.get("angularDrag", 0.05) != 0.05
                    or component.get("linearDrag", 0.0) != 0.0
                    or component.get("mass", 1.0) != 1.0
                    or component.get("angularVelocity", [0.0, 0.0, 0.0])
                        != [0.0, 0.0, 0.0]
                    or component.get("centerOfMass", [0.0, 0.0, 0.0])
                        != [0.0, 0.0, 0.0]
                    or any(
                        bool(value)
                        for value in (
                            component.get("constraints", {})
                            if isinstance(component.get("constraints", {}), dict)
                            else {}
                        ).values()
                    )
                )
                if advanced_rigidbody:
                    findings.append(finding(
                        "reject", "unsupported-advanced-rigidbody",
                        f"「{name}」のRigidbodyは、基本Web物理バックエンドでまだ保持できない質量、抗力、角速度、重心、拘束、または連続衝突の設定を使用しています。",
                        "速度、重力、キネマティックだけを使うか、これらの設定に対応するポータブル版Rigidbodyソルバーを追加してください。",
                    ))
                if object_value.get("parent") is not None:
                    findings.append(finding(
                        "reject", "unsupported-parented-rigidbody",
                        f"「{name}」のRigidbodyには親がありますが、基本AABBソルバーはルート空間のワールド軸で接触を解決します。",
                        "このRigidbodyをルートGameObjectへ移動するか、親子Transformに対応した物理バックエンドを実装してください。",
                    ))
            if component_type == "InputMover":
                for field, fallback in (
                    ("horizontalAction", "MoveHorizontal"),
                    ("verticalAction", "MoveVertical"),
                ):
                    action = component.get(field, fallback)
                    if action not in available_input_actions:
                        findings.append(finding(
                            "reject", "unsupported-input-action",
                            f"「{name}」のInputMoverは、基本Webバインドにないアクション「{action}」を使用しています。",
                            "対応する入力アクションを使用するか、キーボード、ゲームパッド、タッチの割り当てを追加してください。",
                        ))
            if component_type == "SpriteAnimator":
                columns = component.get("columns", 1)
                rows = component.get("rows", 1)
                clips = component.get("clips", [])
                if (
                    not isinstance(columns, int) or columns < 1
                    or not isinstance(rows, int) or rows < 1
                    or not isinstance(clips, list)
                    or any(
                        not isinstance(clip, dict)
                        or not isinstance(clip.get("name", ""), str)
                        or not clip.get("name", "")
                        or not isinstance(
                            clip.get("frameCount", 1), (int, float)
                        )
                        or clip.get("frameCount", 1) < 1
                        or not isinstance(
                            clip.get("framesPerSecond", 10.0), (int, float)
                        )
                        or clip.get("framesPerSecond", 10.0) <= 0
                        for clip in clips
                    )
                ):
                    findings.append(finding(
                        "reject", "invalid-sprite-animation",
                        f"「{name}」のSpriteAnimatorには、無効なスプライトシートのグリッドまたはクリップ定義があります。",
                        "行数、列数、フレーム数、FPSには正の値を指定し、クリップ名を空にしないでください。",
                    ))
            if component_type == "ParallaxLayer":
                reference = component.get("referenceId", 0)
                if reference not in (0, None):
                    component_links.append((name, "ParallaxLayer", reference))
            if component_type == "SpriteRenderer":
                if component.get("renderTexture"):
                    findings.append(finding(
                        "reject", "unsupported-sprite-render-texture",
                        f"「{name}」のSpriteRendererはrenderTexture「{component.get('renderTexture')}」を表示します。",
                        "画像アセットを使用するか、ポータブル版のレンダーテクスチャ読み込みを追加してください。",
                    ))
                sprite_shader = component.get("shader", "")
                sprite_parameters = component.get("customParameters", [])
                if (
                    (isinstance(sprite_shader, str) and sprite_shader)
                    or (
                        isinstance(sprite_parameters, list)
                        and any(
                            isinstance(parameter, list)
                            and any(value != 0 for value in parameter)
                            for parameter in sprite_parameters
                        )
                    )
                ):
                    findings.append(finding(
                        "reject", "unsupported-sprite-custom-shader",
                        f"「{name}」のSpriteRendererは、カスタムシェーダーまたは既定値以外のパラメーターを使用しています。",
                        "Web版では標準のSpriteマテリアルを使用してください。",
                    ))
            if component_type == "TextRenderer":
                font_asset = component.get("fontAsset", "")
                font_family = str(component.get("fontFamily", "sans-serif"))
                if not font_asset and font_family.lower() not in {
                    "sans-serif", "serif", "monospace", "cursive", "fantasy",
                    "system-ui",
                }:
                    findings.append(finding(
                        "warning", "system-font-may-differ",
                        f"「{name}」のTextRendererは、fontAssetを指定せずにシステムフォント「{font_family}」を使用しています。",
                        "ブラウザー間で字形とレイアウトをそろえるには、TTF、OTF、WOFF、またはWOFF2をfontAssetとしてパッケージへ追加してください。",
                    ))
            if component_type == "TransformAnimator":
                controller = component.get("controller", "")
                if isinstance(controller, str) and controller:
                    findings.append(finding(
                        "reject", "unsupported-transform-animator-controller",
                        f"「{name}」のTransformAnimatorはステートマシンコントローラー「{controller}」を使用しています。",
                        "基本WebプロファイルではLamaPonAnimationClipを直接使用するか、ポータブル版のコントローラーランタイムを追加してください。",
                    ))
            if (
                component_type == "Camera"
                and isinstance(object_id, int)
                and object_value.get("enabled", True)
                and component.get("enabled", True)
            ):
                cameras.add(object_id)
            if component_type == "NativeScript":
                script_id = component.get("script", "")
                if not isinstance(script_id, str) or not script_id:
                    findings.append(finding(
                        "reject", "invalid-scene-script",
                        f"「{name}」のNativeScriptにスクリプトIDがありません。",
                        "登録済みのスクリプトをコンポーネントへ割り当ててください。",
                    ))
                else:
                    scene_scripts.add(script_id)
    for name, parent in parent_links:
        if parent not in object_ids:
            findings.append(finding(
                "reject", "missing-scene-parent",
                f"シーンオブジェクト「{name}」が、存在しない親ID {parent}を参照しています。",
                "シーン階層を修復してください。",
            ))
    for name, component_type, target in component_links:
        if target not in object_ids:
            findings.append(finding(
                "reject", "missing-component-reference",
                f"「{name}」の{component_type}が、存在しないオブジェクトID {target}を参照しています。",
                "Web出力を行う前にコンポーネントの参照を修復してください。",
            ))
    main_camera = scene.get("mainCamera")
    if "renderer3d" in declared_modules and main_camera not in cameras:
        findings.append(finding(
            "reject", "invalid-main-camera",
            "シーンのmainCameraが、有効なCameraコンポーネントを参照していません。",
            "Web出力を行う前に、有効なメインカメラを割り当ててください。",
        ))
    for script_id in sorted(scene_scripts - registered_scripts):
        findings.append(finding(
            "reject", "unregistered-scene-script",
            f"シーンはスクリプト「{script_id}」を参照していますが、選択したWebソースにはLAMAPON_SCRIPT_NAMEDによる登録がありません。",
            "C++ソースをexport.web.sourcesへ追加するか、シーンのスクリプトIDを修正してください。",
        ))

    environment = scene.get("environment", {})
    if isinstance(environment, dict):
        for key, label in WEB_UNSUPPORTED_ENVIRONMENT_EFFECTS.items():
            effect = environment.get(key, {})
            if isinstance(effect, dict) and effect.get("enabled", False):
                findings.append(finding(
                    "warning", "unsupported-environment-effect",
                    f"シーンで{label}が有効ですが、webgl2-basic-3dでは描画できません。",
                    "Web版ではこの効果を自動的に無効にします。配布前にプレビューを確認してください。",
                ))

    # ワールド行列の再帰走査でブラウザーのスタックがあふれる前に、
    # シーン階層の循環を検出します。
    parents = {
        value.get("id"): value.get("parent")
        for value in objects
        if isinstance(value, dict) and isinstance(value.get("id"), int)
    }
    for object_id in parents:
        visited: set[int] = set()
        current: Any = object_id
        while isinstance(current, int) and current in parents:
            if current in visited:
                findings.append(finding(
                    "reject", "cyclic-scene-hierarchy",
                    f"シーン階層のオブジェクトID {current}に親子関係の循環があります。",
                    "Web出力を行う前にシーン階層を修復してください。",
                ))
                break
            visited.add(current)
            current = parents[current]

    if not any(item["level"] == "reject" for item in findings):
        findings.append(finding(
            "auto", "portable-contract-complete",
            f"Web互換性チェックに合格しました（API型: {len(api_locations)}、シーンオブジェクト: {len(objects)}、参照アセット: {len(asset_references)}、登録スクリプト: {len(registered_scripts)}）。",
            "Emscriptenによるコンパイル確認を実行できます。",
        ))
    return findings


def validate_web_compatibility(
    source: Path,
    project: dict[str, Any],
    profile_name: str,
    project_kind: str,
) -> list[dict[str, str]]:
    # パスの別名がある環境でも、包含関係と診断に同じ物理プロジェクトルートを
    # 使用できるよう、開始時に一度だけ正規化します。
    source = source.resolve()
    profile = WEB_PROFILES.get(profile_name)
    if profile is None:
        known = ", ".join(sorted(WEB_PROFILES))
        raise ExportError(
            f"Unknown Web compatibility profile {profile_name!r}; "
            f"known profiles: {known}."
        )

    findings = [
        finding(
            "auto",
            "renderer-backend",
            "WebGL2を自動選択し、非対応環境ではWebGL1へ切り替えます。",
            "ブラウザーのGPU APIを利用できない場合に限り、Canvasのソフトウェア描画を使用します。",
        ),
        finding(
            "auto",
            "input-backend",
            "ブラウザーのキーボード入力バックエンドを自動選択します。",
            "Win32メッセージの代わりにLamaPon::Inputを使用してください。",
        ),
        finding(
            "auto",
            "standard-output-name",
            f"Web出力には標準名「{web_artifact_prefix(source, project, project_kind)}.html」を使用します。",
            "すべてのLamaPon Webゲームで同じ規則を使うため、出力ファイル名はエクスポーターが決定します。",
        ),
    ]
    export = project.get("export", {})
    if not isinstance(export, dict):
        raise ExportError("Project export must be an object when specified.")
    modules = resolved_project_modules(source, project, project_kind)
    if project_kind == "lamapon-project":
        findings.append(
            finding(
                "auto",
                "project-detection",
                "通常のLamaPonプロジェクトとして検出し、3D設定、スクリプト、アセットからWeb用モジュールを推定しました。",
                "プロジェクト設定ファイルは変更していません。",
            )
        )
        findings.append(
            finding(
                "info",
                "source-conversion-policy",
                "ネイティブC++コードやシェーダーを文字列置換では書き換えません。動作を変えるにはWeb用バックエンドが必要です。",
                "ポータブル版のLamaPon APIを使用するか、Web専用の実装を追加してから再出力してください。",
            )
        )
    allowed_modules = profile["modules"]
    unsupported_modules = sorted(set(modules) - allowed_modules)
    if unsupported_modules:
        reasons = profile["module_reasons"]
        for module in unsupported_modules:
            findings.append(
                finding(
                    "reject",
                    "unsupported-module",
                    f"モジュール「{module}」はWebプロファイル{profile_name!r}で使用できません: {reasons.get(module, 'このプロファイルに定義がありません')}",
                    "このモジュールのWeb用バックエンドを追加するか、Webターゲットから該当機能を削除してください。",
                )
            )

    web = export.get("web", {})
    if not isinstance(web, dict):
        raise ExportError("Project export.web must be an object when specified.")
    findings.extend(validate_portable_contract(
        source,
        project,
        project_kind,
        web,
        modules,
    ))
    excluded_values = web.get("scanExcludePaths", [])
    if not isinstance(excluded_values, list) or any(
        not isinstance(item, str) or not item for item in excluded_values
    ):
        raise ExportError("export.web.scanExcludePaths must be a string list.")
    excluded = [(source / item).resolve() for item in excluded_values]
    source_hits: list[str] = []
    warnings: list[dict[str, str]] = []
    automatic_exclusions: list[Path] = []
    ignored_directories = {".git", ".lamapon", "CMakeFiles", "build", "third_party"}
    for path in source.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in SOURCE_EXTENSIONS:
            continue
        if any(part in ignored_directories for part in path.relative_to(source).parts):
            continue
        resolved = path.resolve()
        if path.name.lower() in DEFAULT_PLATFORM_SOURCE_NAMES:
            automatic_exclusions.append(path.relative_to(source))
            continue
        if source_is_excluded(resolved, excluded):
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        for line_number, line in enumerate(lines, start=1):
            for token, description in FORBIDDEN_SOURCE_TOKENS.items():
                if token in line:
                    if token == "DirectX::" and "DirectX::" not in (
                        PORTABLE_DIRECTX_TOKEN.sub("", line)
                    ):
                        continue
                    source_hits.append(
                        f"{path.relative_to(source)}:{line_number}: "
                        f"{description} ({token})"
                    )
            for token, message in SOURCE_WARNINGS.items():
                if token in line:
                    warnings.append(
                        finding(
                            "warning",
                            "browser-runtime-difference",
                            f"{path.relative_to(source)}:{line_number}: {message}",
                            "可能な場合はLamaPonのプラットフォームサービスを使用してください。",
                        )
                    )
    if source_hits:
        for hit in source_hits[:32]:
            findings.append(
                finding(
                    "reject",
                    "native-source-dependency",
                    f"Webソースがネイティブ専用の依存関係を使用しています: {hit}",
                    "ポータブル版のLamaPon APIへ置き換えるか、ターゲット専用バックエンドの内部へ移してください。",
                )
            )
        if len(source_hits) > 32:
            findings.append(
                finding(
                    "reject",
                    "native-source-dependency-truncated",
                    f"ほかに{len(source_hits) - 32}件のネイティブ専用依存関係が見つかりました。",
                    "Web出力の診断にあるソース検査結果を開き、残りの依存関係も移植してください。",
                )
            )
    findings.extend(warnings)
    if automatic_exclusions:
        names = ", ".join(str(path) for path in automatic_exclusions)
        findings.append(
            finding(
                "auto",
                "platform-entrypoint",
                f"Web版ではWindows用エントリーポイントを除外します: {names}。",
                "ブラウザー用エントリーポイントはWebバックエンドが提供します。",
            )
        )

    asset_selection = web_asset_roots(source, web)
    if asset_selection is None:
        return findings
    asset_directory, included_asset_roots = asset_selection
    supported_extensions = profile["asset_extensions"]
    asset_files: list[Path] = []
    for root in included_asset_roots:
        if root.is_file():
            asset_files.append(root)
        else:
            asset_files.extend(path for path in root.rglob("*") if path.is_file())
    asset_files = list(dict.fromkeys(path.resolve() for path in asset_files))
    for path in asset_files:
        if (
            path.suffix.lower() in supported_extensions
            and path.suffix.lower() != ".txt"
        ):
            findings.extend(validate_asset_integrity(path, source))
    convertible_assets = [
        path for path in asset_files
        if path.suffix.lower() in WEB_ASSET_CONVERSIONS
    ]
    conversion_groups: dict[tuple[str, str], list[Path]] = {}
    for path in convertible_assets:
        kind, runtime_format = WEB_ASSET_CONVERSIONS[path.suffix.lower()]
        conversion_groups.setdefault((kind, runtime_format), []).append(path)
        if path.stat().st_size == 0:
            findings.append(finding(
                "reject", "empty-convertible-asset",
                f"アセット「{path.relative_to(source)}」が空のため変換できません。",
                "空のアセットを置き換えるか削除してください。",
            ))
    for (kind, runtime_format), paths in sorted(conversion_groups.items()):
        converter = web_asset_converter(web, kind)
        kind_label = WEB_ASSET_KIND_LABELS.get(kind, kind)
        extensions = ", ".join(sorted({path.suffix.lower() for path in paths}))
        if converter is None:
            setting_name, _ = WEB_ASSET_CONVERTER_SETTINGS[kind]
            findings.append(finding(
                "reject", "missing-asset-converter",
                f"{len(paths)}件の{kind_label}アセット（{extensions}）を{runtime_format.upper()}へ変換する必要がありますが、変換ツールがありません。",
                f"Hubの{kind_label}変換モジュールをインストールするか、export.web.converterTools.{setting_name}を設定してください。",
            ))
        else:
            findings.append(finding(
                "auto", "asset-format-conversion",
                f"{len(paths)}件の{kind_label}アセット（{extensions}）を「{converter.name}」でWeb用の{runtime_format.upper()}へ変換します。",
                "元のプロジェクトファイルと仮想アセットパスは変更しません。",
            ))
    unsupported_assets = [
        path.relative_to(source)
        for path in asset_files
        if path.is_file()
        and path.suffix.lower() not in supported_extensions
        and path.suffix.lower() not in WEB_ASSET_CONVERSIONS
        and path.suffix.lower() not in IGNORED_RUNTIME_ASSET_EXTENSIONS
        and not (
            path.suffix.lower() in {".hlsl", ".hlsli"}
            and path.name.lower() == "lamaponlit.hlsl"
        )
    ]
    if unsupported_assets:
        by_extension: dict[str, int] = {}
        for path in unsupported_assets:
            extension = path.suffix.lower() or "(no extension)"
            by_extension[extension] = by_extension.get(extension, 0) + 1
        for extension, count in sorted(by_extension.items()):
            findings.append(
                finding(
                    "reject",
                    "unsupported-asset-format",
                    f"{count}件のアセットが、未対応のWeb形式「{extension}」を使用しています。",
                    "アセットを変換するか、Web用アセットバックエンドを追加してから出力してください。",
                )
            )
    findings.append(
        finding(
            "auto",
            "asset-packaging",
            "ポータブル版のJSONと画像アセットをWebパッケージへコピーします。",
            "ブラウザー用パッケージでは仮想アセットパスを使用します。",
        )
    )
    if project_kind == "lamapon-project":
        findings.append(finding(
            "auto",
            "generated-web-runtime-target",
            "通常のLamaPonプロジェクトを変更せず、ポータブル版のEmscriptenターゲットを生成します。",
            "生成したターゲット、変換済みアセット、出力は、指定したWebビルド／出力フォルダー内に保存します。",
        ))
    return findings


def print_compatibility_report(findings: list[dict[str, str]]) -> None:
    print("Web compatibility report:")
    for item in findings:
        print(f"  [{item['level'].upper()}] {item['message']}")
        print(f"           {item['action']}")


def is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def write_compatibility_report(
    output_directory: Path,
    project: dict[str, Any],
    project_kind: str,
    profile_name: str,
    status: str,
    findings: list[dict[str, str]],
) -> Path:
    output_directory.mkdir(parents=True, exist_ok=True)
    report_path = output_directory / "web-compatibility-report.json"
    summary = {
        level: sum(item["level"] == level for item in findings)
        for level in ("auto", "info", "warning", "reject")
    }
    export = project.get("export", {})
    web = export.get("web", {}) if isinstance(export, dict) else {}
    strict_portable_contract = (
        project_kind in {"lamapon-web-target", "lamapon-project"}
        and isinstance(web, dict)
        and web.get("portableGame", False) is True
    )
    report_path.write_text(
        json.dumps(
            {
                "format": "lamapon.web-compatibility-report",
                "version": 2,
                "projectType": project_kind,
                "projectName": project.get(
                    "gameName",
                    project.get("name", "UnnamedProject"),
                ),
                "profile": profile_name,
                "status": status,
                "strictPortableContract": strict_portable_contract,
                "summary": summary,
                "findings": findings,
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return report_path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_fingerprint(source: Path) -> str:
    """ビルド／キャッシュ出力を除外し、ポータブルソースとアセットをハッシュ化します。"""
    digest = hashlib.sha256()
    ignored_directories = {
        ".git",
        ".lamapon",
        "CMakeFiles",
        "build",
        "third_party",
    }
    paths = [
        path
        for path in source.rglob("*")
        if path.is_file()
        and not any(
            part in ignored_directories
            for part in path.relative_to(source).parts
        )
    ]
    for path in sorted(paths, key=lambda item: item.as_posix()):
        relative = path.relative_to(source).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "little"))
        digest.update(relative)
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    return digest.hexdigest()


def require_relative_file(
    source: Path,
    value: str,
    setting_name: str,
) -> Path:
    if not isinstance(value, str) or not value:
        raise ExportError(f"{setting_name} must be a non-empty path string.")
    path = (source / value).resolve()
    if not is_within(path, source):
        raise ExportError(f"{setting_name} must stay inside the Web source.")
    if not path.is_file():
        raise ExportError(f"{setting_name} was not found: {path}")
    return path


def cmake_bracket(value: Path | str) -> str:
    if isinstance(value, Path):
        # CMakeの角括弧引数はバックスラッシュを保持しますが、値はadd_executable
        # などで再度解析されます。Windowsのドライブパスが2回目の解析で\U形式の
        # エスケープにならないよう、スラッシュ区切りに変換します。
        value = value.as_posix()
    return f"[==[{value}]==]"


def stage_portable_web_assets(
    source: Path,
    web: dict[str, Any],
    profile_name: str,
    generated_directory: Path,
) -> Path | None:
    asset_value = web.get("assetDirectory")
    if asset_value is None:
        return None
    if not isinstance(asset_value, str) or not asset_value:
        raise ExportError("export.web.assetDirectory must be a path string.")
    asset_directory = (source / asset_value).resolve()
    if not asset_directory.is_dir():
        raise ExportError(f"Web asset directory was not found: {asset_directory}")
    include_values = web.get("assetIncludePaths", ["."])
    if not isinstance(include_values, list) or not include_values or any(
        not isinstance(item, str) or not item for item in include_values
    ):
        raise ExportError("export.web.assetIncludePaths must be a string list.")
    supported = WEB_PROFILES[profile_name]["asset_extensions"]
    staging = generated_directory.parent / "web-generated-assets"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    copied = 0
    converted: list[dict[str, Any]] = []
    seen_candidates: set[Path] = set()
    for include_value in include_values:
        include_path = (asset_directory / include_value).resolve()
        if not is_within(include_path, asset_directory) or not include_path.exists():
            raise ExportError(
                f"Included Web asset path is invalid: {include_path}"
            )
        candidates = (
            [include_path]
            if include_path.is_file()
            else [path for path in include_path.rglob("*") if path.is_file()]
        )
        for candidate in candidates:
            candidate = candidate.resolve()
            if candidate in seen_candidates:
                continue
            seen_candidates.add(candidate)
            extension = candidate.suffix.lower()
            conversion = WEB_ASSET_CONVERSIONS.get(extension)
            if extension not in supported and conversion is None:
                continue
            destination = staging / candidate.relative_to(asset_directory)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if conversion is None:
                shutil.copy2(candidate, destination)
            else:
                kind, runtime_format = conversion
                converter = web_asset_converter(web, kind)
                if converter is None:
                    setting_name, _ = WEB_ASSET_CONVERTER_SETTINGS[kind]
                    raise ExportError(
                        f"Asset '{candidate.relative_to(source)}' requires the "
                        f"Hub {kind} converter. Configure "
                        f"export.web.converterTools.{setting_name}."
                    )
                run_asset_conversion(
                    candidate,
                    destination,
                    kind,
                    runtime_format,
                    converter,
                )
                generated_model_images: list[Path] = []
                model_details: dict[str, int] | None = None
                if kind == "model":
                    image_converter = web_asset_converter(web, "image")
                    generated_model_images = externalize_glb_images(
                        destination,
                        image_converter,
                    )
                    model_details = validate_portable_glb(destination)
                conversion_entry: dict[str, Any] = {
                    "path": candidate.relative_to(asset_directory).as_posix(),
                    "sourceFormat": extension.removeprefix("."),
                    "runtimeFormat": runtime_format,
                    "converter": converter.name,
                }
                if model_details is not None:
                    conversion_entry["details"] = model_details
                converted.append(conversion_entry)
                for generated_image in generated_model_images:
                    converted.append({
                        "path": generated_image.relative_to(staging).as_posix(),
                        "sourceFormat": "embedded-model-image",
                        "runtimeFormat": "webp",
                        "converter": image_converter.name,
                    })
            copied += 1
    if copied == 0:
        raise ExportError("No portable Web assets were selected for packaging.")
    if converted:
        (staging / "lamapon-asset-conversions.json").write_text(
            json.dumps(
                {
                    "format": "lamapon.web-asset-conversions",
                    "version": 1,
                    "assets": converted,
                },
                ensure_ascii=False,
                indent=2,
            ) + "\n",
            encoding="utf-8",
        )
    input_actions = portable_project_input_actions(source)
    if input_actions:
        portable_actions = {
            name: [
                binding for binding in bindings
                if binding.get("control") in PORTABLE_INPUT_CONTROLS
            ]
            for name, bindings in input_actions.items()
        }
        (staging / "lamapon-input-actions.json").write_text(
            json.dumps(
                {
                    "format": "lamapon.web-input-actions",
                    "version": 1,
                    "actions": portable_actions,
                },
                ensure_ascii=False,
                indent=2,
            ) + "\n",
            encoding="utf-8",
        )
    return staging


def generate_lamapon_web_target(
    source: Path,
    project: dict[str, Any],
    target: str,
    modules: list[str],
    profile_name: str,
    generated_directory: Path,
) -> Path:
    source = source.resolve()
    export = project.get("export", {})
    if not isinstance(export, dict):
        raise ExportError("Project export must be an object when specified.")
    web = export.get("web", {})
    if not isinstance(web, dict):
        raise ExportError("Project export.web must be an object when specified.")

    source_values = web.get("sources", ["main.cpp"])
    if not isinstance(source_values, list) or not source_values:
        raise ExportError("export.web.sources must be a non-empty string list.")
    if any(not isinstance(item, str) or not item for item in source_values):
        raise ExportError("Every export.web.sources entry must be a path string.")
    source_files = [
        require_relative_file(source, item, "export.web.sources entry")
        for item in source_values
    ]
    if any(path.suffix.lower() not in SOURCE_EXTENSIONS for path in source_files):
        raise ExportError("export.web.sources contains a non-C/C++ source file.")

    shell_value = web.get("shellFile")
    shell_file = (
        require_relative_file(source, shell_value, "export.web.shellFile")
        if shell_value is not None
        else ENGINE_ROOT / "src" / "LamaPon" / "Web" / "default-shell.html"
    )
    if not shell_file.is_file():
        raise ExportError(f"Default Web shell was not found: {shell_file}")

    asset_directory = stage_portable_web_assets(
        source,
        web,
        profile_name,
        generated_directory,
    )

    output_name = web_artifact_prefix(source, project, "lamapon-web-target")
    single_file = web.get("singleFile", True)
    if not isinstance(single_file, bool):
        raise ExportError("export.web.singleFile must be a boolean when specified.")
    portable_game = web.get("portableGame", False)
    if not isinstance(portable_game, bool):
        raise ExportError("export.web.portableGame must be a boolean when specified.")
    game_name = project.get("gameName", project.get("name", target))
    if not isinstance(game_name, str) or not game_name:
        raise ExportError("The Web game name must be a non-empty string.")
    scene_path = web.get("scenePath", "/assets/scenes/Main.scene.json")
    if not isinstance(scene_path, str) or not scene_path.startswith("/assets/"):
        raise ExportError("export.web.scenePath must begin with '/assets/'.")

    generated_directory.mkdir(parents=True, exist_ok=True)
    # CMakeは1つのSOURCESキーワードで複数の値を受け取れます。生成ファイルの
    # 診断を読みやすくするため、1つのブロックへまとめます。
    source_lines = "\n".join(
        ["    SOURCES"]
        + [f"        {cmake_bracket(path)}" for path in source_files]
    )
    module_lines = "\n".join(
        ["    MODULES"] + [f"        {module}" for module in modules]
    )
    single_file_line = "    SINGLE_FILE\n" if single_file else ""
    portable_lines = (
        "\n".join([
            "    PORTABLE_GAME",
            f"    GAME_NAME {cmake_bracket(game_name)}",
            f"    SCENE_PATH {cmake_bracket(scene_path)}",
        ])
        if portable_game
        else ""
    )
    asset_line = (
        f"    ASSET_DIRECTORY {cmake_bracket(asset_directory)}"
        if asset_directory is not None
        else ""
    )
    cmake_path = generated_directory / "CMakeLists.txt"
    cmake_path.write_text(
        "\n".join([
            "cmake_minimum_required(VERSION 3.25)",
            f"project({target} LANGUAGES CXX)",
            "",
            f"include({cmake_bracket(ENGINE_ROOT / 'cmake' / 'LamaPonWeb.cmake')})",
            "",
            f"lamapon_add_web_game({target}",
            source_lines,
            module_lines,
            f"    SHELL_FILE {cmake_bracket(shell_file)}",
            asset_line,
            f"    OUTPUT_NAME {cmake_bracket(output_name)}",
            portable_lines,
            single_file_line.rstrip(),
            ")",
            "",
        ]),
        encoding="utf-8",
    )
    return generated_directory


def verify_web_artifacts(
    artifacts: list[Path],
    single_file: bool,
    expected_prefix: str | None = None,
) -> list[dict[str, str]]:
    checks: list[dict[str, str]] = []
    html_files = [path for path in artifacts if path.suffix == ".html"]
    if not html_files:
        raise ExportError("The Web package has no browser HTML entrypoint.")
    if any(path.stat().st_size == 0 for path in artifacts):
        raise ExportError("The Web package contains an empty artifact.")
    checks.append({
        "code": "non-empty-artifacts",
        "status": "passed",
        "message": "Every generated Web artifact is non-empty.",
    })

    for html_path in html_files:
        html = html_path.read_text(encoding="utf-8", errors="replace")
        lowered = html.lower()
        if re.search(r"\{\{\{\s*SCRIPT\s*\}\}\}", html):
            raise ExportError(
                f"The HTML shell still contains an unexpanded template: "
                f"{html_path}"
            )
        if "<canvas" not in lowered or "<script" not in lowered:
            raise ExportError(
                f"The HTML entrypoint has no Canvas/script runtime: {html_path}"
            )
    checks.append({
        "code": "html-entrypoint",
        "status": "passed",
        "message": "HTML shells are expanded and contain Canvas/script runtime.",
    })
    if expected_prefix is not None:
        expected_name = f"{expected_prefix}.html"
        if len(html_files) != 1 or html_files[0].name != expected_name:
            names = ", ".join(path.name for path in html_files)
            raise ExportError(
                f"Web package must contain exactly '{expected_name}', but "
                f"found: {names}"
            )
        checks.append({
            "code": "standard-output-name",
            "status": "passed",
            "message": f"Browser entrypoint uses '{expected_name}'.",
        })

    external_runtime = [
        path for path in artifacts if path.suffix in {".js", ".wasm", ".data"}
    ]
    if single_file and external_runtime:
        names = ", ".join(path.name for path in external_runtime)
        raise ExportError(
            "singleFile output unexpectedly depends on external runtime "
            f"artifacts: {names}"
        )
    if single_file:
        checks.append({
            "code": "self-contained-html",
            "status": "passed",
            "message": "JavaScript, WebAssembly, and packaged assets are embedded.",
        })
    else:
        suffixes = {path.suffix for path in artifacts}
        if ".js" not in suffixes or ".wasm" not in suffixes:
            raise ExportError(
                "Multi-file Web output requires both JavaScript and Wasm."
            )
        checks.append({
            "code": "multi-file-runtime",
            "status": "passed",
            "message": "HTML, JavaScript, and WebAssembly artifacts are present.",
        })
    return checks


def write_export_manifest(
    output_directory: Path,
    artifacts: list[Path],
    source: Path,
    project: dict[str, Any],
    project_kind: str,
    profile_name: str,
    renderer: str,
    modules: list[str],
    target: str,
    build_type: str,
    single_file: bool,
    verification: list[dict[str, str]],
) -> Path:
    manifest_path = output_directory / "web-export-manifest.json"
    manifest_path.write_text(
        json.dumps(
            {
                "format": "lamapon.web-export-manifest",
                "version": 1,
                "createdAtUtc": datetime.now(UTC).isoformat(),
                "project": {
                    "name": project.get(
                        "gameName",
                        project.get("name", "UnnamedProject"),
                    ),
                    "type": project_kind,
                    "sourceFingerprintSha256": source_fingerprint(source),
                },
                "target": {
                    "name": target,
                    "renderer": renderer,
                    "profile": profile_name,
                    "modules": modules,
                    "buildType": build_type,
                    "singleFile": single_file,
                },
                "verification": {
                    "status": "passed",
                    "checks": verification,
                },
                "artifacts": [
                    {
                        "path": path.relative_to(output_directory).as_posix(),
                        "bytes": path.stat().st_size,
                        "sha256": sha256_file(path),
                    }
                    for path in sorted(
                        (item for item in artifacts if item.is_file()),
                        key=lambda item: item.as_posix(),
                    )
                ],
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    return manifest_path


def web_license_bundle() -> str:
    """ソースツリーと配布SDKのどちらでも同じ著作権・許諾本文を読む。"""
    sources = {
        "LamaPon": "LICENSE",
        "nlohmann-json": "third_party/nlohmann/LICENSE.MIT",
        "cgltf": "third_party/cgltf/LICENSE.txt",
    }
    sections = []
    for name, relative in sources.items():
        path = ENGINE_ROOT / relative
        if not path.is_file():
            path = ENGINE_ROOT / "licenses" / f"{name}.txt"
        if not path.is_file():
            raise ExportError(f"Web runtime license text is missing: {name}")
        sections.append(name + "\n\n" + path.read_text(encoding="utf-8").strip())
    return "\n\n".join(sections)


def copy_web_package(
    build_directory: Path,
    output_directory: Path,
    artifact_prefix: str,
    profile_name: str,
    findings: list[dict[str, str]],
    project: dict[str, Any],
    project_kind: str,
    single_file: bool,
) -> tuple[list[Path], list[dict[str, str]]]:
    build_directory = build_directory.resolve()
    output_directory = output_directory.resolve()
    # 単一HTMLを共有しても許諾本文が失われないよう、本体へ埋め込む。
    # 文字列をHTMLとして解釈させず、ゲーム画面には表示しない。
    license_notice = ('\n<pre id="lamapon-licenses" hidden>'
                      + html.escape(web_license_bundle()) + '</pre>\n')
    output_directory.mkdir(parents=True, exist_ok=True)
    all_artifact_extensions = {
        ".html",
        ".js",
        ".wasm",
        ".data",
        ".mem",
        ".symbols",
        ".map",
    }
    artifact_extensions = {".html"} if single_file else all_artifact_extensions
    copied: list[Path] = []
    artifacts_found = False
    build_artifact_names = {
        candidate.name
        for candidate in build_directory.rglob("*")
        if candidate.is_file()
        and candidate.name.startswith(artifact_prefix)
        and candidate.suffix in artifact_extensions
    }
    build_artifacts = [
        candidate
        for candidate in build_directory.rglob("*")
        if candidate.is_file()
        and candidate.name.startswith(artifact_prefix)
        and candidate.suffix in artifact_extensions
    ]
    verification = verify_web_artifacts(
        build_artifacts,
        single_file,
        artifact_prefix,
    )
    previous_manifest = output_directory / "web-export-manifest.json"
    if previous_manifest.is_file():
        try:
            previous_artifacts = json.loads(
                previous_manifest.read_text(encoding="utf-8")
            ).get("artifacts", [])
        except (json.JSONDecodeError, OSError, AttributeError):
            previous_artifacts = []
        for item in previous_artifacts:
            if not isinstance(item, dict) or not isinstance(item.get("path"), str):
                continue
            previous_path = (output_directory / item["path"]).resolve()
            if (
                is_within(previous_path, output_directory)
                and previous_path.is_file()
                and previous_path.suffix in all_artifact_extensions
                and previous_path.name not in build_artifact_names
            ):
                previous_path.unlink()
    # プロファイル変更で古い.js／.data／.wasm成果物が不要になる場合があります。
    # たとえばSINGLE_FILEではHTMLへ埋め込まれます。利用者がHTMLを直接開いた際に
    # 読み込むランタイムが曖昧にならないよう、新しいパッケージの隣に古いファイルを残しません。
    for existing in output_directory.iterdir():
        if (
            existing.is_file()
            and existing.name.startswith(artifact_prefix)
            and existing.suffix in all_artifact_extensions
            and existing.name not in build_artifact_names
        ):
            existing.unlink()
    for candidate in build_directory.rglob("*"):
        if not candidate.is_file():
            continue
        if candidate.name == "project.json":
            destination = output_directory / candidate.name
        elif (
            candidate.name.startswith(artifact_prefix)
            and candidate.suffix in artifact_extensions
        ):
            destination = output_directory / candidate.name
            artifacts_found = True
        else:
            continue
        shutil.copy2(candidate, destination)
        if destination.suffix == ".html":
            content = destination.read_text(encoding="utf-8")
            closing_body = content.lower().rfind("</body>")
            if closing_body >= 0:
                content = content[:closing_body] + license_notice + content[closing_body:]
            else:
                content += license_notice
            destination.write_text(content, encoding="utf-8")
        copied.append(destination)

    assets = build_directory / "assets"
    if assets.is_dir():
        destination = output_directory / "assets"
        shutil.copytree(assets, destination, dirs_exist_ok=True)
        copied.append(destination)
    if not artifacts_found:
        raise ExportError(
            "The Web build completed but no HTML/JavaScript/Wasm artifacts "
            f"were found in {build_directory}."
        )
    report_path = write_compatibility_report(
        output_directory,
        project,
        project_kind,
        profile_name,
        "ok",
        findings,
    )
    copied.append(report_path)
    return copied, verification


def run_command(command: list[str], cwd: Path, dry_run: bool) -> None:
    # エディターはSDKのPythonエントリーを直接指定します。Windowsのbatを
    # 経由せず、空白・日本語・シェルの特殊文字を引数のまま渡します。
    if Path(command[0]).name in {"emcmake", "emcmake.py"} and Path(command[0]).is_file():
        command = [sys.executable, *command]
    print(f"$ {shlex.join(command)}")
    if not dry_run:
        subprocess.run(command, cwd=cwd, check=True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build a LamaPon project for WebGL2 with Emscripten and "
            "collect its browser package."
        )
    )
    parser.add_argument("project", type=Path, help="Path to project.json")
    parser.add_argument(
        "--output",
        type=Path,
        help=(
            "Directory for HTML, JavaScript, Wasm, and assets "
            "(defaults to project/.lamapon/web)"
        ),
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="CMake build directory (defaults to project/.lamapon/web-build)",
    )
    parser.add_argument(
        "--generator",
        help="CMake generator, for example Ninja",
    )
    parser.add_argument(
        "--build-type",
        choices=("Debug", "Release"),
        default="Release",
    )
    parser.add_argument(
        "--emcmake",
        help="Path to emcmake (defaults to PATH lookup)",
    )
    parser.add_argument(
        "--cmake",
        help="Path to cmake (defaults to PATH lookup)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Validate the project and print commands without running them",
    )
    parser.add_argument(
        "--report-only",
        action="store_true",
        help="Write a compatibility report without running a Web build",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    project_path = arguments.project.resolve()
    _, project = load_project(project_path)
    project_root = lamapon_project_root(project_path, project)
    source, target, renderer, profile, project_kind = require_web_configuration(
        project_path,
        project_root,
        project,
    )
    findings = validate_web_compatibility(
        source,
        project,
        profile,
        project_kind,
    )
    modules = resolved_project_modules(source, project, project_kind)
    output_directory = (
        arguments.output.resolve()
        if arguments.output
        else (project_root / ".lamapon" / "web").resolve()
    )
    build_directory = (
        arguments.build_dir.resolve()
        if arguments.build_dir
        else project_root / ".lamapon" / (
            "web-build-generated"
            if project_kind == "lamapon-web-target"
            else "web-build"
        )
    )
    if output_directory == build_directory or is_within(
        output_directory,
        build_directory,
    ):
        raise ExportError(
            "Web output must not be inside the CMake build directory."
        )
    if output_directory == source:
        raise ExportError("Web output must not replace the source directory.")

    print(f"Web renderer: {renderer}")
    print(f"Web compatibility profile: {profile}")
    print(f"Web source: {source}")
    print(f"Web output: {output_directory}")
    print_compatibility_report(findings)

    if any(item["level"] == "reject" for item in findings):
        if arguments.dry_run:
            print("Web export rejected; dry-run did not write a report.")
            return 2
        report_path = write_compatibility_report(
            output_directory,
            project,
            project_kind,
            profile,
            "rejected",
            findings,
        )
        print(f"Web export rejected; report written to {report_path}")
        return 2

    if arguments.report_only:
        report_path = write_compatibility_report(
            output_directory,
            project,
            project_kind,
            profile,
            "ready",
            findings,
        )
        print(f"Compatibility report written to {report_path}")
        return 0

    if project_kind not in {"cmake", "lamapon-web-target", "lamapon-project"}:
        raise ExportError(
            "This project has no Web CMake target. Use --report-only after "
            "adding a Web runtime target."
        )

    export = project.get("export", {})
    if not isinstance(export, dict):
        raise ExportError("Project export must be an object when specified.")
    web = export.get("web", {})
    if not isinstance(web, dict):
        raise ExportError("Project export.web must be an object when specified.")
    artifact_prefix = web_artifact_prefix(source, project, project_kind)
    single_file = web.get("singleFile", False)
    if not isinstance(single_file, bool):
        raise ExportError("export.web.singleFile must be a boolean when specified.")

    emcmake = arguments.emcmake or shutil.which("emcmake")
    cmake = arguments.cmake or shutil.which("cmake")
    if not arguments.dry_run and not emcmake:
        raise ExportError(
            "emcmake was not found. Activate the Emscripten SDK or pass "
            "--emcmake."
        )
    if not arguments.dry_run and not cmake:
        raise ExportError(
            "cmake was not found. Install CMake or pass --cmake."
        )

    emcmake_command = emcmake or "emcmake"
    cmake_command = cmake or "cmake"
    cmake_source = source
    if project_kind in {"lamapon-web-target", "lamapon-project"}:
        cmake_source = generate_lamapon_web_target(
            source,
            project,
            target,
            modules,
            profile,
            build_directory.parent / "web-generated-cmake",
        )
    configure = [
        emcmake_command,
        cmake_command,
        "-S",
        str(cmake_source),
        "-B",
        str(build_directory),
        f"-DCMAKE_BUILD_TYPE={arguments.build_type}",
        f"-DLAMAPON_WEB_REQUESTED_MODULES={';'.join(modules)}",
        f"-DLAMAPON_WEB_OUTPUT_NAME={artifact_prefix}",
    ]
    if arguments.generator:
        configure.extend(("-G", arguments.generator))
    build = [
        cmake_command,
        "--build",
        str(build_directory),
        "--config",
        arguments.build_type,
        "--target",
        target,
    ]

    run_command(configure, project_root, arguments.dry_run)
    run_command(build, project_root, arguments.dry_run)
    if arguments.dry_run:
        return 0

    copied, verification = copy_web_package(
        build_directory,
        output_directory,
        artifact_prefix,
        profile,
        findings,
        project,
        project_kind,
        single_file,
    )
    manifest_path = write_export_manifest(
        output_directory,
        copied,
        source,
        project,
        project_kind,
        profile,
        renderer,
        modules,
        target,
        arguments.build_type,
        single_file,
        verification,
    )
    copied.append(manifest_path)
    print("Web package:")
    for path in copied:
        print(f"  {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ExportError as error:
        print(f"Web export failed: {error}", file=sys.stderr)
        raise SystemExit(2) from error
    except subprocess.CalledProcessError as error:
        print(
            f"Web export command failed with exit code {error.returncode}.",
            file=sys.stderr,
        )
        raise SystemExit(error.returncode or 1) from error
