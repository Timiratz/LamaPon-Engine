#include "LamaPon/Assets/CmoImporter.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
#pragma pack(push, 1)
    struct CmoMaterial final
    {
        DirectX::XMFLOAT4 ambient;
        DirectX::XMFLOAT4 diffuse;
        DirectX::XMFLOAT4 specular;
        float specularPower;
        DirectX::XMFLOAT4 emissive;
        DirectX::XMFLOAT4X4 uvTransform;
    };

    struct CmoSubMesh final
    {
        std::uint32_t materialIndex;
        std::uint32_t indexBufferIndex;
        std::uint32_t vertexBufferIndex;
        std::uint32_t startIndex;
        std::uint32_t primitiveCount;
    };

    struct CmoVertex final
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT4 tangent;
        std::uint32_t color;
        DirectX::XMFLOAT2 textureCoordinate;
    };

    struct CmoSkinningVertex final
    {
        std::array<std::uint32_t, 4> boneIndices;
        std::array<float, 4> boneWeights;
    };

    struct CmoMeshExtents final
    {
        float centerX;
        float centerY;
        float centerZ;
        float radius;
        float minimumX;
        float minimumY;
        float minimumZ;
        float maximumX;
        float maximumY;
        float maximumZ;
    };

    struct CmoBone final
    {
        std::int32_t parentIndex;
        DirectX::XMFLOAT4X4 inverseBindPose;
        DirectX::XMFLOAT4X4 bindPose;
        DirectX::XMFLOAT4X4 localTransform;
    };

    struct CmoClip final
    {
        float startTime;
        float endTime;
        std::uint32_t keyCount;
    };

    struct CmoKeyframe final
    {
        std::uint32_t boneIndex;
        float time;
        DirectX::XMFLOAT4X4 transform;
    };
