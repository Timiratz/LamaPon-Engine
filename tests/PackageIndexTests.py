"""配布indexとZipの内容を検証し、不正な構成をCIで検出します。"""
from __future__ import annotations

import hashlib
import json
import re
import unittest
import zipfile
from pathlib import Path

# Repository root.
REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
# Package root.
PACKAGES_ROOT = REPOSITORY_ROOT / "packages"
# Package index path.
INDEX_PATH = PACKAGES_ROOT / "index.json"
# Package source root.
SOURCE_ROOT = PACKAGES_ROOT / "src"

# PackageManager.cpp の IsPackageNameSafe と同じ規則です。
SAFE_NAME = re.compile(r"^[a-z0-9_-]{1,64}$")
# PackageManager.cpp の IsAllowedPackageUrl と同じ規則です。
ALLOWED_URL_PREFIXES = (
    "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/",
    "https://github.com/Timiratz/LamaPon-Engine/",
)
# PackageManager.cpp の IsCanonicalPackageSha256 と同じ規則です。
CANONICAL_SHA256 = re.compile(r"^[0-9a-f]{64}$")
# Index fields.
REQUIRED_KEYS = {
    "name",
    "displayName",
    "description",
    "author",
    "version",
    "minimumEngineVersion",
    "downloadUrl",
    "sizeBytes",
    "sha256",
}
# PackageNativeDependencies.cpp が受け付けるキーです。
ALLOWED_NATIVE_KEYS = {
    "includeDirectories",
    "libraries",
    "runtimeFiles",
    "defines",
}
# バイナリはCRLFを含み得るため、テキスト改行の検証対象から外します。
BINARY_SUFFIXES = {".a", ".dll", ".dylib", ".exe", ".lib", ".pdb", ".so",
                   ".png", ".jpg", ".jpeg", ".dds"}


# load_index(): Load the distributed package index.
def load_index() -> dict:
    # Return the decoded package index.
    return json.loads(INDEX_PATH.read_text(encoding="utf-8"))

# package_source_files(source_directory: package root): List source files that enter a package ZIP.
def package_source_files(source_directory: Path) -> list[Path]:
    """Zipへ入るファイルだけを返します（.meta と生成物は除きます）。"""
    # path: candidate source file; return files eligible for package ZIPs.
    return [
        path
        for path in sorted(source_directory.rglob("*"))
        if path.is_file()
        and path.suffix != ".meta"
        and "__pycache__" not in path.parts
    ]


