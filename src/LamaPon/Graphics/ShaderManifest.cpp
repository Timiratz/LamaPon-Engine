#include "LamaPon/Graphics/ShaderManifest.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace
{
    using Json = nlohmann::json;

    struct StageField final
    {
        // JSONのステージキー名
        const char* name;
        // 対応するステージ種別
        // 確認するステージ記述
        LamaPon::ShaderStage stage;
    };

    // 名前とステージ種別の対応表
    constexpr std::array<StageField, 6> StageFields{
        StageField{ "vertex", LamaPon::ShaderStage::Vertex },
        StageField{ "pixel", LamaPon::ShaderStage::Pixel },
        StageField{ "geometry", LamaPon::ShaderStage::Geometry },
        StageField{ "hull", LamaPon::ShaderStage::Hull },
        StageField{ "domain", LamaPon::ShaderStage::Domain },
        StageField{ "compute", LamaPon::ShaderStage::Compute }
    };

    // ステージ名から固定表の記述を借用する(name: 確認するステージ名)。
    [[nodiscard]] const StageField* FindStageField(
        const std::string_view name) noexcept
    {
        // ステージ名と種別の対応記述
        for (const auto& field : StageFields)
        {
            if (name == field.name)
            {
                return &field;
            }
        }
        return nullptr;
    }

    // ステージ種別のコンパイル形式接頭辞を返す(stage: ステージ種別)。
    [[nodiscard]] const char* TargetPrefix(
        const LamaPon::ShaderStage stage) noexcept
    {
        switch (stage)
        {
        case LamaPon::ShaderStage::Vertex:
            return "vs_";
        case LamaPon::ShaderStage::Pixel:
            return "ps_";
        case LamaPon::ShaderStage::Geometry:
            return "gs_";
        case LamaPon::ShaderStage::Hull:
            return "hs_";
        case LamaPon::ShaderStage::Domain:
            return "ds_";
        case LamaPon::ShaderStage::Compute:
            return "cs_";
        }
        return "";
    }

    // ステージ種別の診断用名称を返す(stage: ステージ種別)。
    [[nodiscard]] const char* StageName(
        const LamaPon::ShaderStage stage) noexcept
    {
        // ステージ名と種別の対応記述
        for (const auto& field : StageFields)
        {
            if (field.stage == stage)
            {
                return field.name;
            }
        }
        return "unknown";
    }

    // ASCIIの大文字だけを小文字へ変換する(value: 入力文字列)。
    [[nodiscard]] std::string FoldAsciiLower(
        const std::string_view value)
    {
        // ASCIIを小文字化する文字列
        std::string folded;
        folded.reserve(value.size());
        // 変換または確認する一文字
        for (const char character : value)
        {
            folded.push_back(
                character >= 'A' && character <= 'Z'
                ? static_cast<char>(character + ('a' - 'A'))
                : character);
        }
        return folded;
    }

    // 必須の非空文字列を読み診断を返す(object: 読込元JSON, field: キー名, value: 出力文字列, error: 出力診断, context: 診断用の項目名)。
    [[nodiscard]] bool ReadRequiredString(
        const Json& object,
        const char* field,
        std::string& value,
        std::string& error,
        const std::string& context)
    {
        // JSON項目の参照位置
        const auto found = object.find(field);
        if (found == object.end() || !found->is_string())
        {
            error = context + "." + field
                + " must be a string.";
            return false;
        }
        value = found->get<std::string>();
        if (value.empty())
        {
            error = context + "." + field
                + " must not be empty.";
            return false;
        }
        return true;
    }

    // 指定された文字列だけ読み、未指定なら出力を保持する(object: 読込元JSON, field: キー名, value: 出力文字列, error: 出力診断, context: 診断用の項目名)。
    [[nodiscard]] bool ReadOptionalString(
        const Json& object,
        const char* field,
        std::string& value,
        std::string& error,
        const std::string& context)
    {
        // JSON項目の参照位置
        const auto found = object.find(field);
        if (found == object.end())
        {
            return true;
        }
        if (!found->is_string())
        {
            error = context + "." + field
                + " must be a string.";
            return false;
        }
        value = found->get<std::string>();
        return true;
    }

    // 指定された真偽値だけ読み、未指定なら出力を保持する(object: 読込元JSON, field: キー名, value: 出力真偽値, error: 出力診断, context: 診断用の項目名)。
    [[nodiscard]] bool ReadOptionalBool(
        const Json& object,
        const char* field,
        bool& value,
        std::string& error,
        const std::string& context)
    {
        // JSON項目の参照位置
        const auto found = object.find(field);
        if (found == object.end())
        {
            return true;
        }
        if (!found->is_boolean())
        {
            error = context + "." + field
                + " must be a boolean.";
            return false;
        }
        value = found->get<bool>();
        return true;
    }

    // 資産用途名を検証して種別を出力する(name: 資産用途名, type: 出力する用途種別, error: 出力診断)。
    [[nodiscard]] bool ParseAssetType(
        const std::string_view name,
        LamaPon::ShaderAssetType& type,
        std::string& error)
    {
        if (name == "material")
        {
            type = LamaPon::ShaderAssetType::Material;
            return true;
        }
        if (name == "screenEffect")
        {
            type = LamaPon::ShaderAssetType::ScreenEffect;
            return true;
        }
        if (name == "compute")
        {
            type = LamaPon::ShaderAssetType::Compute;
            return true;
        }
        error = "Shader manifest type must be one of material, "
            "screenEffect, or compute (received '"
            + std::string{ name } + "').";
        return false;
    }

    // 描画パスのrole名を検証して出力する(name: 描画種別名, role: 出力する描画種別, error: 出力診断, context: 診断用の項目名)。
    [[nodiscard]] bool ParsePassRole(
        const std::string_view name,
        LamaPon::ShaderPassRole& role,
        std::string& error,
        const std::string& context)
    {
        if (name == "forward")
        {
            role = LamaPon::ShaderPassRole::Forward;
            return true;
        }
        if (name == "skinned")
        {
            role = LamaPon::ShaderPassRole::Skinned;
            return true;
        }
        if (name == "instanced")
        {
            role = LamaPon::ShaderPassRole::Instanced;
            return true;
        }
        if (name == "outline")
        {
            role = LamaPon::ShaderPassRole::Outline;
            return true;
        }
        if (name == "skinnedOutline")
        {
            role = LamaPon::ShaderPassRole::SkinnedOutline;
            return true;
        }
        if (name == "occluded")
        {
            role = LamaPon::ShaderPassRole::Occluded;
            return true;
        }
        error = context + ".role must be one of forward, skinned, "
            "instanced, outline, skinnedOutline, or occluded "
            "(received '" + std::string{ name } + "').";
        return false;
    }

    // 追加パラメーターの個数
    constexpr std::size_t ShaderPropertyParameterCount = 8;
    // 追加画像の先頭スロット番号
    constexpr std::size_t ShaderPropertyTextureFirstSlot = 7;
    // 追加画像スロットの個数
    constexpr std::size_t ShaderPropertyTextureCount = 4;

    struct ParsedPropertyTarget final
    {
        // 割当の明示指定有無
        bool present{};
        // 画像スロットの割当フラグ
        bool texture{};
        // 追加画像か定数の配列番号
        // 確認する配列要素の番号
        std::size_t index{};
        // 割当する定数の成分番号
        std::array<std::size_t, 4> components{};
        // 割当する成分数
        std::size_t componentCount{};
    };

    // 桁と範囲を検証して符号なし整数を読む(text: 数値文字列, value: 出力整数)。
    [[nodiscard]] bool ParseUnsigned(
        const std::string_view text,
        std::size_t& value) noexcept
    {
        if (text.empty())
        {
            return false;
        }
        value = 0;
        // 変換または確認する一文字
        for (const unsigned char character : text)
        {
            if (std::isdigit(character) == 0)
            {
                return false;
            }
            // 確認した十進数の一桁
            const auto digit = static_cast<std::size_t>(
                character - static_cast<unsigned char>('0'));
            if (value > (
                    std::numeric_limits<std::size_t>::max() - digit)
                    / 10)
            {
                return false;
            }
            value = value * 10 + digit;
        }
        return true;
    }

    // プロパティの型名が対応済みか返す(type: 確認する型名)。
    [[nodiscard]] bool IsSupportedPropertyType(
        const std::string_view type) noexcept
    {
        return type == "float"
            || type == "color"
            || type == "bool"
            || type == "vector"
            || type == "texture";
    }

    // プロパティの定数成分または画像割当を検証する(value: プロパティJSON, property: 出力プロパティ記述, parsed: 出力する割当情報, error: 出力診断, context: 診断用の項目名)。
    [[nodiscard]] bool ParsePropertyTarget(
        const Json& value,
        LamaPon::ShaderPropertyDesc& property,
        ParsedPropertyTarget& parsed,
        std::string& error,
        const std::string& context)
    {
        // JSON項目の参照位置
        const auto found = value.find("target");
        if (found == value.end())
        {
            // 未指定の割当は解析時に補完しない。
            return true;
        }
        if (!found->is_string())
        {
            error = context + ".target must be a string.";
            return false;
        }

        property.target = found->get<std::string>();
        if (property.target.empty())
        {
            error = context + ".target must not be empty when specified.";
            return false;
        }
        parsed.present = true;

        if (property.type == "texture")
        {
            if (property.target.size() < 2
                || (property.target.front() != 't'
                    && property.target.front() != 'T'))
            {
                error = context
                    + ".target for type 'texture' must be t7, t8, t9, "
                      "or t10 (received '"
                    + property.target + "').";
                return false;
            }
            // 画像のレジスタ番号
            std::size_t slot{};
            if (!ParseUnsigned(
                    std::string_view{ property.target }.substr(1),
                    slot)
                || slot < ShaderPropertyTextureFirstSlot
                || slot >= ShaderPropertyTextureFirstSlot
                    + ShaderPropertyTextureCount)
            {
                error = context
                    + ".target for type 'texture' must be t7, t8, t9, "
                      "or t10 (received '"
                    + property.target + "').";
                return false;
            }
            parsed.texture = true;
            parsed.index = slot - ShaderPropertyTextureFirstSlot;
            parsed.componentCount = 1;
            return true;
        }

        if (property.target.front() == 't'
            || property.target.front() == 'T')
        {
            error = context + ".target '" + property.target
                + "' is a texture slot, but type is '" + property.type
                + "'.";
            return false;
        }

        // 定数番号と成分の区切り位置
        const auto dot = property.target.find('.');
        // 定数配列番号の文字列
        const auto indexText = std::string_view{ property.target }.substr(
            0,
            dot);
        // 追加定数配列の番号
        std::size_t parameterIndex{};
        if (!ParseUnsigned(indexText, parameterIndex))
        {
            error = context + ".target has an invalid parameter index: '"
                + property.target + "'.";
            return false;
        }
        if (parameterIndex >= ShaderPropertyParameterCount)
        {
            error = context
                + ".target parameter index must be between 0 and 7 "
                  "(received '"
                + property.target + "').";
            return false;
        }
        parsed.index = parameterIndex;

        // 定数成分の選択文字列
        const auto swizzle = dot == std::string::npos
            ? std::string_view{ "x" }
            : std::string_view{ property.target }.substr(dot + 1);
        if (swizzle.empty() || swizzle.size() > 4)
        {
            error = context
                + ".target component selection must contain 1 to 4 "
                  "components: '"
                + property.target + "'.";
            return false;
        }

        // 成分指定の重複確認フラグ
        std::array<bool, 4> componentsUsed{};
        // 確認する成分指定文字
        for (const char component : swizzle)
        {
            // 定数の成分番号
            std::size_t componentIndex{};
            switch (component)
            {
            case 'x': case 'r': componentIndex = 0; break;
            case 'y': case 'g': componentIndex = 1; break;
            case 'z': case 'b': componentIndex = 2; break;
            case 'w': case 'a': componentIndex = 3; break;
            default:
                error = context
                    + ".target components must use x/y/z/w or r/g/b/a "
                      "(received '"
                    + property.target + "').";
                return false;
            }
            if (componentsUsed[componentIndex])
            {
                error = context + ".target repeats the same component: '"
                    + property.target + "'.";
                return false;
            }
            componentsUsed[componentIndex] = true;
            parsed.components[parsed.componentCount++] = componentIndex;
        }

        // 型に適した成分数の有無
        const bool validComponentCount =
            (property.type == "float" || property.type == "bool")
                ? parsed.componentCount == 1
                : (property.type == "color"
                    ? parsed.componentCount == 3
                        || parsed.componentCount == 4
                    : parsed.componentCount >= 2
                        && parsed.componentCount <= 4);
        if (!validComponentCount)
        {
            // 診断用の必要成分数
            const auto expected = property.type == "color"
                ? "3 or 4"
                : (property.type == "vector" ? "2 to 4" : "exactly 1");
            error = context + ".target for type '" + property.type
                + "' must select " + expected + " component(s) "
                  "(received '"
                + property.target + "').";
            return false;
        }
        return true;
    }

    // 有限でfloatに収まるJSON数値を読む(value: 入力JSON値, number: 出力数値, error: 出力診断, context: 診断用の項目名)。
    [[nodiscard]] bool ReadFiniteFloatNumber(
        const Json& value,
        double& number,
        std::string& error,
        const std::string& context)
    {
        if (!value.is_number())
        {
            error = context + " must be a JSON number.";
            return false;
        }
        number = value.get<double>();
        // float型の最大絶対値
        constexpr auto floatLimit =
            static_cast<double>(std::numeric_limits<float>::max());
        if (!std::isfinite(number)
            || number < -floatLimit
            || number > floatLimit)
        {
            error = context
                + " must be a finite 32-bit floating-point value.";
            return false;
        }
        return true;
    }

    // 既定値の型と成分数を検証する(value: 既定値JSON, property: プロパティ記述, target: 解析済みの割当情報, error: 出力診断, context: 診断用の項目名)。
    [[nodiscard]] bool ValidatePropertyDefault(
        const Json& value,
        const LamaPon::ShaderPropertyDesc& property,
        const ParsedPropertyTarget& target,
        std::string& error,
        const std::string& context)
    {
        if (property.type == "float")
        {
            // 有限性の検証に使う数値
            double ignored{};
            if (!ReadFiniteFloatNumber(
                    value,
                    ignored,
                    error,
                    context + ".default"))
            {
                error = context
                    + ".default for type 'float' must be a finite JSON "
                      "number.";
                return false;
            }
            return true;
        }
        if (property.type == "bool")
        {
            if (!value.is_boolean())
            {
                error = context
                    + ".default for type 'bool' must be a JSON boolean.";
                return false;
            }
            return true;
        }
        if (property.type == "texture")
        {
            error = context
                + ".default is not supported for type 'texture'; an "
                  "unassigned slot uses the engine's white texture.";
            return false;
        }

        if (!value.is_array())
        {
            error = context + ".default for type '" + property.type
                + "' must be an array of JSON numbers.";
            return false;
        }
        // 既定値に必要な成分数
        const auto expectedCount = target.present
            ? target.componentCount
            : value.size();
        // 既定値の成分数の適合結果
        const bool validCount = target.present
            ? value.size() == expectedCount
            : (property.type == "color"
                ? value.size() == 3 || value.size() == 4
                : value.size() >= 2 && value.size() <= 4);
        if (!validCount)
        {
            error = context + ".default for type '" + property.type
                + "' must contain "
                + (target.present
                    ? std::to_string(expectedCount)
                    : (property.type == "color" ? "3 or 4" : "2 to 4"))
                + " number(s).";
            return false;
        }
        // 確認する配列要素の番号
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            // 有限性の検証に使う数値
            double ignored{};
            if (!ReadFiniteFloatNumber(
                    value.at(index),
                    ignored,
                    error,
                    context + ".default["
                        + std::to_string(index) + "]"))
            {
                return false;
            }
        }
        return true;
    }

    // プロパティの型・値・割当の重複を検証する(document: 入力JSON文書, properties: 出力プロパティ一覧, error: 出力診断)。
    [[nodiscard]] bool ParseProperties(
        const Json& document,
        std::vector<LamaPon::ShaderPropertyDesc>& properties,
        std::string& error)
    {
        // JSON項目の参照位置
        const auto found = document.find("properties");
        if (found == document.end())
        {
            return true;
        }
        if (!found->is_array())
        {
            error = "Shader manifest properties must be an array.";
            return false;
        }

        // 追加定数成分の使用済みフラグ
        std::array<
            std::array<bool, 4>,
            ShaderPropertyParameterCount> parameterComponentsUsed{};
        // 画像スロットの使用済みフラグ
        std::array<bool, ShaderPropertyTextureCount> textureSlotsUsed{};

        properties.reserve(found->size());
        // 確認する配列要素の番号
        for (std::size_t index = 0; index < found->size(); ++index)
        {
            // 解析するプロパティJSON
            const auto& value = found->at(index);
            // 診断用の対象項目名
            const auto context = "Shader manifest properties["
                + std::to_string(index) + "]";
            if (!value.is_object())
            {
                error = context + " must be an object.";
                return false;
            }

            // 作成中のプロパティ記述
            LamaPon::ShaderPropertyDesc property;
            if (!ReadRequiredString(
                    value,
                    "name",
                    property.name,
                    error,
                    context)
                || !ReadRequiredString(
                    value,
                    "type",
                    property.type,
                    error,
                    context))
            {
                return false;
            }
            if (!IsSupportedPropertyType(property.type))
            {
                error = context
                    + ".type must be one of float, color, bool, vector, "
                      "or texture (received '"
                    + property.type + "').";
                return false;
            }

            // 解析済みのプロパティ割当
            ParsedPropertyTarget parsedTarget;
            if (!ParsePropertyTarget(
                    value,
                    property,
                    parsedTarget,
                    error,
                    context))
            {
                return false;
            }
            if (parsedTarget.present)
            {
                if (parsedTarget.texture)
                {
                    if (textureSlotsUsed[parsedTarget.index])
                    {
                        error = context + ".target duplicates texture slot '"
                            + property.target + "'.";
                        return false;
                    }
                    textureSlotsUsed[parsedTarget.index] = true;
                }
                else
                {
                    // 割当成分の配列内番号
                    for (std::size_t component = 0;
                        component < parsedTarget.componentCount;
                        ++component)
                    {
                        // 追加定数の成分番号
                        const auto componentIndex =
                            parsedTarget.components[component];
                        if (parameterComponentsUsed[
                                parsedTarget.index][componentIndex])
                        {
                            error = context
                                + ".target overlaps a constant component "
                                  "already used by another property: '"
                                + property.target + "'.";
                            return false;
                        }
                        parameterComponentsUsed[
                            parsedTarget.index][componentIndex] = true;
                    }
                }
            }

            // 既定値JSONの参照位置
            if (const auto defaultValue = value.find("default");
                defaultValue != value.end())
            {
                if (!ValidatePropertyDefault(
                        *defaultValue,
                        property,
                        parsedTarget,
                        error,
                        context))
                {
                    return false;
                }
                property.defaultValue = defaultValue->dump();
            }

            // 下限JSONの参照位置
            const auto minimum = value.find("min");
            // 上限JSONの参照位置
            const auto maximum = value.find("max");
            if ((minimum == value.end()) != (maximum == value.end()))
            {
                error = context
                    + ".min and .max must be specified together.";
                return false;
            }
            if (minimum != value.end())
            {
                if (property.type != "float")
                {
                    error = context
                        + ".min and .max are only supported for type "
                          "'float'.";
                    return false;
                }
                // 検証する数値下限
                double minimumValue{};
                // 検証する数値上限
                double maximumValue{};
                if (!ReadFiniteFloatNumber(
                        *minimum,
                        minimumValue,
                        error,
                        context + ".min")
                    || !ReadFiniteFloatNumber(
                        *maximum,
                        maximumValue,
                        error,
                        context + ".max"))
                {
                    return false;
                }
                if (maximumValue <= minimumValue)
                {
                    error = context
                        + ".max must be greater than .min.";
                    return false;
                }
                if (!property.defaultValue.empty())
                {
                    // 範囲を検証する既定値
                    const double defaultNumber =
                        value.at("default").get<double>();
                    if (defaultNumber < minimumValue
                        || defaultNumber > maximumValue)
                    {
                        error = context
                            + ".default must be within the inclusive "
                              ".min/.max range.";
                        return false;
                    }
                }
                property.minimum = minimumValue;
                property.maximum = maximumValue;
            }
            properties.push_back(std::move(property));
        }
        return true;
    }

    // 描画状態の指定値と深度条件を検証する(passValue: パスのJSON, state: 出力描画状態, error: 出力診断, context: 診断用の項目名)。
    [[nodiscard]] bool ParseRenderState(
        const Json& passValue,
        LamaPon::RenderStateDesc& state,
        std::string& error,
        const std::string& context)
    {
        // JSON項目の参照位置
        const auto found = passValue.find("renderState");
        if (found == passValue.end())
        {
            return true;
        }
        if (!found->is_object())
        {
            error = context + ".renderState must be an object.";
            return false;
        }

        // 描画状態の診断用項目名
        const auto stateContext = context + ".renderState";
        if (!(ReadOptionalString(
                *found,
                "zTest",
                state.zTest,
                error,
                stateContext)
            && ReadOptionalBool(
                *found,
                "zWrite",
                state.zWrite,
                error,
                stateContext)
            && ReadOptionalString(
                *found,
                "cull",
                state.cull,
                error,
                stateContext)
            && ReadOptionalString(
                *found,
                "blend",
                state.blend,
                error,
                stateContext)))
        {
            return false;
        }

        // 小文字化した深度比較方式
        const auto zTest = FoldAsciiLower(state.zTest);
        if (zTest != "lessequal" && zTest != "always")
        {
            error = stateContext
                + ".zTest must be LessEqual or Always (received '"
                + state.zTest + "').";
            return false;
        }
        // 小文字化したカリング方式
        const auto cull = FoldAsciiLower(state.cull);
        if (cull != "back"
            && cull != "front"
            && cull != "off"
            && cull != "none")
        {
            error = stateContext
                + ".cull must be Back, Front, Off, or None (received '"
                + state.cull + "').";
            return false;
        }
        // 小文字化した合成方式
        const auto blend = FoldAsciiLower(state.blend);
        if (blend != "opaque"
            && blend != "alpha"
            && blend != "additive"
            && blend != "premultiplied")
        {
            error = stateContext
                + ".blend must be Opaque, Alpha, Additive, or "
                  "Premultiplied (received '"
                + state.blend + "').";
            return false;
        }
        if (zTest == "always" && state.zWrite)
        {
            error = stateContext
                + " zTest=Always requires zWrite=false; the current "
                  "render-state model cannot safely apply Always with "
                  "depth writes enabled.";
            return false;
        }
        return true;
    }

    // ステージの入口と形式接頭辞を検証する(value: ステージJSON, field: ステージ種別記述, stage: 出力ステージ記述, error: 出力診断, context: 診断用の項目名)。
    [[nodiscard]] bool ParseStage(
        const Json& value,
        const StageField& field,
        LamaPon::ShaderStageDesc& stage,
        std::string& error,
        const std::string& context)
    {
        if (!value.is_object())
        {
            error = context + " must be an object.";
            return false;
        }

        stage.stage = field.stage;
        if (!ReadRequiredString(
                value,
                "entry",
                stage.entryPoint,
                error,
                context)
            || !ReadRequiredString(
                value,
                "target",
                stage.target,
                error,
                context)
            || !ReadOptionalBool(
                value,
                "optional",
                stage.optional,
                error,
                context))
        {
            return false;
        }

        // 宣言されたコンパイル形式
        const std::string_view target{ stage.target };
        // ステージの形式接頭辞
        const std::string_view prefix{ TargetPrefix(stage.stage) };
        if (!target.starts_with(prefix))
        {
            error = context + ".target must use the '"
                + std::string{ prefix }
                + "' profile prefix for the " + field.name
                + " stage (received '" + stage.target + "').";
            return false;
        }
        return true;
    }

    // 各パスのroleとステージと描画状態を読む(document: 入力JSON文書, assetType: シェーダー資産の用途, passes: 出力描画パス一覧, error: 出力診断)。
    [[nodiscard]] bool ParsePasses(
        const Json& document,
        const LamaPon::ShaderAssetType assetType,
        std::vector<LamaPon::ShaderPassDesc>& passes,
        std::string& error)
    {
        // JSON項目の参照位置
        const auto found = document.find("passes");
        if (found == document.end()
            || !found->is_array()
            || found->empty())
        {
            error = "Shader manifest passes must be a non-empty array.";
            return false;
        }

        passes.reserve(found->size());
        // 確認する配列要素の番号
        for (std::size_t index = 0; index < found->size(); ++index)
        {
            // 解析する描画パスJSON
            const auto& value = found->at(index);
            // 診断用の対象項目名
            const auto context = "Shader manifest passes["
                + std::to_string(index) + "]";
            if (!value.is_object())
            {
                error = context + " must be an object.";
                return false;
            }

            // 確認する描画パス記述
            LamaPon::ShaderPassDesc pass;
            if (!ReadOptionalString(
                    value,
                    "name",
                    pass.name,
                    error,
                    context)
                || !ParseRenderState(
                    value,
                    pass.renderState,
                    error,
                    context))
            {
                return false;
            }

            // 描画種別JSONの参照位置
            if (const auto roleValue = value.find("role");
                roleValue != value.end())
            {
                if (assetType != LamaPon::ShaderAssetType::Material)
                {
                    error = context
                        + ".role is only supported for material shader "
                        "passes.";
                    return false;
                }
                if (!roleValue->is_string())
                {
                    error = context + ".role must be a string.";
                    return false;
                }
                if (!ParsePassRole(
                        roleValue->get<std::string>(),
                        pass.role,
                        error,
                        context))
                {
                    return false;
                }
            }

            // ステージJSON項目の反復位置
            for (auto member = value.begin();
                member != value.end();
                ++member)
            {
                if (member.key() == "name"
                    || member.key() == "role"
                    || member.key() == "renderState")
                {
                    continue;
                }
                // ステージ名と種別の対応記述
                const auto* field = FindStageField(member.key());
                if (field == nullptr)
                {
                    error = context + " contains unknown stage '"
                        + member.key() + "'.";
                    return false;
                }

                // 確認するステージ記述
                LamaPon::ShaderStageDesc stage;
                if (!ParseStage(
                        member.value(),
                        *field,
                        stage,
                        error,
                        context + "." + member.key()))
                {
                    return false;
                }
                pass.stages.push_back(std::move(stage));
            }
            passes.push_back(std::move(pass));
        }
        return true;
    }

    // 用途に必須のステージと同時指定の条件を検証する(description: シェーダー資産記述, error: 出力診断)。
    [[nodiscard]] bool ValidateRequiredStages(
        const LamaPon::ShaderAssetDesc& description,
        std::string& error)
    {
        // 確認する配列要素の番号
        for (std::size_t index = 0;
            index < description.passes.size();
            ++index)
        {
            // 確認する描画パス記述
            const auto& pass = description.passes[index];
            if (description.type == LamaPon::ShaderAssetType::Material
                && pass.role == LamaPon::ShaderPassRole::Occluded)
            {
                continue;
            }
            // ハルステージの指定有無
            const bool hasHull = LamaPon::FindShaderStage(
                pass,
                LamaPon::ShaderStage::Hull) != nullptr;
            // ドメインステージの指定有無
            const bool hasDomain = LamaPon::FindShaderStage(
                pass,
                LamaPon::ShaderStage::Domain) != nullptr;
            if (hasHull != hasDomain)
            {
                error = "Shader manifest passes["
                    + std::to_string(index)
                    + "] must declare hull and domain stages together.";
                return false;
            }
            if (hasHull)
            {
                // 借用するハルステージ記述
                const auto* const hull = LamaPon::FindShaderStage(
                    pass,
                    LamaPon::ShaderStage::Hull);
                // 借用するドメインステージ記述
                const auto* const domain = LamaPon::FindShaderStage(
                    pass,
                    LamaPon::ShaderStage::Domain);
                if (hull->optional != domain->optional)
                {
                    error = "Shader manifest passes["
                        + std::to_string(index)
                        + "] hull and domain stages must use the same "
                        "optional value.";
                    return false;
                }
            }
        }

        if (description.type == LamaPon::ShaderAssetType::Material)
        {
            // forwardパスの指定有無
            bool hasForward = false;
            // 確認する配列要素の番号
            for (std::size_t index = 0;
                index < description.passes.size();
                ++index)
            {
                // 確認する描画パス記述
                const auto& pass = description.passes[index];
                hasForward = hasForward
                    || pass.role == LamaPon::ShaderPassRole::Forward;
                if (LamaPon::FindShaderStage(
                        pass,
                        LamaPon::ShaderStage::Compute) != nullptr)
                {
                    error = "Shader manifest material passes["
                        + std::to_string(index)
                        + "] must not declare a compute stage.";
                    return false;
                }

                // 診断用の対象項目名
                const auto context = "Shader manifest material passes["
                    + std::to_string(index) + "]";
                if (pass.role == LamaPon::ShaderPassRole::Occluded)
                {
                    // 遮蔽パスで禁止するステージ
                    for (const auto prohibited : {
                        LamaPon::ShaderStage::Vertex,
                        LamaPon::ShaderStage::Geometry,
                        LamaPon::ShaderStage::Hull,
                        LamaPon::ShaderStage::Domain })
                    {
                        if (LamaPon::FindShaderStage(pass, prohibited)
                            != nullptr)
                        {
                            error = context + " with role 'occluded' may "
                                "only declare a pixel stage; it reuses the "
                                "active primary vertex pipeline.";
                            return false;
                        }
                    }
                    // 借用するピクセル段階の記述
                    const auto* pixel = LamaPon::FindShaderStage(
                        pass,
                        LamaPon::ShaderStage::Pixel);
                    if (pixel == nullptr)
                    {
                        error = context
                            + " with role 'occluded' requires a pixel stage.";
                        return false;
                    }
                    if (pixel->optional)
                    {
                        error = context + " with role 'occluded' pixel "
                            "stage must not be optional.";
                        return false;
                    }
                    continue;
                }

                // 借用する頂点段階の記述
                const auto* vertex = LamaPon::FindShaderStage(
                    pass,
                    LamaPon::ShaderStage::Vertex);
                if (vertex == nullptr)
                {
                    error = context + " requires a vertex stage.";
                    return false;
                }
                if (vertex->optional)
                {
                    error = context
                        + " vertex stage must not be optional.";
                    return false;
                }

                // 借用するピクセル段階の記述
                const auto* pixel = LamaPon::FindShaderStage(
                    pass,
                    LamaPon::ShaderStage::Pixel);
                if (pixel == nullptr)
                {
                    error = context + " requires a pixel stage.";
                    return false;
                }
                if (pixel->optional)
                {
                    error = context
                        + " pixel stage must not be optional.";
                    return false;
                }
            }
            if (!hasForward)
            {
                error = "Shader manifest material requires at least one "
                    "pass with role 'forward'.";
                return false;
            }
        }
        else if (description.type
            == LamaPon::ShaderAssetType::ScreenEffect)
        {
            // 先頭の描画パス記述
            const auto& firstPass = description.passes.front();
            // 借用する頂点段階の記述
            const auto* vertex = LamaPon::FindShaderStage(
                firstPass,
                LamaPon::ShaderStage::Vertex);
            if (vertex == nullptr)
            {
                error = "Shader manifest screenEffect first pass "
                    "requires a vertex stage.";
                return false;
            }
            if (vertex->optional)
            {
                error = "Shader manifest screenEffect first pass "
                    "vertex stage must not be optional.";
                return false;
            }

            // 借用するピクセル段階の記述
            const auto* pixel = LamaPon::FindShaderStage(
                firstPass,
                LamaPon::ShaderStage::Pixel);
            if (pixel == nullptr)
            {
                error = "Shader manifest screenEffect first pass "
                    "requires a pixel stage.";
                return false;
            }
            if (pixel->optional)
            {
                error = "Shader manifest screenEffect first pass "
                    "pixel stage must not be optional.";
                return false;
            }
        }
        else if (description.type == LamaPon::ShaderAssetType::Compute)
        {
            // 必須compute段階の有無
            bool hasRequiredCompute{};
            // 確認する配列要素の番号
            for (std::size_t index = 0;
                index < description.passes.size();
                ++index)
            {
                // 確認する描画パス記述
                const auto& pass = description.passes[index];
                // 借用するcompute段階の記述
                const auto* compute = LamaPon::FindShaderStage(
                    pass,
                    LamaPon::ShaderStage::Compute);
                if (compute == nullptr)
                {
                    continue;
                }
                // 確認するステージ記述
                for (const auto& stage : pass.stages)
                {
                    if (stage.stage == LamaPon::ShaderStage::Compute)
                    {
                        continue;
                    }
                    error = "Shader manifest compute passes["
                        + std::to_string(index)
                        + "] contains unsupported "
                        + StageName(stage.stage)
                        + " stage; a compute pass may contain only a "
                        "compute stage.";
                    return false;
                }
                hasRequiredCompute = hasRequiredCompute
                    || !compute->optional;
            }
            if (hasRequiredCompute)
            {
                return true;
            }
            error = "Shader manifest compute asset requires a "
                "non-optional compute stage.";
            return false;
        }
        return true;
    }
}

