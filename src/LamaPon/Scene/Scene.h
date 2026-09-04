#pragma once

#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
// VolumetricLightFrameを値で持つため（ポスト処理へ受け渡す情報）。
#include "LamaPon/Graphics/RenderPipeline.h"
// ReflectionProbeEnvironmentを値で返すため。
#include "LamaPon/Graphics/ReflectionProbeEnvironment.h"
#include "LamaPon/Physics/PhysicsQuery.h"
#include "LamaPon/Physics/PhysicsFrameClock.h"
#include "LamaPon/Scene/EventBus.h"
#include "LamaPon/Scene/RenderSpatialIndex.h"

#include <DirectXMath.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace LamaPon
{
    class CameraComponent;
    class Component;
    class DataAsset;
    class GameObject;
    class GraphicsDevice;
    class ReflectionProbeComponent;
    class RenderTarget;
    class SceneManager;
    using GameObjectId = std::uint64_t;
    // GameObject.hと同一の別名宣言です（追加シーンの識別番号）。
    using SceneHandle = std::uint32_t;

    struct PhysicsBroadPhaseStats final
    {
        std::size_t colliderCount2D{};
        std::size_t colliderCount3D{};
        std::size_t candidatePairCount2D{};
        std::size_t candidatePairCount3D{};
        std::size_t narrowPhaseTestCount2D{};
        std::size_t narrowPhaseTestCount3D{};
        std::size_t occupiedCellCount2D{};
        std::size_t occupiedCellCount3D{};
        std::size_t oversizedColliderCount2D{};
        std::size_t oversizedColliderCount3D{};
        std::size_t activeContactCount{};
    };

    struct RenderVisibilityStats final
    {
        std::size_t rendererCount{};
        std::size_t visibleRendererCount{};
        std::size_t lodGroupCount{};
        std::size_t lodCulledCount{};
        std::size_t frustumCulledCount{};
        std::size_t occlusionCulledCount{};
        std::size_t automaticLodRendererCount{};
        std::uint64_t automaticLodTrianglesSaved{};
        std::size_t meshInstanceBatchCount{};
        std::size_t meshInstancedRendererCount{};
        std::size_t modelInstanceBatchCount{};
        std::size_t modelInstancedRendererCount{};
        std::size_t spatialNodeCount{};
        std::size_t spatialNodeTestCount{};
        bool spatialIndexReused{};
    };

    // 追加読み込み（Additive）で足したシーン1つ分の情報です。
    struct LoadedSceneInfo final
    {
        SceneHandle handle{};
        std::filesystem::path path;
        std::string name;
        std::size_t rootCount{};
    };

    struct PrefabOverride final
    {
        std::string path;
        std::string sourceValue;
        std::string instanceValue;
        bool sourceExists{};
        bool instanceExists{};
        bool canApplyIndividually{};
    };

    class Scene final
    {
    public:
        explicit Scene(GraphicsDevice& graphics) noexcept;
        ~Scene();

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        GameObject& CreateGameObject(std::string name);
        // エディターの一時プレビュー用。主シーンへ保存せず、
        // ヒエラルキーにも表示しない特別な由来を付けます。
        // 実行中のゲームシーンへ混ざらないよう、利用側で無効化を
        // 維持し、プレビュー描画時だけ有効にします。
        GameObject& CreateEditorPreviewGameObject(std::string name);
        GameObject& DuplicateGameObject(
            const GameObject& source,
            GameObject* targetParent = nullptr,
            bool appendCopySuffix = true);
        bool DestroyGameObject(GameObject& gameObject);
        // ヒエラルキーの並びを変えます。movedをreferenceの直前
        // （insertAfter=falseのとき）または直後へ移します。
        //
        // 並びは見た目だけの話ではありません。ライトの収集は
        // この順で先着N灯を採るので（BuildLightingState）、どの
        // ライトが有効になるかがここで決まります。保存順もこの順です。
        //
        // 親子関係は変えません。子を持つオブジェクトは、その子も
        // 追従して同じ位置へ移動します（走査順で親が子より先に
        // 来る前提を崩さないため）。
        // 並べ替えできなかった場合はfalseを返します。
        bool ReorderGameObject(
            GameObject& moved,
            const GameObject& reference,
            bool insertAfter);
        bool RemoveComponent(GameObject& gameObject, Component& component);
        void DontDestroyOnLoad(
            GameObject& gameObject,
            std::string persistenceKey = {});
        void DestroyOnLoad(GameObject& gameObject);
        [[nodiscard]] GameObject* FindGameObject(GameObjectId id) const noexcept;
        // シーン全体の線形走査です。大きなシーンでは毎フレーム
        // 呼ばず、結果をキャッシュしてください。
        [[nodiscard]] GameObject* FindGameObjectByName(
            std::string_view name) const noexcept;
        [[nodiscard]] std::vector<GameObject*>
            FindGameObjectsByName(std::string_view name) const;
        [[nodiscard]] GameObject* FindGameObjectByTag(
            std::string_view tag) const noexcept;
        [[nodiscard]] std::vector<GameObject*>
            FindGameObjectsByTag(std::string_view tag) const;
        // プロジェクト設定で登録されたタグ一覧。Scene切り替えや
        // Clear()では消えません。空なら未登録タグの検査は無効です。
        void SetRegisteredTags(std::vector<std::string> tags)
        {
            m_registeredTags = std::move(tags);
        }
        [[nodiscard]] const std::vector<std::string>&
            RegisteredTags() const noexcept
        {
            return m_registeredTags;
        }
        [[nodiscard]] bool IsTagRegistered(
            std::string_view tag) const noexcept;
        // 登録タグ一覧が非空のとき、未登録タグの使用へ警告を
        // 出します（Scene/Prefab読み込み時に呼ばれます）。
        void WarnUnregisteredTag(
            const GameObject& gameObject) const;
        // 名前付きイベントバス（Script::On/Emitの実体）。
        // Scene切り替えやClear()では消えません（購読は
        // Scriptの破棄時に自動解除されます）。
        [[nodiscard]] EventBus& Events() noexcept
        {
            return m_events;
        }
        template<typename T>
        [[nodiscard]] T* FindComponentOfType(
            const bool includeInactive = false) const noexcept
        {
            for (const auto& gameObject : m_gameObjects)
            {
                if (!includeInactive
                    && !gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                if (auto* component =
                    gameObject->template GetComponent<T>();
                    component != nullptr
                    && (includeInactive
                        || component->IsEnabled()))
                {
                    return component;
                }
            }
            return nullptr;
        }
        template<typename T>
        [[nodiscard]] std::vector<T*> FindComponentsOfType(
            const bool includeInactive = false) const
        {
            std::vector<T*> results;
            for (const auto& gameObject : m_gameObjects)
            {
                if (!includeInactive
                    && !gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                if (auto* component =
                    gameObject->template GetComponent<T>();
                    component != nullptr
                    && (includeInactive
                        || component->IsEnabled()))
                {
                    results.push_back(component);
                }
            }
            return results;
        }
        void SetMainCamera(CameraComponent& camera) noexcept { m_mainCamera = &camera; }
        void ClearMainCamera() noexcept { m_mainCamera = nullptr; }

        [[nodiscard]] CameraComponent* MainCamera() const noexcept { return m_mainCamera; }
        void SetAmbientLightColor(
            const DirectX::XMFLOAT3& color) noexcept;
        [[nodiscard]] const DirectX::XMFLOAT3&
            AmbientLightColor() const noexcept
        {
            return m_ambientLightColor;
        }
        void SetAmbientLightIntensity(float intensity) noexcept;
        [[nodiscard]] float AmbientLightIntensity() const noexcept
        {
            return m_ambientLightIntensity;
        }
        void SetSkySettings(const SkySettings& settings) noexcept;
        [[nodiscard]] const SkySettings& Sky() const noexcept
        {
            return m_sky;
        }
        // 朝昼夜モードのとき、最初のDirectional Lightの向きから
        // 空の3色を差し替えた設定を返します。切っていればSky()と
        // 同じものが返ります。描画と、Inspectorの見た目確認の
        // 両方から同じ答えを得るために1箇所へ集約しています。
        [[nodiscard]] SkySettings ResolvedSky() const noexcept;
        // 朝昼夜モードで使う太陽（向き・色・角半径）。方向光が
        // 1つも無いときはfalseを返します。
        [[nodiscard]] bool ResolveSkySun(
            DirectX::XMFLOAT3& directionToSun,
            DirectX::XMFLOAT3& color,
            float& angularRadius) const noexcept;
        void SetFogSettings(const FogSettings& settings) noexcept;
        [[nodiscard]] const FogSettings& Fog() const noexcept
        {
            return m_fog;
        }
        // SSAO（遮蔽による陰り）。品質設定側でも有効である必要が
        // あります（Bloomと同じ扱い）。
        void SetAmbientOcclusionSettings(
            const AmbientOcclusionSettings& settings) noexcept;
        [[nodiscard]] const AmbientOcclusionSettings&
            AmbientOcclusion() const noexcept
        {
            return m_ambientOcclusion;
        }
        // ボリュメトリックライト（光の筋）。影付きの平行光源が
        // 必要です（遮るものが分からないと筋が出ないため）。
        void SetTemporalAntiAliasingSettings(
            const TemporalAntiAliasingSettings& settings)
            noexcept;
        [[nodiscard]] const TemporalAntiAliasingSettings&
            TemporalAntiAliasing() const noexcept
        {
            return m_temporalAntiAliasing;
        }
        [[nodiscard]] const TemporalAntiAliasingFrame&
            TemporalFrameData() const noexcept
        {
            return m_temporalFrame;
        }
        void SetScreenSpaceReflectionSettings(
            const ScreenSpaceReflectionSettings& settings)
            noexcept;
        [[nodiscard]] const ScreenSpaceReflectionSettings&
            ScreenSpaceReflection() const noexcept
        {
            return m_screenSpaceReflection;
        }
        // ベイクした間接光（照度ボリューム）。設定を変えても
        // 焼き込み済みデータはそのまま残ります（配置や格子数を
        // 変えたときは、もう一度ベイクしてください）。
        void SetBakedGlobalIlluminationSettings(
            const BakedGlobalIlluminationSettings& settings)
            noexcept;
        [[nodiscard]] const BakedGlobalIlluminationSettings&
            BakedGlobalIllumination() const noexcept
        {
            return m_bakedGiSettings;
        }
        // ベイクを開始します（毎フレーム数点ずつ進みます）。
        void RequestBakedGlobalIlluminationBake() noexcept;
        // 進捗（0〜1）。ベイク中でなければ負の値。
        [[nodiscard]] float
            BakedGlobalIlluminationBakeProgress()
            const noexcept;
        // 焼き込み済みデータを持っているか（表示が効く状態か）。
        [[nodiscard]] bool HasBakedGlobalIllumination()
            const noexcept
        {
            return !m_bakedGiData.empty();
        }
        // ここから3つは直列化専用の入口です（fp16の生値と、
        // それを焼いたときの形）。ゲームコードからは使いません。
        [[nodiscard]] const std::vector<std::uint16_t>&
            BakedGlobalIlluminationPayload() const noexcept
        {
            return m_bakedGiData;
        }
        [[nodiscard]] const BakedGlobalIlluminationSettings&
            BakedGlobalIlluminationBakedShape()
            const noexcept
        {
            return m_bakedGiBakedShape;
        }
        void RestoreBakedGlobalIllumination(
            const BakedGlobalIlluminationSettings& shape,
            std::vector<std::uint16_t> payload) noexcept;
        void SetVolumetricLightSettings(
            const VolumetricLightSettings& settings) noexcept;
        [[nodiscard]] const VolumetricLightSettings&
            VolumetricLight() const noexcept
        {
            return m_volumetricLight;
        }
        // 直前の3D描画が用意した、ポスト処理へ渡す情報。
        // RenderWithMatricesが毎フレーム書き込みます。
        [[nodiscard]] const VolumetricLightFrame&
            VolumetricLightFrameData() const noexcept
        {
            return m_volumetricFrame;
        }
        void SetBloomSettings(const BloomSettings& settings) noexcept;
        [[nodiscard]] const BloomSettings& Bloom() const noexcept
        {
            return m_bloom;
        }
        void SetScreenOutlineSettings(
            const ScreenOutlineSettings& settings) noexcept;
        [[nodiscard]] const ScreenOutlineSettings&
            ScreenOutline() const noexcept
        {
            return m_screenOutline;
        }
        void SetScreenSpaceLensFlareSettings(
            const ScreenSpaceLensFlareSettings& settings) noexcept;
        [[nodiscard]] const ScreenSpaceLensFlareSettings&
            ScreenSpaceLensFlare() const noexcept
        {
            return m_screenSpaceLensFlare;
        }
        // 被写界深度（DoF）。品質設定側でも有効である必要があります
        // （Bloomと同じ扱い）。
        void SetDepthOfFieldSettings(
            const DepthOfFieldSettings& settings) noexcept;
        [[nodiscard]] const DepthOfFieldSettings&
            DepthOfField() const noexcept
        {
            return m_depthOfField;
        }
        // モーションブラー（カメラの動きによるブレ）。品質設定側でも
        // 有効である必要があります（Bloomと同じ扱い）。
        void SetMotionBlurSettings(
            const MotionBlurSettings& settings) noexcept;
        [[nodiscard]] const MotionBlurSettings&
            MotionBlur() const noexcept
        {
            return m_motionBlur;
        }
        // 自動露出（明順応・暗順応）。品質設定側でも有効である必要が
        // あります。トーンマッピングを切っていると効きません（露出は
        // トーンマップのパスの中で掛かるためです）。
        void SetAutoExposureSettings(
            const AutoExposureSettings& settings) noexcept;
        [[nodiscard]] const AutoExposureSettings&
            AutoExposure() const noexcept
        {
            return m_autoExposure;
        }
        // ポスト処理へ渡す一式。設定は今の値をその場で読み、行列は
        // 直前の3D描画（RenderWithMatrices）が控えたものを使います。
        // パスを足しても呼び出し側を直さずに済むよう、1つにまとめて
        // 返します。
        [[nodiscard]] PostProcessFrame
            PostProcessFrameData() const;
        void SetColorGradingSettings(
            const ColorGradingSettings& settings) noexcept;
        [[nodiscard]] const ColorGradingSettings&
            ColorGrading() const noexcept
        {
            return m_colorGrading;
        }
        void SetPhysicsBroadPhaseCellSize(float size) noexcept;
        [[nodiscard]] float PhysicsBroadPhaseCellSize() const noexcept
        {
            return m_physicsBroadPhaseCellSize;
        }
        [[nodiscard]] const PhysicsBroadPhaseStats&
            PhysicsStats() const noexcept
        {
            return m_physicsStats;
        }
        // 互換用の既定値です。現在の設定値はPhysicsTiming().fixedDeltaTime。
        [[nodiscard]] static constexpr float
            FixedPhysicsDeltaTime() noexcept
        {
            return 1.0f / 60.0f;
        }
        [[nodiscard]] float
            PhysicsInterpolationAlpha() const noexcept
        {
            return PhysicsTiming().interpolationAlpha;
        }
        [[nodiscard]] std::size_t
            PhysicsFixedStepsLastFrame() const noexcept
        {
            return PhysicsTiming().fixedSteps;
        }
        // LateUpdateで確定する物理・描画時刻。Updateからは前フレームを参照します。
        // ゴーストや物理追従の表示にはInterpolateTimeを使い、時刻の二重積算を避けます。
        [[nodiscard]] const PhysicsFrameTiming& PhysicsTiming() const noexcept
        {
            return m_physicsClock.Timing();
        }
        [[nodiscard]] bool
            RenderingInterpolatedTransforms()
                const noexcept
        {
            return m_renderingInterpolatedTransforms;
        }
        void SetFrustumCullingEnabled(bool value) noexcept
        {
            m_frustumCullingEnabled = value;
        }
        [[nodiscard]] bool FrustumCullingEnabled() const noexcept
        {
            return m_frustumCullingEnabled;
        }
        void SetOcclusionCullingEnabled(bool value) noexcept
        {
            m_occlusionCullingEnabled = value;
        }
        [[nodiscard]] bool OcclusionCullingEnabled() const noexcept
        {
            return m_occlusionCullingEnabled;
        }
        [[nodiscard]] const RenderVisibilityStats&
            VisibilityStats() const noexcept
        {
            return m_visibilityStats;
        }
        [[nodiscard]] RenderVisibilityStats
            EvaluateRenderVisibility(
                DirectX::FXMMATRIX view,
                DirectX::CXMMATRIX projection);

        [[nodiscard]] const std::vector<std::unique_ptr<GameObject>>& GameObjects() const noexcept
        {
            return m_gameObjects;
        }
        [[nodiscard]] SceneManager&
            Scenes() noexcept
        {
            return *m_sceneManager;
        }
        [[nodiscard]] const SceneManager&
            Scenes() const noexcept
        {
            return *m_sceneManager;
        }

        void Clear() noexcept;
        void SaveToFile(const std::filesystem::path& path) const;
        void LoadFromFile(const std::filesystem::path& path);
        [[nodiscard]] std::string SerializeToJson() const;
        void LoadFromJson(std::string_view json);
        // 追加読み込み（Additive）。今あるGameObjectを消さずに、
        // 別シーンのGameObjectを足します。返り値はそのシーンの
        // ハンドルで、UnloadSceneに渡すと足した分だけ消せます。
        // 環境設定（空・霧・Bloomなど）とMain Cameraは主シーンの
        // ものを優先し、主シーンにMain Cameraが無いときだけ
        // 追加シーンのカメラを採用します。
        SceneHandle MergeFromFile(
            const std::filesystem::path& path);
        SceneHandle MergeFromJson(
            std::string_view json,
            std::filesystem::path sourcePath = {});
        // 追加シーンのGameObjectをまとめて破棄します。主シーン
        // （PrimarySceneHandle）は破棄できません。
        bool UnloadScene(SceneHandle handle);
        bool UnloadScene(
            const std::filesystem::path& path);
        void UnloadAllAdditiveScenes();
        [[nodiscard]] const std::vector<LoadedSceneInfo>&
            AdditiveScenes() const noexcept
        {
            return m_additiveScenes;
        }
        [[nodiscard]] static constexpr SceneHandle
            PrimarySceneHandle() noexcept
        {
            return 0;
        }
        // 読み込み済みなら1以上のハンドル、未読み込みなら0を
        // 返します（主シーン自身の判定には使えません）。
        [[nodiscard]] SceneHandle FindAdditiveScene(
            const std::filesystem::path& path)
                const noexcept;
        [[nodiscard]] const LoadedSceneInfo*
            FindAdditiveScene(
                SceneHandle handle) const noexcept;
        // データアセット（`*.asset.json`）を読み込みます。読めない
        // ときも空のDataAssetを返すため、戻り値は常に有効です
        // （初心者がnull判定を書かなくても落ちないようにするため）。
        [[nodiscard]] std::shared_ptr<const DataAsset>
            LoadDataAsset(
                const std::filesystem::path& path) const;
        [[nodiscard]] std::string SerializePrefabToJson(
            const GameObject& root) const;
        void SavePrefab(
            const GameObject& root,
            const std::filesystem::path& path) const;
        GameObject& InstantiatePrefab(
            const std::filesystem::path& path,
            GameObject* parent = nullptr);
        GameObject& InstantiatePrefabFromJson(
            std::string_view json,
            GameObject* parent = nullptr,
            std::filesystem::path prefabAssetPath = {});
        [[nodiscard]] GameObject* FindPrefabInstanceRoot(
            GameObject& gameObject) const noexcept;
        [[nodiscard]] const GameObject* FindPrefabInstanceRoot(
            const GameObject& gameObject) const noexcept;
        [[nodiscard]] bool HasPrefabOverrides(
            const GameObject& instanceRoot) const;
        [[nodiscard]] std::vector<PrefabOverride>
            GetPrefabOverrides(
                const GameObject& instanceRoot) const;
        void ApplyPrefabOverride(
            const GameObject& instanceRoot,
            std::string_view path) const;
        GameObject& RevertPrefabOverride(
            GameObject& instanceRoot,
            std::string_view path);
        void ApplyPrefabInstance(
            const GameObject& instanceRoot) const;
        GameObject& RevertPrefabInstance(
            GameObject& instanceRoot);

        void Update(float deltaTime);
        [[nodiscard]] bool Raycast(
            const Ray& ray,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        [[nodiscard]] std::vector<PhysicsHit>
            RaycastAll(
                const Ray& ray,
                float maximumDistance,
                const PhysicsQueryFilter& filter = {}) const;
        [[nodiscard]] bool SphereCast(
            const Ray& ray,
            float radius,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        // AABBを半エクステント分膨らませる近似スイープです。
        [[nodiscard]] bool BoxCast(
            const Ray& ray,
            const DirectX::XMFLOAT3& halfExtents,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        // Y軸カプセル（radius/height）の近似スイープです。
        [[nodiscard]] bool CapsuleCast(
            const Ray& ray,
            float radius,
            float height,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        [[nodiscard]] std::vector<PhysicsOverlapHit>
            OverlapBox(
                const Bounds3D& bounds,
                const PhysicsQueryFilter& filter = {}) const;
        [[nodiscard]] std::vector<PhysicsOverlapHit>
            OverlapSphere(
                const DirectX::XMFLOAT3& center,
                float radius,
                const PhysicsQueryFilter& filter = {}) const;
        // 線分start-endと半径で定義されるカプセルの重なり判定
        // （AABB＋線分距離の近似）です。
        [[nodiscard]] std::vector<PhysicsOverlapHit>
            OverlapCapsule(
                const DirectX::XMFLOAT3& start,
                const DirectX::XMFLOAT3& end,
                float radius,
                const PhysicsQueryFilter& filter = {}) const;
        void Render();
        // 描画先テクスチャを持つCameraを、それぞれのレンダー
        // テクスチャへ描きます。Render()の先頭で呼ばれるので、
        // ゲーム側から明示的に呼ぶ必要はありません（エディターは
        // ビューポート描画の前に呼びます）。
        // 2D／UIは描きません（そのテクスチャを表示している
        // SpriteRendererが自分自身を写し込むのを避けるため）。
        void RenderTargetTextures();
        // positionを範囲に含む、ベイク済みで一番近いリフレクション
        // プローブを返します（無ければnullptr）。描画側が
        // オブジェクト単位のIBL差し替えに使います。
        [[nodiscard]] ReflectionProbeComponent*
            NearestReflectionProbe(
                const DirectX::XMFLOAT3& position)
                const noexcept;
        // positionへ適用するプローブを最大2個選び、混ぜる比率まで
        // 含めて返します。描画側（MeshRenderer／ModelRenderer）は
        // これをそのままLitEffectへ渡してください。
        //
        // 影響度が一番大きいものを主、2番目を相手にします。どちらの
        // ブレンド距離も0なら相手を選ばないので、プローブ1個の
        // ときと同じ絵になります。
        [[nodiscard]] ReflectionProbeEnvironment
            ReflectionProbeEnvironmentAt(
                const DirectX::XMFLOAT3& position)
                const noexcept;
        // targetは描画先のレンダーターゲットです。渡すと深度プリパスと
        // SSAOをライティングより前に解決し、SSRにも深度を渡します。
        // どちらも無効ならプリパスを省略します。nullptrでも描画自体は
        // 成立しますが、この経路でSSAOやSSRの深度は生成しません。
        void RenderMainCamera(
            float aspectRatio,
            bool include2D = true,
            RenderTarget* target = nullptr);
        void Render2D();
        // projectionはずらしを含まない素の射影です。TAAが有効な
        // ときは、この中でサブピクセルのずらしを織り込みます
        // （深度プリパスと本描画で必ず同じ行列にするため）。
        void RenderWithMatrices(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            bool include2D,
            bool renderDebug = false,
            RenderTarget* target = nullptr);

    private:
        friend class SceneManager;

        using CollisionKey = std::tuple<GameObjectId, GameObjectId, bool>;

        struct PersistentTransfer final
        {
            std::vector<std::unique_ptr<GameObject>> objects;
            CameraComponent* mainCamera{};
        };
        struct VisibilityResult final
        {
            std::unordered_set<GameObjectId>
                lodHidden;
            std::unordered_set<GameObjectId>
                renderHidden;
            RenderVisibilityStats stats;
        };
        void StepPhysics(float deltaTime);
        void SolveJoints(
            float deltaTime,
            bool applyForces);
        [[nodiscard]] std::size_t
            PhysicsSubstepCount(float deltaTime) const noexcept;
        [[nodiscard]] bool CollisionSuppressedByJoint(
            const GameObject& left,
            const GameObject& right) const noexcept;
        [[nodiscard]] VisibilityResult
            BuildRenderVisibility(
                DirectX::FXMMATRIX view,
                DirectX::CXMMATRIX projection) const;
        [[nodiscard]] std::unordered_set<GameObjectId>
            BuildShadowFrustumHidden(
                DirectX::FXMMATRIX view,
                DirectX::CXMMATRIX projection,
                const std::unordered_set<GameObjectId>&
                    alreadyHidden) const;
        [[nodiscard]] bool
            RefreshRenderSpatialIndex() const;
        [[nodiscard]] LightingState BuildLightingState() const noexcept;
        [[nodiscard]] PersistentTransfer
            ExtractPersistentObjects();
        void MergePersistentObjects(
            PersistentTransfer transfer);
        // LoadFromJsonとMergeFromJsonの共通処理です。additiveが
        // trueならClear()せず、読み込んだGameObjectへ新しい
        // ハンドルを割り当てます。
        SceneHandle ApplySceneJson(
            std::string_view json,
            bool additive,
            std::filesystem::path sourcePath);

        GraphicsDevice& m_graphics;
        std::unique_ptr<SceneManager>
            m_sceneManager;
        std::vector<std::unique_ptr<GameObject>> m_gameObjects;
        // Clear()では消さない（プロジェクト単位の設定のため）。
        std::vector<std::string> m_registeredTags;
        // Clear()では消さない（永続Scriptの購読を保つため。
        // 破棄されたScriptの購読はデストラクタで解除されます）。
        EventBus m_events;
        CameraComponent* m_mainCamera{};
        GameObjectId m_nextId{ 1 };
        // 追加読み込み中のシーンのハンドル。0以外の間に作られた
        // GameObjectは、そのシーンの所属になります。
        SceneHandle m_loadingScene{};
        SceneHandle m_nextSceneHandle{ 1 };
        std::vector<LoadedSceneInfo> m_additiveScenes;
        std::map<CollisionKey, bool> m_activeCollisions;
        DirectX::XMFLOAT3 m_ambientLightColor{
            0.65f,
            0.72f,
            0.85f
        };
        float m_ambientLightIntensity{ 0.35f };
        SkySettings m_sky;
        FogSettings m_fog;
        BloomSettings m_bloom;
        ScreenOutlineSettings m_screenOutline;
        ScreenSpaceLensFlareSettings m_screenSpaceLensFlare;
        AmbientOcclusionSettings m_ambientOcclusion;
        ScreenSpaceReflectionSettings
            m_screenSpaceReflection;
        TemporalAntiAliasingSettings
            m_temporalAntiAliasing;
        TemporalAntiAliasingFrame m_temporalFrame;
        // ジッターを進めるフレーム番号（ビューごとではなく
        // シーン全体で1つ。どのビューも同じ列を使います）。
        std::uint32_t m_temporalFrameIndex{};
        VolumetricLightSettings m_volumetricLight;
        // 影の行列とライト情報は3D描画のときだけ揃うので、
        // そこで作ってポスト処理へ受け渡します。
        VolumetricLightFrame m_volumetricFrame;
        DepthOfFieldSettings m_depthOfField;
        // 射影行列はビューごとに違うので、3D描画のときに詰めて
        // ポスト処理へ受け渡します。設定側はPostProcessFrameData()が
        // その場の値で上書きするので、ここに入っているのは行列だけを
        // 使う前提です。
        DepthOfFieldFrame m_depthOfFieldFrame;
        PostProcessFrame::ScreenOutlineFrame
            m_screenOutlineFrame;
        MotionBlurSettings m_motionBlur;
        MotionBlurFrame m_motionBlurFrame;
        AutoExposureSettings m_autoExposure;
        ColorGradingSettings m_colorGrading;
        float m_physicsBroadPhaseCellSize{ 4.0f };
        PhysicsBroadPhaseStats m_physicsStats;
        Detail::PhysicsFrameClock m_physicsClock;
        bool m_renderingInterpolatedTransforms{};
        // レンダーテクスチャ描画中の再入を防ぎます。
        bool m_renderingTargetTextures{};
        bool m_frustumCullingEnabled{ true };
        bool m_occlusionCullingEnabled{ true };
        RenderVisibilityStats m_visibilityStats;
        // BVHの構築・検索・破棄は索引の所有者へ委譲します。
        mutable RenderSpatialIndex m_renderSpatialIndex;
        // クラスタライトカリングの初期化に失敗した（シェーダーが
        // 無い等）。従来経路で描き続け、毎フレーム再試行しません。
        bool m_clusteredLightingUnavailable{};
        // ベイク待ちのリフレクションプローブを焼きます
        // （フレームの描画が始まる前に呼びます）。
        void BakePendingReflectionProbes();
        // 今フレームの描画で使うプローブ一覧（毎フレーム集め直し）。
        std::vector<ReflectionProbeComponent*>
            m_frameReflectionProbes;
        // ベイク用の共有キューブマップ等（実体はScene.cpp）。
        struct ReflectionProbeBakeResources;
        std::unique_ptr<ReflectionProbeBakeResources>
            m_probeBakeResources;
        // ベイク中の再帰描画でプローブ自身が映り込まないように。
        bool m_bakingReflectionProbes{};
        // SkyboxのIBL畳み込みのディスクキャッシュ鍵。パスが変わった
        // ときだけ読み直して計算するための控えです（0は鍵なし）。
        std::filesystem::path m_skyPrefilterKeyPath;
        std::uint64_t m_skyPrefilterKey{};

        // ベイクした間接光（照度ボリューム）。
        void ProcessBakedGlobalIlluminationBake();
        void EnsureBakedGlobalIlluminationTextures();
        BakedGlobalIlluminationSettings m_bakedGiSettings;
        // 焼き込み済みSH係数（fp16、テクスチャ3枚ぶんを連結。
        // 並びは [R,G,B]×[z,y,x]×RGBA16F）。シーンJSONへは
        // これがそのままbase64で入ります。
        std::vector<std::uint16_t> m_bakedGiData;
        // m_bakedGiDataを焼いたときの格子と配置（設定を後から
        // 変えても、表示は焼いたときの形で続けるため）。
        BakedGlobalIlluminationSettings m_bakedGiBakedShape;
        std::array<
            Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView>,
            3> m_bakedGiViews;
        bool m_bakedGiTexturesDirty{};
        // ベイクの進行状態（毎フレーム数点ずつ進める）。
        bool m_bakedGiBaking{};
        std::size_t m_bakedGiNextProbe{};
        std::vector<float> m_bakedGiWorking;
    };
}
