#include "LamaPon/Editor/ShaderProperties.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/LitMaterial.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace
{
    constexpr std::string_view BlockBegin = "LAMAPON_PROPERTIES";

    // "t7" のようなテクスチャ枠の指定。t7が0番になります。
    bool ParseTextureTarget(
        const std::string& target,
        LamaPon::ShaderPropertyField& field,
        std::string& error)
    {
        if (target.size() < 2
            || (target[0] != 't' && target[0] != 'T'))
        {
            error =
                "textureのtargetは\"t7\"のように書きます: "
                + target;
            return false;
        }
        const auto slot =
            std::strtoul(target.c_str() + 1, nullptr, 10);
        const auto first =
            LamaPon::LitMaterial::CustomTextureFirstSlot;
        if (slot < first
            || slot >= first
                + LamaPon::LitMaterial::CustomTextureCount)
        {
            error = "textureのtargetはt"
                + std::to_string(first)
                + "〜t"
                + std::to_string(
                    first
                    + LamaPon::LitMaterial::
                        CustomTextureCount - 1)
                + "です（t0〜t"
                + std::to_string(first - 1)
                + "はエンジンが使用）: "
                + target;
            return false;
        }
        field.parameterIndex =
            static_cast<std::size_t>(slot - first);
        field.componentCount = 1;
        return true;
    }

    // "0.x" や "1.rgb" のような指定を、パラメーター番号と成分へ分解
    // します。成分の省略時はxだけを使います。
    bool ParseTarget(
        const std::string& target,
        LamaPon::ShaderPropertyField& field,
        std::string& error)
    {
        const auto dot = target.find('.');
        const std::string indexText = dot == std::string::npos
            ? target
            : target.substr(0, dot);
        if (indexText.empty()
            || !std::all_of(
                indexText.begin(),
                indexText.end(),
                [](const unsigned char character)
                {
                    return std::isdigit(character) != 0;
                }))
        {
            error = "targetの番号が不正です: " + target;
            return false;
        }

        const auto parsedIndex =
            std::strtoul(indexText.c_str(), nullptr, 10);
        if (parsedIndex
            >= LamaPon::LitMaterial::CustomParameterCount)
        {
            error = "targetの番号が範囲外です（0～"
                + std::to_string(
                    LamaPon::LitMaterial::
                        CustomParameterCount - 1)
                + "）: "
                + target;
            return false;
        }
        field.parameterIndex =
            static_cast<std::size_t>(parsedIndex);

        const std::string swizzle = dot == std::string::npos
            ? std::string{ "x" }
            : target.substr(dot + 1);
        if (swizzle.empty() || swizzle.size() > 4)
        {
            error = "targetの成分指定が不正です: " + target;
            return false;
        }

        field.componentCount = 0;
        for (const char component : swizzle)
        {
            std::size_t index{};
            switch (component)
            {
            case 'x': case 'r': index = 0; break;
            case 'y': case 'g': index = 1; break;
            case 'z': case 'b': index = 2; break;
            case 'w': case 'a': index = 3; break;
            default:
                error =
                    "成分はx/y/z/wまたはr/g/b/aで指定します: "
                    + target;
                return false;
            }
            field.components[field.componentCount] = index;
            ++field.componentCount;
        }
        return true;
    }

    LamaPon::ShaderPropertyKind ParseKind(
        const std::string& type,
        const std::size_t componentCount)
    {
        if (type == "color")
        {
            return LamaPon::ShaderPropertyKind::Color;
        }
        if (type == "bool")
        {
            return LamaPon::ShaderPropertyKind::Boolean;
        }
        if (type == "vector")
        {
            return LamaPon::ShaderPropertyKind::Vector;
        }
        if (type == "float" || type.empty())
        {
            // 成分が複数ならまとめて数値入力にします。
            return componentCount > 1
                ? LamaPon::ShaderPropertyKind::Vector
                : LamaPon::ShaderPropertyKind::Float;
        }
        return LamaPon::ShaderPropertyKind::Float;
    }
}

