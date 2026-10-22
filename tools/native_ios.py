"""Experimental iOS settings and unsigned bundle validation; never signs or uploads."""
from __future__ import annotations

import plistlib
import re
import struct
from pathlib import Path

from export_web import ExportError


def ios_settings(context: dict) -> dict:
    raw = context['project'].get('export', {}).get('native', {}).get('ios', {})
    if not isinstance(raw, dict):
        raise ExportError('export.native.ios must be an object')
    allowed = {'bundleIdentifier', 'version', 'buildNumber', 'minimumVersion', 'orientation'}
    if set(raw) - allowed:
        raise ExportError('Unknown export.native.ios settings: ' + ', '.join(sorted(set(raw) - allowed)))
    identifier = raw.get('bundleIdentifier')
    if not isinstance(identifier, str) or len(identifier) > 255 or not re.fullmatch(
            r'[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+', identifier):
        raise ExportError('iOS requires an explicit reverse-domain bundleIdentifier')
    version = raw.get('version', '1.0.0')
    build = raw.get('buildNumber', '1')
    minimum = raw.get('minimumVersion', '15.0')
    orientation = raw.get('orientation', 'landscape')
    if not isinstance(version, str) or not re.fullmatch(r'\d{1,3}\.\d{1,3}\.\d{1,3}', version):
        raise ExportError('iOS version must be major.minor.patch, with at most three digits per component')
    if not isinstance(build, str) or not re.fullmatch(r'[1-9]\d{0,8}', build):
        raise ExportError('iOS buildNumber must be a positive numeric string')
    if not isinstance(minimum, str) or not re.fullmatch(r'\d{1,2}\.\d{1,2}', minimum) or int(minimum.split('.')[0]) < 15:
        raise ExportError('iOS minimumVersion must be major.minor and at least 15.0')
    if orientation not in ('landscape', 'portrait', 'all'):
        raise ExportError('iOS orientation must be landscape, portrait or all')
    return dict(bundleIdentifier=identifier, version=version, buildNumber=build,
                minimumVersion=minimum, orientation=orientation)


def info_plist(settings: dict, target: str, name: str) -> str:
    orientations = {
        'landscape': ['UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
        'portrait': ['UIInterfaceOrientationPortrait'],
        'all': ['UIInterfaceOrientationPortrait', 'UIInterfaceOrientationLandscapeLeft',
                'UIInterfaceOrientationLandscapeRight'],
    }[settings['orientation']]
    return plistlib.dumps({
        'CFBundleIdentifier': settings['bundleIdentifier'], 'CFBundleExecutable': target,
        'CFBundleName': name, 'CFBundleDisplayName': name, 'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': settings['version'], 'CFBundleVersion': settings['buildNumber'],
        'MinimumOSVersion': settings['minimumVersion'], 'LSRequiresIPhoneOS': True,
        'UIDeviceFamily': [1], 'UILaunchScreen': {},
        'UISupportedInterfaceOrientations': orientations,
        'UIRequiredDeviceCapabilities': ['arm64', 'opengles-3'],
    }, sort_keys=True).decode('utf-8')


def check_bundle(bundle: Path, settings: dict, target: str) -> dict:
    """Check bundle identity and arm64 Mach-O header; this is not signature validation."""
    if not bundle.is_dir() or bundle.suffix != '.app':
        raise ExportError('iOS output must be an existing .app bundle')
    info = plistlib.loads((bundle / 'Info.plist').read_bytes())
    for key, expected in (('CFBundleIdentifier', settings['bundleIdentifier']),
                          ('CFBundleExecutable', target), ('CFBundlePackageType', 'APPL'),
                          ('CFBundleShortVersionString', settings['version']),
                          ('CFBundleVersion', settings['buildNumber']),
                          ('MinimumOSVersion', settings['minimumVersion'])):
        if info.get(key) != expected:
            raise ExportError('iOS bundle metadata mismatch: ' + key)
    with (bundle / target).open('rb') as binary:
        header = binary.read(32)
    if len(header) != 32 or struct.unpack_from('<I', header)[0] != 0xFEEDFACF \
            or struct.unpack_from('<I', header, 4)[0] != 0x0100000C \
            or struct.unpack_from('<I', header, 12)[0] != 2:
        raise ExportError('iOS executable must be a thin arm64 Mach-O executable')
    return {'architecture': 'arm64', 'bundleIdentifier': settings['bundleIdentifier'],
            'signatureVerified': False, 'deviceVerified': False, 'appStoreReady': False,
            'limitations': ['Mach-O load commands, SDK platform and linked dependencies are not verified',
                            'Distribution signing, icons, privacy declarations and submission need validation']}
