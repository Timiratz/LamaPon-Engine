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


# repository root.
ENGINE_ROOT = Path(__file__).resolve().parent.parent


class ExportError(RuntimeError):
    """利用者が対処できるWeb出力設定またはビルドエラーです。"""


# Web profileと制約。
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

# 既定module。
DEFAULT_MODULES = ["core", "renderer2d", "input"]
# 除外するplatform source名。
DEFAULT_PLATFORM_SOURCE_NAMES = {
    "winmain.cpp",
    "windowsmain.cpp",
    "win32main.cpp",
    "windowsplatform.cpp",
    "win32platform.cpp",
}

# 解析するsource拡張子。
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

# 禁止API token。
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

# portable版で許可するDirectX API。
PORTABLE_DIRECTX_TOKEN = re.compile(
    r"DirectX::(?:XMFLOAT2|XMFLOAT3|XMFLOAT4|XMFLOAT4X4|XMMATRIX|"
    r"XMStoreFloat4x4|XM_PI)"
)

# source検査の警告。
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

# API typeと必要module。
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

# scene componentと必要module。
PORTABLE_SCENE_COMPONENTS: dict[str, str] = {
    "NativeScript": "core",
    "Camera": "renderer3d",
    # AudioListenerは不要で、DirectionalLightはscene環境の既定値を使います。
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

# 処理のないcomponent。
PORTABLE_NOOP_SCENE_COMPONENTS = {"AudioListener", "RenderCulling"}
# 近似対応component。
PORTABLE_APPROXIMATE_SCENE_COMPONENTS = {"DirectionalLight"}

# 新しいnative componentはWeb互換を宣言する前に両registryへ分類します。
# Web判定対象のcomponent。
KNOWN_NATIVE_SCENE_COMPONENTS = {
    "AudioListener", "AudioSource", "Billboard", "Blink2D", "BoxCollider2D",
    "BoxCollider3D", "Camera", "CapsuleCollider3D", "CharacterController",
    "CircleCollider2D", "ConvexHullCollider3D", "DirectionalLight",
    "InputMover", "Joint", "Keyform2D", "LODGroup", "Light2D", "MeshCollider3D",
    "MeshRenderer", "ModelRenderer", "NativeScript", "NetworkIdentity", "NavMesh",
    "NavMeshAgent", "ParallaxLayer", "ParticleSystem", "PointLight",
    "PolygonCollider2D", "ReflectionProbe", "RenderCulling", "Rig2D", "Rigidbody",
    "Rotator", "SphereCollider3D", "SpotLight", "SpriteAnimator",
    "SpriteMask", "SpriteParticles2D", "SpriteRenderer", "SpriteSkin2D", "Sway2D",
    "TextRenderer",
    "Tilemap", "TransformAnimator", "UIButton", "UICanvas", "UIImage",
    "UIInputField", "UILayoutGroup", "UIRectTransform", "UIScrollView",
    "UISlider", "UIToggle",
}

# 2Dキャラクター部品のAPI typeと必要module。
PORTABLE_API_MODULES.update({
    "Blink2DComponent": "renderer2d",
    "Blink2DSettings": "renderer2d",
    "Keyform2DChannel": "core",
    "Keyform2DComponent": "core",
    "Keyform2DKey": "core",
    "Keyform2DPose": "core",
    "Rig2DComponent": "core",
    "Rig2DParameter": "core",
    "SpriteMeshDeformer": "renderer2d",
    "SpriteSkin2DComponent": "renderer2d",
    "SpriteSkinWeight": "renderer2d",
    "Sway2DComponent": "core",
    "Sway2DSettings": "core",
})
# 2Dキャラクター部品のscene componentと必要module。
PORTABLE_SCENE_COMPONENTS.update({
    "Blink2D": "renderer2d",
    "Keyform2D": "core",
    "Rig2D": "core",
    "SpriteSkin2D": "renderer2d",
    "Sway2D": "core",
})

# API名の検索pattern.
LAMAPON_API_TOKEN = re.compile(r"\bLamaPon::([A-Za-z_][A-Za-z0-9_]*)")
# asset path検索pattern.
ASSET_PATH_TOKEN = re.compile(
    r"(?:std::filesystem::path\s*)?\{?\s*"
    r'"((?:textures|audio|scenes|models|fonts)/[^"\r\n]+)"'
)
# 静的script登録pattern.
SCRIPT_REGISTRATION_TOKEN = re.compile(
    r"LAMAPON_SCRIPT_(?:NAMED|WITH_SCHEMA)\s*\(\s*[^,]+,\s*\"([^\"]+)\"",
    re.MULTILINE,
)
# 動的script登録pattern.
DYNAMIC_SCRIPT_TOKEN = re.compile(
    r"AddComponent\s*<\s*LamaPon::NativeScriptComponent\s*>\s*"
    r"\(\s*\"([^\"]+)\"",
    re.MULTILINE,
)
# 非対応環境効果。
WEB_UNSUPPORTED_ENVIRONMENT_EFFECTS = {
    "autoExposure": "自動露出",
    "bloom": "ブルーム",
    "depthOfField": "被写界深度",
    "motionBlur": "モーションブラー",
    "screenSpaceLensFlare": "スクリーンスペースレンズフレア",
}
# 対応input action。
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
# 対応input control。
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
# input call検索pattern.
INPUT_ACTION_TOKEN = re.compile(
    r"\b(?:Value|WasPressed|IsDown|WasReleased)\s*\(\s*\"([^\"]+)\""
)

# 除外asset拡張子。
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

# 仮想asset pathを保ち、browserはfile signatureで変換後の形式を判定します。
# Web asset変換表。
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

# converter設定。
WEB_ASSET_CONVERTER_SETTINGS = {
    # Windowsに同名のconvert.exeがあっても、代替ツールとして使用しません。
    "image": ("imageMagick", ("magick",)),
    "audio": ("ffmpeg", ("ffmpeg",)),
    "model": ("assimp", ("assimp",)),
}
# asset種別の表示名。
WEB_ASSET_KIND_LABELS = {
    "image": "画像",
    "audio": "音声",
    "model": "モデル",
}

# project名の禁止文字。
INVALID_WEB_PROJECT_NAME = re.compile(r'[<>:"/\\|?*\x00-\x1f]')

# 既定asset folder。
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


# portable_project_input_actions(source: project root): valid input action bindingsを読み込みます。
def portable_project_input_actions(source: Path) -> dict[str, list[dict[str, Any]]]:
    """LamaPonプロジェクトから、ブラウザー用に変換できる入力設定を読み込みます。"""
    # project設定file path.
    project_path = source / ".lamapon" / "project.json"
    # project settingsが無ければempty mappingを返します。
    if not project_path.is_file():
        # 設定fileが無ければactionはありません。
        return {}
    # JSON設定をUTF-8で読み込みます。
    try:
        # decoded project.json.
        document = json.loads(project_path.read_text(encoding="utf-8"))
    # 読み込みやJSON parseに失敗した設定は無視します。
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        # 不正な設定からactionを作りません。
        return {}
    # input action一覧.
    actions = document.get("inputActions", []) if isinstance(document, dict) else []
    # action一覧がlistでなければ空mappingを返します。
    if not isinstance(actions, list):
        # 対応するaction一覧が無いため空mappingを返します。
        return {}
    # 正規化済みbinding map.
    result: dict[str, list[dict[str, Any]]] = {}
    # action: 各input actionを正規化。
    for action in actions:
        # mappingでないaction entryを飛ばします。
        if not isinstance(action, dict):
            # 不正なaction entryは処理対象外です。
            continue
        # action name.
        name = action.get("name")
        # control binding一覧.
        bindings = action.get("bindings", [])
        # nameとbinding形式が不正なactionを飛ばします。
        if not isinstance(name, str) or not name or not isinstance(bindings, list):
            # 必須項目が不正なactionは登録しません。
            continue
        # 有効binding一覧.
        normalized: list[dict[str, Any]] = []
        # binding: 各control設定を検証。
        for binding in bindings:
            # mappingでないbinding entryを飛ばします。
            if not isinstance(binding, dict):
                # 不正なbinding entryは使用しません。
                continue
            # input control名.
            control = binding.get("control")
            # control scale.
            scale = binding.get("scale", 1.0)
            # 文字列controlと数値scaleのbindingだけ受け付けます。
            if isinstance(control, str) and control \
                    and isinstance(scale, (int, float)):
                # 検証済みbindingをfloat scaleへそろえて追加します。
                normalized.append({"control": control, "scale": float(scale)})
        # action名に正規化済みbinding一覧を登録します。
        result[name] = normalized
    # 変換できたinput action一覧を返します。
    return result


# web_project_name(source: project root, project: settings, project_kind: loader type): 安定した配布名を返します。
def web_project_name(
    source: Path,
    project: dict[str, Any],
    project_kind: str,
) -> str:
    """配布ファイル名に使用する安定したプロジェクト名を返します。"""
    # projectName override.
    explicit_name = project.get("projectName")
    # 明示されたproject nameを優先します。
    if explicit_name is not None:
        # filename用project name.
        name = explicit_name
    # LamaPon projectではfolder名を使います。
    elif project_kind == "lamapon-project":
        # LamaPon project folderの名前を採用します。
        name = source.name
    # 通常のprojectでは設定名を使います。
    else:
        # project設定名が無ければsource folder名を使います。
        name = project.get("name", source.name)
    # 空または文字列以外のproject nameを拒否します。
    if not isinstance(name, str) or not name.strip():
        raise ExportError("The Web project name must be a non-empty string.")
    # 空白除去後のproject name.
    name = name.strip()
    # pathとして危険なproject nameを拒否します。
    if name in {".", ".."} or INVALID_WEB_PROJECT_NAME.search(name):
        raise ExportError(
            "The Web project name contains characters that cannot be used "
            "in a cross-platform filename."
        )
    # filename用に検証したproject nameを返します。
    return name


# web_artifact_prefix(source: project root, project: settings, project_kind: loader type): artifact prefixを作ります。
def web_artifact_prefix(
    source: Path,
    project: dict[str, Any],
    project_kind: str,
) -> str:
    # Web artifact共通prefixを返します。
    return f"LamaPonWebGL-{web_project_name(source, project, project_kind)}"


# is_lamapon_project(project_path: project file, project: decoded settings): LamaPon形式か判定します。
def is_lamapon_project(project_path: Path, project: dict[str, Any]) -> bool:
    # format markerまたは配置場所でproject種別を判定します。
    return (
        project.get("format") == "LamaPonProject"
        or project_path.parent.name == ".lamapon"
    )


# lamapon_project_root(project_path: project file, project: decoded settings): asset解決rootを返します。
def lamapon_project_root(project_path: Path, project: dict[str, Any]) -> Path:
    # LamaPon project.jsonなら親のproject rootを使います。
    if (
        is_lamapon_project(project_path, project)
        and project_path.parent.name == ".lamapon"
    ):
        # .lamapon folderの親directoryを返します。
        return project_path.parent.parent.resolve()
    # project fileを含むdirectoryを返します。
    return project_path.parent.resolve()


# _online_url_authority(value: URL text, scheme: expected prefix): URL authorityを切り出します。
def _online_url_authority(value: str, scheme: str) -> str:
    # schemeが一致しないURLはauthorityを持ちません。
    if not value.startswith(scheme):
        # schemeが一致しないためempty authorityを返します。
        return ""
    # authority開始offset.
    begin = len(scheme)
    # URL delimiterのoffset一覧.
    ends = [
        value.find(marker, begin)
        for marker in "/?#"
        if value.find(marker, begin) >= 0
    ]
    # authority終端offset.
    end = min(ends) if ends else len(value)
    # scheme直後からauthority終端までを返します。
    # 正規化したservice URLを返します。
    return value[begin:end]


# _is_online_loopback_url(value: URL text): localhost系HTTPか判定します。
def _is_online_loopback_url(value: str) -> bool:
    # HTTP host・port.
    authority = _online_url_authority(value, "http://")
    # host: loopback候補を順に照合。
    for host in ("127.0.0.1", "localhost", "[::1]"):
        # portを省略したloopback hostを受け付けます。
        if authority == host:
            # 一致したloopback hostを返します。
            return True
        # 明示port付きhostならport形式を検査します。
        if authority.startswith(f"{host}:"):
            # host後のport文字列.
            port = authority[len(host) + 1:]
            # ASCII数字で構成されたportだけを受け付けます。
            if port and port.isascii() and port.isdigit():
                # 有効なloopback portならTrueを返します。
                return True
    # loopback形式に一致しなければFalseを返します。
    return False


# _normalize_online_service_base_url(value: URL, allow_insecure_loopback: local HTTP flag): validates and normalizes service URL.
def _normalize_online_service_base_url(
    value: str,
    allow_insecure_loopback: bool,
) -> str:
    # trailing slashを除いたURL。
    value = value.rstrip("/")
    # HTTPS schemeかどうか。
    secure = value.startswith("https://")
    # 明示許可されたlocalhost HTTPかどうか。
    allowed_loopback = (
        allow_insecure_loopback and _is_online_loopback_url(value)
    )
    # authority: 検査するhost・port。
    authority = _online_url_authority(
        value,
        "https://" if secure else "http://",
    )
    # URL長をUTF-8 byte数で検査します。
    try:
        # UTF-8形式のURL byte長。
        byte_length = len(value.encode("utf-8"))
    # UTF-8に変換できないURLを拒否します。
    except UnicodeEncodeError as error:
        # 危険なservice URLを拒否します。
        raise ExportError(
            "Online service URL must be valid UTF-8 text."
        ) from error
    # 安全なHTTPS base URLの形式を検証します。
    if (
        (not secure and not allowed_loopback)
        or not authority
        or "@" in authority
        or byte_length > 2048
        or "?" in value
        or "#" in value
        # character: URL内の制御文字。
        or any(ord(character) <= 0x20 or ord(character) == 0x7F
               for character in value)
    ):
        raise ExportError(
            "Online service URL must be an HTTPS base URL without "
            "userinfo, query, fragment, or control characters."
        )
    return value


# _is_safe_online_namespace(value: ID, maximum_bytes: byte limit): ASCII IDか検証します。
def _is_safe_online_namespace(value: str, maximum_bytes: int) -> bool:
    # 長さ・文字種が有効なnamespaceか確認します。
    return (
        bool(value)
        and len(value) <= maximum_bytes
        and value.isascii()
        # character: 検証中のnamespace文字。
        and all(character.isalnum() or character in "._-"
                for character in value)
    )


# validate_lamapon_online_for_web(project: decoded project settings): validates and filters Web online settings.
def validate_lamapon_online_for_web(project: dict[str, Any]) -> None:
    # 未設定のonline optionはそのまま受け付けます。
    if "online" not in project:
        # online設定が無いため検証を終えます。
        return
    # online: projectが指定した未検証設定。
    online = project["online"]
    # object以外のonline設定を拒否します。
    if not isinstance(online, dict):
        raise ExportError(
            "LamaPonProject online settings must be an object."
        )

    # defaults: 欠落fieldに使う既定値。
    defaults: dict[str, Any] = {
        "enabled": False,
        "serviceBaseUrl": "",
        "gameId": "",
        "environmentId": "production",
        "allowInsecureLoopback": False,
        "openAuthorizationBrowser": True,
    }
    # expected_types: online fieldごとの型。
    expected_types = {
        "enabled": bool,
        "serviceBaseUrl": str,
        "gameId": str,
        "environmentId": str,
        "allowInsecureLoopback": bool,
        "openAuthorizationBrowser": bool,
    }
    # normalized/name/default: 既定値を適用したallowlist。
    normalized = {
        name: online.get(name, default)
        for name, default in defaults.items()
    }
    # name/expected: online fieldと要求型を検査します。
    for name, expected in expected_types.items():
        # 要求型と一致しないonline fieldを拒否します。
        if type(normalized[name]) is not expected:
            raise ExportError(
                f"LamaPonProject online.{name} has an invalid type."
            )

    # enabled: online service利用flag。
    enabled = normalized["enabled"]
    # service_base_url: 接続先base URL。
    service_base_url = normalized["serviceBaseUrl"]
    # game_id: online game ID。
    game_id = normalized["gameId"]
    # environment_id: online environment ID。
    environment_id = normalized["environmentId"]
    # allow_insecure: localhost HTTPの許可flag。
    allow_insecure = normalized["allowInsecureLoopback"]
    # 有効時はservice URLを必須にします。
    if enabled and not service_base_url:
        raise ExportError(
            "Enabled online services require a service URL."
        )
    # 有効時はgame IDを必須にします。
    if enabled and not game_id:
        raise ExportError(
            "Enabled online services require a game ID."
        )
    # 有効時はenvironment IDを必須にします。
    if enabled and not environment_id:
        raise ExportError(
            "Enabled online services require an environment ID."
        )
    # Web exportではinsecure loopbackを拒否します。
    if enabled and allow_insecure:
        raise ExportError(
            "Web export cannot enable online services while allowing "
            "insecure loopback HTTP."
        )
    # 指定されたservice URLを正規化します。
    if service_base_url:
        # 検証済みHTTPS base URLへ更新します。
        service_base_url = _normalize_online_service_base_url(
            service_base_url,
            allow_insecure,
        )
        # Webは配布物なので、Projectで明示的に許可したlocalhostもパッケージへ持ち出しません。
        # Web packageにHTTP endpointを含めません。
        if not service_base_url.startswith("https://"):
            raise ExportError(
                "Web export cannot use insecure loopback HTTP."
            )
        # 正規化したURLをallowlistへ戻します。
        normalized["serviceBaseUrl"] = service_base_url
    # 不正なgame IDを拒否します。
    if game_id and not _is_safe_online_namespace(game_id, 128):
        raise ExportError(
            "Online game ID must use 1 to 128 ASCII letters, digits, "
            "'.', '_', or '-'."
        )
    # 不正なenvironment IDを拒否します。
    if environment_id and not _is_safe_online_namespace(
        environment_id,
        64,
    ):
        raise ExportError(
            "Online environment ID must use 1 to 64 ASCII letters, "
            "digits, '.', '_', or '-'."
        )

    # SecretとWindows専用設定を落とし、allowlistを保存します。
    # Secretを除いた設定でWeb online blockを置き換えます。
    project["online"] = normalized


# load_project(project_path: project.json path): reads and validates project settings.
def load_project(project_path: Path) -> tuple[Path, dict[str, Any]]:
    # project.jsonが無ければexportを止めます。
    if not project_path.is_file():
        # missing project fileを報告します。
        raise ExportError(f"Project file was not found: {project_path}")
    # project JSONをUTF-8で読み込みます。
    try:
        # project: decoded project.json object。
        project = json.loads(project_path.read_text(encoding="utf-8"))
    # error: JSON syntax error details。
    except json.JSONDecodeError as error:
        # JSON errorにfile位置を付けて返します。
        raise ExportError(
            f"Project JSON is invalid at line {error.lineno}, "
            f"column {error.colno}: {project_path}"
        ) from error
    # object以外のproject rootを拒否します。
    if not isinstance(project, dict):
        raise ExportError("The project root must be a JSON object.")
    # LamaPon projectのonline設定を検証します。
    if is_lamapon_project(project_path, project):
        validate_lamapon_online_for_web(project)
    # project rootと検証済み設定を返します。
    return project_path.parent, project


# safe_cmake_target(name: requested target name): CMakeで安全なtarget名へ変換します。
def safe_cmake_target(name: str) -> str:
    # normalized: 文字を置換したtarget name。
    normalized = re.sub(r"[^A-Za-z0-9_.+-]", "_", name.strip())
    # 空または不正な先頭文字を補います。
    if not normalized or not re.match(r"[A-Za-z_]", normalized):
        # CMakeが受け付ける先頭文字を追加します。
        normalized = f"LamaPon_{normalized}"
    # 安全化したCMake target名を返します。
    return normalized


# prepare_normal_lamapon_web_configuration(source: project root, project: decoded settings): creates portable Web configuration in memory.
def prepare_normal_lamapon_web_configuration(
    source: Path,
    project: dict[str, Any],
) -> dict[str, Any]:
    """元projectを書き換えず、明示設定を保持します。"""
    # export: project export configuration。
    export = project.get("export", {})
    # 省略されたexport設定をempty mappingにします。
    if export is None:
        # 欠落したexport設定のdefault値。
        export = {}
    # object以外のexport設定を拒否します。
    if not isinstance(export, dict):
        # 不正なexport設定を報告します。
        raise ExportError("Project export must be an object when specified.")
    # web_value: explicit Web export settings。
    web_value = export.get("web", project.get("webExport", {}))
    # 省略されたWeb設定をempty mappingにします。
    if web_value is None:
        # 欠落したWeb設定のdefault値。
        web_value = {}
    # object以外のWeb設定を拒否します。
    if not isinstance(web_value, dict):
        # 不正なWeb設定を報告します。
        raise ExportError("Project export.web must be an object when specified.")
    # web: 後段へ渡すmutable Web設定。
    web = dict(web_value)

    # script_root: project scripts folder。
    script_root = source / "assets" / "scripts"
    # source list未指定時はproject scriptsから推定します。
    if "sources" not in web:
        # source_files/path: portable buildへ含めるscript files。
        source_files = [
            path for path in script_root.rglob("*")
            if path.is_file()
            and path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx"}
            and path.name.lower() not in DEFAULT_PLATFORM_SOURCE_NAMES
        ] if script_root.is_dir() else []
        # project rootからのrelative source path一覧。
        web["sources"] = [
            path.relative_to(source).as_posix()
            for path in sorted(source_files)
        ]

    # asset_root: project assets folder。
    asset_root = source / "assets"
    # asset include list未指定時は既定folderを選びます。
    if "assetIncludePaths" not in web:
        # name: 存在する既定asset folderを選択。
        web["assetIncludePaths"] = [
            name for name in DEFAULT_RUNTIME_ASSET_DIRECTORIES
            if (asset_root / name).exists()
        ]
    web.setdefault("assetDirectory", "assets")
    # startup_scene: project起動scene path。
    startup_scene = project.get("startupScene", "scenes/Main.scene.json")
    # 不正なstartup scene pathを拒否します。
    if not isinstance(startup_scene, str) or not startup_scene:
        # 不正なstartup scene設定を報告します。
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

    # normalized_export: Web targetを含むexport設定。
    normalized_export = dict(export)
    normalized_export.setdefault("targets", ["web"])
    normalized_export["web"] = web
    # 正規化したexport設定をprojectへ戻します。
    project["export"] = normalized_export
    # 生成したportable Web設定を返します。
    return web


# require_web_configuration(project_path: settings path, project_root: project folder, project: decoded settings): validates the selected Web target.
def require_web_configuration(
    project_path: Path,
    project_root: Path,
    project: dict[str, Any],
) -> tuple[Path, str, str, str, str]:
    # LamaPon projects use generated portable settings.
    if is_lamapon_project(project_path, project):
        # web: normalized LamaPon Web settings。
        web = prepare_normal_lamapon_web_configuration(project_root, project)
        # renderer: requested Web graphics backend。
        renderer = web.get("renderer", "webgl2")
        # target: generated CMake target name。
        target = web.get("cmakeTarget")
        # inferred_modules: modules required by project assets。
        inferred_modules = resolved_project_modules(
            project_root,
            project,
            "lamapon-project",
        )
        # profile: selected compatibility profile。
        profile = web.get(
            "compatibilityProfile",
            "webgl2-basic-3d"
            if "renderer3d" in inferred_modules
            else "webgl2-basic-2d",
        )
        # 未対応rendererを拒否します。
        if renderer != "webgl2":
            # unsupported Web backendを報告します。
            raise ExportError(
                "The first Web backend is webgl2; "
                f"received {renderer!r}."
            )
        # 空または文字列以外のtargetを拒否します。
        if not isinstance(target, str) or not target:
            # target未指定を報告します。
            raise ExportError("Web cmakeTarget must be a non-empty string.")
        # 生成CMakeに安全でないtarget名を拒否します。
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.+-]*", target) is None:
            # 不正なtarget名を報告します。
            raise ExportError(
                "Web cmakeTarget contains characters that are unsafe for "
                "a generated CMake target."
            )
        # 空または文字列以外のprofileを拒否します。
        if not isinstance(profile, str) or not profile:
            # 不正なcompatibility profileを報告します。
            raise ExportError(
                "Web compatibilityProfile must be a non-empty string."
            )
        # LamaPon用Web target設定を返します。
        return project_root, target, renderer, profile, "lamapon-project"

    # export: project export configuration。
    export = project.get("export")
    # export設定が無いprojectを拒否します。
    if not isinstance(export, dict):
        # export未設定を報告します。
        raise ExportError("Project has no export configuration.")
    # targets: requested export target一覧。
    targets = export.get("targets")
    # Web targetが無い設定を拒否します。
    if not isinstance(targets, list) or "web" not in targets:
        # Web target未指定を報告します。
        raise ExportError("Project export.targets does not include 'web'.")
    # web: explicit Web target settings。
    web = export.get("web", {})
    # 省略したWeb設定をempty mappingにします。
    if web is None:
        # 省略時のWeb settings default値。
        web = {}
    # object以外のWeb設定を拒否します。
    if not isinstance(web, dict):
        raise ExportError("Project export.web must be an object when specified.")
    # renderer: requested Web graphics backend。
    renderer = web.get("renderer", "webgl2")
    # 未対応rendererを拒否します。
    if renderer != "webgl2":
        raise ExportError(
            "The first Web backend is webgl2; "
            f"received {renderer!r}."
        )
    # source_directory: Web source folder setting。
    source_directory = web.get("sourceDirectory", ".")
    # build_system: project CMake integration mode。
    build_system = web.get("buildSystem", "cmake")
    # target: requested CMake target name。
    target = web.get("cmakeTarget", project.get("name"))
    # profile: selected compatibility profile。
    profile = web.get("compatibilityProfile", "webgl2-basic-2d")
    # source directoryはnon-empty path stringが必要です。
    if not isinstance(source_directory, str) or not source_directory:
        raise ExportError("export.web.sourceDirectory must be a path string.")
    # 空または文字列以外のtargetを拒否します。
    if not isinstance(target, str) or not target:
        raise ExportError("export.web.cmakeTarget must be a non-empty string.")
    # 生成CMakeに安全でないtarget名を拒否します。
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.+-]*", target) is None:
        raise ExportError(
            "export.web.cmakeTarget contains characters that are unsafe for "
            "a generated CMake target."
        )
    # 空または文字列以外のprofileを拒否します。
    if not isinstance(profile, str) or not profile:
        raise ExportError(
            "export.web.compatibilityProfile must be a non-empty string."
        )
    # 未対応のWeb build systemを拒否します。
    if build_system not in {"cmake", "lamapon"}:
        raise ExportError(
            "export.web.buildSystem must be 'lamapon' or 'cmake'."
        )
    # source: project rootから解決したWeb source folder。
    source = (project_root / source_directory).resolve()
    # 存在しないWeb source folderを拒否します。
    if not source.is_dir():
        raise ExportError(f"Web source directory was not found: {source}")
    # CMake buildならCMakeLists.txtを必須にします。
    if build_system == "cmake" and not (source / "CMakeLists.txt").is_file():
        raise ExportError(f"Web source has no CMakeLists.txt: {source}")
    # validated Web target configurationを返します。
    return source, target, renderer, profile, (
        "lamapon-web-target" if build_system == "lamapon" else "cmake"
    )


