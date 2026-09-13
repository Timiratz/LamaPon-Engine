#include "LamaPon/Assets/SdkmeshImporter.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // DirectXTKのSDKMesh.hと同じ版番号、Direct3D 9の頂点宣言、
    // primitive種別です。
    constexpr std::uint32_t SdkmeshVersion = 101u;
    constexpr std::uint32_t SdkmeshVersion2 = 200u;
    constexpr std::size_t MaximumVertexElements = 32u;
    constexpr std::size_t MaximumVertexStreams = 16u;
    constexpr std::size_t MaximumName = 100u;
    constexpr std::size_t MaximumPath = 260u;

    constexpr std::uint8_t DeclarationFloat1 = 0u;
    constexpr std::uint8_t DeclarationFloat2 = 1u;
    constexpr std::uint8_t DeclarationFloat3 = 2u;
    constexpr std::uint8_t DeclarationFloat4 = 3u;
    constexpr std::uint8_t DeclarationColor = 4u;
    constexpr std::uint8_t DeclarationUByte4 = 5u;
    constexpr std::uint8_t DeclarationUByte4N = 8u;
    constexpr std::uint8_t DeclarationShort4N = 10u;
    constexpr std::uint8_t DeclarationFloat16x2 = 15u;
    constexpr std::uint8_t DeclarationFloat16x4 = 16u;
    constexpr std::uint8_t DeclarationUnused = 17u;
    // DXGI_FORMATを32だけずらした拡張形式です。
    constexpr std::uint8_t DeclarationR10G10B10A2 = 32u + 24u;
    constexpr std::uint8_t DeclarationR11G11B10 = 32u + 26u;
    constexpr std::uint8_t DeclarationR8G8B8A8Snorm = 32u + 31u;

    constexpr std::uint8_t UsagePosition = 0u;
    constexpr std::uint8_t UsageBlendWeight = 1u;
    constexpr std::uint8_t UsageBlendIndices = 2u;
    constexpr std::uint8_t UsageNormal = 3u;
    constexpr std::uint8_t UsageTextureCoordinate = 5u;
    constexpr std::uint8_t UsageTangent = 6u;
    constexpr std::uint8_t UsageBinormal = 7u;
    constexpr std::uint8_t UsageColor = 10u;

    constexpr std::uint32_t PrimitiveTriangleList = 0u;
    constexpr std::uint32_t PrimitiveTriangleStrip = 1u;
    constexpr std::uint32_t Index16 = 0u;
    constexpr std::uint32_t Index32 = 1u;

#pragma pack(push, 4)
    struct SdkmeshVertexElement final
    {
        std::uint16_t stream;
        std::uint16_t offset;
        std::uint8_t type;
        std::uint8_t method;
        std::uint8_t usage;
        std::uint8_t usageIndex;
    };
#pragma pack(pop)

