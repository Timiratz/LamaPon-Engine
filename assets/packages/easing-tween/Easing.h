// イージング関数（動きの緩急）。0〜1の進み具合tを受けて、0〜1の
// 補間量を返します。数式だけのヘッダーで、エンジンへの依存はありま
// せん。Tweenコンポーネントから使いますが、自分のScriptから直接
// 呼んでも構いません。
//
//   const float k = LamaPonEasing::Ease(
//       LamaPonEasing::Type::OutCubic,
//       elapsed / duration);
//   position = start + (goal - start) * k;
//
// 名前の読み方: In=始めがゆっくり、Out=終わりがゆっくり、
// InOut=両端がゆっくり。
#pragma once

#include <cmath>
#include <string_view>

namespace LamaPonEasing
{
    enum class Type
    {
        Linear,
        InQuad,
        OutQuad,
        InOutQuad,
        InCubic,
        OutCubic,
        InOutCubic,
        InQuart,
        OutQuart,
        InOutQuart,
        InSine,
        OutSine,
        InOutSine,
        InExpo,
        OutExpo,
        InOutExpo,
        InCirc,
        OutCirc,
        InOutCirc,
        InBack,
        OutBack,
        InOutBack,
        InElastic,
        OutElastic,
        InOutElastic,
        InBounce,
        OutBounce,
        InOutBounce,
        Count
    };

    namespace Detail
    {
        constexpr float Pi = 3.14159265358979323846f;
        // Back（行きすぎて戻る）の強さ。よく使われる既定値です。
        constexpr float BackOvershoot = 1.70158f;

        [[nodiscard]] inline float Clamp01(const float value) noexcept
        {
            if (value < 0.0f)
            {
                return 0.0f;
            }
            if (value > 1.0f)
            {
                return 1.0f;
            }
            return value;
        }
    }

    [[nodiscard]] inline float Linear(const float t) noexcept
    {
        return t;
    }

    [[nodiscard]] inline float InQuad(const float t) noexcept
    {
        return t * t;
    }

    [[nodiscard]] inline float OutQuad(const float t) noexcept
    {
        return 1.0f - (1.0f - t) * (1.0f - t);
    }

    [[nodiscard]] inline float InOutQuad(const float t) noexcept
    {
        return t < 0.5f
            ? 2.0f * t * t
            : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    }

    [[nodiscard]] inline float InCubic(const float t) noexcept
    {
        return t * t * t;
    }

    [[nodiscard]] inline float OutCubic(const float t) noexcept
    {
        const float inverted = 1.0f - t;
        return 1.0f - inverted * inverted * inverted;
    }

    [[nodiscard]] inline float InOutCubic(const float t) noexcept
    {
        if (t < 0.5f)
        {
            return 4.0f * t * t * t;
        }
        const float inverted = -2.0f * t + 2.0f;
        return 1.0f
            - inverted * inverted * inverted / 2.0f;
    }

    [[nodiscard]] inline float InQuart(const float t) noexcept
    {
        return t * t * t * t;
    }

    [[nodiscard]] inline float OutQuart(const float t) noexcept
    {
        const float inverted = 1.0f - t;
        return 1.0f
            - inverted * inverted * inverted * inverted;
    }

    [[nodiscard]] inline float InOutQuart(const float t) noexcept
    {
        if (t < 0.5f)
        {
            return 8.0f * t * t * t * t;
        }
        const float inverted = -2.0f * t + 2.0f;
        return 1.0f
            - inverted * inverted * inverted * inverted
                / 2.0f;
    }

    [[nodiscard]] inline float InSine(const float t) noexcept
    {
        return 1.0f
            - std::cos(t * Detail::Pi / 2.0f);
    }

    [[nodiscard]] inline float OutSine(const float t) noexcept
    {
        return std::sin(t * Detail::Pi / 2.0f);
    }

    [[nodiscard]] inline float InOutSine(const float t) noexcept
    {
        return -(std::cos(Detail::Pi * t) - 1.0f) / 2.0f;
    }

    [[nodiscard]] inline float InExpo(const float t) noexcept
    {
        return t <= 0.0f
            ? 0.0f
            : std::pow(2.0f, 10.0f * t - 10.0f);
    }

    [[nodiscard]] inline float OutExpo(const float t) noexcept
    {
        return t >= 1.0f
            ? 1.0f
            : 1.0f - std::pow(2.0f, -10.0f * t);
    }

