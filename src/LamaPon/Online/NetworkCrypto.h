#pragma once

#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace LamaPon::Detail
{
    using NetworkKey = std::array<unsigned char, 32>;
    // 暗号学的乱数で32バイトの鍵を生成する。
    NetworkKey RandomNetworkKey();
    // 小文字の16進数へ変換する(bytes: 変換するバイト列)。
    std::string Hex(std::span<const unsigned char> bytes);
    // 小文字16進数を復元する(text: 16進文字列, bytes: 復元先)。
    // 長さ違いでは出力を保持し、不正な文字を検出した場合は出力全体を消去します。
    bool Unhex(std::string_view text, std::span<unsigned char> bytes);
    // HMAC-SHA256を計算する(key: 認証鍵, data: 認証するバイト列)。
    NetworkKey NetworkHmac(std::span<const unsigned char> key, std::span<const unsigned char> data);
    // HKDF-SHA256で32バイトを導出する(salt: 抽出用ソルト, secret: 共有秘密, context: 用途識別文字列)。
    NetworkKey NetworkHkdf(std::span<const unsigned char> salt, std::span<const unsigned char> secret,
        std::string_view context);

    // 秘密鍵は接続ごとに生成し、ファイルやWindowsの鍵ストアには保存しません。
    class NetworkKeyExchange final
    {
    public:
        // 接続ごとにECDH P-256の鍵を作る。
        NetworkKeyExchange();
        // 鍵と暗号プロバイダーを解放する。
        ~NetworkKeyExchange();
        // 秘密鍵の複製を禁止する。
        NetworkKeyExchange(const NetworkKeyExchange&) = delete;
        // 秘密鍵のコピー代入を禁止する。
        NetworkKeyExchange& operator=(const NetworkKeyExchange&) = delete;
        // 72バイトのCNG公開鍵Blobを返す。
        std::array<unsigned char, 72> PublicKey() const;
        // 公開鍵から共有秘密を作る(remote: 相手のCNG公開鍵Blob)。
        // CNGのlittle-endian共有秘密を並べ替えずにHKDFへ渡します。
        NetworkKey Agree(std::span<const unsigned char> remote) const;
    private:
        // ECDHプロバイダー
        BCRYPT_ALG_HANDLE m_algorithm{};
        // 接続専用の秘密鍵
        BCRYPT_KEY_HANDLE m_key{};
    };

    // AES-256-GCMで変換し、認証失敗時は例外を送出する(decrypt: 復号するか, key: 32バイト鍵, nonce: 12バイトの一意値, aad: 認証専用データ, input: 1〜1100バイトの入力, tag: 暗号化出力・復号入力16B)。
    std::string NetworkAesGcm(bool decrypt, const NetworkKey& key,
        std::span<unsigned char> nonce, std::span<unsigned char> aad,
        std::string_view input, std::span<unsigned char> tag);

    // 送信方向ごとに鍵を分け、同一鍵で連番を再利用しません。
    class NetworkCipher final
    {
    public:
        // 未初期化の通信暗号器を作る。
        NetworkCipher() = default;
        // 暗号鍵とプロバイダーを解放する。
        ~NetworkCipher();
        // 鍵と連番の複製を禁止する。
        NetworkCipher(const NetworkCipher&) = delete;
        // 鍵と連番のコピー代入を禁止する。
        NetworkCipher& operator=(const NetworkCipher&) = delete;
        // 新しい鍵で連番を0に初期化する(key: 方向別の32バイト鍵)。
        // 再初期化には必ず別の鍵を使い、同一鍵のnonce再利用を避けます。
        void Initialize(const NetworkKey& key);
        // 連番と認証タグ付きで暗号化する(clear: 1〜1100バイトの平文)。
        std::string Seal(std::string_view clear);
        // 次の連番のパケットを認証して復号する(packet: 暗号化パケット)。
        std::string Open(std::string_view packet);
    private:
        // AESプロバイダー
        BCRYPT_ALG_HANDLE m_algorithm{};
        // 方向別の対称鍵
        BCRYPT_KEY_HANDLE m_key{};
        // 次に送受信する連番
        std::uint64_t m_sequence{};
        // 暗号鍵の初期化済み状態
        bool m_initialized{};
    };
}
