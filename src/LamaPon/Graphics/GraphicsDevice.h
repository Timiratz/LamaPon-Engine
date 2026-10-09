#pragma once

#include "LamaPon/Graphics/EnvironmentRenderer.h"
#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/GraphicsBackend.h"
#include "LamaPon/Graphics/GraphicsDeviceResourceLease.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/GraphicsQuality.h"
#include "LamaPon/Graphics/ShaderVariants.h"
#include "LamaPon/Graphics/SpriteRendering.h"

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DirectX
{
    inline namespace DX11
    {
        class CommonStates;
        class SpriteBatch;
    }
}

namespace LamaPon
{
    namespace Detail
    {
        class GraphicsDeviceApiResources;
        class GraphicsDeviceD3D11Access;
        class GraphicsDeviceD3D12Access;
        struct GraphicsDeviceD3D11Resources;
        struct MaterialShaderDrawRequest;
        struct MaterialShaderPasses;
    }

    struct FrameStatistics final
    {
        // 平滑化した毎秒フレーム数
        float framesPerSecond{};
        // 平滑化したフレームミリ秒
        float frameTimeMilliseconds{};
        // 平滑化したCPU処理ミリ秒
        float cpuTimeMilliseconds{};
        // 記録したフレーム数
        std::uint64_t totalFrames{};

        // 既存モジュールの値コピー互換のため項目の順序を維持し、新規項目は末尾へ追加します。

        // リセット以後の代替使用回数
        std::uint32_t shaderFallbackDraws{};
    };

    // 約500ms間隔で更新するバイト単位のRAM使用量・GPU容量・OS予算です。
    struct GraphicsMemoryStatistics final
    {
        // プロセスの常駐RAM使用量
        std::uint64_t processWorkingSetBytes{};
        // プロセスの専有コミット量
        std::uint64_t processPrivateBytes{};
        // システムの物理RAM使用量
        std::uint64_t systemPhysicalUsedBytes{};
        // システムの物理RAM容量
        std::uint64_t systemPhysicalTotalBytes{};
        // 専用VRAMの搭載容量
        std::uint64_t dedicatedVideoMemoryBytes{};
        // 共有メモリの搭載容量
        std::uint64_t sharedSystemMemoryBytes{};
        // ローカルGPU使用量
        std::uint64_t localVideoMemoryUsageBytes{};
        // ローカルGPU予算
        std::uint64_t localVideoMemoryBudgetBytes{};
        // 非ローカルGPU使用量
        std::uint64_t nonLocalVideoMemoryUsageBytes{};
        // 非ローカルGPU予算
        std::uint64_t nonLocalVideoMemoryBudgetBytes{};
        // いずれかのGPU予算取得可否
        bool videoMemoryBudgetAvailable{};
    };

    class Application;
    class RuntimeServices;
    class AssetManager;
    class AudioSystem;
    class ClusteredLights;
    class DebugRenderer;
    class FrameDebugger;
    class InputSystem;
    class LitEffect;
    class ModelRendererComponent;
    class MeshRendererComponent;
    class ParticleSystemComponent;
    class D3D12EditorModelPreviewRenderer;
    struct LitTextureRequest;
    struct ReflectionProbeEnvironment;
    struct ParticleDrawRequest;
    struct PrimitiveDrawRequest;
    struct ShaderRenderState;
    class SpriteEffect;
    class ShadowMap;
    class RenderTarget;
    class ScreenEffect;
    class ComputeEffect;
    struct TextureResourceSnapshot;
    struct AutoExposureSettings;
    struct AmbientOcclusionSettings;
    struct BloomSettings;
    struct ScreenOutlineSettings;
    struct ScreenSpaceLensFlareSettings;
    struct TemporalAntiAliasingSettings;
    struct TemporalAntiAliasingInputs;
    struct VolumetricLightSettings;
    struct VolumetricLightInputs;
    struct DepthOfFieldSettings;
    struct MotionBlurSettings;
    struct ColorGradingSettings;
    struct SceneLoadingScreenSettings;
    struct SceneTransitionFrame;
    struct VolumetricLightFrame;
    struct TemporalAntiAliasingFrame;
    struct DepthOfFieldFrame;
    struct PostProcessFrame;

    // 深度描画の目的を区別し、Prepassでは主描画と同じ深度を書ける素材だけを描きます。
    enum class DepthPassKind
    {
        None,
        Shadow,
        Prepass
    };

    // 画面効果を挿入する順序で、AfterToneMappingだけLDR、前の3地点はHDRです。
    enum class ScreenEffectPoint : std::uint8_t
    {
        // 3D描画の直後でTAAより前
        BeforePostProcess,
        // 被写界深度・動きぼかしの後
        BeforeBloom,
        // ブルーム・フレアの後
        BeforeToneMapping,
        // 色調変換後の既定位置
        AfterToneMapping
    };

    struct ScreenEffectRequest final
    {
        // 画面効果シェーダーのパス
        std::filesystem::path shader;
        // t1・t2の追加入力画像パス
        std::array<std::filesystem::path, 2>
            auxiliaryTextures{};
        // b0の自作シェーダー引数
        std::array<DirectX::XMFLOAT4, 8>
            customParameters{};
        // 後処理への挿入位置
        ScreenEffectPoint point{
            ScreenEffectPoint::AfterToneMapping };
    };

    // 自作計算シェーダーを実行し、結果を名前付き画像へ書きます。
    // 直接HLSLはCSMainを使い、manifestの入口指定はD3D11経路で解決します。
    struct ComputeEffectRequest final
    {
        // 計算シェーダーの相対パス
        std::filesystem::path shader;
        // 書き込む画像の登録名
        std::string outputTexture;
        // 出力幅（ピクセル）
        std::uint32_t outputWidth{ 512 };
        // 出力高（ピクセル）
        std::uint32_t outputHeight{ 512 };
        // t0・t1の入力画像パス
        std::array<std::filesystem::path, 2> inputTextures{};
        // b0の自作シェーダー引数
        std::array<DirectX::XMFLOAT4, 8>
            customParameters{};
    };

    class GraphicsDevice final
    {
    public:
        // 描画デバイスの共有状態を構築します。
        GraphicsDevice();
        // リース受付を閉じて全資源を解放します。
        ~GraphicsDevice();

        // 描画デバイスのコピーを禁止します。
        GraphicsDevice(const GraphicsDevice&) = delete;
        // 描画デバイスのコピー代入を禁止します。
        GraphicsDevice& operator=(const GraphicsDevice&) = delete;

