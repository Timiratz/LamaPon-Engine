#include "LamaPon/Editor/D3D11EditorModelPreviewRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
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
        // このrenderer自身はGPU resourceを所有しません。Drawの間だけ
        // Device / Context世代を固定し、初期化前のfactory生成は妨げません。
        const auto resourceLease =
            m_graphics.AcquireResourceLease();
        auto* const context = m_graphics.Context();
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
                context,
                m_graphics.States(),
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
            model.model->UpdateEffects(
                [&material](DirectX::IEffect* effect)
                {
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
                m_graphics.States(),
                world,
                view,
                projection,
                wireframe);
        }
    }
}
