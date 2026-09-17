#include "LamaPon/Assets/VboImporter.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
#pragma pack(push, 1)
    struct VboHeader final
    {
        std::uint32_t vertexCount;
        std::uint32_t indexCount;
    };

    struct VboVertex final
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 textureCoordinate;
    };
#pragma pack(pop)

    // ModelRendererComponentのImportedModelVertexと同じ並びです。
    struct CpuModelVertex final
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 normal{};
        DirectX::XMFLOAT4 tangent{};
        std::uint32_t color{ 0xffffffffu };
        DirectX::XMFLOAT2 textureCoordinate{};
        std::uint32_t blendIndices{};
        std::uint32_t blendWeights{};
    };

    static_assert(sizeof(VboHeader) == 8u);
    static_assert(sizeof(VboVertex) == 32u);
    static_assert(sizeof(CpuModelVertex) == 60u);

    template<typename T>
    [[nodiscard]] T ReadValue(
        const std::span<const std::uint8_t> bytes,
        const std::size_t offset,
        const char* const what)
    {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        {
            throw std::runtime_error(
                std::string("The VBO ") + what + " is truncated.");
        }
        T value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    }
}

namespace LamaPon
{
    std::shared_ptr<SkeletalModel> VboImporter::Load(
        AssetManager& assets,
        const std::filesystem::path& path)
    {
        const auto bytes = assets.ReadFileBytes(path);
        return LoadFromMemory(bytes, path);
    }

    std::shared_ptr<SkeletalModel> VboImporter::LoadFromMemory(
        const std::span<const std::uint8_t> bytes,
        const std::filesystem::path& sourcePath)
    {
        const auto header = ReadValue<VboHeader>(bytes, 0u, "header");
        if (header.vertexCount == 0u || header.indexCount == 0u)
        {
            throw std::runtime_error(
                "The VBO file has no drawable data: "
                + PathToUtf8(sourcePath));
        }
        const std::uint64_t vertexBytes64 =
            static_cast<std::uint64_t>(header.vertexCount)
            * sizeof(VboVertex);
        const std::uint64_t indexBytes64 =
            static_cast<std::uint64_t>(header.indexCount)
            * sizeof(std::uint16_t);
        if (vertexBytes64 > (std::numeric_limits<std::size_t>::max)()
            || indexBytes64 > (std::numeric_limits<std::size_t>::max)())
        {
            throw std::runtime_error("The VBO geometry is too large.");
        }
        const auto vertexBytes = static_cast<std::size_t>(vertexBytes64);
        const auto indexBytes = static_cast<std::size_t>(indexBytes64);
        if (sizeof(VboHeader) > bytes.size()
            || vertexBytes > bytes.size() - sizeof(VboHeader)
            || indexBytes
                > bytes.size() - sizeof(VboHeader) - vertexBytes)
        {
            throw std::runtime_error(
                "The VBO geometry is truncated: "
                + PathToUtf8(sourcePath));
        }

        std::vector<CpuModelVertex> vertices;
        vertices.reserve(header.vertexCount);
        Bounds3D bounds{};
        for (std::uint32_t index{}; index < header.vertexCount; ++index)
        {
            const auto source = ReadValue<VboVertex>(
                bytes,
                sizeof(VboHeader)
                    + static_cast<std::size_t>(index) * sizeof(VboVertex),
                "vertex buffer");
            CpuModelVertex vertex;
            vertex.position = source.position;
            vertex.normal = source.normal;
            vertex.textureCoordinate = source.textureCoordinate;
            vertices.push_back(vertex);
            if (index == 0u)
            {
                bounds = { source.position, source.position };
            }
            else
            {
                bounds.minimum.x = (std::min)(bounds.minimum.x, source.position.x);
                bounds.minimum.y = (std::min)(bounds.minimum.y, source.position.y);
                bounds.minimum.z = (std::min)(bounds.minimum.z, source.position.z);
                bounds.maximum.x = (std::max)(bounds.maximum.x, source.position.x);
                bounds.maximum.y = (std::max)(bounds.maximum.y, source.position.y);
                bounds.maximum.z = (std::max)(bounds.maximum.z, source.position.z);
            }
        }

        std::vector<std::uint32_t> indices;
        indices.reserve(header.indexCount);
        const auto indexOffset = sizeof(VboHeader) + vertexBytes;
        for (std::uint32_t index{}; index < header.indexCount; ++index)
        {
            const auto value = ReadValue<std::uint16_t>(
                bytes,
                indexOffset
                    + static_cast<std::size_t>(index) * sizeof(std::uint16_t),
                "index buffer");
            if (value >= header.vertexCount)
            {
                throw std::runtime_error(
                    "The VBO contains an invalid vertex index: "
                    + PathToUtf8(sourcePath));
            }
            indices.push_back(value);
        }
        // DirectXTKと同じ三角形listです。D3D11で描画されない末尾の端数は
        // CPU幾何から除き、両Backendの見える結果を揃えます。
        indices.resize(indices.size() - indices.size() % 3u);
        if (indices.empty())
        {
            throw std::runtime_error(
                "The VBO file has no complete triangles: "
                + PathToUtf8(sourcePath));
        }

        SkeletalPrimitive primitive;
        const auto* const vertexData =
            reinterpret_cast<const std::uint8_t*>(vertices.data());
        primitive.cpuVertexData.assign(
            vertexData,
            vertexData + vertices.size() * sizeof(CpuModelVertex));
        primitive.cpuVertexStride = sizeof(CpuModelVertex);
        primitive.cpuIndices = std::move(indices);
        primitive.indexCount = static_cast<std::uint32_t>(
            primitive.cpuIndices.size());
        primitive.meshNode = 0u;
        primitive.baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        primitive.roughness = 0.5f;
        primitive.localBounds = bounds;
        primitive.hasLocalBounds = true;

        auto model = std::make_shared<SkeletalModel>();
        SkeletalNode node;
        node.name = PathToUtf8(sourcePath.stem());
        model->nodes.push_back(std::move(node));
        model->primitives.push_back(std::move(primitive));
        model->localBounds = bounds;
        model->hasLocalBounds = true;
        return model;
    }
}