        // 借用中のGPU資源がある期間のリースを取得します。
        // 外部のGPU資源より長く保持すると、Sceneや独自描画器が借用中の再初期化を拒否できます。
        [[nodiscard]] GraphicsDeviceResourceLease
            AcquireResourceLease();

        // D3D11で描画を初期化します(window: 描画先のHWND, width: 初期画像幅, height: 初期画像高)。
        void Initialize(HWND window, std::uint32_t width, std::uint32_t height);
        // 要求APIで描画を初期化します(window: 描画先のHWND, width: 初期画像幅, height: 初期画像高, requestedApi: 要求する描画API)。
        void Initialize(
            HWND window,
            std::uint32_t width,
            std::uint32_t height,
            RenderingApi requestedApi);
        // 借用中の資源がないことを確認して初期化します(window: 描画先のHWND, width: 初期画像幅, height: 初期画像高, requestedApi: 要求する描画API, profile: 互換用の起動指定)。
        void Initialize(
            HWND window,
            std::uint32_t width,
            std::uint32_t height,
            RenderingApi requestedApi,
            GraphicsStartupProfile profile);
        // 表示寸法が変わった場合に再作成します(width: 描画幅, height: 描画高)。
        void Resize(std::uint32_t width, std::uint32_t height);

        // 次回初期化のアダプター優先指定を設定します(prefer: ソフトウェア描画の優先)。
        static void SetPreferWarpAdapter(
            const bool prefer) noexcept;

        // 次回初期化の検証レイヤー指定を設定します(enable: 有効化するか)。
        static void SetEnableDebugLayer(
            const bool enable) noexcept;
        // 検証レイヤーの初期化指定を返します。
        [[nodiscard]] static bool
            IsDebugLayerEnabled() noexcept;

        // 表示画像をCPUへ読み戻します(width: 画像幅の出力, height: 画像高の出力)。
        // RGBA8で取得し、EndFrameのPresentより前に呼びます。
        [[nodiscard]] std::vector<std::uint8_t>
            CaptureBackBuffer(
                std::uint32_t& width,
                std::uint32_t& height) const;

        // 計測と段階転送を開始して画面を消去します(clearColor: RGBAの消去色)。
        void BeginFrame(const float clearColor[4]);
        // 実効APIで描画パスを開始します(description: 合成方式・シェーダー・定数・ライト情報)。
        // パスが生存中はリースで再初期化を拒否し、破棄時に終了を試みます。
        [[nodiscard]] SpriteRenderPass BeginSpritePass(
            const SpritePassDescription& description = {});
        // 共通インスタンス頂点資源を更新します(data: 頂点のバイト列)。
        [[nodiscard]] GraphicsBufferHandle AcquireInstanceBufferHandle(
            std::span<const std::byte> data);
        // 頂点資源の設定を転送します(buffer: 同世代の頂点資源, slot: 入力スロット, stride: 頂点間隔のバイト数, offset: 先頭のバイト位置)。
        void BindVertexBuffer(
            const GraphicsBufferHandle& buffer,
            std::uint32_t slot,
            std::uint32_t stride,
            std::uint32_t offset = 0);
        // 入力ビューの設定を転送します(firstSlot: 先頭t番号, resources: 設定するビュー列, fallback: 無効要素の代替ビュー)。
        // D3D12の非空範囲はfalseで、空・無効な要素はD3D11では代替ビューへ置き換えます。
        [[nodiscard]] bool TryBindPixelShaderResources(
            std::uint32_t firstSlot,
            std::span<const GraphicsViewHandle> resources,
            const GraphicsViewHandle& fallback = {}) noexcept;
        // 現在の深度パスを設定します(kind: 深度描画の種類)。
        void SetDepthPass(DepthPassKind kind) noexcept;
        // 現在の深度パスの種類を返します。
        [[nodiscard]] DepthPassKind DepthPass() const noexcept;
        // 現在が深度のみのパスか返します。
        [[nodiscard]] bool IsDepthOnlyPass() const noexcept;
        // 計測と描画記録を確定して画面を表示します。
        void EndFrame();
        // シーンのHDR描画先を設定して消去します(clearColor: RGBAの消去色)。
        void BeginSceneComposition(const float clearColor[4]);
        // シーンを後処理して画面へ合成します(bloom: ブルーム設定, colorGrading: 色調補正設定)。
        void EndSceneComposition(
            const BloomSettings& bloom,
            const ColorGradingSettings& colorGrading);
        // シーンを後処理して画面へ合成します(bloom: ブルーム設定, lensFlare: レンズフレア設定, colorGrading: 色調補正設定)。
        void EndSceneComposition(
            const BloomSettings& bloom,
            const ScreenSpaceLensFlareSettings& lensFlare,
            const ColorGradingSettings& colorGrading);
        // シーンを後処理して画面へ合成します(bloom: ブルーム設定, colorGrading: 色調補正設定, volumetric: 光の筋のフレーム入力, temporal: TAAのフレーム入力)。
        void EndSceneComposition(
            const BloomSettings& bloom,
            const ColorGradingSettings& colorGrading,
            const VolumetricLightFrame& volumetric,
            const TemporalAntiAliasingFrame& temporal);
        // シーンを後処理して画面へ合成します(bloom: ブルーム設定, lensFlare: レンズフレア設定, colorGrading: 色調補正設定, volumetric: 光の筋のフレーム入力, temporal: TAAのフレーム入力)。
        void EndSceneComposition(
            const BloomSettings& bloom,
            const ScreenSpaceLensFlareSettings& lensFlare,
            const ColorGradingSettings& colorGrading,
            const VolumetricLightFrame& volumetric,
            const TemporalAntiAliasingFrame& temporal);
        // 登録効果を含む後処理を実行して画面へ合成します(frame: フレームの後処理設定と入力)。
        void EndSceneComposition(
            const PostProcessFrame& frame);
        // シーンのHDR描画先を借用します。
        // HDR描画はBeginSceneCompositionとEndSceneCompositionの区間で行います。
        [[nodiscard]] RenderTarget*
            SceneCompositionTarget() const noexcept;
        // シーンの射影行列を保存します(projection: 現在の射影行列)。
        void SetSceneProjection(
            const DirectX::XMFLOAT4X4& projection) noexcept;
        // 保存したシーンの射影行列を参照します。
        [[nodiscard]] const DirectX::XMFLOAT4X4&
            SceneProjection() const noexcept;
        // 設定された読み込み画面を描きます(progress: 0～1の進捗, settings: 表示設定, width: 画面幅で0は現在値, height: 画面高で0は現在値)。
        void DrawLoadingScreen(
            float progress,
            const SceneLoadingScreenSettings& settings,
            std::uint32_t width = 0,
            std::uint32_t height = 0);
        // 遷移状態に応じて読み込み画面を描きます(frame: 進捗・透明度・経過時間, settings: 表示設定, width: 画面幅で0は現在値, height: 画面高で0は現在値)。
        void DrawLoadingScreen(
            const SceneTransitionFrame& frame,
            const SceneLoadingScreenSettings& settings,
            std::uint32_t width = 0,
            std::uint32_t height = 0);
        // 縦横比を保って起動ロゴを描きます(logoPath: 画像パス, width: 画面幅で0は現在値, height: 画面高で0は現在値)。
        void DrawStartupLogo(
            const std::filesystem::path& logoPath =
                L"textures/LamaPonEngineLogo.png",
            std::uint32_t width = 0,
            std::uint32_t height = 0);
        // 設定を補正し、必要なら影資源を再生成します(settings: 新しい描画設定)。
        // API変更は再起動で反映し、影の再生成に失敗しても先に保存した設定は戻しません。
        void SetGraphicsSettings(
            const GraphicsSettings& settings);
        // 描画APIの指定を保持して品質を設定します(preset: 適用する品質プリセット)。
        void ApplyQualityPreset(
            GraphicsQualityPreset preset);
        // 現在の描画設定を参照します。
        [[nodiscard]] const GraphicsSettings&
            Settings() const noexcept;
        // 実効APIを返し、バックエンドがなければD3D11です。
        [[nodiscard]] RenderingApi
            ActiveRenderingApi() const noexcept;
        // 起動時に保存した要求APIを返します。
        [[nodiscard]] RenderingApi
            StartupRenderingApi() const noexcept;
        // 起動時の代替API選択理由を返します。
        [[nodiscard]] RenderingApiFallbackReason
            RenderingApiFallback() const noexcept;
        // 初期化済みのD3D12バックエンドか返します。
        [[nodiscard]] bool IsD3D12ExperimentalBootstrap() const noexcept;
        // 初期化済みのD3D12バックエンドか返します。
        [[nodiscard]] bool IsD3D12ExperimentalRenderer() const noexcept
        {
            return IsD3D12ExperimentalBootstrap();
        }
        // フレーム時間と描画回数の統計を参照します。
        [[nodiscard]] const FrameStatistics&
            FrameStats() const noexcept;
        // 最後に採取したメモリ統計を参照します。
        [[nodiscard]] const GraphicsMemoryStatistics&
            MemoryStats() const noexcept;
        // 取得できたメモリ統計を500ms間隔で更新します(force: 採取間隔を無視する指定)。
        void RefreshMemoryStatistics(
            bool force = false) noexcept;
        // 時間を平滑化してフレーム統計を更新します(frameTimeSeconds: フレーム時間の秒数, cpuTimeMilliseconds: CPU処理時間のミリ秒)。
        void RecordFrameStatistics(
            float frameTimeSeconds,
            float cpuTimeMilliseconds) noexcept;
        // フレームの代替シェーダー使用回数をリセットします。
        void ResetShaderFallbackDraws() noexcept;
        // 現在のバックエンドがティアリングを許可するか返します。
        [[nodiscard]] bool TearingAllowed() const noexcept;

