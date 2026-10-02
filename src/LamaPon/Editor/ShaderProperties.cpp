#include "LamaPon/Editor/ShaderProperties.h"

#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/LitMaterial.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace
{
    // プロパティ定義の開始識別子
    constexpr std::string_view BlockBegin = "LAMAPON_PROPERTIES";

    // t7起点の追加texture registerを検証して入力情報へ変換する(target: t番号形式の配置指定, field: 配置を設定する入力情報, error: 失敗理由の出力先)。
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
        // register番号は10進数字だけを受け付ける(character: 判定する文字)。
        if (!std::all_of(
                target.begin() + 1,
                target.end(),
                [](const unsigned char character)
                {
                    return std::isdigit(character) != 0;
                }))
        {
            error =
                "textureのtargetは\"t7\"のように書きます: "
                + target;
            return false;
        }
        // 指定されたtexture register番号
        const auto slot =
            std::strtoul(target.c_str() + 1, nullptr, 10);
        // カスタムtextureの先頭または成分
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

    // 定数配列番号と成分を検証し省略成分はxとして配置する(target: 番号とxyzw・rgbaの成分指定, field: 配置を設定する入力情報, error: 失敗理由の出力先)。
    bool ParseTarget(
        const std::string& target,
        LamaPon::ShaderPropertyField& field,
        std::string& error)
    {
        // 定数配列番号と成分を区切る位置
        const auto dot = target.find('.');
        // 定数配列番号の文字列表記
        const std::string indexText = dot == std::string::npos
            ? target
            : target.substr(0, dot);
        // 定数配列の番号は10進数字だけを受け付ける(character: 判定する文字)。
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

        // 数値へ変換した定数配列番号
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

        // 順序を保つ成分指定の文字列
        const std::string swizzle = dot == std::string::npos
            ? std::string{ "x" }
            : target.substr(dot + 1);
        if (swizzle.empty() || swizzle.size() > 4)
        {
            error = "targetの成分指定が不正です: " + target;
            return false;
        }

        field.componentCount = 0;
        // 同じ成分を重複指定したか
        std::array<bool, 4> componentsUsed{};
        // 指定文字または参照する成分番号
        for (const char component : swizzle)
        {
            // 成分番号または解析中の文字位置
            std::size_t index{};
            // xyzwとrgbaを同じ4成分の番号へ変換する。
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
            if (componentsUsed[index])
            {
                error = "target内で同じ成分を重複指定できません: "
                    + target;
                return false;
            }
            componentsUsed[index] = true;
            field.components[field.componentCount] = index;
            ++field.componentCount;
        }
        return true;
    }

    // 宣言型と成分数を入力形式へ変換し未指定の複数成分はvectorにする(type: 宣言された小文字の型名, componentCount: 配置する定数の成分数)。
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
        // 宣言の有無・入力情報・失敗理由
        ShaderProperties result;
        // 最初の宣言識別子の位置
        const auto blockPosition =
            shaderSource.find(BlockBegin);
        if (blockPosition == std::string_view::npos)
        {
            return result;
        }

        // 識別子の後の最初の配列を、文字列内の括弧を除いて対応する終端まで読み取る。
        // 宣言のJSON配列の開始位置
        const auto arrayStart =
            shaderSource.find('[', blockPosition);
        if (arrayStart == std::string_view::npos)
        {
            result.declared = true;
            result.error =
                "LAMAPON_PROPERTIESの後に[が見つかりません";
            return result;
        }

        // 文字列外の角括弧の深さ
        std::size_t depth = 0;
        // 宣言のJSON配列の終了位置
        std::size_t arrayEnd = std::string_view::npos;
        // 解析位置がJSON文字列の中か
        bool inString = false;
        // 成分番号または解析中の文字位置
        for (std::size_t index = arrayStart;
            index < shaderSource.size();
            ++index)
        {
            // 宣言の括弧対応を調べる文字
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
        // 解釈する宣言配列のJSON範囲
        const auto json = shaderSource.substr(
            arrayStart,
            arrayEnd - arrayStart + 1);
        // 宣言配列のJSON解釈結果
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

        try
        {
            // 解釈するプロパティのJSON項目
            for (const auto& entry : document)
            {
                if (!entry.is_object())
                {
                    continue;
                }
                // Inspectorへ渡す入力情報
                ShaderPropertyField field;
                // 項目のtarget解釈の失敗理由
                std::string error;
                // 宣言された定数成分またはtexture
                const auto target =
                    entry.value("target", std::string{});
                // 宣言されたプロパティの入力形式
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
                // 項目の既定値のJSON位置
                if (const auto defaults = entry.find("default");
                    defaults != entry.end())
                {
                    // 4成分までの既定値
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
                        // 配列から読み取る既定値の成分数
                        const auto count = std::min<std::size_t>(
                            defaults->size(),
                            4);
                        // 成分番号または解析中の文字位置
                        for (std::size_t index = 0;
                            // 配列から読み取る既定値の成分数
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
        }
        // 編集途中の宣言で発生したJSON解釈エラー
        catch (const nlohmann::json::exception& exception)
        {
            // 編集中に型が崩れた宣言は入力情報を消し、Inspectorへ失敗理由を返す。
            result.fields.clear();
            result.error =
                "LAMAPON_PROPERTIESの値を解釈できません: "
                + std::string{ exception.what() };
        }
        return result;
    }

    ShaderProperties ConvertShaderManifestProperties(
        const std::vector<ShaderPropertyDesc>& properties)
    {
        // 宣言の有無・入力情報・失敗理由
        ShaderProperties result;
        if (properties.empty())
        {
            return result;
        }
        result.declared = true;

        // 予約済みの定数配列の成分
        std::array<
            std::array<bool, 4>,
            LitMaterial::CustomParameterCount> parameterComponentsUsed{};
        // 予約済みのカスタムtexture枠
        std::array<
            bool,
            LitMaterial::CustomTextureCount> textureSlotsUsed{};

        // 宣言全体を無効にして生の入力へ戻す(error: Inspectorへ表示する失敗理由)。
        const auto fail = [&result](std::string error)
            -> ShaderProperties
        {
            // 同じ定数を名前付きUIと生のUIで二重編集しないよう、Manifest宣言は全体を無効にする。
            result.fields.clear();
            result.error = std::move(error);
            return result;
        };

        // 省略項目が後続の明示targetを使わないよう先に全targetを予約し、残りを宣言順に自動配置する。
        // Manifest内の宣言順の番号
        for (std::size_t propertyIndex = 0;
            propertyIndex < properties.size();
            ++propertyIndex)
        {
            // 配置するManifestの宣言
            const auto& property = properties[propertyIndex];
            if (property.target.empty())
            {
                continue;
            }
            // 先に予約する明示targetの情報
            ShaderPropertyField reserved;
            // targetの解釈で通知された理由
            std::string parseError;
            // 追加texture枠の宣言か
            const bool texture = property.type == "texture";
            if (!(texture
                    ? ParseTextureTarget(
                        property.target, reserved, parseError)
                    : ParseTarget(
                        property.target, reserved, parseError)))
            {
                return fail("Shader Manifest properties["
                    + std::to_string(propertyIndex) + "]: "
                    + parseError);
            }
            if (texture)
            {
                if (textureSlotsUsed[reserved.parameterIndex])
                {
                    return fail("Shader Manifest property targetが重複しています: "
                        + property.target);
                }
                textureSlotsUsed[reserved.parameterIndex] = true;
            }
            else
            {
                // 指定文字または参照する成分番号
                for (std::size_t component{};
                    component < reserved.componentCount;
                    ++component)
                {
                    // 明示targetの成分を予約済みか
                    auto& used = parameterComponentsUsed[
                        reserved.parameterIndex][reserved.components[component]];
                    if (used)
                    {
                        return fail(
                            "Shader Manifest property targetが重複しています: "
                            + property.target);
                    }
                    used = true;
                }
            }
        }

        // Manifest内の宣言順の番号
        for (std::size_t propertyIndex = 0;
            propertyIndex < properties.size();
            ++propertyIndex)
        {
            // 配置するManifestの宣言
            const auto& property = properties[propertyIndex];
            // 宣言番号と名前を含む診断用の表記
            const std::string context = "Shader Manifest properties["
                + std::to_string(propertyIndex) + "] ('"
                + property.name + "')";
            if (property.name.empty())
            {
                return fail(context + " のnameが空です。");
            }
            // Inspectorへ渡す入力情報
            ShaderPropertyField field;
            field.name = property.name;
            // targetの解釈で通知された理由
            std::string parseError;
            if (property.type == "texture")
            {
                field.kind = ShaderPropertyKind::Texture;
                if (property.target.empty())
                {
                    // 最初の未予約texture枠
                    const auto available = std::find(
                        textureSlotsUsed.begin(),
                        textureSlotsUsed.end(),
                        false);
                    if (available == textureSlotsUsed.end())
                    {
                        return fail(context
                            + " を自動配置できるTexture枠がありません。");
                    }
                    field.parameterIndex = static_cast<std::size_t>(
                        std::distance(textureSlotsUsed.begin(), available));
                    *available = true;
                }
                else if (!ParseTextureTarget(
                    property.target,
                    field,
                    parseError))
                {
                    return fail(context + ": " + parseError);
                }
                if (field.parameterIndex >= textureSlotsUsed.size())
                {
                    return fail(context
                        + " のtexture slotが範囲外です。");
                }
                if (!property.defaultValue.empty())
                {
                    return fail(context
                        + " のtexture defaultはサポートされていません。");
                }
            }
            else
            {
                if (property.type != "float"
                    && property.type != "color"
                    && property.type != "bool"
                    && property.type != "vector")
                {
                    return fail(context + " のtypeが不正です: "
                        + property.type);
                }
                if (property.target.empty())
                {
                    // 自動配置に必要な連続する成分数
                    std::size_t required = 1;
                    if (property.type == "color"
                        || property.type == "vector")
                    {
                        required = 4;
                        if (!property.defaultValue.empty())
                        {
                            // 自動配置用に解析した既定値
                            const auto value = nlohmann::json::parse(
                                property.defaultValue,
                                nullptr,
                                false);
                            if (value.is_array()
                                && value.size() >= 2
                                && value.size() <= 4)
                            {
                                required = value.size();
                            }
                        }
                    }
                    // 連続する定数成分へ配置できたか
                    bool allocated{};
                    // 空き成分を探す定数配列番号
                    for (std::size_t parameter{};
                        parameter < parameterComponentsUsed.size()
                            && !allocated;
                        ++parameter)
                    {
                        // カスタムtextureの先頭または成分
                        for (std::size_t first{};
                            first + required <= 4;
                            ++first)
                        {
                            // 必要な連続成分が全て未予約か
                            bool free = true;
                            // 割り当てる連続成分内の位置
                            for (std::size_t offset{};
                                // 自動配置に必要な連続する成分数
                                offset < required;
                                ++offset)
                            {
                                free = free && !parameterComponentsUsed[
                                    parameter][first + offset];
                            }
                            if (!free)
                            {
                                continue;
                            }
                            field.parameterIndex = parameter;
                            field.componentCount = required;
                            // 割り当てる連続成分内の位置
                            for (std::size_t offset{};
                                // 自動配置に必要な連続する成分数
                                offset < required;
                                ++offset)
                            {
                                field.components[offset] = first + offset;
                                parameterComponentsUsed[parameter]
                                    [first + offset] = true;
                            }
                            allocated = true;
                            break;
                        }
                    }
                    if (!allocated)
                    {
                        return fail(context
                            + " を自動配置できる定数領域がありません。");
                    }
                }
                else if (!ParseTarget(
                    property.target,
                    field,
                    parseError))
                {
                    return fail(context + ": " + parseError);
                }

                // 入力形式と指定成分数が一致するか
                const bool validComponentCount =
                    (property.type == "float"
                        || property.type == "bool")
                        ? field.componentCount == 1
                        : (property.type == "color"
                            ? field.componentCount == 3
                                || field.componentCount == 4
                            : field.componentCount >= 2
                                && field.componentCount <= 4);
                if (!validComponentCount)
                {
                    return fail(context
                        + " のtypeとtargetの成分数が一致しません。");
                }
                field.kind = ParseKind(
                    property.type,
                    field.componentCount);

            }

            if (property.minimum.has_value()
                != property.maximum.has_value())
            {
                return fail(context
                    + " のminとmaxは両方を指定してください。");
            }
            if (property.minimum.has_value())
            {
                if (field.kind != ShaderPropertyKind::Float
                    || !std::isfinite(*property.minimum)
                    || !std::isfinite(*property.maximum)
                    || *property.maximum <= *property.minimum)
                {
                    return fail(context
                        + " のmin/maxが不正です。");
                }
                field.minimum = static_cast<float>(*property.minimum);
                field.maximum = static_cast<float>(*property.maximum);
                if (!std::isfinite(field.minimum)
                    || !std::isfinite(field.maximum))
                {
                    return fail(context
                        + " のmin/maxはfloatの範囲内で指定してください。");
                }
                field.hasRange = true;
            }

            if (!property.defaultValue.empty())
            {
                // Manifestの既定値の解釈結果
                const auto defaultValue = nlohmann::json::parse(
                    property.defaultValue,
                    nullptr,
                    false);
                if (defaultValue.is_discarded())
                {
                    return fail(context
                        + " のdefault JSONが不正です。");
                }

                // 4成分までの既定値
                std::array<float, 4> values{};
                if (field.kind == ShaderPropertyKind::Float)
                {
                    if (!defaultValue.is_number())
                    {
                        return fail(context
                            + " のfloat defaultは数値で指定してください。");
                    }
                    values[0] = defaultValue.get<float>();
                }
                else if (field.kind == ShaderPropertyKind::Boolean)
                {
                    if (!defaultValue.is_boolean())
                    {
                        return fail(context
                            + " のbool defaultは真偽値で指定してください。");
                    }
                    values[0] = defaultValue.get<bool>()
                        ? 1.0f
                        : 0.0f;
                }
                else if (field.kind == ShaderPropertyKind::Color
                    || field.kind == ShaderPropertyKind::Vector)
                {
                    if (!defaultValue.is_array()
                        || defaultValue.size() != field.componentCount)
                    {
                        return fail(context
                            + " のdefault配列とtargetの成分数が一致しません。");
                    }
                    // 指定文字または参照する成分番号
                    for (std::size_t component = 0;
                        component < field.componentCount;
                        ++component)
                    {
                        if (!defaultValue.at(component).is_number())
                        {
                            return fail(context
                                + " のdefault配列には数値だけを指定して"
                                  "ください。");
                        }
                        values[component] =
                            defaultValue.at(component).get<float>();
                    }
                }
                // 既定値は全成分を有限のfloatへ変換できる範囲に限る(value: 検証する成分値)。
                if (!std::all_of(
                        values.begin(),
                        values.end(),
                        [](const float value)
                        {
                            return std::isfinite(value);
                        }))
                {
                    return fail(context
                        + " のdefaultはfloatの範囲内で指定してください。");
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
        // 宣言の有無・入力情報・失敗理由
        ShaderProperties result;
        if (shaderPath.empty())
        {
            return result;
        }
        // HLSL文書を読み込むstream
        std::ifstream input(shaderPath, std::ios::binary);
        if (!input)
        {
            return result;
        }
        // 読込したHLSL文書のstream
        std::ostringstream contents;
        contents << input.rdbuf();
        return ParseShaderProperties(contents.str());
    }
}
