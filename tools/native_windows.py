"""Inspect Windows PE imports and bundled dependencies without loading DLLs."""
from pathlib import Path
import re
import struct

from export_web import ExportError


SYSTEM_DLLS = {
    "kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll", "gdi32.dll", "winmm.dll",
    "imm32.dll", "ole32.dll", "oleaut32.dll", "version.dll", "advapi32.dll", "setupapi.dll",
    "shell32.dll", "combase.dll", "comdlg32.dll", "comctl32.dll", "ws2_32.dll", "dwmapi.dll",
    "d3d11.dll", "d3d12.dll", "dxgi.dll", "opengl32.dll", "shcore.dll", "cfgmgr32.dll",
    "powrprof.dll", "dbghelp.dll", "iphlpapi.dll", "bcrypt.dll", "crypt32.dll", "wintrust.dll",
    "secur32.dll", "uxtheme.dll", "propsys.dll", "psapi.dll", "msimg32.dll", "normaliz.dll",
    "windowscodecs.dll", "avrt.dll", "mfplat.dll", "mf.dll", "mfreadwrite.dll", "ucrtbase.dll",
    "rpcrt4.dll", "imagehlp.dll", "winspool.drv", "winhttp.dll", "wininet.dll", "urlmon.dll",
    "hid.dll", "msvcrt.dll", "winusb.dll", "win32u.dll", "twinapi.appcore.dll",
}


