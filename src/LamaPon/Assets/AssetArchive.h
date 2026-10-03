#pragma once

#include "LamaPon/Core/Crypto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    namespace AssetArchiveLimits
    {
        // 暗号化索引の上限バイト数
        inline constexpr std::uint64_t MaxIndexCipherBytes =
            64ull * 1024 * 1024;
        // 各暗号文の上限バイト数
        inline constexpr std::uint64_t MaxEntryCipherBytes =
            512ull * 1024 * 1024;
        // 登録エントリー数の上限
        inline constexpr std::size_t MaxEntries = 100'000;
        // UTF8パスの上限バイト数
        inline constexpr std::size_t MaxPathBytes = 4'096;
    }

    // .tpakの索引と各エントリーを復号前に認証し、不一致は例外にする。
    class AssetArchive final
    {
    public:
        // 組み込み鍵で認証付きアーカイブを開く(archivePath: アーカイブのパス)。
        [[nodiscard]] static std::unique_ptr<AssetArchive> Open(
            const std::filesystem::path& archivePath);

        // 指定鍵で認証付きアーカイブを開く(archivePath: アーカイブのパス, key: 復号鍵)。
        [[nodiscard]] static std::unique_ptr<AssetArchive> Open(
            const std::filesystem::path& archivePath,
            const Crypto::AesKey& key);

        // 正規化した相対パスの登録有無を返す(relativePath: アセットの相対パス)。
        [[nodiscard]] bool Contains(
            const std::filesystem::path& relativePath) const;
        // 認証後に復号し、未登録はnullopt、認証失敗は例外にする(relativePath: アセットの相対パス)。
        [[nodiscard]] std::optional<std::vector<std::uint8_t>> TryRead(
            const std::filesystem::path& relativePath) const;

        // 登録エントリーの件数を返す。
        [[nodiscard]] std::size_t EntryCount() const noexcept
        {
            return m_entries.size();
        }

    private:
        struct Entry final
        {
            // 暗号データ領域内の相対位置
            std::uint64_t offset{};
            // 暗号文のバイト数
            std::uint64_t size{};
            // エントリーの初期化ベクトル
            std::array<std::uint8_t, 16> iv{};
            // IVと暗号文の認証タグ
            std::array<std::uint8_t, 32> mac{};
        };

        // パスと鍵を保持し認証鍵を一度派生する(archivePath: アーカイブのパス, key: 復号鍵)。
        AssetArchive(
            std::filesystem::path archivePath,
            const Crypto::AesKey& key);

        // 相対パスを正規化してUTF8の小文字キーにする(relativePath: アセットの相対パス)。
        [[nodiscard]] static std::string NormalizeKey(
            const std::filesystem::path& relativePath);

        // 読み直すアーカイブのパス
        std::filesystem::path m_archivePath;
        // エントリーの復号鍵
        Crypto::AesKey m_key{};
        // 一度だけ派生する認証鍵
        Crypto::AesKey m_macKey{};
        // 小文字パスと暗号文位置の索引
        std::unordered_map<std::string, Entry> m_entries;
        // 暗号データ領域の開始位置
        std::uint64_t m_payloadStart{};
    };
}
