// シーン遷移の純粋ロジック（イージング、時間軸、保存形式）を検査します。
// エンジンは覆いの絵を描かないため、覆い具合（Coverage）の変化と
// 各段階の出来事だけを確かめます。

#include "LamaPon/Scene/SceneTransition.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

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

    void TestJson()
    {
        using namespace LamaPon;
        SceneTransitionSettings settings;
        settings.easing = SceneTransitionEasing::EaseOutQuad;
        settings.coverDuration = 0.75f;
        settings.holdDuration = 0.25f;
        settings.revealDuration = 1.5f;
        settings.showLoadingScreen = false;
        settings.blockInput = false;
        settings.fadeMusic = false;

        const auto json = SceneTransitionToJson(settings);
        Require(
            json.at("easing") == "easeOutQuad",
            "The easing must be saved by name.");
        Require(
            !json.contains("effect") && !json.contains("color"),
            "The engine must not save how a transition looks.");
        const auto loaded = SceneTransitionFromJson(
            nlohmann::json::parse(json.dump()));
        Require(
            loaded.easing == settings.easing
                && NearlyEqual(loaded.coverDuration, 0.75f)
                && NearlyEqual(loaded.holdDuration, 0.25f)
                && NearlyEqual(loaded.revealDuration, 1.5f)
                && !loaded.showLoadingScreen
                && !loaded.blockInput
                && !loaded.fadeMusic,
            "Transition settings must round-trip through JSON.");

        // 演出の見た目などのキーは無視し、同じJSONへまとめて保存できます。
        SceneTransitionSettings fallback;
        fallback.coverDuration = 2.0f;
        const auto partial = SceneTransitionFromJson(
            nlohmann::json{
                { "effect", "iris" },
                { "color", nlohmann::json::array({ 1.0, 0.0, 0.0, 1.0 }) },
                { "revealDuration", -4.0 },
                { "holdDuration", 999.0 },
                { "easing", "unknown-easing" },
                { "fadeMusic", "yes" },
            },
            fallback);
        Require(
            NearlyEqual(partial.coverDuration, 2.0f)
                && NearlyEqual(partial.revealDuration, 0.0f)
                && NearlyEqual(partial.holdDuration, 30.0f)
                && partial.easing == fallback.easing
                && partial.fadeMusic == fallback.fadeMusic,
            "Missing or invalid JSON values must use the fallback or be clamped.");
        Require(
            NearlyEqual(
                SceneTransitionFromJson(nlohmann::json(42), fallback)
                    .coverDuration,
                2.0f),
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

        const auto made = MakeSceneTransition(0.8f);
        Require(
            NearlyEqual(made.coverDuration, 0.8f)
                && NearlyEqual(made.revealDuration, 0.8f)
                && NearlyEqual(made.holdDuration, 0.1f)
                && made.easing == SceneTransitionEasing::EaseInOutCubic
                && !IsInstantSceneTransition(made),
            "MakeSceneTransition must fill the durations.");

        // 既定値は従来どおりすぐ切り替える遷移です。
        Require(
            IsInstantSceneTransition(SceneTransitionSettings{})
                && IsInstantSceneTransition(broken) == false
                && IsInstantSceneTransition(sanitized) == false,
            "Only transitions without any duration are instant.");
        SceneTransitionSettings holdOnly;
        holdOnly.holdDuration = 0.3f;
        Require(
            !IsInstantSceneTransition(holdOnly),
            "A hold duration alone must keep the screen covered.");
    }

    void TestTimeline()
    {
        using namespace LamaPon;
        auto settings = MakeSceneTransition(
            0.5f,
            0.2f,
            SceneTransitionEasing::Linear);

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
            "A timed transition must stay covered for the first ready frame.");
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

        // 既定の（時間がすべて0の）遷移は、時間を掛けずに段階だけ進みます。
        SceneTransitionTimeline instant;
        instant.Start(SceneTransitionSettings{});
        events = instant.Advance(0.0f, false);
        Require(
            events.covered && instant.IsFullyCovered(),
            "An instant transition must cover immediately.");
        events = instant.Advance(0.0f, true);
        Require(
            events.revealStarted,
            "An instant transition must reveal on the first ready frame.");
        events = instant.Advance(0.0f, true);
        Require(
            events.finished && !instant.IsActive(),
            "An instant transition must finish immediately.");

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

}

int main()
{
    TestEasing();
    TestJson();
    TestTimeline();
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
