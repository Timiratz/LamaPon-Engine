#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace DirectX
{
    struct XMFLOAT4X4;
}

namespace LamaPon
{
    class ClusteredLights;
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

    struct GraphicsBackendSelection final
    {
        RenderingApi requestedApi{
            RenderingApi::DirectX11 };
        RenderingApi activeApi{
            RenderingApi::DirectX11 };
        RenderingApiFallbackReason fallbackReason{
            RenderingApiFallbackReason::None };
    };

    // 設定値から、現時点で安全に起動できるBackendを選びます。
    // 未実装のAPIは要求値を残したままDirectX 11へ倒します。
    [[nodiscard]] GraphicsBackendSelection
        SelectGraphicsBackend(
            RenderingApi requestedApi) noexcept;

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
        // 描画先の基本操作を行います。RenderTargetの資源表現は現時点では
        // D3D11のままで、将来Backend別の資源へ置き換えるための操作境界です。
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
        // lightingへ設定します。ClusteredLightsの資源表現は現時点では
        // D3D11のままで、将来Backend別の資源へ置き換えるための操作境界です。
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
    };

    // activeApiはSelectGraphicsBackendで解決済みの値を渡します。
    // 現段階で生成できる具象BackendはDirectX 11だけです。
    [[nodiscard]] std::unique_ptr<GraphicsBackend>
        CreateGraphicsBackend(RenderingApi activeApi);
}
