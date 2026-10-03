#include "LamaPon/Editor/DataAssetSchema.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>

namespace LamaPon::EditorDetail
{
    namespace
    {
        // ImGuiへの依存を持たず文字列を小文字へ変換する(value: 変換して返す文字列の所有先)。
        [[nodiscard]] std::string ToLowercase(std::string value)
        {
            // 渡された文字列を小文字にする(character: 符号なしの比較対象文字)。
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

    std::vector<SchemaStringOption> SchemaStringOptions(
        const nlohmann::json& field)
    {
        // 選択肢または型に応じた既定値
        std::vector<SchemaStringOption> result;
        if (!field.is_object() || !field.contains("options")
            || !field.at("options").is_array())
        {
            return result;
        }
        // 解析する文字列の選択肢
        for (const auto& option : field.at("options"))
        {
            // 保存値と表示名を持つ選択肢
            SchemaStringOption entry;
            if (option.is_string())
            {
                entry.value = option.get<std::string>();
                entry.displayName = entry.value;
            }
            else if (option.is_object() && option.contains("value")
                && option.at("value").is_string())
            {
                entry.value = option.at("value").get<std::string>();
                entry.displayName = option.contains("displayName")
                        && option.at("displayName").is_string()
                    ? option.at("displayName").get<std::string>()
                    : entry.value;
            }
            else
            {
                continue;
            }
            // 保存値が同じ選択肢を重複登録しない(existing: 登録済みの選択肢)。
            if (std::ranges::none_of(result, [&entry](const auto& existing)
                { return existing.value == entry.value; }))
            {
                result.push_back(std::move(entry));
            }
        }
        return result;
    }

    nlohmann::json SchemaDefaultValue(
        const nlohmann::json& field)
    {
        // 小文字にしたフィールドの型名
        const std::string type = field.is_object()
                && field.contains("type")
                && field.at("type").is_string()
            ? ToLowercase(
                field.at("type").get<std::string>())
            : std::string{};

        if (type == "object")
        {
            // 選択肢または型に応じた既定値
            auto result = nlohmann::json::object();
            if (field.contains("fields")
                && field.at("fields").is_array())
            {
                // 再帰的に初期化する子フィールド
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
        if (type == "string")
        {
            // 重複を除いた文字列の選択肢
            const auto options = SchemaStringOptions(field);
            if (!options.empty())
            {
                return options.front().value;
            }
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
            // 初期値を作る成分数
            const auto count = SchemaComponentCount(type);
            // 選択肢または型に応じた既定値
            auto result = nlohmann::json::array();
            // 初期化する成分の番号
            for (std::size_t index = 0;
                // 初期値を作る成分数
                index < count;
                ++index)
            {
                // color4の既定alphaを1にするか
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
        // スキーマから作る既定値の集合
        auto values = nlohmann::json::object();
        try
        {
            if (!schemaJson.empty())
            {
                // 解析したデータアセットのスキーマ
                const auto schema = nlohmann::json::parse(
                    schemaJson.begin(),
                    schemaJson.end());
                if (schema.is_object()
                    && schema.contains("fields")
                    && schema.at("fields").is_array())
                {
                    // 初期化するトップ階層の項目
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
            // 不正なスキーマでも値を空にして作成を通し、Inspectorでエラーを表示する。
            values = nlohmann::json::object();
        }

        // 返却するデータアセット文書
        nlohmann::json document;
        document["format"] = "LamaPonDataAsset";
        document["version"] = 1;
        document["type"] = std::string{ typeName };
        document["values"] = std::move(values);
        return document;
    }
}