namespace LamaPon
{
    const ShaderStageDesc* FindShaderStage(
        const ShaderPassDesc& pass,
        const ShaderStage stage) noexcept
    {
        // 種別を比較するステージ記述
        for (const auto& candidate : pass.stages)
        {
            if (candidate.stage == stage)
            {
                return &candidate;
            }
        }
        return nullptr;
    }

    bool IsShaderManifestPath(
        const std::filesystem::path& path) noexcept
    {
        using Character = std::filesystem::path::value_type;
        // 判定する複合拡張子
        constexpr Character suffix[]{
            static_cast<Character>('.'),
            static_cast<Character>('l'),
            static_cast<Character>('a'),
            static_cast<Character>('m'),
            static_cast<Character>('a'),
            static_cast<Character>('s'),
            static_cast<Character>('h'),
            static_cast<Character>('a'),
            static_cast<Character>('d'),
            static_cast<Character>('e'),
            static_cast<Character>('r'),
            static_cast<Character>('.'),
            static_cast<Character>('j'),
            static_cast<Character>('s'),
            static_cast<Character>('o'),
            static_cast<Character>('n')
        };
        // 複合拡張子の文字数
        constexpr auto suffixLength = std::size(suffix);
        // OS固有形式のパス文字列
        const auto& value = path.native();
        if (value.size() < suffixLength)
        {
            return false;
        }

        // ASCIIの大文字だけ小文字へ変換する(character: 確認する文字)。
        const auto foldAscii = [](const Character character) noexcept
        {
            // ASCII大文字の先頭
            constexpr auto upperA = static_cast<Character>('A');
            // ASCII大文字の末尾
            constexpr auto upperZ = static_cast<Character>('Z');
            // ASCII小文字への文字差
            constexpr auto lowerDifference =
                static_cast<Character>('a' - 'A');
            return character >= upperA && character <= upperZ
                ? static_cast<Character>(
                    character + lowerDifference)
                : character;
        };
        // パス内の拡張子開始位置
        const auto offset = value.size() - suffixLength;
        // 確認する配列要素の番号
        for (std::size_t index = 0; index < suffixLength; ++index)
        {
            if (foldAscii(value[offset + index]) != suffix[index])
            {
                return false;
            }
        }
        return true;
    }

