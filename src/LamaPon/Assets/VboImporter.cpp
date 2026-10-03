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
// VBOの8バイトヘッダーと32バイト頂点をファイル上の詰めた配置で読む。
#pragma pack(push, 1)
    struct VboHeader final
    {
        // ファイル内の頂点数
        std::uint32_t vertexCount;
        // ファイル内の16ビット索引数
        std::uint32_t indexCount;
    };

    struct VboVertex final
    {
        // ファイル上の頂点位置
        DirectX::XMFLOAT3 position;
        // ファイル上の頂点法線
        DirectX::XMFLOAT3 normal;
        // ファイル上のUV座標
        DirectX::XMFLOAT2 textureCoordinate;
    };
#pragma pack(pop)

    // ImportedModelVertexと同じ60バイト配置を保つ。
    struct CpuModelVertex final
    {
        // モデル内の頂点位置
        DirectX::XMFLOAT3 position{};
        // モデル内の頂点法線
        DirectX::XMFLOAT3 normal{};
        // 未計算の接線と向き
        DirectX::XMFLOAT4 tangent{};
        // 既定の白RGBAカラー
        std::uint32_t color{ 0xffffffffu };
        // 元VBOのUV座標
        DirectX::XMFLOAT2 textureCoordinate{};
        // 未使用の骨番号
        std::uint32_t blendIndices{};
        // 未使用の骨ウェイト
        std::uint32_t blendWeights{};
    };

    static_assert(sizeof(VboHeader) == 8u);
    static_assert(sizeof(VboVertex) == 32u);
    static_assert(sizeof(CpuModelVertex) == 60u);

    // 配置境界を検証し未整列バイト列から値を写す(bytes: VBOのバイト列, offset: 読み取り位置, what: 診断用の対象名)。
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
        // 未整列領域からコピーした値
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
        // 読み込んだVBOの全バイト列
        const auto bytes = assets.ReadFileBytes(path);
        return LoadFromMemory(bytes, path);
    }

    std::shared_ptr<SkeletalModel> VboImporter::LoadFromMemory(
        const std::span<const std::uint8_t> bytes,
        const std::filesystem::path& sourcePath)
    {
        // 頂点数・索引数のヘッダー
        const auto header = ReadValue<VboHeader>(bytes, 0u, "header");
        if (header.vertexCount == 0u || header.indexCount == 0u)
        {
            throw std::runtime_error(
                "The VBO file has no drawable data: "
                + PathToUtf8(sourcePath));
        }
        // 頂点領域の64ビットバイト数
        const std::uint64_t vertexBytes64 =
            static_cast<std::uint64_t>(header.vertexCount)
            * sizeof(VboVertex);
        // 索引領域の64ビットバイト数
        const std::uint64_t indexBytes64 =
            static_cast<std::uint64_t>(header.indexCount)
            * sizeof(std::uint16_t);
        if (vertexBytes64 > (std::numeric_limits<std::size_t>::max)()
            || indexBytes64 > (std::numeric_limits<std::size_t>::max)())
        {
            throw std::runtime_error("The VBO geometry is too large.");
        }
        // 検証済みの頂点領域バイト数
        const auto vertexBytes = static_cast<std::size_t>(vertexBytes64);
        // 検証済みの索引領域バイト数
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

        // 描画用のCPU頂点配列
        std::vector<CpuModelVertex> vertices;
        vertices.reserve(header.vertexCount);
        // 頂点位置全体の軸平行境界
        Bounds3D bounds{};
        // 頂点または索引の読み込み番号
        for (std::uint32_t index{}; index < header.vertexCount; ++index)
        {
            // ファイル上のVBO頂点
            const auto source = ReadValue<VboVertex>(
                bytes,
                sizeof(VboHeader)
                    + static_cast<std::size_t>(index) * sizeof(VboVertex),
                "vertex buffer");
            // 描画形式へ変換した頂点
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

        // 32ビットへ拡張した頂点索引
        std::vector<std::uint32_t> indices;
        indices.reserve(header.indexCount);
        // 16ビット索引領域の開始位置
        const auto indexOffset = sizeof(VboHeader) + vertexBytes;
        // 頂点または索引の読み込み番号
        for (std::uint32_t index{}; index < header.indexCount; ++index)
        {
            // 16ビットの頂点索引
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
        // 両描画経路をそろえるため、三角形を作れない末尾索引を除く。
        indices.resize(indices.size() - indices.size() % 3u);
        if (indices.empty())
        {
            throw std::runtime_error(
                "The VBO file has no complete triangles: "
                + PathToUtf8(sourcePath));
        }

        // 単一の描画用メッシュ
        SkeletalPrimitive primitive;
        // 描画形式の頂点バイト列
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

        // 作成するCPUモデル
        auto model = std::make_shared<SkeletalModel>();
        // モデル名を持つ単一ノード
        SkeletalNode node;
        node.name = PathToUtf8(sourcePath.stem());
        model->nodes.push_back(std::move(node));
        model->primitives.push_back(std::move(primitive));
        model->localBounds = bounds;
        model->hasLocalBounds = true;
        return model;
    }
}
