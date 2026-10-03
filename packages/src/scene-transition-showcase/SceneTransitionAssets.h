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

// Engineは遷移時間を管理し、このPackageはPreset外観とShader引数を用意します。
namespace LamaPonSceneShowcase
{
    // Data Assetに登録するPreset型名
    inline constexpr auto PresetType = "SceneTransition.Preset";
    // 配布する初期Preset
    inline constexpr auto DefaultPresetPath =
        "packages/scene-transition-showcase/presets/IrisGold.asset.json";
    // Presetに独自Shaderがない場合の描画先
    inline constexpr auto DefaultShaderPath =
        "packages/scene-transition-showcase/shaders/LamaPonSceneTransition.hlsl";

    // Shaderの演出番号と一致する覆い形状
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

    // 覆い方向（Blinds等では順番、Shutterでは軸）
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

    // Shader側の模様番号と一致するPattern
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

    // 遷移の外観設定（時間と動作はSceneTransitionSettingsが保持）
    struct Look final
    {
        // 遷移の描画形状
        Effect effect{ Effect::Fade };
        // 形状が進む方向
        Direction direction{ Direction::LeftToRight };
        // Spriteの覆い色（alphaで旧Sceneの透け具合を調整）
        DirectX::XMFLOAT4 color{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 覆いの縁に使う差し色（alphaが0なら無効）
        DirectX::XMFLOAT4 accentColor{ 1.0f, 0.8f, 0.2f, 0.0f };
        // 差し色帯の太さ（画面短辺比）
        float accentWidth{ 0.03f };
        // 境界のぼかし幅（画面短辺比）
        float softness{};
        // 帯・タイルの分割数または模様の細かさ
        std::uint32_t divisions{ 10 };
        // Blinds／Tiles等の段階時間差（0なら同時）
        float stagger{ 0.5f };
        // Iris等の中心（左上0,0／右下1,1）
        DirectX::XMFLOAT2 focus{ 0.5f, 0.5f };
        // 開くときも同じ方向へ抜けるか
        bool passThrough{ true };
        // Shader形状のPattern
        ShaderPattern shaderPattern{ ShaderPattern::Dissolve };
        // RuleImageの順序を決めるグレースケール画像パス
        std::string ruleTexture;
        // 独自Pixel Shaderパス（空ならDefaultShaderPath）
        std::string shader;
    };

    namespace Detail
    {
        template <typename Enum>
        struct NamedValue final
        {
            // Enum値
            Enum value;
            // JSONへ保存する名前
            std::string_view name;
        };

        // EffectのJSON名とEnum値
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

        // DirectionのJSON名とEnum値
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

        // ShaderPatternのJSON名とEnum値
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
        // Enum値に対応するJSON名を返します(names: 対応表, value: Enum値)。
        [[nodiscard]] std::string_view NameOf(
            const std::array<NamedValue<Enum>, Size>& names,
            const Enum value) noexcept
        {
            // 対応表の各名前(entry: Enum値とJSON名)
            for (const auto& entry : names)
            {
                // 指定値に一致したJSON名を返します。
                if (entry.value == value)
                {
                    return entry.name;
                }
            }
            return names.front().name;
        }

        // JSON名をEnumへ変換します(names: 対応表, values: JSON欄, key: 欄名, fallback: 不明時の値)。
        template <typename Enum, std::size_t Size>
        [[nodiscard]] Enum ValueOf(
            const std::array<NamedValue<Enum>, Size>& names,
            const nlohmann::json& values,
            const char* key,
            const Enum fallback)
        {
            // 指定されたJSON欄
            const auto found = values.find(key);
            // 欄が無いか文字列でなければ既定値を返します。
            if (found == values.end() || !found->is_string())
            {
                return fallback;
            }
            // JSONに保存されたEnum名
            const auto name = found->get<std::string>();
            // Enum値に対応する名前を探します(entry: 対応表項目)。
            for (const auto& entry : names)
            {
                // 一致した名前をEnum値へ戻します。
                if (entry.name == name)
                {
                    return entry.value;
                }
            }
            return fallback;
        }

