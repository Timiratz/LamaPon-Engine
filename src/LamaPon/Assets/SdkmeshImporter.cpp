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

    // SDKMESHの旧材質形式の版
    constexpr std::uint32_t SdkmeshVersion = 101u;
    // SDKMESHのPBR材質形式の版
    constexpr std::uint32_t SdkmeshVersion2 = 200u;
    // 1宣言の最大属性数
    constexpr std::size_t MaximumVertexElements = 32u;
    // メッシュの最大頂点列数
    constexpr std::size_t MaximumVertexStreams = 16u;
    // 固定長名前のバイト数
    constexpr std::size_t MaximumName = 100u;
    // 固定長パスのバイト数
    constexpr std::size_t MaximumPath = 260u;

    // 32ビット実数1成分の形式
    constexpr std::uint8_t DeclarationFloat1 = 0u;
    // 32ビット実数2成分の形式
    constexpr std::uint8_t DeclarationFloat2 = 1u;
    // 32ビット実数3成分の形式
    constexpr std::uint8_t DeclarationFloat3 = 2u;
    // 32ビット実数4成分の形式
    constexpr std::uint8_t DeclarationFloat4 = 3u;
    // BGRA色の格納形式
    constexpr std::uint8_t DeclarationColor = 4u;
    // 符号無し8ビット4成分形式
    constexpr std::uint8_t DeclarationUByte4 = 5u;
    // 正規化8ビット4成分形式
    constexpr std::uint8_t DeclarationUByte4N = 8u;
    // 正規化16ビット4成分形式
    constexpr std::uint8_t DeclarationShort4N = 10u;
    // 半精度実数2成分の形式
    constexpr std::uint8_t DeclarationFloat16x2 = 15u;
    // 半精度実数4成分の形式
    constexpr std::uint8_t DeclarationFloat16x4 = 16u;
    // 頂点宣言の終了形式
    constexpr std::uint8_t DeclarationUnused = 17u;
    // 拡張頂点形式はDXGI_FORMATに32を加えた値を使う。
    // 10・10・10・2ビット形式
    constexpr std::uint8_t DeclarationR10G10B10A2 = 32u + 24u;
    // 11・11・10ビット実数形式
    constexpr std::uint8_t DeclarationR11G11B10 = 32u + 26u;
    // 正規化符号付き8ビット形式
    constexpr std::uint8_t DeclarationR8G8B8A8Snorm = 32u + 31u;

    // 位置属性の識別値
    constexpr std::uint8_t UsagePosition = 0u;
    // ボーン影響度属性の識別値
    constexpr std::uint8_t UsageBlendWeight = 1u;
    // ボーン番号属性の識別値
    constexpr std::uint8_t UsageBlendIndices = 2u;
    // 法線属性の識別値
    constexpr std::uint8_t UsageNormal = 3u;
    // UV属性の識別値
    constexpr std::uint8_t UsageTextureCoordinate = 5u;
    // 接線属性の識別値
    constexpr std::uint8_t UsageTangent = 6u;
    // 従法線属性の識別値
    constexpr std::uint8_t UsageBinormal = 7u;
    // 頂点色属性の識別値
    constexpr std::uint8_t UsageColor = 10u;

    // 三角形リストの識別値
    constexpr std::uint32_t PrimitiveTriangleList = 0u;
    // 三角形ストリップの識別値
    constexpr std::uint32_t PrimitiveTriangleStrip = 1u;
    // 16ビット索引の識別値
    constexpr std::uint32_t Index16 = 0u;
    // 32ビット索引の識別値
    constexpr std::uint32_t Index32 = 1u;

#pragma pack(push, 4)
    struct SdkmeshVertexElement final
    {
        // 頂点ストリーム番号
        std::uint16_t stream;
        // 属性の開始バイト位置
        std::uint16_t offset;
        // 属性の格納形式
        std::uint8_t type;
        // 元の頂点処理方法
        std::uint8_t method;
        // 属性の用途
        std::uint8_t usage;
        // 元の用途内の番号
        std::uint8_t usageIndex;
    };
#pragma pack(pop)

