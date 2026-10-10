#pragma once

#include "LamaPon/Core/Unicode.h"

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace LamaPon
{
    // ディレクトリを作成または確認します(path: 作成先, error: 失敗時のエラー出力)。
    // 空のパスは成功とし、WebDAVの作成エラーは実在を再確認して判定します。
    inline bool EnsureDirectoryExists(
        const std::filesystem::path& path,
        std::error_code& error)
    {
        error.clear();
        if (path.empty())
        {
            return true;
        }

        // 作成先が既に存在するか
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

        // WebDAVでは「作成済みだがエラー」という結果があるため、元のエラーを保持したまま一度だけ実在を再確認します。
        // 作成時に返されたエラー
        const auto creationError = error;
        // 実在確認時のエラー
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

    // UNCパスかを返します(path: 判定対象のパス)。
    // 拡張長ローカルパス（\\?\C:\...）やデバイスパス（\\.\...）は先頭が2区切りでもネットワークではありません。
    inline bool IsUncPath(const std::filesystem::path& path) noexcept
    {
#if defined(_WIN32)
        // 区切りを統一するパス文字列
        auto value = path.native();
        // パス内の各文字
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
#else
        // POSIXの二重スラッシュをWindows UNCとして扱わない。
        static_cast<void>(path);
        return false;
#endif
    }

    // ネットワーク上のパスかを返します(path: 判定対象のパス)。
    // UNCとリモートドライブを検出し、パスを解決できない場合はfalseを返します。
    inline bool UsesNetworkDrive(
        const std::filesystem::path& path) noexcept
    {
#if defined(_WIN32)
        try
        {
            // 判定対象の絶対パス
            const auto absolute = std::filesystem::absolute(path);
            if (IsUncPath(absolute))
            {
                return true;
            }
            // ドライブ種別を調べるルート
            const auto root = absolute.root_path();
            return !root.empty()
                && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
        }
        catch (...)
        {
            return false;
        }
#else
        // POSIX mount種別の検出は別途必要。ここではWindowsのdrive分類を使用しない。
        static_cast<void>(path);
        return false;
#endif
    }

    // ApplicationのcacheDirをLamaPonのユーザーキャッシュ用ベースへ変換します。
    // AndroidのcacheDirはActivityから注入し、ファイルシステムには触れません。
    inline std::filesystem::path AndroidApplicationCachePath(
        const std::filesystem::path& cacheDirectory,
        const wchar_t* subfolder)
    {
        if (cacheDirectory.empty() || !cacheDirectory.is_absolute())
        {
            throw std::invalid_argument("Android cache directory must be absolute");
        }
        const auto name = Detail::CodeUnitsToUtf8<wchar_t>(subfolder);
        const std::u8string utf8Name(name.begin(), name.end());
        return cacheDirectory / "LamaPon" / std::filesystem::path(utf8Name);
    }

    namespace PathUtilsDetail
    {
        inline std::filesystem::path& AndroidApplicationCacheDirectory() noexcept
        {
            static std::filesystem::path directory;
            return directory;
        }
    }

    // Androidランタイムの起動時にActivityから得たcacheDirを登録します。
    inline void SetAndroidApplicationCacheDirectory(std::filesystem::path directory)
    {
        if (directory.empty() || !directory.is_absolute())
        {
            throw std::invalid_argument("Android cache directory must be absolute");
        }
        PathUtilsDetail::AndroidApplicationCacheDirectory() = std::move(directory);
    }

    // パスに対応するキャッシュキーを返します(path: キャッシュ元のパス)。
    // 絶対パスを正規化したFNV-1aの16桁キー。WindowsだけASCIIの大文字小文字を同一視します。
    inline std::wstring PathCacheKey(const std::filesystem::path& path)
    {
        // パス正規化時のエラー
        std::error_code error;
        // 正規化した絶対パス文字列
        auto normalized = std::filesystem::weakly_canonical(
            std::filesystem::absolute(path, error), error).native();
        // パスのFNV-1aハッシュ値
        std::uint64_t hash = 14695981039346656037ull;
        // ハッシュに加える各文字
        for (const auto character : normalized)
        {
#if defined(_WIN32)
            // ASCII大文字を小文字にした値
            const auto folded = static_cast<std::uint64_t>(
                character >= L'A' && character <= L'Z'
                    ? character - L'A' + L'a'
                    : character);
#else
            // Linuxのファイル名は大文字小文字を区別し、UTF-8のバイトを符号なしで扱う。
            const auto folded = static_cast<std::uint64_t>(
                static_cast<unsigned char>(character));
#endif
            hash ^= folded;
            hash *= 1099511628211ull;
        }
        // 16進表記のキーと終端文字
        std::wstring result(16, L'0');
        constexpr std::wstring_view digits = L"0123456789abcdef";
        for (std::size_t index = result.size(); index > 0; --index)
        {
            result[index - 1] = digits[hash & 0xf];
            hash >>= 4;
        }
        return result;
    }

    // ユーザー用キャッシュのパスを返します(subfolder: LamaPon配下のサブフォルダー名)。
    // LOCALAPPDATAを優先し、取得できない場合はOSの一時領域を使います。
    inline std::filesystem::path LocalEngineCachePath(
        const wchar_t* subfolder)
    {
#if defined(_WIN32)
        // ユーザー用データ領域のパス
        std::wstring localAppData(32768, L'\0');
        // 環境変数から読み取った文字数
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
        // 一時領域の取得エラー
        std::error_code error;
        // 代替キャッシュの親ディレクトリ
        auto temporary =
            std::filesystem::temp_directory_path(error);
        if (error)
        {
            temporary = L"C:\\Windows\\Temp";
        }
        return temporary / L"LamaPon" / subfolder;
#elif defined(__ANDROID__)
        // HOMEや/tmpを流用せず、実行中Activityのアプリ専用cacheDirを使います。
        const auto& directory = PathUtilsDetail::AndroidApplicationCacheDirectory();
        if (directory.empty())
        {
            throw std::logic_error("Android cache directory must be supplied by the application");
        }
        return AndroidApplicationCachePath(directory, subfolder);
#else
        std::filesystem::path root;
        if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache && *cache)
        {
            const std::filesystem::path candidate(cache);
            if (candidate.is_absolute()) root = candidate;
        }
        if (root.empty())
        {
            if (const char* home = std::getenv("HOME"); home && *home)
            {
                const std::filesystem::path candidate(home);
                if (candidate.is_absolute()) root = candidate / ".cache";
            }
        }
        if (root.empty()) root = std::filesystem::temp_directory_path();
        // POSIXではpath(wstring)の変換をlocaleに依存させない。
        const auto name = Detail::CodeUnitsToUtf8<wchar_t>(subfolder);
        const std::u8string utf8Name(name.begin(), name.end());
        return root / "LamaPon" / std::filesystem::path(utf8Name);
#endif
    }

    // 実行ファイルの親ディレクトリを返し、パス取得に失敗した場合は空を返します。
    inline std::filesystem::path ExecutableDirectory()
    {
#if defined(_WIN32)
        // 実行ファイルの絶対パス
        std::wstring path(32768, L'\0');
        // 取得したパスの文字数
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
#elif defined(__linux__)
        // Androidではホストプロセスの場所を返す。ゲームのasset rootとして使用しない。
        std::string path(256, '\0');
        while (path.size() <= 1024 * 1024)
        {
            const auto length = readlink("/proc/self/exe", path.data(), path.size());
            if (length < 0) return {};
            if (static_cast<std::size_t>(length) < path.size())
            {
                path.resize(static_cast<std::size_t>(length));
                return std::filesystem::path(path).parent_path();
            }
            path.resize(path.size() * 2);
        }
        return {};
#else
        return {};
#endif
    }

    // UTF-8をWindowsではUTF-16、Linux/AndroidではUTF-32へ変換する(value: UTF-8文字列)。
    // 空文字列または不正な入力には空を返す。
    inline std::wstring Utf8ToWide(const std::string_view value)
    {
        return Detail::Utf8ToCodeUnits<wchar_t>(value);
    }

    // wchar_t文字列をUTF-8へ変換し、不正な入力には空を返す(value: UTF-16またはUTF-32文字列)。
    inline std::string WideToUtf8(const std::wstring_view value)
    {
        return Detail::CodeUnitsToUtf8<wchar_t>(value);
    }

    // UTF-8文字列をパスへ変換します(value: UTF-8のパス文字列)。
    inline std::filesystem::path PathFromUtf8(const std::string_view value)
    {
        if (value.empty()) return {};
        // パス構築用のUTF-8文字列
        const std::u8string utf8{
            reinterpret_cast<const char8_t*>(value.data()),
            reinterpret_cast<const char8_t*>(value.data() + value.size())
        };
        return std::filesystem::path(utf8);
    }

    // パスを区切り文字がスラッシュのUTF-8文字列へ変換します(path: 変換対象のパス)。
    inline std::string PathToUtf8(const std::filesystem::path& path)
    {
        // 汎用形式のUTF-8パス文字列
        const std::u8string utf8 = path.generic_u8string();
        return {
            reinterpret_cast<const char*>(utf8.data()),
            utf8.size()
        };
    }
}
