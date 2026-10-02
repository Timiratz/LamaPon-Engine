"""Inspect a Windows Game Module without loading it or writing files.

Usage: py -3 tools/audit_game_module.py path/to/LamaPonGameModule.dll
"""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path


class InvalidPe(ValueError):
    # 不正なPEヘッダーを示す例外
    pass


# u16(data: 読取対象, offset: バイト位置)は16bit整数を読む。
def u16(data: bytes, offset: int) -> int:
    # 読取範囲外の位置を拒否する
    if offset < 0 or offset + 2 > len(data):
        raise InvalidPe("Truncated PE header")
    return struct.unpack_from("<H", data, offset)[0]


# u32(data: 読取対象, offset: バイト位置)は32bit整数を読む。
def u32(data: bytes, offset: int) -> int:
    # 読取範囲外の位置を拒否する
    if offset < 0 or offset + 4 > len(data):
        raise InvalidPe("Truncated PE header")
    return struct.unpack_from("<I", data, offset)[0]


# inspect(path: 対象モジュール)はPE構造と公開情報を集計する。
def inspect(path: Path) -> dict[str, object]:
    # モジュール全体のバイト列
    data = path.read_bytes()
    # DOSヘッダーの最小長と識別子を確認する
    if len(data) < 64 or data[:2] != b"MZ":
        raise InvalidPe("Not a Windows PE file")
    # PEヘッダー位置
    pe = u32(data, 0x3C)
    # PE署名とヘッダー範囲を確認する
    if pe + 24 > len(data) or data[pe : pe + 4] != b"PE\0\0":
        raise InvalidPe("Missing PE signature")
    # セクション数
    section_count = u16(data, pe + 6)
    # オプションヘッダーの長さ
    optional_size = u16(data, pe + 20)
    # オプションヘッダーの開始位置
    optional = pe + 24
    # セクション表の開始位置
    sections_start = optional + optional_size
    # セクション表がファイル内に収まることを確認する
    if section_count > 96 or sections_start + section_count * 40 > len(data):
        raise InvalidPe("Invalid PE section table")
    # オプションヘッダー形式
    magic = u16(data, optional)
    # PE32+形式のディレクトリ位置
    if magic == 0x20B:
        # PE32+形式のディレクトリ表開始位置
        directories = optional + 112
    # PE32形式のディレクトリ位置
    elif magic == 0x10B:
        # PE32形式のディレクトリ表開始位置
        directories = optional + 96
    # 未対応形式を拒否する
    else:
        raise InvalidPe("Unsupported PE optional header")
    # 必要なディレクトリがヘッダー内にあることを確認する
    if directories + 7 * 8 > sections_start:
        raise InvalidPe("Truncated PE data directories")
    # 仮想アドレスとファイル範囲の対応表
    sections = []
    # 各セクションの範囲を登録する
    for index in range(section_count):
        # セクションヘッダーの位置
        section = sections_start + index * 40
        # メモリ上のセクション長
        virtual_size = u32(data, section + 8)
        # メモリ上の開始アドレス
        virtual_address = u32(data, section + 12)
        # ファイル内のセクション長
        raw_size = u32(data, section + 16)
        # ファイル内の開始位置
        raw_offset = u32(data, section + 20)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    # rva_offset(rva: 仮想アドレス, size: 読取長)はファイル位置を返す。
    def rva_offset(rva: int, size: int) -> int:
        # 仮想アドレスを含むセクションを探す
        for address, span, raw_offset, raw_size in sections:
            # 要求範囲全体がセクション内にあるか確認する
            if address <= rva and rva + size <= address + span:
                # セクション内の相対位置
                relative = rva - address
                # 生データとファイル範囲内の位置だけを返す
                if relative + size <= raw_size and raw_offset + relative + size <= len(data):
                    return raw_offset + relative
        raise InvalidPe("PE directory points outside file data")

    # c_string(offset: バイト位置)は終端付きASCII文字列を読む。
    def c_string(offset: int) -> str:
        # 文字列終端を最大4096バイトで探す
        end = data.find(b"\0", offset, min(len(data), offset + 4096))
        # 終端のないPE文字列を拒否する
        if end < 0:
            raise InvalidPe("Unterminated PE string")
        return data[offset:end].decode("ascii", "replace")

    # エクスポート名一覧
    exports: list[str] = []
    # エクスポート表の仮想アドレス
    export_rva = u32(data, directories)
    # エクスポート表のサイズ
    export_size = u32(data, directories + 4)
    # エクスポート表がある場合だけ名前を読む
    if export_rva and export_size:
        # エクスポートディレクトリのファイル位置
        export = rva_offset(export_rva, 40)
        # 公開名の件数
        name_count = u32(data, export + 24)
        # 異常に大きい公開名数を拒否する
        if name_count > 100_000:
            raise InvalidPe("Unreasonable PE export count")
        # 公開名が存在するとき名前表を読む
        if name_count:
            # 公開名のアドレス表
            name_table = rva_offset(u32(data, export + 32), name_count * 4)
            # 公開名を順番に取得する
            for index in range(name_count):
                exports.append(c_string(rva_offset(u32(data, name_table + index * 4), 1)))

    # CodeViewに記録されたPDB名一覧
    pdb_names: list[str] = []
    # 絶対パスのPDB記録数
    absolute_pdb_paths = 0
    # デバッグ情報の仮想アドレス
    debug_rva = u32(data, directories + 6 * 8)
    # デバッグ情報のサイズ
    debug_size = u32(data, directories + 6 * 8 + 4)
    # デバッグ情報がある場合だけ項目を走査する
    if debug_rva and debug_size:
        # 固定長項目数が妥当か確認する
        if debug_size % 28 or debug_size > 28 * 128:
            raise InvalidPe("Invalid PE debug directory")
        # デバッグ項目のファイル位置
        debug = rva_offset(debug_rva, debug_size)
        # CodeView項目を調べる
        for index in range(debug_size // 28):
            # デバッグ項目の開始位置
            entry = debug + index * 28
            # CodeView以外の項目を飛ばす
            # IMAGE_DEBUG_TYPE_CODEVIEW以外の項目を飛ばします。
            if u32(data, entry + 12) != 2:
                continue
            # PDBレコードの長さ
            size = u32(data, entry + 16)
            # PDBレコードのファイル位置
            offset = u32(data, entry + 24)
            # 有効なRSDSレコードだけを処理する
            if size < 25 or offset + size > len(data) or data[offset : offset + 4] != b"RSDS":
                continue
            # 記録されたPDBパス
            pdb = c_string(offset + 24)
            pdb_names.append(Path(pdb.replace("\\", "/")).name)
            # ドライブまたはUNC形式の絶対パスを数える
            if re.match(r"^[A-Za-z]:[\\/]", pdb) or pdb.startswith("\\\\"):
                # 絶対パスの記録数
                absolute_pdb_paths += 1

    # match: ASCII文字列候補; strings: 8文字以上のASCII列
    strings = [match.group().decode("ascii", "replace")
               # match: 印字可能ASCII列候補
               for match in re.finditer(rb"[ -~]{8,}", data)]
    return {
        "file": str(path),
        "bytes": len(data),
        "exports": exports,
        "pdbNames": pdb_names,
        "absolutePdbPaths": absolute_pdb_paths,
        "asciiStringsAtLeast8": len(strings),
        # RTTI型名に一致する文字列数
        "rttiTypeNames": sum(value.startswith(".?AV") for value in strings),
    }


# main()は引数を検証して監査結果を出力する。
def main() -> int:
    # CLI引数の定義
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    parser.add_argument("--strict", action="store_true",
                        help="fail on absolute PDB paths or unexpected Game Module exports")
    # CLI引数
    args = parser.parse_args()
    # PE解析結果を作成する
    try:
        # 対象モジュールを解析した結果
        report = inspect(args.module)
    # 読取不能または不正なPEをCLIエラーにする
    except (OSError, InvalidPe) as error:
        parser.exit(2, f"audit failed: {error}\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    # strict指定時にエクスポートとPDBパスを検査する
    if args.strict and (report["absolutePdbPaths"]
                        or report["exports"] != ["LamaPonGetGameModule"]):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
