#include "LamaPon/LamaPon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    // 1ラウンドの制限秒数
    constexpr float RoundDuration = 45.0f;
    // 的の生成個数
    constexpr int TargetCount = 6;
    // ボールの再配置間隔
    constexpr float BallRespawnInterval = 6.0f;

    // 決定的な乱数を生成します(state: 乱数状態)。
    std::uint32_t NextRandom(std::uint32_t& state) noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // 指定範囲の乱数を返します(state: 乱数状態, minimum: 下限, maximum: 上限)。
    float RandomRange(
        std::uint32_t& state,
        const float minimum,
        const float maximum) noexcept
    {
        // 24bit乱数を0～1未満へ正規化した値
        const float normalized =
            static_cast<float>(NextRandom(state) & 0xffffffu)
            / static_cast<float>(0x1000000u);
        return minimum + (maximum - minimum) * normalized;
    }

    class TargetRangeGame final : public LamaPon::Script
    {
    public:
        // レンジ・的・照明・UIを作り、開始処理を予約します。
        void Start() override
        {
            // Scene生成先
            auto& scene = GetScene();
            BuildRange(scene);
            BuildTargets(scene);
            BuildRamp(scene);
            BuildLights(scene);
            BuildHud(scene);
            BuildPauseMenu(scene);

            // 的を撃ったときに使う効果音
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

            // Coroutineで開始メッセージを段階表示します。
            StartCoroutine(IntroSequence());
        }

        // 開始案内を表示してラウンドへ移ります。
        LamaPon::Coroutine IntroSequence()
        {
            SetMessage("的を撃て！");
            co_await LamaPon::WaitForSeconds{ 1.6f };
            SetMessage("Pキーで一時停止できます");
            co_await LamaPon::WaitForSeconds{ 2.0f };
            // ラウンド中なら開始案内を消します。
            if (m_roundActive)
            {
                SetMessage("");
            }
        }

        // 入力と進行中のゲーム状態を更新します(deltaTime: 経過秒)。
        void Update(const float deltaTime) override
        {
            // Pキーが押されているか
            const bool pauseKey =
                Graphics().Input().KeyboardState().P;
            // Pキーの押し始めだけ一時停止を切り替えます。
            if (pauseKey && !m_pauseKeyHeld)
            {
                TogglePause();
            }
            m_pauseKeyHeld = pauseKey;

            // 照準をポインタ位置へ合わせます。
            UpdateCrosshair();

            // 一時停止中はメニュー操作だけ処理します。
            if (m_paused)
            {
                UpdatePauseMenu();
                return;
            }

            // ラウンド中の時間・的・ボール・HUDを更新します。
            if (m_roundActive)
            {
                // 残り時間が尽きたか調べます。
                m_timeRemaining -= deltaTime;
                // 終了時刻を過ぎたラウンドを終了します。
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
            // 終了中に左クリックされたら再開します。
            else if (Graphics().Input().Pointer()
                .Button(LamaPon::PointerButton::Left)
                .pressed)
            {
                RestartRound();
            }
        }

    private:
        // 射撃レンジの床・柱・空を作ります(scene: 生成先Scene)。
        void BuildRange(LamaPon::Scene& scene)
        {
            // 床のGameObject
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

            // 奥の柱を等間隔に並べます(index: 柱番号)。
            for (int index = 0; index < 12; ++index)
            {
                // 現在の柱
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

            // レンジを照らすSky設定
            auto sky = scene.Sky();
            sky.enabled = true;
            sky.topColor = { 0.05f, 0.10f, 0.22f };
            sky.horizonColor = { 0.30f, 0.34f, 0.45f };
            sky.groundColor = { 0.03f, 0.03f, 0.05f };
            sky.intensity = 1.0f;
            scene.SetSkySettings(sky);
        }

        // 的のGameObjectと当たり判定を作ります(scene: 生成先Scene)。
        void BuildTargets(LamaPon::Scene& scene)
        {
            // 的を必要数だけ生成します(index: 的番号)。
            for (int index = 0; index < TargetCount; ++index)
            {
                // 現在の的
                auto& target = scene.CreateGameObject(
                    "的 " + std::to_string(index + 1));
                target.SetTag("Target");
                // 金属球（metallic試験）
                // 的の見た目と金属度
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
                // 的へ当たり判定を付けます。
                target.AddComponent<
                    LamaPon::SphereCollider3DComponent>(
                    0.5f);
                m_targets.push_back(&target);
                // 的の初期出現率
                m_targetPop.push_back(1.0f);
                PlaceTarget(*m_targets.back());
            }
        }

        // 傾斜板と物理ボールを作ります(scene: 生成先Scene)。
        void BuildRamp(LamaPon::Scene& scene)
        {
            // 傾斜メッシュを持つGameObject
            auto& ramp = scene.CreateGameObject("スロープ");
            // 傾斜板の衝突形状
            auto& collider = ramp.AddComponent<
                LamaPon::MeshCollider3DComponent>();
            // 衝突メッシュの頂点一覧
            std::vector<DirectX::XMFLOAT3> vertices;
            // 衝突メッシュの三角形一覧
            std::vector<std::uint32_t> indices;
            // 各傾斜区間の高さ
            const DirectX::XMFLOAT3 segmentHeights{
                3.4f, 2.2f, 1.0f };
            // 傾斜一区間の奥行き
            constexpr float SegmentLength = 2.4f;
            // 傾斜板の横位置
            constexpr float RampX = 12.0f;
            // 傾斜板の半幅
            constexpr float HalfWidth = 1.4f;
            // 最初の区間の奥側位置
            float startZ = -12.0f;
            // 各区間の始点・終点高さ
            const float heights[4]{
                segmentHeights.x,
                segmentHeights.y,
                segmentHeights.z,
                0.0f };
            // 3区間の衝突板と見た目を生成します(segment: 区間番号)。
            for (int segment = 0; segment < 3; ++segment)
            {
                // 現区間の手前側Z座標
                const float nearZ =
                    startZ
                    + SegmentLength
                        * static_cast<float>(segment);
                // 現区間の奥側Z座標
                const float farZ = nearZ + SegmentLength;
                // 傾斜板の手前側高さ
                const float topY = heights[segment];
                // 傾斜板の奥側高さ
                const float bottomY = heights[segment + 1];
                // 頂点配列へ追加する先頭位置
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

                // 現区間に対応する見た目
                auto& panel = scene.CreateGameObject(
                    "スロープ板 "
                    + std::to_string(segment + 1));
                // 見た目板の中心高さ
                const float centerY =
                    (topY + bottomY) * 0.5f;
                // 見た目板の中心奥行き
                const float centerZ =
                    (nearZ + farZ) * 0.5f;
                // 区間の落差
                const float drop = topY - bottomY;
                // 落差を含めた板の長さ
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
            // 生成した頂点と三角形を衝突形状へ設定します。
            collider.SetMesh(
                std::move(vertices),
                std::move(indices));

            // 傾斜を転がる物理ボール
            auto& ball = scene.CreateGameObject(
                "スロープボール");
            // ボールの見た目と金属度
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
            // 連続衝突判定を使う剛体
            auto& body = ball.AddComponent<
                LamaPon::RigidbodyComponent>();
            body.SetCollisionDetection(
                LamaPon::CollisionDetectionMode::
                    Continuous);
            m_ball = &ball;
            ResetBall();
        }

        // 太陽光と的を照らすスポットライトを作ります(scene: 生成先Scene)。
        void BuildLights(LamaPon::Scene& scene)
        {
            // 環境を照らす太陽光
            auto& sun = scene.CreateGameObject("太陽光");
            sun.GetTransform().SetEulerAngles(
                { -0.9f, 0.6f, 0.0f });
            // 太陽光の設定
            auto& directional = sun.AddComponent<
                LamaPon::DirectionalLightComponent>();
            directional.SetColor(
                { 0.65f, 0.70f, 0.85f });
            directional.SetIntensity(0.9f);

            // 的の上から照らすスポットライト
            auto& spotObject =
                scene.CreateGameObject("レンジ照明");
            spotObject.GetTransform().position =
                { 0.0f, 9.0f, -6.0f };
            spotObject.GetTransform().SetEulerAngles(
                -1.25f,
                0.0f,
                0.0f);
            // 影を落とすSpot Light設定
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

        // HUDラベルを作ります(scene: 生成先Scene, objectName: Object名, text: 表示文字列, anchor: Anchor比率, pivot: Pivot比率, anchoredPosition: Anchorからの位置, size: 描画寸法, fontSize: 文字サイズ)。
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
            // ラベルを配置するGameObject
            auto& object =
                scene.CreateGameObject(objectName);
            object.AddComponent<
                LamaPon::UIRectTransformComponent>(
                anchor,
                anchor,
                pivot,
                anchoredPosition,
                size);
            // 表示文字列を描くTextRenderer
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

        // スコア・残り時間・照準のHUDを作ります(scene: 生成先Scene)。
        void BuildHud(LamaPon::Scene& scene)
        {
            // スコア表示
            m_scoreText = &CreateLabel(
                scene,
                "HUD スコア",
                "スコア 0",
                { 0.0f, 0.0f },
                { 0.0f, 0.0f },
                { 24.0f, 24.0f },
                { 300.0f, 52.0f },
                30.0f);
            // 残り時間表示
            m_timeText = &CreateLabel(
                scene,
                "HUD 時間",
                "残り 45秒",
                { 1.0f, 0.0f },
                { 1.0f, 0.0f },
                { -24.0f, 24.0f },
                { 260.0f, 52.0f },
                30.0f);
            // 中央の案内表示
            m_messageText = &CreateLabel(
                scene,
                "HUD メッセージ",
                "的をクリックで撃て！ Pで一時停止",
                { 0.5f, 1.0f },
                { 0.5f, 1.0f },
                { 0.0f, -28.0f },
                { 760.0f, 56.0f },
                26.0f);

            // ポインタに追従する照準Object
            auto& crosshair =
                scene.CreateGameObject("照準");
            // ポインタへ合わせる照準Rect
            m_crosshairTransform =
                &crosshair.AddComponent<
                    LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.5f, 0.5f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 12.0f, 12.0f });
            // 照準の赤いImage
            auto& crosshairImage =
                crosshair.AddComponent<
                    LamaPon::UIImageComponent>(
                std::filesystem::path{},
                DirectX::XMFLOAT4{
                    1.0f, 0.25f, 0.25f, 0.9f });
            crosshairImage.SetSortOrder(20);
        }

        // 一時停止パネルと操作部品を作ります(scene: 生成先Scene)。
        void BuildPauseMenu(LamaPon::Scene& scene)
        {
            // 全画面に重ねる一時停止Panel
            auto& panel =
                scene.CreateGameObject("ポーズパネル");
            panel.AddComponent<
                LamaPon::UIRectTransformComponent>(
                DirectX::XMFLOAT2{ 0.5f, 0.5f },
                DirectX::XMFLOAT2{ 0.5f, 0.5f },
                DirectX::XMFLOAT2{ 0.5f, 0.5f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 460.0f, 320.0f });
            // Panelの背景色
            auto& background = panel.AddComponent<
                LamaPon::UIImageComponent>(
                std::filesystem::path{},
                DirectX::XMFLOAT4{
                    0.05f, 0.07f, 0.12f, 0.92f });
            background.SetSortOrder(30);

            // Panelの子UIを配置します(name: Object名, position: 左上位置, size: 描画寸法)。
            const auto addChildRect =
                [&scene, &panel](
                    const std::string& name,
                    const DirectX::XMFLOAT2 position,
                    const DirectX::XMFLOAT2 size)
                    -> LamaPon::GameObject&
            {
                // Panelへ追加する子Object
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

            // 一時停止メニューの見出し
            auto& title = addChildRect(
                "ポーズ見出し",
                { 24.0f, 20.0f },
                { 412.0f, 48.0f });
            // 見出しの文字表示
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

            // 効果音量を調節するSlider
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

            // 影設定を切り替えるToggle
            auto& toggleObject = addChildRect(
                "影トグル",
                { 24.0f, 152.0f },
                { 412.0f, 40.0f });
            m_shadowToggle = &toggleObject.AddComponent<
                LamaPon::UIToggleComponent>(
                std::string{ "スポットライトの影" },
                true);
            m_shadowToggle->SetSortOrder(31);

            // ゲームへ戻るButton
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

        // 的をランダムな位置へ配置します(target: 移動する的)。
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

        // 照準Rectをポインタ位置へ動かします。
        void UpdateCrosshair()
        {
            // 現在のポインタ状態
            const auto& pointer =
                Graphics().Input().Pointer();
            // 有効なポインタ座標を照準へ反映します。
            if (m_crosshairTransform != nullptr
                && pointer.valid)
            {
                m_crosshairTransform->SetAnchoredPosition(
                    pointer.position);
            }
        }

        // ポインタ座標を3D照準Rayへ変換します(ray: 出力するRay)。
        [[nodiscard]] bool ComputeAimRay(
            LamaPon::Ray& ray) const
        {
            // Sceneの主カメラ
            const auto* camera = GetScene().MainCamera();
            // 現在のポインタ状態
            const auto& pointer =
                Graphics().Input().Pointer();
            // カメラかポインタが無ければ照準Rayを作れません。
            if (camera == nullptr || !pointer.valid)
            {
                return false;
            }
            using namespace DirectX;
            // カメラのView行列
            const XMMATRIX view = camera->ViewMatrix();
            // 現画面比率に対応するProjection行列
            const XMMATRIX projection =
                camera->ProjectionMatrix(
                    Graphics().AspectRatio());
            // 逆行列計算で得る行列式
            XMVECTOR determinant{};
            // ViewProjection行列の逆行列
            const XMMATRIX inverseViewProjection =
                XMMatrixInverse(
                    &determinant,
                    view * projection);
            // UI座標系の横幅
            const float width = static_cast<float>(
                Graphics().UIWidth());
            // UI座標系の高さ
            const float height = static_cast<float>(
                Graphics().UIHeight());
            // ポインタの横座標を正規化デバイス座標へ変換
            const float ndcX =
                pointer.position.x
                    / std::max(width, 1.0f)
                    * 2.0f
                - 1.0f;
            // ポインタの縦座標を上下反転して正規化
            const float ndcY =
                1.0f
                - pointer.position.y
                    / std::max(height, 1.0f)
                    * 2.0f;
            // 近クリップ面上の照準点
            const XMVECTOR nearPoint =
                XMVector3TransformCoord(
                    XMVectorSet(ndcX, ndcY, 0.0f, 1.0f),
                    inverseViewProjection);
            // 遠クリップ面上の照準点
            const XMVECTOR farPoint =
                XMVector3TransformCoord(
                    XMVectorSet(ndcX, ndcY, 1.0f, 1.0f),
                    inverseViewProjection);
            // Rayの原点
            XMFLOAT3 origin{};
            // Rayの正規化方向
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

        // 入力位置のRaycastで的を撃ちます。
        void UpdateAim()
        {
            // 現在のポインタ状態
            const auto& pointer =
                Graphics().Input().Pointer();
            // 左クリックが無ければ照準を処理しません。
            if (!pointer
                    .Button(LamaPon::PointerButton::Left)
                    .pressed)
            {
                return;
            }
            // クリック位置から作る照準Ray
            LamaPon::Ray ray{};
            // カメラまたはポインタが無効なら処理しません。
            if (!ComputeAimRay(ray))
            {
                return;
            }
            // Raycastの衝突結果
            LamaPon::PhysicsHit hit{};
            // 衝突先が無い場合は命中しません。
            if (!GetScene().Raycast(
                    ray,
                    300.0f,
                    hit)
                || hit.gameObject == nullptr)
            {
                return;
            }
            // Targetタグ以外は命中対象にしません。
            if (!hit.gameObject->CompareTag("Target"))
            {
                return;
            }
            ++m_score;
            // 効果音Componentがあれば発音します。
            if (m_shotAudio != nullptr)
            {
                m_shotAudio->PlayOneShot();
            }
            // 命中表示をラウンド案内へ戻す予約を更新します。
            SetMessage("ナイスショット！ +1");
            CancelInvoke(m_messageResetHandle);
            m_messageResetHandle = Invoke(
                0.8f,
                [this]
                {
                    // ラウンド継続中だけ開始案内へ戻します。
                    if (m_roundActive)
                    {
                        SetMessage(
                            "的をクリックで撃て！ "
                            "Pで一時停止");
                    }
                });
            // 命中した的の位置と出現率をリセットします(index: 的一覧の番号)。
            for (std::size_t index = 0;
                index < m_targets.size();
                ++index)
            {
                // 命中した的を見つけたら再配置します。
                if (m_targets[index] == hit.gameObject)
                {
                    PlaceTarget(*m_targets[index]);
                    m_targetPop[index] = 0.0f;
                    break;
                }
            }
        }

        // 的の出現アニメーションを進めます(deltaTime: 経過秒)。
        void UpdateTargets(const float deltaTime)
        {
            // 各的の出現率を最大値まで進めます(index: 的一覧の番号)。
            for (std::size_t index = 0;
                index < m_targets.size();
                ++index)
            {
                m_targetPop[index] = std::min(
                    m_targetPop[index]
                        + deltaTime * 3.0f,
                    1.0f);
                // 現在の出現率から求めた一辺の倍率
                const float scale =
                    0.1f + 0.9f * m_targetPop[index];
                m_targets[index]->GetTransform().scale =
                    { scale, scale, scale };
            }
        }

        // 物理ボールを初期位置へ戻します。
        void ResetBall()
        {
            // ボールが未生成なら何もしません。
            if (m_ball == nullptr)
            {
                return;
            }
            m_ball->GetTransform().position =
                { 12.0f, 4.4f, -12.4f };
            // ボールに付いた剛体
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

        // ボールの落下と再配置時間を監視します(deltaTime: 経過秒)。
        void UpdateBall(const float deltaTime)
        {
            m_ballRespawnTimer += deltaTime;
            // ボールがレンジ下へ落ちたか
            const bool fell =
                m_ball != nullptr
                && m_ball->GetTransform().position.y
                    < -8.0f;
            // 落下または間隔経過でボールを戻します。
            if (fell
                || m_ballRespawnTimer
                    >= BallRespawnInterval)
            {
                ResetBall();
            }
        }

        // 表示内容が変わったHUDだけ更新します。
        void UpdateHud()
        {
            // スコアが変わった場合だけ文字を更新します。
            if (m_scoreText != nullptr
                && m_score != m_displayedScore)
            {
                m_displayedScore = m_score;
                m_scoreText->SetText(
                    "スコア "
                    + std::to_string(m_score));
            }
            // 小数秒を切り上げた残り秒数
            const int seconds = std::max(
                static_cast<int>(
                    std::ceil(m_timeRemaining)),
                0);
            // 残り秒が変わった場合だけ文字を更新します。
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

        // 案内文字列を設定します(message: 表示する文字列)。
        void SetMessage(const std::string& message)
        {
            // HUDラベル生成後だけ文字を反映します。
            if (m_messageText != nullptr)
            {
                m_messageText->SetText(message);
            }
        }

        // Pause状態と時間倍率を切り替えます。
        void TogglePause()
        {
            m_paused = !m_paused;
            LamaPon::Time::SetTimeScale(
                m_paused ? 0.0f : 1.0f);
            // Panel生成後だけ表示状態を反映します。
            if (m_pausePanel != nullptr)
            {
                m_pausePanel->SetEnabled(m_paused);
            }
        }

        // Pauseメニューの入力変更を反映します。
        void UpdatePauseMenu()
        {
            // スライダー変更時に効果音Bus音量を設定します。
            if (m_volumeSlider != nullptr
                && m_volumeSlider->ConsumeValueChanged())
            {
                // ミキサーの効果音バスをまとめて調整します。
                Graphics().Audio().SetBusVolume(
                    LamaPon::AudioBus::Effects,
                    m_volumeSlider->Value());
            }
            // Toggle変更時に照明の影設定を反映します。
            if (m_shadowToggle != nullptr
                && m_shadowToggle->ConsumeValueChanged()
                && m_spotLight != nullptr)
            {
                m_spotLight->SetCastsShadows(
                    m_shadowToggle->IsOn());
            }
            // 再開ButtonのクリックでPauseを解除します。
            if (m_resumeButton != nullptr
                && m_resumeButton->ConsumeClick())
            {
                TogglePause();
            }
        }

        // スコア・時間・的・ボールを初期化して再開します。
        void RestartRound()
        {
            m_score = 0;
            m_displayedScore = -1;
            m_displayedSeconds = -1;
            m_timeRemaining = RoundDuration;
            m_roundActive = true;
            SetMessage(
                "的をクリックで撃て！ Pで一時停止");
            // 各的を再配置して出現率を戻します(index: 的一覧の番号)。
            for (std::size_t index = 0;
                index < m_targets.size();
                ++index)
            {
                PlaceTarget(*m_targets[index]);
                m_targetPop[index] = 0.0f;
            }
            ResetBall();
        }

        // 射撃対象のGameObject一覧
        std::vector<LamaPon::GameObject*> m_targets;
        // 対応する的の出現率
        std::vector<float> m_targetPop;
        // スロープ上で転がるボール
        LamaPon::GameObject* m_ball{};
        // PauseメニューのPanel
        LamaPon::GameObject* m_pausePanel{};
        // ポインタ位置に置く照準Rect
        LamaPon::UIRectTransformComponent*
            m_crosshairTransform{};
        // スコア表示ラベル
        LamaPon::TextRendererComponent* m_scoreText{};
        // 残り時間表示ラベル
        LamaPon::TextRendererComponent* m_timeText{};
        // 案内表示ラベル
        LamaPon::TextRendererComponent* m_messageText{};
        // 効果音Bus音量スライダー
        LamaPon::UISliderComponent* m_volumeSlider{};
        // 影設定Toggle
        LamaPon::UIToggleComponent* m_shadowToggle{};
        // Pause解除Button
        LamaPon::UIButtonComponent* m_resumeButton{};
        // 的を照らすスポットライト
        LamaPon::SpotLightComponent* m_spotLight{};
        // 射撃時の効果音Source
        LamaPon::AudioSourceComponent* m_shotAudio{};
        // ラウンド中の命中スコア
        int m_score{};
        // HUDへ最後に反映したスコア
        int m_displayedScore{ -1 };
        // HUDへ最後に反映した残り秒
        int m_displayedSeconds{ -1 };
        // 現在のラウンド残り秒
        float m_timeRemaining{ RoundDuration };
        // ボール再配置までの経過秒
        float m_ballRespawnTimer{};
        // Pauseメニューが開いているか
        bool m_paused{};
        // Pキーが前回Updateで押されていたか
        bool m_pauseKeyHeld{};
        // 射撃ラウンドが進行中か
        bool m_roundActive{ true };
        // 命中メッセージの予約識別子
        std::uint64_t m_messageResetHandle{};
        // 配置用乱数の状態
        std::uint32_t m_randomState{ 0x2f6e2b1u };
    };
}

LAMAPON_SCRIPT_NAMED(
    TargetRangeGame,
    "TargetRange.Game",
    "Target Range Game")
