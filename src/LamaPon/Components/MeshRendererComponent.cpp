#include "LamaPon/Components/MeshRendererComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/FrameDebugDescription.h"
#include "LamaPon/Components/ReflectionProbeComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Access.h"
#include "LamaPon/Graphics/GraphicsRenderServices.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/LitMaterialAsset.h"
#include "LamaPon/Graphics/LitTextureRequest.h"
#include "LamaPon/Graphics/MaterialShaderDrawRequest.h"
#include "LamaPon/Physics/CollisionTypes.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/Transform.h"

#include <CommonStates.h>
#include <GeometricPrimitive.h>
#include <VertexTypes.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

namespace
{
    // 三次元の全成分が有限値か確認する(value: 検証する三次元値)。
    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT3& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z);
    }

    // 二次元の全成分が有限値か確認する(value: 検証する二次元値)。
    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT2& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y);
    }

    // D3D11の基本形状を作り、Planeは1×0.05×1の薄い箱、未対応値は例外とする(context: D3D11の描画コンテキスト, shape: 作成する基本形状)。
    [[nodiscard]] std::unique_ptr<DirectX::GeometricPrimitive>
        CreatePrimitiveShape(
            ID3D11DeviceContext* context,
            const LamaPon::PrimitiveShape shape)
    {
        switch (shape)
        {
        case LamaPon::PrimitiveShape::Cube:
            return DirectX::GeometricPrimitive::CreateCube(context);
        case LamaPon::PrimitiveShape::Sphere:
            return DirectX::GeometricPrimitive::CreateSphere(context);
        case LamaPon::PrimitiveShape::Cylinder:
            return DirectX::GeometricPrimitive::CreateCylinder(context);
        case LamaPon::PrimitiveShape::Plane:
            return DirectX::GeometricPrimitive::CreateBox(
                context,
                DirectX::XMFLOAT3{ 1.0f, 0.05f, 1.0f });
        default:
            throw std::runtime_error("Unsupported primitive shape.");
        }
    }

    // 検証済み頂点と16ビット内の添字からD3D11の三角形メッシュを作る(context: D3D11の描画コンテキスト, vertices: ローカル座標の頂点一覧, indices: 16ビット内の三角形添字)。
    [[nodiscard]] std::unique_ptr<DirectX::GeometricPrimitive>
        CreateProceduralPrimitive(
            ID3D11DeviceContext* context,
            const std::vector<LamaPon::ProceduralMeshVertex>& vertices,
            const std::vector<std::uint32_t>& indices)
    {
        // DirectXTK形式へ写した頂点
        DirectX::GeometricPrimitive::VertexCollection convertedVertices;
        convertedVertices.reserve(vertices.size());
        // 変換するローカル頂点
        for (const auto& vertex : vertices)
        {
            convertedVertices.push_back({
                vertex.position,
                vertex.normal,
                vertex.textureCoordinate,
            });
        }
        // 16ビットへ写した添字
        DirectX::GeometricPrimitive::IndexCollection convertedIndices;
        convertedIndices.reserve(indices.size());
        // 16ビットへ変換する頂点添字
        for (const auto index : indices)
        {
            convertedIndices.push_back(
                static_cast<std::uint16_t>(index));
        }
        return DirectX::GeometricPrimitive::CreateCustom(
            context,
            convertedVertices,
            convertedIndices);
    }

    // 共有画像の現在のGPUビューを取得し、画像や資源がなければ空を返す(asset: ビューを取得する共有画像)。
    [[nodiscard]] LamaPon::GraphicsViewHandle AcquireTextureView(
        const std::shared_ptr<
            const LamaPon::TextureAsset>& asset) noexcept
    {
        if (asset == nullptr)
        {
            return {};
        }
        // 画像のGPU資源の借用
        const auto resources = asset->resources.Acquire();
        return resources != nullptr
            ? resources->shaderResourceView
            : LamaPon::GraphicsViewHandle{};
    }

    // ライトと影などをAPI共通の描画要求へ写し、灯数を要求側の容量に収める(lighting: シーンのライティング状態, request: 設定を写す描画要求)。
    void CopyPrimitiveLighting(
        const LamaPon::LightingState& lighting,
        LamaPon::PrimitiveDrawRequest& request) noexcept
    {
        request.ambientColor = lighting.ambientColor;
        request.ambientIntensity = lighting.ambientIntensity;
        request.directionalLightCount = std::min(
            request.directionalLights.size(),
            lighting.directionalLightCount);
        // コピーする灯や影の番号
        for (std::size_t index{};
            index < request.directionalLightCount;
            ++index)
        {
            // コピーするライトまたは影情報
            const auto& source = lighting.directionalLights[index];
            request.directionalLights[index] = {
                source.direction,
                source.color,
                source.intensity,
                source.angularRadius };
        }
        request.pointLightCount = std::min(
            request.pointLights.size(),
            lighting.pointLightCount);
        // コピーする灯や影の番号
        for (std::size_t index{};
            index < request.pointLightCount;
            ++index)
        {
            // コピーするライトまたは影情報
            const auto& source = lighting.pointLights[index];
            request.pointLights[index] = {
                source.position,
                source.range,
                source.color,
                source.intensity };
        }
        request.spotLightCount = std::min(
            request.spotLights.size(),
            lighting.spotLightCount);
        // コピーする灯や影の番号
        for (std::size_t index{};
            index < request.spotLightCount;
            ++index)
        {
            // コピーするライトまたは影情報
            const auto& source = lighting.spotLights[index];
            request.spotLights[index] = {
                source.position,
                source.range,
                source.direction,
                source.innerConeCosine,
                source.color,
                source.intensity,
                source.outerConeCosine };
        }
        // 平行光のカスケード影情報
        const auto& shadow = lighting.directionalShadow;
        request.directionalShadow.lightViewProjections =
            shadow.lightViewProjections;
        request.directionalShadow.cascadeSplits =
            shadow.cascadeSplits;
        request.directionalShadow.texture = shadow.texture;
        request.directionalShadow.lightIndex = shadow.lightIndex;
        request.directionalShadow.cascadeCount = shadow.cascadeCount;
        request.directionalShadow.bias = shadow.bias;
        request.directionalShadow.normalBias = shadow.normalBias;
        request.directionalShadow.strength = shadow.strength;
        request.directionalShadow.inverseResolution =
            1.0f / std::max(
                lighting.directionalShadowResolution,
                1.0f);
        request.directionalShadow.enabled = shadow.enabled;
        // コピーする灯や影の番号
        for (std::size_t index{};
            index < request.spotShadows.size();
            ++index)
        {
            // コピーするライトまたは影情報
            const auto& source = lighting.spotShadows[index];
            request.spotShadows[index] = {
                source.lightViewProjection,
                source.lightIndex,
                source.bias,
                source.normalBias,
                source.strength,
                source.enabled };
        }
        request.spotShadowTexture = lighting.spotShadowTexture;
        request.pointShadow = {
            lighting.pointShadow.texture,
            lighting.pointShadow.lightIndex,
            lighting.pointShadow.bias,
            lighting.pointShadow.strength,
            lighting.pointShadow.enabled };
        request.localShadowInverseResolution =
            1.0f / std::max(
                lighting.localShadowResolution,
                1.0f);
        request.screenAmbientOcclusion = {
            lighting.screenAmbientOcclusion.texture,
            lighting.screenAmbientOcclusion.inverseWidth,
            lighting.screenAmbientOcclusion.inverseHeight,
            lighting.screenAmbientOcclusion.enabled };
        // スクリーン空間反射の設定
        const auto& reflection = lighting.screenSpaceReflection;
        request.screenSpaceReflection = {
            reflection.texture,
            reflection.depth,
            reflection.previousViewProjection,
            reflection.inverseWidth,
            reflection.inverseHeight,
            reflection.intensity,
            reflection.maximumDistance,
            reflection.thickness,
            reflection.roughnessCutoff,
            reflection.stepCount,
            reflection.depthPyramidMaximumMip,
            reflection.enabled };
        // 環境反射の画像と設定
        const auto& environment = lighting.environment;
        request.environment = {
            environment.texture,
            environment.specular,
            environment.irradiance,
            environment.specularMaximumMip,
            environment.intensity,
            environment.enabled };
        request.fog = {
            lighting.fog.color,
            lighting.fog.startDistance,
            lighting.fog.endDistance,
            lighting.fog.density,
            LamaPon::PrimitiveFogModel::LamaPonLit,
            lighting.fog.enabled };
        // Forward+のライト分割情報
        const auto& clustered = lighting.clustered;
        request.clustered = {
            clustered.lights,
            clustered.lightIndices,
            clustered.clusterCounts,
            clustered.nearPlane,
            clustered.farPlane,
            clustered.inverseWidth,
            clustered.inverseHeight,
            clustered.lightCount,
            clustered.enabled };
        // ベイク済み間接光の設定
        const auto& bakedGi = lighting.bakedGlobalIllumination;
        request.bakedGlobalIllumination = {
            bakedGi.redCoefficients,
            bakedGi.greenCoefficients,
            bakedGi.blueCoefficients,
            bakedGi.volumeMinimum,
            bakedGi.volumeSize,
            bakedGi.resolution,
            bakedGi.intensity,
            bakedGi.enabled };
    }

    // PlaneとCubeの四制御点パッチを作り、それ以外の形状は空を返す(shape: 制御点を作る基本形状)。
    [[nodiscard]] std::vector<DirectX::VertexPositionNormalTexture>
        BuildTessellationControlPoints(const LamaPon::PrimitiveShape shape)
    {
        // 四制御点は(u0,v0)、(u1,v0)、(u0,v1)、(u1,v1)で並べ、u×vが法線を向くよう揃える。
        // 各パッチに四頂点ずつ並べる制御点
        std::vector<DirectX::VertexPositionNormalTexture>
            controlPoints;
        // 四隅の制御点を追加する(center: パッチ中心のローカル位置, uAxis: U方向の全幅ベクトル, vAxis: V方向の全幅ベクトル, normal: 四隅に設定する法線)。
        const auto addQuad =
            [&controlPoints](
                const DirectX::XMFLOAT3& center,
                const DirectX::XMFLOAT3& uAxis,
                const DirectX::XMFLOAT3& vAxis,
                const DirectX::XMFLOAT3& normal)
        {
            // UV位置から制御点を作る(u: 0〜1のU位置, v: 0〜1のV位置)。
            const auto corner =
                [&](const float u, const float v)
            {
                return DirectX::VertexPositionNormalTexture{
                    DirectX::XMFLOAT3{
                        center.x
                            + uAxis.x * (u - 0.5f)
                            + vAxis.x * (v - 0.5f),
                        center.y
                            + uAxis.y * (u - 0.5f)
                            + vAxis.y * (v - 0.5f),
                        center.z
                            + uAxis.z * (u - 0.5f)
                            + vAxis.z * (v - 0.5f) },
                    normal,
                    DirectX::XMFLOAT2{ u, v }
                };
            };
            controlPoints.push_back(corner(0.0f, 0.0f));
            controlPoints.push_back(corner(1.0f, 0.0f));
            controlPoints.push_back(corner(0.0f, 1.0f));
            controlPoints.push_back(corner(1.0f, 1.0f));
        };

        switch (shape)
        {
        case LamaPon::PrimitiveShape::Plane:

            addQuad(
                { 0.0f, 0.0f, 0.0f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, -1.0f },
                { 0.0f, 1.0f, 0.0f });
            break;

        case LamaPon::PrimitiveShape::Cube:

            addQuad(
                { 0.0f, 0.5f, 0.0f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, -1.0f },
                { 0.0f, 1.0f, 0.0f });
            addQuad(
                { 0.0f, -0.5f, 0.0f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f },
                { 0.0f, -1.0f, 0.0f });
            addQuad(
                { 0.5f, 0.0f, 0.0f },
                { 0.0f, 0.0f, -1.0f },
                { 0.0f, 1.0f, 0.0f },
                { 1.0f, 0.0f, 0.0f });
            addQuad(
                { -0.5f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f },
                { 0.0f, 1.0f, 0.0f },
                { -1.0f, 0.0f, 0.0f });
            addQuad(
                { 0.0f, 0.0f, 0.5f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f });
            addQuad(
                { 0.0f, 0.0f, -0.5f },
                { -1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f },
                { 0.0f, 0.0f, -1.0f });
            break;

        default:

            break;
        }
        return controlPoints;
    }

    // 共有Effectの一時フラグ・選択パスと後続描画へ残る可変ジオメトリ段を描画終了時に初期化する。
    class MaterialPassScope final
    {
    public:
        // 描画終了時に共有Effectと可変ジオメトリ段を初期化する境界を作る(effect: 借用した共有Effect, context: 描画コンテキストかnullptr, role: 先頭へ戻すパスの用途)。
        MaterialPassScope(
            LamaPon::LitEffect& effect,
            ID3D11DeviceContext* context,
            const LamaPon::ShaderPassRole role) noexcept
            : m_effect(effect)
            , m_context(context)
            , m_role(role)
        {
        }

        // 描画状態を戻す境界の複製を禁止する。
        MaterialPassScope(const MaterialPassScope&) = delete;
        // 描画状態を戻す境界へのコピー代入を禁止する。
        MaterialPassScope& operator=(
            const MaterialPassScope&) = delete;

        // 一時フラグを解除して使用roleの先頭パスへ戻し、HS・DS・GSを解除する。
        ~MaterialPassScope()
        {
            m_effect.SetTessellationDrawEnabled(false);
            m_effect.SetDepthOnlyEnabled(false);
            m_effect.SetInstancingEnabled(false);
            if (m_effect.PassCount(m_role) != 0)
            {
                try
                {
                    m_effect.SelectPass(m_role, 0);
                }
                catch (...)
                {
                    // 元の描画エラーを妨げないよう、破棄時のパス選択失敗は吸収する。
                }
            }
            if (m_context != nullptr)
            {
                m_context->HSSetShader(nullptr, nullptr, 0);
                m_context->DSSetShader(nullptr, nullptr, 0);
                m_context->GSSetShader(nullptr, nullptr, 0);
            }
        }

    private:
        // 借用した共有Effect
        LamaPon::LitEffect& m_effect;
        // 借用した描画コンテキスト
        ID3D11DeviceContext* m_context{};
        // 先頭へ戻すパスの用途
        LamaPon::ShaderPassRole m_role{};
    };
}