# source_is_excluded(path: asset path, excluded: excluded roots): checks whether an asset is excluded.
def source_is_excluded(path: Path, excluded: list[Path]) -> bool:
    # item: 各excluded rootとの一致・包含を調べます。
    return any(path == item or is_within(path, item) for item in excluded)


# finding(level: severity, code: finding ID, message: detail, action: next step): builds a report entry.
def finding(
    level: str,
    code: str,
    message: str,
    action: str,
) -> dict[str, str]:
    # Report用finding mappingを返します。
    return {
        "level": level,
        "code": code,
        "message": message,
        "action": action,
    }


# web_asset_converter(web: Web export settings, kind: asset kind): resolves a configured or installed converter.
def web_asset_converter(
    web: dict[str, Any],
    kind: str,
) -> Path | None:
    # setting_name/commands: config key and executable candidates。
    setting_name, commands = WEB_ASSET_CONVERTER_SETTINGS[kind]
    # configured: converter paths from Web settings。
    configured = web.get("converterTools", {})
    # 省略されたconverter設定をempty mappingにします。
    if configured is None:
        # converter設定のdefault値。
        configured = {}
    # object以外のconverter設定を拒否します。
    if not isinstance(configured, dict):
        raise ExportError("export.web.converterTools must be an object.")
    # value: kind用に指定されたconverter path。
    value = configured.get(setting_name)
    # 明示指定pathを優先して解決します。
    if value is not None:
        # 空または文字列以外のconverter pathを拒否します。
        if not isinstance(value, str) or not value:
            raise ExportError(
                f"export.web.converterTools.{setting_name} must be a path string."
            )
        # candidate: user指定pathの展開結果。
        candidate = Path(value).expanduser()
        # resolved: 検証するabsolute converter path。
        resolved = (
            candidate.resolve()
            if candidate.is_absolute() or candidate.parent != Path(".")
            else Path(shutil.which(value) or value).resolve()
        )
        # 存在しないconverter pathを拒否します。
        if not resolved.is_file():
            raise ExportError(
                f"Configured Web asset converter was not found: {resolved}"
            )
        # 検証済みconverter pathを返します。
        return resolved
    # command: PATHから順に探すexecutable名。
    for command in commands:
        # located: PATH上で見つかったexecutable。
        located = shutil.which(command)
        # 見つかったconverterを返します。
        if located:
            # 実行可能なconverterのabsolute pathを返します。
            return Path(located).resolve()
    # converterが見つからない場合はNoneを返します。
    return None


# run_asset_conversion(source: input asset, destination: output asset, kind: asset kind, runtime_format: output format, converter: executable): converts one Web asset.
def run_asset_conversion(
    source: Path,
    destination: Path,
    kind: str,
    runtime_format: str,
    converter: Path,
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    # 画像をWeb用formatへ変換します。
    if kind == "image":
        # GIFはnative同様、先頭frameのみtexture化します。
        # image_source: GIFならnative同様に先頭frameを使います。
        image_source = f"{source}[0]" if source.suffix.lower() == ".gif" \
            else str(source)
        # command: image converter invocation。
        command = [str(converter), image_source]
        # WebPはlosslessでpixelとalphaを保ちます。
        if runtime_format == "webp":
            # WebPはlosslessでpixelとalphaを保ちます。
            # lossless WebP encoding optionsを追加します。
            command.extend((
                "-define", "webp:lossless=true",
                "-quality", "100",
            ))
        # output formatとdestinationを指定します。
        command.append(f"{runtime_format}:{destination}")
    # audioをPCM WAVへ変換します。
    elif kind == "audio":
        # command: audio converter invocation。
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
    # modelをGLBへ変換します。
    elif kind == "model":
        # command: model converter invocation。
        command = [
            str(converter),
            "export",
            str(source),
            str(destination),
            f"-f{runtime_format}2",
        ]
    # 未対応asset kindを拒否します。
    else:
        # unknown conversion kindを報告します。
        # 不正なWebP出力を拒否します。
        raise ExportError(f"Unknown Web asset conversion kind: {kind}")
    # result: converter process output and exit status。
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
    )
    # converter失敗・空出力を検出します。
    if result.returncode != 0 or not destination.is_file() \
            or destination.stat().st_size == 0:
        # detail: failure message from converter。
        detail = (result.stderr or result.stdout).strip()
        # 長すぎるconverter errorを切り詰めます。
        if len(detail) > 600:
            # diagnosticを600文字に制限します。
            detail = detail[:600] + "..."
        # 失敗したasset conversionを報告します。
        raise ExportError(
            f"Failed to convert Web asset '{source}' to {runtime_format.upper()}"
            + (f": {detail}" if detail else ".")
        )
    # payload: 出力format検査用header bytes。
    payload = destination.read_bytes()[:16]
    # WebP container signatureを検証します。
    if runtime_format == "webp" and (
        len(payload) < 12
        or payload[:4] != b"RIFF"
        or payload[8:12] != b"WEBP"
    ):
        raise ExportError(
            f"Image converter did not produce valid WebP bytes: {source}"
        )
    # WAV container signatureを検証します。
    if runtime_format == "wav" and (
        len(payload) < 12
        or payload[:4] != b"RIFF"
        or payload[8:12] != b"WAVE"
    ):
        # 不正なWAV出力を拒否します。
        raise ExportError(
            f"Audio converter did not produce valid WAV bytes: {source}"
        )
    # GLB 2.0 headerを検証します。
    if runtime_format == "glb" and (
        len(payload) < 12
        or payload[:4] != b"glTF"
        or struct.unpack_from("<I", payload, 4)[0] != 2
    ):
        # 不正なGLB outputを拒否します。
        raise ExportError(
            f"Model converter did not produce valid GLB 2.0 bytes: {source}"
        )


