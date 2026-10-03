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
    // 等速移動するテスト物体の速度
    constexpr float Speed = 6.0f;

    // Require(condition: 条件, message: 失敗理由)は不成立時に例外を送出する。
    void Require(const bool condition, const char* message)
    {
        // 条件違反を検出する
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // RenderedX(object: 対象物体, scene: 所属Scene)は補間後のX座標を返す。
    float RenderedX(const LamaPon::GameObject& object, const LamaPon::Scene& scene)
    {
        // 補間後のワールド行列
        DirectX::XMFLOAT4X4 matrix{};
        DirectX::XMStoreFloat4x4(&matrix,
            object.InterpolatedWorldMatrix(scene.PhysicsInterpolationAlpha()));
        return matrix._41;
    }

    struct RestorePhysicsSettings final
    {
        // 復元する物理設定
        LamaPon::PhysicsSettings original = LamaPon::ActivePhysicsSettings();
        // テスト前の物理設定へ戻す
        ~RestorePhysicsSettings()
        {
            LamaPon::SetActivePhysicsSettings(original);
        }
    };

    // 剛体なしの移動位置を実際の補間座標と照合する計測器。
    class GhostProbe final : public LamaPon::Component
    {
    public:
        // GhostProbe(scene: 所属Scene, target: 剛体対象)は位置比較用の参照を保持する。
        GhostProbe(LamaPon::Scene& scene, LamaPon::GameObject& target)
            : m_scene(scene), m_target(target)
        {
        }

        // 固定更新経過時間
        double fixedTime{};
        // ゴーストと剛体の最大誤差
        float maximumRelativeError{};
        // 旧Update時計との差
        float oldUpdateClockError{};
        // Update呼び出し数
        std::size_t updateCount{};
        // LateUpdate呼び出し数
        std::size_t lateUpdateCount{};
        // 固定更新呼び出し数
        std::size_t fixedCount{};
        // 初回固定更新で刻みを変更するか
        bool changeStepOnFirstFixed{};
        // 固定更新で受け取った時間刻み
        std::vector<float> receivedSteps;

    protected:
        // Updateごとの固定時計を記録する
        void OnUpdate(float) override
        {
            ++updateCount;
            m_fixedCountBeforeFrame = fixedCount;
            m_oldGhostX = Speed * static_cast<float>(fixedTime);
        }

        // OnFixedUpdate(deltaTime: 固定時間刻み)はゴースト時計を進める。
        void OnFixedUpdate(const float deltaTime) override
        {
            fixedTime += static_cast<double>(deltaTime);
            ++fixedCount;
            receivedSteps.push_back(deltaTime);
            // 初回固定更新で時間刻みを変える
            if (changeStepOnFirstFixed && fixedCount == 1)
            {
                // 現在の物理設定
                auto settings = LamaPon::ActivePhysicsSettings();
                settings.fixedTimeStep = 1.0f / 120.0f;
                LamaPon::SetActivePhysicsSettings(settings);
            }
        }

        // 描画時刻に対応するゴースト位置を剛体と比較する
        void OnLateUpdate(float) override
        {
            ++lateUpdateCount;
            // Sceneの物理タイミング
            const auto& timing = m_scene.PhysicsTiming();
            Require(updateCount == lateUpdateCount,
                "LateUpdate must follow Update exactly once per frame.");
            Require(timing.fixedSteps == fixedCount - m_fixedCountBeforeFrame,
                "LateUpdate must see all completed fixed steps for this frame.");
            Owner().GetTransform().position.x = Speed
                * static_cast<float>(timing.InterpolateTime(fixedTime));
            // 補間後の剛体X座標
            const float bodyX = RenderedX(m_target, m_scene);
            maximumRelativeError = std::max(maximumRelativeError,
                std::abs(Owner().GetTransform().position.x - bodyX));
            oldUpdateClockError = std::max(oldUpdateClockError,
                std::abs(m_oldGhostX - bodyX));
        }

    private:
        // ゴーストが所属するScene
        LamaPon::Scene& m_scene;
        // 位置比較対象の剛体
        LamaPon::GameObject& m_target;
        // フレーム開始前の固定更新数
        std::size_t m_fixedCountBeforeFrame{};
        // 旧時計によるゴースト位置
        float m_oldGhostX{};
    };

    // AddBody(scene: 所属Scene)は等速移動する剛体を追加する。
    LamaPon::GameObject& AddBody(LamaPon::Scene& scene)
    {
        // 速度検証用のゲームオブジェクト
        auto& object = scene.CreateGameObject("Moving body");
        object.AddComponent<LamaPon::RigidbodyComponent>(
            DirectX::XMFLOAT3{ Speed, 0.0f, 0.0f }, false);
        return object;
    }

    // CheckCadence(graphics: 描画デバイス, fixedStep: 固定刻み, frameDeltas: フレーム時間列)は時計同期を検証する。
    void CheckCadence(LamaPon::GraphicsDevice& graphics, const float fixedStep,
        const std::span<const float> frameDeltas)
    {
        // 検証後に物理設定を復元するScope
        RestorePhysicsSettings restore;
        // 変更対象の物理設定
        auto settings = restore.original;
        settings.fixedTimeStep = fixedStep;
        LamaPon::SetActivePhysicsSettings(settings);
        // 更新周期を測定するScene
        LamaPon::Scene scene(graphics);
        // Scene時計を進めてもゲーム時計の起点から位置が一致することを確認する。
        scene.Update(0.1f);
        // 位置同期を比較する剛体
        auto& target = AddBody(scene);
        // 剛体なしのゴースト
        auto& ghost = scene.CreateGameObject("Ghost without Rigidbody");
        // ゴーストの時計計測コンポーネント
        auto& probe = ghost.AddComponent<GhostProbe>(scene, target);
        // 80サイクルのフレーム列を検証する
        for (int cycle = 0; cycle < 80; ++cycle)
        {
            // 各フレーム時間を使ってSceneを更新する
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
        // 90m走行時の丸めを許容し、固定ステップ幅より小さい2mmを誤差閾値にする。
        Require(probe.maximumRelativeError < 0.002f,
            "Ghost and interpolated body drifted under variable frame timing.");
        Require(probe.oldUpdateClockError > 0.005f || frameDeltas.size() == 1,
            "The jitter regression must reproduce the original Update clock mismatch.");
        Require(scene.PhysicsTiming().InterpolateTime(0.0) == 0.0,
            "A newly reset game timer must never sample before its first record.");
    }

    // CheckPauseAndTeleport(graphics: 描画デバイス)は停止・再開・テレポートを検証する。
    void CheckPauseAndTeleport(LamaPon::GraphicsDevice& graphics)
    {
        // 一時停止対象のScene
        LamaPon::Scene scene(graphics);
        // 移動する剛体
        auto& target = AddBody(scene);
        scene.Update(1.0f / 60.0f);
        scene.Update(1.0f / 120.0f);
        // 一時停止中に維持する描画位置
        const float position = RenderedX(target, scene);
        // 一時停止前の物理時計
        const auto timing = scene.PhysicsTiming();
        Require(std::abs(position - 0.05f) < 0.0001f,
            "Pause regression needs a partially interpolated pose.");
        // 停止状態を3フレーム維持する
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

    // CheckDroppedTimeAndReset(graphics: 描画デバイス)は時間上限と時計リセットを検証する。
    void CheckDroppedTimeAndReset(LamaPon::GraphicsDevice& graphics)
    {
        // 検証後に物理設定を復元するScope
        RestorePhysicsSettings restore;
        // 変更対象の物理設定
        auto settings = restore.original;
        settings.maximumCatchUpSteps = 2;
        LamaPon::SetActivePhysicsSettings(settings);
        // 時間上限を検証するScene
        LamaPon::Scene scene(graphics);
        // Scene内の移動剛体
        auto& target = AddBody(scene);
        // 時間同期を計測するゴースト
        auto& probe = scene.CreateGameObject("Ghost")
            .AddComponent<GhostProbe>(scene, target);
        scene.Update(0.5f);
        // 上限適用後の物理時計
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

    // CheckFrameSettingsSnapshot(graphics: 描画デバイス)はフレーム中の設定固定を検証する。
    void CheckFrameSettingsSnapshot(LamaPon::GraphicsDevice& graphics)
    {
        // 検証後に物理設定を復元するScope
        RestorePhysicsSettings restore;
        // 固定刻みを変更するScene
        LamaPon::Scene scene(graphics);
        // 設定変更対象の剛体
        auto& target = AddBody(scene);
        // 固定更新で設定を変更するゴースト
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

    // CheckPhysicsDebugContacts(graphics: 描画デバイス)は接触記録の有効期間を検証する。
    void CheckPhysicsDebugContacts(LamaPon::GraphicsDevice& graphics)
    {
        // 接触記録を検証するScene
        LamaPon::Scene scene(graphics);
        // 接触対象の床
        auto& ground = scene.CreateGameObject("Ground");
        ground.AddComponent<LamaPon::BoxCollider3DComponent>(
            DirectX::XMFLOAT3{ 10.0f, 1.0f, 10.0f });
        // 床に接触する箱
        auto& box = scene.CreateGameObject("Box");
        // 床の上面(0.5)へ少しめり込ませ、最初のステップで接触させます。
        box.GetTransform().position = { 0.0f, 0.95f, 0.0f };
        box.AddComponent<LamaPon::BoxCollider3DComponent>();
        box.AddComponent<LamaPon::RigidbodyComponent>();

        scene.Update(1.0f / 60.0f);
        Require(!scene.IsPhysicsDebugCaptureEnabled()
            && scene.PhysicsDebugContacts().empty(),
            "Physics debug contacts must not be recorded unless enabled.");

        scene.SetPhysicsDebugCaptureEnabled(true);
        scene.Update(1.0f / 60.0f);
        // 有効化後の接触記録
        const auto& contacts = scene.PhysicsDebugContacts();
        Require(!contacts.empty(),
            "Enabled physics debug capture did not record the resting contact.");
        // 記録された接触点の両端を確認する
        for (const auto& contact : contacts)
        {
            // 床と箱の組み合わせ
            const bool pair =
                (contact.left == ground.Id() && contact.right == box.Id())
                || (contact.left == box.Id() && contact.right == ground.Id());
            Require(pair && contact.is3D && !contact.isTrigger
                && std::isfinite(contact.point.y)
                && std::abs(contact.normal.y) > 0.5f,
                "A physics debug contact did not describe the box on the ground.");
        }

        scene.SetPhysicsDebugCaptureEnabled(false);
        Require(scene.PhysicsDebugContacts().empty(),
            "Disabling physics debug capture must release recorded contacts.");
    }
}

// 物理時計の周期・停止・リセットと接触記録を検証する
int main()
{
    // テスト失敗を終了コードに変換する
    try
    {
        // テスト前の物理設定を復元するScope
        RestorePhysicsSettings restore;
        LamaPon::SetActivePhysicsSettings({});
        // テスト用描画デバイス
        LamaPon::GraphicsDevice graphics;
        // 複数の固定更新周期を比較する
        for (const float fixedRate : { 30.0f, 60.0f, 120.0f })
        {
            // 複数の描画周期を比較する
            for (const float frameRate : { 30.0f, 60.0f, 75.0f, 144.0f })
            {
                // 固定周期のフレーム時間列
                const std::array cadence{ 1.0f / frameRate };
                CheckCadence(graphics, 1.0f / fixedRate, cadence);
            }
            // ジッターを含むフレーム時間列
            const std::array variableCadence{
                1.0f / 144.0f, 0.049f, 0.002f, 0.1f, 0.0f, 0.035f };
            CheckCadence(graphics, 1.0f / fixedRate, variableCadence);
        }
        CheckPauseAndTeleport(graphics);
        CheckDroppedTimeAndReset(graphics);
        CheckFrameSettingsSnapshot(graphics);
        CheckPhysicsDebugContacts(graphics);
        std::cout << "Physics presentation timing tests passed (15 cadences, pause, caps, reset, settings, debug contacts).\n";
        // テスト成功を返す
        return 0;
    }
    // 例外内容を出力して失敗終了する
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        // テスト失敗を返す
        return 1;
    }
}
