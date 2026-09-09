#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace LamaPon
{
    class RenderTarget;

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
    };

    // activeApiはSelectGraphicsBackendで解決済みの値を渡します。
    // 現段階で生成できる具象BackendはDirectX 11だけです。
    [[nodiscard]] std::unique_ptr<GraphicsBackend>
        CreateGraphicsBackend(RenderingApi activeApi);
}