# externalize_glb_images(glb_path: converted model, image_converter: image tool): writes image sidecars and rebuilds GLB references.
def externalize_glb_images(
    glb_path: Path,
    image_converter: Path | None,
) -> list[Path]:
    """画像をsidecarへ変換し、GLB内の参照を書き換えます。"""
    # data: source GLB bytes。
    data = glb_path.read_bytes()
    # 短すぎるGLB headerを拒否します。
    if len(data) < 20 or data[:4] != b"glTF":
        raise ExportError(f"Converted model is not a GLB 2.0 file: {glb_path}")
    # GLB magic・version・declared length。
    magic, version, declared_length = struct.unpack_from("<4sII", data, 0)
    # magic・version・file lengthを検証します。
    if magic != b"glTF" or version != 2 or declared_length != len(data):
        raise ExportError(f"Converted model has an invalid GLB header: {glb_path}")
    # chunks: 元GLBの全chunk。
    chunks: list[tuple[int, bytes]] = []
    # offset: 次に読むchunkのbyte位置。
    offset = 12
    # 残りheaderがある間GLB chunkを読みます。
    while offset + 8 <= len(data):
        # chunk lengthとtype。
        length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        # end: 現chunkの終端byte位置。
        end = offset + length
        # fileをはみ出すchunkを拒否します。
        if end > len(data):
            # 切り詰められたchunkを報告します。
            raise ExportError(f"Converted model has a truncated GLB chunk: {glb_path}")
        chunks.append((chunk_type, data[offset:end]))
        # offset: 次chunkの先頭byte位置。
        offset = end
    # payload/chunk_type: JSON chunkの検索。
    json_chunk = next(
        (payload for chunk_type, payload in chunks if chunk_type == 0x4E4F534A),
        None,
    )
    # payload/chunk_type: binary chunkの検索。
    binary_chunk = next(
        (payload for chunk_type, payload in chunks if chunk_type == 0x004E4942),
        None,
    )
    # 必須のGLB JSON chunkを確認します。
    if json_chunk is None:
        raise ExportError(f"Converted model has no GLB JSON chunk: {glb_path}")
    # JSON chunkをUTF-8 documentへdecodeします。
    try:
        # document: decoded GLB JSON object。
        document = json.loads(json_chunk.rstrip(b" \t\r\n\x00").decode("utf-8"))
    # error: GLB JSON decode failure。
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ExportError(f"Converted model has invalid GLB JSON: {glb_path}") from error
    # object以外のGLB rootを拒否します。
    if not isinstance(document, dict):
        raise ExportError(f"Converted model GLB root is not an object: {glb_path}")
    # images: GLBに埋め込まれたimage metadata。
    images = document.get("images", [])
    # buffer_views: image byte範囲のmetadata。
    buffer_views = document.get("bufferViews", [])
    # image・bufferView metadataの型を検証します。
    if not isinstance(images, list) or not isinstance(buffer_views, list):
        raise ExportError(f"Converted model has invalid image metadata: {glb_path}")
    # 画像が無ければ変更せず終了します。
    if not images:
        # image sidecarが無いためempty listを返します。
        return []
    # generated: 作成したimage sidecar path。
    generated: list[Path] = []
    # temporary: 画像変換input用の一時folder。
    with tempfile.TemporaryDirectory(prefix="lamapon-web-model-") as temporary:
        # temporary_root: 変換用folder path。
        temporary_root = Path(temporary)
        # index/image: 各埋込画像を順に変換します。
        for index, image in enumerate(images):
            # URIまたはbufferViewの無い画像を拒否します。
            if not isinstance(image, dict) or "bufferView" not in image:
                raise ExportError(
                    f"Converted model image #{index} is external instead of "
                    f"self-contained: {glb_path}"
                )
            # 画像converter未設定なら変換を止めます。
            if image_converter is None:
                raise ExportError(
                    "The Hub image conversion module is required to normalize "
                    f"embedded model texture #{index}: {glb_path}"
                )
            # view_index: image dataのbufferView番号。
            view_index = image.get("bufferView")
            # bufferView indexとbinary chunkの範囲を検証します。
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
            # view: 対象imageのbufferView。
            view = buffer_views[view_index]
            # GLB内buffer 0以外の参照を拒否します。
            if not isinstance(view, dict) or view.get("buffer", 0) != 0:
                raise ExportError(
                    f"Converted model image #{index} uses an unsupported buffer: "
                    f"{glb_path}"
                )
            # start: image bytesの開始offset。
            start = int(view.get("byteOffset", 0))
            # length: image bytesのbyte数。
            length = int(view.get("byteLength", 0))
            # buffer範囲外のimage bytesを拒否します。
            if start < 0 or length <= 0 or start + length > len(binary_chunk):
                raise ExportError(
                    f"Converted model image #{index} has invalid byte bounds: "
                    f"{glb_path}"
                )
            # mime: embedded image format。
            mime = str(image.get("mimeType", "image/png")).lower()
            # source_extension: converter input用suffix。
            source_extension = {
                "image/jpeg": ".jpg",
                "image/png": ".png",
                "image/webp": ".webp",
            }.get(mime, ".image")
            # temporary_image: converter用に展開したimage file。
            temporary_image = temporary_root / f"image-{index}{source_extension}"
            temporary_image.write_bytes(binary_chunk[start:start + length])
            # sidecar: GLB隣に作成するWebP path。
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
            # GLB内のbinary参照を除きURI参照へ切り替えます。
            image.pop("bufferView", None)
            image.pop("mimeType", None)
            image["uri"] = sidecar.name
            generated.append(sidecar)
    # image sidecarが無ければGLBを変更しません。
    if not generated:
        # sidecarが無ければGLBを書き換えません。
        return []
    # encoded_json: image URIを反映したJSON chunk。
    encoded_json = json.dumps(
        document,
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    # GLB要求に合わせてJSON chunkを4-byte alignします。
    encoded_json += b" " * ((4 - len(encoded_json) % 4) % 4)
    # rebuilt_chunks: 更新後のchunk一覧。
    rebuilt_chunks: list[tuple[int, bytes]] = []
    # replaced_json: JSON chunkを差し替えたか。
    replaced_json = False
    # chunk_type/payload: 元chunkを順に再構成します。
    for chunk_type, payload in chunks:
        # 最初のJSON chunkだけ更新済み内容へ差し替えます。
        if chunk_type == 0x4E4F534A and not replaced_json:
            # 更新済みJSON chunkを追加します。
            rebuilt_chunks.append((chunk_type, encoded_json))
            # 以降のJSON chunkはそのままにします。
            replaced_json = True
        # JSON以外のchunkは元bytesを保持します。
        else:
            # 元chunk bytesを再構成一覧へ追加します。
            rebuilt_chunks.append((chunk_type, payload))
    # total_length: rebuilt GLB全体のbyte数。
    total_length = 12 + sum(8 + len(payload) for _, payload in rebuilt_chunks)
    # rebuilt: 新しいGLB binary data。
    rebuilt = bytearray(struct.pack("<4sII", b"glTF", 2, total_length))
    # chunk_type/payload: chunk headerとbytesを書き出します。
    for chunk_type, payload in rebuilt_chunks:
        # chunk headerをrebuilt GLBへ追加します。
        rebuilt.extend(struct.pack("<II", len(payload), chunk_type))
        rebuilt.extend(payload)
    # 元GLBをWebP URI付きデータで更新します。
    glb_path.write_bytes(rebuilt)
    # 生成したWebP sidecar path一覧を返します。
    return generated


# validate_portable_glb(glb_path: GLB file): rejects features unsupported by the portable renderer.
def validate_portable_glb(glb_path: Path) -> dict[str, int]:
    """ポータブルランタイムで保持できない正規GLB機能を拒否します。"""
    # data: source GLB bytes。
    data = glb_path.read_bytes()
    # 短すぎるGLB headerを拒否します。
    if len(data) < 20 or data[:4] != b"glTF":
        raise ExportError(f"Converted model is not a GLB 2.0 file: {glb_path}")
    # versionとdeclared file length。
    _, version, declared_length = struct.unpack_from("<4sII", data, 0)
    # GLB versionとfile lengthを検証します。
    if version != 2 or declared_length != len(data):
        raise ExportError(f"Converted model has an invalid GLB header: {glb_path}")
    # offset: 次に読むchunkのbyte位置。
    offset = 12
    # json_chunk: GLB JSON chunk。
    json_chunk: bytes | None = None
    # 残りheaderがある間GLB chunkを走査します。
    while offset + 8 <= len(data):
        # chunk lengthとtype。
        length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        # end: 現chunkの終端byte位置。
        end = offset + length
        # fileをはみ出すchunkを拒否します。
        if end > len(data):
            raise ExportError(f"Converted model has a truncated GLB chunk: {glb_path}")
        # 最初に見つかったJSON chunkを保持します。
        if chunk_type == 0x4E4F534A and json_chunk is None:
            # GLB JSON chunk bytes。
            json_chunk = data[offset:end]
        # offset: 次chunkの先頭byte位置。
        offset = end
    # 必須のGLB JSON chunkを確認します。
    if json_chunk is None:
        raise ExportError(f"Converted model has no GLB JSON chunk: {glb_path}")
    # JSON chunkをUTF-8 documentへdecodeします。
    try:
        # document: decoded GLB JSON object。
        document = json.loads(json_chunk.rstrip(b" \t\r\n\x00").decode("utf-8"))
    # error: GLB JSON decode failure。
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ExportError(f"Converted model has invalid GLB JSON: {glb_path}") from error
    # object以外のGLB rootを拒否します。
    if not isinstance(document, dict):
        raise ExportError(f"Converted model GLB root is not an object: {glb_path}")
    # required_extensions: 必須GLB extension一覧。
    required_extensions = document.get("extensionsRequired", [])
    # required extension fieldの型を検証します。
    if not isinstance(required_extensions, list):
        raise ExportError(f"Converted model has invalid required extensions: {glb_path}")
    # 未対応extensionを必須とするGLBを拒否します。
    if required_extensions:
        raise ExportError(
            f"Converted model requires unsupported GLB extension(s) "
            f"{', '.join(map(str, required_extensions))}: {glb_path}"
        )
    # 未使用extensionは許可し、実使用の非対応機能だけ拒否します。
    # unsupported_material_extensions: 描画再現できないmaterial extensions。
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
    # materials: GLB material一覧。
    materials = document.get("materials", [])
    # material metadataの型を検証します。
    if not isinstance(materials, list):
        raise ExportError(f"Converted model has invalid material metadata: {glb_path}")
    # texture_view_names: rendererが読むtexture slot名。
    texture_view_names = {
        "baseColorTexture", "metallicRoughnessTexture", "normalTexture",
        "occlusionTexture", "emissiveTexture",
    }
    # unlit_materials: unlit material数。
    unlit_materials = 0
    # material_index/material: 各materialを順に検査します。
    for material_index, material in enumerate(materials):
        # mapping以外のmaterialを飛ばします。
        if not isinstance(material, dict):
            # 未対応material entryは検査しません。
            continue
        # extensions: materialに有効なextension設定。
        extensions = material.get("extensions", {})
        # material extensionsの型を検証します。
        if not isinstance(extensions, dict):
            raise ExportError(
                f"Converted model material #{material_index} has invalid "
                f"extensions: {glb_path}"
            )
        # active_unsupported/extension: 実際に使われる未対応extension。
        active_unsupported = sorted(
            extension for extension in unsupported_material_extensions
            if extension in extensions and extensions[extension]
        )
        # 描画結果が変わる未対応material extensionを拒否します。
        if active_unsupported:
            raise ExportError(
                f"Converted model material #{material_index} uses advanced "
                f"material extension(s) {', '.join(active_unsupported)} that "
                f"the WebGL material backend cannot reproduce: {glb_path}"
            )
        # unlit extension利用数を集計します。
        if "KHR_materials_unlit" in extensions:
            # unlit material数を加算します。
            unlit_materials += 1
        # specular_extension: specular material設定。
        specular_extension = extensions.get("KHR_materials_specular", {})
        # 未対応のspecular texture slotを拒否します。
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
        # views: rendererで参照するmaterial texture views。
        views: list[dict[str, Any]] = []
        # pbr: metallic-roughness material設定。
        pbr = material.get("pbrMetallicRoughness", {})
        # 対応するPBR texture viewを集めます。
        if isinstance(pbr, dict):
            # key/value: 対応slotのtexture viewを選びます。
            views.extend(
                value for key, value in pbr.items()
                if key in texture_view_names and isinstance(value, dict)
            )
        views.extend(
            value for key, value in material.items()
            if key in texture_view_names and isinstance(value, dict)
        )
        # view: 各material texture coordinate設定。
        for view in views:
            # TEXCOORD_0以外を使うmaterialを拒否します。
            if int(view.get("texCoord", 0)) != 0:
                raise ExportError(
                    f"Converted model material #{material_index} uses UV set "
                    f"{view.get('texCoord')}; the Portable renderer currently "
                    f"supports TEXCOORD_0 only: {glb_path}"
                )
            # view_extensions: texture view extension設定。
            view_extensions = view.get("extensions", {})
            # 未対応のtexture UV transformを拒否します。
            if isinstance(view_extensions, dict) \
                    and "KHR_texture_transform" in view_extensions:
                raise ExportError(
                    f"Converted model material #{material_index} uses a "
                    f"texture UV transform that cannot yet be reproduced "
                    f"exactly by the Portable renderer: {glb_path}"
                )
    # accessors: geometry attribute metadata一覧。
    accessors = document.get("accessors", [])
    # meshes: GLB mesh一覧。
    meshes = document.get("meshes", [])
    # mesh・accessor metadataの型を検証します。
    if not isinstance(accessors, list) or not isinstance(meshes, list):
        raise ExportError(f"Converted model has invalid mesh metadata: {glb_path}")
    # renderable_primitives: portable triangle primitive数。
    renderable_primitives = 0
    # total_vertices: 描画primitiveのvertex合計。
    total_vertices = 0
    # mesh: 各GLB meshを走査します。
    for mesh in meshes:
        # mapping以外のmeshを飛ばします。
        if not isinstance(mesh, dict):
            # 不正mesh entryは検査対象外です。
            continue
        # primitives: meshのtriangle候補一覧。
        primitives = mesh.get("primitives", [])
        # list以外のprimitive entryを飛ばします。
        if not isinstance(primitives, list):
            # 不正primitive containerを飛ばします。
            continue
        # primitive: 各mesh primitiveを検査します。
        for primitive in primitives:
            # triangle list以外のprimitiveを飛ばします。
            if not isinstance(primitive, dict) or primitive.get("mode", 4) != 4:
                # 非triangle primitiveは対象外です。
                continue
            # 未対応morph targetを拒否します。
            if primitive.get("targets"):
                raise ExportError(
                    f"Converted model uses morph targets, which are not yet "
                    f"portable without changing the animation: {glb_path}"
                )
            # extensions: geometry compression metadata。
            extensions = primitive.get("extensions", {})
            # 未対応compressed geometryを拒否します。
            if isinstance(extensions, dict) and (
                "KHR_draco_mesh_compression" in extensions
                or "EXT_meshopt_compression" in extensions
            ):
                raise ExportError(
                    f"Converted model uses compressed geometry unsupported by "
                    f"the current Portable decoder: {glb_path}"
                )
            # attributes: vertex attribute index map。
            attributes = primitive.get("attributes", {})
            # position_index: POSITION accessor番号。
            position_index = attributes.get("POSITION") \
                if isinstance(attributes, dict) else None
            # 無効なPOSITION accessorを持つprimitiveを飛ばします。
            if not isinstance(position_index, int) \
                    or position_index < 0 or position_index >= len(accessors):
                # 利用可能なPOSITION accessorが無いprimitiveを飛ばします。
                continue
            # position: POSITION accessor metadata。
            position = accessors[position_index]
            # vertex_count: POSITION accessorのvertex数。
            vertex_count = position.get("count", 0) \
                if isinstance(position, dict) else 0
            # 空または不正なvertex accessorを飛ばします。
            if not isinstance(vertex_count, int) or vertex_count <= 0:
                # 描画不能なprimitiveを集計しません。
                continue
            renderable_primitives += 1
            total_vertices += vertex_count
    # 描画可能なtriangleが無いGLBを拒否します。
    if renderable_primitives == 0:
        raise ExportError(
            f"Converted model contains no portable triangle primitives: {glb_path}"
        )
    # animations: GLB animation一覧。
    animations = document.get("animations", [])
    # animation metadataの型を検証します。
    if not isinstance(animations, list):
        raise ExportError(f"Converted model has invalid animation metadata: {glb_path}")
    # animation: 各animationを走査します。
    for animation in animations:
        # mapping以外のanimationを飛ばします。
        if not isinstance(animation, dict):
            # 不正animation entryは検査しません。
            continue
        # channel: animation channelを順に検査します。
        for channel in animation.get("channels", []):
            # target: channelが変更するanimation property。
            target = channel.get("target", {}) if isinstance(channel, dict) else {}
            # morph weight animationを拒否します。
            if isinstance(target, dict) and target.get("path") == "weights":
                raise ExportError(
                    f"Converted model animates morph weights, which cannot yet "
                    f"be preserved by the Portable runtime: {glb_path}"
                )
    # skins: GLB skin一覧。
    skins = document.get("skins", [])
    # images: GLB image一覧。
    images = document.get("images", [])
    # 検証結果のmesh・image・material統計を返します。
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


# infer_lamapon_modules(source: project root, project: settings): infers the minimum portable modules.
def infer_lamapon_modules(source: Path, project: dict[str, Any]) -> list[str]:
    """通常のLamaPonプロジェクトが使用する最小限のWebモジュールを推定します。"""
    # modules: required portable runtime modules。
    modules = ["core", "input"]
    # script_files/path/part: portable project source files。
    script_files = [
        path
        for path in source.rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_EXTENSIONS
        and not any(
            part in {".git", ".lamapon", "build", "third_party"}
            for part in path.relative_to(source).parts
        )
    ]
    # script_text: combined project source text。
    script_text = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in script_files
    )
    # api: each LamaPon API name found in scripts。
    for api in LAMAPON_API_TOKEN.findall(script_text):
        # module: runtime module required by the API。
        module = PORTABLE_API_MODULES.get(api)
        # 既知APIのruntime moduleを追加します。
        if module is not None:
            modules.append(module)

    # startup_scene: project起動scene path。
    startup_scene = project.get("startupScene")
    # scene pathが有効な場合だけsceneを解析します。
    if isinstance(startup_scene, str) and startup_scene:
        # scene_path: startup scene file。
        scene_path = source / "assets" / startup_scene
        # startup scene JSONを読み込みます。
        try:
            # scene: decoded startup scene object。
            scene = json.loads(scene_path.read_text(encoding="utf-8"))
        # scene fileが無効ならscene module推定を省略します。
        except (OSError, UnicodeDecodeError, json.JSONDecodeError):
            # 読めないsceneのplaceholder値。
            scene = None
        # 有効なscene objectからcomponentを調べます。
        if isinstance(scene, dict):
            # obj: scene内のobjectを順に走査します。
            for obj in scene.get("objects", []):
                # mapping以外のscene objectを飛ばします。
                if not isinstance(obj, dict):
                    # 不正scene objectをmodule判定から除きます。
                    continue
                # component: 各objectのcomponentを調べます。
                for component in obj.get("components", []):
                    # mapping以外のcomponentを飛ばします。
                    if not isinstance(component, dict):
                        # 不正component entryをmodule判定から除きます。
                        continue
                    # component_type: native scene component名。
                    component_type = component.get("type")
                    # module: componentが必要とするruntime module。
                    module = PORTABLE_SCENE_COMPONENTS.get(component_type)
                    # 対応moduleがあるcomponentを集計します。
                    if module is not None:
                        modules.append(module)

    # 起動sceneが空でもgraphics設定からrenderer moduleを選びます。
    # graphics: project graphics settings。
    graphics = project.get("graphics", {})
    # 未推定の場合はgraphics設定からrenderer moduleを選びます。
    if not ({"renderer2d", "renderer3d"} & set(modules)):
        # sceneが空でもprojectのrendering pathからdefaultを選びます。
        modules.append(
            "renderer3d"
            if isinstance(graphics, dict) and graphics.get("renderingPath")
            else "renderer2d"
        )
    # 重複を除いたportable module一覧を返します。
    return list(dict.fromkeys(modules))


# resolved_project_modules(source: project root, project: settings, project_kind: loader type): validates module selection.
def resolved_project_modules(
    source: Path,
    project: dict[str, Any],
    project_kind: str,
) -> list[str]:
    # export: project export configuration。
    export = project.get("export", {})
    # object以外のexport設定を拒否します。
    if not isinstance(export, dict):
        raise ExportError("Project export must be an object when specified.")
    # modules: requested or inferred runtime module list。
    modules = export.get(
        "modules",
        infer_lamapon_modules(source, project)
        if project_kind == "lamapon-project"
        else DEFAULT_MODULES,
    )
    # 空またはlist以外のmodule設定を拒否します。
    if not isinstance(modules, list) or not modules:
        raise ExportError("Project export.modules must be a string list.")
    # 空または文字列以外のmodule名を拒否します。
    if any(not isinstance(module, str) or not module for module in modules):
        raise ExportError("Every project export.modules entry must be a string.")
    # 重複を除いた検証済みmodule一覧を返します。
    return list(dict.fromkeys(modules))


