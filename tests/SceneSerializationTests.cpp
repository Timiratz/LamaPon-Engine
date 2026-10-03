#include "LamaPon/LamaPon.h"
#include "LamaPon/Assets/ModelLod.h"

#include <nlohmann/json.hpp>

#include <DirectXMath.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    // 更新中に足されるだけのComponent。
    // m_componentsの再確保を起こす役です。
    class MarkerProbeComponent final : public LamaPon::Component
    {
    };

    // 最初のOnUpdateで、GameObjectとComponentをまとめて作ります。
    // ScriptのStartがやることを、Scriptを使わずに再現しています。
    class SpawnOnUpdateProbe final : public LamaPon::Component
    {
    public:
        // scene: child objectを生成するscene。
        LamaPon::Scene* scene{};
        // updateCount: Update callbackの呼出回数。
        int updateCount{};

    protected:
        // OnUpdate(): 初回updateでchildとcomponentを生成します。
        void OnUpdate(float) override
        {
            ++updateCount;
            // このbehaviourのobject生成済み状態を確認します。
            if (m_spawned)
            {
                // spawn済みobjectの再生成を止めて戻ります。
                return;
            }
            m_spawned = true;

            // 64個のfixture要素を順に生成します。
            for (int index = 0; index < 64; ++index)
            {
                scene->CreateGameObject(
                    "Spawned " + std::to_string(index));
            }
            Owner().AddComponent<MarkerProbeComponent>();
        }

    private:
        // m_spawned: 初回updateでobjectを生成済みか。
        bool m_spawned{};
    };

    class CollisionProbeComponent final : public LamaPon::Component
    {
    public:
        // enterCount: collision開始通知数。
        int enterCount{};
        // stayCount: collision継続通知数。
        int stayCount{};
        // exitCount: collision終了通知数。
        int exitCount{};
        // triggerEnterCount: trigger開始通知数。
        int triggerEnterCount{};
        // triggerExitCount: trigger終了通知数。
        int triggerExitCount{};
        // lastPoint: 最後のcontact位置。
        DirectX::XMFLOAT3 lastPoint{};

    protected:
        // OnCollisionEnter(event: contact情報): 回数と位置を記録します。
        void OnCollisionEnter(
            const LamaPon::CollisionEvent& event) override
        {
            ++enterCount;
            lastPoint = event.point;
        }

        // OnCollisionStay(): 継続中collisionの通知数を増やします。
        void OnCollisionStay(const LamaPon::CollisionEvent&) override
        {
            ++stayCount;
        }

        // OnCollisionExit(): collision終了通知数を増やします。
        void OnCollisionExit(const LamaPon::CollisionEvent&) override
        {
            ++exitCount;
        }

        // OnTriggerEnter(event: contact情報): 回数と位置を記録します。
        void OnTriggerEnter(
            const LamaPon::CollisionEvent& event) override
        {
            ++triggerEnterCount;
            lastPoint = event.point;
        }

        // OnTriggerExit(): trigger終了通知数を増やします。
        void OnTriggerExit(const LamaPon::CollisionEvent&) override
        {
            ++triggerExitCount;
        }
    };

    // frame更新時間と物理forceを記録します。
    class FixedUpdateProbeComponent final
        : public LamaPon::Component
    {
    public:
        // fixedUpdateCount: 固定更新callback数。
        int fixedUpdateCount{};
        // updateElapsed: 可変更新の累積秒数。
        float updateElapsed{};
        // fixedElapsed: 固定更新の累積秒数。
        float fixedElapsed{};

    protected:
        // OnUpdate(deltaTime: frame経過秒): 可変更新時間を集計します。
        void OnUpdate(const float deltaTime) override
        {
            updateElapsed += deltaTime;
        }

        // OnFixedUpdate(fixedDeltaTime: 固定更新秒): 時間とrigidbody forceを記録します。
        void OnFixedUpdate(
            const float fixedDeltaTime) override
        {
            ++fixedUpdateCount;
            fixedElapsed += fixedDeltaTime;
            // ownerにrigidbodyがある場合だけforceを加えます。
            if (auto* body =
                    Owner().GetComponent<
                        LamaPon::RigidbodyComponent>())
            {
                body->AddForce(
                    DirectX::XMFLOAT3{
                        1.0f,
                        0.0f,
                        0.0f },
                    LamaPon::ForceMode::Acceleration);
            }
        }
    };

    // active遷移と通常更新のcallback数を記録します。
    class ActiveStateProbeComponent final
        : public LamaPon::Component
    {
    public:
        // enableCount: active化通知数。
        int enableCount{};
        // disableCount: inactive化通知数。
        int disableCount{};
        // updateCount: 通常更新callback数。
        int updateCount{};
        // lateUpdateCount: 遅延更新callback数。
        int lateUpdateCount{};

    protected:
        // OnUpdate(): 通常更新callback数を増やします。
        void OnUpdate(float) override
        {
            ++updateCount;
        }

        // OnLateUpdate(): 遅延更新callback数を増やします。
        void OnLateUpdate(float) override
        {
            ++lateUpdateCount;
        }

        // OnActiveStateChanged(active: 有効状態): enable/disable通知を集計します。
        void OnActiveStateChanged(const bool active) override
        {
            // active状態に応じてenableとdisableを記録します。
            if (active)
            {
                ++enableCount;
            }
            // 非active状態への遷移をdisableとして記録します。
            else
            {
                ++disableCount;
            }
        }
    };

    // collisionとtriggerの開始・継続・終了を記録します。
    class TriggerProbeComponent final
        : public LamaPon::Component
    {
    public:
        // collisionEnterCount: collision開始通知数。
        int collisionEnterCount{};
        // triggerEnterCount: trigger開始通知数。
        int triggerEnterCount{};
        // triggerStayCount: trigger継続通知数。
        int triggerStayCount{};
        // triggerExitCount: trigger終了通知数。
        int triggerExitCount{};

    protected:
        // OnCollisionEnter(): collision開始通知数を増やします。
        void OnCollisionEnter(
            const LamaPon::CollisionEvent&) override
        {
            ++collisionEnterCount;
        }

        // OnTriggerEnter(): trigger開始通知数を増やします。
        void OnTriggerEnter(
            const LamaPon::CollisionEvent&) override
        {
            ++triggerEnterCount;
        }

        // OnTriggerStay(): trigger継続通知数を増やします。
        void OnTriggerStay(
            const LamaPon::CollisionEvent&) override
        {
            ++triggerStayCount;
        }

        // OnTriggerExit(): trigger終了通知数を増やします。
        void OnTriggerExit(
            const LamaPon::CollisionEvent&) override
        {
            ++triggerExitCount;
        }
    };

    // Require(condition: 期待条件, message: 失敗説明): 条件違反をtest failureにします。
    void Require(const bool condition, const char* message)
    {
        // 失敗条件が成立した場合はテストを停止します。
        if (!condition)
        {
            // 不正なfixture状態を例外で通知します。
            throw std::runtime_error(message);
        }
    }

    // NearlyEqual(left: 値A, right: 値B): 浮動小数の許容誤差を判定します。
    bool NearlyEqual(const float left, const float right) noexcept
    {
        // 浮動小数値が許容誤差内か返します。
        return std::abs(left - right) < 0.0001f;
    }
}