# Validate package metadata and ZIP contents.
class PackageIndexTests(unittest.TestCase):
    # setUp(self: test fixture): Load shared package test data.
    def setUp(self):
        # Decoded package index.
        self.index = load_index()
        # Index entries under test.
        self.packages = self.index["packages"]

    # test_project_packages_use_distributed_engine_headers(self: fixture): Check package references against distributed SDK headers.
    def test_project_packages_use_distributed_engine_headers(self):
        # SDK配布時のinclude解決失敗を検出します。
        # cmake: build settings used to identify excluded SDK headers.
        cmake = (REPOSITORY_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        # Excluded headers.
        excluded = set(re.findall(r'PATTERN "([^"/]+\.h)" EXCLUDE', cmake))
        # Include pattern.
        include = re.compile(r'^\s*#\s*include\s*[<"](LamaPon/[^">\n]+)[">]', re.MULTILINE)
        # Project packages.
        for entry in self.packages:
            # Skip non-projects.
            if entry.get("target") != "Project":
                # Ignore engine and library packages.
                continue
            # Source root.
            source = SOURCE_ROOT / entry["name"]
            # Skip entries that are not project packages.
            if not source.is_dir():
                # Ignore packages with no local source tree.
                continue
            # Source files.
            for path in package_source_files(source):
                # C++ source files.
                if path.suffix not in {".h", ".cpp"}:
                    # Ignore package assets and metadata.
                    continue
                # Engine headers.
                for header in include.findall(path.read_text(encoding="utf-8")):
                    # Package failure context.
                    with self.subTest(package=entry["name"], file=path.name, header=header):
                        self.assertTrue((REPOSITORY_ROOT / "src" / header).is_file())
                        self.assertNotIn(Path(header).name, excluded,
                                         "配布SDKに含まれないヘッダーへ依存しています。")

    # test_index_header(self: fixture): Validate the index format and version.
    def test_index_header(self):
        self.assertEqual(self.index["format"], "LamaPonPackageIndex")
        self.assertEqual(self.index["version"], 1)
        self.assertTrue(self.packages, "一覧が空です。")

    # test_entries_are_complete_and_safe(self: fixture): Validate required fields, names, and URLs.
    def test_entries_are_complete_and_safe(self):
        # Seen package names.
        seen = set()
        # Package entries.
        for entry in self.packages:
            # Test one package.
            with self.subTest(package=entry.get("name")):
                self.assertEqual(REQUIRED_KEYS - set(entry), set())
                # Package name.
                name = entry["name"]
                self.assertRegex(name, SAFE_NAME)
                self.assertNotIn(name, seen, "名前が重複しています。")
                seen.add(name)
                self.assertTrue(
                    entry["downloadUrl"].startswith(ALLOWED_URL_PREFIXES),
                    f"配布リポジトリ外のURLです: {entry['downloadUrl']}",
                )
                self.assertTrue(entry["displayName"])
                self.assertTrue(entry["description"])
                self.assertIsInstance(entry["sizeBytes"], int)

    # test_archives_exist_and_match_the_entry(self: fixture): Verify archive size, hash, and manifest.
    def test_archives_exist_and_match_the_entry(self):
        # Indexed archives.
        for entry in self.packages:
            # Test one package.
            with self.subTest(package=entry["name"]):
                # ZIP path.
                archive_path = PACKAGES_ROOT / entry["downloadUrl"].rsplit("/", 1)[-1]
                self.assertTrue(archive_path.is_file(), f"{archive_path} がありません。")
                self.assertEqual(
                    archive_path.stat().st_size,
                    entry["sizeBytes"],
                    "sizeBytesが実際のZipと違います。",
                )
                # Verify integrity before the editor extracts the ZIP.
                self.assertRegex(entry["sha256"], CANONICAL_SHA256)
                self.assertEqual(
                    hashlib.sha256(archive_path.read_bytes()).hexdigest(),
                    entry["sha256"],
                    "sha256が実際のZipと違います。"
                    " build_package.py --update-index で更新してください。",
                )
                # Read manifest.
                with zipfile.ZipFile(archive_path) as archive:
                    # Parsed manifest.
                    manifest = json.loads(
                        archive.read("package.json").decode("utf-8")
                    )
                self.assertEqual(manifest["name"], entry["name"])
                self.assertEqual(manifest["version"], entry["version"])

    # test_archives_carry_a_meta_for_every_file(self: fixture): Require metadata sidecars for packaged files.
    def test_archives_carry_a_meta_for_every_file(self):
        # Check each ZIP.
        for entry in self.packages:
            # Test one package.
            with self.subTest(package=entry["name"]):
                # ZIP path.
                archive_path = PACKAGES_ROOT / entry["downloadUrl"].rsplit("/", 1)[-1]
                # ZIP members.
                with zipfile.ZipFile(archive_path) as archive:
                    # ZIP members.
                    names = set(archive.namelist())
                # Packaged files.
                assets = {name for name in names if not name.endswith(".meta")}
                # Sidecar files.
                metas = {name for name in names if name.endswith(".meta")}
                self.assertEqual(
                    {asset + ".meta" for asset in assets} - metas,
                    set(),
                    ".metaが無いファイルがあります。",
                )
                self.assertEqual(
                    {meta[: -len(".meta")] for meta in metas} - assets,
                    set(),
                    "元ファイルが無い.metaがあります。",
                )

    # test_transition_asset_importers(self: fixture): Verify importer metadata for each transition asset.
    def test_transition_asset_importers(self):
        # entry: transition package selected by item name from the index.
        entry = next(item for item in self.packages
                     if item["name"] == "scene-transition-showcase")
        # Read package ZIP.
        with zipfile.ZipFile(PACKAGES_ROOT / entry["downloadUrl"].rsplit("/", 1)[-1]) as archive:
            # Map importers.
            for name in archive.namelist():
                # Asset JSON imports as data.
                if name.endswith(".asset.json"):
                    # Importer type.
                    expected = "DataAsset"
                # Scene JSON imports as a scene.
                elif name.endswith(".scene.json"):
                    # Importer type.
                    expected = "Scene"
                # HLSL imports as a shader.
                elif name.endswith(".hlsl"):
                    # Importer type.
                    expected = "Shader"
                # PNG imports as a texture.
                elif name.endswith(".png"):
                    # Importer type.
                    expected = "Texture"
                # C++ imports as a script.
                elif name.endswith(".cpp"):
                    # Importer type.
                    expected = "CppScript"
                # Ignore unrelated archive members.
                else:
                    # Leave unrelated package files unchanged.
                    continue
                # Verify one asset at a time.
                with self.subTest(asset=name):
                    # Importer metadata.
                    meta = json.loads(archive.read(name + ".meta"))
                    self.assertEqual(meta["importer"], expected)

    # test_archives_have_no_path_escape(self: fixture): Reject paths that escape the ZIP root.
    def test_archives_have_no_path_escape(self):
        # Check every indexed archive.
        # Skip unrelated archive members.
        for entry in self.packages:
            # Test one package.
            with self.subTest(package=entry["name"]):
                # ZIP path.
                archive_path = PACKAGES_ROOT / entry["downloadUrl"].rsplit("/", 1)[-1]
                # ZIP member paths.
                with zipfile.ZipFile(archive_path) as archive:
                    # Archive member name.
                    for name in archive.namelist():
                        self.assertFalse(name.startswith("/"), name)
                        self.assertNotIn("..", Path(name).parts, name)
                        self.assertNotIn(":", name, name)

    # test_shipped_archive_matches_its_source(self: fixture): Compare shipped ZIP contents with package sources.
    def test_shipped_archive_matches_its_source(self):
        # Deflate bytes vary by zlib version, so compare extracted files.
        # Indexed packages.
        for entry in self.packages:
            # Package name.
            name = entry["name"]
            # Source directory.
            source_directory = SOURCE_ROOT / name
            # Skip packages without tracked source.
            if not source_directory.is_dir():
                # Ignore missing package source directories.
                continue
            # ZIP path for the indexed version.
            # Archive file path.
            archive_path = PACKAGES_ROOT / f"{name}-{entry['version']}.zip"
            # expected: package source bytes keyed by ZIP member path.
            expected = {
                path.relative_to(source_directory).as_posix(): path.read_bytes()
                for path in package_source_files(source_directory)
            }
            # Read shipped package members.
            with zipfile.ZipFile(archive_path) as archive:
                # shipped: ZIP member bytes excluding sidecar metadata.
                shipped = {
                    shipped_name: archive.read(shipped_name)
                    for shipped_name in archive.namelist()
                    if not shipped_name.endswith(".meta")
                }
            self.assertEqual(
                sorted(shipped),
                sorted(expected),
                "Zipの中身とpackages/srcのファイル一覧が違います。"
                " build_package.py で作り直してください。",
            )
            # Compare files.
            for shipped_name, content in sorted(shipped.items()):
                # Test one member.
                with self.subTest(entry=shipped_name):
                    self.assertEqual(
                        content,
                        expected[shipped_name],
                        f"{shipped_name} の中身がpackages/srcと違います。"
                        " build_package.py で作り直してください。"
                        " 改行が変換されていないかも確認してください"
                        "（.gitattributesでLFに固定しています）。",
                    )

    # test_package_sources_use_unix_line_endings(self: fixture): Reject CRLF in files shipped inside package ZIPs.
    def test_package_sources_use_unix_line_endings(self):
        # CRLF changes extracted file contents across platforms.
        # Source packages.
        for entry in self.packages:
            # Source directory.
            source_directory = SOURCE_ROOT / entry["name"]
            # Skip packages without tracked source.
            if not source_directory.is_dir():
                # Ignore missing package source directories.
                continue
            # ZIP source files.
            for path in package_source_files(source_directory):
                # Skip files whose bytes are not text.
                if path.suffix.lower() in BINARY_SUFFIXES:
                    # Binary payloads may contain CRLF bytes.
                    continue
                # Relative source path.
                relative = path.relative_to(SOURCE_ROOT).as_posix()
                # Keep failures scoped to one source file.
                with self.subTest(file=relative):
                    self.assertNotIn(
                        b"\r\n",
                        path.read_bytes(),
                        f"{relative} にCRLFが混ざっています。"
                        " .gitattributes の設定を確認してください。",
                    )

# Validate Discord Presence package metadata and licensing rules.
class DiscordPresencePackageTests(unittest.TestCase):
    # Package name.
    NAME = "discord-presence-sdk"

    # setUp(self: test fixture): Load the package manifest and archive path.
    def setUp(self):
        # Discord package source root.
        self.source = SOURCE_ROOT / self.NAME
        # Parsed package manifest.
        self.manifest = json.loads(
            (self.source / "package.json").read_text(encoding="utf-8")
        )
        # entry: package index row selected by item name.
        entry = next(
            item for item in load_index()["packages"] if item["name"] == self.NAME
        )
        # Versioned package archive path.
        self.archive_path = PACKAGES_ROOT / f"{self.NAME}-{entry['version']}.zip"

    # test_native_manifest_uses_only_allowed_keys(self: fixture): Restrict native package settings to supported keys.
    def test_native_manifest_uses_only_allowed_keys(self):
        # Native settings.
        native = self.manifest["native"]
        self.assertEqual(set(native) - ALLOWED_NATIVE_KEYS, set())
        self.assertEqual(native["libraries"], ["sdk/lib/discord_partner_sdk.lib"])
        self.assertEqual(native["runtimeFiles"], ["sdk/bin/discord_partner_sdk.dll"])
        self.assertEqual(native["includeDirectories"], ["sdk/include"])
        self.assertEqual(native["defines"], ["LAMAPON_DISCORD_SOCIAL_SDK"])

    # test_native_paths_stay_inside_the_package(self: fixture): Reject native paths outside the package root.
    def test_native_paths_stay_inside_the_package(self):
        # Native package settings.
        native = self.manifest["native"]
        # Path-bearing keys.
        for key in ("includeDirectories", "libraries", "runtimeFiles"):
            # Native paths.
            for value in native[key]:
                # Keep failures scoped to one path.
                with self.subTest(entry=value):
                    self.assertFalse(value.startswith("/"), value)
                    self.assertFalse(value.startswith("\\"), value)
                    self.assertNotIn(":", value, value)
                    self.assertNotIn("..", Path(value).parts, value)
                    self.assertNotIn("*", value, value)

    # test_archive_does_not_redistribute_the_sdk(self: fixture): Keep the licensed SDK binary out of the ZIP.
    def test_archive_does_not_redistribute_the_sdk(self):
        # Discord Social SDKはライセンス上同梱できません。
        # Read package ZIP.
        with zipfile.ZipFile(self.archive_path) as archive:
            # ZIP member names.
            names = archive.namelist()
        # Reject SDK binaries.
        for name in names:
            # Test one member.
            with self.subTest(entry=name):
                self.assertNotIn(
                    Path(name).suffix.lower(),
                    {".lib", ".dll", ".so", ".dylib", ".a", ".exe", ".pdb"},
                    f"SDKのバイナリがZipに含まれています: {name}",
                )
        self.assertNotIn("sdk/include/discordpp.h", names)

    # test_archive_explains_where_to_put_the_sdk(self: fixture): Document the expected local SDK layout.
    def test_archive_explains_where_to_put_the_sdk(self):
        # SDK setup guide.
        with zipfile.ZipFile(self.archive_path) as archive:
            # ZIP members.
            names = set(archive.namelist())
            # Setup README.
            readme = archive.read("README.md").decode("utf-8")
        # SDK folders.
        for folder in ("include", "lib", "bin"):
            self.assertIn(f"sdk/{folder}/PLACE_SDK_HERE.txt", names)
        self.assertIn("discord_partner_sdk.lib", readme)
        self.assertIn("discord_partner_sdk.dll", readme)
        self.assertIn("discordpp.h", readme)

    # test_adapter_is_guarded_by_the_package_define(self: fixture): Require the adapter to compile without the optional SDK.
    def test_adapter_is_guarded_by_the_package_define(self):
        # SDK未導入時にadapterを無効化します。
        # Adapter sources.
        for file_name in (
            "DiscordSocialPresenceBackend.h",
            "DiscordSocialPresenceBackend.cpp",
        ):
            # Keep failures scoped to one source file.
            with self.subTest(file=file_name):
                # Adapter source text.
                text = (self.source / file_name).read_text(encoding="utf-8")
                self.assertIn(
                    "#if defined(LAMAPON_DISCORD_SOCIAL_SDK)",
                    text,
                )

    # test_exactly_one_translation_unit_defines_the_sdk_implementation(self: fixture): Require one implementation translation unit.
    def test_exactly_one_translation_unit_defines_the_sdk_implementation(self):
        # discordpp.h はヘッダーオンリーです。
        # DISCORDPP_IMPLEMENTATION must appear in exactly one source file.
        # definers: source paths containing the SDK implementation macro.
        definers = sorted(
            path.name
            for path in self.source.rglob("*.cpp")
            if "#define DISCORDPP_IMPLEMENTATION"
            in path.read_text(encoding="utf-8")
        )
        self.assertEqual(definers, ["DiscordSocialSdkImplementation.cpp"])

    # test_adapter_does_not_touch_discord_login(self: fixture): Keep Rich Presence independent from Discord login.
    def test_adapter_does_not_touch_discord_login(self):
        # Adapter source.
        text = (self.source / "DiscordSocialPresenceBackend.cpp").read_text(
            encoding="utf-8"
        )
        # Forbidden auth APIs.
        for forbidden in (
            "Authorize",
            "UpdateToken",
            "client_secret",
            "GetDefaultPresenceScopes",
            "AuthorizationArgs",
        ):
            # Keep failures scoped to one API or credential.
            with self.subTest(symbol=forbidden):
                self.assertNotIn(forbidden, text)


# Validate the local-only Ollama package contract.
class OllamaPackageTests(unittest.TestCase):
    # Package name.
    NAME = "ollama-ai"
    # Profile fields.
    PROFILE_FIELDS = {
        "model",
        "systemPrompt",
        "temperature",
        "maxTokens",
        "port",
        "fallbackReply",
        "historyLimit",
        "keepAliveMinutes",
    }

    # setUp(self: test fixture): Load Ollama package source and code files.
    def setUp(self):
        # Ollama package source root.
        self.source = SOURCE_ROOT / self.NAME
        # code: C++ source text keyed by file name.
        self.code = {
            path.name: path.read_text(encoding="utf-8")
            for path in package_source_files(self.source)
            if path.suffix in {".h", ".cpp"}
        }

    # test_code_never_reaches_ollama_cloud(self: fixture): Reject cloud endpoints and authentication in package code.
    def test_code_never_reaches_ollama_cloud(self):
        self.assertTrue(self.code, "C++ソースが見つかりません。")
        # Package source files.
        for name, text in self.code.items():
            # Forbidden cloud APIs.
            for forbidden in (
                "ollama.com",
                "https://",
                "OLLAMA_API_KEY",
                "Authorization",
                "Bearer",
                "getenv",
                "GetEnvironmentVariable",
                "web_search",
                "web_fetch",
                "signin",
            ):
                # Isolate file/API failure.
                with self.subTest(file=name, symbol=forbidden):
                    self.assertNotIn(forbidden, text)

    # test_every_request_goes_through_the_endpoint_check(self: fixture): Require endpoint validation before every HTTP request.
    def test_every_request_goes_through_the_endpoint_check(self):
        # callers: source file names mapped to their HTTP call counts.
        callers = {
            name: text.count("HttpSend(")
            for name, text in self.code.items()
            if "HttpSend(" in text
        }
        self.assertEqual(callers, {"OllamaClient.h": 1})
        # HTTP client source.
        client = self.code["OllamaClient.h"]
        self.assertLess(
            client.index("IsAllowedEndpoint(url)"),
            client.index("HttpSend("),
        )

    # test_threads_are_joined_not_detached(self: fixture): Require worker threads to finish before package unload.
    def test_threads_are_joined_not_detached(self):
        # Package sources.
        for name, text in self.code.items():
            # Keep failures scoped to one source file.
            with self.subTest(file=name):
                self.assertNotIn(".detach(", text)
        self.assertIn("std::jthread", self.code["OllamaWorker.h"])

    # test_shipped_profiles_have_no_host_or_key(self: fixture): Reject cloud hosts and credentials in shipped profiles.
    def test_shipped_profiles_have_no_host_or_key(self):
        # Profile files.
        profiles = sorted((self.source / "profiles").glob("*.asset.json"))
        self.assertTrue(profiles, "設定アセットが同梱されていません。")
        # Shipped profiles.
        for path in profiles:
            # Keep failures scoped to one profile.
            with self.subTest(profile=path.name):
                # Profile asset.
                asset = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(asset["type"], "Ollama.ModelProfile")
                self.assertEqual(set(asset["values"]), self.PROFILE_FIELDS)
                self.assertEqual(asset["values"]["port"], 11434)
                self.assertNotIn("cloud", asset["values"]["model"].lower())

    # test_readme_explains_the_local_only_setup(self: fixture): Require local setup instructions in the package README.
    def test_readme_explains_the_local_only_setup(self):
        # Package setup guide.
        readme = (self.source / "README.md").read_text(encoding="utf-8")
        # Local-only guidance.
        for required in (
            "OLLAMA_NO_CLOUD=1",
            '{"disable_ollama_cloud": true}',
            "サインインは不要",
            "ollama pull",
            "Windows専用",
            "モデルのライセンス",
        ):
            # Isolate instruction failure.
            with self.subTest(text=required):
                self.assertIn(required, readme)


# Run tests when invoked as a script.
if __name__ == "__main__":
    unittest.main()
