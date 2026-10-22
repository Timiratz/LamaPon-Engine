#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{
    // 無人検証の引数だけを完全一致で調べます。
    bool IsValidation()
    {
        // Windowsが返す引数数
        int count{};
        // Windows所有の引数配列
        auto** values = CommandLineToArgvW(GetCommandLineW(), &count);
        // 無人検証指定の有無
        bool validation = false;
        if (values != nullptr)
        {
            // 実行名以外の引数番号
            for (int index = 1; index < count; ++index)
                validation |= std::wstring_view(values[index]) == L"--validate-startup";
            LocalFree(values);
        }
        return validation;
    }

    // 診断文をUTF-8へ変換します(text: UTF-16の診断文)。
    std::string ToUtf8(const std::wstring& text)
    {
        // 終端を除く必要バイト数
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        // UTF-8の出力領域
        std::string result(static_cast<std::size_t>(size), '\0');
        if (size > 0)
            WideCharToMultiByte(CP_UTF8, 0, text.data(),
                static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    // Windowsの説明を取得します(code: Win32またはプロセス終了コード)。
    std::wstring ErrorText(const DWORD code)
    {
        // 固定長のシステム説明領域
        std::array<wchar_t, 2048> buffer{};
        // 取得した説明の文字数
        const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
            buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
        return length == 0 ? L"Windowsの説明は取得できませんでした。"
            : std::wstring(buffer.data(), length);
    }

    // 起動失敗の診断を保存します(path: 保存先, text: UTF-8本文)。
    bool WriteReport(const std::filesystem::path& path, const std::string& text)
    {
        // 診断ファイルの出力先
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        file.flush();
        return file.good();
    }

    // 失敗コードと保存先を通知します(launcher: 起動exe, data: Dataフォルダー, stage: 失敗段階, code: 原因コード, validation: 無人検証)。
    void ReportFailure(const std::filesystem::path& launcher,
        const std::filesystem::path& data, const wchar_t* stage,
        const DWORD code, const bool validation)
    try
    {
        // 16進数の原因コード
        std::array<wchar_t, 16> hexadecimal{};
        swprintf_s(hexadecimal.data(), hexadecimal.size(), L"0x%08lX", code);
        // 利用者向けの失敗内容
        std::wstring message = std::wstring(stage) + L"\nコード: "
            + hexadecimal.data() + L" (" + std::to_wstring(code) + L")\n"
            + ErrorText(code);
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            message += L"\nexeと対応する_Dataフォルダーを一緒に配置してください。";
        else if (code == 0xC0000135u || code == ERROR_MOD_NOT_FOUND)
            message += L"\n必要なDLLを読み込めません。配布ファイルの欠落を確認してください。";
        else if (code == 0xC000007Bu || code == 0xC000012Fu || code == ERROR_BAD_EXE_FORMAT
            || code == ERROR_EXE_MACHINE_TYPE_MISMATCH)
            message += L"\nexeやDLLの形式・CPUアーキテクチャが対応していない可能性があります。";
        // レポートのUTC時刻
        SYSTEMTIME time{};
        GetSystemTime(&time);
        // 固定長のUTC時刻表記
        std::array<wchar_t, 64> timestamp{};
        swprintf_s(timestamp.data(), timestamp.size(), L"%04u-%02u-%02u %02u:%02u:%02u UTC",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
        // UTF-8の診断文書
        const auto report = ToUtf8(L"LamaPon launcher failure\nTime: "
            + std::wstring(timestamp.data()) + L"\nLauncher: " + launcher.wstring()
            + L"\nGame: " + (data / L"LamaPonGame.exe").wstring()
            + L"\n" + message + L"\n");
        // 実際に書き込めた保存先
        std::filesystem::path saved;
        // 書き込み候補を調べるエラー
        std::error_code error;
        if (std::filesystem::is_directory(data, error)
            && WriteReport(data / L"StartupFailure.log", report))
            saved = data / L"StartupFailure.log";
        else
        {
            // ユーザーごとの書き込み先
            std::array<wchar_t, MAX_PATH> local{};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA,
                    nullptr, SHGFP_TYPE_CURRENT, local.data())))
            {
                // 配布先に書けない場合の診断フォルダー
                const auto fallback = std::filesystem::path(local.data())
                    / L"LamaPon" / L"StartupReports" / launcher.stem();
                error.clear();
                std::filesystem::create_directories(fallback, error);
                if (!error && WriteReport(fallback / L"StartupFailure.log", report))
                    saved = fallback / L"StartupFailure.log";
            }
        }
        message += saved.empty() ? L"\n\n診断を保存できませんでした。この画面を撮影してください。"
            : L"\n\n診断ファイル:\n" + saved.wstring();
        if (!validation)
            MessageBoxW(nullptr, message.c_str(), L"ゲームの起動エラー", MB_OK | MB_ICONERROR);
    }
    catch (...)
    {
        if (!validation)
            MessageBoxW(nullptr, L"ゲームが失敗し、診断ファイルも保存できませんでした。",
                L"ゲームの起動エラー", MB_OK | MB_ICONERROR);
    }
}

// DLLに依存せずゲームを起動します(arguments: 起動引数, showWindow: 表示指定)。
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int showWindow)
try
{
    // 無人検証ではダイアログを抑止
    const bool validation = IsValidation();
    // ローダー失敗も終了コードで回収
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // 起動exeの絶対パス領域
    std::wstring path(32768, L'\0');
    // 取得したパスの文字数
    const auto length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
    {
        if (!validation)
            MessageBoxW(nullptr, L"ゲームの保存先を取得できませんでした。",
                L"ゲームの起動エラー", MB_OK | MB_ICONERROR);
        return 1;
    }
    path.resize(length);
    // 配布ルートの起動exe
    const std::filesystem::path launcher(path);
    // ゲーム本体の保存先
    const auto data = launcher.parent_path()
        / (launcher.stem().wstring() + L"_Data");
    // 実行するゲーム本体
    const auto game = data / L"LamaPonGame.exe";
    // 元の引数を保持するコマンド
    auto command = L"\"" + game.wstring() + L"\"";
    if (arguments != nullptr && *arguments != L'\0')
        command += L" " + std::wstring(arguments);

    // ゲームの表示設定
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = static_cast<WORD>(showWindow);
    // 起動したプロセス情報
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr,
            FALSE, 0, nullptr, data.c_str(), &startup, &process))
    {
        // 他のAPIを呼ぶ前に失敗を保持
        const DWORD error = GetLastError();
        ReportFailure(launcher, data, L"ゲームを起動できませんでした。", error, validation);
        return static_cast<int>(error);
    }
    CloseHandle(process.hThread);
    // ゲームの終了待ち結果
    const auto wait = WaitForSingleObject(process.hProcess, INFINITE);
    // ゲームまたは待機の終了コード
    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0)
    {
        if (!GetExitCodeProcess(process.hProcess, &exitCode))
            exitCode = GetLastError();
    }
    else
        exitCode = GetLastError();
    CloseHandle(process.hProcess);
    if (exitCode != 0)
        ReportFailure(launcher, data, L"ゲームが正常に終了しませんでした。", exitCode, validation);
    return static_cast<int>(exitCode);
}
catch (...)
{
    if (!IsValidation())
        MessageBoxW(nullptr, L"ゲーム起動の準備中に予期しないエラーが発生しました。",
            L"ゲームの起動エラー", MB_OK | MB_ICONERROR);
    return 1;
}
