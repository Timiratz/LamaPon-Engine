#pragma once

#include "LamaPon/Web/WebInput.h"

#include <cstdint>

namespace LamaPon::Web
{
    // 文字列はゲームループ終了まで有効な領域を指定する。
    struct WebApplicationConfig final
    {
        // 表示するゲーム名
        const char* name{ "LamaPon Game" };
        // 入力先CanvasのCSS指定
        const char* canvasSelector{ "#canvas" };
        // 固定更新の間隔（秒）
        float fixedDeltaTime{ 1.0f / 60.0f };
        // フレーム時間の上限（秒）
        float maximumFrameDeltaTime{ 0.25f };
        // 1フレームの固定更新上限
        std::uint32_t maximumCatchUpSteps{ 15 };
        // ゲーム初期化失敗時の説明
        const char* initializationError{
            "The Web game could not be initialized. See the browser console "
            "and web-compatibility-report.json for details."
        };
    };

    // BeginFrameの補間比は固定更新前のため1以上になり得る。
    struct WebFrame final
    {
        // 上限適用後の経過秒数
        float deltaTime{};
        // 固定更新の余剰時間比
        float fixedStepAlpha{};
        // 起動からの実時間（秒）
        double elapsedSeconds{};
        // 0から始まるフレーム番号
        std::uint64_t index{};
    };

    // ゲーム側が描画・音声・物理を所有し、必要なWeb機能だけをリンクする。
    class WebRuntime final
    {
    public:
        // Web入力サービスを返す。
        [[nodiscard]] WebInput& Input() noexcept { return m_input; }
        // Web入力サービスを参照する。
        [[nodiscard]] const WebInput& Input() const noexcept { return m_input; }

    private:
        // メインループ登録時の入力初期化を許可する。
        friend int RunWebApplication(class IWebApplication&, WebRuntime&);

        // ブラウザーの入力サービス
        WebInput m_input;
    };

    class IWebApplication
    {
    public:
        // Webアプリケーションを破棄する。
        virtual ~IWebApplication() = default;

        // 開始前に検証するゲーム名・Canvas・更新間隔を返す。
        [[nodiscard]] virtual WebApplicationConfig Configuration() const
            noexcept = 0;
        // ゲームを初期化し、失敗時はfalseを返す(runtime: 借用するWebサービス)。
        [[nodiscard]] virtual bool Initialize(WebRuntime& runtime) = 0;
        // 固定更新前のフレーム開始を通知する。
        virtual void BeginFrame(WebRuntime&, const WebFrame&) {}
        // 一定間隔のシミュレーションを進める(runtime: Webサービス, deltaTime: 更新間隔の秒数)。
        virtual void FixedUpdate(WebRuntime& runtime, float deltaTime) = 0;
        // 固定更新後のフレーム処理を行う(runtime: Webサービス, frame: 時間と補間情報)。
        virtual void Update(WebRuntime& runtime, const WebFrame& frame) = 0;
    };

    // applicationとruntimeはメインループ終了まで保持し、Emscripten実行中の呼出元へは戻らない。
    // 初期化後にブラウザーのメインループを登録する(application: Webゲーム, runtime: Webサービス)。
    [[nodiscard]] int RunWebApplication(
        IWebApplication& application,
        WebRuntime& runtime);
}
