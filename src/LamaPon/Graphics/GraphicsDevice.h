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
        struct GraphicsDeviceD3D11Resources;
    }

    struct FrameStatistics final
    {
        float framesPerSecond{};
        float frameTimeMilliseconds{};
        float cpuTimeMilliseconds{};
        std::uint64_t totalFrames{};

        // 以降のフィールドは末尾へ追加します。FrameStatsは参照で返しますが、
        // 呼び出し側が値で受け取ると自分の定義の大きさで写すため、
        // 既存フィールドの位置が動かないことが前提です。

        // 壊れたシェーダーの代役をDrawへ渡した回数です。画素の色による
        // 判定では通常の画像を誤検知するため、描画時に直接数えます。
        // 0は、最後のリセット以降に代役を使用していないことを示します。
        //
        // ResetShaderFallbackDrawsで0に戻せます。CLIでは撮影対象の
        // 1フレームだけを測るため、直前にリセットします。
        std::uint32_t shaderFallbackDraws{};
    };

    // 約0.5秒ごとに更新するプロセスRAMとDXGIメモリ予算です。
    // dedicatedVideoMemoryBytesは搭載量、localVideoMemoryUsageBytesは
    // OSが現在このプロセスへ計上している使用量で、同じ値ではありません。
    struct GraphicsMemoryStatistics final
    {
        std::uint64_t processWorkingSetBytes{};
        std::uint64_t processPrivateBytes{};
        std::uint64_t systemPhysicalUsedBytes{};
        std::uint64_t systemPhysicalTotalBytes{};
        std::uint64_t dedicatedVideoMemoryBytes{};
        std::uint64_t sharedSystemMemoryBytes{};
        std::uint64_t localVideoMemoryUsageBytes{};
        std::uint64_t localVideoMemoryBudgetBytes{};
        std::uint64_t nonLocalVideoMemoryUsageBytes{};
        std::uint64_t nonLocalVideoMemoryBudgetBytes{};
        bool videoMemoryBudgetAvailable{};
    };

    class Application;
    class RuntimeServices;
    class AssetManager;
    class AudioSystem;
    class ClusteredLights;
    class DebugRenderer;
    class InputSystem;
    class LitEffect;
    class ModelRendererComponent;
    class ParticleSystemComponent;
    struct LitTextureRequest;
    struct ReflectionProbeEnvironment;
    struct ParticleDrawRequest;
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
    struct VolumetricLightFrame;
    struct TemporalAntiAliasingFrame;
    struct DepthOfFieldFrame;
    struct PostProcessFrame;

    // 深度だけを書くパスの種別です。
    //
    // Shadow: シャドウマップ。深度が多少ずれても影の形が変わるだけ
    //         なので、不透明なものはすべて描きます。
    // Prepass: メインビューの深度プリパス（SSAOをライティングより
    //          前に用意するため）。メインパスはLESS_EQUALで同じ深度を
    //          描き直すので、プリパスがメインパスより手前へ深度を
    //          書いてしまうとメインパスの描画が消えます。そのため
    //          「まったく同じ深度を書けないもの」（テッセレーションで
    //          頂点を動かすShader、宣言で半透明にしたShader）は
    //          レンダラー側で除外します。
    enum class DepthPassKind
    {
        None,
        Shadow,
        Prepass
    };

    // 画面エフェクトをポスト処理のどこへ差し込むか。
    //
    // 既定のAfterToneMappingでは、LDRへ変換した後に効果を適用します。
    // 前へ置くほど「まだHDRで、後ろのパスの材料になる」効果になります。
    // 例えば BeforeBloom へ置いた発光はBloomで滲みますが、
    // AfterToneMapping へ置いた発光は滲みません。
    //
    // 列挙値はRunPostProcessの呼び出し順と対応します。
    enum class ScreenEffectPoint : std::uint8_t
    {
        // 3D描画直後のHDR。TAAより前なので、この地点で追加した色も
        // 時間方向に均されます。
        BeforePostProcess,
        // 被写界深度・モーションブラーの後、Bloomの前。HDR。
        // Bloom対象の発光表現はこの地点へ配置します。
        BeforeBloom,
        // Bloomとレンズフレアの後、トーンマップの前。HDR。
        BeforeToneMapping,
        // トーンマップ後のLDR（既定。従来の位置）。
        AfterToneMapping
    };

    struct ScreenEffectRequest final
    {
        std::filesystem::path shader;
        std::array<std::filesystem::path, 2>
            auxiliaryTextures{};
        std::array<DirectX::XMFLOAT4, 8>
            customParameters{};
        // 既定は従来と同じ位置なので、指定しなければ挙動は変わりません。
        ScreenEffectPoint point{
            ScreenEffectPoint::AfterToneMapping };
    };

    // 自作Compute Shader（CSMain）を1回走らせる要求です。
    // 結果は名前付きテクスチャへ書かれるので、SpriteRendererや
    // UI Imageの「レンダーテクスチャ」に同じ名前を入れれば
    // そのまま表示できます。
    struct ComputeEffectRequest final
    {
        // assetsからの相対パス。CSMainを持つHLSL。
        std::filesystem::path shader;
        // 書き込み先の名前。無ければ作られます。
        std::string outputTexture;
        std::uint32_t outputWidth{ 512 };
        std::uint32_t outputHeight{ 512 };
        // t0/t1へ入る入力テクスチャ（assetsからの相対パス）。
        std::array<std::filesystem::path, 2> inputTextures{};
        std::array<DirectX::XMFLOAT4, 8>
            customParameters{};
    };

    class GraphicsDevice final
    {
    public:
        GraphicsDevice();
        ~GraphicsDevice();

        GraphicsDevice(const GraphicsDevice&) = delete;
        GraphicsDevice& operator=(const GraphicsDevice&) = delete;

        // このDeviceから作られたresourceを外部で保持する期間のleaseです。
        // Sceneは自動で保持します。独自renderer等も、再初期化を安全に
        // 拒否させる必要がある場合はresourceより長く保持してください。
        [[nodiscard]] GraphicsDeviceResourceLease
            AcquireResourceLease();

        void Initialize(HWND window, std::uint32_t width, std::uint32_t height);
        // 同じインスタンスの再初期化はGraphicsDevice自身が所有するGPU
        // 資源を作り直します。SceneやComponent等が外部に保持する旧Device
        // 資源は呼び出し前に破棄してください。実行中のAPI切り替えを提供する
        // ものではありません。
        void Initialize(
            HWND window,
            std::uint32_t width,
            std::uint32_t height,
            RenderingApi requestedApi);
        void Resize(std::uint32_t width, std::uint32_t height);

        // Initialize前に呼ぶと、GPUの代わりにWARP（CPUラスタライザ）
        // でデバイスを作成します。GPUのないVM・CI環境用です。
        // なお通常初期化でもGPU作成に失敗した場合はWARPへ自動
        // フォールバックします。
        //
        // 実体はLamaPonRuntime.dllの中に1つだけ置きます。ヘッダで
        // inline staticではEXE側とDLL側に別々の実体ができるため、
        // モジュールをまたいで共有する状態はcppに定義します。
        static void SetPreferWarpAdapter(
            const bool prefer) noexcept;

        // Initialize前に呼ぶと、Releaseビルドでも D3D11 の
        // デバッグレイヤーを有効にします（--d3ddebug）。
        //
        // 不正な描画状態をドライバーへ渡す前に診断し、WARPでの
        // プロセス終了を含む問題の原因を特定するために使います。
        // 常時有効にはしません。デバッグレイヤーは重く、開発者向けの
        // SDK部品が要るためです。
        static void SetEnableDebugLayer(
            const bool enable) noexcept;
        [[nodiscard]] static bool
            IsDebugLayerEnabled() noexcept;

        // バックバッファのピクセルをRGBA8で読み出します
        // （スクリーンショット・描画回帰テスト用）。EndFrameの
        // EndFrameのPresentより前に呼び出します。
        [[nodiscard]] std::vector<std::uint8_t>
            CaptureBackBuffer(
                std::uint32_t& width,
                std::uint32_t& height) const;

        void BeginFrame(const float clearColor[4]);
        // API非依存のSprite描画scopeです。passが生きている間はDeviceの
        // 再初期化をleaseで拒否し、destructorで描画終了を試みます。
        [[nodiscard]] SpriteRenderPass BeginSpritePass(
            const SpritePassDescription& description = {});
        // インスタンス描画用の共有ダイナミック頂点バッファを
        // 更新し、API非依存handleで返します。copyして保持できますが、
        // 再初期化前のhandleは新しいBackendではnative解決できません。
        [[nodiscard]] GraphicsBufferHandle AcquireInstanceBufferHandle(
            std::span<const std::byte> data);
        // handleのAPI固有実体を公開せず、vertex bufferとして
        // 指定slotへbindします。empty / stale handleは拒否します。
        void BindVertexBuffer(
            const GraphicsBufferHandle& buffer,
            std::uint32_t slot,
            std::uint32_t stride,
            std::uint32_t offset = 0);
        // API非依存viewをpixel shaderへbindします。empty / stale /
        // 異種viewはentryごとにfallbackへ倒し、範囲不正や未初期化では
        // pipeline stateを変更せずfalseを返します。
        [[nodiscard]] bool TryBindPixelShaderResources(
            std::uint32_t firstSlot,
            std::span<const GraphicsViewHandle> resources,
            const GraphicsViewHandle& fallback = {}) noexcept;
        // 深度だけを書くパスに切り替えます。レンダラーはライティングと
        // ピクセルシェーダーを省いた描画を行います。
        // 種別を分けているのは、メインビューの深度プリパスだけは
        // 「メインパスとまったく同じ深度を書けるもの」に限る必要が
        // あるためです（詳細はDepthPassKind）。
        void SetDepthPass(const DepthPassKind kind) noexcept
        {
            m_depthPass = kind;
        }
        [[nodiscard]] DepthPassKind
            DepthPass() const noexcept
        {
            return m_depthPass;
        }
        [[nodiscard]] bool IsDepthOnlyPass() const noexcept
        {
            return m_depthPass != DepthPassKind::None;
        }
        void EndFrame();
        void BeginSceneComposition(const float clearColor[4]);
        void EndSceneComposition(
            const BloomSettings& bloom,
            const ColorGradingSettings& colorGrading);
        void EndSceneComposition(
            const BloomSettings& bloom,
            const ScreenSpaceLensFlareSettings& lensFlare,
            const ColorGradingSettings& colorGrading);
        // volumetricは光の筋の指定です。シーン側しか知らない情報
        // （影のカスケードと平行光源）が要るので引数で受け取ります。
        // 旧APIのオーバーロードはレンズフレア／光の筋なしとして扱います。
        void EndSceneComposition(
            const BloomSettings& bloom,
            const ColorGradingSettings& colorGrading,
            const VolumetricLightFrame& volumetric,
            const TemporalAntiAliasingFrame& temporal);
        void EndSceneComposition(
            const BloomSettings& bloom,
            const ScreenSpaceLensFlareSettings& lensFlare,
            const ColorGradingSettings& colorGrading,
            const VolumetricLightFrame& volumetric,
            const TemporalAntiAliasingFrame& temporal);
        // 全ポスト処理情報を受け取る共通の入口です。簡易オーバーロードは
        // 不足値を既定値で補い、Scene::PostProcessFrameData()を渡します。
        void EndSceneComposition(
            const PostProcessFrame& frame);
        // ゲーム実行時に3Dを描くHDRターゲット。BeginSceneComposition
        // とEndSceneCompositionの間だけ有効です。深度プリパスを
        // 走らせるためにScene側へ渡します。
        [[nodiscard]] RenderTarget*
            SceneCompositionTarget() const noexcept
        {
            return m_sceneCompositionTarget.get();
        }
        // 直前に3Dを描いたときの射影行列。SSAOが深度をビュー空間へ
        // 戻すのに使います（Scene側の描画が毎回設定します）。
        void SetSceneProjection(
            const DirectX::XMFLOAT4X4& projection) noexcept
        {
            m_sceneProjection = projection;
        }
        [[nodiscard]] const DirectX::XMFLOAT4X4&
            SceneProjection() const noexcept
        {
            return m_sceneProjection;
        }
        void DrawLoadingScreen(
            float progress,
            const SceneLoadingScreenSettings& settings,
            std::uint32_t width = 0,
            std::uint32_t height = 0);
        void DrawStartupLogo(
            const std::filesystem::path& logoPath =
                L"textures/LamaPonEngineLogo.png",
            std::uint32_t width = 0,
            std::uint32_t height = 0);
        void SetGraphicsSettings(
            const GraphicsSettings& settings);
        void ApplyQualityPreset(
            GraphicsQualityPreset preset);
        [[nodiscard]] const GraphicsSettings&
            Settings() const noexcept
        {
            return m_graphicsSettings;
        }
        // 実際に生成されたBackendの描画APIです。要求された設定は
        // Settings().renderingApi に保持します。
        [[nodiscard]] RenderingApi
            ActiveRenderingApi() const noexcept;
        // Initialize時に選択された設定です。実行中の設定変更では
        // 書き換えず、再起動が必要かどうかの判定に使います。
        [[nodiscard]] RenderingApi
            StartupRenderingApi() const noexcept
        {
            return m_startupRenderingApi;
        }
        // 起動時の要求と生成されたBackendが異なる理由です。
        [[nodiscard]] RenderingApiFallbackReason
            RenderingApiFallback() const noexcept
        {
            return m_renderingApiFallbackReason;
        }
        [[nodiscard]] const FrameStatistics&
            FrameStats() const noexcept
        {
            return m_frameStatistics;
        }
        [[nodiscard]] const GraphicsMemoryStatistics&
            MemoryStats() const noexcept
        {
            return m_memoryStatistics;
        }
        // 通常はBeginFrameが低頻度で更新します。ベンチマークで
        // リソース確保直後を測る場合だけforce=trueを指定します。
        void RefreshMemoryStatistics(
            bool force = false) noexcept;
        void RecordFrameStatistics(
            float frameTimeSeconds,
            float cpuTimeMilliseconds) noexcept;
        // 代役シェーダーの使用回数を0へ戻します。「この1フレームで
        // 代役が使われたか」を測りたいときに、そのフレームを描く
        // 直前で呼びます（FrameStatisticsのコメントを参照）。
        void ResetShaderFallbackDraws() noexcept
        {
            m_frameStatistics.shaderFallbackDraws = 0;
        }
        // falseの場合はVSyncを無効にしてもFPSがモニターの
        // リフレッシュレートを超えないため、統計へ表示します。
        [[nodiscard]] bool TearingAllowed() const noexcept;

        [[nodiscard]] bool IsInitialized() const noexcept;
        // API固有のDevice / Contextを呼び出し側へ渡さず、オフスクリーン
        // 描画先を操作するための共通境界です。RenderTargetの具象資源は
        // この境界の内側でactive Backendに対応するopaque stateへ
        // 生成・差し替えられます。
        void ResizeOffscreenTarget(
            RenderTarget& target,
            std::uint32_t width,
            std::uint32_t height);
        // targetを描画先へ設定してから、色と深度をclearColorで初期化
        // します。UIの基準サイズやGPU計測区間は変更しません。
        void BeginOffscreenTarget(
            RenderTarget& target,
            const float clearColor[4]);
        // clearせずにtargetを描画先へ戻します。ポスト処理後に補助表示を
        // 重ねる場合など、完成途中の内容を保ったまま再開するために使います。
        void BindOffscreenTarget(RenderTarget& target);
        // 完成画像を表示専用資源へ確定します。targetのunbindや
        // バックバッファへの復帰は行わず、呼び出し側の描画順を保ちます。
        void PublishOffscreenTarget(RenderTarget& target);
        // カラーを割り当てず、targetの深度だけを描画先にします。
        // 深度はclearせず、描画先の復元も行いません。
        void BindOffscreenTargetDepthOnly(RenderTarget& target);
        // 現在の深度をtarget内のshader-readableなコピーへ控えます。
        // 描画先のbind状態は変更しません。
        void CaptureOffscreenTargetDepth(RenderTarget& target);
        // 現在のHDRカラーと行列をSSRの次フレーム用履歴へ控えます。
        // 現在フレームが旧履歴を読み終えた後に呼んでください。
        void CaptureOffscreenTargetColorHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection);
        // TAAで解決した現在のカラーと、ずらし無しの行列を
        // 次フレーム用履歴へ控えます。
        void CaptureOffscreenTargetTemporalHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection);
        // ポスト処理のD3D11 rendererを公開せず、RenderTargetとneutral
        // frame入力をactive Backendの描画島へ渡す移行境界です。
        [[nodiscard]] bool ResolveOffscreenTargetAmbientOcclusion(
            RenderTarget& target,
            const AmbientOcclusionSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        void ApplyOffscreenTargetTemporalAntiAliasing(
            RenderTarget& target,
            const TemporalAntiAliasingSettings& settings,
            const TemporalAntiAliasingInputs& inputs);
        void ApplyOffscreenTargetVolumetricLight(
            RenderTarget& target,
            const VolumetricLightSettings& settings,
            const VolumetricLightInputs& inputs);
        void ApplyOffscreenTargetDepthOfField(
            RenderTarget& target,
            const DepthOfFieldSettings& settings,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t sampleCount);
        void ApplyOffscreenTargetMotionBlur(
            RenderTarget& target,
            const MotionBlurSettings& settings,
            const DirectX::XMFLOAT4X4& inverseViewProjection,
            const DirectX::XMFLOAT4X4& viewProjection,
            std::uint32_t sampleCount);
        void ApplyOffscreenTargetBloom(
            RenderTarget& target,
            const BloomSettings& settings);
        void ApplyOffscreenTargetScreenSpaceLensFlare(
            RenderTarget& target,
            const ScreenSpaceLensFlareSettings& settings);
        void ApplyOffscreenTargetToneMapping(
            RenderTarget& target,
            const ColorGradingSettings& settings);
        void ApplyOffscreenTargetScreenOutline(
            RenderTarget& target,
            const ScreenOutlineSettings& settings,
            const DirectX::XMFLOAT4X4& projection);
        void ApplyOffscreenTargetFXAA(RenderTarget& target);
        // トーンマップ前のHDRから画面全体の明るさを測り、露出への
        // 補正（段数）を返します。前フレームの非同期readback、CPU側の
        // 順応、現在フレームの測定と次回用転送をこの順で行います。
        [[nodiscard]] float UpdateOffscreenTargetAutoExposure(
            RenderTarget& target,
            const AutoExposureSettings& settings,
            float deltaSeconds);
        // 影マップの指定スライスへ描画を開始し、終了時に元の描画先へ
        // 戻します。無効な影、範囲外、再入、Begin前のEndは何もしません。
        void BeginShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t cascadeIndex);
        void EndShadowMap(ShadowMap& shadowMap);
        // Forward+のライトカリングを実効Backendへ委譲します。
        // width/heightは描画先のピクセルサイズです。
        void UpdateClusteredLights(
            LightingState& lighting,
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            std::uint32_t width,
            std::uint32_t height);
        // 一時描画の前にprimary output bindingを控え、同じ描画区間で
        // 復元します。tokenは取得元のGraphicsDeviceだけで使えます。
        [[nodiscard]] std::unique_ptr<GraphicsOutputState>
            CaptureOutputState();
        void RestoreOutputState(
            const GraphicsOutputState& state);

        // 既存のD3D11描画コード向け互換facadeです。Backend共通interfaceへ
        // D3D11型を持ち込まず、段階的なrenderer移行までここで転送します。
        [[nodiscard]] ID3D11Device* Device() const noexcept;
        [[nodiscard]] ID3D11DeviceContext* Context() const noexcept;
        // 値で返し、再初期化で内部handleが差し替わっても取得時点のresource
        // snapshotを安全に保持できるようにします。
        [[nodiscard]] GraphicsTextureHandle
            WhiteTextureHandle() const noexcept
        {
            return m_whiteTexture;
        }
        [[nodiscard]] GraphicsViewHandle
            WhiteTextureViewHandle() const noexcept
        {
            return m_whiteTextureView;
        }
        // 描画時の互換経路です。empty/stale/別Backendのhandleは例外を
        // 外へ出さずnullptrへ倒し、旧Device resourceのbindを防ぎます。
        [[nodiscard]] ID3D11ShaderResourceView*
            TryResolveD3D11ShaderResourceView(
                const GraphicsViewHandle& view) const noexcept;
        // Texture assetの単一snapshotを解決します。neutral handleがある場合は
        // それを正とし、stale handleからlegacy raw pointerへはfallbackしません。
        [[nodiscard]] ID3D11ShaderResourceView*
            TryResolveD3D11ShaderResourceView(
                const TextureResourceSnapshot& resources)
                const noexcept;
        // API非依存resource操作をactive Backendへ転送します。
        [[nodiscard]] GraphicsTextureHandle CreateTexture2D(
            const GraphicsTexture2DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData);
        [[nodiscard]] GraphicsTextureHandle CreateTexture3D(
            const GraphicsTexture3DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData);
        void UpdateTexture2D(
            const GraphicsTextureHandle& texture,
            std::uint32_t mipLevel,
            const GraphicsTextureSubresourceData& data);
        [[nodiscard]] GraphicsViewHandle CreateShaderResourceView(
            const GraphicsTextureHandle& texture,
            const GraphicsTextureViewDescription& description);
        [[nodiscard]] bool IsGraphicsViewCurrent(
            const GraphicsViewHandle& view) const noexcept;
        [[nodiscard]] AssetManager& Assets() const;
        [[nodiscard]] AssetManager* TryAssets() const noexcept;
        [[nodiscard]] AudioSystem& Audio() const;
        [[nodiscard]] InputSystem& Input() const;
        [[nodiscard]] DirectX::CommonStates& States() const;
        // 宣言blend:additive用の純加算ブレンド（RGB: One+One）。
        // DirectXTKのAdditive（SrcAlpha加重）と違い、書き込み先の
        // アルファを一切汚さない（Alpha: Zero+One）。シーンバッファの
        // アルファは後段（被写界深度のCoC等）が意味を持って読むため、
        // 加算描画がdstA += srcAで積み上げると後段が黒く巻き込まれる。
        [[nodiscard]] ID3D11BlendState* AdditiveBlendPreservingAlpha() const;
        [[nodiscard]] DebugRenderer& Debug() const;
        // GPU区間計測（タイムスタンプクエリ）。フレームの
        // 開始/終了はBeginFrame/EndFrameが自動で行います。
        [[nodiscard]] GpuProfiler& Gpu() noexcept
        {
            return m_gpuProfiler;
        }
        // Sky cubemapをnative SRVへ公開せず、現在のBackend世代に
        // 属するsampleableなTextureCubeかを確認します。
        [[nodiscard]] bool IsSampleableCubeView(
            const GraphicsViewHandle& cubemap) const noexcept;
        // empty / stale / foreign / TextureCube以外のcubemapはグラデーション
        // Skyへ安全にフォールバックします。
        void DrawSky(
            DirectX::FXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const SkySettings& settings,
            const GraphicsViewHandle& cubemap = {},
            const SkySunDescription* sun = nullptr) const;
        // SSRのHi-Z深度ピラミッドを、RenderTargetのnative viewを
        // Sceneへ公開せずに生成します。
        [[nodiscard]] bool TryBuildReflectionDepthPyramid(
            RenderTarget& target,
            float projectionZ,
            float projectionW) const noexcept;
        // GI probeの6面描画とSH readbackをD3D11描画島に閉じ込めます。
        [[nodiscard]] std::optional<std::array<float, 12>>
            BakeIrradianceProbe(
                const EnvironmentProbeFaceRenderer& renderFace) const;
        // 共通Sky IBLをneutral viewへ変換する境界です。同じsource/key
        // ならhandleを再利用し、生成・取込のどこかで失敗した場合は
        // 片方だけを公開せず全emptyを返します。
        [[nodiscard]] PrefilteredEnvironmentViews
            TryGetPrefilteredEnvironmentViews(
                const GraphicsViewHandle& source,
                std::uint64_t cacheKey = 0) const noexcept;
        // Reflection Probeの共有ベイク資源と結果をneutral境界から
        // 扱います。Bake中のScene描画失敗は従来どおり呼び出し元へ
        // 伝え、cache missだけはempty resultとして安全に扱います。
        void PrepareEnvironmentProbeBake() const;
        [[nodiscard]] PrefilteredEnvironmentViews
            BakeReflectionProbeViews(
                const EnvironmentProbeFaceRenderer& renderFace,
                std::optional<std::uint64_t> cacheKey =
                    std::nullopt) const;
        [[nodiscard]] PrefilteredEnvironmentViews
            TryLoadCachedEnvironmentViews(
                std::uint64_t key) const noexcept;
        // [R, G, B] x probe x RGBAのfp16係数から、RGB別のRGBA16F
        // Texture3DをAPI非依存handleで3枚作ります。失敗時は全emptyです。
        [[nodiscard]] std::array<GraphicsViewHandle, 3>
            UploadBakedGlobalIlluminationViews(
                std::uint32_t width,
                std::uint32_t height,
                std::uint32_t depth,
                std::span<const std::uint16_t> coefficients)
                    const noexcept;
        // RendererがAPI固有SRVへ触れずにLitEffectへtextureを設定する
        // 移行用bridgeです。requestは直後の描画が終わるまで保持します。
        // 別DeviceのEffectまたは不正なnon-empty viewは、Effectを変更せず
        // falseで拒否します。empty viewは正常な未指定として扱います。
        [[nodiscard]] bool TrySetLitEffectTextures(
            LitEffect& effect,
            const LitTextureRequest& request) const noexcept;
        bool TrySetLitEffectTextures(
            LitEffect& effect,
            LitTextureRequest&& request) const = delete;
        bool TrySetLitEffectTextures(
            LitEffect& effect,
            const LitTextureRequest&& request) const = delete;
        // LightingState内のneutral viewを描画API固有viewへすべて
        // 解決・検証してからEffectへ一括反映します。lightingは直後の
        // 描画が終わるまで各viewを保持してください。不正な組み合わせや
        // 別DeviceのEffectでは何も変更せずfalseを返します。
        [[nodiscard]] bool TrySetLitEffectLighting(
            LitEffect& effect,
            const LightingState& lighting) const noexcept;
        bool TrySetLitEffectLighting(
            LitEffect& effect,
            LightingState&& lighting) const = delete;
        bool TrySetLitEffectLighting(
            LitEffect& effect,
            const LightingState&& lighting) const = delete;
        // オブジェクト単位のReflection Probeを同世代・正しいcube形状
        // へ全解決してから、LitEffectへprimary/secondaryを一括反映します。
        // probeまたは元Componentは直後のDraw完了までhandleを保持します。
        [[nodiscard]] bool TrySetLitEffectReflectionProbe(
            LitEffect& effect,
            const ReflectionProbeEnvironment& probe) const noexcept;
        bool TrySetLitEffectReflectionProbe(
            LitEffect& effect,
            ReflectionProbeEnvironment&& probe) const = delete;
        bool TrySetLitEffectReflectionProbe(
            LitEffect& effect,
            const ReflectionProbeEnvironment&& probe) const = delete;
        [[nodiscard]] LitEffect& Lit() const;
        // スキニングモデル（glTF/FBX）用のLamaPon Lit。
        // カスタムShader未指定のモデルはSkinnedLitで描画します。
        [[nodiscard]] LitEffect& SkinnedLit() const;
        // コンパイルできなかったシェーダーの代役（マゼンタ一色）。
        // 用意できないときはnullptr（このシェーダー自体が配られて
        // いない古いプロジェクトを開いた場合など）。
        [[nodiscard]] LitEffect* ShaderErrorPlaceholder(
            const bool skinned) const;
        // keywordsはバリアント（#pragma multi_compile）の選択です。
        // 組み合わせごとに別のLitEffectが作られ、それぞれ別々に
        // キャッシュされます。
        // エディターでのシェーダーの非同期コンパイル。入れると、
        // コンパイル中は標準Litで描いて処理を止めません。出来上がった次の
        // フレームで本来のシェーダーへ差し替わります。
        //
        // 書き出したゲーム（アーカイブ）では常に同期です。
        // アセットの読み取りがスレッド安全なのは素のファイルを
        // 読むときだけで、かつ配布物は全部事前コンパイル済みなので
        // 待ち時間がありません。
        void SetAsyncShaderCompilationEnabled(
            const bool enabled) noexcept
        {
            m_asyncShaderCompilation = enabled;
        }
        [[nodiscard]] bool
            IsAsyncShaderCompilationEnabled() const noexcept
        {
            return m_asyncShaderCompilation;
        }
        // そのマテリアルのシェーダーが今コンパイル中か
        // （Inspectorの表示用）。
        [[nodiscard]] bool IsShaderCompiling(
            const std::filesystem::path& shaderPath,
            const ShaderKeywordSet& keywords = {}) const;

        // そのシェーダーが宣言しているバリアント（#pragma
        // multi_compile / shader_feature）。Inspectorのキーワード
        // 一覧と描画時の正規化は、同じ宣言情報を使います。
        // 解釈結果はシェーダーごとにキャッシュします（毎フレーム読み直すと
        // ファイルI/Oが増えるため）。
        [[nodiscard]] const ShaderVariantDeclaration&
            ShaderVariantsFor(
                const std::filesystem::path& shaderPath) const;

        [[nodiscard]] LitEffect& MaterialShader(
            const std::filesystem::path& shaderPath,
            std::uint64_t& generation,
            std::string& error,
            const ShaderKeywordSet& keywords = {}) const;
        [[nodiscard]] LitEffect* SkinnedMaterialShader(
            const std::filesystem::path& shaderPath,
            std::uint64_t& generation,
            std::string& error,
            const ShaderKeywordSet& keywords = {}) const;
        void InvalidateMaterialShader(
            const std::filesystem::path& shaderPath) const;
        void InvalidateSpriteShader(
            const std::filesystem::path& shaderPath) const;
        // Sprite/Particle 共通のピクセルシェーダー経路です。
        // t0以降のテクスチャは呼び出し側が設定し、b0へ8個のfloat4を渡します。
        bool ApplyCustomPixelShader(
            const std::filesystem::path& shaderPath,
            const std::array<
                DirectX::XMFLOAT4,
                8>& customParameters,
            std::uint64_t* generation = nullptr,
            std::string* error = nullptr) const;
        void InvalidateCustomPixelShader(
            const std::filesystem::path& shaderPath) const;
        // 次回の3D画面合成で実行するポストエフェクトを末尾へ追加します。
        // 複数回呼ぶと、登録順に前の結果を次の入力として処理します。
        bool QueueScreenEffect(
            const ScreenEffectRequest& request,
            std::uint64_t* generation = nullptr,
            std::string* error = nullptr);
        void InvalidateScreenEffectShader(
            const std::filesystem::path& shaderPath) const;
        // 自作Compute Shaderをその場で1回走らせます。ScreenEffectと
        // 違って積まずに即実行するのは、書き込み先が画面ではなく
        // 名前付きテクスチャで、ポスト処理の並びと無関係なためです。
        // 失敗したらfalseを返し、errorへ理由が入ります（コンパイル
        // エラーでも直前の正常なシェーダーは保持します）。
        bool DispatchComputeEffect(
            const ComputeEffectRequest& request,
            std::string* error = nullptr);
        void InvalidateComputeEffectShader(
            const std::filesystem::path& shaderPath) const;
        [[nodiscard]] ShadowMap& Shadows() const;
        [[nodiscard]] ShadowMap& SpotShadows() const;
        [[nodiscard]] ShadowMap& PointShadows() const;
        void SetLightingState(const LightingState& lighting) noexcept
        {
            m_lightingState = lighting;
        }
        [[nodiscard]] const LightingState& Lighting() const noexcept
        {
            return m_lightingState;
        }
        [[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }
        [[nodiscard]] std::uint32_t Height() const noexcept { return m_height; }
        void SetUIViewportSize(
            std::uint32_t width,
            std::uint32_t height) noexcept
        {
            m_uiWidth = width == 0 ? 1 : width;
            m_uiHeight = height == 0 ? 1 : height;
        }
        [[nodiscard]] std::uint32_t
            UIWidth() const noexcept
        {
            return m_uiWidth;
        }
        [[nodiscard]] std::uint32_t
            UIHeight() const noexcept
        {
            return m_uiHeight;
        }
        void SetSprite2DOffset(
            const DirectX::XMFLOAT2& offset) noexcept;
        [[nodiscard]] const DirectX::XMFLOAT2&
            Sprite2DOffset() const noexcept;
        [[nodiscard]] float AspectRatio() const noexcept;
        [[nodiscard]] std::uint32_t
            RenderWidth() const noexcept;
        [[nodiscard]] std::uint32_t
            RenderHeight() const noexcept;

        // 名前付きレンダーテクスチャ（Cameraの描画先）。ミニマップ、
        // 防犯カメラ、キャラクターアイコンなど「画面の中に別の
        // カメラの絵を出す」用途に使います。名前で引けるので、
        // SpriteRendererやUI Imageから参照できます。
        // 同じ名前で違うサイズを要求すると作り直します。
        RenderTarget& AcquireRenderTexture(
            const std::string& name,
            std::uint32_t width,
            std::uint32_t height);
        // Compute Shaderの書き込み先。同じ登録簿に入るので、
        // SpriteRendererやUI Imageから同じ名前で表示できます。
        RenderTarget& AcquireComputeTexture(
            const std::string& name,
            std::uint32_t width,
            std::uint32_t height);
        [[nodiscard]] const RenderTarget* FindRenderTexture(
            const std::string& name) const noexcept;
        // 表示用resourceを強所有するAPI非依存handleです。未作成ならempty。
        [[nodiscard]] GraphicsViewHandle
            RenderTextureViewHandle(
                const std::string& name) const noexcept;
        bool ReleaseRenderTexture(
            const std::string& name);
        // Scene切り替えで作り直すため、まとめて解放します。
        void ClearRenderTextures() noexcept;
        [[nodiscard]] std::vector<std::string>
            RenderTextureNames() const;

    private:
        friend class Application;
        friend class ModelRendererComponent;
        friend class ParticleSystemComponent;
        friend class SkeletalModel;
        friend class Detail::SpriteRenderPassState;

        // API 60以前のraw renderer取得口はbinary互換shimとして残し、
        // engine内部のneutral facadeだけが使用します。
        [[nodiscard]] EnvironmentRenderer& Environment() const;
        // Scene compositionの現在カラーを既定出力へ転送します。native
        // view解決とD3D11 copy shaderは実装側の描画島に閉じ込めます。
        void CopyOffscreenTargetToBackBuffer(
            const RenderTarget& target);

        // 組み込みシェーダーの組み立てに失敗した時刻とエラーを保持します。
        struct BuiltInFailure final
        {
            std::string message;
            // 最後に試した時刻（steady_clock、秒）。
            double lastAttempt{};
        };

        // 組み込みシェーダーの失敗を一定時間保持し、毎フレームの
        // 再コンパイルを防ぎます。再試行に成功すると通常状態へ復帰します。
        template <typename T, typename Factory>
        T& BuildBuiltIn(
            std::unique_ptr<T>& slot,
            BuiltInFailure& failure,
            Factory&& factory) const;

        void Shutdown() noexcept;
        [[nodiscard]] static std::shared_ptr<
            Detail::GraphicsDeviceResourceLeaseState>
            CreateResourceLeaseState(GraphicsDevice* owner);
        void BeginResourceTransition();
        void EndResourceTransition() noexcept;
        void CloseResourceLeaseGate() noexcept;
        void QuiesceResourceWork() noexcept;
        void ReleaseResources(bool preserveAudio) noexcept;
        void InitializeResources(
            HWND window,
            std::uint32_t width,
            std::uint32_t height,
            RenderingApi requestedApi);
        void CreateApiResources(RenderingApi activeApi);
        void ResetApiResources() noexcept;
        [[nodiscard]] std::function<void()>
            PrepareD3D11SpriteShader(
                const SpritePassDescription& description,
                SpriteShaderStatus& status);
        // API 41のGame ModuleをLoadLibraryして明確なAPI不一致を
        // 案内するためだけにbinary symbolを1互換期間残すprivate
        // shimです。新規コードはBeginSpritePassを使用します。
        DirectX::SpriteBatch& BeginSprites();
        [[nodiscard]] ID3D11ShaderResourceView*
            PinD3D11TextureForSpriteBatch(
                std::shared_ptr<const TextureResourceSnapshot>
                    resources);
        DirectX::SpriteBatch& BeginSprites(
            const std::filesystem::path& shaderPath,
            const std::array<DirectX::XMFLOAT4, 8>&
                customParameters,
            std::uint64_t* generation = nullptr,
            std::string* error = nullptr,
            const Sprite2DLighting* lighting = nullptr);
        void EndSprites();
        void PushUIScissor(
            float minimumX,
            float minimumY,
            float maximumX,
            float maximumY);
        void PopUIScissor();
        // API 42のGame Moduleが旧公開名を解決してからAPI不一致を
        // 案内できるよう、binary symbolだけを1互換期間残す
        // private shimです。新規コードはRenderTextureViewHandleを
        // 使用します。
        [[nodiscard]] ID3D11ShaderResourceView*
            RenderTextureView(
                const std::string& name) const noexcept;
        // API 43のGame Moduleが旧公開名を解決してからAPI不一致を
        // 案内できるよう、raw buffer入口のシンボルだけを
        // 1互換期間残すprivate shimです。
        [[nodiscard]] ID3D11Buffer* AcquireInstanceBuffer(
            const void* data,
            std::size_t bytes);
        [[nodiscard]] ID3D11Buffer* ResolveD3D11Buffer(
            const GraphicsBufferHandle& buffer) const;
        // API 44のGame Moduleが旧公開名を解決してからAPI不一致を
        // 案内できるよう、raw white texture viewのシンボルだけを
        // 1互換期間残すprivate shimです。
        [[nodiscard]] ID3D11ShaderResourceView*
            WhiteTexture() const noexcept;
        // API 45のGame Moduleが旧公開名を解決してからAPI不一致を
        // 案内できるよう、throwing raw view resolverのシンボルだけを
        // 1互換期間残すprivate shimです。
        [[nodiscard]] ID3D11ShaderResourceView*
            ResolveD3D11ShaderResourceView(
                const GraphicsViewHandle& view) const;
        // API 52のGame Moduleが旧公開名を解決してからAPI不一致を
        // 案内できるよう、raw Baked GI uploadのシンボルだけを
        // 1互換期間残すprivate shimです。
        [[nodiscard]] std::array<Microsoft::WRL::ComPtr<
            ID3D11ShaderResourceView>, 3>
            UploadBakedGlobalIllumination(
                std::uint32_t width,
                std::uint32_t height,
                std::uint32_t depth,
                std::span<const std::uint16_t> coefficients)
                    const noexcept;
        // API 57のGame Moduleが旧raw cache復元名を解決してからAPI
        // 不一致を案内できるよう、binary symbolだけを残すprivate shim。
        [[nodiscard]] EnvironmentRenderer::OwnedPrefilteredEnvironment
            TryLoadCachedEnvironment(std::uint64_t key) const;
        // 移行途中のnative D3D11 viewを、共通描画経路が保持できる
        // Backend世代付きhandleへ変換するprivate shimです。
        // nullは正常な未設定としてempty handleを返します。
        [[nodiscard]] GraphicsViewHandle
            ImportD3D11ShaderResourceView(
                ID3D11ShaderResourceView* view);
        // ParticleSystemが生成したquadを実効APIの共有serviceへ送ります。
        // custom shader cacheはGraphicsDeviceに残し、Componentへ
        // Device / Context / Effectを公開しません。
        [[nodiscard]] bool DrawParticles(
            const ParticleDrawRequest& request,
            const std::filesystem::path& shaderPath,
            const std::array<DirectX::XMFLOAT4, 8>& customParameters,
            std::uint64_t* shaderGeneration,
            std::string* shaderError);
        // API 46のGame Moduleが旧公開名を解決してからAPI不一致を
        // 案内できるよう、外部callerのないD3D11具象facadeも
        // 1互換期間だけprivate shimとして残します。
        [[nodiscard]] ClusteredLights& Clusters() const;
        [[nodiscard]] SpriteEffect*
            SpriteErrorPlaceholder() const;
        [[nodiscard]] std::uint64_t BeginD3D11SpritePass(
            const SpritePassDescription& description,
            bool neutralOwner,
            SpriteShaderStatus* status,
            std::uint64_t* generation,
            std::string* error);
        [[nodiscard]] bool DrawD3D11Sprite(
            std::uint64_t token,
            const SpriteDrawRequest& request);
        [[nodiscard]] bool PushD3D11SpriteScissor(
            std::uint64_t token,
            const SpriteClipRectangle& rectangle);
        [[nodiscard]] bool PopD3D11SpriteScissor(
            std::uint64_t token);
        void EndD3D11SpritePass(std::uint64_t token);
        void AbortD3D11SpritePass(
            std::uint64_t token) noexcept;
        void AbortActiveD3D11SpritePass() noexcept;
        [[nodiscard]] Detail::GraphicsDeviceD3D11Resources*
            TryD3D11ApiResources() const noexcept;
        [[nodiscard]] Detail::GraphicsDeviceD3D11Resources&
            RequireD3D11ApiResources();
        [[nodiscard]] const Detail::GraphicsDeviceD3D11Resources&
            RequireD3D11ApiResources() const;
        void CreateWhiteTexture();
        // 積まれた画面エフェクトを順に適用して待ち行列を空にします。
        // ポスト処理の並びはRunPostProcessが持っているので、その
        // トーンマップ後のフックから画面エフェクトを適用します。
        // その地点に指定された画面エフェクトだけをかけ、かけた分を
        // 待ち行列から取り除きます。4地点すべてを通ると空になります。
        void ApplyQueuedScreenEffects(
            RenderTarget& target,
            ScreenEffectPoint point);

        // 外部resource ownerの数と再初期化中フラグを1つの同期境界で
        // 管理します。lease側もstateを共有するため、破棄順を誤っても
        // leaseの後始末自体が解放済みGraphicsDeviceへ触れません。
        std::shared_ptr<
            Detail::GraphicsDeviceResourceLeaseState>
            m_resourceLeaseState;

        // Device / Context / SwapChainとバックバッファ資源の所有者です。
        // 現在はD3D11Backendだけを生成し、D3D12はrenderer移行完了まで
        // 選択段階で安全にD3D11へフォールバックします。
        std::unique_ptr<GraphicsBackend> m_backend;
        // SpriteBatchや固定機能state等のAPI固有資源はprivateなopaque
        // stateへ集約し、Backendより先に解放します。
        std::unique_ptr<Detail::GraphicsDeviceApiResources>
            m_apiResources;
        GraphicsTextureHandle m_whiteTexture;
        GraphicsViewHandle m_whiteTextureView;
        GraphicsBufferHandle m_instanceBuffer;
        DepthPassKind m_depthPass{ DepthPassKind::None };
        GpuProfiler m_gpuProfiler;
        // WARP強制フラグ（Initialize前にテスト等から設定）。
        // 定義はGraphicsDevice.cpp（DLLの中に1つだけ）。
        static bool s_preferWarpAdapter;
        static bool s_enableDebugLayer;

        // 既存のAssets/Audio/Input APIを保ち、サービスの寿命管理は
        // 専用の所有者へ委譲します。D3Dデバイスより先に終了します。
        std::unique_ptr<RuntimeServices> m_services;
        std::unique_ptr<DebugRenderer> m_debugRenderer;
        mutable std::unique_ptr<EnvironmentRenderer>
            m_environmentRenderer;
        mutable std::unique_ptr<ClusteredLights>
            m_clusteredLights;
        std::unique_ptr<RenderTarget> m_sceneCompositionTarget;
        DirectX::XMFLOAT4X4 m_sceneProjection{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        // 名前付きレンダーテクスチャ。RenderTargetはコピー禁止
        // なのでunique_ptrで保持します。
        std::unordered_map<
            std::string,
            std::unique_ptr<RenderTarget>>
            m_renderTextures;
        mutable std::unique_ptr<LitEffect> m_litEffect;
        mutable std::unique_ptr<LitEffect> m_skinnedLitEffect;
        // コンパイル失敗時の代替シェーダーです。作成に失敗した場合は
        // 毎フレームの再試行による負荷を避けるため再生成しません。
        mutable std::unique_ptr<LitEffect> m_errorEffect;
        mutable std::unique_ptr<LitEffect> m_skinnedErrorEffect;
        mutable std::unique_ptr<SpriteEffect> m_spriteErrorEffect;
        mutable bool m_errorEffectUnavailable{};
        mutable bool m_skinnedErrorEffectUnavailable{};
        mutable bool m_spriteErrorEffectUnavailable{};
        // 組み込みシェーダーには代替描画経路が無いため、失敗を記録して
        // 毎フレームの再コンパイルを防ぎます。BuiltInRetrySecondsごとに
        // 再試行し、成功すると記録を解除します。
        mutable BuiltInFailure m_litFailure;
        mutable BuiltInFailure m_skinnedLitFailure;
        mutable BuiltInFailure m_environmentFailure;
        mutable BuiltInFailure m_clustersFailure;
        struct MaterialShaderEntry;
        struct SpriteShaderEntry;
        struct ScreenShaderEntry;
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<MaterialShaderEntry>>
            m_materialShaders;
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<MaterialShaderEntry>>
            m_skinnedMaterialShaders;
        // シェーダーごとのバリアント宣言。書き換えたら
        // InvalidateMaterialShaderが捨てます。
        mutable std::unordered_map<
            std::filesystem::path,
            ShaderVariantDeclaration>
            m_shaderVariants;
        bool m_asyncShaderCompilation{ true };
        mutable std::uint64_t
            m_materialShaderGeneration{};
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<SpriteShaderEntry>>
            m_spriteShaders;
        mutable std::uint64_t
            m_spriteShaderGeneration{};
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<ScreenShaderEntry>>
            m_screenShaders;
        mutable std::uint64_t
            m_screenShaderGeneration{};
        struct QueuedScreenEffect;
        std::vector<QueuedScreenEffect>
            m_queuedScreenEffects;
        struct ComputeShaderEntry;
        mutable std::unordered_map<
            std::filesystem::path,
            std::unique_ptr<ComputeShaderEntry>>
            m_computeShaders;
        std::unique_ptr<ShadowMap> m_shadowMap;
        std::unique_ptr<ShadowMap> m_spotShadowMap;
        std::unique_ptr<ShadowMap> m_pointShadowMap;
        LightingState m_lightingState;
        GraphicsSettings m_graphicsSettings =
            GraphicsSettingsForPreset(
                GraphicsQualityPreset::High);
        std::uint32_t m_width{};
        std::uint32_t m_height{};
        std::uint32_t m_uiWidth{};
        std::uint32_t m_uiHeight{};
        DirectX::XMFLOAT2 m_sprite2DOffset{};
        // mutable: shaderFallbackDrawsを数えるShaderErrorPlaceholder /
        // SpriteErrorPlaceholderがconstメソッドのためです（このクラスの
        // キャッシュ類と同じ扱い）。
        mutable FrameStatistics m_frameStatistics;
        GraphicsMemoryStatistics m_memoryStatistics;
        std::chrono::steady_clock::time_point
            m_lastMemoryStatisticsSample{};
        RenderingApi m_startupRenderingApi{
            RenderingApi::DirectX11 };
        RenderingApiFallbackReason m_renderingApiFallbackReason{
            RenderingApiFallbackReason::None };
    };
}
