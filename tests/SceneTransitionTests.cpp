// シーン遷移演出の純粋ロジック（イージング、時間軸、形状生成、保存形式）を
// 検査します。GPUを使わず、生成した矩形列をCPUで塗りつぶして
// 「どれだけ画面を覆っているか」を測ります。

#include "LamaPon/Scene/SceneTransition.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right,
        const float tolerance = 1.0e-4f) noexcept
    {
        return std::abs(left - right) <= tolerance;
    }

    constexpr float ScreenWidth = 1280.0f;
    constexpr float ScreenHeight = 720.0f;

    // 1枚のquadが点(x, y)をどれだけ覆うか（0～1）を返します。
    [[nodiscard]] float QuadAlphaAt(
        const LamaPon::SceneTransitionQuad& quad,
        const float x,
        const float y) noexcept
    {
        const float centerX = quad.x + quad.width * 0.5f;
        const float centerY = quad.y + quad.height * 0.5f;
        // 回転を戻して、回転前の矩形の座標系で判定します。
        const float cosine = std::cos(-quad.rotation);
        const float sine = std::sin(-quad.rotation);
        const float dx = x - centerX;
        const float dy = y - centerY;
        const float localX = dx * cosine - dy * sine;
        const float localY = dx * sine + dy * cosine;
        if (std::abs(localX) > quad.width * 0.5f
            || std::abs(localY) > quad.height * 0.5f)
        {
            return 0.0f;
        }
        const float radius =
            quad.width * LamaPon::SceneTransitionCircleRadiusRatio;
        const float distance =
            std::sqrt(localX * localX + localY * localY);
        switch (quad.shape)
        {
        case LamaPon::SceneTransitionShape::Circle:
            return distance <= radius ? quad.color.w : 0.0f;
        case LamaPon::SceneTransitionShape::InverseCircle:
            return distance >= radius ? quad.color.w : 0.0f;
        case LamaPon::SceneTransitionShape::Rectangle:
        default:
            return quad.color.w;
        }
    }

    [[nodiscard]] float AlphaAt(
        const std::vector<LamaPon::SceneTransitionQuad>& quads,
        const float x,
        const float y) noexcept
    {
        float alpha = 0.0f;
        for (const auto& quad : quads)
        {
            const float source = QuadAlphaAt(quad, x, y);
            alpha = alpha + (1.0f - alpha) * source;
        }
        return alpha;
    }

    // 画面を格子状に標本化し、覆われている割合を返します。
    [[nodiscard]] float CoveredFraction(
        const std::vector<LamaPon::SceneTransitionQuad>& quads)
    {
        constexpr int Columns = 96;
        constexpr int Rows = 54;
        float sum = 0.0f;
        for (int row{}; row < Rows; ++row)
        {
            for (int column{}; column < Columns; ++column)
            {
                const float x =
                    (static_cast<float>(column) + 0.5f)
                    * ScreenWidth / static_cast<float>(Columns);
                const float y =
                    (static_cast<float>(row) + 0.5f)
                    * ScreenHeight / static_cast<float>(Rows);
                sum += AlphaAt(quads, x, y);
            }
        }
        return sum / static_cast<float>(Columns * Rows);
    }

    [[nodiscard]] std::vector<LamaPon::SceneTransitionQuad> Build(
        const LamaPon::SceneTransitionSettings& settings,
        const float coverage,
        const bool revealing = false)
    {
        std::vector<LamaPon::SceneTransitionQuad> quads;
        LamaPon::BuildSceneTransitionQuads(
            settings,
            coverage,
            revealing,
            ScreenWidth,
            ScreenHeight,
            quads);
        return quads;
    }

    void TestEasing()
    {
        using LamaPon::SceneTransitionEasing;
        for (auto index = 0;
            index < static_cast<int>(SceneTransitionEasing::Count);
            ++index)
        {
            const auto easing =
                static_cast<SceneTransitionEasing>(index);
            const auto name = std::string(
                LamaPon::SceneTransitionEasingName(easing));
            Require(
                NearlyEqual(
                    LamaPon::EvaluateSceneTransitionEasing(
                        easing,
                        0.0f),
                    0.0f)
                    && NearlyEqual(
                        LamaPon::EvaluateSceneTransitionEasing(
                            easing,
                            1.0f),
                        1.0f),
                "Easing must start at 0 and end at 1: " + name);
            float previous = 0.0f;
            for (int step{}; step <= 100; ++step)
            {
                const float value =
                    LamaPon::EvaluateSceneTransitionEasing(
                        easing,
                        static_cast<float>(step) / 100.0f);
                Require(
                    value + 1.0e-5f >= previous,
                    "Easing must not go backwards: " + name);
                previous = value;
            }
            Require(
                LamaPon::SceneTransitionEasingFromName(name) == easing,
                "Easing name must round-trip: " + name);
            Require(
                NearlyEqual(
                    LamaPon::EvaluateSceneTransitionEasing(
                        easing,
                        -3.0f),
                    0.0f)
                    && NearlyEqual(
                        LamaPon::EvaluateSceneTransitionEasing(
                            easing,
                            4.0f),
                        1.0f),
                "Easing input must be clamped: " + name);
        }
        Require(
            LamaPon::SceneTransitionEasingFromName("unknown")
                == SceneTransitionEasing::EaseInOutCubic,
            "Unknown easing names must fall back to the default.");
    }

    void TestNames()
    {
        using LamaPon::SceneTransitionDirection;
        using LamaPon::SceneTransitionEffect;
        for (auto index = 0;
            index < static_cast<int>(SceneTransitionEffect::Count);
            ++index)
        {
            const auto effect =
                static_cast<SceneTransitionEffect>(index);
            Require(
                LamaPon::SceneTransitionEffectFromName(
                    LamaPon::SceneTransitionEffectName(effect))
                    == effect,
                "Effect name must round-trip: "
                    + std::string(
                        LamaPon::SceneTransitionEffectName(effect)));
        }
        for (auto index = 0;
            index < static_cast<int>(SceneTransitionDirection::Count);
            ++index)
        {
            const auto direction =
                static_cast<SceneTransitionDirection>(index);
            Require(
                LamaPon::SceneTransitionDirectionFromName(
                    LamaPon::SceneTransitionDirectionName(direction))
                    == direction,
                "Direction name must round-trip: "
                    + std::string(
                        LamaPon::SceneTransitionDirectionName(
                            direction)));
        }
        Require(
            LamaPon::SceneTransitionEffectFromName("sparkle")
                == SceneTransitionEffect::None,
            "Unknown effects must fall back to None.");
        using LamaPon::SceneTransitionShaderPattern;
        for (auto index = 0;
            index < static_cast<int>(SceneTransitionShaderPattern::Count);
            ++index)
        {
            const auto pattern =
                static_cast<SceneTransitionShaderPattern>(index);
            Require(
                LamaPon::SceneTransitionShaderPatternFromName(
                    LamaPon::SceneTransitionShaderPatternName(pattern))
                    == pattern,
                "Shader pattern name must round-trip: "
                    + std::string(
                        LamaPon::SceneTransitionShaderPatternName(
                            pattern)));
        }
        Require(
            LamaPon::SceneTransitionShaderPatternFromName("unknown")
                == SceneTransitionShaderPattern::Dissolve,
            "Unknown shader patterns must fall back to Dissolve.");
    }

    void TestJson()
    {
        using namespace LamaPon;
        SceneTransitionSettings settings;
        settings.effect = SceneTransitionEffect::DiamondTiles;
        settings.direction =
            SceneTransitionDirection::BottomRightToTopLeft;
        settings.easing = SceneTransitionEasing::EaseOutQuad;
        settings.coverDuration = 0.75f;
        settings.holdDuration = 0.25f;
        settings.revealDuration = 1.5f;
        settings.color = { 0.1f, 0.2f, 0.3f, 0.9f };
        settings.accentColor = { 1.0f, 0.5f, 0.0f, 1.0f };
        settings.accentWidth = 0.05f;
        settings.softness = 0.2f;
        settings.divisions = 17;
        settings.stagger = 0.8f;
        settings.focus = { 0.25f, 0.75f };
        settings.passThrough = false;
        settings.showLoadingScreen = false;
        settings.blockInput = false;
        settings.fadeMusic = false;
        settings.shaderPattern = SceneTransitionShaderPattern::Heart;
        settings.ruleTexture = u8"textures/ルール画像.png";
        settings.shader = "shaders/MyTransition.hlsl";

        const auto json = SceneTransitionToJson(settings);
        Require(
            json.at("effect") == "diamondTiles"
                && json.at("direction") == "bottomRightToTopLeft"
                && json.at("easing") == "easeOutQuad",
            "Transition enums must be saved by name.");
        const auto loaded = SceneTransitionFromJson(
            nlohmann::json::parse(json.dump()));
        Require(
            loaded.effect == settings.effect
                && loaded.direction == settings.direction
                && loaded.easing == settings.easing
                && NearlyEqual(loaded.coverDuration, 0.75f)
                && NearlyEqual(loaded.holdDuration, 0.25f)
                && NearlyEqual(loaded.revealDuration, 1.5f)
                && NearlyEqual(loaded.color.z, 0.3f)
                && NearlyEqual(loaded.color.w, 0.9f)
                && NearlyEqual(loaded.accentColor.y, 0.5f)
                && NearlyEqual(loaded.accentWidth, 0.05f)
                && NearlyEqual(loaded.softness, 0.2f)
                && loaded.divisions == 17
                && NearlyEqual(loaded.stagger, 0.8f)
                && NearlyEqual(loaded.focus.x, 0.25f)
                && NearlyEqual(loaded.focus.y, 0.75f)
                && !loaded.passThrough
                && !loaded.showLoadingScreen
                && !loaded.blockInput
                && !loaded.fadeMusic
                && loaded.shaderPattern
                    == SceneTransitionShaderPattern::Heart
                && loaded.ruleTexture == settings.ruleTexture
                && loaded.shader == settings.shader,
            "Transition settings must round-trip through JSON.");
        Require(
            json.at("ruleTexture").get<std::string>()
                == "textures/\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\xAB"
                   "\xE7\x94\xBB\xE5\x83\x8F.png",
            "Transition asset paths must be saved as UTF-8.");

        SceneTransitionSettings fallback;
        fallback.effect = SceneTransitionEffect::Fade;
        fallback.coverDuration = 2.0f;
        const auto partial = SceneTransitionFromJson(
            nlohmann::json{
                { "direction", "topToBottom" },
                { "divisions", 999 },
                { "stagger", -4.0 },
                { "color", nlohmann::json::array({ 2.0, -1.0, 0.5, 1.0 }) },
                { "effect", "unknown-effect" },
            },
            fallback);
        Require(
            partial.effect == SceneTransitionEffect::Fade
                && partial.direction
                    == SceneTransitionDirection::TopToBottom
                && NearlyEqual(partial.coverDuration, 2.0f)
                && partial.divisions == 64
                && NearlyEqual(partial.stagger, 0.0f)
                && NearlyEqual(partial.color.x, 1.0f)
                && NearlyEqual(partial.color.y, 0.0f),
            "Missing or invalid JSON values must use the fallback or be clamped.");
        Require(
            SceneTransitionFromJson(nlohmann::json(42), fallback).effect
                == SceneTransitionEffect::Fade,
            "Non-object JSON must return the fallback.");

        SceneTransitionSettings broken;
        broken.coverDuration =
            std::numeric_limits<float>::quiet_NaN();
        broken.revealDuration = -5.0f;
        broken.holdDuration = 1000.0f;
        const auto sanitized = SanitizeSceneTransition(broken);
        Require(
            NearlyEqual(
                sanitized.coverDuration,
                SceneTransitionSettings{}.coverDuration)
                && NearlyEqual(sanitized.revealDuration, 0.0f)
                && NearlyEqual(sanitized.holdDuration, 30.0f),
            "Non-finite or out-of-range durations must be sanitized.");

        const auto made = MakeSceneTransition(
            SceneTransitionEffect::Iris,
            0.8f,
            { 1.0f, 1.0f, 1.0f, 1.0f });
        Require(
            made.effect == SceneTransitionEffect::Iris
                && NearlyEqual(made.coverDuration, 0.8f)
                && NearlyEqual(made.revealDuration, 0.8f)
                && NearlyEqual(made.color.x, 1.0f),
            "MakeSceneTransition must fill the effect, durations and color.");
    }

    void TestTimeline()
    {
        using namespace LamaPon;
        auto settings = MakeSceneTransition(
            SceneTransitionEffect::Fade,
            0.5f);
        settings.holdDuration = 0.2f;
        settings.easing = SceneTransitionEasing::Linear;

        SceneTransitionTimeline timeline;
        Require(
            !timeline.IsActive()
                && NearlyEqual(timeline.Coverage(), 0.0f),
            "A new timeline must be idle.");
        timeline.Start(settings);
        Require(
            timeline.Phase() == SceneTransitionPhase::Covering
                && NearlyEqual(timeline.Coverage(), 0.0f),
            "Start must begin covering from zero.");

        auto events = timeline.Advance(0.25f, false);
        Require(
            !events.covered
                && NearlyEqual(timeline.Coverage(), 0.5f),
            "Covering must follow the cover duration.");
        events = timeline.Advance(0.3f, false);
        Require(
            events.covered
                && timeline.IsFullyCovered()
                && NearlyEqual(timeline.Coverage(), 1.0f),
            "Covering must finish with a covered event.");

        // 準備が整うまでは保持時間を過ぎても開きません。
        for (int frame{}; frame < 10; ++frame)
        {
            events = timeline.Advance(0.1f, false);
            Require(
                !events.revealStarted && timeline.IsFullyCovered(),
                "The transition must stay covered until the scene is ready.");
        }
        events = timeline.Advance(0.016f, true);
        Require(
            !events.revealStarted,
            "A covering effect must stay covered for the first ready frame.");
        events = timeline.Advance(0.016f, true);
        Require(
            events.revealStarted
                && timeline.Phase() == SceneTransitionPhase::Revealing,
            "Revealing must start after the scene has been ready for two frames.");

        float previous = timeline.Coverage();
        bool finished = false;
        for (int frame{}; frame < 100 && !finished; ++frame)
        {
            events = timeline.Advance(0.02f, true);
            Require(
                timeline.Coverage() <= previous + 1.0e-5f,
                "Coverage must decrease while revealing.");
            previous = timeline.Coverage();
            finished = events.finished;
        }
        Require(
            finished
                && !timeline.IsActive()
                && NearlyEqual(timeline.Coverage(), 0.0f),
            "Revealing must finish and return to idle.");

        // 保持時間は準備完了とは別に守ります。
        timeline.Start(settings);
        static_cast<void>(timeline.Advance(1.0f, true));
        Require(timeline.IsFullyCovered(), "Cover must finish.");
        static_cast<void>(timeline.Advance(0.05f, true));
        events = timeline.Advance(0.05f, true);
        Require(
            !events.revealStarted,
            "The hold duration must keep the screen covered.");
        events = timeline.Advance(0.15f, true);
        Require(
            events.revealStarted,
            "Revealing must start once the hold duration has passed.");

        // 覆っている途中で開き直しても、覆い具合は連続します。
        timeline.Reset();
        timeline.Start(settings);
        static_cast<void>(timeline.Advance(0.15f, false));
        const float partial = timeline.Coverage();
        timeline.Reveal();
        Require(
            timeline.Phase() == SceneTransitionPhase::Revealing
                && NearlyEqual(timeline.Coverage(), partial, 1.0e-3f),
            "Reveal must continue from the current coverage.");

        // 開いている途中から次の遷移を始めても、覆い具合は連続します。
        auto cubic = settings;
        cubic.easing = SceneTransitionEasing::EaseInOutCubic;
        static_cast<void>(timeline.Advance(0.05f, true));
        const float beforeRestart = timeline.Coverage();
        timeline.Start(cubic);
        Require(
            timeline.Phase() == SceneTransitionPhase::Covering
                && NearlyEqual(
                    timeline.Coverage(),
                    beforeRestart,
                    1.0e-3f),
            "Restarting while revealing must continue from the current coverage.");

        // Noneは時間を掛けずに段階だけ進みます。
        SceneTransitionTimeline instant;
        instant.Start(SceneTransitionSettings{});
        events = instant.Advance(0.0f, false);
        Require(
            events.covered && instant.IsFullyCovered(),
            "None must cover immediately.");
        events = instant.Advance(0.0f, true);
        Require(
            events.revealStarted,
            "None must reveal on the first ready frame.");
        events = instant.Advance(0.0f, true);
        Require(
            events.finished && !instant.IsActive(),
            "None must finish immediately.");

        // 非有限の経過時間は無視します。
        SceneTransitionTimeline guarded;
        guarded.Start(settings);
        static_cast<void>(guarded.Advance(
            std::numeric_limits<float>::infinity(),
            false));
        static_cast<void>(guarded.Advance(
            std::numeric_limits<float>::quiet_NaN(),
            false));
        Require(
            guarded.Phase() == SceneTransitionPhase::Covering
                && NearlyEqual(guarded.Coverage(), 0.0f),
            "Non-finite delta times must not advance the transition.");
    }

    void TestGeometry()
    {
        using namespace LamaPon;
        for (auto effectIndex = 1;
            effectIndex < static_cast<int>(SceneTransitionEffect::Count);
            ++effectIndex)
        {
            for (auto directionIndex = 0;
                directionIndex
                    < static_cast<int>(SceneTransitionDirection::Count);
                ++directionIndex)
            {
                for (const bool accent : { false, true })
                {
                    auto settings = MakeSceneTransition(
                        static_cast<SceneTransitionEffect>(effectIndex));
                    settings.direction =
                        static_cast<SceneTransitionDirection>(
                            directionIndex);
                    settings.softness = accent ? 0.05f : 0.0f;
                    settings.accentColor.w = accent ? 1.0f : 0.0f;
                    const std::string label =
                        std::string(SceneTransitionEffectName(
                            settings.effect))
                        + "/"
                        + std::string(SceneTransitionDirectionName(
                            settings.direction))
                        + (accent ? "/accent" : "");

                    Require(
                        Build(settings, 0.0f).empty(),
                        "Zero coverage must draw nothing: " + label);
                    const auto full = Build(settings, 1.0f);
                    Require(
                        full.size() == 1
                            && NearlyEqual(full.front().width, ScreenWidth)
                            && NearlyEqual(
                                full.front().height,
                                ScreenHeight),
                        "Full coverage must be one screen rectangle: "
                            + label);

                    for (const bool revealing : { false, true })
                    {
                        float previous = 0.0f;
                        for (int step{ 1 }; step < 20; ++step)
                        {
                            const float coverage =
                                static_cast<float>(step) / 20.0f;
                            const auto quads =
                                Build(settings, coverage, revealing);
                            Require(
                                quads.size() < 5000,
                                "Too many quads: " + label);
                            const float fraction =
                                CoveredFraction(quads);
                            Require(
                                fraction + 0.03f >= previous,
                                "Covered area must grow with coverage: "
                                    + label
                                    + " at "
                                    + std::to_string(coverage)
                                    + " ("
                                    + std::to_string(fraction)
                                    + " < "
                                    + std::to_string(previous)
                                    + ")");
                            previous = std::max(previous, fraction);
                        }
                    }
                    Require(
                        CoveredFraction(Build(settings, 0.02f)) < 0.12f,
                        "Almost no coverage must leave the screen visible: "
                            + label);
                    Require(
                        CoveredFraction(Build(settings, 0.98f)) > 0.9f,
                        "Almost full coverage must hide the screen: "
                            + label);
                }
            }
        }

        // Noneは覆いを描きません。
        Require(
            Build(SceneTransitionSettings{}, 0.5f).empty(),
            "None must not draw a cover.");

        // 左から右へのWipeは、半分の時点で左半分だけを覆います。
        auto wipe = MakeSceneTransition(SceneTransitionEffect::Wipe);
        auto quads = Build(wipe, 0.5f);
        Require(
            AlphaAt(quads, ScreenWidth * 0.25f, ScreenHeight * 0.5f)
                    > 0.99f
                && AlphaAt(
                       quads,
                       ScreenWidth * 0.75f,
                       ScreenHeight * 0.5f)
                    < 0.01f,
            "A left-to-right wipe must cover the left half first.");
        // 開くときは、通り抜ける設定なら覆いが右側へ抜けていきます。
        quads = Build(wipe, 0.5f, true);
        Require(
            AlphaAt(quads, ScreenWidth * 0.75f, ScreenHeight * 0.5f)
                    > 0.99f
                && AlphaAt(
                       quads,
                       ScreenWidth * 0.25f,
                       ScreenHeight * 0.5f)
                    < 0.01f,
            "A pass-through wipe must leave towards the right.");
        wipe.passThrough = false;
        quads = Build(wipe, 0.5f, true);
        Require(
            AlphaAt(quads, ScreenWidth * 0.25f, ScreenHeight * 0.5f)
                > 0.99f,
            "A rewinding wipe must retreat to the left.");

        // 差し色は覆いの先端に帯として現れます。
        wipe = MakeSceneTransition(SceneTransitionEffect::Wipe);
        wipe.accentColor = { 1.0f, 0.2f, 0.4f, 1.0f };
        quads = Build(wipe, 0.5f);
        Require(
            std::any_of(
                quads.begin(),
                quads.end(),
                [](const SceneTransitionQuad& quad)
                {
                    return NearlyEqual(quad.color.y, 0.2f)
                        && NearlyEqual(quad.color.z, 0.4f);
                }),
            "The accent color must be drawn at the wipe edge.");

        // 斜めのWipeは回転した帯で描きます。
        wipe.direction = SceneTransitionDirection::TopLeftToBottomRight;
        quads = Build(wipe, 0.5f);
        Require(
            std::any_of(
                quads.begin(),
                quads.end(),
                [](const SceneTransitionQuad& quad)
                {
                    return quad.rotation != 0.0f;
                })
                && AlphaAt(quads, 10.0f, 10.0f) > 0.99f
                && AlphaAt(
                       quads,
                       ScreenWidth - 10.0f,
                       ScreenHeight - 10.0f)
                    < 0.01f,
            "A diagonal wipe must start from the top-left corner.");

        // Irisは中心を残して周りから閉じます。
        auto iris = MakeSceneTransition(SceneTransitionEffect::Iris);
        quads = Build(iris, 0.5f);
        Require(
            AlphaAt(quads, ScreenWidth * 0.5f, ScreenHeight * 0.5f)
                    < 0.01f
                && AlphaAt(quads, 5.0f, 5.0f) > 0.99f
                && std::any_of(
                    quads.begin(),
                    quads.end(),
                    [](const SceneTransitionQuad& quad)
                    {
                        return quad.shape
                            == SceneTransitionShape::InverseCircle;
                    }),
            "An iris must close around its focus.");
        // 中心を左上へ寄せると、そこが最後まで見えます。
        iris.focus = { 0.1f, 0.2f };
        quads = Build(iris, 0.9f);
        Require(
            AlphaAt(quads, ScreenWidth * 0.1f, ScreenHeight * 0.2f)
                    < 0.01f
                && AlphaAt(
                       quads,
                       ScreenWidth * 0.5f,
                       ScreenHeight * 0.5f)
                    > 0.99f,
            "An iris must follow its focus point.");

        auto diamond = MakeSceneTransition(SceneTransitionEffect::Diamond);
        quads = Build(diamond, 0.5f);
        Require(
            AlphaAt(quads, ScreenWidth * 0.5f, ScreenHeight * 0.5f)
                    < 0.01f
                && AlphaAt(quads, 5.0f, 5.0f) > 0.99f,
            "A diamond iris must close around its focus.");

        // Dotsは丸いテクスチャを使います。
        const auto dots = Build(
            MakeSceneTransition(SceneTransitionEffect::Dots),
            0.5f);
        Require(
            !dots.empty()
                && std::all_of(
                    dots.begin(),
                    dots.end(),
                    [](const SceneTransitionQuad& quad)
                    {
                        return quad.shape
                            == SceneTransitionShape::Circle;
                    }),
            "Dots must be drawn with the circle texture.");

        // 分割数の上限でも描画枚数は抑えます。
        auto tiles = MakeSceneTransition(SceneTransitionEffect::Tiles);
        tiles.divisions = 1000;
        Require(
            Build(tiles, 0.5f).size() <= 64u * 36u * 2u,
            "Tile count must be clamped.");

        // Fadeは全面を半透明で覆います。
        const auto fade = Build(
            MakeSceneTransition(SceneTransitionEffect::Fade),
            0.25f);
        Require(
            fade.size() == 1 && NearlyEqual(fade.front().color.w, 0.25f),
            "A fade must be one translucent rectangle.");

        // Shaderは描けないときの代わりとしてFadeの矩形を返します。
        const auto shaderFallback = Build(
            MakeSceneTransition(SceneTransitionEffect::Shader),
            0.4f);
        Require(
            shaderFallback.size() == 1
                && NearlyEqual(shaderFallback.front().color.w, 0.4f),
            "The shader effect must fall back to a fade rectangle.");

        // 大きさのない画面には何も描きません。
        std::vector<SceneTransitionQuad> empty;
        BuildSceneTransitionQuads(
            MakeSceneTransition(SceneTransitionEffect::Wipe),
            0.5f,
            false,
            0.0f,
            720.0f,
            empty);
        Require(empty.empty(), "A zero-sized canvas must draw nothing.");
    }

    void TestLoadingScreenJson()
    {
        using namespace LamaPon;
        SceneLoadingScreenSettings settings;
        settings.enabled = false;
        settings.message = "海底都市へ移動中...";
        settings.hint = "ヒント: Shiftで走れます";
        settings.backgroundTexture = u8"textures/読み込み背景.png";
        settings.showSpinner = true;
        settings.showPercentage = false;
        settings.smoothProgress = false;
        settings.fadeDuration = 0.5f;
        settings.barFillColor = { 0.1f, 0.7f, 0.9f, 1.0f };
        const auto loaded = SceneLoadingScreenFromJson(
            nlohmann::json::parse(
                SceneLoadingScreenToJson(settings).dump()));
        Require(
            !loaded.enabled
                && loaded.message == settings.message
                && loaded.hint == settings.hint
                && loaded.backgroundTexture == settings.backgroundTexture
                && loaded.showSpinner
                && !loaded.showPercentage
                && !loaded.smoothProgress
                && NearlyEqual(loaded.fadeDuration, 0.5f)
                && NearlyEqual(loaded.barFillColor.y, 0.7f),
            "Loading screen settings must round-trip through JSON.");

        // 古い形式（追加項目が無い）では、従来の見た目のままにします。
        const SceneLoadingScreenSettings defaults;
        const auto legacy = SceneLoadingScreenFromJson(
            nlohmann::json{ { "message", "Loading" } });
        Require(
            legacy.enabled
                && legacy.message == "Loading"
                && legacy.hint.empty()
                && legacy.backgroundTexture.empty()
                && !legacy.showSpinner
                && legacy.showPercentage
                && NearlyEqual(
                    legacy.backgroundColor.w,
                    defaults.backgroundColor.w),
            "Missing loading screen values must keep the classic look.");
        Require(
            !defaults.showSpinner
                && defaults.hint.empty()
                && defaults.backgroundTexture.empty(),
            "New loading screen features must be opt-in.");
    }

    void TestShaderFrame()
    {
        using namespace LamaPon;
        auto settings = MakeSceneTransition(
            SceneTransitionEffect::Shader,
            0.5f,
            { 0.2f, 0.3f, 0.4f, 1.0f });
        settings.shaderPattern = SceneTransitionShaderPattern::RuleImage;
        settings.direction = SceneTransitionDirection::TopToBottom;
        settings.focus = { 0.25f, 0.5f };
        settings.divisions = 12;
        settings.stagger = 0.3f;
        settings.accentColor = { 1.0f, 0.8f, 0.2f, 1.0f };
        settings.accentWidth = 0.05f;

        auto frame = BuildSceneTransitionShaderFrame(
            settings,
            0.6f,
            false,
            ScreenWidth,
            ScreenHeight,
            true);
        Require(
            frame.pattern == SceneTransitionShaderPattern::RuleImage
                && NearlyEqual(frame.parameters[0].x, 0.6f)
                && NearlyEqual(frame.parameters[0].y, 0.02f)
                && NearlyEqual(frame.parameters[0].z, 0.05f)
                && NearlyEqual(frame.parameters[0].w, 0.0f)
                && NearlyEqual(frame.parameters[1].x, 0.0f)
                && NearlyEqual(frame.parameters[1].y, 0.3f)
                && NearlyEqual(frame.parameters[1].z, 12.0f)
                && NearlyEqual(frame.parameters[2].y, 0.8f)
                && NearlyEqual(frame.parameters[3].x, ScreenWidth)
                && NearlyEqual(frame.parameters[3].y, ScreenHeight)
                && NearlyEqual(frame.parameters[3].z, ScreenWidth * 0.25f)
                && NearlyEqual(frame.parameters[3].w, ScreenHeight * 0.5f)
                && NearlyEqual(frame.parameters[4].x, 0.0f)
                && NearlyEqual(frame.parameters[4].y, 1.0f)
                && NearlyEqual(frame.tint.z, 0.4f),
            "Shader parameters must follow the documented layout.");
        for (std::size_t index = 5; index < frame.parameters.size(); ++index)
        {
            Require(
                NearlyEqual(frame.parameters[index].x, 0.0f)
                    && NearlyEqual(frame.parameters[index].w, 0.0f),
                "Engine-owned shader parameters must be left empty.");
        }

        frame = BuildSceneTransitionShaderFrame(
            settings,
            0.6f,
            true,
            ScreenWidth,
            ScreenHeight,
            false);
        Require(
            frame.pattern == SceneTransitionShaderPattern::Dissolve
                && NearlyEqual(
                    frame.parameters[0].w,
                    static_cast<float>(
                        SceneTransitionShaderPattern::Dissolve))
                && NearlyEqual(frame.parameters[1].x, 1.0f),
            "A rule image without a texture must dissolve, and pass-through reveals must invert the order.");

        settings.passThrough = false;
        settings.accentColor.w = 0.0f;
        frame = BuildSceneTransitionShaderFrame(
            settings,
            2.0f,
            true,
            ScreenWidth,
            ScreenHeight,
            true);
        Require(
            NearlyEqual(frame.parameters[1].x, 0.0f)
                && NearlyEqual(frame.parameters[0].z, 0.0f)
                && NearlyEqual(frame.parameters[0].x, 1.0f),
            "Rewinding reveals, disabled accents and coverage clamping must be encoded.");
    }
}

int main()
{
    TestEasing();
    TestNames();
    TestJson();
    TestTimeline();
    TestGeometry();
    TestShaderFrame();
    TestLoadingScreenJson();

    if (g_failures != 0)
    {
        std::cerr << g_failures
            << " scene transition assertion(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Scene transition tests passed.\n";
    return EXIT_SUCCESS;
}
