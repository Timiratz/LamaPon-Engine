#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace LamaPon
{
    inline constexpr std::size_t CloudPreferencesMaxBytes =
        256u * 1024u;
    inline constexpr std::size_t CloudSaveSlotMaxBytes =
        1024u * 1024u;
    inline constexpr std::size_t CloudSaveMaxSlots = 32u;
    inline constexpr std::size_t CloudSaveAccountMaxBytes =
        16u * 1024u * 1024u;
    inline constexpr std::size_t CloudSaveEtagMaxBytes = 96u;

    enum class CloudSaveResourceKind : std::uint8_t
    {
        Preferences,
        SaveSlot
    };

    // 所有者はLamaPon access tokenのsubからサーバーが決めます。
    // このresourceにはplayerIdやDiscord IDを持たせません。
    struct CloudSaveResource final
    {
        CloudSaveResourceKind kind{
            CloudSaveResourceKind::Preferences
        };
        // Preferencesでは必ず空です。
        std::string slot;

        [[nodiscard]] static CloudSaveResource Preferences()
        {
            return {};
        }

        [[nodiscard]] static CloudSaveResource SaveSlot(
            std::string slotName)
        {
            return {
                CloudSaveResourceKind::SaveSlot,
                std::move(slotName)
            };
        }

        // Windows上のsave slot identityは大文字小文字を区別しません。
        // byte比較のoperator==を提供せず、同期層では共通のslot identity
        // 判定を使用します。
    };

    struct CloudSaveManifestItem final
    {
        CloudSaveResource resource;
        // HTTPの強いETagを引用符込みで保持します。
        std::string etag;
        bool deleted{};
        std::uint64_t byteLength{};
        // 32-byte SHA-256のunpadded base64url。tombstoneでは空です。
        std::string sha256;
    };

    struct CloudSaveSnapshot final
    {
        CloudSaveResource resource;
        std::string etag;
        bool deleted{};
        // tombstoneでは空です。非削除時は検証済みのJSONバイト列です。
        std::vector<std::uint8_t> content;
        std::string sha256;
    };
}
