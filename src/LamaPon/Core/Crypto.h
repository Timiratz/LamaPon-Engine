#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace LamaPon::Crypto
{
    inline constexpr std::size_t AesKeySize = 32;
    inline constexpr std::size_t AesIvSize = 16;
    inline constexpr std::size_t MacSize = 32;

    using AesKey = std::array<std::uint8_t, AesKeySize>;
    using AesIv = std::array<std::uint8_t, AesIvSize>;
    using MacTag = std::array<std::uint8_t, MacSize>;

    // このバイナリへ焼き込まれている鍵。書き出したゲームでは
    // 「そのゲームだけの鍵」がエクスポーターによって埋め込まれます
    // （下のKeySlotを参照）。エディターやテストでは、埋め込み前の
    // 既定値がそのまま返ります。
    //
    // 鍵は復号する実行ファイルへ同梱するしかないので、解析者に対する
    // 秘密にはなりません。ここで守れるのは「1本解いても他のゲームは
    // 開けない」ことと、「バイナリを眺めただけでは鍵が拾えない」ことの
    // 2つだけです。
    [[nodiscard]] AesKey ArchiveKey();

    // 書き出しごとに新しい鍵を作ります（CNGの乱数）。
    [[nodiscard]] AesKey RandomKey();
    [[nodiscard]] AesIv RandomIv();

    // 鍵スロット（エクスポーターがバイナリを書き換えるための領域）
    //
    // LamaPonRuntime.dllの.dataには、次の並びのブロックが1つだけあります。
    //
    //     [0,16)  目印（エクスポーターが探すための16バイト）
    //     [16,48) パッド（乱数）
    //     [48,80) 鍵 XOR パッド
    //
    // 書き出しでは、このブロックをゲームごとの鍵で上書きし、目印も
    // 乱数で潰します。実行時はブロックの位置をアドレスで知っている
    // ので目印は要らず、配布物からは探す手掛かりが消えます。
    inline constexpr std::size_t KeySlotMarkerSize = 16;
    inline constexpr std::size_t KeySlotSize =
        KeySlotMarkerSize + AesKeySize + AesKeySize;

    using KeySlot = std::array<std::uint8_t, KeySlotSize>;
    using KeySlotMarker = std::array<std::uint8_t, KeySlotMarkerSize>;

    // 探すための目印。このバイナリのスロットの先頭から読みます。
    // 定数として持つと、同じ並びが.rdataにも載って書き換え先が
    // 2箇所に見えてしまうためです（書き出しはそこで失敗します）。
    [[nodiscard]] KeySlotMarker ExpectedKeySlotMarker();

    // 目印を乱数で潰した、書き込み用のスロットを組み立てます。
    [[nodiscard]] KeySlot MakeKeySlot(const AesKey& key);

    // AES-256-CBC（PKCS#7）とHMAC-SHA256。どちらもCNG（bcrypt）を使用。
    [[nodiscard]] std::vector<std::uint8_t> AesEncrypt(
        const std::uint8_t* data,
        std::size_t size,
        const AesKey& key,
        const AesIv& iv);
    [[nodiscard]] std::vector<std::uint8_t> AesDecrypt(
        const std::uint8_t* data,
        std::size_t size,
        const AesKey& key,
        const AesIv& iv);

    [[nodiscard]] inline std::vector<std::uint8_t> AesEncrypt(
        const std::vector<std::uint8_t>& data,
        const AesKey& key,
        const AesIv& iv)
    {
        return AesEncrypt(data.data(), data.size(), key, iv);
    }

    [[nodiscard]] inline std::vector<std::uint8_t> AesDecrypt(
        const std::vector<std::uint8_t>& data,
        const AesKey& key,
        const AesIv& iv)
    {
        return AesDecrypt(data.data(), data.size(), key, iv);
    }

    // 改ざん検知用の鍵。暗号化と同じ鍵をMACへ流用しないための
    // 派生です（HMAC-SHA256(archiveKey, 固定ラベル)）。
    [[nodiscard]] AesKey DeriveMacKey(const AesKey& archiveKey);

    [[nodiscard]] MacTag Hmac(
        const AesKey& macKey,
        const std::uint8_t* data,
        std::size_t size);

    // MACの比較は必ずこれを通します（内容で早期returnしない）。
    [[nodiscard]] bool MacEquals(
        const MacTag& left,
        const MacTag& right);

    // 暗号文＋IVに対するMAC。encrypt-then-MACなので、復号する前に
    // 検証できます（壊れた／すり替えられた入力をAESへ通さない）。
    [[nodiscard]] MacTag MacForCipherText(
        const AesKey& macKey,
        const AesIv& iv,
        const std::uint8_t* cipherText,
        std::size_t size);

    // 単体ファイル用の暗号化形式
    //
    // 配布物へ同梱する1ファイルを、そのまま置き換えられる形で
    // 包みます。並びは 目印8 + IV16 + MAC32 + 暗号文 です。
    // 読む側は目印を見て「包まれているかどうか」を判断できるので、
    // 平文のまま置かれたファイル（開発中のキャッシュなど）と
    // 混在できます。
    [[nodiscard]] std::vector<std::uint8_t> Seal(
        const std::uint8_t* data,
        std::size_t size,
        const AesKey& key);

    [[nodiscard]] bool IsSealed(
        const std::uint8_t* data,
        std::size_t size);

    // 包まれていない、あるいはMACが合わないときはnulloptを返します
    // （呼び出し側が「無かったこと」にできるように、例外にはしません）。
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> Unseal(
        const std::uint8_t* data,
        std::size_t size,
        const AesKey& key);
}
