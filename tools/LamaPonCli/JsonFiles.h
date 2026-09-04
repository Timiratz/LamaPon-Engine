#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace LamaPon::Cli
{
    [[nodiscard]] nlohmann::json ReadJsonFile(const std::filesystem::path& path);
    // 同じディレクトリの一時ファイルをflushしてから置換します。
    // 部分書き込みを他プロセスへ見せず、失敗は例外として呼び出し側へ返します。
    void WriteTextAtomic(const std::filesystem::path& path, const std::string& text);
    void WriteJsonFile(const std::filesystem::path& path, const nlohmann::json& document);
}
