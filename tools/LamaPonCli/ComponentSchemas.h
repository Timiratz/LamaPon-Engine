#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace LamaPon::Cli
{
    // CLIが扱う組み込みschemaを返します。文書はprocess内で有効です。
    [[nodiscard]] const nlohmann::json& ComponentSchemas();
    // 既知fieldの値型を検証します(componentType: 種別, path: field, value: 値)。未知fieldは通します。
    void ValidateComponentValue(const std::string& componentType,
        const std::string& path, const nlohmann::json& value);
    // component JSON objectの既知fieldを検証します(componentType: 種別, component: object)
    void ValidateComponentObject(const std::string& componentType,
        const nlohmann::json& component);
    // schema操作commandを実行します(action: 操作, type: 種別, category: 分類)
    [[nodiscard]] int RunComponentCommand(const std::wstring& action,
        const std::string& type, const std::string& category);
}
