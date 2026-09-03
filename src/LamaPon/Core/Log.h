#pragma once

#include "LamaPon/Core/Api.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace LamaPon
{
    enum class LogLevel
    {
        Info,
        Warning,
        Error
    };

    struct LogEntry final
    {
        std::uint64_t sequence{};
        std::chrono::system_clock::time_point
            timestamp;
        LogLevel level{ LogLevel::Info };
        std::string message;
        std::string sourceFile;
        std::uint32_t sourceLine{};
        std::uint64_t gameObjectId{};
    };

    [[nodiscard]] std::string_view
        LogLevelName(LogLevel level) noexcept;

    class Logger final
    {
    public:
        [[nodiscard]] static LAMAPON_API Logger&
            Instance() noexcept;

        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        LAMAPON_API void Write(
            LogLevel level,
            std::string message,
            std::uint64_t gameObjectId = 0,
            const std::source_location&
                location =
                    std::source_location::
                        current());
        void Info(
            std::string message,
            std::uint64_t gameObjectId = 0,
            const std::source_location&
                location =
                    std::source_location::
                        current())
        {
            Write(
                LogLevel::Info,
                std::move(message),
                gameObjectId,
                location);
        }
        void Warning(
            std::string message,
            std::uint64_t gameObjectId = 0,
            const std::source_location&
                location =
                    std::source_location::
                        current())
        {
            Write(
                LogLevel::Warning,
                std::move(message),
                gameObjectId,
                location);
        }
        void Error(
            std::string message,
            std::uint64_t gameObjectId = 0,
            const std::source_location&
                location =
                    std::source_location::
                        current())
        {
            Write(
                LogLevel::Error,
                std::move(message),
                gameObjectId,
                location);
        }

        [[nodiscard]] LAMAPON_API std::vector<LogEntry>
            Snapshot() const;
        LAMAPON_API void Clear() noexcept;
        LAMAPON_API void SetCapacity(
            std::size_t capacity) noexcept;
        [[nodiscard]] LAMAPON_API std::size_t
            Capacity() const noexcept;

        [[nodiscard]] LAMAPON_API bool SetFilePath(
            const std::filesystem::path& path,
            bool truncate = true) noexcept;
        [[nodiscard]] LAMAPON_API std::filesystem::path
            FilePath() const;
        LAMAPON_API void CloseFile() noexcept;

    private:
        Logger() = default;

        mutable std::mutex m_mutex;
        std::deque<LogEntry> m_entries;
        std::ofstream m_file;
        std::filesystem::path m_filePath;
        std::size_t m_capacity{ 4096 };
        std::uint64_t m_nextSequence{ 1 };
    };
}