// RunTest(suite: 実行suite): 選択されたscene回帰群を実行します。
int RunTest(const std::string_view suite)
{
    // 選択されたsuiteを実行し、例外を失敗結果へ変換します。
    try
    {
        // graphics: scene fixture用graphics device。
        LamaPon::GraphicsDevice graphics;
        // serialization suiteの検証を選びます。
        if (suite == "serialization")
        {
            // 焼き込みGIの復元はGPUを初期化せず検証できます。
            // 上限内の最大形は受理し、不正な形は既存の有効結果を壊しません。
            LamaPon::Scene bakedGiRestoreScene(graphics);
            // acceptedShape: 受理境界のGI設定。
            LamaPon::BakedGlobalIlluminationSettings acceptedShape;
            acceptedShape.resolutionX = 64;
            acceptedShape.resolutionY = 32;
            acceptedShape.resolutionZ = 16;
            // acceptedPayload: 上限内で受理するGI係数列。
            std::vector<std::uint16_t> acceptedPayload(
                LamaPon::BakedGlobalIlluminationMaximumProbeCount
                    * LamaPon::
                        BakedGlobalIlluminationCoefficientsPerProbe,
                0x3c00);
            bakedGiRestoreScene.RestoreBakedGlobalIllumination(
                acceptedShape,
                acceptedPayload);
            Require(
                bakedGiRestoreScene.HasBakedGlobalIllumination()
                    && bakedGiRestoreScene
                        .BakedGlobalIlluminationBakedShape()
                        .resolutionX == 64
                    && bakedGiRestoreScene
                        .BakedGlobalIlluminationPayload()
                        == acceptedPayload,
                "The maximum valid baked GI payload was rejected.");

            // invalidAxisShape: axis不正のGI設定。
            auto invalidAxisShape = acceptedShape;
            invalidAxisShape.resolutionX = 65;
            invalidAxisShape.resolutionY = 1;
            invalidAxisShape.resolutionZ = 1;
            bakedGiRestoreScene.RestoreBakedGlobalIllumination(
                invalidAxisShape,
                std::vector<std::uint16_t>(
                    65 * LamaPon::
                        BakedGlobalIlluminationCoefficientsPerProbe));

            // excessiveProbeShape: 上限超過GI probe形状。
            auto excessiveProbeShape = acceptedShape;
            excessiveProbeShape.resolutionX = 64;
            excessiveProbeShape.resolutionY = 27;
            excessiveProbeShape.resolutionZ = 19;
            bakedGiRestoreScene.RestoreBakedGlobalIllumination(
                excessiveProbeShape,
                std::vector<std::uint16_t>(
                    64 * 27 * 19 * LamaPon::
                        BakedGlobalIlluminationCoefficientsPerProbe));
            Require(
                bakedGiRestoreScene
                        .BakedGlobalIlluminationBakedShape()
                        .resolutionX == 64
                    && bakedGiRestoreScene
                        .BakedGlobalIlluminationBakedShape()
                        .resolutionY == 32
                    && bakedGiRestoreScene
                        .BakedGlobalIlluminationPayload()
                        == acceptedPayload,
                "Invalid baked GI data replaced a valid payload.");

            // JSON経路でも形をbase64 decode前に拒否します。
            // 65点分の1560バイトを表す文字列でも、許可範囲を超える形は拒否します。
            LamaPon::Scene emptyScene(graphics);
            // invalidBakedGiDocument: 不正baked GI JSON。
            auto invalidBakedGiDocument = nlohmann::json::parse(
                emptyScene.SerializeToJson());
            // bakedGi: serialize対象のbaked GI data。
            auto& bakedGi =
                invalidBakedGiDocument["environment"]
                    ["bakedGlobalIllumination"];
            bakedGi["bakedResolution"] = { 65, 1, 1 };
            bakedGi["data"] = std::string(2080, 'A');
            // invalidBakedGiScene: 不正GI受理確認用scene。
            LamaPon::Scene invalidBakedGiScene(graphics);
            invalidBakedGiScene.LoadFromJson(
                invalidBakedGiDocument.dump());
            Require(
                !invalidBakedGiScene
                    .HasBakedGlobalIllumination(),
                "An invalid baked GI resolution was restored from JSON.");

            // GPU側の所有表現を変更しても、保存形式は従来どおりbaked shapeとfp16 word列をbit単位で維持します。
            // 現在の編集設定とは独立して復元されなければなりません。
            LamaPon::Scene patternedBakedGiScene(graphics);
            // currentSettings: 復元前のGI設定。
            LamaPon::BakedGlobalIlluminationSettings currentSettings;
            currentSettings.enabled = true;
            currentSettings.center = { 9.0f, 8.0f, 7.0f };
            currentSettings.size = { 6.0f, 5.0f, 4.0f };
            currentSettings.resolutionX = 3;
            currentSettings.resolutionY = 2;
            currentSettings.resolutionZ = 1;
            currentSettings.intensity = 0.25f;
            patternedBakedGiScene
                .SetBakedGlobalIlluminationSettings(currentSettings);

            // bakedShape: baked GI境界形状。
            LamaPon::BakedGlobalIlluminationSettings bakedShape;
            bakedShape.enabled = true;
            bakedShape.center = { -1.0f, -2.0f, -3.0f };
            bakedShape.size = { 2.0f, 4.0f, 6.0f };
            bakedShape.resolutionX = 2;
            bakedShape.resolutionY = 1;
            bakedShape.resolutionZ = 2;
            // patternedPayload: 検証patternを設定するGI係数列。
            std::vector<std::uint16_t> patternedPayload(
                4 * LamaPon::
                    BakedGlobalIlluminationCoefficientsPerProbe);
            // 係数payloadの各要素へ検証用patternを設定します。
            for (std::size_t index{};
                index < patternedPayload.size();
                ++index)
            {
                patternedPayload[index] = static_cast<std::uint16_t>(
                    0x1200u + index * 37u);
            }
            patternedBakedGiScene.RestoreBakedGlobalIllumination(
                bakedShape,
                patternedPayload);

            // patternedRoundTripScene: patterned GIの再読込scene。
            LamaPon::Scene patternedRoundTripScene(graphics);
            patternedRoundTripScene.LoadFromJson(
                patternedBakedGiScene.SerializeToJson());
            // restoredCurrent: 復元後のcurrent GI state。
            const auto& restoredCurrent =
                patternedRoundTripScene.BakedGlobalIllumination();
            // restoredShape: 復元後のGI volume shape。
            const auto& restoredShape =
                patternedRoundTripScene
                    .BakedGlobalIlluminationBakedShape();
            Require(
                patternedRoundTripScene.HasBakedGlobalIllumination()
                    && patternedRoundTripScene
                        .BakedGlobalIlluminationPayload()
                        == patternedPayload
                    && restoredCurrent.enabled
                    && restoredCurrent.center.x == 9.0f
                    && restoredCurrent.resolutionX == 3
                    && restoredCurrent.resolutionY == 2
                    && restoredCurrent.resolutionZ == 1
                    && NearlyEqual(restoredCurrent.intensity, 0.25f)
                    && restoredShape.center.x == -1.0f
                    && restoredShape.center.y == -2.0f
                    && restoredShape.center.z == -3.0f
                    && restoredShape.size.x == 2.0f
                    && restoredShape.size.y == 4.0f
                    && restoredShape.size.z == 6.0f
                    && restoredShape.resolutionX == 2
                    && restoredShape.resolutionY == 1
                    && restoredShape.resolutionZ == 2,
                "Baked GI shape or patterned fp16 payload changed during JSON round trip.");
        }

        // source: serialization元scene。
        LamaPon::Scene source(graphics);
        source.SetAmbientLightColor(
            DirectX::XMFLOAT3{ 0.25f, 0.35f, 0.45f });
        source.SetAmbientLightIntensity(0.6f);
        source.SetPhysicsBroadPhaseCellSize(3.5f);
        source.SetFrustumCullingEnabled(false);
        source.SetOcclusionCullingEnabled(true);
        source.SetSkySettings({
            true,
            { 0.1f, 0.2f, 0.3f },
            { 0.4f, 0.5f, 0.6f },
            { 0.02f, 0.03f, 0.04f },
            1.4f
        });
        source.SetFogSettings({
            true,
            { 0.35f, 0.45f, 0.55f },
            6.0f,
            42.0f,
            0.025f
        });
        source.SetBloomSettings({
            true,
            0.8f,
            0.65f,
            3.0f
        });
        source.SetScreenOutlineSettings({
            true,
            { 0.1f, 0.2f, 0.3f },
            0.8f,
            2.5f,
            0.04f,
            0.35f
        });
        source.SetScreenSpaceLensFlareSettings({
            true,
            1.9f,
            0.55f,
            0.42f,
            0.31f,
            0.12f,
            0.27f,
            0.36f
        });
        source.SetDepthOfFieldSettings({
            true,
            7.5f,
            1.25f,
            1.6f,
            14.0f
        });
        source.SetMotionBlurSettings({
            true,
            0.8f,
            22.0f
        });
        source.SetAutoExposureSettings({
            true,
            0.24f,
            0.05f,
            6.5f,
            2.5f,
            0.75f
        });
        source.SetColorGradingSettings({
            false,
            true,
            0.25f,
            1.15f,
            1.2f,
            0.1f,
            -0.05f,
            0.2f
        });

        // root: 保存元sceneのroot GameObject。
        auto& root = source.CreateGameObject("ルート");
        root.GetTransform().position = { 10.0f, 2.0f, 0.0f };
        // RenderCullingComponentの値を、GameObjectの互換アクセサーIsAlwaysVisible/CullingMarginからも読み取れることを検証します。
        root.AddComponent<
            LamaPon::RenderCullingComponent>(true, 12.5f);
        root.AddComponent<LamaPon::DirectionalLightComponent>(
            DirectX::XMFLOAT3{ 1.0f, 0.8f, 0.6f },
            2.25f,
            true,
            32.0f,
            0.002f,
            0.004f,
            0.7f,
            3u,
            0.72f);
        root.AddComponent<LamaPon::PointLightComponent>(
            DirectX::XMFLOAT3{ 0.4f, 0.7f, 1.0f },
            4.5f,
            9.0f);
        root.AddComponent<LamaPon::SpotLightComponent>(
            DirectX::XMFLOAT3{ 1.0f, 0.5f, 0.3f },
            6.0f,
            14.0f,
            DirectX::XMConvertToRadians(18.0f),
            DirectX::XMConvertToRadians(32.0f));
        // sourceProbe: 保存元sceneのtest probe component。
        auto& sourceProbe =
            root.AddComponent<
                LamaPon::ReflectionProbeComponent>(
                7.5f,
                1.25f);
        sourceProbe.SetBoxExtents(
            { 4.0f, 2.5f, 3.5f });
        sourceProbe.SetBlendDistance(2.75f);
        // sourceNavMesh: 保存元sceneのNavMesh component。
        auto& sourceNavMesh =
            root.AddComponent<
                LamaPon::NavMeshComponent>(
                    DirectX::XMFLOAT2{
                        6.0f,
                        6.0f },
                    1.0f,
                    0.35f,
                    1.75f);
        const std::array<
            LamaPon::NavMeshComponent::
                CellCoordinate,
            3> sourceBlockedCells{
                std::pair{ 2u, 1u },
                std::pair{ 2u, 2u },
                std::pair{ 2u, 3u }
            };
        sourceNavMesh.RestoreBake(
            sourceBlockedCells);
        root.AddComponent<
            LamaPon::UICanvasComponent>(
                DirectX::XMFLOAT2{
                    1920.0f,
                    1080.0f },
                0.75f);

        // child: 保存元sceneのchild GameObject。
        auto& child = source.CreateGameObject("子オブジェクト");
        child.SetParent(&root);
        child.GetTransform().position = { 2.0f, 3.0f, 0.0f };
        // sourceSprite: 保存元sceneのsprite renderer component。
        auto& sourceSprite =
            child.AddComponent<LamaPon::SpriteRendererComponent>(
            DirectX::XMFLOAT2{ 64.0f, 32.0f },
            DirectX::XMFLOAT4{ 0.2f, 0.4f, 0.8f, 1.0f },
            std::filesystem::path(L"textures/日本語画像.png"));
        sourceSprite.SetSourceRect(
            { 0.25f, 0.125f, 0.5f, 0.375f });
        sourceSprite.SetShaderPath(
            L"shaders/日本語UI.hlsl");
        sourceSprite.SetCustomParameter(
            0,
            { 1.25f, 2.5f, 3.75f, 5.0f });
        // tilemap: test fixtureのtilemap component。
        auto& tilemap =
            child.AddComponent<
                LamaPon::TilemapComponent>(
                    DirectX::XMFLOAT2{
                        24.0f,
                        16.0f },
                    4,
                    2,
                    DirectX::XMFLOAT4{
                        0.9f,
                        0.8f,
                        0.7f,
                        1.0f },
                    std::filesystem::path(
                        L"textures/日本語タイル.png"));
        tilemap.SetCell(-1, 2, 5);
        tilemap.SetCell(3, 4, 7);
        // sourceParticles: 保存元sceneのparticle system component。
        auto& sourceParticles =
            child.AddComponent<
                LamaPon::ParticleSystemComponent>(
                    128,
                    24.0f,
                    DirectX::XMFLOAT2{
                        0.5f,
                        1.25f },
                    DirectX::XMFLOAT2{
                        1.5f,
                        4.0f },
                    DirectX::XMFLOAT2{
                        0.12f,
                        0.35f },
                    DirectX::XMFLOAT4{
                        0.4f,
                        0.8f,
                        1.0f,
                        0.9f },
                    DirectX::XMFLOAT4{
                        0.1f,
                        0.2f,
                        0.8f,
                        0.0f },
                    LamaPon::ParticleEmitterShape::Sphere,
                    std::filesystem::path(
                        L"textures/日本語粒子.png"));
        sourceParticles.SetEndSizeMultiplier(0.15f);
        sourceParticles.SetRenderMode(
            LamaPon::ParticleRenderMode::Horizontal);
        sourceParticles.SetGravity(
            DirectX::XMFLOAT3{
                0.0f,
                -2.5f,
                0.0f });
        sourceParticles.SetEmitterSize(
            DirectX::XMFLOAT3{
                2.0f,
                1.0f,
                3.0f });
        sourceParticles.SetConeAngle(
            DirectX::XMConvertToRadians(42.0f));
        sourceParticles.SetDuration(3.5f);
        sourceParticles.SetLooping(false);
        sourceParticles.SetPlayOnStart(false);
        sourceParticles.SetPreviewInEditor(false);
        sourceParticles.SetAdditive(false);
        sourceParticles.SetShaderPath(
            L"shaders/ParticleCustom.hlsl");
        sourceParticles.SetAuxiliaryTexturePath(
            L"textures/ParticleMask.png");
        sourceParticles.SetCustomParameter(
            0,
            { 1.0f, 2.0f, 3.0f, 4.0f });
        child.AddComponent<
            LamaPon::UIRectTransformComponent>(
                DirectX::XMFLOAT2{
                    1.0f,
                    1.0f },
                DirectX::XMFLOAT2{
                    1.0f,
                    1.0f },
                DirectX::XMFLOAT2{
                    1.0f,
                    1.0f },
                DirectX::XMFLOAT2{
                    -24.0f,
                    -32.0f },
                DirectX::XMFLOAT2{
                    280.0f,
                    72.0f });
        // sourceButton: 保存元sceneのUI button component。
        auto& sourceButton =
            child.AddComponent<
                LamaPon::UIButtonComponent>(
                    "ゲーム開始",
                    DirectX::XMFLOAT2{
                        280.0f,
                        72.0f },
                    std::filesystem::path(
                        L"textures/UIボタン.png"));
        sourceButton.SetFontFamily(
            "Yu Gothic UI");
        sourceButton.SetFontSize(30.0f);
        sourceButton.SetNormalColor(
            { 0.1f, 0.3f, 0.6f, 0.9f });
        sourceButton.SetHoveredColor(
            { 0.2f, 0.5f, 0.9f, 1.0f });
        sourceButton.SetPressedColor(
            { 0.05f, 0.2f, 0.4f, 1.0f });
        sourceButton.SetDisabledColor(
            { 0.2f, 0.2f, 0.2f, 0.5f });
        sourceButton.SetTextColor(
            { 1.0f, 0.9f, 0.7f, 1.0f });
        sourceButton.SetTargetScene(
            std::filesystem::path(
                L"scenes/次のシーン.scene.json"));
        // sourceNavAgent: 保存元sceneのNavMesh agent component。
        auto& sourceNavAgent =
            child.AddComponent<
                LamaPon::
                    NavMeshAgentComponent>(
                        4.25f,
                        0.2f,
                        true);
        const std::array<
            DirectX::XMFLOAT3,
            3> sourceAgentPath{
                DirectX::XMFLOAT3{
                    12.0f,
                    5.0f,
                    0.0f },
                DirectX::XMFLOAT3{
                    11.0f,
                    5.0f,
                    1.0f },
                DirectX::XMFLOAT3{
                    9.0f,
                    5.0f,
                    2.0f }
            };
        sourceNavAgent.SetPath(
            sourceAgentPath.back(),
            sourceAgentPath);
        child.AddComponent<LamaPon::AudioSourceComponent>(
            std::filesystem::path(L"audio/起動音.wav"),
            0.65f,
            -0.2f,
            0.35f,
            true,
            true,
            true,
            2.5f,
            30.0f);
        child.AddComponent<LamaPon::InputMoverComponent>(
            "MoveHorizontal",
            "MoveVertical",
            4.5f);
        child.AddComponent<
            LamaPon::TransformAnimatorComponent>(
                std::filesystem::path(
                    L"animations/浮遊.animation.json"),
                1.5f,
                false,
                true,
                std::filesystem::path(
                    L"animations/移動.animator.json"));
        child.AddComponent<LamaPon::ModelRendererComponent>(
            std::filesystem::path(L"models/日本語モデル.cmo"),
            true,
            true,
            DirectX::XMFLOAT4{ 0.7f, 0.4f, 0.2f, 1.0f },
            std::filesystem::path(L"textures/モデル色.png"),
            std::filesystem::path(L"textures/モデル法線.png"),
            0.27f,
            1.2f,
            std::filesystem::path(
                L"materials/共有.material.json"),
            2,
            0.75f,
            false,
            false,
            std::filesystem::path(
                L"animations/モデル.animator.json"),
            true,
            "Root");
        child.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Sphere,
            DirectX::XMFLOAT4{ 0.3f, 0.5f, 0.9f, 1.0f },
            std::filesystem::path(L"textures/アルベド.png"),
            std::filesystem::path(L"textures/法線.png"),
            0.32f,
            1.4f,
            std::filesystem::path(
                L"materials/共有.material.json"));
        child.AddComponent<LamaPon::BoxCollider3DComponent>(
            DirectX::XMFLOAT3{ 1.0f, 2.0f, 3.0f },
            DirectX::XMFLOAT3{ 0.1f, 0.2f, 0.3f },
            true,
            3,
            0x10u,
            LamaPon::PhysicsMaterial{ 1.25f, 0.65f });
        child.AddComponent<
            LamaPon::CapsuleCollider3DComponent>(
                0.45f,
                2.4f,
                DirectX::XMFLOAT3{ 0.2f, 0.3f, 0.4f },
                false,
                4,
                0x20u,
                LamaPon::PhysicsMaterial{ 0.75f, 0.35f });
        child.AddComponent<
            LamaPon::SphereCollider3DComponent>(
                0.65f,
                DirectX::XMFLOAT3{ 0.3f, 0.4f, 0.5f },
                true,
                5,
                0x40u,
                LamaPon::PhysicsMaterial{ 0.6f, 0.2f });
        child.AddComponent<
            LamaPon::ConvexHullCollider3DComponent>(
                std::vector<DirectX::XMFLOAT3>{
                    { 0.8f, 0.0f, 0.0f },
                    { -0.8f, 0.0f, 0.0f },
                    { 0.0f, 0.9f, 0.0f },
                    { 0.0f, -0.9f, 0.0f },
                    { 0.0f, 0.0f, 1.1f },
                    { 0.0f, 0.0f, -1.1f }
                },
                DirectX::XMFLOAT3{ 0.4f, 0.5f, 0.6f },
                true,
                6,
                0x80u,
                LamaPon::PhysicsMaterial{ 0.55f, 0.15f });
        child.AddComponent<LamaPon::RigidbodyComponent>(
            DirectX::XMFLOAT3{ 1.0f, 2.0f, 3.0f },
            false,
            true,
            LamaPon::CollisionDetectionMode::Continuous,
            3.5f,
            DirectX::XMFLOAT3{ 0.1f, 0.2f, 0.3f },
            DirectX::XMFLOAT3{ 0.25f, -0.1f, 0.0f },
            0.15f,
            0.25f,
            LamaPon::RigidbodyConstraints{
                false,
                false,
                true
            },
            false);
        child.AddComponent<LamaPon::JointComponent>(
            LamaPon::JointType::Spring,
            root.Id(),
            DirectX::XMFLOAT3{ 0.1f, 0.2f, 0.3f },
            DirectX::XMFLOAT3{ 0.4f, 0.5f, 0.6f },
            DirectX::XMFLOAT3{ 1.0f, 0.0f, 0.0f },
            2.5f,
            18.0f,
            3.0f,
            true,
            true,
            LamaPon::HingeLimits{
                -25.0f,
                40.0f
            },
            true,
            LamaPon::HingeMotor{
                120.0f,
                35.0f
            });
        root.AddComponent<
            LamaPon::LODGroupComponent>(
                std::vector<LamaPon::LODLevel>{
                    { 25.0f, child.Id() }
                },
                80.0f);
        child.AddComponent<LamaPon::TextRendererComponent>(
            "日本語テキスト",
            "Yu Gothic UI",
            28.0f,
            DirectX::XMFLOAT4{ 0.8f, 0.9f, 1.0f, 1.0f },
            DirectX::XMFLOAT2{ 320.0f, 96.0f },
            true,
            LamaPon::TextHorizontalAlignment::Center,
            LamaPon::TextVerticalAlignment::Bottom);

        // cameraObject: 保存元sceneのcamera GameObject。
        auto& cameraObject = source.CreateGameObject("カメラ");
        cameraObject.SetParent(&root);
        // camera: camera component。
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        cameraObject.AddComponent<
            LamaPon::AudioListenerComponent>();
        source.SetMainCamera(camera);

        // outputPath: scene serialization file path。
        const auto outputPath =
            std::filesystem::current_path()
            / "test-output"
            / std::string(suite)
            / "roundtrip.scene.json";
        source.SaveToFile(outputPath);
        // prefabPath: Prefab asset file path。
        const auto prefabPath =
            outputPath.parent_path()
            / std::filesystem::path(
                L"日本語階層.prefab.json");
        source.SavePrefab(root, prefabPath);

        // prefabTarget: Prefab instanceの編集scene。
        LamaPon::Scene prefabTarget(graphics);
        // prefabParent: test sceneのprefab GameObject。
        auto& prefabParent =
            prefabTarget.CreateGameObject("PrefabParent");
        // prefabInstance: test fixtureのprefab instance。
        auto& prefabInstance =
            prefabTarget.InstantiatePrefab(
                prefabPath,
                &prefabParent);
        Require(
            prefabInstance.Name() == "ルート",
            "Prefab root name was changed.");
        Require(
            prefabInstance.Parent() == &prefabParent,
            "Prefab target parent was not applied.");
        Require(
            prefabInstance.Children().size() == 2
                && prefabTarget.GameObjects().size() == 4,
            "Prefab hierarchy was not instantiated.");
        Require(
            prefabInstance.GetComponent<
                LamaPon::DirectionalLightComponent>()
                != nullptr,
            "Prefab root components were not restored.");
        // prefabInputMoverFound: Prefab InputMover検出結果。
        bool prefabInputMoverFound = false;
        // prefabJointConnected: Prefab joint参照の接続状態。
        bool prefabJointConnected = false;
        // prefabLODConnected: Prefab LOD参照の接続状態。
        bool prefabLODConnected = false;
        // prefab instance内のchild componentを走査します。
        for (const auto* prefabChild :
            prefabInstance.Children())
        {
            // prefab childのInputMover設定を検証します。
            if (const auto* mover =
                prefabChild->GetComponent<
                    LamaPon::InputMoverComponent>())
            {
                prefabInputMoverFound =
                    mover->HorizontalAction()
                        == "MoveHorizontal"
                    && NearlyEqual(mover->Speed(), 4.5f);
            }
            // prefab childのInputMover設定を検証します。
            if (const auto* prefabJoint =
                    prefabChild->GetComponent<
                        LamaPon::JointComponent>())
            {
                prefabJointConnected =
                    prefabJoint->ConnectedBodyId()
                        == prefabInstance.Id();
            }
        }
        // prefab LODのtarget参照を検証します。
        if (const auto* prefabLOD =
                prefabInstance.GetComponent<
                    LamaPon::LODGroupComponent>())
        {
            prefabLODConnected =
                prefabLOD->Levels().size() == 1
                && std::ranges::any_of(
                    prefabInstance.Children(),
                    [prefabLOD](
                        const auto* childObject)
                    {
                        // child objectをLOD targetが参照するか返します。
                        return prefabLOD->Levels().
                            front().targetId
                            == childObject->Id();
                    });
        }
        Require(
            prefabInputMoverFound,
            "Prefab child components were not restored.");
        Require(
            prefabJointConnected,
            "Prefab Joint target id was not remapped.");
        Require(
            prefabLODConnected,
            "Prefab LOD target id was not remapped.");
        Require(
            prefabInstance.IsPrefabInstanceRoot()
                && prefabInstance.PrefabAssetPath()
                    == prefabPath.lexically_normal(),
            "Prefab source link was not stored.");
        Require(
            prefabTarget.FindPrefabInstanceRoot(
                *prefabInstance.Children().front())
                == &prefabInstance,
            "Prefab root was not found from a child.");
        // initialPrefabOverrides: 初期状態のPrefab override列。
        const auto initialPrefabOverrides =
            prefabTarget.GetPrefabOverrides(
                prefabInstance);
        // 初期overrideが残る場合は差分を表示します。
        if (!initialPrefabOverrides.empty())
        {
            std::cerr
                << "Unexpected initial Prefab override: "
                << initialPrefabOverrides.front().path
                << " source="
                << initialPrefabOverrides.front().
                    sourceValue
                << " instance="
                << initialPrefabOverrides.front().
                    instanceValue
                << '\n';
        }
        Require(
            initialPrefabOverrides.empty(),
            "A new Prefab instance unexpectedly has overrides.");

        prefabInstance.SetName("項目適用済みルート");
        Require(
            prefabTarget.HasPrefabOverrides(
                prefabInstance),
            "Prefab override was not detected.");
        // nameOverrides: name変更のPrefab override列。
        const auto nameOverrides =
            prefabTarget.GetPrefabOverrides(
                prefabInstance);
        // nameOverridePath: name overrideのJSON path。
        std::string nameOverridePath;
        // prefab overrideから対象pathを探します。
        for (const auto& overrideValue :
            nameOverrides)
        {
            // 対象pathの個別適用可能なoverrideを探します。
            if (overrideValue.path
                    == "/objects/0/name"
                && overrideValue.
                    canApplyIndividually)
            {
                nameOverridePath =
                    overrideValue.path;
            }
        }
        Require(
            !nameOverridePath.empty(),
            "The name override was not reported as an editable property.");
        prefabTarget.ApplyPrefabOverride(
            prefabInstance,
            nameOverridePath);
        Require(
            !prefabTarget.HasPrefabOverrides(
                prefabInstance),
            "Individually applied property still reports an override.");

        // appliedPositionX: 適用後のX座標。
        const float appliedPositionX =
            prefabInstance.GetTransform().position.x;
        prefabInstance.GetTransform().position.x += 5.0f;
        // transformOverrides: transform変更のPrefab override列。
        const auto transformOverrides =
            prefabTarget.GetPrefabOverrides(
                prefabInstance);
        // positionOverridePath: position overrideのJSON path。
        std::string positionOverridePath;
        // prefab overrideから対象pathを探します。
        for (const auto& overrideValue :
            transformOverrides)
        {
            // 対象pathの個別適用可能なoverrideを探します。
            if (overrideValue.path
                    == "/objects/0/transform/position"
                && overrideValue.
                    canApplyIndividually)
            {
                positionOverridePath =
                    overrideValue.path;
            }
        }
        Require(
            !positionOverridePath.empty(),
            "The position override was not reported as an editable property.");
        // propertyRevertedPrefab: property revert後のprefab instance。
        auto& propertyRevertedPrefab =
            prefabTarget.RevertPrefabOverride(
                prefabInstance,
                positionOverridePath);
        Require(
            propertyRevertedPrefab.Name()
                    == "項目適用済みルート"
                && NearlyEqual(
                    propertyRevertedPrefab.
                        GetTransform().position.x,
                    appliedPositionX)
                && !prefabTarget.HasPrefabOverrides(
                    propertyRevertedPrefab),
            "Individual Prefab Revert did not restore the property.");

        propertyRevertedPrefab.SetName(
            "適用済みルート");
        prefabTarget.ApplyPrefabInstance(
            propertyRevertedPrefab);
        Require(
            !prefabTarget.HasPrefabOverrides(
                propertyRevertedPrefab),
            "Applied Prefab still reports overrides.");
        propertyRevertedPrefab.
            GetTransform().position.x += 5.0f;
        // replacedPrefabId: 置換後Prefab asset ID。
        const auto replacedPrefabId =
            propertyRevertedPrefab.Id();
        // revertedPrefab: revert後のprefab instance。
        auto& revertedPrefab =
            prefabTarget.RevertPrefabInstance(
                propertyRevertedPrefab);
        Require(
            revertedPrefab.Id() != replacedPrefabId
                && revertedPrefab.Name()
                    == "適用済みルート"
                && NearlyEqual(
                    revertedPrefab.GetTransform().position.x,
                    appliedPositionX)
                && revertedPrefab.Parent()
                    == &prefabParent
                && revertedPrefab.IsPrefabInstanceRoot()
                && !prefabTarget.HasPrefabOverrides(
                    revertedPrefab),
            "Prefab Revert did not restore the applied hierarchy.");

        // prefabRoundTrip: Prefab JSONの再読込scene。
        LamaPon::Scene prefabRoundTrip(graphics);
        prefabRoundTrip.LoadFromJson(
            prefabTarget.SerializeToJson());
        // prefabLinkRestored: Prefab instance link復元結果。
        bool prefabLinkRestored = false;
        // scene内の各objectを確認します。
        for (const auto& object :
            prefabRoundTrip.GameObjects())
        {
            // 指定名のobjectについて復元状態を確認します。
            if (object->Name()
                    == "適用済みルート")
            {
                prefabLinkRestored =
                    object->IsPrefabInstanceRoot()
                    && object->PrefabAssetPath()
                        == prefabPath.lexically_normal();
            }
        }
        Require(
            prefabLinkRestored,
            "Prefab link did not survive Scene serialization.");

        // objectsBeforeBrokenRevert: revert前のobject数。
        const auto objectsBeforeBrokenRevert =
            prefabTarget.GameObjects().size();
        {
            // brokenPrefab: 壊れたPrefab fixture文書。
            std::ofstream brokenPrefab(
                prefabPath,
                std::ios::binary
                    | std::ios::trunc);
            brokenPrefab << R"({"format":"Broken"})";
        }
        // brokenRevertRejected: 壊れたPrefab revertの拒否結果。
        bool brokenRevertRejected = false;
        // broken prefab文書からのrevertを拒否します。
        try
        {
            static_cast<void>(
                prefabTarget.RevertPrefabInstance(
                    revertedPrefab));
        }
        // 拒否時の例外をテスト結果へ記録します。
        catch (const std::exception&)
        {
            brokenRevertRejected = true;
        }
        Require(
            brokenRevertRejected
                && prefabTarget.GameObjects().size()
                    == objectsBeforeBrokenRevert
                && prefabTarget.FindGameObject(
                    revertedPrefab.Id())
                    == &revertedPrefab,
            "A broken linked Prefab damaged its instance during Revert.");
        prefabTarget.ApplyPrefabInstance(
            revertedPrefab);

        // nestedPrefabPath: nested Prefabのfile path。
        const auto nestedPrefabPath =
            outputPath.parent_path()
            / std::filesystem::path(
                L"ネスト.prefab.json");
        // nestedSource: nested Prefab source scene。
        LamaPon::Scene nestedSource(graphics);
        // nestedSourceRoot: nested prefabのroot GameObject。
        auto& nestedSourceRoot =
            nestedSource.CreateGameObject(
                "ネストされたPrefab");
        nestedSourceRoot.GetTransform().scale = {
            0.5f,
            0.5f,
            0.5f
        };
        nestedSource.SavePrefab(
            nestedSourceRoot,
            nestedPrefabPath);
        // nestedInstance: prefabTargetに生成したnested prefab root。
        auto& nestedInstance =
            prefabTarget.InstantiatePrefab(
                nestedPrefabPath,
                &revertedPrefab);
        Require(
            prefabTarget.FindPrefabInstanceRoot(
                nestedInstance)
                == &nestedInstance,
            "The nearest Nested Prefab root was not selected.");

        // structuralOverrides: structural Prefab override列。
        const auto structuralOverrides =
            prefabTarget.GetPrefabOverrides(
                revertedPrefab);
        // structuralOverrideFound: structural override検出結果。
        bool structuralOverrideFound = false;
        // prefab overrideから対象pathを探します。
        for (const auto& overrideValue :
            structuralOverrides)
        {
            // 対象pathの個別適用可能なoverrideを探します。
            if ((overrideValue.path
                        == "/objects"
                    || overrideValue.path.starts_with(
                        "/objects/"))
                && !overrideValue.
                    canApplyIndividually)
            {
                structuralOverrideFound = true;
            }
        }
        Require(
            structuralOverrideFound,
            "A hierarchy override was not reported as structural.");
        prefabTarget.ApplyPrefabInstance(
            revertedPrefab);

        // nestedPrefabTarget: nested instanceのload先scene。
        LamaPon::Scene nestedPrefabTarget(graphics);
        // restoredOuterPrefab: 読み込み後のprefab instance。
        auto& restoredOuterPrefab =
            nestedPrefabTarget.InstantiatePrefab(
                prefabPath);
        // nestedLinkRestored: nested Prefab link復元結果。
        bool nestedLinkRestored = false;
        // scene内の各objectを確認します。
        for (const auto& object :
            nestedPrefabTarget.GameObjects())
        {
            // 指定名のobjectについて復元状態を確認します。
            if (object->Name()
                    == "ネストされたPrefab")
            {
                nestedLinkRestored =
                    object->IsPrefabInstanceRoot()
                    && object->PrefabAssetPath()
                        == nestedPrefabPath.
                            lexically_normal()
                    && object->Parent()
                        == &restoredOuterPrefab;
            }
        }
        Require(
            nestedLinkRestored,
            "Nested Prefab link was not restored from its parent Prefab.");

        // prefabObjectCount: Prefab内object数。
        const auto prefabObjectCount =
            prefabTarget.GameObjects().size();
        // invalidPrefabRejected: invalid Prefab拒否結果。
        bool invalidPrefabRejected = false;
        // invalid prefab rootのinstance化を拒否します。
        try
        {
            static_cast<void>(
                prefabTarget.InstantiatePrefabFromJson(
                    R"({
                        "format":"LamaPonPrefab",
                        "version":1,
                        "root":1,
                        "objects":[{
                            "id":1,
                            "name":"Broken",
                            "parent":99,
                            "enabled":true,
                            "transform":{
                                "position":[0,0,0],
                                "rotation":[0,0,0],
                                "scale":[1,1,1]
                            },
                            "components":[]
                        }]
                    })"));
        }
        // 拒否時の例外をテスト結果へ記録します。
        catch (const std::exception&)
        {
            invalidPrefabRejected = true;
        }
        Require(
            invalidPrefabRejected
                && prefabTarget.GameObjects().size()
                    == prefabObjectCount,
            "Invalid prefab changed the destination Scene.");

        // loaded: 復元後のscene。
        LamaPon::Scene loaded(graphics);
        loaded.LoadFromJson(source.SerializeToJson());

        Require(loaded.GameObjects().size() == 3, "Unexpected GameObject count.");
        Require(loaded.MainCamera() != nullptr, "Main camera was not restored.");
        Require(
            NearlyEqual(loaded.AmbientLightColor().x, 0.25f)
                && NearlyEqual(loaded.AmbientLightColor().y, 0.35f)
                && NearlyEqual(loaded.AmbientLightColor().z, 0.45f),
            "Ambient light color was not restored.");
        Require(
            NearlyEqual(loaded.AmbientLightIntensity(), 0.6f),
            "Ambient light intensity was not restored.");
        Require(
            loaded.Sky().enabled
                && NearlyEqual(
                    loaded.Sky().topColor.z,
                    0.3f)
                && NearlyEqual(
                    loaded.Sky().intensity,
                    1.4f),
            "Sky settings were not restored.");
        Require(
            loaded.Fog().enabled
                && NearlyEqual(
                    loaded.Fog().endDistance,
                    42.0f)
                && NearlyEqual(
                    loaded.Fog().density,
                    0.025f),
            "Fog settings were not restored.");
        Require(
            loaded.Bloom().enabled
                && NearlyEqual(
                    loaded.Bloom().threshold,
                    0.8f)
                && NearlyEqual(
                    loaded.Bloom().intensity,
                    0.65f)
                && NearlyEqual(
                    loaded.Bloom().radius,
                    3.0f),
            "Bloom settings were not restored.");
        Require(
            loaded.ScreenOutline().enabled
                && NearlyEqual(
                    loaded.ScreenOutline().color.x,
                    0.1f)
                && NearlyEqual(
                    loaded.ScreenOutline().color.y,
                    0.2f)
                && NearlyEqual(
                    loaded.ScreenOutline().color.z,
                    0.3f)
                && NearlyEqual(
                    loaded.ScreenOutline().intensity,
                    0.8f)
                && NearlyEqual(
                    loaded.ScreenOutline().thickness,
                    2.5f)
                && NearlyEqual(
                    loaded.ScreenOutline().depthThreshold,
                    0.04f)
                && NearlyEqual(
                    loaded.ScreenOutline().normalThreshold,
                    0.35f),
            "Screen outline settings were not restored.");
        Require(
            loaded.ScreenSpaceLensFlare().enabled
                && NearlyEqual(
                    loaded.ScreenSpaceLensFlare().threshold,
                    1.9f)
                && NearlyEqual(
                    loaded.ScreenSpaceLensFlare().intensity,
                    0.55f)
                && NearlyEqual(
                    loaded.ScreenSpaceLensFlare().ghostDispersal,
                    0.42f)
                && NearlyEqual(
                    loaded.ScreenSpaceLensFlare().haloWidth,
                    0.31f)
                && NearlyEqual(
                    loaded.ScreenSpaceLensFlare()
                        .chromaticAberration,
                    0.12f)
                && NearlyEqual(
                    loaded.ScreenSpaceLensFlare().streakIntensity,
                    0.27f)
                && NearlyEqual(
                    loaded.ScreenSpaceLensFlare().streakLength,
                    0.36f),
            "Screen space lens flare settings were not restored.");
        Require(
            loaded.DepthOfField().enabled
                && NearlyEqual(
                    loaded.DepthOfField().focusDistance,
                    7.5f)
                && NearlyEqual(
                    loaded.DepthOfField().focusRange,
                    1.25f)
                && NearlyEqual(
                    loaded.DepthOfField().blurStrength,
                    1.6f)
                && NearlyEqual(
                    loaded.DepthOfField().maximumRadius,
                    14.0f),
            "Depth of field settings were not restored.");
        Require(
            loaded.MotionBlur().enabled
                && NearlyEqual(
                    loaded.MotionBlur().intensity,
                    0.8f)
                && NearlyEqual(
                    loaded.MotionBlur().maximumRadius,
                    22.0f),
            "Motion blur settings were not restored.");
        Require(
            loaded.AutoExposure().enabled
                && NearlyEqual(
                    loaded.AutoExposure().keyValue,
                    0.24f)
                && NearlyEqual(
                    loaded.AutoExposure().minimumLuminance,
                    0.05f)
                && NearlyEqual(
                    loaded.AutoExposure().maximumLuminance,
                    6.5f)
                && NearlyEqual(
                    loaded.AutoExposure().speedToBright,
                    2.5f)
                && NearlyEqual(
                    loaded.AutoExposure().speedToDark,
                    0.75f),
            "Auto exposure settings were not restored.");
        Require(
            !loaded.ColorGrading().toneMappingEnabled
                && loaded.ColorGrading().enabled
                && NearlyEqual(
                    loaded.ColorGrading().exposure,
                    0.25f)
                && NearlyEqual(
                    loaded.ColorGrading().saturation,
                    1.2f),
            "Color grading settings were not restored.");
        Require(
            NearlyEqual(
                loaded.PhysicsBroadPhaseCellSize(),
                3.5f),
            "Physics broad-phase cell size was not restored.");
        Require(
            !loaded.FrustumCullingEnabled()
                && loaded.OcclusionCullingEnabled(),
            "Scene culling settings were not restored.");

        // loadedRoot: 読み込み後のroot GameObject。
        auto* loadedRoot = loaded.FindGameObject(root.Id());
        // loadedChild: 読み込み後のchild GameObject。
        auto* loadedChild = loaded.FindGameObject(child.Id());
        // loadedCameraObject: 読み込み後のcamera GameObject。
        auto* loadedCameraObject =
            loaded.FindGameObject(cameraObject.Id());
        Require(loadedRoot != nullptr, "Root was not restored.");
        Require(
            loadedRoot->IsAlwaysVisible()
                && NearlyEqual(
                    loadedRoot->CullingMargin(),
                    12.5f),
            "GameObject culling settings were not restored.");
        Require(
            loadedRoot->GetComponent<
                LamaPon::RenderCullingComponent>()
                != nullptr,
            "Culling settings must round-trip as a"
            " component.");
        // loadedProbe: 読み込み後のtest probe component。
        const auto* loadedProbe =
            loadedRoot->GetComponent<
                LamaPon::ReflectionProbeComponent>();
        Require(
            loadedProbe != nullptr
                && NearlyEqual(loadedProbe->Range(), 7.5f)
                && NearlyEqual(
                    loadedProbe->Intensity(), 1.25f),
            "Reflection probe settings did not"
            " round-trip.");
        // ボックス射影の3軸と有効状態が保存後も保たれること。
        Require(
            loadedProbe != nullptr
                && NearlyEqual(
                    loadedProbe->BoxExtents().x, 4.0f)
                && NearlyEqual(
                    loadedProbe->BoxExtents().y, 2.5f)
                && NearlyEqual(
                    loadedProbe->BoxExtents().z, 3.5f)
                && loadedProbe->UsesBoxProjection(),
            "Reflection probe box projection extents did not"
            " round-trip.");
        // ブレンド距離が0へ戻らず、境界で映り込みが急変しないこと。
        Require(
            loadedProbe != nullptr
                && NearlyEqual(
                    loadedProbe->BlendDistance(), 2.75f),
            "Reflection probe blend distance did not"
            " round-trip.");

        // 光の筋（ボリュメトリック）の設定もシーンへ保存されます。
        // 全項目へ既定値と異なる値を設定し、保存漏れを検出します。
        {
            // volumetricScene: volumetric serialization scene。
            LamaPon::Scene volumetricScene(graphics);
            // volumetric: volumetric light設定。
            LamaPon::VolumetricLightSettings volumetric{};
            volumetric.enabled = true;
            volumetric.intensity = 0.85f;
            volumetric.sampleCount = 24;
            volumetric.maximumDistance = 123.0f;
            volumetric.scattering = 0.42f;
            volumetricScene.SetVolumetricLightSettings(
                volumetric);

            // volumetricLoaded: JSONから復元したvolumetric scene。
            LamaPon::Scene volumetricLoaded(graphics);
            volumetricLoaded.LoadFromJson(
                volumetricScene.SerializeToJson());
            // restored: 読み込み後のscene state。
            const auto& restored =
                volumetricLoaded.VolumetricLight();
            Require(
                restored.enabled
                    && NearlyEqual(
                        restored.intensity, 0.85f)
                    && restored.sampleCount == 24u
                    && NearlyEqual(
                        restored.maximumDistance, 123.0f)
                    && NearlyEqual(
                        restored.scattering, 0.42f),
                "Volumetric light settings did not"
                " round-trip.");
        }

        // 旧形式（オブジェクト直下のalwaysVisible/cullingMargin）は読み込み時にRenderCullingコンポーネントへ変換されます。
        // 既存プロジェクトのシーンを開けなくしないための互換です。
        {
            // legacyScene: legacy serialization検証scene。
            LamaPon::Scene legacyScene(graphics);
            legacyScene.LoadFromJson(R"({
                "format": "LamaPonScene",
                "objects": [
                    {
                        "id": 1,
                        "name": "LegacyCulling",
                        "alwaysVisible": true,
                        "cullingMargin": 3.5,
                        "transform": {
                            "position": [0, 0, 0],
                            "rotation": [0, 0, 0],
                            "scale": [1, 1, 1]
                        }
                    },
                    {
                        "id": 2,
                        "name": "LegacyDefault",
                        "transform": {
                            "position": [0, 0, 0],
                            "rotation": [0, 0, 0],
                            "scale": [1, 1, 1]
                        }
                    },
                    {
                        "id": 3,
                        "name": "LegacyRotated",
                        "transform": {
                            "position": [0, 0, 0],
                            "rotation": [0.5, -1.25, 0.75],
                            "scale": [1, 1, 1]
                        }
                    }
                ]
            })");
            // legacyObject: 旧schemaのGameObject。
            const auto* legacyObject =
                legacyScene.FindGameObjectByName(
                    "LegacyCulling");
            Require(
                legacyObject != nullptr,
                "Legacy object was not loaded.");
            // legacyCulling: 旧schemaのculling settings。
            const auto* legacyCulling =
                legacyObject->GetComponent<
                    LamaPon::RenderCullingComponent>();
            Require(
                legacyCulling != nullptr
                    && legacyCulling->AlwaysVisible()
                    && NearlyEqual(
                        legacyCulling->CullingMargin(),
                        3.5f),
                "Legacy culling keys must migrate into a"
                " RenderCulling component.");
            // rotationQuaternionを持たない旧シーンでも、オイラー角から回転を復元できること。
            const auto* legacyRotated =
                legacyScene.FindGameObjectByName(
                    "LegacyRotated");
            Require(
                legacyRotated != nullptr,
                "Legacy rotated object was not loaded.");
            {
                // restored: 復元処理の結果。
                const auto restored =
                    legacyRotated->GetTransform()
                        .EulerAngles();
                Require(
                    NearlyEqual(restored.x, 0.5f)
                        && NearlyEqual(
                            restored.y, -1.25f)
                        && NearlyEqual(
                            restored.z, 0.75f),
                    "Euler-only scenes must restore their"
                    " rotation through the quaternion.");
            }

            // legacyDefault: 旧schemaのGameObject。
            const auto* legacyDefault =
                legacyScene.FindGameObjectByName(
                    "LegacyDefault");
            Require(
                legacyDefault != nullptr
                    && legacyDefault->GetComponent<
                            LamaPon::
                                RenderCullingComponent>()
                        == nullptr,
                "Objects with default culling must not"
                " grow a component on load.");
        }
        Require(loadedChild != nullptr, "Child was not restored.");
        Require(
            loadedCameraObject != nullptr,
            "Camera GameObject was not restored.");
        // loadedLOD: 読み込み後のLOD settings。
        const auto* loadedLOD =
            loadedRoot->GetComponent<
                LamaPon::LODGroupComponent>();
        Require(
            loadedLOD != nullptr
                && loadedLOD->Levels().size() == 1
                && loadedLOD->Levels().front().
                    targetId == loadedChild->Id()
                && NearlyEqual(
                    loadedLOD->CullDistance(),
                    80.0f),
            "LOD Group was not restored.");
        // loadedLight: 読み込み後のlight component。
        const auto* loadedLight =
            loadedRoot->GetComponent<
                LamaPon::DirectionalLightComponent>();
        Require(
            loadedLight != nullptr,
            "DirectionalLight was not restored.");
        Require(
            NearlyEqual(loadedLight->Color().x, 1.0f)
                && NearlyEqual(loadedLight->Color().y, 0.8f)
                && NearlyEqual(loadedLight->Color().z, 0.6f),
            "DirectionalLight color was not restored.");
        Require(
            NearlyEqual(loadedLight->Intensity(), 2.25f),
            "DirectionalLight intensity was not restored.");
        Require(
            loadedLight->CastsShadows()
                && NearlyEqual(
                    loadedLight->ShadowDistance(),
                    32.0f)
                && NearlyEqual(
                    loadedLight->ShadowBias(),
                    0.002f)
                && NearlyEqual(
                    loadedLight->ShadowNormalBias(),
                    0.004f)
                && NearlyEqual(
                    loadedLight->ShadowStrength(),
                    0.7f)
                && loadedLight->ShadowCascadeCount() == 3u
                && NearlyEqual(
                    loadedLight->ShadowSplitLambda(),
                    0.72f),
            "DirectionalLight shadow settings were not restored.");
        // loadedPointLight: 読み込み後のlight component。
        const auto* loadedPointLight =
            loadedRoot->GetComponent<
                LamaPon::PointLightComponent>();
        Require(
            loadedPointLight != nullptr,
            "PointLight was not restored.");
        Require(
            NearlyEqual(loadedPointLight->Intensity(), 4.5f)
                && NearlyEqual(loadedPointLight->Range(), 9.0f),
            "PointLight settings were not restored.");
        // loadedSpotLight: 読み込み後のlight component。
        const auto* loadedSpotLight =
            loadedRoot->GetComponent<
                LamaPon::SpotLightComponent>();
        Require(
            loadedSpotLight != nullptr,
            "SpotLight was not restored.");
        Require(
            NearlyEqual(loadedSpotLight->Intensity(), 6.0f)
                && NearlyEqual(loadedSpotLight->Range(), 14.0f)
                && NearlyEqual(
                    loadedSpotLight->InnerConeAngle(),
                    DirectX::XMConvertToRadians(18.0f))
                && NearlyEqual(
                    loadedSpotLight->OuterConeAngle(),
                    DirectX::XMConvertToRadians(32.0f)),
            "SpotLight settings were not restored.");
        // loadedNavMesh: 読み込み後のNavMesh component。
        const auto* loadedNavMesh =
            loadedRoot->GetComponent<
                LamaPon::NavMeshComponent>();
        Require(
            loadedNavMesh != nullptr
                && loadedNavMesh->IsBaked()
                && loadedNavMesh->
                    GridWidth() == 6
                && loadedNavMesh->
                    GridDepth() == 6
                && loadedNavMesh->
                    IsBlocked(2, 2)
                && loadedNavMesh->
                    BlockedCellCount() == 3,
            "NavMesh bake data was not restored.");
        // loadedCanvas: 読み込み後のUI canvas component。
        const auto* loadedCanvas =
            loadedRoot->GetComponent<
                LamaPon::UICanvasComponent>();
        Require(
            loadedCanvas != nullptr
                && NearlyEqual(
                    loadedCanvas->
                        ReferenceResolution().x,
                    1920.0f)
                && NearlyEqual(
                    loadedCanvas->
                        MatchWidthOrHeight(),
                    0.75f),
            "UICanvas settings were not restored.");
        Require(loadedChild->Parent() == loadedRoot, "Parent relationship was not restored.");
        Require(loadedRoot->Children().size() == 2, "Child list was not restored.");

        // childWorld: child world transform matrix。
        DirectX::XMFLOAT4X4 childWorld{};
        DirectX::XMStoreFloat4x4(&childWorld, loadedChild->WorldMatrix());
        Require(NearlyEqual(childWorld._41, 12.0f), "World X position is incorrect.");
        Require(NearlyEqual(childWorld._42, 5.0f), "World Y position is incorrect.");

        // sprite: sprite renderer component。
        const auto* sprite = loadedChild->GetComponent<LamaPon::SpriteRendererComponent>();
        Require(sprite != nullptr, "SpriteRenderer was not restored.");
        Require(
            sprite->TexturePath() == std::filesystem::path(L"textures/日本語画像.png"),
            "Texture path was not restored.");
        Require(
            NearlyEqual(
                sprite->SourceRect().x,
                0.25f)
                && NearlyEqual(
                    sprite->SourceRect().y,
                    0.125f)
                && NearlyEqual(
                    sprite->SourceRect().z,
                    0.5f)
                && NearlyEqual(
                    sprite->SourceRect().w,
                    0.375f),
            "Sprite source rectangle was not restored.");
        Require(
            sprite->ShaderPath()
                == std::filesystem::path(
                    L"shaders/日本語UI.hlsl")
                && NearlyEqual(
                    sprite->CustomParameter(0).x,
                    1.25f)
                && NearlyEqual(
                    sprite->CustomParameter(0).w,
                    5.0f),
            "Sprite shader settings were not restored.");
        // loadedTilemap: 読み込み後のtilemap component。
        const auto* loadedTilemap =
            loadedChild->GetComponent<
                LamaPon::TilemapComponent>();
        Require(
            loadedTilemap != nullptr,
            "Tilemap was not restored.");
        Require(
            NearlyEqual(
                loadedTilemap->TileSize().x,
                24.0f)
                && NearlyEqual(
                    loadedTilemap->TileSize().y,
                    16.0f)
                && loadedTilemap->
                    AtlasColumns() == 4
                && loadedTilemap->
                    AtlasRows() == 2,
            "Tilemap atlas settings were not restored.");
        Require(
            loadedTilemap->TexturePath()
                == std::filesystem::path(
                    L"textures/日本語タイル.png")
                && loadedTilemap->
                    TileAt(-1, 2) == 5
                && loadedTilemap->
                    TileAt(3, 4) == 7
                && loadedTilemap->
                    Cells().size() == 2,
            "Tilemap cells were not restored.");
        // loadedParticles: 読み込み後のparticle system component。
        const auto* loadedParticles =
            loadedChild->GetComponent<
                LamaPon::ParticleSystemComponent>();
        Require(
            loadedParticles != nullptr,
            "ParticleSystem was not restored.");
        Require(
            loadedParticles->MaxParticles() == 128
                && NearlyEqual(
                    loadedParticles->EmissionRate(),
                    24.0f)
                && loadedParticles->EmitterShape()
                    == LamaPon::ParticleEmitterShape::Sphere
                && loadedParticles->RenderMode()
                    == LamaPon::ParticleRenderMode::Horizontal
                && NearlyEqual(
                    loadedParticles->EndSizeMultiplier(),
                    0.15f)
                && NearlyEqual(
                    loadedParticles->Gravity().y,
                    -2.5f)
                && NearlyEqual(
                    loadedParticles->EmitterSize().z,
                    3.0f)
                && !loadedParticles->Looping()
                && !loadedParticles->PlayOnStart()
                && !loadedParticles->PreviewInEditor()
                && !loadedParticles->Additive()
                && loadedParticles->TexturePath()
                    == std::filesystem::path(
                        L"textures/日本語粒子.png"),
            "ParticleSystem settings were not restored.");
        Require(
            loadedParticles->ShaderPath()
                == std::filesystem::path(
                    L"shaders/ParticleCustom.hlsl")
                && loadedParticles->
                    AuxiliaryTexturePath()
                    == std::filesystem::path(
                        L"textures/ParticleMask.png")
                && NearlyEqual(
                    loadedParticles->
                        CustomParameter(0).w,
                    4.0f),
            "ParticleSystem shader settings were not restored.");
        // loadedUITransform: 読み込み後のTransform state。
        const auto* loadedUITransform =
            loadedChild->GetComponent<
                LamaPon::
                    UIRectTransformComponent>();
        Require(
            loadedUITransform != nullptr,
            "UIRectTransform was not restored.");
        // resolvedUIRect: 解決済みUI rect。
        const auto resolvedUIRect =
            loadedUITransform->Resolve(
                1280.0f,
                720.0f);
        Require(
            resolvedUIRect.maximum.x
                <= 1280.0f
                && resolvedUIRect.maximum.y
                    <= 720.0f
                && resolvedUIRect.minimum.x
                    < resolvedUIRect.maximum.x
                && resolvedUIRect.minimum.y
                    < resolvedUIRect.maximum.y,
            "UIRectTransform did not resolve inside the viewport.");
        // loadedButton: 読み込み後のUI button component。
        const auto* loadedButton =
            loadedChild->GetComponent<
                LamaPon::UIButtonComponent>();
        Require(
            loadedButton != nullptr
                && loadedButton->Label()
                    == "ゲーム開始"
                && NearlyEqual(
                    loadedButton->FontSize(),
                    30.0f)
                && NearlyEqual(
                    loadedButton->
                        HoveredColor().z,
                    0.9f)
                && loadedButton->TexturePath()
                    == std::filesystem::path(
                        L"textures/UIボタン.png")
                && loadedButton->TargetScene()
                    == std::filesystem::path(
                        L"scenes/次のシーン.scene.json")
                && !loadedButton->
                    ReloadCurrentScene(),
            "UIButton settings were not restored.");
        // loadedNavAgent: 読み込み後のNavMesh agent component。
        const auto* loadedNavAgent =
            loadedChild->GetComponent<
                LamaPon::
                    NavMeshAgentComponent>();
        Require(
            loadedNavAgent != nullptr
                && NearlyEqual(
                    loadedNavAgent->Speed(),
                    4.25f)
                && NearlyEqual(
                    loadedNavAgent->
                        StoppingDistance(),
                    0.2f)
                && loadedNavAgent->
                    Path().size() == 3
                && NearlyEqual(
                    loadedNavAgent->
                        Destination().z,
                    2.0f),
            "NavMeshAgent path was not restored.");
        // audio: audio source component。
        const auto* audio =
            loadedChild->GetComponent<LamaPon::AudioSourceComponent>();
        Require(audio != nullptr, "AudioSource was not restored.");
        Require(
            audio->AudioPath()
                == std::filesystem::path(L"audio/起動音.wav"),
            "Audio path was not restored.");
        Require(
            NearlyEqual(audio->Volume(), 0.65f)
                && NearlyEqual(audio->Pitch(), -0.2f)
                && NearlyEqual(audio->Pan(), 0.35f),
            "Audio properties were not restored.");
        Require(
            audio->Loop() && audio->PlayOnStart(),
            "Audio playback settings were not restored.");
        Require(
            audio->IsSpatial()
                && NearlyEqual(audio->MinimumDistance(), 2.5f)
                && NearlyEqual(audio->MaximumDistance(), 30.0f),
            "Audio spatial settings were not restored.");
        Require(
            loadedCameraObject->GetComponent<
                LamaPon::AudioListenerComponent>() != nullptr,
            "AudioListener was not restored.");
        // inputMover: 復元したInputMover component。
        const auto* inputMover =
            loadedChild->GetComponent<
                LamaPon::InputMoverComponent>();
        Require(
            inputMover != nullptr
                && inputMover->HorizontalAction()
                    == "MoveHorizontal"
                && inputMover->VerticalAction()
                    == "MoveVertical"
                && NearlyEqual(inputMover->Speed(), 4.5f),
            "InputMover was not restored.");
        // animator: test fixtureのanimator component。
        const auto* animator =
            loadedChild->GetComponent<
                LamaPon::TransformAnimatorComponent>();
        Require(
            animator != nullptr
                && animator->ClipPath()
                    == std::filesystem::path(
                        L"animations/浮遊.animation.json")
                && NearlyEqual(
                    animator->Speed(),
                    1.5f)
                && !animator->Loop()
                && animator->PlayOnStart()
                && animator->ControllerPath()
                    == std::filesystem::path(
                        L"animations/移動.animator.json"),
            "TransformAnimator was not restored.");
        // model: model renderer component。
        const auto* model = loadedChild->GetComponent<LamaPon::ModelRendererComponent>();
        Require(model != nullptr, "ModelRenderer was not restored.");
        Require(
            model->ModelPath() == std::filesystem::path(L"models/日本語モデル.cmo"),
            "Model path was not restored.");
        Require(model->IsWireframe(), "Model wireframe state was not restored.");
        Require(
            model->IsMaterialOverrideEnabled(),
            "Model material override state was not restored.");
        Require(
            model->AlbedoTexturePath()
                == std::filesystem::path(
                    L"textures/モデル色.png"),
            "Model albedo path was not restored.");
        Require(
            model->NormalTexturePath()
                == std::filesystem::path(
                    L"textures/モデル法線.png"),
            "Model normal path was not restored.");
        Require(
            NearlyEqual(model->Color().x, 0.7f)
                && NearlyEqual(model->Color().y, 0.4f)
                && NearlyEqual(model->Color().z, 0.2f)
                && NearlyEqual(model->Roughness(), 0.27f)
                && NearlyEqual(model->NormalStrength(), 1.2f),
            "Model material settings were not restored.");
        Require(
            model->AnimationControllerPath()
                == std::filesystem::path(
                    L"animations/モデル.animator.json"),
            "Model Animator Controller was not restored.");
        Require(
            model->ApplyRootMotion()
                && model->RootMotionNode()
                    == "Root",
            "Model Root Motion settings were not restored.");
        Require(
            model->MaterialAssetPath()
                == std::filesystem::path(
                    L"materials/共有.material.json"),
            "Model material asset path was not restored.");
        Require(
            model->AnimationIndex() == 2
                && NearlyEqual(
                    model->AnimationSpeed(),
                    0.75f)
                && !model->AnimationLoop()
                && !model->AnimationPlayOnStart(),
            "Model skeletal animation settings were not restored.");
        // mesh: mesh renderer component。
        const auto* mesh =
            loadedChild->GetComponent<
                LamaPon::MeshRendererComponent>();
        Require(mesh != nullptr, "MeshRenderer was not restored.");
        Require(
            mesh->Shape() == LamaPon::PrimitiveShape::Sphere,
            "MeshRenderer shape was not restored.");
        Require(
            mesh->AlbedoTexturePath()
                == std::filesystem::path(
                    L"textures/アルベド.png"),
            "MeshRenderer albedo path was not restored.");
        Require(
            mesh->NormalTexturePath()
                == std::filesystem::path(
                    L"textures/法線.png"),
            "MeshRenderer normal path was not restored.");
        Require(
            NearlyEqual(mesh->Roughness(), 0.32f)
                && NearlyEqual(mesh->NormalStrength(), 1.4f),
            "MeshRenderer material settings were not restored.");
        Require(
            mesh->MaterialAssetPath()
                == std::filesystem::path(
                    L"materials/共有.material.json"),
            "MeshRenderer material asset path was not restored.");

        // materialPath: material asset path。
        const auto materialPath =
            outputPath.parent_path()
            / "roundtrip.material.json";
        // sourceMaterial: serialization元のlit material。
        LamaPon::LitMaterial sourceMaterial{
            DirectX::XMFLOAT4{ 0.2f, 0.4f, 0.6f, 0.8f },
            std::filesystem::path(
                L"textures/日本語アルベド.png"),
            std::filesystem::path(
                L"textures/日本語法線.png"),
            0.23f,
            1.35f
        };
        sourceMaterial.SetShader(
            L"shaders/日本語カスタム.hlsl");
        sourceMaterial.SetCustomParameter(
            0,
            DirectX::XMFLOAT4{ 0.1f, 0.2f, 0.3f, 0.4f });
        sourceMaterial.SetCustomParameter(
            3,
            DirectX::XMFLOAT4{ 4.0f, 3.0f, 2.0f, 1.0f });
        LamaPon::SaveLitMaterialAsset(
            materialPath,
            sourceMaterial);
        // loadedMaterial: JSONから復元したmaterial。
        const auto loadedMaterial =
            LamaPon::LoadLitMaterialAsset(materialPath);
        Require(
            NearlyEqual(
                loadedMaterial.BaseColor().x,
                0.2f)
                && NearlyEqual(
                    loadedMaterial.BaseColor().w,
                    0.8f)
                && NearlyEqual(
                    loadedMaterial.Roughness(),
                    0.23f)
                && NearlyEqual(
                    loadedMaterial.NormalStrength(),
                    1.35f),
            "LitMaterial values were not restored.");
        Require(
            loadedMaterial.AlbedoTexture()
                == std::filesystem::path(
                    L"textures/日本語アルベド.png")
                && loadedMaterial.NormalTexture()
                    == std::filesystem::path(
                        L"textures/日本語法線.png"),
            "LitMaterial texture paths were not restored.");
        Require(
            loadedMaterial.Shader()
                == std::filesystem::path(
                    L"shaders/日本語カスタム.hlsl")
                && NearlyEqual(
                    loadedMaterial.CustomParameter(0).z,
                    0.3f)
                && NearlyEqual(
                    loadedMaterial.CustomParameter(3).x,
                    4.0f),
            "LitMaterial shader settings were not restored.");
        // collider: test fixtureのcollider component。
        const auto* collider =
            loadedChild->GetComponent<LamaPon::BoxCollider3DComponent>();
        Require(collider != nullptr, "BoxCollider3D was not restored.");
        Require(collider->IsTrigger(), "Collider trigger state was not restored.");
        Require(collider->Layer() == 3, "Collider layer was not restored.");
        Require(collider->CollisionMask() == 0x10u, "Collider mask was not restored.");
        Require(
            NearlyEqual(collider->Material().friction, 1.25f)
                && NearlyEqual(
                    collider->Material().restitution,
                    0.65f),
            "BoxCollider3D physics material was not restored.");
        // capsule: test fixtureのcapsule collider。
        const auto* capsule =
            loadedChild->GetComponent<
                LamaPon::CapsuleCollider3DComponent>();
        Require(
            capsule != nullptr
                && NearlyEqual(capsule->Radius(), 0.45f)
                && NearlyEqual(capsule->Height(), 2.4f)
                && capsule->Layer() == 4
                && capsule->CollisionMask() == 0x20u
                && NearlyEqual(
                    capsule->Material().friction,
                    0.75f)
                && NearlyEqual(
                    capsule->Material().restitution,
                    0.35f),
            "CapsuleCollider3D was not restored.");
        // sphere: test fixtureのsphere collider。
        const auto* sphere =
            loadedChild->GetComponent<
                LamaPon::SphereCollider3DComponent>();
        Require(
            sphere != nullptr
                && NearlyEqual(sphere->Radius(), 0.65f)
                && NearlyEqual(sphere->Offset().x, 0.3f)
                && NearlyEqual(sphere->Offset().y, 0.4f)
                && NearlyEqual(sphere->Offset().z, 0.5f)
                && sphere->IsTrigger()
                && sphere->Layer() == 5
                && sphere->CollisionMask() == 0x40u
                && NearlyEqual(
                    sphere->Material().friction,
                    0.6f)
                && NearlyEqual(
                    sphere->Material().restitution,
                    0.2f),
            "SphereCollider3D was not restored.");
        // hull: test fixtureのconvex mesh collider。
        const auto* hull =
            loadedChild->GetComponent<
                LamaPon::ConvexHullCollider3DComponent>();
        Require(
            hull != nullptr
                && hull->Points().size() == 6
                && NearlyEqual(hull->Points()[0].x, 0.8f)
                && NearlyEqual(hull->Points()[4].z, 1.1f)
                && NearlyEqual(hull->Offset().x, 0.4f)
                && NearlyEqual(hull->Offset().y, 0.5f)
                && NearlyEqual(hull->Offset().z, 0.6f)
                && hull->IsTrigger()
                && hull->Layer() == 6
                && hull->CollisionMask() == 0x80u
                && NearlyEqual(
                    hull->Material().friction,
                    0.55f)
                && NearlyEqual(
                    hull->Material().restitution,
                    0.15f),
            "ConvexHullCollider3D was not restored.");
        // rigidbody: Rigidbody component。
        const auto* rigidbody =
            loadedChild->GetComponent<LamaPon::RigidbodyComponent>();
        Require(rigidbody != nullptr, "Rigidbody was not restored.");
        Require(!rigidbody->UsesGravity(), "Rigidbody gravity state was not restored.");
        Require(rigidbody->IsKinematic(), "Rigidbody kinematic state was not restored.");
        Require(
            rigidbody->CollisionDetection()
                == LamaPon::CollisionDetectionMode::Continuous,
            "Rigidbody CCD mode was not restored.");
        Require(
            NearlyEqual(rigidbody->Mass(), 3.5f)
                && NearlyEqual(
                    rigidbody->AngularVelocity().x,
                    0.1f)
                && NearlyEqual(
                    rigidbody->AngularVelocity().y,
                    0.2f)
                && NearlyEqual(
                    rigidbody->AngularVelocity().z,
                    0.0f)
                && NearlyEqual(
                    rigidbody->CenterOfMass().x,
                    0.25f)
                && NearlyEqual(
                    rigidbody->CenterOfMass().y,
                    -0.1f)
                && NearlyEqual(
                    rigidbody->LinearDrag(),
                    0.15f)
                && NearlyEqual(
                    rigidbody->AngularDrag(),
                    0.25f)
                && rigidbody->Constraints()
                    .freezeRotationZ
                && !rigidbody->Interpolates(),
            "Rigidbody angular settings were not restored.");
        // joint: test fixtureのphysics joint component。
        const auto* joint =
            loadedChild->GetComponent<LamaPon::JointComponent>();
        Require(
            joint != nullptr
                && joint->Type() == LamaPon::JointType::Spring
                && joint->ConnectedBodyId() == loadedRoot->Id()
                && NearlyEqual(joint->RestLength(), 2.5f)
                && NearlyEqual(joint->Stiffness(), 18.0f)
                && NearlyEqual(joint->Damping(), 3.0f)
                && joint->UseLimits()
                && NearlyEqual(
                    joint->Limits()
                        .minimumAngleDegrees,
                    -25.0f)
                && NearlyEqual(
                    joint->Limits()
                        .maximumAngleDegrees,
                    40.0f)
                && joint->UseMotor()
                && NearlyEqual(
                    joint->Motor()
                        .targetVelocityDegrees,
                    120.0f)
                && NearlyEqual(
                    joint->Motor()
                        .maximumTorque,
                    35.0f)
                && joint->CollideConnected(),
            "Joint was not restored.");
        // text: text renderer component。
        const auto* text =
            loadedChild->GetComponent<LamaPon::TextRendererComponent>();
        Require(text != nullptr, "TextRenderer was not restored.");
        Require(text->Text() == "日本語テキスト", "Japanese text was not restored.");
        Require(NearlyEqual(text->FontSize(), 28.0f), "Text font size was not restored.");
        Require(
            NearlyEqual(text->LayoutSize().x, 320.0f)
                && NearlyEqual(text->LayoutSize().y, 96.0f),
            "Text layout size was not restored.");
        Require(text->WordWrap(), "Text word wrapping was not restored.");
        Require(
            text->HorizontalAlignment()
                == LamaPon::TextHorizontalAlignment::Center,
            "Text horizontal alignment was not restored.");
        Require(
            text->VerticalAlignment()
                == LamaPon::TextVerticalAlignment::Bottom,
            "Text vertical alignment was not restored.");

        Require(
            loaded.RemoveComponent(*loadedChild, *loadedChild->GetComponent<LamaPon::SpriteRendererComponent>()),
            "Component removal failed.");
        Require(
            loadedChild->GetComponent<LamaPon::SpriteRendererComponent>() == nullptr,
            "Removed component is still present.");

        // cycleRejected: parent cycle拒否結果。
        bool cycleRejected = false;
        // parent cycleを作る再配置を拒否します。
        try
        {
            loadedRoot->SetParent(loadedChild);
        }
        // 拒否時の例外をテスト結果へ記録します。
        catch (const std::invalid_argument&)
        {
            cycleRejected = true;
        }
        Require(cycleRejected, "Hierarchy cycle was not rejected.");

        Require(loaded.DestroyGameObject(*loadedRoot), "GameObject deletion failed.");
        Require(loaded.GameObjects().empty(), "Recursive hierarchy deletion failed.");
        Require(loaded.MainCamera() == nullptr, "Deleted main camera was not cleared.");

        // fileLoaded: duplicate後の保存scene。
        LamaPon::Scene fileLoaded(graphics);
        fileLoaded.LoadFromFile(outputPath);
        Require(fileLoaded.GameObjects().size() == 3, "File scene loading failed.");

        // fileRoot: 読み込み後のroot GameObject。
        auto* fileRoot = fileLoaded.FindGameObject(root.Id());
        Require(fileRoot != nullptr, "File-loaded root was not found.");
        // duplicatedRoot: 複製後のroot GameObject。
        auto& duplicatedRoot = fileLoaded.DuplicateGameObject(*fileRoot);
        Require(duplicatedRoot.Name() == "ルート Copy", "Duplicate name is incorrect.");
        Require(duplicatedRoot.Parent() == nullptr, "Duplicate parent is incorrect.");
        Require(duplicatedRoot.Children().size() == 2, "Duplicate hierarchy is incomplete.");
        Require(
            duplicatedRoot.IsAlwaysVisible()
                && NearlyEqual(
                    duplicatedRoot.CullingMargin(),
                    12.5f),
            "Duplicate did not copy GameObject culling settings.");
        // duplicatedJointConnected: 複製joint参照の接続状態。
        bool duplicatedJointConnected{};
        // duplicatedJointSettings: 複製joint設定の保持状態。
        bool duplicatedJointSettings{};
        // duplicatedLODConnected: 複製LOD参照の接続状態。
        bool duplicatedLODConnected{};
        // 複製されたprefab childを順に検証します。
        for (const auto* duplicateChild :
            duplicatedRoot.Children())
        {
            // 複製childのjoint接続先を検証します。
            if (const auto* duplicateJoint =
                    duplicateChild->GetComponent<
                        LamaPon::JointComponent>())
            {
                duplicatedJointConnected =
                    duplicateJoint->ConnectedBodyId()
                        == duplicatedRoot.Id();
                duplicatedJointSettings =
                    duplicateJoint->UseLimits()
                    && NearlyEqual(
                        duplicateJoint->Limits()
                            .minimumAngleDegrees,
                        -25.0f)
                    && duplicateJoint->UseMotor()
                    && NearlyEqual(
                        duplicateJoint->Motor()
                            .maximumTorque,
                        35.0f);
            }
        }
        // 複製LODのchild参照を検証します。
        if (const auto* duplicateLOD =
                duplicatedRoot.GetComponent<
                    LamaPon::LODGroupComponent>())
        {
            duplicatedLODConnected =
                duplicateLOD->Levels().size() == 1
                && std::ranges::any_of(
                    duplicatedRoot.Children(),
                    [duplicateLOD](
                        const auto* childObject)
                    {
                        // 複製childをLOD targetが参照するか返します。
                        return duplicateLOD->Levels().
                            front().targetId
                            == childObject->Id();
                    });
        }
        Require(
            duplicatedJointConnected,
            "Duplicated Joint target id was not remapped.");
        Require(
            duplicatedJointSettings,
            "Duplicated Hinge settings are incomplete.");
        Require(
            duplicatedLODConnected,
            "Duplicated LOD target id was not remapped.");
        Require(fileLoaded.GameObjects().size() == 6, "Duplicate object count is incorrect.");

        // 複製後のsprite renderer。
        LamaPon::SpriteRendererComponent* duplicatedSprite{};
        // 複製後のaudio source。
        LamaPon::AudioSourceComponent* duplicatedAudio{};
        // 複製後のaudio listener。
        LamaPon::AudioListenerComponent* duplicatedListener{};
        LamaPon::TransformAnimatorComponent*
            duplicatedAnimator{};
        LamaPon::RigidbodyComponent*
            duplicatedRigidbody{};
        LamaPon::SphereCollider3DComponent*
            duplicatedSphere{};
        // 複製childから描画・音声・物理componentを収集します。
        for (auto* duplicatedChild : duplicatedRoot.Children())
        {
            // SpriteRendererがあれば複製後の参照を保持します。
            if (auto* spriteComponent =
                duplicatedChild->GetComponent<LamaPon::SpriteRendererComponent>())
            {
                duplicatedSprite = spriteComponent;
            }
            // AudioSourceがあれば複製後の参照を保持します。
            if (auto* audioComponent =
                duplicatedChild->GetComponent<
                    LamaPon::AudioSourceComponent>())
            {
                duplicatedAudio = audioComponent;
            }
            // AudioListenerがあれば複製後の参照を保持します。
            if (auto* listenerComponent =
                duplicatedChild->GetComponent<
                    LamaPon::AudioListenerComponent>())
            {
                duplicatedListener = listenerComponent;
            }
            // TransformAnimatorがあれば複製後の参照を保持します。
            if (auto* animatorComponent =
                duplicatedChild->GetComponent<
                    LamaPon::TransformAnimatorComponent>())
            {
                duplicatedAnimator =
                    animatorComponent;
            }
            // Rigidbodyがあれば複製後の参照を保持します。
            if (auto* rigidbodyComponent =
                duplicatedChild->GetComponent<
                    LamaPon::RigidbodyComponent>())
            {
                duplicatedRigidbody =
                    rigidbodyComponent;
            }
            // SphereColliderがあれば複製後の参照を保持します。
            if (auto* sphereComponent =
                duplicatedChild->GetComponent<
                    LamaPon::SphereCollider3DComponent>())
            {
                duplicatedSphere = sphereComponent;
            }
        }
        Require(duplicatedSprite != nullptr, "Duplicated component is missing.");
        Require(
            duplicatedSprite->ShaderPath()
                == std::filesystem::path(
                    L"shaders/日本語UI.hlsl")
                && NearlyEqual(
                    duplicatedSprite->
                        CustomParameter(0).z,
                    3.75f),
            "Duplicated Sprite shader settings are incomplete.");
        Require(
            duplicatedAudio != nullptr
                && duplicatedAudio->IsSpatial()
                && NearlyEqual(
                    duplicatedAudio->MinimumDistance(),
                    2.5f)
                && NearlyEqual(
                    duplicatedAudio->MaximumDistance(),
                    30.0f),
            "Duplicated spatial AudioSource is incomplete.");
        Require(
            duplicatedListener != nullptr,
            "Duplicated AudioListener is missing.");
        Require(
            duplicatedAnimator != nullptr
                && NearlyEqual(
                    duplicatedAnimator->Speed(),
                    1.5f)
                && !duplicatedAnimator->Loop()
                && duplicatedAnimator->
                    ControllerPath()
                    == std::filesystem::path(
                        L"animations/移動.animator.json"),
            "Duplicated TransformAnimator is incomplete.");
        Require(
            duplicatedRigidbody != nullptr
                && NearlyEqual(
                    duplicatedRigidbody->Mass(),
                    3.5f)
                && NearlyEqual(
                    duplicatedRigidbody->CenterOfMass().x,
                    0.25f)
                && NearlyEqual(
                    duplicatedRigidbody->AngularDrag(),
                    0.25f)
                && duplicatedRigidbody->Constraints()
                    .freezeRotationZ
                && !duplicatedRigidbody
                    ->Interpolates(),
            "Duplicated Rigidbody angular settings are incomplete.");
        Require(
            duplicatedSphere != nullptr
                && NearlyEqual(
                    duplicatedSphere->Radius(),
                    0.65f)
                && duplicatedSphere->Layer() == 5
                && duplicatedSphere->CollisionMask()
                    == 0x40u,
            "Duplicated SphereCollider3D is incomplete.");
        duplicatedSprite->SetTexturePath("textures/changed.png");
        Require(
            duplicatedSprite->TexturePath() == std::filesystem::path("textures/changed.png"),
            "Texture path replacement failed.");

        // clipboardTarget: clipboard paste先scene。
        LamaPon::Scene clipboardTarget(graphics);
        // pastedRoot: test sceneのroot GameObject。
        auto& pastedRoot = clipboardTarget.DuplicateGameObject(duplicatedRoot);
        Require(pastedRoot.Children().size() == 2, "Cross-scene paste hierarchy failed.");
        Require(
            clipboardTarget.GameObjects().size() == 3,
            "Cross-scene paste object count is incorrect.");

        // Tag・検索API・階層アクティブ・ライフサイクル・Timeの検証
        {
            // tagScene: tag serialization scene。
            LamaPon::Scene tagScene(graphics);
            // taggedParent: test sceneのparent GameObject。
            auto& taggedParent =
                tagScene.CreateGameObject("親オブジェクト");
            taggedParent.SetTag("Player");
            // taggedChild: test sceneのchild GameObject。
            auto& taggedChild =
                tagScene.CreateGameObject("子オブジェクト");
            taggedChild.SetTag("Enemy");
            taggedChild.SetParent(&taggedParent);
            // probe: test fixtureのtest probe component。
            auto& probe = taggedChild.AddComponent<
                ActiveStateProbeComponent>();

            Require(
                tagScene.FindGameObjectByName("親オブジェクト")
                    == &taggedParent,
                "FindGameObjectByName failed.");
            Require(
                tagScene.FindGameObjectByTag("Player")
                    == &taggedParent,
                "FindGameObjectByTag failed.");
            Require(
                tagScene.FindGameObjectsByTag("Enemy").size() == 1
                    && tagScene.FindGameObjectsByTag("None").empty(),
                "FindGameObjectsByTag failed.");
            Require(
                taggedChild.CompareTag("Enemy")
                    && !taggedChild.CompareTag("Player"),
                "CompareTag failed.");
            Require(
                tagScene.FindComponentOfType<
                        ActiveStateProbeComponent>(true)
                    == &probe,
                "FindComponentOfType failed.");

            // 階層アクティブ：親を無効化すると子も実効的に非アクティブ
            tagScene.Update(0.016f);
            Require(
                probe.IsActiveAndEnabled()
                    && probe.enableCount == 1,
                "Component did not become active after update.");
            taggedParent.SetEnabled(false);
            Require(
                !taggedChild.IsActiveInHierarchy()
                    && taggedChild.IsEnabled(),
                "IsActiveInHierarchy did not consider ancestors.");
            Require(
                !probe.IsActiveAndEnabled()
                    && probe.disableCount == 1,
                "OnActiveStateChanged(false) did not fire.");
            probe.updateCount = 0;
            tagScene.Update(0.016f);
            Require(
                probe.updateCount == 0,
                "Disabled parent still updated its children.");
            taggedParent.SetEnabled(true);
            Require(
                probe.enableCount == 2,
                "OnActiveStateChanged(true) did not fire on re-enable.");
            tagScene.Update(0.016f);
            // LateUpdateは最初のUpdateと再有効化後のUpdateで計2回
            Require(
                probe.updateCount == 1
                    && probe.lateUpdateCount == 2,
                "LateUpdate pass did not run.");

            // Tagのシリアライズ往復
            const auto tagJson = tagScene.SerializeToJson();
            // tagLoaded: JSONから復元したtag scene。
            LamaPon::Scene tagLoaded(graphics);
            tagLoaded.LoadFromJson(tagJson);
            // loadedPlayer: 読み込み後のGameObject。
            const auto* loadedPlayer =
                tagLoaded.FindGameObjectByTag("Player");
            Require(
                loadedPlayer != nullptr
                    && loadedPlayer->Name() == "親オブジェクト",
                "Tag did not round-trip through scene JSON.");

            // タグ登録リスト：登録判定・Clear()をまたぐ保持・未登録タグでも読み込みは成功（警告のみ）
            tagLoaded.SetRegisteredTags({ "Player" });
            Require(
                tagLoaded.IsTagRegistered("Player")
                    && !tagLoaded.IsTagRegistered("Enemy"),
                "IsTagRegistered failed.");
            tagLoaded.Clear();
            Require(
                tagLoaded.RegisteredTags().size() == 1,
                "Registered tags must survive Scene::Clear.");
            tagLoaded.LoadFromJson(tagJson);
            Require(
                tagLoaded.FindGameObjectByTag("Enemy")
                    != nullptr,
                "Unregistered tag must still load (warning only).");

            // プロジェクト設定のタグ一覧の保存往復と重複検証
            {
                // projectSettings: 検証対象project settings。
                LamaPon::ProjectSettings projectSettings;
                projectSettings.tags = {
                    "Player",
                    "Enemy",
                    "Collectible" };
                // 3スイートが並列実行されるため、書き込み先はスイート別のディレクトリにします（共有すると書きかけの空ファイルを他プロセスが読みます）。
                const auto projectPath =
                    std::filesystem::current_path()
                    / "test-output"
                    / std::string(suite)
                    / "project-tags.json";
                LamaPon::SaveProjectSettings(
                    projectPath,
                    projectSettings,
                    LamaPon::ProjectSettingsFileType::
                        Project);
                // loadedSettings: JSONから復元したproject settings。
                const auto loadedSettings =
                    LamaPon::LoadProjectSettings(
                        projectPath);
                Require(
                    loadedSettings.tags
                        == projectSettings.tags,
                    "Project tags did not round-trip.");

                projectSettings.tags = { "A", "A" };
                // duplicateRejected: 重複tagの拒否結果。
                bool duplicateRejected = false;
                // duplicate tagを含むproject settingsを拒否します。
                try
                {
                    LamaPon::ValidateProjectSettings(
                        projectSettings);
                }
                // 拒否時の例外をテスト結果へ記録します。
                catch (const std::exception&)
                {
                    duplicateRejected = true;
                }
                Require(
                    duplicateRejected,
                    "Duplicate tags must fail validation.");
            }

            // 階層検索API：GetComponentInChildren/InParent/FindChild
            {
                // searchScene: scene search検証scene。
                LamaPon::Scene searchScene(graphics);
                // nestedRoot: nested prefabのroot GameObject。
                auto& nestedRoot =
                    searchScene.CreateGameObject("ルート");
                // arm: test sceneのarm GameObject。
                auto& arm =
                    searchScene.CreateGameObject("腕");
                arm.SetParent(&nestedRoot);
                // weapon: test sceneのweapon GameObject。
                auto& weapon =
                    searchScene.CreateGameObject("武器");
                weapon.SetParent(&arm);
                // rootProbe: test fixtureのtest probe component。
                auto& rootProbe = nestedRoot.AddComponent<
                    ActiveStateProbeComponent>();
                // weaponProbe: test fixtureのtest probe component。
                auto& weaponProbe = weapon.AddComponent<
                    ActiveStateProbeComponent>();

                Require(
                    nestedRoot.GetComponentInChildren<
                            ActiveStateProbeComponent>()
                        == &rootProbe,
                    "GetComponentInChildren must include self.");
                Require(
                    arm.GetComponentInChildren<
                            ActiveStateProbeComponent>()
                        == &weaponProbe,
                    "GetComponentInChildren must search descendants.");
                Require(
                    nestedRoot.GetComponentsInChildren<
                            ActiveStateProbeComponent>()
                        .size() == 2,
                    "GetComponentsInChildren must collect descendants.");
                Require(
                    weapon.GetComponentInParent<
                            ActiveStateProbeComponent>()
                        == &weaponProbe,
                    "GetComponentInParent must include self.");
                Require(
                    arm.GetComponentInParent<
                            ActiveStateProbeComponent>()
                        == &rootProbe,
                    "GetComponentInParent must walk ancestors.");
                Require(
                    weapon.GetComponentsInParent<
                            ActiveStateProbeComponent>()
                        .size() == 2,
                    "GetComponentsInParent must collect ancestors.");

                // 非アクティブ階層は既定でスキップし、includeInactive=trueで含めます。
                weapon.SetEnabled(false);
                Require(
                    arm.GetComponentInChildren<
                            ActiveStateProbeComponent>()
                        == nullptr,
                    "Inactive descendants must be skipped by default.");
                Require(
                    arm.GetComponentInChildren<
                            ActiveStateProbeComponent>(true)
                        == &weaponProbe,
                    "includeInactive must reach inactive descendants.");
                Require(
                    nestedRoot.GetComponentsInChildren<
                            ActiveStateProbeComponent>()
                        .size() == 1,
                    "GetComponentsInChildren must skip inactive by default.");
                weapon.SetEnabled(true);

                Require(
                    nestedRoot.FindChild("腕") == &arm,
                    "FindChild direct lookup failed.");
                Require(
                    nestedRoot.FindChild("腕/武器") == &weapon,
                    "FindChild path lookup failed.");
                Require(
                    nestedRoot.FindChild("武器") == nullptr,
                    "FindChild must match direct children per segment.");
                Require(
                    nestedRoot.FindChild("腕/") == nullptr
                        && nestedRoot.FindChild("") == nullptr,
                    "FindChild must reject empty segments.");
            }

            // JobSystem：全要素を1回ずつ処理・逐次フォールバック・ネスト・例外伝播
            {
                // jobs: persistence job queue。
                auto& jobs =
                    LamaPon::JobSystem::Instance();
                Require(
                    jobs.WorkerCount() >= 1,
                    "JobSystem must own workers.");

                // Count: atomic callback数の期待値。
                constexpr std::size_t Count = 10000;
                // 並列callbackごとの実行回数。
                std::vector<std::atomic<int>> touched(
                    Count);
                jobs.ParallelFor(
                    Count,
                    64,
                    [&touched](
                        const std::size_t begin,
                        const std::size_t end)
                    {
                        // 変更されたbyte範囲を検証します。
                        for (std::size_t index = begin;
                            index < end;
                            ++index)
                        {
                            touched[index].fetch_add(1);
                        }
                    });
                // allOnce: 全callbackが一度ずつ呼ばれた結果。
                bool allOnce = true;
                // 変更されたbyte範囲を検証します。
                for (const auto& value : touched)
                {
                    allOnce = allOnce
                        && value.load() == 1;
                }
                Require(
                    allOnce,
                    "ParallelFor must visit every index exactly once.");

                // ネスト呼び出しは逐次実行で完了する
                std::atomic<int> nested{ 0 };
                jobs.ParallelFor(
                    8,
                    1,
                    [&jobs, &nested](
                        const std::size_t begin,
                        const std::size_t end)
                    {
                        jobs.ParallelFor(
                            end - begin,
                            1,
                            [&nested](
                                const std::size_t innerBegin,
                                const std::size_t innerEnd)
                            {
                                nested.fetch_add(
                                    static_cast<int>(
                                        innerEnd
                                        - innerBegin));
                            });
                    });
                Require(
                    nested.load() == 8,
                    "Nested ParallelFor must fall back to serial.");

                // 本体の例外は呼び出し元へ伝わる
                bool thrown = false;
                // ParallelFor bodyの例外を呼び出し元へ返すことを確認します。
                try
                {
                    jobs.ParallelFor(
                        128,
                        8,
                        [](const std::size_t begin,
                            const std::size_t)
                        {
                            // 検査対象が上限に達した時点で探索を止めます。
                            if (begin == 64)
                            {
                                // 不正なfixture状態を例外で通知します。
                                throw std::runtime_error(
                                    "job failure");
                            }
                        });
                }
                // 拒否時の例外をテスト結果へ記録します。
                catch (const std::exception&)
                {
                    thrown = true;
                }
                Require(
                    thrown,
                    "ParallelFor must propagate exceptions.");

                // 例外後も正常に使える
                std::atomic<int> after{ 0 };
                jobs.ParallelFor(
                    256,
                    16,
                    [&after](
                        const std::size_t begin,
                        const std::size_t end)
                    {
                        after.fetch_add(
                            static_cast<int>(
                                end - begin));
                    });
                Require(
                    after.load() == 256,
                    "ParallelFor must keep working after an exception.");
            }

            // イベントバス：購読・発行・引数・解除・再入・UIButtonのクリックイベント名の保存往復
            {
                // events: event dispatch用bus。
                LamaPon::EventBus events;
                // received: event callback数。
                int received = 0;
                // lastNumber: 最後に受けたevent値。
                float lastNumber = 0.0f;
                // handle: event subscription handle。
                const auto handle = events.Subscribe(
                    "EnemyDied",
                    [&received, &lastNumber](
                        const LamaPon::EventArgs& args)
                    {
                        ++received;
                        lastNumber = args.number;
                    });
                Require(
                    handle != 0
                        && events.SubscriptionCount()
                            == 1,
                    "Subscribe must register a handler.");
                // payload: serialized payload bytes。
                LamaPon::EventArgs payload;
                payload.number = 100.0f;
                events.Publish("EnemyDied", payload);
                events.Publish("OtherEvent", payload);
                Require(
                    received == 1
                        && lastNumber == 100.0f,
                    "Publish must reach matching handlers only.");

                events.Unsubscribe(handle);
                events.Publish("EnemyDied", payload);
                Require(
                    received == 1
                        && events.SubscriptionCount()
                            == 0,
                    "Unsubscribe must stop delivery.");

                // observed: event observerの結果。
                int observed = 0;
                // observableSubscription: observable event subscription。
                auto observableSubscription =
                    events.Observe("EnemyDied")
                        .Take(1)
                        .Subscribe(
                            [&observed](
                                const LamaPon::EventArgs& args)
                            {
                                observed += static_cast<int>(
                                    args.number);
                            });
                events.Publish("EnemyDied", payload);
                events.Publish("EnemyDied", payload);
                Require(
                    observed == 100,
                    "EventBus Observe/Take bridge failed.");

                // 発行中の自己解除と発行中購読の遅延を検証
                std::uint64_t selfHandle = 0;
                selfHandle = events.Subscribe(
                    "Once",
                    [&events, &received, &selfHandle](
                        const LamaPon::EventArgs&)
                    {
                        ++received;
                        events.Unsubscribe(selfHandle);
                        // 発行中に追加した購読は次回から呼ばれる。
                        static_cast<void>(
                            events.Subscribe(
                                "Once",
                                [&received](
                                    const LamaPon::
                                        EventArgs&)
                                {
                                    received += 10;
                                }));
                    });
                events.Publish("Once");
                Require(
                    received == 2,
                    "Self-unsubscribe during publish failed.");
                events.Publish("Once");
                Require(
                    received == 12,
                    "Late subscriber must receive later publishes.");

                // UIButtonのクリックイベント名の保存往復
                LamaPon::Scene buttonScene(graphics);
                // buttonObject: test sceneのbutton GameObject。
                auto& buttonObject =
                    buttonScene.CreateGameObject(
                        "開始ボタン");
                // button: UI button component。
                auto& button =
                    buttonObject.AddComponent<
                        LamaPon::UIButtonComponent>();
                button.SetClickEventName("StartGame");
                // buttonJson: button sceneの保存JSON。
                const auto buttonJson =
                    buttonScene.SerializeToJson();
                // buttonLoaded: JSONから復元したbutton scene。
                LamaPon::Scene buttonLoaded(graphics);
                buttonLoaded.LoadFromJson(buttonJson);
                // eventButton: 読み込み後のbutton GameObject。
                const auto* eventButton =
                    buttonLoaded
                        .FindGameObjectByName(
                            "開始ボタン")
                        ->GetComponent<
                            LamaPon::
                                UIButtonComponent>();
                Require(
                    eventButton != nullptr
                        && eventButton
                            ->ClickEventName()
                            == "StartGame",
                    "Button click event did not round-trip.");
                Require(
                    nlohmann::json::parse(buttonJson)
                            .dump()
                            .find("\"transition\"")
                        == std::string::npos,
                    "Buttons must not save a transition of their own.");

                // 以前の形式で保存したボタン専用の遷移（"transition"）は読み込みで無視し、保存し直すと消えます。
                // 演出はScene側のScriptやSpriteが描きます。
                auto legacyButtonJson =
                    nlohmann::json::parse(buttonJson);
                // injected: legacy componentを注入した状態。
                bool injected = false;
                // serialized componentのtypeを順に確認します。
                for (auto& object : legacyButtonJson.at("objects"))
                {
                    // serialized componentのtypeを順に確認します。
                    for (auto& component : object.at("components"))
                    {
                        // legacy button componentだけを変換対象にします。
                        if (component.value("type", std::string{})
                            == "UIButton")
                        {
                            component["transition"] = {
                                { "effect", "iris" },
                                { "coverDuration", 0.6 }
                            };
                            injected = true;
                        }
                    }
                }
                // legacyButtonLoaded: legacy JSONから復元したbutton scene。
                LamaPon::Scene legacyButtonLoaded(graphics);
                legacyButtonLoaded.LoadFromJson(
                    legacyButtonJson.dump());
                // legacyButton: 旧schemaのbutton GameObject。
                const auto* legacyButton =
                    legacyButtonLoaded
                        .FindGameObjectByName(
                            "開始ボタン")
                        ->GetComponent<
                            LamaPon::
                                UIButtonComponent>();
                Require(
                    injected
                        && legacyButton != nullptr
                        && legacyButton->ClickEventName()
                            == "StartGame"
                        && legacyButtonLoaded.SerializeToJson()
                                .find("\"transition\"")
                            == std::string::npos,
                    "Legacy button transitions must load and be dropped on save.");
            }

            // スプライトアニメーション：コマ送り・ループ・非ループ停止・ソース矩形・シリアライズ往復
            {
                // spriteScene: sprite serialization scene。
                LamaPon::Scene spriteScene(graphics);
                // player: test sceneのGameObject。
                auto& player =
                    spriteScene.CreateGameObject(
                        "Player");
                // renderer: sprite renderer component。
                auto& renderer = player.AddComponent<
                    LamaPon::SpriteRendererComponent>();
                // overrideAnimator: test fixtureのanimator component。
                auto& overrideAnimator = player.AddComponent<
                    LamaPon::SpriteAnimatorComponent>(
                    4,
                    2);
                overrideAnimator.AddClip(
                    { "walk", 0, 4, 10.0f, true });
                overrideAnimator.AddClip(
                    { "jump", 4, 3, 10.0f, false });
                overrideAnimator.SetDefaultClip("walk");

                spriteScene.Update(0.0f);
                Require(
                    overrideAnimator.IsPlaying()
                        && overrideAnimator.ActiveClipName()
                            == "walk",
                    "Default clip must auto-play.");
                Require(
                    overrideAnimator.CurrentFrame() == 0,
                    "Playback must start at frame 0.");
                spriteScene.Update(0.25f);
                Require(
                    overrideAnimator.CurrentFrame() == 2,
                    "10fps x 0.25s must reach frame 2.");
                Require(
                    renderer.SourceRect().x == 0.5f
                        && renderer.SourceRect().y
                            == 0.0f
                        && renderer.SourceRect().z
                            == 0.25f
                        && renderer.SourceRect().w
                            == 0.5f,
                    "Frame 2 source rect is wrong.");
                spriteScene.Update(0.2f);
                Require(
                    overrideAnimator.CurrentFrame() == 0,
                    "Looping clip must wrap to frame 0.");

                Require(
                    overrideAnimator.Play("jump"),
                    "Play must find the jump clip.");
                spriteScene.Update(1.0f);
                Require(
                    !overrideAnimator.IsPlaying()
                        && overrideAnimator.CurrentFrame() == 6,
                    "Non-loop clip must stop on the last frame.");

                // spriteJson: sprite componentの保存JSON。
                const auto spriteJson =
                    spriteScene.SerializeToJson();
                // spriteLoaded: JSONから復元したsprite scene。
                LamaPon::Scene spriteLoaded(graphics);
                spriteLoaded.LoadFromJson(spriteJson);
                // overridePlayer: 読み込み後のGameObject。
                const auto* overridePlayer =
                    spriteLoaded.FindGameObjectByName(
                        "Player");
                Require(
                    overridePlayer != nullptr,
                    "Sprite scene did not round-trip.");
                // loadedAnimator: 読み込み後のanimator component。
                const auto* loadedAnimator =
                    overridePlayer->GetComponent<
                        LamaPon::
                            SpriteAnimatorComponent>();
                Require(
                    loadedAnimator != nullptr
                        && loadedAnimator->Columns() == 4
                        && loadedAnimator->Rows() == 2
                        && loadedAnimator->Clips()
                            .size() == 2
                        && loadedAnimator->DefaultClip()
                            == "walk"
                        && !loadedAnimator->Clips()[1]
                            .loop,
                    "SpriteAnimator did not round-trip.");
                // loadedRenderer: 読み込み後のrenderer component。
                const auto* loadedRenderer =
                    overridePlayer->GetComponent<
                        LamaPon::
                            SpriteRendererComponent>();
                Require(
                    loadedRenderer != nullptr
                        && loadedRenderer->SourceRect().x
                            == 0.5f
                        && loadedRenderer->SourceRect().y
                            == 0.5f,
                    "Sprite source rect did not round-trip.");

                // 複製でもクリップが引き継がれる
                auto& duplicated =
                    spriteScene.DuplicateGameObject(
                        player);
                // duplicatedOverrideAnimator: 複製後のanimator component。
                const auto* duplicatedOverrideAnimator =
                    duplicated.GetComponent<
                        LamaPon::
                            SpriteAnimatorComponent>();
                Require(
                    duplicatedOverrideAnimator != nullptr
                        && duplicatedOverrideAnimator->Clips()
                            .size() == 2
                        && duplicatedOverrideAnimator
                            ->DefaultClip() == "walk",
                    "SpriteAnimator did not duplicate.");
            }

            // トリガーイベントの振り分け：isTriggerはOnTrigger*へ
            LamaPon::Scene triggerScene(graphics);
            // triggerObject: test sceneのtrigger GameObject。
            auto& triggerObject =
                triggerScene.CreateGameObject("トリガー");
            triggerObject.AddComponent<
                LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f },
                true);
            // triggerProbe: test fixtureのtest probe component。
            auto& triggerProbe = triggerObject.AddComponent<
                TriggerProbeComponent>();
            // visitorObject: test sceneのvisitor GameObject。
            auto& visitorObject =
                triggerScene.CreateGameObject("侵入者");
            visitorObject.AddComponent<
                LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f });
            triggerScene.Update(
                LamaPon::Scene::FixedPhysicsDeltaTime());
            Require(
                triggerProbe.triggerEnterCount > 0
                    && triggerProbe.collisionEnterCount == 0,
                "Trigger contact did not route to OnTriggerEnter.");
            visitorObject.GetTransform().position =
                { 100.0f, 0.0f, 0.0f };
            // 各frameのscene状態を順に更新します。
            for (int frame{}; frame < 4; ++frame)
            {
                triggerScene.Update(
                    LamaPon::Scene::FixedPhysicsDeltaTime());
            }
            Require(
                triggerProbe.triggerExitCount > 0,
                "Trigger separation did not route to OnTriggerExit.");

            // Time API
            LamaPon::Time::Detail::Reset();
            LamaPon::Time::SetTimeScale(0.5f);
            LamaPon::Time::Detail::AdvanceFrame(0.02f);
            Require(
                NearlyEqual(
                    LamaPon::Time::UnscaledDeltaTime(),
                    0.02f)
                    && NearlyEqual(
                        LamaPon::Time::DeltaTime(),
                        0.01f)
                    && LamaPon::Time::FrameCount() == 1,
                "Time scaling is incorrect.");
            LamaPon::Time::SetTimeScale(0.0f);
            Require(
                LamaPon::Time::IsPaused(),
                "Time pause flag is incorrect.");
            LamaPon::Time::Detail::Reset();
            Require(
                NearlyEqual(LamaPon::Time::TimeScale(), 1.0f),
                "Time reset did not restore the scale.");

            // UIウィジェットのシリアライズ往復
            LamaPon::Scene widgetScene(graphics);
            // widgetObject: test sceneのGameObject。
            auto& widgetObject =
                widgetScene.CreateGameObject(
                    "ウィジェット");
            // sourceImage: 保存元sceneのUI image component。
            auto& sourceImage =
                widgetObject.AddComponent<
                    LamaPon::UIImageComponent>(
                    std::filesystem::path{
                        "textures/panel.png" },
                    DirectX::XMFLOAT4{
                        0.5f, 0.6f, 0.7f, 0.8f });
            sourceImage.SetBorder(
                { 8.0f, 12.0f, 16.0f, 20.0f });
            sourceImage.SetSortOrder(3);
            // sourceToggle: 保存元sceneのUI toggle component。
            auto& sourceToggle =
                widgetObject.AddComponent<
                    LamaPon::UIToggleComponent>(
                    "サウンド", true);
            sourceToggle.SetFontSize(30.0f);
            // sourceSlider: 保存元sceneのUI slider component。
            auto& sourceSlider =
                widgetObject.AddComponent<
                    LamaPon::UISliderComponent>(
                    -10.0f, 10.0f, 2.5f);
            sourceSlider.SetWholeNumbers(true);
            // sourceField: 保存元sceneのUI input field component。
            auto& sourceField =
                widgetObject.AddComponent<
                    LamaPon::UIInputFieldComponent>(
                    "こんにちは", "名前を入力");
            sourceField.SetMaxLength(32);
            // sourceLayout: 保存元sceneのUI layout component。
            auto& sourceLayout =
                widgetObject.AddComponent<
                    LamaPon::UILayoutGroupComponent>(
                    LamaPon::UILayoutAxis::Horizontal,
                    12.0f);
            sourceLayout.SetChildAlignment(
                LamaPon::UILayoutAlignment::Center);
            sourceLayout.SetPadding(
                { 4.0f, 5.0f, 6.0f, 7.0f });

            // wholeNumbersで丸められる
            Require(
                NearlyEqual(sourceSlider.Value(), 3.0f)
                    || NearlyEqual(
                        sourceSlider.Value(),
                        2.0f),
                "Slider whole-number rounding failed.");
            sourceSlider.SetNormalizedValue(1.0f);
            Require(
                NearlyEqual(
                    sourceSlider.Value(),
                    10.0f)
                    && sourceSlider
                        .ConsumeValueChanged(),
                "Slider normalized value failed.");

            // widgetJson: UI widgetの保存JSON。
            const auto widgetJson =
                widgetScene.SerializeToJson();
            // widgetLoaded: JSONから復元したwidget scene。
            LamaPon::Scene widgetLoaded(graphics);
            widgetLoaded.LoadFromJson(widgetJson);
            // loadedWidgetObject: 読み込み後のGameObject。
            const auto* loadedWidgetObject =
                widgetLoaded.FindGameObjectByName(
                    "ウィジェット");
            Require(
                loadedWidgetObject != nullptr,
                "Widget object did not round-trip.");
            // loadedImage: 読み込み後のUI image component。
            const auto* loadedImage =
                loadedWidgetObject->GetComponent<
                    LamaPon::UIImageComponent>();
            Require(
                loadedImage != nullptr
                    && loadedImage->TexturePath()
                        == std::filesystem::path{
                            "textures/panel.png" }
                    && NearlyEqual(
                        loadedImage->Border().y,
                        12.0f)
                    && loadedImage->SortOrder() == 3,
                "UIImage did not round-trip.");
            // loadedToggle: 読み込み後のUI toggle component。
            const auto* loadedToggle =
                loadedWidgetObject->GetComponent<
                    LamaPon::UIToggleComponent>();
            Require(
                loadedToggle != nullptr
                    && loadedToggle->IsOn()
                    && loadedToggle->Label()
                        == "サウンド"
                    && NearlyEqual(
                        loadedToggle->FontSize(),
                        30.0f),
                "UIToggle did not round-trip.");
            // loadedSlider: 読み込み後のUI slider component。
            const auto* loadedSlider =
                loadedWidgetObject->GetComponent<
                    LamaPon::UISliderComponent>();
            Require(
                loadedSlider != nullptr
                    && NearlyEqual(
                        loadedSlider->MinimumValue(),
                        -10.0f)
                    && NearlyEqual(
                        loadedSlider->Value(),
                        10.0f)
                    && loadedSlider->WholeNumbers(),
                "UISlider did not round-trip.");
            // loadedField: 読み込み後のUI input field component。
            const auto* loadedField =
                loadedWidgetObject->GetComponent<
                    LamaPon::UIInputFieldComponent>();
            Require(
                loadedField != nullptr
                    && loadedField->Text()
                        == "こんにちは"
                    && loadedField->Placeholder()
                        == "名前を入力"
                    && loadedField->MaxLength() == 32,
                "UIInputField did not round-trip.");
            // loadedLayout: 読み込み後のUI layout component。
            const auto* loadedLayout =
                loadedWidgetObject->GetComponent<
                    LamaPon::UILayoutGroupComponent>();
            Require(
                loadedLayout != nullptr
                    && loadedLayout->Axis()
                        == LamaPon::UILayoutAxis::
                            Horizontal
                    && NearlyEqual(
                        loadedLayout->Spacing(),
                        12.0f)
                    && loadedLayout->ChildAlignment()
                        == LamaPon::UILayoutAlignment::
                            Center
                    && NearlyEqual(
                        loadedLayout->Padding().w,
                        7.0f),
                "UILayoutGroup did not round-trip.");

            // LayoutGroupの子整列
            LamaPon::Scene layoutScene(graphics);
            // layoutRoot: test sceneのroot GameObject。
            auto& layoutRoot =
                layoutScene.CreateGameObject("整列親");
            layoutRoot.AddComponent<
                LamaPon::UIRectTransformComponent>(
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 300.0f, 300.0f });
            // verticalLayout: UI layout component。
            auto& verticalLayout =
                layoutRoot.AddComponent<
                    LamaPon::UILayoutGroupComponent>(
                    LamaPon::UILayoutAxis::Vertical,
                    10.0f);
            verticalLayout.SetPadding(
                { 5.0f, 6.0f, 5.0f, 6.0f });
            // firstChild: test sceneのchild GameObject。
            auto& firstChild =
                layoutScene.CreateGameObject("子1");
            firstChild.SetParent(&layoutRoot);
            // firstChildTransform: test fixtureのTransform state。
            auto& firstChildTransform =
                firstChild.AddComponent<
                    LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 100.0f, 40.0f });
            // secondChild: test sceneのchild GameObject。
            auto& secondChild =
                layoutScene.CreateGameObject("子2");
            secondChild.SetParent(&layoutRoot);
            // secondChildTransform: test fixtureのTransform state。
            auto& secondChildTransform =
                secondChild.AddComponent<
                    LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 100.0f, 40.0f });
            layoutScene.Update(0.016f);
            Require(
                NearlyEqual(
                    firstChildTransform
                        .AnchoredPosition().x,
                    5.0f)
                    && NearlyEqual(
                        firstChildTransform
                            .AnchoredPosition().y,
                        6.0f),
                "Layout group first child position failed.");
            Require(
                NearlyEqual(
                    secondChildTransform
                        .AnchoredPosition().y,
                    56.0f),
                "Layout group second child position failed.");

            // Mesh Collider：10x10の床（2三角形）でクエリと衝突を検証
            LamaPon::Scene meshScene(graphics);
            // floorObject: test sceneのGameObject。
            auto& floorObject =
                meshScene.CreateGameObject("メッシュ床");
            // floorCollider: test fixtureのcollider component。
            auto& floorCollider =
                floorObject.AddComponent<
                    LamaPon::MeshCollider3DComponent>();
            floorCollider.SetMesh(
                {
                    { -5.0f, 0.0f, -5.0f },
                    { 5.0f, 0.0f, -5.0f },
                    { 5.0f, 0.0f, 5.0f },
                    { -5.0f, 0.0f, 5.0f }
                },
                { 0, 1, 2, 0, 2, 3 });
            Require(
                floorCollider.HasMesh()
                    && floorCollider.TriangleCount()
                        == 2,
                "Mesh collider build failed.");

            // meshHit: mesh raycast hit。
            LamaPon::PhysicsHit meshHit{};
            // meshRayFound: mesh ray hitの検出状態。
            const bool meshRayFound =
                meshScene.Raycast(
                    LamaPon::Ray{
                        { 1.0f, 3.0f, 1.0f },
                        { 0.0f, -1.0f, 0.0f } },
                    10.0f,
                    meshHit);
            Require(
                meshRayFound
                    && meshHit.meshCollider
                        == &floorCollider
                    && NearlyEqual(
                        meshHit.distance,
                        3.0f)
                    && NearlyEqual(
                        meshHit.point.y,
                        0.0f)
                    && meshHit.normal.y > 0.9f,
                "Mesh collider raycast failed.");
            // meshMiss: mesh raycast miss。
            LamaPon::PhysicsHit meshMiss{};
            Require(
                !meshScene.Raycast(
                    LamaPon::Ray{
                        { 20.0f, 3.0f, 0.0f },
                        { 0.0f, -1.0f, 0.0f } },
                    10.0f,
                    meshMiss),
                "Mesh collider raycast hit outside the mesh.");

            // meshOverlaps: mesh overlap結果。
            const auto meshOverlaps =
                meshScene.OverlapBox(
                    LamaPon::Bounds3D{
                        { -1.0f, -0.5f, -1.0f },
                        { 1.0f, 0.5f, 1.0f } });
            Require(
                meshOverlaps.size() == 1
                    && meshOverlaps.front().meshCollider
                        == &floorCollider,
                "Mesh collider overlap failed.");
            Require(
                meshScene.OverlapBox(
                    LamaPon::Bounds3D{
                        { -1.0f, 1.0f, -1.0f },
                        { 1.0f, 2.0f, 1.0f } }).empty(),
                "Mesh collider overlap above the floor.");

            // 球Rigidbodyがメッシュ床の上で静止する
            auto& meshBall =
                meshScene.CreateGameObject("落下球");
            meshBall.GetTransform().position =
                { 0.0f, 2.0f, 0.0f };
            meshBall.AddComponent<
                LamaPon::SphereCollider3DComponent>(
                0.5f);
            // meshBallBody: Rigidbody component。
            auto& meshBallBody =
                meshBall.AddComponent<
                    LamaPon::RigidbodyComponent>();
            static_cast<void>(meshBallBody);
            // 各frameのscene状態を順に更新します。
            for (int frame{}; frame < 180; ++frame)
            {
                meshScene.Update(
                    LamaPon::Scene::
                        FixedPhysicsDeltaTime());
            }
            Require(
                meshBall.GetTransform().position.y
                        > 0.25f
                    && meshBall.GetTransform().position.y
                        < 0.8f,
                "Sphere did not rest on the mesh collider.");

            // シリアライズ往復（モデルパス・物理設定）
            LamaPon::Scene meshSaveScene(graphics);
            // meshSaveObject: test sceneのGameObject。
            auto& meshSaveObject =
                meshSaveScene.CreateGameObject(
                    "ステージ");
            // meshSaveCollider: test fixtureのcollider component。
            auto& meshSaveCollider =
                meshSaveObject.AddComponent<
                    LamaPon::MeshCollider3DComponent>(
                    std::filesystem::path{
                        "models/stage.glb" },
                    DirectX::XMFLOAT3{
                        0.0f, -1.0f, 0.0f },
                    false,
                    4,
                    0x30u,
                    LamaPon::PhysicsMaterial{
                        0.7f,
                        0.25f });
            static_cast<void>(meshSaveCollider);
            // meshJson: mesh sceneの保存JSON。
            const auto meshJson =
                meshSaveScene.SerializeToJson();
            // meshLoadScene: mesh JSONのload先scene。
            LamaPon::Scene meshLoadScene(graphics);
            meshLoadScene.LoadFromJson(meshJson);
            // loadedStage: 読み込み後のGameObject。
            const auto* loadedStage =
                meshLoadScene.FindGameObjectByName(
                    "ステージ");
            // loadedMeshCollider: 読み込み後のcollider component。
            const auto* loadedMeshCollider =
                loadedStage != nullptr
                    ? loadedStage->GetComponent<
                        LamaPon::MeshCollider3DComponent>()
                    : nullptr;
            Require(
                loadedMeshCollider != nullptr
                    && loadedMeshCollider->ModelPath()
                        == std::filesystem::path{
                            "models/stage.glb" }
                    && loadedMeshCollider->Layer() == 4
                    && loadedMeshCollider
                        ->CollisionMask() == 0x30u
                    && NearlyEqual(
                        loadedMeshCollider->Offset().y,
                        -1.0f)
                    && NearlyEqual(
                        loadedMeshCollider
                            ->Material().friction,
                        0.7f),
                "Mesh collider did not round-trip.");

            // PhysicsMaterialの合成モード
            {
                // left: 左側physics material。
                LamaPon::PhysicsMaterial left{
                    0.2f, 0.1f };
                // right: 右側physics material。
                LamaPon::PhysicsMaterial right{
                    0.8f, 0.5f };
                // defaults: default physics material。
                const auto defaults =
                    LamaPon::CombinePhysicsMaterials(
                        left,
                        right);
                Require(
                    NearlyEqual(
                        defaults.friction,
                        std::sqrt(0.2f * 0.8f))
                        && NearlyEqual(
                            defaults.restitution,
                            0.5f),
                    "Default material combine changed.");
                left.frictionCombine =
                    LamaPon::PhysicsMaterialCombine::
                        Maximum;
                left.restitutionCombine =
                    LamaPon::PhysicsMaterialCombine::
                        Average;
                right.restitutionCombine =
                    LamaPon::PhysicsMaterialCombine::
                        Average;
                // custom: custom physics material。
                const auto custom =
                    LamaPon::CombinePhysicsMaterials(
                        left,
                        right);
                Require(
                    NearlyEqual(custom.friction, 0.8f)
                        && NearlyEqual(
                            custom.restitution,
                            0.3f),
                    "Material combine modes failed.");
            }

            // カプセル×ボックスの2点マニフォールド
            {
                // physicsCapsule: physics query用capsule。
                const LamaPon::Capsule3D physicsCapsule{
                    { -1.0f, 0.4f, 0.0f },
                    { 1.0f, 0.4f, 0.0f },
                    0.5f };
                // floorBox: 床用oriented box。
                LamaPon::OrientedBox3D floorBox{};
                floorBox.center = { 0.0f, -0.5f, 0.0f };
                floorBox.halfExtents =
                    { 5.0f, 0.5f, 5.0f };
                // manifold: collision manifold。
                const auto manifold =
                    LamaPon::IntersectManifold(
                        physicsCapsule,
                        floorBox);
                Require(
                    manifold.has_value()
                        && manifold->pointCount == 2
                        && manifold->normal.y > 0.9f
                        && manifold->penetration > 0.05f,
                    "Capsule-box manifold failed.");
            }

            // CircleCollider2D：床ボックスの上で円が静止する
            LamaPon::Scene circleScene(graphics);
            // circleGround: test sceneのground GameObject。
            auto& circleGround =
                circleScene.CreateGameObject("2D床");
            circleGround.AddComponent<
                LamaPon::BoxCollider2DComponent>(
                DirectX::XMFLOAT2{ 10.0f, 1.0f });
            // circleBall: test sceneのGameObject。
            auto& circleBall =
                circleScene.CreateGameObject("2D円");
            circleBall.GetTransform().position =
                { 0.0f, 2.0f, 0.0f };
            // circleCollider: test fixtureのcollider component。
            auto& circleCollider =
                circleBall.AddComponent<
                    LamaPon::CircleCollider2DComponent>(
                    0.5f);
            // circleBody: Rigidbody component。
            auto& circleBody =
                circleBall.AddComponent<
                    LamaPon::RigidbodyComponent>();
            // 2D用の拘束：Z位置とX/Y回転を固定します。
            circleBody.SetConstraints({
                true,
                true,
                false,
                false,
                false,
                true });
            // 各frameのscene状態を順に更新します。
            for (int frame{}; frame < 180; ++frame)
            {
                circleScene.Update(
                    LamaPon::Scene::
                        FixedPhysicsDeltaTime());
            }
            Require(
                circleBall.GetTransform().position.y
                        > 0.75f
                    && circleBall.GetTransform()
                        .position.y < 1.3f,
                "Circle did not rest on the 2D floor.");
            Require(
                std::abs(
                    circleBall.GetTransform()
                        .position.z) < 0.0001f,
                "Frozen Z position drifted in 2D.");

            // CircleCollider2Dと合成モードのシリアライズ往復
            auto material = circleCollider.Material();
            material.friction = 1.5f;
            material.frictionCombine =
                LamaPon::PhysicsMaterialCombine::Minimum;
            circleCollider.SetMaterial(material);
            circleCollider.SetLayer(6);
            circleCollider.SetCollisionMask(0x44u);
            // circleJson: circle colliderの保存JSON。
            const auto circleJson =
                circleScene.SerializeToJson();
            // circleLoaded: JSONから復元したcircle scene。
            LamaPon::Scene circleLoaded(graphics);
            circleLoaded.LoadFromJson(circleJson);
            // loadedBall: 読み込み後のGameObject。
            const auto* loadedBall =
                circleLoaded.FindGameObjectByName(
                    "2D円");
            // loadedCircle: 読み込み後のCircleCollider2D。
            const auto* loadedCircle =
                loadedBall != nullptr
                    ? loadedBall->GetComponent<
                        LamaPon::
                            CircleCollider2DComponent>()
                    : nullptr;
            Require(
                loadedCircle != nullptr
                    && NearlyEqual(
                        loadedCircle->Radius(),
                        0.5f)
                    && loadedCircle->Layer() == 6
                    && loadedCircle->CollisionMask()
                        == 0x44u
                    && NearlyEqual(
                        loadedCircle
                            ->Material().friction,
                        1.5f)
                    && loadedCircle->Material()
                            .frictionCombine
                        == LamaPon::
                            PhysicsMaterialCombine::
                                Minimum,
                "Circle collider did not round-trip.");
            // loadedCircleBody: 読み込み後のRigidbody component。
            const auto* loadedCircleBody =
                loadedBall->GetComponent<
                    LamaPon::RigidbodyComponent>();
            Require(
                loadedCircleBody != nullptr
                    && loadedCircleBody->Constraints()
                        .freezePositionZ
                    && loadedCircleBody->Constraints()
                        .freezeRotationX
                    && !loadedCircleBody->Constraints()
                        .freezePositionX,
                "Position constraints did not round-trip.");

            // PolygonCollider2D：床ボックスの上で三角形が平らな辺で静止する
            LamaPon::Scene polygonScene(graphics);
            // polygonGround: test sceneのground GameObject。
            auto& polygonGround =
                polygonScene.CreateGameObject("2D床");
            polygonGround.AddComponent<
                LamaPon::BoxCollider2DComponent>(
                DirectX::XMFLOAT2{ 10.0f, 1.0f });
            // polygonFalling: test sceneのGameObject。
            auto& polygonFalling =
                polygonScene.CreateGameObject("2D三角形");
            polygonFalling.GetTransform().position =
                { 0.0f, 3.0f, 0.0f };
            // polygonCollider: test fixtureのcollider component。
            auto& polygonCollider =
                polygonFalling.AddComponent<
                    LamaPon::PolygonCollider2DComponent>(
                    std::vector<DirectX::XMFLOAT2>{
                        { 0.0f, 0.5f },
                        { -0.5f, -0.5f },
                        { 0.5f, -0.5f } });
            // polygonBody: Rigidbody component。
            auto& polygonBody =
                polygonFalling.AddComponent<
                    LamaPon::RigidbodyComponent>();
            // 2D用の拘束：Z位置とX/Y回転を固定します。
            polygonBody.SetConstraints({
                true,
                true,
                false,
                false,
                false,
                true });
            // 各frameのscene状態を順に更新します。
            for (int frame{}; frame < 180; ++frame)
            {
                polygonScene.Update(
                    LamaPon::Scene::
                        FixedPhysicsDeltaTime());
            }
            Require(
                polygonFalling.GetTransform().position.y
                        > 0.85f
                    && polygonFalling.GetTransform()
                        .position.y < 1.25f,
                "Polygon did not rest on the 2D floor.");
            Require(
                std::abs(
                    polygonFalling.GetTransform()
                        .EulerAngles().z) < 0.1f,
                "Polygon tipped over while resting"
                    " on its flat edge.");

            // PolygonCollider2Dと合成モードのシリアライズ往復
            auto polygonMaterial = polygonCollider.Material();
            polygonMaterial.friction = 1.5f;
            polygonMaterial.frictionCombine =
                LamaPon::PhysicsMaterialCombine::Minimum;
            polygonCollider.SetMaterial(polygonMaterial);
            polygonCollider.SetLayer(6);
            polygonCollider.SetCollisionMask(0x44u);
            // polygonJson: polygon colliderの保存JSON。
            const auto polygonJson =
                polygonScene.SerializeToJson();
            // polygonLoaded: JSONから復元したpolygon scene。
            LamaPon::Scene polygonLoaded(graphics);
            polygonLoaded.LoadFromJson(polygonJson);
            // loadedTriangle: 読み込み後のGameObject。
            const auto* loadedTriangle =
                polygonLoaded.FindGameObjectByName(
                    "2D三角形");
            // loadedPolygon: 読み込み後のPolygonCollider2D。
            const auto* loadedPolygon =
                loadedTriangle != nullptr
                    ? loadedTriangle->GetComponent<
                        LamaPon::
                            PolygonCollider2DComponent>()
                    : nullptr;
            Require(
                loadedPolygon != nullptr
                    && loadedPolygon->Vertices().size()
                        == 3
                    && loadedPolygon->Layer() == 6
                    && loadedPolygon->CollisionMask()
                        == 0x44u
                    && NearlyEqual(
                        loadedPolygon
                            ->Material().friction,
                        1.5f)
                    && loadedPolygon->Material()
                            .frictionCombine
                        == LamaPon::
                            PhysicsMaterialCombine::
                                Minimum,
                "Polygon collider did not round-trip.");

            // Tilemap::ComputeCollisionRects：隣接セルの貪欲な矩形マージ
            {
                // tilemapScene: tilemap serialization scene。
                LamaPon::Scene tilemapScene(graphics);
                // tilemapObject: test sceneのtilemap GameObject。
                auto& tilemapObject =
                    tilemapScene.CreateGameObject("床タイル");
                // collisionTilemap: test fixtureのtilemap component。
                auto& collisionTilemap =
                    tilemapObject.AddComponent<
                        LamaPon::TilemapComponent>(
                            DirectX::XMFLOAT2{
                                16.0f, 16.0f });
                // 3x2の矩形ブロック：1個の矩形へマージされるはずです。
                for (int y{}; y < 2; ++y)
                {
                    // 横方向のfixture要素を順に生成します。
                    for (int x{}; x < 3; ++x)
                    {
                        collisionTilemap.SetCell(x, y, 0);
                    }
                }
                // 離れた1セル：別の矩形になるはずです。
                collisionTilemap.SetCell(10, 10, 0);

                // rects: tilemapから生成した矩形列。
                const auto rects =
                    collisionTilemap.ComputeCollisionRects();
                Require(
                    rects.size() == 2,
                    "Tilemap collider rects did not merge"
                        " as expected.");

                // blockRect: 遮られたtileの矩形。
                const auto blockRect = std::find_if(
                    rects.begin(),
                    rects.end(),
                    [](const auto& rect)
                    {
                        // 浮動小数値が許容誤差内か返します。
                        return NearlyEqual(
                            rect.size.x, 48.0f);
                    });
                Require(
                    blockRect != rects.end()
                        && NearlyEqual(
                            blockRect->size.y, 32.0f)
                        && NearlyEqual(
                            blockRect->center.x, 24.0f)
                        && NearlyEqual(
                            blockRect->center.y, 16.0f),
                    "Merged tilemap block rect had"
                        " unexpected size or center.");

                // singleRect: 単一tileから得た矩形。
                const auto singleRect = std::find_if(
                    rects.begin(),
                    rects.end(),
                    [](const auto& rect)
                    {
                        // 浮動小数値が許容誤差内か返します。
                        return NearlyEqual(
                            rect.size.x, 16.0f);
                    });
                Require(
                    singleRect != rects.end()
                        && NearlyEqual(
                            singleRect->size.y, 16.0f)
                        && NearlyEqual(
                            singleRect->center.x, 168.0f)
                        && NearlyEqual(
                            singleRect->center.y, 168.0f),
                    "Isolated tilemap cell rect had"
                        " unexpected size or center.");

                // 描画順（背景／地形／前景の重ね順）の往復確認。
                collisionTilemap.SetSortOrder(-5);
                // tilemapJson: tilemapの保存JSON。
                const auto tilemapJson =
                    tilemapScene.SerializeToJson();
                // tilemapLoaded: JSONから復元したtilemap scene。
                LamaPon::Scene tilemapLoaded(graphics);
                tilemapLoaded.LoadFromJson(tilemapJson);
                // loadedTilemapObject: 読み込み後のtilemap GameObject。
                const auto* loadedTilemapObject =
                    tilemapLoaded.FindGameObjectByName(
                        "床タイル");
                // loadedCollisionTilemap: 読み込み後のtilemap component。
                const auto* loadedCollisionTilemap =
                    loadedTilemapObject != nullptr
                        ? loadedTilemapObject
                            ->GetComponent<
                                LamaPon::
                                    TilemapComponent>()
                        : nullptr;
                Require(
                    loadedCollisionTilemap != nullptr
                        && loadedCollisionTilemap->SortOrder()
                            == -5,
                    "Tilemap SortOrder did not"
                        " round-trip.");
            }

            // ParallaxLayer：参照の移動量に倍率を掛けて追従する
            {
                // parallaxScene: parallax serialization scene。
                LamaPon::Scene parallaxScene(graphics);
                // referenceObject: test sceneのGameObject。
                auto& referenceObject =
                    parallaxScene.CreateGameObject(
                        "参照");
                // backgroundObject: test sceneのground GameObject。
                auto& backgroundObject =
                    parallaxScene.CreateGameObject(
                        "背景");
                backgroundObject.GetTransform()
                    .position =
                        { 100.0f, 50.0f, 0.0f };
                // parallax: backgroundObjectのParallaxLayer component。
                auto& parallax =
                    backgroundObject.AddComponent<
                        LamaPon::
                            ParallaxLayerComponent>();
                parallax.SetFactor({ 0.5f, 0.5f });
                parallax.SetReferenceId(
                    referenceObject.Id());

                // 初回Updateは原点記録のみで、位置は動かないはずです。
                parallaxScene.Update(1.0f / 60.0f);
                Require(
                    NearlyEqual(
                        backgroundObject
                            .GetTransform()
                            .position.x,
                        100.0f)
                        && NearlyEqual(
                            backgroundObject
                                .GetTransform()
                                .position.y,
                            50.0f),
                    "ParallaxLayer moved on the frame"
                        " it captured its origin.");

                referenceObject.GetTransform()
                    .position.x += 20.0f;
                referenceObject.GetTransform()
                    .position.y += 10.0f;
                parallaxScene.Update(1.0f / 60.0f);
                Require(
                    NearlyEqual(
                        backgroundObject
                            .GetTransform()
                            .position.x,
                        110.0f)
                        && NearlyEqual(
                            backgroundObject
                                .GetTransform()
                                .position.y,
                            55.0f),
                    "ParallaxLayer did not move by"
                        " factor * reference delta.");

                // parallaxJson: parallax sceneの保存JSON。
                const auto parallaxJson =
                    parallaxScene.SerializeToJson();
                // parallaxLoaded: JSONから復元したparallax scene。
                LamaPon::Scene parallaxLoaded(graphics);
                parallaxLoaded.LoadFromJson(
                    parallaxJson);
                // loadedBackground: 読み込み後のground GameObject。
                const auto* loadedBackground =
                    parallaxLoaded.FindGameObjectByName(
                        "背景");
                // loadedParallax: 読み込み後のParallaxLayer component。
                const auto* loadedParallax =
                    loadedBackground != nullptr
                        ? loadedBackground
                            ->GetComponent<
                                LamaPon::
                                    ParallaxLayerComponent>()
                        : nullptr;
                // loadedReference: 読み込み後のGameObject。
                const auto* loadedReference =
                    parallaxLoaded.FindGameObjectByName(
                        "参照");
                Require(
                    loadedParallax != nullptr
                        && NearlyEqual(
                            loadedParallax->Factor().x,
                            0.5f)
                        && NearlyEqual(
                            loadedParallax->Factor().y,
                            0.5f)
                        && loadedReference != nullptr
                        && loadedParallax
                                ->ReferenceId()
                            == loadedReference->Id(),
                    "ParallaxLayer's factor or"
                        " reference did not round-trip.");
            }

            // Light2D：シリアライズ往復とワールド座標
            {
                // light2DScene: 2D light serialization scene。
                LamaPon::Scene light2DScene(graphics);
                // torch: test sceneのtorch GameObject。
                auto& torch =
                    light2DScene.CreateGameObject("たいまつ");
                torch.GetTransform().position =
                    { 42.0f, -7.0f, 0.0f };
                // light2D: test fixtureのlight component。
                auto& light2D =
                    torch.AddComponent<
                        LamaPon::Light2DComponent>();
                light2D.SetColor(
                    { 0.2f, 0.6f, 1.0f });
                light2D.SetIntensity(2.5f);
                light2D.SetRadius(320.0f);

                // worldPosition: world座標。
                const auto worldPosition =
                    light2D.WorldPosition();
                Require(
                    NearlyEqual(worldPosition.x, 42.0f)
                        && NearlyEqual(
                            worldPosition.y, -7.0f),
                    "Light2D world position did not"
                        " follow the Transform.");

                // light2DJson: 2D lightの保存JSON。
                const auto light2DJson =
                    light2DScene.SerializeToJson();
                // light2DLoaded: JSONから復元した2D light scene。
                LamaPon::Scene light2DLoaded(graphics);
                light2DLoaded.LoadFromJson(light2DJson);
                // loadedTorch: 読み込み後のtorch GameObject。
                const auto* loadedTorch =
                    light2DLoaded.FindGameObjectByName(
                        "たいまつ");
                // loadedLight2D: 読み込み後のlight component。
                const auto* loadedLight2D =
                    loadedTorch != nullptr
                        ? loadedTorch->GetComponent<
                            LamaPon::Light2DComponent>()
                        : nullptr;
                Require(
                    loadedLight2D != nullptr
                        && NearlyEqual(
                            loadedLight2D->Color().x,
                            0.2f)
                        && NearlyEqual(
                            loadedLight2D->Color().y,
                            0.6f)
                        && NearlyEqual(
                            loadedLight2D->Color().z,
                            1.0f)
                        && NearlyEqual(
                            loadedLight2D->Intensity(),
                            2.5f)
                        && NearlyEqual(
                            loadedLight2D->Radius(),
                            320.0f),
                    "Light2D did not round-trip.");
            }

            // SpriteMask：シリアライズ往復とSpriteRendererの
            // MaskInteraction
            {
                // spriteMaskScene: sprite mask serialization scene。
                LamaPon::Scene spriteMaskScene(graphics);
                // maskObject: test sceneのGameObject。
                auto& maskObject =
                    spriteMaskScene.CreateGameObject(
                        "マスク");
                maskObject.GetTransform().position =
                    { 10.0f, 20.0f, 0.0f };
                // spriteMask: test fixtureのsprite renderer component。
                auto& spriteMask =
                    maskObject.AddComponent<
                        LamaPon::SpriteMaskComponent>();
                spriteMask.SetShape(
                    LamaPon::SpriteMaskShape::Circle);
                spriteMask.SetSize(
                    { 250.0f, 250.0f });

                // maskWorldPosition: mask vertexのworld座標。
                const auto maskWorldPosition =
                    spriteMask.WorldPosition();
                Require(
                    NearlyEqual(
                        maskWorldPosition.x, 10.0f)
                        && NearlyEqual(
                            maskWorldPosition.y, 20.0f),
                    "SpriteMask world position did not"
                        " follow the Transform.");

                // fogObject: test sceneのGameObject。
                auto& fogObject =
                    spriteMaskScene.CreateGameObject(
                        "霧");
                // fogSprite: sprite renderer component。
                auto& fogSprite =
                    fogObject.AddComponent<
                        LamaPon::SpriteRendererComponent>();
                fogSprite.SetMaskInteraction(
                    LamaPon::SpriteMaskInteraction::
                        VisibleOutsideMask);

                // spriteMaskJson: sprite maskの保存JSON。
                const auto spriteMaskJson =
                    spriteMaskScene.SerializeToJson();
                // spriteMaskLoaded: JSONから復元したmask scene。
                LamaPon::Scene spriteMaskLoaded(graphics);
                spriteMaskLoaded.LoadFromJson(
                    spriteMaskJson);
                // loadedMaskObject: 読み込み後のGameObject。
                const auto* loadedMaskObject =
                    spriteMaskLoaded.FindGameObjectByName(
                        "マスク");
                // loadedMask: 読み込み後のSpriteMask component。
                const auto* loadedMask =
                    loadedMaskObject != nullptr
                        ? loadedMaskObject
                            ->GetComponent<
                                LamaPon::
                                    SpriteMaskComponent>()
                        : nullptr;
                Require(
                    loadedMask != nullptr
                        && loadedMask->Shape()
                            == LamaPon::SpriteMaskShape::
                                Circle
                        && NearlyEqual(
                            loadedMask->Size().x,
                            250.0f),
                    "SpriteMask did not round-trip.");

                // loadedFogObject: 読み込み後のGameObject。
                const auto* loadedFogObject =
                    spriteMaskLoaded.FindGameObjectByName(
                        "霧");
                // loadedFogSprite: 読み込み後のsprite renderer component。
                const auto* loadedFogSprite =
                    loadedFogObject != nullptr
                        ? loadedFogObject
                            ->GetComponent<
                                LamaPon::
                                    SpriteRendererComponent>()
                        : nullptr;
                Require(
                    loadedFogSprite != nullptr
                        && loadedFogSprite
                                ->MaskInteraction()
                            == LamaPon::
                                SpriteMaskInteraction::
                                    VisibleOutsideMask,
                    "SpriteRenderer's MaskInteraction did"
                        " not round-trip.");
            }

            // UIScrollView：コンテンツ高さ・クランプ・Resolveシフト
            LamaPon::Scene scrollScene(graphics);
            // scrollRoot: test sceneのroot GameObject。
            auto& scrollRoot =
                scrollScene.CreateGameObject(
                    "スクロール");
            scrollRoot.AddComponent<
                LamaPon::UIRectTransformComponent>(
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 200.0f, 300.0f });
            // scrollView: UI scroll view component。
            auto& scrollView =
                scrollRoot.AddComponent<
                    LamaPon::UIScrollViewComponent>();
            // scrollItem: test sceneのGameObject。
            auto& scrollItem =
                scrollScene.CreateGameObject(
                    "スクロール項目");
            scrollItem.SetParent(&scrollRoot);
            // scrollItemTransform: test fixtureのscroll view component。
            auto& scrollItemTransform =
                scrollItem.AddComponent<
                    LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 100.0f },
                    DirectX::XMFLOAT2{
                        200.0f, 500.0f });
            scrollScene.Update(0.016f);
            Require(
                NearlyEqual(
                    scrollView.ContentHeight(),
                    600.0f)
                    && NearlyEqual(
                        scrollView
                            .MaximumScrollOffset(),
                        300.0f),
                "Scroll view content height failed.");
            scrollView.SetScrollOffset(1000.0f);
            Require(
                NearlyEqual(
                    scrollView.ScrollOffset(),
                    300.0f),
                "Scroll offset clamp failed.");
            // shiftedRect: 移動後のtile rect。
            const auto shiftedRect =
                scrollItemTransform.Resolve(
                    800.0f,
                    600.0f);
            Require(
                NearlyEqual(
                    shiftedRect.minimum.y,
                    -200.0f),
                "Scroll offset did not shift children.");
            scrollView.SetScrollOffset(0.0f);
            // unshiftedRect: 移動前のtile rect。
            const auto unshiftedRect =
                scrollItemTransform.Resolve(
                    800.0f,
                    600.0f);
            Require(
                NearlyEqual(
                    unshiftedRect.minimum.y,
                    100.0f),
                "Scroll reset did not restore children.");

            // ScrollViewのシリアライズ往復
            scrollView.SetScrollSpeed(96.0f);
            scrollView.SetSortOrder(2);
            // scrollJson: scroll viewの保存JSON。
            const auto scrollJson =
                scrollScene.SerializeToJson();
            // scrollLoaded: JSONから復元したscroll scene。
            LamaPon::Scene scrollLoaded(graphics);
            scrollLoaded.LoadFromJson(scrollJson);
            // loadedScrollRoot: 読み込み後のroot GameObject。
            const auto* loadedScrollRoot =
                scrollLoaded.FindGameObjectByName(
                    "スクロール");
            // loadedScrollView: 読み込み後のUI scroll view component。
            const auto* loadedScrollView =
                loadedScrollRoot != nullptr
                    ? loadedScrollRoot->GetComponent<
                        LamaPon::UIScrollViewComponent>()
                    : nullptr;
            Require(
                loadedScrollView != nullptr
                    && NearlyEqual(
                        loadedScrollView->ScrollSpeed(),
                        96.0f)
                    && loadedScrollView->SortOrder()
                        == 2,
                "Scroll view did not round-trip.");

            // PBR（metallic）・スカイキューブマップ・ライト影の往復
            LamaPon::Scene pbrScene(graphics);
            // pbrObject: test sceneのGameObject。
            auto& pbrObject =
                pbrScene.CreateGameObject("金属キューブ");
            // pbrMesh: mesh renderer component。
            auto& pbrMesh = pbrObject.AddComponent<
                LamaPon::MeshRendererComponent>();
            pbrMesh.SetMetallic(0.75f);
            // shadowSpotObject: test sceneのGameObject。
            auto& shadowSpotObject =
                pbrScene.CreateGameObject("影スポット");
            // shadowSpot: 影用SpotLight component。
            auto& shadowSpot =
                shadowSpotObject.AddComponent<
                    LamaPon::SpotLightComponent>();
            shadowSpot.SetCastsShadows(true);
            shadowSpot.SetShadowStrength(0.6f);
            shadowSpot.SetShadowBias(0.004f);
            // shadowPointObject: test sceneのGameObject。
            auto& shadowPointObject =
                pbrScene.CreateGameObject("影ポイント");
            // shadowPoint: 影用PointLight component。
            auto& shadowPoint =
                shadowPointObject.AddComponent<
                    LamaPon::PointLightComponent>();
            shadowPoint.SetCastsShadows(true);
            shadowPoint.SetShadowStrength(0.7f);
            // pbrSky: PBR test用sky texture。
            auto pbrSky = pbrScene.Sky();
            pbrSky.enabled = true;
            pbrSky.cubemapPath = "textures/sky.dds";
            pbrSky.iblIntensity = 1.5f;
            pbrScene.SetSkySettings(pbrSky);

            // pbrJson: PBR sceneの保存JSON。
            const auto pbrJson =
                pbrScene.SerializeToJson();
            // pbrDocument: PBR material文書。
            const auto pbrDocument =
                nlohmann::json::parse(pbrJson);
            // serializedSky: serialize済みsky settings。
            const auto& serializedSky =
                pbrDocument.at("environment").at("sky");
            Require(
                !serializedSky.contains("texture")
                    && !serializedSky.contains("specular")
                    && !serializedSky.contains("irradiance")
                    && !serializedSky.contains("specularMaximumMip"),
                "Runtime IBL handles leaked into scene serialization.");
            // pbrLoaded: JSONから復元したPBR scene。
            LamaPon::Scene pbrLoaded(graphics);
            pbrLoaded.LoadFromJson(pbrJson);
            // loadedPbrObject: 読み込み後のGameObject。
            const auto* loadedPbrObject =
                pbrLoaded.FindGameObjectByName(
                    "金属キューブ");
            // loadedPbrMesh: 読み込み後のmesh renderer component。
            const auto* loadedPbrMesh =
                loadedPbrObject != nullptr
                    ? loadedPbrObject->GetComponent<
                        LamaPon::MeshRendererComponent>()
                    : nullptr;
            Require(
                loadedPbrMesh != nullptr
                    && NearlyEqual(
                        loadedPbrMesh->Metallic(),
                        0.75f),
                "Metallic did not round-trip.");
            // loadedShadowSpot: 読み込み後のGameObject。
            const auto* loadedShadowSpot =
                pbrLoaded.FindGameObjectByName(
                    "影スポット")
                    ->GetComponent<
                        LamaPon::SpotLightComponent>();
            Require(
                loadedShadowSpot != nullptr
                    && loadedShadowSpot->CastsShadows()
                    && NearlyEqual(
                        loadedShadowSpot
                            ->ShadowStrength(),
                        0.6f)
                    && NearlyEqual(
                        loadedShadowSpot->ShadowBias(),
                        0.004f),
                "Spot shadow settings did not round-trip.");
            // loadedShadowPoint: 読み込み後のGameObject。
            const auto* loadedShadowPoint =
                pbrLoaded.FindGameObjectByName(
                    "影ポイント")
                    ->GetComponent<
                        LamaPon::PointLightComponent>();
            Require(
                loadedShadowPoint != nullptr
                    && loadedShadowPoint->CastsShadows()
                    && NearlyEqual(
                        loadedShadowPoint
                            ->ShadowStrength(),
                        0.7f),
                "Point shadow settings did not round-trip.");
            Require(
                pbrLoaded.Sky().cubemapPath
                        == std::filesystem::path{
                            "textures/sky.dds" }
                    && NearlyEqual(
                        pbrLoaded.Sky().iblIntensity,
                        1.5f),
                "Sky cubemap settings did not round-trip.");

            // AudioSourceのストリーミング/バス設定の往復
            auto& bgmObject =
                pbrScene.CreateGameObject("BGM");
            // bgmSource: audio source component。
            auto& bgmSource = bgmObject.AddComponent<
                LamaPon::AudioSourceComponent>(
                std::filesystem::path{
                    "audio/startup.ogg" },
                0.8f);
            bgmSource.SetBus(LamaPon::AudioBus::Music);
            bgmSource.SetStreaming(true);
            // audioJson: audio componentの保存JSON。
            const auto audioJson =
                pbrScene.SerializeToJson();
            // audioLoaded: JSONから復元したaudio scene。
            LamaPon::Scene audioLoaded(graphics);
            audioLoaded.LoadFromJson(audioJson);
            // loadedBgm: 読み込み後のGameObject。
            const auto* loadedBgm =
                audioLoaded.FindGameObjectByName("BGM")
                    ->GetComponent<
                        LamaPon::AudioSourceComponent>();
            Require(
                loadedBgm != nullptr
                    && loadedBgm->IsStreaming()
                    && loadedBgm->Bus()
                        == LamaPon::AudioBus::Music
                    && NearlyEqual(
                        loadedBgm->Volume(),
                        0.8f),
                "Audio streaming settings did not round-trip.");
        }

        // simulation suiteの検証を選びます。
        if (suite == "simulation")
        {
        // ヒエラルキーの並び替え。
        // 並びは見た目だけでなく、ライトの収集順（先着N灯）と保存順を決めるので、順序そのものを検査します。
        {
            // reorderScene: object順序検証scene。
            LamaPon::Scene reorderScene(graphics);
            // first: test sceneのGameObject。
            auto& first =
                reorderScene.CreateGameObject("First");
            // second: test sceneのGameObject。
            auto& second =
                reorderScene.CreateGameObject("Second");
            // third: test sceneのGameObject。
            auto& third =
                reorderScene.CreateGameObject("Third");
            // persistentChild: 永続化対象のchild GameObject。
            auto& persistentChild =
                reorderScene.CreateGameObject("Child");
            persistentChild.SetParent(&third);

            // rootOrder: root objectの保存順序。
            const auto rootOrder =
                [&reorderScene]
                {
                    // names: sceneから集めたobject名。
                    std::string names;
                    // scene内の各objectを確認します。
                    for (const auto& object :
                        reorderScene.GameObjects())
                    {
                        names += object->Name();
                        names += ',';
                    }
                    // 収集したscene object名を返します。
                    return names;
                };

            Require(
                rootOrder()
                    == "First,Second,Third,Child,",
                "Unexpected initial hierarchy order.");

            // ThirdをFirstの前へ。
            // 子も一緒に動く必要があります。
            Require(
                reorderScene.ReorderGameObject(
                    third,
                    first,
                    false),
                "Reordering before the first object failed.");
            Require(
                rootOrder()
                    == "Third,Child,First,Second,",
                "Moving before an object must carry the"
                " subtree with it.");

            // SecondをThirdの直後へ（子の後ろに入ること）。
            Require(
                reorderScene.ReorderGameObject(
                    second,
                    third,
                    true),
                "Reordering after an object failed.");
            Require(
                rootOrder()
                    == "Third,Second,Child,First,",
                "Inserting after an object must land"
                " immediately after it.");

            // 自分の子孫を基準にした移動は弾きます。
            Require(
                !reorderScene.ReorderGameObject(
                    third,
                    persistentChild,
                    false),
                "Reordering relative to a descendant must"
                " be rejected.");
            // 自分自身も弾きます。
            Require(
                !reorderScene.ReorderGameObject(
                    first,
                    first,
                    true),
                "Reordering relative to itself must be"
                " rejected.");
            Require(
                rootOrder()
                    == "Third,Second,Child,First,",
                "A rejected reorder must not change the"
                " order.");

            // 子同士の並び替え。
            // 表示順の出どころが親のm_childrenなので、m_gameObjectsだけを直しても表示順は変わりません。
            // 両方の並びを検査します。
            auto& parent =
                reorderScene.CreateGameObject("Parent");
            // childA: test sceneのchild GameObject。
            auto& childA =
                reorderScene.CreateGameObject("ChildA");
            // childB: test sceneのchild GameObject。
            auto& childB =
                reorderScene.CreateGameObject("ChildB");
            // childC: test sceneのchild GameObject。
            auto& childC =
                reorderScene.CreateGameObject("ChildC");
            childA.SetParent(&parent);
            childB.SetParent(&parent);
            childC.SetParent(&parent);

            // childOrder: childの保存順序。
            const auto childOrder =
                [&parent]
                {
                    // names: sceneから集めたobject名。
                    std::string names;
                    // scene内の各objectを確認します。
                    for (const auto* object :
                        parent.Children())
                    {
                        names += object->Name();
                        names += ',';
                    }
                    // 収集したscene object名を返します。
                    return names;
                };

            Require(
                childOrder() == "ChildA,ChildB,ChildC,",
                "Unexpected initial child order.");
            Require(
                reorderScene.ReorderGameObject(
                    childC,
                    childA,
                    false),
                "Reordering a child failed.");
            Require(
                childOrder() == "ChildC,ChildA,ChildB,",
                "Reordering a child must update the"
                " parent's child list, not just the scene"
                " list.");
            Require(
                reorderScene.ReorderGameObject(
                    childC,
                    childB,
                    true),
                "Reordering a child after a sibling"
                " failed.");
            Require(
                childOrder() == "ChildA,ChildB,ChildC,",
                "Inserting a child after a sibling must"
                " land immediately after it.");

            // 並びが保存へ載ることも確認します（再起動後に同じ並びで開けること）。
            const auto snapshot =
                reorderScene.SerializeToJson();
            // loadedScene: JSONから復元したscene。
            LamaPon::Scene loadedScene(graphics);
            loadedScene.LoadFromJson(snapshot);
            // loadedNames: 復元後object名一覧。
            std::string loadedNames;
            // scene内の各objectを確認します。
            for (const auto& object :
                loadedScene.GameObjects())
            {
                loadedNames += object->Name();
                loadedNames += ',';
            }
            Require(
                loadedNames
                    == "Third,Second,Child,First,Parent,"
                       "ChildA,ChildB,ChildC,",
                "Hierarchy order must survive a save and"
                " load round trip.");
            // 子の並びも読み込み後に保たれること。
            const auto* loadedParent =
                loadedScene.FindGameObjectByName("Parent");
            Require(
                loadedParent != nullptr,
                "The parent object must survive loading.");
            // loadedChildNames: 復元後child名の順序。
            std::string loadedChildNames;
            // 読込後のhierarchy childを検証します。
            for (const auto* loadedHierarchyChild :
                loadedParent->Children())
            {
                loadedChildNames += loadedHierarchyChild->Name();
                loadedChildNames += ',';
            }
            Require(
                loadedChildNames
                    == "ChildA,ChildB,ChildC,",
                "Child order must survive a save and load"
                " round trip.");
        }

        // thirtyFpsScene: 30 FPS simulation scene。
        LamaPon::Scene thirtyFpsScene(graphics);
        // thirtyFpsObject: test sceneのGameObject。
        auto& thirtyFpsObject =
            thirtyFpsScene.CreateGameObject(
                "30 FPS fixed body");
        // thirtyFpsBody: Rigidbody component。
        auto& thirtyFpsBody =
            thirtyFpsObject.AddComponent<
                LamaPon::RigidbodyComponent>();
        thirtyFpsBody.SetUseGravity(false);
        // thirtyFpsProbe: test fixtureのtest probe component。
        auto& thirtyFpsProbe =
            thirtyFpsObject.AddComponent<
                FixedUpdateProbeComponent>();
        // 各frameのscene状態を順に更新します。
        for (int frame{}; frame < 30; ++frame)
        {
            thirtyFpsScene.Update(1.0f / 30.0f);
        }

        // highFpsScene: high-FPS simulation scene。
        LamaPon::Scene highFpsScene(graphics);
        // highFpsObject: test sceneのGameObject。
        auto& highFpsObject =
            highFpsScene.CreateGameObject(
                "144 FPS fixed body");
        // highFpsBody: Rigidbody component。
        auto& highFpsBody =
            highFpsObject.AddComponent<
                LamaPon::RigidbodyComponent>();
        highFpsBody.SetUseGravity(false);
        // highFpsProbe: test fixtureのtest probe component。
        auto& highFpsProbe =
            highFpsObject.AddComponent<
                FixedUpdateProbeComponent>();
        // 各frameのscene状態を順に更新します。
        for (int frame{}; frame < 144; ++frame)
        {
            highFpsScene.Update(1.0f / 144.0f);
        }
        Require(
            thirtyFpsProbe.fixedUpdateCount == 60
                && highFpsProbe.fixedUpdateCount == 60
                && std::abs(
                    thirtyFpsProbe.updateElapsed
                        - 1.0f) < 0.0001f
                && std::abs(
                    highFpsProbe.updateElapsed
                        - 1.0f) < 0.0001f
                && std::abs(
                    thirtyFpsProbe.fixedElapsed
                        - 1.0f) < 0.0001f
                && std::abs(
                    highFpsProbe.fixedElapsed
                        - 1.0f) < 0.0001f,
            "FixedUpdate rate changed with render frame rate.");
        Require(
            std::abs(
                thirtyFpsBody.Velocity().x
                    - highFpsBody.Velocity().x)
                    < 0.0001f
                && std::abs(
                    thirtyFpsObject.GetTransform()
                        .position.x
                    - highFpsObject.GetTransform()
                        .position.x)
                    < 0.0001f,
            "Physics result changed between 30 and 144 FPS.");

        // interpolationScene: interpolation検証scene。
        LamaPon::Scene interpolationScene(graphics);
        interpolationScene.Update(1.0f / 120.0f);
        Require(
            interpolationScene
                .PhysicsFixedStepsLastFrame() == 0
                && std::abs(
                    interpolationScene
                        .PhysicsInterpolationAlpha()
                    - 0.5f) < 0.001f,
            "Physics interpolation alpha is incorrect before a fixed step.");
        interpolationScene.Update(1.0f / 120.0f);
        Require(
            interpolationScene
                .PhysicsFixedStepsLastFrame() == 1
                && interpolationScene
                    .PhysicsInterpolationAlpha()
                    < 0.001f,
            "Physics fixed-step accumulator is incorrect.");

        // renderInterpolationScene: 描画補間検証scene。
        LamaPon::Scene renderInterpolationScene(graphics);
        // interpolationParent: test sceneのparent GameObject。
        auto& interpolationParent =
            renderInterpolationScene.CreateGameObject(
                "Interpolated parent");
        // interpolationBody: Rigidbody component。
        auto& interpolationBody =
            interpolationParent.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{
                        6.0f,
                        0.0f,
                        0.0f },
                    false);
        // interpolationChild: test sceneのchild GameObject。
        auto& interpolationChild =
            renderInterpolationScene.CreateGameObject(
                "Interpolated child");
        interpolationChild.SetParent(
            &interpolationParent);
        interpolationChild.GetTransform().position =
            { 2.0f, 0.0f, 0.0f };
        renderInterpolationScene.Update(
            1.0f / 60.0f);
        renderInterpolationScene.Update(
            1.0f / 120.0f);
        // interpolatedParentMatrix: 補間後のparent matrix。
        DirectX::XMFLOAT4X4 interpolatedParentMatrix{};
        DirectX::XMStoreFloat4x4(
            &interpolatedParentMatrix,
            interpolationParent
                .InterpolatedWorldMatrix(
                    renderInterpolationScene
                        .PhysicsInterpolationAlpha()));
        // interpolatedChildMatrix: 補間後のchild matrix。
        DirectX::XMFLOAT4X4 interpolatedChildMatrix{};
        DirectX::XMStoreFloat4x4(
            &interpolatedChildMatrix,
            interpolationChild
                .InterpolatedWorldMatrix(
                    renderInterpolationScene
                        .PhysicsInterpolationAlpha()));
        Require(
            NearlyEqual(
                interpolationParent
                    .GetTransform().position.x,
                0.1f)
                && std::abs(
                    interpolatedParentMatrix._41
                        - 0.05f) < 0.0001f
                && std::abs(
                    interpolatedChildMatrix._41
                        - 2.05f) < 0.0001f,
            "Rigidbody render interpolation or child inheritance is incorrect.");
        interpolationBody.SetInterpolate(false);
        renderInterpolationScene.Update(
            1.0f / 120.0f);
        DirectX::XMStoreFloat4x4(
            &interpolatedParentMatrix,
            interpolationParent
                .InterpolatedWorldMatrix(0.0f));
        Require(
            NearlyEqual(
                interpolatedParentMatrix._41,
                interpolationParent
                    .GetTransform().position.x),
            "Disabled Rigidbody interpolation still changed the render transform.");

        // physicsScene: physics simulation検証scene。
        LamaPon::Scene physicsScene(graphics);
        // ground: test sceneのground GameObject。
        auto& ground = physicsScene.CreateGameObject("床");
        ground.AddComponent<LamaPon::BoxCollider3DComponent>(
            DirectX::XMFLOAT3{ 10.0f, 1.0f, 10.0f });

        // fallingBox: test sceneのbox GameObject。
        auto& fallingBox = physicsScene.CreateGameObject("落下物");
        fallingBox.GetTransform().position = { 0.0f, 5.0f, 0.0f };
        fallingBox.AddComponent<LamaPon::BoxCollider3DComponent>();
        // fallingBody: Rigidbody component。
        auto& fallingBody = fallingBox.AddComponent<LamaPon::RigidbodyComponent>();
        // collisionProbe: collision probe component。
        auto& collisionProbe = fallingBox.AddComponent<CollisionProbeComponent>();

        // 指定step数だけsimulationを進めます。
        for (int step = 0; step < 240; ++step)
        {
            physicsScene.Update(1.0f / 60.0f);
        }

        Require(
            std::abs(
                fallingBox.GetTransform().position.y
                    - 1.0f)
                < 0.01f,
            "Falling body did not settle on the floor.");
        Require(
            NearlyEqual(fallingBody.Velocity().y, 0.0f),
            "Collision did not stop inward velocity.");
        Require(collisionProbe.enterCount >= 1, "Collision enter was not reported.");
        Require(collisionProbe.stayCount >= 1, "Collision stay was not reported.");
        Require(
            std::isfinite(collisionProbe.lastPoint.x)
                && std::isfinite(collisionProbe.lastPoint.y)
                && std::isfinite(collisionProbe.lastPoint.z),
            "Collision contact point was not reported.");

        fallingBox.TranslateWorld({ 0.0f, 5.0f, 0.0f });
        physicsScene.Update(0.0f);
        Require(collisionProbe.exitCount >= 1, "Collision exit was not reported.");

        // 衝突マトリクスで切ったペアは、コライダーのマスクが許していても当たらないこと。
        // レイヤー0（床）と1を切った状態で、レイヤー0の箱は床に載り、レイヤー1の箱は素通りする。
        // 両方を同じシーンへ置き、物理更新全体の停止を対照で除外します。
        {
            // savedPhysics: 保存済みphysics設定。
            const auto savedPhysics =
                LamaPon::ActivePhysicsSettings();
            // matrixPhysics: matrix変換後のphysics scene。
            auto matrixPhysics = savedPhysics;
            matrixPhysics.collisionMatrix[0] &=
                ~(1u << 1);
            matrixPhysics.collisionMatrix[1] &=
                ~(1u << 0);
            LamaPon::SetActivePhysicsSettings(
                matrixPhysics);

            // matrixScene: matrix transform検証scene。
            LamaPon::Scene matrixScene(graphics);
            // matrixGround: test sceneのground GameObject。
            auto& matrixGround =
                matrixScene.CreateGameObject("床");
            matrixGround.AddComponent<
                LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 10.0f, 1.0f, 10.0f });

            // solidBox: test sceneのbox GameObject。
            auto& solidBox =
                matrixScene.CreateGameObject("載る箱");
            solidBox.GetTransform().position =
                { -2.0f, 5.0f, 0.0f };
            solidBox.AddComponent<
                LamaPon::BoxCollider3DComponent>();
            solidBox.AddComponent<
                LamaPon::RigidbodyComponent>();

            // ghostBox: test sceneのbox GameObject。
            auto& ghostBox =
                matrixScene.CreateGameObject(
                    "すり抜ける箱");
            ghostBox.GetTransform().position =
                { 2.0f, 5.0f, 0.0f };
            // ghostCollider: test fixtureのcollider component。
            auto& ghostCollider =
                ghostBox.AddComponent<
                    LamaPon::BoxCollider3DComponent>();
            ghostCollider.SetLayer(1);
            ghostBox.AddComponent<
                LamaPon::RigidbodyComponent>();

            // 指定step数だけsimulationを進めます。
            for (int step = 0; step < 240; ++step)
            {
                matrixScene.Update(1.0f / 60.0f);
            }
            Require(
                std::abs(
                    solidBox.GetTransform().position.y
                        - 1.0f)
                    < 0.01f,
                "The layer-0 box must still rest on the"
                " floor while the matrix cuts 0-1.");
            Require(
                ghostBox.GetTransform().position.y
                    < -5.0f,
                "A pair cut in the collision matrix must"
                " not collide.");

            LamaPon::SetActivePhysicsSettings(
                savedPhysics);
        }

        // diagonal: 対角移動の距離。
        constexpr float diagonal = 0.70710678f;
        // rotatedBox: 回転したoriented box。
        const LamaPon::OrientedBox3D rotatedBox{
            {},
            {
                DirectX::XMFLOAT3{ diagonal, diagonal, 0.0f },
                DirectX::XMFLOAT3{ -diagonal, diagonal, 0.0f },
                DirectX::XMFLOAT3{ 0.0f, 0.0f, 1.0f }
            },
            { 1.0f, 0.3f, 0.5f }
        };
        // nearbyBox: 近接box collider。
        LamaPon::OrientedBox3D nearbyBox;
        nearbyBox.center = { 0.0f, 1.0f, 0.0f };
        Require(
            LamaPon::Intersect(rotatedBox, nearbyBox).has_value(),
            "Rotated OBB collision was not detected.");
        // manifoldBox: manifold用box。
        LamaPon::OrientedBox3D manifoldBox;
        manifoldBox.center =
            { 0.0f, 0.9f, 0.0f };
        // manifoldGround: ground contactのmanifold。
        LamaPon::OrientedBox3D manifoldGround;
        manifoldGround.halfExtents =
            { 5.0f, 0.5f, 5.0f };
        // faceManifold: face contact manifold。
        const auto faceManifold =
            LamaPon::IntersectManifold(
                manifoldBox,
                manifoldGround);
        Require(
            faceManifold.has_value()
                && faceManifold->pointCount == 4,
            "Box face contact did not generate a four-point manifold.");
        nearbyBox.center = { 0.0f, 3.0f, 0.0f };
        Require(
            !LamaPon::Intersect(rotatedBox, nearbyBox).has_value(),
            "Separated rotated OBBs incorrectly collided.");

        // testCapsule: 比較用test capsule。
        const LamaPon::Capsule3D testCapsule{
            { 0.0f, 0.7f, 0.0f },
            { 0.0f, 1.7f, 0.0f },
            0.5f
        };
        Require(
            LamaPon::Intersect(
                testCapsule,
                LamaPon::OrientedBox3D{}).has_value(),
            "Capsule-to-box collision was not detected.");
        Require(
            LamaPon::Intersect(
                testCapsule,
                LamaPon::Capsule3D{
                    { 0.7f, 0.7f, 0.0f },
                    { 0.7f, 1.7f, 0.0f },
                    0.5f }).has_value(),
            "Capsule-to-capsule collision was not detected.");
        // testSphere: 比較用test sphere。
        const LamaPon::Sphere3D testSphere{
            { 0.0f, 0.7f, 0.0f },
            0.5f
        };
        Require(
            LamaPon::Intersect(
                testSphere,
                LamaPon::Sphere3D{
                    { 0.7f, 0.7f, 0.0f },
                    0.5f }).has_value(),
            "Sphere-to-sphere collision was not detected.");
        Require(
            LamaPon::Intersect(
                testSphere,
                LamaPon::OrientedBox3D{}).has_value(),
            "Sphere-to-box collision was not detected.");
        Require(
            LamaPon::Intersect(
                testSphere,
                testCapsule).has_value(),
            "Sphere-to-capsule collision was not detected.");
        // sphereBounds: sphere collider bounds。
        const auto sphereBounds =
            LamaPon::BoundsOf(testSphere);
        Require(
            NearlyEqual(sphereBounds.minimum.x, -0.5f)
                && NearlyEqual(
                    sphereBounds.maximum.y,
                    1.2f),
            "Sphere bounds are incorrect.");

        // testHull: 比較用test hull。
        const LamaPon::ConvexHull3D testHull{
            std::vector<DirectX::XMFLOAT3>{
                { 0.5f, 0.7f, 0.0f },
                { -0.5f, 0.7f, 0.0f },
                { 0.0f, 1.2f, 0.0f },
                { 0.0f, 0.2f, 0.0f },
                { 0.0f, 0.7f, 0.5f },
                { 0.0f, 0.7f, -0.5f }
            }
        };
        // otherHull: 比較用convex hull。
        const auto otherHull =
            LamaPon::ConvexHull3D{
                std::vector<DirectX::XMFLOAT3>{
                    { 0.7f, 0.7f, 0.0f },
                    { -0.3f, 0.7f, 0.0f },
                    { 0.7f, 1.2f, 0.0f },
                    { 0.7f, 0.2f, 0.0f },
                    { 0.7f, 0.7f, 0.5f },
                    { 0.7f, 0.7f, -0.5f }
                }
            };
        // hullHullContact: hull同士のcontact。
        const auto hullHullContact =
            LamaPon::Intersect(testHull, otherHull);
        Require(
            hullHullContact.has_value()
                && hullHullContact->penetration > 0.0f,
            "Hull-to-hull collision was not detected.");
        Require(
            hullHullContact->normal.x < 0.0f,
            "Hull-to-hull contact normal points the wrong way.");
        Require(
            LamaPon::Intersect(
                testHull,
                LamaPon::OrientedBox3D{}).has_value(),
            "Hull-to-box collision was not detected.");
        Require(
            LamaPon::Intersect(
                testHull,
                testCapsule).has_value(),
            "Hull-to-capsule collision was not detected.");
        Require(
            LamaPon::Intersect(
                testHull,
                testSphere).has_value(),
            "Hull-to-sphere collision was not detected.");
        // separatedHull: 分離状態のconvex hull。
        const auto separatedHull =
            LamaPon::ConvexHull3D{
                std::vector<DirectX::XMFLOAT3>{
                    { 10.5f, 0.7f, 0.0f },
                    { 9.5f, 0.7f, 0.0f },
                    { 10.0f, 1.2f, 0.0f },
                    { 10.0f, 0.2f, 0.0f },
                    { 10.0f, 0.7f, 0.5f },
                    { 10.0f, 0.7f, -0.5f }
                }
            };
        Require(
            !LamaPon::Intersect(
                testHull,
                separatedHull).has_value(),
            "Separated convex hulls incorrectly collided.");
        // hullBounds: convex hull bounds。
        const auto hullBounds =
            LamaPon::BoundsOf(testHull);
        Require(
            NearlyEqual(hullBounds.minimum.y, 0.2f)
                && NearlyEqual(hullBounds.maximum.y, 1.2f),
            "Convex hull bounds are incorrect.");

        // sphereScene: sphere collider検証scene。
        LamaPon::Scene sphereScene(graphics);
        // sphereGround: test sceneのground GameObject。
        auto& sphereGround =
            sphereScene.CreateGameObject("Sphere ground");
        sphereGround.GetTransform().position =
            { 0.0f, -0.5f, 0.0f };
        sphereGround.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{
                    10.0f,
                    1.0f,
                    10.0f });
        // fallingSphere: test sceneのsphere GameObject。
        auto& fallingSphere =
            sphereScene.CreateGameObject("Falling sphere");
        fallingSphere.GetTransform().position =
            { 0.0f, 3.0f, 0.0f };
        fallingSphere.AddComponent<
            LamaPon::SphereCollider3DComponent>(0.5f);
        // sphereBody: Rigidbody component。
        auto& sphereBody =
            fallingSphere.AddComponent<
                LamaPon::RigidbodyComponent>();
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 300; ++step)
        {
            sphereScene.Update(1.0f / 60.0f);
        }
        Require(
            std::abs(
                fallingSphere.GetTransform().position.y
                    - 0.5f) < 0.03f
                && std::abs(
                    sphereBody.Velocity().y) < 0.05f,
            "A dynamic sphere did not settle on a box.");

        // hullScene: convex hull検証scene。
        LamaPon::Scene hullScene(graphics);
        // hullGround: test sceneのground GameObject。
        auto& hullGround =
            hullScene.CreateGameObject("Hull ground");
        hullGround.GetTransform().position =
            { 0.0f, -0.5f, 0.0f };
        hullGround.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{
                    10.0f,
                    1.0f,
                    10.0f });
        // fallingHull: test sceneのGameObject。
        auto& fallingHull =
            hullScene.CreateGameObject("Falling hull");
        fallingHull.GetTransform().position =
            { 0.0f, 3.0f, 0.0f };
        fallingHull.AddComponent<
            LamaPon::ConvexHullCollider3DComponent>();
        // hullBody: Rigidbody component。
        auto& hullBody =
            fallingHull.AddComponent<
                LamaPon::RigidbodyComponent>();
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 300; ++step)
        {
            hullScene.Update(1.0f / 60.0f);
        }
        Require(
            std::abs(
                fallingHull.GetTransform().position.y
                    - 0.5f) < 0.03f
                && std::abs(
                    hullBody.Velocity().y) < 0.05f,
            "A dynamic convex hull did not settle on a box.");

        // compoundScene: compound collider検証scene。
        LamaPon::Scene compoundScene(graphics);
        // compoundRoot: test sceneのroot GameObject。
        auto& compoundRoot =
            compoundScene.CreateGameObject("Compound root");
        compoundRoot.GetTransform().position =
            { 0.0f, 3.0f, 0.0f };
        // compoundBody: Rigidbody component。
        auto& compoundBody =
            compoundRoot.AddComponent<
                LamaPon::RigidbodyComponent>();
        compoundBody.SetConstraints({
            true,
            true,
            true
        });
        // compoundBox: test sceneのbox GameObject。
        auto& compoundBox =
            compoundScene.CreateGameObject("Compound box");
        compoundBox.SetParent(&compoundRoot);
        compoundBox.GetTransform().position =
            { -0.45f, 0.0f, 0.0f };
        compoundBox.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        // compoundBoxProbe: collision probe component。
        auto& compoundBoxProbe =
            compoundBox.AddComponent<
                CollisionProbeComponent>();
        // compoundSphere: test sceneのsphere GameObject。
        auto& compoundSphere =
            compoundScene.CreateGameObject("Compound sphere");
        compoundSphere.SetParent(&compoundRoot);
        compoundSphere.GetTransform().position =
            { 0.45f, 0.0f, 0.0f };
        compoundSphere.AddComponent<
            LamaPon::SphereCollider3DComponent>(0.5f);
        // compoundSphereProbe: collision probe component。
        auto& compoundSphereProbe =
            compoundSphere.AddComponent<
                CollisionProbeComponent>();
        compoundScene.Update(0.0f);
        Require(
            compoundBoxProbe.enterCount == 0
                && compoundSphereProbe.enterCount == 0,
            "Child colliders on one Rigidbody collided with each other.");
        // compoundGround: test sceneのground GameObject。
        auto& compoundGround =
            compoundScene.CreateGameObject("Compound ground");
        compoundGround.GetTransform().position =
            { 0.0f, -0.5f, 0.0f };
        compoundGround.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{
                    10.0f,
                    1.0f,
                    10.0f });
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 300; ++step)
        {
            compoundScene.Update(1.0f / 60.0f);
        }
        Require(
            std::abs(
                compoundRoot.GetTransform().position.y
                    - 0.5f) < 0.05f
                && NearlyEqual(
                    compoundBox.GetTransform().position.x,
                    -0.45f)
                && NearlyEqual(
                    compoundSphere.GetTransform().position.x,
                    0.45f)
                && (compoundBoxProbe.enterCount > 0
                    || compoundSphereProbe.enterCount > 0),
            "Child colliders did not resolve through their parent Rigidbody.");

        // materialScene: material serialization scene。
        LamaPon::Scene materialScene(graphics);
        // materialGround: test sceneのground GameObject。
        auto& materialGround =
            materialScene.CreateGameObject("Material ground");
        materialGround.GetTransform().position =
            { 0.0f, -0.5f, 0.0f };
        materialGround.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 20.0f, 1.0f, 20.0f },
                DirectX::XMFLOAT3{},
                false,
                0,
                0xffffffffu,
                LamaPon::PhysicsMaterial{ 1.0f, 0.0f });
        // bouncingCapsule: test sceneのGameObject。
        auto& bouncingCapsule =
            materialScene.CreateGameObject("Bouncing capsule");
        bouncingCapsule.GetTransform().position =
            { 0.0f, 3.0f, 0.0f };
        bouncingCapsule.AddComponent<
            LamaPon::CapsuleCollider3DComponent>(
                0.5f,
                2.0f,
                DirectX::XMFLOAT3{},
                false,
                0,
                0xffffffffu,
                LamaPon::PhysicsMaterial{ 0.25f, 0.8f });
        // bouncingBody: Rigidbody component。
        auto& bouncingBody =
            bouncingCapsule.AddComponent<
                LamaPon::RigidbodyComponent>();
        // fellFast: 落下中に基準速度を超えた状態。
        bool fellFast{};
        // bounced: 反発が発生した状態。
        bool bounced{};
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 240; ++step)
        {
            materialScene.Update(1.0f / 60.0f);
            fellFast = fellFast
                || bouncingBody.Velocity().y < -1.0f;
            bounced = bounced
                || (fellFast
                    && bouncingBody.Velocity().y > 1.0f);
        }
        Require(
            bounced,
            "Physics material restitution did not bounce a capsule.");

        // slidingBox: test sceneのbox GameObject。
        auto& slidingBox =
            materialScene.CreateGameObject("Sliding box");
        slidingBox.GetTransform().position =
            { 3.0f, 0.5f, 0.0f };
        slidingBox.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                DirectX::XMFLOAT3{},
                false,
                0,
                0xffffffffu,
                LamaPon::PhysicsMaterial{ 1.0f, 0.0f });
        // slidingBody: Rigidbody component。
        auto& slidingBody =
            slidingBox.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{ 3.0f, 0.0f, 0.0f });
        // producedRollingMotion: rolling motion発生結果。
        bool producedRollingMotion{};
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 120; ++step)
        {
            materialScene.Update(1.0f / 60.0f);
            producedRollingMotion =
                producedRollingMotion
                || std::abs(
                    slidingBody.AngularVelocity().z)
                    > 0.1f;
        }
        Require(
            std::abs(slidingBody.Velocity().x) < 3.0f
                && producedRollingMotion,
            "Physics material friction did not produce rolling motion.");

        // angularScene: 回転物理の検証scene。
        LamaPon::Scene angularScene(graphics);
        // torqueBox: test sceneのbox GameObject。
        auto& torqueBox =
            angularScene.CreateGameObject("Torque box");
        torqueBox.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 2.0f, 2.0f, 2.0f });
        // torqueBody: Rigidbody component。
        auto& torqueBody =
            torqueBox.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{},
                    false,
                    false,
                    LamaPon::CollisionDetectionMode::Discrete,
                    2.0f);
        torqueBody.AddForceAtPosition(
            DirectX::XMFLOAT3{ 0.0f, 2.0f, 0.0f },
            DirectX::XMFLOAT3{ 1.0f, 0.0f, 0.0f },
            LamaPon::ForceMode::Impulse);
        Require(
            NearlyEqual(torqueBody.Velocity().y, 1.0f)
                && torqueBody.AngularVelocity().z > 0.1f,
            "Off-center impulse did not create linear and angular velocity.");
        angularScene.Update(0.1f);
        Require(
            torqueBox.GetTransform().EulerAngles().z > 0.01f,
            "Angular velocity did not rotate the GameObject.");
        torqueBody.SetConstraints(
            LamaPon::RigidbodyConstraints{
                false,
                false,
                true
            });
        torqueBody.SetAngularVelocity({});
        torqueBody.AddTorque(
            DirectX::XMFLOAT3{ 0.0f, 0.0f, 10.0f },
            LamaPon::ForceMode::Impulse);
        Require(
            NearlyEqual(
                torqueBody.AngularVelocity().z,
                0.0f),
            "Freeze Rotation did not block angular impulse.");

        // ledgeScene: ledge collision検証scene。
        LamaPon::Scene ledgeScene(graphics);
        // ledge: test sceneのGameObject。
        auto& ledge =
            ledgeScene.CreateGameObject("Ledge");
        ledge.GetTransform().position =
            { 0.0f, -0.5f, 0.0f };
        ledge.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 4.0f, 1.0f, 4.0f });
        // ledgeBox: test sceneのbox GameObject。
        auto& ledgeBox =
            ledgeScene.CreateGameObject("Ledge box");
        ledgeBox.GetTransform().position =
            { 2.15f, 0.5f, 0.0f };
        ledgeBox.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        ledgeBox.AddComponent<
            LamaPon::RigidbodyComponent>();
        // maximumLedgeRotation: ledge上の最大回転角。
        float maximumLedgeRotation{};
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 120; ++step)
        {
            ledgeScene.Update(1.0f / 60.0f);
            maximumLedgeRotation = std::max(
                maximumLedgeRotation,
                std::abs(
                    ledgeBox.GetTransform()
                        .EulerAngles().z));
        }
        Require(
            maximumLedgeRotation > 0.05f,
            "An off-center ledge contact did not tip the body.");

        // stackScene: 積層physics検証scene。
        LamaPon::Scene stackScene(graphics);
        // stackGround: test sceneのground GameObject。
        auto& stackGround =
            stackScene.CreateGameObject("Stack ground");
        stackGround.GetTransform().position =
            { 0.0f, -0.5f, 0.0f };
        stackGround.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{
                    10.0f,
                    1.0f,
                    10.0f });
        std::array<LamaPon::GameObject*, 3>
            stackedBoxes{};
        std::array<
            LamaPon::RigidbodyComponent*,
            3> stackedBodies{};
        // 対象bufferの各indexを順に処理します。
        for (std::size_t index{};
            index < stackedBoxes.size();
            ++index)
        {
            // box: test sceneのbox GameObject。
            auto& box = stackScene.CreateGameObject(
                "Stack box "
                + std::to_string(index));
            box.GetTransform().position = {
                0.0f,
                0.5f
                    + static_cast<float>(index),
                0.0f
            };
            box.AddComponent<
                LamaPon::BoxCollider3DComponent>();
            stackedBoxes[index] = &box;
            stackedBodies[index] =
                &box.AddComponent<
                    LamaPon::RigidbodyComponent>();
        }
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 600; ++step)
        {
            stackScene.Update(1.0f / 60.0f);
        }
        // 対象bufferの各indexを順に処理します。
        for (std::size_t index{};
            index < stackedBoxes.size();
            ++index)
        {
            Require(
                std::abs(
                    stackedBoxes[index]
                        ->GetTransform().position.y
                    - (0.5f
                        + static_cast<float>(
                            index)))
                    < 0.1f
                    && stackedBodies[index]
                        ->IsSleeping(),
                "The box stack did not settle stably.");
        }
        stackedBodies.back()->AddForce(
            DirectX::XMFLOAT3{
                1.0f,
                0.0f,
                0.0f },
            LamaPon::ForceMode::Impulse);
        Require(
            !stackedBodies.back()->IsSleeping()
                && stackedBodies.back()
                    ->Velocity().x > 0.5f,
            "An impulse did not wake a sleeping Rigidbody.");

        // jointScene: joint serialization scene。
        LamaPon::Scene jointScene(graphics);
        // fixedAnchor: test sceneのanchor GameObject。
        auto& fixedAnchor =
            jointScene.CreateGameObject("Fixed anchor");
        fixedAnchor.GetTransform().position =
            { 0.0f, 0.0f, 0.0f };
        fixedAnchor.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        // fixedBody: test sceneのGameObject。
        auto& fixedBody =
            jointScene.CreateGameObject("Fixed body");
        fixedBody.GetTransform().position =
            { 2.0f, 0.0f, 0.0f };
        fixedBody.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        // fixedRigidbody: Rigidbody component。
        auto& fixedRigidbody =
            fixedBody.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{
                        8.0f,
                        0.0f,
                        0.0f },
                    false);
        // fixedJoint: test fixtureのphysics joint component。
        auto& fixedJoint = fixedBody.AddComponent<
            LamaPon::JointComponent>(
                LamaPon::JointType::Fixed,
                fixedAnchor.Id(),
                DirectX::XMFLOAT3{
                    -2.0f,
                    0.0f,
                    0.0f });
        jointScene.Update(1.0f / 60.0f);
        Require(
            NearlyEqual(
                fixedBody.GetTransform().position.x,
                2.0f)
                && NearlyEqual(
                    fixedRigidbody.Velocity().x,
                    0.0f),
            "Fixed joint did not constrain its anchor.");
        fixedJoint.SetType(LamaPon::JointType::Hinge);
        fixedBody.GetTransform().SetEulerAngles(
            0.0f, 0.75f, 0.0f);
        fixedRigidbody.SetVelocity(
            { 5.0f, 0.0f, 0.0f });
        jointScene.Update(1.0f / 60.0f);
        Require(
            NearlyEqual(
                fixedBody.GetTransform().EulerAngles().y,
                0.75f)
                && std::isfinite(
                fixedBody.GetTransform().position.x),
            "Hinge joint did not preserve free rotation.");

        // hingeScene: hinge joint検証scene。
        LamaPon::Scene hingeScene(graphics);
        // hingeAnchor: test sceneのanchor GameObject。
        auto& hingeAnchor =
            hingeScene.CreateGameObject("Hinge anchor");
        // motorBody: test sceneのGameObject。
        auto& motorBody =
            hingeScene.CreateGameObject("Motor body");
        motorBody.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        // motorRigidbody: Rigidbody component。
        auto& motorRigidbody =
            motorBody.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{},
                    false);
        // motorJoint: test fixtureのphysics joint component。
        auto& motorJoint =
            motorBody.AddComponent<
                LamaPon::JointComponent>(
                    LamaPon::JointType::Hinge,
                    hingeAnchor.Id(),
                    DirectX::XMFLOAT3{},
                    DirectX::XMFLOAT3{},
                    DirectX::XMFLOAT3{
                        0.0f,
                        0.0f,
                        1.0f },
                    1.0f,
                    20.0f,
                    2.0f,
                    false,
                    true,
                    LamaPon::HingeLimits{
                        -20.0f,
                        20.0f
                    },
                    true,
                    LamaPon::HingeMotor{
                        180.0f,
                        100.0f
                    });
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 120; ++step)
        {
            hingeScene.Update(1.0f / 60.0f);
        }
        // radiansToDegrees: radianからdegreeへの倍率。
        constexpr float radiansToDegrees =
            180.0f / DirectX::XM_PI;
        // positiveMotorAngle: 正方向motor角度。
        const float positiveMotorAngle =
            motorJoint.HingeAngleRadians(
                hingeAnchor)
            * radiansToDegrees;
        Require(
            positiveMotorAngle > 10.0f
                && positiveMotorAngle <= 20.1f,
            "Hinge motor or upper angle limit failed.");

        motorJoint.SetMotor(
            LamaPon::HingeMotor{
                -180.0f,
                100.0f
            });
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 240; ++step)
        {
            hingeScene.Update(1.0f / 60.0f);
        }
        // negativeMotorAngle: 負方向motor角度。
        const float negativeMotorAngle =
            motorJoint.HingeAngleRadians(
                hingeAnchor)
            * radiansToDegrees;
        Require(
            negativeMotorAngle < -10.0f
                && negativeMotorAngle >= -20.1f,
            "Hinge motor or lower angle limit failed.");

        motorRigidbody.AddTorque(
            DirectX::XMFLOAT3{
                20.0f,
                0.0f,
                0.0f },
            LamaPon::ForceMode::Impulse);
        hingeScene.Update(1.0f / 60.0f);
        Require(
            std::abs(
                motorRigidbody
                    .AngularVelocity().x)
                < 0.001f,
            "Hinge did not lock rotation outside its axis.");

        // springBody: test sceneのGameObject。
        auto& springBody =
            jointScene.CreateGameObject("Spring body");
        springBody.GetTransform().position =
            { 4.0f, 3.0f, 0.0f };
        springBody.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{
                    0.5f,
                    0.5f,
                    0.5f });
        springBody.AddComponent<
            LamaPon::RigidbodyComponent>(
                DirectX::XMFLOAT3{},
                false);
        springBody.AddComponent<
            LamaPon::JointComponent>(
                LamaPon::JointType::Spring,
                fixedAnchor.Id(),
                DirectX::XMFLOAT3{},
                DirectX::XMFLOAT3{},
                DirectX::XMFLOAT3{
                    0.0f,
                    1.0f,
                    0.0f },
                1.0f,
                24.0f,
                4.0f);
        // initialSpringDistance: 初期spring長。
        const float initialSpringDistance =
            std::sqrt(25.0f);
        // 指定step数だけsimulationを進めます。
        for (int step{}; step < 30; ++step)
        {
            jointScene.Update(1.0f / 60.0f);
        }
        // springPosition: spring endpoint位置。
        const auto springPosition =
            springBody.GetTransform().position;
        // springDistance: 現在のspring長。
        const float springDistance = std::sqrt(
            springPosition.x * springPosition.x
            + springPosition.y * springPosition.y
            + springPosition.z * springPosition.z);
        Require(
            springDistance < initialSpringDistance,
            "Spring joint did not pull toward its rest length.");

        // ccdScene: CCD検証scene。
        LamaPon::Scene ccdScene(graphics);
        // thinWall: test sceneのwall GameObject。
        auto& thinWall =
            ccdScene.CreateGameObject("Thin wall");
        thinWall.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{
                    0.1f,
                    4.0f,
                    4.0f });
        // bullet: test sceneのGameObject。
        auto& bullet =
            ccdScene.CreateGameObject("CCD bullet");
        bullet.GetTransform().position =
            { -5.0f, 0.0f, 0.0f };
        bullet.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{
                    0.2f,
                    0.2f,
                    0.2f });
        // bulletBody: Rigidbody component。
        auto& bulletBody =
            bullet.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{
                        100.0f,
                        0.0f,
                        0.0f },
                    false,
                    false,
                    LamaPon::CollisionDetectionMode::Continuous);
        ccdScene.Update(0.1f);
        Require(
            bullet.GetTransform().position.x < 0.0f
                && NearlyEqual(
                    bulletBody.Velocity().x,
                    0.0f),
            "Continuous collision detection missed a thin wall.");

        // visibilityView: visibility用view行列。
        const auto visibilityView =
            DirectX::XMMatrixIdentity();
        // visibilityProjection: visibility用projection行列。
        const auto visibilityProjection =
            DirectX::XMMatrixPerspectiveFovRH(
                DirectX::XM_PIDIV4,
                16.0f / 9.0f,
                0.1f,
                500.0f);

        struct LodTestVertex final
        {
            // position: fixtureのworld position。
            DirectX::XMFLOAT3 position{};
        };
        // lodVertices: LOD mesh vertex列。
        std::vector<LodTestVertex> lodVertices;
        // lodIndices: LOD mesh index列。
        std::vector<std::uint32_t> lodIndices;
        // lodGridSize: 生成するLOD grid幅。
        constexpr std::uint32_t lodGridSize = 16;
        lodVertices.reserve(lodGridSize * lodGridSize);
        // LOD gridの各row vertexを生成します。
        for (std::uint32_t y = 0; y < lodGridSize; ++y)
        {
            // LOD gridの各column vertexを生成します。
            for (std::uint32_t x = 0; x < lodGridSize; ++x)
            {
                lodVertices.push_back({ {
                    static_cast<float>(x),
                    static_cast<float>(y),
                    std::sin(static_cast<float>(x + y) * 0.2f)
                        * 0.1f
                } });
            }
        }
        // LOD gridの各row vertexを生成します。
        for (std::uint32_t y = 0; y + 1 < lodGridSize; ++y)
        {
            // LOD gridの各column vertexを生成します。
            for (std::uint32_t x = 0; x + 1 < lodGridSize; ++x)
            {
                // topLeft: 左上のscreen座標。
                const auto topLeft = y * lodGridSize + x;
                // topRight: 右上のscreen座標。
                const auto topRight = topLeft + 1;
                // bottomLeft: 左下のscreen座標。
                const auto bottomLeft = topLeft + lodGridSize;
                // bottomRight: 右下のscreen座標。
                const auto bottomRight = bottomLeft + 1;
                lodIndices.insert(
                    lodIndices.end(),
                    {
                        topLeft, bottomLeft, topRight,
                        topRight, bottomLeft, bottomRight
                    });
            }
        }
        // generatedLods: 生成されたLOD mesh列。
        const auto generatedLods =
            LamaPon::ModelLod::BuildLevels<LodTestVertex>(
                lodVertices,
                lodIndices,
                LamaPon::Bounds3D{
                    { 0.0f, 0.0f, -0.1f },
                    {
                        static_cast<float>(lodGridSize - 1),
                        static_cast<float>(lodGridSize - 1),
                        0.1f
                    }
                });
        Require(
            !generatedLods[0].empty()
                && !generatedLods[1].empty()
                && generatedLods[0].size() < lodIndices.size()
                && generatedLods[1].size()
                    < generatedLods[0].size(),
            "Automatic model LOD generation did not reduce geometry.");

        // lodScene: LOD serialization scene。
        LamaPon::Scene lodScene(graphics);
        lodScene.SetFrustumCullingEnabled(
            false);
        lodScene.SetOcclusionCullingEnabled(
            false);
        // lodRoot: test sceneのroot GameObject。
        auto& lodRoot =
            lodScene.CreateGameObject("LOD root");
        lodRoot.GetTransform().position =
            { 0.0f, 0.0f, -10.0f };
        // lodHigh: test sceneのGameObject。
        auto& lodHigh =
            lodScene.CreateGameObject("LOD high");
        lodHigh.SetParent(&lodRoot);
        lodHigh.AddComponent<
            LamaPon::MeshRendererComponent>();
        // lodLow: test sceneのGameObject。
        auto& lodLow =
            lodScene.CreateGameObject("LOD low");
        lodLow.SetParent(&lodRoot);
        lodLow.AddComponent<
            LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube);
        lodRoot.AddComponent<
            LamaPon::LODGroupComponent>(
                std::vector<LamaPon::LODLevel>{
                    { 20.0f, lodHigh.Id() },
                    { 50.0f, lodLow.Id() }
                },
                80.0f);
        // lodStats: LOD mesh統計。
        auto lodStats =
            lodScene.EvaluateRenderVisibility(
                visibilityView,
                visibilityProjection);
        Require(
            lodStats.rendererCount == 2
                && lodStats.visibleRendererCount
                    == 1
                && lodStats.lodGroupCount == 1
                && lodStats.lodCulledCount == 1,
            "LOD Group did not select one renderer.");
        lodRoot.GetTransform().position.z =
            -100.0f;
        lodStats =
            lodScene.EvaluateRenderVisibility(
                visibilityView,
                visibilityProjection);
        Require(
            lodStats.visibleRendererCount == 0
                && lodStats.lodCulledCount == 2,
            "LOD cull distance did not hide the group.");

        // frustumScene: frustum culling検証scene。
        LamaPon::Scene frustumScene(graphics);
        frustumScene.SetOcclusionCullingEnabled(
            false);
        // visibleMesh: test sceneのGameObject。
        auto& visibleMesh =
            frustumScene.CreateGameObject(
                "Visible mesh");
        visibleMesh.GetTransform().position =
            { 0.0f, 0.0f, -5.0f };
        visibleMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        // outsideMesh: test sceneのGameObject。
        auto& outsideMesh =
            frustumScene.CreateGameObject(
                "Outside mesh");
        outsideMesh.GetTransform().position =
            { 100.0f, 0.0f, -5.0f };
        outsideMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        // alwaysVisibleMesh: test sceneのGameObject。
        auto& alwaysVisibleMesh =
            frustumScene.CreateGameObject(
                "Always visible mesh");
        alwaysVisibleMesh.GetTransform().position =
            { 120.0f, 0.0f, -5.0f };
        alwaysVisibleMesh.AddComponent<
            LamaPon::RenderCullingComponent>(true);
        alwaysVisibleMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        // marginMesh: test sceneのGameObject。
        auto& marginMesh =
            frustumScene.CreateGameObject(
                "Margin mesh");
        marginMesh.GetTransform().position =
            { 10.0f, 0.0f, -5.0f };
        marginMesh.AddComponent<
            LamaPon::RenderCullingComponent>(false, 7.0f);
        marginMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        // frustumStats: frustum判定統計。
        const auto frustumStats =
            frustumScene.EvaluateRenderVisibility(
                visibilityView,
                visibilityProjection);
        Require(
            frustumStats.rendererCount == 4
                && frustumStats.
                    visibleRendererCount == 3
                && frustumStats.
                    frustumCulledCount == 1,
            "Per-GameObject frustum culling settings were not applied.");

        // spatialScene: spatial hash検証scene。
        LamaPon::Scene spatialScene(graphics);
        spatialScene.SetOcclusionCullingEnabled(false);
        // spatialVisible: test sceneのGameObject。
        auto& spatialVisible =
            spatialScene.CreateGameObject("Spatial visible");
        spatialVisible.GetTransform().position =
            { 0.0f, 0.0f, -5.0f };
        spatialVisible.AddComponent<
            LamaPon::MeshRendererComponent>();
        // 対象bufferの各indexを順に処理します。
        for (int index = 0; index < 128; ++index)
        {
            // spatialOutside: test sceneのGameObject。
            auto& spatialOutside =
                spatialScene.CreateGameObject(
                    "Spatial outside "
                    + std::to_string(index));
            spatialOutside.GetTransform().position = {
                1000.0f + static_cast<float>(index) * 3.0f,
                static_cast<float>(index % 8),
                -5.0f
            };
            spatialOutside.AddComponent<
                LamaPon::MeshRendererComponent>();
        }
        // firstSpatialStats: 初回spatial hash統計。
        const auto firstSpatialStats =
            spatialScene.EvaluateRenderVisibility(
                visibilityView,
                visibilityProjection);
        // reusedSpatialStats: 再利用後のspatial hash統計。
        const auto reusedSpatialStats =
            spatialScene.EvaluateRenderVisibility(
                visibilityView,
                visibilityProjection);
        Require(
            firstSpatialStats.rendererCount == 129
                && firstSpatialStats.visibleRendererCount == 1
                && firstSpatialStats.frustumCulledCount == 128
                && firstSpatialStats.spatialNodeCount > 1
                && firstSpatialStats.spatialNodeTestCount
                    < firstSpatialStats.rendererCount,
            "Render BVH did not prune the off-screen object cluster.");
        Require(
            !firstSpatialStats.spatialIndexReused
                && reusedSpatialStats.spatialIndexReused,
            "Render BVH was rebuilt even though bounds were unchanged.");

        // lowQuality: low quality LOD。
        const auto lowQuality =
            LamaPon::GraphicsSettingsForPreset(
                LamaPon::GraphicsQualityPreset::Low);
        // mediumQuality: medium quality LOD。
        const auto mediumQuality =
            LamaPon::GraphicsSettingsForPreset(
                LamaPon::GraphicsQualityPreset::Medium);
        // highQuality: high quality LOD。
        const auto highQuality =
            LamaPon::GraphicsSettingsForPreset(
                LamaPon::GraphicsQualityPreset::High);
        // ultraQuality: ultra quality LOD。
        const auto ultraQuality =
            LamaPon::GraphicsSettingsForPreset(
                LamaPon::GraphicsQualityPreset::Ultra);
        Require(
            lowQuality.renderScale < mediumQuality.renderScale
                && mediumQuality.renderScale
                    < highQuality.renderScale
                && lowQuality.automaticLodQuality
                    < mediumQuality.automaticLodQuality
                && mediumQuality.automaticLodQuality
                    < highQuality.automaticLodQuality
                && highQuality.automaticLodQuality
                    < ultraQuality.automaticLodQuality
                && lowQuality.runtimeTextureCompression
                && mediumQuality.runtimeTextureCompression
                && !ultraQuality.runtimeTextureCompression,
            "Graphics quality presets must scale resolution, LOD, and texture memory monotonically.");
        // invalidLodQuality: 不正LOD quality値。
        auto invalidLodQuality = highQuality;
        invalidLodQuality.automaticLodQuality = 8.0f;
        Require(
            LamaPon::ClampGraphicsSettings(
                invalidLodQuality).automaticLodQuality == 2.0f,
            "Automatic LOD quality was not clamped.");

        // occlusionScene: occlusion検証scene。
        LamaPon::Scene occlusionScene(graphics);
        // occluder: test sceneのGameObject。
        auto& occluder =
            occlusionScene.CreateGameObject(
                "Occluder");
        occluder.GetTransform().position =
            { 0.0f, 0.0f, -5.0f };
        occluder.GetTransform().scale =
            { 8.0f, 8.0f, 1.0f };
        occluder.AddComponent<
            LamaPon::MeshRendererComponent>();
        // hiddenMesh: test sceneのGameObject。
        auto& hiddenMesh =
            occlusionScene.CreateGameObject(
                "Hidden mesh");
        hiddenMesh.GetTransform().position =
            { 0.0f, 0.0f, -10.0f };
        hiddenMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        // alwaysVisibleHiddenMesh: test sceneのGameObject。
        auto& alwaysVisibleHiddenMesh =
            occlusionScene.CreateGameObject(
                "Always visible hidden mesh");
        alwaysVisibleHiddenMesh.GetTransform().position =
            { 0.0f, 0.0f, -12.0f };
        alwaysVisibleHiddenMesh.AddComponent<
            LamaPon::RenderCullingComponent>(true);
        alwaysVisibleHiddenMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        // occlusionStats: occlusion判定統計。
        const auto occlusionStats =
            occlusionScene.
                EvaluateRenderVisibility(
                    visibilityView,
                    visibilityProjection);
        Require(
            occlusionStats.rendererCount == 3
                && occlusionStats.
                    visibleRendererCount == 2
                && occlusionStats.
                    occlusionCulledCount == 1,
            "Always-visible renderer did not bypass occlusion culling.");

        // triggerScene: trigger collision検証scene。
        LamaPon::Scene triggerScene(graphics);
        // triggerArea: test sceneのtrigger GameObject。
        auto& triggerArea = triggerScene.CreateGameObject("2Dトリガー");
        triggerArea.AddComponent<LamaPon::BoxCollider2DComponent>(
            DirectX::XMFLOAT2{ 10.0f, 10.0f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            true,
            5,
            1u << 6);

        // triggerVisitor: test sceneのtrigger GameObject。
        auto& triggerVisitor = triggerScene.CreateGameObject("訪問者");
        triggerVisitor.AddComponent<LamaPon::BoxCollider2DComponent>(
            DirectX::XMFLOAT2{ 1.0f, 1.0f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            false,
            6,
            1u << 5);
        // triggerProbe: collision probe component。
        auto& triggerProbe = triggerVisitor.AddComponent<CollisionProbeComponent>();
        triggerScene.Update(0.0f);
        // isTriggerの接触はOnTrigger*へ届く（OnCollision*には届かない）
        Require(
            triggerProbe.triggerEnterCount == 1
                && triggerProbe.enterCount == 0,
            "2D trigger enter was not reported.");
        Require(
            NearlyEqual(triggerVisitor.GetTransform().position.x, 0.0f),
            "2D trigger incorrectly resolved position.");

        triggerVisitor.TranslateWorld({ 20.0f, 0.0f, 0.0f });
        triggerScene.Update(0.0f);
        Require(
            triggerProbe.triggerExitCount == 1,
            "2D trigger exit was not reported.");

        // filteredVisitor: test sceneのvisitor GameObject。
        auto& filteredVisitor = triggerScene.CreateGameObject("除外対象");
        filteredVisitor.AddComponent<LamaPon::BoxCollider2DComponent>(
            DirectX::XMFLOAT2{ 1.0f, 1.0f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            false,
            7,
            0u);
        // filteredProbe: collision probe component。
        auto& filteredProbe = filteredVisitor.AddComponent<CollisionProbeComponent>();
        triggerScene.Update(0.0f);
        Require(
            filteredProbe.enterCount == 0,
            "Collision mask did not filter the 2D collision.");

        std::vector<LamaPon::Bounds3D>
            broadPhaseBounds;
        broadPhaseBounds.push_back({
            { -0.5f, -0.5f, -0.5f },
            { 0.5f, 0.5f, 0.5f }
        });
        broadPhaseBounds.push_back({
            { -0.25f, -0.5f, -0.5f },
            { 0.75f, 0.5f, 0.5f }
        });
        // 100回の保存更新を順に検証します。
        for (int index = 1; index <= 100; ++index)
        {
            // position: transform位置。
            const float position =
                static_cast<float>(index) * 10.0f;
            broadPhaseBounds.push_back({
                {
                    position - 0.5f,
                    -0.5f,
                    -0.5f
                },
                {
                    position + 0.5f,
                    0.5f,
                    0.5f
                }
            });
        }
        // broadPhase: broad-phase query結果。
        const auto broadPhase =
            LamaPon::BuildSpatialHashPairs(
                broadPhaseBounds,
                2.0f);
        Require(
            broadPhase.pairs.size() == 1
                && broadPhase.pairs.front().left == 0
                && broadPhase.pairs.front().right == 1,
            "Spatial hash did not reduce separated 3D pairs.");

        // oversizedBounds: 広域objectのbounds。
        const std::array oversizedBounds{
            LamaPon::Bounds3D{
                { -10000.0f, -10000.0f, -10000.0f },
                { 10000.0f, 10000.0f, 10000.0f }
            },
            LamaPon::Bounds3D{
                { 20000.0f, 0.0f, 0.0f },
                { 20001.0f, 1.0f, 1.0f }
            }
        };
        // oversizedBroadPhase: 広域objectのbroad-phase結果。
        const auto oversizedBroadPhase =
            LamaPon::BuildSpatialHashPairs(
                oversizedBounds,
                1.0f);
        Require(
            oversizedBroadPhase.oversizedColliderCount == 1
                && oversizedBroadPhase.pairs.size() == 1,
            "Oversized collider fallback did not preserve candidates.");

        // broadPhaseScene: broad-phase検証scene。
        LamaPon::Scene broadPhaseScene(graphics);
        // 64個のfixture要素を順に生成します。
        for (int index = 0; index < 64; ++index)
        {
            // object: test sceneのGameObject。
            auto& object =
                broadPhaseScene.CreateGameObject(
                    "分離Collider");
            object.GetTransform().position.x =
                static_cast<float>(index) * 10.0f;
            object.AddComponent<
                LamaPon::BoxCollider3DComponent>();
        }
        broadPhaseScene.Update(0.0f);
        // broadPhaseStats: broad-phase statistics。
        const auto& broadPhaseStats =
            broadPhaseScene.PhysicsStats();
        Require(
            broadPhaseStats.colliderCount3D == 64
                && broadPhaseStats.candidatePairCount3D == 0
                && broadPhaseStats.narrowPhaseTestCount3D == 0,
            "Scene physics did not use spatial broad phase.");

        // raycastBounds: raycast対象bounds。
        const LamaPon::Bounds3D raycastBounds{
            { -1.0f, -1.0f, -1.0f },
            { 1.0f, 1.0f, 1.0f }
        };
        // hitDistance: ray hitまでの距離。
        float hitDistance{};
        Require(
            LamaPon::RayIntersectsBounds(
                {
                    { 0.0f, 0.0f, 5.0f },
                    { 0.0f, 0.0f, -1.0f }
                },
                raycastBounds,
                hitDistance),
            "Ray did not hit the AABB.");
        Require(NearlyEqual(hitDistance, 4.0f), "Ray hit distance is incorrect.");
        Require(
            !LamaPon::RayIntersectsBounds(
                {
                    { 3.0f, 0.0f, 5.0f },
                    { 0.0f, 0.0f, -1.0f }
                },
                raycastBounds,
                hitDistance),
            "Ray incorrectly hit the AABB.");

        // transformedBounds: transform後のbounds。
        const auto transformedBounds = LamaPon::TransformBounds(
            raycastBounds,
            DirectX::XMMatrixScaling(2.0f, 1.0f, 0.5f)
                * DirectX::XMMatrixTranslation(4.0f, 2.0f, -3.0f));
        Require(
            NearlyEqual(transformedBounds.minimum.x, 2.0f)
                && NearlyEqual(transformedBounds.maximum.x, 6.0f)
                && NearlyEqual(transformedBounds.minimum.y, 1.0f)
                && NearlyEqual(transformedBounds.maximum.y, 3.0f)
                && NearlyEqual(transformedBounds.minimum.z, -3.5f)
                && NearlyEqual(transformedBounds.maximum.z, -2.5f),
            "Transformed AABB is incorrect.");

        // queryScene: spatial query検証scene。
        LamaPon::Scene queryScene(graphics);
        // queryNear: test sceneのGameObject。
        auto& queryNear =
            queryScene.CreateGameObject("Query near");
        queryNear.GetTransform().position =
            { 0.0f, 0.0f, -2.0f };
        queryNear.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                DirectX::XMFLOAT3{},
                false,
                3);
        // queryFar: test sceneのGameObject。
        auto& queryFar =
            queryScene.CreateGameObject("Query far");
        queryFar.GetTransform().position =
            { 0.0f, 0.0f, -5.0f };
        queryFar.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                DirectX::XMFLOAT3{},
                false,
                3);
        // queryTrigger: test sceneのtrigger GameObject。
        auto& queryTrigger =
            queryScene.CreateGameObject("Query trigger");
        queryTrigger.GetTransform().position =
            { 0.0f, 0.0f, -1.0f };
        queryTrigger.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 0.25f, 0.25f, 0.25f },
                DirectX::XMFLOAT3{},
                true,
                3);
        // queryCapsule: test sceneのGameObject。
        auto& queryCapsule =
            queryScene.CreateGameObject("Query capsule");
        queryCapsule.GetTransform().position =
            { 3.0f, 0.0f, -3.0f };
        queryCapsule.AddComponent<
            LamaPon::CapsuleCollider3DComponent>(
                0.5f,
                2.0f,
                DirectX::XMFLOAT3{},
                false,
                3);
        // querySphere: test sceneのsphere GameObject。
        auto& querySphere =
            queryScene.CreateGameObject("Query sphere");
        querySphere.GetTransform().position =
            { -3.0f, 0.0f, -4.0f };
        querySphere.AddComponent<
            LamaPon::SphereCollider3DComponent>(
                0.75f,
                DirectX::XMFLOAT3{},
                false,
                3);

        // physicsHit: physics query hit。
        LamaPon::PhysicsHit physicsHit{};
        // queryRay: spatial query用ray。
        const LamaPon::Ray queryRay{
            { 0.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, -10.0f }
        };
        Require(
            queryScene.Raycast(
                queryRay,
                20.0f,
                physicsHit,
                { 1u << 3, false, 0 }),
            "Scene raycast did not find a collider.");
        Require(
            physicsHit.gameObject == &queryNear
                && NearlyEqual(physicsHit.distance, 1.5f)
                && NearlyEqual(physicsHit.normal.z, 1.0f),
            "Scene raycast returned an incorrect hit.");
        // allPhysicsHits: raycastで得た全hit。
        const auto allPhysicsHits =
            queryScene.RaycastAll(
                queryRay,
                20.0f,
                { 1u << 3, false, 0 });
        Require(
            allPhysicsHits.size() == 2
                && allPhysicsHits[0].gameObject == &queryNear
                && allPhysicsHits[1].gameObject == &queryFar,
            "RaycastAll was not sorted or filtered.");
        Require(
            queryScene.SphereCast(
                queryRay,
                0.5f,
                20.0f,
                physicsHit,
                { 1u << 3, false, 0 })
                && physicsHit.gameObject == &queryNear
                && NearlyEqual(physicsHit.distance, 1.0f),
            "SphereCast returned an incorrect hit.");
        Require(
            queryScene.OverlapBox(
                {
                    { -0.6f, -0.6f, -2.6f },
                    { 0.6f, 0.6f, -1.4f }
                },
                { 1u << 3, false, 0 }).size() == 1,
            "OverlapBox returned an incorrect result.");
        Require(
            queryScene.OverlapSphere(
                { 0.0f, 0.0f, -5.0f },
                0.25f,
                { 1u << 3, false, 0 }).size() == 1,
            "OverlapSphere returned an incorrect result.");
        Require(
            queryScene.Raycast(
                {
                    { 3.0f, 0.0f, 0.0f },
                    { 0.0f, 0.0f, -1.0f }
                },
                20.0f,
                physicsHit,
                { 1u << 3, false, 0 })
                && physicsHit.gameObject == &queryCapsule
                && physicsHit.collider == nullptr
                && physicsHit.capsuleCollider != nullptr,
            "Physics query did not return a capsule collider.");
        // capsuleOverlap: capsule overlapの検出結果。
        const auto capsuleOverlap =
            queryScene.OverlapSphere(
                { 3.0f, 0.0f, -3.0f },
                0.25f,
                { 1u << 3, false, 0 });
        Require(
            capsuleOverlap.size() == 1
                && capsuleOverlap.front().gameObject
                    == &queryCapsule
                && capsuleOverlap.front().capsuleCollider
                    != nullptr,
            "OverlapSphere did not return a capsule collider.");
        Require(
            queryScene.Raycast(
                {
                    { -3.0f, 0.0f, 0.0f },
                    { 0.0f, 0.0f, -1.0f }
                },
                20.0f,
                physicsHit,
                { 1u << 3, false, 0 })
                && physicsHit.gameObject == &querySphere
                && physicsHit.collider == nullptr
                && physicsHit.capsuleCollider == nullptr
                && physicsHit.sphereCollider != nullptr,
            "Physics query did not return a sphere collider.");
        // sphereOverlap: sphere overlap結果。
        const auto sphereOverlap =
            queryScene.OverlapSphere(
                { -3.0f, 0.0f, -4.0f },
                0.25f,
                { 1u << 3, false, 0 });
        Require(
            sphereOverlap.size() == 1
                && sphereOverlap.front().gameObject
                    == &querySphere
                && sphereOverlap.front().sphereCollider
                    != nullptr,
            "OverlapSphere did not return a sphere collider.");

        // controllerScene: controller検証scene。
        LamaPon::Scene controllerScene(graphics);
        // controllerGround: test sceneのground GameObject。
        auto& controllerGround =
            controllerScene.CreateGameObject("Controller ground");
        controllerGround.GetTransform().position =
            { 0.0f, -0.5f, 0.0f };
        controllerGround.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 20.0f, 1.0f, 20.0f },
                DirectX::XMFLOAT3{},
                false,
                1,
                1u << 2);
        // controllerWall: test sceneのwall GameObject。
        auto& controllerWall =
            controllerScene.CreateGameObject("Controller wall");
        controllerWall.GetTransform().position =
            { 2.0f, 1.5f, 0.0f };
        controllerWall.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 1.0f, 3.0f, 4.0f },
                DirectX::XMFLOAT3{},
                false,
                1,
                1u << 2);
        // player: test sceneのGameObject。
        auto& player =
            controllerScene.CreateGameObject("Player");
        player.GetTransform().position =
            { 0.0f, 2.0f, 0.0f };
        // controller: playerのCharacterController component。
        auto& controller = player.AddComponent<
            LamaPon::CharacterControllerComponent>();
        controller.SetUseInput(false);
        controller.SetCollisionMask(1u << 1);

        // 指定step数だけsimulationを進めます。
        for (int step = 0; step < 240; ++step)
        {
            controllerScene.Update(1.0f / 60.0f);
        }
        Require(
            controller.IsGrounded()
                && std::abs(
                    player.GetTransform().position.y)
                    < 0.08f,
            "Character Controller did not settle on the ground.");

        controller.Move({ 5.0f, 0.0f, 0.0f });
        controllerScene.Update(0.0f);
        Require(
            player.GetTransform().position.x < 1.2f,
            "Character Controller passed through a wall.");
        // groundedHeight: ground接触時の高さ。
        const float groundedHeight =
            player.GetTransform().position.y;
        controller.Jump();
        controllerScene.Update(1.0f / 60.0f);
        Require(
            !controller.IsGrounded()
                && player.GetTransform().position.y
                    > groundedHeight,
            "Character Controller jump failed.");

        // controllerRoundTrip: controller再読込後のscene。
        LamaPon::Scene controllerRoundTrip(graphics);
        controllerRoundTrip.LoadFromJson(
            controllerScene.SerializeToJson());
        // restoredController: 読み込み後のGameObject。
        const auto* restoredController =
            controllerRoundTrip.FindGameObject(
                player.Id())->GetComponent<
                    LamaPon::CharacterControllerComponent>();
        Require(
            restoredController != nullptr
                && !restoredController->UseInput()
                && restoredController->CollisionMask()
                    == (1u << 1)
                && restoredController->JumpAction()
                    == "Jump",
            "Character Controller serialization failed.");

        // navigationScene: NavMesh検証scene。
        LamaPon::Scene navigationScene(graphics);
        // navigationObject: test sceneのGameObject。
        auto& navigationObject =
            navigationScene.CreateGameObject(
                "NavMesh");
        // navigation: NavMesh component。
        auto& navigation =
            navigationObject.AddComponent<
                LamaPon::NavMeshComponent>(
                    DirectX::XMFLOAT2{
                        7.0f,
                        7.0f },
                    1.0f,
                    0.0f,
                    1.8f);
        // navigationObstacle: NavMesh obstacle。
        const std::array navigationObstacle{
            LamaPon::Bounds3D{
                {
                    -0.45f,
                    -0.1f,
                    -0.45f
                },
                {
                    0.45f,
                    1.0f,
                    0.45f
                }
            }
        };
        navigation.Bake(
            navigationObstacle);
        Require(
            navigation.IsBlocked(3, 3),
            "NavMesh bake did not rasterize an obstacle.");

        std::vector<
            LamaPon::NavMeshComponent::
                CellCoordinate> wall;
        // z layerごとにwall用NavMesh cellを構成します。
        for (std::uint32_t z{};
            z < 7;
            ++z)
        {
            // 最終z layer以外は追加対象から除きます。
            if (z != 3)
            {
                wall.emplace_back(3, z);
            }
        }
        navigation.RestoreBake(wall);
        // navigationStart: NavMeshの開始cell。
        const auto navigationStart =
            navigation.CellCenter(1, 3);
        // navigationDestination: NavMeshの目的cell。
        const auto navigationDestination =
            navigation.CellCenter(5, 3);
        // navigationPath: NavMeshの探索path。
        const auto navigationPath =
            navigation.FindPath(
                navigationStart,
                navigationDestination);
        Require(
            navigationPath.size() >= 2,
            "A* did not find the open NavMesh corridor.");

        // navigationAgentObject: test sceneのGameObject。
        auto& navigationAgentObject =
            navigationScene.CreateGameObject(
                "Agent");
        navigationAgentObject.
            GetTransform().position =
                navigationStart;
        // navigationAgent: destinationを進むNavMeshAgent。
        auto& navigationAgent =
            navigationAgentObject.AddComponent<
                LamaPon::
                    NavMeshAgentComponent>(
                        4.0f,
                        0.02f,
                        false);
        Require(
            navigationAgent.SetDestination(
                navigationDestination,
                navigation),
            "NavMeshAgent rejected a valid path.");
        // 指定step数だけsimulationを進めます。
        for (int step{};
            step < 120
                && !navigationAgent.
                    HasArrived();
            ++step)
        {
            navigationScene.Update(0.05f);
        }
        DirectX::XMFLOAT4X4
            navigationAgentWorld{};
        DirectX::XMStoreFloat4x4(
            &navigationAgentWorld,
            navigationAgentObject.
                WorldMatrix());
        Require(
            navigationAgent.HasArrived()
                && std::abs(
                    navigationAgentWorld._41
                    - navigationDestination.x)
                    < 0.05f
                && std::abs(
                    navigationAgentWorld._43
                    - navigationDestination.z)
                    < 0.05f,
            "NavMeshAgent did not follow its path.");

        wall.emplace_back(3, 3);
        navigation.RestoreBake(wall);
        Require(
            navigation.FindPath(
                navigationStart,
                navigationDestination).
                    empty(),
            "A* crossed a fully blocked NavMesh wall.");

        // パス平滑化：障害物のないグリッドでは視線が通るため経路は始点と終点の2点に縮退します。
        navigation.RestoreBake({});
        // smoothedPath: 平滑化済みNavMesh path。
        const auto smoothedPath =
            navigation.FindPath(
                navigation.CellCenter(0, 0),
                navigation.CellCenter(6, 4));
        Require(
            smoothedPath.size() == 2,
            "String pulling did not straighten an open path.");

        // 複数サーフェス：高さが最も近いNavMeshが選ばれます。
        auto& upperFloorObject =
            navigationScene.CreateGameObject(
                "上階サーフェス");
        upperFloorObject.GetTransform().position =
            { 0.0f, 6.0f, 0.0f };
        // upperNavigation: NavMesh component。
        auto& upperNavigation =
            upperFloorObject.AddComponent<
                LamaPon::NavMeshComponent>(
                DirectX::XMFLOAT2{ 7.0f, 7.0f },
                1.0f);
        upperNavigation.RestoreBake({});
        // upperAgentObject: test sceneのGameObject。
        auto& upperAgentObject =
            navigationScene.CreateGameObject(
                "上階エージェント");
        upperAgentObject.GetTransform().position =
            { 0.0f, 6.0f, 0.0f };
        // upperAgent: NavMesh agent。
        auto& upperAgent =
            upperAgentObject.AddComponent<
                LamaPon::NavMeshAgentComponent>();
        Require(
            upperAgent.FindBestNavMesh(
                DirectX::XMFLOAT3{ 2.0f, 6.0f, 2.0f })
                == &upperNavigation,
            "Agent did not pick the closest-height surface.");
        Require(
            upperAgent.SetDestination(
                DirectX::XMFLOAT3{
                    2.0f, 6.0f, 2.0f }),
            "Agent auto surface selection failed.");

        // particleScene: particle system検証scene。
        LamaPon::Scene particleScene(graphics);
        // particleObject: test sceneのGameObject。
        auto& particleObject =
            particleScene.CreateGameObject(
                "Particle probe");
        // particleProbe: particle system component。
        auto& particleProbe =
            particleObject.AddComponent<
                LamaPon::ParticleSystemComponent>(
                    12,
                    0.0f,
                    DirectX::XMFLOAT2{
                        0.1f,
                        0.1f });
        particleProbe.Emit(32);
        Require(
            particleProbe.ActiveParticleCount() == 12,
            "ParticleSystem exceeded its particle limit.");
        particleProbe.Stop(false);
        particleProbe.UpdatePreview(0.1f);
        Require(
            particleProbe.ActiveParticleCount() == 0,
            "Expired particles were not removed.");
        particleProbe.SetEmissionRate(20.0f);
        particleProbe.Restart();
        particleProbe.UpdatePreview(0.1f);
        Require(
            particleProbe.ActiveParticleCount() == 2,
            "Particle emission rate was not simulated.");

        // Update中にGameObjectやComponentを追加しても、走査中のコンテナ再確保で参照が無効にならないこと。
        {
            // spawnScene: spawn callback検証scene。
            LamaPon::Scene spawnScene(graphics);
            // spawner: test sceneのspawner GameObject。
            auto& spawner =
                spawnScene.CreateGameObject("Spawner");
            // 追加後も後続要素を走査する条件にするため、生成役を先頭に置きます。
            for (int index = 0; index < 4; ++index)
            {
                spawnScene.CreateGameObject(
                    "Bystander " + std::to_string(index));
            }

            // probe: test fixtureのtest probe component。
            auto& probe =
                spawner.AddComponent<SpawnOnUpdateProbe>();
            probe.scene = &spawnScene;

            spawnScene.Update(0.016f);

            Require(
                probe.updateCount == 1,
                "The spawning component did not update.");
            Require(
                spawnScene.GameObjects().size() == 5 + 64,
                "GameObjects created during Update were lost.");
            Require(
                spawner.Components().size() == 2,
                "A Component added during Update was lost.");

            // 2フレーム目も、増えた分をそのまま走査できること。
            spawnScene.Update(0.016f);
            Require(
                probe.updateCount == 2,
                "The scene did not survive the next frame.");
        }
        }

        // scene-manager suiteの検証を選びます。
        if (suite == "scene-manager")
        {
        // persistentRoundTrip: persistent stateの再読込scene。
        LamaPon::Scene persistentRoundTrip(
            graphics);
        // persistentRoundTripRoot: 永続化対象のroot GameObject。
        auto& persistentRoundTripRoot =
            persistentRoundTrip.CreateGameObject(
                "Persistent round-trip");
        // persistentRoundTripChild: 永続化対象のchild GameObject。
        auto& persistentRoundTripChild =
            persistentRoundTrip.CreateGameObject(
                "Persistent child");
        persistentRoundTripChild.SetParent(
            &persistentRoundTripRoot);
        persistentRoundTrip.DontDestroyOnLoad(
            persistentRoundTripRoot,
            "GameSession");
        // restoredPersistence: 復元されたpersistent設定。
        LamaPon::Scene restoredPersistence(
            graphics);
        restoredPersistence.LoadFromJson(
            persistentRoundTrip.SerializeToJson());
        // restoredPersistentRoot: 読み込み後のroot GameObject。
        auto* restoredPersistentRoot =
            restoredPersistence.FindGameObject(
                persistentRoundTripRoot.Id());
        Require(
            restoredPersistentRoot != nullptr
                && restoredPersistentRoot->IsPersistent()
                && restoredPersistentRoot->PersistenceKey()
                    == "GameSession"
                && restoredPersistentRoot->Children().size()
                    == 1,
            "Persistent GameObject settings were not restored.");
        // rejectedPersistentChild: persistent child拒否結果。
        bool rejectedPersistentChild{};
        // persistent rootのchildだけを維持する契約を確認します。
        try
        {
            persistentRoundTrip.DontDestroyOnLoad(
                persistentRoundTripChild,
                "InvalidChild");
        }
        // 拒否時の例外をテスト結果へ記録します。
        catch (const std::invalid_argument&)
        {
            rejectedPersistentChild = true;
        }
        Require(
            rejectedPersistentChild,
            "A child GameObject was incorrectly marked persistent.");

        // transitionPath: transition先scene path。
        const auto transitionPath =
            outputPath.parent_path()
            / "transition-target.scene.json";
        // transitionTarget: transition先の読み込み対象。
        LamaPon::Scene transitionTarget(
            graphics);
        transitionTarget.CreateGameObject(
            "Transition target");
        // bootstrapSession: persistent runtime session。
        auto& bootstrapSession =
            transitionTarget.CreateGameObject(
                "Bootstrap game session");
        transitionTarget.DontDestroyOnLoad(
            bootstrapSession,
            "GameSession");
        transitionTarget.SaveToFile(
            transitionPath);

        // persistentSession: persistent runtime session。
        auto& persistentSession =
            fileLoaded.CreateGameObject(
                "Runtime game session");
        persistentSession.GetTransform().position.x =
            42.0f;
        // persistentSessionChild: 永続化対象のchild GameObject。
        auto& persistentSessionChild =
            fileLoaded.CreateGameObject(
                "Runtime session child");
        persistentSessionChild.SetParent(
            &persistentSession);
        fileLoaded.DontDestroyOnLoad(
            persistentSession,
            "GameSession");
        // persistentSessionPointer: persistent session stateへのpointer。
        auto* persistentSessionPointer =
            &persistentSession;
        // persistentChildPointer: persistent session stateへのpointer。
        auto* persistentChildPointer =
            &persistentSessionChild;
        // runtimeState: runtime game state。
        auto& runtimeState =
            fileLoaded.Scenes().State();
        runtimeState.SetInteger(
            "score",
            1250);
        runtimeState.SetNumber(
            "health",
            87.5);
        runtimeState.SetBoolean(
            "bossDefeated",
            true);
        runtimeState.SetString(
            "checkpoint",
            "harbor");
        // originalObjectCount: 複製前のobject数。
        const auto originalObjectCount =
            fileLoaded.GameObjects().size();
        Require(
            fileLoaded.Scenes().RequestLoad(
                transitionPath)
                && fileLoaded.Scenes().
                    HasPendingLoad(),
            "SceneManager rejected a valid scene request.");
        Require(
            fileLoaded.GameObjects().size()
                == originalObjectCount,
            "SceneManager loaded a scene immediately during a request.");
        fileLoaded.Update(0.0f);
        Require(
            fileLoaded.GameObjects().size() == 3
                && fileLoaded.GameObjects().
                    front()->Name()
                    == "Transition target"
                && fileLoaded.Scenes().
                    CurrentScenePath()
                    == transitionPath.
                        lexically_normal()
                && fileLoaded.Scenes().
                    LoadRevision() == 1,
            "Deferred scene transition failed.");
        Require(
            fileLoaded.FindGameObject(
                persistentSessionPointer->Id())
                    == persistentSessionPointer
                && persistentSessionPointer->
                    GetTransform().position.x
                    == 42.0f
                && persistentSessionPointer->
                    Children().size() == 1
                && persistentSessionPointer->
                    Children().front()
                    == persistentChildPointer,
            "Persistent hierarchy identity or runtime state was lost.");
        Require(
            fileLoaded.Scenes().State().Integer(
                "score") == 1250
                && NearlyEqual(
                    static_cast<float>(
                        fileLoaded.Scenes().State().
                            Number("health")),
                    87.5f)
                && fileLoaded.Scenes().State().
                    Boolean("bossDefeated")
                && fileLoaded.Scenes().State().
                    String("checkpoint")
                    == "harbor",
            "Runtime game state did not survive a scene transition.");
        Require(
            std::ranges::count_if(
                fileLoaded.GameObjects(),
                [](const auto& object)
                {
                    // objectのpersistent状態を返します。
                    return object->IsPersistent()
                        && object->PersistenceKey()
                            == "GameSession";
                }) == 1,
            "Persistent bootstrap duplicate was not removed.");

        fileLoaded.CreateGameObject(
            "Runtime-only object");
        Require(
            fileLoaded.Scenes().
                RequestReload(),
            "SceneManager rejected reload.");
        fileLoaded.Update(0.0f);
        Require(
            fileLoaded.GameObjects().size() == 3
                && fileLoaded.FindGameObject(
                    persistentSessionPointer->Id())
                    == persistentSessionPointer
                && fileLoaded.Scenes().
                    LoadRevision() == 2,
            "Scene reload did not restore the file.");

        Require(
            fileLoaded.Scenes().RequestLoad(
                outputPath.parent_path()
                    / "missing.scene.json"),
            "SceneManager rejected a deferred missing path.");
        fileLoaded.Update(0.0f);
        Require(
            fileLoaded.GameObjects().size() == 3
                && fileLoaded.FindGameObject(
                    persistentSessionPointer->Id())
                    == persistentSessionPointer
                && !fileLoaded.Scenes().
                    LastError().empty()
                && fileLoaded.Scenes().
                    LoadRevision() == 2,
            "Failed scene transition did not preserve the current scene.");

        // malformedPath: 不正文書fixtureのpath。
        const auto malformedPath =
            outputPath.parent_path()
            / "malformed.scene.json";
        {
            // malformed: 不正scene JSON。
            std::ofstream malformed(
                malformedPath,
                std::ios::binary
                    | std::ios::trunc);
            malformed
                << R"({"format":"LamaPonScene","version":1,"objects":[{"id":1,"name":"broken"}]})";
        }
        Require(
            fileLoaded.Scenes().
                RequestLoad(malformedPath),
            "SceneManager rejected a malformed scene request too early.");
        fileLoaded.Update(0.0f);
        Require(
            fileLoaded.GameObjects().size() == 3
                && fileLoaded.GameObjects().
                    front()->Name()
                    == "Transition target"
                && fileLoaded.FindGameObject(
                    persistentSessionPointer->Id())
                    == persistentSessionPointer
                && !fileLoaded.Scenes().
                    LastError().empty()
                && fileLoaded.Scenes().
                    LoadRevision() == 2,
            "Scene rollback did not restore state after a partial load.");

        // asyncPath: 非同期load対象file path。
        const auto asyncPath =
            outputPath.parent_path()
            / "async-target.scene.json";
        // asyncTarget: 非同期loadの受け入れ先scene。
        LamaPon::Scene asyncTarget(graphics);
        asyncTarget.CreateGameObject(
            "Async transition target");
        asyncTarget.SaveToFile(asyncPath);
        // preloadPath: preload対象file path。
        const auto preloadPath =
            outputPath.parent_path()
            / "background-preload.bin";
        {
            // preload: preload用scene文書。
            std::ofstream preload(
                preloadPath,
                std::ios::binary
                    | std::ios::trunc);
            preload << "prefetched asset bytes";
        }
        {
            // input: 入力scene JSON。
            std::ifstream input(
                asyncPath,
                std::ios::binary);
            // asyncDocument: 非同期load用scene文書。
            auto asyncDocument =
                nlohmann::json::parse(input);
            asyncDocument["assetManifest"] =
                nlohmann::json::array({
                    LamaPon::PathToUtf8(
                        preloadPath)
                });
            // output: scene serializationの出力JSON。
            std::ofstream output(
                asyncPath,
                std::ios::binary
                    | std::ios::trunc);
            output << asyncDocument.dump(2)
                   << '\n';
        }

        // asyncScenes: 非同期load後のscene list。
        auto& asyncScenes = fileLoaded.Scenes();
        asyncScenes.SetMinimumLoadingScreenDuration(
            0.0f);
        asyncScenes.LoadingScreen().message =
            "非同期ロード中";
        asyncScenes.LoadingScreen().
            showPercentage = false;
        Require(
            asyncScenes.RequestLoadAsync(asyncPath)
                && asyncScenes.IsLoading()
                && asyncScenes.HasPendingLoad()
                && asyncScenes.PendingScenePath()
                    == asyncPath.lexically_normal(),
            "Asynchronous scene request was not started.");
        // 完了または上限到達まで再試行します。
        for (int attempt{};
            attempt < 1000
                && asyncScenes.LoadState()
                    != LamaPon::SceneLoadState::Succeeded
                && asyncScenes.LoadState()
                    != LamaPon::SceneLoadState::Failed;
            ++attempt)
        {
            fileLoaded.Update(0.0f);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        Require(
            asyncScenes.LoadState()
                    == LamaPon::SceneLoadState::Succeeded
                && NearlyEqual(
                    asyncScenes.LoadProgress(),
                    1.0f)
                && asyncScenes.LoadRevision() == 3
                && asyncScenes.
                    PrefetchedAssetCount() == 1
                && asyncScenes.
                    PrefetchedAssetBytes() > 0
                && fileLoaded.GameObjects().size()
                    == 3
                && fileLoaded.GameObjects().
                    front()->Name()
                    == "Async transition target"
                && fileLoaded.FindGameObject(
                    persistentSessionPointer->Id())
                    == persistentSessionPointer
                && asyncScenes.LoadingScreen().
                    message == "非同期ロード中"
                && !asyncScenes.LoadingScreen().
                    showPercentage,
            "Asynchronous scene activation failed.");
        std::filesystem::remove(preloadPath);
        // cachedPreload: cache済みpreload結果。
        const auto cachedPreload =
            graphics.Assets().ReadFileBytes(
                preloadPath);
        Require(
            std::string(
                cachedPreload.begin(),
                cachedPreload.end())
                == "prefetched asset bytes",
            "Background-prefetched bytes were not reused.");

        Require(
            asyncScenes.RequestLoadAsync(
                malformedPath),
            "Asynchronous malformed scene request was rejected too early.");
        // 完了または上限到達まで再試行します。
        for (int attempt{};
            attempt < 1000
                && asyncScenes.LoadState()
                    != LamaPon::SceneLoadState::Succeeded
                && asyncScenes.LoadState()
                    != LamaPon::SceneLoadState::Failed;
            ++attempt)
        {
            fileLoaded.Update(0.0f);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        Require(
            asyncScenes.LoadState()
                    == LamaPon::SceneLoadState::Failed
                && asyncScenes.LoadRevision() == 3
                && fileLoaded.GameObjects().
                    front()->Name()
                    == "Async transition target"
                && !asyncScenes.LastError().empty(),
            "Asynchronous activation rollback failed.");

        Require(
            asyncScenes.RequestLoadAsync(
                transitionPath),
            "Cancellable asynchronous load did not start.");
        asyncScenes.CancelPending();
        // 完了または上限到達まで再試行します。
        for (int attempt{};
            attempt < 100
                && asyncScenes.LoadState()
                    != LamaPon::SceneLoadState::Cancelled;
            ++attempt)
        {
            fileLoaded.Update(0.0f);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        Require(
            asyncScenes.LoadState()
                    == LamaPon::SceneLoadState::Cancelled
                && asyncScenes.LoadRevision() == 3
                && fileLoaded.GameObjects().
                    front()->Name()
                    == "Async transition target",
            "Asynchronous scene cancellation failed.");

        // Sceneのdestructorは、まだ実行中かもしれないworkerへcancelを要求してjoinしてからGraphicsDevice resource leaseを返します。
        // scopeを即座に抜けてもAssetManagerの借用が残らないことを確認します。
        {
            // shortLivedScene: 短時間で破棄するscene。
            LamaPon::Scene shortLivedScene(graphics);
            Require(
                shortLivedScene.Scenes().RequestLoadAsync(
                    transitionPath),
                "Short-lived asynchronous scene load did not start.");
        }
        Require(
            graphics.Assets().FileExists(transitionPath),
            "Destroying a loading Scene invalidated AssetManager access.");

        // シーン遷移演出: 旧シーンを覆い終えるまで新シーンを有効化せず、Started → Covered → Finishedの順にイベントを発行します。
        // 遷移はTime::UnscaledDeltaTimeの実時間で進むため、時計を明示的に進めて検査します。
        {
            // transitionHost: transitionを受けるhost scene。
            LamaPon::Scene transitionHost(graphics);
            transitionHost.CreateGameObject("Before transition");
            // transitionScenes: scene transition後のscene list。
            auto& transitionScenes = transitionHost.Scenes();
            transitionScenes.SetMinimumLoadingScreenDuration(0.0f);
            // transitionEvents: scene transition event列。
            std::vector<std::string> transitionEvents;
            // nameWhenCovered: occlusion中に表示するname。
            std::string nameWhenCovered;
            // started: task開始状態。
            const auto started = transitionHost.Events().Subscribe(
                LamaPon::SceneTransitionStartedEvent,
                [&transitionEvents](const LamaPon::EventArgs& args)
                {
                    transitionEvents.push_back("started:" + args.text);
                });
            // covered: 遮蔽判定の結果。
            const auto covered = transitionHost.Events().Subscribe(
                LamaPon::SceneTransitionCoveredEvent,
                [&](const LamaPon::EventArgs&)
                {
                    transitionEvents.push_back("covered");
                    nameWhenCovered =
                        transitionHost.GameObjects().front()->Name();
                });
            // finished: task完了状態。
            const auto finished = transitionHost.Events().Subscribe(
                LamaPon::SceneTransitionFinishedEvent,
                [&transitionEvents](const LamaPon::EventArgs&)
                {
                    transitionEvents.push_back("finished");
                });
            // advanceFrames: 指定数のframeを進める処理。
            const auto advanceFrames =
                [&](const int frames, const float seconds)
                {
                    // 各frameのscene状態を順に更新します。
                    for (int frame{}; frame < frames; ++frame)
                    {
                        LamaPon::Time::Detail::AdvanceFrame(seconds);
                        transitionHost.Update(seconds);
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(1));
                    }
                };
            // runUntilIdle: 非同期完了まで更新する処理。
            const auto runUntilIdle = [&]()
            {
                // 各frameのscene状態を順に更新します。
                for (int frame{};
                    frame < 2000 && transitionScenes.IsTransitioning();
                    ++frame)
                {
                    advanceFrames(1, 1.0f / 60.0f);
                }
            };

            // エンジンは覆いを描かないので、時間だけを指定します。
            const auto fade = LamaPon::MakeSceneTransition(0.1f, 0.0f);
            Require(
                transitionScenes.RequestLoadAsync(transitionPath, fade)
                    && transitionScenes.IsTransitioning()
                    && transitionScenes.IsInputBlocked()
                    && transitionEvents.size() == 1
                    && transitionEvents.front()
                        == "started:"
                            + LamaPon::PathToUtf8(
                                transitionPath.lexically_normal()),
                "A transition load did not start covering.");
            // 時計を止めたまま読み込みを完了させても、覆い終えるまでは旧シーンのままです。
            for (int attempt{};
                attempt < 1000
                    && transitionScenes.LoadState()
                        != LamaPon::SceneLoadState::ReadyToActivate;
                ++attempt)
            {
                advanceFrames(1, 0.0f);
            }
            advanceFrames(3, 0.0f);
            Require(
                transitionScenes.LoadState()
                        == LamaPon::SceneLoadState::ReadyToActivate
                    && transitionHost.GameObjects().front()->Name()
                        == "Before transition"
                    && transitionScenes.TransitionPhase()
                        == LamaPon::SceneTransitionPhase::Covering,
                "A scene was activated before the transition covered the screen.");
            runUntilIdle();
            Require(
                transitionScenes.LoadState()
                        == LamaPon::SceneLoadState::Succeeded
                    && transitionHost.GameObjects().front()->Name()
                        == "Transition target"
                    && nameWhenCovered == "Before transition"
                    && transitionEvents.size() == 3
                    && transitionEvents[1] == "covered"
                    && transitionEvents[2] == "finished"
                    && !transitionScenes.IsTransitioning()
                    && !transitionScenes.IsInputBlocked()
                    && NearlyEqual(
                        transitionScenes.TransitionCoverage(),
                        0.0f),
                "The transition did not cover, activate and reveal in order.");

            // シーンを切り替えない演出でも同じイベントが届きます。
            transitionEvents.clear();
            // revisionBeforePlay: play開始前のscene revision。
            const auto revisionBeforePlay =
                transitionScenes.LoadRevision();
            Require(
                transitionScenes.PlayTransition(fade)
                    && transitionScenes.IsInputBlocked(),
                "PlayTransition did not start.");
            runUntilIdle();
            Require(
                transitionEvents.size() == 3
                    && transitionEvents[0] == "started:"
                    && transitionEvents[1] == "covered"
                    && transitionEvents[2] == "finished"
                    && transitionScenes.LoadRevision()
                        == revisionBeforePlay,
                "PlayTransition must not load a scene.");

            // blockInputを外した遷移はボタンを止めず、ResetTransitionで途中から打ち切れます。
            auto unblocked = fade;
            unblocked.blockInput = false;
            Require(
                transitionScenes.PlayTransition(unblocked)
                    && transitionScenes.IsTransitioning()
                    && !transitionScenes.IsInputBlocked(),
                "A transition without blockInput must not block buttons.");
            advanceFrames(3, 1.0f / 60.0f);
            transitionScenes.ResetTransition();
            Require(
                !transitionScenes.IsTransitioning()
                    && NearlyEqual(
                        transitionScenes.TransitionCoverage(),
                        0.0f),
                "ResetTransition must clear an active transition.");

            // 読み込みに失敗したら、覆いを開いて元のシーンへ戻ります。
            transitionEvents.clear();
            Require(
                transitionScenes.RequestLoadAsync(malformedPath, fade),
                "A failing transition load was rejected too early.");
            runUntilIdle();
            Require(
                transitionScenes.LoadState()
                        == LamaPon::SceneLoadState::Failed
                    && transitionHost.GameObjects().front()->Name()
                        == "Transition target"
                    && !transitionEvents.empty()
                    && transitionEvents.back() == "finished"
                    && !transitionScenes.IsTransitioning(),
                "A failed transition load did not reveal the previous scene.");

            // 覆っている途中のキャンセルは、今の覆い具合から開き直します。
            Require(
                transitionScenes.RequestLoadAsync(transitionPath, fade),
                "A cancellable transition load did not start.");
            advanceFrames(2, 1.0f / 60.0f);
            transitionScenes.CancelPending();
            Require(
                transitionScenes.TransitionPhase()
                    == LamaPon::SceneTransitionPhase::Revealing,
                "Cancelling a transition load must start revealing.");
            runUntilIdle();
            Require(
                !transitionScenes.IsTransitioning()
                    && transitionScenes.LoadState()
                        == LamaPon::SceneLoadState::Cancelled,
                "A cancelled transition did not finish.");

            // 遷移付きの同期読み込みも、覆い終えるまで切り替えを待ちます。
            transitionHost.GameObjects().front()->SetName("Marker");
            Require(
                transitionScenes.RequestLoad(transitionPath, fade)
                    && !transitionScenes.ProcessPending()
                    && transitionScenes.HasPendingLoad()
                    && transitionHost.GameObjects().front()->Name()
                        == "Marker",
                "A synchronous transition load did not wait for the cover.");
            runUntilIdle();
            Require(
                transitionHost.GameObjects().front()->Name()
                        == "Transition target"
                    && !transitionScenes.HasPendingLoad(),
                "A synchronous transition load did not activate.");

            // 引数なしの非同期読み込みは既定の遷移を使い、既定のすぐ切り替える遷移は従来どおり読み込み画面だけを表示します。
            Require(
                LamaPon::IsInstantSceneTransition(
                    transitionScenes.DefaultTransition()),
                "The default transition must stay instant for compatibility.");
            Require(
                transitionScenes.RequestLoadAsync(transitionPath),
                "A default transition load did not start.");
            // legacyFrame: 旧形式から復元したframe。
            const auto legacyFrame = transitionScenes.TransitionFrame();
            Require(
                legacyFrame.legacyLoadingScreen
                    && NearlyEqual(legacyFrame.loadingScreenAlpha, 1.0f),
                "A load without a timed transition must show the classic loading screen.");
            runUntilIdle();
            // 完了または上限到達まで再試行します。
            for (int attempt{};
                attempt < 1000 && transitionScenes.IsLoading();
                ++attempt)
            {
                advanceFrames(1, 1.0f / 60.0f);
            }
            transitionScenes.SetDefaultTransition(fade);
            Require(
                transitionScenes.RequestReloadAsync()
                    && NearlyEqual(
                        transitionScenes.ActiveTransition().coverDuration,
                        0.1f),
                "Reloading without arguments must use the default transition.");
            runUntilIdle();
            // 完了または上限到達まで再試行します。
            for (int attempt{};
                attempt < 1000 && transitionScenes.IsLoading();
                ++attempt)
            {
                advanceFrames(1, 1.0f / 60.0f);
            }
            transitionHost.Events().Unsubscribe(started);
            transitionHost.Events().Unsubscribe(covered);
            transitionHost.Events().Unsubscribe(finished);
            LamaPon::Time::Detail::Reset();
        }

        // 追加シーンの読み込みと破棄を検証します。
        const auto additivePath =
            outputPath.parent_path()
            / "additive-hud.scene.json";
        {
            // additiveSource: 追加読込するsource scene。
            LamaPon::Scene additiveSource(graphics);
            // hudRoot: test sceneのHUD GameObject。
            auto& hudRoot =
                additiveSource.CreateGameObject(
                    "HUD root");
            // hudChild: test sceneのHUD GameObject。
            auto& hudChild =
                additiveSource.CreateGameObject(
                    "HUD child");
            hudChild.SetParent(&hudRoot);
            additiveSource.SaveToFile(additivePath);
        }

        // additiveHost: additive sceneを受けるhost。
        LamaPon::Scene additiveHost(graphics);
        // hostRoot: test sceneのroot GameObject。
        auto& hostRoot =
            additiveHost.CreateGameObject(
                "Host root");
        // hostRootId: host scene rootのID。
        const auto hostRootId = hostRoot.Id();
        // additiveHandle: additive sceneのload要求。
        const auto additiveHandle =
            additiveHost.MergeFromFile(additivePath);
        Require(
            additiveHandle
                    != LamaPon::Scene::
                        PrimarySceneHandle()
                && additiveHost.GameObjects().size()
                    == 3
                && additiveHost.FindGameObject(
                    hostRootId) != nullptr,
            "Additive load did not keep the existing scene.");
        Require(
            additiveHost.AdditiveScenes().size() == 1
                && additiveHost.AdditiveScenes().
                    front().handle == additiveHandle
                && additiveHost.AdditiveScenes().
                    front().rootCount == 1
                && additiveHost.AdditiveScenes().
                    front().name == "additive-hud.scene"
                && additiveHost.FindAdditiveScene(
                    additivePath) == additiveHandle,
            "Additive scene registry was not updated.");

        // mergedRoot: 読み込み後のroot GameObject。
        auto* mergedRoot =
            additiveHost.FindGameObjectByName(
                "HUD root");
        // mergedChild: 読み込み後のchild GameObject。
        auto* mergedChild =
            additiveHost.FindGameObjectByName(
                "HUD child");
        Require(
            mergedRoot != nullptr
                && mergedChild != nullptr
                && mergedRoot->SourceScene()
                    == additiveHandle
                && mergedChild->SourceScene()
                    == additiveHandle
                && mergedChild->Parent() == mergedRoot
                && mergedRoot->Id() != hostRootId
                && mergedChild->Id() != hostRootId,
            "Additive objects were not reparented or reindexed.");

        // 同じシーンの二重読み込みは拒否します。
        Require(
            !additiveHost.Scenes().
                RequestLoadAdditive(additivePath),
            "Duplicate additive load was accepted.");

        // 主シーンの保存に追加シーンが混ざらないこと。
        const auto hostJson =
            additiveHost.SerializeToJson();
        Require(
            hostJson.find("HUD root")
                    == std::string::npos
                && hostJson.find("Host root")
                    != std::string::npos,
            "Additive objects leaked into the primary scene file.");

        // 破棄すると、その由来のGameObjectだけが消えること。
        Require(
            additiveHost.UnloadScene(additiveHandle)
                && additiveHost.GameObjects().size()
                    == 1
                && additiveHost.AdditiveScenes().
                    empty()
                && additiveHost.FindGameObject(
                    hostRootId) != nullptr,
            "Unloading an additive scene did not remove exactly its objects.");
        Require(
            !additiveHost.UnloadScene(
                LamaPon::Scene::
                    PrimarySceneHandle()),
            "The primary scene was unloadable.");

        // SceneManager経由の遅延追加読み込みと破棄。
        Require(
            additiveHost.Scenes().
                RequestLoadAdditive(additivePath)
                && additiveHost.GameObjects().size()
                    == 1,
            "Additive request was applied immediately.");
        additiveHost.Update(0.0f);
        Require(
            additiveHost.GameObjects().size() == 3
                && additiveHost.AdditiveScenes().
                    size() == 1,
            "Deferred additive load failed.");
        Require(
            additiveHost.Scenes().
                RequestUnload(additivePath),
            "Additive unload request was rejected.");
        additiveHost.Update(0.0f);
        Require(
            additiveHost.GameObjects().size() == 1
                && additiveHost.AdditiveScenes().
                    empty(),
            "Deferred additive unload failed.");

        // 切り替え読み込みでは追加シーンも一緒に片付きます。
        Require(
            additiveHost.Scenes().
                RequestLoadAdditive(additivePath),
            "Additive request before a replace load was rejected.");
        additiveHost.Update(0.0f);
        Require(
            additiveHost.AdditiveScenes().size() == 1,
            "Additive scene was not loaded before the replace load.");
        Require(
            additiveHost.Scenes().RequestLoad(
                transitionPath),
            "Replace load after an additive load was rejected.");
        additiveHost.Update(0.0f);
        Require(
            additiveHost.AdditiveScenes().empty()
                && additiveHost.
                    FindGameObjectByName(
                        "HUD root") == nullptr,
            "Additive scenes survived a replace load.");
        }

        // プロジェクト設定の重力がRigidbodyの速度へ反映されることを確認します。
        {
            // gravityScene: gravity設定検証scene。
            LamaPon::Scene gravityScene(graphics);
            // faller: test sceneのfaller GameObject。
            auto& faller =
                gravityScene.CreateGameObject("Faller");
            // body: Rigidbody component。
            auto& body =
                faller.AddComponent<
                    LamaPon::RigidbodyComponent>();
            // step: simulation更新step。
            constexpr float step = 1.0f / 60.0f;

            // moon: moon colliderのfixture。
            LamaPon::PhysicsSettings moon;
            moon.gravity = { 0.0f, -1.62f, 0.0f };
            LamaPon::SetActivePhysicsSettings(moon);
            body.Integrate(faller, step);
            // moonVelocity: moonの初速。
            const float moonVelocity = body.Velocity().y;

            body.SetVelocity({});
            // earth: earth colliderのfixture。
            LamaPon::PhysicsSettings earth;
            LamaPon::SetActivePhysicsSettings(earth);
            body.Integrate(faller, step);
            // earthVelocity: earthの初速。
            const float earthVelocity = body.Velocity().y;

            std::cout
                << "gravity from settings: moon="
                << moonVelocity
                << " earth=" << earthVelocity
                << std::endl;
            Require(
                moonVelocity < 0.0f && earthVelocity < 0.0f,
                "gravity must pull the body down");
            // 重力が強い設定ほど下向きの速度が大きくなること。
            Require(
                earthVelocity < moonVelocity,
                "a stronger gravity must accelerate the body"
                " more; if these match, the project setting"
                " never reached the integrator");
            Require(
                std::abs(moonVelocity - (-1.62f * step))
                    < 1e-4f,
                "the body must accelerate by exactly the"
                " configured gravity");

            // 重力がY軸へ固定されず、横向きにも適用されること。
            body.SetVelocity({});
            // sideways: 横方向移動の状態。
            LamaPon::PhysicsSettings sideways;
            sideways.gravity = { 4.0f, 0.0f, 0.0f };
            LamaPon::SetActivePhysicsSettings(sideways);
            body.Integrate(faller, step);
            Require(
                std::abs(body.Velocity().x - 4.0f * step)
                        < 1e-4f
                    && std::abs(body.Velocity().y) < 1e-4f,
                "gravity must work on every axis, not just Y");

            LamaPon::SetActivePhysicsSettings(
                LamaPon::PhysicsSettings{});
        }

        // DCDの速度制限は有効時だけ適用し、既定では警告のみにします。
        {
            // clampScene: 物理値clamp検証scene。
            LamaPon::Scene clampScene(graphics);
            // fast: test sceneのGameObject。
            auto& fast = clampScene.CreateGameObject("Fast");
            // body: Rigidbody component。
            auto& body =
                fast.AddComponent<
                    LamaPon::RigidbodyComponent>();
            body.SetUseGravity(false);
            // step: simulation更新step。
            constexpr float step = 1.0f / 60.0f;
            // quick: test用player velocity vector。
            const DirectX::XMFLOAT3 quick{ 100.0f, 0.0f, 0.0f };

            // 既定（頭打ちオフ）。
            // 速いままであること。
            LamaPon::PhysicsSettings warnOnly;
            warnOnly.discreteSafeSpeed = 10.0f;
            LamaPon::SetActivePhysicsSettings(warnOnly);
            body.SetVelocity(quick);
            body.Integrate(fast, step);
            // unclamped: clamp前のphysics値。
            const float unclamped = body.Velocity().x;

            // 制限を有効にすると、しきい値まで速度が下がること。
            LamaPon::PhysicsSettings clamped = warnOnly;
            clamped.clampDiscreteSpeed = true;
            LamaPon::SetActivePhysicsSettings(clamped);
            body.SetVelocity(quick);
            body.Integrate(fast, step);
            // limited: 最大velocity制限結果。
            const float limited = body.Velocity().x;

            // CCDを選んだ物体は対象外であること。
            body.SetCollisionDetection(
                LamaPon::CollisionDetectionMode::Continuous);
            body.SetVelocity(quick);
            body.Integrate(fast, step);
            // continuous: continuous collision検出結果。
            const float continuous = body.Velocity().x;

            std::cout
                << "dcd speed limit: default=" << unclamped
                << " clamped=" << limited
                << " ccd=" << continuous
                << std::endl;
            Require(
                unclamped > 90.0f,
                "the default must only warn, never change the"
                " velocity");
            Require(
                std::abs(limited - 10.0f) < 0.5f,
                "turning the clamp on must limit the speed to"
                " the configured value");
            Require(
                continuous > 90.0f,
                "a body using CCD must never be clamped;"
                " continuous detection already protects it");

            LamaPon::SetActivePhysicsSettings(
                LamaPon::PhysicsSettings{});
        }

        std::cout << "Scene test suite passed: " << suite << '\n';
        // 全suite成功をprocess exit codeで示します。
        return 0;
    }
    // 拒否時の例外をテスト結果へ記録します。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        // test failureをprocess exit codeで示します。
        return 1;
    }
}

