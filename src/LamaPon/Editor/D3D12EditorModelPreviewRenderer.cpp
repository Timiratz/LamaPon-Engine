#include "LamaPon/Editor/D3D12EditorModelPreviewRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <VertexTypes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace
{
    struct ImportedModelVertex final
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 normal{};
        DirectX::XMFLOAT4 tangent{};
        std::uint32_t color{};
        DirectX::XMFLOAT2 textureCoordinate{};
        std::uint32_t blendIndices{};
        std::uint32_t blendWeights{};
    };

    static_assert(
        sizeof(ImportedModelVertex)
            == sizeof(DirectX::
                VertexPositionNormalTangentColorTextureSkinning));

    void CopyLighting(
        const LamaPon::LightingState& lighting,
        LamaPon::PrimitiveDrawRequest& request) noexcept
    {
        request.ambientColor = lighting.ambientColor;
        request.ambientIntensity = lighting.ambientIntensity;
        request.directionalLightCount = std::min(
            request.directionalLights.size(),
            lighting.directionalLightCount);
        for (std::size_t index{};
            index < request.directionalLightCount;
            ++index)
        {
            const auto& source = lighting.directionalLights[index];
            request.directionalLights[index] = {
                source.direction,
                source.color,
                source.intensity,
                source.angularRadius
            };
        }
        request.pointLightCount = std::min(
            request.pointLights.size(),
            lighting.pointLightCount);
        for (std::size_t index{};
            index < request.pointLightCount;
            ++index)
        {
            const auto& source = lighting.pointLights[index];
            request.pointLights[index] = {
                source.position,
                source.range,
                source.color,
                source.intensity
            };
        }
        request.spotLightCount = std::min(
            request.spotLights.size(),
            lighting.spotLightCount);
        for (std::size_t index{};
            index < request.spotLightCount;
            ++index)
        {
            const auto& source = lighting.spotLights[index];
            request.spotLights[index] = {
                source.position,
                source.range,
                source.direction,
                source.innerConeCosine,
                source.color,
                source.intensity,
                source.outerConeCosine
            };
        }
    }
}

namespace LamaPon
{
    D3D12EditorModelPreviewRenderer::D3D12EditorModelPreviewRenderer(
        GraphicsDevice& graphics) noexcept
        : m_graphics(graphics)
    {
    }