#pragma pack(pop)

    struct CpuModelVertex final
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 normal{};
        DirectX::XMFLOAT4 tangent{};
        std::uint32_t color{};
        DirectX::XMFLOAT2 textureCoordinate{};
        std::uint32_t blendIndices{};
        std::uint32_t blendWeights{};
    };

    static_assert(sizeof(CmoMaterial) == 132u);
    static_assert(sizeof(CmoSubMesh) == 20u);
    static_assert(sizeof(CmoVertex) == 52u);
    static_assert(sizeof(CmoSkinningVertex) == 32u);
    static_assert(sizeof(CmoMeshExtents) == 40u);
    static_assert(sizeof(CmoBone) == 196u);
    static_assert(sizeof(CmoClip) == 12u);
    static_assert(sizeof(CmoKeyframe) == 72u);
    static_assert(sizeof(CpuModelVertex) == 60u);

    class CmoReader final
    {
    public:
        explicit CmoReader(const std::span<const std::uint8_t> bytes) noexcept
            : m_bytes(bytes)
        {
        }

        template<typename T>
        [[nodiscard]] T Read()
        {
            Ensure(sizeof(T));
            T result{};
            std::memcpy(&result, m_bytes.data() + m_offset, sizeof(T));
            m_offset += sizeof(T);
            return result;
        }

        template<typename T>
        [[nodiscard]] std::vector<T> ReadVector(const std::size_t count)
        {
            if (count > Remaining() / sizeof(T))
            {
                throw std::runtime_error("The CMO array is truncated.");
            }
            std::vector<T> result(count);
            if (!result.empty())
            {
                std::memcpy(
                    result.data(),
                    m_bytes.data() + m_offset,
                    count * sizeof(T));
            }
            m_offset += count * sizeof(T);
            return result;
        }

        [[nodiscard]] std::wstring ReadWideString()
        {
            const auto count = Read<std::uint32_t>();
            if (count > Remaining() / sizeof(wchar_t))
            {
                throw std::runtime_error("The CMO string is truncated.");
            }
            std::wstring result(count, L'\0');
            if (!result.empty())
            {
                std::memcpy(
                    result.data(),
                    m_bytes.data() + m_offset,
                    count * sizeof(wchar_t));
            }
            m_offset += count * sizeof(wchar_t);
            while (!result.empty() && result.back() == L'\0')
            {
                result.pop_back();
            }
            return result;
        }

    private:
        [[nodiscard]] std::size_t Remaining() const noexcept
        {
            return m_bytes.size() - m_offset;
        }

        void Ensure(const std::size_t byteCount) const
        {
            if (byteCount > Remaining())
            {
                throw std::runtime_error("The CMO file is truncated.");
            }
        }

        std::span<const std::uint8_t> m_bytes;
        std::size_t m_offset{};
    };

    struct CmoMaterialRecord final
    {
        CmoMaterial material{};
        std::array<std::wstring, 8> textures;
    };

    [[nodiscard]] std::filesystem::path FindTextureDirectory(
        const std::filesystem::path& modelPath)
    {
        auto directory = modelPath.parent_path();
        for (std::size_t depth{};
            depth < 8u && !directory.empty();
            ++depth)
        {
            const auto sharedTextures = directory / L"ModelTexture";
            if (std::filesystem::is_directory(sharedTextures))
            {
                return sharedTextures;
            }
            const auto parent = directory.parent_path();
            if (parent == directory)
            {
                break;
            }
            directory = parent;
        }
        return modelPath.parent_path();
    }

    [[nodiscard]] LamaPon::GraphicsViewHandle LoadTextureView(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& modelPath,
        const std::wstring& name,
        const LamaPon::TextureLoader::TextureUsage usage)
    {
        if (name.empty())
        {
            return {};
        }
        auto path = std::filesystem::path(name);
        if (!path.is_absolute())
        {
            path = FindTextureDirectory(modelPath) / path;
            if (!assets.FileExists(path)
                && FindTextureDirectory(modelPath)
                    != modelPath.parent_path())
            {
                const auto besideModel = modelPath.parent_path() / name;
                if (assets.FileExists(besideModel))
                {
                    path = besideModel;
                }
            }
        }
        const auto texture = assets.LoadTexture(path, usage);
        if (texture == nullptr)
        {
            return {};
        }
        const auto resources = texture->resources.Acquire();
        return resources != nullptr
            ? resources->shaderResourceView
            : LamaPon::GraphicsViewHandle{};
    }

    [[nodiscard]] LamaPon::SkeletalPoseTransform DecomposeTransform(
        const DirectX::XMFLOAT4X4& matrix)
    {
        DirectX::XMVECTOR scale{};
        DirectX::XMVECTOR rotation{};
        DirectX::XMVECTOR translation{};
        if (!DirectX::XMMatrixDecompose(
                &scale,
                &rotation,
                &translation,
                DirectX::XMLoadFloat4x4(&matrix)))
        {
            throw std::runtime_error(
                "A CMO bone transform cannot be decomposed.");
        }
        LamaPon::SkeletalPoseTransform result;
        DirectX::XMStoreFloat3(&result.scale, scale);
        DirectX::XMStoreFloat4(
            &result.rotation,
            DirectX::XMQuaternionNormalize(rotation));
        DirectX::XMStoreFloat3(&result.translation, translation);
        return result;
    }

    [[nodiscard]] std::uint32_t PackBytes(
        const std::array<std::uint8_t, 4>& values) noexcept
    {
        return values[0]
            | static_cast<std::uint32_t>(values[1]) << 8u
            | static_cast<std::uint32_t>(values[2]) << 16u
            | static_cast<std::uint32_t>(values[3]) << 24u;
    }

    // D3D11のDirectXTK EffectFactoryはCMOのUVTransformを使いません。
    // DirectXTKのCMO loaderはテクスチャ座標のVを反転して読み込むため、
    // 同じ向きへ揃えます（WARPでD3D11とUVを比べて確かめています）。
    [[nodiscard]] CpuModelVertex ConvertVertex(
        const CmoVertex& source,
        const CmoSkinningVertex* const skinning)
    {
        CpuModelVertex result;
        result.position = source.position;
        result.normal = source.normal;
        result.tangent = source.tangent;
        result.color = source.color;
        result.textureCoordinate = {
            source.textureCoordinate.x,
            1.0f - source.textureCoordinate.y };
        if (skinning != nullptr)
        {
            std::array<std::uint8_t, 4> indices{};
            std::array<std::uint8_t, 4> weights{};
            for (std::size_t influence{}; influence < 4u; ++influence)
            {
                indices[influence] = static_cast<std::uint8_t>(
                    std::min(skinning->boneIndices[influence], 255u));
                weights[influence] = static_cast<std::uint8_t>(
                    std::lround(std::clamp(
                        skinning->boneWeights[influence],
                        0.0f,
                        1.0f) * 255.0f));
            }
            result.blendIndices = PackBytes(indices);
            result.blendWeights = PackBytes(weights);
        }
        return result;
    }

    void ExpandBounds(
        LamaPon::SkeletalModel& model,
        const CmoMeshExtents& extents) noexcept
    {
        const DirectX::XMFLOAT3 minimum{
            extents.minimumX,
            extents.minimumY,
            extents.minimumZ };
        const DirectX::XMFLOAT3 maximum{
            extents.maximumX,
            extents.maximumY,
            extents.maximumZ };
        if (!model.hasLocalBounds)
        {
            model.localBounds = { minimum, maximum };
            model.hasLocalBounds = true;
            return;
        }
        model.localBounds.minimum.x = std::min(
            model.localBounds.minimum.x,
            minimum.x);
        model.localBounds.minimum.y = std::min(
            model.localBounds.minimum.y,
            minimum.y);
        model.localBounds.minimum.z = std::min(
            model.localBounds.minimum.z,
            minimum.z);
        model.localBounds.maximum.x = std::max(
            model.localBounds.maximum.x,
            maximum.x);
        model.localBounds.maximum.y = std::max(
            model.localBounds.maximum.y,
            maximum.y);
        model.localBounds.maximum.z = std::max(
            model.localBounds.maximum.z,
            maximum.z);
    }
}