def inspect_pe(stream, size: int, name: str) -> dict:
    def reject(reason):
        raise ExportError(f"Invalid Windows PE file {name}: {reason}")

    def read(offset, length):
        if offset < 0 or length < 0 or offset + length > size:
            reject("truncated or out-of-range data")
        stream.seek(offset)
        value = stream.read(length)
        if len(value) != length:
            reject("truncated data")
        return value

    dos = read(0, 64)
    if dos[:2] != b"MZ":
        reject("DOS header is missing")
    offset = struct.unpack_from("<I", dos, 60)[0]
    header = read(offset, 24)
    if header[:4] != b"PE\0\0":
        reject("PE signature is missing")
    machine, section_count = struct.unpack_from("<HH", header, 4)
    characteristics = struct.unpack_from("<H", header, 22)[0]
    if not characteristics & 2 or bool(characteristics & 0x2000) != name.lower().endswith((".dll", ".drv")):
        reject("image kind does not match executable or DLL output")
    optional_size = struct.unpack_from("<H", header, 20)[0]
    if machine not in {0x14c, 0x8664, 0xaa64} or not 1 <= section_count <= 96:
        reject("unsupported architecture or section table")
    optional = read(offset + 24, optional_size)
    if len(optional) < 2:
        reject("optional header is missing")
    magic = struct.unpack_from("<H", optional)[0]
    directory_offset = {0x10b: 96, 0x20b: 112}.get(magic)
    if directory_offset is None or len(optional) < directory_offset:
        reject("invalid optional header")
    if (machine == 0x14c) != (magic == 0x10b):
        reject("architecture and optional header disagree")
    directory_count = struct.unpack_from("<I", optional, directory_offset - 4)[0]
    if directory_count > 16 or len(optional) < directory_offset + directory_count * 8:
        reject("invalid data directory table")
    sections = []
    for index in range(section_count):
        section = read(offset + 24 + optional_size + index * 40, 40)
        virtual_size, address, raw_size, raw_offset = struct.unpack_from("<IIII", section, 8)
        if raw_offset + raw_size > size:
            reject("section extends past file")
        sections.append((address, virtual_size, raw_offset, raw_size))

    def at_rva(address, length):
        for base, virtual_size, raw_offset, raw_size in sections:
            if base <= address and address + length <= base + min(virtual_size, raw_size):
                return read(raw_offset + address - base, length)
        reject("import data is outside a file-backed section")

    def dll_name(address):
        value = bytearray()
        for index in range(256):
            byte = at_rva(address + index, 1)
            if byte == b"\0":
                break
            value.extend(byte)
        else:
            reject("unterminated DLL name")
        try:
            result = value.decode("ascii").lower()
        except UnicodeError:
            reject("DLL name is not ASCII")
        if not result or any(character in result for character in "/\\:") \
                or result in {".", ".."} or not result.endswith((".dll", ".drv")):
            reject("invalid DLL name")
        return result

    imports = set()
    for directory_index, entry_size, name_offset in ((1, 20, 12), (13, 32, 4)):
        if directory_count <= directory_index:
            continue
        address, length = struct.unpack_from("<II", optional, directory_offset + directory_index * 8)
        if address == 0 and length == 0:
            continue
        if not address or length < entry_size or length > 1024 * 1024:
            reject("invalid import directory")
        for index in range(length // entry_size):
            entry = at_rva(address + index * entry_size, entry_size)
            if not any(entry):
                break
            # Modern delay-import descriptors use RVAs. Reject legacy VA form
            # rather than silently omitting dependencies from the package gate.
            if directory_index == 13 and struct.unpack_from("<I", entry)[0] != 1:
                reject("unsupported delay-import address form")
            imports.add(dll_name(struct.unpack_from("<I", entry, name_offset)[0]))
        else:
            reject("import directory is not terminated")
    return {"machine": machine, "imports": sorted(imports)}


def check_package(executable: Path, expected_runtime_libraries=()) -> dict:
    root = executable.parent.resolve()
    bundled = {}
    for path in root.iterdir():
        if path.is_file() and path.suffix.lower() in {".dll", ".drv"}:
            key = path.name.lower()
            if key in bundled or path.resolve().parent != root:
                raise ExportError("Windows DLL names collide or escape the game output: " + path.name)
            bundled[key] = path
    expected_runtime_libraries = {name.lower() for name in expected_runtime_libraries}
    if any(not re.fullmatch(r"[A-Za-z0-9_+.-]+\.dll", name) for name in expected_runtime_libraries):
        raise ExportError("Windows declared runtime DLL name is invalid")
    missing_runtime_libraries = expected_runtime_libraries - bundled.keys()
    if missing_runtime_libraries:
        raise ExportError("Windows declared runtime DLL is not bundled: "
                          + ", ".join(sorted(missing_runtime_libraries)))
    # Inspect every staged DLL, including runtime files loaded dynamically by a package.
    pending = [executable, *bundled.values()]
    checked, systems, required = set(), set(), set(bundled)
    machine = None
    while pending:
        path = pending.pop()
        if path.name.lower() in checked:
            continue
        checked.add(path.name.lower())
        with path.open("rb") as stream:
            image = inspect_pe(stream, path.stat().st_size, path.name)
        if machine is None:
            machine = image["machine"]
        elif machine != image["machine"]:
            raise ExportError("Windows dependency has a different CPU architecture: " + path.name)
        for dependency in image["imports"]:
            if re.fullmatch(r"(?:msvcp|vcruntime|concrt|msvcr)\d+(?:_\d+)?d(?:_[a-z_]+)?\.dll", dependency):
                raise ExportError("Windows Debug runtimes cannot be packaged for distribution; use Release: " + dependency)
            if dependency in SYSTEM_DLLS or dependency.startswith(("api-ms-win-", "ext-ms-win-")):
                systems.add(dependency)
            elif dependency in bundled:
                required.add(dependency)
                pending.append(bundled[dependency])
            else:
                raise ExportError("Windows game dependency is not bundled: " + dependency)
    msvc_runtime = any(re.match(r"(?:msvcp|vcruntime|concrt|msvcr)\d", name) for name in required)
    notice = root / "licenses/WindowsRuntime.txt"
    if msvc_runtime and (not notice.is_file() or not notice.stat().st_size):
        raise ExportError("Windows runtime deployment notice was not staged")
    return {"machine": machine, "bundledLibraries": sorted(required),
            "runtimeNoticeIncluded": msvc_runtime,
            "systemLibraries": sorted(systems), "cleanMachineVerified": False}