    void D3D12EditorModelPreviewRenderer::DrawModel(
        const ModelAsset& asset,
        DirectX::FXMMATRIX world,
        DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const LitMaterial& material,
        const bool wireframe)
    {
        const auto resourceLease = m_graphics.AcquireResourceLease();
        if (!m_graphics.IsInitialized()
            || m_graphics.ActiveRenderingApi()
                != RenderingApi::DirectX12Experimental)
        {
            throw std::logic_error(
                "The DirectX 12 editor model preview renderer requires an "
                "initialized DirectX 12 graphics backend.");
        }
        if (!asset.skeletalModel)
        {
            throw std::invalid_argument(
                "The DirectX 12 editor model preview requires API-neutral "
                "model geometry.");
        }

        const auto& model = *asset.skeletalModel;
        std::vector<SkeletalPoseTransform> localPose;
        std::vector<DirectX::XMFLOAT4X4> globalPose;
        SkeletalModel::SamplePose(
            model.nodes,
            nullptr,
            0.0f,
            localPose,
            globalPose);
        if (globalPose.empty() && !model.primitives.empty())
        {
            throw std::invalid_argument(
                "The DirectX 12 editor model preview has no model pose.");
        }

        bool drewAny{};
        for (const auto& primitive : model.primitives)
        {
            if (primitive.cpuVertexStride < sizeof(ImportedModelVertex)
                || primitive.cpuVertexData.empty()
                || primitive.cpuVertexData.size()
                    % primitive.cpuVertexStride != 0
                || primitive.cpuIndices.empty()
                || primitive.meshNode >= globalPose.size())
            {
                continue;
            }

            const auto meshGlobal = DirectX::XMLoadFloat4x4(
                &globalPose[primitive.meshNode]);
            std::vector<DirectX::XMMATRIX> palette;
            if (primitive.skin >= 0)
            {
                const auto skinIndex = static_cast<std::size_t>(
                    primitive.skin);
                if (skinIndex >= model.skins.size())
                {
                    continue;
                }
                const auto& skin = model.skins[skinIndex];
                const auto inverseMesh = DirectX::XMMatrixInverse(
                    nullptr,
                    meshGlobal);
                palette.reserve(skin.joints.size());
                for (std::size_t jointIndex{};
                    jointIndex < skin.joints.size();
                    ++jointIndex)
                {
                    const auto nodeIndex = skin.joints[jointIndex];
                    if (nodeIndex >= globalPose.size())
                    {
                        palette.push_back(DirectX::XMMatrixIdentity());
                        continue;
                    }
                    const auto inverseBind =
                        jointIndex < skin.inverseBindMatrices.size()
                        ? DirectX::XMLoadFloat4x4(
                            &skin.inverseBindMatrices[jointIndex])
                        : DirectX::XMMatrixIdentity();
                    palette.push_back(
                        inverseBind
                        * DirectX::XMLoadFloat4x4(&globalPose[nodeIndex])
                        * inverseMesh);
                }
            }

            const auto vertexCount = primitive.cpuVertexData.size()
                / primitive.cpuVertexStride;
            std::vector<PrimitiveRenderVertex> vertices;
            vertices.reserve(vertexCount);
            for (std::size_t index{}; index < vertexCount; ++index)
            {
                ImportedModelVertex source{};
                std::memcpy(
                    &source,
                    primitive.cpuVertexData.data()
                        + index * primitive.cpuVertexStride,
                    sizeof(source));
                auto position = DirectX::XMLoadFloat3(&source.position);
                auto normal = DirectX::XMLoadFloat3(&source.normal);
                if (!palette.empty())
                {
                    DirectX::XMVECTOR skinnedPosition =
                        DirectX::XMVectorZero();
                    DirectX::XMVECTOR skinnedNormal =
                        DirectX::XMVectorZero();
                    float totalWeight{};
                    for (std::size_t influence{}; influence < 4u; ++influence)
                    {
                        const auto bone = static_cast<std::uint8_t>(
                            source.blendIndices >> (influence * 8u));
                        const float weight = static_cast<float>(
                            static_cast<std::uint8_t>(
                                source.blendWeights >> (influence * 8u)))
                            / 255.0f;
                        if (bone >= palette.size() || weight <= 0.0f)
                        {
                            continue;
                        }
                        skinnedPosition = DirectX::XMVectorAdd(
                            skinnedPosition,
                            DirectX::XMVectorScale(
                                DirectX::XMVector3TransformCoord(
                                    position,
                                    palette[bone]),
                                weight));
                        skinnedNormal = DirectX::XMVectorAdd(
                            skinnedNormal,
                            DirectX::XMVectorScale(
                                DirectX::XMVector3TransformNormal(
                                    normal,
                                    palette[bone]),
                                weight));
                        totalWeight += weight;
                    }
                    if (totalWeight > 0.0001f)
                    {
                        position = DirectX::XMVectorScale(
                            skinnedPosition,
                            1.0f / totalWeight);
                        normal = DirectX::XMVector3Normalize(skinnedNormal);
                    }
                }
                PrimitiveRenderVertex vertex;
                DirectX::XMStoreFloat3(&vertex.position, position);
                DirectX::XMStoreFloat3(&vertex.normal, normal);
                vertex.textureCoordinate = source.textureCoordinate;
                vertices.push_back(vertex);
            }

            PrimitiveDrawRequest request;
            request.shape = PrimitiveRenderShape::Procedural;
            request.vertices = vertices;
            request.indices = primitive.cpuIndices;
            DirectX::XMStoreFloat4x4(
                &request.world,
                meshGlobal * world);
            DirectX::XMStoreFloat4x4(&request.view, view);
            DirectX::XMStoreFloat4x4(&request.projection, projection);
            request.baseColor = material.BaseColor();
            request.roughness = material.Roughness();
            request.metallic = material.Metallic();
            request.normalStrength = material.NormalStrength();
            request.occlusionStrength = material.OcclusionStrength();
            request.emissiveFactor = material.EmissiveColor();
            request.fallbackTexture = m_graphics.WhiteTextureViewHandle();
            request.wireframe = wireframe;
            CopyLighting(m_graphics.Lighting(), request);
            drewAny = m_graphics.DrawPrimitive(request) || drewAny;
        }
        if (!drewAny)
        {
            throw std::runtime_error(
                "The DirectX 12 editor model preview could not draw the "
                "model geometry.");
        }
    }
}