namespace LamaPon
{
    std::shared_ptr<SkeletalModel> CmoImporter::Load(
        AssetManager& assets,
        const std::filesystem::path& path)
    {
        const auto bytes = assets.ReadFileBytes(path);
        CmoReader reader(bytes);
        const auto meshCount = reader.Read<std::uint32_t>();
        if (meshCount == 0u || meshCount > 65535u)
        {
            throw std::runtime_error(
                "The CMO file has an invalid mesh count: "
                + PathToUtf8(path));
        }

        auto model = std::make_shared<SkeletalModel>();
        for (std::uint32_t meshIndex{};
            meshIndex < meshCount;
            ++meshIndex)
        {
            auto meshName = reader.ReadWideString();
            const auto materialCount = reader.Read<std::uint32_t>();
            if (materialCount > 65535u)
            {
                throw std::runtime_error(
                    "The CMO file has too many materials.");
            }
            std::vector<CmoMaterialRecord> materials;
            materials.reserve(std::max(materialCount, 1u));
            for (std::uint32_t materialIndex{};
                materialIndex < materialCount;
                ++materialIndex)
            {
                static_cast<void>(reader.ReadWideString());
                CmoMaterialRecord record;
                record.material = reader.Read<CmoMaterial>();
                static_cast<void>(reader.ReadWideString());
                for (auto& texture : record.textures)
                {
                    texture = reader.ReadWideString();
                }
                materials.push_back(std::move(record));
            }
            if (materials.empty())
            {
                CmoMaterialRecord record;
                record.material.diffuse = {
                    0.8f, 0.8f, 0.8f, 1.0f };
                DirectX::XMStoreFloat4x4(
                    &record.material.uvTransform,
                    DirectX::XMMatrixIdentity());
                materials.push_back(std::move(record));
            }

            const bool hasSkeleton = reader.Read<std::uint8_t>() != 0u;
            const auto subMeshes = reader.ReadVector<CmoSubMesh>(
                reader.Read<std::uint32_t>());
            if (subMeshes.empty())
            {
                throw std::runtime_error("The CMO mesh has no submeshes.");
            }

            const auto indexBufferCount = reader.Read<std::uint32_t>();
            if (indexBufferCount == 0u || indexBufferCount > 65535u)
            {
                throw std::runtime_error(
                    "The CMO mesh has an invalid index buffer count.");
            }
            std::vector<std::vector<std::uint16_t>> indexBuffers;
            indexBuffers.reserve(indexBufferCount);
            for (std::uint32_t index{};
                index < indexBufferCount;
                ++index)
            {
                indexBuffers.push_back(
                    reader.ReadVector<std::uint16_t>(
                        reader.Read<std::uint32_t>()));
                if (indexBuffers.back().empty())
                {
                    throw std::runtime_error(
                        "The CMO mesh has an empty index buffer.");
                }
            }

            const auto vertexBufferCount = reader.Read<std::uint32_t>();
            if (vertexBufferCount == 0u || vertexBufferCount > 65535u)
            {
                throw std::runtime_error(
                    "The CMO mesh has an invalid vertex buffer count.");
            }
            std::vector<std::vector<CmoVertex>> vertexBuffers;
            vertexBuffers.reserve(vertexBufferCount);
            for (std::uint32_t index{};
                index < vertexBufferCount;
                ++index)
            {
                vertexBuffers.push_back(
                    reader.ReadVector<CmoVertex>(
                        reader.Read<std::uint32_t>()));
                if (vertexBuffers.back().empty())
                {
                    throw std::runtime_error(
                        "The CMO mesh has an empty vertex buffer.");
                }
            }

            const auto skinningBufferCount = reader.Read<std::uint32_t>();
            if (skinningBufferCount != 0u
                && skinningBufferCount != vertexBufferCount)
            {
                throw std::runtime_error(
                    "The CMO skinning streams do not match its vertices.");
            }
            std::vector<std::vector<CmoSkinningVertex>> skinningBuffers;
            skinningBuffers.reserve(skinningBufferCount);
            for (std::uint32_t index{};
                index < skinningBufferCount;
                ++index)
            {
                skinningBuffers.push_back(
                    reader.ReadVector<CmoSkinningVertex>(
                        reader.Read<std::uint32_t>()));
                if (skinningBuffers.back().size()
                    != vertexBuffers[index].size())
                {
                    throw std::runtime_error(
                        "The CMO skinning vertex count is invalid.");
                }
            }

            const auto extents = reader.Read<CmoMeshExtents>();
            ExpandBounds(*model, extents);

            std::size_t meshNode{};
            std::ptrdiff_t skinIndex = -1;
            if (hasSkeleton)
            {
                const auto boneCount = reader.Read<std::uint32_t>();
                if (boneCount == 0u || boneCount > 255u)
                {
                    throw std::runtime_error(
                        "The CMO skeleton has an unsupported bone count.");
                }
                const auto nodeOffset = model->nodes.size();
                meshNode = nodeOffset;
                std::vector<CmoBone> bones;
                bones.reserve(boneCount);
                for (std::uint32_t boneIndex{};
                    boneIndex < boneCount;
                    ++boneIndex)
                {
                    SkeletalNode node;
                    node.name = WideToUtf8(reader.ReadWideString());
                    const auto bone = reader.Read<CmoBone>();
                    if (bone.parentIndex >= static_cast<std::int32_t>(boneCount))
                    {
                        throw std::runtime_error(
                            "The CMO skeleton has an invalid parent index.");
                    }
                    node.parent = bone.parentIndex >= 0
                        ? static_cast<std::ptrdiff_t>(nodeOffset)
                            + bone.parentIndex
                        : -1;
                    node.bindPose = DecomposeTransform(
                        bone.localTransform);
                    model->nodes.push_back(std::move(node));
                    bones.push_back(bone);
                }
                if (!skinningBuffers.empty())
                {
                    SkeletalSkin skin;
                    skin.name = WideToUtf8(meshName);
                    skin.joints.reserve(bones.size());
                    skin.inverseBindMatrices.reserve(bones.size());
                    for (std::size_t boneIndex{};
                        boneIndex < bones.size();
                        ++boneIndex)
                    {
                        skin.joints.push_back(nodeOffset + boneIndex);
                        skin.inverseBindMatrices.push_back(
                            bones[boneIndex].inverseBindPose);
                    }
                    skinIndex = static_cast<std::ptrdiff_t>(
                        model->skins.size());
                    model->skins.push_back(std::move(skin));
                }

                const auto clipCount = reader.Read<std::uint32_t>();
                if (clipCount > 65535u)
                {
                    throw std::runtime_error(
                        "The CMO file has too many animation clips.");
                }
                for (std::uint32_t clipIndex{};
                    clipIndex < clipCount;
                    ++clipIndex)
                {
                    static_cast<void>(reader.ReadWideString());
                    const auto clip = reader.Read<CmoClip>();
                    static_cast<void>(
                        reader.ReadVector<CmoKeyframe>(clip.keyCount));
                }
            }
            else
            {
                SkeletalNode node;
                node.name = WideToUtf8(meshName);
                meshNode = model->nodes.size();
                model->nodes.push_back(std::move(node));
                if (!skinningBuffers.empty())
                {
                    throw std::runtime_error(
                        "The CMO mesh has skinning data without a skeleton.");
                }
            }

            for (const auto& subMesh : subMeshes)
            {
                if (subMesh.materialIndex >= materials.size()
                    || subMesh.indexBufferIndex >= indexBuffers.size()
                    || subMesh.vertexBufferIndex >= vertexBuffers.size())
                {
                    throw std::runtime_error(
                        "The CMO submesh references invalid data.");
                }
                const auto indexCount64 =
                    static_cast<std::uint64_t>(subMesh.primitiveCount) * 3u;
                const auto& sourceIndices =
                    indexBuffers[subMesh.indexBufferIndex];
                if (indexCount64 > sourceIndices.size()
                    || subMesh.startIndex
                        > sourceIndices.size()
                            - static_cast<std::size_t>(indexCount64))
                {
                    throw std::runtime_error(
                        "The CMO submesh index range is invalid.");
                }
                const auto& sourceVertices =
                    vertexBuffers[subMesh.vertexBufferIndex];
                const auto& material = materials[subMesh.materialIndex];
                const auto* skinning = skinningBuffers.empty()
                    ? nullptr
                    : &skinningBuffers[subMesh.vertexBufferIndex];

                std::vector<CpuModelVertex> vertices;
                vertices.reserve(sourceVertices.size());
                for (std::size_t vertexIndex{};
                    vertexIndex < sourceVertices.size();
                    ++vertexIndex)
                {
                    vertices.push_back(ConvertVertex(
                        sourceVertices[vertexIndex],
                        skinning != nullptr
                            ? &(*skinning)[vertexIndex]
                            : nullptr));
                }

                SkeletalPrimitive primitive;
                const auto* vertexBytes = reinterpret_cast<const std::uint8_t*>(
                    vertices.data());
                primitive.cpuVertexData.assign(
                    vertexBytes,
                    vertexBytes + vertices.size() * sizeof(CpuModelVertex));
                primitive.cpuVertexStride = sizeof(CpuModelVertex);
                primitive.cpuIndices.reserve(
                    static_cast<std::size_t>(indexCount64));
                const auto indexEnd = subMesh.startIndex
                    + static_cast<std::size_t>(indexCount64);
                for (std::size_t index = subMesh.startIndex;
                    index < indexEnd;
                    ++index)
                {
                    if (sourceIndices[index] >= sourceVertices.size())
                    {
                        throw std::runtime_error(
                            "The CMO submesh contains an invalid vertex index.");
                    }
                    primitive.cpuIndices.push_back(sourceIndices[index]);
                }
                primitive.indexCount = static_cast<std::uint32_t>(
                    primitive.cpuIndices.size());
                primitive.meshNode = meshNode;
                primitive.skin = skinIndex;
                // Material上書きが無いとき、D3D11はDirectXTKのEffectで
                // Diffuse／Emissive Colorと内蔵のalbedo／normalを使って
                // 描きます。PBRのpipelineではSpecular Powerを粗さへ近似します。
                // 上書き中の合成規則はModelRendererComponentが揃えます。
                primitive.baseColor = material.material.diffuse;
                primitive.roughness = std::clamp(
                    std::sqrt(2.0f / (
                        std::max(material.material.specularPower, 0.0f)
                            + 2.0f)),
                    0.04f,
                    1.0f);
                primitive.emissiveFactor = {
                    material.material.emissive.x,
                    material.material.emissive.y,
                    material.material.emissive.z };
                primitive.alpha = primitive.baseColor.w < 0.999f;
                primitive.localBounds = {
                    { extents.minimumX, extents.minimumY, extents.minimumZ },
                    { extents.maximumX, extents.maximumY, extents.maximumZ } };
                primitive.hasLocalBounds = true;
                primitive.embeddedTextures.albedo = LoadTextureView(
                    assets,
                    path,
                    material.textures[0],
                    TextureLoader::TextureUsage::Color);
                primitive.embeddedTextures.normal = LoadTextureView(
                    assets,
                    path,
                    material.textures[2],
                    TextureLoader::TextureUsage::NormalMap);
                // DirectXTKのEffectとD3D11の共通Lit経路はemissive textureを
                // 使わないため、発光は色だけを保持します。
                primitive.embeddedTextures.emissiveFactor =
                    primitive.emissiveFactor;
                model->primitives.push_back(std::move(primitive));
            }
        }

        if (model->primitives.empty())
        {
            throw std::runtime_error(
                "The CMO file has no drawable primitives: "
                + PathToUtf8(path));
        }
        return model;
    }
}