#pragma pack(push, 8)
    struct SdkmeshHeader final
    {
        std::uint32_t version;
        std::uint8_t isBigEndian;
        std::uint64_t headerSize;
        std::uint64_t nonBufferDataSize;
        std::uint64_t bufferDataSize;
        std::uint32_t numVertexBuffers;
        std::uint32_t numIndexBuffers;
        std::uint32_t numMeshes;
        std::uint32_t numTotalSubsets;
        std::uint32_t numFrames;
        std::uint32_t numMaterials;
        std::uint64_t vertexStreamHeadersOffset;
        std::uint64_t indexStreamHeadersOffset;
        std::uint64_t meshDataOffset;
        std::uint64_t subsetDataOffset;
        std::uint64_t frameDataOffset;
        std::uint64_t materialDataOffset;
    };

    struct SdkmeshVertexBufferHeader final
    {
        std::uint64_t numVertices;
        std::uint64_t sizeBytes;
        std::uint64_t strideBytes;
        std::array<SdkmeshVertexElement, MaximumVertexElements> declaration;
        std::uint64_t dataOffset;
    };

    struct SdkmeshIndexBufferHeader final
    {
        std::uint64_t numIndices;
        std::uint64_t sizeBytes;
        std::uint32_t indexType;
        std::uint64_t dataOffset;
    };

    struct SdkmeshMesh final
    {
        char name[MaximumName];
        std::uint8_t numVertexBuffers;
        std::uint32_t vertexBuffers[MaximumVertexStreams];
        std::uint32_t indexBuffer;
        std::uint32_t numSubsets;
        std::uint32_t numFrameInfluences;
        DirectX::XMFLOAT3 boundingBoxCenter;
        DirectX::XMFLOAT3 boundingBoxExtents;
        std::uint64_t subsetOffset;
        std::uint64_t frameInfluenceOffset;
    };

    struct SdkmeshSubset final
    {
        char name[MaximumName];
        std::uint32_t materialId;
        std::uint32_t primitiveType;
        std::uint64_t indexStart;
        std::uint64_t indexCount;
        std::uint64_t vertexStart;
        std::uint64_t vertexCount;
    };

    struct SdkmeshMaterial final
    {
        char name[MaximumName];
        char materialInstancePath[MaximumPath];
        char diffuseTexture[MaximumPath];
        char normalTexture[MaximumPath];
        char specularTexture[MaximumPath];
        DirectX::XMFLOAT4 diffuse;
        DirectX::XMFLOAT4 ambient;
        DirectX::XMFLOAT4 specular;
        DirectX::XMFLOAT4 emissive;
        float power;
        std::uint64_t runtimeHandles[6];
    };

    struct SdkmeshMaterialV2 final
    {
        char name[MaximumName];
        char rmaTexture[MaximumPath];
        char albedoTexture[MaximumPath];
        char normalTexture[MaximumPath];
        char emissiveTexture[MaximumPath];
        float alpha;
        char reserved[60];
        std::uint64_t runtimeHandles[6];
    };
