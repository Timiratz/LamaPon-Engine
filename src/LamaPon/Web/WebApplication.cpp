#include "LamaPon/Web/WebApplication.h"

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <exception>

namespace
{
    using namespace LamaPon::Web;

    // RAFが停止する非表示期間も記録し、復帰フレームの時間差を捨てる。
    EM_JS(void, InitializeVisibilityState, (), {
        if (document.__lamaponVisibility) return;
        document.__lamaponVisibility = { changed: false };
        document.addEventListener('visibilitychange', () => {
            document.__lamaponVisibility.changed = true;
        });
    });
    EM_JS(int, ReadVisibilityState, (), {
        if (document.hidden || document.__lamaponContextLost) return 2;
        const state = document.__lamaponVisibility;
        if (state && state.changed) {
            state.changed = false;
            return 1;
        }
        return 0;
    });

    // 初期化失敗を画面とコンソールへ表示する(message: UTF-8の説明)。
    EM_JS(void, ReportWebApplicationError, (const char* message), {
        // 表示するエラー文字列
        const text = UTF8ToString(message);
        // エラーを表示するDOM要素
        const help = document.querySelector("#help");
        if (help) {
            help.textContent = text;
            help.style.color = "#ffd0d0";
        }
        const reload = document.getElementById("reload");
        if (reload) reload.hidden = false;
        if (document.body) {
            document.body.dataset.lamaponStatus = "failed";
            document.body.dataset.lamaponError = text;
        }
        console.error("LamaPon Web: " + text);
    });

    // 状態と計測値をDOMへ公開する(name: ゲーム名, status: 実行状態, currentMilliseconds: 今回の処理ms, maximumMilliseconds: 最大処理ms, fixedDeltaTime: 固定更新秒数, frameIndex: フレーム番号)。
    EM_JS(void, PublishWebApplicationState,
          (const char* name, const char* status, float currentMilliseconds,
           float maximumMilliseconds, float fixedDeltaTime,
           double frameIndex), {
        if (!document.body) return;
        document.body.dataset.lamaponGame = UTF8ToString(name);
        document.body.dataset.lamaponStatus = UTF8ToString(status);
        document.body.dataset.lamaponTickMilliseconds =
            currentMilliseconds.toFixed(2);
        document.body.dataset.lamaponTickMaximum =
            maximumMilliseconds.toFixed(2);
        document.body.dataset.lamaponFixedDelta = fixedDeltaTime.toFixed(7);
        document.body.dataset.lamaponFrame = String(Math.floor(frameIndex));
    });

    struct ApplicationLoop final
    {
        // ループ中に借用するWebゲーム
        IWebApplication& application;
        // 借用するWebサービス
        WebRuntime& runtime;
        // 起動時の更新・Canvas設定
        WebApplicationConfig config;
        // ループ開始の実時刻（秒）
        double startSeconds{};
        // 前フレームの実時刻（秒）
        double lastFrameSeconds{};
        // 固定更新に未消費の秒数
        float fixedAccumulator{};
        // 起動30フレーム後の最大処理ms
        float maximumTickMilliseconds{};
        // 計測済みのフレーム数
        std::uint32_t measuredTickCount{};
        // 次のフレーム番号
        std::uint64_t frameIndex{};