# selected_web_source_files(source: source root, web: Web settings): resolves project source and header files.
def selected_web_source_files(source: Path, web: dict[str, Any]) -> list[Path]:
    # source_values: configured source path list。
    source_values = web.get("sources")
    # source未指定時はsource treeから検索します。
    if source_values is None:
        # C/C++ source一覧をtreeから返します。
        return [
            path
            for path in source.rglob("*")
            if path.is_file() and path.suffix.lower() in SOURCE_EXTENSIONS
        ]
    # 配列形式でないWeb source指定を拒否します。
    if not isinstance(source_values, list) or any(
        not isinstance(value, str) or not value for value in source_values
    ):
        raise ExportError("export.web.sources must be a non-empty string list.")
    # selected/value: configured project source paths。
    selected = [require_relative_file(
        source, value, "export.web.sources entry") for value in source_values]
    # headers/path/part: project header files。
    headers = [
        path.resolve()
        for path in source.rglob("*")
        if path.is_file() and path.suffix.lower() in {".h", ".hh", ".hpp", ".hxx"}
        and not any(part in {".git", ".lamapon", "build", "third_party"}
                    for part in path.relative_to(source).parts)
    ]
    # 重複を除いたsourceとheader一覧を返します。
    return list(dict.fromkeys([*selected, *headers]))


# web_asset_roots(source: project root, web: Web settings): resolves included asset folders.
def web_asset_roots(
    source: Path,
    web: dict[str, Any],
) -> tuple[Path, list[Path]] | None:
    # asset_value: configured asset directory。
    asset_value = web.get("assetDirectory", "assets")
    # assetDirectoryはnon-empty path stringが必要です。
    if not isinstance(asset_value, str) or not asset_value:
        raise ExportError("export.web.assetDirectory must be a path string.")
    # asset_directory: resolved project asset folder。
    asset_directory = (source / asset_value).resolve()
    # asset folder未作成ならasset scanを省略します。
    if not asset_directory.exists():
        # asset folderが無ければNoneを返します。
        return None
    # fileをasset directoryとして指定した場合は拒否します。
    if not asset_directory.is_dir():
        raise ExportError(f"Web asset path is not a directory: {asset_directory}")
    # include_values: asset directory内のinclude path一覧。
    include_values = web.get("assetIncludePaths", ["."])
    # asset include pathはnon-empty string listが必要です。
    if not isinstance(include_values, list) or not include_values or any(
        not isinstance(item, str) or not item for item in include_values
    ):
        raise ExportError("export.web.assetIncludePaths must be a string list.")
    # included_roots/item: 解決したasset include folder。
    included_roots = [(asset_directory / item).resolve() for item in include_values]
    # asset folder外を指すinclude pathを拒否します。
    if any(not is_within(path, asset_directory) for path in included_roots):
        raise ExportError("export.web.assetIncludePaths must stay inside assets.")
    # 存在しないinclude folderを拒否します。
    if any(not path.exists() for path in included_roots):
        # missing: 最初に見つかった存在しないinclude path。
        missing = next(path for path in included_roots if not path.exists())
        # missing include pathを報告します。
        raise ExportError(f"Included Web asset path was not found: {missing}")
    # asset rootとinclude folder一覧を返します。
    return asset_directory, included_roots


# asset_is_selected(path: asset file, roots: included roots): checks whether a file belongs to an included folder.
def asset_is_selected(path: Path, roots: list[Path]) -> bool:
    # root: 各include rootとの一致・包含を調べます。
    return any(path == root or is_within(path, root) for root in roots)


# validate_asset_integrity(path: asset file, source: project root): reports corrupt or oversized Web assets.
def validate_asset_integrity(
    path: Path,
    source: Path,
) -> list[dict[str, str]]:
    # findings: integrity diagnostics for this asset。
    findings: list[dict[str, str]] = []
    # relative: project-relative asset path。
    relative = path.relative_to(source)
    # extension: lowercase asset suffix。
    extension = path.suffix.lower()
    # asset bytesを読み込みます。
    try:
        # data: raw asset bytes。
        data = path.read_bytes()
    # error: asset read failure。
    except OSError as error:
        # 読み込めないassetのreject findingを返します。
        return [finding(
            "reject", "unreadable-web-asset",
            f"アセット「{relative}」を読み込めませんでした: {error}",
            "ファイルのアクセス権を修復するか、アセットを置き換えてください。",
        )]
    # empty assetをrejectします。
    if not data:
        # empty assetのreject findingを返します。
        return [finding(
            "reject", "empty-web-asset",
            f"アセット「{relative}」が空です。",
            "空のアセットを置き換えるか削除してください。",
        )]
    # PNG headerとtexture sizeを検証します。
    if extension == ".png":
        # 短いまたは不正なPNG headerを報告します。
        if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
            findings.append(finding(
                "reject", "corrupt-png-asset",
                f"アセット「{relative}」のPNGヘッダーが正しくありません。",
                "テクスチャを有効なPNGとして再出力してください。",
            ))
        # PNG headerが有効なら寸法を読み取ります。
        else:
            # width/height: PNG texture dimensions。
            width, height = struct.unpack(">II", data[16:24])
            # 0px textureをrejectします。
            if width == 0 or height == 0:
                findings.append(finding(
                    "reject", "invalid-texture-size",
                    f"テクスチャ「{relative}」の幅または高さが0です。",
                    "テクスチャを再出力してください。",
                ))
            # memory負荷の高いtextureをwarningにします。
            elif width > 4096 or height > 4096:
                findings.append(finding(
                    "warning", "large-web-texture",
                    f"テクスチャ「{relative}」のサイズは{width}x{height}です。ブラウザーのメモリを多く消費する可能性があります。",
                    "Web用テクスチャは4096以下を目安に縮小してください。",
                ))
    # JPEG boundary markerを検証します。
    elif extension in {".jpg", ".jpeg"}:
        # 不正なJPEG境界markerを報告します。
        if len(data) < 4 or not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
            findings.append(finding(
                "reject", "corrupt-jpeg-asset",
                f"アセット「{relative}」のJPEG境界マーカーが正しくありません。",
                "テクスチャを有効なJPEGとして再出力してください。",
            ))
    # WebP container headerを検証します。
    elif extension == ".webp":
        # 不正なWebP signatureを報告します。
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
    # RIFF/WAVE headerを検証します。
    elif extension == ".wav":
        # 不正なWAV signatureを報告します。
        if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
            findings.append(finding(
                "reject", "corrupt-wav-asset",
                f"アセット「{relative}」のRIFF/WAVEヘッダーが正しくありません。",
                "音声を有効なWAVファイルとして再出力してください。",
            ))
    # JSON assetのUTF-8 syntaxを検証します。
    elif extension == ".json":
        # JSON assetをUTF-8でparseします。
        try:
            # decoded JSON構文を検査します。
            json.loads(data.decode("utf-8"))
        # 不正なUTF-8またはJSONをfindingにします。
        except (UnicodeDecodeError, json.JSONDecodeError):
            # invalid JSON assetのreject findingを追加します。
            findings.append(finding(
                "reject", "invalid-json-asset",
                f"アセット「{relative}」は有効なUTF-8 JSONではありません。",
                "JSONアセットを修復するか再出力してください。",
            ))
    # 収集したasset integrity findingsを返します。
    return findings


