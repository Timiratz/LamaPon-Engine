#include "LamaPon/Core/Crypto.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace
{
    void ThrowIfFailed(const NTSTATUS status, const char* operation)
    {
        if (status < 0)
        {
            throw std::runtime_error(
                std::string{ operation }
                + " failed with NTSTATUS "
                + std::to_string(status));
        }
    }

    // 書き出し時にエクスポーターが直接書き換える鍵スロットです
    // （並びの説明はCrypto.hを参照）。
    //
    // constでもconstexprでもないのは、書き込み可能なセクション
    // （.data）へ置いてバイナリ書き換えを成立させるためです。
    // volatileは、コンパイラが中身を定数として畳み込み、鍵の
    // 組み立てをコード側へ展開してしまうのを防ぎます。
    volatile std::uint8_t g_archiveKeySlot[
        LamaPon::Crypto::KeySlotSize] = {
        // 目印（エクスポーターが探すための16バイト）
        0x36, 0x62, 0x28, 0xa5, 0xaf, 0x19, 0x1b, 0x85,
        0x58, 0x03, 0xd6, 0xa1, 0xbb, 0xe9, 0x72, 0x54,
        // パッド
        0xef, 0x5a, 0xac, 0x36, 0x09, 0xa2, 0xea, 0xd6,
        0x04, 0x00, 0x8a, 0xab, 0x70, 0x3a, 0x04, 0x6a,
        0xe8, 0x09, 0x3c, 0x92, 0xd6, 0xa6, 0xdd, 0x5b,
        0xa2, 0xa8, 0x4e, 0xbf, 0x8b, 0xb3, 0x72, 0x21,
        // 鍵 XOR パッド
        0x1e, 0x8d, 0x60, 0xf7, 0xc9, 0x69, 0x3b, 0x52,
        0x3d, 0x9a, 0x75, 0x1f, 0x32, 0x1d, 0xac, 0xa4,
        0x07, 0x5d, 0xb5, 0xbf, 0x70, 0x9d, 0x0f, 0xa3,
        0xb1, 0x19, 0x42, 0x34, 0x4e, 0xd9, 0xef, 0x56
    };

    // 封筒の目印。中身がAESなので、拡張子ではなく先頭バイトで
    // 「包まれているか」を判断します。
    constexpr std::array<std::uint8_t, 8> SealMagic{
        'T', 'R', 'D', 'N', 'S', 'E', 'A', 'L'
    };
    constexpr std::size_t SealHeaderSize =
        SealMagic.size()
        + LamaPon::Crypto::AesIvSize
        + LamaPon::Crypto::MacSize;

    // MAC鍵を作るときのラベル。暗号化の鍵をそのままMACへ使うと、
    // 片方の性質がもう片方へ漏れます。
    constexpr std::array<std::uint8_t, 22> MacKeyLabel{
        'T', 'r', 'i', 'd', 'e', 'n', 't', '.',
        'A', 'r', 'c', 'h', 'i', 'v', 'e', '.',
        'M', 'a', 'c', '.', 'v', '1'
    };

    void FillRandom(std::uint8_t* data, const std::size_t size)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        ThrowIfFailed(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_RNG_ALGORITHM,
                nullptr,
                0),
            "BCryptOpenAlgorithmProvider(RNG)");
        const struct AlgorithmGuard final
        {
            BCRYPT_ALG_HANDLE handle;
            ~AlgorithmGuard()
            {
                BCryptCloseAlgorithmProvider(handle, 0);
            }
        } algorithmGuard{ algorithm };

        ThrowIfFailed(
            BCryptGenRandom(
                algorithm,
                data,
                static_cast<ULONG>(size),
                0),
            "BCryptGenRandom");
    }

    std::vector<std::uint8_t> RunCipher(
        const std::uint8_t* data,
        const std::size_t size,
        const LamaPon::Crypto::AesKey& key,
        const LamaPon::Crypto::AesIv& iv,
        const bool encrypt)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        ThrowIfFailed(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_AES_ALGORITHM,
                nullptr,
                0),
            "BCryptOpenAlgorithmProvider");
        const struct AlgorithmGuard final
        {
            BCRYPT_ALG_HANDLE handle;
            ~AlgorithmGuard()
            {
                BCryptCloseAlgorithmProvider(handle, 0);
            }
        } algorithmGuard{ algorithm };

        ThrowIfFailed(
            BCryptSetProperty(
                algorithm,
                BCRYPT_CHAINING_MODE,
                reinterpret_cast<PUCHAR>(
                    const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                sizeof(BCRYPT_CHAIN_MODE_CBC),
                0),
            "BCryptSetProperty(chaining mode)");

        BCRYPT_KEY_HANDLE keyHandle{};
        ThrowIfFailed(
            BCryptGenerateSymmetricKey(
                algorithm,
                &keyHandle,
                nullptr,
                0,
                const_cast<PUCHAR>(key.data()),
                static_cast<ULONG>(key.size()),
                0),
            "BCryptGenerateSymmetricKey");
        const struct KeyGuard final
        {
            BCRYPT_KEY_HANDLE handle;
            ~KeyGuard()
            {
                BCryptDestroyKey(handle);
            }
        } keyGuard{ keyHandle };

        LamaPon::Crypto::AesIv ivCopy = iv;
        ULONG resultSize{};
        constexpr ULONG flags = BCRYPT_BLOCK_PADDING;

        const auto runOnce = [&](
            PUCHAR input,
            const ULONG inputSize,
            PUCHAR output,
            const ULONG outputCapacity)
        {
            return encrypt
                ? BCryptEncrypt(
                    keyHandle,
                    input,
                    inputSize,
                    nullptr,
                    ivCopy.data(),
                    static_cast<ULONG>(ivCopy.size()),
                    output,
                    outputCapacity,
                    &resultSize,
                    flags)
                : BCryptDecrypt(
                    keyHandle,
                    input,
                    inputSize,
                    nullptr,
                    ivCopy.data(),
                    static_cast<ULONG>(ivCopy.size()),
                    output,
                    outputCapacity,
                    &resultSize,
                    flags);
        };

        auto* mutableInput = const_cast<PUCHAR>(data);
        const auto inputSize = static_cast<ULONG>(size);

        ULONG requiredSize{};
        ThrowIfFailed(
            runOnce(mutableInput, inputSize, nullptr, 0),
            "BCryptEncrypt/Decrypt (size query)");
        requiredSize = resultSize;

        std::vector<std::uint8_t> output(requiredSize);
        ThrowIfFailed(
            runOnce(
                mutableInput,
                inputSize,
                output.data(),
                static_cast<ULONG>(output.size())),
            "BCryptEncrypt/Decrypt");
        output.resize(resultSize);
        return output;
    }
}