#pragma pack(push, 8)
    struct SdkmeshHeader final
    {
        // 格納形式の版番号
        std::uint32_t version;
        // ビッグエンディアンの有無
        std::uint8_t isBigEndian;
        // ヘッダー全体のバイト数
        std::uint64_t headerSize;
        // 非バッファー領域のバイト数
        std::uint64_t nonBufferDataSize;
        // バッファー領域のバイト数
        std::uint64_t bufferDataSize;
        // 頂点バッファーの数
        std::uint32_t numVertexBuffers;
        // 索引バッファーの数
        std::uint32_t numIndexBuffers;
        // メッシュの数
        std::uint32_t numMeshes;
        // 描画部分の総数
        std::uint32_t numTotalSubsets;
        // 元のフレーム数
        std::uint32_t numFrames;
        // 材質の数
        std::uint32_t numMaterials;
        // 頂点ヘッダーの先頭位置
        std::uint64_t vertexStreamHeadersOffset;
        // 索引ヘッダーの先頭位置
        std::uint64_t indexStreamHeadersOffset;
        // メッシュ情報の先頭位置
        std::uint64_t meshDataOffset;
        // 描画部分の先頭位置
        std::uint64_t subsetDataOffset;
        // 元フレーム情報の先頭位置
        std::uint64_t frameDataOffset;
        // 材質情報の先頭位置
        std::uint64_t materialDataOffset;
    };

    struct SdkmeshVertexBufferHeader final
    {
        // 頂点の数
        std::uint64_t numVertices;
        // 頂点列のバイト数
        std::uint64_t sizeBytes;
        // 1頂点のバイト数
        std::uint64_t strideBytes;
        // 頂点属性の宣言列
        std::array<SdkmeshVertexElement, MaximumVertexElements> declaration;
        // 頂点列の先頭バイト位置
        std::uint64_t dataOffset;
    };

    struct SdkmeshIndexBufferHeader final
    {
        // 索引の数
        std::uint64_t numIndices;
        // 索引列のバイト数
        std::uint64_t sizeBytes;
        // 16または32ビットの形式
        std::uint32_t indexType;
        // 索引列の先頭バイト位置
        std::uint64_t dataOffset;
    };

    struct SdkmeshMesh final
    {
        // 固定長のメッシュ名
        char name[MaximumName];
        // 頂点ストリームの数
        std::uint8_t numVertexBuffers;
        // 頂点ストリームの番号列
        std::uint32_t vertexBuffers[MaximumVertexStreams];
        // 使用する索引バッファー番号
        std::uint32_t indexBuffer;
        // 描画部分の数
        std::uint32_t numSubsets;
        // 元の影響フレーム数
        std::uint32_t numFrameInfluences;
        // 局所境界の中心
        DirectX::XMFLOAT3 boundingBoxCenter;
        // 局所境界の半幅
        DirectX::XMFLOAT3 boundingBoxExtents;
        // 描画部分番号列の先頭位置
        std::uint64_t subsetOffset;
        // 元の影響フレーム列の位置
        std::uint64_t frameInfluenceOffset;
    };

    struct SdkmeshSubset final
    {
        // 固定長の描画部分名
        char name[MaximumName];
        // 使用する材質の番号
        std::uint32_t materialId;
        // 三角形リスト等の構成形式
        std::uint32_t primitiveType;
        // 索引列の開始位置
        std::uint64_t indexStart;
        // 描画する索引の数
        std::uint64_t indexCount;
        // 索引に加える頂点開始位置
        std::uint64_t vertexStart;
        // 元の描画頂点数
        std::uint64_t vertexCount;
    };

    struct SdkmeshMaterial final
    {
        // 固定長の材質名
        char name[MaximumName];
        // 元の材質インスタンスパス
        char materialInstancePath[MaximumPath];
        // 拡散色画像名
        char diffuseTexture[MaximumPath];
        // 法線画像名
        char normalTexture[MaximumPath];
        // 未使用の鏡面反射画像名
        char specularTexture[MaximumPath];
        // 拡散色と不透明度
        DirectX::XMFLOAT4 diffuse;
        // 元の環境光色
        DirectX::XMFLOAT4 ambient;
        // 鏡面反射の有効判定色
        DirectX::XMFLOAT4 specular;
        // 発光色
        DirectX::XMFLOAT4 emissive;
        // 粗さへ近似する反射指数
        float power;
        // 未使用の実行時ハンドル枠
        std::uint64_t runtimeHandles[6];
    };

    struct SdkmeshMaterialV2 final
    {
        // 固定長の材質名
        char name[MaximumName];
        // 未使用のRMA画像名
        char rmaTexture[MaximumPath];
        // 基本色画像名
        char albedoTexture[MaximumPath];
        // 法線画像名
        char normalTexture[MaximumPath];
        // 未使用の発光画像名
        char emissiveTexture[MaximumPath];
        // 不透明度、0は不透明
        float alpha;
        // 形式互換の予約領域
        char reserved[60];
        // 未使用の実行時ハンドル枠
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

    // ImportedModelVertexと同じ60バイトの配置を使う。
    struct CpuModelVertex final
    {
        // 局所座標の頂点位置
        DirectX::XMFLOAT3 position{};
        // 頂点法線
        DirectX::XMFLOAT3 normal{};
        // 接線と従法線の向き
        DirectX::XMFLOAT4 tangent{};
        // RGBA各8ビットの頂点色
        std::uint32_t color{ 0xffffffffu };
        // 最初のUV座標
        DirectX::XMFLOAT2 textureCoordinate{};
        // 4ボーン番号の8ビット列
        std::uint32_t blendIndices{};
        // 4影響度の8ビット列
        std::uint32_t blendWeights{};
    };

    static_assert(sizeof(CpuModelVertex) == 60u);

    struct VertexElement final
    {
        // 属性の開始バイト位置
        std::size_t offset{};
        // 属性の格納形式
        std::uint8_t type{};
        // 対応する属性の有無
        bool present{};
    };

    struct VertexLayout final
    {
        // 位置属性の配置
        VertexElement position;
        // 法線属性の配置
        VertexElement normal;
        // 接線属性の配置
        VertexElement tangent;
        // 色属性の配置
        VertexElement color;
        // 最初のUV属性の配置
        VertexElement textureCoordinate;
        // ボーン番号属性の配置
        VertexElement blendIndices;
        // 影響度属性の配置
        VertexElement blendWeights;
    };

    struct ImportedMaterial final
    {
        // 基本色と不透明度
        DirectX::XMFLOAT4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        // 発光色
        DirectX::XMFLOAT3 emissive{};
        // 粗さ、反射無効時は1
        float roughness{ 1.0f };
        // 基本色画像のビュー
        LamaPon::GraphicsViewHandle albedo;
        // 法線画像のビュー
        LamaPon::GraphicsViewHandle normal;
        // 材質の変換済み状態
        bool imported{};
    };

    // 範囲を検査して指定型の配列をコピーする(bytes: 元バイト列, offset: 開始バイト位置, count: 要素数, what: 診断用の対象名)。
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
        // 指定範囲からコピーした配列
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

    // 固定長文字列をNULまたは容量まで読む(text: 文字列の先頭, capacity: 最大バイト数)。
    [[nodiscard]] std::string ReadName(
        const char* const text,
        const std::size_t capacity)
    {
        // 文字列の終端位置
        const auto* const end = std::find(text, text + capacity, '\0');
        return std::string(text, end);
    }

    // 方向の頂点形式のバイト数を返し、未対応形式には0を返す(type: 頂点形式)。
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

    // 色の頂点形式のバイト数を返し、未対応形式には0を返す(type: 頂点形式)。
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

    // UVの頂点形式のバイト数を返し、未対応形式には0を返す(type: 頂点形式)。
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

    // 連続する対応要素を読み、未対応要素や隙間で打ち切る(vertexBuffer: 元頂点バッファーの宣言)。
    [[nodiscard]] VertexLayout ParseVertexLayout(
        const SdkmeshVertexBufferHeader& vertexBuffer)
    {
        // 読み取った頂点属性の配置
        VertexLayout layout;
        // 次の属性のバイト位置
        std::size_t offset{};
        // 元の頂点属性宣言
        for (const auto& element : vertexBuffer.declaration)
        {
            if (element.usage == 0xffu
                || element.type == DeclarationUnused
                || element.offset != offset)
            {
                break;
            }
            // 属性のバイト数
            std::size_t size{};
            // 属性配置の書き込み先
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
                // 共通描画では最初のUVだけを使う。
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

    // 整列されていない値を指定型へコピーする(data: 必要サイズを持つ元バイト列)。
    template<typename T>
    [[nodiscard]] T ReadValue(const std::uint8_t* const data) noexcept
    {
        // 展開先の値
        T value{};
        std::memcpy(&value, data, sizeof(T));
        return value;
    }

    // 0〜1の成分を−1〜1へ写す(value: 元の3成分)。
    [[nodiscard]] DirectX::XMFLOAT3 Unbias(
        const DirectX::XMFLOAT3& value) noexcept
    {
        return {
            value.x * 2.0f - 1.0f,
            value.y * 2.0f - 1.0f,
            value.z * 2.0f - 1.0f };
    }

    // 頂点の方向成分を展開する(data: 成分の先頭, type: 頂点形式)。
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
            // 展開する8ビット成分列
            const auto bytes = ReadValue<std::array<std::uint8_t, 4>>(data);
            return Unbias({
                bytes[0] / 255.0f,
                bytes[1] / 255.0f,
                bytes[2] / 255.0f });
        }
        case DeclarationShort4N:
        {
            // 展開する成分値の列
            const auto values = ReadValue<std::array<std::int16_t, 4>>(data);
            return {
                (std::max)(values[0] / 32767.0f, -1.0f),
                (std::max)(values[1] / 32767.0f, -1.0f),
                (std::max)(values[2] / 32767.0f, -1.0f) };
        }
        case DeclarationFloat16x4:
        {
            // 展開する成分値の列
            const auto values =
                ReadValue<std::array<DirectX::PackedVector::HALF, 4>>(data);
            return {
                DirectX::PackedVector::XMConvertHalfToFloat(values[0]),
                DirectX::PackedVector::XMConvertHalfToFloat(values[1]),
                DirectX::PackedVector::XMConvertHalfToFloat(values[2]) };
        }
        case DeclarationR10G10B10A2:
        {
            // 圧縮形式の成分値
            const auto packed = ReadValue<std::uint32_t>(data);
            return Unbias({
                (packed & 0x3ffu) / 1023.0f,
                ((packed >> 10u) & 0x3ffu) / 1023.0f,
                ((packed >> 20u) & 0x3ffu) / 1023.0f });
        }
        case DeclarationR11G11B10:
        {
            // 圧縮形式の成分値
            const auto packed =
                ReadValue<DirectX::PackedVector::XMFLOAT3PK>(data);
            // 展開先の値
            DirectX::XMFLOAT3 value{};
            DirectX::XMStoreFloat3(
                &value,
                DirectX::PackedVector::XMLoadFloat3PK(&packed));
            return Unbias(value);
        }
        case DeclarationR8G8B8A8Snorm:
        {
            // 展開する成分値の列
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

    // 頂点のUV先頭2成分を展開する(data: 成分の先頭, type: 頂点形式)。
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
            // 展開する成分値の列
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

    // 対応する頂点属性を共通CPU形式へ写す(vertex: 元頂点の先頭, layout: 属性の配置)。
    [[nodiscard]] CpuModelVertex ReadVertex(
        const std::uint8_t* const vertex,
        const VertexLayout& layout) noexcept
    {
        // 変換先の共通CPU頂点
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
            // 展開した接線方向
            const auto tangent = ReadDirection(
                vertex + layout.tangent.offset,
                layout.tangent.type);
            result.tangent = { tangent.x, tangent.y, tangent.z, 1.0f };
        }
        // 色はD3DCOLORだけを変換し、他形式は既定の白を使う。
        if (layout.color.present
            && layout.color.type == DeclarationColor)
        {
            // D3DCOLORのBGRAを、CMOと同じRGBAの並びへ入れ替えます。
            // 元のBGRA頂点色
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

    // 16または32ビット索引を32ビット列へ読み込む(bytes: 元バイト列, indexBuffer: 索引の配置と形式)。
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
            // 展開する成分値の列
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

    // 区切りごとに向きを揃えてストリップを三角形列へ変換する(strip: 元の索引列, restartIndex: 区切りの索引値)。
    [[nodiscard]] std::vector<std::uint32_t> ExpandTriangleStrip(
        const std::span<const std::uint32_t> strip,
        const std::uint32_t restartIndex)
    {
        // 展開した三角形索引列
        std::vector<std::uint32_t> triangles;
        // 区切り内の開始位置
        std::size_t runStart{};
        // 再開索引で区切り、奇数三角形の向きを反転して縮退三角形を除く。
        while (runStart < strip.size())
        {
            // 次の区切り位置
            auto runEnd = runStart;
            while (runEnd < strip.size() && strip[runEnd] != restartIndex)
            {
                ++runEnd;
            }
            // 三角形索引列の位置
            for (std::size_t index = runStart; index + 2u < runEnd; ++index)
            {
                // 三角形の第1頂点番号
                auto a = strip[index];
                // 三角形の第2頂点番号
                auto b = strip[index + 1u];
                // 三角形の第3頂点番号
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

    // モデルの親を基準に画像ビューを取得する(assets: 画像の取得元, modelPath: 元モデルのパス, name: UTF-8の画像名, usage: 色または法線の用途)。
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

        // 画像の解決先パス
        auto path = LamaPon::PathFromUtf8(name);
        if (path.is_relative())
        {
            path = modelPath.parent_path() / path;
        }
        // 取得した画像アセット
        const auto texture = assets.LoadTexture(path, usage);
        if (texture == nullptr)
        {
            return {};
        }
        // 取得時点のGPUリソース
        const auto resources = texture->resources.Acquire();
        return resources != nullptr
            ? resources->shaderResourceView
            : LamaPon::GraphicsViewHandle{};
    }

    // DirectXTK互換の材質を共通描画形式へ変換する(assets: 画像の取得元, modelPath: 元モデルのパス, version: 格納形式の版, material: 101版の材質, materialV2: 200版の材質, hasTangents: 接線属性の有無)。
    [[nodiscard]] ImportedMaterial ImportMaterial(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& modelPath,
        const std::uint32_t version,
        const SdkmeshMaterial& material,
        const SdkmeshMaterialV2& materialV2,
        const bool hasTangents)
    {
        // 変換先の共通材質
        ImportedMaterial result;
        result.imported = true;
        if (version == SdkmeshVersion2)
        {
            // 200版でもDirectXTK互換のため、albedoとnormal画像だけを使う。
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

        // 4成分が全て0かを調べる(value: 判定する色)。
        const auto isZero = [](const DirectX::XMFLOAT4& value) noexcept
        {
            return value.x == 0.0f
                && value.y == 0.0f
                && value.z == 0.0f
                && value.w == 0.0f;
        };
        // 101版の色が無ければ白を使い、不透明度0は不透明として扱う。
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
            // 鏡面反射色と正の反射指数が揃う場合だけ、指数から粗さを近似する。
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
        // 101版では接線の無い頂点に法線画像を適用しない。
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

    // モデルの境界をメッシュ境界と統合する(model: 更新するモデル, bounds: メッシュの局所境界)。
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
        // モデル境界の最小座標
        auto& minimum = model.localBounds.minimum;
        // モデル境界の最大座標
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
        // 元モデルのバイト列
        const auto fileBytes = assets.ReadFileBytes(path);
        return LoadFromMemory(assets, fileBytes, path);
    }

    std::shared_ptr<SkeletalModel> SdkmeshImporter::LoadFromMemory(
        AssetManager& assets,
        const std::span<const std::uint8_t> bytes,
        const std::filesystem::path& sourcePath)
    {
        // ファイルのヘッダー
        const auto header = ReadArray<SdkmeshHeader>(
            bytes,
            0u,
            1u,
            "header").front();

        // 頂点・索引ヘッダー込みの長さ
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
        // バッファー領域の開始位置
        const auto bufferDataOffset =
            header.headerSize + header.nonBufferDataSize;
        if (bufferDataOffset > bytes.size()
            || header.bufferDataSize > bytes.size() - bufferDataOffset)
        {
            throw std::runtime_error("The SDKMESH buffer data is truncated.");
        }

        // 頂点バッファーのヘッダー列
        const auto vertexBuffers = ReadArray<SdkmeshVertexBufferHeader>(
            bytes,
            header.vertexStreamHeadersOffset,
            header.numVertexBuffers,
            "vertex buffer headers");
        // 索引バッファーのヘッダー列
        const auto indexBuffers = ReadArray<SdkmeshIndexBufferHeader>(
            bytes,
            header.indexStreamHeadersOffset,
            header.numIndexBuffers,
            "index buffer headers");
        // 元メッシュの一覧
        const auto meshes = ReadArray<SdkmeshMesh>(
            bytes,
            header.meshDataOffset,
            header.numMeshes,
            "meshes");
        // 描画部分の一覧
        const auto subsets = ReadArray<SdkmeshSubset>(
            bytes,
            header.subsetDataOffset,
            header.numTotalSubsets,
            "subsets");
        // 101版として読んだ材質列
        const auto materials = ReadArray<SdkmeshMaterial>(
            bytes,
            header.materialDataOffset,
            header.numMaterials,
            "materials");
        // 200版として読んだ材質列
        const auto materialsV2 = ReadArray<SdkmeshMaterialV2>(
            bytes,
            header.materialDataOffset,
            header.numMaterials,
            "materials");

        // 頂点列ごとの属性配置
        std::vector<VertexLayout> layouts;
        layouts.reserve(vertexBuffers.size());
        // 使用する頂点バッファー情報
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

        // 読み込み先のCPUモデル
        auto model = std::make_shared<SkeletalModel>();
        // 材質ごとの変換結果
        std::vector<ImportedMaterial> importedMaterials(materials.size());
        // 読み込むメッシュ
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
            // メッシュの描画部分番号列
            const auto subsetTable = ReadArray<std::uint32_t>(
                bytes,
                mesh.subsetOffset,
                mesh.numSubsets,
                "subset table");

            // ファイルが指定する局所境界
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

            // DirectXTK互換のため、フレーム行列を適用せずメッシュごとにノードを置く。
            // 追加するメッシュのノード
            SkeletalNode node;
            node.name = ReadName(mesh.name, MaximumName);
            // 追加したメッシュノード番号
            const auto meshNode = model->nodes.size();
            model->nodes.push_back(std::move(node));

            // 先頭の頂点ストリーム番号
            const auto vertexBufferIndex = mesh.vertexBuffers[0];
            // 使用する頂点バッファー情報
            const auto& vertexBuffer = vertexBuffers[vertexBufferIndex];
            // 読み取った頂点属性の配置
            const auto& layout = layouts[vertexBufferIndex];
            // 使用する索引バッファー情報
            const auto& indexBuffer = indexBuffers[mesh.indexBuffer];
            // 読み込んだ32ビット索引列
            const auto indices = ReadIndices(bytes, indexBuffer);
            // 読み込む描画部分番号
            for (const auto subsetIndex : subsetTable)
            {
                if (subsetIndex >= subsets.size())
                {
                    throw std::runtime_error(
                        "The SDKMESH mesh references an invalid subset.");
                }
                // 読み込む描画部分
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
                // 描画部分の元索引列
                const std::span<const std::uint32_t> subsetIndices(
                    indices.data() + subset.indexStart,
                    static_cast<std::size_t>(subset.indexCount));
                // 展開した三角形索引列
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

                // minimumIndex: 最小索引, maximumIndex: 最大索引
                const auto [minimumIndex, maximumIndex] =
                    std::ranges::minmax(triangles);
                // コピーする頂点の先頭番号
                const std::uint64_t firstVertex =
                    subset.vertexStart + minimumIndex;
                // コピーする頂点範囲の長さ
                const std::uint64_t vertexCount =
                    static_cast<std::uint64_t>(maximumIndex)
                    - minimumIndex + 1u;
                if (firstVertex > vertexBuffer.numVertices
                    || vertexCount > vertexBuffer.numVertices - firstVertex)
                {
                    throw std::runtime_error(
                        "The SDKMESH subset contains an invalid vertex index.");
                }

                // 描画部分が使う変換済み材質
                auto& material = importedMaterials[subset.materialId];
                // 同じ材質の法線画像の有無は、最初に参照した描画部分の接線で決まる。
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

                // 変換した共通CPU頂点列
                std::vector<CpuModelVertex> vertices;
                vertices.reserve(static_cast<std::size_t>(vertexCount));
                // 元の頂点バッファーの先頭
                const auto* const vertexData =
                    bytes.data() + vertexBuffer.dataOffset;
                // コピー範囲内の頂点番号
                for (std::uint64_t vertex{}; vertex < vertexCount; ++vertex)
                {
                    vertices.push_back(ReadVertex(
                        vertexData
                            + (firstVertex + vertex)
                                * vertexBuffer.strideBytes,
                        layout));
                }

                // 追加するCPU描画部分
                SkeletalPrimitive primitive;
                // 共通CPU頂点列のバイト列
                const auto* const vertexBytes =
                    reinterpret_cast<const std::uint8_t*>(vertices.data());
                primitive.cpuVertexData.assign(
                    vertexBytes,
                    vertexBytes + vertices.size() * sizeof(CpuModelVertex));
                primitive.cpuVertexStride = sizeof(CpuModelVertex);
                primitive.cpuIndices.reserve(triangles.size());
                // 最小索引を引く元の頂点番号
                for (const auto index : triangles)
                {
                    primitive.cpuIndices.push_back(index - minimumIndex);
                }
                primitive.indexCount = static_cast<std::uint32_t>(
                    primitive.cpuIndices.size());
                primitive.meshNode = meshNode;

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
