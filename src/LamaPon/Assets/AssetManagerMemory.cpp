#include "LamaPon/Assets/AssetManager.h"

#include "LamaPon/Animation/AnimationClip.h"
#include "LamaPon/Core/MemorySnapshot.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/DxgiTextureLayout.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <Model.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_set>

namespace LamaPon
{
    namespace
    {
        // ルートからの相対名を使い、使えなければファイル名を返す(path: 元資源のパス, assetRoot: 表示の基準ルート)。
        [[nodiscard]] std::string DisplayName(
            const std::filesystem::path& path,
            const std::filesystem::path& assetRoot)
        {
            if (path.empty())
            {
                return {};
            }
            if (!assetRoot.empty())
            {
                // 基準ルートからの相対パス
                const auto relative =
                    path.lexically_normal().lexically_relative(
                        assetRoot.lexically_normal());
                if (!relative.empty()
                    && relative.native().front() != L'.')
                {
                    return PathToUtf8(relative.generic_wstring());
                }
            }
            return PathToUtf8(path.filename());
        }

        // 画像の幅と高さを表示文字列にする(width: 幅（画素）, height: 高さ（画素）)。
        [[nodiscard]] std::string Dimensions(
            const std::uint32_t width,
            const std::uint32_t height)
        {
            return std::to_string(width) + "x" + std::to_string(height);
        }

        // ビューが指す2D画像の全ミップ・配列の容量を見積もる(view: 任意のD3D11ビュー)。
        [[nodiscard]] std::uint64_t ShaderResourceBytes(
            ID3D11ShaderResourceView* const view) noexcept
        {
            if (view == nullptr)
            {
                return 0;
            }
            try
            {
                // ビューが保持するnative資源
                Microsoft::WRL::ComPtr<ID3D11Resource> resource;
                view->GetResource(resource.GetAddressOf());
                // 集計するnative 2D画像
                Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                if (resource == nullptr
                    || FAILED(resource.As(&texture)))
                {
                    return 0;
                }
                // 集計するnative資源の設定
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                // 1配列要素の全ミップ推定量
                std::uint64_t total{};
                // 集計中ミップの幅（画素）
                std::uint32_t width = description.Width;
                // 集計中ミップの高さ（画素）
                std::uint32_t height = description.Height;
                // 集計するミップの番号
                for (UINT level{}; level < description.MipLevels; ++level)
                {
                    try
                    {
                        // 画像形式の最小行配置
                        const auto layout = Detail::RequiredTextureLayout(
                            description.Format,
                            width,
                            height);
                        total += static_cast<std::uint64_t>(
                                layout.minimumRowBytes)
                            * layout.rowCount;
                    }
                    // 未対応形式は1画素4バイトとして推定する。
                    catch (const std::exception&)
                    {
                        total += static_cast<std::uint64_t>(width)
                            * height * 4u;
                    }
                    width = std::max(width / 2u, 1u);
                    height = std::max(height / 2u, 1u);
                }
                return total * std::max<UINT>(description.ArraySize, 1);
            }
            catch (...)
            {
                return 0;
            }
        }

        struct ModelMemory final
        {
            // 幾何とnative画像のGPU推定量
            std::uint64_t gpuBytes{};
            // 保持するCPU幾何のバイト数
            std::uint64_t cpuBytes{};
            // 幾何の頂点数
            std::uint64_t vertices{};
            // 幾何の三角形数
            std::uint64_t triangles{};
            // 描画部分の数
            std::size_t parts{};
        };