        // バックエンドの初期化状態を返します。
        [[nodiscard]] bool IsInitialized() const noexcept;
        // 描画先資源を再作成します(target: 更新する描画先, width: 画像幅, height: 画像高)。
        void ResizeOffscreenTarget(
            RenderTarget& target,
            std::uint32_t width,
            std::uint32_t height);
        // 描画先を設定して色と深度を消去します(target: 描画先, clearColor: RGBAの消去色)。
        void BeginOffscreenTarget(
            RenderTarget& target,
            const float clearColor[4]);
        // 画像を消さず描画先を設定します(target: 描画先)。
        void BindOffscreenTarget(RenderTarget& target);
        // 描画先を変えず表示用画像へ確定します(target: 確定する描画先)。
        void PublishOffscreenTarget(RenderTarget& target);
        // 消去・復元せず深度だけを設定します(target: 深度の描画先)。
        void BindOffscreenTargetDepthOnly(RenderTarget& target);
        // 描画先を変えず読み取り用深度を保存します(target: 保存先)。
        void CaptureOffscreenTargetDepth(RenderTarget& target);
        // SSR用HDR履歴を保存します(target: 保存先, viewProjection: 画像描画時の行列)。
        // 現在フレームが旧履歴を読み終えた後に保存します。
        void CaptureOffscreenTargetColorHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection);
        // TAA用履歴を保存します(target: 保存先, viewProjection: ジッターなしの再投影行列)。
        void CaptureOffscreenTargetTemporalHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection);
        // 深度から遮蔽率を生成したか返します(target: 深度入力と遮蔽率の出力先, settings: 遮蔽の設定, projection: 深度復元の射影行列, sampleCount: 採取点数)。
        [[nodiscard]] bool ResolveOffscreenTargetAmbientOcclusion(
            RenderTarget& target,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // 再投影した履歴を混ぜます(target: 処理する描画先, settings: TAAの設定, inputs: 履歴と再投影行列)。
        void ApplyOffscreenTargetTemporalAntiAliasing(
            RenderTarget& target,
            const TemporalAntiAliasingSettings& settings,
            const TemporalAntiAliasingInputs& inputs);
        // 影から光の筋を合成します(target: HDRの描画先, settings: 光の筋の設定, inputs: カメラと光源・影情報)。
        void ApplyOffscreenTargetVolumetricLight(
            RenderTarget& target,
            const VolumetricLightSettings& settings,
            const VolumetricLightInputs& inputs);
        // 焦点帯の外を深度に応じてぼかします(target: HDRの描画先, settings: 被写界深度の設定, projection: 深度復元の射影行列, sampleCount: 採取点数)。
        void ApplyOffscreenTargetDepthOfField(
            RenderTarget& target,
            const DepthOfFieldSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        // 前フレームとの差に沿って色を平均します(target: HDRの描画先, settings: 動きぼかしの設定, inverseViewProjection: 世界座標を復元する逆行列, viewProjection: 保存する現在の射影行列, sampleCount: 採取点数)。
        void ApplyOffscreenTargetMotionBlur(
            RenderTarget& target,
            const MotionBlurSettings& settings,
            const DirectX::XMFLOAT4X4& inverseViewProjection,
            const DirectX::XMFLOAT4X4& viewProjection,
            std::uint32_t sampleCount);
        // 高輝度部のぼかしを適用します(target: HDRの描画先, settings: ブルームの設定)。
        void ApplyOffscreenTargetBloom(
            RenderTarget& target,
            const BloomSettings& settings);
        // 光条・ゴースト・ハローを合成します(target: HDRの描画先, settings: レンズフレアの設定)。
        void ApplyOffscreenTargetScreenSpaceLensFlare(
            RenderTarget& target,
            const ScreenSpaceLensFlareSettings& settings);
        // ACES近似と色調補正を適用します(target: HDRの描画先, settings: 色調補正の設定)。
        void ApplyOffscreenTargetToneMapping(
            RenderTarget& target,
            const ColorGradingSettings& settings);
        // 深度と法線から輪郭を合成します(target: 処理する描画先, settings: 輪郭の設定, projection: 深度復元の射影行列)。
        void ApplyOffscreenTargetScreenOutline(
            RenderTarget& target,
            const ScreenOutlineSettings& settings,
            const DirectX::XMFLOAT4X4& projection);
        // 輝度の縁を検出して平滑化します(target: 処理する描画先)。
        void ApplyOffscreenTargetFXAA(RenderTarget& target);
        // 輝度へ順応した露出補正EVを返します(target: HDR入力と測定値の保存先, settings: 自動露出の設定, deltaSeconds: 前回からの経過秒)。
        [[nodiscard]] float UpdateOffscreenTargetAutoExposure(
            RenderTarget& target,
            const AutoExposureSettings& settings,
            float deltaSeconds);
        // 影の描画を開始します(shadowMap: 影の描画先, cascadeIndex: 描画面の番号)。
        void BeginShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t cascadeIndex);
        // 影描画前の出力へ復元します(shadowMap: 終了する影の描画先)。
        void EndShadowMap(ShadowMap& shadowMap);
        // Forward+の番号表を更新します(lighting: ライト情報と結果, view: ビュー行列, projection: 射影行列, width: 画面幅, height: 画面高)。
        void UpdateClusteredLights(
            LightingState& lighting,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            std::uint32_t width,
            std::uint32_t height);
        // 同じ描画区間で使う主描画先の復元状態を保存します。
        [[nodiscard]] std::unique_ptr<GraphicsOutputState>
            CaptureOutputState();
        // 主描画先を復元します(state: 同じバックエンドで保存した状態)。
        void RestoreOutputState(
            const GraphicsOutputState& state);

        // 所有する白い代替画像のハンドルを返します。
        [[nodiscard]] GraphicsTextureHandle
            WhiteTextureHandle() const noexcept;
        // 白い代替画像の読み取りビューを返します。
        [[nodiscard]] GraphicsViewHandle
            WhiteTextureViewHandle() const noexcept;
        // 2D画像の生成を転送します(description: 形式・寸法・更新方式, initialData: 全ミップの初期値)。
        [[nodiscard]] GraphicsTextureHandle CreateTexture2D(
            const GraphicsTexture2DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData);
        // 3D画像の生成を転送します(description: 形式と寸法, initialData: 全ミップの体積データ)。
        [[nodiscard]] GraphicsTextureHandle CreateTexture3D(
            const GraphicsTexture3DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData);
        // 描画スレッドでミップ更新を転送します(texture: 更新可能な同世代の画像, mipLevel: ミップ番号, data: 画像と行ピッチ)。
        void UpdateTexture2D(
            const GraphicsTextureHandle& texture,
            std::uint32_t mipLevel,
            const GraphicsTextureSubresourceData& data);
        // 読み取りビューの生成を転送します(texture: 同世代の画像, description: 形式とミップ範囲)。
        [[nodiscard]] GraphicsViewHandle CreateShaderResourceView(
            const GraphicsTextureHandle& texture,
            const GraphicsTextureViewDescription& description);
        // 現在の資源世代に属するか返します(view: 確認するビュー)。
        [[nodiscard]] bool IsGraphicsViewCurrent(
            const GraphicsViewHandle& view) const noexcept;
        // 必要なら資産管理器を生成して参照します。
        [[nodiscard]] AssetManager& Assets() const;
        // 生成済みの資産管理器を借用し、なければnullptrです。
        [[nodiscard]] AssetManager* TryAssets() const noexcept;
        // 実行サービスが所有する音声システムを参照します。
        [[nodiscard]] AudioSystem& Audio() const;
        // 実行サービスが所有する入力システムを参照します。
        [[nodiscard]] InputSystem& Input() const;
        // 線分描画器を参照し、未初期化ならlogic_errorです。
        [[nodiscard]] DebugRenderer& Debug() const;
        // 所有するGPU計測器を参照します。
        [[nodiscard]] GpuProfiler& Gpu() noexcept;
        // 所有する描画記録器を参照します。
        [[nodiscard]] FrameDebugger& FrameDebug() noexcept;
        // 現在の基盤で描画可能なキューブか調べる(cubemap: 確認する参照)。
        [[nodiscard]] bool IsSampleableCubeView(
            const GraphicsViewHandle& cubemap) const noexcept;
        // 有効なキューブまたはグラデーションの空を描く(view: ビュー行列, projection: 射影行列, settings: 空の設定, cubemap: 任意のキューブ参照, sun: 任意の太陽設定)。
        void DrawSky(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const SkySettings& settings,
            const GraphicsViewHandle& cubemap = {},
            const SkySunDescription* sun = nullptr) const;
        // 反射用深度を生成し、失敗なら偽を返す(target: 描画先, projectionZ: 射影行列の深度係数, projectionW: 射影行列の深度定数)。
        [[nodiscard]] bool TryBuildReflectionDepthPyramid(
            RenderTarget& target,
            float projectionZ,
            float projectionW) const noexcept;
        // D3D11で環境光の係数をベイクする(renderFace: キューブ各面の描画処理)。
        [[nodiscard]] std::optional<std::array<float, 12>>
            BakeIrradianceProbe(
                const EnvironmentProbeFaceRenderer& renderFace) const;
        // 環境マップを畳み込み、失敗なら空を返す(source: 元のキューブ参照, cacheKey: キャッシュ識別子)。
        // 同じ入力とキーなら再利用し、失敗では片方だけの結果を公開しません。
        [[nodiscard]] PrefilteredEnvironmentViews
            TryGetPrefilteredEnvironmentViews(
                const GraphicsViewHandle& source,
                std::uint64_t cacheKey = 0) const noexcept;
        // 稼働中のAPIで環境プローブのベイクを準備する。
        void PrepareEnvironmentProbeBake() const;
        // 反射用環境マップをベイクする(renderFace: キューブ各面の描画処理, cacheKey: 任意のキャッシュ識別子)。
        [[nodiscard]] PrefilteredEnvironmentViews
            BakeReflectionProbeViews(
                const EnvironmentProbeFaceRenderer& renderFace,
                std::optional<std::uint64_t> cacheKey =
                    std::nullopt) const;
        // 環境マップのキャッシュを取り込み、失敗なら空を返す(key: キャッシュ識別子)。
        [[nodiscard]] PrefilteredEnvironmentViews
            TryLoadCachedEnvironmentViews(
                std::uint64_t key) const noexcept;
        // RGB別のGI入力を生成し、失敗なら全て空です(width: プローブ列数, height: プローブ行数, depth: プローブ奥行数, coefficients: RGB順に各4本の半精度SH係数)。
        [[nodiscard]] std::array<GraphicsViewHandle, 3>
            UploadBakedGlobalIlluminationViews(
                std::uint32_t width,
                std::uint32_t height,
                std::uint32_t depth,
                std::span<const std::uint16_t> coefficients)
                    const noexcept;
        // 全ビューの検証後に画像を設定したか返します(effect: 同デバイスの照明効果, request: 描画終了まで保持する画像入力)。
        // 不正な非空ビューでは変更せずfalseで、空は正常な未指定として扱います。
        [[nodiscard]] bool TrySetLitEffectTextures(
            LitEffect& effect,
            const LitTextureRequest& request) const noexcept;
        // 一時的な画像入力を禁止します(effect: 設定先の照明効果, request: 保持期間を満たさない入力)。
        bool TrySetLitEffectTextures(
            LitEffect& effect,
            LitTextureRequest&& request) const = delete;
        // 一時的な画像入力を禁止します(effect: 設定先の照明効果, request: 保持期間を満たさない入力)。
        bool TrySetLitEffectTextures(
            LitEffect& effect,
            const LitTextureRequest&& request) const = delete;
        // 全ビューの検証後に照明を設定したか返します(effect: 同デバイスの照明効果, lighting: 描画終了まで保持する入力)。
        // 不正な組み合わせでは効果を変更せずfalseを返します。
        [[nodiscard]] bool TrySetLitEffectLighting(
            LitEffect& effect,
            const LightingState& lighting) const noexcept;
        // 一時的な照明入力を禁止します(effect: 設定先の照明効果, lighting: 保持期間を満たさない入力)。
        bool TrySetLitEffectLighting(
            LitEffect& effect,
            LightingState&& lighting) const = delete;
        // 一時的な照明入力を禁止します(effect: 設定先の照明効果, lighting: 保持期間を満たさない入力)。
        bool TrySetLitEffectLighting(
            LitEffect& effect,
            const LightingState&& lighting) const = delete;
        // 全キューブの検証後に反射入力を設定します(effect: 同デバイスの照明効果, probe: 描画終了まで保持する入力)。
        [[nodiscard]] bool TrySetLitEffectReflectionProbe(
            LitEffect& effect,
            const ReflectionProbeEnvironment& probe) const noexcept;
        // 一時的な反射入力を禁止します(effect: 設定先の照明効果, probe: 保持期間を満たさない入力)。
        bool TrySetLitEffectReflectionProbe(
            LitEffect& effect,
            ReflectionProbeEnvironment&& probe) const = delete;
        // 一時的な反射入力を禁止します(effect: 設定先の照明効果, probe: 保持期間を満たさない入力)。
        bool TrySetLitEffectReflectionProbe(
            LitEffect& effect,
            const ReflectionProbeEnvironment&& probe) const = delete;
        // D3D11標準照明効果を遅延生成して返します。
        [[nodiscard]] LitEffect& Lit() const;
        // D3D11骨変形の標準照明効果を遅延生成して返します。
        [[nodiscard]] LitEffect& SkinnedLit() const;
        // 代替表示効果を返し、生成失敗ならnullptrです(skinned: 骨変形の指定)。
        [[nodiscard]] LitEffect* ShaderErrorPlaceholder(
            const bool skinned) const;
        // 非同期シェーダー準備の使用を設定します(enabled: 非同期準備の有効化)。
        // アーカイブ資産は同期準備し、非同期の準備中は標準Litで描画を続けます。
        void SetAsyncShaderCompilationEnabled(
            bool enabled) noexcept;
        // 非同期シェーダー準備の使用指定を返します。
        [[nodiscard]] bool
            IsAsyncShaderCompilationEnabled() const noexcept;
        // 対象マテリアルが非同期準備中か返します(shaderPath: シェーダーのパス, keywords: 選択するキーワード)。
        [[nodiscard]] bool IsShaderCompiling(
            const std::filesystem::path& shaderPath,
            const ShaderKeywordSet& keywords = {}) const;

        // 宣言されたキーワード一覧をキャッシュして返します(shaderPath: シェーダーのパス)。
        [[nodiscard]] const ShaderVariantDeclaration&
            ShaderVariantsFor(
                const std::filesystem::path& shaderPath) const;

        // D3D11マテリアル効果を取得します(shaderPath: シェーダーのパス, generation: 世代番号の出力, error: 失敗理由の出力, keywords: 選択するキーワード)。
        // 非同期準備中は標準Lit、失敗時は用意できればマゼンタの代替効果を返します。
        [[nodiscard]] LitEffect& MaterialShader(
            const std::filesystem::path& shaderPath,
            std::uint64_t& generation,
            std::string& error,
            const ShaderKeywordSet& keywords = {}) const;
        // 骨変形のD3D11マテリアル効果を取得します(shaderPath: シェーダーのパス, generation: 世代番号の出力, error: 失敗理由の出力, keywords: 選択するキーワード)。
        [[nodiscard]] LitEffect* SkinnedMaterialShader(
            const std::filesystem::path& shaderPath,
            std::uint64_t& generation,
            std::string& error,
            const ShaderKeywordSet& keywords = {}) const;
        // 次回のマテリアル再読込を要求します(shaderPath: 全キーワードの元パス)。
        void InvalidateMaterialShader(
            const std::filesystem::path& shaderPath) const;
        // D3D12の自作マテリアルで描画します(request: 描画条件, material: 素材・骨・追加パス, generation: 世代番号の出力, error: 失敗理由の出力)。
        // D3D12以外はfalseで、コンパイル失敗は用意できれば代替シェーダーで描きます。
        [[nodiscard]] bool DrawMaterialShaderPrimitive(
            const PrimitiveDrawRequest& request,
            const Detail::MaterialShaderDrawRequest& material,
            std::uint64_t& generation,
            std::string& error);
        // D3D12の準備済み描画状態を返します(shaderPath: シェーダーのパス, keywords: 選択するキーワード, state: 成功時の状態出力)。
        [[nodiscard]] bool TryGetMaterialShaderRenderState(
            const std::filesystem::path& shaderPath,
            const ShaderKeywordSet& keywords,
            ShaderRenderState& state) const;
        // D3D12の追加パスを準備して有無を返します(shaderPath: シェーダーのパス, keywords: 選択するキーワード)。
        [[nodiscard]] Detail::MaterialShaderPasses PrepareMaterialShaderPasses(
            const std::filesystem::path& shaderPath,
            const ShaderKeywordSet& keywords);
        // 次回のスプライト再読込を要求します(shaderPath: シェーダーのパス)。
        void InvalidateSpriteShader(
            const std::filesystem::path& shaderPath) const;
        // 自作画素シェーダーとb0を設定します(shaderPath: シェーダーのパス, customParameters: b0の8本の定数, generation: 世代番号の任意出力, error: 失敗理由の任意出力)。
        // t0以降の入力画像は呼び出し側で設定します。
        bool ApplyCustomPixelShader(
            const std::filesystem::path& shaderPath,
            const std::array<
                DirectX::XMFLOAT4,
                8>& customParameters,
            std::uint64_t* generation = nullptr,
            std::string* error = nullptr) const;
        // 次回の自作画素シェーダー再読込を要求します(shaderPath: シェーダーのパス)。
        void InvalidateCustomPixelShader(
            const std::filesystem::path& shaderPath) const;
        // 次の画面合成へ効果を登録できたか返します(request: 挿入位置・画像・定数, generation: 世代番号の任意出力, error: 失敗理由の任意出力)。
        // 同じ地点の効果は登録順で適用し、直前の正常版を使えた場合はerrorがあってもtrueです。
        bool QueueScreenEffect(
            const ScreenEffectRequest& request,
            std::uint64_t* generation = nullptr,
            std::string* error = nullptr);
        // 次回の画面効果再読込を要求します(shaderPath: シェーダーのパス)。
        void InvalidateScreenEffectShader(
            const std::filesystem::path& shaderPath) const;
        // 名前付き画像へ計算処理を実行できたか返します(request: 入出力画像と定数, error: 失敗理由の任意出力)。
        // 直前の正常版を実行できた場合はerrorがあってもtrueで、戻り値と失敗理由を別々に確認します。
        bool DispatchComputeEffect(
            const ComputeEffectRequest& request,
            std::string* error = nullptr);
        // 次回の計算効果再読込を要求します(shaderPath: シェーダーのパス)。
        void InvalidateComputeEffectShader(
            const std::filesystem::path& shaderPath) const;
        // 平行光用の影描画先を借用し、未初期化ならlogic_errorです。
        [[nodiscard]] ShadowMap& Shadows() const;
        // スポット光用の影描画先を借用し、未初期化ならlogic_errorです。
        [[nodiscard]] ShadowMap& SpotShadows() const;
        // 点光源用の影描画先を借用し、未初期化ならlogic_errorです。
        [[nodiscard]] ShadowMap& PointShadows() const;
        // 現在のライト情報をコピーします(lighting: 描画に使用するライト一覧)。
        void SetLightingState(const LightingState& lighting) noexcept;
        // 保存した現在のライト情報を参照します。
        [[nodiscard]] const LightingState& Lighting() const noexcept;
        // バックバッファの幅をピクセル単位で返します。
        [[nodiscard]] std::uint32_t Width() const noexcept;
        // バックバッファの高をピクセル単位で返します。
        [[nodiscard]] std::uint32_t Height() const noexcept;
        // UIの基準寸法を設定します(width: 幅で0は1, height: 高で0は1)。
        void SetUIViewportSize(
            std::uint32_t width,
            std::uint32_t height) noexcept;
        // UIの基準幅をピクセル単位で返します。
        [[nodiscard]] std::uint32_t
            UIWidth() const noexcept;
        // UIの基準高をピクセル単位で返します。
        [[nodiscard]] std::uint32_t
            UIHeight() const noexcept;
        // 2D描画の共通オフセットを設定します(offset: 画面上の移動量)。
        void SetSprite2DOffset(
            const DirectX::XMFLOAT2& offset) noexcept;
        // 2D描画の共通オフセットを参照します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            Sprite2DOffset() const noexcept;
        // 画面の幅を高さの下限1で割った縦横比を返します。
        [[nodiscard]] float AspectRatio() const noexcept;
        // 描画倍率を反映した画像幅を最小1で返します。
        [[nodiscard]] std::uint32_t
            RenderWidth() const noexcept;
        // 描画倍率を反映した画像高を最小1で返します。
        [[nodiscard]] std::uint32_t
            RenderHeight() const noexcept;

        // 名前で共有する描画先を取得・生成します(name: 登録名, width: 画像幅で0は1, height: 画像高で0は1)。
        RenderTarget& AcquireRenderTexture(
            const std::string& name,
            std::uint32_t width,
            std::uint32_t height);
        // 名前で共有する計算用描画先を取得・生成します(name: 通常描画先と別の登録名, width: 画像幅で0は1, height: 画像高で0は1)。
        RenderTarget& AcquireComputeTexture(
            const std::string& name,
            std::uint32_t width,
            std::uint32_t height);
        // 描画先を借用し、未登録ならnullptrです(name: 登録名)。
        [[nodiscard]] const RenderTarget* FindRenderTexture(
            const std::string& name) const noexcept;
        // 表示画像を返し、無効な世代なら空です(name: 描画先の登録名)。
        [[nodiscard]] GraphicsViewHandle
            RenderTextureViewHandle(
                const std::string& name) const noexcept;
        // 描画先の登録を解放したか返します(name: 登録名)。
        bool ReleaseRenderTexture(
            const std::string& name);
        // 登録した全ての描画先を解放します。
        void ClearRenderTextures() noexcept;
        // 登録した描画先の名前を名前順で返します。
        [[nodiscard]] std::vector<std::string>
            RenderTextureNames() const;

    // 旧Game ModuleへのAPI不一致案内のため、privateな互換入口のシンボルを維持します。
    private:
        friend class Application;
        friend class ModelRendererComponent;
        friend class MeshRendererComponent;
        friend class ParticleSystemComponent;
        friend class D3D12EditorModelPreviewRenderer;
        friend class SkeletalModel;
        friend class Detail::GraphicsDeviceD3D11Access;
        friend class Detail::GraphicsDeviceD3D12Access;
        friend struct Detail::GraphicsDeviceD3D11Resources;
        friend class Detail::SpriteRenderPassState;

        // 借用するD3D11デバイスを返し、他のAPIなら空を返す。
        [[nodiscard]] ID3D11Device* Device() const noexcept;
        // 借用するD3D11コンテキストを返し、他のAPIなら空を返す。
        [[nodiscard]] ID3D11DeviceContext* Context() const noexcept;
        // 共通の描画状態を借用し、未初期化なら例外を送出する。
        [[nodiscard]] DirectX::CommonStates& States() const;
        // アルファ保持の加算混合を遅延生成して借用し、API資源がなければ空を返す。
        [[nodiscard]] ID3D11BlendState*
            AdditiveBlendPreservingAlpha() const;
        // D3D11参照を借用し、解決失敗なら空を返す(view: 解決する中立参照)。
        [[nodiscard]] ID3D11ShaderResourceView*
            TryResolveD3D11ShaderResourceView(
                const GraphicsViewHandle& view) const noexcept;
        // 中立参照を優先してD3D11参照を借用する(resources: 保持する資源のスナップショット)。
        // 非空の中立ハンドルを優先し、無効な世代から旧ポインターへ戻しません。
        [[nodiscard]] ID3D11ShaderResourceView*
            TryResolveD3D11ShaderResourceView(
                const TextureResourceSnapshot& resources)
                const noexcept;

        // D3D11環境描画器を遅延生成して返します。
        [[nodiscard]] EnvironmentRenderer& Environment() const;
        // 処理済みの色をバックバッファ全体へ転写します(target: 現在の資源世代の描画先)。
        void CopyOffscreenTargetToBackBuffer(
            const RenderTarget& target);

        // 組込資源の生成失敗を再試行まで保持します。
        struct BuiltInFailure final
        {
            // 前回の資源生成失敗文
            std::string message;
            // 最終試行の単調時計秒数
            double lastAttempt{};
        };

        // 組込資源を遅延生成します(slot: 資源の所有先, failure: 前回失敗と試行時刻, factory: 資源を返す生成関数)。
        // 失敗は記録して再送出し、2秒間は再コンパイルせず同じ例外を返します。
        template <typename T, typename Factory>
        T& BuildBuiltIn(
            std::unique_ptr<T>& slot,
            BuiltInFailure& failure,
            Factory&& factory) const;

        // 音声を含む全資源を停止して解放します。
        void Shutdown() noexcept;
        // 資源の借用受付を作ります(owner: デバイス寿命で無効化する所有元)。
        [[nodiscard]] static std::shared_ptr<
            Detail::GraphicsDeviceResourceLeaseState>
            CreateResourceLeaseState(GraphicsDevice* owner);
        // 借用中なら拒否し、新しいリースの受付を停止します。
        void BeginResourceTransition();
        // 資源移行を終了してリースの受付を再開します。
        void EndResourceTransition() noexcept;
        // 借用受付を閉じてパスを中止し、所有デバイスの参照を外します。
        void CloseResourceLeaseGate() noexcept;
        // API資源と実行サービスを借用する作業を停止します。
        void QuiesceResourceWork() noexcept;
        // 借用作業を停止して高レベル資源から解放します(preserveAudio: 再初期化用に音声を保持)。
        void ReleaseResources(bool preserveAudio) noexcept;
        // バックエンドと共有資源を生成します(window: 描画先のHWND, width: 初期画像幅で0は1, height: 初期画像高で0は1, requestedApi: 要求する描画API, profile: 互換用の起動指定)。
        // D3D12起動時のruntime_errorでは全資源を解放してからD3D11で再試行します。
        void InitializeResources(
            HWND window,
            std::uint32_t width,
            std::uint32_t height,
            RenderingApi requestedApi,
            GraphicsStartupProfile profile);
        // 稼働中の基盤に対応するAPI資源を生成する(activeApi: 稼働中の描画API)。
        void CreateApiResources(RenderingApi activeApi);
        // API固有資源を解放して所有元を空にする。
        void ResetApiResources() noexcept;
        // パス用シェーダーを準備して設定関数を返します(description: シェーダー・定数・照明, status: 使用状態の出力)。
        [[nodiscard]] std::function<void()>
            PrepareD3D11SpriteShader(
                const SpritePassDescription& description,
                SpriteShaderStatus& status);
        // 互換用のD3D11バッチを開始して貸し出します。
        DirectX::SpriteBatch& BeginSprites();
        // 旧画像描画の終了までテクスチャを保持する(resources: 保持する資源スナップショット)。
        [[nodiscard]] ID3D11ShaderResourceView*
            PinD3D11TextureForSpriteBatch(
                std::shared_ptr<const TextureResourceSnapshot>
                    resources);
        // 互換用のD3D11バッチを開始します(shaderPath: 自作シェーダーのパス, customParameters: b0の8本の定数, generation: 世代番号の任意出力, error: 失敗理由の任意出力, lighting: b1照明で空なら既定)。
        DirectX::SpriteBatch& BeginSprites(
            const std::filesystem::path& shaderPath,
            const std::array<DirectX::XMFLOAT4, 8>&
                customParameters,
            std::uint64_t* generation = nullptr,
            std::string* error = nullptr,
            const Sprite2DLighting* lighting = nullptr);
        // 互換用のD3D11バッチを送信して閉じます。
        void EndSprites();
        // 互換用バッチへクリップを積みます(minimumX: 左端の画素位置, minimumY: 上端の画素位置, maximumX: 右端の画素位置, maximumY: 下端の画素位置)。
        void PushUIScissor(
            float minimumX,
            float minimumY,
            float maximumX,
            float maximumY);
        // 互換用バッチを直前のクリップ範囲へ戻します。
        void PopUIScissor();
        // 描画テクスチャのD3D11参照を借用する(name: テクスチャ名)。
        [[nodiscard]] ID3D11ShaderResourceView*
            RenderTextureView(
                const std::string& name) const noexcept;
        // インスタンス情報を転送してD3D11バッファーを借用する(data: 転送元, bytes: 転送バイト数)。
        [[nodiscard]] ID3D11Buffer* AcquireInstanceBuffer(
            const void* data,
            std::size_t bytes);
        // 現在のD3D11ネイティブバッファーを借用する(buffer: 解決する中立バッファー)。
        [[nodiscard]] ID3D11Buffer* ResolveD3D11Buffer(
            const GraphicsBufferHandle& buffer) const;
        // 白テクスチャを借用し、参照の解決失敗なら空を返す。
        [[nodiscard]] ID3D11ShaderResourceView*
            WhiteTexture() const noexcept;
        // 現在のD3D11ネイティブ参照を借用する(view: 解決する中立参照)。
        [[nodiscard]] ID3D11ShaderResourceView*
            ResolveD3D11ShaderResourceView(
                const GraphicsViewHandle& view) const;
        // GI係数を転送して旧D3D11参照を保持する(width: 格子の横数, height: 格子の縦数, depth: 格子の奥行数, coefficients: 半精度の係数列)。
        [[nodiscard]] std::array<Microsoft::WRL::ComPtr<
            ID3D11ShaderResourceView>, 3>
            UploadBakedGlobalIllumination(
                std::uint32_t width,
                std::uint32_t height,
                std::uint32_t depth,
                std::span<const std::uint16_t> coefficients)
                    const noexcept;
        // D3D11用環境マップのキャッシュを読み込む(key: キャッシュ識別子)。
        [[nodiscard]] EnvironmentRenderer::OwnedPrefilteredEnvironment
            TryLoadCachedEnvironment(std::uint64_t key) const;
        // D3D11参照を現在の基盤へ取り込む(view: 保持するネイティブ参照)。
        [[nodiscard]] GraphicsViewHandle
            ImportD3D11ShaderResourceView(
                ID3D11ShaderResourceView* view);
        // 粒子の矩形を実効APIで描画します(request: 頂点・行列・画像, shaderPath: 自作シェーダーのパス, customParameters: b0の8本の定数, shaderGeneration: 世代番号の任意出力, shaderError: 失敗理由の任意出力)。
        [[nodiscard]] bool DrawParticles(
            const ParticleDrawRequest& request,
            const std::filesystem::path& shaderPath,
            const std::array<DirectX::XMFLOAT4, 8>& customParameters,
            std::uint64_t* shaderGeneration,
            std::string* shaderError);
        // 共有描画サービスで図形を描けたか返します(request: 頂点・行列・素材・照明)。
        [[nodiscard]] bool DrawPrimitive(
            const PrimitiveDrawRequest& request);
        // Forward+資源を遅延生成して返します。
        [[nodiscard]] ClusteredLights& Clusters() const;
        // スプライトの代替効果を返し、生成失敗ならnullptrです。
        [[nodiscard]] SpriteEffect*
            SpriteErrorPlaceholder() const;
        // D3D12の自作シェーダーで粒子を描画します(request: 頂点・行列・画像, shaderPath: 自作シェーダーのパス, customParameters: b0の8本の定数, shaderGeneration: 世代番号の任意出力, shaderError: 失敗理由の任意出力)。
        // 元シェーダーと代替を準備できなければfalseで、呼び出し側が既定の描画へ戻します。
        [[nodiscard]] bool DrawD3D12CustomParticles(
            const ParticleDrawRequest& request,
            const std::filesystem::path& shaderPath,
            const std::array<DirectX::XMFLOAT4, 8>& customParameters,
            std::uint64_t* shaderGeneration,
            std::string* shaderError);
        // D3D11パスを開始して番号を返します(description: 合成・シェーダー・定数, neutralOwner: 共通パスの所有形式, status: 使用状態の任意出力, generation: 世代番号の任意出力, error: 失敗文の任意出力)。
        [[nodiscard]] std::uint64_t BeginD3D11SpritePass(
            const SpritePassDescription& description,
            bool neutralOwner,
            SpriteShaderStatus* status,
            std::uint64_t* generation,
            std::string* error);
        // 有効な画像指定をパスへ積めたか返します(token: パス識別番号, request: 画像と位置・色・変形)。
        [[nodiscard]] bool DrawD3D11Sprite(
            std::uint64_t token,
            const SpriteDrawRequest& request);
        // 待機中の画像を送信してから検証済みのメッシュを描けたか返します(token: パス識別番号, request: 画像と頂点・索引)。
        [[nodiscard]] bool DrawD3D11SpriteMesh(
            std::uint64_t token,
            const SpriteMeshDrawRequest& request);
        // 共通パスへ有限の矩形を積めたか返します(token: パス識別番号, rectangle: ピクセル座標の矩形)。
        [[nodiscard]] bool PushD3D11SpriteScissor(
            std::uint64_t token,
            const SpriteClipRectangle& rectangle);
        // 共通パスを直前のクリップへ戻せたか返します(token: パス識別番号)。
        [[nodiscard]] bool PopD3D11SpriteScissor(
            std::uint64_t token);
        // 共通パスを送信して閉じます(token: パス識別番号)。
        void EndD3D11SpritePass(std::uint64_t token);
        // 終了を試み、例外を外へ出しません(token: 共通パスの識別番号)。
        void AbortD3D11SpritePass(
            std::uint64_t token) noexcept;
        // 使用中の共通・互換用パスを例外なしで終了します。
        void AbortActiveD3D11SpritePass() noexcept;
        // 現在のD3D11資源を借用し、不一致や未生成なら空を返す。
        [[nodiscard]] Detail::GraphicsDeviceD3D11Resources*
            TryD3D11ApiResources() const noexcept;
        // D3D11資源を借用し、未生成なら例外を送出する。
        [[nodiscard]] Detail::GraphicsDeviceD3D11Resources&
            RequireD3D11ApiResources();
        // D3D11資源を借用し、未生成なら例外を送出する。
        [[nodiscard]] const Detail::GraphicsDeviceD3D11Resources&
            RequireD3D11ApiResources() const;
        // 白い画像とSRVを両方生成してから公開します。
        void CreateWhiteTexture();
        // 指定地点の登録効果を順に適用して取り除きます(target: 処理する描画先, point: 後処理への挿入位置)。
        void ApplyQueuedScreenEffects(
            RenderTarget& target,
            ScreenEffectPoint point);


        // ソフトウェア描画の優先指定
        static bool s_preferWarpAdapter;
        // 検証レイヤーの有効化指定
        static bool s_enableDebugLayer;
        struct MaterialShaderEntry;
        struct SpriteShaderEntry;
        struct ScreenShaderEntry;
        struct QueuedScreenEffect;
        struct ComputeShaderEntry;

        // 状態の実体をSDK外へ隠し、Initialize・Resizeでも保持して返却参照の住所を維持します。
        struct State;
        // 住所を保持する共有状態
        const std::unique_ptr<State> m_state;
    };
}
