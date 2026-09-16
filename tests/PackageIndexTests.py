"""配布パッケージ一覧（packages/index.json）と同梱Zipの検証です。

エディターの「拡張機能」はこの index.json を読みます。エントリの
書き間違いやZipの入れ忘れは、利用者側で初めて分かると直しにくいので
CIで止めます。Discord用パッケージについては、ライセンス上同梱できない
SDK本体がZipへ紛れ込んでいないことも確認します。
"""
from __future__ import annotations

import json
import re
import unittest
import zipfile
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
PACKAGES_ROOT = REPOSITORY_ROOT / "packages"
INDEX_PATH = PACKAGES_ROOT / "index.json"
SOURCE_ROOT = PACKAGES_ROOT / "src"

# PackageManager.cpp の IsPackageNameSafe と同じ規則です。
SAFE_NAME = re.compile(r"^[a-z0-9_-]{1,64}$")
# PackageManager.cpp の IsAllowedPackageUrl と同じ規則です。
ALLOWED_URL_PREFIXES = (
    "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/",
    "https://github.com/Timiratz/LamaPon-Engine/",
)
REQUIRED_KEYS = {
    "name",
    "displayName",
    "description",
    "author",
    "version",
    "minimumEngineVersion",
    "downloadUrl",
    "sizeBytes",
}
# PackageNativeDependencies.cpp が受け付けるキーです。
ALLOWED_NATIVE_KEYS = {
    "includeDirectories",
    "libraries",
    "runtimeFiles",
    "defines",
}


def load_index() -> dict:
    return json.loads(INDEX_PATH.read_text(encoding="utf-8"))


def package_source_files(source_directory: Path) -> list[Path]:
    """Zipへ入るファイルだけを返します（.meta と生成物は除きます）。"""
    return [
        path
        for path in sorted(source_directory.rglob("*"))
        if path.is_file()
        and path.suffix != ".meta"
        and "__pycache__" not in path.parts
    ]


class PackageIndexTests(unittest.TestCase):
    def setUp(self):
        self.index = load_index()
        self.packages = self.index["packages"]

    def test_index_header(self):
        self.assertEqual(self.index["format"], "LamaPonPackageIndex")
        self.assertEqual(self.index["version"], 1)
        self.assertTrue(self.packages, "一覧が空です。")

    def test_entries_are_complete_and_safe(self):
        seen = set()
        for entry in self.packages:
            with self.subTest(package=entry.get("name")):
                self.assertEqual(REQUIRED_KEYS - set(entry), set())
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

    def test_archives_exist_and_match_the_entry(self):
        for entry in self.packages:
            with self.subTest(package=entry["name"]):
                archive_path = PACKAGES_ROOT / entry["downloadUrl"].rsplit("/", 1)[-1]
                self.assertTrue(archive_path.is_file(), f"{archive_path} がありません。")
                self.assertEqual(
                    archive_path.stat().st_size,
                    entry["sizeBytes"],
                    "sizeBytesが実際のZipと違います。",
                )
                with zipfile.ZipFile(archive_path) as archive:
                    manifest = json.loads(
                        archive.read("package.json").decode("utf-8")
                    )
                self.assertEqual(manifest["name"], entry["name"])
                self.assertEqual(manifest["version"], entry["version"])

    def test_archives_carry_a_meta_for_every_file(self):
        for entry in self.packages:
            with self.subTest(package=entry["name"]):
                archive_path = PACKAGES_ROOT / entry["downloadUrl"].rsplit("/", 1)[-1]
                with zipfile.ZipFile(archive_path) as archive:
                    names = set(archive.namelist())
                assets = {name for name in names if not name.endswith(".meta")}
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

    def test_archives_have_no_path_escape(self):
        for entry in self.packages:
            with self.subTest(package=entry["name"]):
                archive_path = PACKAGES_ROOT / entry["downloadUrl"].rsplit("/", 1)[-1]
                with zipfile.ZipFile(archive_path) as archive:
                    for name in archive.namelist():
                        self.assertFalse(name.startswith("/"), name)
                        self.assertNotIn("..", Path(name).parts, name)
                        self.assertNotIn(":", name, name)

    def test_shipped_archive_matches_its_source(self):
        # packages/src へソースがあるものは、Zipの中身がそのソースと
        # 一致することを確かめます。手で差し替えたZipを配ってしまわない
        # ためです。
        #
        # Zipのバイト列ではなく展開後の中身を比べます。deflateの出力は
        # zlibの版で変わり得るので、バイト比較にすると環境差で落ちます。
        for entry in self.packages:
            name = entry["name"]
            source_directory = SOURCE_ROOT / name
            if not source_directory.is_dir():
                continue
            with self.subTest(package=name):
                archive_path = PACKAGES_ROOT / f"{name}-{entry['version']}.zip"
                expected = {
                    path.relative_to(source_directory).as_posix(): path.read_bytes()
                    for path in package_source_files(source_directory)
                }
                with zipfile.ZipFile(archive_path) as archive:
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
                for shipped_name, content in sorted(shipped.items()):
                    with self.subTest(entry=shipped_name):
                        self.assertEqual(
                            content,
                            expected[shipped_name],
                            f"{shipped_name} の中身がpackages/srcと違います。"
                            " build_package.py で作り直してください。"
                            " 改行が変換されていないかも確認してください"
                            "（.gitattributesでLFに固定しています）。",
                        )

    def test_package_sources_use_unix_line_endings(self):
        # Zipへそのまま入るので、CRLFが混ざるとOSによって利用者が
        # 受け取る中身が変わります。.gitattributes で固定していますが、
        # 設定漏れに気付けるようにここでも確かめます。
        for entry in self.packages:
            source_directory = SOURCE_ROOT / entry["name"]
            if not source_directory.is_dir():
                continue
            for path in package_source_files(source_directory):
                relative = path.relative_to(SOURCE_ROOT).as_posix()
                with self.subTest(file=relative):
                    self.assertNotIn(
                        b"\r\n",
                        path.read_bytes(),
                        f"{relative} にCRLFが混ざっています。"
                        " .gitattributes の設定を確認してください。",
                    )


