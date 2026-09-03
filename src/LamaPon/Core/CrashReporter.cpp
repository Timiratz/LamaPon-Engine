#include "LamaPon/Core/CrashReporter.h"
#include "LamaPon/Core/Version.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <system_error>

namespace
{
    std::filesystem::path g_outputDirectory;
    std::string g_applicationName{ "LamaPon" };
    LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter{};
    bool g_installed{};

    std::string SanitizeName(std::string value)
    {
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

    std::filesystem::path ReportStem()
    {
        SYSTEMTIME time{};
        GetSystemTime(&time);
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

    bool WriteTextFile(
        const std::filesystem::path& path,
        const EXCEPTION_POINTERS* information,
        const std::string_view reason) noexcept
    {
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

        std::array<char, 2048> text{};
        const DWORD exceptionCode =
            information != nullptr
                && information->ExceptionRecord != nullptr
                ? information->ExceptionRecord->ExceptionCode
                : 0;
        const auto exceptionAddress =
            information != nullptr
                && information->ExceptionRecord != nullptr
                ? reinterpret_cast<std::uintptr_t>(
                    information->ExceptionRecord->
                        ExceptionAddress)
                : 0;
        const int length = std::snprintf(
            text.data(),
            text.size(),
            "LamaPon crash report\n"
            "Application: %s\n"
            "Engine version: %.*s\n"
            "Build revision: %.*s\n"
            "Process: %lu\n"
            "Thread: %lu\n"
            "Exception code: 0x%08lX\n"
            "Exception address: 0x%llX\n"
            "Reason: %.*s\n",
            g_applicationName.c_str(),
            static_cast<int>(
                LamaPon::VersionString.size()),
            LamaPon::VersionString.data(),
            static_cast<int>(
                LamaPon::BuildRevision.size()),
            LamaPon::BuildRevision.data(),
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
        DWORD written{};
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

    bool WriteReport(
        EXCEPTION_POINTERS* information,
        const std::string_view reason) noexcept
    {
        try
        {
            std::error_code error;
            std::filesystem::create_directories(
                g_outputDirectory,
                error);
            const auto stem = ReportStem();
            const bool wroteText = WriteTextFile(
                stem.wstring() + L".txt",
                information,
                reason);

            if (information == nullptr)
            {
                return wroteText;
            }

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

            MINIDUMP_EXCEPTION_INFORMATION dumpInformation{};
            dumpInformation.ThreadId = GetCurrentThreadId();
            dumpInformation.ExceptionPointers = information;
            dumpInformation.ClientPointers = FALSE;
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
