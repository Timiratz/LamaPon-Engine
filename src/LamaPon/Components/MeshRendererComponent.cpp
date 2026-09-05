#include "LamaPon/Components/MeshRendererComponent.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/ReflectionProbeComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/LitMaterialAsset.h"
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
#include <stdexcept>
#include <utility>

namespace
{
    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT3& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z);
    }

    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT2& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y);
    }

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

    [[nodiscard]] std::unique_ptr<DirectX::GeometricPrimitive>
        CreateProceduralPrimitive(
            ID3D11DeviceContext* context,
            const std::vector<LamaPon::ProceduralMeshVertex>& vertices,
            const std::vector<std::uint32_t>& indices)
    {
        DirectX::GeometricPrimitive::VertexCollection convertedVertices;
        convertedVertices.reserve(vertices.size());
        for (const auto& vertex : vertices)
        {
            convertedVertices.push_back({
                vertex.position,
                vertex.normal,
                vertex.textureCoordinate,
            });
        }
        DirectX::GeometricPrimitive::IndexCollection convertedIndices;
        convertedIndices.reserve(indices.size());
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

    // 読み込み済みテクスチャとマテリアル値から、Effectへ渡す
    // PBRマップ一式を組み立てます。未設定はnullptrのままにして、
    // シェーダー側では「マップなし」として扱わせます。
    LamaPon::LitEffect::PbrTextures BuildPbrTextures(
        const std::shared_ptr<
            const LamaPon::TextureAsset>& roughness,
        const std::shared_ptr<
            const LamaPon::TextureAsset>& metallic,
        const std::shared_ptr<
            const LamaPon::TextureAsset>& occlusion,
        const std::shared_ptr<
            const LamaPon::TextureAsset>& emissive,
        const LamaPon::LitMaterial& material) noexcept
    {
        LamaPon::LitEffect::PbrTextures textures{};
        textures.roughness = roughness
            ? roughness->view.Get()
            : nullptr;
        textures.metallic = metallic
            ? metallic->view.Get()
            : nullptr;
        textures.occlusion = occlusion
            ? occlusion->view.Get()
            : nullptr;
        textures.emissive = emissive
            ? emissive->view.Get()
            : nullptr;
        textures.occlusionStrength =
            material.OcclusionStrength();
        textures.emissiveFactor = material.EmissiveColor();
        return textures;
    }
}

