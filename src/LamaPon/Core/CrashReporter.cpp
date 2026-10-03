#include "LamaPon/Core/CrashReporter.h"
#include "LamaPon/Core/BuildInfo.h"
#include "LamaPon/Core/Version.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <system_error>

namespace
{
    // 診断文書とダンプの保存先
    std::filesystem::path g_outputDirectory;
    // 診断ファイル名に使うアプリ名
    std::string g_applicationName{ "LamaPon" };
    // 登録前の未処理例外フィルター
    LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter{};
    // 例外フィルターを登録済みか
    bool g_installed{};

    // アプリ名をファイル名に使える文字へ整えます(value: アプリ名の複製)。
    std::string SanitizeName(std::string value)
    {
        // ファイル名に使えない文字かを判定します(character: アプリ名の各文字)。
        std::ranges::replace_if(
            value,
            [](const char character)
            {
                return !((character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '-'
                    || character == '_');
            },
            '_');
        return value.empty() ? "LamaPon" : value;
    }

    // UTC時刻とプロセス・スレッド番号を含む診断ファイルの拡張子前のパスを返します。
    std::filesystem::path ReportStem()
    {
        // 診断名に付ける現在のUTC日時
        SYSTEMTIME time{};
        GetSystemTime(&time);
        // 診断ファイル名を組み立てる領域
        std::array<char, 128> value{};
        std::snprintf(
            value.data(),
            value.size(),
            "%s-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%lu",
            g_applicationName.c_str(),
            time.wYear,
            time.wMonth,
            time.wDay,
            time.wHour,
            time.wMinute,
            time.wSecond,
            time.wMilliseconds,
            GetCurrentProcessId(),
            GetCurrentThreadId());
        return g_outputDirectory / value.data();
    }

    // 診断文書を出力します(path: 保存先, information: SEH例外情報、なければnull, reason: 診断理由)。
    bool WriteTextFile(
        const std::filesystem::path& path,
        const EXCEPTION_POINTERS* information,
        const std::string_view reason) noexcept
    {
        // 診断文書を書き込むハンドル
        const HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        // 診断文書の固定長出力領域
        std::array<char, 2048> text{};
        // SEH例外コード、情報なしは0
        const DWORD exceptionCode =
            information != nullptr
                && information->ExceptionRecord != nullptr
                ? information->ExceptionRecord->ExceptionCode
                : 0;
        // SEH例外の発生アドレス
        const auto exceptionAddress =
            information != nullptr
                && information->ExceptionRecord != nullptr
                ? reinterpret_cast<std::uintptr_t>(
                    information->ExceptionRecord->
                        ExceptionAddress)
                : 0;
        // 実行バイナリのビルド情報
        // 例外処理中の割り当てを避けるため、埋め込み済みの文字列を直接書きます。
        const auto& build = LamaPon::GetBuildInfo();
        // 整形結果の必要バイト数
        const int length = std::snprintf(
            text.data(),
            text.size(),
            "LamaPon crash report\n"
            "Application: %s\n"
            "Branch: %.*s\n"
            "Commit: %.*s%s\n"
            "Commit subject: %.*s\n"
            "Compatibility version: %.*s\n"
            "Process: %lu\n"
            "Thread: %lu\n"
            "Exception code: 0x%08lX\n"
            "Exception address: 0x%llX\n"
            "Reason: %.*s\n",
            g_applicationName.c_str(),
            static_cast<int>(build.branch.size()),
            build.branch.data(),
            static_cast<int>(build.commitFull.size()),
            build.commitFull.data(),
            build.dirty ? " (uncommitted changes)" : "",
            static_cast<int>(
                std::min<std::size_t>(
                    build.commitSubject.size(),
                    256)),
            build.commitSubject.data(),
            static_cast<int>(
                LamaPon::VersionString.size()),
            LamaPon::VersionString.data(),
            GetCurrentProcessId(),
            GetCurrentThreadId(),
            exceptionCode,
            static_cast<unsigned long long>(
                exceptionAddress),
            static_cast<int>(
                std::min<std::size_t>(
                    reason.size(),
                    1024)),
            reason.data());
        // ファイルへ書き込めたバイト数
        DWORD written{};
        // 文書の整形と書き込みの成功可否
        const bool succeeded =
            length > 0
            && WriteFile(
                file,
                text.data(),
                static_cast<DWORD>(
                    std::min<std::size_t>(
                        static_cast<std::size_t>(length),
                        text.size())),
                &written,
                nullptr);
        CloseHandle(file);
        return succeeded;
    }

    // 診断文書と必要ならダンプを出力します(information: SEH例外情報、文書だけならnull, reason: 診断理由)。
    bool WriteReport(
        EXCEPTION_POINTERS* information,
        const std::string_view reason) noexcept
    {
        try
        {
            // 診断フォルダー作成時のエラー
            std::error_code error;
            std::filesystem::create_directories(
                g_outputDirectory,
                error);
            // 診断文書とダンプの共通パス
            const auto stem = ReportStem();
            // 診断文書を保存できたか
            const bool wroteText = WriteTextFile(
                stem.wstring() + L".txt",
                information,
                reason);

            if (information == nullptr)
            {
                return wroteText;
            }

            // ダンプを書き込むハンドル
            const HANDLE dump = CreateFileW(
                (stem.wstring() + L".dmp").c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (dump == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            // ダンプへ渡す発生スレッドと例外
            MINIDUMP_EXCEPTION_INFORMATION dumpInformation{};
            dumpInformation.ThreadId = GetCurrentThreadId();
            dumpInformation.ExceptionPointers = information;
            dumpInformation.ClientPointers = FALSE;
            // ダンプを保存できたか
            const bool wroteDump = MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                dump,
                static_cast<MINIDUMP_TYPE>(
                    MiniDumpWithDataSegs
                    | MiniDumpWithHandleData
                    | MiniDumpWithThreadInfo
                    | MiniDumpWithUnloadedModules),
                &dumpInformation,
                nullptr,
                nullptr);
            CloseHandle(dump);
            return wroteText && wroteDump;
        }
        catch (...)
        {
            return false;
        }
    }

    // 未処理のSEH例外を記録しプロセスを終了させます(information: 発生した例外情報)。
    LONG WINAPI HandleUnhandledException(
        EXCEPTION_POINTERS* information)
    {
        WriteReport(information, "Unhandled SEH exception");
        return EXCEPTION_EXECUTE_HANDLER;
    }
}

namespace LamaPon
{
    void CrashReporter::Install(
        std::filesystem::path outputDirectory,
        std::string applicationName)
    {
        if (outputDirectory.empty())
        {
            outputDirectory =
                std::filesystem::current_path() / L"Crashes";
        }
        // 診断フォルダー作成時のエラー
        std::error_code error;
        std::filesystem::create_directories(
            outputDirectory,
            error);
        g_outputDirectory = std::move(outputDirectory);
        g_applicationName =
            SanitizeName(std::move(applicationName));
        if (!g_installed)
        {
            g_previousFilter =
                SetUnhandledExceptionFilter(
                    &HandleUnhandledException);
            g_installed = true;
        }
    }

    void CrashReporter::Uninstall() noexcept
    {
        if (!g_installed)
        {
            return;
        }
        SetUnhandledExceptionFilter(g_previousFilter);
        g_previousFilter = nullptr;
        g_installed = false;
    }

    bool CrashReporter::IsInstalled() noexcept
    {
        return g_installed;
    }

    bool CrashReporter::WriteDiagnostic(
        const std::string_view reason) noexcept
    {
        return WriteReport(nullptr, reason);
    }
}
