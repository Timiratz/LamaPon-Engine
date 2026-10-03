#include "LamaPon/Editor/ScriptEditorDetection.h"

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <sstream>
#include <string_view>

namespace
{
    // Windows既知フォルダーのパスを返し取得失敗なら空を返す(id: 調べる既知フォルダーのID)。
    std::filesystem::path KnownFolder(const KNOWNFOLDERID& id)
    {
        // OSが割り当てた既知フォルダー文字列
        PWSTR value{};
        // 既知フォルダー取得のHRESULT
        const HRESULT result = SHGetKnownFolderPath(
            id,
            0,
            nullptr,
            &value);
        if (FAILED(result) || value == nullptr)
        {
            if (value != nullptr)
            {
                CoTaskMemFree(value);
            }
            return {};
        }
        // 返却用に複製したフォルダーパス
        const std::filesystem::path path{ value };
        CoTaskMemFree(value);
        return path;
    }

    // 通常ファイルが存在して実行パスが未登録なら選択肢へ追加する(options: 追加する選択肢の一覧, label: 設定欄へ表示する名前, executablePath: 存在を確認する実行パス)。
    void AddIfExecutableExists(
        std::vector<LamaPon::ScriptEditorOption>& options,
        std::string label,
        const std::filesystem::path& executablePath)
    {
        // 実行ファイルの存在確認エラー
        std::error_code error;
        if (!std::filesystem::is_regular_file(
                executablePath,
                error))
        {
            return;
        }
        // 同じ実行パスの重複を調べる(option: 登録済みのエディター)。
        const bool alreadyPresent = std::ranges::any_of(
            options,
            [&executablePath](
                const LamaPon::ScriptEditorOption& option)
            {
                return option.executablePath == executablePath;
            });
        if (alreadyPresent)
        {
            return;
        }
        options.push_back(
            {
                std::move(label),
                executablePath
            });
    }

    // ユーザーとシステムの既定配置先からVS Codeを探す(options: 検出結果を追加する一覧)。
    void DetectVisualStudioCode(
        std::vector<LamaPon::ScriptEditorOption>& options)
    {
        // ユーザー単位のアプリ配置先
        const auto userPrograms =
            KnownFolder(FOLDERID_UserProgramFiles);
        if (!userPrograms.empty())
        {
            AddIfExecutableExists(
                options,
                "Visual Studio Code",
                userPrograms
                    / L"Microsoft VS Code"
                    / L"Code.exe");
            AddIfExecutableExists(
                options,
                "Visual Studio Code - Insiders",
                userPrograms
                    / L"Microsoft VS Code Insiders"
                    / L"Code - Insiders.exe");
        }
        // 調べるProgramFilesの種別
        for (const auto& folderId :
            { FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86 })
        {
            // システムのアプリ配置先
            const auto programFiles = KnownFolder(folderId);
            if (programFiles.empty())
            {
                continue;
            }
            AddIfExecutableExists(
                options,
                "Visual Studio Code",
                programFiles
                    / L"Microsoft VS Code"
                    / L"Code.exe");
        }
    }

    // vswhereを非表示で起動し標準出力とエラーをpipeから読み取る(executable: 起動する実行ファイル, arguments: 起動時に渡す引数)。
    std::string RunProcessCaptureOutput(
        const std::filesystem::path& executable,
        const std::wstring& arguments)
    {
        // 書込pipeを継承させる設定
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        // 子の出力を読み取るpipe handle
        HANDLE readHandle{};
        // 子へ渡す出力pipe handle
        HANDLE writeHandle{};
        if (!CreatePipe(
            &readHandle,
            &writeHandle,
            &security,
            0))
        {
            return {};
        }
        if (!SetHandleInformation(
            readHandle,
            HANDLE_FLAG_INHERIT,
            0))
        {
            CloseHandle(readHandle);
            CloseHandle(writeHandle);
            return {};
        }

        // 標準出力とエラーの転送先設定
        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdOutput = writeHandle;
        startupInfo.hStdError = writeHandle;

        // 実行ファイルを引用した起動引数
        std::wstring commandLine =
            L"\"" + executable.wstring() + L"\" " + arguments;

        // 起動した子processのhandle
        PROCESS_INFORMATION processInfo{};
        // 子processを起動できたか
        const bool started = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo);
        CloseHandle(writeHandle);
        if (!started)
        {
            CloseHandle(readHandle);
            return {};
        }

