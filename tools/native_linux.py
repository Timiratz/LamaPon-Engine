"""Read Linux ELF dependencies without executing the game or invoking ldd."""
from pathlib import Path
import re
import struct

from export_web import ExportError


# These remain requirements on the destination OS, not bundled redistributables.
SYSTEM_LIBRARIES = {
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
    "libresolv.so.2", "libutil.so.1", "libgcc_s.so.1", "libstdc++.so.6",
    "ld-linux-x86-64.so.2", "ld-linux-aarch64.so.1",
}
INTERPRETERS = {
    62: {"/lib64/ld-linux-x86-64.so.2", "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"},
    183: {"/lib/ld-linux-aarch64.so.1", "/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"},
}
LIBRARY_NAME = re.compile(r"[A-Za-z0-9_+.-]+\.so(?:\.[A-Za-z0-9_+.-]+)*")


def inspect_elf(stream, size: int, name: str) -> dict:
    def reject(reason):
        raise ExportError(f"Invalid Linux ELF file {name}: {reason}")

    def read(offset, length):
        if offset < 0 or length < 0 or offset + length > size:
            reject("truncated or out-of-range data")
        stream.seek(offset)
        value = stream.read(length)
        if len(value) != length:
            reject("truncated data")
        return value

    header = read(0, 64)
    if header[:7] != b"\x7fELF\x02\x01\x01" or header[7] not in {0, 3}:
        reject("expected a 64-bit little-endian Linux ELF")
    kind, machine, version = struct.unpack_from("<HHI", header, 16)
    ph_offset = struct.unpack_from("<Q", header, 32)[0]
    entry_point = struct.unpack_from("<Q", header, 24)[0]
    header_size, ph_size, ph_count = struct.unpack_from("<HHH", header, 52)
    if kind not in {2, 3} or machine not in INTERPRETERS or version != 1:
        reject("unsupported image kind or CPU architecture")
    if header_size != 64 or ph_size != 56 or not 1 <= ph_count <= 4096:
        reject("invalid program header table")
    headers = read(ph_offset, ph_size * ph_count)
    loads, executable_ranges, dynamics, interpreters = [], [], [], []
    for index in range(ph_count):
        segment, flags, offset, address, physical, file_size, memory_size, alignment = struct.unpack_from(
            "<IIQQQQQQ", headers, index * ph_size)
        if offset + file_size > size or file_size > memory_size:
            reject("invalid segment bounds")
        if segment == 1:
            if alignment > 1 and (alignment & (alignment - 1) or address % alignment != offset % alignment):
                reject("invalid LOAD alignment")
            loads.append((address, offset, file_size))
            if flags & 1:
                executable_ranges.append((address, address + file_size))
        elif segment == 2:
            dynamics.append((offset, address, file_size))
        elif segment == 3:
            if not 2 <= file_size <= 4096:
                reject("invalid interpreter size")
            text = read(offset, file_size)
            if text[-1:] != b"\0" or b"\0" in text[:-1]:
                reject("invalid interpreter string")
            try:
                interpreter = text[:-1].decode("ascii")
            except UnicodeError:
                reject("interpreter is not ASCII")
            if interpreter not in INTERPRETERS[machine]:
                reject("interpreter does not match the Linux CPU architecture")
            interpreters.append(interpreter)
    if not loads or len(dynamics) > 1 or len(interpreters) > 1:
        reject("missing LOAD or duplicate DYNAMIC/INTERP segments")

    def at_address(address, length):
        candidates = [offset + address - start for start, offset, count in loads
                      if start <= address and address + length <= start + count]
        if len(candidates) != 1:
            reject("dynamic address is not unambiguously backed by file data")
        return read(candidates[0], length)

    result = {"machine": machine, "kind": kind, "interpreter": next(iter(interpreters), None),
              "entryPoint": entry_point,
              "executableEntry": bool(entry_point) and any(start <= entry_point < end for start, end in executable_ranges),
              "flags1": 0,
              "needed": [], "soname": None, "rpath": [], "runpath": []}
    if not dynamics:
        if interpreters:
            reject("interpreter present without dynamic metadata")
        return result
    offset, address, length = dynamics[0]
    if length < 16 or length % 16 or length > 1024 * 1024:
        reject("invalid dynamic segment size")
    data = at_address(address, length)
    if data != read(offset, length):
        reject("dynamic segment file and virtual addresses disagree")
    tags, needed = {}, []
    for position in range(0, length, 16):
        tag, value = struct.unpack_from("<qQ", data, position)
        if tag == 0:
            break
        if tag in {0x6ffffefb, 0x6ffffefc, 0x7ffffffd, 0x7fffffff}:
            reject("audit/filter library loading is not supported by the distribution checker")
        if tag == 1:
            needed.append(value)
        elif tag in {5, 10, 14, 15, 29, 0x6ffffffb}:
            if tag in tags:
                reject("duplicate dynamic metadata")
            tags[tag] = value
    else:
        reject("dynamic segment is not terminated")
    if 5 not in tags or not 1 <= tags.get(10, 0) <= 64 * 1024 * 1024:
        reject("missing or oversized dynamic string table")
    strings = at_address(tags[5], tags[10])

    def string(position):
        if position >= len(strings):
            reject("string offset outside the dynamic string table")
        end = strings.find(b"\0", position, min(len(strings), position + 4097))
        if end < 0:
            reject("unterminated or oversized dynamic string")
        try:
            return strings[position:end].decode("ascii")
        except UnicodeError:
            reject("dynamic dependency string is not ASCII")

    def library(position):
        value = string(position)
        if not LIBRARY_NAME.fullmatch(value):
            reject("dependency must be a bare shared library name")
        return value

    result["needed"] = list(dict.fromkeys(library(position) for position in needed))
    result["flags1"] = tags.get(0x6ffffffb, 0)
    if 14 in tags:
        result["soname"] = library(tags[14])
    for key, tag in (("rpath", 15), ("runpath", 29)):
        if tag in tags:
            paths = string(tags[tag]).split(":")
            if any(path not in {"$ORIGIN", "${ORIGIN}"} for path in paths):
                reject("runtime search paths must name only the game directory ($ORIGIN)")
            result[key] = paths
    return result


