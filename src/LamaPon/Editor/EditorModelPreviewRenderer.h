#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"

#include <DirectXMath.h>

#include <memory>

namespace LamaPon
{
    class GraphicsDevice;
    class LitMaterial;
    struct ModelAsset;

    // 呼出し側が開始・公開する現在の描画先へモデルだけを描くプレビュー描画契約。
    class EditorModelPreviewRenderer
    {
    public:
        // 派生rendererのモデル資源を解放する。
        virtual ~EditorModelPreviewRenderer() = default;

        // プレビュー描画の基底を作る。
        EditorModelPreviewRenderer() = default;
        // プレビュー描画資源の共有を禁止する。
        EditorModelPreviewRenderer(
            const EditorModelPreviewRenderer&) = delete;
        // プレビュー描画資源の共有を禁止する。
        EditorModelPreviewRenderer& operator=(
            const EditorModelPreviewRenderer&) = delete;

        // 対応する描画APIを返す。
        [[nodiscard]] virtual RenderingApi
            Api() const noexcept = 0;
        // 現在の描画先へモデルを描く(model: 描画するモデル, world: モデルのworld行列, view: 視点行列, projection: 投影行列, material: 適用する材質, wireframe: ワイヤーフレーム描画にするか)。
        virtual void DrawModel(
            const ModelAsset& model,
            DirectX::FXMMATRIX world,
            DirectX::CXMMATRIX view,
            DirectX::CXMMATRIX projection,
            const LitMaterial& material,
            bool wireframe) = 0;
    };

    // 描画時はGraphicsDeviceを初期化済みにし、呼出し中のresource leaseで並行再初期化を拒否する。
    // 実効APIに対応するプレビューrendererを作る(activeApi: 解決済みの実効API, graphics: rendererより長く存続する描画装置)。
    [[nodiscard]] std::unique_ptr<EditorModelPreviewRenderer>
        CreateEditorModelPreviewRenderer(
            RenderingApi activeApi,
            GraphicsDevice& graphics);
}
