#include "LamaPon/Core/Crypto.h"

#include <Windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace
{
    // CNGの失敗を例外へ変換します(status: NTSTATUSの結果, operation: 診断に付ける操作名)。
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

    struct LocalDataBlob final
    {
        // 消去して解放するDPAPI出力
        DATA_BLOB value{};

        // DPAPIが確保した領域を消去して解放します。
        ~LocalDataBlob()
        {
            if (value.pbData != nullptr)
            {
                SecureZeroMemory(value.pbData, value.cbData);
                LocalFree(value.pbData);
            }
        }
    };

    // DPAPIで保護または復元します(data: 入力先頭, size: 入力バイト数, entropy: 用途を分離する補助データ, entropySize: 補助データのバイト数, protect: 保護ならtrue)。
    LamaPon::Crypto::CurrentUserProtectionResult RunCurrentUserProtection(
        const std::uint8_t* data,
        const std::size_t size,
        const std::uint8_t* entropy,
        const std::size_t entropySize,
        const bool protect)
    {
        using LamaPon::Crypto::CurrentUserProtectionResult;
        using LamaPon::Crypto::CurrentUserProtectionStatus;

        // DPAPIの処理結果と出力
        CurrentUserProtectionResult result;
        if ((size != 0 && data == nullptr)
            || (entropySize != 0 && entropy == nullptr)
            || size > std::numeric_limits<DWORD>::max()
            || entropySize > std::numeric_limits<DWORD>::max())
        {
            result.status = protect
                ? CurrentUserProtectionStatus::Unavailable
                : CurrentUserProtectionStatus::InvalidData;
            result.platformError = ERROR_INVALID_PARAMETER;
            return result;
        }

        // DPAPIへ渡す入力データ
        DATA_BLOB input{
            static_cast<DWORD>(size),
            const_cast<BYTE*>(
                reinterpret_cast<const BYTE*>(data))
        };
        // 保護用途を分離する補助データ
        DATA_BLOB optionalEntropy{
            static_cast<DWORD>(entropySize),
            const_cast<BYTE*>(
                reinterpret_cast<const BYTE*>(entropy))
        };
        // DPAPIが確保する出力領域
        LocalDataBlob output;
        // DPAPIが返す保護時の説明
        LPWSTR description{};
        // DPAPI処理に成功したか
        const BOOL succeeded = protect
            ? CryptProtectData(
                &input,
                L"LamaPon online session",
                entropySize == 0 ? nullptr : &optionalEntropy,
                nullptr,
                nullptr,
                CRYPTPROTECT_UI_FORBIDDEN,
                &output.value)
            : CryptUnprotectData(
                &input,
                &description,
                entropySize == 0 ? nullptr : &optionalEntropy,
                nullptr,
                nullptr,
                CRYPTPROTECT_UI_FORBIDDEN,
                &output.value);
        // DPAPI処理直後のエラー番号
        const DWORD operationError = succeeded == FALSE
            ? GetLastError()
            : ERROR_SUCCESS;
        if (description != nullptr)
        {
            // 説明文字列と終端のバイト数
            const auto bytes = (wcslen(description) + 1)
                * sizeof(wchar_t);
            SecureZeroMemory(description, bytes);
            LocalFree(description);
        }
        if (succeeded == FALSE)
        {
            result.status = !protect
                    && operationError == ERROR_INVALID_DATA
                ? CurrentUserProtectionStatus::InvalidData
                : CurrentUserProtectionStatus::Unavailable;
            result.platformError = operationError;
            return result;
        }

        if (output.value.cbData != 0)
        {
            result.data.assign(
                output.value.pbData,
                output.value.pbData + output.value.cbData);
        }
        result.status = CurrentUserProtectionStatus::Succeeded;
        return result;
    }

    // 書き出し時に置換する鍵スロット
    // 書き換え可能な.dataへ配置するため、constやconstexprにはしません。
    // volatileで鍵の定数展開を防ぎ、80バイトの配置はCrypto.hのKeySlotと揃えます。
    volatile std::uint8_t g_archiveKeySlot[
        LamaPon::Crypto::KeySlotSize] = {
        0x36, 0x62, 0x28, 0xa5, 0xaf, 0x19, 0x1b, 0x85,
        0x58, 0x03, 0xd6, 0xa1, 0xbb, 0xe9, 0x72, 0x54,
        0xef, 0x5a, 0xac, 0x36, 0x09, 0xa2, 0xea, 0xd6,
        0x04, 0x00, 0x8a, 0xab, 0x70, 0x3a, 0x04, 0x6a,
        0xe8, 0x09, 0x3c, 0x92, 0xd6, 0xa6, 0xdd, 0x5b,
        0xa2, 0xa8, 0x4e, 0xbf, 0x8b, 0xb3, 0x72, 0x21,
        0x1e, 0x8d, 0x60, 0xf7, 0xc9, 0x69, 0x3b, 0x52,
        0x3d, 0x9a, 0x75, 0x1f, 0x32, 0x1d, 0xac, 0xa4,
        0x07, 0x5d, 0xb5, 0xbf, 0x70, 0x9d, 0x0f, 0xa3,
        0xb1, 0x19, 0x42, 0x34, 0x4e, 0xd9, 0xef, 0x56
    };

    // ファイル暗号形式の識別バイト列
    constexpr std::array<std::uint8_t, 8> SealMagic{
        'T', 'R', 'D', 'N', 'S', 'E', 'A', 'L'
    };
    // 目印とIVとMACの総バイト数
    constexpr std::size_t SealHeaderSize =
        SealMagic.size()
        + LamaPon::Crypto::AesIvSize
        + LamaPon::Crypto::MacSize;

    // 認証鍵の派生用途を分離するラベル
    constexpr std::array<std::uint8_t, 22> MacKeyLabel{
        'T', 'r', 'i', 'd', 'e', 'n', 't', '.',
        'A', 'r', 'c', 'h', 'i', 'v', 'e', '.',
        'M', 'a', 'c', '.', 'v', '1'
    };

    // CNGの乱数で領域を埋めます(data: 出力先頭, size: 出力バイト数)。
    void FillRandom(std::uint8_t* data, const std::size_t size)
    {
        // CNGアルゴリズムのハンドル
        BCRYPT_ALG_HANDLE algorithm{};
        ThrowIfFailed(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_RNG_ALGORITHM,
                nullptr,
                0),
            "BCryptOpenAlgorithmProvider(RNG)");
        // アルゴリズムを自動解放するガード
        const struct AlgorithmGuard final
        {
            // 解放対象のアルゴリズム
            BCRYPT_ALG_HANDLE handle;
            // アルゴリズムのハンドルを閉じます。
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

    // AES-CBC処理を実行します(data: 入力先頭, size: 入力バイト数, key: 暗号鍵, iv: 初期化ベクトル, encrypt: 暗号化ならtrue)。
    std::vector<std::uint8_t> RunCipher(
        const std::uint8_t* data,
        const std::size_t size,
        const LamaPon::Crypto::AesKey& key,
        const LamaPon::Crypto::AesIv& iv,
        const bool encrypt)
    {
        // CNGアルゴリズムのハンドル
        BCRYPT_ALG_HANDLE algorithm{};
        ThrowIfFailed(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_AES_ALGORITHM,
                nullptr,
                0),
            "BCryptOpenAlgorithmProvider");
        // アルゴリズムを自動解放するガード
        const struct AlgorithmGuard final
        {
            // 解放対象のアルゴリズム
            BCRYPT_ALG_HANDLE handle;
            // アルゴリズムのハンドルを閉じます。
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

        // CNGへ登録したAES鍵
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
        // AES鍵を自動解放するガード
        const struct KeyGuard final
        {
            // 解放対象のAES鍵
            BCRYPT_KEY_HANDLE handle;
            // CNGへ登録したAES鍵を破棄します。
            ~KeyGuard()
            {
                BCryptDestroyKey(handle);
            }
        } keyGuard{ keyHandle };

        // CNGが書き換えるIVの複製
        LamaPon::Crypto::AesIv ivCopy = iv;
        // CNGが返す出力バイト数
        ULONG resultSize{};
        // PKCS#7パディングの指定
        constexpr ULONG flags = BCRYPT_BLOCK_PADDING;

        // AESを一度実行します(input: 入力先頭, inputSize: 入力バイト数, output: 出力先頭または容量照会時のnull, outputCapacity: 出力容量)。
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

        // CNGの入力型へ合わせたポインター
        auto* mutableInput = const_cast<PUCHAR>(data);
        // CNGへ渡す入力バイト数
        const auto inputSize = static_cast<ULONG>(size);

        // 容量照会で得る必要バイト数
        ULONG requiredSize{};
        ThrowIfFailed(
            runOnce(mutableInput, inputSize, nullptr, 0),
            "BCryptEncrypt/Decrypt (size query)");
        requiredSize = resultSize;

        // 暗号化または復号したバイト列
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
    CurrentUserProtectionResult ProtectForCurrentUser(
        const std::uint8_t* data,
        const std::size_t size,
        const std::uint8_t* entropy,
        const std::size_t entropySize)
    {
        return RunCurrentUserProtection(
            data,
            size,
            entropy,
            entropySize,
            true);
    }

    CurrentUserProtectionResult UnprotectForCurrentUser(
        const std::uint8_t* data,
        const std::size_t size,
        const std::uint8_t* entropy,
        const std::size_t entropySize)
    {
        return RunCurrentUserProtection(
            data,
            size,
            entropy,
            entropySize,
            false);
    }

    void SecureErase(
        std::uint8_t* data,
        const std::size_t size) noexcept
    {
        if (data != nullptr && size != 0)
        {
            SecureZeroMemory(data, size);
        }
    }

    void SecureErase(std::vector<std::uint8_t>& data) noexcept
    {
        SecureErase(data.data(), data.size());
        data.clear();
    }

    void SecureErase(std::string& data) noexcept
    {
        if (!data.empty())
        {
            SecureZeroMemory(data.data(), data.size());
        }
        data.clear();
    }

    AesKey ArchiveKey()
    {
        // 復元または生成するAES鍵
        AesKey key{};
        // 鍵内の処理バイト位置
        for (std::size_t index = 0; index < key.size(); ++index)
        {
            // 鍵を隠すXORパッドのバイト
            const auto pad =
                g_archiveKeySlot[KeySlotMarkerSize + index];
            // パッドとXORされた鍵のバイト
            const auto stored =
                g_archiveKeySlot[
                    KeySlotMarkerSize + AesKeySize + index];
            key[index] = static_cast<std::uint8_t>(stored ^ pad);
        }
        return key;
    }

    AesKey RandomKey()
    {
        // 復元または生成するAES鍵
        AesKey key{};
        FillRandom(key.data(), key.size());
        return key;
    }

    AesIv RandomIv()
    {
        // 暗号処理の初期化ベクトル
        AesIv iv{};
        FillRandom(iv.data(), iv.size());
        return iv;
    }

    KeySlotMarker ExpectedKeySlotMarker()
    {
        // 実行中の鍵スロットの目印
        KeySlotMarker marker{};
        // 目印内の読み取り位置
        for (std::size_t index = 0; index < marker.size(); ++index)
        {
            marker[index] = g_archiveKeySlot[index];
        }
        return marker;
    }

    KeySlot MakeKeySlot(const AesKey& key)
    {
        // バイナリに埋め込む鍵スロット
        KeySlot slot{};
        // 配布物から目印を消すため、目印とパッドを乱数で置換します。
        FillRandom(slot.data(), KeySlotMarkerSize + AesKeySize);
        // 鍵内の処理バイト位置
        for (std::size_t index = 0; index < key.size(); ++index)
        {
            // 鍵を隠すXORパッドのバイト
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
        // CNGアルゴリズムのハンドル
        BCRYPT_ALG_HANDLE algorithm{};
        ThrowIfFailed(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                BCRYPT_ALG_HANDLE_HMAC_FLAG),
            "BCryptOpenAlgorithmProvider(HMAC-SHA256)");
        // アルゴリズムを自動解放するガード
        const struct AlgorithmGuard final
        {
            // 解放対象のアルゴリズム
            BCRYPT_ALG_HANDLE handle;
            // アルゴリズムのハンドルを閉じます。
            ~AlgorithmGuard()
            {
                BCryptCloseAlgorithmProvider(handle, 0);
            }
        } algorithmGuard{ algorithm };

        // HMAC計算状態のハンドル
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
        // HMAC計算状態を解放するガード
        const struct HashGuard final
        {
            // 解放対象のHMAC計算状態
            BCRYPT_HASH_HANDLE handle;
            // HMAC計算状態を破棄します。
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

        // 計算したHMAC認証タグ
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
        // 固定ラベルから派生させたタグ
        const auto tag = Hmac(
            archiveKey,
            MacKeyLabel.data(),
            MacKeyLabel.size());
        // アーカイブ用の認証鍵
        AesKey macKey{};
        std::copy(tag.begin(), tag.end(), macKey.begin());
        return macKey;
    }

    Sha256Digest Sha256(
        const std::uint8_t* data,
        const std::size_t size)
    {
        if ((size != 0 && data == nullptr)
            || size > std::numeric_limits<ULONG>::max())
        {
            throw std::invalid_argument(
                "SHA-256 input is invalid or too large.");
        }
        // CNGアルゴリズムのハンドル
        BCRYPT_ALG_HANDLE algorithm{};
        ThrowIfFailed(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0),
            "BCryptOpenAlgorithmProvider(SHA-256)");
        // アルゴリズムを自動解放するガード
        const struct AlgorithmGuard final
        {
            // 解放対象のアルゴリズム
            BCRYPT_ALG_HANDLE handle;
            // アルゴリズムのハンドルを閉じます。
            ~AlgorithmGuard()
            {
                BCryptCloseAlgorithmProvider(handle, 0);
            }
        } algorithmGuard{ algorithm };

        // SHA-256計算結果の32バイト
        Sha256Digest digest{};
        ThrowIfFailed(
            BCryptHash(
                algorithm,
                nullptr,
                0,
                const_cast<PUCHAR>(data),
                static_cast<ULONG>(size),
                digest.data(),
                static_cast<ULONG>(digest.size())),
            "BCryptHash(SHA-256)");
        return digest;
    }

    std::string Sha256Hex(
        const std::uint8_t* data,
        const std::size_t size)
    {
        // 小文字16進表記の数字一覧
        constexpr char Digits[] = "0123456789abcdef";
        // 16進表記へ変換するダイジェスト
        const auto digest = Sha256(data, size);
        // 小文字16進64桁の出力文字列
        std::string text;
        text.reserve(digest.size() * 2);
        // 16進表記へ変換する各バイト
        for (const auto value : digest)
        {
            text.push_back(Digits[value >> 4]);
            text.push_back(Digits[value & 0x0f]);
        }
        return text;
    }

    bool MacEquals(const MacTag& left, const MacTag& right)
    {
        // 全バイトの不一致ビットの累積
        std::uint8_t difference{};
        // 認証タグ内の比較位置
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
        // MAC対象のIVと暗号文の連結
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
        // この暗号化専用のランダムIV
        const auto iv = RandomIv();
        // 平文をAES-CBCで暗号化した結果
        const auto cipherText = AesEncrypt(data, size, key, iv);
        // IVと暗号文に対する認証タグ
        const auto tag = MacForCipherText(
            DeriveMacKey(key),
            iv,
            cipherText.data(),
            cipherText.size());

        // ヘッダーを含むファイル暗号データ
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

        // 暗号処理の初期化ベクトル
        AesIv iv{};
        std::copy_n(data + SealMagic.size(), iv.size(), iv.begin());
        // ヘッダーに格納された認証タグ
        MacTag storedTag{};
        std::copy_n(
            data + SealMagic.size() + iv.size(),
            storedTag.size(),
            storedTag.begin());
        // ヘッダー後の暗号文先頭
        const std::uint8_t* cipherText = data + SealHeaderSize;
        // ヘッダーを除く暗号文バイト数
        const std::size_t cipherSize = size - SealHeaderSize;

        try
        {
            // 復号前の照合に使う認証鍵
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
            // 検証や復号の失敗は復元不能として返します。
            return std::nullopt;
        }
    }
}
