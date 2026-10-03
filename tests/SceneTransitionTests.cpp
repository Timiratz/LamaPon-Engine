#include "LamaPon/Scene/SceneTransition.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

// シーン遷移の進捗・イベント・保存形式を検証します。
namespace
{
    // テスト失敗数
    int g_failures = 0;

    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を検査失敗として記録する。
    void Require(const bool condition, const std::string& message)
    {
        // 検査条件の不成立を記録する。
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    // NearlyEqual(left: 実値, right: 期待値, tolerance: 許容誤差): 浮動小数値を誤差範囲で比較する。
    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right,
        const float tolerance = 1.0e-4f) noexcept
    {
        return std::abs(left - right) <= tolerance;
    }

    // TestEasing(): 全イージングの端点、単調性、名前変換を検証する。
    void TestEasing()
    {
        using LamaPon::SceneTransitionEasing;
        // 全イージング種別を確認する。
        for (auto index = 0;
            index < static_cast<int>(SceneTransitionEasing::Count);
            ++index)
        {
            // 検査対象のイージング種別
            const auto easing =
                static_cast<SceneTransitionEasing>(index);
            // 保存用イージング名
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
            // 前回サンプル値
            float previous = 0.0f;
            // 入力範囲を100段階で調べる。
            for (int step{}; step <= 100; ++step)
            {
                // 現在入力の評価値
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

    // TestJson(): シーン遷移設定の保存、読込、補正を検証する。
    void TestJson()
    {
        using namespace LamaPon;
        // JSON往復の入力設定
        SceneTransitionSettings settings;
        settings.easing = SceneTransitionEasing::EaseOutQuad;
        settings.coverDuration = 0.75f;
        settings.holdDuration = 0.25f;
        settings.revealDuration = 1.5f;
        settings.showLoadingScreen = false;
        settings.blockInput = false;
        settings.fadeMusic = false;

        // 設定をシリアライズしたJSON
        const auto json = SceneTransitionToJson(settings);
        Require(
            json.at("easing") == "easeOutQuad",
            "The easing must be saved by name.");
        Require(
            !json.contains("effect") && !json.contains("color"),
            "The engine must not save how a transition looks.");
        // JSONから復元した設定
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
        // 一部欠落・不正値を含む復元設定
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

        // 不正な時間値を含む設定
        SceneTransitionSettings broken;
        broken.coverDuration =
            std::numeric_limits<float>::quiet_NaN();
        broken.revealDuration = -5.0f;
        broken.holdDuration = 1000.0f;
        // 不正値を補正した設定
        const auto sanitized = SanitizeSceneTransition(broken);
        Require(
            NearlyEqual(
                sanitized.coverDuration,
                SceneTransitionSettings{}.coverDuration)
                && NearlyEqual(sanitized.revealDuration, 0.0f)
                && NearlyEqual(sanitized.holdDuration, 30.0f),
            "Non-finite or out-of-range durations must be sanitized.");

        // 指定時間から生成した遷移設定
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
        // 保持時間だけを持つ遷移設定
        SceneTransitionSettings holdOnly;
        holdOnly.holdDuration = 0.3f;
        Require(
            !IsInstantSceneTransition(holdOnly),
            "A hold duration alone must keep the screen covered.");
    }

    // TestTimeline(): 遷移の時間、準備待ち、再開動作を検証する。
    void TestTimeline()
    {
        using namespace LamaPon;
        // 時間付き遷移設定
        auto settings = MakeSceneTransition(
            0.5f,
            0.2f,
            SceneTransitionEasing::Linear);

        // 遷移進行状態
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

        // 最初のカバー進行イベント
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

        // frame: シーン準備待ち中にカバーを保つ描画フレーム番号。
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

        // 直前のカバー率
        float previous = timeline.Coverage();
        // 解除完了状態
        bool finished = false;
        // frame: 解除完了まで進める描画フレーム番号。
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

        // 保持時間がシーン準備完了とは別に適用される。
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

        // 現在のカバー率から解除を続ける。
        timeline.Reset();
        timeline.Start(settings);
        static_cast<void>(timeline.Advance(0.15f, false));
        // 途中解除を開始した時点のカバー率
        const float partial = timeline.Coverage();
        timeline.Reveal();
        Require(
            timeline.Phase() == SceneTransitionPhase::Revealing
                && NearlyEqual(timeline.Coverage(), partial, 1.0e-3f),
            "Reveal must continue from the current coverage.");

        // 解除中に再開始してもカバー率を維持する。
        auto cubic = settings;
        cubic.easing = SceneTransitionEasing::EaseInOutCubic;
        static_cast<void>(timeline.Advance(0.05f, true));
        // 再開始直前のカバー率
        const float beforeRestart = timeline.Coverage();
        timeline.Start(cubic);
        Require(
            timeline.Phase() == SceneTransitionPhase::Covering
                && NearlyEqual(
                    timeline.Coverage(),
                    beforeRestart,
                    1.0e-3f),
            "Restarting while revealing must continue from the current coverage.");

        // 時間ゼロの遷移を即時に進める。
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

        // 非有限時間で状態が進まないことを確認する。
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

    // TestLoadingScreenJson(): 読込画面設定のJSON往復と既定値を検証する。
    void TestLoadingScreenJson()
    {
        using namespace LamaPon;
        // JSON往復の入力設定
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
        // JSONから復元した読込画面設定
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

        // 追加項目のない古いJSONから既定表示を復元する。
        // 読込画面設定の従来既定値
        const SceneLoadingScreenSettings defaults;
        // 古い形式から復元した設定
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

// main(): シーン遷移の全テストを実行して結果を返す。
int main()
{
    TestEasing();
    TestJson();
    TestTimeline();
    TestLoadingScreenJson();

    // 失敗があれば非成功の終了コードを返す。
    if (g_failures != 0)
    {
        std::cerr << g_failures
            << " scene transition assertion(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Scene transition tests passed.\n";
    return EXIT_SUCCESS;
}