# validate_portable_contract(source: Project root, project: 設定, project_kind: 種別, web: Web設定, modules: Module一覧): ポータブルWeb出力の契約と参照アセットを検証します。
def validate_portable_contract(
    source: Path,
    project: dict[str, Any],
    project_kind: str,
    web: dict[str, Any],
    modules: list[str],
) -> list[dict[str, str]]:
    # 非ポータブル形式はこの検査の対象外です。
    if project_kind not in {"lamapon-web-target", "lamapon-project"} \
            or not web.get("portableGame", False):
        return []
    # source: 参照診断に使う実体パス。
    source = source.resolve()
    # findings: 互換性診断の一覧。
    findings: list[dict[str, str]] = []
    # source_files: Web対象のソース一覧。
    source_files = selected_web_source_files(source, web)
    # configured_input_actions: Project固有の入力設定。
    configured_input_actions = portable_project_input_actions(source)
    # available_input_actions: Webで利用できる入力名。
    available_input_actions = PORTABLE_INPUT_ACTIONS \
        | set(configured_input_actions)
    # source_text: ソースパスと内容の対応。
    source_text: dict[Path, str] = {
        path: path.read_text(encoding="utf-8", errors="replace")
        for path in source_files
    }
    # declared_modules: Web出力で選択されたModule。
    declared_modules = set(modules)
    # required: 必須となる基盤Module名。
    for required in ("core", "input"):
        # 必須Moduleがなければrejectします。
        if required not in declared_modules:
            findings.append(finding(
                "reject", "missing-portable-base-module",
                f"ポータブルWebゲームには「{required}」モジュールが必要です。",
                f"export.modulesに「{required}」を追加してください。",
            ))
    # 2Dまたは3D Rendererの指定を確認します。
    if not ({"renderer2d", "renderer3d"} & declared_modules):
        findings.append(finding(
            "reject", "missing-renderer-module",
            "ポータブルWebゲームにはrenderer2dまたはrenderer3dが必要です。",
            "プロジェクトが使用するレンダラーを指定してください。",
        ))
    # api_locations: 使用APIと参照位置。
    api_locations: dict[str, list[str]] = {}
    # asset_references: 使用アセットと参照位置。
    asset_references: dict[str, list[str]] = {}
    # registered_scripts: Webソースで登録されたScript名。
    registered_scripts: set[str] = set()
    # dynamic_scripts: 動的生成されたScriptと使用位置。
    dynamic_scripts: dict[str, list[str]] = {}
    # input_actions: 使用入力Actionと参照位置。
    input_actions: dict[str, list[str]] = {}
    # path/text: Web対象ソースのパスと内容。
    for path, text in source_text.items():
        # relative: Project相対のソースパス。
        relative = path.relative_to(source)
        # line_number/line: 検査中の行番号と内容。
        for line_number, line in enumerate(text.splitlines(), start=1):
            # API使用箇所を記録します。
            for api in LAMAPON_API_TOKEN.findall(line):
                api_locations.setdefault(api, []).append(
                    f"{relative}:{line_number}"
                )
            # アセット参照箇所を記録します。
            for asset in ASSET_PATH_TOKEN.findall(line):
                asset_references.setdefault(asset, []).append(
                    f"{relative}:{line_number}"
                )
            # 入力Action使用箇所を記録します。
            for action in INPUT_ACTION_TOKEN.findall(line):
                input_actions.setdefault(action, []).append(
                    f"{relative}:{line_number}"
                )
        registered_scripts.update(SCRIPT_REGISTRATION_TOKEN.findall(text))
        # 動的Script生成箇所を記録します。
        for script_id in DYNAMIC_SCRIPT_TOKEN.findall(text):
            dynamic_scripts.setdefault(script_id, []).append(str(relative))

    # api/locations: API名と参照箇所。
    for api, locations in sorted(api_locations.items()):
        # required_module: APIが必要とするModule。
        required_module = PORTABLE_API_MODULES.get(api)
        # location: 最初に見つかった使用箇所。
        location = locations[0]
        # 未実装APIはrejectします。
        if required_module is None:
            findings.append(finding(
                "reject",
                "unsupported-portable-api",
                f"{location}: LamaPon::{api}にはポータブルWeb版の実装がありません。",
                "Web出力を行う前に、このAPIをポータブルランタイムへ実装してテストしてください。",
            ))
        # 必要Moduleが未選択ならrejectします。
        elif required_module not in declared_modules:
            findings.append(finding(
                "reject",
                "missing-required-module",
                f"{location}: LamaPon::{api}には「{required_module}」モジュールが必要ですが、プロジェクトで指定されていません。",
                f"export.modulesに「{required_module}」を追加してください。",
            ))
    # script_id/locations: 動的Script名と生成箇所。
    for script_id, locations in sorted(dynamic_scripts.items()):
        # 選択ソースに登録がないScriptをrejectします。
        if script_id not in registered_scripts:
            findings.append(finding(
                "reject", "unregistered-dynamic-script",
                f"{locations[0]}はNativeScript「{script_id}」を作成しますが、選択したWebソースに登録がありません。",
                "LAMAPON_SCRIPT_NAMEDを含むソースを追加するか、スクリプトIDを修正してください。",
            ))
    # action/locations: 入力Action名と参照箇所。
    for action, locations in sorted(input_actions.items()):
        # 未対応Actionはrejectします。
        if action not in available_input_actions:
            findings.append(finding(
                "reject", "unsupported-input-action",
                f"{locations[0]}は入力アクション「{action}」を使用しますが、ポータブルWeb入力マップに定義がありません。",
                "Web出力を行う前に、ポータブル入力バックエンドへアクションとブラウザー用バインドを追加してください。",
            ))
            continue
        # unsupported_controls: Web入力が扱えない操作名。
        unsupported_controls = sorted({
            str(binding.get("control"))
            for binding in configured_input_actions.get(action, [])
            if binding.get("control") not in PORTABLE_INPUT_CONTROLS
        })
        # 未対応操作があるActionをrejectします。
        if unsupported_controls:
            findings.append(finding(
                "reject", "unsupported-input-control",
                f"入力アクション「{action}」には、ブラウザーバックエンドで割り当てられない操作があります: {', '.join(unsupported_controls)}。",
                "ポータブル入力バックエンドが対応するキーボード、マウス、標準ゲームパッドの操作を使用してください。",
            ))
        # Project固有Actionの自動変換を案内します。
        elif action not in PORTABLE_INPUT_ACTIONS:
            findings.append(finding(
                "auto", "project-input-action-map",
                f"プロジェクトの入力アクション「{action}」は、出力時にLamaPonプロジェクトのバインドから変換されます。",
                "このアクション名のためにエンジンを変更する必要はありません。",
            ))

    # asset_selection: アセット格納先と対象ルート。
    asset_selection = web_asset_roots(source, web)
    # asset_directory: Projectアセットの格納先。
    asset_directory: Path | None = None
    # included_roots: Web出力へ含めるアセット。
    included_roots: list[Path] = []
    # 選択済みなら格納先と対象ルートを展開します。
    if asset_selection is not None:
        # asset_directory/included_roots: 選択済みアセット情報。
        asset_directory, included_roots = asset_selection
    # reference/locations: アセット名と参照箇所。
    for reference, locations in sorted(asset_references.items()):
        # アセット格納先がない参照をrejectします。
        if asset_directory is None:
            findings.append(finding(
                "reject",
                "missing-asset-directory",
                f"{locations[0]}は「{reference}」を参照していますが、アセットフォルダーがありません。",
                "アセットフォルダーを復元するか、参照を削除してください。",
            ))
            continue
        # target: 参照先の実体パス。
        target = (asset_directory / reference).resolve()
        # 格納先外または存在しない参照をrejectします。
        if not is_within(target, asset_directory) or not target.is_file():
            findings.append(finding(
                "reject",
                "missing-referenced-asset",
                f"{locations[0]}が参照するアセット「{reference}」が見つかりません。",
                "アセットを追加するか、ポータブルアセットパスを修正してください。",
            ))
        # 未選択アセットへの参照をrejectします。
        elif not asset_is_selected(target, included_roots):
            findings.append(finding(
                "reject",
                "unpackaged-referenced-asset",
                f"{locations[0]}が参照する「{reference}」はassetIncludePathsの対象外です。",
                "参照先を含むアセットパスをWebパッケージへ追加してください。",
            ))

    # scene_path: Projectで指定された起動シーン。
    scene_path = web.get("scenePath", "/assets/scenes/Main.scene.json")
    # 起動パスの形式が違えば以降のScene検査を省きます。
    if not isinstance(scene_path, str) or not scene_path.startswith("/assets/"):
        findings.append(finding(
            "reject", "invalid-scene-path",
            "ポータブルシーンのパスは「/assets/」で始める必要があります。",
            "export.web.scenePathに、パッケージへ含めるシーンJSONのパスを指定してください。",
        ))
        return findings
    # scene_file: 起動SceneのProject内パス。
    scene_file = (
        asset_directory / scene_path.removeprefix("/assets/")
        if asset_directory is not None else None
    )
    # Sceneファイルが存在しない場合は検査を終えます。
    if scene_file is None or not scene_file.is_file():
        findings.append(finding(
            "reject", "missing-startup-scene",
            f"起動シーンが見つかりません: {scene_path}",
            "シーンをパッケージへ追加するか、export.web.scenePathを修正してください。",
        ))
        return findings
    # 起動Sceneが出力対象に含まれるか確認します。
    if not asset_is_selected(scene_file.resolve(), included_roots):
        findings.append(finding(
            "reject", "unpackaged-startup-scene",
            f"起動シーン「{scene_path}」はassetIncludePathsの対象外です。",
            "シーンを含むフォルダーをWebパッケージへ追加してください。",
        ))
    # scene: 読み込んだ起動Sceneデータ。
    try:
        # scene: JSONから復元した起動Scene。
        scene = json.loads(scene_file.read_text(encoding="utf-8"))
    # error: Scene JSONの構文エラー。
    except json.JSONDecodeError as error:
        findings.append(finding(
            "reject", "invalid-scene-json",
            f"{scene_file.relative_to(source)}:{error.lineno}: シーンJSONが正しくありません。",
            "Web出力を行う前にシーンファイルを修正してください。",
        ))
        return findings
    # 未対応形式のSceneは以降の検査を省きます。
    if not isinstance(scene, dict) or scene.get("format") != "LamaPonScene":
        findings.append(finding(
            "reject", "invalid-scene-format",
            f"{scene_file.relative_to(source)}はLamaPonScene形式ではありません。",
            "互換性のあるLamaPon Editorでシーンを開き、保存し直してください。",
        ))
        return findings

    # scene_asset_strings(value: nested JSON value): Collect asset-path strings from a Scene value.
    def scene_asset_strings(value: Any) -> list[str]:
        # 対応する相対アセットパスだけを返します。
        if isinstance(value, str) and value.startswith(
            (
                "textures/", "audio/", "scenes/", "models/", "fonts/",
                "materials/", "animations/", "shaders/",
            )
        ):
            return [value]
        # 配列要素を再帰走査します。
        if isinstance(value, list):
            return [item for child in value for item in scene_asset_strings(child)]
        # オブジェクト値を再帰走査します。
        if isinstance(value, dict):
            return [item for child in value.values()
                    for item in scene_asset_strings(child)]
        return []

    # reference: Sceneから抽出した一意なアセットパス。
    for reference in sorted(set(scene_asset_strings(scene))):
        # target: Scene参照先の実体パス。
        target = (asset_directory / reference).resolve()
        # scene_location: Project相対のSceneファイル名。
        scene_location = str(scene_file.relative_to(source))
        # Sceneからの参照が格納先外または欠落ならrejectします。
        if not is_within(target, asset_directory) or not target.is_file():
            findings.append(finding(
                "reject", "missing-scene-asset",
                f"{scene_location}が参照するアセット「{reference}」が見つかりません。",
                "アセットを追加するか、シーン内の参照を修正してください。",
            ))
        # Sceneからの参照が未選択ならrejectします。
        elif not asset_is_selected(target, included_roots):
            findings.append(finding(
                "reject", "unpackaged-scene-asset",
                f"{scene_location}が参照する「{reference}」はassetIncludePathsの対象外です。",
                "アセットをWebパッケージへ追加してください。",
            ))

    # material_files: 出力対象のMaterialファイル。
    material_files: list[Path] = []
    # root: Materialを探すアセットルート。
    for root in included_roots:
        # candidates: ルート内の候補ファイル。
        candidates = [root] if root.is_file() else root.rglob("*.material.json")
        # path: 対象拡張子に一致するMaterial。
        material_files.extend(
            path.resolve() for path in candidates
            if path.is_file() and path.name.lower().endswith(".material.json")
        )
    # 重複を除いたMaterialを検査します。
    for material_file in dict.fromkeys(material_files):
        # location: Project相対のMaterialパス。
        location = str(material_file.relative_to(source))
        # material: 読み込んだMaterial設定。
        try:
            # material: JSONから復元したMaterial。
            material = json.loads(material_file.read_text(encoding="utf-8"))
        # 壊れたMaterialはこの検査では対象外です。
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        # 未対応形式のMaterialをrejectします。
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
        # field: Materialが参照する標準Texture欄。
        for field in (
            "albedoTexture", "normalTexture", "roughnessTexture",
            "metallicTexture", "occlusionTexture", "emissiveTexture",
        ):
            # reference: 欄に指定されたTextureパス。
            reference = material.get(field, "")
            # 空欄は参照なしとして扱います。
            if not isinstance(reference, str) or not reference:
                continue
            # target: Texture参照先の実体パス。
            target = (asset_directory / reference).resolve()
            # 欠落または格納先外のTextureをrejectします。
            if not is_within(target, asset_directory) or not target.is_file():
                findings.append(finding(
                    "reject", "missing-material-asset",
                    f"マテリアル「{location}」が参照する{field}「{reference}」が見つかりません。",
                    "テクスチャを追加するか、マテリアル内の参照を修正してください。",
                ))
            # 未選択Textureへの参照をrejectします。
            elif not asset_is_selected(target, included_roots):
                findings.append(finding(
                    "reject", "unpackaged-material-asset",
                    f"マテリアル「{location}」が参照する「{reference}」はassetIncludePathsの対象外です。",
                    "テクスチャをWebパッケージへ追加してください。",
                ))
        # shader: Materialに指定されたシェーダー。
        shader = material.get("shader", "")
        # 明示シェーダーのWeb対応を検査します。
        if isinstance(shader, str) and shader:
            # 標準シェーダーはWebGL組み込み実装へ置換します。
            if Path(shader).name.lower() == "lamaponlit.hlsl":
                findings.append(finding(
                    "auto", "standard-shader-backend-replacement",
                    f"マテリアル「{location}」にはLamaPonLitが設定されています。WebGLでは対応する組み込みGLSLバックエンドを使用します。",
                    "プロジェクトの変更は不要です。",
                ))
            # HLSLカスタムシェーダーはWeb出力でrejectします。
            else:
                findings.append(finding(
                    "reject", "unsupported-custom-material-shader",
                    f"マテリアル「{location}」はカスタムHLSLシェーダー「{shader}」を使用しています。",
                    "ポータブル版のシェーダーグラフまたはGLSLバックエンドを用意するか、Webでは標準のLamaPonLitマテリアルを使用してください。",
                ))
        # custom_textures: カスタムTexture欄の設定。
        custom_textures = material.get("customTextures", [])
        # カスタムTexture欄の使用をrejectします。
        if isinstance(custom_textures, list) and any(custom_textures):
            findings.append(finding(
                "reject", "unsupported-custom-material-textures",
                f"マテリアル「{location}」はカスタムシェーダーのテクスチャスロットを使用しています。",
                "カスタムテクスチャスロットには、ポータブル版のカスタムシェーダーバックエンドが必要です。",
            ))
        # custom_parameters: カスタムShader値の設定。
        custom_parameters = material.get("customParameters", [])
        # 既定値以外のカスタム値をrejectします。
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

    # animation_files: 出力対象のAnimationファイル。
    animation_files: list[Path] = []
    # root: Animationを探すアセットルート。
    for root in included_roots:
        # candidates: ルート内の候補ファイル。
        candidates = [root] if root.is_file() else root.rglob("*.animation.json")
        # path: 対象拡張子に一致するAnimation。
        animation_files.extend(
            path.resolve() for path in candidates
            if path.is_file() and path.name.lower().endswith(".animation.json")
        )
    # 重複を除いたAnimationを検査します。
    for animation_file in dict.fromkeys(animation_files):
        # location: Project相対のAnimationパス。
        location = str(animation_file.relative_to(source))
        # animation: 読み込んだAnimation設定。
        try:
            # animation: JSONから復元したAnimation。
            animation = json.loads(animation_file.read_text(encoding="utf-8"))
            # keyframes: Animationのキーフレーム一覧。
            keyframes = animation.get("keyframes", [])
            # duration: Animation全体の再生秒数。
            duration = animation.get("duration", 0.0)
            # times: 辞書形式のキーフレーム時刻。
            times = [
                keyframe.get("time")
                for keyframe in keyframes
                if isinstance(keyframe, dict)
            ]
            # valid_vectors: 各キーフレームの座標成分検証。
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
            # valid: Web Animation形式の総合検証結果。
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
        # invalid形式はreject対象として扱います。
        except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
            # valid: 読込・構文検証に失敗。
            valid = False
        # 無効なAnimationをrejectします。
        if not valid:
            findings.append(finding(
                "reject", "invalid-portable-animation",
                f"アニメーション「{location}」は有効なLamaPonAnimationClip version 1形式ではありません。",
                "互換性のあるLamaPon Editorでクリップを開き、保存し直してください。",
            ))

    # objects: Sceneに保存されたGameObject一覧。
    objects = scene.get("objects", [])
    # Object一覧が配列でないSceneは検査を終えます。
    if not isinstance(objects, list):
        findings.append(finding(
            "reject", "invalid-scene-objects",
            "シーンの「objects」は配列である必要があります。",
            "シーンを修復するか保存し直してください。",
        ))
        return findings
    # object_ids: 一意性を確認したObject ID。
    object_ids: set[int] = set()
    # parent_links: Object名と親ID。
    parent_links: list[tuple[str, Any]] = []
    # component_links: Object名と参照Component。
    component_links: list[tuple[str, str, Any]] = []
    # scene_scripts: Sceneが参照するScript名。
    scene_scripts: set[str] = set()
    # cameras: 有効なCameraを持つObject ID。
    cameras: set[int] = set()
    # index/object_value: Scene Objectの位置と設定。
    for index, object_value in enumerate(objects):
        # 不正なObject値をrejectします。
        if not isinstance(object_value, dict):
            findings.append(finding(
                "reject", "invalid-scene-object",
                f"シーンオブジェクト#{index}はオブジェクト形式ではありません。",
                "シーンを修復するか保存し直してください。",
            ))
            continue
        # object_id: Scene内のObject識別子。
        object_id = object_value.get("id")
        # name: 診断に使うObject表示名。
        name = str(object_value.get("name", f"#{index}"))
        # 欠落または重複IDをrejectします。
        if not isinstance(object_id, int) or object_id in object_ids:
            findings.append(finding(
                "reject", "invalid-scene-object-id",
                f"シーンオブジェクト「{name}」の整数IDがないか、ほかのオブジェクトと重複しています。",
                "シーンを修復するか保存し直してください。",
            ))
        # 一意なIDをScene参照検査へ登録します。
        else:
            object_ids.add(object_id)
        # parent設定があるObjectの参照を記録します。
        if object_value.get("parent") is not None:
            parent_links.append((name, object_value.get("parent")))
        # components: Objectに設定されたComponent一覧。
        components = object_value.get("components", [])
        # Component一覧が配列でないObjectをrejectします。
        if not isinstance(components, list):
            findings.append(finding(
                "reject", "invalid-scene-components",
                f"シーンオブジェクト「{name}」のcomponentsは配列である必要があります。",
                "シーンを修復するか保存し直してください。",
            ))
            continue
        # component: Objectに設定された各Component。
        for component in components:
            # component_type: ComponentのScene形式名。
            component_type = component.get("type") if isinstance(component, dict) else None
            # required_module: Componentが必要とするModule。
            required_module = PORTABLE_SCENE_COMPONENTS.get(str(component_type))
            # ポータブル対応Componentが未登録ならrejectします。
            if required_module is None:
                # known_native: エンジン内で認識されている型か。
                known_native = str(component_type) in KNOWN_NATIVE_SCENE_COMPONENTS
                findings.append(finding(
                    "reject",
                    "unsupported-scene-component",
                    f"シーンオブジェクト「{name}」は、{'既知のネイティブ' if known_native else '未登録の'}コンポーネント「{component_type}」を使用していますが、ポータブルランタイムに対応するバックエンドがありません。",
                    "Web版から削除するか、ポータブルランタイムへ実装してテストしてから出力してください。",
                ))
                continue
            # Scene Moduleが未選択ならrejectします。
            if required_module not in declared_modules:
                findings.append(finding(
                    "reject", "missing-scene-module",
                    f"シーンの「{name}」にある{component_type}コンポーネントには「{required_module}」モジュールが必要です。",
                    f"export.modulesに「{required_module}」を追加してください。",
                ))
            # no-op ComponentのWeb置換内容を案内します。
            if component_type in PORTABLE_NOOP_SCENE_COMPONENTS:
                # description: Web上で置き換える挙動。
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
            # 近似描画Componentの制約を案内します。
            if component_type in PORTABLE_APPROXIMATE_SCENE_COMPONENTS:
                findings.append(finding(
                    "warning", "scene-component-approximation",
                    f"シーンの「{name}」にある{component_type}コンポーネントには、Web版の簡易ライティングを使用します。",
                    "プレビューでライティングを確認してください。このプロファイルでは高度な影やネイティブシェーダーの動作を再現しません。",
                ))
                # 影描画が有効ならWeb非対応を警告します。
                if component.get("castsShadows", False):
                    findings.append(finding(
                        "warning", "unsupported-web-shadows",
                        f"シーンのライト「{name}」で影が有効ですが、webgl2-basic-3dでは描画できません。",
                        "Web版では影を無効にするか、影に対応したプロファイルを使用してください。",
                    ))
            # CameraのRenderTexture依存を検査します。
            if component_type == "Camera":
                # RenderTextureへの描画はWeb基本版で未対応です。
                if component.get("targetTexture"):
                    findings.append(finding(
                        "reject", "unsupported-camera-render-texture",
                        f"「{name}」のカメラはtargetTexture「{component.get('targetTexture')}」へ描画します。",
                        "基本WebプロファイルではメインCanvasのカメラを使用するか、ポータブル版のレンダーテクスチャバックエンドを追加してください。",
                    ))
            # MeshRendererの形状とShader設定を検査します。
            if component_type == "MeshRenderer":
                # 未対応のPrimitive形状をrejectします。
                if str(component.get("shape", "Cube")) not in {
                    "Cube", "Sphere", "Cylinder", "Plane",
                }:
                    findings.append(finding(
                        "reject", "unsupported-primitive-shape",
                        f"「{name}」のMeshRendererは、未対応のプリミティブ形状「{component.get('shape')}」を使用しています。",
                        "Cube、Sphere、Cylinder、Plane、またはModelRendererを使用してください。",
                    ))
                # shader: MeshRendererのShaderパス。
                shader = component.get("shader", "")
                # 標準以外のShaderをrejectします。
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
                # mesh_custom_textures: カスタムTexture割り当て。
                mesh_custom_textures = [
                    value for key, value in component.items()
                    if key.startswith("customTexture")
                    and isinstance(value, str) and value
                ]
                # mesh_custom_parameters: カスタムShader値。
                mesh_custom_parameters = component.get("customParameters", [])
                # カスタムShader設定があるMeshをrejectします。
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
                # Web非対応のworldOverlayをrejectします。
                if component.get("worldOverlay", False):
                    findings.append(finding(
                        "reject", "unsupported-world-overlay",
                        f"「{name}」のMeshRendererでworldOverlayが有効です。",
                        "worldOverlayを無効にするか、ポータブル版のオーバーレイパスを追加してください。",
                    ))
            # ModelRendererのWeb描画制約を検査します。
            if component_type == "ModelRenderer":
                # ワイヤーフレーム描画はWeb基本版で未対応です。
                if component.get("wireframe", False):
                    findings.append(finding(
                        "reject", "unsupported-model-wireframe",
                        f"「{name}」のModelRendererでワイヤーフレーム描画が有効です。",
                        "Web版ではワイヤーフレームを無効にするか、ポータブル版のライン描画バックエンドを追加してください。",
                    ))
                # controller: ModelRendererのAnimator Controller名。
                controller = component.get("animationController", "")
                # State Machine ControllerはWeb未対応です。
                if isinstance(controller, str) and controller:
                    findings.append(finding(
                        "reject", "unsupported-animation-controller",
                        f"「{name}」のModelRendererはAnimator Controller「{controller}」を使用していますが、ステートマシンはポータブルランタイムで未対応です。",
                        "モデルに埋め込まれたアニメーション操作を使用するか、ポータブル版のAnimator Controllerランタイムを追加してください。",
                    ))
                # Root MotionはWeb基本版で未対応です。
                if component.get("applyRootMotion", False):
                    findings.append(finding(
                        "reject", "unsupported-root-motion",
                        f"「{name}」のModelRendererでアニメーションのルートモーションが有効です。",
                        "ルートモーションを無効にするか、ポータブル版のルートモーション処理を追加してください。",
                    ))
                # shader: ModelRendererのShaderパス。
                shader = component.get("shader", "")
                # 標準以外のHLSL Shaderをrejectします。
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
                # Shader KeywordはWeb基本版で未対応です。
                if component.get("shaderKeywords"):
                    findings.append(finding(
                        "reject", "unsupported-model-shader-keywords",
                        f"「{name}」のModelRendererでカスタムシェーダーキーワードが有効です。",
                        "シェーダーバリアントには、ポータブル版のシェーダーバックエンドが必要です。",
                    ))
                # Legacy ShadingはWeb基本版で再現できません。
                if component.get("useLegacyShading", False):
                    findings.append(finding(
                        "reject", "unsupported-legacy-model-shading",
                        f"「{name}」のModelRendererは、Web版のPBRバックエンドで再現できない従来のネイティブシェーディングを使用しています。",
                        "LamaPonLit PBRを使用するか、対応するポータブルバックエンドを追加してください。",
                    ))
                # 埋め込みMaterial色の保持設定は未対応です。
                if component.get("preserveEmbeddedMaterialColor", False):
                    findings.append(finding(
                        "reject", "unsupported-preserve-material-color",
                        f"「{name}」のModelRendererでは、オーバーライド時に埋め込みマテリアルの色を保持する設定が有効です。",
                        "Web版へこの設定を実装するまでは、必要なベースカラーをマテリアルへ反映してください。",
                    ))
                # custom_textures: カスタムTexture割り当て。
                custom_textures = [
                    value for key, value in component.items()
                    if key.startswith("customTexture")
                    and isinstance(value, str) and value
                ]
                # custom_parameters: カスタムShader値。
                custom_parameters = component.get("customParameters", [])
                # カスタムBindingを持つModelをrejectします。
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
            # AudioSourceのWeb再生制約を検査します。
            if component_type == "AudioSource":
                # 空間音響は距離減衰とパンで近似します。
                if component.get("spatial", False):
                    findings.append(finding(
                        "warning", "web-spatial-audio-approximation",
                        f"「{name}」のAudioSourceで3D空間音響が有効です。",
                        "Web Audioの距離減衰とパンを使用します。ブラウザーのプレビューで、リスナー位置と減衰範囲を確認してください。",
                    ))
                # ストリーミング音声はバッファ再生へ置換します。
                if component.get("streaming", False):
                    findings.append(finding(
                        "warning", "web-audio-buffered-stream",
                        f"「{name}」のAudioSourceでストリーミング再生が有効です。",
                        "基本Webプロファイルでは、パッケージ内の音声を再生前にメモリへデコードします。",
                    ))
            # ParticleSystemのWeb描画制約を検査します。
            if component_type == "ParticleSystem":
                # shape: Particle emitterの形状名。
                shape = str(component.get("shape", "Point"))
                # 近似対象外のEmitter形状を警告します。
                if shape not in {"Point", "Cone", "Sphere", "Box"}:
                    findings.append(finding(
                        "warning", "web-particle-shape-approximation",
                        f"「{name}」のParticleSystemはエミッター形状「{shape}」を使用しています。",
                        "基本WebプロファイルではPointエミッターへ置き換えます。プレビューで効果を確認してください。",
                    ))
                # 未対応Particle描画Modeをrejectします。
                if str(component.get("renderMode", "Billboard")) not in {
                    "Billboard", "Horizontal",
                }:
                    findings.append(finding(
                        "reject", "unsupported-particle-render-mode",
                        f"「{name}」のParticleSystemは、未対応の描画モード「{component.get('renderMode')}」を使用しています。",
                        "BillboardまたはHorizontalを使用してください。",
                    ))
                # particle_shader: Particle用Shaderパス。
                particle_shader = component.get("shader", "")
                # auxiliary_texture: Particle補助Texture。
                auxiliary_texture = component.get("auxiliaryTexture", "")
                # custom_parameters: Particle Shader値。
                custom_parameters = component.get("customParameters", [])
                # カスタムShader設定のあるParticleをrejectします。
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
            # BoxCollider3Dは簡易AABB物理を警告します。
            if component_type == "BoxCollider3D":
                findings.append(finding(
                    "warning", "web-basic-box-physics",
                    f"「{name}」のBoxCollider3Dには、決定論的なAABB方式のブラウザー物理バックエンドを使用します。",
                    "回転、摩擦の合成方法、連続衝突は近似されます。プレビューでゲームプレイを確認してください。",
                ))
            # MeshCollider3Dの未対応をrejectします。
            if component_type == "MeshCollider3D":
                findings.append(finding(
                    "reject", "unsupported-scene-mesh-collider",
                    f"「{name}」のMeshCollider3Dは、ポータブルシーンローダーでモデルから衝突用三角形をまだ生成できません。",
                    "このWebプロファイルではBoxCollider3Dを使用するか、ポータブル版のメッシュコライダー用デコーダーを追加してください。",
                ))
            # RigidbodyのWeb物理設定を検査します。
            if component_type == "Rigidbody":
                # advanced_rigidbody: 基本Solver非対応設定の有無。
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
                # 高度なRigidbody設定をrejectします。
                if advanced_rigidbody:
                    findings.append(finding(
                        "reject", "unsupported-advanced-rigidbody",
                        f"「{name}」のRigidbodyは、基本Web物理バックエンドでまだ保持できない質量、抗力、角速度、重心、拘束、または連続衝突の設定を使用しています。",
                        "速度、重力、キネマティックだけを使うか、これらの設定に対応するポータブル版Rigidbodyソルバーを追加してください。",
                    ))
                # 親付きRigidbodyは簡易Solverで未対応です。
                if object_value.get("parent") is not None:
                    findings.append(finding(
                        "reject", "unsupported-parented-rigidbody",
                        f"「{name}」のRigidbodyには親がありますが、基本AABBソルバーはルート空間のワールド軸で接触を解決します。",
                        "このRigidbodyをルートGameObjectへ移動するか、親子Transformに対応した物理バックエンドを実装してください。",
                    ))
            # InputMoverのAction割当を検査します。
            if component_type == "InputMover":
                # field/fallback: Action欄と未設定時の名前。
                for field, fallback in (
                    ("horizontalAction", "MoveHorizontal"),
                    ("verticalAction", "MoveVertical"),
                ):
                    # action: Componentに指定された入力名。
                    action = component.get(field, fallback)
                    # Web入力mapにないActionをrejectします。
                    if action not in available_input_actions:
                        findings.append(finding(
                            "reject", "unsupported-input-action",
                            f"「{name}」のInputMoverは、基本Webバインドにないアクション「{action}」を使用しています。",
                            "対応する入力アクションを使用するか、キーボード、ゲームパッド、タッチの割り当てを追加してください。",
                        ))
            # SpriteAnimatorのシートとClipを検査します。
            if component_type == "SpriteAnimator":
                # columns: シート列数。
                columns = component.get("columns", 1)
                # rows: シート行数。
                rows = component.get("rows", 1)
                # clips: Sprite Animation定義。
                clips = component.get("clips", [])
                # 不正なグリッドまたはClipをrejectします。
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
            # Parallax参照先をObject検査へ登録します。
            if component_type == "ParallaxLayer":
                # reference: ParallaxLayerの参照ID。
                reference = component.get("referenceId", 0)
                # 有効な参照IDだけ後段で存在を確認します。
                if reference not in (0, None):
                    component_links.append((name, "ParallaxLayer", reference))
            # SpriteRendererのTextureとShader制約を検査します。
            if component_type == "SpriteRenderer":
                # RenderTextureを使うSpriteRendererをrejectします。
                if component.get("renderTexture"):
                    findings.append(finding(
                        "reject", "unsupported-sprite-render-texture",
                        f"「{name}」のSpriteRendererはrenderTexture「{component.get('renderTexture')}」を表示します。",
                        "画像アセットを使用するか、ポータブル版のレンダーテクスチャ読み込みを追加してください。",
                    ))
                # sprite_shader: SpriteRendererのShaderパス。
                sprite_shader = component.get("shader", "")
                # sprite_parameters: カスタムShader値。
                sprite_parameters = component.get("customParameters", [])
                # カスタムShaderを使うSpriteRendererをrejectします。
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
            # TextRendererのフォント設定を検査します。
            if component_type == "TextRenderer":
                # font_asset: パッケージするFontファイル。
                font_asset = component.get("fontAsset", "")
                # font_family: ブラウザーへ要求するFont名。
                font_family = str(component.get("fontFamily", "sans-serif"))
                # 配布Fontのない独自System Fontを警告します。
                if not font_asset and font_family.lower() not in {
                    "sans-serif", "serif", "monospace", "cursive", "fantasy",
                    "system-ui",
                }:
                    findings.append(finding(
                        "warning", "system-font-may-differ",
                        f"「{name}」のTextRendererは、fontAssetを指定せずにシステムフォント「{font_family}」を使用しています。",
                        "ブラウザー間で字形とレイアウトをそろえるには、TTF、OTF、WOFF、またはWOFF2をfontAssetとしてパッケージへ追加してください。",
                    ))
            # TransformAnimatorのController設定を検査します。
            if component_type == "TransformAnimator":
                # controller: Animator Controllerのパス。
                controller = component.get("controller", "")
                # 未対応Controllerの利用をrejectします。
                if isinstance(controller, str) and controller:
                    findings.append(finding(
                        "reject", "unsupported-transform-animator-controller",
                        f"「{name}」のTransformAnimatorはステートマシンコントローラー「{controller}」を使用しています。",
                        "基本WebプロファイルではLamaPonAnimationClipを直接使用するか、ポータブル版のコントローラーランタイムを追加してください。",
                    ))
            # 有効なCameraだけScene参照検査へ登録します。
            if (
                component_type == "Camera"
                and isinstance(object_id, int)
                and object_value.get("enabled", True)
                and component.get("enabled", True)
            ):
                cameras.add(object_id)
            # NativeScriptのIDをScene参照検査へ登録します。
            if component_type == "NativeScript":
                # script_id: Componentに指定されたScript名。
                script_id = component.get("script", "")
                # Script IDが空または不正ならrejectします。
                if not isinstance(script_id, str) or not script_id:
                    findings.append(finding(
                        "reject", "invalid-scene-script",
                        f"「{name}」のNativeScriptにスクリプトIDがありません。",
                        "登録済みのスクリプトをコンポーネントへ割り当ててください。",
                    ))
                # 有効IDをScene参照検査へ登録します。
                else:
                    scene_scripts.add(script_id)
    # name/parent: 親参照元のObject名と親ID。
    for name, parent in parent_links:
        # 存在しない親Objectへの参照をrejectします。
        if parent not in object_ids:
            findings.append(finding(
                "reject", "missing-scene-parent",
                f"シーンオブジェクト「{name}」が、存在しない親ID {parent}を参照しています。",
                "シーン階層を修復してください。",
            ))
    # name/component_type/target: Component参照の検査情報。
    for name, component_type, target in component_links:
        # 存在しないObjectへのComponent参照をrejectします。
        if target not in object_ids:
            findings.append(finding(
                "reject", "missing-component-reference",
                f"「{name}」の{component_type}が、存在しないオブジェクトID {target}を参照しています。",
                "Web出力を行う前にコンポーネントの参照を修復してください。",
            ))
    # main_camera: Sceneで指定された主Camera ID。
    main_camera = scene.get("mainCamera")
    # 3D Moduleに有効な主Cameraがなければrejectします。
    if "renderer3d" in declared_modules and main_camera not in cameras:
        findings.append(finding(
            "reject", "invalid-main-camera",
            "シーンのmainCameraが、有効なCameraコンポーネントを参照していません。",
            "Web出力を行う前に、有効なメインカメラを割り当ててください。",
        ))
    # script_id: 登録漏れを確認するScene Script名。
    for script_id in sorted(scene_scripts - registered_scripts):
        findings.append(finding(
            "reject", "unregistered-scene-script",
            f"シーンはスクリプト「{script_id}」を参照していますが、選択したWebソースにはLAMAPON_SCRIPT_NAMEDによる登録がありません。",
            "C++ソースをexport.web.sourcesへ追加するか、シーンのスクリプトIDを修正してください。",
        ))

    # environment: Sceneの環境効果設定。
    environment = scene.get("environment", {})
    # 環境効果の設定がオブジェクト形式か確認します。
    if isinstance(environment, dict):
        # key/label: 検査する効果名と表示名。
        for key, label in WEB_UNSUPPORTED_ENVIRONMENT_EFFECTS.items():
            # effect: Sceneに保存された効果設定。
            effect = environment.get(key, {})
            # 有効な非対応効果を警告します。
            if isinstance(effect, dict) and effect.get("enabled", False):
                findings.append(finding(
                    "warning", "unsupported-environment-effect",
                    f"シーンで{label}が有効ですが、webgl2-basic-3dでは描画できません。",
                    "Web版ではこの効果を自動的に無効にします。配布前にプレビューを確認してください。",
                ))

    # ワールド行列の再帰走査でブラウザーのスタックがあふれる前に、シーン階層の循環を検出します。
    # parents: Object IDから親IDへの対応表。
    parents = {
        value.get("id"): value.get("parent")
        for value in objects
        if isinstance(value, dict) and isinstance(value.get("id"), int)
    }
    # object_id: 親子循環を調べる開始Object ID。
    for object_id in parents:
        # visited: 現在の走査経路で訪問したObject ID。
        visited: set[int] = set()
        # current: 親参照をたどっているObject ID。
        current: Any = object_id
        # 親をたどり、循環またはルートで走査を終えます。
        while isinstance(current, int) and current in parents:
            # 再訪したIDがあれば親子循環です。
            if current in visited:
                findings.append(finding(
                    "reject", "cyclic-scene-hierarchy",
                    f"シーン階層のオブジェクトID {current}に親子関係の循環があります。",
                    "Web出力を行う前にシーン階層を修復してください。",
                ))
                break
            # 既訪問IDを記録して親へ進みます。
            visited.add(current)
            # current: 親Object IDへ走査を進めます。
            current = parents[current]

    # rejectがなければ適合完了findingを追加します。
    if not any(item["level"] == "reject" for item in findings):
        findings.append(finding(
            "auto", "portable-contract-complete",
            f"Web互換性チェックに合格しました（API型: {len(api_locations)}、シーンオブジェクト: {len(objects)}、参照アセット: {len(asset_references)}、登録スクリプト: {len(registered_scripts)}）。",
            "Emscriptenによるコンパイル確認を実行できます。",
        ))
    return findings


