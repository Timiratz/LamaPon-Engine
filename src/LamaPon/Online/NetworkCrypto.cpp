#include "LamaPon/Online/NetworkCrypto.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace LamaPon::Detail
{
    namespace
    {
        // CNGの失敗を例外へ変換する(status: CNG終了コード)。
        void Check(const NTSTATUS status)
        {
            if (status < 0) throw std::runtime_error("Windowsの通信暗号処理に失敗しました。");
        }
        struct Algorithm final
        {
            // 暗号プロバイダーの所有先
            BCRYPT_ALG_HANDLE value{};
            // 暗号プロバイダーを解放する。
            ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
        };
        struct Hash final
        {
            // HMAC状態の所有先
            BCRYPT_HASH_HANDLE value{};
            // HMAC状態を解放する。
            ~Hash() { if (value) BCryptDestroyHash(value); }
        };
        struct Key final
        {
            // 暗号鍵の所有先
            BCRYPT_KEY_HANDLE value{};
            // 暗号鍵を解放する。
            ~Key() { if (value) BCryptDestroyKey(value); }
        };
        struct Secret final
        {
            // 共有秘密の所有先
            BCRYPT_SECRET_HANDLE value{};
            // 共有秘密を解放する。
            ~Secret() { if (value) BCryptDestroySecret(value); }
        };
        // CNGの読取引数として借用し、書き換えには使わない(value: 入力バイト列)。
        unsigned char* Bytes(std::span<const unsigned char> value)
        {
            return const_cast<unsigned char*>(value.data());
        }
    }

    NetworkKey RandomNetworkKey()
    {
        // 乱数で生成する32バイト鍵
        NetworkKey key{};
        Check(BCryptGenRandom(nullptr, key.data(), static_cast<ULONG>(key.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
        return key;
    }
    std::string Hex(const std::span<const unsigned char> bytes)
    {
        // 小文字16進数の文字表
        constexpr char alphabet[] = "0123456789abcdef";
        // 16進数の出力
        std::string text;
        text.reserve(bytes.size() * 2);
        // 16進数に変換するバイト
        for (const auto byte : bytes) { text += alphabet[byte >> 4]; text += alphabet[byte & 15]; }
        return text;
    }
    bool Unhex(const std::string_view text, const std::span<unsigned char> bytes)
    {
        if (text.size() != bytes.size() * 2) return false;
        // 16進数1桁を変換(c: 入力文字)。
        const auto digit = [](const char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        // バイトの処理位置
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            // high: 上位桁・low: 下位桁
            const int high = digit(text[index * 2]), low = digit(text[index * 2 + 1]);
            if (high < 0 || low < 0) { SecureZeroMemory(bytes.data(), bytes.size()); return false; }
            bytes[index] = static_cast<unsigned char>(high * 16 + low);
        }
        return true;
    }
    NetworkKey NetworkHmac(const std::span<const unsigned char> key, const std::span<const unsigned char> data)
    {
        // 暗号プロバイダーの所有先
        Algorithm algorithm;
        // HMAC状態の所有先
        Hash hash;
        Check(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));
        Check(BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, Bytes(key), static_cast<ULONG>(key.size()), 0));
        Check(BCryptHashData(hash.value, Bytes(data), static_cast<ULONG>(data.size()), 0));
        // 計算結果の出力先
        NetworkKey result{};
        Check(BCryptFinishHash(hash.value, result.data(), static_cast<ULONG>(result.size()), 0));
        return result;
    }
    NetworkKey NetworkHkdf(const std::span<const unsigned char> salt, const std::span<const unsigned char> secret,
        const std::string_view context)
    {
        // 抽出と1ブロックの展開でAES-256用の32バイトを導出します。
        // HKDFの抽出済み鍵
        auto extracted = NetworkHmac(salt, secret);
        // 用途識別子と展開ブロック番号
        std::vector<unsigned char> info(context.begin(), context.end()); info.push_back(1);
        // 計算結果の出力先
        const auto result = NetworkHmac(extracted, info);
        SecureZeroMemory(extracted.data(), extracted.size());
        return result;
    }
    NetworkKeyExchange::NetworkKeyExchange()
    {
        try
        {
            Check(BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0));
            Check(BCryptGenerateKeyPair(m_algorithm, &m_key, 256, 0));
            Check(BCryptFinalizeKeyPair(m_key, 0));
        }
        catch (...)
        {
            if (m_key) BCryptDestroyKey(m_key);
            if (m_algorithm) BCryptCloseAlgorithmProvider(m_algorithm, 0);
            throw;
        }
    }
    NetworkKeyExchange::~NetworkKeyExchange()
    {
        if (m_key) BCryptDestroyKey(m_key);
        if (m_algorithm) BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }
    std::array<unsigned char, 72> NetworkKeyExchange::PublicKey() const
    {
        // CNG公開鍵Blobの出力先
        std::array<unsigned char, 72> result{};
        // CNGが書き出したバイト数
        ULONG written{};
        Check(BCryptExportKey(m_key, nullptr, BCRYPT_ECCPUBLIC_BLOB, result.data(), static_cast<ULONG>(result.size()), &written, 0));
        if (written != result.size()) throw std::runtime_error("通信公開鍵の形式が無効です。");
        return result;
    }
    NetworkKey NetworkKeyExchange::Agree(const std::span<const unsigned char> remote) const
    {
        if (remote.size() != 72) throw std::runtime_error("通信公開鍵の長さが無効です。");
        // CNG公開鍵Blobの形式
        BCRYPT_ECCKEY_BLOB header{};
        std::copy_n(remote.data(), sizeof(header), reinterpret_cast<unsigned char*>(&header));
        if (header.dwMagic != BCRYPT_ECDH_PUBLIC_P256_MAGIC || header.cbKey != 32)
            throw std::runtime_error("通信公開鍵の形式が無効です。");
        // 相手の公開鍵の所有先
        Key key;
        // ECDH共有秘密の所有先
        Secret secret;
        Check(BCryptImportKeyPair(m_algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.value,
            Bytes(remote), static_cast<ULONG>(remote.size()), 0));
        Check(BCryptSecretAgreement(m_key, key.value, &secret.value, 0));
        // 計算結果の出力先
        NetworkKey result{};
        // CNGが書き出したバイト数
        ULONG written{};
        // Windowsが返すlittle-endianの共有秘密を双方ともそのままHKDFへ渡します。
        Check(BCryptDeriveKey(secret.value, BCRYPT_KDF_RAW_SECRET, nullptr, result.data(), static_cast<ULONG>(result.size()), &written, 0));
        if (written != result.size()) throw std::runtime_error("共有鍵の長さが無効です。");
        return result;
    }
    // AES-GCMで変換し、失敗時は出力を消去して例外にする(key: CNG暗号鍵, decrypt: 復号するか, nonce: 12バイトの一意値, aad: 認証専用データ, input: 1〜1100バイトの入力, tag: 暗号化出力・復号入力16B)。
    static std::string Gcm(const BCRYPT_KEY_HANDLE key, const bool decrypt,
        const std::span<unsigned char> nonce, const std::span<unsigned char> aad,
        const std::string_view input, const std::span<unsigned char> tag)
    {
        if (nonce.size() != 12 || tag.size() != 16 || input.empty() || input.size() > 1100)
            throw std::runtime_error("暗号化パケットの長さが無効です。");
        // GCMの一意値と認証情報
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data(); info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = aad.empty() ? nullptr : aad.data(); info.cbAuthData = static_cast<ULONG>(aad.size());
        info.pbTag = tag.data(); info.cbTag = static_cast<ULONG>(tag.size());
        // 暗号化または復号の出力先
        std::string result(input.size(), '\0');
        // CNGが書き出したバイト数
        ULONG written{};
        // 暗号化または復号のCNG関数
        const auto operation = decrypt ? BCryptDecrypt : BCryptEncrypt;
        // CNG変換の終了コード
        const auto status = operation(key, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
            static_cast<ULONG>(input.size()), &info, nullptr, 0,
            reinterpret_cast<PUCHAR>(result.data()), static_cast<ULONG>(result.size()), &written, 0);
        if (status < 0 || written != result.size())
        {
            SecureZeroMemory(result.data(), result.size());
            throw std::runtime_error("通信データの認証に失敗しました。");
        }
        return result;
    }
    std::string NetworkAesGcm(const bool decrypt, const NetworkKey& key,
        const std::span<unsigned char> nonce, const std::span<unsigned char> aad,
        const std::string_view input, const std::span<unsigned char> tag)
    {
        // 暗号プロバイダーの所有先
        Algorithm algorithm;
        // 一時的なAES鍵の所有先
        Key cipher;
        Check(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_AES_ALGORITHM, nullptr, 0));
        Check(BCryptSetProperty(algorithm.value, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
        Check(BCryptGenerateSymmetricKey(algorithm.value, &cipher.value, nullptr, 0, Bytes(key), static_cast<ULONG>(key.size()), 0));
        return Gcm(cipher.value, decrypt, nonce, aad, input, tag);
    }
    NetworkCipher::~NetworkCipher()
    {
        if (m_key) BCryptDestroyKey(m_key);
        if (m_algorithm) BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }
    void NetworkCipher::Initialize(const NetworkKey& key)
    {
        if (m_key) BCryptDestroyKey(m_key);
        if (m_algorithm) BCryptCloseAlgorithmProvider(m_algorithm, 0);
        m_key = nullptr; m_algorithm = nullptr; m_initialized = false;
        Check(BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0));
        Check(BCryptSetProperty(m_algorithm, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
        Check(BCryptGenerateSymmetricKey(m_algorithm, &m_key, nullptr, 0, Bytes(key), static_cast<ULONG>(key.size()), 0));
        m_sequence = 0; m_initialized = true;
    }
    std::string NetworkCipher::Seal(const std::string_view clear)
    {
        if (!m_initialized || m_sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("通信鍵の再作成が必要です。");
        // 12バイトのGCM一意値
        std::array<unsigned char, 12> nonce{};
        // 形式バージョンと64ビット連番
        std::array<unsigned char, 9> header{}; header[0] = 2;
        // バイトの処理位置
        for (unsigned int index = 0; index < 8; ++index)
            nonce[index + 4] = header[index + 1] = static_cast<unsigned char>(m_sequence >> (56 - index * 8));
        // 16バイトの認証タグ
        std::array<unsigned char, 16> tag{};
        // 認証済み暗号文
        auto encrypted = Gcm(m_key, false, nonce, header, clear, tag);
        // ヘッダーと暗号文と認証タグ
        std::string packet(reinterpret_cast<const char*>(header.data()), header.size());
        packet += encrypted; packet.append(reinterpret_cast<const char*>(tag.data()), tag.size());
        ++m_sequence;
        return packet;
    }
    std::string NetworkCipher::Open(const std::string_view packet)
    {
        if (!m_initialized || packet.size() <= 25 || packet.size() > 1125 || packet[0] != 2
            || m_sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("暗号化パケットの形式が無効です。");
        // 12バイトのGCM一意値
        std::array<unsigned char, 12> nonce{};
        // 形式バージョンと64ビット連番
        std::array<unsigned char, 9> header{};
        std::copy_n(packet.data(), header.size(), header.data());
        // 受信パケットの連番
        std::uint64_t sequence{};
        // バイトの処理位置
        for (unsigned int index = 0; index < 8; ++index)
        {
            nonce[index + 4] = header[index + 1]; sequence = (sequence << 8) | header[index + 1];
        }
        if (sequence != m_sequence) throw std::runtime_error("通信連番の再送または順序違反です。");
        // 16バイトの認証タグ
        std::array<unsigned char, 16> tag{};
        std::copy_n(packet.end() - 16, tag.size(), tag.data());
        // 認証済みの復号結果
        auto clear = Gcm(m_key, true, nonce, header, packet.substr(9, packet.size() - 25), tag);
        ++m_sequence;
        return clear;
    }
}
