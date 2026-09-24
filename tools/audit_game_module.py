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
    pass


def u16(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise InvalidPe("Truncated PE header")
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise InvalidPe("Truncated PE header")
    return struct.unpack_from("<I", data, offset)[0]


def inspect(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise InvalidPe("Not a Windows PE file")
    pe = u32(data, 0x3C)
    if pe + 24 > len(data) or data[pe : pe + 4] != b"PE\0\0":
        raise InvalidPe("Missing PE signature")
    section_count = u16(data, pe + 6)
    optional_size = u16(data, pe + 20)
    optional = pe + 24
    sections_start = optional + optional_size
    if section_count > 96 or sections_start + section_count * 40 > len(data):
        raise InvalidPe("Invalid PE section table")
    magic = u16(data, optional)
    if magic == 0x20B:
        directories = optional + 112
    elif magic == 0x10B:
        directories = optional + 96
    else:
        raise InvalidPe("Unsupported PE optional header")
    if directories + 7 * 8 > sections_start:
        raise InvalidPe("Truncated PE data directories")
    sections = []
    for index in range(section_count):
        section = sections_start + index * 40
        virtual_size = u32(data, section + 8)
        virtual_address = u32(data, section + 12)
        raw_size = u32(data, section + 16)
        raw_offset = u32(data, section + 20)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))

    def rva_offset(rva: int, size: int) -> int:
        for address, span, raw_offset, raw_size in sections:
            if address <= rva and rva + size <= address + span:
                relative = rva - address
                if relative + size <= raw_size and raw_offset + relative + size <= len(data):
                    return raw_offset + relative
        raise InvalidPe("PE directory points outside file data")

    def c_string(offset: int) -> str:
        end = data.find(b"\0", offset, min(len(data), offset + 4096))
        if end < 0:
            raise InvalidPe("Unterminated PE string")
        return data[offset:end].decode("ascii", "replace")

    exports: list[str] = []
    export_rva = u32(data, directories)
    export_size = u32(data, directories + 4)
    if export_rva and export_size:
        export = rva_offset(export_rva, 40)
        name_count = u32(data, export + 24)
        if name_count > 100_000:
            raise InvalidPe("Unreasonable PE export count")
        if name_count:
            name_table = rva_offset(u32(data, export + 32), name_count * 4)
            for index in range(name_count):
                exports.append(c_string(rva_offset(u32(data, name_table + index * 4), 1)))

    pdb_names: list[str] = []
    absolute_pdb_paths = 0
    debug_rva = u32(data, directories + 6 * 8)
    debug_size = u32(data, directories + 6 * 8 + 4)
    if debug_rva and debug_size:
        if debug_size % 28 or debug_size > 28 * 128:
            raise InvalidPe("Invalid PE debug directory")
        debug = rva_offset(debug_rva, debug_size)
        for index in range(debug_size // 28):
            entry = debug + index * 28
            if u32(data, entry + 12) != 2:  # IMAGE_DEBUG_TYPE_CODEVIEW
                continue
            size = u32(data, entry + 16)
            offset = u32(data, entry + 24)
            if size < 25 or offset + size > len(data) or data[offset : offset + 4] != b"RSDS":
                continue
            pdb = c_string(offset + 24)
            pdb_names.append(Path(pdb.replace("\\", "/")).name)
            if re.match(r"^[A-Za-z]:[\\/]", pdb) or pdb.startswith("\\\\"):
                absolute_pdb_paths += 1

    strings = [match.group().decode("ascii", "replace")
               for match in re.finditer(rb"[ -~]{8,}", data)]
    return {
        "file": str(path),
        "bytes": len(data),
        "exports": exports,
        "pdbNames": pdb_names,
        "absolutePdbPaths": absolute_pdb_paths,
        "asciiStringsAtLeast8": len(strings),
        "rttiTypeNames": sum(value.startswith(".?AV") for value in strings),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    parser.add_argument("--strict", action="store_true",
                        help="fail on absolute PDB paths or unexpected Game Module exports")
    args = parser.parse_args()
    try:
        report = inspect(args.module)
    except (OSError, InvalidPe) as error:
        parser.exit(2, f"audit failed: {error}\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    if args.strict and (report["absolutePdbPaths"]
                        or report["exports"] != ["LamaPonGetGameModule"]):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