    [[nodiscard]] inline float InOutExpo(const float t) noexcept
    {
        if (t <= 0.0f)
        {
            return 0.0f;
        }
        if (t >= 1.0f)
        {
            return 1.0f;
        }
        return t < 0.5f
            ? std::pow(2.0f, 20.0f * t - 10.0f) / 2.0f
            : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f))
                / 2.0f;
    }

    [[nodiscard]] inline float InCirc(const float t) noexcept
    {
        return 1.0f
            - std::sqrt(std::max(0.0f, 1.0f - t * t));
    }

    [[nodiscard]] inline float OutCirc(const float t) noexcept
    {
        const float shifted = t - 1.0f;
        return std::sqrt(
            std::max(0.0f, 1.0f - shifted * shifted));
    }

    [[nodiscard]] inline float InOutCirc(const float t) noexcept
    {
        if (t < 0.5f)
        {
            const float doubled = 2.0f * t;
            return (1.0f
                - std::sqrt(
                    std::max(
                        0.0f,
                        1.0f - doubled * doubled)))
                / 2.0f;
        }
        const float shifted = -2.0f * t + 2.0f;
        return (std::sqrt(
                std::max(
                    0.0f,
                    1.0f - shifted * shifted))
            + 1.0f)
            / 2.0f;
    }

    [[nodiscard]] inline float InBack(const float t) noexcept
    {
        constexpr float c3 = Detail::BackOvershoot + 1.0f;
        return c3 * t * t * t
            - Detail::BackOvershoot * t * t;
    }

    [[nodiscard]] inline float OutBack(const float t) noexcept
    {
        constexpr float c3 = Detail::BackOvershoot + 1.0f;
        const float shifted = t - 1.0f;
        return 1.0f
            + c3 * shifted * shifted * shifted
            + Detail::BackOvershoot * shifted * shifted;
    }

    [[nodiscard]] inline float InOutBack(const float t) noexcept
    {
        constexpr float c2 =
            Detail::BackOvershoot * 1.525f;
        if (t < 0.5f)
        {
            const float doubled = 2.0f * t;
            return doubled * doubled
                * ((c2 + 1.0f) * doubled - c2)
                / 2.0f;
        }
        const float shifted = 2.0f * t - 2.0f;
        return (shifted * shifted
                * ((c2 + 1.0f) * shifted + c2)
            + 2.0f)
            / 2.0f;
    }

    [[nodiscard]] inline float InElastic(const float t) noexcept
    {
        if (t <= 0.0f)
        {
            return 0.0f;
        }
        if (t >= 1.0f)
        {
            return 1.0f;
        }
        constexpr float period =
            2.0f * Detail::Pi / 3.0f;
        return -std::pow(2.0f, 10.0f * t - 10.0f)
            * std::sin((t * 10.0f - 10.75f) * period);
    }

    [[nodiscard]] inline float OutElastic(const float t) noexcept
    {
        if (t <= 0.0f)
        {
            return 0.0f;
        }
        if (t >= 1.0f)
        {
            return 1.0f;
        }
        constexpr float period =
            2.0f * Detail::Pi / 3.0f;
        return std::pow(2.0f, -10.0f * t)
            * std::sin((t * 10.0f - 0.75f) * period)
            + 1.0f;
    }

    [[nodiscard]] inline float InOutElastic(const float t) noexcept
    {
        if (t <= 0.0f)
        {
            return 0.0f;
        }
        if (t >= 1.0f)
        {
            return 1.0f;
        }
        constexpr float period =
            2.0f * Detail::Pi / 4.5f;
        if (t < 0.5f)
        {
            return -(std::pow(2.0f, 20.0f * t - 10.0f)
                * std::sin((20.0f * t - 11.125f) * period))
                / 2.0f;
        }
        return std::pow(2.0f, -20.0f * t + 10.0f)
            * std::sin((20.0f * t - 11.125f) * period)
            / 2.0f
            + 1.0f;
    }

    [[nodiscard]] inline float OutBounce(float t) noexcept
    {
        constexpr float n1 = 7.5625f;
        constexpr float d1 = 2.75f;
        if (t < 1.0f / d1)
        {
            return n1 * t * t;
        }
        if (t < 2.0f / d1)
        {
            t -= 1.5f / d1;
            return n1 * t * t + 0.75f;
        }
        if (t < 2.5f / d1)
        {
            t -= 2.25f / d1;
            return n1 * t * t + 0.9375f;
        }
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }

    [[nodiscard]] inline float InBounce(const float t) noexcept
    {
        return 1.0f - OutBounce(1.0f - t);
    }

    [[nodiscard]] inline float InOutBounce(const float t) noexcept
    {
        return t < 0.5f
            ? (1.0f - OutBounce(1.0f - 2.0f * t)) / 2.0f
            : (1.0f + OutBounce(2.0f * t - 1.0f)) / 2.0f;
    }

    // 種別を指定して呼びます。tは0〜1へ丸めます。
    [[nodiscard]] inline float Ease(
        const Type type,
        const float progress) noexcept
    {
        const float t = Detail::Clamp01(progress);
        switch (type)
        {
        case Type::InQuad: return InQuad(t);
        case Type::OutQuad: return OutQuad(t);
        case Type::InOutQuad: return InOutQuad(t);
        case Type::InCubic: return InCubic(t);
        case Type::OutCubic: return OutCubic(t);
        case Type::InOutCubic: return InOutCubic(t);
        case Type::InQuart: return InQuart(t);
        case Type::OutQuart: return OutQuart(t);
        case Type::InOutQuart: return InOutQuart(t);
        case Type::InSine: return InSine(t);
        case Type::OutSine: return OutSine(t);
        case Type::InOutSine: return InOutSine(t);
        case Type::InExpo: return InExpo(t);
        case Type::OutExpo: return OutExpo(t);
        case Type::InOutExpo: return InOutExpo(t);
        case Type::InCirc: return InCirc(t);
        case Type::OutCirc: return OutCirc(t);
        case Type::InOutCirc: return InOutCirc(t);
        case Type::InBack: return InBack(t);
        case Type::OutBack: return OutBack(t);
        case Type::InOutBack: return InOutBack(t);
        case Type::InElastic: return InElastic(t);
        case Type::OutElastic: return OutElastic(t);
        case Type::InOutElastic: return InOutElastic(t);
        case Type::InBounce: return InBounce(t);
        case Type::OutBounce: return OutBounce(t);
        case Type::InOutBounce: return InOutBounce(t);
        case Type::Linear:
        case Type::Count:
        default:
            return Linear(t);
        }
    }

    // Inspectorの選択肢や、保存された文字列から種別へ戻すため。
    [[nodiscard]] inline std::string_view TypeName(
        const Type type) noexcept
    {
        switch (type)
        {
        case Type::InQuad: return "InQuad";
        case Type::OutQuad: return "OutQuad";
        case Type::InOutQuad: return "InOutQuad";
        case Type::InCubic: return "InCubic";
        case Type::OutCubic: return "OutCubic";
        case Type::InOutCubic: return "InOutCubic";
        case Type::InQuart: return "InQuart";
        case Type::OutQuart: return "OutQuart";
        case Type::InOutQuart: return "InOutQuart";
        case Type::InSine: return "InSine";
        case Type::OutSine: return "OutSine";
        case Type::InOutSine: return "InOutSine";
        case Type::InExpo: return "InExpo";
        case Type::OutExpo: return "OutExpo";
        case Type::InOutExpo: return "InOutExpo";
        case Type::InCirc: return "InCirc";
        case Type::OutCirc: return "OutCirc";
        case Type::InOutCirc: return "InOutCirc";
        case Type::InBack: return "InBack";
        case Type::OutBack: return "OutBack";
        case Type::InOutBack: return "InOutBack";
        case Type::InElastic: return "InElastic";
        case Type::OutElastic: return "OutElastic";
        case Type::InOutElastic: return "InOutElastic";
        case Type::InBounce: return "InBounce";
        case Type::OutBounce: return "OutBounce";
        case Type::InOutBounce: return "InOutBounce";
        case Type::Linear:
        case Type::Count:
        default:
            return "Linear";
        }
    }

    // 未知の名前はLinearになります。
    [[nodiscard]] inline Type TypeFromName(
        const std::string_view name) noexcept
    {
        for (int index = 0;
            index < static_cast<int>(Type::Count);
            ++index)
        {
            const auto type = static_cast<Type>(index);
            if (TypeName(type) == name)
            {
                return type;
            }
        }
        return Type::Linear;
    }
}
