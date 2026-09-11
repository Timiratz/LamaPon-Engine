#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"
#include "LamaPon/Graphics/GraphicsResource.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace DirectX
{
    struct XMFLOAT4X4;
}

namespace LamaPon
{
    class AssetManager;
    class ClusteredLights;
    class DebugDrawingBackend;
    class GpuProfilerBackend;
    class RenderTarget;
    class ShadowMap;
    struct LightingState;

    enum class RenderingApiFallbackReason
    {
        None,
        NotImplemented,
        Unsupported,
        InitializationFailed,
        UnknownApi
    };

    // 起動経路ごとの描画機能要件です。EditorやCLIのように完全な
    // rendererを使う経路は既定のFullRendererを選び、D3D12 bootstrapは
    // Gameの実験的なclear/present検証だけで明示的に許可します。
    enum class GraphicsStartupProfile : std::uint8_t
    {
        FullRenderer,
        AllowD3D12ExperimentalBootstrap
    };

    struct GraphicsBackendSelection final
    {
        RenderingApi requestedApi{
            RenderingApi::DirectX11 };
        RenderingApi activeApi{
            RenderingApi::DirectX11 };
        RenderingApiFallbackReason fallbackReason{
            RenderingApiFallbackReason::None };
    };

    // 設定値から、完全な描画機能を安全に起動できるBackendを選びます。
    // 既定では未実装のAPIを要求値のままDirectX 11へ倒します。
    [[nodiscard]] GraphicsBackendSelection
        SelectGraphicsBackend(
            RenderingApi requestedApi) noexcept;
    // D3D12のpresentation bootstrapを明示的に許可するGame起動だけで
    // DirectX12Experimentalを選びます。完全なrendererを必要とする
    // Editor / CLIはFullRendererを指定してD3D11 fallbackを保ちます。
    [[nodiscard]] GraphicsBackendSelection
        SelectGraphicsBackend(
            RenderingApi requestedApi,
            GraphicsStartupProfile profile) noexcept;

    struct GraphicsBackendCreateInfo final
    {
        // 公開する共通Backend契約へWindows/D3D型を持ち込まないため、
        // HWNDは呼び出し側でvoid*として渡します。
        void* nativeWindow{};
        std::uint32_t width{};
        std::uint32_t height{};
        bool preferWarpAdapter{};
        bool enableDebugLayer{};
    };

    // Backendが報告するAPI-neutralなGPUメモリ情報です。搭載量と
    // OS予算を分け、予算APIを利用できない環境ではavailable=falseで
    // usage/budgetを0のまま返します。
    struct GraphicsVideoMemoryStatistics final
    {
        std::uint64_t dedicatedBytes{};
        std::uint64_t sharedSystemBytes{};
        std::uint64_t localUsageBytes{};
        std::uint64_t localBudgetBytes{};
        std::uint64_t nonLocalUsageBytes{};
        std::uint64_t nonLocalBudgetBytes{};
        bool adapterAvailable{};
        bool descriptionAvailable{};
        bool localBudgetAvailable{};
        bool nonLocalBudgetAvailable{};
    };

    // 一時的に別の描画先へ切り替えた後、元のprimary output bindingへ
    // 戻すためのBackend固有tokenです。全pipeline stateではなく、色の
    // slot 0、深度、先頭viewportだけを保持します。
    class GraphicsOutputState
    {
    public:
        virtual ~GraphicsOutputState() = default;

        GraphicsOutputState(const GraphicsOutputState&) = delete;
        GraphicsOutputState& operator=(
            const GraphicsOutputState&) = delete;

    protected:
        GraphicsOutputState() = default;
    };

    // SwapChainを持つ描画Backendの最小共通契約です。DeviceやContext、
    // RenderTargetViewなどのAPI固有型は具象Backendだけが公開します。
    class GraphicsBackend
    {
    public:
        virtual ~GraphicsBackend() = default;

        GraphicsBackend() = default;
        GraphicsBackend(const GraphicsBackend&) = delete;
        GraphicsBackend& operator=(
            const GraphicsBackend&) = delete;

        [[nodiscard]] virtual RenderingApi
            Api() const noexcept = 0;
        [[nodiscard]] virtual bool
            IsInitialized() const noexcept = 0;

        virtual void Initialize(
            const GraphicsBackendCreateInfo& createInfo) = 0;

        // GraphicsDeviceが所有するGPU資源を破棄する前に呼びます。
        // Contextが保持する参照を外し、GPUへ残った命令を送ります。
        virtual void PrepareForResourceRelease() noexcept = 0;
        virtual void Shutdown() noexcept = 0;

        virtual void Resize(
            std::uint32_t width,
            std::uint32_t height) = 0;
        virtual void BindAndClearBackBuffer(
            const float clearColor[4]) = 0;
        virtual void DrainDebugMessages() = 0;
        virtual void Present(bool vSyncEnabled) = 0;

        [[nodiscard]] virtual std::vector<std::uint8_t>
            CaptureBackBuffer(
                std::uint32_t& width,
                std::uint32_t& height) const = 0;
        [[nodiscard]] virtual bool
            TearingAllowed() const noexcept = 0;

