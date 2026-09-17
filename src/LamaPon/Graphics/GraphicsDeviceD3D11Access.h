#pragma once

// D3D11 renderer実装と描画回帰テストだけが使うSDK非公開bridgeです。
// GraphicsDeviceの公開APIへnative Device / Context / Viewを戻さず、
// 段階移行中のD3D11描画島からprivate互換facadeへ到達させます。
#include "LamaPon/Graphics/GraphicsDevice.h"

namespace LamaPon::Detail
{
    class GraphicsDeviceD3D11Access final
    {
    public:
        [[nodiscard]] static ID3D11Device* Device(
            const GraphicsDevice& graphics) noexcept
        {
            return graphics.Device();
        }

        [[nodiscard]] static ID3D11DeviceContext* Context(
            const GraphicsDevice& graphics) noexcept
        {
            return graphics.Context();
        }

        [[nodiscard]] static DirectX::CommonStates& States(
            const GraphicsDevice& graphics)
        {
            return graphics.States();
        }

        [[nodiscard]] static ID3D11BlendState*
            AdditiveBlendPreservingAlpha(
                const GraphicsDevice& graphics)
        {
            return graphics.AdditiveBlendPreservingAlpha();
        }

        [[nodiscard]] static ID3D11ShaderResourceView*
            TryResolveD3D11ShaderResourceView(
                const GraphicsDevice& graphics,
                const GraphicsViewHandle& view) noexcept
        {
            return graphics.TryResolveD3D11ShaderResourceView(view);
        }

        [[nodiscard]] static ID3D11ShaderResourceView*
            TryResolveD3D11ShaderResourceView(
                const GraphicsDevice& graphics,
                const TextureResourceSnapshot& resources) noexcept
        {
            return graphics.TryResolveD3D11ShaderResourceView(resources);
        }
    };
}
