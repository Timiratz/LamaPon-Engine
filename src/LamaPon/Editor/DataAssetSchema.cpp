#include "LamaPon/Editor/DataAssetSchema.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>

namespace LamaPon::EditorDetail
{
    namespace
    {
        // EditorLayerShared.hのLowercaseと同じですが、こちらは
        // imgui.hを引き込まないためにこの翻訳単位へ置いています。
        [[nodiscard]] std::string ToLowercase(std::string value)
        {
            std::ranges::transform(
                value,
                value.begin(),
                [](const unsigned char character)
                {
                    return static_cast<char>(
                        std::tolower(character));
                });
            return value;
        }
    }

    std::size_t SchemaComponentCount(
        const std::string_view type) noexcept
    {
        if (type == "vec2")
        {
            return 2;
        }
        if (type == "vec3" || type == "color3")
        {
            return 3;
        }
        return 4;
    }

    nlohmann::json SchemaDefaultValue(
        const nlohmann::json& field)
    {
        const std::string type = field.is_object()
                && field.contains("type")
                && field.at("type").is_string()
            ? ToLowercase(
                field.at("type").get<std::string>())
            : std::string{};

        if (type == "object")
        {
            auto result = nlohmann::json::object();
            if (field.contains("fields")
                && field.at("fields").is_array())
            {
                for (const auto& child : field.at("fields"))
                {
                    if (child.is_object()
                        && child.contains("name")
                        && child.at("name").is_string())
                    {
                        result[child.at("name")
                            .get<std::string>()] =
                                SchemaDefaultValue(child);
                    }
                }
            }
            return result;
        }
        if (type == "list")
        {
            return nlohmann::json::array();
        }
        if (field.is_object() && field.contains("default"))
        {
            return field.at("default");
        }
        if (type == "bool")
        {
            return false;
        }
        if (type == "int")
        {
            return 0;
        }
        if (type == "float")
        {
            return 0.0;
        }
        if (type == "vec2"
            || type == "vec3"
            || type == "vec4"
            || type == "color3"
            || type == "color4")
        {
            const auto count = SchemaComponentCount(type);
            auto result = nlohmann::json::array();
            for (std::size_t index = 0;
                index < count;
                ++index)
            {
                // 色のアルファだけは1（透明で作られると
                // 「何も出ない」と勘違いさせるため）。
                const bool isColorAlpha =
                    type == "color4" && index == 3;
                result.push_back(
                    isColorAlpha ? 1.0 : 0.0);
            }
            return result;
        }
        // string / asset / 未知の型は空文字にします。
        return std::string{};
    }

    nlohmann::json MakeDataAssetDocument(
        const std::string_view typeName,
        const std::string_view schemaJson)
    {
        auto values = nlohmann::json::object();
        try
        {
            if (!schemaJson.empty())
            {
                const auto schema = nlohmann::json::parse(
                    schemaJson.begin(),
                    schemaJson.end());
                if (schema.is_object()
                    && schema.contains("fields")
                    && schema.at("fields").is_array())
                {
                    for (const auto& field :
                        schema.at("fields"))
                    {
                        if (field.is_object()
                            && field.contains("name")
                            && field.at("name").is_string())
                        {
                            values[field.at("name")
                                .get<std::string>()] =
                                    SchemaDefaultValue(field);
                        }
                    }
                }
            }
        }
        catch (const std::exception&)
        {
            // スキーマが壊れていても作成は通します。値が空の
            // まま作られ、Inspectorがスキーマのエラーを出します。
            values = nlohmann::json::object();
        }

        nlohmann::json document;
        document["format"] = "LamaPonDataAsset";
        document["version"] = 1;
        document["type"] = std::string{ typeName };
        document["values"] = std::move(values);
        return document;
    }
}