        // クリアせず、既定のバックバッファを描画先へ戻します。
        // 深度ターゲットは外し、バックバッファ用viewportを設定します。
        // Initializeが成功したBackendに対して呼びます。
        // 既存virtualのslotを維持するため、新しい契約は末尾へ追加します。
        virtual void BindBackBuffer() = 0;

        // API固有のDevice / Contextを呼び出し側へ渡さず、オフスクリーン
        // 描画先の基本操作を行います。RenderTargetの具象資源はopaqueな
        // Backend stateが所有し、この入口がBackend別stateの生成境界です。
        // targetの所有権は移さず、各呼び出しの間だけ参照します。
        // 既存virtualのslotを維持するため、新しい契約は末尾へ追加します。
        virtual void ResizeOffscreenTarget(
            RenderTarget& target,
            std::uint32_t width,
            std::uint32_t height) = 0;
        // targetを描画先へ設定し、色と深度をclearColorで初期化します。
        virtual void BeginOffscreenTarget(
            RenderTarget& target,
            const float clearColor[4]) = 0;
        // 内容を消さず、targetを描画先へ戻します。
        virtual void BindOffscreenTarget(
            RenderTarget& target) = 0;
        // 完成画像を表示専用資源へ確定します。描画先は変更しません。
        virtual void PublishOffscreenTarget(
            RenderTarget& target) = 0;
        // カラーを割り当てず、targetの深度だけを描画先にします。
        // 深度はclearせず、描画先の復元も行いません。
        // 既存virtualのslotを維持するため、新しい契約は末尾へ追加します。
        virtual void BindOffscreenTargetDepthOnly(
            RenderTarget& target) = 0;
        // 現在の深度をtarget内のshader-readableなコピーへ控えます。
        // 描画先のbind状態は変更しません。
        virtual void CaptureOffscreenTargetDepth(
            RenderTarget& target) = 0;
        // 現在のHDRカラーを、SSRが次フレームで読む履歴へ控えます。
        // viewProjectionは控えた画像を描いたときの行列です。
        // 描画先のbind状態は変更しません。
        virtual void CaptureOffscreenTargetColorHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) = 0;
        // 現在のTAA解決結果を次フレームの履歴へ控えます。
        // viewProjectionは再投影に使うずらし無しの行列です。
        // 描画先のbind状態は変更しません。
        virtual void CaptureOffscreenTargetTemporalHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) = 0;
        // 前フレームに控えた画面全体の平均輝度を、GPUを待たずに
        // 読みます。初回または転送が未完了ならnulloptを返します。
        // 値はGPU上の格納形式ではなく、線形空間の輝度です。
        [[nodiscard]] virtual std::optional<float>
            TryReadOffscreenTargetLuminance(
                RenderTarget& target) = 0;
        // 現在の輝度測定結果を、次フレームの非同期読み出し用に
        // 控えます。描画先のbind状態は変更しません。
        virtual void CaptureOffscreenTargetLuminance(
            RenderTarget& target) = 0;
        // 影用の深度配列（cube=trueでは6面のTextureCube）を作ります。
        virtual void InitializeShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t resolution,
            std::uint32_t cascadeCount,
            bool cube) = 0;
        // 指定した影スライスを描画先へ設定して深度をclearします。
        // 無効な影、範囲外のスライス、描画中の再入では何もしません。
        virtual void BeginShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t cascadeIndex) = 0;
        // BeginShadowMap前の描画先とviewportを復元します。
        // Begin前に呼ばれた場合は何もしません。
        virtual void EndShadowMap(
            ShadowMap& shadowMap) = 0;
        // Forward+用のライト一覧をGPUへ送り、クラスタごとの番号表を
        // lightingへ設定します。native資源はClusteredLightsのopaque
        // Backend stateに閉じ込め、共通契約へAPI固有型を出しません。
        virtual void UpdateClusteredLights(
            ClusteredLights& clusteredLights,
            LightingState& lighting,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t width,
            std::uint32_t height) = 0;
        // 現在のprimary output bindingを同じBackendで復元するtokenとして
        // 控えます。資源解放やResizeをまたがず、同じ描画区間で使います。
        // D3D12 Backendではgetterではなく論理bind状態から実装します。
        [[nodiscard]] virtual std::unique_ptr<GraphicsOutputState>
            CaptureOutputState() = 0;
        virtual void RestoreOutputState(
            const GraphicsOutputState& state) = 0;
        // Device型を公開せず、実効adapterの容量と現在のOS予算を
        // 取得します。性能表示用なので失敗時は空の値へ倒します。
        [[nodiscard]] virtual GraphicsVideoMemoryStatistics
            QueryVideoMemoryStatistics() const noexcept = 0;
        // Collider、grid、gizmo等が生成した共通線分を描くsinkです。
        // Backendが初期化済みの間だけ生成し、Backendより先に破棄します。
        [[nodiscard]] virtual std::unique_ptr<DebugDrawingBackend>
            CreateDebugDrawingBackend() = 0;
        // GPU計測driverはBackendが所有し、初期化済みの間だけ
        // facadeへ貸し出します。未初期化時はnullptrです。
        // 既存virtualのslotを維持するため、新しい契約は末尾へ追加します。
        [[nodiscard]] virtual GpuProfilerBackend*
            ProfilerBackend() noexcept = 0;

        // API非依存handleを返す最初のresource生成境界です。viewはtextureを
        // 強所有し、Backendを再初期化した後もhandleの破棄自体は安全です。
        // 既存virtualのslotを維持するため、新しい契約は末尾へ追加します。
        [[nodiscard]] virtual GraphicsTextureHandle
            CreateSolidRgba8Texture(
                const std::array<std::uint8_t, 4>& color) = 0;
        [[nodiscard]] virtual GraphicsViewHandle
            CreateShaderResourceView(
                const GraphicsTextureHandle& texture) = 0;

        // 共有の動的頂点bufferを必要容量へ拡張し、先頭からdataを書きます。
        // allocation / upload失敗時はbufferを変更せずfalseを返します。
        // 別Backend世代のhandleはprogrammer errorとしてinvalid_argumentで
        // 拒否します。
        [[nodiscard]] virtual bool UpdateDynamicVertexBuffer(
            GraphicsBufferHandle& buffer,
            std::span<const std::byte> data) = 0;

        // Asset側がAPI固有Deviceへ触れずに2D textureを生成・更新する境界です。
        // initialDataは空（PerMipUpdateのみ）か、全mip分を先頭から並べます。
        // Immutableは全mip必須かつ後続更新不可、PerMipUpdateは後から更新可能
        // というdescriptionの契約を各Backendで同じように守ります。
        // 生成はAsset準備workerからも呼べる必要があります。
        // UpdateTexture2Dはimmediate command submissionを伴うためrender
        // thread専用です。
        [[nodiscard]] virtual GraphicsTextureHandle CreateTexture2D(
            const GraphicsTexture2DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData) = 0;
        virtual void UpdateTexture2D(
            const GraphicsTextureHandle& texture,
            std::uint32_t mipLevel,
            const GraphicsTextureSubresourceData& data) = 0;
        [[nodiscard]] virtual GraphicsViewHandle
            CreateShaderResourceView(
                const GraphicsTextureHandle& texture,
                const GraphicsTextureViewDescription& description) = 0;

        // API 39で追加した互換slotです。現在はRenderTarget自身が表示
        // handleを所有するため、そのhandleをBackend世代検証して返します。
        // vtable互換のためslot自体は維持します。
        [[nodiscard]] virtual GraphicsViewHandle
            CreateOffscreenDisplayView(
                const RenderTarget& target) = 0;

        // API固有のbuffer実体を公開せず、現在の入力アセンブラへ
        // vertex bufferを1本bindします。既存virtualのslotを維持する
        // ため、新しい契約は末尾へ追加します。
        virtual void BindVertexBuffer(
            const GraphicsBufferHandle& buffer,
            std::uint32_t slot,
            std::uint32_t stride,
            std::uint32_t offset) = 0;

        // Shader resource viewをAPI固有pointerへ解決せず、pixel shaderの
        // register space 0にある連続したtNへbindします。emptyまたは無効なentryはfallbackへ
        // 置き換え、fallbackも無効ならnullをbindします。slot範囲が不正な
        // 場合だけ何も変更せずfalseを返します。既存virtualのslotを
        // 維持するため、新しい契約は末尾へ追加します。呼び出し側はrecord
        // した描画が終わるまでhandleを保持し、BackendはGPU完了まで必要な
        // native resource / descriptorの寿命を保証します。
        [[nodiscard]] virtual bool TryBindPixelShaderResources(
            std::uint32_t firstSlot,
            std::span<const GraphicsViewHandle> resources,
            const GraphicsViewHandle& fallback) noexcept = 0;

        // 3D volumeの全mipを初期dataから生成します。既存virtual slotを
        // 維持するため末尾へ追加します。
        [[nodiscard]] virtual GraphicsTextureHandle CreateTexture3D(
            const GraphicsTexture3DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData) = 0;

        // 長寿命Componentが保持するviewを、Backend再初期化後に安全に
        // 再生成できるよう、現在のresource domain所属かだけを調べます。
        // native pointerの解決やpipeline変更は行いません。既存virtual
        // slotを維持するため末尾へ追加します。
        [[nodiscard]] virtual bool IsViewCurrent(
            const GraphicsViewHandle& view) const noexcept = 0;

        // Forward+資源のnative表現を具象Backendへ閉じ込めます。既存の
        // virtual slotを維持するため末尾へ追加します。実装は全資源と
        // neutral viewを完成させてからownerへ一度に公開します。
        virtual void InitializeClusteredLights(
            ClusteredLights& clusteredLights,
            AssetManager& assets,
            const std::filesystem::path& shaderPath) = 0;
    };

    // 通常起動ではSelectGraphicsBackendで解決済みの値を渡します。
    // DirectX 12 Experimentalは段階実装中の内部Backendも生成できます。
    [[nodiscard]] std::unique_ptr<GraphicsBackend>
        CreateGraphicsBackend(RenderingApi activeApi);
}
