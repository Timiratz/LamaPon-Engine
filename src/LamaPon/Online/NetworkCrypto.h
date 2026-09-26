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
    NetworkKey RandomNetworkKey();
    std::string Hex(std::span<const unsigned char> bytes);
    bool Unhex(std::string_view text, std::span<unsigned char> bytes);
    NetworkKey NetworkHmac(std::span<const unsigned char> key, std::span<const unsigned char> data);
    NetworkKey NetworkHkdf(std::span<const unsigned char> salt, std::span<const unsigned char> secret,
        std::string_view context);

    // 秘密鍵は接続ごとに生成し、ファイルやWindowsの鍵ストアには保存しません。
    class NetworkKeyExchange final
    {
    public:
        NetworkKeyExchange();
        ~NetworkKeyExchange();
        NetworkKeyExchange(const NetworkKeyExchange&) = delete;
        NetworkKeyExchange& operator=(const NetworkKeyExchange&) = delete;
        std::array<unsigned char, 72> PublicKey() const;
        NetworkKey Agree(std::span<const unsigned char> remote) const;
    private:
        BCRYPT_ALG_HANDLE m_algorithm{};
        BCRYPT_KEY_HANDLE m_key{};
    };

    std::string NetworkAesGcm(bool decrypt, const NetworkKey& key,
        std::span<unsigned char> nonce, std::span<unsigned char> aad,
        std::string_view input, std::span<unsigned char> tag);

    // 送信方向ごとに異なる鍵を使用します。連番は同じ接続内で再利用しません。
    class NetworkCipher final
    {
    public:
        NetworkCipher() = default;
        ~NetworkCipher();
        NetworkCipher(const NetworkCipher&) = delete;
        NetworkCipher& operator=(const NetworkCipher&) = delete;
        void Initialize(const NetworkKey& key);
        std::string Seal(std::string_view clear);
        std::string Open(std::string_view packet);
    private:
        BCRYPT_ALG_HANDLE m_algorithm{};
        BCRYPT_KEY_HANDLE m_key{};
        std::uint64_t m_sequence{};
        bool m_initialized{};
    };
}