#pragma pack(pop)

    static_assert(sizeof(SdkmeshVertexElement) == 8u);
    static_assert(sizeof(SdkmeshHeader) == 104u);
    static_assert(sizeof(SdkmeshVertexBufferHeader) == 288u);
    static_assert(sizeof(SdkmeshIndexBufferHeader) == 32u);
    static_assert(sizeof(SdkmeshMesh) == 224u);
    static_assert(sizeof(SdkmeshSubset) == 144u);
    static_assert(sizeof(SdkmeshMaterial) == 1256u);
    static_assert(sizeof(SdkmeshMaterialV2) == sizeof(SdkmeshMaterial));

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

    static_assert(sizeof(CpuModelVertex) == 60u);

    struct VertexElement final
    {
        std::size_t offset{};
        std::uint8_t type{};
        bool present{};
    };

    struct VertexLayout final
    {
        VertexElement position;
        VertexElement normal;
        VertexElement tangent;
        VertexElement color;
        VertexElement textureCoordinate;
        VertexElement blendIndices;
        VertexElement blendWeights;
    };

    struct ImportedMaterial final
    {
        DirectX::XMFLOAT4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT3 emissive{};
        // DirectXTKがspecularを無効にする材質は、PBR側では最も粗くします。
        float roughness{ 1.0f };
        LamaPon::GraphicsViewHandle albedo;
        LamaPon::GraphicsViewHandle normal;
        bool imported{};
    };

    template<typename T>
    [[nodiscard]] std::vector<T> ReadArray(
        const std::span<const std::uint8_t> bytes,
        const std::uint64_t offset,
        const std::uint64_t count,
        const char* const what)
    {
        if (count > (std::numeric_limits<std::size_t>::max)() / sizeof(T)
            || offset > bytes.size()
            || count * sizeof(T) > bytes.size() - offset)
        {
            throw std::runtime_error(
                std::string("The SDKMESH ") + what + " is out of range.");
        }
        std::vector<T> result(static_cast<std::size_t>(count));
        if (!result.empty())
        {
            std::memcpy(
                result.data(),
                bytes.data() + offset,
                result.size() * sizeof(T));
        }
        return result;
    }

    [[nodiscard]] std::string ReadName(
        const char* const text,
        const std::size_t capacity)
    {
        const auto* const end = std::find(text, text + capacity, '\0');
        return std::string(text, end);
    }

    [[nodiscard]] std::size_t DirectionSize(const std::uint8_t type) noexcept
    {
        switch (type)
        {
        case DeclarationFloat3:
            return 12u;
        case DeclarationUByte4N:
        case DeclarationR10G10B10A2:
        case DeclarationR11G11B10:
        case DeclarationR8G8B8A8Snorm:
            return 4u;
        case DeclarationShort4N:
        case DeclarationFloat16x4:
            return 8u;
        default:
            return 0u;
        }
    }

    [[nodiscard]] std::size_t ColorSize(const std::uint8_t type) noexcept
    {
        switch (type)
        {
        case DeclarationFloat4:
            return 16u;
        case DeclarationColor:
        case DeclarationUByte4N:
        case DeclarationR10G10B10A2:
        case DeclarationR11G11B10:
            return 4u;
        case DeclarationFloat16x4:
            return 8u;
        default:
            return 0u;
        }
    }

    [[nodiscard]] std::size_t TextureCoordinateSize(
        const std::uint8_t type) noexcept
    {
        switch (type)
        {
        case DeclarationFloat1:
        case DeclarationFloat16x2:
            return 4u;
        case DeclarationFloat2:
        case DeclarationFloat16x4:
            return 8u;
        case DeclarationFloat3:
            return 12u;
        case DeclarationFloat4:
            return 16u;
        default:
            return 0u;
        }
    }

    // DirectXTKのGetInputLayoutDescと同じく、先頭から隙間なく並ぶ対応形式
    // だけを読み、未対応の要素に来たらそこで打ち切ります。
    [[nodiscard]] VertexLayout ParseVertexLayout(
        const SdkmeshVertexBufferHeader& vertexBuffer)
    {
        VertexLayout layout;
        std::size_t offset{};
        for (const auto& element : vertexBuffer.declaration)
        {
            if (element.usage == 0xffu
                || element.type == DeclarationUnused
                || element.offset != offset)
            {
                break;
            }
            std::size_t size{};
            VertexElement* destination{};
            switch (element.usage)
            {
            case UsagePosition:
                size = element.type == DeclarationFloat3 ? 12u : 0u;
                destination = &layout.position;
                break;
            case UsageNormal:
                size = DirectionSize(element.type);
                destination = &layout.normal;
                break;
            case UsageTangent:
                size = DirectionSize(element.type);
                destination = &layout.tangent;
                break;
            case UsageBinormal:
                size = DirectionSize(element.type);
                break;
            case UsageColor:
                size = ColorSize(element.type);
                destination = &layout.color;
                break;
            case UsageTextureCoordinate:
                size = TextureCoordinateSize(element.type);
                // DirectXTKは出現順にTEXCOORD0／1を割り当てます。2つ目のUV
                // （DualTexture）は共通Litでも使いません。
                if (!layout.textureCoordinate.present)
                {
                    destination = &layout.textureCoordinate;
                }
                break;
            case UsageBlendIndices:
                size = element.type == DeclarationUByte4 ? 4u : 0u;
                destination = &layout.blendIndices;
                break;
            case UsageBlendWeight:
                size = element.type == DeclarationUByte4N ? 4u : 0u;
                destination = &layout.blendWeights;
                break;
            default:
                break;
            }
            if (size == 0u)
            {
                break;
            }
            if (destination != nullptr)
            {
                *destination = { offset, element.type, true };
            }
            offset += size;
        }
        if (!layout.position.present)
        {
            throw std::runtime_error(
                "The SDKMESH vertex buffer has no position.");
        }
        if (offset > vertexBuffer.strideBytes)
        {
            throw std::runtime_error(
                "The SDKMESH vertex declaration exceeds its stride.");
        }
        return layout;
    }

    template<typename T>
    [[nodiscard]] T ReadValue(const std::uint8_t* const data) noexcept
    {
        T value{};
        std::memcpy(&value, data, sizeof(T));
        return value;
    }

    [[nodiscard]] DirectX::XMFLOAT3 Unbias(
        const DirectX::XMFLOAT3& value) noexcept
    {
        return {
            value.x * 2.0f - 1.0f,
            value.y * 2.0f - 1.0f,
            value.z * 2.0f - 1.0f };
    }

    // DirectXTKがbiased vertex normalとして扱うUNORM系は、[-1, 1]へ戻します。
    [[nodiscard]] DirectX::XMFLOAT3 ReadDirection(
        const std::uint8_t* const data,
        const std::uint8_t type) noexcept
    {
        switch (type)
        {
        case DeclarationFloat3:
            return ReadValue<DirectX::XMFLOAT3>(data);
        case DeclarationUByte4N:
        {
            const auto bytes = ReadValue<std::array<std::uint8_t, 4>>(data);
            return Unbias({
                bytes[0] / 255.0f,
                bytes[1] / 255.0f,
                bytes[2] / 255.0f });
        }
        case DeclarationShort4N:
        {
            const auto values = ReadValue<std::array<std::int16_t, 4>>(data);
            return {
                (std::max)(values[0] / 32767.0f, -1.0f),
                (std::max)(values[1] / 32767.0f, -1.0f),
                (std::max)(values[2] / 32767.0f, -1.0f) };
        }
        case DeclarationFloat16x4:
        {
            const auto values =
                ReadValue<std::array<DirectX::PackedVector::HALF, 4>>(data);
            return {
                DirectX::PackedVector::XMConvertHalfToFloat(values[0]),
                DirectX::PackedVector::XMConvertHalfToFloat(values[1]),
                DirectX::PackedVector::XMConvertHalfToFloat(values[2]) };
        }
        case DeclarationR10G10B10A2:
        {
            const auto packed = ReadValue<std::uint32_t>(data);
            return Unbias({
                (packed & 0x3ffu) / 1023.0f,
                ((packed >> 10u) & 0x3ffu) / 1023.0f,
                ((packed >> 20u) & 0x3ffu) / 1023.0f });
        }
        case DeclarationR11G11B10:
        {
            const auto packed =
                ReadValue<DirectX::PackedVector::XMFLOAT3PK>(data);
            DirectX::XMFLOAT3 value{};
            DirectX::XMStoreFloat3(
                &value,
                DirectX::PackedVector::XMLoadFloat3PK(&packed));
            return Unbias(value);
        }
        case DeclarationR8G8B8A8Snorm:
        {
            const auto values = ReadValue<std::array<std::int8_t, 4>>(data);
            return {
                (std::max)(values[0] / 127.0f, -1.0f),
                (std::max)(values[1] / 127.0f, -1.0f),
                (std::max)(values[2] / 127.0f, -1.0f) };
        }
        default:
            return {};
        }
    }

    [[nodiscard]] DirectX::XMFLOAT2 ReadTextureCoordinate(
        const std::uint8_t* const data,
        const std::uint8_t type) noexcept
    {
        switch (type)
        {
        case DeclarationFloat1:
            return { ReadValue<float>(data), 0.0f };
        case DeclarationFloat2:
        case DeclarationFloat3:
        case DeclarationFloat4:
            return ReadValue<DirectX::XMFLOAT2>(data);
        case DeclarationFloat16x2:
        case DeclarationFloat16x4:
        {
            const auto values =
                ReadValue<std::array<DirectX::PackedVector::HALF, 2>>(data);
            return {
                DirectX::PackedVector::XMConvertHalfToFloat(values[0]),
                DirectX::PackedVector::XMConvertHalfToFloat(values[1]) };
        }
        default:
            return {};
        }
    }

    [[nodiscard]] CpuModelVertex ReadVertex(
        const std::uint8_t* const vertex,
        const VertexLayout& layout) noexcept
    {
        CpuModelVertex result;
        result.position = ReadValue<DirectX::XMFLOAT3>(
            vertex + layout.position.offset);
        if (layout.normal.present)
        {
            result.normal = ReadDirection(
                vertex + layout.normal.offset,
                layout.normal.type);
        }
        if (layout.tangent.present)
        {
            const auto tangent = ReadDirection(
                vertex + layout.tangent.offset,
                layout.tangent.type);
            result.tangent = { tangent.x, tangent.y, tangent.z, 1.0f };
        }
        if (layout.color.present
            && layout.color.type == DeclarationColor)
        {
            // D3DCOLORのBGRAを、CMOと同じRGBAの並びへ入れ替えます。
            const auto bgra = ReadValue<std::uint32_t>(
                vertex + layout.color.offset);
            result.color = (bgra & 0xff00ff00u)
                | ((bgra >> 16u) & 0xffu)
                | ((bgra & 0xffu) << 16u);
        }
        if (layout.textureCoordinate.present)
        {
            result.textureCoordinate = ReadTextureCoordinate(
                vertex + layout.textureCoordinate.offset,
                layout.textureCoordinate.type);
        }
        if (layout.blendIndices.present)
        {
            result.blendIndices = ReadValue<std::uint32_t>(
                vertex + layout.blendIndices.offset);
        }
        if (layout.blendWeights.present)
        {
            result.blendWeights = ReadValue<std::uint32_t>(
                vertex + layout.blendWeights.offset);
        }
        return result;
    }

    [[nodiscard]] std::vector<std::uint32_t> ReadIndices(
        const std::span<const std::uint8_t> bytes,
        const SdkmeshIndexBufferHeader& indexBuffer)
    {
        if (indexBuffer.indexType == Index16)
        {
            if (indexBuffer.numIndices > indexBuffer.sizeBytes / 2u)
            {
                throw std::runtime_error(
                    "The SDKMESH index buffer size is invalid.");
            }
            const auto values = ReadArray<std::uint16_t>(
                bytes,
                indexBuffer.dataOffset,
                indexBuffer.numIndices,
                "index buffer");
            return { values.begin(), values.end() };
        }
        if (indexBuffer.indexType == Index32)
        {
            if (indexBuffer.numIndices > indexBuffer.sizeBytes / 4u)
            {
                throw std::runtime_error(
                    "The SDKMESH index buffer size is invalid.");
            }
            return ReadArray<std::uint32_t>(
                bytes,
                indexBuffer.dataOffset,
                indexBuffer.numIndices,
                "index buffer");
        }
        throw std::runtime_error("The SDKMESH index buffer type is invalid.");
    }

    // D3D11の三角形stripと同じく、奇数番目の三角形は向きを揃え直し、
    // restart indexと縮退三角形で区切ります。
    [[nodiscard]] std::vector<std::uint32_t> ExpandTriangleStrip(
        const std::span<const std::uint32_t> strip,
        const std::uint32_t restartIndex)
    {
        std::vector<std::uint32_t> triangles;
        std::size_t runStart{};
        while (runStart < strip.size())
        {
            auto runEnd = runStart;
            while (runEnd < strip.size() && strip[runEnd] != restartIndex)
            {
                ++runEnd;
            }
            for (std::size_t index = runStart; index + 2u < runEnd; ++index)
            {
                auto a = strip[index];
                auto b = strip[index + 1u];
                const auto c = strip[index + 2u];
                if ((index - runStart) % 2u != 0u)
                {
                    std::swap(a, b);
                }
                if (a != b && b != c && a != c)
                {
                    triangles.insert(triangles.end(), { a, b, c });
                }
            }
            runStart = runEnd + 1u;
        }
        return triangles;
    }

    [[nodiscard]] LamaPon::GraphicsViewHandle LoadTextureView(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& modelPath,
        const std::string& name,
        const LamaPon::TextureLoader::TextureUsage usage)
    {
        if (name.empty())
        {
            return {};
        }
        // DirectXTKのEffectFactoryと同じく、UTF-8の名前をモデルのフォルダー
        // から探します。
        auto path = LamaPon::PathFromUtf8(name);
        if (path.is_relative())
        {
            path = modelPath.parent_path() / path;
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

    // DirectXTKのLoadMaterialと同じ既定値です。色の無い旧形式は白、
    // alphaの0は不透明として扱い、specular colorとpowerが揃ったときだけ
    // specularを有効にします。
    [[nodiscard]] ImportedMaterial ImportMaterial(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& modelPath,
        const std::uint32_t version,
        const SdkmeshMaterial& material,
        const SdkmeshMaterialV2& materialV2,
        const bool hasTangents)
    {
        ImportedMaterial result;
        result.imported = true;
        if (version == SdkmeshVersion2)
        {
            // DirectXTKのEffectFactoryは、PBR版でもalbedoとnormalだけを使います。
            result.baseColor.w = materialV2.alpha == 0.0f
                ? 1.0f
                : materialV2.alpha;
            result.albedo = LoadTextureView(
                assets,
                modelPath,
                ReadName(materialV2.albedoTexture, MaximumPath),
                LamaPon::TextureLoader::TextureUsage::Color);
            result.normal = LoadTextureView(
                assets,
                modelPath,
                ReadName(materialV2.normalTexture, MaximumPath),
                LamaPon::TextureLoader::TextureUsage::NormalMap);
            return result;
        }

        const auto isZero = [](const DirectX::XMFLOAT4& value) noexcept
        {
            return value.x == 0.0f
                && value.y == 0.0f
                && value.z == 0.0f
                && value.w == 0.0f;
        };
        if (!isZero(material.ambient) || !isZero(material.diffuse))
        {
            result.baseColor = {
                material.diffuse.x,
                material.diffuse.y,
                material.diffuse.z,
                material.diffuse.w != 1.0f && material.diffuse.w != 0.0f
                    ? material.diffuse.w
                    : 1.0f };
            result.emissive = {
                material.emissive.x,
                material.emissive.y,
                material.emissive.z };
            if (material.power > 0.0f
                && (material.specular.x != 0.0f
                    || material.specular.y != 0.0f
                    || material.specular.z != 0.0f))
            {
                result.roughness = std::clamp(
                    std::sqrt(2.0f / (material.power + 2.0f)),
                    0.04f,
                    1.0f);
            }
        }
        result.albedo = LoadTextureView(
            assets,
            modelPath,
            ReadName(material.diffuseTexture, MaximumPath),
            LamaPon::TextureLoader::TextureUsage::Color);
        // DirectXTKはtangentの無い頂点ではnormal mapを捨てます。
        if (hasTangents)
        {
            result.normal = LoadTextureView(
                assets,
                modelPath,
                ReadName(material.normalTexture, MaximumPath),
                LamaPon::TextureLoader::TextureUsage::NormalMap);
        }
        return result;
    }

    void ExpandBounds(
        LamaPon::SkeletalModel& model,
        const LamaPon::Bounds3D& bounds) noexcept
    {
        if (!model.hasLocalBounds)
        {
            model.localBounds = bounds;
            model.hasLocalBounds = true;
            return;
        }
        auto& minimum = model.localBounds.minimum;
        auto& maximum = model.localBounds.maximum;
        minimum.x = (std::min)(minimum.x, bounds.minimum.x);
        minimum.y = (std::min)(minimum.y, bounds.minimum.y);
        minimum.z = (std::min)(minimum.z, bounds.minimum.z);
        maximum.x = (std::max)(maximum.x, bounds.maximum.x);
        maximum.y = (std::max)(maximum.y, bounds.maximum.y);
        maximum.z = (std::max)(maximum.z, bounds.maximum.z);
    }
}

namespace LamaPon
{
    std::shared_ptr<SkeletalModel> SdkmeshImporter::Load(
        AssetManager& assets,
        const std::filesystem::path& path)
    {
        const auto fileBytes = assets.ReadFileBytes(path);
        return LoadFromMemory(assets, fileBytes, path);
    }

    std::shared_ptr<SkeletalModel> SdkmeshImporter::LoadFromMemory(
        AssetManager& assets,
        const std::span<const std::uint8_t> bytes,
        const std::filesystem::path& sourcePath)
    {
        const auto header = ReadArray<SdkmeshHeader>(
            bytes,
            0u,
            1u,
            "header").front();
        // DirectXTKと同じく、header sizeはVB／IB headerまでを含みます。
        const std::uint64_t expectedHeaderSize = sizeof(SdkmeshHeader)
            + static_cast<std::uint64_t>(header.numVertexBuffers)
                * sizeof(SdkmeshVertexBufferHeader)
            + static_cast<std::uint64_t>(header.numIndexBuffers)
                * sizeof(SdkmeshIndexBufferHeader);
        if ((header.version != SdkmeshVersion
                && header.version != SdkmeshVersion2)
            || header.isBigEndian != 0u
            || header.headerSize != expectedHeaderSize
            || header.headerSize > bytes.size())
        {
            throw std::runtime_error(
                "The SDKMESH header is unsupported: "
                + PathToUtf8(sourcePath));
        }
        if (header.numMeshes == 0u
            || header.numVertexBuffers == 0u
            || header.numIndexBuffers == 0u
            || header.numTotalSubsets == 0u
            || header.numMaterials == 0u)
        {
            throw std::runtime_error(
                "The SDKMESH file has no drawable data: "
                + PathToUtf8(sourcePath));
        }
        const auto bufferDataOffset =
            header.headerSize + header.nonBufferDataSize;
        if (bufferDataOffset > bytes.size()
            || header.bufferDataSize > bytes.size() - bufferDataOffset)
        {
            throw std::runtime_error("The SDKMESH buffer data is truncated.");
        }

        const auto vertexBuffers = ReadArray<SdkmeshVertexBufferHeader>(
            bytes,
            header.vertexStreamHeadersOffset,
            header.numVertexBuffers,
            "vertex buffer headers");
        const auto indexBuffers = ReadArray<SdkmeshIndexBufferHeader>(
            bytes,
            header.indexStreamHeadersOffset,
            header.numIndexBuffers,
            "index buffer headers");
        const auto meshes = ReadArray<SdkmeshMesh>(
            bytes,
            header.meshDataOffset,
            header.numMeshes,
            "meshes");
        const auto subsets = ReadArray<SdkmeshSubset>(
            bytes,
            header.subsetDataOffset,
            header.numTotalSubsets,
            "subsets");
        const auto materials = ReadArray<SdkmeshMaterial>(
            bytes,
            header.materialDataOffset,
            header.numMaterials,
            "materials");
        const auto materialsV2 = ReadArray<SdkmeshMaterialV2>(
            bytes,
            header.materialDataOffset,
            header.numMaterials,
            "materials");

        std::vector<VertexLayout> layouts;
        layouts.reserve(vertexBuffers.size());
        for (const auto& vertexBuffer : vertexBuffers)
        {
            if (vertexBuffer.strideBytes == 0u
                || vertexBuffer.numVertices
                    > vertexBuffer.sizeBytes / vertexBuffer.strideBytes
                || vertexBuffer.dataOffset > bytes.size()
                || vertexBuffer.sizeBytes
                    > bytes.size() - vertexBuffer.dataOffset)
            {
                throw std::runtime_error(
                    "The SDKMESH vertex buffer is out of range.");
            }
            layouts.push_back(ParseVertexLayout(vertexBuffer));
        }

        auto model = std::make_shared<SkeletalModel>();
        std::vector<ImportedMaterial> importedMaterials(materials.size());
        for (const auto& mesh : meshes)
        {
            if (mesh.numSubsets == 0u
                || mesh.numVertexBuffers == 0u
                || mesh.indexBuffer >= indexBuffers.size()
                || mesh.vertexBuffers[0] >= vertexBuffers.size())
            {
                throw std::runtime_error(
                    "The SDKMESH mesh references invalid buffers.");
            }
            const auto subsetTable = ReadArray<std::uint32_t>(
                bytes,
                mesh.subsetOffset,
                mesh.numSubsets,
                "subset table");
            // D3D11のCalculateModelBoundsと同じく、ファイルのmesh境界を使います。
            const Bounds3D meshBounds{
                {
                    mesh.boundingBoxCenter.x - mesh.boundingBoxExtents.x,
                    mesh.boundingBoxCenter.y - mesh.boundingBoxExtents.y,
                    mesh.boundingBoxCenter.z - mesh.boundingBoxExtents.z
                },
                {
                    mesh.boundingBoxCenter.x + mesh.boundingBoxExtents.x,
                    mesh.boundingBoxCenter.y + mesh.boundingBoxExtents.y,
                    mesh.boundingBoxCenter.z + mesh.boundingBoxExtents.z
                } };
            ExpandBounds(*model, meshBounds);

            // DirectXTKはbone無しで読むため、frameの行列は描画へ使いません。
            SkeletalNode node;
            node.name = ReadName(mesh.name, MaximumName);
            const auto meshNode = model->nodes.size();
            model->nodes.push_back(std::move(node));

            const auto vertexBufferIndex = mesh.vertexBuffers[0];
            const auto& vertexBuffer = vertexBuffers[vertexBufferIndex];
            const auto& layout = layouts[vertexBufferIndex];
            const auto& indexBuffer = indexBuffers[mesh.indexBuffer];
            const auto indices = ReadIndices(bytes, indexBuffer);
            for (const auto subsetIndex : subsetTable)
            {
                if (subsetIndex >= subsets.size())
                {
                    throw std::runtime_error(
                        "The SDKMESH mesh references an invalid subset.");
                }
                const auto& subset = subsets[subsetIndex];
                if (subset.materialId >= materials.size())
                {
                    throw std::runtime_error(
                        "The SDKMESH subset references an invalid material.");
                }
                if (subset.indexStart > indices.size()
                    || subset.indexCount > indices.size() - subset.indexStart)
                {
                    throw std::runtime_error(
                        "The SDKMESH subset index range is invalid.");
                }
                // D3D12の基本3D pipelineは三角形だけを描きます。
                if (subset.primitiveType != PrimitiveTriangleList
                    && subset.primitiveType != PrimitiveTriangleStrip)
                {
                    continue;
                }
                const std::span<const std::uint32_t> subsetIndices(
                    indices.data() + subset.indexStart,
                    static_cast<std::size_t>(subset.indexCount));
                auto triangles =
                    subset.primitiveType == PrimitiveTriangleStrip
                    ? ExpandTriangleStrip(
                        subsetIndices,
                        indexBuffer.indexType == Index16
                            ? 0xffffu
                            : 0xffffffffu)
                    : std::vector<std::uint32_t>(
                        subsetIndices.begin(),
                        subsetIndices.end());
                triangles.resize(triangles.size() - triangles.size() % 3u);
                if (triangles.empty())
                {
                    continue;
                }

                // DirectXTKのDrawIndexedと同じく、VertexStartを各indexの基準に
                // します。使われている範囲の頂点だけをCPU幾何へ写します。
                const auto [minimumIndex, maximumIndex] =
                    std::ranges::minmax(triangles);
                const std::uint64_t firstVertex =
                    subset.vertexStart + minimumIndex;
                const std::uint64_t vertexCount =
                    static_cast<std::uint64_t>(maximumIndex)
                    - minimumIndex + 1u;
                if (firstVertex > vertexBuffer.numVertices
                    || vertexCount > vertexBuffer.numVertices - firstVertex)
                {
                    throw std::runtime_error(
                        "The SDKMESH subset contains an invalid vertex index.");
                }

                auto& material = importedMaterials[subset.materialId];
                if (!material.imported)
                {
                    material = ImportMaterial(
                        assets,
                        sourcePath,
                        header.version,
                        materials[subset.materialId],
                        materialsV2[subset.materialId],
                        layout.tangent.present);
                }

                std::vector<CpuModelVertex> vertices;
                vertices.reserve(static_cast<std::size_t>(vertexCount));
                const auto* const vertexData =
                    bytes.data() + vertexBuffer.dataOffset;
                for (std::uint64_t vertex{}; vertex < vertexCount; ++vertex)
                {
                    vertices.push_back(ReadVertex(
                        vertexData
                            + (firstVertex + vertex)
                                * vertexBuffer.strideBytes,
                        layout));
                }

                SkeletalPrimitive primitive;
                const auto* const vertexBytes =
                    reinterpret_cast<const std::uint8_t*>(vertices.data());
                primitive.cpuVertexData.assign(
                    vertexBytes,
                    vertexBytes + vertices.size() * sizeof(CpuModelVertex));
                primitive.cpuVertexStride = sizeof(CpuModelVertex);
                primitive.cpuIndices.reserve(triangles.size());
                for (const auto index : triangles)
                {
                    primitive.cpuIndices.push_back(index - minimumIndex);
                }
                primitive.indexCount = static_cast<std::uint32_t>(
                    primitive.cpuIndices.size());
                primitive.meshNode = meshNode;
                // DirectXTKはbone無しで読むため、skinは使わずbind poseで描きます。
                primitive.baseColor = material.baseColor;
                primitive.roughness = material.roughness;
                primitive.emissiveFactor = material.emissive;
                primitive.alpha = material.baseColor.w < 1.0f;
                primitive.localBounds = meshBounds;
                primitive.hasLocalBounds = true;
                primitive.embeddedTextures.albedo = material.albedo;
                primitive.embeddedTextures.normal = material.normal;
                primitive.embeddedTextures.emissiveFactor = material.emissive;
                model->primitives.push_back(std::move(primitive));
            }
        }

        if (model->primitives.empty())
        {
            throw std::runtime_error(
                "The SDKMESH file has no drawable triangles: "
                + PathToUtf8(sourcePath));
        }
        return model;
    }
}
