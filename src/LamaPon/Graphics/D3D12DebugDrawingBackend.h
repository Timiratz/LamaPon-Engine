#pragma once

#include "LamaPon/Graphics/DebugRenderer.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>

namespace LamaPon
{
    class D3D12Backend;

    // Editorのgrid／gizmo／boundsを、現在のD3D12描画先へ重ねる内部実装です。
    class D3D12DebugDrawingBackend final : public DebugDrawingBackend
    {
    public:
        explicit D3D12DebugDrawingBackend(D3D12Backend& backend);

        void DrawLines(
            std::span<const DebugLine> lines,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection) override;

    private:
        [[nodiscard]] ID3D12PipelineState* PipelineState(
            DXGI_FORMAT colorFormat,
            DXGI_FORMAT depthFormat);

        D3D12Backend* m_backend{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3DBlob> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3DBlob> m_pixelShader;
        // [RGBA8/RGBA16F][no depth/D24/D32]
        std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 6>
            m_pipelineStates;
    };
}
