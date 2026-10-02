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
// CMOの格納順とサイズを保つため、ファイル構造体の詰め物を除く。
#pragma pack(push, 1)
    struct CmoMaterial final
    {
        // 元の環境光色
        DirectX::XMFLOAT4 ambient;
        // 拡散色と不透明度
        DirectX::XMFLOAT4 diffuse;
        // 元の鏡面反射色
        DirectX::XMFLOAT4 specular;
        // 粗さへ近似する反射指数
        float specularPower;
        // 発光色
        DirectX::XMFLOAT4 emissive;
        // 互換のため未適用のUV変換
        DirectX::XMFLOAT4X4 uvTransform;
    };

    struct CmoSubMesh final
    {
        // 使用する材質の番号
        std::uint32_t materialIndex;
        // 使用する索引バッファー番号
        std::uint32_t indexBufferIndex;
        // 使用する頂点バッファー番号
        std::uint32_t vertexBufferIndex;
        // 索引列の開始位置
        std::uint32_t startIndex;
        // 三角形の数
        std::uint32_t primitiveCount;
    };

    struct CmoVertex final
    {
        // 局所座標の頂点位置
        DirectX::XMFLOAT3 position;
        // 頂点法線
        DirectX::XMFLOAT3 normal;
        // 接線と従法線の向き
        DirectX::XMFLOAT4 tangent;
        // RGBA各8ビットの頂点色
        std::uint32_t color;
        // 元のUV座標
        DirectX::XMFLOAT2 textureCoordinate;
    };

    struct CmoSkinningVertex final
    {
        // 影響する4ボーンの番号
        std::array<std::uint32_t, 4> boneIndices;
        // 4ボーンの影響度
        std::array<float, 4> boneWeights;
    };

    struct CmoMeshExtents final
    {
        // 境界球の中心X座標
        float centerX;
        // 境界球の中心Y座標
        float centerY;
        // 境界球の中心Z座標
        float centerZ;
        // 境界球の半径
        float radius;
        // 境界の最小X座標
        float minimumX;
        // 境界の最小Y座標
        float minimumY;
        // 境界の最小Z座標
        float minimumZ;
        // 境界の最大X座標
        float maximumX;
        // 境界の最大Y座標
        float maximumY;
        // 境界の最大Z座標
        float maximumZ;
    };

    struct CmoBone final
    {
        // 親ボーン番号、負数は根
        std::int32_t parentIndex;
        // スキン用の逆バインド行列
        DirectX::XMFLOAT4X4 inverseBindPose;
        // 元のバインド行列
        DirectX::XMFLOAT4X4 bindPose;
        // ボーンの局所変換
        DirectX::XMFLOAT4X4 localTransform;
    };

    struct CmoClip final
    {
        // クリップの開始秒
        float startTime;
        // クリップの終了秒
        float endTime;
        // 変換キーの数
        std::uint32_t keyCount;
    };

    struct CmoKeyframe final
    {
        // 変換するボーンの番号
        std::uint32_t boneIndex;
        // キーの時刻（秒）
        float time;
        // キーの変換行列
        DirectX::XMFLOAT4X4 transform;
    };
