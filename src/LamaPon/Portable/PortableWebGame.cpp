#include "LamaPon/LamaPon.h"

#include "LamaPon/Web/WebApplication.h"
#include "LamaPon/Web/WebAudioRuntime.h"
#include "LamaPon/Web/WebRenderer3D.h"

#include <emscripten.h>

#include <algorithm>
#include <memory>

#ifndef LAMAPON_PORTABLE_GAME_NAME
#define LAMAPON_PORTABLE_GAME_NAME "LamaPon Portable Game"
#endif

#ifndef LAMAPON_PORTABLE_SCENE_PATH
#define LAMAPON_PORTABLE_SCENE_PATH "/assets/scenes/Main.scene.json"
#endif

#ifndef LAMAPON_WEB_AUDIO_ENABLED
#define LAMAPON_WEB_AUDIO_ENABLED 0
#endif

namespace
{
    using namespace LamaPon::Web;

    // URLのautopilot=1指定を調べる。
    EM_JS(int, PortableAutopilotEnabled, (), {
        try {
            return new URLSearchParams(location.search).get("autopilot") === "1";
        }
        // URL解析の失敗時は自動操作を無効にする(error: 解析エラー)。
        catch (error) {
            return 0;
        }
    });

    class PortableGame final : public IWebApplication
    {
    public:
        // Portable版のゲーム名・Canvas・更新間隔を返す。
        [[nodiscard]] WebApplicationConfig Configuration() const
            noexcept override
        {
            return {
                LAMAPON_PORTABLE_GAME_NAME,
                "#canvas",
                1.0f / 60.0f,
                0.25f,
                15,
                "The LamaPon scene or one of its C++ scripts could not be "
                "initialized by the portable Web runtime.",
            };
        }

        // Sceneを読みScriptを開始する(runtime: 借用するWebサービス)。
        [[nodiscard]] bool Initialize(WebRuntime& runtime) override
        {
            if (!m_renderer.Initialize("#canvas", 1280, 720))
            {
                return false;
            }
#if LAMAPON_WEB_AUDIO_ENABLED
            m_audio.Initialize();
#endif
            m_scene = std::make_unique<LamaPon::Scene>(
                m_renderer,
                m_audio,
                runtime.Input());
            if (!m_scene->Load(LAMAPON_PORTABLE_SCENE_PATH))
            {
                return false;
            }
            if (PortableAutopilotEnabled() != 0)
            {
                // 自動操作へ渡すScene状態
                auto& state = m_scene->Scenes().State();
                state.SetString("test_command", "autopilot_on");
                state.SetInteger("test_command_seq", 1);
            }
            m_scene->StartScripts();
            return true;
        }

        // 入力があればWeb音声のロック解除を要求する(runtime: Webサービス)。
        void BeginFrame(WebRuntime& runtime, const WebFrame&) override
        {
#if LAMAPON_WEB_AUDIO_ENABLED
            if (runtime.Input().WasPressed("Space")
                || runtime.Input().PointerButtonPressed(0)
                || runtime.Input().WasGamepadPressed(0)
                || runtime.Input().WasPressed("KeyW")
                || runtime.Input().WasPressed("ArrowUp")
                || runtime.Input().WasPressed("KeyS")
                || runtime.Input().WasPressed("ArrowDown")
                || runtime.Input().WasPressed("KeyA")
                || runtime.Input().WasPressed("ArrowLeft")
                || runtime.Input().WasPressed("KeyD")
                || runtime.Input().WasPressed("ArrowRight"))
            {
                m_audio.UnlockFromUserGesture();
            }
#else
            (void)runtime;
#endif
        }

        // Sceneの固定更新を進める(deltaTime: 更新間隔の秒数)。
        void FixedUpdate(WebRuntime&, float deltaTime) override
        {
            m_scene->FixedUpdate(deltaTime);
        }

        // Sceneの更新を最大1/60秒ずつ進めて描画する(frame: 時間と補間情報)。
        void Update(WebRuntime&, const WebFrame& frame) override
        {
            m_scene->SetPhysicsInterpolationAlpha(frame.fixedStepAlpha);
            // Script内の積分へ長い経過時間を渡さないよう、Updateを1/60秒以下に分割する。
            // Script更新の最大秒数
            constexpr float MaximumSimulationStep = 1.0f / 60.0f;
            // 今回まだ更新していない秒数
            float remaining = std::max(frame.deltaTime, 0.0f);
            // 今回最初の分割更新か
            bool firstStep = true;
            if (remaining <= 0.0f)
            {
                m_scene->Graphics().Input().SetEdgeEventsEnabled(true);
                m_scene->Update(0.0f);
            }
            while (remaining > 0.0f)
            {
                // この分割更新の秒数
                const float step = std::min(
                    remaining, MaximumSimulationStep);
                // 押下・解放イベントは同一ブラウザーフレームの最初の分割更新だけへ渡す。
                m_scene->Graphics().Input().SetEdgeEventsEnabled(firstStep);
                m_scene->Update(step);
                remaining -= step;
                firstStep = false;
            }
            m_scene->Graphics().Input().SetEdgeEventsEnabled(true);
            m_scene->Render();
        }

    private:
        // Scene描画用のWebレンダラー
        Renderer3D m_renderer;
        // Sceneが借用する音声サービス
        WebAudioRuntime m_audio;
        // 音声と描画より先に破棄するScene
        std::unique_ptr<LamaPon::Scene> m_scene;
    };
}

// Portable版のWebメインループを登録する。
int main()
{
    // 終了まで保持するWebサービス
    static WebRuntime runtime;
    // 終了まで保持するWebゲーム
    static PortableGame game;
    return RunWebApplication(game, runtime);
}
