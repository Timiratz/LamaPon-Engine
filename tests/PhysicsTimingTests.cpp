#include "LamaPon/LamaPon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace
{
    constexpr float Speed = 6.0f;

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    float RenderedX(const LamaPon::GameObject& object, const LamaPon::Scene& scene)
    {
        DirectX::XMFLOAT4X4 matrix{};
        DirectX::XMStoreFloat4x4(&matrix,
            object.InterpolatedWorldMatrix(scene.PhysicsInterpolationAlpha()));
        return matrix._41;
    }

    struct RestorePhysicsSettings final
    {
        LamaPon::PhysicsSettings original = LamaPon::ActivePhysicsSettings();
        ~RestorePhysicsSettings()
        {
            LamaPon::SetActivePhysicsSettings(original);
        }
    };

    // 剛体を持たないゴーストで、等速走行の記録を描画します。時計の計算式を
    // テスト内で再現せず、実際の物理・補間行列との位置の一致を確認します。
    class GhostProbe final : public LamaPon::Component
    {
    public:
        GhostProbe(LamaPon::Scene& scene, LamaPon::GameObject& target)
            : m_scene(scene), m_target(target)
        {
        }

        double fixedTime{};
        float maximumRelativeError{};
        float oldUpdateClockError{};
        std::size_t updateCount{};
        std::size_t lateUpdateCount{};
        std::size_t fixedCount{};
        bool changeStepOnFirstFixed{};
        std::vector<float> receivedSteps;

    protected:
        void OnUpdate(float) override
        {
            ++updateCount;
            m_fixedCountBeforeFrame = fixedCount;
            m_oldGhostX = Speed * static_cast<float>(fixedTime);
        }

        void OnFixedUpdate(const float deltaTime) override
        {
            fixedTime += static_cast<double>(deltaTime);
            ++fixedCount;
            receivedSteps.push_back(deltaTime);
            if (changeStepOnFirstFixed && fixedCount == 1)
            {
                auto settings = LamaPon::ActivePhysicsSettings();
                settings.fixedTimeStep = 1.0f / 120.0f;
                LamaPon::SetActivePhysicsSettings(settings);
            }
        }

        void OnLateUpdate(float) override
        {
            ++lateUpdateCount;
            const auto& timing = m_scene.PhysicsTiming();
            Require(updateCount == lateUpdateCount,
                "LateUpdate must follow Update exactly once per frame.");
            Require(timing.fixedSteps == fixedCount - m_fixedCountBeforeFrame,
                "LateUpdate must see all completed fixed steps for this frame.");
            Owner().GetTransform().position.x = Speed
                * static_cast<float>(timing.InterpolateTime(fixedTime));
            const float bodyX = RenderedX(m_target, m_scene);
            maximumRelativeError = std::max(maximumRelativeError,
                std::abs(Owner().GetTransform().position.x - bodyX));
            oldUpdateClockError = std::max(oldUpdateClockError,
                std::abs(m_oldGhostX - bodyX));
        }

    private:
        LamaPon::Scene& m_scene;
        LamaPon::GameObject& m_target;
        std::size_t m_fixedCountBeforeFrame{};
        float m_oldGhostX{};
    };

    LamaPon::GameObject& AddBody(LamaPon::Scene& scene)
    {
        auto& object = scene.CreateGameObject("Moving body");
        object.AddComponent<LamaPon::RigidbodyComponent>(
            DirectX::XMFLOAT3{ Speed, 0.0f, 0.0f }, false);
        return object;
    }

    void CheckCadence(LamaPon::GraphicsDevice& graphics, const float fixedStep,
        const std::span<const float> frameDeltas)
    {
        RestorePhysicsSettings restore;
        auto settings = restore.original;
        settings.fixedTimeStep = fixedStep;
        LamaPon::SetActivePhysicsSettings(settings);
        LamaPon::Scene scene(graphics);
        // レース開始前にもScene時計は進みます。ゲーム固有の0始まりの
        // タイマーを受け取れることも、同じ位置比較で検証します。
        scene.Update(0.1f);
        auto& target = AddBody(scene);
        auto& ghost = scene.CreateGameObject("Ghost without Rigidbody");
        auto& probe = ghost.AddComponent<GhostProbe>(scene, target);
        for (int cycle = 0; cycle < 80; ++cycle)
        {
            for (const float delta : frameDeltas)
            {
                scene.Update(delta);
            }
        }
        std::cout << "fixedHz=" << 1.0f / fixedStep
            << " frameHz=" << (frameDeltas.size() == 1 ? 1.0f / frameDeltas.front() : 0.0f)
            << " maxGhostError=" << probe.maximumRelativeError
            << " oldUpdateError=" << probe.oldUpdateClockError
            << " fixedTime=" << probe.fixedTime
            << " bodyX=" << target.GetTransform().position.x << '\n';
        // 約90mの走行で生じるfloat座標の積算誤差を許容します。2mmは
        // この速度の固定1ステップ（最短5cm）より十分小さい閾値です。
        Require(probe.maximumRelativeError < 0.002f,
            "Ghost and interpolated body drifted under variable frame timing.");
        Require(probe.oldUpdateClockError > 0.005f || frameDeltas.size() == 1,
            "The jitter regression must reproduce the original Update clock mismatch.");
        Require(scene.PhysicsTiming().InterpolateTime(0.0) == 0.0,
            "A newly reset game timer must never sample before its first record.");
    }

    void CheckPauseAndTeleport(LamaPon::GraphicsDevice& graphics)
    {
        LamaPon::Scene scene(graphics);
        auto& target = AddBody(scene);
        scene.Update(1.0f / 60.0f);
        scene.Update(1.0f / 120.0f);
        const float position = RenderedX(target, scene);
        const auto timing = scene.PhysicsTiming();
        Require(std::abs(position - 0.05f) < 0.0001f,
            "Pause regression needs a partially interpolated pose.");
        for (int frame = 0; frame < 3; ++frame)
        {
            scene.Update(0.0f);
            Require(RenderedX(target, scene) == position
                && scene.PhysicsTiming().PresentationTime() == timing.PresentationTime()
                && scene.PhysicsFixedStepsLastFrame() == 0,
                "Pausing must preserve both the displayed pose and its clock.");
        }
        scene.Update(1.0f / 240.0f);
        Require(std::abs(RenderedX(target, scene) - 0.075f) < 0.0001f,
            "Resuming before another fixed step must advance interpolation smoothly.");
        target.GetTransform().position.x = 10.0f;
        scene.Update(0.0f);
        Require(RenderedX(target, scene) == 10.0f,
            "Zero-time editor edits must still synchronize teleported transforms.");
    }

    void CheckDroppedTimeAndReset(LamaPon::GraphicsDevice& graphics)
    {
        RestorePhysicsSettings restore;
        auto settings = restore.original;
        settings.maximumCatchUpSteps = 2;
        LamaPon::SetActivePhysicsSettings(settings);
        LamaPon::Scene scene(graphics);
        auto& target = AddBody(scene);
        auto& probe = scene.CreateGameObject("Ghost")
            .AddComponent<GhostProbe>(scene, target);
        scene.Update(0.5f);
        const auto timing = scene.PhysicsTiming();
        Require(timing.fixedSteps == 2
            && std::abs(timing.simulatedTime + timing.discardedDeltaTime - 0.5) < 1e-6
            && timing.discardedDeltaTime > 0.46
            && timing.discardedTime == timing.discardedDeltaTime,
            "Clamped and capped elapsed time must be reported without advancing simulation.");
        Require(probe.maximumRelativeError < 0.0001f,
            "A capped catch-up frame must not let the ghost jump ahead of physics.");
        scene.Update(0.0f);
        Require(scene.PhysicsTiming().discardedDeltaTime == 0.0
            && scene.PhysicsTiming().discardedTime == timing.discardedTime,
            "Discard diagnostics must distinguish current frame and accumulated totals.");
        scene.Clear();
        Require(scene.PhysicsTiming().simulatedTime == 0.0
            && scene.PhysicsTiming().discardedTime == 0.0
            && scene.PhysicsTiming().PresentationTime() == 0.0
            && scene.PhysicsFixedStepsLastFrame() == 0
            && scene.PhysicsInterpolationAlpha() == 0.0f,
            "Clear must reset the whole scene clock and interpolation history.");
        scene.Update(1.0f / 120.0f);
        Require(scene.PhysicsFixedStepsLastFrame() == 0,
            "Clear must discard the previous accumulator.");
    }

    void CheckFrameSettingsSnapshot(LamaPon::GraphicsDevice& graphics)
    {
        RestorePhysicsSettings restore;
        LamaPon::Scene scene(graphics);
        auto& target = AddBody(scene);
        auto& probe = scene.CreateGameObject("Settings changer")
            .AddComponent<GhostProbe>(scene, target);
        probe.changeStepOnFirstFixed = true;
        scene.Update(1.0f / 30.0f);
        Require(probe.receivedSteps.size() == 2
            && probe.receivedSteps[0] == 1.0f / 60.0f
            && probe.receivedSteps[1] == 1.0f / 60.0f
            && std::abs(target.GetTransform().position.x - 0.2f) < 0.0001f,
            "A callback must not change the timestep half way through a frame.");
        scene.Update(1.0f / 120.0f);
        Require(probe.receivedSteps.size() == 3
            && probe.receivedSteps.back() == 1.0f / 120.0f
            && scene.PhysicsTiming().fixedDeltaTime == 1.0f / 120.0f
            && probe.maximumRelativeError < 0.0001f,
            "New timestep settings must apply consistently on the next frame.");
    }
}

int main()
{
    try
    {
        RestorePhysicsSettings restore;
        LamaPon::SetActivePhysicsSettings({});
        LamaPon::GraphicsDevice graphics;
        for (const float fixedRate : { 30.0f, 60.0f, 120.0f })
        {
            for (const float frameRate : { 30.0f, 60.0f, 75.0f, 144.0f })
            {
                const std::array cadence{ 1.0f / frameRate };
                CheckCadence(graphics, 1.0f / fixedRate, cadence);
            }
            const std::array variableCadence{
                1.0f / 144.0f, 0.049f, 0.002f, 0.1f, 0.0f, 0.035f };
            CheckCadence(graphics, 1.0f / fixedRate, variableCadence);
        }
        CheckPauseAndTeleport(graphics);
        CheckDroppedTimeAndReset(graphics);
        CheckFrameSettingsSnapshot(graphics);
        std::cout << "Physics presentation timing tests passed (15 cadences, pause, caps, reset, settings).\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
