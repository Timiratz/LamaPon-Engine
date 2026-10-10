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


# split_key(key: 分割する鍵): 鍵をKEY_SLOT_COUNT個のデータ片へXOR分割する。
def split_key(key: bytes) -> list[bytes]:
    # parts は各スロットのデータ片。
    parts = [os.urandom(SIM.AES_KEY_SIZE) for _ in range(SIM.KEY_SLOT_COUNT - 1)]
    # last は残りのXORで決まる最終データ片。
    last = bytearray(key)
    for part in parts:
        for index in range(len(last)):
            last[index] ^= part[index]
    parts.append(bytes(last))
    return parts


# build_runtime_with_key(key: 埋め込む鍵, markers: 各スロットの目印, adjacent: 連続配置か, size: DLLの総バイト数): 分割鍵スロットを埋めたダミーDLLを作る。
def build_runtime_with_key(
    key: bytes,
    markers: list[bytes],
    adjacent: bool = False,
    size: int = 4096,
) -> bytes:
    # parts は分割した各スロットのデータ片。
    parts = split_key(key)
    # slots は目印とデータを連結したスロット群。
    slots = [markers[i] + parts[i] for i in range(SIM.KEY_SLOT_COUNT)]
    # blob は乱数で埋めたDLL本体。
    blob = bytearray(os.urandom(size))
    if adjacent:
        # offsets は隣接配置(本物のセクション分離前の最悪形)の位置。
        offsets = [1234 + i * SIM.KEY_SLOT_SIZE for i in range(SIM.KEY_SLOT_COUNT)]
    else:
        # offsets は離して配置した位置(本物のセクション分離を模す)。
        offsets = [200, 1600, 3200]
    for slot, offset in zip(slots, offsets):
        blob[offset:offset + len(slot)] = slot
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

    # random_markers(self: テストケース): スロット数ぶんの乱数目印を作る。
    def random_markers(self):
        return [os.urandom(SIM.KEY_SLOT_MARKER_SIZE) for _ in range(SIM.KEY_SLOT_COUNT)]

    # test_split_key_slot_resists_static_extraction(self: テストケース): 分割スロットを離して置けば静的鍵抽出が成立しないことを固定化する。
    def test_split_key_slot_resists_static_extraction(self):
        # key はこの配布物の鍵。
        key = os.urandom(32)
        # header は解析済みtpakヘッダー。
        header = SIM.parse_tpak(build_tpak(key, {"a.bin": os.urandom(64)}))
        # dll は分割スロットを離して埋めたDLL。
        dll = build_runtime_with_key(key, self.random_markers(), adjacent=False)
        # report は鍵抽出攻撃の結果。
        report = SIM.Report()
        # recovered は静的抽出で得た鍵(分離済みならNone)。
        recovered = SIM.bruteforce_key(report, dll, header)
        self.assertIsNone(recovered)
        # スロットが別領域へ離れていれば単一の連続窓から鍵を再構成できない。
        self.assertEqual(status_of(report, "runtime-static-key"), "PASS")

    # test_adjacent_split_slots_are_still_extractable(self: テストケース): スロットを隣接配置すると走査が鍵を再構成でき、分離不十分を検出できることを固定化する。
    def test_adjacent_split_slots_are_still_extractable(self):
        # key はこの配布物の鍵。
        key = os.urandom(32)
        # header は解析済みtpakヘッダー。
        header = SIM.parse_tpak(build_tpak(key, {"a.bin": os.urandom(64)}))
        # dll は分割スロットを隣接させて埋めたDLL。
        dll = build_runtime_with_key(key, self.random_markers(), adjacent=True)
        # report は鍵抽出攻撃の結果。
        report = SIM.Report()
        # recovered は静的抽出で得た鍵。
        recovered = SIM.bruteforce_key(report, dll, header)
        self.assertEqual(recovered, key)
        # 分割していても隣接していれば1窓で再構成でき、走査はそれを検出する。
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
        # unpatched は全スロットの既定目印を残したDLL。
        unpatched = build_runtime_with_key(key, list(SIM.DEFAULT_KEY_SLOT_MARKERS))
        # patched は乱数目印へ置換したDLL。
        patched = build_runtime_with_key(key, self.random_markers())
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

    # test_integrity_manifest_detection(self: テストケース): 改ざん検知マニフェストの有無・形式を正しく判定することを確認する。
    def test_integrity_manifest_detection(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            # dist は配布物を模した一時フォルダ。
            dist = Path(directory)
            # runtime はダミーのRuntime DLL。
            runtime = dist / "LamaPonRuntime.dll"
            runtime.write_bytes(os.urandom(64))
            # manifest が無ければFAIL。
            report = SIM.Report()
            SIM.scan_integrity_manifest(report, runtime)
            self.assertEqual(status_of(report, "runtime-integrity-manifest"), "FAIL")
            # 封印済みmanifestがあればPASS。
            (dist / "integrity.dat").write_bytes(SIM.SEAL_MAGIC + os.urandom(96))
            report = SIM.Report()
            SIM.scan_integrity_manifest(report, runtime)
            self.assertEqual(status_of(report, "runtime-integrity-manifest"), "PASS")
            # 封印形式でないmanifestはFAIL。
            (dist / "integrity.dat").write_bytes(b"NOTSEAL0" + os.urandom(96))
            report = SIM.Report()
            SIM.scan_integrity_manifest(report, runtime)
            self.assertEqual(status_of(report, "runtime-integrity-manifest"), "FAIL")


if __name__ == "__main__":
    unittest.main()