# validate_web_compatibility(source: Project root, project: 設定, profile_name: Web Profile, project_kind: 種別): Web互換性の診断を収集します。
def validate_web_compatibility(
    source: Path,
    project: dict[str, Any],
    profile_name: str,
    project_kind: str,
) -> list[dict[str, str]]:
    # source: 診断に使う実体プロジェクトパス。
    source = source.resolve()
    # profile: 選択されたWeb互換性設定。
    profile = WEB_PROFILES.get(profile_name)
    # 未登録プロファイルは設定誤りとして中止します。
    if profile is None:
        # known: 利用できるProfile名一覧。
        known = ", ".join(sorted(WEB_PROFILES))
        raise ExportError(
            f"Unknown Web compatibility profile {profile_name!r}; "
            f"known profiles: {known}."
        )

    # findings: 互換性診断の一覧。
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
    # export: Projectの出力設定。
    export = project.get("export", {})
    # export設定がObject形式でない場合は中止します。
    if not isinstance(export, dict):
        raise ExportError("Project export must be an object when specified.")
    # modules: Web用に解決したModule一覧。
    modules = resolved_project_modules(source, project, project_kind)
    # 通常Projectの検出方針を診断へ記録します。
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
    # allowed_modules: Profileが許可するModule。
    allowed_modules = profile["modules"]
    # unsupported_modules: Profile非対応Module。
    unsupported_modules = sorted(set(modules) - allowed_modules)
    # 非対応Moduleごとにreject理由を追加します。
    if unsupported_modules:
        # reasons: Module別の非対応理由。
        reasons = profile["module_reasons"]
        # module: Reject対象のModule名。
        for module in unsupported_modules:
            findings.append(
                finding(
                    "reject",
                    "unsupported-module",
                    f"モジュール「{module}」はWebプロファイル{profile_name!r}で使用できません: {reasons.get(module, 'このプロファイルに定義がありません')}",
                    "このモジュールのWeb用バックエンドを追加するか、Webターゲットから該当機能を削除してください。",
                )
            )

    # web: Project内のWeb出力設定。
    web = export.get("web", {})
    # web設定がObject形式でない場合は中止します。
    if not isinstance(web, dict):
        raise ExportError("Project export.web must be an object when specified.")
    # ポータブルランタイム要件を診断へ追加します。
    findings.extend(validate_portable_contract(
        source,
        project,
        project_kind,
        web,
        modules,
    ))
    # excluded_values: 利用者指定の除外パス。
    excluded_values = web.get("scanExcludePaths", [])
    # 除外値は空でない文字列の配列に限定します。
    if not isinstance(excluded_values, list) or any(
        not isinstance(item, str) or not item for item in excluded_values
    ):
        raise ExportError("export.web.scanExcludePaths must be a string list.")
    # excluded: 実体パスへ解決した除外先。
    excluded = [(source / item).resolve() for item in excluded_values]
    # source_hits: 拒否対象APIの検出結果。
    source_hits: list[str] = []
    # warnings: ブラウザー差の診断。
    warnings: list[dict[str, str]] = []
    # automatic_exclusions: 自動除外するPlatform入口。
    automatic_exclusions: list[Path] = []
    # ignored_directories: ソース走査から除くDirectory。
    ignored_directories = {".git", ".lamapon", "CMakeFiles", "build", "third_party"}
    # path: Project内の候補ソース。
    for path in source.rglob("*"):
        # ソース形式以外のファイルを飛ばします。
        if not path.is_file() or path.suffix.lower() not in SOURCE_EXTENSIONS:
            continue
        # 除外Directory内のソースを飛ばします。
        if any(part in ignored_directories for part in path.relative_to(source).parts):
            continue
        # resolved: 物理パスへ解決したソース。
        resolved = path.resolve()
        # Windows専用の入口ファイルを自動除外します。
        if path.name.lower() in DEFAULT_PLATFORM_SOURCE_NAMES:
            automatic_exclusions.append(path.relative_to(source))
            continue
        # 利用者指定の除外対象を飛ばします。
        if source_is_excluded(resolved, excluded):
            continue
        # lines: 読み込んだソースの行一覧。
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        # line_number/line: 検査対象の位置と内容。
        for line_number, line in enumerate(lines, start=1):
            # token/description: 禁止APIと診断名。
            for token, description in FORBIDDEN_SOURCE_TOKENS.items():
                # tokenを含む行を依存関係候補として調べます。
                if token in line:
                    # 許可済みDirectX型だけなら検出から除外します。
                    if token == "DirectX::" and "DirectX::" not in (
                        PORTABLE_DIRECTX_TOKEN.sub("", line)
                    ):
                        continue
                    source_hits.append(
                        f"{path.relative_to(source)}:{line_number}: "
                        f"{description} ({token})"
                    )
            # token/message: 警告APIと差異説明。
            for token, message in SOURCE_WARNINGS.items():
                # 対象APIの行を警告診断にします。
                if token in line:
                    warnings.append(
                        finding(
                            "warning",
                            "browser-runtime-difference",
                            f"{path.relative_to(source)}:{line_number}: {message}",
                            "可能な場合はLamaPonのプラットフォームサービスを使用してください。",
                        )
                    )
    # ネイティブ依存関係の検出結果をrejectへ変換します。
    if source_hits:
        # hit: 表示上限までの依存箇所。
        for hit in source_hits[:32]:
            findings.append(
                finding(
                    "reject",
                    "native-source-dependency",
                    f"Webソースがネイティブ専用の依存関係を使用しています: {hit}",
                    "ポータブル版のLamaPon APIへ置き換えるか、ターゲット専用バックエンドの内部へ移してください。",
                )
            )
        # 上限を超えた依存箇所があれば件数を追加します。
        if len(source_hits) > 32:
            findings.append(
                finding(
                    "reject",
                    "native-source-dependency-truncated",
                    f"ほかに{len(source_hits) - 32}件のネイティブ専用依存関係が見つかりました。",
                    "Web出力の診断にあるソース検査結果を開き、残りの依存関係も移植してください。",
                )
            )
    # 収集したブラウザー差の警告を追加します。
    findings.extend(warnings)
    # 自動除外した入口を診断へ記録します。
    if automatic_exclusions:
        # names: 除外したPlatform入口の表示名。
        names = ", ".join(str(path) for path in automatic_exclusions)
        findings.append(
            finding(
                "auto",
                "platform-entrypoint",
                f"Web版ではWindows用エントリーポイントを除外します: {names}。",
                "ブラウザー用エントリーポイントはWebバックエンドが提供します。",
            )
        )

    # asset_selection: アセット格納先と選択ルート。
    asset_selection = web_asset_roots(source, web)
    # アセット指定がない場合はソース診断を返します。
    if asset_selection is None:
        return findings
    # asset_directory/included_asset_roots: 出力対象アセット情報。
    asset_directory, included_asset_roots = asset_selection
    # supported_extensions: Profile対応の拡張子。
    supported_extensions = profile["asset_extensions"]
    # asset_files: Web出力で選択されたファイル。
    asset_files: list[Path] = []
    # root: 選択済みアセットの検索起点。
    for root in included_asset_roots:
        # 単一ファイルとDirectoryを別々に収集します。
        if root.is_file():
            asset_files.append(root)
        else:
            asset_files.extend(path for path in root.rglob("*") if path.is_file())
    # 重複ファイルと別名パスを整理します。
    asset_files = list(dict.fromkeys(path.resolve() for path in asset_files))
    # path: 拡張子と内容を検査するアセット。
    for path in asset_files:
        # 対応形式の破損・サイズを検査します。
        if (
            path.suffix.lower() in supported_extensions
            and path.suffix.lower() != ".txt"
        ):
            findings.extend(validate_asset_integrity(path, source))
    # convertible_assets: 変換が必要なアセット。
    convertible_assets = [
        path for path in asset_files
        if path.suffix.lower() in WEB_ASSET_CONVERSIONS
    ]
    # conversion_groups: 種別と実行形式ごとの変換対象。
    conversion_groups: dict[tuple[str, str], list[Path]] = {}
    # path: 変換対象にまとめるアセット。
    for path in convertible_assets:
        # kind/runtime_format: 変換種別と出力形式。
        kind, runtime_format = WEB_ASSET_CONVERSIONS[path.suffix.lower()]
        conversion_groups.setdefault((kind, runtime_format), []).append(path)
        # 空ファイルは変換できないためrejectします。
        if path.stat().st_size == 0:
            findings.append(finding(
                "reject", "empty-convertible-asset",
                f"アセット「{path.relative_to(source)}」が空のため変換できません。",
                "空のアセットを置き換えるか削除してください。",
            ))
    # kind/runtime_format/paths: 変換設定ごとのアセット群。
    for (kind, runtime_format), paths in sorted(conversion_groups.items()):
        # converter: 対応する変換ツール設定。
        converter = web_asset_converter(web, kind)
        # kind_label: 診断に表示するアセット種別。
        kind_label = WEB_ASSET_KIND_LABELS.get(kind, kind)
        # extensions: 変換対象の拡張子一覧。
        extensions = ", ".join(sorted({path.suffix.lower() for path in paths}))
        # converter未設定なら不足をrejectします。
        if converter is None:
            # setting_name: 設定ファイルのToolキー。
            setting_name, _ = WEB_ASSET_CONVERTER_SETTINGS[kind]
            findings.append(finding(
                "reject", "missing-asset-converter",
                f"{len(paths)}件の{kind_label}アセット（{extensions}）を{runtime_format.upper()}へ変換する必要がありますが、変換ツールがありません。",
                f"Hubの{kind_label}変換モジュールをインストールするか、export.web.converterTools.{setting_name}を設定してください。",
            ))
        # converter設定済みなら自動変換を案内します。
        else:
            findings.append(finding(
                "auto", "asset-format-conversion",
                f"{len(paths)}件の{kind_label}アセット（{extensions}）を「{converter.name}」でWeb用の{runtime_format.upper()}へ変換します。",
                "元のプロジェクトファイルと仮想アセットパスは変更しません。",
            ))
    # unsupported_assets: 対応・変換・無視対象外のアセット。
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
    # 非対応アセットを拡張子別に集計します。
    if unsupported_assets:
        # by_extension: 拡張子別の未対応件数。
        by_extension: dict[str, int] = {}
        # path: 拡張子を集計する未対応アセット。
        for path in unsupported_assets:
            # extension: 判定結果に使う拡張子。
            extension = path.suffix.lower() or "(no extension)"
            by_extension[extension] = by_extension.get(extension, 0) + 1
        # extension/count: 拡張子と未対応件数。
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
    # 通常Project用の生成Runtime案内を追加します。
    if project_kind == "lamapon-project":
        findings.append(finding(
            "auto",
            "generated-web-runtime-target",
            "通常のLamaPonプロジェクトを変更せず、ポータブル版のEmscriptenターゲットを生成します。",
            "生成したターゲット、変換済みアセット、出力は、指定したWebビルド／出力フォルダー内に保存します。",
        ))
    return findings