class DiscordPresencePackageTests(unittest.TestCase):
    NAME = "discord-presence-sdk"

    def setUp(self):
        self.source = SOURCE_ROOT / self.NAME
        self.manifest = json.loads(
            (self.source / "package.json").read_text(encoding="utf-8")
        )
        entry = next(
            item for item in load_index()["packages"] if item["name"] == self.NAME
        )
        self.archive_path = PACKAGES_ROOT / f"{self.NAME}-{entry['version']}.zip"

    def test_native_manifest_uses_only_allowed_keys(self):
        native = self.manifest["native"]
        self.assertEqual(set(native) - ALLOWED_NATIVE_KEYS, set())
        self.assertEqual(native["libraries"], ["sdk/lib/discord_partner_sdk.lib"])
        self.assertEqual(native["runtimeFiles"], ["sdk/bin/discord_partner_sdk.dll"])
        self.assertEqual(native["includeDirectories"], ["sdk/include"])
        self.assertEqual(native["defines"], ["LAMAPON_DISCORD_SOCIAL_SDK"])

    def test_native_paths_stay_inside_the_package(self):
        native = self.manifest["native"]
        for key in ("includeDirectories", "libraries", "runtimeFiles"):
            for value in native[key]:
                with self.subTest(entry=value):
                    self.assertFalse(value.startswith("/"), value)
                    self.assertFalse(value.startswith("\\"), value)
                    self.assertNotIn(":", value, value)
                    self.assertNotIn("..", Path(value).parts, value)
                    self.assertNotIn("*", value, value)

    def test_archive_does_not_redistribute_the_sdk(self):
        # Discord Social SDKはライセンス上同梱できません。誤って
        # sdkフォルダーへ置いたまま作り直しても気付けるようにします。
        with zipfile.ZipFile(self.archive_path) as archive:
            names = archive.namelist()
        for name in names:
            with self.subTest(entry=name):
                self.assertNotIn(
                    Path(name).suffix.lower(),
                    {".lib", ".dll", ".so", ".dylib", ".a", ".exe", ".pdb"},
                    f"SDKのバイナリがZipに含まれています: {name}",
                )
        self.assertNotIn("sdk/include/discordpp.h", names)

    def test_archive_explains_where_to_put_the_sdk(self):
        with zipfile.ZipFile(self.archive_path) as archive:
            names = set(archive.namelist())
            readme = archive.read("README.md").decode("utf-8")
        for folder in ("include", "lib", "bin"):
            self.assertIn(f"sdk/{folder}/PLACE_SDK_HERE.txt", names)
        self.assertIn("discord_partner_sdk.lib", readme)
        self.assertIn("discord_partner_sdk.dll", readme)
        self.assertIn("discordpp.h", readme)

    def test_adapter_is_guarded_by_the_package_define(self):
        # SDKが無い状態でGame Moduleへ紛れ込んでも、コンパイルエラーに
        # ならず「何も持たないファイル」になることを担保します。
        for file_name in (
            "DiscordSocialPresenceBackend.h",
            "DiscordSocialPresenceBackend.cpp",
        ):
            with self.subTest(file=file_name):
                text = (self.source / file_name).read_text(encoding="utf-8")
                self.assertIn(
                    "#if defined(LAMAPON_DISCORD_SOCIAL_SDK)",
                    text,
                )

    def test_exactly_one_translation_unit_defines_the_sdk_implementation(self):
        # discordpp.h はヘッダーオンリーです。DISCORDPP_IMPLEMENTATION を
        # 定義した.cppが0個だと「未解決の外部シンボル」、2個以上だと
        # 「多重定義」でリンクできません。ちょうど1つを担保します。
        definers = sorted(
            path.name
            for path in self.source.rglob("*.cpp")
            if "#define DISCORDPP_IMPLEMENTATION"
            in path.read_text(encoding="utf-8")
        )
        self.assertEqual(definers, ["DiscordSocialSdkImplementation.cpp"])

    def test_adapter_does_not_touch_discord_login(self):
        # Rich PresenceはDiscordアカウント連携から独立しています。
        # tokenやOAuthへ触れていないことを確かめます。
        text = (self.source / "DiscordSocialPresenceBackend.cpp").read_text(
            encoding="utf-8"
        )
        for forbidden in (
            "Authorize",
            "UpdateToken",
            "client_secret",
            "GetDefaultPresenceScopes",
            "AuthorizationArgs",
        ):
            with self.subTest(symbol=forbidden):
                self.assertNotIn(forbidden, text)


if __name__ == "__main__":
    unittest.main()
