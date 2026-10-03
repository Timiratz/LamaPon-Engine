#pragma once

#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsDeviceResourceLease.h"
#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/ReflectionProbeEnvironment.h"
#include "LamaPon/Physics/PhysicsQuery.h"
#include "LamaPon/Physics/PhysicsFrameClock.h"
#include "LamaPon/Scene/EventBus.h"
#include "LamaPon/Scene/RenderSpatialIndex.h"

#include <DirectXMath.h>

#include <array>
#include <memory>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
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
    // シーン番号は0が主シーン、1以上が追加シーンです。
    using SceneHandle = std::uint32_t;

    struct PhysicsBroadPhaseStats final
    {
        // 2Dコライダー数
        std::size_t colliderCount2D{};
        // 3Dコライダー数
        std::size_t colliderCount3D{};
        // 2D接触候補の組数
        std::size_t candidatePairCount2D{};
        // 3D接触候補の組数
        std::size_t candidatePairCount3D{};
        // 2Dの詳細判定を行った組数
        std::size_t narrowPhaseTestCount2D{};
        // 3Dの詳細判定を行った組数
        std::size_t narrowPhaseTestCount3D{};
        // 2D格子の使用セル数
        std::size_t occupiedCellCount2D{};
        // 3D格子の使用セル数
        std::size_t occupiedCellCount3D{};
        // 全組比較する2Dコライダー数
        std::size_t oversizedColliderCount2D{};
        // 全組比較する3Dコライダー数
        std::size_t oversizedColliderCount3D{};
        // 現在有効な接触の組数
        std::size_t activeContactCount{};
    };

    // 物理デバッガーの接触点はワールド座標で、法線はleftへ通知する向きです。
    struct PhysicsDebugContact final
    {
        // 法線を受け取る対象番号
        GameObjectId left{};
        // 接触相手の対象番号
        GameObjectId right{};
        // ワールド座標の接触点
        DirectX::XMFLOAT3 point{};
        // 第1対象側のワールド接触法線
        DirectX::XMFLOAT3 normal{};
        // 形状同士のめり込み量
        float penetration{};
        // 3D物理の接触である指定
        bool is3D{};
        // トリガー接触の指定
        bool isTrigger{};
    };

    struct RenderVisibilityStats final
    {
        // 判定する描画対象数
        std::size_t rendererCount{};
        // 判定後に表示する描画対象数
        std::size_t visibleRendererCount{};
        // 有効なLODグループ数
        std::size_t lodGroupCount{};
        // LODで非表示にした対象数
        std::size_t lodCulledCount{};
        // 視錐台外で非表示にした対象数
        std::size_t frustumCulledCount{};
        // 遮蔽で非表示にした対象数
        std::size_t occlusionCulledCount{};
        // 自動LODで簡略化する対象数
        std::size_t automaticLodRendererCount{};
        // 自動LODで省略した三角形数
        std::uint64_t automaticLodTrianglesSaved{};
        // メッシュの一括描画バッチ数
        std::size_t meshInstanceBatchCount{};
        // 一括描画したメッシュ対象数
        std::size_t meshInstancedRendererCount{};
        // モデルの一括描画バッチ数
        std::size_t modelInstanceBatchCount{};
        // 一括描画したモデル対象数
        std::size_t modelInstancedRendererCount{};
        // 描画候補の索引ノード数
        std::size_t spatialNodeCount{};
        // 境界を判定した索引ノード数
        std::size_t spatialNodeTestCount{};
        // 描画候補索引を再利用した状態
        bool spatialIndexReused{};
    };

    // 追加読み込み（Additive）で足したシーン1つ分の情報です。
    struct LoadedSceneInfo final
    {
        // 追加シーンの識別番号
        SceneHandle handle{};
        // 追加シーンの生成元パス
        std::filesystem::path path;
        // 追加シーンの表示名
        std::string name;
        // 追加したルートオブジェクト数
        std::size_t rootCount{};
    };

    struct PrefabOverride final
    {
        // 差分を指すJSONポインター
        std::string path;
        // 元アセットの値の表示文字列
        std::string sourceValue;
        // インスタンス値の表示文字列
        std::string instanceValue;
        // 元アセットに値がある指定
        bool sourceExists{};
        // インスタンスに値がある指定
        bool instanceExists{};
        // 差分を個別反映できる指定
        bool canApplyIndividually{};
    };

    class Scene final
    {
    public:
        // 描画資源の保持を確保してシーンを生成します(graphics: 借用する描画機器)。
        explicit Scene(GraphicsDevice& graphics);
        // シーンが所有する対象を破棄し、ワーカー終了後に描画資源の保持を解放します。
        ~Scene();

        // シーンと資源の所有権を維持するためコピーを禁止します。
        Scene(const Scene&) = delete;
        // 描画資源と物体所有権の重複を防ぐため代入を禁止します。
        Scene& operator=(const Scene&) = delete;

        // 論理画面の寸法変更を要求して成功したか返します(width: 1〜16384のピクセル幅, height: 1〜16384のピクセル高さ)。
        // 実行中はウィンドウ、エディター再生中はゲームビューを対象とし、変更処理が未登録ならfalseを返します。
        [[nodiscard]] bool SetWindowSize(
            std::uint32_t width, std::uint32_t height);
        // 登録した取得処理の論理寸法を返し、未登録なら描画機器の寸法を返します。
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
            WindowSize() const;
        // 論理画面の寸法変更と取得の処理を登録します(setter: 幅と高さを受け成否を返す処理, getter: 幅と高さを返す処理)。
        void SetWindowSizeCallbacks(
            std::function<bool(std::uint32_t, std::uint32_t)> setter,
            std::function<std::pair<std::uint32_t, std::uint32_t>()> getter);

        // 現在読み込み中のシーン所属として新しい対象を生成します(name: 表示名)。
        GameObject& CreateGameObject(std::string name);
        // 対応するコンポーネントと子孫を複製します(source: 複製元, targetParent: 所属する親かnullptr, appendCopySuffix: ルート名にCopyを付ける指定)。
        // 新しいコンポーネント型は複製分岐への追加が必要で、未対応型は複製対象外です。
        // 保持指定とプローブのベイク結果は対象外で、内部のジョイントとLOD参照は複製先へ変えます。
        GameObject& DuplicateGameObject(
            const GameObject& source,
            GameObject* targetParent = nullptr,
            bool appendCopySuffix = true);
        // 対象と子孫を破棄し所属外ならfalseを返します(gameObject: 削除する対象)。
        // メインカメラ・ジョイント・LODの対象参照を解除します。
        bool DestroyGameObject(GameObject& gameObject);
        // 親子関係を保ち対象と子孫の並びを移します(moved: 移動対象, reference: 基準対象, insertAfter: trueなら直後)。
        // 並びは保存と先着ライトの採用に影響し、所属外・同一対象・子孫基準ならfalseを返します。
        bool ReorderGameObject(
            GameObject& moved,
            const GameObject& reference,
            bool insertAfter);
        // 対象が所有するコンポーネントを削除し、メインカメラなら指定を解除します(gameObject: 所有者, component: 削除対象)。
        bool RemoveComponent(GameObject& gameObject, Component& component);
        // ルートと子孫を次のシーンでも保持する指定を付けます(gameObject: 所属するルート, persistenceKey: 保持対象のキー)。
        // 所属外・非ルート・重複キーはinvalid_argumentを送出し、空のキーは名前または番号で補います。
        void DontDestroyOnLoad(
            GameObject& gameObject,
            std::string persistenceKey = {});
        // 次のシーン切り替えでの保持指定を解除します(gameObject: このシーンに属する対象)。
        void DestroyOnLoad(GameObject& gameObject);
        // 識別番号が一致する対象を返し未発見ならnullptrを返します(id: オブジェクト番号)。
        [[nodiscard]] GameObject* FindGameObject(GameObjectId id) const noexcept;
        // 登録順に名前を完全一致で探し最初の対象かnullptrを返します(name: 検索する名前)。
        // 線形走査のため頻繁に使う場合は結果を保持して再利用します。
        [[nodiscard]] GameObject* FindGameObjectByName(
            std::string_view name) const noexcept;
        // 名前が完全一致する対象を登録順に収集します(name: 検索する名前)。
        [[nodiscard]] std::vector<GameObject*>
            FindGameObjectsByName(std::string_view name) const;
        // タグが完全一致する最初の対象かnullptrを返します(tag: 検索するタグ)。
        [[nodiscard]] GameObject* FindGameObjectByTag(
            std::string_view tag) const noexcept;
        // タグが完全一致する対象を登録順に収集します(tag: 検索するタグ)。
        [[nodiscard]] std::vector<GameObject*>
            FindGameObjectsByTag(std::string_view tag) const;
        // プロジェクトの登録タグを置き換えます(tags: 登録するタグ一覧)。
        // 空なら未登録タグの警告を無効にし、シーン切り替えやClearでも一覧を保持します。
        void SetRegisteredTags(std::vector<std::string> tags)
        {
            m_registeredTags = std::move(tags);
        }
        // プロジェクトの登録タグ一覧への読み取り参照を返します。
        [[nodiscard]] const std::vector<std::string>&
            RegisteredTags() const noexcept
        {
            return m_registeredTags;
        }
        // 登録タグに完全一致する名前があるか返します(tag: 調べるタグ)。
        [[nodiscard]] bool IsTagRegistered(
            std::string_view tag) const noexcept;
        // 一覧が非空の場合に未登録タグを警告します(gameObject: タグを調べる対象)。
        void WarnUnregisteredTag(
            const GameObject& gameObject) const;
        // Clearやシーン切り替えで保持する名前付きイベントバスへの参照を返します。
        [[nodiscard]] EventBus& Events() noexcept
        {
            return m_events;
        }
        // 登録順で最初のTを探します(includeInactive: 無効な階層とコンポーネントも含む指定)。
        template<typename T>
        [[nodiscard]] T* FindComponentOfType(
            const bool includeInactive = false) const noexcept
        {
            // 検索するシーンのオブジェクト
            for (const auto& gameObject : m_gameObjects)
            {
                if (!includeInactive
                    && !gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                // 型が一致したコンポーネント
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
        // 各オブジェクトの最初のTを登録順で収集します(includeInactive: 無効な階層とコンポーネントも含む指定)。
        template<typename T>
        [[nodiscard]] std::vector<T*> FindComponentsOfType(
            const bool includeInactive = false) const
        {
            // オブジェクトごとの検索結果
            std::vector<T*> results;
            // 検索するシーンのオブジェクト
            for (const auto& gameObject : m_gameObjects)
            {
                if (!includeInactive
                    && !gameObject->IsActiveInHierarchy())
                {
                    continue;
                }
                // 型が一致したコンポーネント
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
        // メインカメラを指定します(camera: シーンに属するカメラ)。
        void SetMainCamera(CameraComponent& camera) noexcept { m_mainCamera = &camera; }
        // メインカメラの指定を解除します。
        void ClearMainCamera() noexcept { m_mainCamera = nullptr; }

        // メインカメラの非所有ポインターを返し、未指定ならnullptrを返します。
        [[nodiscard]] CameraComponent* MainCamera() const noexcept { return m_mainCamera; }
        // 環境光のRGBを0〜1へ制限して設定します(color: 有限なRGB色)。
        void SetAmbientLightColor(
            const DirectX::XMFLOAT3& color) noexcept;
        // 環境光のRGBへの読み取り参照を返します。
        [[nodiscard]] const DirectX::XMFLOAT3&
            AmbientLightColor() const noexcept
        {
            return m_ambientLightColor;
        }
        // 環境光の強さを0〜4へ制限して設定します(intensity: 有限な強さ)。
        void SetAmbientLightIntensity(float intensity) noexcept;
        // 環境光の強さを返します。
        [[nodiscard]] float AmbientLightIntensity() const noexcept
        {
            return m_ambientLightIntensity;
        }
        // 空の設定を保存し色と強さを0〜8へ制限します(settings: 有限値の空設定)。
        void SetSkySettings(const SkySettings& settings) noexcept;
        // 保存した空の設定への読み取り参照を返します。
        [[nodiscard]] const SkySettings& Sky() const noexcept
        {
            return m_sky;
        }
        // 太陽連動時は最初の有効な方向光で空の三色を求め、それ以外は保存設定を返します。
        [[nodiscard]] SkySettings ResolvedSky() const noexcept;
        // 最初の有効な方向光から太陽情報を返します(directionToSun: 太陽への方向の出力, color: 強さを含むRGBの出力, angularRadius: 角半径のラジアン出力)。
        // 方向光がなければ出力を変更せずfalseを返します。
        [[nodiscard]] bool ResolveSkySun(
            DirectX::XMFLOAT3& directionToSun,
            DirectX::XMFLOAT3& color,
            float& angularRadius) const noexcept;
        // 霧の設定を保存し色・距離・密度を制限します(settings: 有限値の霧設定)。
        void SetFogSettings(const FogSettings& settings) noexcept;
        // 保存した霧の設定への読み取り参照を返します。
        [[nodiscard]] const FogSettings& Fog() const noexcept
        {
            return m_fog;
        }
        // 環境遮蔽の半径と強さを制限して設定します(settings: 有限値の環境遮蔽設定)。
        // 表示には品質設定側の環境遮蔽も有効にする必要があります。
        void SetAmbientOcclusionSettings(
            const AmbientOcclusionSettings& settings) noexcept;
        // 保存した環境遮蔽の設定への読み取り参照を返します。
        [[nodiscard]] const AmbientOcclusionSettings&
            AmbientOcclusion() const noexcept
        {
            return m_ambientOcclusion;
        }
        // 履歴係数・ジッター・色制限許容値を制限して設定します(settings: 有限値の時間AA設定)。
        void SetTemporalAntiAliasingSettings(
            const TemporalAntiAliasingSettings& settings)
            noexcept;
        // 保存した時間AAの設定への読み取り参照を返します。
        [[nodiscard]] const TemporalAntiAliasingSettings&
            TemporalAntiAliasing() const noexcept
        {
            return m_temporalAntiAliasing;
        }
        // 直前の3D描画で保存した時間AAのフレーム情報を返します。
        [[nodiscard]] const TemporalAntiAliasingFrame&
            TemporalFrameData() const noexcept
        {
            return m_temporalFrame;
        }
        // 画面反射の強さ・距離・探索回数などを制限して設定します(settings: 有限値の画面反射設定)。
        void SetScreenSpaceReflectionSettings(
            const ScreenSpaceReflectionSettings& settings)
            noexcept;
        // 保存した画面反射の設定への読み取り参照を返します。
        [[nodiscard]] const ScreenSpaceReflectionSettings&
            ScreenSpaceReflection() const noexcept
        {
            return m_screenSpaceReflection;
        }
        // 間接光の格子寸法・点数・強さを制限して設定します(settings: 有限値の間接光設定)。
        // 焼き込み済みデータの格子は次のベイクまで保持するため、配置や点数の変更後は再ベイクします。
        void SetBakedGlobalIlluminationSettings(
            const BakedGlobalIlluminationSettings& settings)
            noexcept;
        // 次回のベイクに使う間接光設定への読み取り参照を返します。
        [[nodiscard]] const BakedGlobalIlluminationSettings&
            BakedGlobalIllumination() const noexcept
        {
            return m_bakedGiSettings;
        }
        // 要求時の格子形状で間接光のベイクを開始または最初からやり直します。
        // 作業係数の確保に失敗した場合は、例外を伝播せず戻ります。
        void RequestBakedGlobalIlluminationBake() noexcept;
        // ベイク中は完了点数の比率0〜1を返し、それ以外は-1を返します。
        [[nodiscard]] float
            BakedGlobalIlluminationBakeProgress()
            const noexcept;
        // 保存された間接光係数が存在するかを返します。
        // 設定の有効状態やGPUビューの準備完了は判定しません。
        [[nodiscard]] bool HasBakedGlobalIllumination()
            const noexcept
        {
            return !m_bakedGiData.empty();
        }
        // シーン保存用のfp16係数列への読み取り参照を返します。
        // 並びは[R,G,B]×[z,y,x]×(x,y,z,定数項)です。
        [[nodiscard]] const std::vector<std::uint16_t>&
            BakedGlobalIlluminationPayload() const noexcept
        {
            return m_bakedGiData;
        }
        // シーン保存用に係数列をベイクした格子形状への参照を返します。
        [[nodiscard]] const BakedGlobalIlluminationSettings&
            BakedGlobalIlluminationBakedShape()
            const noexcept
        {
            return m_bakedGiBakedShape;
        }
        // 格子点数と係数列の長さを検証して保存済み間接光を復元します(shape: ベイク時の格子形状, payload: 所有権を移すfp16係数列)。
        // 不整合な入力は無視し、成功時は進行中のベイクを終了してGPU転送を予約します。
        void RestoreBakedGlobalIllumination(
            const BakedGlobalIlluminationSettings& shape,
            std::vector<std::uint16_t> payload) noexcept;
        // 光の筋の強さ・距離・散乱・標本数を制限して設定します(settings: 有限値のボリュメトリック光設定)。
        // 表示には影付きの平行光が必要です。
        void SetVolumetricLightSettings(
            const VolumetricLightSettings& settings) noexcept;
        // 保存したボリュメトリック光の設定への読み取り参照を返します。
        [[nodiscard]] const VolumetricLightSettings&
            VolumetricLight() const noexcept
        {
            return m_volumetricLight;
        }
        // 直前の3D描画で作ったポスト処理用の光と影の情報を返します。
        [[nodiscard]] const VolumetricLightFrame&
            VolumetricLightFrameData() const noexcept
        {
            return m_volumetricFrame;
        }
        // ブルームの閾値・強さ・半径を制限して設定します(settings: 有限値のブルーム設定)。
        void SetBloomSettings(const BloomSettings& settings) noexcept;
        // 保存したブルームの設定への読み取り参照を返します。
        [[nodiscard]] const BloomSettings& Bloom() const noexcept
        {
            return m_bloom;
        }
        // 輪郭線の色・強さ・幅・判定閾値を制限して設定します(settings: 有限値の画面輪郭設定)。
        void SetScreenOutlineSettings(
            const ScreenOutlineSettings& settings) noexcept;
        // 保存した画面輪郭線の設定への読み取り参照を返します。
        [[nodiscard]] const ScreenOutlineSettings&
            ScreenOutline() const noexcept
        {
            return m_screenOutline;
        }
        // 画面フレアの閾値・強さ・像配置などを制限して設定します(settings: 有限値の画面フレア設定)。
        void SetScreenSpaceLensFlareSettings(
            const ScreenSpaceLensFlareSettings& settings) noexcept;
        // 保存した画面フレアの設定への読み取り参照を返します。
        [[nodiscard]] const ScreenSpaceLensFlareSettings&
            ScreenSpaceLensFlare() const noexcept
        {
            return m_screenSpaceLensFlare;
        }
        // 焦点距離・範囲・強さ・画素半径を制限して設定します(settings: 有限値の被写界深度設定)。
        // 表示には品質設定側の被写界深度も有効にする必要があります。
        void SetDepthOfFieldSettings(
            const DepthOfFieldSettings& settings) noexcept;
        // 保存した被写界深度の設定への読み取り参照を返します。
        [[nodiscard]] const DepthOfFieldSettings&
            DepthOfField() const noexcept
        {
            return m_depthOfField;
        }
        // カメラ由来のブラーの強さと画素半径を制限して設定します(settings: 有限値のブラー設定)。
        // 表示には品質設定側のモーションブラーも有効にする必要があります。
        void SetMotionBlurSettings(
            const MotionBlurSettings& settings) noexcept;
        // 保存したモーションブラーの設定への読み取り参照を返します。
        [[nodiscard]] const MotionBlurSettings&
            MotionBlur() const noexcept
        {
            return m_motionBlur;
        }
        // 自動露出の輝度範囲・基準・順応速度を制限して設定します(settings: 有限値の自動露出設定)。
        // 表示には品質設定側の自動露出とトーンマッピングを有効にする必要があります。
        void SetAutoExposureSettings(
            const AutoExposureSettings& settings) noexcept;
        // 保存した自動露出の設定への読み取り参照を返します。
        [[nodiscard]] const AutoExposureSettings&
            AutoExposure() const noexcept
        {
            return m_autoExposure;
        }
        // 現在の設定と直前の3D描画の行列を合わせたポスト処理情報を返します。
        // 自動露出の順応にはtimeScaleに依存しない経過秒数を使います。
        [[nodiscard]] PostProcessFrame
            PostProcessFrameData() const;
        // 露出・コントラスト・彩度・色温度などを制限して設定します(settings: 有限値の色調補正設定)。
        void SetColorGradingSettings(
            const ColorGradingSettings& settings) noexcept;
        // 保存した色調補正の設定への読み取り参照を返します。
        [[nodiscard]] const ColorGradingSettings&
            ColorGrading() const noexcept
        {
            return m_colorGrading;
        }
        // 物理候補格子の幅を0.25〜100へ制限して設定します(size: 有限なセル幅)。
        void SetPhysicsBroadPhaseCellSize(float size) noexcept;
        // 物理候補格子の現在のセル幅を返します。
        [[nodiscard]] float PhysicsBroadPhaseCellSize() const noexcept
        {
            return m_physicsBroadPhaseCellSize;
        }
        // 直近の物理ステップの候補と接触の統計を返します。
        [[nodiscard]] const PhysicsBroadPhaseStats&
            PhysicsStats() const noexcept
        {
            return m_physicsStats;
        }
        // 物理接触点の記録を切り替え、無効化時は記録を消します(enabled: 記録するか)。
        void SetPhysicsDebugCaptureEnabled(bool enabled) noexcept;
        // 物理接触点の記録が有効かを返します。
        [[nodiscard]] bool IsPhysicsDebugCaptureEnabled() const noexcept
        {
            return m_physicsDebugCaptureEnabled;
        }
        // 最後の物理ステップの接触を返し、同じ接触組は最初の通知だけを記録します。
        [[nodiscard]] const std::vector<PhysicsDebugContact>&
            PhysicsDebugContacts() const noexcept
        {
            return m_physicsDebugContacts;
        }
        // 互換用の固定刻み1/60秒を返し、現在の設定はPhysicsTimingで取得します。
        [[nodiscard]] static constexpr float
            FixedPhysicsDeltaTime() noexcept
        {
            return 1.0f / 60.0f;
        }
        // 物理時計が求めた描画補間率を返します。
        [[nodiscard]] float
            PhysicsInterpolationAlpha() const noexcept
        {
            return PhysicsTiming().interpolationAlpha;
        }
        // 物理時計が記録したフレーム内の固定更新回数を返します。
        [[nodiscard]] std::size_t
            PhysicsFixedStepsLastFrame() const noexcept
        {
            return PhysicsTiming().fixedSteps;
        }
        // 物理時計が保持する物理と描画の時刻を返します。
        // 通常はLateUpdateで確定し、Update中の追従描画には補間時刻を使います。
        [[nodiscard]] const PhysicsFrameTiming& PhysicsTiming() const noexcept
        {
            return m_physicsClock.Timing();
        }
        // 現在の描画が物理変換の補間を使っているか返します。
        [[nodiscard]] bool
            RenderingInterpolatedTransforms()
                const noexcept
        {
            return m_renderingInterpolatedTransforms;
        }
        // 視錐台カリングの有効状態を設定します(value: 有効にする指定)。
        void SetFrustumCullingEnabled(bool value) noexcept
        {
            m_frustumCullingEnabled = value;
        }
        // 視錐台カリングが有効か返します。
        [[nodiscard]] bool FrustumCullingEnabled() const noexcept
        {
            return m_frustumCullingEnabled;
        }
        // 遮蔽カリングの有効状態を設定します(value: 有効にする指定)。
        void SetOcclusionCullingEnabled(bool value) noexcept
        {
            m_occlusionCullingEnabled = value;
        }
        // 遮蔽カリングが有効か返します。
        [[nodiscard]] bool OcclusionCullingEnabled() const noexcept
        {
            return m_occlusionCullingEnabled;
        }
        // 直近の可視判定と描画の統計への参照を返します。
        [[nodiscard]] const RenderVisibilityStats&
            VisibilityStats() const noexcept
        {
            return m_visibilityStats;
        }
        // 可視判定を実行して最新の統計を保存して返します(view: 可逆なビュー行列, projection: 投影行列)。
        [[nodiscard]] RenderVisibilityStats
            EvaluateRenderVisibility(
                DirectX::FXMMATRIX view,
                DirectX::CXMMATRIX projection);

        // シーンが所有するオブジェクト一覧への読み取り参照を返します。
        [[nodiscard]] const std::vector<std::unique_ptr<GameObject>>& GameObjects() const noexcept
        {
            return m_gameObjects;
        }
        // シーン読み込みと遷移の管理への参照を返します。
        [[nodiscard]] SceneManager&
            Scenes() noexcept
        {
            return *m_sceneManager;
        }
        // シーン読み込みと遷移の管理への読み取り参照を返します。
        [[nodiscard]] const SceneManager&
            Scenes() const noexcept
        {
            return *m_sceneManager;
        }

        // オブジェクト番号の再利用を識別するシーン内容の世代を返します。
        [[nodiscard]] std::uint64_t ContentRevision() const noexcept { return m_contentRevision; }
        // 全オブジェクトと追加シーンを破棄し環境・物理・描画状態を既定値へ戻します。
        // 登録タグ、イベントバス、シーン管理と接触記録の有効指定は保持します。
        void Clear() noexcept;
        // 主シーンの保存用JSONを上書き保存します(path: 保存先)。
        // 親フォルダーを作成し、保存先を先に切り詰めるため失敗時に旧内容を保持せず、失敗は例外を伝播します。
        void SaveToFile(const std::filesystem::path& path) const;
        // アセット経由で主シーンを読み、成功時に現在のシーンパスを更新します(path: 読み込み元)。
        // 主シーンの復元開始後に失敗した場合は、読み込み前の内容を保持しません。
        void LoadFromFile(const std::filesystem::path& path);
        // 主シーンの物体・設定・ベイク済み間接光を二スペース字下げのJSONへ変換します。
        // 追加シーンの物体と、それに属する主カメラや親参照は保存対象から除外します。
        [[nodiscard]] std::string SerializeToJson() const;
        // シーンJSONの旧形式を更新して主シーンを置き換えます(json: 入力JSON)。
        // 復元開始後の失敗は以前の内容へ戻さず、現在のシーンパスは変更しません。
        void LoadFromJson(std::string_view json);
        // アセット経由でシーンを追加読み込みし、破棄に使う番号を返します(path: 読み込み元)。
        // 主シーンの環境・物理・描画設定を維持し、物体番号を振り直し、主カメラがない場合だけ追加側を採用します。
        SceneHandle MergeFromFile(
            const std::filesystem::path& path);
        // 既存の物体を残してシーンJSONを追加読み込みし、破棄に使う番号を返します(json: 入力JSON, sourcePath: 追加シーンに記録する生成パス)。
        // 設定は主シーンを維持し、内部参照を新番号へ対応付け、対象外への参照は0にして、失敗時は追加分を破棄します。
        SceneHandle MergeFromJson(
            std::string_view json,
            std::filesystem::path sourcePath = {});
        // 追加シーンに属する対象を破棄します(handle: 追加シーン番号)。
        // 主シーンや未登録の番号はfalseを返し、対象の子孫もDestroyGameObjectで破棄します。
        bool UnloadScene(SceneHandle handle);
        // 生成元パスが一致する追加シーンを破棄し、未発見ならfalseを返します(path: 解決済みの生成元パス)。
        bool UnloadScene(
            const std::filesystem::path& path);
        // 登録された追加シーンをすべて破棄します。
        void UnloadAllAdditiveScenes();
        // 読み込み済みの追加シーン情報への読み取り参照を返します。
        [[nodiscard]] const std::vector<LoadedSceneInfo>&
            AdditiveScenes() const noexcept
        {
            return m_additiveScenes;
        }
        // 主シーンを表す番号0を返します。
        [[nodiscard]] static constexpr SceneHandle
            PrimarySceneHandle() noexcept
        {
            return 0;
        }
        // 正規化した生成元パスが一致する追加シーン番号を返します(path: 解決済みの生成元パス)。
        // 未発見や空のパスは0を返し、主シーン自身の検索には使いません。
        [[nodiscard]] SceneHandle FindAdditiveScene(
            const std::filesystem::path& path)
                const noexcept;
        // 追加シーン情報の非所有ポインターを返し、未発見ならnullptrを返します(handle: 追加シーン番号)。
        [[nodiscard]] const LoadedSceneInfo*
            FindAdditiveScene(
                SceneHandle handle) const noexcept;
        // データアセットを読み、空パスまたは読み込みのstd::exceptionでは共有の空データを返します(path: 読み込み元)。
        [[nodiscard]] std::shared_ptr<const DataAsset>
            LoadDataAsset(
                const std::filesystem::path& path) const;
        // シーンに属するルートと子孫をプリハブJSONへ変換します(root: 保存する階層のルート)。
        // 番号はローカルな連番へ変え、階層外へのジョイント・視差・LOD参照は0にし、保持指定とルート自身のプリハブ参照は含めません。
        [[nodiscard]] std::string SerializePrefabToJson(
            const GameObject& root) const;
        // 階層をプリハブJSONへ変換し、書き込み完了後に保存先を置換します(root: このシーンの保存対象ルート, path: 保存先)。
        // .tmpを付けた固定保存先を使うため同じパスへの同時保存を避け、失敗は例外を伝播します。
        void SavePrefab(
            const GameObject& root,
            const std::filesystem::path& path) const;
        // プリハブをアセットから読み込み、新しい物体番号で配置します(path: プリハブ参照パス, parent: 同じシーンの親かnullptr)。
        GameObject& InstantiatePrefab(
            const std::filesystem::path& path,
            GameObject* parent = nullptr);
        // プリハブJSONを別シーンへ復元して複製し、新しいルートを返します(json: 入力JSON, parent: 同じシーンの親かnullptr, prefabAssetPath: 実体に記録する参照パス)。
        // 保存番号を内部の新番号へ対応付け、階層は1〜4096物体の単一ルートを要求します。
        GameObject& InstantiatePrefabFromJson(
            std::string_view json,
            GameObject* parent = nullptr,
            std::filesystem::path prefabAssetPath = {});
        // 自身から親へ最も近いプリハブ実体ルートの非所有参照を探し、未発見か他シーン所属ならnullptrを返します(gameObject: 検索開始の物体)。
        [[nodiscard]] GameObject* FindPrefabInstanceRoot(
            GameObject& gameObject) const noexcept;
        // 自身から親へ最も近いプリハブ実体ルートの読み取り参照を探し、未発見か他シーン所属ならnullptrを返します(gameObject: 検索開始の物体)。
        [[nodiscard]] const GameObject* FindPrefabInstanceRoot(
            const GameObject& gameObject) const noexcept;
        // リンク先ファイルと現在の実体を比較して差分があるか返します(instanceRoot: このシーンのプリハブ実体ルート)。
        // リンク先を読み込んで検証するため、未発見や不正なプリハブは例外を伝播します。
        [[nodiscard]] bool HasPrefabOverrides(
            const GameObject& instanceRoot) const;
        // リンク先の物体配列と現在の実体を比較して設定差分を返します(instanceRoot: このシーンのプリハブ実体ルート)。
        // 構造配列は要素位置で比較し、構造変更やキーの追加・削除は個別適用を禁止します。
        [[nodiscard]] std::vector<PrefabOverride>
            GetPrefabOverrides(
                const GameObject& instanceRoot) const;
        // 個別適用できる実体の値をリンク先プリハブへ保存します(instanceRoot: このシーンのプリハブ実体ルート, path: 差分のJSON Pointer)。
        // 未発見か個別適用できない差分は例外にし、変更後の階層を検証してファイルを置換します。
        void ApplyPrefabOverride(
            const GameObject& instanceRoot,
            std::string_view path) const;
        // 個別復元できる値をリンク先から戻して実体の階層全体を再生成します(instanceRoot: 置き換えるプリハブ実体, path: 差分のJSON Pointer)。
        // 戻り値が新ルートで、旧ルートと子孫の参照・番号・保持指定は引き継がないため、外部の参照を更新します。
        GameObject& RevertPrefabOverride(
            GameObject& instanceRoot,
            std::string_view path);
        // 現在の実体の階層全体でリンク先プリハブを上書き保存します(instanceRoot: このシーンのプリハブ実体ルート)。
        void ApplyPrefabInstance(
            const GameObject& instanceRoot) const;
        // リンク先プリハブから実体の階層全体を再生成して新しいルートを返します(instanceRoot: 置き換えるプリハブ実体)。
        // 旧ルートと子孫の参照・番号・保持指定は引き継がないため、外部の参照を更新します。
        GameObject& RevertPrefabInstance(
            GameObject& instanceRoot);

        // 遷移・オブジェクト・固定物理・終盤更新を実行します(deltaTime: timeScale適用後の経過秒数)。
        void Update(float deltaTime);
        // 最も近い三次元接触を取得し、未接触ならfalseで出力を保持します(ray: 有限値の原点と方向, maximumDistance: 非負の探索距離, hit: 成功時の接触情報, filter: 対象条件)。
        // 以下の問い合わせは物体自身の有効状態と対象層を使い、親の有効状態・形状のcollisionMask・衝突マトリクスを参照しません。
        // メッシュは三角形、その他はAABBで判定し、境界の内部開始では距離0と零法線を返します。
        [[nodiscard]] bool Raycast(
            const Ray& ray,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        // 接触した三次元形状ごとの最初の接触を距離順で返します(ray: 有限値の原点と方向, maximumDistance: 非負の探索距離, filter: 対象条件)。
        // メッシュは三角形、その他はAABBで判定し、同じ物体の別形状は個別の結果になります。
        [[nodiscard]] std::vector<PhysicsHit>
            RaycastAll(
                const Ray& ray,
                float maximumDistance,
                const PhysicsQueryFilter& filter = {}) const;
        // 球スイープの最も近い接触を近似し、未接触ならfalseで出力を保持します(ray: 有限値の原点と方向, radius: 非負の半径, maximumDistance: 非負の移動距離, hit: 成功時の接触情報, filter: 対象条件)。
        // メッシュ以外は半径で拡張したAABBを使い、メッシュは中心レイの三角形接触距離から半径を引くため側面の接触を見逃します。
        [[nodiscard]] bool SphereCast(
            const Ray& ray,
            float radius,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        // 軸平行箱スイープの最も近い接触を近似し、未接触ならfalseで出力を保持します(ray: 有限値の原点と方向, halfExtents: 非負のXYZ半寸法, maximumDistance: 非負の移動距離, hit: 成功時の接触情報, filter: 対象条件)。
        // メッシュを含む全形状のAABBを半寸法で拡張して判定し、返す位置は箱中心の移動位置です。
        [[nodiscard]] bool BoxCast(
            const Ray& ray,
            const DirectX::XMFLOAT3& halfExtents,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        // Y軸カプセルを包む箱でスイープの最も近い接触を近似します(ray: 有限値の原点と方向, radius: 非負の半径, height: 両端を含む非負の全高, maximumDistance: 非負の移動距離, hit: 成功時の接触情報, filter: 対象条件)。
        // BoxCastを使用するためカプセル端の曲面は判定せず、未接触ならfalseで出力を保持します。
        [[nodiscard]] bool CapsuleCast(
            const Ray& ray,
            float radius,
            float height,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        // 軸平行箱に重なる三次元形状の非所有参照を返します(bounds: 有限値の問い合わせ境界, filter: 対象条件)。
        // メッシュは三角形、その他はAABBで判定し、一次判定で面が接するだけの組を除外します。
        [[nodiscard]] std::vector<PhysicsOverlapHit>
            OverlapBox(
                const Bounds3D& bounds,
                const PhysicsQueryFilter& filter = {}) const;
        // 球に重なる三次元形状の非所有参照を近似して返します(center: 有限なワールド中心, radius: 有限で非負の半径, filter: 対象条件)。
        // メッシュ以外は球とAABBの距離、メッシュは球を包むAABBと三角形で判定します。
        [[nodiscard]] std::vector<PhysicsOverlapHit>
            OverlapSphere(
                const DirectX::XMFLOAT3& center,
                float radius,
                const PhysicsQueryFilter& filter = {}) const;
        // 線分と半径で表すカプセルに重なる形状の非所有参照を近似して返します(start: 有限な線分始点, end: 有限な線分終点, radius: 有限で非負の半径, filter: 対象条件)。
        // メッシュ以外は線分上の9点とAABBの距離、メッシュはカプセル全体のAABBと三角形を調べるため厳密な接触判定には使いません。
        [[nodiscard]] std::vector<PhysicsOverlapHit>
            OverlapCapsule(
                const DirectX::XMFLOAT3& start,
                const DirectX::XMFLOAT3& end,
                float radius,
                const PhysicsQueryFilter& filter = {}) const;
        // 描画先テクスチャを更新してメインカメラと2Dを描きます。
        // フレーム開始・終了、背景のクリアとメイン画面のポスト処理は呼び出し側で行います。
        void Render();
        // テクスチャ→3D合成・ポスト処理→2D/UIの順に描きます(clearColor: 背景のRGBA)。
        // BeginFrameとEndFrameは呼び出し側で行います。
        void RenderGameFrame(const float clearColor[4]);
        // 有効なカメラの描画先テクスチャへ3Dとポスト処理を描きます。
        // 再入は無視し、2Dを除外して描画後に表示用テクスチャへコピーします。
        void RenderTargetTextures();
        // 今回の描画用一覧から範囲内で最も近いベイク済みプローブの非所有参照を返します(position: ワールド位置)。
        // 対象がない場合とプローブのベイク中はnullptrを返します。
        [[nodiscard]] ReflectionProbeComponent*
            NearestReflectionProbe(
                const DirectX::XMFLOAT3& position)
                const noexcept;
        // 影響度上位二つのプローブから描画用の環境と混合比を求めます(position: ワールド位置)。
        // 等影響度では近い方を優先し、両者のブレンド距離が0なら混ぜず、ベイク中は無効な環境を返します。
        [[nodiscard]] ReflectionProbeEnvironment
            ReflectionProbeEnvironmentAt(
                const DirectX::XMFLOAT3& position)
                const noexcept;
        // 有効なメインカメラで描き、カメラがない場合は指定された2Dのみ描きます(aspectRatio: カメラの幅高さ比, include2D: 2Dも描くか, target: 深度と履歴を作る描画先)。
        // targetがnullptrの場合は、この経路でSSAO・SSR用の深度と描画先の履歴を生成しません。
        void RenderMainCamera(
            float aspectRatio,
            bool include2D = true,
            RenderTarget* target = nullptr);
        // 物理変換の補間を有効にしてスプライトとUIを描きます。
        void Render2D();
        // 指定行列で影・3D・任意のデバッグ描画と2Dを描きます(view: 可逆なビュー行列, projection: ジッターなしの射影, include2D: 2Dも描くか, renderDebug: デバッグ表示を描くか, target: 深度と履歴を作る描画先)。
        // 深度とカラーに同じTAAジッターを使い、履歴の再投影はジッターなしの行列で行います。
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
            // 次のシーンへ移す所有対象
            std::vector<std::unique_ptr<GameObject>> objects;
            // 保持対象に属するメインカメラ
            CameraComponent* mainCamera{};
        };
        struct VisibilityResult final
        {
            // LODで非表示のオブジェクト番号
            std::unordered_set<GameObjectId>
                lodHidden;
            // 全可視判定で非表示の対象番号
            std::unordered_set<GameObjectId>
                renderHidden;
            // 可視判定と描画の統計
            RenderVisibilityStats stats;
        };
        // 剛体を積分して衝突・拘束・休止状態を更新します(deltaTime: 一物理ステップの秒数)。
        // 連続判定は重心からの近似球スイープで、同じ衝突組への通知は物理ステップごとに一度です。
        void StepPhysics(float deltaTime);
        // ジョイントの位置・速度・角度を補正します(deltaTime: 物理刻み秒数, applyForces: ばねとモーターの力を加える指定)。
        void SolveJoints(
            float deltaTime,
            bool applyForces);
        // 連続判定ボディ自身の寸法と移動距離から1〜8の補助刻み数を返します(deltaTime: 有限な物理経過秒数)。
        [[nodiscard]] std::size_t
            PhysicsSubstepCount(float deltaTime) const noexcept;
        // 両者の最初の有効なジョイントが接触を禁止するか返します(left: 一方の対象, right: もう一方の対象)。
        [[nodiscard]] bool CollisionSuppressedByJoint(
            const GameObject& left,
            const GameObject& right) const noexcept;
        // LOD・視錐台・遮蔽で非表示とする対象を求めます(view: 可逆なビュー行列, projection: 投影行列)。
        [[nodiscard]] VisibilityResult
            BuildRenderVisibility(
                DirectX::FXMMATRIX view,
                DirectX::CXMMATRIX projection) const;
        // 影の視錐台外の描画対象を収集します(view: 影のビュー行列, projection: 影の投影行列, alreadyHidden: 既に非表示の対象)。
        [[nodiscard]] std::unordered_set<GameObjectId>
            BuildShadowFrustumHidden(
                DirectX::FXMMATRIX view,
                DirectX::CXMMATRIX projection,
                const std::unordered_set<GameObjectId>&
                    alreadyHidden) const;
        // 有効な描画対象の境界を並列に求めて索引を更新し、再利用したか返します。
        [[nodiscard]] bool
            RefreshRenderSpatialIndex() const;
        // 有効な光源から固定配列とForward+用の照明情報を組み立てます。
        // ポイント→スポットの物体順が影番号の対応に使われるため、順序の変更は影割り当てと揃えます。
        [[nodiscard]] LightingState BuildLightingState() const noexcept;
        // 保持指定のルートと子孫の所有権を取り出し、含まれるメインカメラも引き継ぎます。
        [[nodiscard]] PersistentTransfer
            ExtractPersistentObjects();
        // 保持対象を主シーン所属として取り込みます(transfer: 所有権を渡す対象とメインカメラ)。
        // 重複する保持キーの既存対象は破棄し、番号が衝突する既存対象は新しい番号へ変えます。
        void MergePersistentObjects(
            PersistentTransfer transfer);
        // Sceneの再読み込みによるGameObject IDの再利用を識別します。
        // オブジェクト番号の再利用世代
        std::uint64_t m_contentRevision{};

        // シーンJSONを更新して物体・設定・親と内部参照を復元します(json: 入力JSON, additive: 既存内容へ追加するか, sourcePath: 追加シーンの生成パス)。
        // 追加読み込みの失敗は追加分を破棄し、主シーンの読み込みはClear後の失敗をロールバックしません。
        SceneHandle ApplySceneJson(
            std::string_view json,
            bool additive,
            std::filesystem::path sourcePath);

        // 借用する描画機器
        GraphicsDevice& m_graphics;
        // 論理画面の幅と高さを変える処理
        std::function<bool(std::uint32_t, std::uint32_t)>
            m_windowSizeSetter;
        // 論理画面の幅と高さを返す処理
        std::function<std::pair<std::uint32_t, std::uint32_t>()>
            m_windowSizeGetter;
        // 逆順破棄によりワーカーと全コンポーネントの終了後まで描画資源を保持します。
        // シーンが利用中の描画資源の保持
        GraphicsDeviceResourceLease
            m_graphicsResourceLease;
        // シーン読み込みと遷移の所有管理
        std::unique_ptr<SceneManager>
            m_sceneManager;
        // シーンが所有するオブジェクト
        std::vector<std::unique_ptr<GameObject>> m_gameObjects;
        // Clear()では消さない（プロジェクト単位の設定のため）。
        // プロジェクトの登録タグ一覧
        std::vector<std::string> m_registeredTags;
        // Clear()では消さない（永続Scriptの購読を保つため。破棄されたScriptの購読はデストラクタで解除されます）。
        // シーンをまたぐ名前付きイベント
        EventBus m_events;
        // 非所有のメインカメラ
        CameraComponent* m_mainCamera{};
        // 次に割り当てるオブジェクト番号
        GameObjectId m_nextId{ 1 };
        // 追加読み込み中のシーンのハンドル。
        // 0以外の間に作られたGameObjectは、そのシーンの所属になります。
        // 生成中の追加シーンの所属番号
        SceneHandle m_loadingScene{};
        // 次に割り当てる追加シーン番号
        SceneHandle m_nextSceneHandle{ 1 };
        // 読み込み済み追加シーンの情報
        std::vector<LoadedSceneInfo> m_additiveScenes;
        // 現在有効な接触組の状態
        std::map<CollisionKey, bool> m_activeCollisions;
        // 環境光のRGB色
        DirectX::XMFLOAT3 m_ambientLightColor{
            0.65f,
            0.72f,
            0.85f
        };
        // 環境光の強さ
        float m_ambientLightIntensity{ 0.35f };
        // 空の描画設定
        SkySettings m_sky;
        // 霧の描画設定
        FogSettings m_fog;
        // ブルームの描画設定
        BloomSettings m_bloom;
        // 画面輪郭線の描画設定
        ScreenOutlineSettings m_screenOutline;
        // 画面上のレンズフレア設定
        ScreenSpaceLensFlareSettings m_screenSpaceLensFlare;
        // 画面上の環境遮蔽の設定
        AmbientOcclusionSettings m_ambientOcclusion;
        // 画面上の反射の設定
        ScreenSpaceReflectionSettings
            m_screenSpaceReflection;
        // 時間方向のアンチエイリアス設定
        TemporalAntiAliasingSettings
            m_temporalAntiAliasing;
        // 直前の3D描画の時間AA状態
        TemporalAntiAliasingFrame m_temporalFrame;
        // 全ビューで同じジッター列を使い、番号はシーン全体で進めます。
        // シーン共通のジッター番号
        std::uint32_t m_temporalFrameIndex{};
        // ボリュメトリック光の設定
        VolumetricLightSettings m_volumetricLight;
        // 影の行列とライト情報は3D描画のときだけ揃うので、そこで作ってポスト処理へ受け渡します。
        // 直前の3D描画の光と影情報
        VolumetricLightFrame m_volumetricFrame;
        // 被写界深度の設定
        DepthOfFieldSettings m_depthOfField;
        // 3D描画時の行列を保持し、PostProcessFrameDataが現在の設定を合わせます。
        // 直前の3D描画の被写界深度行列
        DepthOfFieldFrame m_depthOfFieldFrame;
        // 直前の3D描画の輪郭用行列
        PostProcessFrame::ScreenOutlineFrame
            m_screenOutlineFrame;
        // カメラ由来のブラー設定
        MotionBlurSettings m_motionBlur;
        // 直前の3D描画のブラー用行列
        MotionBlurFrame m_motionBlurFrame;
        // 自動露出の設定
        AutoExposureSettings m_autoExposure;
        // 色調補正の設定
        ColorGradingSettings m_colorGrading;
        // 物理候補格子のセル幅
        float m_physicsBroadPhaseCellSize{ 4.0f };
        // 直近の物理ステップの統計
        PhysicsBroadPhaseStats m_physicsStats;
        // デバッグ表示に保存する接触
        std::vector<PhysicsDebugContact> m_physicsDebugContacts;
        // 接触点記録の有効状態
        bool m_physicsDebugCaptureEnabled{};
        // 固定更新と描画補間の時刻管理
        Detail::PhysicsFrameClock m_physicsClock;
        // 描画で物理補間を使う状態
        bool m_renderingInterpolatedTransforms{};
        // レンダーテクスチャ描画中の再入を防ぎます。
        // テクスチャ先へ描画中の状態
        bool m_renderingTargetTextures{};
        // 視錐台カリングの有効状態
        bool m_frustumCullingEnabled{ true };
        // 遮蔽カリングの有効状態
        bool m_occlusionCullingEnabled{ true };
        // 直近の可視判定と描画の統計
        RenderVisibilityStats m_visibilityStats;
        // BVHの構築・検索・破棄は索引の所有者へ委譲します。
        // 描画候補の境界索引
        mutable RenderSpatialIndex m_renderSpatialIndex;
        // 初期化に失敗したら従来経路で描画し、毎フレームの再試行を避けます。
        // クラスタ照明の初期化失敗状態
        bool m_clusteredLightingUnavailable{};
        // 要求のある有効な反射プローブをキャッシュから復元または同期ベイクします。
        // 再入中と描画未初期化時は処理せず、描画準備の失敗時は要求を解除します。
        void BakePendingReflectionProbes();
        // 今フレームの描画で使うプローブ一覧（毎フレーム集め直し）。
        // 今回描画する非所有のプローブ一覧
        std::vector<ReflectionProbeComponent*>
            m_frameReflectionProbes;
        // ベイク中の再帰描画でプローブ自身が映り込まないように。
        // プローブの再帰描画中の状態
        bool m_bakingReflectionProbes{};
        // 空のパスまたはビューの世代変更時に、照明キャッシュ用の内容を再ハッシュします。
        // 空の照明キャッシュの元パス
        std::filesystem::path m_skyPrefilterKeyPath;
        // 空の照明キャッシュの元ビュー
        GraphicsViewHandle m_skyPrefilterKeySourceView;
        // 空の照明キャッシュのキー
        std::uint64_t m_skyPrefilterKey{};

        // 間接光のベイクを一呼び出しで最大8点進め、完了時にfp16へ変換します。
        // 再入中と描画未初期化時は待機し、描画失敗時は進行中のベイクを終了します。
        void ProcessBakedGlobalIlluminationBake();
        // 更新された間接光係数をGPUのRGB別3Dテクスチャへ転送します。
        void EnsureBakedGlobalIlluminationTextures();
        // 間接光のベイク設定
        BakedGlobalIlluminationSettings m_bakedGiSettings;
        // fp16のSH係数は[R,G,B]×[z,y,x]×RGBA16F順で連結し、JSONへbase64で保存します。
        // 保存済み間接光のfp16係数
        std::vector<std::uint16_t> m_bakedGiData;
        // m_bakedGiDataを焼いたときの格子と配置（設定を後から変えても、表示は焼いたときの形で続けるため）。
        // 保存済み間接光の格子と配置
        BakedGlobalIlluminationSettings m_bakedGiBakedShape;
        // 表示する間接光のRGBビュー
        std::array<GraphicsViewHandle, 3> m_bakedGiViews;
        // 間接光のGPU転送待ち状態
        bool m_bakedGiTexturesDirty{};
        // ベイクの進行状態（毎フレーム数点ずつ進める）。
        // 間接光をベイク中の状態
        bool m_bakedGiBaking{};
        // 次にベイクする格子点番号
        std::size_t m_bakedGiNextProbe{};
        // ベイク中の間接光係数
        std::vector<float> m_bakedGiWorking;
    };
}