namespace LamaPon
{
    ShaderProperties ParseShaderProperties(
        const std::string_view shaderSource)
    {
        ShaderProperties result;
        const auto blockPosition =
            shaderSource.find(BlockBegin);
        if (blockPosition == std::string_view::npos)
        {
            return result;
        }

        // 宣言はコメント内に書かれます。ブロック名の後ろから
        // 最初の '[' 〜 対応する ']' までをJSONとして読みます。
        const auto arrayStart =
            shaderSource.find('[', blockPosition);
        if (arrayStart == std::string_view::npos)
        {
            result.declared = true;
            result.error =
                "LAMAPON_PROPERTIESの後に[が見つかりません";
            return result;
        }

        std::size_t depth = 0;
        std::size_t arrayEnd = std::string_view::npos;
        bool inString = false;
        for (std::size_t index = arrayStart;
            index < shaderSource.size();
            ++index)
        {
            const char character = shaderSource[index];
            if (inString)
            {
                if (character == '\\')
                {
                    ++index;
                }
                else if (character == '"')
                {
                    inString = false;
                }
                continue;
            }
            if (character == '"')
            {
                inString = true;
            }
            else if (character == '[')
            {
                ++depth;
            }
            else if (character == ']')
            {
                --depth;
                if (depth == 0)
                {
                    arrayEnd = index;
                    break;
                }
            }
        }
        if (arrayEnd == std::string_view::npos)
        {
            result.declared = true;
            result.error =
                "LAMAPON_PROPERTIESの]が見つかりません";
            return result;
        }

        result.declared = true;
        const auto json = shaderSource.substr(
            arrayStart,
            arrayEnd - arrayStart + 1);
        const auto document = nlohmann::json::parse(
            json,
            nullptr,
            false);
        if (document.is_discarded()
            || !document.is_array())
        {
            result.error =
                "LAMAPON_PROPERTIESのJSONを解釈できません";
            return result;
        }

        for (const auto& entry : document)
        {
            if (!entry.is_object())
            {
                continue;
            }
            ShaderPropertyField field;
            std::string error;
            const auto target =
                entry.value("target", std::string{});
            const auto type =
                entry.value("type", std::string{});
            if (type == "texture")
            {
                field.kind = ShaderPropertyKind::Texture;
                if (!ParseTextureTarget(
                        target,
                        field,
                        error))
                {
                    result.error = error;
                    continue;
                }
                field.name = entry.value(
                    "name",
                    std::string{ "テクスチャ" });
                result.fields.push_back(std::move(field));
                continue;
            }
            if (!ParseTarget(target, field, error))
            {
                result.error = error;
                continue;
            }
            field.name = entry.value(
                "name",
                std::string{ "パラメーター" });
            field.kind = ParseKind(
                type,
                field.componentCount);
            if (entry.contains("min")
                && entry.contains("max"))
            {
                field.minimum = entry.value("min", 0.0f);
                field.maximum = entry.value("max", 1.0f);
                field.hasRange =
                    field.maximum > field.minimum;
            }
            if (const auto defaults = entry.find("default");
                defaults != entry.end())
            {
                std::array<float, 4> values{};
                if (defaults->is_number())
                {
                    values[0] = defaults->get<float>();
                }
                else if (defaults->is_boolean())
                {
                    values[0] = defaults->get<bool>()
                        ? 1.0f
                        : 0.0f;
                }
                else if (defaults->is_array())
                {
                    const auto count = std::min<std::size_t>(
                        defaults->size(),
                        4);
                    for (std::size_t index = 0;
                        index < count;
                        ++index)
                    {
                        values[index] =
                            defaults->at(index)
                                .get<float>();
                    }
                }
                field.defaultValue = values;
            }
            result.fields.push_back(std::move(field));
        }
        return result;
    }

    ShaderProperties LoadShaderProperties(
        const std::filesystem::path& shaderPath)
    {
        ShaderProperties result;
        if (shaderPath.empty())
        {
            return result;
        }
        std::ifstream input(shaderPath, std::ios::binary);
        if (!input)
        {
            return result;
        }
        std::ostringstream contents;
        contents << input.rdbuf();
        return ParseShaderProperties(contents.str());
    }
}
