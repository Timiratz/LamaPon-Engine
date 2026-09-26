#include "LamaPon/Scene/SceneTransition.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace
{
    using LamaPon::SceneTransitionDirection;
    using LamaPon::SceneTransitionEasing;
    using LamaPon::SceneTransitionEffect;
    using LamaPon::SceneTransitionQuad;
    using LamaPon::SceneTransitionSettings;
    using LamaPon::SceneTransitionShaderPattern;
    using LamaPon::SceneTransitionShape;

    constexpr float Pi = 3.14159265358979f;
    constexpr float InverseSqrt2 = 0.70710678f;
    constexpr float Sqrt2 = 1.41421356f;

    template <typename Enum>
    struct NamedValue final
    {
        Enum value;
        std::string_view name;
    };

    constexpr std::array EffectNames{
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::None, "none" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::Fade, "fade" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::Wipe, "wipe" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::Iris, "iris" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::Diamond, "diamond" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::Blinds, "blinds" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::Tiles, "tiles" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::DiamondTiles, "diamondTiles" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::Dots, "dots" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::Shutter, "shutter" },
        NamedValue<SceneTransitionEffect>{
            SceneTransitionEffect::Shader, "shader" },
    };
    static_assert(
        EffectNames.size()
        == static_cast<std::size_t>(SceneTransitionEffect::Count));

    constexpr std::array DirectionNames{
        NamedValue<SceneTransitionDirection>{
            SceneTransitionDirection::LeftToRight, "leftToRight" },
        NamedValue<SceneTransitionDirection>{
            SceneTransitionDirection::RightToLeft, "rightToLeft" },
        NamedValue<SceneTransitionDirection>{
            SceneTransitionDirection::TopToBottom, "topToBottom" },
        NamedValue<SceneTransitionDirection>{
            SceneTransitionDirection::BottomToTop, "bottomToTop" },
        NamedValue<SceneTransitionDirection>{
            SceneTransitionDirection::TopLeftToBottomRight,
            "topLeftToBottomRight" },
        NamedValue<SceneTransitionDirection>{
            SceneTransitionDirection::TopRightToBottomLeft,
            "topRightToBottomLeft" },
        NamedValue<SceneTransitionDirection>{
            SceneTransitionDirection::BottomLeftToTopRight,
            "bottomLeftToTopRight" },
        NamedValue<SceneTransitionDirection>{
            SceneTransitionDirection::BottomRightToTopLeft,
            "bottomRightToTopLeft" },
    };
    static_assert(
        DirectionNames.size()
        == static_cast<std::size_t>(SceneTransitionDirection::Count));

    constexpr std::array EasingNames{
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::Linear, "linear" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInQuad, "easeInQuad" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseOutQuad, "easeOutQuad" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInOutQuad, "easeInOutQuad" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInCubic, "easeInCubic" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseOutCubic, "easeOutCubic" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInOutCubic, "easeInOutCubic" },
        NamedValue<SceneTransitionEasing>{
            SceneTransitionEasing::EaseInOutSine, "easeInOutSine" },
    };
    static_assert(
        EasingNames.size()
        == static_cast<std::size_t>(SceneTransitionEasing::Count));

    constexpr std::array ShaderPatternNames{
        NamedValue<SceneTransitionShaderPattern>{
            SceneTransitionShaderPattern::RuleImage, "ruleImage" },
        NamedValue<SceneTransitionShaderPattern>{
            SceneTransitionShaderPattern::Dissolve, "dissolve" },
        NamedValue<SceneTransitionShaderPattern>{
            SceneTransitionShaderPattern::Clock, "clock" },
        NamedValue<SceneTransitionShaderPattern>{
            SceneTransitionShaderPattern::Spiral, "spiral" },
        NamedValue<SceneTransitionShaderPattern>{
            SceneTransitionShaderPattern::Ripple, "ripple" },
        NamedValue<SceneTransitionShaderPattern>{
            SceneTransitionShaderPattern::Hexagons, "hexagons" },
        NamedValue<SceneTransitionShaderPattern>{
            SceneTransitionShaderPattern::Heart, "heart" },
    };
    static_assert(
        ShaderPatternNames.size()
        == static_cast<std::size_t>(
            SceneTransitionShaderPattern::Count));

    // PathUtils.hはWindows.hに依存するため、同じ変換をここで行います
    // （保存形式はPathToUtf8と同じgeneric形式のUTF-8です）。
    [[nodiscard]] std::string PathToUtf8String(
        const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {
            reinterpret_cast<const char*>(utf8.data()),
            utf8.size()
        };
    }

    [[nodiscard]] std::filesystem::path PathFromUtf8String(
        const std::string& value)
    {
        return std::filesystem::path(
            std::u8string(
                reinterpret_cast<const char8_t*>(value.data()),
                reinterpret_cast<const char8_t*>(
                    value.data() + value.size())));
    }

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

    template <typename Enum, std::size_t Size>
    [[nodiscard]] Enum ValueOf(
        const std::array<NamedValue<Enum>, Size>& names,
        const std::string_view name,
        const Enum fallback) noexcept
    {
        for (const auto& entry : names)
        {
            if (entry.name == name)
            {
                return entry.value;
            }
        }
        return fallback;
    }

    [[nodiscard]] float FiniteOr(
        const float value,
        const float fallback) noexcept
    {
        return std::isfinite(value) ? value : fallback;
    }

    [[nodiscard]] float Saturate(const float value) noexcept
    {
        return std::clamp(FiniteOr(value, 0.0f), 0.0f, 1.0f);
    }

    [[nodiscard]] DirectX::XMFLOAT4 SaturateColor(
        const DirectX::XMFLOAT4& color) noexcept
    {
        return {
            Saturate(color.x),
            Saturate(color.y),
            Saturate(color.z),
            Saturate(color.w)
        };
    }

    [[nodiscard]] DirectX::XMFLOAT4 WithAlphaScale(
        const DirectX::XMFLOAT4& color,
        const float scale) noexcept
    {
        return {
            color.x,
            color.y,
            color.z,
            color.w * std::clamp(scale, 0.0f, 1.0f)
        };
    }

    // イージングは単調増加なので、二分探索で逆関数を求めます。
    // 途中から覆い直す・開き直すときに見た目を連続させるためです。
    [[nodiscard]] float InverseEasing(
        const SceneTransitionEasing easing,
        const float value) noexcept
    {
        const float target = Saturate(value);
        float low = 0.0f;
        float high = 1.0f;
        for (int iteration{}; iteration < 24; ++iteration)
        {
            const float middle = (low + high) * 0.5f;
            if (LamaPon::EvaluateSceneTransitionEasing(
                    easing,
                    middle)
                < target)
            {
                low = middle;
            }
            else
            {
                high = middle;
            }
        }
        return (low + high) * 0.5f;
    }

    struct Vector2 final
    {
        float x{};
        float y{};
    };

    [[nodiscard]] Vector2 DirectionVector(
        const SceneTransitionDirection direction) noexcept
    {
        switch (direction)
        {
        case SceneTransitionDirection::RightToLeft:
            return { -1.0f, 0.0f };
        case SceneTransitionDirection::TopToBottom:
            return { 0.0f, 1.0f };
        case SceneTransitionDirection::BottomToTop:
            return { 0.0f, -1.0f };
        case SceneTransitionDirection::TopLeftToBottomRight:
            return { InverseSqrt2, InverseSqrt2 };
        case SceneTransitionDirection::TopRightToBottomLeft:
            return { -InverseSqrt2, InverseSqrt2 };
        case SceneTransitionDirection::BottomLeftToTopRight:
            return { InverseSqrt2, -InverseSqrt2 };
        case SceneTransitionDirection::BottomRightToTopLeft:
            return { -InverseSqrt2, -InverseSqrt2 };
        case SceneTransitionDirection::LeftToRight:
        default:
            return { 1.0f, 0.0f };
        }
    }

    [[nodiscard]] SceneTransitionDirection Opposite(
        const SceneTransitionDirection direction) noexcept
    {
        switch (direction)
        {
        case SceneTransitionDirection::RightToLeft:
            return SceneTransitionDirection::LeftToRight;
        case SceneTransitionDirection::TopToBottom:
            return SceneTransitionDirection::BottomToTop;
        case SceneTransitionDirection::BottomToTop:
            return SceneTransitionDirection::TopToBottom;
        case SceneTransitionDirection::TopLeftToBottomRight:
            return SceneTransitionDirection::BottomRightToTopLeft;
        case SceneTransitionDirection::TopRightToBottomLeft:
            return SceneTransitionDirection::BottomLeftToTopRight;
        case SceneTransitionDirection::BottomLeftToTopRight:
            return SceneTransitionDirection::TopRightToBottomLeft;
        case SceneTransitionDirection::BottomRightToTopLeft:
            return SceneTransitionDirection::TopLeftToBottomRight;
        case SceneTransitionDirection::LeftToRight:
        default:
            return SceneTransitionDirection::RightToLeft;
        }
    }

    // 順番orderの要素が、全体の進み具合coverageのときにどこまで
    // 進んでいるかです。staggerが大きいほど1つずつ順に動きます。
    [[nodiscard]] float StaggeredProgress(
        const float coverage,
        const float order,
        const float stagger) noexcept
    {
        const float spread = std::clamp(stagger, 0.0f, 0.95f);
        if (spread <= 0.0f)
        {
            return Saturate(coverage);
        }
        return Saturate(
            (coverage - Saturate(order) * spread)
            / (1.0f - spread));
    }

    void PushRectangle(
        std::vector<SceneTransitionQuad>& quads,
        const float x,
        const float y,
        const float width,
        const float height,
        const DirectX::XMFLOAT4& color)
    {
        if (width <= 0.0f
            || height <= 0.0f
            || color.w <= 0.0f)
        {
            return;
        }
        SceneTransitionQuad quad;
        quad.x = x;
        quad.y = y;
        quad.width = width;
        quad.height = height;
        quad.color = color;
        quads.push_back(quad);
    }

    // 向きnへ進む帯を、画面を横切る矩形として積みます。位置は
    // 画面の四隅をnへ射影した範囲[MinAlong, MaxAlong]で指定します。
    class BandBuilder final
    {
    public:
        BandBuilder(
            const Vector2 direction,
            const float width,
            const float height,
            std::vector<SceneTransitionQuad>& quads)
            : m_along(direction)
            , m_across{ -direction.y, direction.x }
            , m_quads(quads)
        {
            const std::array<Vector2, 4> corners{
                Vector2{ 0.0f, 0.0f },
                Vector2{ width, 0.0f },
                Vector2{ 0.0f, height },
                Vector2{ width, height }
            };
            m_minAlong = m_minAcross = 1.0e30f;
            m_maxAlong = m_maxAcross = -1.0e30f;
            for (const auto& corner : corners)
            {
                const float along =
                    corner.x * m_along.x + corner.y * m_along.y;
                const float across =
                    corner.x * m_across.x + corner.y * m_across.y;
                m_minAlong = std::min(m_minAlong, along);
                m_maxAlong = std::max(m_maxAlong, along);
                m_minAcross = std::min(m_minAcross, across);
                m_maxAcross = std::max(m_maxAcross, across);
            }
        }

        [[nodiscard]] float MinAlong() const noexcept
        {
            return m_minAlong;
        }
        [[nodiscard]] float MaxAlong() const noexcept
        {
            return m_maxAlong;
        }
        [[nodiscard]] float Span() const noexcept
        {
            return m_maxAlong - m_minAlong;
        }

        void Emit(
            float from,
            float to,
            const DirectX::XMFLOAT4& color) const
        {
            from = std::max(from, m_minAlong);
            to = std::min(to, m_maxAlong);
            if (to - from <= 0.0f || color.w <= 0.0f)
            {
                return;
            }
            const bool horizontal = std::abs(m_along.y) < 1.0e-6f;
            const bool vertical = std::abs(m_along.x) < 1.0e-6f;
            // 斜めの帯は回転の誤差で画面の角に隙間ができないよう、
            // 横切る方向だけ少し広げます。
            const float margin =
                horizontal || vertical ? 0.0f : 1.0f;
            const float acrossFrom = m_minAcross - margin;
            const float acrossTo = m_maxAcross + margin;
            const float alongCenter = (from + to) * 0.5f;
            const float acrossCenter = (acrossFrom + acrossTo) * 0.5f;
            const float centerX =
                m_along.x * alongCenter + m_across.x * acrossCenter;
            const float centerY =
                m_along.y * alongCenter + m_across.y * acrossCenter;
            const float alongSize = to - from;
            const float acrossSize = acrossTo - acrossFrom;
            if (horizontal)
            {
                PushRectangle(
                    m_quads,
                    centerX - alongSize * 0.5f,
                    centerY - acrossSize * 0.5f,
                    alongSize,
                    acrossSize,
                    color);
                return;
            }
            if (vertical)
            {
                PushRectangle(
                    m_quads,
                    centerX - acrossSize * 0.5f,
                    centerY - alongSize * 0.5f,
                    acrossSize,
                    alongSize,
                    color);
                return;
            }
            SceneTransitionQuad quad;
            quad.x = centerX - alongSize * 0.5f;
            quad.y = centerY - acrossSize * 0.5f;
            quad.width = alongSize;
            quad.height = acrossSize;
            quad.rotation = std::atan2(m_along.y, m_along.x);
            quad.color = color;
            m_quads.push_back(quad);
        }

        // fromから向きsign（+1で進む向き、-1で戻る向き）へ、
        // 不透明から透明へ段階的に薄くなる帯を積みます。
        void EmitGradient(
            const float from,
            const float length,
            const float sign,
            const DirectX::XMFLOAT4& color) const
        {
            if (length <= 0.0f)
            {
                return;
            }
            const int steps = std::clamp(
                static_cast<int>(std::ceil(length / 4.0f)),
                2,
                24);
            const float step = length / static_cast<float>(steps);
            for (int index{}; index < steps; ++index)
            {
                const float a = from + sign * step * static_cast<float>(index);
                const float b = a + sign * step;
                Emit(
                    std::min(a, b),
                    std::max(a, b),
                    WithAlphaScale(
                        color,
                        1.0f
                            - (static_cast<float>(index) + 0.5f)
                                / static_cast<float>(steps)));
            }
        }

    private:
        Vector2 m_along;
        Vector2 m_across;
        std::vector<SceneTransitionQuad>& m_quads;
        float m_minAlong{};
        float m_maxAlong{};
        float m_minAcross{};
        float m_maxAcross{};
    };

    struct BuildContext final
    {
        const SceneTransitionSettings& settings;
        float coverage{};
        float width{};
        float height{};
        Vector2 direction{};
        std::vector<SceneTransitionQuad>& quads;

        [[nodiscard]] float ShortSide() const noexcept
        {
            return std::min(width, height);
        }
        [[nodiscard]] bool HasAccent() const noexcept
        {
            return settings.accentColor.w > 0.0f
                && settings.accentWidth > 0.0f;
        }
        [[nodiscard]] float AccentPixels() const noexcept
        {
            return HasAccent()
                ? settings.accentWidth * ShortSide()
                : 0.0f;
        }
        void FullScreen(const DirectX::XMFLOAT4& color) const
        {
            PushRectangle(quads, 0.0f, 0.0f, width, height, color);
        }
    };

    // 帯の先端（差し色とぼかし）を含めて、minAlongから
    // lengthぶんを覆い終える片側の覆いです。WipeとShutterで使います。
    void BuildEdgeCover(
        const BuildContext& context,
        const BandBuilder& band,
        const float length)
    {
        const float accent = context.AccentPixels();
        const float soft =
            context.settings.softness * context.ShortSide();
        const float front =
            band.MinAlong()
            + context.coverage * (length + accent + soft)
            - (accent + soft);
        band.Emit(band.MinAlong(), front, context.settings.color);
        if (accent > 0.0f)
        {
            band.Emit(
                front,
                front + accent,
                context.settings.accentColor);
        }
        band.EmitGradient(
            front + accent,
            soft,
            1.0f,
            context.HasAccent()
                ? context.settings.accentColor
                : context.settings.color);
    }

    // 中心(centerX, centerY)、半径radiusの円の外側を塗ります。
    void BuildInverseCircle(
        const BuildContext& context,
        const float centerX,
        const float centerY,
        const float radius,
        const DirectX::XMFLOAT4& color)
    {
        if (radius <= 0.5f)
        {
            context.FullScreen(color);
            return;
        }
        const float size =
            radius / LamaPon::SceneTransitionCircleRadiusRatio;
        const float left = centerX - size * 0.5f;
        const float top = centerY - size * 0.5f;
        const float right = left + size;
        const float bottom = top + size;
        SceneTransitionQuad quad;
        quad.x = left;
        quad.y = top;
        quad.width = size;
        quad.height = size;
        quad.shape = SceneTransitionShape::InverseCircle;
        quad.color = color;
        context.quads.push_back(quad);

        const float width = context.width;
        const float height = context.height;
        if (top > 0.0f)
        {
            PushRectangle(
                context.quads,
                0.0f,
                0.0f,
                width,
                std::min(top, height),
                color);
        }
        if (bottom < height)
        {
            const float y = std::max(bottom, 0.0f);
            PushRectangle(
                context.quads,
                0.0f,
                y,
                width,
                height - y,
                color);
        }
        const float bandTop = std::clamp(top, 0.0f, height);
        const float bandBottom = std::clamp(bottom, 0.0f, height);
        if (bandBottom > bandTop)
        {
            if (left > 0.0f)
            {
                PushRectangle(
                    context.quads,
                    0.0f,
                    bandTop,
                    std::min(left, width),
                    bandBottom - bandTop,
                    color);
            }
            if (right < width)
            {
                const float x = std::max(right, 0.0f);
                PushRectangle(
                    context.quads,
                    x,
                    bandTop,
                    width - x,
                    bandBottom - bandTop,
                    color);
            }
        }
    }

    // 中心からのマンハッタン距離がradiusのひし形の外側を、4辺それぞれの
    // 外側の半平面を覆う帯で塗ります。
    void BuildInverseDiamond(
        const BuildContext& context,
        const float centerX,
        const float centerY,
        const float radius,
        const DirectX::XMFLOAT4& color)
    {
        if (radius <= 0.0f)
        {
            context.FullScreen(color);
            return;
        }
        constexpr std::array<Vector2, 4> normals{
            Vector2{ InverseSqrt2, InverseSqrt2 },
            Vector2{ InverseSqrt2, -InverseSqrt2 },
            Vector2{ -InverseSqrt2, InverseSqrt2 },
            Vector2{ -InverseSqrt2, -InverseSqrt2 }
        };
        for (const auto& normal : normals)
        {
            const BandBuilder band(
                normal,
                context.width,
                context.height,
                context.quads);
            const float edge =
                centerX * normal.x
                + centerY * normal.y
                + radius * InverseSqrt2;
            band.Emit(edge, band.MaxAlong(), color);
        }
    }

    void BuildIrisLike(
        const BuildContext& context,
        const bool diamond)
    {
        const float centerX =
            Saturate(context.settings.focus.x) * context.width;
        const float centerY =
            Saturate(context.settings.focus.y) * context.height;
        const std::array<Vector2, 4> corners{
            Vector2{ 0.0f, 0.0f },
            Vector2{ context.width, 0.0f },
            Vector2{ 0.0f, context.height },
            Vector2{ context.width, context.height }
        };
        float farthest = 0.0f;
        for (const auto& corner : corners)
        {
            const float dx = corner.x - centerX;
            const float dy = corner.y - centerY;
            farthest = std::max(
                farthest,
                diamond
                    ? std::abs(dx) + std::abs(dy)
                    : std::sqrt(dx * dx + dy * dy));
        }
        // ひし形の辺に垂直な幅をaccentにそろえるため、マンハッタン距離では
        // √2倍します。
        const float accent =
            context.AccentPixels() * (diamond ? Sqrt2 : 1.0f);
        const float radius =
            (1.0f - context.coverage) * (farthest + accent) - accent;
        const auto build =
            diamond ? BuildInverseDiamond : BuildInverseCircle;
        if (accent > 0.0f)
        {
            build(
                context,
                centerX,
                centerY,
                radius,
                context.settings.accentColor);
        }
        build(
            context,
            centerX,
            centerY,
            radius + accent,
            context.settings.color);
    }

    // 差し色を使う分割型の演出では、差し色が先に伸び、少し遅れて
    // 覆いの色が追いかけます。覆い終える瞬間に差し色から覆いの色へ
    // 一斉に切り替わる見た目を避けるためです。
    constexpr float AccentLead = 0.25f;

    struct LayeredProgress final
    {
        float accent{};
        float main{};
    };

    [[nodiscard]] LayeredProgress LayeredCoverage(
        const BuildContext& context) noexcept
    {
        if (!context.HasAccent())
        {
            return { 0.0f, context.coverage };
        }
        const float scaled = context.coverage * (1.0f + AccentLead);
        return {
            std::min(scaled, 1.0f),
            std::max(scaled - AccentLead, 0.0f)
        };
    }

    void BuildBlinds(const BuildContext& context)
    {
        const BandBuilder band(
            context.direction,
            context.width,
            context.height,
            context.quads);
        const auto count = std::clamp<std::uint32_t>(
            context.settings.divisions,
            1u,
            64u);
        const float stripe =
            band.Span() / static_cast<float>(count);
        const auto layered = LayeredCoverage(context);
        for (std::uint32_t index{}; index < count; ++index)
        {
            const float order = count > 1
                ? static_cast<float>(index)
                    / static_cast<float>(count - 1)
                : 0.0f;
            const float from =
                band.MinAlong() + stripe * static_cast<float>(index);
            if (context.HasAccent())
            {
                band.Emit(
                    from,
                    from
                        + stripe
                            * StaggeredProgress(
                                layered.accent,
                                order,
                                context.settings.stagger),
                    context.settings.accentColor);
            }
            band.Emit(
                from,
                from
                    + stripe
                        * StaggeredProgress(
                            layered.main,
                            order,
                            context.settings.stagger),
                context.settings.color);
        }
    }

    // 升目の中心に1枚分の図形を積みます。localは0～1の大きさです。
    void PushTile(
        const BuildContext& context,
        const SceneTransitionEffect effect,
        const float centerX,
        const float centerY,
        const float cell,
        const float local,
        const DirectX::XMFLOAT4& color)
    {
        if (local <= 0.0f || color.w <= 0.0f)
        {
            return;
        }
        SceneTransitionQuad quad;
        quad.color = color;
        float size{};
        if (effect == SceneTransitionEffect::DiamondTiles)
        {
            // 45度回した正方形が升目を覆い切るには、一辺が升目の
            // √2倍必要です。
            size = local * (cell * Sqrt2 + 1.0f);
            quad.rotation = Pi * 0.25f;
        }
        else if (effect == SceneTransitionEffect::Dots)
        {
            const float radius = local * (cell * InverseSqrt2 + 0.5f);
            size = radius / LamaPon::SceneTransitionCircleRadiusRatio;
            quad.shape = SceneTransitionShape::Circle;
        }
        else
        {
            // 隣の升目と1pxだけ重ね、継ぎ目の隙間を防ぎます。
            size = local * (cell + 1.0f);
        }
        quad.x = centerX - size * 0.5f;
        quad.y = centerY - size * 0.5f;
        quad.width = size;
        quad.height = size;
        context.quads.push_back(quad);
    }

    void BuildTiles(
        const BuildContext& context,
        const SceneTransitionEffect effect)
    {
        const auto columns = std::clamp<std::uint32_t>(
            context.settings.divisions,
            1u,
            64u);
        const float cell =
            context.width / static_cast<float>(columns);
        const auto rows = static_cast<std::uint32_t>(
            std::max(1.0f, std::ceil(context.height / cell)));
        const BandBuilder band(
            context.direction,
            context.width,
            context.height,
            context.quads);
        const float span = std::max(band.Span(), 1.0f);
        const auto layered = LayeredCoverage(context);
        // 重なり合う図形（ひし形・ドット）で差し色が覆いの色の上へ
        // 描かれないよう、差し色の層を先に全部積みます。
        for (const bool accentLayer : { true, false })
        {
            if (accentLayer && !context.HasAccent())
            {
                continue;
            }
            for (std::uint32_t row{}; row < rows; ++row)
            {
                for (std::uint32_t column{}; column < columns; ++column)
                {
                    const float centerX =
                        (static_cast<float>(column) + 0.5f) * cell;
                    const float centerY =
                        (static_cast<float>(row) + 0.5f) * cell;
                    const float order =
                        (centerX * context.direction.x
                            + centerY * context.direction.y
                            - band.MinAlong())
                        / span;
                    PushTile(
                        context,
                        effect,
                        centerX,
                        centerY,
                        cell,
                        StaggeredProgress(
                            accentLayer ? layered.accent : layered.main,
                            order,
                            context.settings.stagger),
                        accentLayer
                            ? context.settings.accentColor
                            : context.settings.color);
                }
            }
        }
    }

    [[nodiscard]] float ReadNumber(
        const nlohmann::json& value,
        const char* key,
        const float fallback)
    {
        const auto found = value.find(key);
        return found != value.end() && found->is_number()
            ? FiniteOr(found->get<float>(), fallback)
            : fallback;
    }

    [[nodiscard]] bool ReadBoolean(
        const nlohmann::json& value,
        const char* key,
        const bool fallback)
    {
        const auto found = value.find(key);
        return found != value.end() && found->is_boolean()
            ? found->get<bool>()
            : fallback;
    }

    [[nodiscard]] DirectX::XMFLOAT4 ReadColor(
        const nlohmann::json& value,
        const char* key,
        const DirectX::XMFLOAT4& fallback)
    {
        const auto found = value.find(key);
        if (found == value.end()
            || !found->is_array()
            || found->size() != 4
            || !std::all_of(
                found->begin(),
                found->end(),
                [](const nlohmann::json& item)
                {
                    return item.is_number();
                }))
        {
            return fallback;
        }
        return {
            FiniteOr((*found)[0].get<float>(), fallback.x),
            FiniteOr((*found)[1].get<float>(), fallback.y),
            FiniteOr((*found)[2].get<float>(), fallback.z),
            FiniteOr((*found)[3].get<float>(), fallback.w)
        };
    }

    [[nodiscard]] std::string ReadString(
        const nlohmann::json& value,
        const char* key)
    {
        const auto found = value.find(key);
        return found != value.end() && found->is_string()
            ? found->get<std::string>()
            : std::string{};
    }
}

