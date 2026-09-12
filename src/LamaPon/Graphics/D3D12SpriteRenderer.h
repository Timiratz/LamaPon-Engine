#pragma once

// DirectX 12 ExperimentalでSpriteRenderPassを描く既定pipelineです。D3D11の
// DirectXTK SpriteBatchと同じ座標・UV・blend規則で、Deferred順のquadを
// 現在のprimary / offscreen outputへ送ります。Runtime内部headerで、
// SDKにはinstallしません。
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
    class RenderTarget;
    struct BloomSettings;
    struct ColorGradingSettings;
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
        // HDR Scene textureを現在の出力全体へ転送し、指定されていれば
        // カラーグレーディングとACES近似を適用します。
        // GraphicsDeviceの最終合成専用です。
        void CompositeScene(
            const GraphicsViewHandle& texture,
            const GraphicsViewHandle& fallbackTexture,
            const ColorGradingSettings& colorGrading);
        // offscreen targetのcurrent colorから、D3D11のPSBloomと同じ9tapで
        // 高輝度部を滲ませてpost colorへ書き、両者を交換します。
        // GraphicsDeviceのpost-process専用です。
        void ApplyBloom(
            RenderTarget& target,
            const GraphicsViewHandle& fallbackTexture,
            const BloomSettings& settings);

    private:
        // quadを塗るpixel shaderです。None以外は出力全体へ1枚を描く
        // fullscreen pass用で、root constants（b1）の意味もshaderごとに
        // 異なります。
        enum class FullscreenProgram : std::uint8_t
        {
            None,
            ToneMap,
            Bloom
        };

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
        // 現在の出力全体へtextureを1枚、指定shaderで描きます。
        void DrawFullscreen(
            const GraphicsViewHandle& texture,
            const GraphicsViewHandle& fallbackTexture,
            FullscreenProgram program,
            const std::array<float, 8>& constants);
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            SpriteBlendMode blend,
            bool scissored,
            DXGI_FORMAT colorFormat,
            FullscreenProgram program);

        D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_toneMapPixelShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_bloomPixelShader;
        // pixel shader、出力format、blend modeごとに、通常passとscissor
        // passのcull違いを持ちます。
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 48>
            m_pipelineStates;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
        GraphicsViewHandle m_fallbackTexture;
        std::vector<QueuedSprite> m_sprites;
        std::vector<D3D12_RECT> m_scissorStack;
        SpriteBlendMode m_blend{ SpriteBlendMode::NonPremultiplied };
        std::array<float, 8> m_passConstants{};
        FullscreenProgram m_program{ FullscreenProgram::None };
        std::uint64_t m_activeToken{};
        std::uint64_t m_nextToken{ 1 };
        bool m_failed{};
    };
}
