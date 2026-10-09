"""Inspect in-memory ELF fixtures; no temporary folders or executable loading."""
import io
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import native_linux as LINUX


class NativeLinuxToolTests(unittest.TestCase):
    def elf(self, needed=(), soname=None, rpath=None, runpath=None, machine=62, main=True, flags1=None, extra=()):
        data = bytearray(4096)
        data[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<HHIQQQIHHHHHH", data, 16, 3, machine, 1,
                         0x1100 if main else 0, 64, 0, 0, 64, 56, 3 if main else 2, 0, 0, 0)
        struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0x1000, 0, len(data), len(data), 4096)
        strings = bytearray(b"\0")
        tags = [(5, 0x1400)]
        for tag, values in ((1, needed), (14, () if soname is None else (soname,)),
                            (15, () if rpath is None else (rpath,)),
                            (29, () if runpath is None else (runpath,))):
            for value in values:
                tags.append((tag, len(strings)))
                strings.extend(value.encode("ascii") + b"\0")
        tags += [(0x6ffffffb, 0x08000000 if flags1 is None and main else flags1 or 0)]
        tags += list(extra) + [(10, len(strings)), (0, 0)]
        length = len(tags) * 16
        struct.pack_into("<IIQQQQQQ", data, 120, 2, 6, 512, 0x1200, 0, length, length, 8)
        for index, (tag, value) in enumerate(tags):
            struct.pack_into("<qQ", data, 512 + index * 16, tag, value)
        data[1024:1024 + len(strings)] = strings
        if main:
            interpreter = sorted(LINUX.INTERPRETERS[machine])[0].encode("ascii") + b"\0"
            struct.pack_into("<IIQQQQQQ", data, 176, 3, 4, 256, 0x1100, 0,
                             len(interpreter), len(interpreter), 1)
            data[256:256 + len(interpreter)] = interpreter
        return bytes(data)

    def inspect(self, data):
        return LINUX.inspect_elf(io.BytesIO(data), len(data), "Game")

    def package(self, contents, escape=None, runtime_libraries=()):
        root = ROOT / "test-output/platform-core/not-created-elf-probe"
        files = {root / name: data for name, data in contents.items()}
        def resolve(path, *args, **kwargs):
            return ROOT / path.name if path.name == escape else path
        with mock.patch.object(Path, "iterdir", return_value=iter(files)), \
                mock.patch.object(Path, "resolve", resolve), \
                mock.patch.object(Path, "is_file", lambda path: path in files), \
                mock.patch.object(Path, "open", lambda path, *args, **kwargs: io.BytesIO(files[path])), \
                mock.patch.object(Path, "stat", lambda path, *args, **kwargs: SimpleNamespace(st_size=len(files[path]))):
            return LINUX.check_package(root / "Game", runtime_libraries)

    def test_reads_needed_soname_and_search_paths(self):
        result = self.inspect(self.elf(["libSDL3.so.0", "libc.so.6"], runpath="$ORIGIN"))
        self.assertEqual(result["needed"], ["libSDL3.so.0", "libc.so.6"])
        self.assertEqual(result["runpath"], ["$ORIGIN"])
        self.assertTrue(result["executableEntry"])
        self.assertEqual(self.inspect(self.elf(soname="libSDL3.so.0", main=False))["soname"], "libSDL3.so.0")

    def test_rejects_headers_bounds_and_alignment(self):
        original = self.elf()
        wrong_class = bytearray(original); wrong_class[4] = 1
        wrong_endian = bytearray(original); wrong_endian[5] = 2
        wrong_cpu = bytearray(original); struct.pack_into("<H", wrong_cpu, 18, 40)
        bad_table = bytearray(original); struct.pack_into("<H", bad_table, 54, 55)
        bad_alignment = bytearray(original); struct.pack_into("<Q", bad_alignment, 112, 1000)
        for data in (original[:32], original[:-1], wrong_class, wrong_endian, wrong_cpu, bad_table, bad_alignment):
            with self.subTest(data=bytes(data[:24])):
                with self.assertRaises(LINUX.ExportError):
                    self.inspect(data)

    def test_rejects_dynamic_virtual_address_and_unterminated_metadata(self):
        original = self.elf()
        out_of_file = bytearray(original); struct.pack_into("<Q", out_of_file, 512 + 8, 0x3000)
        disagreement = bytearray(original); struct.pack_into("<Q", disagreement, 120 + 8, 528)
        unterminated = bytearray(original)
        dynamic_length = struct.unpack_from("<Q", original, 120 + 32)[0]
        struct.pack_into("<qQ", unterminated, 512 + dynamic_length - 16, 1, 0)
        bad_string = bytearray(self.elf(["libSDL3.so.0"])); struct.pack_into("<Q", bad_string, 536, 5000)
        for data in (out_of_file, disagreement, unterminated, bad_string):
            with self.assertRaises(LINUX.ExportError):
                self.inspect(data)

    def test_rejects_nonportable_library_names_and_search_paths(self):
        for name in ("/opt/SDK/libSDL3.so.0", "../libSDL3.so.0", "libSDL3.dll", ""):
            with self.subTest(name=name), self.assertRaises(LINUX.ExportError):
                self.inspect(self.elf([name]))
        for path in ("/home/developer/build/sdl", "$ORIGIN/../SDK", ".", "$ORIGIN:", ""):
            with self.subTest(path=path), self.assertRaises(LINUX.ExportError):
                self.inspect(self.elf(runpath=path))

    def test_recursive_dependencies_and_cycles_keep_verification_limits(self):
        result = self.package({
            "Game": self.elf(["libSDL3.so.0", "libstdc++.so.6"], runpath="$ORIGIN"),
            "libSDL3.so.0": self.elf(["libCodec.so.1", "libc.so.6"], soname="libSDL3.so.0", runpath="$ORIGIN", main=False),
            "libCodec.so.1": self.elf(["libSDL3.so.0"], soname="libCodec.so.1", rpath="${ORIGIN}", main=False),
        })
        self.assertEqual(result["bundledLibraries"], ["libCodec.so.1", "libSDL3.so.0"])
        self.assertEqual(result["systemLibraries"], ["libc.so.6", "libstdc++.so.6"])
        self.assertFalse(result["abiCompatibilityVerified"])
        self.assertFalse(result["cleanMachineVerified"])

    def test_staged_runtime_libraries_are_checked_even_when_dynamically_loaded(self):
        result = self.package({"Game": self.elf(),
                               "libPlugin.so": self.elf(soname="libPlugin.so", main=False)},
                              runtime_libraries=["libPlugin.so"])
        self.assertEqual(result["bundledLibraries"], ["libPlugin.so"])
        with self.assertRaisesRegex(LINUX.ExportError, "declared runtime library is not bundled"):
            self.package({"Game": self.elf()}, runtime_libraries=["libPlugin.so"])
        with self.assertRaisesRegex(LINUX.ExportError, "CPU architecture"):
            self.package({"Game": self.elf(),
                          "libPlugin.so": self.elf(soname="libPlugin.so", main=False, machine=183)})

    def test_rejects_missing_indirect_dependency_and_wrong_case(self):
        for dependencies in (["libCodec.so.1"], ["LIBC.so.6"]):
            with self.assertRaisesRegex(LINUX.ExportError, "not bundled"):
                self.package({"Game": self.elf(["libSDL3.so.0"], runpath="$ORIGIN"),
                              "libSDL3.so.0": self.elf(dependencies, main=False)})

    def test_rejects_wrong_cpu_soname_or_library_kind(self):
        for library in (self.elf(main=False, machine=183), self.elf(soname="libOther.so.0", main=False), self.elf()):
            with self.assertRaises(LINUX.ExportError):
                self.package({"Game": self.elf(["libSDL3.so.0"], runpath="$ORIGIN"), "libSDL3.so.0": library})

    def test_local_dependencies_need_their_own_origin_search_path(self):
        with self.assertRaisesRegex(LINUX.ExportError, "requires.*ORIGIN"):
            self.package({"Game": self.elf(["libSDL3.so.0"], runpath="$ORIGIN"),
                          "libSDL3.so.0": self.elf(["libCodec.so.1"], main=False),
                          "libCodec.so.1": self.elf(main=False)})
        with self.assertRaisesRegex(LINUX.ExportError, "requires.*ORIGIN"):
            self.package({"Game": self.elf(["libstdc++.so.6"]),
                          "libstdc++.so.6": self.elf(main=False)})

    def test_rejects_escaping_symlink_missing_entry_and_foreign_loader(self):
        with self.assertRaisesRegex(LINUX.ExportError, "escapes"):
            self.package({"Game": self.elf(["libSDL3.so.0"], runpath="$ORIGIN"),
                          "libSDL3.so.0": self.elf(main=False)}, escape="libSDL3.so.0")
        with self.assertRaisesRegex(LINUX.ExportError, "entry point"):
            self.package({"Game": self.elf(main=False)})
        with self.assertRaisesRegex(LINUX.ExportError, "different CPU"):
            self.package({"Game": self.elf(["ld-linux-aarch64.so.1"])})

    def test_loader_flags_and_aarch64_output(self):
        result = self.package({"Game": self.elf(["libc.so.6"], machine=183)})
        self.assertEqual(result["machine"], 183)
        # A shared object may have its own interpreter (e.g. libc); PIE flags,
        # rather than PT_INTERP alone, distinguish a PIE dependency.
        result = self.package({"Game": self.elf(["libExample.so.1"], runpath="$ORIGIN"),
                               "libExample.so.1": self.elf(main=True, flags1=0, soname="libExample.so.1")})
        self.assertEqual(result["bundledLibraries"], ["libExample.so.1"])
        with self.assertRaisesRegex(LINUX.ExportError, "default-library search"):
            self.package({"Game": self.elf(["libc.so.6"], flags1=0x08000800)})

    def test_special_library_loading_cannot_bypass_dependency_gate(self):
        for tag in (0x6ffffefb, 0x6ffffefc, 0x7ffffffd, 0x7fffffff):
            with self.subTest(tag=tag), self.assertRaisesRegex(LINUX.ExportError, "audit/filter"):
                self.inspect(self.elf(extra=[(tag, 0)]))


if __name__ == "__main__":
    unittest.main()
