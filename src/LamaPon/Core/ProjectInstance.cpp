#include "LamaPon/Core/ProjectInstance.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <stdexcept>
#include <system_error>

namespace
{
    std::wstring ProjectComparisonKey(
        const std::filesystem::path& projectRoot)
    {
        std::error_code error;
        auto normalized = std::filesystem::absolute(
            projectRoot,
            error);
        if (error)
        {
            throw std::system_error(
                error,
                "Could not resolve the project folder.");
        }

        const auto canonical = std::filesystem::weakly_canonical(
            normalized,
            error);
        if (!error)
        {
            normalized = canonical;
        }
        normalized = normalized.lexically_normal();

        auto key = normalized.native();
        std::transform(
            key.begin(),
            key.end(),
            key.begin(),
            [](const wchar_t value)
            {
                return static_cast<wchar_t>(std::towlower(value));
            });
        return key;
    }

    std::uint64_t StablePathHash(const std::wstring& value) noexcept
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const wchar_t character : value)
        {
            const auto code = static_cast<std::uint16_t>(character);
            hash ^= static_cast<std::uint8_t>(code & 0xffu);
            hash *= 1099511628211ull;
            hash ^= static_cast<std::uint8_t>((code >> 8u) & 0xffu);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::wstring ProjectMutexName(
        const std::filesystem::path& projectRoot)
    {
        return L"Local\\LamaPon.Editor.Project."
            + std::to_wstring(
                StablePathHash(ProjectComparisonKey(projectRoot)));
    }

    std::system_error WindowsError(
        const DWORD error,
        const char* message)
    {
        return std::system_error(
            static_cast<int>(error),
            std::system_category(),
            message);
    }
}

namespace LamaPon
{
    ProjectInstanceLock::ProjectInstanceLock(
        const std::filesystem::path& projectRoot)
    {
        const auto name = ProjectMutexName(projectRoot);
        const HANDLE handle = CreateMutexW(
            nullptr,
            FALSE,
            name.c_str());
        if (handle == nullptr)
        {
            throw WindowsError(
                GetLastError(),
                "Could not create the project instance lock.");
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(handle);
            return;
        }
        m_handle = handle;
    }

    ProjectInstanceLock::~ProjectInstanceLock()
    {
        Release();
    }

    void ProjectInstanceLock::Release() noexcept
    {
        if (m_handle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(m_handle));
            m_handle = nullptr;
        }
    }

    bool IsProjectEditorOpen(
        const std::filesystem::path& projectRoot)
    {
        const auto name = ProjectMutexName(projectRoot);
        const HANDLE handle = OpenMutexW(
            SYNCHRONIZE,
            FALSE,
            name.c_str());
        if (handle != nullptr)
        {
            CloseHandle(handle);
            return true;
        }

        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
        {
            return false;
        }
        throw WindowsError(
            error,
            "Could not inspect the project instance lock.");
    }
}