namespace LamaPon
{
    SceneTransitionSettings MakeSceneTransition(
        const SceneTransitionEffect effect,
        const float durationSeconds,
        const DirectX::XMFLOAT4& color)
    {
        SceneTransitionSettings settings;
        settings.effect = effect;
        settings.coverDuration = durationSeconds;
        settings.revealDuration = durationSeconds;
        settings.color = color;
        return SanitizeSceneTransition(settings);
    }

    SceneTransitionSettings SanitizeSceneTransition(
        const SceneTransitionSettings& settings)
    {
        const SceneTransitionSettings defaults;
        SceneTransitionSettings result = settings;
        if (result.effect >= SceneTransitionEffect::Count)
        {
            result.effect = defaults.effect;
        }
        if (result.direction >= SceneTransitionDirection::Count)
        {
            result.direction = defaults.direction;
        }
        if (result.easing >= SceneTransitionEasing::Count)
        {
            result.easing = defaults.easing;
        }
        if (result.shaderPattern
            >= SceneTransitionShaderPattern::Count)
        {
            result.shaderPattern = defaults.shaderPattern;
        }
        constexpr float MaximumSeconds = 30.0f;
        result.coverDuration = std::clamp(
            FiniteOr(result.coverDuration, defaults.coverDuration),
            0.0f,
            MaximumSeconds);
        result.holdDuration = std::clamp(
            FiniteOr(result.holdDuration, defaults.holdDuration),
            0.0f,
            MaximumSeconds);
        result.revealDuration = std::clamp(
            FiniteOr(result.revealDuration, defaults.revealDuration),
            0.0f,
            MaximumSeconds);
        result.color = SaturateColor(result.color);
        result.accentColor = SaturateColor(result.accentColor);
        result.accentWidth = std::clamp(
            FiniteOr(result.accentWidth, defaults.accentWidth),
            0.0f,
            0.5f);
        result.softness = std::clamp(
            FiniteOr(result.softness, defaults.softness),
            0.0f,
            1.0f);
        result.divisions =
            std::clamp<std::uint32_t>(result.divisions, 1u, 64u);
        result.stagger = std::clamp(
            FiniteOr(result.stagger, defaults.stagger),
            0.0f,
            0.95f);
        result.focus = {
            Saturate(result.focus.x),
            Saturate(result.focus.y)
        };
        return result;
    }

