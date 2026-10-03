#include "LamaPon/Editor/D3D11EditorModelPreviewRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Access.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <Effects.h>
#include <Model.h>

#include <stdexcept>

namespace LamaPon
{
    D3D11EditorModelPreviewRenderer::D3D11EditorModelPreviewRenderer(
        GraphicsDevice& graphics) noexcept
        : m_graphics(graphics)
    {
    }

    void D3D11EditorModelPreviewRenderer::DrawModel(
        const ModelAsset& model,
        DirectX::FXMMATRIX world,
        DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const LitMaterial& material,
        const bool wireframe)
    {
        // GPU資源を所有しないため構築時に初期化を要求せず、描画中だけdevice・contextの世代を固定する。
        // 描画中だけ世代を固定する使用権
        const auto resourceLease =
            m_graphics.AcquireResourceLease();
        // 初期化済みD3D11 contextの借用
        auto* const context =
            Detail::GraphicsDeviceD3D11Access::Context(m_graphics);
        if (context == nullptr)
        {
            throw std::logic_error(
                "The DirectX 11 editor model preview renderer requires an "
                "initialized DirectX 11 context.");
        }
        if (!model.skeletalModel && !model.model)
        {
            throw std::invalid_argument(
                "The editor model preview requires a loaded model asset.");
        }

        if (model.skeletalModel)
        {
            model.skeletalModel->Draw(
                m_graphics,
                m_graphics.Lighting(),
                world,
                view,
                projection,
                nullptr,
                0.0f,
                wireframe,
                &material);
        }
        else if (model.model)
        {
            // textureを無効にして材質色を適用する(effect: モデルのeffectの借用)。
            model.model->UpdateEffects(
                [&material](DirectX::IEffect* effect)
                {
                    // 材質色を適用するBasicEffect
                    if (auto* const basic =
                            dynamic_cast<DirectX::BasicEffect*>(effect))
                    {
                        basic->SetTextureEnabled(false);
                        basic->SetDiffuseColor(
                            DirectX::XMLoadFloat4(
                                &material.BaseColor()));
                    }
                });
            model.model->Draw(
                context,
                Detail::GraphicsDeviceD3D11Access::States(m_graphics),
                world,
                view,
                projection,
                wireframe);
        }
    }
}
