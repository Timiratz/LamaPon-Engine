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
        // 通常の診断情報
        Info,
        // 動作を継続できる警告
        Warning,
        // 処理の失敗や異常
        Error
    };

    struct LogEntry final
    {
        // 記録順を示す通し番号
        std::uint64_t sequence{};
        // 記録時点のシステム時刻
        std::chrono::system_clock::time_point
            timestamp;
        // 診断の重要度
        LogLevel level{ LogLevel::Info };
        // 診断メッセージ本文
        std::string message;
        // 記録元のソースファイル名
        std::string sourceFile;
        // 記録元のソース行番号
        std::uint32_t sourceLine{};
        // 関連オブジェクトID、0はなし
        std::uint64_t gameObjectId{};
    };

    // 診断レベルの表示名を返します(level: 診断の重要度)。
    [[nodiscard]] std::string_view
        LogLevelName(LogLevel level) noexcept;

    class Logger final
    {
    public:
        // プロセスで共有するロガーを返します。
        [[nodiscard]] static LAMAPON_API Logger&
            Instance() noexcept;

        // 共有ロガーの複製を禁止します。
        Logger(const Logger&) = delete;
        // 共有ロガーのコピー代入を禁止します。
        Logger& operator=(const Logger&) = delete;

        // 診断を記録します(level: 重要度, message: 本文, gameObjectId: 関連ID、0はなし, location: 呼び出し位置)。
        // 記録は排他制御し、ファイル出力が有効なら毎回フラッシュします。
        LAMAPON_API void Write(
            LogLevel level,
            std::string message,
            std::uint64_t gameObjectId = 0,
            const std::source_location&
                location =
                    std::source_location::
                        current());
        // 情報を記録します(message: 本文, gameObjectId: 関連ID、0はなし, location: 呼び出し位置)。
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
        // 警告を記録します(message: 本文, gameObjectId: 関連ID、0はなし, location: 呼び出し位置)。
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
        // エラーを記録します(message: 本文, gameObjectId: 関連ID、0はなし, location: 呼び出し位置)。
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

        // 現在保持する診断を記録順で複製して返します。
        [[nodiscard]] LAMAPON_API std::vector<LogEntry>
            Snapshot() const;
        // メモリー内の診断を消去します。
        LAMAPON_API void Clear() noexcept;
        // 診断の保持上限を64～100000件に設定します(capacity: 保持件数)。
        LAMAPON_API void SetCapacity(
            std::size_t capacity) noexcept;
        // メモリー内の診断の保持上限を返します。
        [[nodiscard]] LAMAPON_API std::size_t
            Capacity() const noexcept;

        // 診断ファイルを開き成功可否を返します(path: 出力先, truncate: 既存内容を消去するか)。
        // 切り替え時に以前のファイルを閉じ、失敗時はファイル出力を無効にします。
        [[nodiscard]] LAMAPON_API bool SetFilePath(
            const std::filesystem::path& path,
            bool truncate = true) noexcept;
        // 現在の診断ファイルのパスを返します。
        [[nodiscard]] LAMAPON_API std::filesystem::path
            FilePath() const;
        // 診断ファイルを閉じてファイル出力を無効にします。
        LAMAPON_API void CloseFile() noexcept;

    private:
        // 共有ロガーの初期状態を構築します。
        Logger() = default;

        // 記録と出力設定の排他制御
        mutable std::mutex m_mutex;
        // 記録順で保持する診断
        std::deque<LogEntry> m_entries;
        // 診断を書き込むファイル
        std::ofstream m_file;
        // 開いている診断ファイルのパス
        std::filesystem::path m_filePath;
        // メモリーに保持する最大件数
        std::size_t m_capacity{ 4096 };
        // 次の診断へ付ける通し番号
        std::uint64_t m_nextSequence{ 1 };
    };
}