namespace LamaPon::Crypto
{
    AesKey ArchiveKey()
    {
        AesKey key{};
        for (std::size_t index = 0; index < key.size(); ++index)
        {
            const auto pad =
                g_archiveKeySlot[KeySlotMarkerSize + index];
            const auto stored =
                g_archiveKeySlot[
                    KeySlotMarkerSize + AesKeySize + index];
            key[index] = static_cast<std::uint8_t>(stored ^ pad);
        }
        return key;
    }

    AesKey RandomKey()
    {
        AesKey key{};
        FillRandom(key.data(), key.size());
        return key;
    }

    AesIv RandomIv()
    {
        AesIv iv{};
        FillRandom(iv.data(), iv.size());
        return iv;
    }

    KeySlotMarker ExpectedKeySlotMarker()
    {
        // 定数を返さず、動いているバイナリのスロットの先頭を
        // 読みます。ここを定数で書くと、同じ16バイトが.rdataにも
        // 載り、書き出し時のバイト列検索が2箇所に当たって
        // 「どちらが本物か決められない」で失敗します。
        //
        // エディター（＝書き出す側）のLamaPonRuntime.dllは書き換え
        // られていないので、ここには必ず目印が入っています。
        KeySlotMarker marker{};
        for (std::size_t index = 0; index < marker.size(); ++index)
        {
            marker[index] = g_archiveKeySlot[index];
        }
        return marker;
    }

    KeySlot MakeKeySlot(const AesKey& key)
    {
        KeySlot slot{};
        // 目印は乱数で潰します。配布物には「ここに鍵がある」と
        // 分かる並びを残しません。
        FillRandom(slot.data(), KeySlotMarkerSize + AesKeySize);
        for (std::size_t index = 0; index < key.size(); ++index)
        {
            const auto pad = slot[KeySlotMarkerSize + index];
            slot[KeySlotMarkerSize + AesKeySize + index] =
                static_cast<std::uint8_t>(key[index] ^ pad);
        }
        return slot;
    }

    std::vector<std::uint8_t> AesEncrypt(
        const std::uint8_t* data,
        const std::size_t size,
        const AesKey& key,
        const AesIv& iv)
    {
        return RunCipher(data, size, key, iv, true);
    }

    std::vector<std::uint8_t> AesDecrypt(
        const std::uint8_t* data,
        const std::size_t size,
        const AesKey& key,
        const AesIv& iv)
    {
        return RunCipher(data, size, key, iv, false);
    }

