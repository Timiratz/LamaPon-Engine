#include "LamaPon/Web/WebApplication.h"

#include <emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace
{
    using namespace LamaPon::Web;

    EM_JS(void, ReportWebApplicationError, (const char* message), {
        const text = UTF8ToString(message);
        const help = document.querySelector("#help");
        if (help) {
            help.textContent = text;
            help.style.color = "#ffd0d0";
        }
        if (document.body) {
            document.body.dataset.lamaponStatus = "failed";
            document.body.dataset.lamaponError = text;
        }
        console.error("LamaPon Web: " + text);
    });

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
        IWebApplication& application;
        WebRuntime& runtime;
        WebApplicationConfig config;
        double startSeconds{};
        double lastFrameSeconds{};
        float fixedAccumulator{};
        float maximumTickMilliseconds{};
        std::uint32_t measuredTickCount{};
        std::uint64_t frameIndex{};

        void Tick()
        {
            const double tickStartedMilliseconds = emscripten_get_now();
            runtime.Input().BeginFrame();

            const double nowSeconds = tickStartedMilliseconds * 0.001;
            const float frameDelta = std::clamp(
                static_cast<float>(nowSeconds - lastFrameSeconds),
                0.0f,
                config.maximumFrameDeltaTime);
            lastFrameSeconds = nowSeconds;
            fixedAccumulator = std::min(
                config.maximumFrameDeltaTime,
                fixedAccumulator + frameDelta);

            const WebFrame beginFrame{
                frameDelta,
                config.fixedDeltaTime > 0.0f
                    ? fixedAccumulator / config.fixedDeltaTime
                    : 0.0f,
                nowSeconds - startSeconds,
                frameIndex,
            };
            application.BeginFrame(runtime, beginFrame);

            std::uint32_t fixedSteps{};
            while (fixedAccumulator >= config.fixedDeltaTime
                   && fixedSteps < config.maximumCatchUpSteps)
            {
                application.FixedUpdate(runtime, config.fixedDeltaTime);
                fixedAccumulator -= config.fixedDeltaTime;
                ++fixedSteps;
            }
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

        static void Callback(void* userData)
        {
            static_cast<ApplicationLoop*>(userData)->Tick();
        }
    };

    std::unique_ptr<ApplicationLoop> ActiveLoop;
}

namespace LamaPon::Web
{
    int RunWebApplication(
        IWebApplication& application,
        WebRuntime& runtime)
    {
        const WebApplicationConfig config = application.Configuration();
        if (config.name == nullptr || config.name[0] == '\0'
            || config.canvasSelector == nullptr
            || config.canvasSelector[0] == '\0'
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
        if (!application.Initialize(runtime))
        {
            ReportWebApplicationError(config.initializationError);
            return 1;
        }

        const double nowSeconds = emscripten_get_now() * 0.001;
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