# print_compatibility_report(findings: 診断一覧): Web互換性の診断を表示します。
def print_compatibility_report(findings: list[dict[str, str]]) -> None:
    print("Web compatibility report:")
    # item: 表示する互換性診断。
    for item in findings:
        print(f"  [{item['level'].upper()}] {item['message']}")
        print(f"           {item['action']}")


# is_within(path: 対象パス, parent: 許可する親パス): pathがparentの内側にあるか判定します。
def is_within(path: Path, parent: Path) -> bool:
    # 親パスとの相対化に失敗すれば範囲外です。
    try:
        path.relative_to(parent)
        return True
    # relative_toが示す範囲外結果をFalseにします。
    except ValueError:
        return False


# write_compatibility_report(output_directory: 保存先, project: 設定, project_kind: 種別, profile_name: Web Profile, status: 結果, findings: 診断一覧): 互換性レポートを保存します。
def write_compatibility_report(
    output_directory: Path,
    project: dict[str, Any],
    project_kind: str,
    profile_name: str,
    status: str,
    findings: list[dict[str, str]],
) -> Path:
    output_directory.mkdir(parents=True, exist_ok=True)
    # report_path: 保存する診断JSONのパス。
    report_path = output_directory / "web-compatibility-report.json"
    # summary: 診断Level別の件数。
    summary = {
        level: sum(item["level"] == level for item in findings)
        for level in ("auto", "info", "warning", "reject")
    }
    # export: Projectの出力設定。
    export = project.get("export", {})
    # web: Web用設定または空設定。
    web = export.get("web", {}) if isinstance(export, dict) else {}
    # strict_portable_contract: 厳格な契約検査が有効か。
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


# sha256_file(path: ファイルパス): ファイルのSHA-256を返します。
def sha256_file(path: Path) -> str:
    # digest: ファイル内容のSHA-256。
    digest = hashlib.sha256()
    # stream: 読み取り中のBinary file。
    with path.open("rb") as stream:
        # chunk: 一度に読み込むファイル断片。
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


# source_fingerprint(source: Project root): Build出力を除くソースとアセットをHash化します。
def source_fingerprint(source: Path) -> str:
    # digest: 相対パスとファイル内容のSHA-256。
    digest = hashlib.sha256()
    # ignored_directories: Fingerprintから外す生成Directory。
    ignored_directories = {
        ".git",
        ".lamapon",
        "CMakeFiles",
        "build",
        "third_party",
    }
    # paths: Hash対象として選択したProject内ファイル。
    paths = [
        path
        for path in source.rglob("*")
        if path.is_file()
        and not any(
            part in ignored_directories
            for part in path.relative_to(source).parts
        )
    ]
    # path: Fingerprintへ加えるファイル。
    for path in sorted(paths, key=lambda item: item.as_posix()):
        # relative: Hashへ含めるProject相対パス。
        relative = path.relative_to(source).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "little"))
        digest.update(relative)
        # stream: 内容Hash用に開いたBinary file。
        with path.open("rb") as stream:
            # chunk: 一度に読み込むファイル断片。
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    return digest.hexdigest()


# require_relative_file(source: Project root, value: 相対パス, setting_name: 設定名): source内の必須ファイルを解決します。
def require_relative_file(
    source: Path,
    value: str,
    setting_name: str,
) -> Path:
    # 空または文字列以外の設定値を拒否します。
    if not isinstance(value, str) or not value:
        raise ExportError(f"{setting_name} must be a non-empty path string.")
    # path: Project内へ解決した設定ファイル。
    path = (source / value).resolve()
    # Project外へ抜ける相対パスを拒否します。
    if not is_within(path, source):
        raise ExportError(f"{setting_name} must stay inside the Web source.")
    # 存在しないファイルを拒否します。
    if not path.is_file():
        raise ExportError(f"{setting_name} was not found: {path}")
    return path


# cmake_bracket(value: CMake値): 値をCMakeの角括弧引数へ変換します。
def cmake_bracket(value: Path | str) -> str:
    # PathはCMake用のスラッシュ区切りへ変換します。
    if isinstance(value, Path):
        # CMakeの角括弧引数はバックスラッシュを保持しますが、値はadd_executableなどで再度解析されます。
        # Windowsのドライブパスが2回目の解析で\U形式のエスケープにならないよう、スラッシュ区切りに変換します。
        value = value.as_posix()
    return f"[==[{value}]==]"


# stage_portable_web_assets(source: Project root, web: Web設定, profile_name: Web Profile, generated_directory: 生成先): 選択アセットを変換してWeb用に配置します。
def stage_portable_web_assets(
    source: Path,
    web: dict[str, Any],
    profile_name: str,
    generated_directory: Path,
) -> Path | None:
    # asset_value: Project設定のアセットDirectory。
    asset_value = web.get("assetDirectory")
    # assetDirectory未指定ならアセット処理を省きます。
    if asset_value is None:
        return None
    # 空でない相対Directory文字列を要求します。
    if not isinstance(asset_value, str) or not asset_value:
        raise ExportError("export.web.assetDirectory must be a path string.")
    # asset_directory: Project内の実体アセットDirectory。
    asset_directory = (source / asset_value).resolve()
    # 存在しないアセットDirectoryを拒否します。
    if not asset_directory.is_dir():
        raise ExportError(f"Web asset directory was not found: {asset_directory}")
    # include_values: 出力する相対アセットパス。
    include_values = web.get("assetIncludePaths", ["."])
    # include指定は空でない文字列配列に限定します。
    if not isinstance(include_values, list) or not include_values or any(
        not isinstance(item, str) or not item for item in include_values
    ):
        raise ExportError("export.web.assetIncludePaths must be a string list.")
    # supported: Profileが直接出力できる拡張子。
    supported = WEB_PROFILES[profile_name]["asset_extensions"]
    # staging: 変換・梱包用の生成Directory。
    staging = generated_directory.parent / "web-generated-assets"
    # 以前の生成結果を除いて作り直します。
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)
    # copied: 処理したアセット数。
    copied = 0
    # converted: 変換結果のManifest項目。
    converted: list[dict[str, Any]] = []
    # seen_candidates: 重複処理を避ける実体パス。
    seen_candidates: set[Path] = set()
    # include_value: 追加する各相対アセットパス。
    for include_value in include_values:
        # include_path: Project内へ解決した指定先。
        include_path = (asset_directory / include_value).resolve()
        # Directory外または存在しない指定先を拒否します。
        if not is_within(include_path, asset_directory) or not include_path.exists():
            raise ExportError(
                f"Included Web asset path is invalid: {include_path}"
            )
        # ファイル指定とDirectory指定から候補を作ります。
        candidates = (
            [include_path]
            if include_path.is_file()
            else [path for path in include_path.rglob("*") if path.is_file()]
        )
        # candidate: stagingするProjectアセット。
        for candidate in candidates:
            # candidate: 別名を除いた実体ファイルパス。
            candidate = candidate.resolve()
            # 重複候補は一度だけ処理します。
            if candidate in seen_candidates:
                continue
            seen_candidates.add(candidate)
            # extension: 入力アセットの小文字拡張子。
            extension = candidate.suffix.lower()
            # conversion: 必要なWeb形式変換の設定。
            conversion = WEB_ASSET_CONVERSIONS.get(extension)
            # 非対応で変換定義もない形式は飛ばします。
            if extension not in supported and conversion is None:
                continue
            # destination: staging先の相対ファイルパス。
            destination = staging / candidate.relative_to(asset_directory)
            destination.parent.mkdir(parents=True, exist_ok=True)
            # 変換不要な対応形式はそのままコピーします。
            if conversion is None:
                shutil.copy2(candidate, destination)
            # 変換定義のある形式をWeb向けに出力します。
            else:
                # kind/runtime_format: 変換種別と出力形式。
                kind, runtime_format = conversion
                # converter: 設定済みの形式変換ツール。
                converter = web_asset_converter(web, kind)
                # 必須Converterが未設定なら出力を中止します。
                if converter is None:
                    # setting_name: 必要なConverter設定キー。
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
                # generated_model_images: GLBから分離した画像。
                generated_model_images: list[Path] = []
                # model_details: 検証済みGLBの要約情報。
                model_details: dict[str, int] | None = None
                # GLBの画像を外部化して構造を検証します。
                if kind == "model":
                    # image_converter: 埋め込み画像用Converter。
                    image_converter = web_asset_converter(web, "image")
                    # generated_model_images: GLBから分離した画像一覧。
                    generated_model_images = externalize_glb_images(
                        destination,
                        image_converter,
                    )
                    # model_details: 検証済みGLBの要約情報。
                    model_details = validate_portable_glb(destination)
                # conversion_entry: 変換結果のManifest情報。
                conversion_entry: dict[str, Any] = {
                    "path": candidate.relative_to(asset_directory).as_posix(),
                    "sourceFormat": extension.removeprefix("."),
                    "runtimeFormat": runtime_format,
                    "converter": converter.name,
                }
                # 取得できたGLB詳細をManifestへ記録します。
                if model_details is not None:
                    conversion_entry["details"] = model_details
                converted.append(conversion_entry)
                # generated_image: GLBから外部化した画像。
                for generated_image in generated_model_images:
                    converted.append({
                        "path": generated_image.relative_to(staging).as_posix(),
                        "sourceFormat": "embedded-model-image",
                        "runtimeFormat": "webp",
                        "converter": image_converter.name,
                    })
            copied += 1
    # 1件も選択されなければ出力を中止します。
    if copied == 0:
        raise ExportError("No portable Web assets were selected for packaging.")
    # 変換結果がある場合だけManifestを書き出します。
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
    # input_actions: Project固有の入力割当。
    input_actions = portable_project_input_actions(source)
    # 入力設定がある場合だけWeb用JSONを作成します。
    if input_actions:
        # portable_actions: Web対応Controlだけに絞った割当。
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


# generate_lamapon_web_target(source: Project root, project: 設定, target: CMake Target, modules: 選択Module, profile_name: Web Profile, generated_directory: 出力先): ポータブルWeb Targetを生成します。
def generate_lamapon_web_target(
    source: Path,
    project: dict[str, Any],
    target: str,
    modules: list[str],
    profile_name: str,
    generated_directory: Path,
) -> Path:
    # source: 生成設定に使う実体Projectパス。
    source = source.resolve()
    # export: Projectの出力設定。
    export = project.get("export", {})
    # export設定がObject形式でない場合は中止します。
    if not isinstance(export, dict):
        raise ExportError("Project export must be an object when specified.")
    # web: Web専用の生成設定。
    web = export.get("web", {})
    # web設定がObject形式でない場合は中止します。
    if not isinstance(web, dict):
        raise ExportError("Project export.web must be an object when specified.")

    # source_values: Project内のWeb用ソース指定。
    source_values = web.get("sources", ["main.cpp"])
    # ソース指定は空でない文字列配列に限定します。
    if not isinstance(source_values, list) or not source_values:
        raise ExportError("export.web.sources must be a non-empty string list.")
    # 空欄または文字列以外のSource指定を拒否します。
    if any(not isinstance(item, str) or not item for item in source_values):
        raise ExportError("Every export.web.sources entry must be a path string.")
    # source_files: 実在するWeb用ソースファイル。
    source_files = [
        require_relative_file(source, item, "export.web.sources entry")
        for item in source_values
    ]
    # C/C++以外のソースを拒否します。
    if any(path.suffix.lower() not in SOURCE_EXTENSIONS for path in source_files):
        raise ExportError("export.web.sources contains a non-C/C++ source file.")

    # shell_value: Projectが指定するHTML shell path。
    shell_value = web.get("shellFile")
    # shell_file: 指定shellまたはEngine標準shell。
    shell_file = (
        require_relative_file(source, shell_value, "export.web.shellFile")
        if shell_value is not None
        else ENGINE_ROOT / "src" / "LamaPon" / "Web" / "default-shell.html"
    )
    # 存在しないHTML shellを拒否します。
    if not shell_file.is_file():
        raise ExportError(f"Default Web shell was not found: {shell_file}")

    # asset_directory: Web用に準備したアセット先。
    asset_directory = stage_portable_web_assets(
        source,
        web,
        profile_name,
        generated_directory,
    )

    # output_name: Web成果物の共通名。
    output_name = web_artifact_prefix(source, project, "lamapon-web-target")
    # single_file: Runtimeを単一HTMLへまとめるか。
    single_file = web.get("singleFile", True)
    # singleFileは真偽値に限定します。
    if not isinstance(single_file, bool):
        raise ExportError("export.web.singleFile must be a boolean when specified.")
    # portable_game: ポータブルゲームRuntimeを生成するか。
    portable_game = web.get("portableGame", False)
    # portableGameは真偽値に限定します。
    if not isinstance(portable_game, bool):
        raise ExportError("export.web.portableGame must be a boolean when specified.")
    # game_name: HTMLとRuntimeへ渡すゲーム名。
    game_name = project.get("gameName", project.get("name", target))
    # ゲーム名は空でない文字列に限定します。
    if not isinstance(game_name, str) or not game_name:
        raise ExportError("The Web game name must be a non-empty string.")
    # scene_path: パッケージ内の起動Scene URL。
    scene_path = web.get("scenePath", "/assets/scenes/Main.scene.json")
    # 起動Scene URLの形式を検証します。
    if not isinstance(scene_path, str) or not scene_path.startswith("/assets/"):
        raise ExportError("export.web.scenePath must begin with '/assets/'.")

    generated_directory.mkdir(parents=True, exist_ok=True)
    # source_lines: CMakeのSOURCES引数。
    source_lines = "\n".join(
        ["    SOURCES"]
        + [f"        {cmake_bracket(path)}" for path in source_files]
    )
    # module_lines: CMakeのMODULES引数。
    module_lines = "\n".join(
        ["    MODULES"] + [f"        {module}" for module in modules]
    )
    # single_file_line: 単一HTML指定用のCMake行。
    single_file_line = "    SINGLE_FILE\n" if single_file else ""
    # portable_lines: ポータブルGame引数のCMake行。
    portable_lines = (
        "\n".join([
            "    PORTABLE_GAME",
            f"    GAME_NAME {cmake_bracket(game_name)}",
            f"    SCENE_PATH {cmake_bracket(scene_path)}",
        ])
        if portable_game
        else ""
    )
    # asset_line: staging済みアセットDirectoryのCMake行。
    asset_line = (
        f"    ASSET_DIRECTORY {cmake_bracket(asset_directory)}"
        if asset_directory is not None
        else ""
    )
    # cmake_path: 生成するWebターゲット設定ファイル。
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


