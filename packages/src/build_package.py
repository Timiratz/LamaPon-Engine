#!/usr/bin/env python3
"""配布用パッケージZipとindex.jsonのエントリを作ります。

エディターの「パッケージを作成...」と同じ形のZipを、Windows以外でも
作れるようにしたものです。packages/src/<名前>/ の中身をそのままZipの
ルートへ入れ、足りない .meta を補います。

    python3 packages/src/build_package.py discord-presence-sdk
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
import zipfile

REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE_ROOT = REPOSITORY_ROOT / "packages" / "src"
OUTPUT_ROOT = REPOSITORY_ROOT / "packages"

# AssetDatabase::ImporterFor と同じ対応です。今のパッケージが使う
# 拡張子だけを見ます。
IMPORTERS = {".cpp": "CppScript"}


def importer_for(path: pathlib.Path) -> str:
    return IMPORTERS.get(path.suffix.lower(), "Default")


def guid_for(package: str, relative: str) -> str:
    # 作り直しても同じGUIDになるよう、パスから決めます。エディターが
    # 作るGUIDと同じ32桁の16進です。
    digest = hashlib.md5(f"{package}/{relative}".encode("utf-8"))
    return digest.hexdigest()


def collect(package_directory: pathlib.Path) -> list[pathlib.Path]:
    files = [
        path
        for path in sorted(package_directory.rglob("*"))
        if path.is_file() and path.suffix != ".meta"
    ]
    if not files:
        raise SystemExit(f"no files found in {package_directory}")
    return files


def build(package: str) -> dict:
    package_directory = SOURCE_ROOT / package
    if not package_directory.is_dir():
        raise SystemExit(f"package source not found: {package_directory}")

    manifest_path = package_directory / "package.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("name") != package:
        raise SystemExit(
            f'package.json name "{manifest.get("name")}" != folder "{package}"'
        )
    version = manifest["version"]

    zip_path = OUTPUT_ROOT / f"{package}-{version}.zip"
    files = collect(package_directory)

    # 中身が同じなら毎回同じZipになるよう、更新日時を固定します。
    date_time = (1980, 1, 1, 0, 0, 0)
    with zipfile.ZipFile(
        zip_path, "w", compression=zipfile.ZIP_DEFLATED
    ) as archive:
        for path in files:
            relative = path.relative_to(package_directory).as_posix()
            info = zipfile.ZipInfo(relative, date_time)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())

            meta = json.dumps(
                {
                    "format": "LamaPonAssetMeta",
                    "guid": guid_for(package, relative),
                    "importer": importer_for(path),
                    "version": 1,
                },
                indent=2,
                ensure_ascii=False,
            ) + "\n"
            meta_info = zipfile.ZipInfo(relative + ".meta", date_time)
            meta_info.compress_type = zipfile.ZIP_DEFLATED
            meta_info.external_attr = 0o644 << 16
            archive.writestr(meta_info, meta.encode("utf-8"))

    entry = {
        "name": manifest["name"],
        "displayName": manifest["displayName"],
        "description": manifest["description"],
        "author": manifest["author"],
        "version": version,
        "minimumEngineVersion": manifest["minimumEngineVersion"],
        "downloadUrl": (
            "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/"
            f"main/packages/{zip_path.name}"
        ),
        "sizeBytes": zip_path.stat().st_size,
    }
    print(f"{zip_path.relative_to(REPOSITORY_ROOT)}: "
          f"{len(files)} files, {entry['sizeBytes']} bytes")
    return entry


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packages", nargs="+")
    parser.add_argument(
        "--update-index",
        action="store_true",
        help="packages/index.json の該当エントリを書き換えます。",
    )
    arguments = parser.parse_args()

    entries = {package: build(package) for package in arguments.packages}
    if not arguments.update_index:
        for entry in entries.values():
            print(json.dumps(entry, indent=2, ensure_ascii=False))
        return 0

    index_path = OUTPUT_ROOT / "index.json"
    index = json.loads(index_path.read_text(encoding="utf-8"))
    listed = index.setdefault("packages", [])
    for name, entry in entries.items():
        for position, existing in enumerate(listed):
            if existing.get("name") == name:
                listed[position] = entry
                break
        else:
            listed.append(entry)
    listed.sort(key=lambda item: item.get("name", ""))
    index_path.write_text(
        json.dumps(index, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"updated {index_path.relative_to(REPOSITORY_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
