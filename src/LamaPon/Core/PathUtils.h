#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace LamaPon
{
    // WindowsのWebDAVリダイレクターは、ディレクトリの作成自体には
    // 成功していてもERROR_NOT_SUPPORTEDを返すことがあります。
    // 作成後に実在を確認することで、通常のローカルパスと共有ドライブを
    // 同じ呼び出し側で安全に扱えるようにします。
    inline bool EnsureDirectoryExists(
        const std::filesystem::path& path,
        std::error_code& error)
    {
        error.clear();
        if (path.empty())
        {
            return true;
        }

        const bool alreadyExists =
            std::filesystem::is_directory(path, error);
        if (alreadyExists)
        {
            error.clear();
            return true;
        }
        if (error
            && error != std::errc::no_such_file_or_directory)
        {
            return false;
        }

        error.clear();
        std::filesystem::create_directories(path, error);
        if (!error)
        {
            return true;
        }

        // WebDAVでは「作成済みだがエラー」という結果があるため、
        // 元のエラーを保持したまま一度だけ実在を再確認します。
        const auto creationError = error;
        std::error_code verificationError;
        if (std::filesystem::is_directory(
                path,
                verificationError))
        {
            error.clear();
            return true;
        }
        error = creationError;
        return false;
    }

    // UNCパス（\\server\share...）の判定。拡張長ローカルパス（\\?\C:\...）や
    // デバイスパス（\\.\...）は先頭が2区切りでもネットワークではありません。
    inline bool IsUncPath(const std::filesystem::path& path) noexcept
    {
        auto value = path.native();
        for (wchar_t& character : value)
        {
            if (character == L'/')
            {
                character = L'\\';
            }
        }
        if (value.starts_with(L"\\\\?\\UNC\\")
            || value.starts_with(L"\\\\?\\unc\\"))
        {
            return true;
        }
        if (value.starts_with(L"\\\\?\\")
            || value.starts_with(L"\\\\.\\"))
        {
            return false;
        }
        return value.starts_with(L"\\\\");
    }

    // UNC、割り当てドライブ、WebDAVを判定します。共有ドライブ上で
    // ファイル操作やDLL読み込みを行えない場合に、呼び出し側が
    // ユーザーのローカル領域へ切り替えるために使います。
    inline bool UsesNetworkDrive(
        const std::filesystem::path& path) noexcept
    {
        try
        {
            const auto absolute = std::filesystem::absolute(path);
            if (IsUncPath(absolute))
            {
                return true;
            }
            const auto root = absolute.root_path();
            return !root.empty()
                && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
        }
        catch (...)
        {
            return false;
        }
    }

    // パスからユーザー単位キャッシュのサブフォルダー名を作ります。
    // 大文字小文字を畳んだFNV-1aなので、同じ場所なら常に同じ鍵になり、
    // 別プロジェクトの成果物と混ざりません（Build Cacheと同じ方式）。
    inline std::wstring PathCacheKey(const std::filesystem::path& path)
    {
        std::error_code error;
        auto normalized = std::filesystem::weakly_canonical(
            std::filesystem::absolute(path, error), error).native();
        std::uint64_t hash = 14695981039346656037ull;
        for (const wchar_t character : normalized)
        {
            const auto folded = static_cast<std::uint64_t>(
                character >= L'A' && character <= L'Z'
                    ? character - L'A' + L'a'
                    : character);
            hash ^= folded;
            hash *= 1099511628211ull;
        }
        wchar_t buffer[17];
        swprintf_s(buffer, L"%016llx", hash);
        return buffer;
    }

    // %LOCALAPPDATA%\LamaPon\<subfolder>。エンジンを更新しても残る
    // ユーザー単位の作業領域（Build Cacheと同じ置き場）。LOCALAPPDATAが
    // 引けない環境（サービス等）では一時フォルダーへ落とします。
    inline std::filesystem::path LocalEngineCachePath(
        const wchar_t* subfolder)
    {
        std::wstring localAppData(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        if (length != 0 && length < localAppData.size())
        {
            localAppData.resize(length);
            return std::filesystem::path(localAppData)
                / L"LamaPon"
                / subfolder;
        }
        std::error_code error;
        auto temporary =
            std::filesystem::temp_directory_path(error);
        if (error)
        {
            temporary = L"C:\\Windows\\Temp";
        }
        return temporary / L"LamaPon" / subfolder;
    }

    inline std::filesystem::path ExecutableDirectory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr,
            path.data(),
            static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    inline std::wstring Utf8ToWide(const std::string_view value)
    {
        if (value.empty())
        {
            return {};
        }

        const int length = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0);
        if (length <= 0)
        {
            return {};
        }

        std::wstring result(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length);
        return result;
    }

    inline std::string WideToUtf8(const std::wstring_view value)
    {
        if (value.empty())
        {
            return {};
        }

        const int length = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (length <= 0)
        {
            return {};
        }

        std::string result(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length,
            nullptr,
            nullptr);
        return result;
    }

    inline std::filesystem::path PathFromUtf8(const std::string_view value)
    {
        const std::u8string utf8{
            reinterpret_cast<const char8_t*>(value.data()),
            reinterpret_cast<const char8_t*>(value.data() + value.size())
        };
        return std::filesystem::path(utf8);
    }

    inline std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {
            reinterpret_cast<const char*>(utf8.data()),
            utf8.size()
        };
    }
}