        // 子processの標準出力とエラー
        std::string output;
        // pipeから読み取る作業バッファ
        std::array<char, 4096> buffer{};
        // pipeから今回読み取ったバイト数
        DWORD bytesRead{};
        while (ReadFile(
            readHandle,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            nullptr)
            && bytesRead > 0)
        {
            output.append(buffer.data(), bytesRead);
        }
        CloseHandle(readHandle);

        WaitForSingleObject(processInfo.hProcess, 10000);
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
        return output;
    }

    // vswhereでprereleaseを含むMSBuild付きVisual Studioを探す(options: 検出結果を追加する一覧)。
    void DetectVisualStudio(
        std::vector<LamaPon::ScriptEditorOption>& options)
    {
        // vswhereを探すProgramFilesX86
        const auto programFilesX86 =
            KnownFolder(FOLDERID_ProgramFilesX86);
        if (programFilesX86.empty())
        {
            return;
        }
        // VS Installerの検出用実行パス
        const auto vswhere =
            programFilesX86
            / L"Microsoft Visual Studio"
            / L"Installer"
            / L"vswhere.exe";
        // 実行ファイルの存在確認エラー
        std::error_code error;
        if (!std::filesystem::is_regular_file(vswhere, error))
        {
            return;
        }

        // MSBuildを含む製品だけを列挙してインストーラーだけの状態を除く。
        // 子processの標準出力とエラー
        const std::string output = RunProcessCaptureOutput(
            vswhere,
            L"-all -prerelease -products * "
            L"-requires Microsoft.Component.MSBuild -nologo");

        // 検出した製品の表示名
        std::string label;
        // 検出した製品の実行パス
        std::string productPath;
        // 製品情報を登録して作業値を消す処理
        const auto flush =
            [&options, &label, &productPath]()
            {
                if (!label.empty() && !productPath.empty())
                {
                    AddIfExecutableExists(
                        options,
                        label,
                        std::filesystem::path(productPath));
                }
                label.clear();
                productPath.clear();
            };

        // 表示名の出力行を判別する接頭辞
        constexpr std::string_view displayNamePrefix{
            "displayName: "
        };
        // 製品パスの出力行を判別する接頭辞
        constexpr std::string_view productPathPrefix{
            "productPath: "
        };
        // vswhereの出力を1行ずつ読むstream
        std::istringstream stream(output);
        // 解析するvswhere出力の1行
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                flush();
                continue;
            }
            if (line.starts_with(displayNamePrefix))
            {
                label = line.substr(displayNamePrefix.size());
            }
            else if (line.starts_with(productPathPrefix))
            {
                productPath =
                    line.substr(productPathPrefix.size());
            }
        }
        flush();
    }
}

namespace LamaPon
{
    std::wstring BuildScriptEditorArguments(
        const std::filesystem::path& editor,
        const std::filesystem::path& source,
        const std::uint32_t line,
        const std::uint32_t column)
    {
        // 種類の判定用に小文字にしたexe名
        auto executable = editor.filename().wstring();
        // 実行ファイル名を種類判定用の小文字へ変換する(character: 比較する文字)。
        std::ranges::transform(
            executable,
            executable.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
        // 空白に備えて引用したsourceパス
        const auto quotedSource = L"\"" + source.wstring() + L"\"";
        if (line == 0)
        {
            return quotedSource;
        }
        if (executable == L"code.exe"
            || executable == L"code - insiders.exe")
        {
            return L"--goto \"" + source.wstring()
                + L":" + std::to_wstring(line)
                + L":" + std::to_wstring(std::max(column, 1u))
                + L"\"";
        }
        if (executable == L"devenv.exe")
        {
            return quotedSource + L" /command \"Edit.Goto "
                + std::to_wstring(line) + L"\"";
        }
        return quotedSource;
    }

    std::vector<ScriptEditorOption> DetectScriptEditors()
    {
        // 見つかったエディターの選択肢
        std::vector<ScriptEditorOption> options;
        DetectVisualStudioCode(options);
        DetectVisualStudio(options);
        return options;
    }
}