    float EvaluateSceneTransitionEasing(
        const SceneTransitionEasing easing,
        const float t) noexcept
    {
        const float x = Saturate(t);
        switch (easing)
        {
        case SceneTransitionEasing::EaseInQuad:
            return x * x;
        case SceneTransitionEasing::EaseOutQuad:
            return 1.0f - (1.0f - x) * (1.0f - x);
        case SceneTransitionEasing::EaseInOutQuad:
            return x < 0.5f
                ? 2.0f * x * x
                : 1.0f - std::pow(-2.0f * x + 2.0f, 2.0f) * 0.5f;
        case SceneTransitionEasing::EaseInCubic:
            return x * x * x;
        case SceneTransitionEasing::EaseOutCubic:
            return 1.0f - std::pow(1.0f - x, 3.0f);
        case SceneTransitionEasing::EaseInOutCubic:
            return x < 0.5f
                ? 4.0f * x * x * x
                : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) * 0.5f;
        case SceneTransitionEasing::EaseInOutSine:
            return -(std::cos(Pi * x) - 1.0f) * 0.5f;
        case SceneTransitionEasing::Linear:
        default:
            return x;
        }
    }

    std::string_view SceneTransitionEffectName(
        const SceneTransitionEffect effect) noexcept
    {
        return NameOf(EffectNames, effect);
    }

    SceneTransitionEffect SceneTransitionEffectFromName(
        const std::string_view name) noexcept
    {
        return ValueOf(EffectNames, name, SceneTransitionEffect::None);
    }

    std::string_view SceneTransitionDirectionName(
        const SceneTransitionDirection direction) noexcept
    {
        return NameOf(DirectionNames, direction);
    }

    SceneTransitionDirection SceneTransitionDirectionFromName(
        const std::string_view name) noexcept
    {
        return ValueOf(
            DirectionNames,
            name,
            SceneTransitionDirection::LeftToRight);
    }

    std::string_view SceneTransitionEasingName(
        const SceneTransitionEasing easing) noexcept
    {
        return NameOf(EasingNames, easing);
    }

    SceneTransitionEasing SceneTransitionEasingFromName(
        const std::string_view name) noexcept
    {
        return ValueOf(
            EasingNames,
            name,
            SceneTransitionEasing::EaseInOutCubic);
    }

    std::string_view SceneTransitionShaderPatternName(
        const SceneTransitionShaderPattern pattern) noexcept
    {
        return NameOf(ShaderPatternNames, pattern);
    }

    SceneTransitionShaderPattern SceneTransitionShaderPatternFromName(
        const std::string_view name) noexcept
    {
        return ValueOf(
            ShaderPatternNames,
            name,
            SceneTransitionShaderPattern::Dissolve);
    }

    nlohmann::json SceneTransitionToJson(
        const SceneTransitionSettings& source)
    {
        const auto settings = SanitizeSceneTransition(source);
        const auto color = [](const DirectX::XMFLOAT4& value)
        {
            return nlohmann::json::array(
                { value.x, value.y, value.z, value.w });
        };
        return nlohmann::json{
            {
                "effect",
                std::string(SceneTransitionEffectName(settings.effect))
            },
            {
                "direction",
                std::string(
                    SceneTransitionDirectionName(settings.direction))
            },
            {
                "easing",
                std::string(SceneTransitionEasingName(settings.easing))
            },
            { "coverDuration", settings.coverDuration },
            { "holdDuration", settings.holdDuration },
            { "revealDuration", settings.revealDuration },
            { "color", color(settings.color) },
            { "accentColor", color(settings.accentColor) },
            { "accentWidth", settings.accentWidth },
            { "softness", settings.softness },
            { "divisions", settings.divisions },
            { "stagger", settings.stagger },
            {
                "focus",
                nlohmann::json::array(
                    { settings.focus.x, settings.focus.y })
            },
            { "passThrough", settings.passThrough },
            { "showLoadingScreen", settings.showLoadingScreen },
            { "blockInput", settings.blockInput },
            { "fadeMusic", settings.fadeMusic },
            {
                "shaderPattern",
                std::string(SceneTransitionShaderPatternName(
                    settings.shaderPattern))
            },
            { "ruleTexture", PathToUtf8String(settings.ruleTexture) },
            { "shader", PathToUtf8String(settings.shader) },
        };
    }

    SceneTransitionSettings SceneTransitionFromJson(
        const nlohmann::json& value,
        const SceneTransitionSettings& fallback)
    {
        if (!value.is_object())
        {
            return SanitizeSceneTransition(fallback);
        }
        SceneTransitionSettings settings = fallback;
        if (const auto name = ReadString(value, "effect");
            !name.empty())
        {
            settings.effect = ValueOf(
                EffectNames,
                name,
                fallback.effect);
        }
        if (const auto name = ReadString(value, "direction");
            !name.empty())
        {
            settings.direction = ValueOf(
                DirectionNames,
                name,
                fallback.direction);
        }
        if (const auto name = ReadString(value, "easing");
            !name.empty())
        {
            settings.easing = ValueOf(
                EasingNames,
                name,
                fallback.easing);
        }
        settings.coverDuration = ReadNumber(
            value, "coverDuration", fallback.coverDuration);
        settings.holdDuration = ReadNumber(
            value, "holdDuration", fallback.holdDuration);
        settings.revealDuration = ReadNumber(
            value, "revealDuration", fallback.revealDuration);
        settings.color = ReadColor(value, "color", fallback.color);
        settings.accentColor = ReadColor(
            value, "accentColor", fallback.accentColor);
        settings.accentWidth = ReadNumber(
            value, "accentWidth", fallback.accentWidth);
        settings.softness = ReadNumber(
            value, "softness", fallback.softness);
        if (const auto divisions = value.find("divisions");
            divisions != value.end()
            && divisions->is_number_integer())
        {
            settings.divisions = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(
                    divisions->get<std::int64_t>(),
                    1,
                    64));
        }
        settings.stagger = ReadNumber(
            value, "stagger", fallback.stagger);
        if (const auto focus = value.find("focus");
            focus != value.end()
            && focus->is_array()
            && focus->size() == 2
            && (*focus)[0].is_number()
            && (*focus)[1].is_number())
        {
            settings.focus = {
                FiniteOr((*focus)[0].get<float>(), fallback.focus.x),
                FiniteOr((*focus)[1].get<float>(), fallback.focus.y)
            };
        }
        settings.passThrough = ReadBoolean(
            value, "passThrough", fallback.passThrough);
        settings.showLoadingScreen = ReadBoolean(
            value, "showLoadingScreen", fallback.showLoadingScreen);
        settings.blockInput = ReadBoolean(
            value, "blockInput", fallback.blockInput);
        settings.fadeMusic = ReadBoolean(
            value, "fadeMusic", fallback.fadeMusic);
        if (const auto name = ReadString(value, "shaderPattern");
            !name.empty())
        {
            settings.shaderPattern = ValueOf(
                ShaderPatternNames,
                name,
                fallback.shaderPattern);
        }
        if (const auto found = value.find("ruleTexture");
            found != value.end() && found->is_string())
        {
            settings.ruleTexture =
                PathFromUtf8String(found->get<std::string>());
        }
        if (const auto found = value.find("shader");
            found != value.end() && found->is_string())
        {
            settings.shader =
                PathFromUtf8String(found->get<std::string>());
        }
        return SanitizeSceneTransition(settings);
    }

    nlohmann::json SceneLoadingScreenToJson(
        const SceneLoadingScreenSettings& settings)
    {
        const auto color = [](const DirectX::XMFLOAT4& value)
        {
            return nlohmann::json::array(
                { value.x, value.y, value.z, value.w });
        };
        return nlohmann::json{
            { "enabled", settings.enabled },
            { "message", settings.message },
            { "backgroundColor", color(settings.backgroundColor) },
            { "barBackgroundColor", color(settings.barBackgroundColor) },
            { "barFillColor", color(settings.barFillColor) },
            { "textColor", color(settings.textColor) },
            { "showPercentage", settings.showPercentage },
            { "hint", settings.hint },
            {
                "backgroundTexture",
                PathToUtf8String(settings.backgroundTexture)
            },
            { "showSpinner", settings.showSpinner },
            { "smoothProgress", settings.smoothProgress },
            {
                "fadeDuration",
                std::clamp(
                    FiniteOr(settings.fadeDuration, 0.2f),
                    0.0f,
                    5.0f)
            },
        };
    }

    SceneLoadingScreenSettings SceneLoadingScreenFromJson(
        const nlohmann::json& value,
        const SceneLoadingScreenSettings& fallback)
    {
        if (!value.is_object())
        {
            return fallback;
        }
        SceneLoadingScreenSettings settings = fallback;
        settings.enabled =
            ReadBoolean(value, "enabled", fallback.enabled);
        if (const auto found = value.find("message");
            found != value.end() && found->is_string())
        {
            settings.message = found->get<std::string>();
        }
        settings.backgroundColor = SaturateColor(ReadColor(
            value, "backgroundColor", fallback.backgroundColor));
        settings.barBackgroundColor = SaturateColor(ReadColor(
            value, "barBackgroundColor", fallback.barBackgroundColor));
        settings.barFillColor = SaturateColor(ReadColor(
            value, "barFillColor", fallback.barFillColor));
        settings.textColor = SaturateColor(ReadColor(
            value, "textColor", fallback.textColor));
        settings.showPercentage = ReadBoolean(
            value, "showPercentage", fallback.showPercentage);
        if (const auto found = value.find("hint");
            found != value.end() && found->is_string())
        {
            settings.hint = found->get<std::string>();
        }
        if (const auto found = value.find("backgroundTexture");
            found != value.end() && found->is_string())
        {
            settings.backgroundTexture =
                PathFromUtf8String(found->get<std::string>());
        }
        settings.showSpinner = ReadBoolean(
            value, "showSpinner", fallback.showSpinner);
        settings.smoothProgress = ReadBoolean(
            value, "smoothProgress", fallback.smoothProgress);
        settings.fadeDuration = std::clamp(
            ReadNumber(value, "fadeDuration", fallback.fadeDuration),
            0.0f,
            5.0f);
        return settings;
    }

    void SceneTransitionTimeline::Start(
        const SceneTransitionSettings& settings)
    {
        const float currentCoverage = Coverage();
        const auto previousPhase = m_phase;
        m_settings = SanitizeSceneTransition(settings);
        m_heldSeconds = 0.0f;
        m_readyFrames = 0;
        if (previousPhase == SceneTransitionPhase::Covered)
        {
            m_progress = 1.0f;
            return;
        }
        m_phase = SceneTransitionPhase::Covering;
        m_progress = previousPhase == SceneTransitionPhase::Idle
            ? 0.0f
            : InverseEasing(m_settings.easing, currentCoverage);
    }

    SceneTransitionTimelineEvents SceneTransitionTimeline::Advance(
        float deltaSeconds,
        const bool readyToReveal) noexcept
    {
        SceneTransitionTimelineEvents events;
        deltaSeconds = std::max(FiniteOr(deltaSeconds, 0.0f), 0.0f);
        // Noneは覆いを描かないので、時間を掛けずに段階だけ進めます。
        const bool instant =
            m_settings.effect == SceneTransitionEffect::None;
        const auto advance = [this, deltaSeconds](
                const float duration) noexcept
            {
                m_progress = duration > 0.0f
                    ? std::min(m_progress + deltaSeconds / duration, 1.0f)
                    : 1.0f;
            };
        switch (m_phase)
        {
        case SceneTransitionPhase::Covering:
            advance(instant ? 0.0f : m_settings.coverDuration);
            if (m_progress >= 1.0f)
            {
                m_phase = SceneTransitionPhase::Covered;
                m_progress = 1.0f;
                m_heldSeconds = 0.0f;
                m_readyFrames = 0;
                events.covered = true;
            }
            break;
        case SceneTransitionPhase::Covered:
        {
            m_heldSeconds += deltaSeconds;
            m_readyFrames = readyToReveal
                ? std::min(m_readyFrames + 1, 1000u)
                : 0u;
            // 有効化した直後の新シーンは、Startの呼び出しや大きな
            // テクスチャの転送で最初の数フレームが不安定になりやすいため、
            // 覆いのある演出では準備完了から2フレーム覆ったまま待ちます。
            const std::uint32_t requiredFrames = instant ? 1u : 2u;
            const float hold = instant ? 0.0f : m_settings.holdDuration;
            if (m_readyFrames >= requiredFrames
                && m_heldSeconds >= hold)
            {
                m_phase = SceneTransitionPhase::Revealing;
                m_progress = 0.0f;
                events.revealStarted = true;
            }
            break;
        }
        case SceneTransitionPhase::Revealing:
            advance(instant ? 0.0f : m_settings.revealDuration);
            if (m_progress >= 1.0f)
            {
                Reset();
                events.finished = true;
            }
            break;
        case SceneTransitionPhase::Idle:
        default:
            break;
        }
        return events;
    }

    void SceneTransitionTimeline::Reveal() noexcept
    {
        if (m_phase == SceneTransitionPhase::Idle
            || m_phase == SceneTransitionPhase::Revealing)
        {
            return;
        }
        const float currentCoverage = Coverage();
        m_phase = SceneTransitionPhase::Revealing;
        m_progress = InverseEasing(
            m_settings.easing,
            1.0f - currentCoverage);
        m_heldSeconds = 0.0f;
        m_readyFrames = 0;
    }

    void SceneTransitionTimeline::Reset() noexcept
    {
        m_phase = SceneTransitionPhase::Idle;
        m_progress = 0.0f;
        m_heldSeconds = 0.0f;
        m_readyFrames = 0;
    }

    float SceneTransitionTimeline::Coverage() const noexcept
    {
        switch (m_phase)
        {
        case SceneTransitionPhase::Covering:
            return EvaluateSceneTransitionEasing(
                m_settings.easing,
                m_progress);
        case SceneTransitionPhase::Covered:
            return 1.0f;
        case SceneTransitionPhase::Revealing:
            return 1.0f
                - EvaluateSceneTransitionEasing(
                    m_settings.easing,
                    m_progress);
        case SceneTransitionPhase::Idle:
        default:
            return 0.0f;
        }
    }

    SceneTransitionShaderFrame BuildSceneTransitionShaderFrame(
        const SceneTransitionSettings& source,
        const float coverage,
        const bool revealing,
        const float width,
        const float height,
        const bool hasRuleTexture)
    {
        const auto settings = SanitizeSceneTransition(source);
        SceneTransitionShaderFrame frame;
        frame.pattern =
            settings.shaderPattern == SceneTransitionShaderPattern::RuleImage
                && !hasRuleTexture
                ? SceneTransitionShaderPattern::Dissolve
                : settings.shaderPattern;
        // 境界が0だとジャギーが目立つため、シェーダーでは最小限の
        // ぼかしを残します。
        const float softness = std::max(settings.softness, 0.02f);
        const bool hasAccent =
            settings.accentColor.w > 0.0f && settings.accentWidth > 0.0f;
        const auto direction = DirectionVector(settings.direction);
        frame.parameters[0] = {
            Saturate(coverage),
            softness,
            hasAccent ? settings.accentWidth : 0.0f,
            static_cast<float>(frame.pattern)
        };
        // 通り抜ける演出では、先に覆った画素から先に開けます。
        frame.parameters[1] = {
            revealing && settings.passThrough ? 1.0f : 0.0f,
            settings.stagger,
            static_cast<float>(settings.divisions),
            0.0f
        };
        frame.parameters[2] = settings.accentColor;
        frame.parameters[3] = {
            std::max(width, 0.0f),
            std::max(height, 0.0f),
            settings.focus.x * std::max(width, 0.0f),
            settings.focus.y * std::max(height, 0.0f)
        };
        frame.parameters[4] = { direction.x, direction.y, 0.0f, 0.0f };
        frame.tint = settings.color;
        return frame;
    }

    void BuildSceneTransitionQuads(
        const SceneTransitionSettings& source,
        const float coverage,
        const bool revealing,
        const float width,
        const float height,
        std::vector<SceneTransitionQuad>& quads)
    {
        quads.clear();
        const auto settings = SanitizeSceneTransition(source);
        const float clamped = Saturate(coverage);
        if (!(width > 0.0f)
            || !(height > 0.0f)
            || settings.effect == SceneTransitionEffect::None
            || clamped <= 0.0f)
        {
            return;
        }
        const auto direction =
            revealing && settings.passThrough
                ? Opposite(settings.direction)
                : settings.direction;
        const BuildContext context{
            settings,
            clamped,
            width,
            height,
            DirectionVector(direction),
            quads
        };
        if (clamped >= 1.0f)
        {
            context.FullScreen(settings.color);
            return;
        }
        switch (settings.effect)
        {
        case SceneTransitionEffect::Fade:
        case SceneTransitionEffect::Shader:
            context.FullScreen(WithAlphaScale(settings.color, clamped));
            break;
        case SceneTransitionEffect::Wipe:
        {
            const BandBuilder band(
                context.direction,
                width,
                height,
                quads);
            BuildEdgeCover(context, band, band.Span());
            break;
        }
        case SceneTransitionEffect::Shutter:
        {
            const BandBuilder forward(
                context.direction,
                width,
                height,
                quads);
            const BandBuilder backward(
                { -context.direction.x, -context.direction.y },
                width,
                height,
                quads);
            BuildEdgeCover(context, forward, forward.Span() * 0.5f);
            BuildEdgeCover(context, backward, backward.Span() * 0.5f);
            break;
        }
        case SceneTransitionEffect::Iris:
            BuildIrisLike(context, false);
            break;
        case SceneTransitionEffect::Diamond:
            BuildIrisLike(context, true);
            break;
        case SceneTransitionEffect::Blinds:
            BuildBlinds(context);
            break;
        case SceneTransitionEffect::Tiles:
        case SceneTransitionEffect::DiamondTiles:
        case SceneTransitionEffect::Dots:
            BuildTiles(context, settings.effect);
            break;
        case SceneTransitionEffect::None:
        case SceneTransitionEffect::Count:
        default:
            break;
        }
    }
}
