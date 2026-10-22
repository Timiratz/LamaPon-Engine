"""Exercise iOS metadata and export generation in memory, without temporary files."""
import io
import json
import plistlib
import struct
import unittest
from pathlib import Path
from unittest import mock

import NativeExportToolTests as fixture

NATIVE, ROOT = fixture.NATIVE, fixture.ROOT
import native_ios


class NativeIosToolTests(unittest.TestCase):
    def settings(self, **changes):
        return native_ios.ios_settings({'project': {'export': {'native': {'ios': {
            'bundleIdentifier': 'org.example.game', **changes}}}}})

    def inspect(self):
        return fixture.NativeExportToolTests().inspect_fixture(platform='ios', project_settings={
            'export': {'native': {'sources': ['NativeSmoke.cpp'],
                                  'ios': {'bundleIdentifier': 'org.example.game'}}}})

    def test_missing_identifier_is_rejected_before_generation(self):
        report, _ = fixture.NativeExportToolTests().inspect_fixture(platform='ios')
        self.assertFalse(report['canGenerateBuildProject'])
        self.assertIn('invalid-ios-settings', [f['code'] for f in report['findings']])

    def test_invalid_metadata_is_rejected(self):
        for changes in ({'bundleIdentifier': 'a/b'}, {'buildNumber': '0'}, {'version': '1;rm'},
                        {'minimumVersion': '14.0'}, {'orientation': 'up'}, {'unknown': True},
                        {'buildNumber': 1}, {'version': '1.0'}):
            with self.subTest(changes=changes), self.assertRaises(NATIVE.portable.ExportError):
                self.settings(**changes)

    def test_xml_names_are_escaped_and_launch_screen_is_present(self):
        settings = self.settings(orientation='portrait')
        info = plistlib.loads(native_ios.info_plist(settings, 'Game', 'A&B <Game>').encode())
        self.assertEqual(info['CFBundleDisplayName'], 'A&B <Game>')
        self.assertEqual(info['UISupportedInterfaceOrientations'], ['UIInterfaceOrientationPortrait'])
        self.assertEqual(info['UILaunchScreen'], {})
        self.assertTrue(info['LSRequiresIPhoneOS'])

    def test_generation_remains_explicitly_unverified(self):
        report, context = self.inspect()
        self.assertTrue(report['canGenerateBuildProject'], report['findings'])
        self.assertFalse(report['verified'])
        written = {}
        with mock.patch.object(Path, 'exists', return_value=False), mock.patch.object(Path, 'mkdir'), \
                mock.patch.object(Path, 'write_text', autospec=True,
                                  side_effect=lambda p, text, **kw: written.setdefault(p.name, text)):
            NATIVE.generate_build_project(ROOT / 'unused-ios-output', 'ios', report, context)
        self.assertIn('CMAKE_SYSTEM_NAME STREQUAL "iOS"', written['CMakeLists.txt'])
        self.assertIn('LAMAPON_IOS_INFO_PLIST', written['CMakeLists.txt'])
        description = json.loads(written['native-build-project.json'])
        self.assertEqual(description['ios']['bundleIdentifier'], 'org.example.game')
        self.assertEqual(plistlib.loads(written['Info.plist'].encode())['CFBundleExecutable'], description['target'])

    def test_bundle_does_not_claim_signing_or_app_store_readiness(self):
        settings = self.settings()
        plist = native_ios.info_plist(settings, 'Game', 'Game').encode()
        header = struct.pack('<8I', 0xFEEDFACF, 0x0100000C, 0, 2, 0, 0, 0, 0)
        with mock.patch.object(Path, 'is_dir', return_value=True), \
                mock.patch.object(Path, 'read_bytes', return_value=plist), \
                mock.patch.object(Path, 'open', return_value=io.BytesIO(header)):
            result = native_ios.check_bundle(Path('Game.app'), settings, 'Game')
        self.assertFalse(result['signatureVerified'])
        self.assertFalse(result['appStoreReady'])

    def test_wrong_cpu_or_filetype_is_rejected(self):
        settings = self.settings()
        plist = native_ios.info_plist(settings, 'Game', 'Game').encode()
        for cpu, filetype in ((0x01000007, 2), (0x0100000C, 6)):
            header = struct.pack('<8I', 0xFEEDFACF, cpu, 0, filetype, 0, 0, 0, 0)
            with mock.patch.object(Path, 'is_dir', return_value=True), \
                    mock.patch.object(Path, 'read_bytes', return_value=plist), \
                    mock.patch.object(Path, 'open', return_value=io.BytesIO(header)), \
                    self.assertRaises(NATIVE.portable.ExportError):
                native_ios.check_bundle(Path('Game.app'), settings, 'Game')

    def test_ios_packages_select_arm64_static_libraries_and_reject_shared_files(self):
        settings = self.settings()
        for field, name, valid in (('libraries', 'ios/libgame.a', True),
                                   ('libraries', 'ios/libgame.so', False),
                                   ('runtimeFiles', 'ios/libgame.dylib', False)):
            manifest = {'nativeVariants': {'ios-arm64': {field: [name], 'licenseFiles': ['LICENSE.txt']}}}
            with self.subTest(field=field, name=name), \
                    mock.patch.object(native_ios, 'ios_settings', return_value=settings):
                report, context = fixture.NativeExportToolTests().inspect_fixture_with_package_manifest(
                    'ios', json.dumps(manifest), [name, 'LICENSE.txt'])
            self.assertEqual(report['canGenerateBuildProject'], valid, report['findings'])
            if valid:
                self.assertIn('ios-arm64', context['package_dependencies'])


if __name__ == '__main__':
    # Avoid collecting the imported fixture helper's tests a second time.
    unittest.main(defaultTest='NativeIosToolTests')
