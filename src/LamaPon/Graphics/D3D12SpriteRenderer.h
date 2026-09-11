#pragma once

// DirectX 12 ExperimentalでSpriteRenderPassを描く既定pipelineです。D3D11の
// DirectXTK SpriteBatchと同じ座標・UV・blend規則で、Deferred順のquadを
// primary outputへ送ります。Runtime内部headerで、SDKにはinstallしません。
#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/SpriteRendering.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace LamaPon
{
    class D3D12Backend;
}

namespace LamaPon::Detail
{
    class D3D12SpriteRenderer final
    {
    public:
        explicit D3D12SpriteRenderer(D3D12Backend& backend);
        ~D3D12SpriteRenderer() noexcept;

        D3D12SpriteRenderer(const D3D12SpriteRenderer&) = delete;
        D3D12SpriteRenderer& operator=(
            const D3D12SpriteRenderer&) = delete;

        // passを開始してtokenを返します。custom pixel shaderはまだD3D12
        // pipelineを持たないため、既定pipelineで描いて理由をstatusへ返します。
        [[nodiscard]] std::uint64_t Begin(
            const SpritePassDescription& description,
            const GraphicsViewHandle& fallbackTexture,
            SpriteShaderStatus& status);
        [[nodiscard]] bool Draw(
            std::uint64_t token,
            const SpriteDrawRequest& request);
        [[nodiscard]] bool PushScissor(
            std::uint64_t token,
            const SpriteClipRectangle& rectangle);
        [[nodiscard]] bool PopScissor(std::uint64_t token);
        void End(std::uint64_t token);
        // DirectXTKのSpriteBatchと同じく、積んだSpriteを可能な範囲で描いて
        // passを閉じます。例外は外へ出しません。
        void Abort(std::uint64_t token) noexcept;

    private:
        struct Vertex final
        {
            DirectX::XMFLOAT3 position{};
            DirectX::XMFLOAT4 color{};
            DirectX::XMFLOAT2 textureCoordinate{};
        };

        struct QueuedSprite final
        {
            std::array<Vertex, 4> vertices{};
            D3D12_GPU_DESCRIPTOR_HANDLE texture{};
            // 描画を記録するまでviewを保持します。記録後はBackendの遅延
            // 解放がGPU完了まで実体とdescriptorを守ります。
            GraphicsViewHandle view;
        };

        void RequireOwner(std::uint64_t token) const;
        void Flush();
        // 描画送信に失敗したpassを閉じ、再初期化まで新しいpassを拒否します。
        void FlushOrFail();
        void ClearPass() noexcept;
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            SpriteBlendMode blend,
            bool scissored);

        D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        // blend modeごとに、通常passとscissor passのcull違いを持ちます。
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 8>
            m_pipelineStates;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
        GraphicsViewHandle m_fallbackTexture;
        std::vector<QueuedSprite> m_sprites;
        std::vector<D3D12_RECT> m_scissorStack;
        SpriteBlendMode m_blend{ SpriteBlendMode::NonPremultiplied };
        std::uint64_t m_activeToken{};
        std::uint64_t m_nextToken{ 1 };
        bool m_failed{};
    };
}
