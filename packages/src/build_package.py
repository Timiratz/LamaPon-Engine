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

# エンジンリポジトリのルート
REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
# パッケージのソース格納先
SOURCE_ROOT = REPOSITORY_ROOT / "packages" / "src"
# 配布Zipと索引の格納先
OUTPUT_ROOT = REPOSITORY_ROOT / "packages"

# AssetDatabase::ImporterForと同じ拡張子対応
IMPORTERS = {".cpp": "CppScript", ".hlsl": "Shader", ".png": "Texture"}


# アセット種別に対応するインポーター名を返します(path: 判定するアセットのパス)。
def importer_for(path: pathlib.Path) -> str:
    # Scene JSONは専用Importerへ渡します。
    if path.name.lower().endswith(".scene.json"):
        return "Scene"
    # DataAsset JSONは専用Importerへ渡します。
    if path.name.lower().endswith(".asset.json"):
        return "DataAsset"
    return IMPORTERS.get(path.suffix.lower(), "Default")


# パスから再現可能な32桁GUIDを作ります(package: パッケージ名, relative: パッケージ内の相対パス)。
def guid_for(package: str, relative: str) -> str:
    # パッケージ名とパスのハッシュ
    digest = hashlib.md5(f"{package}/{relative}".encode("utf-8"))
    return digest.hexdigest()


# メタ情報を除く梱包対象を列挙します(package_directory: パッケージのソース格納先)。
def collect(package_directory: pathlib.Path) -> list[pathlib.Path]:
    # files: 梱包対象
    # path: 検査中のfile
    files = [
        path
        for path in sorted(package_directory.rglob("*"))
        if path.is_file() and path.suffix != ".meta"
    ]
    # 空Packageは配布できません。
    if not files:
        raise SystemExit(f"no files found in {package_directory}")
    return files


# パッケージZipを生成し索引のエントリを返します(package: ビルドするパッケージ名)。
def build(package: str) -> dict:
    # 対象パッケージのソース格納先
    package_directory = SOURCE_ROOT / package
    # Source directoryがないPackageは中止します。
    if not package_directory.is_dir():
        raise SystemExit(f"package source not found: {package_directory}")

    # パッケージ定義のパス
    manifest_path = package_directory / "package.json"
    # 読み込んだパッケージ定義
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    # 定義名とdirectory名が異なる場合は中止します。
    if manifest.get("name") != package:
        raise SystemExit(
            f'package.json name "{manifest.get("name")}" != folder "{package}"'
        )
    # 配布するパッケージの版
    version = manifest["version"]

    # 生成する配布Zipのパス
    zip_path = OUTPUT_ROOT / f"{package}-{version}.zip"
    # 梱包対象のファイル一覧
    files = collect(package_directory)

    # 中身が同じなら毎回同じZipになるよう、更新日時を固定します。
    # Zip内に記録する固定日時
    date_time = (1980, 1, 1, 0, 0, 0)
    # 作成中の配布Zip
    with zipfile.ZipFile(
        zip_path, "w", compression=zipfile.ZIP_DEFLATED
    ) as archive:
        # 梱包中のソースファイル
        for path in files:
            # Zipルートからの相対パス
            relative = path.relative_to(package_directory).as_posix()
            # ファイルのZipエントリ設定
            info = zipfile.ZipInfo(relative, date_time)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())

            # 補完するアセットメタ情報
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
            # メタ情報のZipエントリ設定
            meta_info = zipfile.ZipInfo(relative + ".meta", date_time)
            meta_info.compress_type = zipfile.ZIP_DEFLATED
            meta_info.external_attr = 0o644 << 16
            archive.writestr(meta_info, meta.encode("utf-8"))

    # sha256は展開前の照合に使うため、Zip生成後のバイト列から計算します。
    # 配布索引へ登録するエントリ
    entry = {
        "name": manifest["name"],
        "displayName": manifest["displayName"],
        "description": manifest["description"],
        "author": manifest["author"],
        "version": version,
        "minimumEngineVersion": manifest["minimumEngineVersion"],
        "activation": manifest.get("activation", "Immediate"),
        "target": manifest.get("target", "Project"),
        "downloadUrl": (
            "https://raw.githubusercontent.com/Timiratz/LamaPon-Engine/"
            f"main/packages/{zip_path.name}"
        ),
        "sizeBytes": zip_path.stat().st_size,
        "sha256": hashlib.sha256(zip_path.read_bytes()).hexdigest(),
    }
    print(f"{zip_path.relative_to(REPOSITORY_ROOT)}: "
          f"{len(files)} files, {entry['sizeBytes']} bytes")
    return entry


# 指定パッケージを生成し、要求された場合は配布索引も更新します。
def main() -> int:
    # CLIオプションの解析器
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packages", nargs="+")
    parser.add_argument(
        "--update-index",
        action="store_true",
        help="packages/index.json の該当エントリを書き換えます。",
    )
    # 解析済みのCLIオプション
    arguments = parser.parse_args()

    # entries: 索引項目
    # package: 対象名
    entries = {package: build(package) for package in arguments.packages}
    # indexを更新しない場合は生成情報を表示します。
    if not arguments.update_index:
        # 出力するパッケージ情報
        for entry in entries.values():
            print(json.dumps(entry, indent=2, ensure_ascii=False))
        return 0

    # 更新する配布索引のパス
    index_path = OUTPUT_ROOT / "index.json"
    # 読み込んだ配布索引
    index = json.loads(index_path.read_text(encoding="utf-8"))
    # 配布索引内のパッケージ一覧
    listed = index.setdefault("packages", [])
    # name: Package名
    # entry: 生成済み情報
    for name, entry in entries.items():
        # position: 一覧内のindex
        # existing: 登録済み情報
        for position, existing in enumerate(listed):
            # 同名項目は生成済み情報で置き換えます。
            if existing.get("name") == name:
                listed[position] = entry
                break
        # 既存項目がない場合は新規登録します。
        else:
            listed.append(entry)
    # パッケージ名を並べ替えのキーにします(item: 索引のエントリ)。
    listed.sort(key=lambda item: item.get("name", ""))
    index_path.write_text(
        json.dumps(index, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"updated {index_path.relative_to(REPOSITORY_ROOT)}")
    return 0


# Scriptとして実行された場合だけCLIを起動します。
if __name__ == "__main__":
    sys.exit(main())