namespace LamaPon
{
    void MeshRendererComponent::ApplyShaderRenderState(
        const ShaderRenderState& state) const
    {
        auto* context = m_graphics->Context();
        auto& states = m_graphics->States();
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
            // DirectXTKのAdditiveはSrcAlpha加重で、しかも書き込み先の
            // アルファへsrcAを積み上げる。シーンバッファのアルファは
            // 後段が意味を持って読むため、汚さない純加算を使う
            // （ShaderRenderState.hのCreateAdditiveBlendPreservingAlpha参照）。
            auto* const additive =
                m_graphics->AdditiveBlendPreservingAlpha();
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
        auto* context = m_graphics->Context();
        auto& states = m_graphics->States();
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

    std::array<
        ID3D11ShaderResourceView*,
        LitMaterial::CustomTextureCount>
        MeshRendererComponent::ResolveCustomTextureViews() const noexcept
    {
        // 未設定の枠はnullptrにします（LitEffect側で白へ差し替え）。
        std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount> views{};
        for (std::size_t index = 0;
            index < views.size();
            ++index)
        {
            views[index] = m_customTextures[index]
                ? m_customTextures[index]->view.Get()
                : nullptr;
        }
        return views;
    }

    struct MeshRendererComponent::InputLayoutHolder final
    {
        Microsoft::WRL::ComPtr<ID3D11InputLayout> value;
    };

    struct MeshRendererComponent::TessellationPatchHolder final
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        // 4制御点パッチの本数×4。Planeは1面で4、Cubeは6面で24です。
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
            for (auto& vertex : vertices)
            {
                vertex.normal = {};
            }
            for (std::size_t index{}; index < indices.size(); index += 3u)
            {
                auto& first = vertices[indices[index]];
                auto& second = vertices[indices[index + 1u]];
                auto& third = vertices[indices[index + 2u]];
                const auto edgeA = DirectX::XMVectorSubtract(
                    DirectX::XMLoadFloat3(&second.position),
                    DirectX::XMLoadFloat3(&first.position));
                const auto edgeB = DirectX::XMVectorSubtract(
                    DirectX::XMLoadFloat3(&third.position),
                    DirectX::XMLoadFloat3(&first.position));
                DirectX::XMFLOAT3 faceNormal{};
                DirectX::XMStoreFloat3(
                    &faceNormal,
                    DirectX::XMVector3Cross(edgeA, edgeB));
                for (auto* vertex : { &first, &second, &third })
                {
                    vertex->normal.x += faceNormal.x;
                    vertex->normal.y += faceNormal.y;
                    vertex->normal.z += faceNormal.z;
                }
            }
        }
        for (auto& vertex : vertices)
        {
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

        DirectX::XMFLOAT3 minimum = vertices.front().position;
        DirectX::XMFLOAT3 maximum = vertices.front().position;
        for (const auto& vertex : vertices)
        {
            minimum.x = std::min(minimum.x, vertex.position.x);
            minimum.y = std::min(minimum.y, vertex.position.y);
            minimum.z = std::min(minimum.z, vertex.position.z);
            maximum.x = std::max(maximum.x, vertex.position.x);
            maximum.y = std::max(maximum.y, vertex.position.y);
            maximum.z = std::max(maximum.z, vertex.position.z);
        }

        std::unique_ptr<DirectX::GeometricPrimitive> primitive;
        if (m_graphics != nullptr)
        {
            // GPU作成が失敗した場合は、現在表示中のメッシュとCPU側の
            // データを両方そのまま残す（強い例外保証）。
            primitive = CreateProceduralPrimitive(
                m_graphics->Context(),
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
        std::unique_ptr<DirectX::GeometricPrimitive> primitive;
        if (m_graphics != nullptr)
        {
            primitive = CreatePrimitiveShape(
                m_graphics->Context(),
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

    void MeshRendererComponent::ApplyMaterial(
        const LitMaterial& material)
    {
        std::shared_ptr<const TextureAsset> albedo;
        std::shared_ptr<const TextureAsset> normal;
        std::shared_ptr<const TextureAsset> roughness;
        std::shared_ptr<const TextureAsset> metallic;
        std::shared_ptr<const TextureAsset> occlusion;
        std::shared_ptr<const TextureAsset> emissive;
        if (m_assets != nullptr)
        {
            // スロットごとに用途を渡します。法線はBC5、粗さ・
            // 金属度・遮蔽はBC1、色はBC1/BC3です。
            using Usage = TextureLoader::TextureUsage;
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
        }

        m_material = material;
        m_albedoTexture = std::move(albedo);
        m_normalTexture = std::move(normal);
        m_roughnessTexture = std::move(roughness);
        m_metallicTexture = std::move(metallic);
        m_occlusionTexture = std::move(occlusion);
        m_emissiveTexture = std::move(emissive);
        RefreshShader(false);
    }

    void MeshRendererComponent::BuildTessellationPatches(
        GraphicsDevice& graphics)
    {
        m_tessellationPatches.reset();
        if (HasProceduralMesh())
        {
            // 任意三角形を4制御点パッチへ自動変換はできません。
            // テセレーションShaderはRefreshShaderで代役へ倒します。
            return;
        }

        // 1つの四角パッチ。制御点の並びは Plane から続く
        // (u0,v0) (u1,v0) (u0,v1) (u1,v1) で、u×v が法線になる向きに
        // 揃えます。揃えておくと、面ごとに表裏が裏返りません。
        std::vector<DirectX::VertexPositionNormalTexture>
            controlPoints;
        const auto addQuad =
            [&controlPoints](
                const DirectX::XMFLOAT3& center,
                const DirectX::XMFLOAT3& uAxis,
                const DirectX::XMFLOAT3& vAxis,
                const DirectX::XMFLOAT3& normal)
        {
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

        switch (m_shape)
        {
        case PrimitiveShape::Plane:
            // 原作の SnowSurface と同じ並びの4制御点になります。
            addQuad(
                { 0.0f, 0.0f, 0.0f },
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, -1.0f },
                { 0.0f, 1.0f, 0.0f });
            break;

        case PrimitiveShape::Cube:
            // 6面をそれぞれ1枚の四角パッチにします。面の中心は
            // 法線方向へ0.5（DirectXTKのCubeは一辺1）。
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
            // Sphere／Cylinderは四角パッチに割れません。作らない
            // ことが「この形では使えない」の印になります。
            return;
        }

        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(
            controlPoints.size()
            * sizeof(DirectX::VertexPositionNormalTexture));
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = controlPoints.data();

        auto patches =
            std::make_unique<TessellationPatchHolder>();
        patches->controlPointCount =
            static_cast<UINT>(controlPoints.size());
        const HRESULT result =
            graphics.Device()->CreateBuffer(
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
                graphics.Context(),
                m_proceduralVertices,
                m_proceduralIndices)
            : CreatePrimitiveShape(
                graphics.Context(),
                m_shape);
    }

    bool MeshRendererComponent::CanDrawTessellatedPatch()
        const noexcept
    {
        // 形状名への個別対応を不要にするため、テセレーション対応は
        // 入力レイアウトと制御点バッファの生成結果で判定します。
        return m_effect != nullptr
            && m_effect->HasTessellation()
            && m_inputLayout
            && m_tessellationPatches
            && m_tessellationPatches->vertexBuffer
            && m_tessellationPatches->controlPointCount > 0;
    }

    void MeshRendererComponent::DrawTessellatedPatch() const
    {
        auto* context = m_graphics->Context();
        const UINT stride =
            sizeof(DirectX::VertexPositionNormalTexture);
        const UINT offset = 0;
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
            m_inputLayout->value.Get());
        context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
        // パッチで描くのはここだけです。Applyはこの指定を見て
        // ハル／ドメインを束ねます（既定では束ねません）。
        m_effect->SetTessellationDrawEnabled(true);
        m_effect->Apply(context);
        context->Draw(
            m_tessellationPatches->controlPointCount,
            0);
        m_effect->SetTessellationDrawEnabled(false);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        // ハル／ドメインシェーダーを解除した後は三角形トポロジーへ戻し、
        // 後続の描画がパッチトポロジーを引き継がないようにします。
        context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void MeshRendererComponent::ApplyReflectionProbe() const
    {
        if (m_effect == nullptr)
        {
            return;
        }
        const auto world = Owner().WorldMatrix();
        DirectX::XMFLOAT3 position{};
        DirectX::XMStoreFloat3(&position, world.r[3]);
        // プローブの選択とブレンドの比率はScene側が決めます
        // （ModelRendererと同じ手順を1箇所に置くため）。
        m_effect->SetEnvironmentOverride(
            Owner().GetScene()
                .ReflectionProbeEnvironmentAt(position));
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

        std::uint64_t generation{};
        std::string compileError;
        auto* selected = &m_graphics->MaterialShader(
            m_material.Shader(),
            generation,
            compileError,
            m_material.ShaderKeywords());
        m_shaderError = std::move(compileError);
        // テセレーションは四角パッチ用の制御点を生成できるPlaneとCubeに
        // 対応します。Sphere／CylinderではHSMain/DSMainを実行できず、
        // テセレーション用頂点シェーダーだけでは描画が成立しません。
        // 非対応形状では描画消失を避けるため代替シェーダーを使用します。
        //
        // 対応判定には形状名ではなく制御点バッファの有無を使用し、
        // 新しい対応形状にも同じ条件を適用します。
        if (m_shaderError.empty()
            && selected->HasTessellation()
            && !m_tessellationPatches)
        {
            m_shaderError =
                "This shader uses tessellation (HSMain/DSMain),"
                " which only works on shapes that can be split"
                " into quad patches (Plane and Cube).";
            if (auto* const placeholder =
                    m_graphics->ShaderErrorPlaceholder(false))
            {
                selected = placeholder;
            }
        }
        auto& effect = *selected;
        if (m_effect == &effect
            && m_activeShaderPath == m_material.Shader()
            && m_shaderGeneration == generation)
        {
            return;
        }

        auto inputLayout = std::make_unique<InputLayoutHolder>();
        try
        {
            m_primitive->CreateInputLayout(
                &effect,
                inputLayout->value.ReleaseAndGetAddressOf());
        }
        catch (const std::exception& exception)
        {
            m_shaderError = exception.what();
            // 入力レイアウトを作れなかったときにm_effectを古いまま
            // 残すと、破棄済みのシェーダーを指したままになります。
            // 描かない方を選びます。
            m_effect = nullptr;
            m_inputLayout.reset();
            return;
        }
        m_inputLayout = std::move(inputLayout);

        // インスタンス描画用の入力レイアウト（対応シェーダーのみ）。
        m_instancedInputLayout.reset();
        if (effect.SupportsInstancing()
            && effect.InstancedVertexShaderByteCode()
                != nullptr)
        {
            std::array<D3D11_INPUT_ELEMENT_DESC, 8>
                elements{};
            std::copy_n(
                DirectX::VertexPositionNormalTexture::
                    InputElements,
                DirectX::VertexPositionNormalTexture::
                    InputElementCount,
                elements.begin());
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
            auto* byteCode =
                effect.InstancedVertexShaderByteCode();
            auto instancedLayout =
                std::make_unique<InputLayoutHolder>();
            if (SUCCEEDED(
                m_graphics->Device()->CreateInputLayout(
                    elements.data(),
                    static_cast<UINT>(elements.size()),
                    byteCode->GetBufferPointer(),
                    byteCode->GetBufferSize(),
                    instancedLayout->value
                        .ReleaseAndGetAddressOf())))
            {
                m_instancedInputLayout =
                    std::move(instancedLayout);
            }
        }

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

        BuildActivePrimitive(graphics);

        BuildTessellationPatches(graphics);

        RefreshShader(false);

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
        // カスタムShaderが宣言した追加テクスチャ（t7以降）。
        for (std::size_t index = 0;
            index < LitMaterial::CustomTextureCount;
            ++index)
        {
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
        // このパスで既にインスタンス描画済みならスキップします。
        if (m_instancedThisPass)
        {
            m_instancedThisPass = false;
            return;
        }
        RefreshShader(false);
        if (!m_primitive
            || m_effect == nullptr
            || !m_inputLayout
            || m_graphics == nullptr)
        {
            return;
        }

        // ジオメトリシェーダーを束ねたまま抜けると、スプライトや
        // ポスト処理などGSを設定しない経路まで巻き込みます
        // （ハル／ドメインとまったく同じ理由）。抜け道が多い関数なので
        // RAIIで外します。
        const GeometryShaderScope geometryScope{
            m_effect->HasGeometryShader()
                ? m_graphics->Context()
                : nullptr
        };

        // 深度のみのパス（シャドウ／深度プリパス）。半透明は従来から
        // 深度を書かないため影を落としません。
        if (m_graphics->IsDepthOnlyPass())
        {
            if (m_worldOverlay
                || m_material.BaseColor().w < 1.0f)
            {
                return;
            }
            // テッセレーションShaderは、パッチで描ける形のときだけ
            // 深度パスへ出します。描けない形（板以外、頂点バッファ
            // 未作成）で出すと、位置を出す段（ドメイン）が無いまま
            // 頂点シェーダーだけが刺さり、ラスタライザーへ位置が
            // 届かない不正な描画になります。
            if (m_effect->HasTessellation()
                && !CanDrawTessellatedPatch())
            {
                return;
            }
            // 深度プリパスはメインパスとまったく同じ深度を書ける
            // ものだけに限ります。宣言で半透明にしたShaderは
            // メインパスで深度を書かないので、残すとメインパスの
            // 描画が消えます。テッセレーションは下でメインパスと
            // 同じように分割して描くため、深度も一致します。
            if (m_graphics->DepthPass()
                == DepthPassKind::Prepass)
            {
                const auto& renderState =
                    m_effect->RenderState();
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
            // 頂点位置がマテリアルのカスタム値に依存するShaderがあるため、
            // 深度パスにもマテリアルを渡します。テセレーションの起伏や
            // ジオメトリシェーダーの押し出し量をメインパスと一致させます。
            m_effect->SetMaterial(m_material);
            m_effect->SetDepthOnlyEnabled(true);
            if (CanDrawTessellatedPatch())
            {
                // 影も深度も、分割後の形で書きます。板のまま
                // 書くと、起伏が影を落とさず自分にも影が乗りません。
                //
                // この経路はGeometricPrimitive::Drawを通らないため、
                // 描画状態を明示します。直前のパスが残した状態にかかわらず、
                // 深度と影を書き込める状態へ設定します。
                // 分割後の三角形の向きは板の指定に依らないので、
                // 影は両面で書きます。
                auto* context = m_graphics->Context();
                auto& states = m_graphics->States();
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
                DrawTessellatedPatch();
            }
            else
            {
                m_primitive->Draw(
                    m_effect,
                    m_inputLayout->value.Get(),
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
        m_effect->SetTextures(
            m_albedoTexture
                ? m_albedoTexture->view.Get()
                : m_graphics->WhiteTexture(),
            m_normalTexture
                ? m_normalTexture->view.Get()
                : nullptr,
            BuildPbrTextures(
                m_roughnessTexture,
                m_metallicTexture,
                m_occlusionTexture,
                m_emissiveTexture,
                m_material));
        m_effect->SetCustomTextures(
            ResolveCustomTextureViews());
        m_effect->SetLighting(m_graphics->Lighting());
        ApplyReflectionProbe();
        if (CanDrawTessellatedPatch())
        {
            auto* context = m_graphics->Context();
            auto& states = m_graphics->States();
            constexpr float blendFactor[4]{};
            // 描画状態はShaderの宣言（LAMAPON_RENDER_STATE）に
            // 従い、不透明な地形を含む各マテリアルの設定を反映します。
            const auto& renderState = m_effect->RenderState();
            if (renderState.declared)
            {
                ApplyShaderRenderState(renderState);
            }
            else
            {
                // 宣言が無いときは通常のメッシュと同じ既定へ。
                // ベースカラーのアルファが1未満なら半透明扱い、
                // というのも他の経路と揃えます。
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
            // ワールドオーバーレイ（常に手前）の指定だけは、
            // 宣言より優先します。用途が「必ず見せる」なので。
            if (m_worldOverlay)
            {
                context->OMSetDepthStencilState(
                    states.DepthNone(),
                    0);
            }
            ApplyCullModeOverride();

            DrawTessellatedPatch();
            return;
        }
        if (m_worldOverlay)
        {
            m_primitive->Draw(
                m_effect,
                m_inputLayout->value.Get(),
                true,
                false,
                [this]()
                {
                    auto* context = m_graphics->Context();
                    auto& states = m_graphics->States();
                    constexpr float blendFactor[4]{};
                    context->OMSetBlendState(
                        states.NonPremultiplied(),
                        blendFactor,
                        0xffffffffu);
                    context->OMSetDepthStencilState(
                        states.DepthNone(),
                        0);
                    // 深度なしPlaneの表裏を同時に描くと、反転した画像まで
                    // 重なります。原作と同じ表面カリングで片側だけ描画します。
                    context->RSSetState(
                        states.CullCounterClockwise());
                    ApplyCullModeOverride();
                });
            return;
        }
        // Shaderが描画状態を宣言している場合は、その指定で描きます。
        const auto& renderState = m_effect->RenderState();
        if (renderState.declared)
        {
            m_primitive->Draw(
                m_effect,
                m_inputLayout->value.Get(),
                renderState.blend
                    != ShaderBlendMode::Opaque,
                false,
                [this, &renderState]()
                {
                    ApplyShaderRenderState(renderState);
                });
            return;
        }
        m_primitive->Draw(
            m_effect,
            m_inputLayout->value.Get(),
            m_material.BaseColor().w < 1.0f,
            false,
            [this]()
            {
                ApplyCullModeOverride();
            });
    }

    bool MeshRendererComponent::IsAlphaBlended3D() const
    {
        // 判定の条件は描画側（Render/RenderInstancedBatchの
        // m_primitive->Draw呼び出し）と同じものです。片方だけ
        // 直すと「並べ替えの対象から外れたのに半透明で描かれる」
        // 物ができるので、変えるときは必ず両方。
        if (m_effect != nullptr
            && m_effect->RenderState().declared)
        {
            const auto blend = m_effect->RenderState().blend;
            // 加算は順番によらないので並べ替えません。
            return blend == ShaderBlendMode::Alpha
                || blend == ShaderBlendMode::Premultiplied;
        }
        // Shaderがまだ用意できていないときは不透明として扱います。
        // 描画側もその状態では宣言を読めないので、揃います。
        return m_material.BaseColor().w < 1.0f;
    }

    bool MeshRendererComponent::CanBeInstanced()
        const noexcept
    {
        return !m_worldOverlay
            && !HasProceduralMesh()
            && m_primitive != nullptr
            && m_effect != nullptr
            && m_effect->SupportsInstancing()
            && m_instancedInputLayout != nullptr
            && m_graphics != nullptr;
    }

    std::uint64_t
        MeshRendererComponent::InstanceBatchKey()
            const noexcept
    {
        // FNV-1aで形状・シェーダー・テクスチャ・マテリアル値を
        // まとめます。色はインスタンス属性なので含めません。
        std::uint64_t hash = 14695981039346656037ull;
        const auto combineBytes =
            [&hash](
                const void* data,
                const std::size_t size) noexcept
        {
            const auto* bytes =
                static_cast<const unsigned char*>(data);
            for (std::size_t index = 0;
                index < size;
                ++index)
            {
                hash ^= bytes[index];
                hash *= 1099511628211ull;
            }
        };
        const auto combineFloat =
            [&combineBytes](const float value) noexcept
        {
            combineBytes(&value, sizeof(value));
        };
        const auto combinePath =
            [&combineBytes](
                const std::filesystem::path& path)
        {
            const auto& native = path.native();
            combineBytes(
                native.data(),
                native.size()
                    * sizeof(
                        std::filesystem::path::
                            value_type));
        };
        const auto shape =
            static_cast<std::uint32_t>(m_shape);
        combineBytes(&shape, sizeof(shape));
        combinePath(m_material.Shader());
        combinePath(m_material.AlbedoTexture());
        combinePath(m_material.NormalTexture());
        combineFloat(m_material.Roughness());
        combineFloat(m_material.NormalStrength());
        combineFloat(m_material.Metallic());
        const bool alpha =
            m_material.BaseColor().w < 1.0f;
        combineBytes(&alpha, sizeof(alpha));
        combineBytes(&m_worldOverlay, sizeof(m_worldOverlay));
        combineBytes(&m_cullModeOverride, sizeof(m_cullModeOverride));
        combineBytes(&m_cullMode, sizeof(m_cullMode));
        for (const auto& parameter :
            m_material.CustomParameters())
        {
            combineBytes(&parameter, sizeof(parameter));
        }
        return hash;
    }

    void MeshRendererComponent::RenderInstancedBatch(
        const std::vector<MeshRendererComponent*>& batch,
        DirectX::FXMMATRIX view,
        DirectX::CXMMATRIX projection)
    {
        RefreshShader(false);
        if (batch.empty() || !CanBeInstanced())
        {
            return;
        }

        struct InstanceData final
        {
            DirectX::XMFLOAT4X4 world;
            DirectX::XMFLOAT4 color;
        };
        static_assert(sizeof(InstanceData) == 80);

        std::vector<InstanceData> instances;
        instances.reserve(batch.size());
        for (const auto* component : batch)
        {
            InstanceData data{};
            DirectX::XMStoreFloat4x4(
                &data.world,
                component->Owner().WorldMatrix());
            data.color =
                component->m_material.BaseColor();
            instances.push_back(data);
        }

        auto* instanceBuffer =
            m_graphics->AcquireInstanceBuffer(
                instances.data(),
                instances.size() * sizeof(InstanceData));
        if (instanceBuffer == nullptr)
        {
            return;
        }

        m_effect->SetMatrices(
            DirectX::XMMatrixIdentity(),
            view,
            projection);
        m_effect->SetMaterial(m_material);
        m_effect->SetTextures(
            m_albedoTexture
                ? m_albedoTexture->view.Get()
                : m_graphics->WhiteTexture(),
            m_normalTexture
                ? m_normalTexture->view.Get()
                : nullptr,
            BuildPbrTextures(
                m_roughnessTexture,
                m_metallicTexture,
                m_occlusionTexture,
                m_emissiveTexture,
                m_material));
        m_effect->SetCustomTextures(
            ResolveCustomTextureViews());
        m_effect->SetLighting(m_graphics->Lighting());
        // インスタンスバッチは1回のDrawなので、代表として自分の
        // 位置のプローブを使います（バッチは同じ形状・マテリアルの
        // 集まりで、たいてい近くに固まっているため）。
        ApplyReflectionProbe();
        m_effect->SetInstancingEnabled(true);
        auto* context = m_graphics->Context();
        // バッチキーにはShaderが含まれるため、バッチ内の宣言は同じです。
        // 描画状態はバッチごとに1回だけ適用します。
        const auto& renderState = m_effect->RenderState();
        m_primitive->DrawInstanced(
            m_effect,
            m_instancedInputLayout->value.Get(),
            static_cast<std::uint32_t>(instances.size()),
            renderState.declared
                ? renderState.blend != ShaderBlendMode::Opaque
                : m_material.BaseColor().w < 1.0f,
            false,
            0,
            [this, context, instanceBuffer, &renderState]
            {
                ID3D11Buffer* buffers[]{ instanceBuffer };
                const UINT strides[]{
                    sizeof(InstanceData) };
                const UINT offsets[]{ 0 };
                context->IASetVertexBuffers(
                    1,
                    1,
                    buffers,
                    strides,
                    offsets);
                if (renderState.declared)
                {
                    ApplyShaderRenderState(renderState);
                }
                else
                {
                    ApplyCullModeOverride();
                }
            });
        m_effect->SetInstancingEnabled(false);

        // このパスの個別描画をスキップさせます。
        for (auto* component : batch)
        {
            component->m_instancedThisPass = true;
        }
    }
}
