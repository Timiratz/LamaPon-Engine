#include "JsonFiles.h"
#include "LamaPon/Core/PathUtils.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace LamaPon::Cli
{
    // JSON fileをparseして返します(path: 入力file)
    [[nodiscard]] nlohmann::json ReadJsonFile(
        const std::filesystem::path& path)
    {
        // binary read用のinput streamです。
        std::ifstream input(path, std::ios::binary);
        // fileを開けなければpath付き例外にします。
        if (!input)
        {
            throw std::runtime_error(
                "Could not open JSON file: "
                + LamaPon::PathToUtf8(path));
        }
        return nlohmann::json::parse(
            std::string{
                std::istreambuf_iterator<char>{ input },
                std::istreambuf_iterator<char>{} });
    }

    // textを同directoryの一時fileからatomic置換します(path: 出力先, text: 内容)
    void WriteTextAtomic(
        const std::filesystem::path& path,
        const std::string& text)
    {
        // 出力parent directory作成時のerror codeです。
        std::error_code directoryError;
        std::filesystem::create_directories(
            path.parent_path(),
            directoryError);
        // parent directoryを作成できなければ置換を始めません。
        if (directoryError)
        {
            throw std::runtime_error(
                "Could not create job directory: "
                + directoryError.message());
        }

        // destinationと同directoryに作る一時file pathです。
        const auto temporary =
            path.parent_path()
            / (path.filename().wstring()
                + L".tmp-"
                + std::to_wstring(GetCurrentProcessId())
                + L"-"
                + std::to_wstring(GetTickCount64()));
        {
            // 一時fileへbinary outputします。
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            // 一時fileを作成できなければ例外にします。
            if (!output)
            {
                throw std::runtime_error(
                    "Could not write temporary job file: "
                    + LamaPon::PathToUtf8(temporary));
            }
            // text全体を書いてstorageへflushします。
            output.write(
                text.data(),
                static_cast<std::streamsize>(text.size()));
            output.flush();
            // write/flush失敗時は一時fileを置換しません。
            if (!output)
            {
                throw std::runtime_error(
                    "Could not flush temporary job file: "
                    + LamaPon::PathToUtf8(temporary));
            }
        }

        // 最後のMoveFileExW失敗codeです。
        DWORD moveError = ERROR_SUCCESS;
        // 一時fileの置換を最大50回retryします(attempt: retry番号)
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            // flush済み一時fileをdestinationへ置換します。
            if (MoveFileExW(
                    temporary.c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING
                        | MOVEFILE_WRITE_THROUGH))
            {
                return;
            }
            // 最後に取得したWin32 errorです。
            moveError = GetLastError();
            // lock以外のerrorはretryしません。
            if (moveError != ERROR_ACCESS_DENIED
                && moveError != ERROR_SHARING_VIOLATION)
            {
                break;
            }
            // 一時lockにはattemptに応じた待機を挟みます。
            Sleep(std::min(100, 5 * (attempt + 1)));
        }
        // 置換失敗後の一時file cleanup結果です。
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        throw std::runtime_error(
            "Could not replace job file (Win32 error "
            + std::to_string(moveError)
            + "): "
            + LamaPon::PathToUtf8(path));
    }

    // JSONを整形しatomic保存します(path: 出力先, document: JSON値)
    void WriteJsonFile(
        const std::filesystem::path& path,
        const nlohmann::json& document)
    {
        WriteTextAtomic(
            path,
            document.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace));
    }

}