    MacTag Hmac(
        const AesKey& macKey,
        const std::uint8_t* data,
        const std::size_t size)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        ThrowIfFailed(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                BCRYPT_ALG_HANDLE_HMAC_FLAG),
            "BCryptOpenAlgorithmProvider(HMAC-SHA256)");
        const struct AlgorithmGuard final
        {
            BCRYPT_ALG_HANDLE handle;
            ~AlgorithmGuard()
            {
                BCryptCloseAlgorithmProvider(handle, 0);
            }
        } algorithmGuard{ algorithm };

        BCRYPT_HASH_HANDLE hash{};
        ThrowIfFailed(
            BCryptCreateHash(
                algorithm,
                &hash,
                nullptr,
                0,
                reinterpret_cast<PUCHAR>(
                    const_cast<std::uint8_t*>(macKey.data())),
                static_cast<ULONG>(macKey.size()),
                0),
            "BCryptCreateHash");
        const struct HashGuard final
        {
            BCRYPT_HASH_HANDLE handle;
            ~HashGuard()
            {
                BCryptDestroyHash(handle);
            }
        } hashGuard{ hash };

        if (size > 0)
        {
            ThrowIfFailed(
                BCryptHashData(
                    hash,
                    const_cast<PUCHAR>(data),
                    static_cast<ULONG>(size),
                    0),
                "BCryptHashData");
        }

        MacTag tag{};
        ThrowIfFailed(
            BCryptFinishHash(
                hash,
                tag.data(),
                static_cast<ULONG>(tag.size()),
                0),
            "BCryptFinishHash");
        return tag;
    }

    AesKey DeriveMacKey(const AesKey& archiveKey)
    {
        const auto tag = Hmac(
            archiveKey,
            MacKeyLabel.data(),
            MacKeyLabel.size());
        AesKey macKey{};
        std::copy(tag.begin(), tag.end(), macKey.begin());
        return macKey;
    }

    bool MacEquals(const MacTag& left, const MacTag& right)
    {
        // 一致した長さで実行時間が変わらないよう、全バイトを見ます。
        std::uint8_t difference{};
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            difference = static_cast<std::uint8_t>(
                difference | (left[index] ^ right[index]));
        }
        return difference == 0;
    }

    MacTag MacForCipherText(
        const AesKey& macKey,
        const AesIv& iv,
        const std::uint8_t* cipherText,
        const std::size_t size)
    {
        std::vector<std::uint8_t> message;
        message.reserve(iv.size() + size);
        message.insert(message.end(), iv.begin(), iv.end());
        message.insert(message.end(), cipherText, cipherText + size);
        return Hmac(macKey, message.data(), message.size());
    }

    std::vector<std::uint8_t> Seal(
        const std::uint8_t* data,
        const std::size_t size,
        const AesKey& key)
    {
        const auto iv = RandomIv();
        const auto cipherText = AesEncrypt(data, size, key, iv);
        const auto tag = MacForCipherText(
            DeriveMacKey(key),
            iv,
            cipherText.data(),
            cipherText.size());

        std::vector<std::uint8_t> sealed;
        sealed.reserve(SealHeaderSize + cipherText.size());
        sealed.insert(
            sealed.end(),
            SealMagic.begin(),
            SealMagic.end());
        sealed.insert(sealed.end(), iv.begin(), iv.end());
        sealed.insert(sealed.end(), tag.begin(), tag.end());
        sealed.insert(
            sealed.end(),
            cipherText.begin(),
            cipherText.end());
        return sealed;
    }

    bool IsSealed(const std::uint8_t* data, const std::size_t size)
    {
        return size >= SealHeaderSize
            && std::equal(
                SealMagic.begin(),
                SealMagic.end(),
                data);
    }

    std::optional<std::vector<std::uint8_t>> Unseal(
        const std::uint8_t* data,
        const std::size_t size,
        const AesKey& key)
    {
        if (!IsSealed(data, size))
        {
            return std::nullopt;
        }

        AesIv iv{};
        std::copy_n(data + SealMagic.size(), iv.size(), iv.begin());
        MacTag storedTag{};
        std::copy_n(
            data + SealMagic.size() + iv.size(),
            storedTag.size(),
            storedTag.begin());
        const std::uint8_t* cipherText = data + SealHeaderSize;
        const std::size_t cipherSize = size - SealHeaderSize;

        try
        {
            const auto macKey = DeriveMacKey(key);
            if (!MacEquals(
                    storedTag,
                    MacForCipherText(
                        macKey,
                        iv,
                        cipherText,
                        cipherSize)))
            {
                return std::nullopt;
            }
            return AesDecrypt(cipherText, cipherSize, key, iv);
        }
        catch (const std::exception&)
        {
            // 鍵違いや壊れた入力。呼び出し側が「無かったこと」に
            // できるよう、ここでは投げません。
            return std::nullopt;
        }
    }
}
