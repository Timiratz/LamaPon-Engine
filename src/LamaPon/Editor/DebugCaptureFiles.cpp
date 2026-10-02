#include "LamaPon/Editor/DebugCaptureFiles.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace LamaPon::DebugCaptureFiles
{
    std::string LocalTimestamp()
    {
        // ファイル名に使う現在の現地時刻
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        // 現地時刻の年月日時分秒
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        // 時刻を整形するstream
        std::ostringstream text;
        text << std::put_time(&local, "%Y%m%d-%H%M%S");
        return text.str();
    }

    std::filesystem::path UniqueCapturePath(
        const std::filesystem::path& directory,
        const std::string& prefix,
        const std::string& extension)
    {
        // 接頭辞と時刻から作るファイル名
        const auto stem = prefix + "-" + LocalTimestamp();
        // ファイルシステム操作のエラー
        std::error_code error;
        // 重複を避ける保存パスの候補
        auto candidate =
            directory
            / std::filesystem::path{ stem + extension };
        // 同じ秒の名前衝突を避ける連番
        for (int suffix = 2;
            std::filesystem::exists(candidate, error)
            && suffix < 1000;
            ++suffix)
        {
            candidate =
                directory
                / std::filesystem::path{
                    stem + "-" + std::to_string(suffix) + extension };
        }
        return candidate;
    }

    bool EnsureCaptureDirectory(
        const std::filesystem::path& directory) noexcept
    {
        try
        {
            // ファイルシステム操作のエラー
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            // WebDAVなどでは作成済みでも失敗を返すことがあるため、最終的に存在するかで判断します。
            if (!std::filesystem::is_directory(directory, error))
            {
                return false;
            }
            // 記録を共有対象から外す設定パス
            const auto ignorePath = directory / ".gitignore";
            if (!std::filesystem::exists(ignorePath, error))
            {
                // 計測記録を共有対象から外す設定の出力
                std::ofstream ignore(
                    ignorePath,
                    std::ios::binary | std::ios::trunc);
                ignore
                    << "# 解析の記録は端末ごとの計測値なので"
                       "Gitで共有しません。\n*\n";
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::vector<std::filesystem::path> ListCaptures(
        const std::filesystem::path& directory,
        const std::string& extension)
    {
        struct Entry final
        {
            // 見つかった計測記録のパス
            std::filesystem::path path;
            // 記録の更新時刻・取得失敗時は0
            std::filesystem::file_time_type modified;
        };
        // 更新時刻付き計測記録の一覧
        std::vector<Entry> entries;
        // ファイルシステム操作のエラー
        std::error_code error;
        // 保存フォルダー直下の列挙
        std::filesystem::directory_iterator iterator(directory, error);
        if (error)
        {
            return {};
        }
        // 列挙中または返却する計測記録
        for (const auto& entry : iterator)
        {
            // 個別記録の情報取得エラー
            std::error_code entryError;
            if (!entry.is_regular_file(entryError)
                || entry.path().extension() != extension)
            {
                continue;
            }
            // 列挙した記録の更新時刻
            const auto modified = entry.last_write_time(entryError);
            entries.push_back({
                entry.path(),
                entryError ? std::filesystem::file_time_type{} : modified
            });
        }
        // 更新時刻の降順に並べ同時刻はパスの降順に揃える(left: 比較する記録, right: 比較対象の記録)。
        std::ranges::sort(
            entries,
            [](const Entry& left, const Entry& right)
            {
                if (left.modified != right.modified)
                {
                    return left.modified > right.modified;
                }
                return left.path > right.path;
            });
        // 新しい順の記録パスの返却先
        std::vector<std::filesystem::path> paths;
        paths.reserve(entries.size());
        // 列挙中または返却する計測記録
        for (auto& entry : entries)
        {
            paths.push_back(std::move(entry.path));
        }
        return paths;
    }
}
