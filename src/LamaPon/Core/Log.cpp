#include "LamaPon/Core/Log.h"

#include "LamaPon/Core/PathUtils.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace
{
    // ローカル時刻をミリ秒付き文字列にします(time: 記録時刻)。
    std::string TimeText(
        const std::chrono::system_clock::
            time_point time)
    {
        // 秒未満のミリ秒部分
        const auto milliseconds =
            std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    time.time_since_epoch())
                % 1000;
        // ローカル時刻変換用の秒単位時刻
        const std::time_t raw =
            std::chrono::system_clock::
                to_time_t(time);
        // 日時のローカル時刻成分
        std::tm local{};
        localtime_s(&local, &raw);

        // 日時文字列を組み立てる出力
        std::ostringstream stream;
        stream
            << std::put_time(
                &local,
                "%Y-%m-%d %H:%M:%S")
            << '.'
            << std::setfill('0')
            << std::setw(3)
            << milliseconds.count();
        return stream.str();
    }
}

namespace LamaPon
{
    std::string_view LogLevelName(
        const LogLevel level) noexcept
    {
        switch (level)
        {
        case LogLevel::Warning:
            return "Warning";
        case LogLevel::Error:
            return "Error";
        default:
            return "Info";
        }
    }

    Logger& Logger::Instance() noexcept
    {
        // プロセスで共有するロガー
        static Logger logger;
        return logger;
    }

    void Logger::Write(
        const LogLevel level,
        std::string message,
        const std::uint64_t gameObjectId,
        const std::source_location&
            location)
    {
        // 今回記録する診断の内容
        LogEntry entry{
            {},
            std::chrono::system_clock::now(),
            level,
            std::move(message),
            location.file_name(),
            location.line(),
            gameObjectId
        };

        // 記録と出力設定を保護するロック
        std::scoped_lock lock(m_mutex);
        entry.sequence = m_nextSequence++;
        m_entries.push_back(entry);
        while (m_entries.size() > m_capacity)
        {
            m_entries.pop_front();
        }

        if (m_file)
        {
            m_file
                << TimeText(entry.timestamp)
                << " ["
                << LogLevelName(entry.level)
                << "] "
                << entry.message;
            if (entry.gameObjectId != 0)
            {
                m_file
                    << " [GameObject "
                    << entry.gameObjectId
                    << ']';
            }
            if (!entry.sourceFile.empty())
            {
                m_file
                    << " ("
                    << PathToUtf8(
                        std::filesystem::path(
                            entry.sourceFile)
                            .filename())
                    << ':'
                    << entry.sourceLine
                    << ')';
            }
            m_file << '\n';
            m_file.flush();
        }
    }

    std::vector<LogEntry>
        Logger::Snapshot() const
    {
        // 記録と出力設定を保護するロック
        std::scoped_lock lock(m_mutex);
        return {
            m_entries.begin(),
            m_entries.end()
        };
    }

    void Logger::Clear() noexcept
    {
        // 記録と出力設定を保護するロック
        std::scoped_lock lock(m_mutex);
        m_entries.clear();
    }

    void Logger::SetCapacity(
        const std::size_t capacity) noexcept
    {
        // 記録と出力設定を保護するロック
        std::scoped_lock lock(m_mutex);
        m_capacity =
            std::clamp<std::size_t>(
                capacity,
                64,
                100000);
        while (m_entries.size() > m_capacity)
        {
            m_entries.pop_front();
        }
    }

    std::size_t Logger::Capacity() const noexcept
    {
        // 記録と出力設定を保護するロック
        std::scoped_lock lock(m_mutex);
        return m_capacity;
    }

    bool Logger::SetFilePath(
        const std::filesystem::path& path,
        const bool truncate) noexcept
    {
        // 記録と出力設定を保護するロック
        std::scoped_lock lock(m_mutex);
        m_file.close();
        m_file.clear();
        m_filePath.clear();
        try
        {
            if (!path.parent_path().empty())
            {
                std::filesystem::
                    create_directories(
                        path.parent_path());
            }
            m_file.open(
                path,
                std::ios::binary
                    | std::ios::out
                    | (truncate
                        ? std::ios::trunc
                        : std::ios::app));
            if (!m_file)
            {
                return false;
            }
            m_filePath = path;
            return true;
        }
        catch (...)
        {
            m_file.close();
            m_filePath.clear();
            return false;
        }
    }

    std::filesystem::path
        Logger::FilePath() const
    {
        // 記録と出力設定を保護するロック
        std::scoped_lock lock(m_mutex);
        return m_filePath;
    }

    void Logger::CloseFile() noexcept
    {
        // 記録と出力設定を保護するロック
        std::scoped_lock lock(m_mutex);
        m_file.close();
        m_filePath.clear();
    }
}
