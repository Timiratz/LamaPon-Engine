// 単体関数は0〜1の進捗を前提とし、入力制限が必要な場合はEaseを使います。
#pragma once

#include <cmath>
#include <string_view>

namespace LamaPonEasing
{
    enum class Type
    {
        // 一定速度の曲線
        Linear,
        // 二次曲線で始端を緩める曲線
        InQuad,
        // 二次曲線で終端を緩める曲線
        OutQuad,
        // 二次曲線で両端を緩める曲線
        InOutQuad,
        // 三次曲線で始端を緩める曲線
        InCubic,
        // 三次曲線で終端を緩める曲線
        OutCubic,
        // 三次曲線で両端を緩める曲線
        InOutCubic,
        // 四次曲線で始端を緩める曲線
        InQuart,
        // 四次曲線で終端を緩める曲線
        OutQuart,
        // 四次曲線で両端を緩める曲線
        InOutQuart,
        // 正弦曲線で始端を緩める曲線
        InSine,
        // 正弦曲線で終端を緩める曲線
        OutSine,
        // 正弦曲線で両端を緩める曲線
        InOutSine,
        // 指数曲線で始端を緩める曲線
        InExpo,
        // 指数曲線で終端を緩める曲線
        OutExpo,
        // 指数曲線で両端を緩める曲線
        InOutExpo,
        // 円弧で始端を緩める曲線
        InCirc,
        // 円弧で終端を緩める曲線
        OutCirc,
        // 円弧で両端を緩める曲線
        InOutCirc,
        // 行き過ぎで始端を緩める曲線
        InBack,
        // 行き過ぎで終端を緩める曲線
        OutBack,
        // 行き過ぎで両端を緩める曲線
        InOutBack,
        // 弾性振動で始端を緩める曲線
        InElastic,
        // 弾性振動で終端を緩める曲線
        OutElastic,
        // 弾性振動で両端を緩める曲線
        InOutElastic,
        // 反跳で始端を緩める曲線
        InBounce,
        // 反跳で終端を緩める曲線
        OutBounce,
        // 反跳で両端を緩める曲線
        InOutBounce,
        // イージング種別の個数
        Count
    };

    namespace Detail
    {
        // 円周率
        constexpr float Pi = 3.14159265358979323846f;
        // 行き過ぎ曲線の強度係数
        constexpr float BackOvershoot = 1.70158f;
        // 入力を0〜1へ制限します(value: 入力値)。
        [[nodiscard]] inline float Clamp01(const float value) noexcept
        {
            // 0未満は下限へ補正します。
            if (value < 0.0f)
            {
                return 0.0f;
            }
            // 1超過は上限へ補正します。
            if (value > 1.0f)
            {
                return 1.0f;
            }
            return value;
        }
    }

    // 進捗をそのまま返します(t: 0〜1の進捗)。
    [[nodiscard]] inline float Linear(const float t) noexcept
    {
        return t;
    }

    // 二次曲線で始端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InQuad(const float t) noexcept
    {
        return t * t;
    }

    // 二次曲線で終端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float OutQuad(const float t) noexcept
    {
        return 1.0f - (1.0f - t) * (1.0f - t);
    }

    // 二次曲線で両端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InOutQuad(const float t) noexcept
    {
        return t < 0.5f
            ? 2.0f * t * t
            : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    }

    // 三次曲線で始端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InCubic(const float t) noexcept
    {
        return t * t * t;
    }

    // 三次曲線で終端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float OutCubic(const float t) noexcept
    {
        // 終端から逆算した進捗
        const float inverted = 1.0f - t;
        return 1.0f - inverted * inverted * inverted;
    }

    // 三次曲線で両端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InOutCubic(const float t) noexcept
    {
        // 前半は始端側の三次式で補間します。
        if (t < 0.5f)
        {
            return 4.0f * t * t * t;
        }
        // 終端から逆算した進捗
        const float inverted = -2.0f * t + 2.0f;
        return 1.0f
            - inverted * inverted * inverted / 2.0f;
    }

    // 四次曲線で始端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InQuart(const float t) noexcept
    {
        return t * t * t * t;
    }

    // 四次曲線で終端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float OutQuart(const float t) noexcept
    {
        // 終端から逆算した進捗
        const float inverted = 1.0f - t;
        return 1.0f
            - inverted * inverted * inverted * inverted;
    }

