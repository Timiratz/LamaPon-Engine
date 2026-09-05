// マウス照準、Tag検索、Time.timeScaleによる一時停止、UI、
// metallicマテリアル、スポット影、インスタンシング、
// 手続きMesh Collider、CCDを確認する射撃ゲームのサンプルです。
#include "LamaPon/LamaPon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr float RoundDuration = 45.0f;
    constexpr int TargetCount = 6;
    constexpr float BallRespawnInterval = 6.0f;

    // 乱数（決定的なxorshift。ゲーム用途には十分）。
    std::uint32_t NextRandom(std::uint32_t& state) noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float RandomRange(
        std::uint32_t& state,
        const float minimum,
        const float maximum) noexcept
    {
        const float normalized =
            static_cast<float>(NextRandom(state) & 0xffffffu)
            / static_cast<float>(0x1000000u);
        return minimum + (maximum - minimum) * normalized;
    }

    class TargetRangeGame final : public LamaPon::Script
    {
    public:
        void Start() override
        {
            auto& scene = GetScene();
            BuildRange(scene);
            BuildTargets(scene);
            BuildRamp(scene);
            BuildLights(scene);
            BuildHud(scene);
            BuildPauseMenu(scene);

            m_shotAudio = &AddComponent<
                LamaPon::AudioSourceComponent>(
                std::filesystem::path{
                    "audio/startup.wav" },
                0.4f,
                0.0f,
                0.0f,
                false,
                false,
                false,
                1.0f,
                20.0f);

            LamaPon::Time::SetTimeScale(1.0f);

            // コルーチンの使用例：開始メッセージを段階的に
            // 切り替えます（co_awaitで待機）。
            StartCoroutine(IntroSequence());
        }

        LamaPon::Coroutine IntroSequence()
        {
            SetMessage("的を撃て！");
            co_await LamaPon::WaitForSeconds{ 1.6f };
            SetMessage("Pキーで一時停止できます");
            co_await LamaPon::WaitForSeconds{ 2.0f };
            if (m_roundActive)
            {
                SetMessage("");
            }
        }

        void Update(const float deltaTime) override
        {
            // Escapeはランタイム終了に割り当て済みのためPで一時停止。
            const bool pauseKey =
                Graphics().Input().KeyboardState().P;
            if (pauseKey && !m_pauseKeyHeld)
            {
                TogglePause();
            }
            m_pauseKeyHeld = pauseKey;

            UpdateCrosshair();

            if (m_paused)
            {
                UpdatePauseMenu();
                return;
            }

            if (m_roundActive)
            {
                m_timeRemaining -= deltaTime;
                if (m_timeRemaining <= 0.0f)
                {
                    m_timeRemaining = 0.0f;
                    m_roundActive = false;
                    SetMessage(
                        "終了！ スコア "
                        + std::to_string(m_score)
                        + " — 左クリックで再挑戦");
                }
                UpdateAim();
                UpdateTargets(deltaTime);
                UpdateBall(deltaTime);
                UpdateHud();
            }
            else if (Graphics().Input().Pointer()
                .Button(LamaPon::PointerButton::Left)
                .pressed)
            {
                RestartRound();
            }
        }

    private:
        void BuildRange(LamaPon::Scene& scene)
        {
            // 床（BoxColliderで受ける）
            auto& floor =
                scene.CreateGameObject("レンジ床");
            floor.GetTransform().position = { 0.0f, -0.5f, -4.0f };
            floor.GetTransform().scale = { 30.0f, 1.0f, 26.0f };
            floor.AddComponent<
                LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{
                    0.24f, 0.26f, 0.30f, 1.0f },
                std::filesystem::path{},
                std::filesystem::path{},
                0.85f,
                1.0f);
            floor.AddComponent<
                LamaPon::BoxCollider3DComponent>();

            // 奥の柱列：同一マテリアルのキューブ
            // → GPUインスタンシングで1ドローになります。
            for (int index = 0; index < 12; ++index)
            {
                auto& pillar = scene.CreateGameObject(
                    "柱 " + std::to_string(index + 1));
                pillar.GetTransform().position = {
                    -11.0f + 2.0f * static_cast<float>(index),
                    2.5f,
                    -16.0f };
                pillar.GetTransform().scale =
                    { 0.8f, 6.0f, 0.8f };
                pillar.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Cube,
                    DirectX::XMFLOAT4{
                        0.45f, 0.47f, 0.52f, 1.0f },
                    std::filesystem::path{},
                    std::filesystem::path{},
                    0.6f,
                    1.0f);
            }

            // 空
            auto sky = scene.Sky();
            sky.enabled = true;
            sky.topColor = { 0.05f, 0.10f, 0.22f };
            sky.horizonColor = { 0.30f, 0.34f, 0.45f };
            sky.groundColor = { 0.03f, 0.03f, 0.05f };
            sky.intensity = 1.0f;
            scene.SetSkySettings(sky);
        }

        void BuildTargets(LamaPon::Scene& scene)
        {
            for (int index = 0; index < TargetCount; ++index)
            {
                auto& target = scene.CreateGameObject(
                    "的 " + std::to_string(index + 1));
                target.SetTag("Target");
                // 金属球（metallic試験）
                auto& renderer = target.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Sphere,
                    DirectX::XMFLOAT4{
                        1.0f, 0.72f, 0.18f, 1.0f },
                    std::filesystem::path{},
                    std::filesystem::path{},
                    0.22f,
                    1.0f);
                renderer.SetMetallic(0.9f);
                target.AddComponent<
                    LamaPon::SphereCollider3DComponent>(
                    0.5f);
                m_targets.push_back(&target);
                m_targetPop.push_back(1.0f);
                PlaceTarget(*m_targets.back());
            }
        }

        void BuildRamp(LamaPon::Scene& scene)
        {
            // 手続き生成のMesh Collider（3枚の傾斜クアッド）。
            // 見た目は同じ頂点から作った薄板レンダラーで一致させます。
            auto& ramp = scene.CreateGameObject("スロープ");
            auto& collider = ramp.AddComponent<
                LamaPon::MeshCollider3DComponent>();
            std::vector<DirectX::XMFLOAT3> vertices;
            std::vector<std::uint32_t> indices;
            const DirectX::XMFLOAT3 segmentHeights{
                3.4f, 2.2f, 1.0f };
            constexpr float SegmentLength = 2.4f;
            constexpr float RampX = 12.0f;
            constexpr float HalfWidth = 1.4f;
            float startZ = -12.0f;
            const float heights[4]{
                segmentHeights.x,
                segmentHeights.y,
                segmentHeights.z,
                0.0f };
            for (int segment = 0; segment < 3; ++segment)
            {
                const float nearZ =
                    startZ
                    + SegmentLength
                        * static_cast<float>(segment);
                const float farZ = nearZ + SegmentLength;
                const float topY = heights[segment];
                const float bottomY = heights[segment + 1];
                const auto baseVertex =
                    static_cast<std::uint32_t>(
                        vertices.size());
                vertices.push_back(
                    { RampX - HalfWidth, topY, nearZ });
                vertices.push_back(
                    { RampX + HalfWidth, topY, nearZ });
                vertices.push_back(
                    { RampX + HalfWidth, bottomY, farZ });
                vertices.push_back(
                    { RampX - HalfWidth, bottomY, farZ });
                indices.insert(
                    indices.end(),
                    {
                        baseVertex,
                        baseVertex + 1,
                        baseVertex + 2,
                        baseVertex,
                        baseVertex + 2,
                        baseVertex + 3
                    });

                // 対応する見た目（傾き付きの薄いキューブ）
                auto& panel = scene.CreateGameObject(
                    "スロープ板 "
                    + std::to_string(segment + 1));
                const float centerY =
                    (topY + bottomY) * 0.5f;
                const float centerZ =
                    (nearZ + farZ) * 0.5f;
                const float drop = topY - bottomY;
                const float length = std::sqrt(
                    SegmentLength * SegmentLength
                    + drop * drop);
                panel.GetTransform().position =
                    { RampX, centerY, centerZ };
                panel.GetTransform().SetEulerAngles(
                    -std::atan2(drop, SegmentLength),
                    0.0f,
                    0.0f);
                panel.GetTransform().scale =
                    { HalfWidth * 2.0f, 0.08f, length };
                panel.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Cube,
                    DirectX::XMFLOAT4{
                        0.55f, 0.35f, 0.25f, 1.0f },
                    std::filesystem::path{},
                    std::filesystem::path{},
                    0.7f,
                    1.0f);
            }
            collider.SetMesh(
                std::move(vertices),
                std::move(indices));

            // CCD付きの金属ボールが転がり続けます。
            auto& ball = scene.CreateGameObject(
                "スロープボール");
            auto& ballRenderer = ball.AddComponent<
                LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Sphere,
                DirectX::XMFLOAT4{
                    0.85f, 0.88f, 0.95f, 1.0f },
                std::filesystem::path{},
                std::filesystem::path{},
                0.15f,
                1.0f);
            ballRenderer.SetMetallic(1.0f);
            ball.AddComponent<
                LamaPon::SphereCollider3DComponent>(
                0.35f);
            auto& body = ball.AddComponent<
                LamaPon::RigidbodyComponent>();
            body.SetCollisionDetection(
                LamaPon::CollisionDetectionMode::
                    Continuous);
            m_ball = &ball;
            ResetBall();
        }

        void BuildLights(LamaPon::Scene& scene)
        {
            auto& sun = scene.CreateGameObject("太陽光");
            sun.GetTransform().SetEulerAngles(
                { -0.9f, 0.6f, 0.0f });
            auto& directional = sun.AddComponent<
                LamaPon::DirectionalLightComponent>();
            directional.SetColor(
                { 0.65f, 0.70f, 0.85f });
            directional.SetIntensity(0.9f);

            // 的の真上からスポットライトの影を落とします。
            auto& spotObject =
                scene.CreateGameObject("レンジ照明");
            spotObject.GetTransform().position =
                { 0.0f, 9.0f, -6.0f };
            spotObject.GetTransform().SetEulerAngles(
                -1.25f,
                0.0f,
                0.0f);
            auto& spot = spotObject.AddComponent<
                LamaPon::SpotLightComponent>(
                DirectX::XMFLOAT3{ 1.0f, 0.95f, 0.8f },
                18.0f,
                26.0f,
                DirectX::XMConvertToRadians(28.0f),
                DirectX::XMConvertToRadians(42.0f));
            spot.SetCastsShadows(true);
            m_spotLight = &spot;
        }

        LamaPon::TextRendererComponent& CreateLabel(
            LamaPon::Scene& scene,
            const std::string& objectName,
            const std::string& text,
            const DirectX::XMFLOAT2 anchor,
            const DirectX::XMFLOAT2 pivot,
            const DirectX::XMFLOAT2 anchoredPosition,
            const DirectX::XMFLOAT2 size,
            const float fontSize)
        {
            auto& object =
                scene.CreateGameObject(objectName);
            object.AddComponent<
                LamaPon::UIRectTransformComponent>(
                anchor,
                anchor,
                pivot,
                anchoredPosition,
                size);
            auto& label = object.AddComponent<
                LamaPon::TextRendererComponent>(
                text,
                std::string{ "Yu Gothic UI" },
                fontSize,
                DirectX::XMFLOAT4{
                    1.0f, 1.0f, 1.0f, 1.0f },
                size,
                false,
                LamaPon::TextHorizontalAlignment::Left,
                LamaPon::TextVerticalAlignment::Center);
            label.SetSortOrder(10);
            return label;
        }

        void BuildHud(LamaPon::Scene& scene)
        {
            m_scoreText = &CreateLabel(
                scene,
                "HUD スコア",
                "スコア 0",
                { 0.0f, 0.0f },
                { 0.0f, 0.0f },
                { 24.0f, 24.0f },
                { 300.0f, 52.0f },
                30.0f);
            m_timeText = &CreateLabel(
                scene,
                "HUD 時間",
                "残り 45秒",
                { 1.0f, 0.0f },
                { 1.0f, 0.0f },
                { -24.0f, 24.0f },
                { 260.0f, 52.0f },
                30.0f);
            m_messageText = &CreateLabel(
                scene,
                "HUD メッセージ",
                "的をクリックで撃て！ Pで一時停止",
                { 0.5f, 1.0f },
                { 0.5f, 1.0f },
                { 0.0f, -28.0f },
                { 760.0f, 56.0f },
                26.0f);

            // 照準（ポインタ追従のUI Image）
            auto& crosshair =
                scene.CreateGameObject("照準");
            m_crosshairTransform =
                &crosshair.AddComponent<
                    LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 12.0f, 12.0f });
            auto& crosshairImage =
                crosshair.AddComponent<
                    LamaPon::UIImageComponent>(
                std::filesystem::path{},
                DirectX::XMFLOAT4{
                    1.0f, 0.25f, 0.25f, 0.9f });
            crosshairImage.SetSortOrder(20);
        }

        void BuildPauseMenu(LamaPon::Scene& scene)
        {
            auto& panel =
                scene.CreateGameObject("ポーズパネル");
            panel.AddComponent<
                LamaPon::UIRectTransformComponent>(
                DirectX::XMFLOAT2{ 0.5f, 0.5f },
                DirectX::XMFLOAT2{ 0.5f, 0.5f },
                DirectX::XMFLOAT2{ 0.5f, 0.5f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 460.0f, 320.0f });
            auto& background = panel.AddComponent<
                LamaPon::UIImageComponent>(
                std::filesystem::path{},
                DirectX::XMFLOAT4{
                    0.05f, 0.07f, 0.12f, 0.92f });
            background.SetSortOrder(30);

            const auto addChildRect =
                [&scene, &panel](
                    const std::string& name,
                    const DirectX::XMFLOAT2 position,
                    const DirectX::XMFLOAT2 size)
                    -> LamaPon::GameObject&
            {
                auto& child =
                    scene.CreateGameObject(name);
                child.SetParent(&panel);
                child.AddComponent<
                    LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    position,
                    size);
                return child;
            };

            auto& title = addChildRect(
                "ポーズ見出し",
                { 24.0f, 20.0f },
                { 412.0f, 48.0f });
            auto& titleText = title.AddComponent<
                LamaPon::TextRendererComponent>(
                std::string{ "一時停止中" },
                std::string{ "Yu Gothic UI" },
                30.0f,
                DirectX::XMFLOAT4{
                    1.0f, 1.0f, 1.0f, 1.0f },
                DirectX::XMFLOAT2{ 412.0f, 48.0f },
                false,
                LamaPon::TextHorizontalAlignment::Center,
                LamaPon::TextVerticalAlignment::Center);
            titleText.SetSortOrder(31);

            auto& sliderObject = addChildRect(
                "SE音量スライダー",
                { 24.0f, 96.0f },
                { 412.0f, 30.0f });
            m_volumeSlider = &sliderObject.AddComponent<
                LamaPon::UISliderComponent>(
                0.0f,
                1.0f,
                0.4f);
            m_volumeSlider->SetSortOrder(31);

            auto& toggleObject = addChildRect(
                "影トグル",
                { 24.0f, 152.0f },
                { 412.0f, 40.0f });
            m_shadowToggle = &toggleObject.AddComponent<
                LamaPon::UIToggleComponent>(
                std::string{ "スポットライトの影" },
                true);
            m_shadowToggle->SetSortOrder(31);

            auto& buttonObject = addChildRect(
                "再開ボタン",
                { 110.0f, 226.0f },
                { 240.0f, 60.0f });
            m_resumeButton = &buttonObject.AddComponent<
                LamaPon::UIButtonComponent>(
                std::string{ "再開 (P)" });
            m_resumeButton->SetSortOrder(31);

            m_pausePanel = &panel;
            panel.SetEnabled(false);
        }

        void PlaceTarget(LamaPon::GameObject& target)
        {
            target.GetTransform().position = {
                RandomRange(m_randomState, -8.0f, 8.0f),
                RandomRange(m_randomState, 1.2f, 5.5f),
                RandomRange(m_randomState, -13.0f, -8.0f)
            };
            target.GetTransform().scale =
                { 0.1f, 0.1f, 0.1f };
        }

        void UpdateCrosshair()
        {
            const auto& pointer =
                Graphics().Input().Pointer();
            if (m_crosshairTransform != nullptr
                && pointer.valid)
            {
                m_crosshairTransform->SetAnchoredPosition(
                    pointer.position);
            }
        }

        [[nodiscard]] bool ComputeAimRay(
            LamaPon::Ray& ray) const
        {
            // GetScene()はScript基底の初心者向けショートカット。
            const auto* camera = GetScene().MainCamera();
            const auto& pointer =
                Graphics().Input().Pointer();
            if (camera == nullptr || !pointer.valid)
            {
                return false;
            }
            using namespace DirectX;
            // 描画と同じ行列でカーソル位置をレイへ逆射影します。
            const XMMATRIX view = camera->ViewMatrix();
            const XMMATRIX projection =
                camera->ProjectionMatrix(
                    Graphics().AspectRatio());
            XMVECTOR determinant{};
            const XMMATRIX inverseViewProjection =
                XMMatrixInverse(
                    &determinant,
                    view * projection);
            const float width = static_cast<float>(
                Graphics().UIWidth());
            const float height = static_cast<float>(
                Graphics().UIHeight());
            const float ndcX =
                pointer.position.x
                    / std::max(width, 1.0f)
                    * 2.0f
                - 1.0f;
            const float ndcY =
                1.0f
                - pointer.position.y
                    / std::max(height, 1.0f)
                    * 2.0f;
            const XMVECTOR nearPoint =
                XMVector3TransformCoord(
                    XMVectorSet(ndcX, ndcY, 0.0f, 1.0f),
                    inverseViewProjection);
            const XMVECTOR farPoint =
                XMVector3TransformCoord(
                    XMVectorSet(ndcX, ndcY, 1.0f, 1.0f),
                    inverseViewProjection);
            XMFLOAT3 origin{};
            XMFLOAT3 direction{};
            XMStoreFloat3(&origin, nearPoint);
            XMStoreFloat3(
                &direction,
                XMVector3Normalize(
                    XMVectorSubtract(
                        farPoint,
                        nearPoint)));
            ray = { origin, direction };
            return true;
        }

        void UpdateAim()
        {
            const auto& pointer =
                Graphics().Input().Pointer();
            if (!pointer
                    .Button(LamaPon::PointerButton::Left)
                    .pressed)
            {
                return;
            }
            LamaPon::Ray ray{};
            if (!ComputeAimRay(ray))
            {
                return;
            }
            LamaPon::PhysicsHit hit{};
            if (!GetScene().Raycast(
                    ray,
                    300.0f,
                    hit)
                || hit.gameObject == nullptr)
            {
                return;
            }
            if (!hit.gameObject->CompareTag("Target"))
            {
                return;
            }
            ++m_score;
            if (m_shotAudio != nullptr)
            {
                m_shotAudio->PlayOneShot();
            }
            // Invokeタイマーの使用例：命中表示を0.8秒後に戻します。
            SetMessage("ナイスショット！ +1");
            CancelInvoke(m_messageResetHandle);
            m_messageResetHandle = Invoke(
                0.8f,
                [this]
                {
                    if (m_roundActive)
                    {
                        SetMessage(
                            "的をクリックで撃て！ "
                            "Pで一時停止");
                    }
                });
            // 命中した的を別の位置へ湧き直し
            for (std::size_t index = 0;
                index < m_targets.size();
                ++index)
            {
                if (m_targets[index] == hit.gameObject)
                {
                    PlaceTarget(*m_targets[index]);
                    m_targetPop[index] = 0.0f;
                    break;
                }
            }
        }

        void UpdateTargets(const float deltaTime)
        {
            // 出現ポップ（スケールアニメーション）。インスタンス
            // 描画でもワールド行列は毎フレーム反映されます。
            for (std::size_t index = 0;
                index < m_targets.size();
                ++index)
            {
                m_targetPop[index] = std::min(
                    m_targetPop[index]
                        + deltaTime * 3.0f,
                    1.0f);
                const float scale =
                    0.1f + 0.9f * m_targetPop[index];
                m_targets[index]->GetTransform().scale =
                    { scale, scale, scale };
            }
        }

        void ResetBall()
        {
            if (m_ball == nullptr)
            {
                return;
            }
            m_ball->GetTransform().position =
                { 12.0f, 4.4f, -12.4f };
            if (auto* body = m_ball->GetComponent<
                LamaPon::RigidbodyComponent>())
            {
                body->SetVelocity(
                    { 0.0f, 0.0f, 2.0f });
                body->SetAngularVelocity(
                    { 0.0f, 0.0f, 0.0f });
            }
            m_ballRespawnTimer = 0.0f;
        }

        void UpdateBall(const float deltaTime)
        {
            m_ballRespawnTimer += deltaTime;
            const bool fell =
                m_ball != nullptr
                && m_ball->GetTransform().position.y
                    < -8.0f;
            if (fell
                || m_ballRespawnTimer
                    >= BallRespawnInterval)
            {
                ResetBall();
            }
        }

        void UpdateHud()
        {
            // テキストテクスチャキャッシュを増やしすぎないよう
            // 秒単位・変化時のみ更新します。
            if (m_scoreText != nullptr
                && m_score != m_displayedScore)
            {
                m_displayedScore = m_score;
                m_scoreText->SetText(
                    "スコア "
                    + std::to_string(m_score));
            }
            const int seconds = std::max(
                static_cast<int>(
                    std::ceil(m_timeRemaining)),
                0);
            if (m_timeText != nullptr
                && seconds != m_displayedSeconds)
            {
                m_displayedSeconds = seconds;
                m_timeText->SetText(
                    "残り "
                    + std::to_string(seconds)
                    + "秒");
            }
        }

        void SetMessage(const std::string& message)
        {
            if (m_messageText != nullptr)
            {
                m_messageText->SetText(message);
            }
        }

        void TogglePause()
        {
            m_paused = !m_paused;
            LamaPon::Time::SetTimeScale(
                m_paused ? 0.0f : 1.0f);
            if (m_pausePanel != nullptr)
            {
                m_pausePanel->SetEnabled(m_paused);
            }
        }

        void UpdatePauseMenu()
        {
            if (m_volumeSlider != nullptr
                && m_volumeSlider->ConsumeValueChanged())
            {
                // ミキサーの効果音バスをまとめて調整します。
                Graphics().Audio().SetBusVolume(
                    LamaPon::AudioBus::Effects,
                    m_volumeSlider->Value());
            }
            if (m_shadowToggle != nullptr
                && m_shadowToggle->ConsumeValueChanged()
                && m_spotLight != nullptr)
            {
                m_spotLight->SetCastsShadows(
                    m_shadowToggle->IsOn());
            }
            if (m_resumeButton != nullptr
                && m_resumeButton->ConsumeClick())
            {
                TogglePause();
            }
        }

        void RestartRound()
        {
            m_score = 0;
            m_displayedScore = -1;
            m_displayedSeconds = -1;
            m_timeRemaining = RoundDuration;
            m_roundActive = true;
            SetMessage(
                "的をクリックで撃て！ Pで一時停止");
            for (std::size_t index = 0;
                index < m_targets.size();
                ++index)
            {
                PlaceTarget(*m_targets[index]);
                m_targetPop[index] = 0.0f;
            }
            ResetBall();
        }

        std::vector<LamaPon::GameObject*> m_targets;
        std::vector<float> m_targetPop;
        LamaPon::GameObject* m_ball{};
        LamaPon::GameObject* m_pausePanel{};
        LamaPon::UIRectTransformComponent*
            m_crosshairTransform{};
        LamaPon::TextRendererComponent* m_scoreText{};
        LamaPon::TextRendererComponent* m_timeText{};
        LamaPon::TextRendererComponent* m_messageText{};
        LamaPon::UISliderComponent* m_volumeSlider{};
        LamaPon::UIToggleComponent* m_shadowToggle{};
        LamaPon::UIButtonComponent* m_resumeButton{};
        LamaPon::SpotLightComponent* m_spotLight{};
        LamaPon::AudioSourceComponent* m_shotAudio{};
        int m_score{};
        int m_displayedScore{ -1 };
        int m_displayedSeconds{ -1 };
        float m_timeRemaining{ RoundDuration };
        float m_ballRespawnTimer{};
        bool m_paused{};
        bool m_pauseKeyHeld{};
        bool m_roundActive{ true };
        std::uint64_t m_messageResetHandle{};
        std::uint32_t m_randomState{ 0x2f6e2b1u };
    };
}

LAMAPON_SCRIPT_NAMED(
    TargetRangeGame,
    "TargetRange.Game",
    "Target Range Game")