# verify_web_artifacts(artifacts: 生成物, single_file: 出力形式, expected_prefix: 任意の名前): Web Packageの入口を検証します。
def verify_web_artifacts(
    artifacts: list[Path],
    single_file: bool,
    expected_prefix: str | None = None,
) -> list[dict[str, str]]:
    # checks: 成果物検証結果。
    checks: list[dict[str, str]] = []
    # html_files: ブラウザー入口となるHTML。
    html_files = [path for path in artifacts if path.suffix == ".html"]
    # HTML入口のない成果物を拒否します。
    if not html_files:
        raise ExportError("The Web package has no browser HTML entrypoint.")
    # 空ファイルを含む成果物を拒否します。
    if any(path.stat().st_size == 0 for path in artifacts):
        raise ExportError("The Web package contains an empty artifact.")
    checks.append({
        "code": "non-empty-artifacts",
        "status": "passed",
        "message": "Every generated Web artifact is non-empty.",
    })

    # html_path: 検証する各Browser入口。
    for html_path in html_files:
        # html: 置換後のHTML本文。
        html = html_path.read_text(encoding="utf-8", errors="replace")
        # lowered: 大文字小文字を無視する検索本文。
        lowered = html.lower()
        # 未展開Templateを残したHTMLを拒否します。
        if re.search(r"\{\{\{\s*SCRIPT\s*\}\}\}", html):
            raise ExportError(
                f"The HTML shell still contains an unexpanded template: "
                f"{html_path}"
            )
        # CanvasまたはScript RuntimeがないHTMLを拒否します。
        if "<canvas" not in lowered or "<script" not in lowered:
            raise ExportError(
                f"The HTML entrypoint has no Canvas/script runtime: {html_path}"
            )
    checks.append({
        "code": "html-entrypoint",
        "status": "passed",
        "message": "HTML shells are expanded and contain Canvas/script runtime.",
    })
    # 標準出力名を要求された場合にファイル名を確認します。
    if expected_prefix is not None:
        # expected_name: 期待される入口HTML名。
        expected_name = f"{expected_prefix}.html"
        # 名前やファイル数が合わない場合は実名を表示します。
        if len(html_files) != 1 or html_files[0].name != expected_name:
            # names: 見つかったHTML名。
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

    # external_runtime: HTML外に依存するRuntimeファイル。
    external_runtime = [
        path for path in artifacts if path.suffix in {".js", ".wasm", ".data"}
    ]
    # singleFileに外部Runtime依存があれば拒否します。
    if single_file and external_runtime:
        # names: 外部に見つかったRuntime名。
        names = ", ".join(path.name for path in external_runtime)
        raise ExportError(
            "singleFile output unexpectedly depends on external runtime "
            f"artifacts: {names}"
        )
    # 単一HTMLなら自己完結性を記録します。
    if single_file:
        checks.append({
            "code": "self-contained-html",
            "status": "passed",
            "message": "JavaScript, WebAssembly, and packaged assets are embedded.",
        })
    # 複数ファイル出力ではJavaScriptとWasmを要求します。
    else:
        # suffixes: 生成成果物の拡張子集合。
        suffixes = {path.suffix for path in artifacts}
        # JavaScriptまたはWasmの欠落を拒否します。
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


# write_export_manifest(output_directory: Package先, artifacts: 生成物, source: Project root, project: 設定, project_kind: 種別, profile_name: Web Profile, renderer: backend, modules: 選択Module, target: CMake Target, build_type: 構成, single_file: 出力形式, verification: 検証結果): Export Manifestを保存します。
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
    # manifest_path: 保存するExport Manifest。
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
                    # path: Hash情報を記録する成果物。
                    for path in sorted(
                        # item: ファイルとして実在する成果物。
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


# web_license_bundle(): Web Runtimeに同梱するLicense本文を読み込みます。
def web_license_bundle() -> str:
    # sources: ライセンス名とEngine内の相対パス。
    sources = {
        "LamaPon": "LICENSE",
        "nlohmann-json": "third_party/nlohmann/LICENSE.MIT",
        "cgltf": "third_party/cgltf/LICENSE.txt",
    }
    # sections: HTMLへ埋め込む許諾本文。
    sections = []
    # name/relative: 表示名とライセンスの検索先。
    for name, relative in sources.items():
        # path: source tree内のLicense候補。
        path = ENGINE_ROOT / relative
        # 配布SDKのfallback Licenseを使います。
        if not path.is_file():
            # path: License fallbackの実体パス。
            path = ENGINE_ROOT / "licenses" / f"{name}.txt"
        # Licenseが見つからない場合はPackagingを中止します。
        if not path.is_file():
            raise ExportError(f"Web runtime license text is missing: {name}")
        sections.append(name + "\n\n" + path.read_text(encoding="utf-8").strip())
    return "\n\n".join(sections)


# copy_web_package(build_directory: Build先, output_directory: Package先, artifact_prefix: 出力名, profile_name: Web Profile, findings: 診断一覧, project: 設定, project_kind: 種別, single_file: 出力形式): Browser Packageを配置して検証します。
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
    # build_directory: 成果物を読む実体Build先。
    build_directory = build_directory.resolve()
    # output_directory: Packageを書き込む実体出力先。
    output_directory = output_directory.resolve()
    # 単一HTMLを共有しても許諾本文が失われないよう、本体へ埋め込む。
    # 文字列をHTMLとして解釈させず、ゲーム画面には表示しない。
    license_notice = ('\n<pre id="lamapon-licenses" hidden>'
                      + html.escape(web_license_bundle()) + '</pre>\n')
    output_directory.mkdir(parents=True, exist_ok=True)
    # artifact_extensions全体: CMake Build成果物の許可拡張子。
    all_artifact_extensions = {
        ".html",
        ".js",
        ".wasm",
        ".data",
        ".mem",
        ".symbols",
        ".map",
    }
    # artifact_extensions: 出力形式に合わせたコピー対象拡張子。
    artifact_extensions = {".html"} if single_file else all_artifact_extensions
    # copied: 出力先へコピーしたファイル。
    copied: list[Path] = []
    # artifacts_found: HTML/JS/Wasmを検出したか。
    artifacts_found = False
    # build_artifact_names: Build Directory内のPackage成果物名。
    build_artifact_names = {
        candidate.name
        for candidate in build_directory.rglob("*")
        if candidate.is_file()
        and candidate.name.startswith(artifact_prefix)
        and candidate.suffix in artifact_extensions
    }
    # build_artifacts: 検証・コピーするBuild成果物。
    build_artifacts = [
        candidate
        for candidate in build_directory.rglob("*")
        if candidate.is_file()
        and candidate.name.startswith(artifact_prefix)
        and candidate.suffix in artifact_extensions
    ]
    # verification: HTMLとRuntime依存の検査結果。
    verification = verify_web_artifacts(
        build_artifacts,
        single_file,
        artifact_prefix,
    )
    # previous_manifest: 前回出力の成果物一覧。
    previous_manifest = output_directory / "web-export-manifest.json"
    # 前回Manifestがあれば古い成果物を整理します。
    if previous_manifest.is_file():
        # Manifestの破損時は空一覧として扱います。
        try:
            # previous_artifacts: 前回Manifestの成果物配列。
            previous_artifacts = json.loads(
                previous_manifest.read_text(encoding="utf-8")
            ).get("artifacts", [])
        # 読めないManifestは掃除対象の列挙に使いません。
        except (json.JSONDecodeError, OSError, AttributeError):
            # previous_artifacts: 壊れたManifestには空一覧を使います。
            previous_artifacts = []
        # item: Manifestに記録された前回ファイル。
        for item in previous_artifacts:
            # pathのない不正項目を飛ばします。
            if not isinstance(item, dict) or not isinstance(item.get("path"), str):
                continue
            # previous_path: 出力Directoryへ解決した旧成果物。
            previous_path = (output_directory / item["path"]).resolve()
            # 安全な古い成果物だけを削除します。
            if (
                is_within(previous_path, output_directory)
                and previous_path.is_file()
                and previous_path.suffix in all_artifact_extensions
                and previous_path.name not in build_artifact_names
            ):
                previous_path.unlink()
    # 新Packageで不要になった同名Runtime成果物を整理します。
    # singleFile出力に旧JavaScript/Wasmが残ると読込先が曖昧になります。
    # existing: 出力Directory内の既存項目。
    for existing in output_directory.iterdir():
        # 同じPrefixの不要な成果物を削除します。
        if (
            existing.is_file()
            and existing.name.startswith(artifact_prefix)
            and existing.suffix in all_artifact_extensions
            and existing.name not in build_artifact_names
        ):
            existing.unlink()
    # candidate: Build Directory内の出力候補。
    for candidate in build_directory.rglob("*"):
        # ファイル以外のBuild項目を飛ばします。
        if not candidate.is_file():
            continue
        # project.jsonはPackage設定としてコピーします。
        if candidate.name == "project.json":
            # destination: 出力先の設定ファイル。
            destination = output_directory / candidate.name
        # Prefixと形式が一致するWeb成果物をコピーします。
        elif (
            candidate.name.startswith(artifact_prefix)
            and candidate.suffix in artifact_extensions
        ):
            # destination: 出力先のBrowser成果物。
            destination = output_directory / candidate.name
            # Web成果物を検出したことを記録します。
            artifacts_found = True
        # Package対象でないBuildファイルを飛ばします。
        else:
            continue
        shutil.copy2(candidate, destination)
        # HTML入口へ非表示のLicense本文を埋め込みます。
        if destination.suffix == ".html":
            # content: UTF-8で読み込んだBrowser入口。
            content = destination.read_text(encoding="utf-8")
            # closing_body: 大文字小文字を無視したbody終端位置。
            closing_body = content.lower().rfind("</body>")
            # body直前へLicense本文を挿入します。
            if closing_body >= 0:
                # content: License本文を追加したBrowser入口。
                content = content[:closing_body] + license_notice + content[closing_body:]
            # bodyタグがないHTML末尾へLicense本文を追加します。
            else:
                content += license_notice
            destination.write_text(content, encoding="utf-8")
        copied.append(destination)

    # assets: Build側の出力Asset Directory。
    assets = build_directory / "assets"
    # 存在するAsset DirectoryをPackageへコピーします。
    if assets.is_dir():
        # destination: Package内のAsset Directory。
        destination = output_directory / "assets"
        shutil.copytree(assets, destination, dirs_exist_ok=True)
        copied.append(destination)
    # Browser成果物がなければBuild結果を拒否します。
    if not artifacts_found:
        raise ExportError(
            "The Web build completed but no HTML/JavaScript/Wasm artifacts "
            f"were found in {build_directory}."
        )
    # report_path: Package内の互換性診断JSON。
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


# run_command(command: argv, cwd: 作業Directory, dry_run: 表示のみ): dry-run以外でコマンドを実行します。
def run_command(command: list[str], cwd: Path, dry_run: bool) -> None:
    # エディターはSDKのPythonエントリーを直接指定します。
    # Windowsのbatを経由せず、空白・日本語・シェルの特殊文字を引数のまま渡します。
    # Python経由で呼ぶ必要があるEmscripten入口を補います。
    if Path(command[0]).name in {"emcmake", "emcmake.py"} and Path(command[0]).is_file():
        # command: Pythonを先頭にした実行argv。
        command = [sys.executable, *command]
    print(f"$ {shlex.join(command)}")
    # dry-run以外では終了コードを検証して実行します。
    if not dry_run:
        subprocess.run(command, cwd=cwd, check=True)


# parse_arguments(): Web ExportのCLI引数を解析します。
def parse_arguments() -> argparse.Namespace:
    # parser: Web exporterのCLI option parser。
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


# main(): Web Projectを検証・Buildし、配布Packageを作成します。
def main() -> int:
    # arguments: 解析済みCLI options。
    arguments = parse_arguments()
    # project_path: CLIで指定されたProject設定。
    project_path = arguments.project.resolve()
    # project: 読み込んだProject設定。
    _, project = load_project(project_path)
    # project_root: Project資産の基準Directory。
    project_root = lamapon_project_root(project_path, project)
    # source/target/renderer/profile/project_kind: 解決したWeb build設定。
    source, target, renderer, profile, project_kind = require_web_configuration(
        project_path,
        project_root,
        project,
    )
    # findings: Web互換性診断。
    findings = validate_web_compatibility(
        source,
        project,
        profile,
        project_kind,
    )
    # modules: Buildへ渡すWeb Module。
    modules = resolved_project_modules(source, project, project_kind)
    # output_directory: 成果物の指定先または既定先。
    output_directory = (
        arguments.output.resolve()
        if arguments.output
        else (project_root / ".lamapon" / "web").resolve()
    )
    # build_directory: CMake Buildの指定先または既定先。
    build_directory = (
        arguments.build_dir.resolve()
        if arguments.build_dir
        else project_root / ".lamapon" / (
            "web-build-generated"
            if project_kind == "lamapon-web-target"
            else "web-build"
        )
    )
    # 出力先がBuild Directory内または同一なら拒否します。
    if output_directory == build_directory or is_within(
        output_directory,
        build_directory,
    ):
        raise ExportError(
            "Web output must not be inside the CMake build directory."
        )
    # 出力先がSource Directoryと同一なら拒否します。
    if output_directory == source:
        raise ExportError("Web output must not replace the source directory.")

    print(f"Web renderer: {renderer}")
    print(f"Web compatibility profile: {profile}")
    print(f"Web source: {source}")
    print(f"Web output: {output_directory}")
    print_compatibility_report(findings)

    # reject診断があればBuild開始前に失敗を返します。
    if any(item["level"] == "reject" for item in findings):
        # dry-runではレポートを書かず拒否します。
        if arguments.dry_run:
            print("Web export rejected; dry-run did not write a report.")
            return 2
        # report_path: reject結果を書き出すJSON。
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

    # --report-onlyでは診断を保存して終了します。
    if arguments.report_only:
        # report_path: ready結果を書き出すJSON。
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

    # CMake Web TargetのないProjectはBuildできません。
    if project_kind not in {"cmake", "lamapon-web-target", "lamapon-project"}:
        raise ExportError(
            "This project has no Web CMake target. Use --report-only after "
            "adding a Web runtime target."
        )

    # export: Projectの出力設定。
    export = project.get("export", {})
    # export設定のObject形式を確認します。
    if not isinstance(export, dict):
        raise ExportError("Project export must be an object when specified.")
    # web: ProjectのWeb出力設定。
    web = export.get("web", {})
    # web設定のObject形式を確認します。
    if not isinstance(web, dict):
        raise ExportError("Project export.web must be an object when specified.")
    # artifact_prefix: HTMLとRuntimeの共通出力名。
    artifact_prefix = web_artifact_prefix(source, project, project_kind)
    # single_file: Runtimeを単一HTMLへまとめる設定。
    single_file = web.get("singleFile", False)
    # singleFileを真偽値に限定します。
    if not isinstance(single_file, bool):
        raise ExportError("export.web.singleFile must be a boolean when specified.")

    # emcmake: CLI指定またはPATH上のEmscripten Wrapper。
    emcmake = arguments.emcmake or shutil.which("emcmake")
    # cmake: CLI指定またはPATH上のCMake。
    cmake = arguments.cmake or shutil.which("cmake")
    # 通常実行でemcmakeが見つからなければ中止します。
    if not arguments.dry_run and not emcmake:
        raise ExportError(
            "emcmake was not found. Activate the Emscripten SDK or pass "
            "--emcmake."
        )
    # 通常実行でcmakeが見つからなければ中止します。
    if not arguments.dry_run and not cmake:
        raise ExportError(
            "cmake was not found. Install CMake or pass --cmake."
        )

    # emcmake_command: CMake構成に使うWrapper名。
    emcmake_command = emcmake or "emcmake"
    # cmake_command: Configure/Buildに使うCMake名。
    cmake_command = cmake or "cmake"
    # cmake_source: Configure対象のSource Directory。
    cmake_source = source
    # LamaPon Project向けの生成Targetを準備します。
    if project_kind in {"lamapon-web-target", "lamapon-project"}:
        # cmake_source: 生成CMake設定の出力Directory。
        cmake_source = generate_lamapon_web_target(
            source,
            project,
            target,
            modules,
            profile,
            build_directory.parent / "web-generated-cmake",
        )
    # configure: CMake configure command argv。
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
    # generator指定があればConfigure引数へ加えます。
    if arguments.generator:
        configure.extend(("-G", arguments.generator))
    # build: CMake build command argv。
    build = [
        cmake_command,
        "--build",
        str(build_directory),
        "--config",
        arguments.build_type,
        "--target",
        target,
    ]

    # ConfigureとBuildを順に実行します。
    run_command(configure, project_root, arguments.dry_run)
    run_command(build, project_root, arguments.dry_run)
    # dry-runでは実際の成果物収集を省きます。
    if arguments.dry_run:
        return 0

    # copied/verification: 配置した成果物と検証結果。
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
    # manifest_path: 検証情報を含むExport Manifest。
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
    # path: 利用者へ表示する出力ファイル。
    for path in copied:
        print(f"  {path}")
    return 0


# Scriptとして直接実行された場合だけCLIを起動します。
if __name__ == "__main__":
    # CLI実行時のExportErrorを終了コード2へ変換します。
    try:
        raise SystemExit(main())
    # error: Web出力時の検証・設定エラー。
    except ExportError as error:
        print(f"Web export failed: {error}", file=sys.stderr)
        raise SystemExit(2) from error
    # error: 外部Build commandの失敗情報。
    except subprocess.CalledProcessError as error:
        print(
            f"Web export command failed with exit code {error.returncode}.",
            file=sys.stderr,
        )
        raise SystemExit(error.returncode or 1) from error
