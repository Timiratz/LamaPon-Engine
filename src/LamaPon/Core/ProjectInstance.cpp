#include "LamaPon/Core/ProjectInstance.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <stdexcept>
#include <system_error>

namespace
{
    // プロジェクトパスの比較キーを返します(projectRoot: プロジェクトのルート)。
    std::wstring ProjectComparisonKey(
        const std::filesystem::path& projectRoot)
    {
        // パスの解決エラー
        std::error_code error;
        // 正規化するプロジェクト絶対パス
        auto normalized = std::filesystem::absolute(
            projectRoot,
            error);
        if (error)
        {
            throw std::system_error(
                error,
                "Could not resolve the project folder.");
        }

        // 実在部分のリンクを解決したパス
        const auto canonical = std::filesystem::weakly_canonical(
            normalized,
            error);
        if (!error)
        {
            normalized = canonical;
        }
        normalized = normalized.lexically_normal();

        // 大文字小文字を統一する比較キー
        auto key = normalized.native();
        // 文字を小文字へ変換します(value: パスの各文字)。
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

    // UTF-16の各バイトからFNV-1aを求めます(value: 正規化済みパスの比較キー)。
    std::uint64_t StablePathHash(const std::wstring& value) noexcept
    {
        // パスを識別するFNV-1aハッシュ
        std::uint64_t hash = 14695981039346656037ull;
        // ハッシュに加える各文字
        for (const wchar_t character : value)
        {
            // 文字のUTF-16コード単位
            const auto code = static_cast<std::uint16_t>(character);
            hash ^= static_cast<std::uint8_t>(code & 0xffu);
            hash *= 1099511628211ull;
            hash ^= static_cast<std::uint8_t>((code >> 8u) & 0xffu);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    // プロジェクトの起動目印の名前を返します(projectRoot: プロジェクトのルート)。
    std::wstring ProjectMutexName(
        const std::filesystem::path& projectRoot)
    {
        return L"Local\\LamaPon.Editor.Project."
            + std::to_wstring(
                StablePathHash(ProjectComparisonKey(projectRoot)));
    }

    // Win32エラーから例外を構築します(error: エラー番号, message: 操作の説明)。
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
        // プロジェクト固有の起動目印名
        const auto name = ProjectMutexName(projectRoot);
        // 作成した起動目印のハンドル
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
        // プロジェクト固有の起動目印名
        const auto name = ProjectMutexName(projectRoot);
        // 検査用に開いた起動目印
        const HANDLE handle = OpenMutexW(
            SYNCHRONIZE,
            FALSE,
            name.c_str());
        if (handle != nullptr)
        {
            CloseHandle(handle);
            return true;
        }

        // 目印を開けなかった理由
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
