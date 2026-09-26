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
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::ostringstream text;
        text << std::put_time(&local, "%Y%m%d-%H%M%S");
        return text.str();
    }

    std::filesystem::path UniqueCapturePath(
        const std::filesystem::path& directory,
        const std::string& prefix,
        const std::string& extension)
    {
        const auto stem = prefix + "-" + LocalTimestamp();
        std::error_code error;
        auto candidate =
            directory
            / std::filesystem::path{ stem + extension };
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
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            // WebDAVなどでは作成済みでも失敗を返すことがあるため、
            // 最終的に存在するかで判断します。
            if (!std::filesystem::is_directory(directory, error))
            {
                return false;
            }
            const auto ignorePath = directory / ".gitignore";
            if (!std::filesystem::exists(ignorePath, error))
            {
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
            std::filesystem::path path;
            std::filesystem::file_time_type modified;
        };
        std::vector<Entry> entries;
        std::error_code error;
        std::filesystem::directory_iterator iterator(directory, error);
        if (error)
        {
            return {};
        }
        for (const auto& entry : iterator)
        {
            std::error_code entryError;
            if (!entry.is_regular_file(entryError)
                || entry.path().extension() != extension)
            {
                continue;
            }
            const auto modified = entry.last_write_time(entryError);
            entries.push_back({
                entry.path(),
                entryError ? std::filesystem::file_time_type{} : modified
            });
        }
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
        std::vector<std::filesystem::path> paths;
        paths.reserve(entries.size());
        for (auto& entry : entries)
        {
            paths.push_back(std::move(entry.path));
        }
        return paths;
    }
}