    // 四次曲線で両端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InOutQuart(const float t) noexcept
    {
        // 前半は始端側の四次式で補間します。
        if (t < 0.5f)
        {
            return 8.0f * t * t * t * t;
        }
        // 終端から逆算した進捗
        const float inverted = -2.0f * t + 2.0f;
        return 1.0f
            - inverted * inverted * inverted * inverted
                / 2.0f;
    }

    // 正弦曲線で始端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InSine(const float t) noexcept
    {
        return 1.0f
            - std::cos(t * Detail::Pi / 2.0f);
    }

    // 正弦曲線で終端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float OutSine(const float t) noexcept
    {
        return std::sin(t * Detail::Pi / 2.0f);
    }

    // 正弦曲線で両端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InOutSine(const float t) noexcept
    {
        return -(std::cos(Detail::Pi * t) - 1.0f) / 2.0f;
    }

    // 指数曲線で始端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InExpo(const float t) noexcept
    {
        return t <= 0.0f
            ? 0.0f
            : std::pow(2.0f, 10.0f * t - 10.0f);
    }

    // 指数曲線で終端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float OutExpo(const float t) noexcept
    {
        return t >= 1.0f
            ? 1.0f
            : 1.0f - std::pow(2.0f, -10.0f * t);
    }

    // 指数曲線で両端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InOutExpo(const float t) noexcept
    {
        // 指数式の誤差を避けて始端を固定します。
        if (t <= 0.0f)
        {
            return 0.0f;
        }
        // 指数式の誤差を避けて終端を固定します。
        if (t >= 1.0f)
        {
            return 1.0f;
        }
        return t < 0.5f
            ? std::pow(2.0f, 20.0f * t - 10.0f) / 2.0f
            : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f))
                / 2.0f;
    }

    // 円弧で始端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InCirc(const float t) noexcept
    {
        return 1.0f
            - std::sqrt(std::max(0.0f, 1.0f - t * t));
    }

    // 円弧で終端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float OutCirc(const float t) noexcept
    {
        // 曲線評価用の相対進捗
        const float shifted = t - 1.0f;
        return std::sqrt(
            std::max(0.0f, 1.0f - shifted * shifted));
    }

    // 円弧で両端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InOutCirc(const float t) noexcept
    {
        // 前半は始端側の円弧で補間します。
        if (t < 0.5f)
        {
            // 前半区間を0〜1にする進捗
            const float doubled = 2.0f * t;
            return (1.0f
                - std::sqrt(
                    std::max(
                        0.0f,
                        1.0f - doubled * doubled)))
                / 2.0f;
        }
        // 曲線評価用の相対進捗
        const float shifted = -2.0f * t + 2.0f;
        return (std::sqrt(
                std::max(
                    0.0f,
                    1.0f - shifted * shifted))
            + 1.0f)
            / 2.0f;
    }

    // 行き過ぎで始端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InBack(const float t) noexcept
    {
        // 単方向の行き過ぎ曲線係数
        constexpr float c3 = Detail::BackOvershoot + 1.0f;
        return c3 * t * t * t
            - Detail::BackOvershoot * t * t;
    }

    // 行き過ぎで終端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float OutBack(const float t) noexcept
    {
        // 単方向の行き過ぎ曲線係数
        constexpr float c3 = Detail::BackOvershoot + 1.0f;
        // 曲線評価用の相対進捗
        const float shifted = t - 1.0f;
        return 1.0f
            + c3 * shifted * shifted * shifted
            + Detail::BackOvershoot * shifted * shifted;
    }

    // 行き過ぎで両端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InOutBack(const float t) noexcept
    {
        // 往復用の行き過ぎ強度係数
        constexpr float c2 =
            Detail::BackOvershoot * 1.525f;
        // 前半は始端側の行き過ぎ式で補間します。
        if (t < 0.5f)
        {
            // 前半区間を0〜1にする進捗
            const float doubled = 2.0f * t;
            return doubled * doubled
                * ((c2 + 1.0f) * doubled - c2)
                / 2.0f;
        }
        // 曲線評価用の相対進捗
        const float shifted = 2.0f * t - 2.0f;
        return (shifted * shifted
                * ((c2 + 1.0f) * shifted + c2)
            + 2.0f)
            / 2.0f;
    }

    // 弾性振動で始端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InElastic(const float t) noexcept
    {
        // 指数式の誤差を避けて始端を固定します。
        if (t <= 0.0f)
        {
            return 0.0f;
        }
        // 指数式の誤差を避けて終端を固定します。
        if (t >= 1.0f)
        {
            return 1.0f;
        }
        // 弾性振動の位相換算係数
        constexpr float period =
            2.0f * Detail::Pi / 3.0f;
        return -std::pow(2.0f, 10.0f * t - 10.0f)
            * std::sin((t * 10.0f - 10.75f) * period);
    }

    // 弾性振動で終端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float OutElastic(const float t) noexcept
    {
        // 指数式の誤差を避けて始端を固定します。
        if (t <= 0.0f)
        {
            return 0.0f;
        }
        // 指数式の誤差を避けて終端を固定します。
        if (t >= 1.0f)
        {
            return 1.0f;
        }
        // 弾性振動の位相換算係数
        constexpr float period =
            2.0f * Detail::Pi / 3.0f;
        return std::pow(2.0f, -10.0f * t)
            * std::sin((t * 10.0f - 0.75f) * period)
            + 1.0f;
    }

    // 弾性振動で両端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InOutElastic(const float t) noexcept
    {
        // 指数式の誤差を避けて始端を固定します。
        if (t <= 0.0f)
        {
            return 0.0f;
        }
        // 指数式の誤差を避けて終端を固定します。
        if (t >= 1.0f)
        {
            return 1.0f;
        }
        // 弾性振動の位相換算係数
        constexpr float period =
            2.0f * Detail::Pi / 4.5f;
        // 前半は始端側の振動式で補間します。
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

    // 反跳で終端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float OutBounce(float t) noexcept
    {
        // 反跳放物線の曲率係数
        constexpr float n1 = 7.5625f;
        // 反跳区間を分ける倍率
        constexpr float d1 = 2.75f;
        // 第1反跳区間は基準放物線で評価します。
        if (t < 1.0f / d1)
        {
            return n1 * t * t;
        }
        // 第2反跳区間は1.5/d1だけ進捗をずらします。
        if (t < 2.0f / d1)
        {
            t -= 1.5f / d1;
            return n1 * t * t + 0.75f;
        }
        // 第3反跳区間は2.25/d1だけ進捗をずらします。
        if (t < 2.5f / d1)
        {
            t -= 2.25f / d1;
            return n1 * t * t + 0.9375f;
        }
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }

    // 反跳で始端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InBounce(const float t) noexcept
    {
        return 1.0f - OutBounce(1.0f - t);
    }

    // 反跳で両端を緩めた値を求めます(t: 0〜1の進捗)。
    [[nodiscard]] inline float InOutBounce(const float t) noexcept
    {
        return t < 0.5f
            ? (1.0f - OutBounce(1.0f - 2.0f * t)) / 2.0f
            : (1.0f + OutBounce(2.0f * t - 1.0f)) / 2.0f;
    }

    // 進捗を0〜1へ制限して選んだ曲線を評価します(type: 曲線種別, progress: 進捗)。
    // 不明な種別はLinearで、BackとElasticの出力は0〜1を越える場合があります。
    [[nodiscard]] inline float Ease(
        const Type type,
        const float progress) noexcept
    {
        // 入力制限済みの進捗
        const float t = Detail::Clamp01(progress);
        // 種別に対応するイージング関数を選びます。
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
            // Linearと未知の種別は等速補間へ委ねます。
            return Linear(t);
        }
    }

    // 曲線種別の保存名を返します(type: 曲線種別)。
    // 不明な種別はLinearの名前を返します。
    [[nodiscard]] inline std::string_view TypeName(
        const Type type) noexcept
    {
        // 種別を保存形式で表す名前へ変換します。
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
            // 未知の種別も互換性のあるLinear名へ戻します。
            return "Linear";
        }
    }

    // 保存名から曲線種別を求めます(name: 保存名)。
    // 大文字小文字を区別し、未知の名前はLinearです。
    [[nodiscard]] inline Type TypeFromName(
        const std::string_view name) noexcept
    {
        // 保存名を照合する曲線番号
        for (int index = 0;
            index < static_cast<int>(Type::Count);
            ++index)
        {
            // 照合中の曲線種別
            const auto type = static_cast<Type>(index);
            // 保存名が一致した曲線を返します。
            if (TypeName(type) == name)
            {
                return type;
            }
        }
        // 未知の保存名はLinearへフォールバックします。
        return Type::Linear;
    }
}