// main(argumentCount: 引数数, arguments: 引数列): suiteを選んでprocess終了codeを返します。
int main(const int argumentCount, char** arguments)
{
    // suite: 実行対象suite名。
    std::string_view suite = "serialization";
    // suite引数の有無に応じてCLIを解析します。
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--suite")
    {
        suite = arguments[2];
    }
    // --suite以外の引数形式を拒否します。
    else if (argumentCount != 1)
    {
        std::cerr
            << "Usage: LamaPonSceneTests "
               "[--suite serialization|simulation|scene-manager]\n";
        // invalid CLI argumentsをexit codeで示します。
        return 2;
    }
    // 未対応suite名を拒否します。
    if (suite != "serialization"
        && suite != "simulation"
        && suite != "scene-manager")
    {
        std::cerr << "Unknown scene test suite: " << suite << '\n';
        // invalid CLI argumentsをexit codeで示します。
        return 2;
    }

    // comResult: COM initialization result。
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // uninitializeCom: COM初期化解除が必要な状態。
    const bool uninitializeCom = SUCCEEDED(comResult);

    // Sceneの読み込みで遅延生成されるCOMオブジェクトを先に破棄するため、RunTestから戻った後にCoUninitializeを呼びます。
    const int exitCode = RunTest(suite);

    // 初期化したCOM状態だけを解放します。
    if (uninitializeCom)
    {
        CoUninitialize();
    }
    // 選択suiteの終了codeをprocessへ返します。
    return exitCode;
}
