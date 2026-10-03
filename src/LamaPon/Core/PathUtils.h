#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

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
    }

    // ネットワーク上のパスかを返します(path: 判定対象のパス)。
    // UNCとリモートドライブを検出し、パスを解決できない場合はfalseを返します。
    inline bool UsesNetworkDrive(
        const std::filesystem::path& path) noexcept
    {
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
    }

    // パスに対応するキャッシュキーを返します(path: キャッシュ元のパス)。
    // 絶対パスを正規化し、ASCIIの大文字小文字を同一視するFNV-1aで16桁のキーを作ります。
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
        for (const wchar_t character : normalized)
        {
            // ASCII大文字を小文字にした値
            const auto folded = static_cast<std::uint64_t>(
                character >= L'A' && character <= L'Z'
                    ? character - L'A' + L'a'
                    : character);
            hash ^= folded;
            hash *= 1099511628211ull;
        }
        // 16進表記のキーと終端文字
        wchar_t buffer[17];
        swprintf_s(buffer, L"%016llx", hash);
        return buffer;
    }

    // ユーザー用キャッシュのパスを返します(subfolder: LamaPon配下のサブフォルダー名)。
    // LOCALAPPDATAを優先し、取得できない場合はOSの一時領域を使います。
    inline std::filesystem::path LocalEngineCachePath(
        const wchar_t* subfolder)
    {
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
    }

    // 実行ファイルの親ディレクトリを返し、パス取得に失敗した場合は空を返します。
    inline std::filesystem::path ExecutableDirectory()
    {
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
    }

    // UTF-8をUTF-16へ変換します(value: 変換する文字列)。
    // 空文字列または不正なUTF-8には空を返します。
    inline std::wstring Utf8ToWide(const std::string_view value)
    {
        if (value.empty())
        {
            return {};
        }

        // 変換後のUTF-16コード単位数
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

        // 変換後のUTF-16文字列
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

    // UTF-16をUTF-8へ変換します(value: 変換する文字列)。
    // 空文字列または不正なUTF-16には空を返します。
    inline std::string WideToUtf8(const std::wstring_view value)
    {
        if (value.empty())
        {
            return {};
        }

        // 変換後のUTF-8バイト数
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

        // 変換後のUTF-8文字列
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

    // UTF-8文字列をパスへ変換します(value: UTF-8のパス文字列)。
    inline std::filesystem::path PathFromUtf8(const std::string_view value)
    {
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
