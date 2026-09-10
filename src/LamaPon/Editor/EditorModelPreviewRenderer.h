#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"

#include <DirectXMath.h>

#include <memory>

namespace LamaPon
{
    class GraphicsDevice;
    class LitMaterial;
    struct ModelAsset;

    // エディター内のモデルプレビュー描画から、描画API固有のモデル
    // 実装と送信処理を分離するための契約です。描画先の開始・公開は
    // 呼び出し側が行い、このrendererは現在の描画先へモデルだけを描きます。
    class EditorModelPreviewRenderer
    {
    public:
        virtual ~EditorModelPreviewRenderer() = default;

        EditorModelPreviewRenderer() = default;
        EditorModelPreviewRenderer(
            const EditorModelPreviewRenderer&) = delete;
        EditorModelPreviewRenderer& operator=(
            const EditorModelPreviewRenderer&) = delete;

        [[nodiscard]] virtual RenderingApi
            Api() const noexcept = 0;
        virtual void DrawModel(
            const ModelAsset& model,
            DirectX::FXMMATRIX world,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const LitMaterial& material,
            bool wireframe) = 0;
    };

    // activeApiにはGraphicsDeviceで解決済みの実効APIを渡します。
    // 現段階で生成できるモデルプレビューrendererはDirectX 11だけです。
    // graphicsは返されたrendererより長く生存させ、DrawModel時には
    // 初期化済みである必要があります。描画呼び出し中はresource leaseを
    // 保持し、並行するGraphicsDeviceの再初期化を拒否します。
    [[nodiscard]] std::unique_ptr<EditorModelPreviewRenderer>
        CreateEditorModelPreviewRenderer(
            RenderingApi activeApi,
            GraphicsDevice& graphics);
}