#pragma pack(pop)

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
        std::uint32_t color{};
        // V反転済みのUV座標
        DirectX::XMFLOAT2 textureCoordinate{};
        // 4ボーン番号の8ビット列
        std::uint32_t blendIndices{};
        // 4影響度の8ビット列
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
        // 借用したCMOバイト列を先頭から読む準備をする(bytes: 読み取り元のバイト列)。
        // 読み取り終了まで、呼出元は元バイト列を保持する。
        explicit CmoReader(const std::span<const std::uint8_t> bytes) noexcept
            : m_bytes(bytes)
        {
        }

        // 残りサイズを検査して次の値をコピーし、読み取り位置を進める。
        template<typename T>
        [[nodiscard]] T Read()
        {
            Ensure(sizeof(T));
            // 次の値のコピー
            T result{};
            std::memcpy(&result, m_bytes.data() + m_offset, sizeof(T));
            m_offset += sizeof(T);
            return result;
        }

        // 残りサイズを検査して次の配列をコピーする(count: 読み取る要素数)。
        template<typename T>
        [[nodiscard]] std::vector<T> ReadVector(const std::size_t count)
        {
            if (count > Remaining() / sizeof(T))
            {
                throw std::runtime_error("The CMO array is truncated.");
            }
            // 読み取った配列
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

        // UTF-16文字列を読み、末尾のNULを除く。
        [[nodiscard]] std::wstring ReadWideString()
        {
            // 文字列のUTF-16要素数
            const auto count = Read<std::uint32_t>();
            if (count > Remaining() / sizeof(wchar_t))
            {
                throw std::runtime_error("The CMO string is truncated.");
            }
            // 読み取った文字列
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
        // 読み取り位置から末尾までのバイト数を返す。
        [[nodiscard]] std::size_t Remaining() const noexcept
        {
            return m_bytes.size() - m_offset;
        }

        // 残りサイズが足りなければ例外を送出する(byteCount: 必要なバイト数)。
        void Ensure(const std::size_t byteCount) const
        {
            if (byteCount > Remaining())
            {
                throw std::runtime_error("The CMO file is truncated.");
            }
        }

        // 呼出元が保持するCMO列
        std::span<const std::uint8_t> m_bytes;
        // 次に読むバイト位置
        std::size_t m_offset{};
    };

    struct CmoMaterialRecord final
    {
        // 元のCMO材質
        CmoMaterial material{};
        // CMOの8枠の画像名
        std::array<std::wstring, 8> textures;
    };

    // 祖先8階層以内のModelTextureを探し、無ければモデルの親を返す(modelPath: 元モデルのパス)。
    [[nodiscard]] std::filesystem::path FindTextureDirectory(
        const std::filesystem::path& modelPath)
    {
        // 探索中の親フォルダー
        auto directory = modelPath.parent_path();
        // 探索した祖先の階層数
        for (std::size_t depth{};
            depth < 8u && !directory.empty();
            ++depth)
        {
            // 祖先の共有画像フォルダー
            const auto sharedTextures = directory / L"ModelTexture";
            if (std::filesystem::is_directory(sharedTextures))
            {
                return sharedTextures;
            }
            // 次に探索する親フォルダー
            const auto parent = directory.parent_path();
            if (parent == directory)
            {
                break;
            }
            directory = parent;
        }
        return modelPath.parent_path();
    }

    // 共有画像フォルダーとモデル脇から画像ビューを取得する(assets: 画像の取得元, modelPath: 元モデルのパス, name: 画像名, usage: 色または法線の用途)。
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
        // 画像の候補パス
        auto path = std::filesystem::path(name);
        if (!path.is_absolute())
        {
            path = FindTextureDirectory(modelPath) / path;
            if (!assets.FileExists(path)
                && FindTextureDirectory(modelPath)
                    != modelPath.parent_path())
            {
                // モデル脇の画像パス
                const auto besideModel = modelPath.parent_path() / name;
                if (assets.FileExists(besideModel))
                {
                    path = besideModel;
                }
            }
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

    // 行列を位置・正規化した回転・倍率へ分解する(matrix: ボーンの局所変換)。
    [[nodiscard]] LamaPon::SkeletalPoseTransform DecomposeTransform(
        const DirectX::XMFLOAT4X4& matrix)
    {
        // 分解した拡大倍率
        DirectX::XMVECTOR scale{};
        // 分解した回転
        DirectX::XMVECTOR rotation{};
        // 分解した移動量
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
        // 分解した局所変換
        LamaPon::SkeletalPoseTransform result;
        DirectX::XMStoreFloat3(&result.scale, scale);
        DirectX::XMStoreFloat4(
            &result.rotation,
            DirectX::XMQuaternionNormalize(rotation));
        DirectX::XMStoreFloat3(&result.translation, translation);
        return result;
    }

    // 4値を下位から順に32ビットへ詰める(values: 詰める4バイト)。
    [[nodiscard]] std::uint32_t PackBytes(
        const std::array<std::uint8_t, 4>& values) noexcept
    {
        return values[0]
            | static_cast<std::uint32_t>(values[1]) << 8u
            | static_cast<std::uint32_t>(values[2]) << 16u
            | static_cast<std::uint32_t>(values[3]) << 24u;
    }

    // CMO頂点を共通CPU形式へ変換する(source: 元頂点, skinning: 任意のボーンと重み)。
    [[nodiscard]] CpuModelVertex ConvertVertex(
        const CmoVertex& source,
        const CmoSkinningVertex* const skinning)
    {
        // 変換先の共通CPU頂点
        CpuModelVertex result;
        result.position = source.position;
        result.normal = source.normal;
        result.tangent = source.tangent;
        result.color = source.color;
        // DirectXTK互換のためUV変換を適用せず、Vだけを反転する。
        result.textureCoordinate = {
            source.textureCoordinate.x,
            1.0f - source.textureCoordinate.y };
        if (skinning != nullptr)
        {
            // 8ビット化するボーン番号
            std::array<std::uint8_t, 4> indices{};
            // 8ビット化する影響度
            std::array<std::uint8_t, 4> weights{};
            // ボーンの影響枠
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

    // モデルの境界をメッシュの境界と統合する(model: 更新するモデル, extents: CMOの境界)。
    void ExpandBounds(
        LamaPon::SkeletalModel& model,
        const CmoMeshExtents& extents) noexcept
    {
        // メッシュ境界の最小座標
        const DirectX::XMFLOAT3 minimum{
            extents.minimumX,
            extents.minimumY,
            extents.minimumZ };
        // メッシュ境界の最大座標
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
        // CMOファイルのバイト列
        const auto bytes = assets.ReadFileBytes(path);
        // CMO列の読み取り位置
        CmoReader reader(bytes);
        // 読み込むメッシュの数
        const auto meshCount = reader.Read<std::uint32_t>();
        if (meshCount == 0u || meshCount > 65535u)
        {
            throw std::runtime_error(
                "The CMO file has an invalid mesh count: "
                + PathToUtf8(path));
        }

        // 読み込み先のCPUモデル
        auto model = std::make_shared<SkeletalModel>();
        // 読み込むメッシュ番号
        for (std::uint32_t meshIndex{};
            // 読み込むメッシュの数
            meshIndex < meshCount;
            ++meshIndex)
        {
            // 元のメッシュ名
            auto meshName = reader.ReadWideString();
            // 元の材質の数
            const auto materialCount = reader.Read<std::uint32_t>();
            if (materialCount > 65535u)
            {
                throw std::runtime_error(
                    "The CMO file has too many materials.");
            }
            // メッシュの材質一覧
            std::vector<CmoMaterialRecord> materials;
            materials.reserve(std::max(materialCount, 1u));
            // 読み込む材質番号
            for (std::uint32_t materialIndex{};
                // 元の材質の数
                materialIndex < materialCount;
                ++materialIndex)
            {
                static_cast<void>(reader.ReadWideString());
                // 読み込む材質と画像名
                CmoMaterialRecord record;
                record.material = reader.Read<CmoMaterial>();
                static_cast<void>(reader.ReadWideString());
                // 読み込む画像名の枠
                for (auto& texture : record.textures)
                {
                    texture = reader.ReadWideString();
                }
                materials.push_back(std::move(record));
            }
            if (materials.empty())
            {
                // 材質未指定時の既定値
                CmoMaterialRecord record;
                record.material.diffuse = {
                    0.8f, 0.8f, 0.8f, 1.0f };
                DirectX::XMStoreFloat4x4(
                    &record.material.uvTransform,
                    DirectX::XMMatrixIdentity());
                materials.push_back(std::move(record));
            }

            // ボーン情報の有無
            const bool hasSkeleton = reader.Read<std::uint8_t>() != 0u;
            // 材質ごとの描画部分
            const auto subMeshes = reader.ReadVector<CmoSubMesh>(
                reader.Read<std::uint32_t>());
            if (subMeshes.empty())
            {
                throw std::runtime_error("The CMO mesh has no submeshes.");
            }

            // 索引バッファーの数
            const auto indexBufferCount = reader.Read<std::uint32_t>();
            if (indexBufferCount == 0u || indexBufferCount > 65535u)
            {
                throw std::runtime_error(
                    "The CMO mesh has an invalid index buffer count.");
            }
            // 16ビット索引列の一覧
            std::vector<std::vector<std::uint16_t>> indexBuffers;
            indexBuffers.reserve(indexBufferCount);
            // 読み込むバッファーの番号
            for (std::uint32_t index{};
                // 索引バッファーの数
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

            // 頂点バッファーの数
            const auto vertexBufferCount = reader.Read<std::uint32_t>();
            if (vertexBufferCount == 0u || vertexBufferCount > 65535u)
            {
                throw std::runtime_error(
                    "The CMO mesh has an invalid vertex buffer count.");
            }
            // 元頂点列の一覧
            std::vector<std::vector<CmoVertex>> vertexBuffers;
            vertexBuffers.reserve(vertexBufferCount);
            // 読み込むバッファーの番号
            for (std::uint32_t index{};
                // 頂点バッファーの数
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

            // スキン頂点列の数
            const auto skinningBufferCount = reader.Read<std::uint32_t>();
            if (skinningBufferCount != 0u
                && skinningBufferCount != vertexBufferCount)
            {
                throw std::runtime_error(
                    "The CMO skinning streams do not match its vertices.");
            }
            // ボーンと影響度の頂点列
            std::vector<std::vector<CmoSkinningVertex>> skinningBuffers;
            skinningBuffers.reserve(skinningBufferCount);
            // 読み込むバッファーの番号
            for (std::uint32_t index{};
                // スキン頂点列の数
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

            // 元メッシュの局所境界
            const auto extents = reader.Read<CmoMeshExtents>();
            ExpandBounds(*model, extents);

            // 描画部分が属するノード番号
            std::size_t meshNode{};
            // スキン番号、無しは負数
            std::ptrdiff_t skinIndex = -1;
            if (hasSkeleton)
            {
                // メッシュのボーン数
                const auto boneCount = reader.Read<std::uint32_t>();
                if (boneCount == 0u || boneCount > 255u)
                {
                    throw std::runtime_error(
                        "The CMO skeleton has an unsupported bone count.");
                }
                // 追加するボーンの開始番号
                const auto nodeOffset = model->nodes.size();
                meshNode = nodeOffset;
                // 元ボーンの変換情報
                std::vector<CmoBone> bones;
                bones.reserve(boneCount);
                // 読み込むボーン番号
                for (std::uint32_t boneIndex{};
                    // メッシュのボーン数
                    boneIndex < boneCount;
                    ++boneIndex)
                {
                    // 追加するモデルノード
                    SkeletalNode node;
                    node.name = WideToUtf8(reader.ReadWideString());
                    // 元のボーンと変換行列
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
                    // 追加するスキン
                    SkeletalSkin skin;
                    skin.name = WideToUtf8(meshName);
                    skin.joints.reserve(bones.size());
                    skin.inverseBindMatrices.reserve(bones.size());
                    // 読み込むボーン番号
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

                // 読み飛ばすクリップ数
                const auto clipCount = reader.Read<std::uint32_t>();
                if (clipCount > 65535u)
                {
                    throw std::runtime_error(
                        "The CMO file has too many animation clips.");
                }
                // 読み飛ばすクリップ番号
                // クリップは読み飛ばし、再生用アニメーションへ変換しない。
                for (std::uint32_t clipIndex{};
                    // 読み飛ばすクリップ数
                    clipIndex < clipCount;
                    ++clipIndex)
                {
                    static_cast<void>(reader.ReadWideString());
                    // 読み飛ばすクリップの情報
                    const auto clip = reader.Read<CmoClip>();
                    static_cast<void>(
                        reader.ReadVector<CmoKeyframe>(clip.keyCount));
                }
            }
            else
            {
                // 追加するモデルノード
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

            // 読み込む描画部分
            for (const auto& subMesh : subMeshes)
            {
                if (subMesh.materialIndex >= materials.size()
                    || subMesh.indexBufferIndex >= indexBuffers.size()
                    || subMesh.vertexBufferIndex >= vertexBuffers.size())
                {
                    throw std::runtime_error(
                        "The CMO submesh references invalid data.");
                }
                // 描画部分の索引数
                const auto indexCount64 =
                    static_cast<std::uint64_t>(subMesh.primitiveCount) * 3u;
                // 元の索引バッファー
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
                // 元の頂点バッファー
                const auto& sourceVertices =
                    vertexBuffers[subMesh.vertexBufferIndex];
                // 描画部分が使う材質
                const auto& material = materials[subMesh.materialIndex];
                // 対応するスキン頂点列
                const auto* skinning = skinningBuffers.empty()
                    ? nullptr
                    : &skinningBuffers[subMesh.vertexBufferIndex];

                // 変換した共通CPU頂点列
                std::vector<CpuModelVertex> vertices;
                vertices.reserve(sourceVertices.size());
                // 変換する頂点番号
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

                // 追加するCPU描画部分
                SkeletalPrimitive primitive;
                // 共通CPU頂点列のバイト列
                const auto* vertexBytes = reinterpret_cast<const std::uint8_t*>(
                    vertices.data());
                primitive.cpuVertexData.assign(
                    vertexBytes,
                    vertexBytes + vertices.size() * sizeof(CpuModelVertex));
                primitive.cpuVertexStride = sizeof(CpuModelVertex);
                primitive.cpuIndices.reserve(
                    static_cast<std::size_t>(indexCount64));
                // 描画部分の索引終端
                const auto indexEnd = subMesh.startIndex
                    + static_cast<std::size_t>(indexCount64);
                // コピーする索引列の位置
                for (std::size_t index = subMesh.startIndex;
                    // 描画部分の索引終端
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
                // 材質上書き時の合成はModelRendererComponentが行う。
                primitive.baseColor = material.material.diffuse;
                // CMOの鏡面反射指数をPBRの粗さへ近似する。
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
                // DirectXTK互換のため、発光画像は使わず発光色だけを保持する。
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
