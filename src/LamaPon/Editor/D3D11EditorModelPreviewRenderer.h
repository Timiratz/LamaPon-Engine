#pragma once

#include "LamaPon/Editor/EditorModelPreviewRenderer.h"

namespace LamaPon
{
    class D3D11EditorModelPreviewRenderer final
        : public EditorModelPreviewRenderer
    {
    public:
        explicit D3D11EditorModelPreviewRenderer(
            GraphicsDevice& graphics) noexcept;

        [[nodiscard]] RenderingApi
            Api() const noexcept override
        {
            return RenderingApi::DirectX11;
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
