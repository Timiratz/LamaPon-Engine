#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace LamaPon::Cli
{
    // CLIが扱う組み込みフィールドの定義。返した文書はプロセス中有効です。
    [[nodiscard]] const nlohmann::json& ComponentSchemas();
    // 未登録の型・フィールドは拡張用として通し、既知の型違いだけを拒否。
    void ValidateComponentValue(const std::string& componentType,
        const std::string& path, const nlohmann::json& value);
    void ValidateComponentObject(const std::string& componentType,
        const nlohmann::json& component);
    [[nodiscard]] int RunComponentCommand(const std::wstring& action,
        const std::string& type, const std::string& category);
}
