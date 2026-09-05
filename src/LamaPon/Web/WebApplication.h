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

    // すべてのWebゲームが利用するブラウザー向けサービスです。任意の
    // 描画、音声、物理機能はゲーム側で所有し、使わない機能をWasmへ
    // リンクせずに済む構造を保ちます。
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

    // ブラウザーのメインループを登録します。Emscriptenで実行中は
    // 戻らないため、GameObjectとRuntimeはプロセス終了まで保持します。
    [[nodiscard]] int RunWebApplication(
        IWebApplication& application,
        WebRuntime& runtime);
}
