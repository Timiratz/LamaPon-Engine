#pragma once

#include "LamaPon/Editor/EditorModelPreviewRenderer.h"

namespace LamaPon
{
    class D3D11EditorModelPreviewRenderer final
        : public EditorModelPreviewRenderer
    {
    public:
        // 描画時まで初期化を要求せず描画装置を借用する(graphics: rendererより長く存続する装置)。
        explicit D3D11EditorModelPreviewRenderer(
            GraphicsDevice& graphics) noexcept;

        // DirectX11を返す。
        [[nodiscard]] RenderingApi
            Api() const noexcept override
        {
            return RenderingApi::DirectX11;
        }
        // 現在の描画先へ骨格モデルまたは材質色の通常モデルを描く(model: 読込済みのモデル, world: モデルのworld行列, view: 視点行列, projection: 投影行列, material: 適用する材質, wireframe: ワイヤーフレーム描画にするか)。
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
