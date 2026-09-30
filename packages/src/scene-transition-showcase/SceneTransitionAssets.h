#pragma once

#include "LamaPon/Assets/DataAsset.h"
#include "LamaPon/Scene/SceneTransition.h"

#include <DirectXMath.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

// エンジンは遷移の時間（覆う→切り替える→開く）と覆い具合だけを
// 計算し、画面を覆う絵は描きません。このヘッダーは、プリセットの
// 「見た目」（Look）を読み、自前のシェーダーへ渡す値を作ります。
namespace LamaPonSceneShowcase
{
    inline constexpr auto PresetType = "SceneTransition.Preset";
    inline constexpr auto DefaultPresetPath =
        "packages/scene-transition-showcase/presets/IrisGold.asset.json";
    // 覆いを描くピクセルシェーダーです（プリセットのshaderが空のとき）。
    inline constexpr auto DefaultShaderPath =
        "packages/scene-transition-showcase/shaders/LamaPonSceneTransition.hlsl";

    // 覆いの形です。番号はシェーダーの演出番号と同じです。
    enum class Effect : std::uint8_t
    {
        // 覆わずにすぐ切り替えます。
        None,
        Fade,
        Wipe,
        Iris,
        Diamond,
        Blinds,
        Tiles,
        DiamondTiles,
        Dots,
        Shutter,
        // shaderPatternの模様で覆います。
        Shader,
        Count
    };

    // 覆いが進む向きです。Blinds／Tiles／Dotsでは現れる順番、
    // Shutterでは閉じる軸に使います。
    enum class Direction : std::uint8_t
    {
        LeftToRight,
        RightToLeft,
        TopToBottom,
        BottomToTop,
        TopLeftToBottomRight,
        TopRightToBottomLeft,
        BottomLeftToTopRight,
        BottomRightToTopLeft,
        Count
    };

    // Shader演出の模様です。番号はシェーダーの模様番号と同じです。
    enum class ShaderPattern : std::uint8_t
    {
        RuleImage,
        Dissolve,
        Clock,
        Spiral,
        Ripple,
        Hexagons,
        Heart,
        Count
    };

    // 演出の見た目です。時間と動作はLamaPon::SceneTransitionSettingsが
    // 持ちます。
    struct Look final
    {
        Effect effect{ Effect::Fade };
        Direction direction{ Direction::LeftToRight };
        // 覆いの色。alphaを1未満にすると旧シーンが透けます。
        DirectX::XMFLOAT4 color{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 覆いの縁へ入れる差し色です。alphaが0なら使いません。
        DirectX::XMFLOAT4 accentColor{ 1.0f, 0.8f, 0.2f, 0.0f };
        // 差し色の帯の太さ（画面の短辺に対する比率）。
        float accentWidth{ 0.03f };
        // 境界をぼかす幅（画面の短辺に対する比率）。
        float softness{};
        // Blindsの帯の本数、Tiles系の横方向の個数、模様の細かさ。
        std::uint32_t divisions{ 10 };
        // Blinds／Tiles系の時間差。0で一斉、1に近いほど順番に現れます。
        float stagger{ 0.5f };
        // Iris／Diamondなどの中心（左上0,0／右下1,1）。
        DirectX::XMFLOAT2 focus{ 0.5f, 0.5f };
        // 開くときも覆ったときと同じ向きへ抜けます。
        bool passThrough{ true };
        ShaderPattern shaderPattern{ ShaderPattern::Dissolve };
        // Spriteのテクスチャ（t0）へ渡す画像（assets相対）。RuleImageでは
        // 覆う順番を決めるグレースケール画像です。
        std::string ruleTexture;
        // 独自のピクセルシェーダー（assets相対の.hlsl）。空なら
        // DefaultShaderPathを使います。
        std::string shader;
    };

    namespace Detail
    {
        template <typename Enum>
        struct NamedValue final
        {
            Enum value;
            std::string_view name;
        };

        inline constexpr std::array EffectNames{
            NamedValue<Effect>{ Effect::None, "none" },
            NamedValue<Effect>{ Effect::Fade, "fade" },
            NamedValue<Effect>{ Effect::Wipe, "wipe" },
            NamedValue<Effect>{ Effect::Iris, "iris" },
            NamedValue<Effect>{ Effect::Diamond, "diamond" },
            NamedValue<Effect>{ Effect::Blinds, "blinds" },
            NamedValue<Effect>{ Effect::Tiles, "tiles" },
            NamedValue<Effect>{ Effect::DiamondTiles, "diamondTiles" },
            NamedValue<Effect>{ Effect::Dots, "dots" },
            NamedValue<Effect>{ Effect::Shutter, "shutter" },
            NamedValue<Effect>{ Effect::Shader, "shader" },
        };
        static_assert(EffectNames.size()
            == static_cast<std::size_t>(Effect::Count));

