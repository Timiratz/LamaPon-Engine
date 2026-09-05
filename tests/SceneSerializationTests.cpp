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
    // 更新中に足されるだけのComponent。m_componentsの再確保を起こす役です。
    class MarkerProbeComponent final : public LamaPon::Component
    {
    };

    // 最初のOnUpdateで、GameObjectとComponentをまとめて作ります。
    // ScriptのStartがやることを、Scriptを使わずに再現しています。
    class SpawnOnUpdateProbe final : public LamaPon::Component
    {
    public:
        LamaPon::Scene* scene{};
        int updateCount{};

    protected:
        void OnUpdate(float) override
        {
            ++updateCount;
            if (m_spawned)
            {
                return;
            }
            m_spawned = true;

            for (int index = 0; index < 64; ++index)
            {
                scene->CreateGameObject(
                    "Spawned " + std::to_string(index));
            }
            Owner().AddComponent<MarkerProbeComponent>();
        }

    private:
        bool m_spawned{};
    };

    class CollisionProbeComponent final : public LamaPon::Component
    {
    public:
        int enterCount{};
        int stayCount{};
        int exitCount{};
        int triggerEnterCount{};
        int triggerExitCount{};
        DirectX::XMFLOAT3 lastPoint{};

    protected:
        void OnCollisionEnter(
            const LamaPon::CollisionEvent& event) override
        {
            ++enterCount;
            lastPoint = event.point;
        }

        void OnCollisionStay(const LamaPon::CollisionEvent&) override
        {
            ++stayCount;
        }

        void OnCollisionExit(const LamaPon::CollisionEvent&) override
        {
            ++exitCount;
        }

        void OnTriggerEnter(
            const LamaPon::CollisionEvent& event) override
        {
            ++triggerEnterCount;
            lastPoint = event.point;
        }

        void OnTriggerExit(const LamaPon::CollisionEvent&) override
        {
            ++triggerExitCount;
        }
    };

    class FixedUpdateProbeComponent final
        : public LamaPon::Component
    {
    public:
        int fixedUpdateCount{};
        float updateElapsed{};
        float fixedElapsed{};

    protected:
        void OnUpdate(const float deltaTime) override
        {
            updateElapsed += deltaTime;
        }

        void OnFixedUpdate(
            const float fixedDeltaTime) override
        {
            ++fixedUpdateCount;
            fixedElapsed += fixedDeltaTime;
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

    class ActiveStateProbeComponent final
        : public LamaPon::Component
    {
    public:
        int enableCount{};
        int disableCount{};
        int updateCount{};
        int lateUpdateCount{};

    protected:
        void OnUpdate(float) override
        {
            ++updateCount;
        }

        void OnLateUpdate(float) override
        {
            ++lateUpdateCount;
        }

        void OnActiveStateChanged(const bool active) override
        {
            if (active)
            {
                ++enableCount;
            }
            else
            {
                ++disableCount;
            }
        }
    };

    class TriggerProbeComponent final
        : public LamaPon::Component
    {
    public:
        int collisionEnterCount{};
        int triggerEnterCount{};
        int triggerStayCount{};
        int triggerExitCount{};

    protected:
        void OnCollisionEnter(
            const LamaPon::CollisionEvent&) override
        {
            ++collisionEnterCount;
        }

        void OnTriggerEnter(
            const LamaPon::CollisionEvent&) override
        {
            ++triggerEnterCount;
        }

        void OnTriggerStay(
            const LamaPon::CollisionEvent&) override
        {
            ++triggerStayCount;
        }

        void OnTriggerExit(
            const LamaPon::CollisionEvent&) override
        {
            ++triggerExitCount;
        }
    };

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool NearlyEqual(const float left, const float right) noexcept
    {
        return std::abs(left - right) < 0.0001f;
    }
}

int RunTest(const std::string_view suite)
{
    try
    {
        LamaPon::GraphicsDevice graphics;
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

        auto& root = source.CreateGameObject("ルート");
        root.GetTransform().position = { 10.0f, 2.0f, 0.0f };
        // RenderCullingComponentの値を、GameObjectの互換アクセサー
        // IsAlwaysVisible/CullingMarginからも読み取れることを検証します。
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
        auto& sourceProbe =
            root.AddComponent<
                LamaPon::ReflectionProbeComponent>(
                7.5f,
                1.25f);
        sourceProbe.SetBoxExtents(
            { 4.0f, 2.5f, 3.5f });
        sourceProbe.SetBlendDistance(2.75f);
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

        auto& child = source.CreateGameObject("子オブジェクト");
        child.SetParent(&root);
        child.GetTransform().position = { 2.0f, 3.0f, 0.0f };
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

        auto& cameraObject = source.CreateGameObject("カメラ");
        cameraObject.SetParent(&root);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        cameraObject.AddComponent<
            LamaPon::AudioListenerComponent>();
        source.SetMainCamera(camera);

        const auto outputPath =
            std::filesystem::current_path()
            / "test-output"
            / std::string(suite)
            / "roundtrip.scene.json";
        source.SaveToFile(outputPath);
        const auto prefabPath =
            outputPath.parent_path()
            / std::filesystem::path(
                L"日本語階層.prefab.json");
        source.SavePrefab(root, prefabPath);

        LamaPon::Scene prefabTarget(graphics);
        auto& prefabParent =
            prefabTarget.CreateGameObject("PrefabParent");
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
        bool prefabInputMoverFound = false;
        bool prefabJointConnected = false;
        bool prefabLODConnected = false;
        for (const auto* prefabChild :
            prefabInstance.Children())
        {
            if (const auto* mover =
                prefabChild->GetComponent<
                    LamaPon::InputMoverComponent>())
            {
                prefabInputMoverFound =
                    mover->HorizontalAction()
                        == "MoveHorizontal"
                    && NearlyEqual(mover->Speed(), 4.5f);
            }
            if (const auto* prefabJoint =
                    prefabChild->GetComponent<
                        LamaPon::JointComponent>())
            {
                prefabJointConnected =
                    prefabJoint->ConnectedBodyId()
                        == prefabInstance.Id();
            }
        }
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
        const auto initialPrefabOverrides =
            prefabTarget.GetPrefabOverrides(
                prefabInstance);
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
        const auto nameOverrides =
            prefabTarget.GetPrefabOverrides(
                prefabInstance);
        std::string nameOverridePath;
        for (const auto& overrideValue :
            nameOverrides)
        {
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

        const float appliedPositionX =
            prefabInstance.GetTransform().position.x;
        prefabInstance.GetTransform().position.x += 5.0f;
        const auto transformOverrides =
            prefabTarget.GetPrefabOverrides(
                prefabInstance);
        std::string positionOverridePath;
        for (const auto& overrideValue :
            transformOverrides)
        {
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
        const auto replacedPrefabId =
            propertyRevertedPrefab.Id();
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

        LamaPon::Scene prefabRoundTrip(graphics);
        prefabRoundTrip.LoadFromJson(
            prefabTarget.SerializeToJson());
        bool prefabLinkRestored = false;
        for (const auto& object :
            prefabRoundTrip.GameObjects())
        {
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

        const auto objectsBeforeBrokenRevert =
            prefabTarget.GameObjects().size();
        {
            std::ofstream brokenPrefab(
                prefabPath,
                std::ios::binary
                    | std::ios::trunc);
            brokenPrefab << R"({"format":"Broken"})";
        }
        bool brokenRevertRejected = false;
        try
        {
            static_cast<void>(
                prefabTarget.RevertPrefabInstance(
                    revertedPrefab));
        }
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

        const auto nestedPrefabPath =
            outputPath.parent_path()
            / std::filesystem::path(
                L"ネスト.prefab.json");
        LamaPon::Scene nestedSource(graphics);
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
        auto& nestedInstance =
            prefabTarget.InstantiatePrefab(
                nestedPrefabPath,
                &revertedPrefab);
        Require(
            prefabTarget.FindPrefabInstanceRoot(
                nestedInstance)
                == &nestedInstance,
            "The nearest Nested Prefab root was not selected.");

        const auto structuralOverrides =
            prefabTarget.GetPrefabOverrides(
                revertedPrefab);
        bool structuralOverrideFound = false;
        for (const auto& overrideValue :
            structuralOverrides)
        {
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

        LamaPon::Scene nestedPrefabTarget(graphics);
        auto& restoredOuterPrefab =
            nestedPrefabTarget.InstantiatePrefab(
                prefabPath);
        bool nestedLinkRestored = false;
        for (const auto& object :
            nestedPrefabTarget.GameObjects())
        {
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

        const auto prefabObjectCount =
            prefabTarget.GameObjects().size();
        bool invalidPrefabRejected = false;
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
        catch (const std::exception&)
        {
            invalidPrefabRejected = true;
        }
        Require(
            invalidPrefabRejected
                && prefabTarget.GameObjects().size()
                    == prefabObjectCount,
            "Invalid prefab changed the destination Scene.");

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

        auto* loadedRoot = loaded.FindGameObject(root.Id());
        auto* loadedChild = loaded.FindGameObject(child.Id());
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
            LamaPon::Scene volumetricScene(graphics);
            LamaPon::VolumetricLightSettings volumetric{};
            volumetric.enabled = true;
            volumetric.intensity = 0.85f;
            volumetric.sampleCount = 24;
            volumetric.maximumDistance = 123.0f;
            volumetric.scattering = 0.42f;
            volumetricScene.SetVolumetricLightSettings(
                volumetric);

            LamaPon::Scene volumetricLoaded(graphics);
            volumetricLoaded.LoadFromJson(
                volumetricScene.SerializeToJson());
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

        // 旧形式（オブジェクト直下のalwaysVisible/cullingMargin）は
        // 読み込み時にRenderCullingコンポーネントへ変換されます。
        // 既存プロジェクトのシーンを開けなくしないための互換です。
        {
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
            const auto* legacyObject =
                legacyScene.FindGameObjectByName(
                    "LegacyCulling");
            Require(
                legacyObject != nullptr,
                "Legacy object was not loaded.");
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
            // rotationQuaternionを持たない旧シーンでも、
            // オイラー角から回転を復元できること。
            const auto* legacyRotated =
                legacyScene.FindGameObjectByName(
                    "LegacyRotated");
            Require(
                legacyRotated != nullptr,
                "Legacy rotated object was not loaded.");
            {
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

        DirectX::XMFLOAT4X4 childWorld{};
        DirectX::XMStoreFloat4x4(&childWorld, loadedChild->WorldMatrix());
        Require(NearlyEqual(childWorld._41, 12.0f), "World X position is incorrect.");
        Require(NearlyEqual(childWorld._42, 5.0f), "World Y position is incorrect.");

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
        const auto* loadedUITransform =
            loadedChild->GetComponent<
                LamaPon::
                    UIRectTransformComponent>();
        Require(
            loadedUITransform != nullptr,
            "UIRectTransform was not restored.");
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

        const auto materialPath =
            outputPath.parent_path()
            / "roundtrip.material.json";
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

        bool cycleRejected = false;
        try
        {
            loadedRoot->SetParent(loadedChild);
        }
        catch (const std::invalid_argument&)
        {
            cycleRejected = true;
        }
        Require(cycleRejected, "Hierarchy cycle was not rejected.");

        Require(loaded.DestroyGameObject(*loadedRoot), "GameObject deletion failed.");
        Require(loaded.GameObjects().empty(), "Recursive hierarchy deletion failed.");
        Require(loaded.MainCamera() == nullptr, "Deleted main camera was not cleared.");

        LamaPon::Scene fileLoaded(graphics);
        fileLoaded.LoadFromFile(outputPath);
        Require(fileLoaded.GameObjects().size() == 3, "File scene loading failed.");

        auto* fileRoot = fileLoaded.FindGameObject(root.Id());
        Require(fileRoot != nullptr, "File-loaded root was not found.");
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
        bool duplicatedJointConnected{};
        bool duplicatedJointSettings{};
        bool duplicatedLODConnected{};
        for (const auto* duplicateChild :
            duplicatedRoot.Children())
        {
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

        LamaPon::SpriteRendererComponent* duplicatedSprite{};
        LamaPon::AudioSourceComponent* duplicatedAudio{};
        LamaPon::AudioListenerComponent* duplicatedListener{};
        LamaPon::TransformAnimatorComponent*
            duplicatedAnimator{};
        LamaPon::RigidbodyComponent*
            duplicatedRigidbody{};
        LamaPon::SphereCollider3DComponent*
            duplicatedSphere{};
        for (auto* duplicatedChild : duplicatedRoot.Children())
        {
            if (auto* spriteComponent =
                duplicatedChild->GetComponent<LamaPon::SpriteRendererComponent>())
            {
                duplicatedSprite = spriteComponent;
            }
            if (auto* audioComponent =
                duplicatedChild->GetComponent<
                    LamaPon::AudioSourceComponent>())
            {
                duplicatedAudio = audioComponent;
            }
            if (auto* listenerComponent =
                duplicatedChild->GetComponent<
                    LamaPon::AudioListenerComponent>())
            {
                duplicatedListener = listenerComponent;
            }
            if (auto* animatorComponent =
                duplicatedChild->GetComponent<
                    LamaPon::TransformAnimatorComponent>())
            {
                duplicatedAnimator =
                    animatorComponent;
            }
            if (auto* rigidbodyComponent =
                duplicatedChild->GetComponent<
                    LamaPon::RigidbodyComponent>())
            {
                duplicatedRigidbody =
                    rigidbodyComponent;
            }
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

        LamaPon::Scene clipboardTarget(graphics);
        auto& pastedRoot = clipboardTarget.DuplicateGameObject(duplicatedRoot);
        Require(pastedRoot.Children().size() == 2, "Cross-scene paste hierarchy failed.");
        Require(
            clipboardTarget.GameObjects().size() == 3,
            "Cross-scene paste object count is incorrect.");

        // Tag・検索API・階層アクティブ・ライフサイクル・Timeの検証
        {
            LamaPon::Scene tagScene(graphics);
            auto& taggedParent =
                tagScene.CreateGameObject("親オブジェクト");
            taggedParent.SetTag("Player");
            auto& taggedChild =
                tagScene.CreateGameObject("子オブジェクト");
            taggedChild.SetTag("Enemy");
            taggedChild.SetParent(&taggedParent);
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
            LamaPon::Scene tagLoaded(graphics);
            tagLoaded.LoadFromJson(tagJson);
            const auto* loadedPlayer =
                tagLoaded.FindGameObjectByTag("Player");
            Require(
                loadedPlayer != nullptr
                    && loadedPlayer->Name() == "親オブジェクト",
                "Tag did not round-trip through scene JSON.");

            // タグ登録リスト：登録判定・Clear()をまたぐ保持・
            // 未登録タグでも読み込みは成功（警告のみ）
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
                LamaPon::ProjectSettings projectSettings;
                projectSettings.tags = {
                    "Player",
                    "Enemy",
                    "Collectible" };
                // 3スイートが並列実行されるため、書き込み先は
                // スイート別のディレクトリにします（共有すると
                // 書きかけの空ファイルを他プロセスが読みます）。
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
                const auto loadedSettings =
                    LamaPon::LoadProjectSettings(
                        projectPath);
                Require(
                    loadedSettings.tags
                        == projectSettings.tags,
                    "Project tags did not round-trip.");

                projectSettings.tags = { "A", "A" };
                bool duplicateRejected = false;
                try
                {
                    LamaPon::ValidateProjectSettings(
                        projectSettings);
                }
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
                LamaPon::Scene searchScene(graphics);
                auto& nestedRoot =
                    searchScene.CreateGameObject("ルート");
                auto& arm =
                    searchScene.CreateGameObject("腕");
                arm.SetParent(&nestedRoot);
                auto& weapon =
                    searchScene.CreateGameObject("武器");
                weapon.SetParent(&arm);
                auto& rootProbe = nestedRoot.AddComponent<
                    ActiveStateProbeComponent>();
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

                // 非アクティブ階層は既定でスキップし、
                // includeInactive=trueで含めます。
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

            // JobSystem：全要素を1回ずつ処理・逐次フォールバック・
            // ネスト・例外伝播
            {
                auto& jobs =
                    LamaPon::JobSystem::Instance();
                Require(
                    jobs.WorkerCount() >= 1,
                    "JobSystem must own workers.");

                constexpr std::size_t Count = 10000;
                std::vector<std::atomic<int>> touched(
                    Count);
                jobs.ParallelFor(
                    Count,
                    64,
                    [&touched](
                        const std::size_t begin,
                        const std::size_t end)
                    {
                        for (std::size_t index = begin;
                            index < end;
                            ++index)
                        {
                            touched[index].fetch_add(1);
                        }
                    });
                bool allOnce = true;
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
                try
                {
                    jobs.ParallelFor(
                        128,
                        8,
                        [](const std::size_t begin,
                            const std::size_t)
                        {
                            if (begin == 64)
                            {
                                throw std::runtime_error(
                                    "job failure");
                            }
                        });
                }
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

            // イベントバス：購読・発行・引数・解除・再入・
            // UIButtonのクリックイベント名の保存往復
            {
                LamaPon::EventBus events;
                int received = 0;
                float lastNumber = 0.0f;
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

                int observed = 0;
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
                auto& buttonObject =
                    buttonScene.CreateGameObject(
                        "開始ボタン");
                auto& button =
                    buttonObject.AddComponent<
                        LamaPon::UIButtonComponent>();
                button.SetClickEventName("StartGame");
                const auto buttonJson =
                    buttonScene.SerializeToJson();
                LamaPon::Scene buttonLoaded(graphics);
                buttonLoaded.LoadFromJson(buttonJson);
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
            }

            // スプライトアニメーション：コマ送り・ループ・
            // 非ループ停止・ソース矩形・シリアライズ往復
            {
                LamaPon::Scene spriteScene(graphics);
                auto& player =
                    spriteScene.CreateGameObject(
                        "Player");
                auto& renderer = player.AddComponent<
                    LamaPon::SpriteRendererComponent>();
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

                const auto spriteJson =
                    spriteScene.SerializeToJson();
                LamaPon::Scene spriteLoaded(graphics);
                spriteLoaded.LoadFromJson(spriteJson);
                const auto* overridePlayer =
                    spriteLoaded.FindGameObjectByName(
                        "Player");
                Require(
                    overridePlayer != nullptr,
                    "Sprite scene did not round-trip.");
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
            auto& triggerObject =
                triggerScene.CreateGameObject("トリガー");
            triggerObject.AddComponent<
                LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f },
                true);
            auto& triggerProbe = triggerObject.AddComponent<
                TriggerProbeComponent>();
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
            auto& widgetObject =
                widgetScene.CreateGameObject(
                    "ウィジェット");
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
            auto& sourceToggle =
                widgetObject.AddComponent<
                    LamaPon::UIToggleComponent>(
                    "サウンド", true);
            sourceToggle.SetFontSize(30.0f);
            auto& sourceSlider =
                widgetObject.AddComponent<
                    LamaPon::UISliderComponent>(
                    -10.0f, 10.0f, 2.5f);
            sourceSlider.SetWholeNumbers(true);
            auto& sourceField =
                widgetObject.AddComponent<
                    LamaPon::UIInputFieldComponent>(
                    "こんにちは", "名前を入力");
            sourceField.SetMaxLength(32);
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

            const auto widgetJson =
                widgetScene.SerializeToJson();
            LamaPon::Scene widgetLoaded(graphics);
            widgetLoaded.LoadFromJson(widgetJson);
            const auto* loadedWidgetObject =
                widgetLoaded.FindGameObjectByName(
                    "ウィジェット");
            Require(
                loadedWidgetObject != nullptr,
                "Widget object did not round-trip.");
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
            auto& layoutRoot =
                layoutScene.CreateGameObject("整列親");
            layoutRoot.AddComponent<
                LamaPon::UIRectTransformComponent>(
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 300.0f, 300.0f });
            auto& verticalLayout =
                layoutRoot.AddComponent<
                    LamaPon::UILayoutGroupComponent>(
                    LamaPon::UILayoutAxis::Vertical,
                    10.0f);
            verticalLayout.SetPadding(
                { 5.0f, 6.0f, 5.0f, 6.0f });
            auto& firstChild =
                layoutScene.CreateGameObject("子1");
            firstChild.SetParent(&layoutRoot);
            auto& firstChildTransform =
                firstChild.AddComponent<
                    LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 100.0f, 40.0f });
            auto& secondChild =
                layoutScene.CreateGameObject("子2");
            secondChild.SetParent(&layoutRoot);
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
            auto& floorObject =
                meshScene.CreateGameObject("メッシュ床");
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

            LamaPon::PhysicsHit meshHit{};
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
            LamaPon::PhysicsHit meshMiss{};
            Require(
                !meshScene.Raycast(
                    LamaPon::Ray{
                        { 20.0f, 3.0f, 0.0f },
                        { 0.0f, -1.0f, 0.0f } },
                    10.0f,
                    meshMiss),
                "Mesh collider raycast hit outside the mesh.");

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
            auto& meshBallBody =
                meshBall.AddComponent<
                    LamaPon::RigidbodyComponent>();
            static_cast<void>(meshBallBody);
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
            auto& meshSaveObject =
                meshSaveScene.CreateGameObject(
                    "ステージ");
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
            const auto meshJson =
                meshSaveScene.SerializeToJson();
            LamaPon::Scene meshLoadScene(graphics);
            meshLoadScene.LoadFromJson(meshJson);
            const auto* loadedStage =
                meshLoadScene.FindGameObjectByName(
                    "ステージ");
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
                LamaPon::PhysicsMaterial left{
                    0.2f, 0.1f };
                LamaPon::PhysicsMaterial right{
                    0.8f, 0.5f };
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
                const LamaPon::Capsule3D physicsCapsule{
                    { -1.0f, 0.4f, 0.0f },
                    { 1.0f, 0.4f, 0.0f },
                    0.5f };
                LamaPon::OrientedBox3D floorBox{};
                floorBox.center = { 0.0f, -0.5f, 0.0f };
                floorBox.halfExtents =
                    { 5.0f, 0.5f, 5.0f };
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
            auto& circleGround =
                circleScene.CreateGameObject("2D床");
            circleGround.AddComponent<
                LamaPon::BoxCollider2DComponent>(
                DirectX::XMFLOAT2{ 10.0f, 1.0f });
            auto& circleBall =
                circleScene.CreateGameObject("2D円");
            circleBall.GetTransform().position =
                { 0.0f, 2.0f, 0.0f };
            auto& circleCollider =
                circleBall.AddComponent<
                    LamaPon::CircleCollider2DComponent>(
                    0.5f);
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
            const auto circleJson =
                circleScene.SerializeToJson();
            LamaPon::Scene circleLoaded(graphics);
            circleLoaded.LoadFromJson(circleJson);
            const auto* loadedBall =
                circleLoaded.FindGameObjectByName(
                    "2D円");
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
            auto& polygonGround =
                polygonScene.CreateGameObject("2D床");
            polygonGround.AddComponent<
                LamaPon::BoxCollider2DComponent>(
                DirectX::XMFLOAT2{ 10.0f, 1.0f });
            auto& polygonFalling =
                polygonScene.CreateGameObject("2D三角形");
            polygonFalling.GetTransform().position =
                { 0.0f, 3.0f, 0.0f };
            auto& polygonCollider =
                polygonFalling.AddComponent<
                    LamaPon::PolygonCollider2DComponent>(
                    std::vector<DirectX::XMFLOAT2>{
                        { 0.0f, 0.5f },
                        { -0.5f, -0.5f },
                        { 0.5f, -0.5f } });
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
            const auto polygonJson =
                polygonScene.SerializeToJson();
            LamaPon::Scene polygonLoaded(graphics);
            polygonLoaded.LoadFromJson(polygonJson);
            const auto* loadedTriangle =
                polygonLoaded.FindGameObjectByName(
                    "2D三角形");
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
                LamaPon::Scene tilemapScene(graphics);
                auto& tilemapObject =
                    tilemapScene.CreateGameObject("床タイル");
                auto& collisionTilemap =
                    tilemapObject.AddComponent<
                        LamaPon::TilemapComponent>(
                            DirectX::XMFLOAT2{
                                16.0f, 16.0f });
                // 3x2の矩形ブロック：1個の矩形へマージされるはずです。
                for (int y{}; y < 2; ++y)
                {
                    for (int x{}; x < 3; ++x)
                    {
                        collisionTilemap.SetCell(x, y, 0);
                    }
                }
                // 離れた1セル：別の矩形になるはずです。
                collisionTilemap.SetCell(10, 10, 0);

                const auto rects =
                    collisionTilemap.ComputeCollisionRects();
                Require(
                    rects.size() == 2,
                    "Tilemap collider rects did not merge"
                        " as expected.");

                const auto blockRect = std::find_if(
                    rects.begin(),
                    rects.end(),
                    [](const auto& rect)
                    {
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

                const auto singleRect = std::find_if(
                    rects.begin(),
                    rects.end(),
                    [](const auto& rect)
                    {
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
                const auto tilemapJson =
                    tilemapScene.SerializeToJson();
                LamaPon::Scene tilemapLoaded(graphics);
                tilemapLoaded.LoadFromJson(tilemapJson);
                const auto* loadedTilemapObject =
                    tilemapLoaded.FindGameObjectByName(
                        "床タイル");
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
                LamaPon::Scene parallaxScene(graphics);
                auto& referenceObject =
                    parallaxScene.CreateGameObject(
                        "参照");
                auto& backgroundObject =
                    parallaxScene.CreateGameObject(
                        "背景");
                backgroundObject.GetTransform()
                    .position =
                        { 100.0f, 50.0f, 0.0f };
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

                const auto parallaxJson =
                    parallaxScene.SerializeToJson();
                LamaPon::Scene parallaxLoaded(graphics);
                parallaxLoaded.LoadFromJson(
                    parallaxJson);
                const auto* loadedBackground =
                    parallaxLoaded.FindGameObjectByName(
                        "背景");
                const auto* loadedParallax =
                    loadedBackground != nullptr
                        ? loadedBackground
                            ->GetComponent<
                                LamaPon::
                                    ParallaxLayerComponent>()
                        : nullptr;
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
                LamaPon::Scene light2DScene(graphics);
                auto& torch =
                    light2DScene.CreateGameObject("たいまつ");
                torch.GetTransform().position =
                    { 42.0f, -7.0f, 0.0f };
                auto& light2D =
                    torch.AddComponent<
                        LamaPon::Light2DComponent>();
                light2D.SetColor(
                    { 0.2f, 0.6f, 1.0f });
                light2D.SetIntensity(2.5f);
                light2D.SetRadius(320.0f);

                const auto worldPosition =
                    light2D.WorldPosition();
                Require(
                    NearlyEqual(worldPosition.x, 42.0f)
                        && NearlyEqual(
                            worldPosition.y, -7.0f),
                    "Light2D world position did not"
                        " follow the Transform.");

                const auto light2DJson =
                    light2DScene.SerializeToJson();
                LamaPon::Scene light2DLoaded(graphics);
                light2DLoaded.LoadFromJson(light2DJson);
                const auto* loadedTorch =
                    light2DLoaded.FindGameObjectByName(
                        "たいまつ");
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
                LamaPon::Scene spriteMaskScene(graphics);
                auto& maskObject =
                    spriteMaskScene.CreateGameObject(
                        "マスク");
                maskObject.GetTransform().position =
                    { 10.0f, 20.0f, 0.0f };
                auto& spriteMask =
                    maskObject.AddComponent<
                        LamaPon::SpriteMaskComponent>();
                spriteMask.SetShape(
                    LamaPon::SpriteMaskShape::Circle);
                spriteMask.SetSize(
                    { 250.0f, 250.0f });

                const auto maskWorldPosition =
                    spriteMask.WorldPosition();
                Require(
                    NearlyEqual(
                        maskWorldPosition.x, 10.0f)
                        && NearlyEqual(
                            maskWorldPosition.y, 20.0f),
                    "SpriteMask world position did not"
                        " follow the Transform.");

                auto& fogObject =
                    spriteMaskScene.CreateGameObject(
                        "霧");
                auto& fogSprite =
                    fogObject.AddComponent<
                        LamaPon::SpriteRendererComponent>();
                fogSprite.SetMaskInteraction(
                    LamaPon::SpriteMaskInteraction::
                        VisibleOutsideMask);

                const auto spriteMaskJson =
                    spriteMaskScene.SerializeToJson();
                LamaPon::Scene spriteMaskLoaded(graphics);
                spriteMaskLoaded.LoadFromJson(
                    spriteMaskJson);
                const auto* loadedMaskObject =
                    spriteMaskLoaded.FindGameObjectByName(
                        "マスク");
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

                const auto* loadedFogObject =
                    spriteMaskLoaded.FindGameObjectByName(
                        "霧");
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
            auto& scrollView =
                scrollRoot.AddComponent<
                    LamaPon::UIScrollViewComponent>();
            auto& scrollItem =
                scrollScene.CreateGameObject(
                    "スクロール項目");
            scrollItem.SetParent(&scrollRoot);
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
            const auto scrollJson =
                scrollScene.SerializeToJson();
            LamaPon::Scene scrollLoaded(graphics);
            scrollLoaded.LoadFromJson(scrollJson);
            const auto* loadedScrollRoot =
                scrollLoaded.FindGameObjectByName(
                    "スクロール");
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
            auto& pbrObject =
                pbrScene.CreateGameObject("金属キューブ");
            auto& pbrMesh = pbrObject.AddComponent<
                LamaPon::MeshRendererComponent>();
            pbrMesh.SetMetallic(0.75f);
            auto& shadowSpotObject =
                pbrScene.CreateGameObject("影スポット");
            auto& shadowSpot =
                shadowSpotObject.AddComponent<
                    LamaPon::SpotLightComponent>();
            shadowSpot.SetCastsShadows(true);
            shadowSpot.SetShadowStrength(0.6f);
            shadowSpot.SetShadowBias(0.004f);
            auto& shadowPointObject =
                pbrScene.CreateGameObject("影ポイント");
            auto& shadowPoint =
                shadowPointObject.AddComponent<
                    LamaPon::PointLightComponent>();
            shadowPoint.SetCastsShadows(true);
            shadowPoint.SetShadowStrength(0.7f);
            auto pbrSky = pbrScene.Sky();
            pbrSky.enabled = true;
            pbrSky.cubemapPath = "textures/sky.dds";
            pbrSky.iblIntensity = 1.5f;
            pbrScene.SetSkySettings(pbrSky);

            const auto pbrJson =
                pbrScene.SerializeToJson();
            LamaPon::Scene pbrLoaded(graphics);
            pbrLoaded.LoadFromJson(pbrJson);
            const auto* loadedPbrObject =
                pbrLoaded.FindGameObjectByName(
                    "金属キューブ");
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
            auto& bgmSource = bgmObject.AddComponent<
                LamaPon::AudioSourceComponent>(
                std::filesystem::path{
                    "audio/startup.ogg" },
                0.8f);
            bgmSource.SetBus(LamaPon::AudioBus::Music);
            bgmSource.SetStreaming(true);
            const auto audioJson =
                pbrScene.SerializeToJson();
            LamaPon::Scene audioLoaded(graphics);
            audioLoaded.LoadFromJson(audioJson);
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

        if (suite == "simulation")
        {
        // ヒエラルキーの並び替え。並びは見た目だけでなく、ライトの
        // 収集順（先着N灯）と保存順を決めるので、順序そのものを
        // 検査します。
        {
            LamaPon::Scene reorderScene(graphics);
            auto& first =
                reorderScene.CreateGameObject("First");
            auto& second =
                reorderScene.CreateGameObject("Second");
            auto& third =
                reorderScene.CreateGameObject("Third");
            auto& persistentChild =
                reorderScene.CreateGameObject("Child");
            persistentChild.SetParent(&third);

            const auto rootOrder =
                [&reorderScene]
                {
                    std::string names;
                    for (const auto& object :
                        reorderScene.GameObjects())
                    {
                        names += object->Name();
                        names += ',';
                    }
                    return names;
                };

            Require(
                rootOrder()
                    == "First,Second,Third,Child,",
                "Unexpected initial hierarchy order.");

            // ThirdをFirstの前へ。子も一緒に動く必要があります。
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

            // 子同士の並び替え。表示順の出どころが親のm_children
            // なので、m_gameObjectsだけを直しても表示順は変わりません。
            // 両方の並びを検査します。
            auto& parent =
                reorderScene.CreateGameObject("Parent");
            auto& childA =
                reorderScene.CreateGameObject("ChildA");
            auto& childB =
                reorderScene.CreateGameObject("ChildB");
            auto& childC =
                reorderScene.CreateGameObject("ChildC");
            childA.SetParent(&parent);
            childB.SetParent(&parent);
            childC.SetParent(&parent);

            const auto childOrder =
                [&parent]
                {
                    std::string names;
                    for (const auto* object :
                        parent.Children())
                    {
                        names += object->Name();
                        names += ',';
                    }
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

            // 並びが保存へ載ることも確認します（再起動後に
            // 同じ並びで開けること）。
            const auto snapshot =
                reorderScene.SerializeToJson();
            LamaPon::Scene loadedScene(graphics);
            loadedScene.LoadFromJson(snapshot);
            std::string loadedNames;
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
            std::string loadedChildNames;
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

        LamaPon::Scene thirtyFpsScene(graphics);
        auto& thirtyFpsObject =
            thirtyFpsScene.CreateGameObject(
                "30 FPS fixed body");
        auto& thirtyFpsBody =
            thirtyFpsObject.AddComponent<
                LamaPon::RigidbodyComponent>();
        thirtyFpsBody.SetUseGravity(false);
        auto& thirtyFpsProbe =
            thirtyFpsObject.AddComponent<
                FixedUpdateProbeComponent>();
        for (int frame{}; frame < 30; ++frame)
        {
            thirtyFpsScene.Update(1.0f / 30.0f);
        }

        LamaPon::Scene highFpsScene(graphics);
        auto& highFpsObject =
            highFpsScene.CreateGameObject(
                "144 FPS fixed body");
        auto& highFpsBody =
            highFpsObject.AddComponent<
                LamaPon::RigidbodyComponent>();
        highFpsBody.SetUseGravity(false);
        auto& highFpsProbe =
            highFpsObject.AddComponent<
                FixedUpdateProbeComponent>();
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

        LamaPon::Scene renderInterpolationScene(graphics);
        auto& interpolationParent =
            renderInterpolationScene.CreateGameObject(
                "Interpolated parent");
        auto& interpolationBody =
            interpolationParent.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{
                        6.0f,
                        0.0f,
                        0.0f },
                    false);
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
        DirectX::XMFLOAT4X4 interpolatedParentMatrix{};
        DirectX::XMStoreFloat4x4(
            &interpolatedParentMatrix,
            interpolationParent
                .InterpolatedWorldMatrix(
                    renderInterpolationScene
                        .PhysicsInterpolationAlpha()));
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

        LamaPon::Scene physicsScene(graphics);
        auto& ground = physicsScene.CreateGameObject("床");
        ground.AddComponent<LamaPon::BoxCollider3DComponent>(
            DirectX::XMFLOAT3{ 10.0f, 1.0f, 10.0f });

        auto& fallingBox = physicsScene.CreateGameObject("落下物");
        fallingBox.GetTransform().position = { 0.0f, 5.0f, 0.0f };
        fallingBox.AddComponent<LamaPon::BoxCollider3DComponent>();
        auto& fallingBody = fallingBox.AddComponent<LamaPon::RigidbodyComponent>();
        auto& collisionProbe = fallingBox.AddComponent<CollisionProbeComponent>();

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

        // 衝突マトリクスで切ったペアは、コライダーのマスクが許して
        // いても当たらないこと。レイヤー0（床）と1を切った状態で、
        // レイヤー0の箱は床に載り、レイヤー1の箱は素通りする。
        // 両方を同じシーンへ置き、物理更新全体の停止を対照で除外します。
        {
            const auto savedPhysics =
                LamaPon::ActivePhysicsSettings();
            auto matrixPhysics = savedPhysics;
            matrixPhysics.collisionMatrix[0] &=
                ~(1u << 1);
            matrixPhysics.collisionMatrix[1] &=
                ~(1u << 0);
            LamaPon::SetActivePhysicsSettings(
                matrixPhysics);

            LamaPon::Scene matrixScene(graphics);
            auto& matrixGround =
                matrixScene.CreateGameObject("床");
            matrixGround.AddComponent<
                LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 10.0f, 1.0f, 10.0f });

            auto& solidBox =
                matrixScene.CreateGameObject("載る箱");
            solidBox.GetTransform().position =
                { -2.0f, 5.0f, 0.0f };
            solidBox.AddComponent<
                LamaPon::BoxCollider3DComponent>();
            solidBox.AddComponent<
                LamaPon::RigidbodyComponent>();

            auto& ghostBox =
                matrixScene.CreateGameObject(
                    "すり抜ける箱");
            ghostBox.GetTransform().position =
                { 2.0f, 5.0f, 0.0f };
            auto& ghostCollider =
                ghostBox.AddComponent<
                    LamaPon::BoxCollider3DComponent>();
            ghostCollider.SetLayer(1);
            ghostBox.AddComponent<
                LamaPon::RigidbodyComponent>();

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

        constexpr float diagonal = 0.70710678f;
        const LamaPon::OrientedBox3D rotatedBox{
            {},
            {
                DirectX::XMFLOAT3{ diagonal, diagonal, 0.0f },
                DirectX::XMFLOAT3{ -diagonal, diagonal, 0.0f },
                DirectX::XMFLOAT3{ 0.0f, 0.0f, 1.0f }
            },
            { 1.0f, 0.3f, 0.5f }
        };
        LamaPon::OrientedBox3D nearbyBox;
        nearbyBox.center = { 0.0f, 1.0f, 0.0f };
        Require(
            LamaPon::Intersect(rotatedBox, nearbyBox).has_value(),
            "Rotated OBB collision was not detected.");
        LamaPon::OrientedBox3D manifoldBox;
        manifoldBox.center =
            { 0.0f, 0.9f, 0.0f };
        LamaPon::OrientedBox3D manifoldGround;
        manifoldGround.halfExtents =
            { 5.0f, 0.5f, 5.0f };
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
        const auto sphereBounds =
            LamaPon::BoundsOf(testSphere);
        Require(
            NearlyEqual(sphereBounds.minimum.x, -0.5f)
                && NearlyEqual(
                    sphereBounds.maximum.y,
                    1.2f),
            "Sphere bounds are incorrect.");

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
        const auto hullBounds =
            LamaPon::BoundsOf(testHull);
        Require(
            NearlyEqual(hullBounds.minimum.y, 0.2f)
                && NearlyEqual(hullBounds.maximum.y, 1.2f),
            "Convex hull bounds are incorrect.");

        LamaPon::Scene sphereScene(graphics);
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
        auto& fallingSphere =
            sphereScene.CreateGameObject("Falling sphere");
        fallingSphere.GetTransform().position =
            { 0.0f, 3.0f, 0.0f };
        fallingSphere.AddComponent<
            LamaPon::SphereCollider3DComponent>(0.5f);
        auto& sphereBody =
            fallingSphere.AddComponent<
                LamaPon::RigidbodyComponent>();
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

        LamaPon::Scene hullScene(graphics);
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
        auto& fallingHull =
            hullScene.CreateGameObject("Falling hull");
        fallingHull.GetTransform().position =
            { 0.0f, 3.0f, 0.0f };
        fallingHull.AddComponent<
            LamaPon::ConvexHullCollider3DComponent>();
        auto& hullBody =
            fallingHull.AddComponent<
                LamaPon::RigidbodyComponent>();
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

        LamaPon::Scene compoundScene(graphics);
        auto& compoundRoot =
            compoundScene.CreateGameObject("Compound root");
        compoundRoot.GetTransform().position =
            { 0.0f, 3.0f, 0.0f };
        auto& compoundBody =
            compoundRoot.AddComponent<
                LamaPon::RigidbodyComponent>();
        compoundBody.SetConstraints({
            true,
            true,
            true
        });
        auto& compoundBox =
            compoundScene.CreateGameObject("Compound box");
        compoundBox.SetParent(&compoundRoot);
        compoundBox.GetTransform().position =
            { -0.45f, 0.0f, 0.0f };
        compoundBox.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        auto& compoundBoxProbe =
            compoundBox.AddComponent<
                CollisionProbeComponent>();
        auto& compoundSphere =
            compoundScene.CreateGameObject("Compound sphere");
        compoundSphere.SetParent(&compoundRoot);
        compoundSphere.GetTransform().position =
            { 0.45f, 0.0f, 0.0f };
        compoundSphere.AddComponent<
            LamaPon::SphereCollider3DComponent>(0.5f);
        auto& compoundSphereProbe =
            compoundSphere.AddComponent<
                CollisionProbeComponent>();
        compoundScene.Update(0.0f);
        Require(
            compoundBoxProbe.enterCount == 0
                && compoundSphereProbe.enterCount == 0,
            "Child colliders on one Rigidbody collided with each other.");
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

        LamaPon::Scene materialScene(graphics);
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
        auto& bouncingBody =
            bouncingCapsule.AddComponent<
                LamaPon::RigidbodyComponent>();
        bool fellFast{};
        bool bounced{};
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
        auto& slidingBody =
            slidingBox.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{ 3.0f, 0.0f, 0.0f });
        bool producedRollingMotion{};
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

        LamaPon::Scene angularScene(graphics);
        auto& torqueBox =
            angularScene.CreateGameObject("Torque box");
        torqueBox.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 2.0f, 2.0f, 2.0f });
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

        LamaPon::Scene ledgeScene(graphics);
        auto& ledge =
            ledgeScene.CreateGameObject("Ledge");
        ledge.GetTransform().position =
            { 0.0f, -0.5f, 0.0f };
        ledge.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{ 4.0f, 1.0f, 4.0f });
        auto& ledgeBox =
            ledgeScene.CreateGameObject("Ledge box");
        ledgeBox.GetTransform().position =
            { 2.15f, 0.5f, 0.0f };
        ledgeBox.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        ledgeBox.AddComponent<
            LamaPon::RigidbodyComponent>();
        float maximumLedgeRotation{};
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

        LamaPon::Scene stackScene(graphics);
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
        for (std::size_t index{};
            index < stackedBoxes.size();
            ++index)
        {
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
        for (int step{}; step < 600; ++step)
        {
            stackScene.Update(1.0f / 60.0f);
        }
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

        LamaPon::Scene jointScene(graphics);
        auto& fixedAnchor =
            jointScene.CreateGameObject("Fixed anchor");
        fixedAnchor.GetTransform().position =
            { 0.0f, 0.0f, 0.0f };
        fixedAnchor.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        auto& fixedBody =
            jointScene.CreateGameObject("Fixed body");
        fixedBody.GetTransform().position =
            { 2.0f, 0.0f, 0.0f };
        fixedBody.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        auto& fixedRigidbody =
            fixedBody.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{
                        8.0f,
                        0.0f,
                        0.0f },
                    false);
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

        LamaPon::Scene hingeScene(graphics);
        auto& hingeAnchor =
            hingeScene.CreateGameObject("Hinge anchor");
        auto& motorBody =
            hingeScene.CreateGameObject("Motor body");
        motorBody.AddComponent<
            LamaPon::BoxCollider3DComponent>();
        auto& motorRigidbody =
            motorBody.AddComponent<
                LamaPon::RigidbodyComponent>(
                    DirectX::XMFLOAT3{},
                    false);
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
        for (int step{}; step < 120; ++step)
        {
            hingeScene.Update(1.0f / 60.0f);
        }
        constexpr float radiansToDegrees =
            180.0f / DirectX::XM_PI;
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
        for (int step{}; step < 240; ++step)
        {
            hingeScene.Update(1.0f / 60.0f);
        }
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
        const float initialSpringDistance =
            std::sqrt(25.0f);
        for (int step{}; step < 30; ++step)
        {
            jointScene.Update(1.0f / 60.0f);
        }
        const auto springPosition =
            springBody.GetTransform().position;
        const float springDistance = std::sqrt(
            springPosition.x * springPosition.x
            + springPosition.y * springPosition.y
            + springPosition.z * springPosition.z);
        Require(
            springDistance < initialSpringDistance,
            "Spring joint did not pull toward its rest length.");

        LamaPon::Scene ccdScene(graphics);
        auto& thinWall =
            ccdScene.CreateGameObject("Thin wall");
        thinWall.AddComponent<
            LamaPon::BoxCollider3DComponent>(
                DirectX::XMFLOAT3{
                    0.1f,
                    4.0f,
                    4.0f });
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

        const auto visibilityView =
            DirectX::XMMatrixIdentity();
        const auto visibilityProjection =
            DirectX::XMMatrixPerspectiveFovRH(
                DirectX::XM_PIDIV4,
                16.0f / 9.0f,
                0.1f,
                500.0f);

        struct LodTestVertex final
        {
            DirectX::XMFLOAT3 position{};
        };
        std::vector<LodTestVertex> lodVertices;
        std::vector<std::uint32_t> lodIndices;
        constexpr std::uint32_t lodGridSize = 16;
        lodVertices.reserve(lodGridSize * lodGridSize);
        for (std::uint32_t y = 0; y < lodGridSize; ++y)
        {
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
        for (std::uint32_t y = 0; y + 1 < lodGridSize; ++y)
        {
            for (std::uint32_t x = 0; x + 1 < lodGridSize; ++x)
            {
                const auto topLeft = y * lodGridSize + x;
                const auto topRight = topLeft + 1;
                const auto bottomLeft = topLeft + lodGridSize;
                const auto bottomRight = bottomLeft + 1;
                lodIndices.insert(
                    lodIndices.end(),
                    {
                        topLeft, bottomLeft, topRight,
                        topRight, bottomLeft, bottomRight
                    });
            }
        }
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

        LamaPon::Scene lodScene(graphics);
        lodScene.SetFrustumCullingEnabled(
            false);
        lodScene.SetOcclusionCullingEnabled(
            false);
        auto& lodRoot =
            lodScene.CreateGameObject("LOD root");
        lodRoot.GetTransform().position =
            { 0.0f, 0.0f, -10.0f };
        auto& lodHigh =
            lodScene.CreateGameObject("LOD high");
        lodHigh.SetParent(&lodRoot);
        lodHigh.AddComponent<
            LamaPon::MeshRendererComponent>();
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

        LamaPon::Scene frustumScene(graphics);
        frustumScene.SetOcclusionCullingEnabled(
            false);
        auto& visibleMesh =
            frustumScene.CreateGameObject(
                "Visible mesh");
        visibleMesh.GetTransform().position =
            { 0.0f, 0.0f, -5.0f };
        visibleMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        auto& outsideMesh =
            frustumScene.CreateGameObject(
                "Outside mesh");
        outsideMesh.GetTransform().position =
            { 100.0f, 0.0f, -5.0f };
        outsideMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        auto& alwaysVisibleMesh =
            frustumScene.CreateGameObject(
                "Always visible mesh");
        alwaysVisibleMesh.GetTransform().position =
            { 120.0f, 0.0f, -5.0f };
        alwaysVisibleMesh.AddComponent<
            LamaPon::RenderCullingComponent>(true);
        alwaysVisibleMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        auto& marginMesh =
            frustumScene.CreateGameObject(
                "Margin mesh");
        marginMesh.GetTransform().position =
            { 10.0f, 0.0f, -5.0f };
        marginMesh.AddComponent<
            LamaPon::RenderCullingComponent>(false, 7.0f);
        marginMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
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

        LamaPon::Scene spatialScene(graphics);
        spatialScene.SetOcclusionCullingEnabled(false);
        auto& spatialVisible =
            spatialScene.CreateGameObject("Spatial visible");
        spatialVisible.GetTransform().position =
            { 0.0f, 0.0f, -5.0f };
        spatialVisible.AddComponent<
            LamaPon::MeshRendererComponent>();
        for (int index = 0; index < 128; ++index)
        {
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
        const auto firstSpatialStats =
            spatialScene.EvaluateRenderVisibility(
                visibilityView,
                visibilityProjection);
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

        const auto lowQuality =
            LamaPon::GraphicsSettingsForPreset(
                LamaPon::GraphicsQualityPreset::Low);
        const auto mediumQuality =
            LamaPon::GraphicsSettingsForPreset(
                LamaPon::GraphicsQualityPreset::Medium);
        const auto highQuality =
            LamaPon::GraphicsSettingsForPreset(
                LamaPon::GraphicsQualityPreset::High);
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
        auto invalidLodQuality = highQuality;
        invalidLodQuality.automaticLodQuality = 8.0f;
        Require(
            LamaPon::ClampGraphicsSettings(
                invalidLodQuality).automaticLodQuality == 2.0f,
            "Automatic LOD quality was not clamped.");

        LamaPon::Scene occlusionScene(graphics);
        auto& occluder =
            occlusionScene.CreateGameObject(
                "Occluder");
        occluder.GetTransform().position =
            { 0.0f, 0.0f, -5.0f };
        occluder.GetTransform().scale =
            { 8.0f, 8.0f, 1.0f };
        occluder.AddComponent<
            LamaPon::MeshRendererComponent>();
        auto& hiddenMesh =
            occlusionScene.CreateGameObject(
                "Hidden mesh");
        hiddenMesh.GetTransform().position =
            { 0.0f, 0.0f, -10.0f };
        hiddenMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
        auto& alwaysVisibleHiddenMesh =
            occlusionScene.CreateGameObject(
                "Always visible hidden mesh");
        alwaysVisibleHiddenMesh.GetTransform().position =
            { 0.0f, 0.0f, -12.0f };
        alwaysVisibleHiddenMesh.AddComponent<
            LamaPon::RenderCullingComponent>(true);
        alwaysVisibleHiddenMesh.AddComponent<
            LamaPon::MeshRendererComponent>();
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

        LamaPon::Scene triggerScene(graphics);
        auto& triggerArea = triggerScene.CreateGameObject("2Dトリガー");
        triggerArea.AddComponent<LamaPon::BoxCollider2DComponent>(
            DirectX::XMFLOAT2{ 10.0f, 10.0f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            true,
            5,
            1u << 6);

        auto& triggerVisitor = triggerScene.CreateGameObject("訪問者");
        triggerVisitor.AddComponent<LamaPon::BoxCollider2DComponent>(
            DirectX::XMFLOAT2{ 1.0f, 1.0f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            false,
            6,
            1u << 5);
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

        auto& filteredVisitor = triggerScene.CreateGameObject("除外対象");
        filteredVisitor.AddComponent<LamaPon::BoxCollider2DComponent>(
            DirectX::XMFLOAT2{ 1.0f, 1.0f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            false,
            7,
            0u);
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
        for (int index = 1; index <= 100; ++index)
        {
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
        const auto broadPhase =
            LamaPon::BuildSpatialHashPairs(
                broadPhaseBounds,
                2.0f);
        Require(
            broadPhase.pairs.size() == 1
                && broadPhase.pairs.front().left == 0
                && broadPhase.pairs.front().right == 1,
            "Spatial hash did not reduce separated 3D pairs.");

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
        const auto oversizedBroadPhase =
            LamaPon::BuildSpatialHashPairs(
                oversizedBounds,
                1.0f);
        Require(
            oversizedBroadPhase.oversizedColliderCount == 1
                && oversizedBroadPhase.pairs.size() == 1,
            "Oversized collider fallback did not preserve candidates.");

        LamaPon::Scene broadPhaseScene(graphics);
        for (int index = 0; index < 64; ++index)
        {
            auto& object =
                broadPhaseScene.CreateGameObject(
                    "分離Collider");
            object.GetTransform().position.x =
                static_cast<float>(index) * 10.0f;
            object.AddComponent<
                LamaPon::BoxCollider3DComponent>();
        }
        broadPhaseScene.Update(0.0f);
        const auto& broadPhaseStats =
            broadPhaseScene.PhysicsStats();
        Require(
            broadPhaseStats.colliderCount3D == 64
                && broadPhaseStats.candidatePairCount3D == 0
                && broadPhaseStats.narrowPhaseTestCount3D == 0,
            "Scene physics did not use spatial broad phase.");

        const LamaPon::Bounds3D raycastBounds{
            { -1.0f, -1.0f, -1.0f },
            { 1.0f, 1.0f, 1.0f }
        };
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

        LamaPon::Scene queryScene(graphics);
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

        LamaPon::PhysicsHit physicsHit{};
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

        LamaPon::Scene controllerScene(graphics);
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
        auto& player =
            controllerScene.CreateGameObject("Player");
        player.GetTransform().position =
            { 0.0f, 2.0f, 0.0f };
        auto& controller = player.AddComponent<
            LamaPon::CharacterControllerComponent>();
        controller.SetUseInput(false);
        controller.SetCollisionMask(1u << 1);

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
        const float groundedHeight =
            player.GetTransform().position.y;
        controller.Jump();
        controllerScene.Update(1.0f / 60.0f);
        Require(
            !controller.IsGrounded()
                && player.GetTransform().position.y
                    > groundedHeight,
            "Character Controller jump failed.");

        LamaPon::Scene controllerRoundTrip(graphics);
        controllerRoundTrip.LoadFromJson(
            controllerScene.SerializeToJson());
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

        LamaPon::Scene navigationScene(graphics);
        auto& navigationObject =
            navigationScene.CreateGameObject(
                "NavMesh");
        auto& navigation =
            navigationObject.AddComponent<
                LamaPon::NavMeshComponent>(
                    DirectX::XMFLOAT2{
                        7.0f,
                        7.0f },
                    1.0f,
                    0.0f,
                    1.8f);
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
        for (std::uint32_t z{};
            z < 7;
            ++z)
        {
            if (z != 3)
            {
                wall.emplace_back(3, z);
            }
        }
        navigation.RestoreBake(wall);
        const auto navigationStart =
            navigation.CellCenter(1, 3);
        const auto navigationDestination =
            navigation.CellCenter(5, 3);
        const auto navigationPath =
            navigation.FindPath(
                navigationStart,
                navigationDestination);
        Require(
            navigationPath.size() >= 2,
            "A* did not find the open NavMesh corridor.");

        auto& navigationAgentObject =
            navigationScene.CreateGameObject(
                "Agent");
        navigationAgentObject.
            GetTransform().position =
                navigationStart;
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

        // パス平滑化：障害物のないグリッドでは視線が通るため
        // 経路は始点と終点の2点に縮退します。
        navigation.RestoreBake({});
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
        auto& upperNavigation =
            upperFloorObject.AddComponent<
                LamaPon::NavMeshComponent>(
                DirectX::XMFLOAT2{ 7.0f, 7.0f },
                1.0f);
        upperNavigation.RestoreBake({});
        auto& upperAgentObject =
            navigationScene.CreateGameObject(
                "上階エージェント");
        upperAgentObject.GetTransform().position =
            { 0.0f, 6.0f, 0.0f };
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

        LamaPon::Scene particleScene(graphics);
        auto& particleObject =
            particleScene.CreateGameObject(
                "Particle probe");
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

        // Update中にGameObjectやComponentを追加しても、
        // 走査中のコンテナ再確保で参照が無効にならないこと。
        {
            LamaPon::Scene spawnScene(graphics);
            auto& spawner =
                spawnScene.CreateGameObject("Spawner");
            // 追加後も後続要素を走査する条件にするため、生成役を先頭に置きます。
            for (int index = 0; index < 4; ++index)
            {
                spawnScene.CreateGameObject(
                    "Bystander " + std::to_string(index));
            }

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

        if (suite == "scene-manager")
        {
        LamaPon::Scene persistentRoundTrip(
            graphics);
        auto& persistentRoundTripRoot =
            persistentRoundTrip.CreateGameObject(
                "Persistent round-trip");
        auto& persistentRoundTripChild =
            persistentRoundTrip.CreateGameObject(
                "Persistent child");
        persistentRoundTripChild.SetParent(
            &persistentRoundTripRoot);
        persistentRoundTrip.DontDestroyOnLoad(
            persistentRoundTripRoot,
            "GameSession");
        LamaPon::Scene restoredPersistence(
            graphics);
        restoredPersistence.LoadFromJson(
            persistentRoundTrip.SerializeToJson());
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
        bool rejectedPersistentChild{};
        try
        {
            persistentRoundTrip.DontDestroyOnLoad(
                persistentRoundTripChild,
                "InvalidChild");
        }
        catch (const std::invalid_argument&)
        {
            rejectedPersistentChild = true;
        }
        Require(
            rejectedPersistentChild,
            "A child GameObject was incorrectly marked persistent.");

        const auto transitionPath =
            outputPath.parent_path()
            / "transition-target.scene.json";
        LamaPon::Scene transitionTarget(
            graphics);
        transitionTarget.CreateGameObject(
            "Transition target");
        auto& bootstrapSession =
            transitionTarget.CreateGameObject(
                "Bootstrap game session");
        transitionTarget.DontDestroyOnLoad(
            bootstrapSession,
            "GameSession");
        transitionTarget.SaveToFile(
            transitionPath);

        auto& persistentSession =
            fileLoaded.CreateGameObject(
                "Runtime game session");
        persistentSession.GetTransform().position.x =
            42.0f;
        auto& persistentSessionChild =
            fileLoaded.CreateGameObject(
                "Runtime session child");
        persistentSessionChild.SetParent(
            &persistentSession);
        fileLoaded.DontDestroyOnLoad(
            persistentSession,
            "GameSession");
        auto* persistentSessionPointer =
            &persistentSession;
        auto* persistentChildPointer =
            &persistentSessionChild;
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

        const auto malformedPath =
            outputPath.parent_path()
            / "malformed.scene.json";
        {
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

        const auto asyncPath =
            outputPath.parent_path()
            / "async-target.scene.json";
        LamaPon::Scene asyncTarget(graphics);
        asyncTarget.CreateGameObject(
            "Async transition target");
        asyncTarget.SaveToFile(asyncPath);
        const auto preloadPath =
            outputPath.parent_path()
            / "background-preload.bin";
        {
            std::ofstream preload(
                preloadPath,
                std::ios::binary
                    | std::ios::trunc);
            preload << "prefetched asset bytes";
        }
        {
            std::ifstream input(
                asyncPath,
                std::ios::binary);
            auto asyncDocument =
                nlohmann::json::parse(input);
            asyncDocument["assetManifest"] =
                nlohmann::json::array({
                    LamaPon::PathToUtf8(
                        preloadPath)
                });
            std::ofstream output(
                asyncPath,
                std::ios::binary
                    | std::ios::trunc);
            output << asyncDocument.dump(2)
                   << '\n';
        }

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

        // 追加シーンの読み込みと破棄を検証します。
        const auto additivePath =
            outputPath.parent_path()
            / "additive-hud.scene.json";
        {
            LamaPon::Scene additiveSource(graphics);
            auto& hudRoot =
                additiveSource.CreateGameObject(
                    "HUD root");
            auto& hudChild =
                additiveSource.CreateGameObject(
                    "HUD child");
            hudChild.SetParent(&hudRoot);
            additiveSource.SaveToFile(additivePath);
        }

        LamaPon::Scene additiveHost(graphics);
        auto& hostRoot =
            additiveHost.CreateGameObject(
                "Host root");
        const auto hostRootId = hostRoot.Id();
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

        auto* mergedRoot =
            additiveHost.FindGameObjectByName(
                "HUD root");
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

        // プロジェクト設定の重力がRigidbodyの速度へ反映されることを
        // 確認します。
        {
            LamaPon::Scene gravityScene(graphics);
            auto& faller =
                gravityScene.CreateGameObject("Faller");
            auto& body =
                faller.AddComponent<
                    LamaPon::RigidbodyComponent>();
            constexpr float step = 1.0f / 60.0f;

            LamaPon::PhysicsSettings moon;
            moon.gravity = { 0.0f, -1.62f, 0.0f };
            LamaPon::SetActivePhysicsSettings(moon);
            body.Integrate(faller, step);
            const float moonVelocity = body.Velocity().y;

            body.SetVelocity({});
            LamaPon::PhysicsSettings earth;
            LamaPon::SetActivePhysicsSettings(earth);
            body.Integrate(faller, step);
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
            LamaPon::Scene clampScene(graphics);
            auto& fast = clampScene.CreateGameObject("Fast");
            auto& body =
                fast.AddComponent<
                    LamaPon::RigidbodyComponent>();
            body.SetUseGravity(false);
            constexpr float step = 1.0f / 60.0f;
            const DirectX::XMFLOAT3 quick{ 100.0f, 0.0f, 0.0f };

            // 既定（頭打ちオフ）。速いままであること。
            LamaPon::PhysicsSettings warnOnly;
            warnOnly.discreteSafeSpeed = 10.0f;
            LamaPon::SetActivePhysicsSettings(warnOnly);
            body.SetVelocity(quick);
            body.Integrate(fast, step);
            const float unclamped = body.Velocity().x;

            // 制限を有効にすると、しきい値まで速度が下がること。
            LamaPon::PhysicsSettings clamped = warnOnly;
            clamped.clampDiscreteSpeed = true;
            LamaPon::SetActivePhysicsSettings(clamped);
            body.SetVelocity(quick);
            body.Integrate(fast, step);
            const float limited = body.Velocity().x;

            // CCDを選んだ物体は対象外であること。
            body.SetCollisionDetection(
                LamaPon::CollisionDetectionMode::Continuous);
            body.SetVelocity(quick);
            body.Integrate(fast, step);
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
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}

int main(const int argumentCount, char** arguments)
{
    std::string_view suite = "serialization";
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--suite")
    {
        suite = arguments[2];
    }
    else if (argumentCount != 1)
    {
        std::cerr
            << "Usage: LamaPonSceneTests "
               "[--suite serialization|simulation|scene-manager]\n";
        return 2;
    }
    if (suite != "serialization"
        && suite != "simulation"
        && suite != "scene-manager")
    {
        std::cerr << "Unknown scene test suite: " << suite << '\n';
        return 2;
    }

    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);

    // Sceneの読み込みで遅延生成されるCOMオブジェクトを先に破棄するため、
    // RunTestから戻った後にCoUninitializeを呼びます。
    const int exitCode = RunTest(suite);

    if (uninitializeCom)
    {
        CoUninitialize();
    }
    return exitCode;
}
