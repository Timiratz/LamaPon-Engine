#pragma once

#include "LamaPon/Web/WebInput.h"

#include <cstdint>

namespace LamaPon::Web
{
    struct WebApplicationConfig final
    {
        const char* name{ "LamaPon Game" };
        const char* canvasSelector{ "#canvas" };
        float fixedDeltaTime{ 1.0f / 60.0f };
        float maximumFrameDeltaTime{ 0.25f };
        std::uint32_t maximumCatchUpSteps{ 15 };
        const char* initializationError{
            "The Web game could not be initialized. See the browser console "
            "and web-compatibility-report.json for details."
        };
    };

    struct WebFrame final
    {
        float deltaTime{};
        float fixedStepAlpha{};
        double elapsedSeconds{};
        std::uint64_t index{};
    };

    // すべてのWeb Gameが利用するBrowser Platform Serviceです。
    // 任意のRenderer、Audio、PhysicsはGame Target側で所有し、要求しない
    // ProjectのWasmにはLinkされない構造を保ちます。
    class WebRuntime final
    {
    public:
        [[nodiscard]] WebInput& Input() noexcept { return m_input; }
        [[nodiscard]] const WebInput& Input() const noexcept { return m_input; }

    private:
        friend int RunWebApplication(class IWebApplication&, WebRuntime&);

        WebInput m_input;
    };

    class IWebApplication
    {
    public:
        virtual ~IWebApplication() = default;

        [[nodiscard]] virtual WebApplicationConfig Configuration() const
            noexcept = 0;
        [[nodiscard]] virtual bool Initialize(WebRuntime& runtime) = 0;
        virtual void BeginFrame(WebRuntime&, const WebFrame&) {}
        virtual void FixedUpdate(WebRuntime& runtime, float deltaTime) = 0;
        virtual void Update(WebRuntime& runtime, const WebFrame& frame) = 0;
    };

    // Browser Main Loopを登録します。Emscriptenで正常実行中は戻らないため、
    // Game ObjectとRuntimeにはstatic相当のProcess Lifetimeが必要です。
    [[nodiscard]] int RunWebApplication(
        IWebApplication& application,
        WebRuntime& runtime);
}
