#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace LamaPon::Crypto
{
    // AES-256鍵のバイト数
    inline constexpr std::size_t AesKeySize = 32;
    // AES初期化ベクトルのバイト数
    inline constexpr std::size_t AesIvSize = 16;
    // HMAC-SHA256タグのバイト数
    inline constexpr std::size_t MacSize = 32;

    using AesKey = std::array<std::uint8_t, AesKeySize>;
    using AesIv = std::array<std::uint8_t, AesIvSize>;
    using MacTag = std::array<std::uint8_t, MacSize>;

    enum class CurrentUserProtectionStatus : std::uint8_t
    {
        // 保護または復元の成功
        Succeeded,
        // 復元対象データの不正
        InvalidData,
        // 保護または復元を実行できない
        Unavailable
    };

    struct CurrentUserProtectionResult final
    {
        // ユーザー保護処理の結果
        CurrentUserProtectionStatus status{
            CurrentUserProtectionStatus::Unavailable
        };
        // 保護または復元したバイト列
        std::vector<std::uint8_t> data;
        // 診断用のWin32エラー番号
        std::uint32_t platformError{};

        // ユーザー保護処理が成功したかを返します。
        [[nodiscard]] bool Succeeded() const noexcept
        {
            return status == CurrentUserProtectionStatus::Succeeded;
        }
    };

    // DPAPIで現在のユーザー用に保護します(data: 入力先頭, size: 入力バイト数, entropy: 用途を分離する補助データ, entropySize: 補助データのバイト数)。
    // 復元には同じWindowsユーザーと同じ補助データが必要です。
    [[nodiscard]] CurrentUserProtectionResult ProtectForCurrentUser(
        const std::uint8_t* data,
        std::size_t size,
        const std::uint8_t* entropy,
        std::size_t entropySize);
    // DPAPIで現在のユーザー用のデータを復元します(data: 保護データ先頭, size: 入力バイト数, entropy: 保護時の補助データ, entropySize: 補助データのバイト数)。
    [[nodiscard]] CurrentUserProtectionResult UnprotectForCurrentUser(
        const std::uint8_t* data,
        std::size_t size,
        const std::uint8_t* entropy,
        std::size_t entropySize);

    // 使用中の領域をゼロで消去します(data: 消去先頭, size: 消去するバイト数)。
    // 他の領域に残ったコピーは消去しないため、呼び出し側でも秘密の複製を抑えます。
    void SecureErase(
        std::uint8_t* data,
        std::size_t size) noexcept;
    // バイト列の使用領域をゼロで消去して空にします(data: 消去するバイト列)。
    void SecureErase(std::vector<std::uint8_t>& data) noexcept;
    // 文字列の使用領域をゼロで消去して空にします(data: 消去する文字列)。
    void SecureErase(std::string& data) noexcept;

    // 実行バイナリの鍵スロットからアーカイブ鍵を復元します。
    // 書き出し時にゲーム固有の鍵を埋め込み、未書き出しのエディターとテストでは既定値を使います。
    // 配布バイナリに鍵を含むため、実行ファイルの解析に対する秘密性は保証しません。
    [[nodiscard]] AesKey ArchiveKey();

    // CNGの乱数でAES-256鍵を生成します。
    [[nodiscard]] AesKey RandomKey();
    // CNGの乱数でAES初期化ベクトルを生成します。
    [[nodiscard]] AesIv RandomIv();

    // 鍵スロットを探す目印のバイト数
    inline constexpr std::size_t KeySlotMarkerSize = 16;
    // 鍵を分割して埋め込むスロットの数
    // 各スロットは目印の後ろにデータを持ち、実行時に全データのXORで鍵を合成します。
    inline constexpr std::size_t KeySlotCount = 3;
    // 1スロットの全長(目印16とデータ32)
    // スロットは[0,16)が目印、[16,48)がデータで構成します。
    inline constexpr std::size_t KeySlotSize =
        KeySlotMarkerSize + AesKeySize;

    using KeySlot = std::array<std::uint8_t, KeySlotSize>;
    using KeySlotMarker = std::array<std::uint8_t, KeySlotMarkerSize>;

    // 実行バイナリから指定スロットの目印を読み取ります(index: 0からKeySlotCount-1)。
    // 目印を別の定数へ複製すると検索先が重複するため、必ずスロット自身から読みます。
    [[nodiscard]] KeySlotMarker ExpectedKeySlotMarker(std::size_t index);

    // 目印を乱数で置換した分割鍵スロット群を組み立てます(key: 埋め込むアーカイブ鍵)。
    // 鍵は全スロットのデータのXORへ分割され、どの単一スロットからも復元できません。
    [[nodiscard]] std::array<KeySlot, KeySlotCount>
        MakeKeySlots(const AesKey& key);

    // AES-256-CBCとPKCS#7で暗号化します(data: 平文先頭, size: 平文バイト数, key: 暗号鍵, iv: 初期化ベクトル)。
    // 入力はULONGの範囲内に限り、CNGの失敗には例外を送出します。
    [[nodiscard]] std::vector<std::uint8_t> AesEncrypt(
        const std::uint8_t* data,
        std::size_t size,
        const AesKey& key,
        const AesIv& iv);
    // AES-256-CBCとPKCS#7で復号します(data: 暗号文先頭, size: 暗号文バイト数, key: 暗号鍵, iv: 初期化ベクトル)。
    // 入力はULONGの範囲内に限り、事前のMAC検証は呼び出し側で行います。
    [[nodiscard]] std::vector<std::uint8_t> AesDecrypt(
        const std::uint8_t* data,
        std::size_t size,
        const AesKey& key,
        const AesIv& iv);

    // バイト列をAES-256-CBCで暗号化します(data: 平文, key: 暗号鍵, iv: 初期化ベクトル)。
    [[nodiscard]] inline std::vector<std::uint8_t> AesEncrypt(
        const std::vector<std::uint8_t>& data,
        const AesKey& key,
        const AesIv& iv)
    {
        return AesEncrypt(data.data(), data.size(), key, iv);
    }

    // MAC検証済みのバイト列をAES-256-CBCで復号します(data: 暗号文, key: 暗号鍵, iv: 初期化ベクトル)。
    [[nodiscard]] inline std::vector<std::uint8_t> AesDecrypt(
        const std::vector<std::uint8_t>& data,
        const AesKey& key,
        const AesIv& iv)
    {
        return AesDecrypt(data.data(), data.size(), key, iv);
    }

    // 固定ラベルのHMACから認証用の鍵を派生させます(archiveKey: アーカイブの暗号鍵)。
    [[nodiscard]] AesKey DeriveMacKey(const AesKey& archiveKey);

    // データのHMAC-SHA256タグを返します(macKey: 認証鍵, data: 入力先頭, size: 入力バイト数)。
    // 入力バイト数はULONGの範囲内に限ります。
    [[nodiscard]] MacTag Hmac(
        const AesKey& macKey,
        const std::uint8_t* data,
        std::size_t size);

    // 公開データの同一性確認用SHA-256（CNG）。
    // 配布パッケージの照合など、鍵を使わない検証に使います。
    using Sha256Digest = std::array<std::uint8_t, 32>;

    // データのSHA-256ダイジェストを返します(data: 入力先頭, size: 入力バイト数)。
    [[nodiscard]] Sha256Digest Sha256(
        const std::uint8_t* data,
        std::size_t size);

    // SHA-256を小文字の16進64桁で返します(data: 入力先頭, size: 入力バイト数)。
    [[nodiscard]] std::string Sha256Hex(
        const std::uint8_t* data,
        std::size_t size);

    // 全バイトを比較してMACの一致を返します(left: 比較元のタグ, right: 比較先のタグ)。
    [[nodiscard]] bool MacEquals(
        const MacTag& left,
        const MacTag& right);

    // IVと暗号文を連結したMACを返します(macKey: 認証鍵, iv: 初期化ベクトル, cipherText: 暗号文先頭, size: 暗号文バイト数)。
    [[nodiscard]] MacTag MacForCipherText(
        const AesKey& macKey,
        const AesIv& iv,
        const std::uint8_t* cipherText,
        std::size_t size);

    // ファイル用の認証付き暗号データを作ります(data: 平文先頭, size: 平文バイト数, key: 暗号鍵)。
    // 出力は目印8バイト、IV16バイト、MAC32バイト、暗号文の順に格納します。
    [[nodiscard]] std::vector<std::uint8_t> Seal(
        const std::uint8_t* data,
        std::size_t size,
        const AesKey& key);

    // ファイル暗号形式のヘッダーがあるかを返します(data: 入力先頭, size: 入力バイト数)。
    [[nodiscard]] bool IsSealed(
        const std::uint8_t* data,
        std::size_t size);

    // MAC検証後にファイル暗号データを復元します(data: 入力先頭, size: 入力バイト数, key: 暗号鍵)。
    // ヘッダー不一致・MAC不一致・復号失敗にはnulloptを返します。
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> Unseal(
        const std::uint8_t* data,
        std::size_t size,
        const AesKey& key);
}