        // 共通モデルの幾何とnative画像の容量を加算する(model: 集計するモデル, memory: 集計結果の追記先)。
        void AccumulateSkeletalModel(
            const SkeletalModel& model,
            ModelMemory& memory)
        {

            // モデル内の重複しないビュー
            std::unordered_set<ID3D11ShaderResourceView*> textures;
            // 集計する共通描画部分
            for (const auto& primitive : model.primitives)
            {
                // CPU幾何とLODのバイト数
                std::uint64_t geometryBytes =
                    primitive.cpuVertexData.size()
                    + primitive.cpuIndices.size()
                        * sizeof(std::uint32_t);
                // 集計するLODの索引列
                for (const auto& lodIndices : primitive.cpuLodIndices)
                {
                    geometryBytes +=
                        lodIndices.size() * sizeof(std::uint32_t);
                }
                // 共通幾何のCPU保持量とGPU転送量を同じバイト数として見積もる。
                memory.gpuBytes += geometryBytes;
                memory.cpuBytes += geometryBytes;
                if (primitive.cpuVertexStride > 0)
                {
                    memory.vertices +=
                        primitive.cpuVertexData.size()
                        / primitive.cpuVertexStride;
                }
                memory.triangles += primitive.indexCount / 3u;
                ++memory.parts;
                // 集計するnative画像ビュー
                for (auto* const texture : {
                        primitive.texture.Get(),
                        primitive.normalTexture.Get(),
                        primitive.roughnessTexture.Get(),
                        primitive.metallicTexture.Get(),
                        primitive.occlusionTexture.Get(),
                        primitive.emissiveTexture.Get() })
                {
                    if (texture != nullptr)
                    {
                        textures.insert(texture);
                    }
                }
            }
            // 集計するnative画像ビュー
            for (auto* const texture : textures)
            {
                memory.gpuBytes += ShaderResourceBytes(texture);
            }
        }

        // 共有バッファーの重複を除いてDirectXTK幾何の容量を加算する(model: 集計するモデル, memory: 集計結果の追記先)。
        void AccumulateDirectXModel(
            const DirectX::Model& model,
            ModelMemory& memory)
        {

            // 重複しないnativeバッファー
            std::unordered_set<ID3D11Buffer*> buffers;
            // 集計するDirectXTKメッシュ
            for (const auto& mesh : model.meshes)
            {
                if (mesh == nullptr)
                {
                    continue;
                }
                // 集計するDirectXTK描画部分
                for (const auto& part : mesh->meshParts)
                {
                    if (part == nullptr)
                    {
                        continue;
                    }
                    ++memory.parts;
                    memory.triangles += part->indexCount / 3u;
                    // 集計するnativeバッファー
                    for (auto* const buffer : {
                            part->vertexBuffer.Get(),
                            part->indexBuffer.Get() })
                    {
                        if (buffer == nullptr
                            || !buffers.insert(buffer).second)
                        {
                            continue;
                        }
                        // 集計するnative資源の設定
                        D3D11_BUFFER_DESC description{};
                        buffer->GetDesc(&description);
                        memory.gpuBytes += description.ByteWidth;
                        if (buffer == part->vertexBuffer.Get()
                            && part->vertexStride > 0)
                        {
                            memory.vertices +=
                                description.ByteWidth
                                / part->vertexStride;
                        }
                    }
                }
            }
        }
    }

