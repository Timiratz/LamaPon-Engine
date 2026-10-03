#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace LamaPon::Cli
{
    // JSON fileを読み込みます(path: 入力file)
    [[nodiscard]] nlohmann::json ReadJsonFile(const std::filesystem::path& path);
    // textを一時file経由で置換します(path: 出力先, text: 本文)。同directoryで置換し失敗は例外にします。
    void WriteTextAtomic(const std::filesystem::path& path, const std::string& text);
    // JSONを整形してfileへ保存します(path: 出力先, document: JSON値)
    void WriteJsonFile(const std::filesystem::path& path, const nlohmann::json& document);
}
