"""書き出したLamaPonゲームの保護を、作者自身が攻撃者視点で自己診断するツール。

対象は自分でビルドした配布物に限ります。tpakの形式検査、索引・エントリの
改ざん検知、配布バイナリからの鍵抽出可否を調べ、どの防御が効いていて
どこに残リスクがあるかを一覧にします。AES-256-CBCとHMAC-SHA256を
標準ライブラリだけで再現するため、追加パッケージなしで実機でも動きます。

使い方:
    py -3 tools/attack_sim.py --dist path/to/exported/game
    py -3 tools/attack_sim.py --tpak assets.tpak --runtime LamaPonRuntime.dll
    py -3 tools/attack_sim.py --dist path/to/game --json
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# ARCHIVE_MAGIC は認証付きtpakの識別子(Crypto/AssetArchiveと一致)。
ARCHIVE_MAGIC = b"TRDNPAK2"
# LEGACY_ARCHIVE_MAGICS は認証なし旧形式として警告する識別子の候補。
LEGACY_ARCHIVE_MAGICS = (b"TRDNPAK1", b"TRDNPAK\x00", b"TRIDPAK1")
# SEAL_MAGIC はシェーダーキャッシュなど単一ファイル暗号の識別子。
SEAL_MAGIC = b"TRDNSEAL"
# MAC_KEY_LABEL は認証鍵派生の固定ラベル(Crypto::DeriveMacKeyと一致)。
MAC_KEY_LABEL = b"Trident.Archive.Mac.v1"
# TPAK_HEADER_SIZE は識別子・索引長・IV・MACの合計バイト数。
TPAK_HEADER_SIZE = len(ARCHIVE_MAGIC) + 8 + 16 + 32

# KEY_SLOT_MARKER_SIZE は鍵スロット先頭の目印バイト数。
KEY_SLOT_MARKER_SIZE = 16
# AES_KEY_SIZE はAES-256鍵のバイト数。
AES_KEY_SIZE = 32
# KEY_SLOT_SIZE は1スロットの全長(目印16とデータ32)。
KEY_SLOT_SIZE = KEY_SLOT_MARKER_SIZE + AES_KEY_SIZE
# KEY_SLOT_COUNT は鍵を分割して埋め込むスロット数(Crypto.hと一致)。
KEY_SLOT_COUNT = 3
# COMBINED_SLOT_SIZE は全スロットが隣接した場合の合計長。
COMBINED_SLOT_SIZE = KEY_SLOT_SIZE * KEY_SLOT_COUNT

# DEFAULT_KEY_SLOT_MARKERS は未書き出しRuntimeに残る各スロットの既定目印(Crypto.cppの初期値)。
# 配布物では書き出し時に乱数へ置換されるため、残っていれば鍵未埋込を疑う。
DEFAULT_KEY_SLOT_MARKERS = (
    bytes(
        (
            0x0B, 0xB9, 0xA7, 0x11, 0xA6, 0x0F, 0x98, 0x97,
            0x2D, 0x5A, 0xB5, 0x8B, 0xA2, 0xEE, 0xCB, 0xEC,
        )
    ),
    bytes(
        (
            0x73, 0x4F, 0x0A, 0xAB, 0x3B, 0x42, 0x21, 0xAE,
            0xCA, 0x1C, 0xAD, 0xA0, 0xCB, 0x87, 0xBE, 0x5F,
        )
    ),
    bytes(
        (
            0x87, 0xE9, 0xA9, 0x4D, 0xA9, 0xDB, 0xDE, 0x47,
            0x01, 0x7D, 0xC3, 0xFE, 0x17, 0x35, 0x85, 0xDF,
        )
    ),
)


# AES S-boxのうち復号・暗号で参照する置換表。
_SBOX = bytes.fromhex(
    "637c777bf26b6fc53001672bfed7ab76"
    "ca82c97dfa5947f0add4a2af9ca472c0"
    "b7fd9326363ff7cc34a5e5f171d83115"
    "04c723c31896059a071280e2eb27b275"
    "09832c1a1b6e5aa0523bd6b329e32f84"
    "53d100ed20fcb15b6acbbe394a4c58cf"
    "d0efaafb434d338545f9027f503c9fa8"
    "51a3408f929d38f5bcb6da2110fff3d2"
    "cd0c13ec5f974417c4a77e3d645d1973"
    "60814fdc222a908846eeb814de5e0bdb"
    "e0323a0a4906245cc2d3ac629195e479"
    "e7c8376d8dd54ea96c56f4ea657aae08"
    "ba78252e1ca6b4c6e8dd741f4bbd8b8a"
    "703eb5664803f60e613557b986c11d9e"
    "e1f8981169d98e949b1e87e9ce5528df"
    "8ca1890dbfe6426841992d0fb054bb16"
)
# _INV_SBOX は復号で使う逆置換表。
_INV_SBOX = bytes(_SBOX.index(value) for value in range(256))
# _RCON はAES鍵拡張のラウンド定数。
_RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36, 0x6C, 0xD8, 0xAB, 0x4D)


# _xtime(value: GF(2^8)の1バイト): 2倍算を既約多項式で畳み込む。
def _xtime(value: int) -> int:
    value <<= 1
    if value & 0x100:
        value ^= 0x11B
    return value & 0xFF


# _mul(a: 乗数, b: 乗数): GF(2^8)上の乗算を行う。
def _mul(a: int, b: int) -> int:
    # result は積の累積。
    result = 0
    for _ in range(8):
        if b & 1:
            result ^= a
        a = _xtime(a)
        b >>= 1
    return result & 0xFF


# Aes256 はAES-256-CBC(PKCS#7)の暗号化・復号を標準ライブラリだけで行う。
# 追加依存を避けて実機でも動かすための最小実装で、鍵確定後の数回の復号を想定する。
class Aes256:
    # __init__(self: 本体, key: 32バイトのAES鍵): ラウンド鍵を展開する。
    def __init__(self, key: bytes):
        if len(key) != AES_KEY_SIZE:
            raise ValueError("AES-256 key must be 32 bytes.")
        # _round_keys は16バイトごとのラウンド鍵一覧。
        self._round_keys = self._expand_key(key)

    # _expand_key(self: 本体, key: AES鍵): 15ラウンド分の鍵語を生成する。
    @staticmethod
    def _expand_key(key: bytes) -> list[list[int]]:
        # words は4バイト語の一覧。
        words = [list(key[index * 4:index * 4 + 4]) for index in range(8)]
        # rcon_index は参照するラウンド定数の位置。
        rcon_index = 0
        for index in range(8, 4 * 15):
            # temp は直前の語。
            temp = list(words[index - 1])
            if index % 8 == 0:
                temp = temp[1:] + temp[:1]
                temp = [_SBOX[value] for value in temp]
                temp[0] ^= _RCON[rcon_index]
                rcon_index += 1
            elif index % 8 == 4:
                temp = [_SBOX[value] for value in temp]
            words.append([words[index - 8][byte] ^ temp[byte] for byte in range(4)])
        # round_keys はラウンド鍵(16バイト)の一覧。
        round_keys = []
        for index in range(15):
            # block は1ラウンド分の16バイト。
            block = []
            for word in words[index * 4:index * 4 + 4]:
                block.extend(word)
            round_keys.append(block)
        return round_keys

    # _add_round_key(state: 状態16バイト, round_key: ラウンド鍵): 排他的論理和を取る。
    @staticmethod
    def _add_round_key(state: list[int], round_key: list[int]) -> None:
        for index in range(16):
            state[index] ^= round_key[index]

    # _inv_sub_bytes(state: 状態16バイト): 逆S-box置換を適用する。
    @staticmethod
    def _inv_sub_bytes(state: list[int]) -> None:
        for index in range(16):
            state[index] = _INV_SBOX[state[index]]

    # _inv_shift_rows(state: 状態16バイト): 行を逆方向に巡回させる。
    @staticmethod
    def _inv_shift_rows(state: list[int]) -> None:
        # columns は列優先の状態を行ごとに並べ替える作業配列。
        columns = state[:]
        for row in range(1, 4):
            for col in range(4):
                state[col * 4 + row] = columns[((col - row) % 4) * 4 + row]

    # _inv_mix_columns(state: 状態16バイト): 列ごとに逆混合変換を適用する。
    @staticmethod
    def _inv_mix_columns(state: list[int]) -> None:
        for col in range(4):
            # base は対象列の先頭位置。
            base = col * 4
            a0, a1, a2, a3 = state[base:base + 4]
            state[base] = _mul(a0, 14) ^ _mul(a1, 11) ^ _mul(a2, 13) ^ _mul(a3, 9)
            state[base + 1] = _mul(a0, 9) ^ _mul(a1, 14) ^ _mul(a2, 11) ^ _mul(a3, 13)
            state[base + 2] = _mul(a0, 13) ^ _mul(a1, 9) ^ _mul(a2, 14) ^ _mul(a3, 11)
            state[base + 3] = _mul(a0, 11) ^ _mul(a1, 13) ^ _mul(a2, 9) ^ _mul(a3, 14)

    # _sub_bytes(state: 状態16バイト): S-box置換を適用する。
    @staticmethod
    def _sub_bytes(state: list[int]) -> None:
        for index in range(16):
            state[index] = _SBOX[state[index]]

    # _shift_rows(state: 状態16バイト): 行を前方向に巡回させる。
    @staticmethod
    def _shift_rows(state: list[int]) -> None:
        # columns は巡回前の状態を保持する作業配列。
        columns = state[:]
        for row in range(1, 4):
            for col in range(4):
                state[col * 4 + row] = columns[((col + row) % 4) * 4 + row]

    # _mix_columns(state: 状態16バイト): 列ごとに混合変換を適用する。
    @staticmethod
    def _mix_columns(state: list[int]) -> None:
        for col in range(4):
            # base は対象列の先頭位置。
            base = col * 4
            a0, a1, a2, a3 = state[base:base + 4]
            state[base] = _mul(a0, 2) ^ _mul(a1, 3) ^ a2 ^ a3
            state[base + 1] = a0 ^ _mul(a1, 2) ^ _mul(a2, 3) ^ a3
            state[base + 2] = a0 ^ a1 ^ _mul(a2, 2) ^ _mul(a3, 3)
            state[base + 3] = _mul(a0, 3) ^ a1 ^ a2 ^ _mul(a3, 2)

    # _encrypt_block(self: 本体, block: 平文16バイト): 1ブロックを暗号化する。
    def _encrypt_block(self, block: bytes) -> bytes:
        # state は変換中の16バイト状態。
        state = list(block)
        self._add_round_key(state, self._round_keys[0])
        for index in range(1, 14):
            self._sub_bytes(state)
            self._shift_rows(state)
            self._mix_columns(state)
            self._add_round_key(state, self._round_keys[index])
        self._sub_bytes(state)
        self._shift_rows(state)
        self._add_round_key(state, self._round_keys[14])
        return bytes(state)

    # _decrypt_block(self: 本体, block: 暗号文16バイト): 1ブロックを復号する。
    def _decrypt_block(self, block: bytes) -> bytes:
        # state は変換中の16バイト状態。
        state = list(block)
        self._add_round_key(state, self._round_keys[14])
        for index in range(13, 0, -1):
            self._inv_shift_rows(state)
            self._inv_sub_bytes(state)
            self._add_round_key(state, self._round_keys[index])
            self._inv_mix_columns(state)
        self._inv_shift_rows(state)
        self._inv_sub_bytes(state)
        self._add_round_key(state, self._round_keys[0])
        return bytes(state)

    # encrypt_cbc(self: 本体, plaintext: 平文, iv: 初期化ベクトル): CBCで暗号化しPKCS#7を付す。
    def encrypt_cbc(self, plaintext: bytes, iv: bytes) -> bytes:
        # padding はPKCS#7で補うバイト数。
        padding = 16 - (len(plaintext) % 16)
        plaintext = plaintext + bytes([padding]) * padding
        # previous は直前の暗号文ブロック(初回はIV)。
        previous = iv
        # output は連結した暗号文。
        output = bytearray()
        for offset in range(0, len(plaintext), 16):
            # block はXOR後の平文ブロック。
            block = bytes(a ^ b for a, b in zip(plaintext[offset:offset + 16], previous))
            previous = self._encrypt_block(block)
            output.extend(previous)
        return bytes(output)

    # decrypt_cbc(self: 本体, ciphertext: 暗号文, iv: 初期化ベクトル): CBCで復号しPKCS#7を外す。
    def decrypt_cbc(self, ciphertext: bytes, iv: bytes) -> bytes:
        if len(ciphertext) == 0 or len(ciphertext) % 16 != 0:
            raise ValueError("Ciphertext length must be a positive multiple of 16.")
        # previous は直前の暗号文ブロック(初回はIV)。
        previous = iv
        # output は連結した平文。
        output = bytearray()
        for offset in range(0, len(ciphertext), 16):
            # block は現在の暗号文ブロック。
            block = ciphertext[offset:offset + 16]
            decrypted = self._decrypt_block(block)
            output.extend(a ^ b for a, b in zip(decrypted, previous))
            previous = block
        # padding は末尾のPKCS#7バイト数。
        padding = output[-1]
        if padding < 1 or padding > 16 or any(value != padding for value in output[-padding:]):
            raise ValueError("Invalid PKCS#7 padding.")
        return bytes(output[:-padding])


# derive_mac_key(key: アーカイブ鍵): 固定ラベルのHMACから認証鍵を派生する。
def derive_mac_key(key: bytes) -> bytes:
    return hmac.new(key, MAC_KEY_LABEL, hashlib.sha256).digest()


# mac_for_ciphertext(mac_key: 認証鍵, iv: 初期化ベクトル, ciphertext: 暗号文): IVと暗号文のHMACを返す。
def mac_for_ciphertext(mac_key: bytes, iv: bytes, ciphertext: bytes) -> bytes:
    return hmac.new(mac_key, iv + ciphertext, hashlib.sha256).digest()


# recover_key_from_window(window: 連続スロット候補): 各スロットのデータ部をXORして鍵を復元する。
def recover_key_from_window(window: bytes) -> bytes:
    if len(window) != COMBINED_SLOT_SIZE:
        raise ValueError("Combined key slot window has the wrong size.")
    # key は合成中の鍵バイト列。
    key = bytearray(AES_KEY_SIZE)
    # 連続スロットのデータ部をXORして鍵を再構成する。
    for slot in range(KEY_SLOT_COUNT):
        # base はこのスロットのデータ先頭。
        base = slot * KEY_SLOT_SIZE + KEY_SLOT_MARKER_SIZE
        for index in range(AES_KEY_SIZE):
            key[index] ^= window[base + index]
    return bytes(key)


# TpakHeader はtpak先頭から読み取った索引領域の情報。
@dataclass
class TpakHeader:
    # magic は形式識別子。
    magic: bytes
    # index_iv は索引の初期化ベクトル。
    index_iv: bytes
    # index_mac は索引の認証タグ。
    index_mac: bytes
    # index_cipher は暗号化された索引。
    index_cipher: bytes
    # payload は各エントリー暗号文の連結領域。
    payload: bytes


# parse_tpak(data: tpak全体のバイト列): ヘッダーを読み取り、壊れていればValueErrorを送出する。
def parse_tpak(data: bytes) -> TpakHeader:
    if len(data) < TPAK_HEADER_SIZE:
        raise ValueError("Truncated tpak header.")
    # magic は先頭の識別子。
    magic = data[0:8]
    # index_size は索引暗号文のバイト数。
    (index_size,) = struct.unpack_from("<Q", data, 8)
    # index_iv は索引IV。
    index_iv = data[16:32]
    # index_mac は索引の認証タグ。
    index_mac = data[32:64]
    if index_size > len(data) - TPAK_HEADER_SIZE:
        raise ValueError("Declared index size exceeds file length.")
    # index_cipher は暗号化索引。
    index_cipher = data[64:64 + index_size]
    # payload は索引の後ろに続くエントリー暗号文。
    payload = data[64 + index_size:]
    return TpakHeader(magic, index_iv, index_mac, index_cipher, payload)


# index_mac_valid(key: アーカイブ鍵, header: tpakヘッダー): 索引MACが鍵と一致するか返す。
def index_mac_valid(key: bytes, header: TpakHeader) -> bool:
    # expected は鍵から計算した索引MAC。
    expected = mac_for_ciphertext(derive_mac_key(key), header.index_iv, header.index_cipher)
    return hmac.compare_digest(expected, header.index_mac)


# decrypt_index(key: アーカイブ鍵, header: tpakヘッダー): 認証後に索引JSONを復号して返す。
def decrypt_index(key: bytes, header: TpakHeader) -> dict:
    if not index_mac_valid(key, header):
        raise ValueError("Index MAC does not match the key.")
    # plain は復号した索引バイト列。
    plain = Aes256(key).decrypt_cbc(header.index_cipher, header.index_iv)
    return json.loads(plain.decode("utf-8"))


# looks_like_plaintext_json(blob: 検査するバイト列): 索引が平文JSONとして読めてしまうかを判定する。
def looks_like_plaintext_json(blob: bytes) -> bool:
    # sample は先頭の非空白バイト。
    sample = blob.lstrip()[:1]
    if sample not in (b"{", b"["):
        return False
    try:
        json.loads(blob.decode("utf-8"))
        return True
    except (ValueError, UnicodeDecodeError):
        return False


# Finding は診断1項目の結果。
@dataclass
class Finding:
    # ident は項目の識別子。
    ident: str
    # title は項目名。
    title: str
    # status はPASS(防御成立)/FAIL(抜けた)/INFO/SKIP。
    status: str
    # detail は補足説明。
    detail: str


# Report は診断全体の結果。
@dataclass
class Report:
    # findings は各診断項目。
    findings: list[Finding] = field(default_factory=list)

    # add(self: 本体, ident: 識別子, title: 項目名, status: 判定, detail: 補足): 1項目を追加する。
    def add(self, ident: str, title: str, status: str, detail: str) -> None:
        self.findings.append(Finding(ident, title, status, detail))

    # failed(self: 本体): FAIL判定があるかを返す。
    def failed(self) -> bool:
        return any(finding.status == "FAIL" for finding in self.findings)


# inspect_tpak(report: 集計先, data: tpakバイト列): 鍵なしで分かる形式と平文露出を検査する。
def inspect_tpak(report: Report, data: bytes) -> Optional[TpakHeader]:
    if data[0:8] in LEGACY_ARCHIVE_MAGICS:
        report.add(
            "tpak-legacy-format",
            "tpakが認証なし旧形式",
            "FAIL",
            "旧形式は現行ランタイムが拒否する。現行ランタイム向けに再エクスポートが必要。",
        )
        return None
    try:
        header = parse_tpak(data)
    except ValueError as error:
        report.add("tpak-parse", "tpakヘッダー解析", "INFO", f"tpakとして解析できない: {error}")
        return None
    if header.magic != ARCHIVE_MAGIC:
        report.add(
            "tpak-magic",
            "tpak識別子",
            "INFO",
            f"想定外の識別子 {header.magic!r}。tpakではない可能性。",
        )
        return None
    report.add("tpak-magic", "tpak識別子", "PASS", "認証付き形式TRDNPAK2。")
    if looks_like_plaintext_json(header.index_cipher):
        report.add(
            "tpak-index-plaintext",
            "索引の平文露出",
            "FAIL",
            "索引が平文JSONとして読める。何が入っているか鍵なしで分かってしまう。",
        )
    else:
        report.add(
            "tpak-index-plaintext",
            "索引の平文露出",
            "PASS",
            "索引は暗号化されており、鍵なしでは中身の一覧を得られない。",
        )
    return header


# scan_default_marker(report: 集計先, dll: Runtimeバイト列): 既定の鍵スロット目印の残存を調べる。
def scan_default_marker(report: Report, dll: bytes) -> None:
    # remaining は残存した既定目印の番号一覧。
    remaining = [
        index
        for index, marker in enumerate(DEFAULT_KEY_SLOT_MARKERS)
        if marker in dll
    ]
    if remaining:
        report.add(
            "runtime-default-marker",
            "既定の鍵スロット目印",
            "FAIL",
            f"未書き出しの既定目印が残っている(スロット{remaining})。"
            "鍵が埋め込まれていない可能性が高い。配布前に確認。",
        )
    else:
        report.add(
            "runtime-default-marker",
            "既定の鍵スロット目印",
            "PASS",
            "全スロットの既定目印は残っていない。書き出し時に乱数へ置換済み。",
        )


# bruteforce_key(report: 集計先, dll: Runtimeバイト列, header: tpakヘッダー): 連続窓から鍵を静的抽出できるか試す。
# 鍵は複数スロットのデータXORへ分割済み。スロットが隣接していれば1窓で再構成でき、
# 別セクションへ離れていれば再構成できない。索引MACで正解を判定する。
def bruteforce_key(report: Report, dll: bytes, header: TpakHeader) -> Optional[bytes]:
    # start は所要時間計測の開始時刻。
    start = time.monotonic()
    # tries は検証した窓の数。
    tries = 0
    for offset in range(0, len(dll) - COMBINED_SLOT_SIZE + 1):
        tries += 1
        # candidate は窓から復元した鍵候補。
        candidate = recover_key_from_window(
            dll[offset:offset + COMBINED_SLOT_SIZE])
        if index_mac_valid(candidate, header):
            # elapsed は鍵特定までの秒数。
            elapsed = time.monotonic() - start
            report.add(
                "runtime-static-key",
                "配布バイナリからの静的鍵抽出",
                "FAIL",
                (
                    f"位置{offset}の{COMBINED_SLOT_SIZE}バイト窓から鍵を再構成し、索引MACが一致した。"
                    f"分割スロットが隣接しており、デバッガ不要の静的解析で鍵が取り出せる"
                    f"(試行{tries}回/{elapsed:.2f}秒)。"
                ),
            )
            return candidate
    # elapsed は全走査にかかった秒数。
    elapsed = time.monotonic() - start
    report.add(
        "runtime-static-key",
        "配布バイナリからの静的鍵抽出",
        "PASS",
        (
            f"連続する全{tries}窓を試しても索引MACの通る鍵は見つからなかった({elapsed:.2f}秒)。"
            "分割スロットが離れており、単一の連続領域からは鍵を再構成できない。"
        ),
    )
    return None


# attempt_tamper_detection(report: 集計先, key: 鍵, header: tpakヘッダー): 索引を1バイト改変して認証が落ちるか確認する。
def attempt_tamper_detection(report: Report, key: bytes, header: TpakHeader) -> None:
    if len(header.index_cipher) == 0:
        report.add("tpak-tamper", "改ざん検知", "SKIP", "索引が空のため検査を省略。")
        return
    # tampered は末尾1バイトを反転した索引暗号文。
    tampered = bytearray(header.index_cipher)
    tampered[-1] ^= 0x01
    # forged は改変後のヘッダー。
    forged = TpakHeader(header.magic, header.index_iv, header.index_mac, bytes(tampered), header.payload)
    if index_mac_valid(key, forged):
        report.add(
            "tpak-tamper",
            "改ざん検知",
            "FAIL",
            "索引を改変してもMACが通った。改ざんが検知されない。",
        )
    else:
        report.add(
            "tpak-tamper",
            "改ざん検知",
            "PASS",
            "索引を1バイト改変するとMACが一致せず、読み込みが失敗する。",
        )


# dump_asset_names(report: 集計先, key: 鍵, header: tpakヘッダー): 鍵が得られた場合に索引からアセット名を取り出せるか示す。
def dump_asset_names(report: Report, key: bytes, header: TpakHeader) -> None:
    try:
        # index は復号した索引JSON。
        index = decrypt_index(key, header)
        # entries は登録アセットの一覧。
        entries = index.get("entries", [])
        # names は先頭数件のアセットパス。
        names = [entry.get("path", "?") for entry in entries[:5]]
        report.add(
            "tpak-asset-dump",
            "鍵取得後のアセット取り出し",
            "FAIL",
            f"鍵が得られたため全{len(entries)}件のアセットを復号・列挙できる。例: {names}",
        )
    except ValueError as error:
        report.add("tpak-asset-dump", "鍵取得後のアセット取り出し", "INFO", f"索引を復号できない: {error}")


# find_dist_files(dist: 配布フォルダ): tpakとRuntime DLLを探す。
def find_dist_files(dist: Path) -> tuple[Optional[Path], Optional[Path]]:
    # tpak は見つかったアセットアーカイブ。
    tpak = next(iter(sorted(dist.glob("*.tpak"))), None)
    # runtime は見つかったRuntime DLL。
    runtime = next(iter(sorted(dist.glob("LamaPonRuntime*.dll"))), None)
    return tpak, runtime


# scan_integrity_manifest(report: 集計先, runtime_path: RuntimeのDLLパス): 改ざん検知マニフェストの有無を調べる。
def scan_integrity_manifest(report: Report, runtime_path: Path) -> None:
    # manifest は配布物に置く整合性マニフェスト。
    manifest = runtime_path.parent / "integrity.dat"
    if not manifest.is_file():
        report.add(
            "runtime-integrity-manifest",
            "ランタイム/モジュールの改ざん検知",
            "FAIL",
            "integrity.datが無い。ランタイムやGame Moduleを差し替えても検知されない。",
        )
        return
    # head はマニフェスト先頭の識別子。
    head = manifest.read_bytes()[: len(SEAL_MAGIC)]
    if head != SEAL_MAGIC:
        report.add(
            "runtime-integrity-manifest",
            "ランタイム/モジュールの改ざん検知",
            "FAIL",
            "integrity.datが封印形式でない。改ざん検知が機能しない可能性。",
        )
        return
    report.add(
        "runtime-integrity-manifest",
        "ランタイム/モジュールの改ざん検知",
        "PASS",
        "封印済みintegrity.datがある。起動時にランタイムとGame Moduleのハッシュを照合する。",
    )


# run_diagnostics(tpak_path: アーカイブのパス, runtime_path: RuntimeのDLLパス): 一連の攻撃を実行して結果を返す。
def run_diagnostics(tpak_path: Optional[Path], runtime_path: Optional[Path]) -> Report:
    # report は集計結果。
    report = Report()
    # header は解析済みtpakヘッダー。
    header = None
    if tpak_path is not None:
        # data はtpak全体のバイト列。
        data = tpak_path.read_bytes()
        header = inspect_tpak(report, data)
    if runtime_path is not None:
        # dll はRuntimeのバイト列。
        dll = runtime_path.read_bytes()
        scan_default_marker(report, dll)
        scan_integrity_manifest(report, runtime_path)
        if header is not None:
            # recovered は静的抽出で得られた鍵(なければNone)。
            recovered = bruteforce_key(report, dll, header)
            if recovered is not None:
                attempt_tamper_detection(report, recovered, header)
                dump_asset_names(report, recovered, header)
    return report


# render_text(report: 診断結果): 人間可読のサマリを文字列にする。
def render_text(report: Report) -> str:
    # lines は出力行の一覧。
    lines = ["書き出しゲームのDRM自己診断", "=" * 40]
    for finding in report.findings:
        lines.append(f"[{finding.status:4}] {finding.title}")
        lines.append(f"       {finding.detail}")
    # fails はFAIL項目数。
    fails = sum(1 for finding in report.findings if finding.status == "FAIL")
    lines.append("=" * 40)
    lines.append(f"FAIL(残リスク): {fails} 件 / 全 {len(report.findings)} 項目")
    return "\n".join(lines)


# render_json(report: 診断結果): マシン可読のJSON文字列にする。
def render_json(report: Report) -> str:
    return json.dumps(
        {
            "findings": [
                {"id": f.ident, "title": f.title, "status": f.status, "detail": f.detail}
                for f in report.findings
            ],
        },
        ensure_ascii=False,
        indent=2,
    )


# main(argv: コマンドライン引数): 入力を解釈して診断を実行する。
def main(argv: Optional[list[str]] = None) -> int:
    # parser は引数の定義。
    parser = argparse.ArgumentParser(description="書き出したLamaPonゲームの保護を自己診断する。")
    parser.add_argument("--dist", type=Path, help="配布フォルダ(tpakとRuntime DLLを自動検出)。")
    parser.add_argument("--tpak", type=Path, help="アセットアーカイブのパス。")
    parser.add_argument("--runtime", type=Path, help="Runtime DLLのパス。")
    parser.add_argument("--json", action="store_true", help="結果をJSONで出力する。")
    # args は解釈済み引数。
    args = parser.parse_args(argv)

    # tpak_path は対象tpak。
    tpak_path = args.tpak
    # runtime_path は対象Runtime DLL。
    runtime_path = args.runtime
    if args.dist is not None:
        tpak_path, runtime_path = find_dist_files(args.dist)
    if tpak_path is None and runtime_path is None:
        parser.error("--dist か --tpak/--runtime のいずれかを指定してください。")

    # report は診断結果。
    report = run_diagnostics(tpak_path, runtime_path)
    print(render_json(report) if args.json else render_text(report))
    return 1 if report.failed() else 0


if __name__ == "__main__":
    sys.exit(main())