        // 入力・固定更新・フレーム更新の順に処理し、計測値を公開する。
        void Tick()
        {
            // フレーム処理開始時刻（ms）
            const double tickStartedMilliseconds = emscripten_get_now();
            const int visibility = ReadVisibilityState();
            if (visibility != 0)
            {
                lastFrameSeconds = tickStartedMilliseconds * 0.001;
                fixedAccumulator = 0.0f;
                runtime.Input().Reset();
                if (visibility == 2) return;
            }
            runtime.Input().BeginFrame();

            // 現在の実時刻（秒）
            const double nowSeconds = tickStartedMilliseconds * 0.001;
            // 上限適用後の経過秒数
            const float frameDelta = std::clamp(
                static_cast<float>(nowSeconds - lastFrameSeconds),
                0.0f,
                config.maximumFrameDeltaTime);
            lastFrameSeconds = nowSeconds;
            fixedAccumulator = std::min(
                config.maximumFrameDeltaTime,
                fixedAccumulator + frameDelta);

            // 固定更新前のフレーム情報
            const WebFrame beginFrame{
                frameDelta,
                config.fixedDeltaTime > 0.0f
                    ? fixedAccumulator / config.fixedDeltaTime
                    : 0.0f,
                nowSeconds - startSeconds,
                frameIndex,
            };
            application.BeginFrame(runtime, beginFrame);

            // 今回実行した固定更新数
            std::uint32_t fixedSteps{};
            while (fixedAccumulator >= config.fixedDeltaTime
                   && fixedSteps < config.maximumCatchUpSteps)
            {
                application.FixedUpdate(runtime, config.fixedDeltaTime);
                fixedAccumulator -= config.fixedDeltaTime;
                ++fixedSteps;
            }
            // 固定更新の上限に達したら過去の未処理ステップを捨て、余剰時間だけ残す。
            if (fixedSteps == config.maximumCatchUpSteps
                && fixedAccumulator >= config.fixedDeltaTime)
            {
                fixedAccumulator = std::fmod(
                    fixedAccumulator,
                    config.fixedDeltaTime);
            }

            application.Update(
                runtime,
                {
                    frameDelta,
                    config.fixedDeltaTime > 0.0f
                        ? fixedAccumulator / config.fixedDeltaTime
                        : 0.0f,
                    nowSeconds - startSeconds,
                    frameIndex,
                });
            runtime.Input().EndFrame();

            // 今回のフレーム処理時間（ms）
            const float tickMilliseconds = static_cast<float>(
                emscripten_get_now() - tickStartedMilliseconds);
            ++measuredTickCount;
            if (measuredTickCount > 30)
            {
                maximumTickMilliseconds = std::max(
                    maximumTickMilliseconds,
                    tickMilliseconds);
            }
            PublishWebApplicationState(
                config.name,
                "running",
                tickMilliseconds,
                maximumTickMilliseconds,
                config.fixedDeltaTime,
                static_cast<double>(frameIndex));
            ++frameIndex;
        }

        // 登録したゲームループを進める(userData: 借用するループ状態)。
        static void Callback(void* userData)
        {
            auto& loop = *static_cast<ApplicationLoop*>(userData);
            try { loop.Tick(); }
            catch (const std::exception& error)
            {
                emscripten_cancel_main_loop();
                loop.runtime.Input().Reset();
                ReportWebApplicationError(error.what());
            }
            catch (...)
            {
                emscripten_cancel_main_loop();
                loop.runtime.Input().Reset();
                ReportWebApplicationError("The Web game stopped after an unexpected error.");
            }
        }
    };

    // メインループの状態所有者
    std::unique_ptr<ApplicationLoop> ActiveLoop;
}

namespace LamaPon::Web
{
    int RunWebApplication(
        IWebApplication& application,
        WebRuntime& runtime)
    {
        // ゲームの起動設定
        const WebApplicationConfig config = application.Configuration();
        if (config.name == nullptr || config.name[0] == '\0'
            || config.canvasSelector == nullptr
            || config.canvasSelector[0] == '\0'
            || !std::isfinite(config.fixedDeltaTime)
            || !std::isfinite(config.maximumFrameDeltaTime)
            || config.fixedDeltaTime <= 0.0f
            || config.maximumFrameDeltaTime < config.fixedDeltaTime
            || config.maximumCatchUpSteps == 0)
        {
            ReportWebApplicationError(
                "The Web application configuration is invalid.");
            return 1;
        }
        if (!runtime.m_input.Initialize(config.canvasSelector))
        {
            ReportWebApplicationError(
                "The browser input backend could not be initialized.");
            return 1;
        }
        bool initialized{};
        try { initialized = application.Initialize(runtime); }
        catch (const std::exception& error)
        {
            ReportWebApplicationError(error.what());
            return 1;
        }
        catch (...)
        {
            ReportWebApplicationError(config.initializationError);
            return 1;
        }
        if (!initialized)
        {
            ReportWebApplicationError(config.initializationError);
            return 1;
        }

        // 現在の実時刻（秒）
        const double nowSeconds = emscripten_get_now() * 0.001;
        InitializeVisibilityState();
        ActiveLoop = std::make_unique<ApplicationLoop>(ApplicationLoop{
            application,
            runtime,
            config,
            nowSeconds,
            nowSeconds,
        });
        PublishWebApplicationState(
            config.name,
            "starting",
            0.0f,
            0.0f,
            config.fixedDeltaTime,
            0);
        emscripten_set_main_loop_arg(
            &ApplicationLoop::Callback,
            ActiveLoop.get(),
            0,
            EM_TRUE);
        return 0;
    }
}