        inline constexpr std::array DirectionNames{
            NamedValue<Direction>{ Direction::LeftToRight, "leftToRight" },
            NamedValue<Direction>{ Direction::RightToLeft, "rightToLeft" },
            NamedValue<Direction>{ Direction::TopToBottom, "topToBottom" },
            NamedValue<Direction>{ Direction::BottomToTop, "bottomToTop" },
            NamedValue<Direction>{
                Direction::TopLeftToBottomRight, "topLeftToBottomRight" },
            NamedValue<Direction>{
                Direction::TopRightToBottomLeft, "topRightToBottomLeft" },
            NamedValue<Direction>{
                Direction::BottomLeftToTopRight, "bottomLeftToTopRight" },
            NamedValue<Direction>{
                Direction::BottomRightToTopLeft, "bottomRightToTopLeft" },
        };
        static_assert(DirectionNames.size()
            == static_cast<std::size_t>(Direction::Count));

        inline constexpr std::array PatternNames{
            NamedValue<ShaderPattern>{ ShaderPattern::RuleImage, "ruleImage" },
            NamedValue<ShaderPattern>{ ShaderPattern::Dissolve, "dissolve" },
            NamedValue<ShaderPattern>{ ShaderPattern::Clock, "clock" },
            NamedValue<ShaderPattern>{ ShaderPattern::Spiral, "spiral" },
            NamedValue<ShaderPattern>{ ShaderPattern::Ripple, "ripple" },
            NamedValue<ShaderPattern>{ ShaderPattern::Hexagons, "hexagons" },
            NamedValue<ShaderPattern>{ ShaderPattern::Heart, "heart" },
        };
        static_assert(PatternNames.size()
            == static_cast<std::size_t>(ShaderPattern::Count));

        template <typename Enum, std::size_t Size>
        [[nodiscard]] std::string_view NameOf(
            const std::array<NamedValue<Enum>, Size>& names,
            const Enum value) noexcept
        {
            for (const auto& entry : names)
            {
                if (entry.value == value)
                {
                    return entry.name;
                }
            }
            return names.front().name;
        }

        // 未知の名前（新しい版で増えた演出など）はfallbackへ戻します。
        template <typename Enum, std::size_t Size>
        [[nodiscard]] Enum ValueOf(
            const std::array<NamedValue<Enum>, Size>& names,
            const nlohmann::json& values,
            const char* key,
            const Enum fallback)
        {
            const auto found = values.find(key);
            if (found == values.end() || !found->is_string())
            {
                return fallback;
            }
            const auto name = found->get<std::string>();
            for (const auto& entry : names)
            {
                if (entry.name == name)
                {
                    return entry.value;
                }
            }
            return fallback;
        }

        [[nodiscard]] inline float Finite(
            const float value,
            const float fallback) noexcept
        {
            return std::isfinite(value) ? value : fallback;
        }

        [[nodiscard]] inline float Saturate(const float value) noexcept
        {
            return std::clamp(Finite(value, 0.0f), 0.0f, 1.0f);
        }

        [[nodiscard]] inline float ReadNumber(
            const nlohmann::json& values,
            const char* key,
            const float fallback)
        {
            const auto found = values.find(key);
            return found != values.end() && found->is_number()
                ? Finite(found->get<float>(), fallback)
                : fallback;
        }

        [[nodiscard]] inline DirectX::XMFLOAT4 ReadColor(
            const nlohmann::json& values,
            const char* key,
            const DirectX::XMFLOAT4& fallback)
        {
            const auto found = values.find(key);
            if (found == values.end() || !found->is_array()
                || found->size() != 4)
            {
                return fallback;
            }
            for (const auto& item : *found)
            {
                if (!item.is_number())
                {
                    return fallback;
                }
            }
            return {
                Saturate((*found)[0].get<float>()),
                Saturate((*found)[1].get<float>()),
                Saturate((*found)[2].get<float>()),
                Saturate((*found)[3].get<float>())
            };
        }

        [[nodiscard]] inline nlohmann::json ColorToJson(
            const DirectX::XMFLOAT4& color)
        {
            return nlohmann::json::array(
                { color.x, color.y, color.z, color.w });
        }

