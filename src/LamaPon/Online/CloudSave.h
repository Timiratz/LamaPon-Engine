#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace LamaPon
{
    // 設定JSONの上限バイト数
    inline constexpr std::size_t CloudPreferencesMaxBytes =
        256u * 1024u;
    // スロットの上限バイト数
    inline constexpr std::size_t CloudSaveSlotMaxBytes =
        1024u * 1024u;
    // 保存スロット数の上限
    inline constexpr std::size_t CloudSaveMaxSlots = 32u;
    // アカウントの上限バイト数
    inline constexpr std::size_t CloudSaveAccountMaxBytes =
        16u * 1024u * 1024u;
    // ETagの上限バイト数
    inline constexpr std::size_t CloudSaveEtagMaxBytes = 96u;

    enum class CloudSaveResourceKind : std::uint8_t
    {
        Preferences,
        SaveSlot
    };

    // 所有者はサーバーがaccess tokenのsubから決め、識別子をこの型に含めません。
    // スロット名は共通の識別判定で比較し、Windowsでは大文字小文字を区別しません。
    struct CloudSaveResource final
    {
        // 保存データの種別
        CloudSaveResourceKind kind{
            CloudSaveResourceKind::Preferences
        };
        // スロット名・設定では空
        std::string slot;

        // 設定データの保存先を作る。
        [[nodiscard]] static CloudSaveResource Preferences()
        {
            return {};
        }

        // スロットの保存先を作る(slotName: スロット名)。
        [[nodiscard]] static CloudSaveResource SaveSlot(
            std::string slotName)
        {
            return {
                CloudSaveResourceKind::SaveSlot,
                std::move(slotName)
            };
        }
    };

    // SHA-256は32バイトの値をパディングなしbase64urlで保持します。
    struct CloudSaveManifestItem final
    {
        // 保存先の識別情報
        CloudSaveResource resource;
        // 引用符を含む強いETag
        std::string etag;
        // 削除済みの印
        bool deleted{};
        // 内容のバイト数
        std::uint64_t byteLength{};
        // SHA-256・削除時は空
        std::string sha256;
    };

    struct CloudSaveSnapshot final
    {
        // 保存先の識別情報
        CloudSaveResource resource;
        // 引用符を含む強いETag
        std::string etag;
        // 削除済みの印
        bool deleted{};
        // 検証済みJSON・削除時は空
        std::vector<std::uint8_t> content;
        // SHA-256・削除時は空
        std::string sha256;
    };
}
