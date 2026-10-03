#pragma once

#include "LamaPon/Graphics/GraphicsDevice.h"

namespace LamaPon::Detail
{
    class GraphicsDeviceD3D11Access final
    {
    public:
        // 現在のD3D11デバイスを借用し、他APIまたは不在ならヌルを返す(graphics: 描画デバイス)。
        [[nodiscard]] static ID3D11Device* Device(
            const GraphicsDevice& graphics) noexcept
        {
            return graphics.Device();
        }

        // 現在のD3D11コンテキストを借用し、他APIまたは不在ならヌルを返す(graphics: 描画デバイス)。
        [[nodiscard]] static ID3D11DeviceContext* Context(
            const GraphicsDevice& graphics) noexcept
        {
            return graphics.Context();
        }

        // 共通描画状態を借用し、D3D11資源がなければlogic_errorを送出する(graphics: 描画デバイス)。
        [[nodiscard]] static DirectX::CommonStates& States(
            const GraphicsDevice& graphics)
        {
            return graphics.States();
        }

        // アルファを維持する加算状態を借用し、D3D11資源がなければヌルを返す(graphics: 描画デバイス)。
        [[nodiscard]] static ID3D11BlendState*
            AdditiveBlendPreservingAlpha(
                const GraphicsDevice& graphics)
        {
            return graphics.AdditiveBlendPreservingAlpha();
        }

        // 同じ世代のSRVを借用し、空または解決失敗ならヌルを返す(graphics: 描画デバイス, view: 読込ビューのハンドル)。
        [[nodiscard]] static ID3D11ShaderResourceView*
            TryResolveD3D11ShaderResourceView(
                const GraphicsDevice& graphics,
                const GraphicsViewHandle& view) noexcept
        {
            return graphics.TryResolveD3D11ShaderResourceView(view);
        }

        // 同じ世代のSRVを借用し、無効ならヌルを返す(graphics: 描画デバイス, resources: テクスチャ資源の保持情報)。
        // 旧SRVへの代替は両ハンドルが空で、所有デバイスが一致するときだけ許可する。
        [[nodiscard]] static ID3D11ShaderResourceView*
            TryResolveD3D11ShaderResourceView(
                const GraphicsDevice& graphics,
                const TextureResourceSnapshot& resources) noexcept
        {
            return graphics.TryResolveD3D11ShaderResourceView(resources);
        }
    };
}