    void AssetManager::AppendMemoryEntries(
        std::vector<MemorySnapshotEntry>& entries) const
    {
        {
            // 画像キャッシュの読み取りロック
            std::scoped_lock lock(m_textureMutex);
            // key: 画像のキャッシュキー, texture: 保持する画像
            for (const auto& [key, texture] : m_textureCache)
            {
                if (texture == nullptr)
                {
                    continue;
                }
                // キャッシュ資源の内訳
                MemorySnapshotEntry entry;
                entry.category = MemoryCategory::Texture;
                entry.name = texture->sourcePath.empty()
                    ? WideToUtf8(key)
                    : DisplayName(texture->sourcePath, m_assetRoot);
                entry.detail = Dimensions(texture->width, texture->height)
                    + (texture->isCube ? " キューブ" : "");
                entry.gpuBytes = texture->gpuBytes;
                if (entry.gpuBytes == 0)
                {
                    // 記録量が不明ならRGBA8の全ミップとして推定する。
                    entry.gpuBytes = EstimateTextureBytes(
                        texture->width,
                        texture->height,
                        texture->isCube ? 6u : 1u,
                        Detail::MaximumTextureMipLevels(
                            texture->width,
                            texture->height),
                        32,
                        false);
                    entry.detail += "（推定）";
                }
                entries.push_back(std::move(entry));
            }
        }

        {
            // モデルキャッシュの読取ロック
            std::scoped_lock lock(m_modelMutex);
            // key: モデルのキャッシュキー, model: 保持するモデル
            for (const auto& [key, model] : m_modelCache)
            {
                if (model == nullptr)
                {
                    continue;
                }
                // モデル幾何と画像の推定量
                ModelMemory memory;
                if (model->skeletalModel != nullptr)
                {
                    AccumulateSkeletalModel(*model->skeletalModel, memory);
                }
                if (model->model != nullptr)
                {
                    AccumulateDirectXModel(*model->model, memory);
                }
                // キャッシュ資源の内訳
                MemorySnapshotEntry entry;
                entry.category = MemoryCategory::Model;
                entry.name = model->sourcePath.empty()
                    ? WideToUtf8(key)
                    : DisplayName(model->sourcePath, m_assetRoot);
                // モデルの幾何数の表示文
                char detail[96]{};
                std::snprintf(
                    detail,
                    sizeof(detail),
                    "%zu部位 / %llu頂点 / %llu三角形",
                    memory.parts,
                    static_cast<unsigned long long>(memory.vertices),
                    static_cast<unsigned long long>(memory.triangles));
                entry.detail = detail;
                entry.gpuBytes = memory.gpuBytes;
                entry.cpuBytes = memory.cpuBytes;
                entries.push_back(std::move(entry));
            }
        }

        // key: 文字とフォントのキー, cached: 文字画像の記録
        for (const auto& [key, cached] : m_textCache)
        {
            if (cached.asset == nullptr)
            {
                continue;
            }
            // キャッシュ資源の内訳
            MemorySnapshotEntry entry;
            entry.category = MemoryCategory::TextTexture;
            // 文字とフォントのキーは表示用に80バイトで省略する。
            // 表示用に省略する文字画像キー
            auto name = WideToUtf8(key);
            if (name.size() > 80)
            {
                name.resize(80);
                // UTF-8の途中で切れた文字を取り除きます。
                while (!name.empty()
                    && (static_cast<unsigned char>(name.back()) & 0xC0u)
                        == 0x80u)
                {
                    name.pop_back();
                }
                if (!name.empty()
                    && (static_cast<unsigned char>(name.back()) & 0x80u)
                        != 0)
                {
                    name.pop_back();
                }
                name += "…";
            }
            entry.name = std::move(name);
            entry.detail =
                Dimensions(cached.asset->width, cached.asset->height);
            entry.gpuBytes = cached.bytes;
            entries.push_back(std::move(entry));
        }

        // key: アニメーションのキー, clip: 保持するクリップ
        for (const auto& [key, clip] : m_animationCache)
        {
            if (clip == nullptr)
            {
                continue;
            }
            // キャッシュ資源の内訳
            MemorySnapshotEntry entry;
            entry.category = MemoryCategory::Animation;
            entry.name = WideToUtf8(key);
            entry.detail =
                std::to_string(clip->Keyframes().size()) + "キー";
            entry.cpuBytes =
                clip->Keyframes().size() * sizeof(TransformKeyframe);
            entries.push_back(std::move(entry));
        }

        // key: データ資源のキー, dataAsset: 保持するデータ
        for (const auto& [key, dataAsset] : m_dataAssetCache)
        {
            if (dataAsset == nullptr)
            {
                continue;
            }
            // データ資源は値の容量を推定せず、キャッシュにある項目だけを記録する。
            // キャッシュ資源の内訳
            MemorySnapshotEntry entry;
            entry.category = MemoryCategory::DataAsset;
            entry.name = WideToUtf8(key);
            entries.push_back(std::move(entry));
        }

        {
            // 先読みキャッシュの読取ロック
            std::scoped_lock lock(m_prefetchMutex);
            // key: 先読みファイルのキー, bytes: 保持する内容
            for (const auto& [key, bytes] : m_prefetchedBytes)
            {
                if (bytes == nullptr)
                {
                    continue;
                }
                // キャッシュ資源の内訳
                MemorySnapshotEntry entry;
                entry.category = MemoryCategory::PrefetchedFile;
                entry.name = WideToUtf8(key);
                entry.cpuBytes = bytes->size();
                entries.push_back(std::move(entry));
            }
        }
    }
}