        // 有限値を保ちます(value: 入力, fallback: 非有限時の値)。
        [[nodiscard]] inline float Finite(
            const float value,
            const float fallback) noexcept
        {
            return std::isfinite(value) ? value : fallback;
        }

        // 有限値を0～1へ制限します(value: 入力値)。
        [[nodiscard]] inline float Saturate(const float value) noexcept
        {
            return std::clamp(Finite(value, 0.0f), 0.0f, 1.0f);
        }

        // 数値欄を検証して読みます(values: JSON欄, key: 欄名, fallback: 既定値)。
        [[nodiscard]] inline float ReadNumber(
            const nlohmann::json& values,
            const char* key,
            const float fallback)
        {
            // 指定されたJSON欄
            const auto found = values.find(key);
            return found != values.end() && found->is_number()
                ? Finite(found->get<float>(), fallback)
                : fallback;
        }

        // RGBA配列を検証して読みます(values: JSON欄, key: 欄名, fallback: 既定色)。
        [[nodiscard]] inline DirectX::XMFLOAT4 ReadColor(
            const nlohmann::json& values,
            const char* key,
            const DirectX::XMFLOAT4& fallback)
        {
            // 指定されたJSON欄
            const auto found = values.find(key);
            // RGBA配列でなければ既定色を返します。
            if (found == values.end() || !found->is_array()
                || found->size() != 4)
            {
                return fallback;
            }
            // RGBAを構成する各要素(item: 色成分)。
            for (const auto& item : *found)
            {
                // 数値以外の成分は既定色へ戻します。
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

        // RGBA色をJSON配列へ変換します(color: 保存する色)。
        [[nodiscard]] inline nlohmann::json ColorToJson(
            const DirectX::XMFLOAT4& color)
        {
            return nlohmann::json::array(
                { color.x, color.y, color.z, color.w });
        }

        // 方向Enumを単位ベクトルへ変換します(direction: 進行方向)。
        [[nodiscard]] inline DirectX::XMFLOAT2 DirectionVector(
            const Direction direction) noexcept
        {
            // 斜め方向の正規化成分
            constexpr float Diagonal = 0.70710678f;
            // 方向ごとの単位ベクトルを選びます。
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

    // EffectのJSON名を返します(effect: 演出形状)。
    [[nodiscard]] inline std::string_view EffectName(
        const Effect effect) noexcept
    {
        return Detail::NameOf(Detail::EffectNames, effect);
    }

    // Presetに時間がない場合の遷移時間を返します。
    [[nodiscard]] inline LamaPon::SceneTransitionSettings DefaultTiming()
    {
        return LamaPon::MakeSceneTransition(0.4f, 0.1f);
    }

    // 見た目設定を安全な範囲へ補正します(look: 補正対象)。
    [[nodiscard]] inline Look SanitizeLook(Look look)
    {
        // Lookの既定値
        const Look defaults;
        // 未定義の演出番号を既定値へ戻します。
        if (look.effect >= Effect::Count)
        {
            look.effect = defaults.effect;
        }
        // 未定義の向き番号を既定値へ戻します。
        if (look.direction >= Direction::Count)
        {
            look.direction = defaults.direction;
        }
        // 未定義の模様番号を既定値へ戻します。
        if (look.shaderPattern >= ShaderPattern::Count)
        {
            look.shaderPattern = defaults.shaderPattern;
        }
        // 各色成分を0～1へ制限します(color: 補正するRGBA色)。
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

    // JSONから外観を読みます(values: Preset欄, fallback: 欠落時の外観)。時間設定欄は無視します。
    [[nodiscard]] inline Look LookFromJson(
        const nlohmann::json& values,
        const Look& fallback = {})
    {
        // JSON object以外は既定外観へ戻します。
        if (!values.is_object())
        {
            return SanitizeLook(fallback);
        }
        // 欠落欄を補う外観設定
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
        // JSON数値から分割数を読みます。
        if (const auto divisions = values.find("divisions");
            divisions != values.end() && divisions->is_number())
        {
            look.divisions = static_cast<std::uint32_t>(std::clamp(
                Detail::Finite(divisions->get<float>(), 10.0f),
                1.0f,
                64.0f));
        }
        look.stagger = Detail::ReadNumber(values, "stagger", fallback.stagger);
        // 2要素のFocus配列を読みます。
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
        // passThroughのBoolean欄を読みます。
        if (const auto found = values.find("passThrough");
            found != values.end() && found->is_boolean())
        {
            look.passThrough = found->get<bool>();
        }
        // Rule Textureの文字列欄を読みます。
        if (const auto found = values.find("ruleTexture");
            found != values.end() && found->is_string())
        {
            look.ruleTexture = found->get<std::string>();
        }
        // Shader pathの文字列欄を読みます。
        if (const auto found = values.find("shader");
            found != values.end() && found->is_string())
        {
            look.shader = found->get<std::string>();
        }
        return SanitizeLook(look);
    }

    // 外観設定をPreset用JSONへ変換します(source: 保存するLook)。
    [[nodiscard]] inline nlohmann::json LookToJson(const Look& source)
    {
        // 範囲を補正した保存対象
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

    // Presetを読みます(asset: Data Asset, timing: 時間出力, look: 外観出力)。
    // 型・内容不正はfalse、演出Noneは遷移時間を0にします。
    [[nodiscard]] inline bool ReadPreset(
        const LamaPon::DataAsset& asset,
        LamaPon::SceneTransitionSettings& timing,
        Look& look)
    {
        // Data Asset型と内容を検証します。
        if (asset.TypeName() != PresetType || asset.IsEmpty())
        {
            return false;
        }
        // Assetから解析したJSON文書
        const auto document = nlohmann::json::parse(
            asset.SerializeToJson(), nullptr, false);
        // values objectがない文書を拒否します。
        if (!document.is_object() || !document.contains("values")
            || !document.at("values").is_object())
        {
            return false;
        }
        // Presetの値オブジェクト
        const auto& values = document.at("values");
        timing = LamaPon::SceneTransitionFromJson(values, DefaultTiming());
        look = LookFromJson(values);
        // 演出なしは即時切替へ補正します。
        if (look.effect == Effect::None)
        {
            timing.coverDuration = 0.0f;
            timing.holdDuration = 0.0f;
            timing.revealDuration = 0.0f;
        }
        return true;
    }

    // Shaderへ渡す追加引数（[5]～[7]はSprite描画時にEngineが上書き）
    struct ShaderFrame final
    {
        // CustomParameters[0]～[4]へ渡す値
        std::array<DirectX::XMFLOAT4, 5> parameters{};
        // RuleImage未設定時の代替Patternを含む選択値
        ShaderPattern pattern{ ShaderPattern::Dissolve };
    };

    // Shader引数を作ります(source: 外観, coverage: 覆い割合, revealing: 開く途中か, hasRuleTexture: 画像の有無)。
    // RuleImage用TextureがなければDissolveへ置き換えます。
    [[nodiscard]] inline ShaderFrame BuildShaderFrame(
        const Look& source,
        const float coverage,
        const bool revealing,
        const bool hasRuleTexture)
    {
        // 範囲を補正した外観設定
        const auto look = SanitizeLook(source);
        // Shader引数と有効Pattern
        ShaderFrame frame;
        frame.pattern =
            look.shaderPattern == ShaderPattern::RuleImage && !hasRuleTexture
                ? ShaderPattern::Dissolve
                : look.shaderPattern;
        // 差し色帯を描くか
        const bool hasAccent =
            look.accentColor.w > 0.0f && look.accentWidth > 0.0f;
        // 開く方向を反転するか
        const bool reverse = revealing && look.passThrough;
        // Patternへ渡す進行方向
        auto direction = Detail::DirectionVector(look.direction);
        // Shape効果だけ進行方向を反転します。Patternは順序で反転します。
        if (reverse && look.effect != Effect::Shader)
        {
            direction = { -direction.x, -direction.y };
        }
        // Shader Patternは境界のジャギーを防ぐ最小ぼかしを保ちます。
        // Shaderへ渡す最終ぼかし幅
        const float softness = look.effect == Effect::Shader
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
