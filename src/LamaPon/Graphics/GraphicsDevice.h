#pragma once

#include "LamaPon/Graphics/GpuProfiler.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/GraphicsQuality.h"
#include "LamaPon/Graphics/ShaderVariants.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
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
    struct FrameStatistics final
    {
        float framesPerSecond{};
        float frameTimeMilliseconds{};
        float cpuTimeMilliseconds{};
        std::uint64_t totalFrames{};

        // ---- 以降は末尾へ足すこと（FrameStatsは参照で返しますが、
        // 呼び出し側が値で受け取ると自分の定義の大きさで写すため、
        // 既存フィールドの位置が動かないことが前提です）。----

        // 壊れたシェーダーの代役（マゼンタ）が使われた回数。
        //
        // なぜ要るか: 撮った絵の「マゼンタっぽい画素数」を数えると、
        // 本当にピンクや水色を使った絵で誤検知します（実測: 正常な
        // シーンで画面の7.6%）。代役へ差し替えたかどうかはエンジン
        // 自身が知っている事実なので、推測せずここで数えます。
        // **0なら代役は一度も使われていない**と言い切れます。
        //
        // ResetShaderFallbackDrawsで0に戻せます。CLIは撮る1フレーム
        // だけを測るために、最後のフレームの手前で戻します。
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
    class EnvironmentRenderer;
    class InputSystem;
    class LitEffect;
    class SpriteEffect;
    class ShadowMap;
    class RenderTarget;
    class ScreenEffect;
    class ComputeEffect;
    struct BloomSettings;
    struct ScreenSpaceLensFlareSettings;
    struct ColorGradingSettings;
    struct Sprite2DLighting;
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
    // 既定は AfterToneMapping で、これは以前からの唯一の場所です。
    // 前へ置くほど「まだHDRで、後ろのパスの材料になる」効果になります。
    // 例えば BeforeBloom へ置いた発光はBloomで滲みますが、
    // AfterToneMapping へ置いた発光は滲みません。
    //
    // **並びの実体は RunPostProcess にしかありません。** 値を足すときは
    // あちらの呼び出し位置と一緒に増やしてください。
    enum class ScreenEffectPoint : std::uint8_t
    {
        // 3Dを描き終えた素のHDR。TAAより前なので、ここで足した色も
        // 時間方向に均されます。
        BeforePostProcess,
        // 被写界深度・モーションブラーの後、Bloomの前。HDR。
        // 「光らせたいもの」はここへ置きます。
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

        void Initialize(HWND window, std::uint32_t width, std::uint32_t height);
        void Resize(std::uint32_t width, std::uint32_t height);

        // Initialize前に呼ぶと、GPUの代わりにWARP（CPUラスタライザ）
        // でデバイスを作成します。GPUのないVM・CI環境用です。
        // なお通常初期化でもGPU作成に失敗した場合はWARPへ自動
        // フォールバックします。
        //
        // 実体はLamaPonRuntime.dllの中に1つだけ置きます。ヘッダで
        // `inline static`にすると、EXE側とDLL側で**別々の実体**に
        // なり、EXEが立てたフラグをDLLのInitializeが見ません
        // （2026-08-07にこれで--warpと--d3ddebugが無言で効いて
        // いませんでした）。設定する側と読む側がモジュールを
        // またぐ変数は、必ずcppへ置くこと。
        static void SetPreferWarpAdapter(
            const bool prefer) noexcept;

        // Initialize前に呼ぶと、Releaseビルドでも D3D11 の
        // デバッグレイヤーを有効にします（`--d3ddebug`）。
        //
        // なぜ要るか: 不正な描画状態はデバッグレイヤーが有効なら
        // 「読めるエラー」で分かりますが、無効だとそのままドライバー
        // へ渡ります。**WARPは不正な描画を弾かず自分の中で落ちる**
        // ので、Releaseだと何の手がかりも無くプロセスが消えます
        // （2026-08-07にテセレーションShaderで実際に起きました）。
        // 常時有効にはしません。デバッグレイヤーは重く、開発者向けの
        // SDK部品が要るためです。
        static void SetEnableDebugLayer(
            const bool enable) noexcept;
        [[nodiscard]] static bool
            IsDebugLayerEnabled() noexcept;

        // バックバッファのピクセルをRGBA8で読み出します
        // （スクリーンショット・描画回帰テスト用）。EndFrameの
        // Present前に呼んでください。
        [[nodiscard]] std::vector<std::uint8_t>
            CaptureBackBuffer(
                std::uint32_t& width,
                std::uint32_t& height) const;

        void BeginFrame(const float clearColor[4]);
        DirectX::SpriteBatch& BeginSprites();
        // lightingを渡すと、組み込みの2D照明Shaderが読む灯り一覧を
        // b1の専用バッファへ載せます。CustomParameters（8本しかなく、
        // 自作Shaderの持ち物）を使わないので16灯まで扱えます。
        // nullptrのときは空の一覧を載せます（前の描画の灯りが残って
        // 「消したのにまだ光る」状態になるのを防ぐため）。
        DirectX::SpriteBatch& BeginSprites(
            const std::filesystem::path& shaderPath,
            const std::array<DirectX::XMFLOAT4, 8>&
                customParameters,
            std::uint64_t* generation = nullptr,
            std::string* error = nullptr,
            const Sprite2DLighting* lighting = nullptr);
        void EndSprites();
        // 2D/UIスプライトパス中にクリッピング矩形を適用します
        // （UIピクセル座標）。入れ子は交差矩形になります。
        void PushUIScissor(
            float minimumX,
            float minimumY,
            float maximumX,
            float maximumY);
        void PopUIScissor();
        // インスタンス描画用の共有ダイナミック頂点バッファへ
        // データを書き込み、そのバッファを返します（スロット1用）。
        [[nodiscard]] ID3D11Buffer* AcquireInstanceBuffer(
            const void* data,
            std::size_t bytes);
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
        // 本来の入口。上のオーバーロードは足りないぶんを既定値で
        // 埋めてここへ流します。Scene::PostProcessFrameData()を
        // そのまま渡してください。
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
        // falseなら、VSyncを切ってもモニターのリフレッシュレートが
        // FPSの上限になります（「FPS上限を上げたのに数字が動かない」
        // の答えがここにあるので、エディターの統計へ出しています）。
        [[nodiscard]] bool TearingAllowed() const noexcept
        {
            return m_tearingAllowed;
        }

        [[nodiscard]] bool IsInitialized() const noexcept { return m_device != nullptr; }
        [[nodiscard]] ID3D11Device* Device() const noexcept { return m_device.Get(); }
        [[nodiscard]] ID3D11DeviceContext* Context() const noexcept { return m_context.Get(); }
        [[nodiscard]] ID3D11ShaderResourceView* WhiteTexture() const noexcept { return m_whiteTexture.Get(); }
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
        [[nodiscard]] EnvironmentRenderer& Environment() const;
        // クラスタライトカリング（Forward+）。初回アクセス時に
        // Compute Shaderをコンパイルして作ります。
        [[nodiscard]] ClusteredLights& Clusters() const;
        [[nodiscard]] LitEffect& Lit() const;
        // スキニングモデル（glTF/FBX）用のLamaPon Lit。
        // カスタムShader未指定のモデルはこれで描かれます。
        [[nodiscard]] LitEffect& SkinnedLit() const;
        // コンパイルできなかったシェーダーの代役（マゼンタ一色）。
        // 用意できないときはnullptr（このシェーダー自体が配られて
        // いない古いプロジェクトを開いた場合など）。
        [[nodiscard]] LitEffect* ShaderErrorPlaceholder(
            const bool skinned) const;
        // 2D（スプライト／UI／パーティクル）用の同じもの。
        [[nodiscard]] SpriteEffect*
            SpriteErrorPlaceholder() const;
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
        // 一覧と、描画時の正規化の両方がこれを使います。
        // 解釈結果はシェーダーごとに覚えます（毎フレーム読み直すと
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
        // 表示用SRV（描画完了後のコピー）。未作成ならnullptr。
        [[nodiscard]] ID3D11ShaderResourceView*
            RenderTextureView(
                const std::string& name) const noexcept;
        bool ReleaseRenderTexture(
            const std::string& name);
        // Scene切り替えで作り直すため、まとめて解放します。
        void ClearRenderTextures() noexcept;
        [[nodiscard]] std::vector<std::string>
            RenderTextureNames() const;

    private:
        friend class Application;

        // 組み込みシェーダー（Lit・Environment・LightCulling）を
        // 組み立てられなかったときの記録。詳しくは実体の側
        // （BuildBuiltIn）にあります。
        struct BuiltInFailure final
        {
            std::string message;
            // 最後に試した時刻（steady_clock、秒）。
            double lastAttempt{};
        };

        // 組み込みシェーダーを1回だけ組み立て、失敗を覚えます。
        // 毎フレーム作り直すと、壊れている間エディターが事実上
        // 止まるためです。一定時間ごとに試し直すので、直せば
        // そのまま復帰します。
        template <typename T, typename Factory>
        T& BuildBuiltIn(
            std::unique_ptr<T>& slot,
            BuiltInFailure& failure,
            Factory&& factory) const;

        void Shutdown() noexcept;
        void CreateSizeDependentResources();
        void CreateWhiteTexture();
        // 起動時に選ばれたアダプター名をログへ出します
        // （WARPかどうかが分かるように）。
        void LogSelectedAdapter() const;
        // 積まれた画面エフェクトを順に適用して待ち行列を空にします。
        // ポスト処理の並びはRunPostProcessが持っているので、その
        // トーンマップ後フックからここが呼ばれます。
        // その地点に指定された画面エフェクトだけをかけ、かけた分を
        // 待ち行列から取り除きます。4地点すべてを通ると空になります。
        void ApplyQueuedScreenEffects(
            RenderTarget& target,
            ScreenEffectPoint point);

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencilView;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_whiteTexture;
        std::unique_ptr<DirectX::SpriteBatch> m_spriteBatch;
        std::unique_ptr<DirectX::CommonStates> m_commonStates;
        mutable Microsoft::WRL::ComPtr<ID3D11BlendState>
            m_additiveBlendPreservingAlpha;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState>
            m_uiScissorRasterizer;
        std::vector<D3D11_RECT> m_uiScissorStack;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_instanceBuffer;
        std::size_t m_instanceBufferCapacity{};
        DepthPassKind m_depthPass{ DepthPassKind::None };
        // ティアリング許可（可変リフレッシュ／リフレッシュレート超え）。
        // これが無いと、VSyncを切ってもモニターのリフレッシュレートが
        // そのままFPSの上限になります。詳しくはInitializeを参照。
        bool m_tearingAllowed{};
        GpuProfiler m_gpuProfiler;
        // WARP強制フラグ（Initialize前にテスト等から設定）。
        // 定義はGraphicsDevice.cpp（DLLの中に1つだけ）。
        static bool s_preferWarpAdapter;
        static bool s_enableDebugLayer;

        // デバッグレイヤーが出したメッセージをログへ流します。
        // OutputDebugStringはデバッガーを繋いでいないと読めないため、
        // これが無いと --d3ddebug を付けても何も見えません。
        void DrainDebugMessages();
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> m_infoQueue;
        std::uint64_t m_debugMessagesLogged{};
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
        // コンパイル失敗の代役。作れなかったときは二度と試しません
        // （毎フレーム失敗を繰り返すと、壊れている間だけ極端に
        // 重くなるため）。
        mutable std::unique_ptr<LitEffect> m_errorEffect;
        mutable std::unique_ptr<LitEffect> m_skinnedErrorEffect;
        mutable std::unique_ptr<SpriteEffect> m_spriteErrorEffect;
        mutable bool m_errorEffectUnavailable{};
        mutable bool m_skinnedErrorEffectUnavailable{};
        mutable bool m_spriteErrorEffectUnavailable{};
        // 組み込みシェーダー（Lit・Environment・LightCulling）を
        // 組み立てられなかったときの記録です。ユーザーのShaderと違って
        // 代役を差し込める場所が無いので、投げること自体は変えられ
        // ません。**同じ失敗を毎フレーム作り直さないため**にここへ
        // 覚えます——大きなHLSLのコンパイルを毎フレーム走らせると、
        // 壊れている間エディターが事実上止まります。
        //
        // 直したら戻ってこられるよう、一定時間ごとに試し直します
        // （BuiltInRetrySeconds）。失敗はキャッシュに残らないので、
        // ファイルを直せば次の試行で通ります。
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
        D3D11_VIEWPORT m_viewport{};
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
    };
}
