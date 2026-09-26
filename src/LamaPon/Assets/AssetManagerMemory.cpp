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
        // アセットルート配下のファイルはルートからの相対パス、外部の
        // ファイルはファイル名だけを表示名にします。スナップショットを
        // 別のPCで開いても同じ資源として比較できます。
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

        [[nodiscard]] std::string Dimensions(
            const std::uint32_t width,
            const std::uint32_t height)
        {
            return std::to_string(width) + "x" + std::to_string(height);
        }

        // native SRVが指すtextureの全subresourceの大きさです。未対応の
        // 形式は1ピクセル4バイトとして数えます。
        [[nodiscard]] std::uint64_t ShaderResourceBytes(
            ID3D11ShaderResourceView* const view) noexcept
        {
            if (view == nullptr)
            {
                return 0;
            }
            try
            {
                Microsoft::WRL::ComPtr<ID3D11Resource> resource;
                view->GetResource(resource.GetAddressOf());
                Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                if (resource == nullptr
                    || FAILED(resource.As(&texture)))
                {
                    return 0;
                }
                D3D11_TEXTURE2D_DESC description{};
                texture->GetDesc(&description);
                std::uint64_t total{};
                std::uint32_t width = description.Width;
                std::uint32_t height = description.Height;
                for (UINT level{}; level < description.MipLevels; ++level)
                {
                    try
                    {
                        const auto layout = Detail::RequiredTextureLayout(
                            description.Format,
                            width,
                            height);
                        total += static_cast<std::uint64_t>(
                                layout.minimumRowBytes)
                            * layout.rowCount;
                    }
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
            std::uint64_t gpuBytes{};
            std::uint64_t cpuBytes{};
            std::uint64_t vertices{};
            std::uint64_t triangles{};
            std::size_t parts{};
        };

        void AccumulateSkeletalModel(
            const SkeletalModel& model,
            ModelMemory& memory)
        {
            // 同じ画像を複数のプリミティブが共有するため、SRV単位で
            // 1回だけ数えます。
            std::unordered_set<ID3D11ShaderResourceView*> textures;
            for (const auto& primitive : model.primitives)
            {
                std::uint64_t geometryBytes =
                    primitive.cpuVertexData.size()
                    + primitive.cpuIndices.size()
                        * sizeof(std::uint32_t);
                for (const auto& lodIndices : primitive.cpuLodIndices)
                {
                    geometryBytes +=
                        lodIndices.size() * sizeof(std::uint32_t);
                }
                // CPU側の複製（他Backend用のmirror）とGPUのbufferが
                // 同じ大きさで両方に存在します。
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
            for (auto* const texture : textures)
            {
                memory.gpuBytes += ShaderResourceBytes(texture);
            }
        }

        void AccumulateDirectXModel(
            const DirectX::Model& model,
            ModelMemory& memory)
        {
            // SDKMESHは複数のpartが同じbufferを共有するため重複を除きます。
            std::unordered_set<ID3D11Buffer*> buffers;
            for (const auto& mesh : model.meshes)
            {
                if (mesh == nullptr)
                {
                    continue;
                }
                for (const auto& part : mesh->meshParts)
                {
                    if (part == nullptr)
                    {
                        continue;
                    }
                    ++memory.parts;
                    memory.triangles += part->indexCount / 3u;
                    for (auto* const buffer : {
                            part->vertexBuffer.Get(),
                            part->indexBuffer.Get() })
                    {
                        if (buffer == nullptr
                            || !buffers.insert(buffer).second)
                        {
                            continue;
                        }
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
            std::scoped_lock lock(m_textureMutex);
            for (const auto& [key, texture] : m_textureCache)
            {
                if (texture == nullptr)
                {
                    continue;
                }
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
                    // 経路によっては読み込み時に量を控えていないため、
                    // RGBA8の全mip列として見積もります。
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
            std::scoped_lock lock(m_modelMutex);
            for (const auto& [key, model] : m_modelCache)
            {
                if (model == nullptr)
                {
                    continue;
                }
                ModelMemory memory;
                if (model->skeletalModel != nullptr)
                {
                    AccumulateSkeletalModel(*model->skeletalModel, memory);
                }
                if (model->model != nullptr)
                {
                    AccumulateDirectXModel(*model->model, memory);
                }
                MemorySnapshotEntry entry;
                entry.category = MemoryCategory::Model;
                entry.name = model->sourcePath.empty()
                    ? WideToUtf8(key)
                    : DisplayName(model->sourcePath, m_assetRoot);
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

        for (const auto& [key, cached] : m_textCache)
        {
            if (cached.asset == nullptr)
            {
                continue;
            }
            MemorySnapshotEntry entry;
            entry.category = MemoryCategory::TextTexture;
            // 鍵は文字列とフォント指定を連結したものです。長い文章でも
            // 一覧が崩れないよう先頭だけを使います。
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

        for (const auto& [key, clip] : m_animationCache)
        {
            if (clip == nullptr)
            {
                continue;
            }
            MemorySnapshotEntry entry;
            entry.category = MemoryCategory::Animation;
            entry.name = WideToUtf8(key);
            entry.detail =
                std::to_string(clip->Keyframes().size()) + "キー";
            entry.cpuBytes =
                clip->Keyframes().size() * sizeof(TransformKeyframe);
            entries.push_back(std::move(entry));
        }

        for (const auto& [key, dataAsset] : m_dataAssetCache)
        {
            if (dataAsset == nullptr)
            {
                continue;
            }
            // 値の内部表現は公開していないため、件数だけを記録します。
            MemorySnapshotEntry entry;
            entry.category = MemoryCategory::DataAsset;
            entry.name = WideToUtf8(key);
            entries.push_back(std::move(entry));
        }

        {
            std::scoped_lock lock(m_prefetchMutex);
            for (const auto& [key, bytes] : m_prefetchedBytes)
            {
                if (bytes == nullptr)
                {
                    continue;
                }
                MemorySnapshotEntry entry;
                entry.category = MemoryCategory::PrefetchedFile;
                entry.name = WideToUtf8(key);
                entry.cpuBytes = bytes->size();
                entries.push_back(std::move(entry));
            }
        }
    }
}
