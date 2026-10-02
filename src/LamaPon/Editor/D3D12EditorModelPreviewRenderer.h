#pragma once

#include "LamaPon/Editor/EditorModelPreviewRenderer.h"

namespace LamaPon
{
    class D3D12EditorModelPreviewRenderer final
        : public EditorModelPreviewRenderer
    {
    public:
        // 描画時まで初期化を要求せず描画装置を借用する(graphics: rendererより長く存続する装置)。
        explicit D3D12EditorModelPreviewRenderer(
            GraphicsDevice& graphics) noexcept;

        // 実験的DirectX12を返す。
        [[nodiscard]] RenderingApi Api() const noexcept override
        {
            return RenderingApi::DirectX12Experimental;
        }
        // CPU形状を既定姿勢で骨変形して材質と照明を適用する(model: CPU形状を持つ骨格モデル, world: モデルのworld行列, view: 視点行列, projection: 投影行列, material: 適用する材質, wireframe: ワイヤーフレーム描画にするか)。
        void DrawModel(
            const ModelAsset& model,
            DirectX::FXMMATRIX world,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const LitMaterial& material,
            bool wireframe) override;

    private:
        // rendererより長く存続する描画装置
        GraphicsDevice& m_graphics;
    };
}
