#include "LamaPon/Online/NetworkCrypto.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace LamaPon::Detail
{
    namespace
    {
        void Check(const NTSTATUS status)
        {
            if (status < 0) throw std::runtime_error("Windowsの通信暗号処理に失敗しました。");
        }
        struct Algorithm final
        {
            BCRYPT_ALG_HANDLE value{};
            ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
        };
        struct Hash final
        {
            BCRYPT_HASH_HANDLE value{};
            ~Hash() { if (value) BCryptDestroyHash(value); }
        };
        struct Key final
        {
            BCRYPT_KEY_HANDLE value{};
            ~Key() { if (value) BCryptDestroyKey(value); }
        };
        struct Secret final
        {
            BCRYPT_SECRET_HANDLE value{};
            ~Secret() { if (value) BCryptDestroySecret(value); }
        };
        unsigned char* Bytes(std::span<const unsigned char> value)
        {
            return const_cast<unsigned char*>(value.data());
        }
    }

    NetworkKey RandomNetworkKey()
    {
        NetworkKey key{};
        Check(BCryptGenRandom(nullptr, key.data(), static_cast<ULONG>(key.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
        return key;
    }
    std::string Hex(const std::span<const unsigned char> bytes)
    {
        constexpr char alphabet[] = "0123456789abcdef";
        std::string text;
        text.reserve(bytes.size() * 2);
        for (const auto byte : bytes) { text += alphabet[byte >> 4]; text += alphabet[byte & 15]; }
        return text;
    }
    bool Unhex(const std::string_view text, const std::span<unsigned char> bytes)
    {
        if (text.size() != bytes.size() * 2) return false;
        const auto digit = [](const char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            const int high = digit(text[index * 2]), low = digit(text[index * 2 + 1]);
            if (high < 0 || low < 0) { SecureZeroMemory(bytes.data(), bytes.size()); return false; }
            bytes[index] = static_cast<unsigned char>(high * 16 + low);
        }
        return true;
    }
    NetworkKey NetworkHmac(const std::span<const unsigned char> key, const std::span<const unsigned char> data)
    {
        Algorithm algorithm; Hash hash;
        Check(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG));
        Check(BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, Bytes(key), static_cast<ULONG>(key.size()), 0));
        Check(BCryptHashData(hash.value, Bytes(data), static_cast<ULONG>(data.size()), 0));
        NetworkKey result{};
        Check(BCryptFinishHash(hash.value, result.data(), static_cast<ULONG>(result.size()), 0));
        return result;
    }
    NetworkKey NetworkHkdf(const std::span<const unsigned char> salt, const std::span<const unsigned char> secret,
        const std::string_view context)
    {
        // RFC 5869のExtractとExpandを使い、AES-256用の先頭32バイトを取得します。
        auto extracted = NetworkHmac(salt, secret);
        std::vector<unsigned char> info(context.begin(), context.end()); info.push_back(1);
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
        std::array<unsigned char, 72> result{}; ULONG written{};
        Check(BCryptExportKey(m_key, nullptr, BCRYPT_ECCPUBLIC_BLOB, result.data(), static_cast<ULONG>(result.size()), &written, 0));
        if (written != result.size()) throw std::runtime_error("通信公開鍵の形式が無効です。");
        return result;
    }
    NetworkKey NetworkKeyExchange::Agree(const std::span<const unsigned char> remote) const
    {
        if (remote.size() != 72) throw std::runtime_error("通信公開鍵の長さが無効です。");
        BCRYPT_ECCKEY_BLOB header{};
        std::copy_n(remote.data(), sizeof(header), reinterpret_cast<unsigned char*>(&header));
        if (header.dwMagic != BCRYPT_ECDH_PUBLIC_P256_MAGIC || header.cbKey != 32)
            throw std::runtime_error("通信公開鍵の形式が無効です。");
        Key key; Secret secret;
        Check(BCryptImportKeyPair(m_algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.value,
            Bytes(remote), static_cast<ULONG>(remote.size()), 0));
        Check(BCryptSecretAgreement(m_key, key.value, &secret.value, 0));
        NetworkKey result{}; ULONG written{};
        // Windowsが返すlittle-endianの共有秘密を双方ともそのままHKDFへ渡します。
        Check(BCryptDeriveKey(secret.value, BCRYPT_KDF_RAW_SECRET, nullptr, result.data(), static_cast<ULONG>(result.size()), &written, 0));
        if (written != result.size()) throw std::runtime_error("共有鍵の長さが無効です。");
        return result;
    }
    static std::string Gcm(const BCRYPT_KEY_HANDLE key, const bool decrypt,
        const std::span<unsigned char> nonce, const std::span<unsigned char> aad,
        const std::string_view input, const std::span<unsigned char> tag)
    {
        if (nonce.size() != 12 || tag.size() != 16 || input.empty() || input.size() > 1100)
            throw std::runtime_error("暗号化パケットの長さが無効です。");
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info; BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data(); info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = aad.empty() ? nullptr : aad.data(); info.cbAuthData = static_cast<ULONG>(aad.size());
        info.pbTag = tag.data(); info.cbTag = static_cast<ULONG>(tag.size());
        std::string result(input.size(), '\0'); ULONG written{};
        const auto operation = decrypt ? BCryptDecrypt : BCryptEncrypt;
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
        Algorithm algorithm; Key cipher;
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
        std::array<unsigned char, 12> nonce{};
        std::array<unsigned char, 9> header{}; header[0] = 2;
        for (unsigned int index = 0; index < 8; ++index)
            nonce[index + 4] = header[index + 1] = static_cast<unsigned char>(m_sequence >> (56 - index * 8));
        std::array<unsigned char, 16> tag{};
        auto encrypted = Gcm(m_key, false, nonce, header, clear, tag);
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
        std::array<unsigned char, 12> nonce{};
        std::array<unsigned char, 9> header{};
        std::copy_n(packet.data(), header.size(), header.data());
        std::uint64_t sequence{};
        for (unsigned int index = 0; index < 8; ++index)
        {
            nonce[index + 4] = header[index + 1]; sequence = (sequence << 8) | header[index + 1];
        }
        if (sequence != m_sequence) throw std::runtime_error("通信連番の再送または順序違反です。");
        std::array<unsigned char, 16> tag{};
        std::copy_n(packet.end() - 16, tag.size(), tag.data());
        auto clear = Gcm(m_key, true, nonce, header, packet.substr(9, packet.size() - 25), tag);
        ++m_sequence;
        return clear;
    }
}