namespace LamaPon
{
    void MeshRendererComponent::ApplyShaderRenderState(
        const ShaderRenderState& state) const
    {
        // 借用したD3D11コンテキスト
        auto* context = Detail::GraphicsDeviceD3D11Access::Context(
            *m_graphics);
        // 借用した共通の描画状態
        auto& states = Detail::GraphicsDeviceD3D11Access::States(
            *m_graphics);
        // 合成状態に渡す定数色
        constexpr float blendFactor[4]{};
        switch (state.blend)
        {
        case ShaderBlendMode::Alpha:
            context->OMSetBlendState(
                states.NonPremultiplied(),
                blendFactor,
                0xffffffffu);
            break;
        case ShaderBlendMode::Additive:
        {
            // 後段が読むシーンのアルファを保持するため、利用可能なら純加算専用の合成状態を使う。
            // アルファを保持する加算状態
            auto* const additive = Detail::GraphicsDeviceD3D11Access::
                AdditiveBlendPreservingAlpha(*m_graphics);
            context->OMSetBlendState(
                additive != nullptr ? additive : states.Additive(),
                blendFactor,
                0xffffffffu);
            break;
        }
        case ShaderBlendMode::Premultiplied:
            context->OMSetBlendState(
                states.AlphaBlend(),
                blendFactor,
                0xffffffffu);
            break;
        case ShaderBlendMode::Opaque:
        default:
            context->OMSetBlendState(
                states.Opaque(),
                blendFactor,
                0xffffffffu);
            break;
        }
        context->OMSetDepthStencilState(
            state.depthTest
                ? (state.depthWrite
                    ? states.DepthDefault()
                    : states.DepthRead())
                : states.DepthNone(),
            0);
        // 明示設定を優先したカリング
        const auto cull = m_cullModeOverride
            ? m_cullMode
            : state.cull;
        switch (cull)
        {
        case ShaderCullMode::Front:
            context->RSSetState(
                states.CullClockwise());
            break;
        case ShaderCullMode::None:
            context->RSSetState(states.CullNone());
            break;
        case ShaderCullMode::Back:
        default:
            context->RSSetState(
                states.CullCounterClockwise());
            break;
        }
    }

    void MeshRendererComponent::ApplyCullModeOverride() const
    {
        if (!m_cullModeOverride || m_graphics == nullptr)
        {
            return;
        }
        // 借用したD3D11コンテキスト
        auto* context = Detail::GraphicsDeviceD3D11Access::Context(
            *m_graphics);
        // 借用した共通の描画状態
        auto& states = Detail::GraphicsDeviceD3D11Access::States(
            *m_graphics);
        switch (m_cullMode)
        {
        case ShaderCullMode::Front:
            context->RSSetState(states.CullClockwise());
            break;
        case ShaderCullMode::None:
            context->RSSetState(states.CullNone());
            break;
        case ShaderCullMode::Back:
        default:
            context->RSSetState(states.CullCounterClockwise());
            break;
        }
    }

    LitTextureRequest
        MeshRendererComponent::BuildLitTextureRequest() const noexcept
    {
        // 材質画像をまとめる共通要求
        LitTextureRequest request{};
        request.albedo = AcquireTextureView(m_albedoTexture);
        request.normal = AcquireTextureView(m_normalTexture);
        request.roughness = AcquireTextureView(m_roughnessTexture);
        request.metallic = AcquireTextureView(m_metallicTexture);
        request.occlusion = AcquireTextureView(m_occlusionTexture);
        request.emissive = AcquireTextureView(m_emissiveTexture);
        // 追加画像のスロット番号
        for (std::size_t index = 0;
            index < request.customTextures.size();
            ++index)
        {
            request.customTextures[index] =
                AcquireTextureView(m_customTextures[index]);
        }
        request.occlusionStrength =
            m_material.OcclusionStrength();
        request.emissiveFactor = m_material.EmissiveColor();
        return request;
    }

    struct MeshRendererComponent::InputLayoutHolder final
    {
        // 所有するD3D11入力レイアウト
        Microsoft::WRL::ComPtr<ID3D11InputLayout> value;
    };

    struct MeshRendererComponent::TessellationPatchHolder final
    {
        // 所有する四制御点の頂点バッファ
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        // 四制御点パッチの全頂点数
        UINT controlPointCount{};
    };

    MeshRendererComponent::MeshRendererComponent(
        const PrimitiveShape shape,
        const DirectX::XMFLOAT4 color,
        std::filesystem::path albedoTexture,
        std::filesystem::path normalTexture,
        const float roughness,
        const float normalStrength,
        std::filesystem::path materialAsset) noexcept
        : m_shape(shape)
        , m_material(
            color,
            std::move(albedoTexture),
            std::move(normalTexture),
            roughness,
            normalStrength)
        , m_materialAssetPath(std::move(materialAsset))
    {
    }

    MeshRendererComponent::~MeshRendererComponent() = default;