def check_package(executable: Path, expected_runtime_libraries=()) -> dict:
    root = executable.parent.resolve()
    if executable.resolve().parent != root:
        raise ExportError("Linux executable escapes the game output")
    bundled = {}
    for path in root.iterdir():
        if path.is_file() and LIBRARY_NAME.fullmatch(path.name):
            if path.resolve().parent != root:
                raise ExportError("Linux library escapes the game output: " + path.name)
            bundled[path.name] = path
    expected_runtime_libraries = set(expected_runtime_libraries)
    if any(not LIBRARY_NAME.fullmatch(name) for name in expected_runtime_libraries):
        raise ExportError("Linux declared runtime library name is invalid")
    missing_runtime_libraries = expected_runtime_libraries - bundled.keys()
    if missing_runtime_libraries:
        raise ExportError("Linux declared runtime library is not bundled: "
                          + ", ".join(sorted(missing_runtime_libraries)))
    # Inspect every staged shared object, including dlopen-style runtime files
    # that are not reachable through a static DT_NEEDED edge from the game.
    pending = [executable, *bundled.values()]
    checked, systems, required, images = set(), set(), set(bundled), {}
    machine = None
    while pending:
        path = pending.pop()
        if path.name in checked:
            continue
        checked.add(path.name)
        with path.open("rb") as stream:
            image = inspect_elf(stream, path.stat().st_size, path.name)
        if machine is None:
            machine = image["machine"]
        elif machine != image["machine"]:
            raise ExportError("Linux dependency has a different CPU architecture: " + path.name)
        if path != executable and (image["kind"] != 3 or image["flags1"] & 0x08000000):
            raise ExportError("Linux dependency is not a shared library: " + path.name)
        if path == executable and (not image["executableEntry"] or (image["needed"] and not image["interpreter"])):
            raise ExportError("Linux game is missing an executable entry point or dynamic interpreter")
        if path != executable and image["soname"] not in {None, path.name}:
            raise ExportError("Linux library name does not match its SONAME: " + path.name)
        images[path.name] = image
        for dependency in image["needed"]:
            # A local copy takes precedence over the OS contract; inspect it too.
            if dependency in bundled:
                if not (image["runpath"] or image["rpath"]):
                    raise ExportError("Linux bundled dependency requires $ORIGIN in its parent's search path: " + dependency)
                required.add(dependency)
                pending.append(bundled[dependency])
            elif dependency in SYSTEM_LIBRARIES:
                if image["flags1"] & 0x00000800:
                    raise ExportError("Linux OS runtime dependency requires default-library search: " + dependency)
                if dependency.startswith("ld-linux-") and dependency not in {
                        Path(value).name for value in INTERPRETERS[machine]}:
                    raise ExportError("Linux loader dependency has a different CPU architecture: " + dependency)
                systems.add(dependency)
            else:
                raise ExportError("Linux game dependency is not bundled: " + dependency)
    return {"machine": machine, "bundledLibraries": sorted(required), "systemLibraries": sorted(systems),
            "images": images, "abiCompatibilityVerified": False, "cleanMachineVerified": False}