        [[nodiscard]] inline DirectX::XMFLOAT2 DirectionVector(
            const Direction direction) noexcept
        {
            constexpr float Diagonal = 0.70710678f;
            switch (direction)
            {
            case Direction::RightToLeft:
                return { -1.0f, 0.0f };
            case Direction::TopToBottom:
                return { 0.0f, 1.0f };
            case Direction::BottomToTop:
                return { 0.0f, -1.0f };
            case Direction::TopLeftToBottomRight:
                return { Diagonal, Diagonal };
            case Direction::TopRightToBottomLeft:
                return { -Diagonal, Diagonal };
            case Direction::BottomLeftToTopRight:
                return { Diagonal, -Diagonal };
            case Direction::BottomRightToTopLeft:
                return { -Diagonal, -Diagonal };
            case Direction::LeftToRight:
            default:
                return { 1.0f, 0.0f };
            }
        }
    }

    [[nodiscard]] inline std::string_view EffectName(
        const Effect effect) noexcept
    {
        return Detail::NameOf(Detail::EffectNames, effect);
    }

    // プリセットに時間が書かれていないときの時間です。
    [[nodiscard]] inline LamaPon::SceneTransitionSettings DefaultTiming()
    {
        return LamaPon::MakeSceneTransition(0.4f, 0.1f);
    }

    // 範囲外の値を安全な範囲へ丸めます。
    [[nodiscard]] inline Look SanitizeLook(Look look)
    {
        const Look defaults;
        if (look.effect >= Effect::Count)
        {
            look.effect = defaults.effect;
        }
        if (look.direction >= Direction::Count)
        {
            look.direction = defaults.direction;
        }
        if (look.shaderPattern >= ShaderPattern::Count)
        {
            look.shaderPattern = defaults.shaderPattern;
        }
        const auto saturate = [](const DirectX::XMFLOAT4& color)
            {
                return DirectX::XMFLOAT4{
                    Detail::Saturate(color.x),
                    Detail::Saturate(color.y),
                    Detail::Saturate(color.z),
                    Detail::Saturate(color.w)
                };
            };
        look.color = saturate(look.color);
        look.accentColor = saturate(look.accentColor);
        look.accentWidth = std::clamp(
            Detail::Finite(look.accentWidth, defaults.accentWidth),
            0.0f,
            0.5f);
        look.softness = std::clamp(
            Detail::Finite(look.softness, defaults.softness),
            0.0f,
            1.0f);
        look.divisions = std::clamp<std::uint32_t>(look.divisions, 1u, 64u);
        look.stagger = std::clamp(
            Detail::Finite(look.stagger, defaults.stagger),
            0.0f,
            0.95f);
        look.focus = {
            Detail::Saturate(look.focus.x),
            Detail::Saturate(look.focus.y)
        };
        return look;
    }

    // プリセットのvaluesと同じキーで読み書きします。時間と動作の
    // キーは無視します（SceneTransitionFromJsonが読みます）。
    [[nodiscard]] inline Look LookFromJson(
        const nlohmann::json& values,
        const Look& fallback = {})
    {
        if (!values.is_object())
        {
            return SanitizeLook(fallback);
        }
        Look look = fallback;
        look.effect = Detail::ValueOf(
            Detail::EffectNames, values, "effect", fallback.effect);
        look.direction = Detail::ValueOf(
            Detail::DirectionNames, values, "direction", fallback.direction);
        look.shaderPattern = Detail::ValueOf(
            Detail::PatternNames,
            values,
            "shaderPattern",
            fallback.shaderPattern);
        look.color = Detail::ReadColor(values, "color", fallback.color);
        look.accentColor = Detail::ReadColor(
            values, "accentColor", fallback.accentColor);
        look.accentWidth = Detail::ReadNumber(
            values, "accentWidth", fallback.accentWidth);
        look.softness = Detail::ReadNumber(
            values, "softness", fallback.softness);
        // DataAssetは整数もdoubleで保持するため、数値なら丸めて読みます。
        if (const auto divisions = values.find("divisions");
            divisions != values.end() && divisions->is_number())
        {
            look.divisions = static_cast<std::uint32_t>(std::clamp(
                Detail::Finite(divisions->get<float>(), 10.0f),
                1.0f,
                64.0f));
        }
        look.stagger = Detail::ReadNumber(values, "stagger", fallback.stagger);
        if (const auto focus = values.find("focus");
            focus != values.end() && focus->is_array()
            && focus->size() == 2
            && (*focus)[0].is_number() && (*focus)[1].is_number())
        {
            look.focus = {
                (*focus)[0].get<float>(),
                (*focus)[1].get<float>()
            };
        }
        if (const auto found = values.find("passThrough");
            found != values.end() && found->is_boolean())
        {
            look.passThrough = found->get<bool>();
        }
        if (const auto found = values.find("ruleTexture");
            found != values.end() && found->is_string())
        {
            look.ruleTexture = found->get<std::string>();
        }
        if (const auto found = values.find("shader");
            found != values.end() && found->is_string())
        {
            look.shader = found->get<std::string>();
        }
        return SanitizeLook(look);
    }

