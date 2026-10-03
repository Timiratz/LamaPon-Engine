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
        // モデル空間の頂点位置
        DirectX::XMFLOAT3 position{};
        // モデル空間の頂点法線
        DirectX::XMFLOAT3 normal{};
        // 頂点接線と向き
        DirectX::XMFLOAT4 tangent{};
        // 詰め合わせた頂点色
        std::uint32_t color{};
        // 頂点のtexture座標
        DirectX::XMFLOAT2 textureCoordinate{};
        // 4つの8bit骨番号
        std::uint32_t blendIndices{};
        // 4つの8bit骨の重み
        std::uint32_t blendWeights{};
    };

    // memcpyする頂点配置はDirectXTKのskin付き頂点と同じサイズを要求する。
    static_assert(
        sizeof(ImportedModelVertex)
            == sizeof(DirectX::
                VertexPositionNormalTangentColorTextureSkinning));

    // 受信できる数に制限して現在の照明を描画要求へ複製する(lighting: 現在の照明の借用, request: 照明を設定する描画要求)。
    void CopyLighting(
        const LamaPon::LightingState& lighting,
        LamaPon::PrimitiveDrawRequest& request) noexcept
    {
        request.ambientColor = lighting.ambientColor;
        request.ambientIntensity = lighting.ambientIntensity;
        request.directionalLightCount = std::min(
            request.directionalLights.size(),
            lighting.directionalLightCount);
        // 照明または変換する頂点の番号
        for (std::size_t index{};
            index < request.directionalLightCount;
            ++index)
        {
            // コピーする照明または元の頂点
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
        // 照明または変換する頂点の番号
        for (std::size_t index{};
            index < request.pointLightCount;
            ++index)
        {
            // コピーする照明または元の頂点
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
        // 照明または変換する頂点の番号
        for (std::size_t index{};
            index < request.spotLightCount;
            ++index)
        {
            // コピーする照明または元の頂点
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

    // CPU形状を既定姿勢で骨変形して材質と照明を適用する(asset: CPU形状を持つ骨格モデル, world: モデルのworld行列, view: 視点行列, projection: 投影行列, material: 適用する材質, wireframe: ワイヤーフレーム描画にするか)。
    void D3D12EditorModelPreviewRenderer::DrawModel(
        const ModelAsset& asset,
        DirectX::FXMMATRIX world,
        DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const LitMaterial& material,
        const bool wireframe)
    {
        // 描画中だけ世代を固定する使用権
        const auto resourceLease = m_graphics.AcquireResourceLease();
        if (!m_graphics.IsInitialized()
            || m_graphics.ActiveRenderingApi()
                != RenderingApi::DirectX12Experimental)
        {
            throw std::logic_error(
                "The DirectX 12 editor model preview renderer requires an "
                "initialized DirectX 12 graphics backend.");
        }
        // D3D11専用モデルは使わずAPIに依存しないCPU形状を要求する。
        if (!asset.skeletalModel)
        {
            throw std::invalid_argument(
                "The DirectX 12 editor model preview requires API-neutral "
                "model geometry.");
        }

        // 描画する骨格モデルの借用
        const auto& model = *asset.skeletalModel;
        // 既定姿勢のローカル変換
        std::vector<SkeletalPoseTransform> localPose;
        // 既定姿勢の累積変換行列
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

        // 有効な形状を1つ以上描けたか
        bool drewAny{};
        // 描画するモデル内の形状
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

            // 形状ノードの累積変換行列
            const auto meshGlobal = DirectX::XMLoadFloat4x4(
                &globalPose[primitive.meshNode]);
            // 形状空間へ変換する骨行列
            std::vector<DirectX::XMMATRIX> palette;
            if (primitive.skin >= 0)
            {
                // 形状が使用するskinの番号
                const auto skinIndex = static_cast<std::size_t>(
                    primitive.skin);
                if (skinIndex >= model.skins.size())
                {
                    continue;
                }
                // 骨と逆bind行列の借用
                const auto& skin = model.skins[skinIndex];
                // 形状の累積変換の逆行列
                const auto inverseMesh = DirectX::XMMatrixInverse(
                    nullptr,
                    meshGlobal);
                palette.reserve(skin.joints.size());
                // skin内の骨の番号
                for (std::size_t jointIndex{};
                    jointIndex < skin.joints.size();
                    ++jointIndex)
                {
                    // 姿勢を参照する骨ノード番号
                    const auto nodeIndex = skin.joints[jointIndex];
                    if (nodeIndex >= globalPose.size())
                    {
                        palette.push_back(DirectX::XMMatrixIdentity());
                        continue;
                    }
                    // bind姿勢を戻す逆行列
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

            // strideから求めた頂点数
            const auto vertexCount = primitive.cpuVertexData.size()
                / primitive.cpuVertexStride;
            // 送信する変換済み頂点の配列
            std::vector<PrimitiveRenderVertex> vertices;
            vertices.reserve(vertexCount);
            // 照明または変換する頂点の番号
            for (std::size_t index{}; index < vertexCount; ++index)
            {
                // コピーする照明または元の頂点
                ImportedModelVertex source{};
                std::memcpy(
                    &source,
                    primitive.cpuVertexData.data()
                        + index * primitive.cpuVertexStride,
                    sizeof(source));
                // 骨変形を反映する頂点位置
                auto position = DirectX::XMLoadFloat3(&source.position);
                // 骨変形を反映する頂点法線
                auto normal = DirectX::XMLoadFloat3(&source.normal);
                if (!palette.empty())
                {
                    // 骨の重みで合成した頂点位置
                    DirectX::XMVECTOR skinnedPosition =
                        DirectX::XMVectorZero();
                    // 骨の重みで合成した頂点法線
                    DirectX::XMVECTOR skinnedNormal =
                        DirectX::XMVectorZero();
                    // 有効な骨の重みの合計
                    float totalWeight{};
                    // 頂点に影響する骨枠の番号
                    for (std::size_t influence{}; influence < 4u; ++influence)
                    {
                        // 詰め合わせから取り出す骨番号
                        const auto bone = static_cast<std::uint8_t>(
                            source.blendIndices >> (influence * 8u));
                        // 8bitの骨重みを0から1へ変換
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
                // 送信用の頂点
                PrimitiveRenderVertex vertex;
                DirectX::XMStoreFloat3(&vertex.position, position);
                DirectX::XMStoreFloat3(&vertex.normal, normal);
                vertex.textureCoordinate = source.textureCoordinate;
                vertices.push_back(vertex);
            }

            // 材質と照明を含む形状の描画要求
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