    bool ParseShaderAssetDesc(
        const std::string_view jsonText,
        ShaderAssetDesc& outDesc,
        std::string& error)
    {
        error.clear();

        try
        {
            // 解析したシェーダーJSON文書
            const auto document = Json::parse(
                jsonText.begin(),
                jsonText.end());
            if (!document.is_object())
            {
                error = "Shader manifest root must be an object.";
                return false;
            }

            // 作成中のシェーダー資産記述
            ShaderAssetDesc description;
            // 版番号JSONの参照位置
            const auto version = document.find("version");
            if (version == document.end()
                || !version->is_number_integer())
            {
                error = "Shader manifest version must be the integer 1.";
                return false;
            }
            if (*version != 1)
            {
                error = "Unsupported shader manifest version "
                    + version->dump()
                    + "; only version 1 is supported.";
                return false;
            }
            description.version = 1;

            if (!ReadRequiredString(
                    document,
                    "name",
                    description.name,
                    error,
                    "Shader manifest"))
            {
                return false;
            }

            // シェーダー資産の用途名
            std::string assetType;
            if (!ReadRequiredString(
                    document,
                    "type",
                    assetType,
                    error,
                    "Shader manifest")
                || !ParseAssetType(
                    assetType,
                    description.type,
                    error))
            {
                return false;
            }

            // HLSLソースのパス文字列
            std::string source;
            if (!ReadRequiredString(
                    document,
                    "source",
                    source,
                    error,
                    "Shader manifest"))
            {
                return false;
            }
            // 検証する資産ルート相対パス
            const auto sourcePath = PathFromUtf8(source);
            if (sourcePath.has_root_name()
                || sourcePath.has_root_directory()
                || sourcePath.is_absolute())
            {
                error = "Shader manifest source must be an "
                    "asset-root-relative path.";
                return false;
            }
            // HLSLパスの構成要素
            for (const auto& part : sourcePath)
            {
                if (part == L"..")
                {
                    error = "Shader manifest source must not contain "
                        "'..' path components.";
                    return false;
                }
            }
            description.source = sourcePath.lexically_normal();

            if (!ParseProperties(
                    document,
                    description.properties,
                    error)
                || !ParsePasses(
                    document,
                    description.type,
                    description.passes,
                    error)
                || !ValidateRequiredStages(description, error))
            {
                return false;
            }

            outDesc = std::move(description);
            return true;
        }
        // JSON構文解析で発生した例外
        catch (const Json::parse_error& exception)
        {
            error = "Shader manifest JSON parse failed: "
                + std::string{ exception.what() };
            return false;
        }
        // 変換または読込で発生した例外
        catch (const std::exception& exception)
        {
            error = "Shader manifest conversion failed: "
                + std::string{ exception.what() };
            return false;
        }
    }

    bool LoadShaderAssetDesc(
        AssetManager& assets,
        const std::filesystem::path& manifestPath,
        ShaderAssetDesc& outDesc,
        std::string& error)
    {
        error.clear();
        if (manifestPath.empty())
        {
            error = "Shader manifest path must not be empty.";
            return false;
        }

        try
        {
            if (!assets.FileExists(manifestPath))
            {
                error = "Shader manifest file does not exist: "
                    + PathToUtf8(manifestPath);
                return false;
            }

            // 読み込んだ宣言ファイルの内容
            const auto bytes = assets.ReadFileBytesFresh(manifestPath);
            // 宣言内容の借用文字列
            const std::string_view jsonText{
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size()
            };
            // 宣言の検証診断
            std::string parseError;
            if (!ParseShaderAssetDesc(
                    jsonText,
                    outDesc,
                    parseError))
            {
                error = "Shader manifest '"
                    + PathToUtf8(manifestPath)
                    + "': " + parseError;
                return false;
            }
            return true;
        }
        // 変換または読込で発生した例外
        catch (const std::exception& exception)
        {
            error = "Shader manifest could not be read: "
                + PathToUtf8(manifestPath)
                + ": " + exception.what();
            return false;
        }
    }
}
