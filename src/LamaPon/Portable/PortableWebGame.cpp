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

    EM_JS(int, PortableAutopilotEnabled, (), {
        try {
            return new URLSearchParams(location.search).get("autopilot") === "1";
        } catch (error) {
            return 0;
        }
    });

    class PortableGame final : public IWebApplication
    {
    public:
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
                auto& state = m_scene->Scenes().State();
                state.SetString("test_command", "autopilot_on");
                state.SetInteger("test_command_seq", 1);
            }
            m_scene->StartScripts();
            return true;
        }

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

        void FixedUpdate(WebRuntime&, float deltaTime) override
        {
            m_scene->FixedUpdate(deltaTime);
        }

        void Update(WebRuntime&, const WebFrame& frame) override
        {
            m_scene->SetPhysicsInterpolationAlpha(frame.fixedStepAlpha);
            // Script側の簡易PhysicsはUpdate()で処理されることが多いため、
            // 遅いブラウザフレームを60Hz単位に分割します。明示的な積分でも
            // Windows版と同じJump高度やObstacle Timingを保ちます。
            constexpr float MaximumSimulationStep = 1.0f / 60.0f;
            float remaining = std::max(frame.deltaTime, 0.0f);
            bool firstStep = true;
            if (remaining <= 0.0f)
            {
                m_scene->Graphics().Input().SetEdgeEventsEnabled(true);
                m_scene->Update(0.0f);
            }
            while (remaining > 0.0f)
            {
                const float step = std::min(
                    remaining, MaximumSimulationStep);
                m_scene->Graphics().Input().SetEdgeEventsEnabled(firstStep);
                m_scene->Update(step);
                remaining -= step;
                firstStep = false;
            }
            m_scene->Graphics().Input().SetEdgeEventsEnabled(true);
            m_scene->Render();
        }

    private:
        Renderer3D m_renderer;
        WebAudioRuntime m_audio;
        std::unique_ptr<LamaPon::Scene> m_scene;
    };
}

int main()
{
    static WebRuntime runtime;
    static PortableGame game;
    return RunWebApplication(game, runtime);
}
