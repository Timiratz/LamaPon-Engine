#include "JsonFiles.h"
#include "LamaPon/Core/PathUtils.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace LamaPon::Cli
{
    [[nodiscard]] nlohmann::json ReadJsonFile(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
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

    void WriteTextAtomic(
        const std::filesystem::path& path,
        const std::string& text)
    {
        std::error_code directoryError;
        std::filesystem::create_directories(
            path.parent_path(),
            directoryError);
        if (directoryError)
        {
            throw std::runtime_error(
                "Could not create job directory: "
                + directoryError.message());
        }

        const auto temporary =
            path.parent_path()
            / (path.filename().wstring()
                + L".tmp-"
                + std::to_wstring(GetCurrentProcessId())
                + L"-"
                + std::to_wstring(GetTickCount64()));
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Could not write temporary job file: "
                    + LamaPon::PathToUtf8(temporary));
            }
            output.write(
                text.data(),
                static_cast<std::streamsize>(text.size()));
            output.flush();
            if (!output)
            {
                throw std::runtime_error(
                    "Could not flush temporary job file: "
                    + LamaPon::PathToUtf8(temporary));
            }
        }

        DWORD moveError = ERROR_SUCCESS;
        // ウイルススキャンなどによる一時ロックは数秒続く場合があるため、
        // 待機時間を延ばしながら合計約4秒まで再試行します。
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            if (MoveFileExW(
                    temporary.c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING
                        | MOVEFILE_WRITE_THROUGH))
            {
                return;
            }
            moveError = GetLastError();
            if (moveError != ERROR_ACCESS_DENIED
                && moveError != ERROR_SHARING_VIOLATION)
            {
                break;
            }
            Sleep(std::min(100, 5 * (attempt + 1)));
        }
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        throw std::runtime_error(
            "Could not replace job file (Win32 error "
            + std::to_string(moveError)
            + "): "
            + LamaPon::PathToUtf8(path));
    }

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