    [[nodiscard]] inline nlohmann::json LookToJson(const Look& source)
    {
        const auto look = SanitizeLook(source);
        return nlohmann::json{
            { "effect", std::string(EffectName(look.effect)) },
            {
                "direction",
                std::string(Detail::NameOf(
                    Detail::DirectionNames, look.direction))
            },
            { "color", Detail::ColorToJson(look.color) },
            { "accentColor", Detail::ColorToJson(look.accentColor) },
            { "accentWidth", look.accentWidth },
            { "softness", look.softness },
            { "divisions", look.divisions },
            { "stagger", look.stagger },
            { "focus", nlohmann::json::array({ look.focus.x, look.focus.y }) },
            { "passThrough", look.passThrough },
            {
                "shaderPattern",
                std::string(Detail::NameOf(
                    Detail::PatternNames, look.shaderPattern))
            },
            { "ruleTexture", look.ruleTexture },
            { "shader", look.shader },
        };
    }

    // DataAssetはLoadDataAssetから取得します。型の違うアセットは読まず、
    // 欠けた項目・範囲外の値は既定値と補正で埋めます。「なし」の
    // 演出は覆う絵が無いので、時間を0にしてすぐ切り替えます。
    [[nodiscard]] inline bool ReadPreset(
        const LamaPon::DataAsset& asset,
        LamaPon::SceneTransitionSettings& timing,
        Look& look)
    {
        if (asset.TypeName() != PresetType || asset.IsEmpty())
        {
            return false;
        }
        const auto document = nlohmann::json::parse(
            asset.SerializeToJson(), nullptr, false);
        if (!document.is_object() || !document.contains("values")
            || !document.at("values").is_object())
        {
            return false;
        }
        const auto& values = document.at("values");
        timing = LamaPon::SceneTransitionFromJson(values, DefaultTiming());
        look = LookFromJson(values);
        if (look.effect == Effect::None)
        {
            timing.coverDuration = 0.0f;
            timing.holdDuration = 0.0f;
            timing.revealDuration = 0.0f;
        }
        return true;
    }

    // シェーダーのCustomParameters[0]～[4]です。[5]～[7]はSpriteの描画時に
    // エンジンが上書きします（[5]が覆いの色、[6].zwが描画サイズ）。
    //   [0] = coverage, ぼかし幅, 差し色の幅, 演出の番号
    //   [1] = 順番を反転するか(0/1), stagger, divisions, 模様の番号
    //   [2] = 差し色（premultiplyしない色）
    //   [3] = 中心x, 中心y（0～1）, 向きx, 向きy
    //   [4] = 未使用
    struct ShaderFrame final
    {
        std::array<DirectX::XMFLOAT4, 5> parameters{};
        ShaderPattern pattern{ ShaderPattern::Dissolve };
    };

    // coverageはSceneManager::TransitionCoverage()、revealingは開く途中か
    // どうかです。hasRuleTextureがfalseならRuleImageをDissolveに
    // 置き換えます。
    [[nodiscard]] inline ShaderFrame BuildShaderFrame(
        const Look& source,
        const float coverage,
        const bool revealing,
        const bool hasRuleTexture)
    {
        const auto look = SanitizeLook(source);
        ShaderFrame frame;
        frame.pattern =
            look.shaderPattern == ShaderPattern::RuleImage && !hasRuleTexture
                ? ShaderPattern::Dissolve
                : look.shaderPattern;
        const bool hasAccent =
            look.accentColor.w > 0.0f && look.accentWidth > 0.0f;
        const bool reverse = revealing && look.passThrough;
        auto direction = Detail::DirectionVector(look.direction);
        // 形の演出は、通り抜けるときに向きを反転します。模様は順番を
        // 反転するので向きはそのままです。
        if (reverse && look.effect != Effect::Shader)
        {
            direction = { -direction.x, -direction.y };
        }
        const float softness = look.effect == Effect::Shader
            // 模様の境界が0だとジャギーが目立つため、最小限のぼかしを
            // 残します。
            ? std::max(look.softness, 0.02f)
            : look.softness;
        frame.parameters[0] = {
            Detail::Saturate(coverage),
            softness,
            hasAccent ? look.accentWidth : 0.0f,
            static_cast<float>(look.effect)
        };
        frame.parameters[1] = {
            reverse && look.effect == Effect::Shader ? 1.0f : 0.0f,
            look.stagger,
            static_cast<float>(look.divisions),
            static_cast<float>(frame.pattern)
        };
        frame.parameters[2] = look.accentColor;
        frame.parameters[3] = {
            look.focus.x,
            look.focus.y,
            direction.x,
            direction.y
        };
        return frame;
    }
}
