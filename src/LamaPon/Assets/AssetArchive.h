#pragma once

#include "LamaPon/Core/Crypto.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    // AssetPackerが書き出す暗号化アーカイブ（.tpak）の読み取り専用
    // インターフェースです。各エントリはAES-256-CBCで個別に暗号化し、
    // 必要なアセットだけを復号できます。
    //
    // エントリと索引にはHMAC-SHA256が付いていて、復号する前に
    // 検証します。合わなければ例外です（読み飛ばして続けると、
    // 差し替えられたアセットをそのまま使ってしまいます）。
    class AssetArchive final
    {
    public:
        // このバイナリへ焼き込まれた鍵で開きます（＝書き出された
        // ゲームが自分のassets.tpakを開くときの経路）。
        [[nodiscard]] static std::unique_ptr<AssetArchive> Open(
            const std::filesystem::path& archivePath);

        // 鍵を明示して開きます。書き出した側（エディターやテスト）が
        // 「今作ったアーカイブが本当にその鍵で開くか」を確かめる
        // ための口です。
        [[nodiscard]] static std::unique_ptr<AssetArchive> Open(
            const std::filesystem::path& archivePath,
            const Crypto::AesKey& key);

        [[nodiscard]] bool Contains(
            const std::filesystem::path& relativePath) const;
        [[nodiscard]] std::optional<std::vector<std::uint8_t>> TryRead(
            const std::filesystem::path& relativePath) const;

        [[nodiscard]] std::size_t EntryCount() const noexcept
        {
            return m_entries.size();
        }

    private:
        struct Entry final
        {
            std::uint64_t offset{};
            std::uint64_t size{};
            std::array<std::uint8_t, 16> iv{};
            std::array<std::uint8_t, 32> mac{};
        };

        AssetArchive(
            std::filesystem::path archivePath,
            const Crypto::AesKey& key);

        [[nodiscard]] static std::string NormalizeKey(
            const std::filesystem::path& relativePath);

        std::filesystem::path m_archivePath;
        Crypto::AesKey m_key{};
        // 読むたびに派生させると、アセット1つごとにCNGのプロバイダーを
        // 開き直すことになるので、開いたときに1回だけ作ります。
        Crypto::AesKey m_macKey{};
        std::unordered_map<std::string, Entry> m_entries;
        std::uint64_t m_payloadStart{};
    };
}
