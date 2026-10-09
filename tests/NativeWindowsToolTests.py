"""Inspect synthetic PE files in memory; no temporary files or DLL loading."""
import io
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import native_windows as WINDOWS


class NativeWindowsToolTests(unittest.TestCase):
    def pe(self, imports=(), delay=(), machine=0x8664, dll=False):
        data = bytearray(2048)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 128)
        data[128:132] = b"PE\0\0"
        struct.pack_into("<HH", data, 132, machine, 1)
        struct.pack_into("<HH", data, 148, 240, 2 | (0x2000 if dll else 0))
        struct.pack_into("<H", data, 152, 0x20b)
        struct.pack_into("<I", data, 152 + 108, 16)
        struct.pack_into("<IIII", data, 392 + 8, 1536, 0x1000, 1536, 512)
        name_address = 0x1200
        for index, names, directory, address, stride, name_offset in (
                (1, imports, 1, 0x1100, 20, 12), (13, delay, 13, 0x1400, 32, 4)):
            if not names:
                continue
            struct.pack_into("<II", data, 152 + 112 + directory * 8, address, (len(names) + 1) * stride)
            for number, name in enumerate(names):
                location = 512 + address - 0x1000 + number * stride
                if index == 13:
                    struct.pack_into("<I", data, location, 1)
                struct.pack_into("<I", data, location + name_offset, name_address)
                encoded = name.encode("ascii") + b"\0"
                start = 512 + name_address - 0x1000
                data[start:start + len(encoded)] = encoded
                name_address += 64
        return bytes(data)

    def inspect(self, data, name="Game.exe"):
        return WINDOWS.inspect_pe(io.BytesIO(data), len(data), name)

    def package(self, contents, notice=True, runtime_libraries=()):
        root = ROOT / "test-output/platform-core/not-created-pe-probe"
        files = {root / name: content for name, content in contents.items()}
        if notice:
            files[root / "licenses/WindowsRuntime.txt"] = b"Runtime deployment notice"
        with mock.patch.object(Path, "iterdir", return_value=iter(files)), \
                mock.patch.object(Path, "resolve", lambda path, *args, **kwargs: path), \
                mock.patch.object(Path, "is_file", lambda path: path in files), \
                mock.patch.object(Path, "open", lambda path, *args, **kwargs: io.BytesIO(files[path])), \
                mock.patch.object(Path, "stat", lambda path, *args, **kwargs: SimpleNamespace(st_size=len(files[path]))):
            return WINDOWS.check_package(root / "Game.exe", runtime_libraries)

    def test_reads_regular_and_delay_imports_without_loading(self):
        result = self.inspect(self.pe(["KERNEL32.dll", "MSVCP140.dll"], ["SDL3.dll"]))
        self.assertEqual(result["imports"], ["kernel32.dll", "msvcp140.dll", "sdl3.dll"])
        self.assertEqual(result["machine"], 0x8664)

    def test_rejects_truncated_headers_section_ranges_and_import_addresses(self):
        valid = self.pe(["MSVCP140.dll"])
        section = bytearray(valid); struct.pack_into("<I", section, 392 + 20, len(valid))
        address = bytearray(valid); struct.pack_into("<I", address, 512 + 0x100 + 12, 0xfffffff0)
        for data in (valid[:63], valid[:160], valid[:700], section, address, b"MZ" + bytes(100)):
            with self.subTest(size=len(data)), self.assertRaises(WINDOWS.ExportError):
                self.inspect(data)

    def test_rejects_architecture_header_mismatch_and_unsafe_dll_names(self):
        for name in ("../MSVCP140.dll", "C:MSVCP140.dll", "MSVCP140/evil.dll"):
            with self.subTest(name=name), self.assertRaises(WINDOWS.ExportError):
                self.inspect(self.pe([name]))
        with self.assertRaises(WINDOWS.ExportError):
            self.inspect(self.pe(machine=0x14c))
        with self.assertRaises(WINDOWS.ExportError):
            self.inspect(self.pe(dll=True))

    def test_recursive_dependency_graph_and_api_sets(self):
        result = self.package({
            "Game.exe": self.pe(["MSVCP140.dll", "KERNEL32.dll"]),
            "msvcp140.dll": self.pe(["vcruntime140.dll", "api-ms-win-crt-runtime-l1-1-0.dll"], dll=True),
            "vcruntime140.dll": self.pe(["KERNEL32.dll"], dll=True),
        })
        self.assertEqual(result["bundledLibraries"], ["msvcp140.dll", "vcruntime140.dll"])
        self.assertFalse(result["cleanMachineVerified"])

    def test_staged_runtime_dlls_are_checked_even_when_loaded_dynamically(self):
        result = self.package({"Game.exe": self.pe(),
                               "Plugin.dll": self.pe(dll=True)}, runtime_libraries=["Plugin.dll"])
        self.assertEqual(result["bundledLibraries"], ["plugin.dll"])
        with self.assertRaisesRegex(WINDOWS.ExportError, "declared runtime DLL is not bundled"):
            self.package({"Game.exe": self.pe()}, runtime_libraries=["Plugin.dll"])
        with self.assertRaisesRegex(WINDOWS.ExportError, "CPU architecture"):
            self.package({"Game.exe": self.pe(),
                          "Plugin.dll": self.pe(dll=True, machine=0xaa64)})

    def test_missing_transitive_dependency_cannot_pass(self):
        with self.assertRaisesRegex(WINDOWS.ExportError, "vcruntime140.dll"):
            self.package({"Game.exe": self.pe(["MSVCP140.dll"]),
                          "msvcp140.dll": self.pe(["vcruntime140.dll"], dll=True)})

    def test_wrong_cpu_library_cannot_pass(self):
        with self.assertRaisesRegex(WINDOWS.ExportError, "CPU architecture"):
            self.package({"Game.exe": self.pe(["MSVCP140.dll"]),
                          "msvcp140.dll": self.pe(dll=True, machine=0xaa64)})

    def test_msvc_runtime_requires_its_deployment_notice(self):
        with self.assertRaisesRegex(WINDOWS.ExportError, "notice"):
            self.package({"Game.exe": self.pe(["MSVCP140.dll"]),
                          "msvcp140.dll": self.pe(dll=True)}, notice=False)

    def test_missing_delayed_dependency_and_debug_runtime_cannot_pass(self):
        for dependency in ("SDL3.dll", "vcruntime140d.dll", "msvcp140d_atomic_wait.dll"):
            with self.subTest(dependency=dependency), self.assertRaises(WINDOWS.ExportError):
                self.package({"Game.exe": self.pe(delay=[dependency])})


if __name__ == "__main__":
    unittest.main()
