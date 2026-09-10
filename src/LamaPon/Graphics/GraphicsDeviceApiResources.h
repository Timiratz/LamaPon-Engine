#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <d3d11.h>
#include <wrl/client.h>

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
    struct TextureResourceSnapshot;

    namespace Detail
    {
        // GraphicsDeviceの公開レイアウトから隔離するDirectX 11固有状態です。
        // GraphicsDeviceApiResourcesだけが所有し、Backendより先に破棄します。
        struct GraphicsDeviceD3D11Resources final
        {
            GraphicsDeviceD3D11Resources(
                ID3D11Device* device,
                ID3D11DeviceContext* context);
            ~GraphicsDeviceD3D11Resources();

            GraphicsDeviceD3D11Resources(
                const GraphicsDeviceD3D11Resources&) = delete;
            GraphicsDeviceD3D11Resources& operator=(
                const GraphicsDeviceD3D11Resources&) = delete;

            // 部分初期化の巻き戻しと通常終了の両方から安全に呼べます。
            void Reset() noexcept;

            std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
            std::vector<std::shared_ptr<
                const TextureResourceSnapshot>> spriteTexturePins;
            std::unique_ptr<DirectX::CommonStates> commonStates;
            mutable Microsoft::WRL::ComPtr<ID3D11BlendState>
                additiveBlendPreservingAlpha;
            Microsoft::WRL::ComPtr<ID3D11RasterizerState>
                uiScissorRasterizer;
            std::vector<D3D11_RECT> uiScissorStack;
        };

        // 将来の描画APIごとの状態を同じ所有境界へ追加するための
        // GraphicsDevice側のopaqueコンテナーです。
        class GraphicsDeviceApiResources final
        {
        public:
            GraphicsDeviceApiResources();
            ~GraphicsDeviceApiResources();

            GraphicsDeviceApiResources(
                const GraphicsDeviceApiResources&) = delete;
            GraphicsDeviceApiResources& operator=(
                const GraphicsDeviceApiResources&) = delete;

            void Reset() noexcept;

            std::unordered_map<std::string, GraphicsViewHandle>
                namedRenderTextureViews;
            std::unique_ptr<GraphicsDeviceD3D11Resources> d3d11;
        };

        [[nodiscard]] std::unique_ptr<GraphicsDeviceApiResources>
            CreateD3D11GraphicsDeviceApiResources(
                ID3D11Device* device,
                ID3D11DeviceContext* context);
    }
}
