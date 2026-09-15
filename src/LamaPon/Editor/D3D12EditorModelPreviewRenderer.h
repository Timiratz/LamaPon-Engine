#pragma once

#include "LamaPon/Editor/EditorModelPreviewRenderer.h"

namespace LamaPon
{
    class D3D12EditorModelPreviewRenderer final
        : public EditorModelPreviewRenderer
    {
    public:
        explicit D3D12EditorModelPreviewRenderer(
            GraphicsDevice& graphics) noexcept;

        [[nodiscard]] RenderingApi Api() const noexcept override
        {
            return RenderingApi::DirectX12Experimental;
        }
        void DrawModel(
            const ModelAsset& model,
            DirectX::FXMMATRIX world,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const LitMaterial& material,
            bool wireframe) override;

    private:
        GraphicsDevice& m_graphics;
    };
}
