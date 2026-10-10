"""書き出し保護の自己診断ツール tools/attack_sim.py の回帰テスト。

本物と同一仕様のtpakと鍵スロット埋込DLLをPythonで生成し、既知攻撃の
判定結果を固定化する。保護を強化したら、ここで FAIL が PASS へ変わること
(残リスクが塞がったこと)を確認できる。追加パッケージには依存しない。
"""

from __future__ import annotations

import importlib.util
import json
import os
import struct
import sys
import unittest
from pathlib import Path


# TOOL_PATH は自己診断ツールのパス。
TOOL_PATH = Path(__file__).resolve().parents[1] / "tools" / "attack_sim.py"
# SPEC はツールの読込仕様。
SPEC = importlib.util.spec_from_file_location("attack_sim", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
# SIM は読込済みの自己診断ツール。
SIM = importlib.util.module_from_spec(SPEC)
# dataclass解決のためimport前にモジュールを登録する。
sys.modules["attack_sim"] = SIM
SPEC.loader.exec_module(SIM)


# build_tpak(key: アーカイブ鍵, files: 相対パスと中身の対応): 本物と同一仕様のtpakを組み立てる。
def build_tpak(key: bytes, files: dict[str, bytes]) -> bytes:
    # mac_key は索引とエントリーの認証鍵。
    mac_key = SIM.derive_mac_key(key)
    # aes は暗号化器。
    aes = SIM.Aes256(key)
    # entries は索引へ書くエントリー情報。
    entries = []
    # payload は連結したエントリー暗号文。
    payload = bytearray()
    # offset は次のエントリーの相対位置。
    offset = 0
    for path, data in files.items():
        # iv はこのエントリーのIV。
        iv = os.urandom(16)
        # cipher はエントリーの暗号文。
        cipher = aes.encrypt_cbc(data, iv)
        # mac はIVと暗号文の認証タグ。
        mac = SIM.mac_for_ciphertext(mac_key, iv, cipher)
        entries.append(
            {"path": path, "offset": offset, "size": len(cipher), "iv": list(iv), "mac": list(mac)}
        )
        payload.extend(cipher)
        offset += len(cipher)
    # index_json は暗号化前の索引。
    index_json = json.dumps({"entries": entries}).encode("utf-8")
    # index_iv は索引のIV。
    index_iv = os.urandom(16)
    # index_cipher は暗号化した索引。
    index_cipher = aes.encrypt_cbc(index_json, index_iv)
    # index_mac は索引の認証タグ。
    index_mac = SIM.mac_for_ciphertext(mac_key, index_iv, index_cipher)
    return (
        SIM.ARCHIVE_MAGIC
        + struct.pack("<Q", len(index_cipher))
        + index_iv
        + index_mac
        + bytes(index_cipher)
        + bytes(payload)
    )


# build_runtime_with_key(key: 埋め込む鍵, marker: 鍵スロットの目印, size: DLLの総バイト数): 鍵スロットを1個埋めたダミーDLLを作る。
def build_runtime_with_key(key: bytes, marker: bytes, size: int = 4096) -> bytes:
    # pad はXORパッド。
    pad = os.urandom(SIM.AES_KEY_SIZE)
    # stored は鍵をパッドでXORした領域。
    stored = bytes(a ^ b for a, b in zip(key, pad))
    # slot は80バイトの鍵スロット。
    slot = marker + pad + stored
    # blob は乱数で埋めたDLL本体。
    blob = bytearray(os.urandom(size))
    # position はスロットを置く位置。
    position = 1234
    blob[position:position + len(slot)] = slot
    return bytes(blob)


# status_of(report: 診断結果, ident: 項目の識別子): 指定項目の判定を取り出す。
def status_of(report, ident: str) -> str:
    for finding in report.findings:
        if finding.ident == ident:
            return finding.status
    return "ABSENT"


# 自己診断ツールの攻撃判定を検証する。
class ExportSecurityAttackTests(unittest.TestCase):
    # test_index_is_not_plaintext(self: テストケース): 正常tpakの索引が平文露出しないことを確認する。
    def test_index_is_not_plaintext(self):
        # key はこの配布物の鍵。
        key = os.urandom(32)
        # data はtpakバイト列。
        data = build_tpak(key, {"scenes/main.scene": b"{}", "textures/logo.png": os.urandom(200)})
        # report は形式検査の結果。
        report = SIM.Report()
        SIM.inspect_tpak(report, data)
        self.assertEqual(status_of(report, "tpak-magic"), "PASS")
        self.assertEqual(status_of(report, "tpak-index-plaintext"), "PASS")

    # test_legacy_format_is_flagged(self: テストケース): 認証なし旧形式がFAILになることを確認する。
    def test_legacy_format_is_flagged(self):
        # report は形式検査の結果。
        report = SIM.Report()
        SIM.inspect_tpak(report, SIM.LEGACY_ARCHIVE_MAGICS[0] + os.urandom(128))
        self.assertEqual(status_of(report, "tpak-legacy-format"), "FAIL")

    # test_static_key_extraction_currently_succeeds(self: テストケース): 現状は静的鍵抽出が成立する残リスクを固定化する。
    def test_static_key_extraction_currently_succeeds(self):
        # key はこの配布物の鍵。
        key = os.urandom(32)
        # header は解析済みtpakヘッダー。
        header = SIM.parse_tpak(build_tpak(key, {"a.bin": os.urandom(64)}))
        # dll は乱数目印で鍵スロットを埋めたDLL。
        dll = build_runtime_with_key(key, os.urandom(16))
        # report は鍵抽出攻撃の結果。
        report = SIM.Report()
        # recovered は静的抽出で得た鍵。
        recovered = SIM.bruteforce_key(report, dll, header)
        self.assertEqual(recovered, key)
        # 鍵スロットが連続80バイトで配布バイナリに残る限り、静的抽出は成立する。
        self.assertEqual(status_of(report, "runtime-static-key"), "FAIL")

    # test_wrong_key_does_not_authenticate(self: テストケース): 別配布物の鍵では索引MACが通らないことを確認する。
    def test_wrong_key_does_not_authenticate(self):
        # header は解析済みtpakヘッダー。
        header = SIM.parse_tpak(build_tpak(os.urandom(32), {"a.bin": os.urandom(48)}))
        self.assertFalse(SIM.index_mac_valid(os.urandom(32), header))

    # test_tamper_is_detected(self: テストケース): 鍵があっても索引改変で認証が落ちることを確認する。
    def test_tamper_is_detected(self):
        # key はこの配布物の鍵。
        key = os.urandom(32)
        # header は解析済みtpakヘッダー。
        header = SIM.parse_tpak(build_tpak(key, {"a.bin": os.urandom(48)}))
        # report は改ざん検知の結果。
        report = SIM.Report()
        SIM.attempt_tamper_detection(report, key, header)
        self.assertEqual(status_of(report, "tpak-tamper"), "PASS")

    # test_default_marker_detection(self: テストケース): 既定目印の残存を検出し、乱数目印は見逃さないことを確認する。
    def test_default_marker_detection(self):
        # key はこの配布物の鍵。
        key = os.urandom(32)
        # unpatched は既定目印を残したDLL。
        unpatched = build_runtime_with_key(key, SIM.DEFAULT_KEY_SLOT_MARKER)
        # patched は乱数目印へ置換したDLL。
        patched = build_runtime_with_key(key, os.urandom(16))
        # report_unpatched は未書き出しDLLの検査結果。
        report_unpatched = SIM.Report()
        SIM.scan_default_marker(report_unpatched, unpatched)
        self.assertEqual(status_of(report_unpatched, "runtime-default-marker"), "FAIL")
        # report_patched は書き出し済みDLLの検査結果。
        report_patched = SIM.Report()
        SIM.scan_default_marker(report_patched, patched)
        self.assertEqual(status_of(report_patched, "runtime-default-marker"), "PASS")

    # test_asset_dump_after_key_recovery(self: テストケース): 鍵が得られると全アセットを列挙できることを確認する。
    def test_asset_dump_after_key_recovery(self):
        # key はこの配布物の鍵。
        key = os.urandom(32)
        # header は解析済みtpakヘッダー。
        header = SIM.parse_tpak(build_tpak(key, {"scenes/main.scene": b"{}", "audio/bgm.ogg": os.urandom(80)}))
        # index は復号した索引。
        index = SIM.decrypt_index(key, header)
        # paths は索引に並ぶアセットパス。
        paths = {entry["path"] for entry in index["entries"]}
        self.assertEqual(paths, {"scenes/main.scene", "audio/bgm.ogg"})


if __name__ == "__main__":
    unittest.main()