    void MeshRendererComponent::SetProceduralMesh(
        std::vector<ProceduralMeshVertex> vertices,
        std::vector<std::uint32_t> indices,
        const bool recalculateNormals)
    {
        if (vertices.empty() || indices.empty())
        {
            throw std::invalid_argument(
                "A procedural mesh requires vertices and indices."
                " Use ClearProceduralMesh to restore the primitive.");
        }
        // 16ビット添字で扱える頂点数
        constexpr std::size_t MaximumVertexCount =
            static_cast<std::size_t>(
                std::numeric_limits<std::uint16_t>::max()) + 1u;
        if (vertices.size() > MaximumVertexCount)
        {
            throw std::invalid_argument(
                "A procedural mesh cannot exceed 65536 vertices.");
        }
        if (indices.size() < 3u || indices.size() % 3u != 0u)
        {
            throw std::invalid_argument(
                "Procedural mesh indices must contain complete triangles.");
        }
        // 検証・法線処理・境界計算の頂点
        for (const auto& vertex : vertices)
        {
            if (!IsFinite(vertex.position)
                || !IsFinite(vertex.normal)
                || !IsFinite(vertex.textureCoordinate))
            {
                throw std::invalid_argument(
                    "Procedural mesh vertices must contain finite values.");
            }
        }
        // 頂点添字か三角形の開始番号
        for (const auto index : indices)
        {
            if (index >= vertices.size())
            {
                throw std::out_of_range(
                    "A procedural mesh index is outside the vertex array.");
            }
        }

        if (recalculateNormals)
        {
            // 検証・法線処理・境界計算の頂点
            for (auto& vertex : vertices)
            {
                vertex.normal = {};
            }
            // 頂点添字か三角形の開始番号
            for (std::size_t index{}; index < indices.size(); index += 3u)
            {
                // 三角形の最初の頂点
                auto& first = vertices[indices[index]];
                // 三角形の二番目の頂点
                auto& second = vertices[indices[index + 1u]];
                // 三角形の三番目の頂点
                auto& third = vertices[indices[index + 2u]];
                // 一番目から二番目への辺
                const auto edgeA = DirectX::XMVectorSubtract(
                    DirectX::XMLoadFloat3(&second.position),
                    DirectX::XMLoadFloat3(&first.position));
                // 一番目から三番目への辺
                const auto edgeB = DirectX::XMVectorSubtract(
                    DirectX::XMLoadFloat3(&third.position),
                    DirectX::XMLoadFloat3(&first.position));
                // 面積に比例する三角形の法線
                DirectX::XMFLOAT3 faceNormal{};
                DirectX::XMStoreFloat3(
                    &faceNormal,
                    DirectX::XMVector3Cross(edgeA, edgeB));
                // 検証・法線処理・境界計算の頂点
                for (auto* vertex : { &first, &second, &third })
                {
                    vertex->normal.x += faceNormal.x;
                    vertex->normal.y += faceNormal.y;
                    vertex->normal.z += faceNormal.z;
                }
            }
        }
        // 検証・法線処理・境界計算の頂点
        for (auto& vertex : vertices)
        {
            // 正規化前の法線の長さ
            const float length = std::sqrt(
                vertex.normal.x * vertex.normal.x
                    + vertex.normal.y * vertex.normal.y
                    + vertex.normal.z * vertex.normal.z);
            if (length <= 1.0e-6f)
            {
                vertex.normal = { 0.0f, 1.0f, 0.0f };
            }
            else
            {
                vertex.normal.x /= length;
                vertex.normal.y /= length;
                vertex.normal.z /= length;
            }
        }

        // 計算したローカルAABB最小端
        DirectX::XMFLOAT3 minimum = vertices.front().position;
        // 計算したローカルAABB最大端
        DirectX::XMFLOAT3 maximum = vertices.front().position;
        // 検証・法線処理・境界計算の頂点
        for (const auto& vertex : vertices)
        {
            minimum.x = std::min(minimum.x, vertex.position.x);
            minimum.y = std::min(minimum.y, vertex.position.y);
            minimum.z = std::min(minimum.z, vertex.position.z);
            maximum.x = std::max(maximum.x, vertex.position.x);
            maximum.y = std::max(maximum.y, vertex.position.y);
            maximum.z = std::max(maximum.z, vertex.position.z);
        }

        // 切り替え前に作るGPUメッシュ
        std::unique_ptr<DirectX::GeometricPrimitive> primitive;
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX11)
        {
            // GPUメッシュの生成が成功するまで、現在のGPU資源とCPU頂点・添字を変更しない。
            primitive = CreateProceduralPrimitive(
                Detail::GraphicsDeviceD3D11Access::Context(
                    *m_graphics),
                vertices,
                indices);
        }
        m_proceduralVertices = std::move(vertices);
        m_proceduralIndices = std::move(indices);
        m_proceduralBoundsMinimum = minimum;
        m_proceduralBoundsMaximum = maximum;
        if (primitive)
        {
            m_primitive = std::move(primitive);
        }
        m_tessellationPatches.reset();
        m_instancedThisPass = false;
        RefreshShader(false);
    }

    void MeshRendererComponent::ClearProceduralMesh()
    {
        if (!HasProceduralMesh())
        {
            return;
        }
        // 切り替え前に作るGPUメッシュ
        std::unique_ptr<DirectX::GeometricPrimitive> primitive;
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX11)
        {
            primitive = CreatePrimitiveShape(
                Detail::GraphicsDeviceD3D11Access::Context(
                    *m_graphics),
                m_shape);
        }
        m_proceduralVertices.clear();
        m_proceduralIndices.clear();
        m_proceduralBoundsMinimum = {};
        m_proceduralBoundsMaximum = {};
        if (primitive)
        {
            m_primitive = std::move(primitive);
            BuildTessellationPatches(*m_graphics);
        }
        m_instancedThisPass = false;
        RefreshShader(false);
    }

    bool MeshRendererComponent::TryGetLocalBounds(
        Bounds3D& bounds) const noexcept
    {
        if (!HasProceduralMesh())
        {
            return false;
        }
        bounds = {
            m_proceduralBoundsMinimum,
            m_proceduralBoundsMaximum,
        };
        return true;
    }

    void MeshRendererComponent::SetAlbedoTexturePath(
        std::filesystem::path path)
    {
        // 差し替え前に読む共有画像
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_material.SetAlbedoTexture(std::move(path));
        m_albedoTexture = std::move(texture);
    }

    void MeshRendererComponent::SetCustomTexturePath(
        const std::size_t index,
        std::filesystem::path path)
    {
        if (index >= LitMaterial::CustomTextureCount)
        {
            return;
        }
        // 差し替え前に読む共有画像
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_material.SetCustomTexture(index, std::move(path));
        m_customTextures[index] = std::move(texture);
    }

    void MeshRendererComponent::SetNormalTexturePath(
        std::filesystem::path path)
    {
        // 差し替え前に読む共有画像
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_material.SetNormalTexture(std::move(path));
        m_normalTexture = std::move(texture);
    }

    void MeshRendererComponent::SetRoughnessTexturePath(
        std::filesystem::path path)
    {
        // 差し替え前に読む共有画像
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_material.SetRoughnessTexture(std::move(path));
        m_roughnessTexture = std::move(texture);
    }

    void MeshRendererComponent::SetMetallicTexturePath(
        std::filesystem::path path)
    {
        // 差し替え前に読む共有画像
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_material.SetMetallicTexture(std::move(path));
        m_metallicTexture = std::move(texture);
    }

    void MeshRendererComponent::SetOcclusionTexturePath(
        std::filesystem::path path)
    {
        // 差し替え前に読む共有画像
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_material.SetOcclusionTexture(std::move(path));
        m_occlusionTexture = std::move(texture);
    }

    void MeshRendererComponent::SetEmissiveTexturePath(
        std::filesystem::path path)
    {
        // 差し替え前に読む共有画像
        std::shared_ptr<const TextureAsset> texture;
        if (m_assets != nullptr && !path.empty())
        {
            texture = m_assets->LoadTexture(path);
        }
        m_material.SetEmissiveTexture(std::move(path));
        m_emissiveTexture = std::move(texture);
    }

    void MeshRendererComponent::SetShaderPath(
        std::filesystem::path path)
    {
        m_material.SetShader(std::move(path));
        RefreshShader(false);
    }

    void MeshRendererComponent::ReloadShader()
    {
        RefreshShader(true);
    }

    void MeshRendererComponent::SetMaterialAssetPath(
        std::filesystem::path path)
    {
        if (!path.empty() && m_assets != nullptr)
        {
            ApplyMaterial(LoadLitMaterialAsset(
                m_assets->ResolvePath(path),
                &m_assets->Database(),
                m_assets));
        }
        m_materialAssetPath = std::move(path);
    }

    void MeshRendererComponent::ReloadMaterialAsset()
    {
        if (m_materialAssetPath.empty())
        {
            return;
        }
        if (m_assets == nullptr)
        {
            throw std::runtime_error(
                "MeshRenderer is not initialized.");
        }
        ApplyMaterial(LoadLitMaterialAsset(
            m_assets->ResolvePath(m_materialAssetPath),
            &m_assets->Database(),
            m_assets));
    }

    void MeshRendererComponent::SetMaterial(
        const LitMaterial& material)
    {
        m_materialAssetPath.clear();
        ApplyMaterial(material);
    }

    void MeshRendererComponent::ApplyMaterial(
        const LitMaterial& material)
    {
        // 差し替え前に読む基本色画像
        std::shared_ptr<const TextureAsset> albedo;
        // 差し替え前に読む法線画像
        std::shared_ptr<const TextureAsset> normal;
        // 差し替え前に読む粗さ画像
        std::shared_ptr<const TextureAsset> roughness;
        // 差し替え前に読む金属度画像
        std::shared_ptr<const TextureAsset> metallic;
        // 差し替え前に読む遮蔽画像
        std::shared_ptr<const TextureAsset> occlusion;
        // 差し替え前に読む発光画像
        std::shared_ptr<const TextureAsset> emissive;
        // 差し替え前に読む追加画像の配列
        std::array<
            std::shared_ptr<const TextureAsset>,
            LitMaterial::CustomTextureCount> customTextures{};
        if (m_assets != nullptr)
        {
            // 法線・データ・色の用途を指定し、画像の圧縮形式を選択させる。
            // 読み込み時の画像用途の型
            using Usage = TextureLoader::TextureUsage;
            // 画像を用途別に読み、空のパスなら解除する(path: 画像のパス, usage: 圧縮形式を選ぶ画像用途)。
            const auto load =
                [this](
                    const std::filesystem::path& path,
                    const Usage usage)
                -> std::shared_ptr<const TextureAsset>
                {
                    if (path.empty())
                    {
                        return {};
                    }
                    return m_assets->LoadTexture(path, usage);
                };
            albedo = load(material.AlbedoTexture(), Usage::Color);
            normal = load(
                material.NormalTexture(),
                Usage::NormalMap);
            roughness = load(
                material.RoughnessTexture(),
                Usage::DataMap);
            metallic = load(
                material.MetallicTexture(),
                Usage::DataMap);
            occlusion = load(
                material.OcclusionTexture(),
                Usage::DataMap);
            emissive = load(
                material.EmissiveTexture(),
                Usage::Color);
            // 読み込む追加画像のスロット
            for (std::size_t index{};
                index < customTextures.size();
                ++index)
            {
                customTextures[index] = load(
                    material.CustomTexture(index),
                    Usage::Color);
            }
        }

        m_material = material;
        m_albedoTexture = std::move(albedo);
        m_normalTexture = std::move(normal);
        m_roughnessTexture = std::move(roughness);
        m_metallicTexture = std::move(metallic);
        m_occlusionTexture = std::move(occlusion);
        m_emissiveTexture = std::move(emissive);
        m_customTextures = std::move(customTextures);
        RefreshShader(false);
    }

    void MeshRendererComponent::BuildTessellationPatches(
        GraphicsDevice& graphics)
    {
        m_tessellationPatches.reset();
        if (HasProceduralMesh())
        {
            // 手続き三角形を四制御点へ変換しないため、シェーダー同期時に代替描画を選ぶ。
            return;
        }

        // 形状から作った四制御点一覧
        const auto controlPoints = BuildTessellationControlPoints(m_shape);
        if (controlPoints.empty())
        {

            return;
        }

        // 制御点バッファの生成指定
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(
            controlPoints.size()
            * sizeof(DirectX::VertexPositionNormalTexture));
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        // バッファの初期頂点データ
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = controlPoints.data();

        // 差し替える四制御点のGPU資源
        auto patches =
            std::make_unique<TessellationPatchHolder>();
        patches->controlPointCount =
            static_cast<UINT>(controlPoints.size());
        // D3D11バッファ生成の結果
        const HRESULT result =
            Detail::GraphicsDeviceD3D11Access::Device(graphics)
                ->CreateBuffer(
                &description,
                &data,
                patches->vertexBuffer.ReleaseAndGetAddressOf());
        if (FAILED(result))
        {
            throw std::runtime_error(
                "Could not create tessellation patch buffer.");
        }
        m_tessellationPatches = std::move(patches);
    }

    void MeshRendererComponent::BuildActivePrimitive(
        GraphicsDevice& graphics)
    {
        m_primitive = HasProceduralMesh()
            ? CreateProceduralPrimitive(
                Detail::GraphicsDeviceD3D11Access::Context(graphics),
                m_proceduralVertices,
                m_proceduralIndices)
            : CreatePrimitiveShape(
                Detail::GraphicsDeviceD3D11Access::Context(graphics),
                m_shape);
    }

    bool MeshRendererComponent::CanDrawTessellatedPatch()
        const noexcept
    {

        return m_effect != nullptr
            && m_effect->HasTessellation()
            && m_tessellationPatches
            && m_tessellationPatches->vertexBuffer
            && m_tessellationPatches->controlPointCount > 0;
    }

    void MeshRendererComponent::DrawTessellatedPatch(
        ID3D11InputLayout* const inputLayout) const
    {
        // 借用したD3D11コンテキスト
        auto* context = Detail::GraphicsDeviceD3D11Access::Context(
            *m_graphics);
        // 一つの制御点のバイト数
        const UINT stride =
            sizeof(DirectX::VertexPositionNormalTexture);
        // 制御点バッファの開始位置
        const UINT offset = 0;
        // 描画に借用する制御点バッファ
        ID3D11Buffer* buffers[]{
            m_tessellationPatches->vertexBuffer.Get()
        };
        context->IASetVertexBuffers(
            0,
            1,
            buffers,
            &stride,
            &offset);
        context->IASetInputLayout(
            inputLayout);
        context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
        // Applyはテセレーション有効フラグが立つ間だけHS・DSを設定する。
        m_effect->SetTessellationDrawEnabled(true);
        m_effect->Apply(context);
        context->Draw(
            m_tessellationPatches->controlPointCount,
            0);
        m_effect->SetTessellationDrawEnabled(false);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        // 後続描画へパッチ指定を残さないよう、三角形トポロジーへ戻す。
        context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void MeshRendererComponent::ApplyReflectionProbe() const
    {
        if (m_effect == nullptr)
        {
            return;
        }
        // 所有者のワールド行列
        const auto world = Owner().WorldMatrix();
        // プローブを選ぶワールド位置
        DirectX::XMFLOAT3 position{};
        DirectX::XMStoreFloat3(&position, world.r[3]);

        // シーンが選んだプローブ環境
        const auto probe = Owner().GetScene()
            .ReflectionProbeEnvironmentAt(position);
        // プローブのビューを描画まで保持し、無効なら直前に設定した環境反射を維持する。
        static_cast<void>(
            m_graphics->TrySetLitEffectReflectionProbe(
                *m_effect,
                probe));
    }

    void MeshRendererComponent::RefreshShader(
        const bool forceReload)
    {
        if (m_graphics == nullptr || !m_primitive)
        {
            return;
        }
        if (forceReload)
        {
            m_graphics->InvalidateMaterialShader(
                m_material.Shader());
        }

        // 取得したシェーダーの世代
        std::uint64_t generation{};
        // 取得時のコンパイル診断
        std::string compileError;
        // 今回選択する共有Effect
        auto* selected = &m_graphics->MaterialShader(
            m_material.Shader(),
            generation,
            compileError,
            m_material.ShaderKeywords());
        m_shaderError = std::move(compileError);
        // 後段のforwardパスもHS・DSを持てるため、制御点がなければ全色パスのテセレーションを調べる。
        // 制御点なしで分割を要するパス
        bool hasTessellatedColorPass = false;
        if (m_shaderError.empty() && !m_tessellationPatches)
        {
            // シェーダーパスか追加画像の番号
            for (std::size_t index = 0;
                index < selected->ColorPassCount();
                ++index)
            {
                selected->SelectColorPass(index);
                hasTessellatedColorPass =
                    hasTessellatedColorPass
                    || selected->SelectedPassHasTessellation(
                        ShaderPassRole::Forward);
            }
            selected->SelectColorPass(0);
        }
        // 制御点を作れない形状でテセレーションが必要なら、診断を残して代替シェーダーを要求する。
        if (hasTessellatedColorPass)
        {
            m_shaderError =
                "This shader uses tessellation (HSMain/DSMain),"
                " which only works on shapes that can be split"
                " into quad patches (Plane and Cube).";
            // 取得できた代替表示のEffect
            if (auto* const placeholder =
                    m_graphics->ShaderErrorPlaceholder(false))
            {
                selected = placeholder;
            }
        }
        // 選択した共有Effectの参照
        auto& effect = *selected;
        if (m_effect == &effect
            && m_activeShaderPath == m_material.Shader()
            && m_shaderGeneration == generation)
        {
            return;
        }

        // 色パス順に作る入力レイアウト
        std::vector<std::unique_ptr<InputLayoutHolder>>
            colorInputLayouts;
        try
        {
            // 入力レイアウトを作る色パス数
            const auto colorPassCount = effect.IsManifestEffect()
                ? effect.ColorPassCount()
                : 1u;
            if (colorPassCount == 0)
            {
                throw std::runtime_error(
                    "A material shader requires at least one color pass.");
            }
            colorInputLayouts.reserve(colorPassCount);
            // シェーダーパスか追加画像の番号
            for (std::size_t index = 0;
                // 入力レイアウトを作る色パス数
                index < colorPassCount;
                ++index)
            {
                // 対象パスの頂点シェーダー
                auto* const byteCode =
                    effect.ColorPassVertexShaderByteCode(index);
                if (byteCode == nullptr)
                {
                    throw std::runtime_error(
                        "Material color pass "
                        + std::to_string(index)
                        + " has no vertex shader bytecode.");
                }
                // 作成する入力レイアウト
                auto layout =
                    std::make_unique<InputLayoutHolder>();
                // D3D11入力レイアウト生成結果
                const HRESULT result =
                    Detail::GraphicsDeviceD3D11Access::Device(
                        *m_graphics)->CreateInputLayout(
                        DirectX::VertexPositionNormalTexture::
                            InputElements,
                        DirectX::VertexPositionNormalTexture::
                            InputElementCount,
                        byteCode->GetBufferPointer(),
                        byteCode->GetBufferSize(),
                        layout->value.ReleaseAndGetAddressOf());
                if (FAILED(result))
                {
                    throw std::runtime_error(
                        "Could not create the input layout for material "
                        "color pass " + std::to_string(index)
                        + " (HRESULT "
                        + std::to_string(
                            static_cast<unsigned long>(result))
                        + ").");
                }
                colorInputLayouts.push_back(std::move(layout));
            }
        }
        // 入力レイアウト生成時の例外
        catch (const std::exception& exception)
        {
            m_shaderError = exception.what();
            // 置換で解放済みのEffectを参照しないよう、入力レイアウト失敗時は描画参照を解除する。
            m_effect = nullptr;
            m_colorInputLayouts.clear();
            m_instancedInputLayouts.clear();
            return;
        }

        // 全バッチパスの入力レイアウトが揃わなければ、部分的にバッチ化せず通常描画へ戻す。
        // バッチパス順の入力レイアウト
        std::vector<std::unique_ptr<InputLayoutHolder>>
            instancedInputLayouts;
        // 入力レイアウトを作るバッチ数
        const auto instancedPassCount = effect.PassCount(
            ShaderPassRole::Instanced);
        if (instancedPassCount != 0)
        {
            // 頂点と変換行列・色の入力形式
            std::array<D3D11_INPUT_ELEMENT_DESC, 8>
                elements{};
            std::copy_n(
                DirectX::VertexPositionNormalTexture::
                    InputElements,
                DirectX::VertexPositionNormalTexture::
                    InputElementCount,
                elements.begin());
            // 変換行列と色の四成分形式
            constexpr auto instanceFormat =
                DXGI_FORMAT_R32G32B32A32_FLOAT;
            elements[3] = {
                "INSTANCE_TRANSFORM", 0, instanceFormat,
                1, 0,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };
            elements[4] = {
                "INSTANCE_TRANSFORM", 1, instanceFormat,
                1, 16,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };
            elements[5] = {
                "INSTANCE_TRANSFORM", 2, instanceFormat,
                1, 32,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };
            elements[6] = {
                "INSTANCE_TRANSFORM", 3, instanceFormat,
                1, 48,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };
            elements[7] = {
                "INSTANCE_COLOR", 0, instanceFormat,
                1, 64,
                D3D11_INPUT_PER_INSTANCE_DATA, 1 };
            instancedInputLayouts.reserve(instancedPassCount);
            // シェーダーパスか追加画像の番号
            for (std::size_t index = 0;
                // 入力レイアウトを作るバッチ数
                index < instancedPassCount;
                ++index)
            {
                effect.SelectPass(
                    ShaderPassRole::Instanced,
                    index);
                // 対象パスの頂点シェーダー
                auto* const byteCode =
                    effect.SelectedPassVertexShaderByteCode(
                        ShaderPassRole::Instanced);
                if (byteCode == nullptr)
                {
                    instancedInputLayouts.clear();
                    break;
                }
                // 作成する入力レイアウト
                auto layout =
                    std::make_unique<InputLayoutHolder>();
                // D3D11入力レイアウト生成結果
                const HRESULT result =
                    Detail::GraphicsDeviceD3D11Access::Device(
                        *m_graphics)->CreateInputLayout(
                        elements.data(),
                        static_cast<UINT>(elements.size()),
                        byteCode->GetBufferPointer(),
                        byteCode->GetBufferSize(),
                        layout->value.ReleaseAndGetAddressOf());
                if (FAILED(result))
                {
                    instancedInputLayouts.clear();
                    break;
                }
                instancedInputLayouts.push_back(std::move(layout));
            }
            effect.SelectPass(ShaderPassRole::Instanced, 0);
        }
        effect.SelectColorPass(0);

        m_colorInputLayouts = std::move(colorInputLayouts);
        m_instancedInputLayouts = std::move(instancedInputLayouts);
        m_effect = &effect;
        m_activeShaderPath = m_material.Shader();
        m_shaderGeneration = generation;
    }

    void MeshRendererComponent::OnInitialize(GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        m_assets = &graphics.Assets();

        if (!m_materialAssetPath.empty())
        {
            m_material = LoadLitMaterialAsset(
                m_assets->ResolvePath(m_materialAssetPath),
                &m_assets->Database(),
                m_assets);
        }

        // D3D12では共通のCPU形状と材質を描画サービスへ渡すため、D3D11資源を生成しない。
        if (graphics.ActiveRenderingApi()
            == RenderingApi::DirectX11)
        {
            BuildActivePrimitive(graphics);
            BuildTessellationPatches(graphics);
            RefreshShader(false);
        }

        if (!m_material.AlbedoTexture().empty())
        {
            m_albedoTexture = m_assets->LoadTexture(
                m_material.AlbedoTexture());
        }
        if (!m_material.NormalTexture().empty())
        {
            m_normalTexture = m_assets->LoadTexture(
                m_material.NormalTexture(),
                TextureLoader::TextureUsage::NormalMap);
        }
        if (!m_material.RoughnessTexture().empty())
        {
            m_roughnessTexture = m_assets->LoadTexture(
                m_material.RoughnessTexture(),
                TextureLoader::TextureUsage::DataMap);
        }
        if (!m_material.MetallicTexture().empty())
        {
            m_metallicTexture = m_assets->LoadTexture(
                m_material.MetallicTexture(),
                TextureLoader::TextureUsage::DataMap);
        }
        if (!m_material.OcclusionTexture().empty())
        {
            m_occlusionTexture = m_assets->LoadTexture(
                m_material.OcclusionTexture(),
                TextureLoader::TextureUsage::DataMap);
        }
        if (!m_material.EmissiveTexture().empty())
        {
            m_emissiveTexture = m_assets->LoadTexture(
                m_material.EmissiveTexture());
        }

        // シェーダーパスか追加画像の番号
        for (std::size_t index = 0;
            index < LitMaterial::CustomTextureCount;
            ++index)
        {
            // 追加画像のパス
            const auto& path =
                m_material.CustomTexture(index);
            m_customTextures[index] = path.empty()
                ? nullptr
                : m_assets->LoadTexture(path);
        }
    }

    void MeshRendererComponent::OnRender3D(
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {

        if (m_instancedThisPass)
        {
            m_instancedThisPass = false;
            return;
        }
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental)
        {
            // 現在が深度だけの描画パスか
            const bool depthOnly = m_graphics->IsDepthOnlyPass();
            if (depthOnly
                && (m_worldOverlay
                    || m_material.BaseColor().w < 1.0f))
            {
                return;
            }
            // 現在APIのメッシュ描画要求
            PrimitiveDrawRequest request;
            switch (m_shape)
            {
            case PrimitiveShape::Cube:
                request.shape = PrimitiveRenderShape::Cube;
                break;
            case PrimitiveShape::Sphere:
                request.shape = PrimitiveRenderShape::Sphere;
                break;
            case PrimitiveShape::Cylinder:
                request.shape = PrimitiveRenderShape::Cylinder;
                break;
            case PrimitiveShape::Plane:
                request.shape = PrimitiveRenderShape::Plane;
                break;
            default:
                return;
            }

            // API共通形式に写す手続き頂点
            std::vector<PrimitiveRenderVertex> proceduralVertices;
            if (HasProceduralMesh())
            {
                request.shape = PrimitiveRenderShape::Procedural;
                proceduralVertices.reserve(m_proceduralVertices.size());
                // 変換する手続きメッシュ頂点
                for (const auto& vertex : m_proceduralVertices)
                {
                    proceduralVertices.push_back({
                        vertex.position,
                        vertex.normal,
                        vertex.textureCoordinate });
                }
                request.vertices = proceduralVertices;
                request.indices = m_proceduralIndices;
            }
            DirectX::XMStoreFloat4x4(
                &request.world,
                Owner().WorldMatrix());
            DirectX::XMStoreFloat4x4(&request.view, view);
            DirectX::XMStoreFloat4x4(&request.projection, projection);
            request.baseColor = m_material.BaseColor();
            request.roughness = m_material.Roughness();
            request.metallic = m_material.Metallic();
            request.normalStrength =
                m_material.NormalStrength();
            request.occlusionStrength =
                m_material.OcclusionStrength();
            request.emissiveFactor =
                m_material.EmissiveColor();
            CopyPrimitiveLighting(m_graphics->Lighting(), request);
            if (!depthOnly)
            {
                // 位置から選んだプローブを渡し、無効なら描画側が既定の環境反射を使う。
                // プローブを選ぶワールド位置
                DirectX::XMFLOAT3 position{};
                DirectX::XMStoreFloat3(
                    &position,
                    Owner().WorldMatrix().r[3]);
                request.reflectionProbe = Owner().GetScene()
                    .ReflectionProbeEnvironmentAt(position);
            }
            if (!request.directionalShadow.texture
                && m_graphics->Shadows().IsValid())
            {
                request.directionalShadow.texture =
                    m_graphics->Shadows().ViewHandle();
            }
            if (!request.spotShadowTexture
                && m_graphics->SpotShadows().IsValid())
            {
                request.spotShadowTexture =
                    m_graphics->SpotShadows().ViewHandle();
            }
            if (!request.pointShadow.texture
                && m_graphics->PointShadows().IsValid())
            {
                request.pointShadow.texture =
                    m_graphics->PointShadows().ViewHandle();
            }
            // 描画まで保持する材質画像要求
            const auto textures = BuildLitTextureRequest();
            request.albedo = textures.albedo;
            request.normalTexture = textures.normal;
            request.roughnessTexture = textures.roughness;
            request.metallicTexture = textures.metallic;
            request.occlusionTexture = textures.occlusion;
            request.emissiveTexture = textures.emissive;
            request.fallbackTexture =
                m_graphics->WhiteTextureViewHandle();
            request.alphaBlend =
                m_worldOverlay || request.baseColor.w < 1.0f;
            request.depthTest = !m_worldOverlay;
            request.depthWrite =
                request.depthTest && !request.alphaBlend;
            request.depthOnly = depthOnly;
            if (depthOnly)
            {
                request.alphaBlend = false;
                request.depthTest = true;
                request.depthWrite = true;
            }
            // 色・深度ともカリングは明示設定を優先し、未指定なら裏面を除外する。
            request.cull = m_cullModeOverride
                ? m_cullMode
                : ShaderCullMode::Back;
            if (!m_material.Shader().empty())
            {
                // カスタム材質の描画診断を記録し、コンパイル失敗時は描画側の代替表示を使う。
                // カスタム材質の描画要求
                Detail::MaterialShaderDrawRequest material;
                material.material = &m_material;
                material.customTextures = textures.customTextures;
                material.worldOverlay = m_worldOverlay;
                // 手続きメッシュは四制御点へ変換せず、基本形状で制御点が作れる場合だけ分割描画する。
                // 共通形式に写す四制御点
                std::vector<PrimitiveRenderVertex> tessellationPatches;
                if (!HasProceduralMesh())
                {
                    // 変換する四制御点の一つ
                    for (const auto& point :
                        BuildTessellationControlPoints(m_shape))
                    {
                        tessellationPatches.push_back({
                            point.position,
                            point.normal,
                            point.textureCoordinate });
                    }
                }
                material.tessellationPatches = tessellationPatches;
                if (m_cullModeOverride)
                {
                    material.cullOverride = m_cullMode;
                }
                // 描画に使用したシェーダー世代
                std::uint64_t generation{};
                // 今回のシェーダー描画の診断
                std::string shaderError;
                static_cast<void>(m_graphics->DrawMaterialShaderPrimitive(
                    request,
                    material,
                    generation,
                    shaderError));
                m_shaderError = std::move(shaderError);
                m_shaderGeneration = generation;
                m_activeShaderPath = m_material.Shader();
                return;
            }
            static_cast<void>(m_graphics->DrawPrimitive(request));
            return;
        }
        RefreshShader(false);
        if (!m_primitive
            || m_effect == nullptr
            || m_colorInputLayouts.empty()
            || m_graphics == nullptr)
        {
            return;
        }

        // 借用したD3D11コンテキスト
        auto* const context =
            Detail::GraphicsDeviceD3D11Access::Context(*m_graphics);
        // 共有Effectをforward先頭へ同期し、全終了経路で選択パスと一時フラグ・HS・DS・GSを戻す。
        m_effect->SetInstancingEnabled(false);
        m_effect->SelectColorPass(0);
        // 描画終了時に共有状態を戻す境界
        const MaterialPassScope passScope{
            *m_effect,
            context,
            ShaderPassRole::Forward
        };

        // 複数の色パスがあっても深度と影には先頭パスだけを使い、重ね描きと基本色の半透明は省く。
        if (m_graphics->IsDepthOnlyPass())
        {
            if (m_worldOverlay
                || m_material.BaseColor().w < 1.0f)
            {
                return;
            }
            // 位置を出力するDSが欠ける不正描画を避けるため、分割用制御点がない形状は深度描画から省く。
            if (m_effect->HasTessellation()
                && !CanDrawTessellatedPatch())
            {
                return;
            }
            // 色パスと深度を一致させるため、先頭パスの宣言が非不透明合成または深度書込無効ならプリパスを省く。
            if (m_graphics->DepthPass()
                == DepthPassKind::Prepass)
            {
                // 選択した色パスの描画状態
                const auto& renderState =
                    m_effect->ColorPassRenderState(0);
                if (renderState.declared
                    && (renderState.blend
                            != ShaderBlendMode::Opaque
                        || !renderState.depthWrite))
                {
                    return;
                }
            }
            m_effect->SetMatrices(
                Owner().WorldMatrix(),
                view,
                projection);
            // 材質定数による起伏や押し出しを色パスと一致させるため、深度パスにも材質を渡す。
            m_effect->SetMaterial(m_material);
            m_effect->SetDepthOnlyEnabled(true);
            if (CanDrawTessellatedPatch())
            {
                // 分割後の形で影と深度を書き、専用パッチ経路では描画状態を明示して設定する。
                // 借用した共通の描画状態
                auto& states = Detail::GraphicsDeviceD3D11Access::States(
                    *m_graphics);
                // 合成状態に渡す定数色
                constexpr float blendFactor[4]{};
                context->OMSetBlendState(
                    states.Opaque(),
                    blendFactor,
                    0xffffffffu);
                context->OMSetDepthStencilState(
                    states.DepthDefault(),
                    0);
                context->RSSetState(states.CullNone());
                ApplyCullModeOverride();
                DrawTessellatedPatch(
                    m_colorInputLayouts.front()->value.Get());
            }
            else
            {
                // 深度描画の状態設定後に明示カリングを適用する。
                m_primitive->Draw(
                    m_effect,
                    m_colorInputLayouts.front()->value.Get(),
                    false,
                    false,
                    [this]()
                    {
                        ApplyCullModeOverride();
                    });
            }
            m_effect->SetDepthOnlyEnabled(false);
            return;
        }

        m_effect->SetMatrices(
            Owner().WorldMatrix(),
            view,
            projection);
        m_effect->SetMaterial(m_material);
        // 画像のビューと資源は一回の描画完了まで要求の中で保持する。
        // 描画まで保持する材質画像要求
        const auto textures = BuildLitTextureRequest();
        if (!m_graphics->TrySetLitEffectTextures(
                *m_effect,
                textures))
        {
            return;
        }
        if (!m_graphics->TrySetLitEffectLighting(
                *m_effect,
                m_graphics->Lighting()))
        {
            return;
        }
        ApplyReflectionProbe();
        // Manifestの全forwardパスは宣言順に実行し、直接HLSLは一つの色パスとして扱う。
        // 宣言順に描く色パスの個数
        const auto colorPassCount = m_effect->IsManifestEffect()
            ? m_effect->ColorPassCount()
            : 1u;
        // 現在の色パスの番号
        for (std::size_t passIndex = 0;
            // 宣言順に描く色パスの個数
            passIndex < colorPassCount;
            ++passIndex)
        {
            if (passIndex >= m_colorInputLayouts.size()
                || !m_colorInputLayouts[passIndex])
            {
                break;
            }
            m_effect->SelectColorPass(passIndex);
            // 現在の色パスの入力レイアウト
            auto* const inputLayout =
                m_colorInputLayouts[passIndex]->value.Get();
            // 選択した色パスの描画状態
            const auto& renderState =
                m_effect->ColorPassRenderState(passIndex);

            if (m_effect->SelectedPassHasTessellation(
                    ShaderPassRole::Forward))
            {
                if (!CanDrawTessellatedPatch())
                {
                    continue;
                }
                // 借用した共通の描画状態
                auto& states = Detail::GraphicsDeviceD3D11Access::States(
                    *m_graphics);
                // 合成状態に渡す定数色
                constexpr float blendFactor[4]{};

                if (renderState.declared)
                {
                    ApplyShaderRenderState(renderState);
                }
                else
                {
                    // 基本色のアルファが1未満か
                    const bool translucent =
                        m_material.BaseColor().w < 1.0f;
                    context->OMSetBlendState(
                        translucent
                            ? states.NonPremultiplied()
                            : states.Opaque(),
                        blendFactor,
                        0xffffffffu);
                    context->OMSetDepthStencilState(
                        translucent
                            ? states.DepthRead()
                            : states.DepthDefault(),
                        0);
                    context->RSSetState(states.CullNone());
                }

                if (m_worldOverlay)
                {
                    context->OMSetDepthStencilState(
                        states.DepthNone(),
                        0);
                }
                ApplyCullModeOverride();
                DrawTessellatedPatch(inputLayout);
                continue;
            }

            if (m_worldOverlay)
            {
                // 通常の状態設定後に深度なしの透過と明示カリングを適用する。
                m_primitive->Draw(
                    m_effect,
                    inputLayout,
                    true,
                    false,
                    [this]()
                    {
                        // 描画中のD3D11コンテキスト
                        auto* drawContext =
                            Detail::GraphicsDeviceD3D11Access::Context(
                                *m_graphics);
                        // 借用した共通の描画状態
                        auto& states =
                            Detail::GraphicsDeviceD3D11Access::States(
                                *m_graphics);
                        // 合成状態に渡す定数色
                        constexpr float blendFactor[4]{};
                        drawContext->OMSetBlendState(
                            states.NonPremultiplied(),
                            blendFactor,
                            0xffffffffu);
                        drawContext->OMSetDepthStencilState(
                            states.DepthNone(),
                            0);
                        drawContext->RSSetState(
                            states.CullCounterClockwise());
                        ApplyCullModeOverride();
                    });
                continue;
            }


            if (renderState.declared)
            {
                // 通常の状態設定後に選択パスの合成・深度・カリングを適用する。
                m_primitive->Draw(
                    m_effect,
                    inputLayout,
                    renderState.blend
                        != ShaderBlendMode::Opaque,
                    false,
                    [this, &renderState]()
                    {
                        ApplyShaderRenderState(renderState);
                    });
                continue;
            }
            // 通常の状態設定後に明示カリングを適用する。
            m_primitive->Draw(
                m_effect,
                inputLayout,
                m_material.BaseColor().w < 1.0f,
                false,
                [this]()
                {
                    ApplyCullModeOverride();
                });
        }
    }

    bool MeshRendererComponent::IsAlphaBlended3D() const
    {
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental)
        {
            if (!m_material.Shader().empty())
            {
                // 判定するパスの描画状態
                ShaderRenderState state;
                if (m_graphics->TryGetMaterialShaderRenderState(
                        m_material.Shader(),
                        m_material.ShaderKeywords(),
                        state)
                    && state.declared)
                {
                    return state.blend == ShaderBlendMode::Alpha
                        || state.blend
                            == ShaderBlendMode::Premultiplied;
                }
            }
            return m_material.BaseColor().w < 1.0f;
        }
        // 共有Effectの置換後に古いポインターを参照しないよう、能力確認でもシェーダーを同期する。
        const_cast<MeshRendererComponent*>(this)
            ->RefreshShader(false);
        // 整列対象と実際の合成が食い違わないよう、透過の判定条件は描画経路と合わせて変更する。
        if (m_effect != nullptr)
        {
            // 全色パスに描画状態宣言があるか
            bool allPassesDeclareState =
                m_effect->ColorPassCount() != 0;
            // 色またはバッチパスの番号
            for (std::size_t index = 0;
                index < m_effect->ColorPassCount();
                ++index)
            {
                // 判定するパスの描画状態
                const auto& state =
                    m_effect->ColorPassRenderState(index);
                allPassesDeclareState =
                    allPassesDeclareState && state.declared;
                // 一つでもアルファ系の合成があれば整列対象とし、純加算だけなら整列を必要としない。
                if (state.declared
                    && (state.blend == ShaderBlendMode::Alpha
                        || state.blend
                            == ShaderBlendMode::Premultiplied))
                {
                    return true;
                }
            }
            if (allPassesDeclareState)
            {
                return false;
            }
        }
        // 宣言を取得できないパスがあれば、基本色のアルファで整列区分を判定する。
        return m_material.BaseColor().w < 1.0f;
    }

    bool MeshRendererComponent::CanBeInstanced() const
    {
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental)
        {
            if (m_worldOverlay
                || HasProceduralMesh()
                || m_graphics->IsDepthOnlyPass())
            {
                return false;
            }
            if (m_material.Shader().empty())
            {
                return true;
            }
            try
            {
                return m_graphics->PrepareMaterialShaderPasses(
                    m_material.Shader(),
                    m_material.ShaderKeywords()).instanced;
            }
            catch (...)
            {
                return false;
            }
        }

        // 個別描画より先に能力を調べるため、共有Effectの世代とポインターをここで同期する。
        const_cast<MeshRendererComponent*>(this)
            ->RefreshShader(false);
        // バッチの入力形式が全パスで有効か調べる(layout: 判定する入力レイアウト)。
        if (m_worldOverlay
            || HasProceduralMesh()
            || m_primitive == nullptr
            || m_effect == nullptr
            || !m_effect->SupportsInstancing()
            || m_instancedInputLayouts.empty()
            || m_instancedInputLayouts.size()
                != m_effect->PassCount(
                    ShaderPassRole::Instanced)
            || !std::ranges::all_of(
                m_instancedInputLayouts,
                [](const auto& layout)
                {
                    return layout != nullptr
                        && layout->value != nullptr;
                })
            || m_graphics == nullptr)
        {
            return false;
        }

        // 整列を要するバッチパスがあるか
        bool hasOrderDependentInstancedPass{};
        // 色またはバッチパスの番号
        for (std::size_t index = 0;
            index < m_effect->PassCount(
                ShaderPassRole::Instanced);
            ++index)
        {
            m_effect->SelectPass(
                ShaderPassRole::Instanced,
                index);
            // 判定するパスの描画状態
            const auto& state =
                m_effect->SelectedPassRenderState(
                    ShaderPassRole::Instanced);
            // アルファ系はオブジェクトを遠い順に整列するため、一括描画の対象から除く。
            // 現在のパスで遠近整列を要するか
            const bool orderDependent = state.declared
                ? state.blend == ShaderBlendMode::Alpha
                    || state.blend
                        == ShaderBlendMode::Premultiplied
                : m_material.BaseColor().w < 1.0f;
            if (orderDependent)
            {
                hasOrderDependentInstancedPass = true;
                break;
            }
        }
        m_effect->SelectPass(ShaderPassRole::Instanced, 0);
        return !hasOrderDependentInstancedPass;
    }

    std::uint64_t
        MeshRendererComponent::InstanceBatchKey()
            const noexcept
    {
        // 材質と描画設定をFNV-1aでまとめ、組み込みLitの基本色だけはインスタンス属性として分離する。
        // FNV-1aで更新するバッチキー
        std::uint64_t hash = 14695981039346656037ull;
        // 生のバイト列をキーへ追加する(data: sizeバイトのデータ先頭, size: 追加するバイト数)。
        const auto combineBytes =
            [&hash](
                const void* data,
                const std::size_t size) noexcept
        {
            // ハッシュに追加するバイト列
            const auto* bytes =
                static_cast<const unsigned char*>(data);
            // 追加するバイト列の番号
            for (std::size_t index = 0;
                index < size;
                ++index)
            {
                hash ^= bytes[index];
                hash *= 1099511628211ull;
            }
        };
        // 浮動小数点値のビット列をキーへ追加する(value: 追加する値)。
        const auto combineFloat =
            [&combineBytes](const float value) noexcept
        {
            combineBytes(&value, sizeof(value));
        };
        // 個数を64ビットへ揃えてキーへ追加する(value: 追加する個数)。
        const auto combineCount =
            [&combineBytes](const std::size_t value) noexcept
        {
            // 64ビットに揃えた個数値
            const auto fixedWidth =
                static_cast<std::uint64_t>(value);
            combineBytes(&fixedWidth, sizeof(fixedWidth));
        };
        // 四成分を順にキーへ追加する(value: 追加する四成分の値)。
        const auto combineFloat4 =
            [&combineFloat](
                const DirectX::XMFLOAT4& value) noexcept
        {
            combineFloat(value.x);
            combineFloat(value.y);
            combineFloat(value.z);
            combineFloat(value.w);
        };
        // パスのネイティブ文字列と長さをキーへ追加する(path: 追加する画像等のパス)。
        const auto combinePath =
            [&combineBytes, &combineCount](
                const std::filesystem::path& path)
        {
            // パスのネイティブ文字列
            const auto& native = path.native();
            // パス文字列のバイト数
            const auto byteCount = native.size()
                * sizeof(
                    std::filesystem::path::value_type);
            combineCount(native.size());
            combineBytes(
                native.data(),
                byteCount);
        };
        // 32ビットに揃えた形状値
        const auto shape =
            static_cast<std::uint32_t>(m_shape);
        combineBytes(&shape, sizeof(shape));
        combinePath(m_material.Shader());
        combinePath(m_material.AlbedoTexture());
        combinePath(m_material.NormalTexture());
        combinePath(m_material.RoughnessTexture());
        combinePath(m_material.MetallicTexture());
        combinePath(m_material.OcclusionTexture());
        combinePath(m_material.EmissiveTexture());
        // キーに含める追加画像パス
        for (const auto& texture : m_material.CustomTextures())
        {
            combinePath(texture);
        }
        combineFloat(m_material.Roughness());
        combineFloat(m_material.NormalStrength());
        combineFloat(m_material.Metallic());
        combineFloat(m_material.OcclusionStrength());
        // キーに含める発光のRGB倍率
        const auto& emissive = m_material.EmissiveColor();
        combineFloat(emissive.x);
        combineFloat(emissive.y);
        combineFloat(emissive.z);
        // カスタムシェーダーは材質の基本色を直接読めるため、異なる色の部品を同じ代表材質へまとめない。
        if (!m_material.Shader().empty())
        {
            combineFloat4(m_material.BaseColor());
        }
        // 基本色のアルファが1未満か
        const bool alpha =
            m_material.BaseColor().w < 1.0f;
        combineBytes(&alpha, sizeof(alpha));
        combineBytes(&m_worldOverlay, sizeof(m_worldOverlay));
        combineBytes(&m_cullModeOverride, sizeof(m_cullModeOverride));
        combineBytes(&m_cullMode, sizeof(m_cullMode));
        // キーに含めるカスタム定数
        for (const auto& parameter :
            m_material.CustomParameters())
        {
            combineFloat4(parameter);
        }
        // キーに含めるカスタムベクトル
        for (const auto& vector : m_material.CustomVectors())
        {
            combineFloat4(vector);
        }
        // 整列済みのキーワード一覧
        const auto& keywords =
            m_material.ShaderKeywords().Keywords();
        combineCount(keywords.size());
        // キーに含めるキーワード
        for (const auto& keyword : keywords)
        {
            combineCount(keyword.size());
            combineBytes(
                keyword.data(),
                keyword.size());
        }
        // 同じパスでも置換途中の異なるEffectを混ぜないよう、現在のポインターもキーに含める。
        // 現在の共有Effectの識別値
        const auto effectIdentity = reinterpret_cast<
            std::uintptr_t>(m_effect);
        combineBytes(&effectIdentity, sizeof(effectIdentity));
        return hash;
    }

    void MeshRendererComponent::RenderInstancedBatch(
        const std::vector<MeshRendererComponent*>& batch,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        if (m_graphics != nullptr
            && m_graphics->ActiveRenderingApi()
                == RenderingApi::DirectX12Experimental)
        {
            if (batch.size() < 2 || !CanBeInstanced())
            {
                return;
            }
            // 代表のthisの材質・光源・プローブを共有し、各部品のワールド変換と色を別属性で渡す。
            // 各部品の変換行列と色の配列
            std::vector<Detail::MaterialShaderInstanceData> instances;
            // D3D12で描画する部品一覧
            std::vector<MeshRendererComponent*> instancedComponents;
            instances.reserve(batch.size());
            instancedComponents.reserve(batch.size());
            // バッチに含まれる描画部品
            for (auto* const component : batch)
            {
                if (component == nullptr || !component->CanBeInstanced())
                {
                    continue;
                }
                // 追加する部品の変換行列と色
                Detail::MaterialShaderInstanceData instance{};
                DirectX::XMStoreFloat4x4(
                    &instance.world,
                    component->Owner().WorldMatrix());
                instance.color = component->m_material.BaseColor();
                instances.push_back(instance);
                instancedComponents.push_back(component);
            }
            if (instances.size() < 2)
            {
                return;
            }

            // API共通のバッチ描画要求
            PrimitiveDrawRequest request;
            switch (m_shape)
            {
            case PrimitiveShape::Cube:
                request.shape = PrimitiveRenderShape::Cube;
                break;
            case PrimitiveShape::Sphere:
                request.shape = PrimitiveRenderShape::Sphere;
                break;
            case PrimitiveShape::Cylinder:
                request.shape = PrimitiveRenderShape::Cylinder;
                break;
            case PrimitiveShape::Plane:
                request.shape = PrimitiveRenderShape::Plane;
                break;
            default:
                return;
            }
            DirectX::XMStoreFloat4x4(
                &request.world,
                DirectX::XMMatrixIdentity());
            DirectX::XMStoreFloat4x4(&request.view, view);
            DirectX::XMStoreFloat4x4(&request.projection, projection);
            request.baseColor = m_material.BaseColor();
            request.roughness = m_material.Roughness();
            request.metallic = m_material.Metallic();
            request.normalStrength = m_material.NormalStrength();
            request.occlusionStrength = m_material.OcclusionStrength();
            request.emissiveFactor = m_material.EmissiveColor();
            CopyPrimitiveLighting(m_graphics->Lighting(), request);
            // 代表のプローブを選ぶ位置
            DirectX::XMFLOAT3 position{};
            DirectX::XMStoreFloat3(
                &position,
                Owner().WorldMatrix().r[3]);
            request.reflectionProbe = Owner().GetScene()
                .ReflectionProbeEnvironmentAt(position);
            if (!request.directionalShadow.texture
                && m_graphics->Shadows().IsValid())
            {
                request.directionalShadow.texture =
                    m_graphics->Shadows().ViewHandle();
            }
            if (!request.spotShadowTexture
                && m_graphics->SpotShadows().IsValid())
            {
                request.spotShadowTexture =
                    m_graphics->SpotShadows().ViewHandle();
            }
            if (!request.pointShadow.texture
                && m_graphics->PointShadows().IsValid())
            {
                request.pointShadow.texture =
                    m_graphics->PointShadows().ViewHandle();
            }
            // 描画まで保持する材質画像要求
            const auto textures = BuildLitTextureRequest();
            request.albedo = textures.albedo;
            request.normalTexture = textures.normal;
            request.roughnessTexture = textures.roughness;
            request.metallicTexture = textures.metallic;
            request.occlusionTexture = textures.occlusion;
            request.emissiveTexture = textures.emissive;
            request.fallbackTexture = m_graphics->WhiteTextureViewHandle();
            request.alphaBlend = request.baseColor.w < 1.0f;
            request.depthTest = true;
            request.depthWrite = !request.alphaBlend;
            request.cull = m_cullModeOverride
                ? m_cullMode
                : ShaderCullMode::Back;

            // バッチの描画に成功したか
            bool drawn{};
            if (m_material.Shader().empty())
            {

                request.instances = instances;
                drawn = m_graphics->DrawPrimitive(request);
            }
            else
            {
                // カスタム材質のバッチ描画要求
                Detail::MaterialShaderDrawRequest material;
                material.material = &m_material;
                material.instances = instances;
                material.customTextures = textures.customTextures;
                if (m_cullModeOverride)
                {
                    material.cullOverride = m_cullMode;
                }
                // 描画に使用したシェーダー世代
                std::uint64_t generation{};
                // 今回のシェーダー描画の診断
                std::string shaderError;
                drawn = m_graphics->DrawMaterialShaderPrimitive(
                    request,
                    material,
                    generation,
                    shaderError);
                m_shaderError = std::move(shaderError);
                m_shaderGeneration = generation;
                m_activeShaderPath = m_material.Shader();
            }
            if (!drawn)
            {
                return;
            }

            // バッチに含まれる描画部品
            for (auto* const component : instancedComponents)
            {
                component->m_instancedThisPass = true;
            }
            return;
        }
        if (batch.empty())
        {
            return;
        }
        // 個別描画を省く前に全部品のEffectを同期し、置換後の古いポインターを使わない。
        // 代表のEffectも同期済みか
        bool refreshedThis{};
        // バッチに含まれる描画部品
        for (auto* const component : batch)
        {
            if (component == nullptr)
            {
                return;
            }
            component->RefreshShader(false);
            refreshedThis = refreshedThis || component == this;
        }
        if (!refreshedThis)
        {
            RefreshShader(false);
        }
        if (!CanBeInstanced())
        {
            return;
        }
        // 同期後の代表のバッチキー
        const auto refreshedBatchKey = InstanceBatchKey();
        // 同期後も全員が代表と同種か調べる(component: 判定する描画部品)。
        if (std::ranges::any_of(
            batch,
            [refreshedBatchKey](
                const MeshRendererComponent* const component)
            {
                return !component->CanBeInstanced()
                    || component->InstanceBatchKey()
                        != refreshedBatchKey;
            }))
        {
            return;
        }

        struct InstanceData final
        {
            // 部品のワールド行列
            DirectX::XMFLOAT4X4 world;
            // 部品の基本色RGBA
            DirectX::XMFLOAT4 color;
        };
        // slot 1の行列四行と色は80バイトの入力形式を維持する。
        static_assert(sizeof(InstanceData) == 80);

        // 各部品の変換行列と色の配列
        std::vector<InstanceData> instances;
        instances.reserve(batch.size());
        // バッチに含まれる描画部品
        for (const auto* component : batch)
        {
            // 配列へ追加する部品の変換と色
            InstanceData data{};
            DirectX::XMStoreFloat4x4(
                &data.world,
                component->Owner().WorldMatrix());
            data.color =
                component->m_material.BaseColor();
            instances.push_back(data);
        }

        // 描画用のインスタンス属性資源
        const auto instanceBuffer =
            m_graphics->AcquireInstanceBufferHandle(
                std::as_bytes(std::span{ instances }));
        if (!instanceBuffer)
        {
            return;
        }

        m_effect->SetMatrices(
            DirectX::XMMatrixIdentity(),
            view,
            projection);
        m_effect->SetMaterial(m_material);
        // 描画が完了するまで要求内の画像ビューを保持する。
        // 描画まで保持する材質画像要求
        const auto textures = BuildLitTextureRequest();
        if (!m_graphics->TrySetLitEffectTextures(
                *m_effect,
                textures))
        {
            return;
        }
        if (!m_graphics->TrySetLitEffectLighting(
                *m_effect,
                m_graphics->Lighting()))
        {
            return;
        }
        // 全バッチパスで代表のthisの位置のプローブを共有する。
        ApplyReflectionProbe();
        // 借用したD3D11コンテキスト
        auto* const context =
            Detail::GraphicsDeviceD3D11Access::Context(*m_graphics);
        // 描画終了時に共有状態を戻す境界
        const MaterialPassScope passScope{
            *m_effect,
            context,
            ShaderPassRole::Instanced
        };
        m_effect->SetTessellationDrawEnabled(false);
        m_effect->SetInstancingEnabled(true);
        // 全instancedパスを宣言順に描き、直接HLSLは一パスとして扱う。
        // 宣言順に描くバッチパスの個数
        const auto passCount = m_effect->PassCount(
            ShaderPassRole::Instanced);
        // 現在のバッチパスの番号
        for (std::size_t passIndex = 0;
            // 宣言順に描くバッチパスの個数
            passIndex < passCount;
            ++passIndex)
        {
            if (passIndex >= m_instancedInputLayouts.size()
                || !m_instancedInputLayouts[passIndex])
            {
                break;
            }
            m_effect->SelectPass(
                ShaderPassRole::Instanced,
                passIndex);
            // 現在のバッチパスの描画状態
            const auto& renderState =
                m_effect->SelectedPassRenderState(
                    ShaderPassRole::Instanced);
            // 状態設定後にインスタンス属性をslot 1へ結び、選択パスの描画状態を適用する。
            m_primitive->DrawInstanced(
                m_effect,
                m_instancedInputLayouts[passIndex]->value.Get(),
                static_cast<std::uint32_t>(instances.size()),
                renderState.declared
                    ? renderState.blend != ShaderBlendMode::Opaque
                    : m_material.BaseColor().w < 1.0f,
                false,
                0,
                [this, instanceBuffer, &renderState]
            {
                m_graphics->BindVertexBuffer(
                    instanceBuffer,
                    1,
                    static_cast<std::uint32_t>(
                        sizeof(InstanceData)));
                if (renderState.declared)
                {
                    ApplyShaderRenderState(renderState);
                }
                else
                {
                    ApplyCullModeOverride();
                }
                });
        }


        // バッチに含まれる描画部品
        for (auto* component : batch)
        {
            component->m_instancedThisPass = true;
        }
    }

    bool MeshRendererComponent::DescribeDrawEvent(
        FrameDebugDrawDescription& description) const
    {

        if (m_instancedThisPass)
        {
            return false;
        }
        if (HasProceduralMesh())
        {
            description.geometry = "手続きメッシュ";
            description.vertexCount = m_proceduralVertices.size();
            description.triangleCount = m_proceduralIndices.size() / 3u;
        }
        else
        {
            switch (m_shape)
            {
            case PrimitiveShape::Cube:
                description.geometry = "立方体";
                break;
            case PrimitiveShape::Sphere:
                description.geometry = "球";
                break;
            case PrimitiveShape::Cylinder:
                description.geometry = "円柱";
                break;
            case PrimitiveShape::Plane:
                description.geometry = "平面";
                break;
            }
        }
        description.material = Detail::FrameDebugMaterialLabel(
            m_materialAssetPath,
            m_material.Shader(),
            m_material.AlbedoTexture());
        Detail::AppendFrameDebugItem(
            description.state,
            IsAlphaBlended3D() ? "アルファ合成" : "不透明");
        Detail::AppendFrameDebugItem(
            description.state,
            Detail::FrameDebugCullLabel(m_cullMode));
        if (m_worldOverlay)
        {
            Detail::AppendFrameDebugItem(description.state, "最前面表示");
        }
        return true;
    }
}
