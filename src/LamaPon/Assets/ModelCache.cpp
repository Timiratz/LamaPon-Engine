#include "LamaPon/Assets/ModelCache.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/ModelLod.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <DDSTextureLoader.h>
#include <Effects.h>
#include <VertexTypes.h>
#include <WICTextureLoader.h>

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstring>
#include <cwchar>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{
    using ModelVertex =
        DirectX::VertexPositionNormalTangentColorTextureSkinning;

    // ファイル形式の版。保存項目やインポーターの構成を変更した場合に更新します。
    // キャッシュキーへ含まれるため、異なる版のエントリは使用されません。
    constexpr std::uint32_t FormatVersion = 3;

    constexpr char Magic[4] = { 'T', 'M', 'D', 'L' };

    // 破損ファイルによる過大な確保を防ぐための読み取り上限です。
    constexpr std::uint32_t MaximumCount = 1u << 24;
    constexpr std::uint32_t MaximumStringLength = 1u << 16;

    std::mutex g_directoryMutex;
    std::filesystem::path g_directoryOverride;

    [[nodiscard]] std::filesystem::path DefaultDirectory()
    {
        std::wstring localAppData(32768, L'\0');
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        std::filesystem::path root;
        if (length > 0 && length < localAppData.size())
        {
            localAppData.resize(length);
            root = localAppData;
        }
        else
        {
            std::error_code error;
            root = std::filesystem::temp_directory_path(error);
            if (error)
            {
                return {};
            }
        }
        return root / L"LamaPon" / L"model-cache";
    }

    [[nodiscard]] std::filesystem::path EntryPath(
        const std::uint64_t key)
    {
        const auto directory =
            LamaPon::ModelCache::CacheDirectory();
        if (directory.empty())
        {
            return {};
        }
        wchar_t name[32]{};
        swprintf_s(name, L"%016llx.tmdl", key);
        return directory / name;
    }

    [[nodiscard]] std::uint64_t HashBytes(
        const std::span<const std::uint8_t> bytes) noexcept
    {
        // キャッシュ識別用のFNV-1a 64です。暗号学的強度は必要ありません。
        std::uint64_t hash = 14695981039346656037ull;
        for (const std::uint8_t byte : bytes)
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    void WriteFileAtomically(
        const std::filesystem::path& destination,
        const std::vector<std::uint8_t>& bytes)
    {
        // 同じモデルが同期要求とバックグラウンド準備で重なっても、
        // 共通の.tmpファイルを同時に書かないようにします。
        static std::mutex writeMutex;
        const std::scoped_lock lock(writeMutex);
        // 途中で落ちても壊れたファイルが残らないよう、別名で書いて
        // から置き換えます（shader-cache／texture-cacheと同じ）。
        auto temporary = destination;
        temporary += L".tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return;
            }
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!output)
            {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, destination, error);
        if (error)
        {
            std::filesystem::remove(temporary, error);
        }
    }

    // 書き込み処理

    void AppendBytes(
        std::vector<std::uint8_t>& output,
        const void* data,
        const std::size_t size)
    {
        const auto* begin =
            static_cast<const std::uint8_t*>(data);
        output.insert(output.end(), begin, begin + size);
    }

    void AppendU8(
        std::vector<std::uint8_t>& output,
        const std::uint8_t value)
    {
        output.push_back(value);
    }

    void AppendU32(
        std::vector<std::uint8_t>& output,
        const std::uint32_t value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    void AppendU64(
        std::vector<std::uint8_t>& output,
        const std::uint64_t value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    void AppendI64(
        std::vector<std::uint8_t>& output,
        const std::int64_t value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    void AppendI32(
        std::vector<std::uint8_t>& output,
        const std::int32_t value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    void AppendF32(
        std::vector<std::uint8_t>& output,
        const float value)
    {
        AppendBytes(output, &value, sizeof(value));
    }

    void AppendString(
        std::vector<std::uint8_t>& output,
        const std::string& value)
    {
        AppendU32(
            output,
            static_cast<std::uint32_t>(value.size()));
        AppendBytes(output, value.data(), value.size());
    }

    // 読み取り処理

    // 範囲外を検出した場合はfailを設定し、以降の読み取りを無効にします。
    struct Reader final
    {
        const std::uint8_t* data{};
        std::size_t size{};
        std::size_t offset{};
        bool failed{};

        [[nodiscard]] bool ReadRaw(
            void* destination,
            const std::size_t count) noexcept
        {
            if (failed || offset + count > size)
            {
                failed = true;
                return false;
            }
            std::memcpy(destination, data + offset, count);
            offset += count;
            return true;
        }

        template <typename T>
        [[nodiscard]] T Read() noexcept
        {
            T value{};
            static_cast<void>(ReadRaw(&value, sizeof(T)));
            return value;
        }

        [[nodiscard]] std::string ReadString() noexcept
        {
            const auto length = Read<std::uint32_t>();
            if (failed || length > MaximumStringLength
                || offset + length > size)
            {
                failed = true;
                return {};
            }
            std::string value(
                reinterpret_cast<const char*>(data + offset),
                length);
            offset += length;
            return value;
        }
    };

    void ThrowIfFailed(
        const HRESULT result,
        const char* what)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(what);
        }
    }

    void CreateBuffer(
        ID3D11Device* device,
        LamaPon::AssetManager& assets,
        const void* data,
        const std::size_t byteCount,
        const UINT bindFlags,
        ID3D11Buffer** output)
    {
        if (byteCount == 0
            || byteCount > std::numeric_limits<UINT>::max())
        {
            throw std::runtime_error(
                "Cached model buffer has an invalid size.");
        }
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(byteCount);
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = bindFlags;
        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = data;
        assets.WaitForModelUploadBudget(byteCount);
        ThrowIfFailed(
            device->CreateBuffer(
                &description,
                &initialData,
                output),
            "Creating a cached model buffer");
    }

    // キャッシュ復元後も直接インポート時と同じ描画になるよう、
    // 各インポーターと同じエフェクトと入力レイアウトを作成します。
    void BuildPrimitiveEffect(
        ID3D11Device* device,
        LamaPon::SkeletalPrimitive& primitive)
    {
        primitive.effect =
            std::make_shared<DirectX::SkinnedEffect>(device);
        primitive.effect->SetWeightsPerVertex(4);
        const void* shaderBytecode{};
        std::size_t shaderBytecodeSize{};
        primitive.effect->GetVertexShaderBytecode(
            &shaderBytecode,
            &shaderBytecodeSize);
        ThrowIfFailed(
            device->CreateInputLayout(
                ModelVertex::InputElements,
                ModelVertex::InputElementCount,
                shaderBytecode,
                shaderBytecodeSize,
                primitive.inputLayout.ReleaseAndGetAddressOf()),
            "Creating a cached model input layout");
    }

    // FBXのEnableAlphaCutoutと同じもの。
    void BuildCutoutEffect(
        ID3D11Device* device,
        LamaPon::SkeletalPrimitive& primitive)
    {
        primitive.cutoutEffect =
            std::make_shared<DirectX::SkinnedDGSLEffect>(device);
        primitive.cutoutEffect->SetWeightsPerVertex(4);
        primitive.cutoutEffect->SetTextureEnabled(true);
        primitive.cutoutEffect->SetAlphaDiscardEnable(true);
        const void* shaderBytecode{};
        std::size_t shaderBytecodeSize{};
        primitive.cutoutEffect->GetVertexShaderBytecode(
            &shaderBytecode,
            &shaderBytecodeSize);
        ThrowIfFailed(
            device->CreateInputLayout(
                ModelVertex::InputElements,
                ModelVertex::InputElementCount,
                shaderBytecode,
                shaderBytecodeSize,
                primitive.cutoutInputLayout
                    .ReleaseAndGetAddressOf()),
            "Creating a cached model cutout input layout");
    }

    [[nodiscard]]
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        CreateImageView(
            LamaPon::AssetManager& assets,
            const std::span<const std::uint8_t> bytes,
            const bool isDds,
            const LamaPon::TextureLoader::TextureUsage usage)
    {
        // 初回インポートとキャッシュ復元で同じ形式になるよう、
        // 画像は共通の読み込み経路を通します。
        return assets.CreateTextureViewFromMemory(
            bytes,
            isDds,
            usage);
    }

    // チャンネルの読み書き

    void AppendVectorChannel(
        std::vector<std::uint8_t>& output,
        const LamaPon::SkeletalVectorChannel& channel)
    {
        AppendU8(
            output,
            static_cast<std::uint8_t>(channel.interpolation));
        AppendU32(
            output,
            static_cast<std::uint32_t>(channel.keys.size()));
        // キーは全てPOD（float10個）なので丸ごと書きます。
        AppendBytes(
            output,
            channel.keys.data(),
            channel.keys.size()
                * sizeof(LamaPon::SkeletalVectorKey));
    }

    void AppendQuaternionChannel(
        std::vector<std::uint8_t>& output,
        const LamaPon::SkeletalQuaternionChannel& channel)
    {
        AppendU8(
            output,
            static_cast<std::uint8_t>(channel.interpolation));
        AppendU32(
            output,
            static_cast<std::uint32_t>(channel.keys.size()));
        AppendBytes(
            output,
            channel.keys.data(),
            channel.keys.size()
                * sizeof(LamaPon::SkeletalQuaternionKey));
    }

    [[nodiscard]] bool ReadVectorChannel(
        Reader& reader,
        LamaPon::SkeletalVectorChannel& channel)
    {
        const auto interpolation = reader.Read<std::uint8_t>();
        if (interpolation > 2)
        {
            return false;
        }
        channel.interpolation =
            static_cast<LamaPon::SkeletalInterpolation>(
                interpolation);
        const auto keyCount = reader.Read<std::uint32_t>();
        if (reader.failed || keyCount > MaximumCount)
        {
            return false;
        }
        channel.keys.resize(keyCount);
        return reader.ReadRaw(
            channel.keys.data(),
            keyCount * sizeof(LamaPon::SkeletalVectorKey));
    }

    [[nodiscard]] bool ReadQuaternionChannel(
        Reader& reader,
        LamaPon::SkeletalQuaternionChannel& channel)
    {
        const auto interpolation = reader.Read<std::uint8_t>();
        if (interpolation > 2)
        {
            return false;
        }
        channel.interpolation =
            static_cast<LamaPon::SkeletalInterpolation>(
                interpolation);
        const auto keyCount = reader.Read<std::uint32_t>();
        if (reader.failed || keyCount > MaximumCount)
        {
            return false;
        }
        channel.keys.resize(keyCount);
        return reader.ReadRaw(
            channel.keys.data(),
            keyCount * sizeof(LamaPon::SkeletalQuaternionKey));
    }
}

namespace LamaPon::ModelCache
{
    void Recorder::RegisterEmbeddedImage(
        ID3D11ShaderResourceView* const view,
        const std::span<const std::uint8_t> bytes,
        const bool isDds,
        const bool hasTransparency,
        const TextureLoader::TextureUsage usage)
    {
        if (view == nullptr)
        {
            return;
        }
        Image image;
        image.bytes.assign(bytes.begin(), bytes.end());
        image.isDds = isDds;
        image.hasTransparency = hasTransparency;
        image.usage = usage;
        m_imageBySrv.emplace(
            view,
            static_cast<std::int32_t>(images.size()));
        images.emplace_back(std::move(image));
    }

    void Recorder::RegisterExternalImage(
        ID3D11ShaderResourceView* const view,
        const std::filesystem::path& path,
        const std::span<const std::uint8_t> bytes,
        const bool isDds,
        const bool hasTransparency,
        const TextureLoader::TextureUsage usage)
    {
        if (view == nullptr)
        {
            return;
        }
        Image image;
        image.external = true;
        image.externalPath = path;
        image.externalHash = HashBytes(bytes);
        image.isDds = isDds;
        image.hasTransparency = hasTransparency;
        image.usage = usage;
        m_imageBySrv.emplace(
            view,
            static_cast<std::int32_t>(images.size()));
        images.emplace_back(std::move(image));
    }

    void Recorder::RegisterDependency(
        const std::filesystem::path& path,
        const bool exists,
        const std::span<const std::uint8_t> bytes)
    {
        Dependency dependency;
        dependency.path = path;
        dependency.exists = exists;
        dependency.hash = exists ? HashBytes(bytes) : 0;
        dependencies.emplace_back(std::move(dependency));
    }

    void Recorder::AddGeometry(
        const void* const vertices,
        const std::size_t vertexCount,
        const std::size_t vertexStride,
        const std::uint32_t* const indices,
        const std::size_t indexCount)
    {
        Geometry geometry;
        geometry.vertexStride =
            static_cast<std::uint32_t>(vertexStride);
        const auto* begin =
            static_cast<const std::uint8_t*>(vertices);
        geometry.vertexBytes.assign(
            begin,
            begin + vertexCount * vertexStride);
        if (indices != nullptr)
        {
            geometry.indices.assign(
                indices,
                indices + indexCount);
        }
        geometries.emplace_back(std::move(geometry));
    }

    std::int32_t Recorder::ImageIndexFor(
        ID3D11ShaderResourceView* const view) const noexcept
    {
        if (view == nullptr)
        {
            return -1;
        }
        const auto found = m_imageBySrv.find(view);
        return found != m_imageBySrv.end() ? found->second : -1;
    }

    std::filesystem::path CacheDirectory()
    {
        {
            const std::lock_guard<std::mutex> lock(
                g_directoryMutex);
            if (!g_directoryOverride.empty())
            {
                return g_directoryOverride;
            }
        }
        return DefaultDirectory();
    }

    void SetCacheDirectoryOverride(
        std::filesystem::path directory)
    {
        const std::lock_guard<std::mutex> lock(g_directoryMutex);
        g_directoryOverride = std::move(directory);
    }

    std::uint64_t ComputeKey(
        const std::span<const std::uint8_t> sourceBytes,
        const std::uint32_t importerKind) noexcept
    {
        std::uint64_t hash = HashBytes(sourceBytes);
        hash ^= importerKind * 0x9e3779b97f4a7c15ull;
        hash *= 1099511628211ull;
        hash ^= FormatVersion;
        hash *= 1099511628211ull;
        // 頂点レイアウトの変更時にキャッシュを無効化するため、
        // レイアウト識別値をキーへ含めます。
        hash ^= sizeof(ModelVertex);
        hash *= 1099511628211ull;
        return hash;
    }

    std::shared_ptr<SkeletalModel> TryLoad(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        AssetManager& assets,
        const std::uint64_t key)
    {
        // API互換性のためcontext引数を保持しますが、テクスチャ復元では使用しません。
        static_cast<void>(context);
        try
        {
            const auto path = EntryPath(key);
            if (path.empty())
            {
                return nullptr;
            }
            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                return nullptr;
            }
            input.seekg(0, std::ios::end);
            const std::streamoff fileSize = input.tellg();
            if (fileSize <= 0)
            {
                return nullptr;
            }
            input.seekg(0, std::ios::beg);
            std::vector<std::uint8_t> bytes(
                static_cast<std::size_t>(fileSize));
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                fileSize);
            if (!input)
            {
                return nullptr;
            }
            input.close();

            Reader reader{ bytes.data(), bytes.size() };
            char magic[4]{};
            if (!reader.ReadRaw(magic, sizeof(magic))
                || std::memcmp(magic, Magic, sizeof(Magic)) != 0)
            {
                return nullptr;
            }
            if (reader.Read<std::uint32_t>() != FormatVersion)
            {
                return nullptr;
            }
            if (reader.Read<std::uint32_t>()
                != static_cast<std::uint32_t>(
                    sizeof(ModelVertex)))
            {
                return nullptr;
            }

            // 依存ファイルの照合。1つでも変わっていたら丸ごと
            // 作り直しへ落とします。
            const auto dependencyCount =
                reader.Read<std::uint32_t>();
            if (reader.failed
                || dependencyCount > MaximumCount)
            {
                return nullptr;
            }
            for (std::uint32_t index = 0;
                index < dependencyCount;
                ++index)
            {
                const auto dependencyPath =
                    LamaPon::PathFromUtf8(reader.ReadString());
                const bool expectedExists =
                    reader.Read<std::uint8_t>() != 0;
                const auto expectedHash =
                    reader.Read<std::uint64_t>();
                if (reader.failed)
                {
                    return nullptr;
                }
                const bool exists =
                    assets.FileExists(dependencyPath);
                if (exists != expectedExists)
                {
                    return nullptr;
                }
                if (exists)
                {
                    const auto dependencyBytes =
                        assets.ReadFileBytes(dependencyPath);
                    if (HashBytes(dependencyBytes)
                        != expectedHash)
                    {
                        return nullptr;
                    }
                }
            }

            // 外部参照画像を再読み込みして内容を照合します
            // （照合に読むバイト列がそのままSRVの材料になるので、
            // 二度読みにはなりません）。
            const auto imageCount = reader.Read<std::uint32_t>();
            if (reader.failed || imageCount > MaximumCount)
            {
                return nullptr;
            }
            std::vector<
                Microsoft::WRL::ComPtr<
                    ID3D11ShaderResourceView>> views(imageCount);
            std::vector<std::uint8_t> imageTransparency(
                imageCount);
            for (std::uint32_t index = 0;
                index < imageCount;
                ++index)
            {
                const bool external =
                    reader.Read<std::uint8_t>() != 0;
                const bool isDds =
                    reader.Read<std::uint8_t>() != 0;
                imageTransparency[index] =
                    reader.Read<std::uint8_t>();
                const auto rawUsage =
                    reader.Read<std::uint8_t>();
                // 未知の用途は色テクスチャとして扱い、安全な圧縮形式へフォールバックします。
                const auto usage = rawUsage
                        <= static_cast<std::uint8_t>(
                            TextureLoader::TextureUsage::DataMap)
                    ? static_cast<TextureLoader::TextureUsage>(
                        rawUsage)
                    : TextureLoader::TextureUsage::Color;
                if (external)
                {
                    const auto imagePath =
                        LamaPon::PathFromUtf8(
                            reader.ReadString());
                    const auto expectedHash =
                        reader.Read<std::uint64_t>();
                    if (reader.failed
                        || !assets.FileExists(imagePath))
                    {
                        return nullptr;
                    }
                    const auto imageBytes =
                        assets.ReadFileBytes(imagePath);
                    if (HashBytes(imageBytes) != expectedHash)
                    {
                        return nullptr;
                    }
                    views[index] = CreateImageView(
                        assets,
                        imageBytes,
                        isDds,
                        usage);
                }
                else
                {
                    const auto byteCount =
                        reader.Read<std::uint64_t>();
                    // byteCountはファイル由来なので、先に単体で
                    // 検査します（offsetとの和は桁あふれしうる）。
                    if (reader.failed
                        || byteCount > reader.size
                        || reader.offset + byteCount
                            > reader.size)
                    {
                        return nullptr;
                    }
                    views[index] = CreateImageView(
                        assets,
                        std::span<const std::uint8_t>(
                            reader.data + reader.offset,
                            static_cast<std::size_t>(
                                byteCount)),
                        isDds,
                        usage);
                    reader.offset += static_cast<std::size_t>(
                        byteCount);
                }
            }

            auto model = std::make_shared<SkeletalModel>();
            model->hasLocalBounds =
                reader.Read<std::uint8_t>() != 0;
            if (model->hasLocalBounds
                && !reader.ReadRaw(
                    &model->localBounds,
                    sizeof(model->localBounds)))
            {
                return nullptr;
            }

            const auto nodeCount = reader.Read<std::uint32_t>();
            // ノード0個のモデルは存在しません（インポーターが先に
            // 投げます）。0が読めたらファイルが壊れています。
            if (reader.failed
                || nodeCount == 0
                || nodeCount > MaximumCount)
            {
                return nullptr;
            }
            model->nodes.resize(nodeCount);
            for (auto& node : model->nodes)
            {
                node.name = reader.ReadString();
                node.parent = static_cast<std::ptrdiff_t>(
                    reader.Read<std::int64_t>());
                if (!reader.ReadRaw(
                    &node.bindPose,
                    sizeof(node.bindPose)))
                {
                    return nullptr;
                }
            }

            const auto skinCount = reader.Read<std::uint32_t>();
            if (reader.failed || skinCount > MaximumCount)
            {
                return nullptr;
            }
            model->skins.resize(skinCount);
            for (auto& skin : model->skins)
            {
                skin.name = reader.ReadString();
                const auto jointCount =
                    reader.Read<std::uint32_t>();
                if (reader.failed || jointCount > MaximumCount)
                {
                    return nullptr;
                }
                skin.joints.resize(jointCount);
                skin.inverseBindMatrices.resize(jointCount);
                for (auto& joint : skin.joints)
                {
                    joint = static_cast<std::size_t>(
                        reader.Read<std::uint64_t>());
                    // 範囲外の参照は姿勢計算で落ちるので弾きます。
                    if (joint >= model->nodes.size())
                    {
                        return nullptr;
                    }
                }
                if (!reader.ReadRaw(
                    skin.inverseBindMatrices.data(),
                    jointCount
                        * sizeof(DirectX::XMFLOAT4X4)))
                {
                    return nullptr;
                }
            }

            const auto animationCount =
                reader.Read<std::uint32_t>();
            if (reader.failed || animationCount > MaximumCount)
            {
                return nullptr;
            }
            model->animations.resize(animationCount);
            for (auto& clip : model->animations)
            {
                clip.name = reader.ReadString();
                clip.duration = reader.Read<float>();
                const auto trackCount =
                    reader.Read<std::uint32_t>();
                if (reader.failed || trackCount > MaximumCount)
                {
                    return nullptr;
                }
                clip.tracks.resize(trackCount);
                for (auto& track : clip.tracks)
                {
                    track.node = static_cast<std::size_t>(
                        reader.Read<std::uint64_t>());
                    if (track.node >= model->nodes.size())
                    {
                        return nullptr;
                    }
                    if (!ReadVectorChannel(
                            reader,
                            track.translation)
                        || !ReadQuaternionChannel(
                            reader,
                            track.rotation)
                        || !ReadVectorChannel(
                            reader,
                            track.scale))
                    {
                        return nullptr;
                    }
                }
            }

            const auto primitiveCount =
                reader.Read<std::uint32_t>();
            if (reader.failed
                || primitiveCount == 0
                || primitiveCount > MaximumCount)
            {
                return nullptr;
            }
            model->primitives.resize(primitiveCount);
            for (auto& primitive : model->primitives)
            {
                primitive.meshNode = static_cast<std::size_t>(
                    reader.Read<std::uint64_t>());
                primitive.skin = static_cast<std::ptrdiff_t>(
                    reader.Read<std::int64_t>());
                primitive.indexCount =
                    reader.Read<std::uint32_t>();
                primitive.hasLocalBounds =
                    reader.Read<std::uint8_t>() != 0;
                if (primitive.hasLocalBounds
                    && !reader.ReadRaw(
                        &primitive.localBounds,
                        sizeof(primitive.localBounds)))
                {
                    return nullptr;
                }
                // 描画時の範囲外参照を防ぐため、読み込み時に拒否します。
                if (primitive.meshNode >= model->nodes.size()
                    || primitive.skin
                        >= static_cast<std::ptrdiff_t>(
                            model->skins.size())
                    || primitive.skin < -1)
                {
                    return nullptr;
                }
                if (!reader.ReadRaw(
                        &primitive.baseColor,
                        sizeof(primitive.baseColor)))
                {
                    return nullptr;
                }
                primitive.roughness = reader.Read<float>();
                primitive.metallic = reader.Read<float>();
                primitive.occlusionStrength =
                    reader.Read<float>();
                if (!reader.ReadRaw(
                        &primitive.emissiveFactor,
                        sizeof(primitive.emissiveFactor)))
                {
                    return nullptr;
                }
                primitive.alpha =
                    reader.Read<std::uint8_t>() != 0;
                primitive.textureHasTransparency =
                    reader.Read<std::uint8_t>() != 0;
                primitive.doubleSided =
                    reader.Read<std::uint8_t>() != 0;
                const bool hasCutout =
                    reader.Read<std::uint8_t>() != 0;

                std::int32_t slots[6]{};
                if (!reader.ReadRaw(slots, sizeof(slots)))
                {
                    return nullptr;
                }
                const auto viewAt =
                    [&views](const std::int32_t slot)
                    -> Microsoft::WRL::ComPtr<
                        ID3D11ShaderResourceView>
                {
                    if (slot < 0
                        || static_cast<std::size_t>(slot)
                            >= views.size())
                    {
                        return {};
                    }
                    return views[
                        static_cast<std::size_t>(slot)];
                };
                primitive.texture = viewAt(slots[0]);
                primitive.normalTexture = viewAt(slots[1]);
                primitive.roughnessTexture = viewAt(slots[2]);
                primitive.metallicTexture = viewAt(slots[3]);
                primitive.occlusionTexture = viewAt(slots[4]);
                primitive.emissiveTexture = viewAt(slots[5]);

                const auto vertexCount =
                    reader.Read<std::uint64_t>();
                // 掛け算の前に単体で検査します（桁あふれよけ）。
                if (reader.failed
                    || vertexCount == 0
                    || vertexCount
                        > reader.size / sizeof(ModelVertex))
                {
                    return nullptr;
                }
                const std::size_t vertexBytes =
                    static_cast<std::size_t>(vertexCount)
                    * sizeof(ModelVertex);
                if (reader.offset + vertexBytes > reader.size)
                {
                    return nullptr;
                }
                std::vector<ModelVertex> cpuVertices(
                    static_cast<std::size_t>(vertexCount));
                std::memcpy(
                    cpuVertices.data(),
                    reader.data + reader.offset,
                    vertexBytes);
                CreateBuffer(
                    device,
                    assets,
                    cpuVertices.data(),
                    vertexBytes,
                    D3D11_BIND_VERTEX_BUFFER,
                    primitive.vertexBuffer
                        .ReleaseAndGetAddressOf());
                reader.offset += vertexBytes;

                const auto storedIndexCount =
                    reader.Read<std::uint64_t>();
                if (reader.failed)
                {
                    return nullptr;
                }
                std::vector<std::uint32_t> cpuIndices;
                if (storedIndexCount == 0)
                {
                    // 添字を省略した記録は0..N-1の連番として
                    // 復元します。
                    cpuIndices.resize(
                        primitive.indexCount);
                    for (std::uint32_t index = 0;
                        index < primitive.indexCount;
                        ++index)
                    {
                        cpuIndices[index] = index;
                    }
                    CreateBuffer(
                        device,
                        assets,
                        cpuIndices.data(),
                        cpuIndices.size()
                            * sizeof(std::uint32_t),
                        D3D11_BIND_INDEX_BUFFER,
                        primitive.indexBuffer
                            .ReleaseAndGetAddressOf());
                }
                else
                {
                    if (storedIndexCount
                        > reader.size
                            / sizeof(std::uint32_t))
                    {
                        return nullptr;
                    }
                    const std::size_t indexBytes =
                        static_cast<std::size_t>(
                            storedIndexCount)
                        * sizeof(std::uint32_t);
                    if (reader.offset + indexBytes
                        > reader.size)
                    {
                        return nullptr;
                    }
                    cpuIndices.resize(
                        static_cast<std::size_t>(
                            storedIndexCount));
                    std::memcpy(
                        cpuIndices.data(),
                        reader.data + reader.offset,
                        indexBytes);
                    CreateBuffer(
                        device,
                        assets,
                        cpuIndices.data(),
                        indexBytes,
                        D3D11_BIND_INDEX_BUFFER,
                        primitive.indexBuffer
                            .ReleaseAndGetAddressOf());
                    reader.offset += indexBytes;
                }

                if (primitive.skin < 0
                    && primitive.hasLocalBounds)
                {
                    const auto lodLevels =
                        ModelLod::BuildLevels<ModelVertex>(
                            cpuVertices,
                            cpuIndices,
                            primitive.localBounds);
                    for (std::size_t level = 0;
                        level < lodLevels.size();
                        ++level)
                    {
                        if (lodLevels[level].empty())
                        {
                            continue;
                        }
                        CreateBuffer(
                            device,
                            assets,
                            lodLevels[level].data(),
                            lodLevels[level].size()
                                * sizeof(std::uint32_t),
                            D3D11_BIND_INDEX_BUFFER,
                            primitive.lodIndexBuffers[level]
                                .ReleaseAndGetAddressOf());
                        primitive.lodIndexCounts[level] =
                            static_cast<std::uint32_t>(
                                lodLevels[level].size());
                    }
                }

                BuildPrimitiveEffect(device, primitive);
                if (hasCutout)
                {
                    BuildCutoutEffect(device, primitive);
                }
            }

            // 末尾にゴミが付いているファイルも信用しません。
            if (reader.failed || reader.offset != reader.size)
            {
                return nullptr;
            }
            return model;
        }
        catch (...)
        {
            // 読み取りまたは復元に失敗した場合はキャッシュ不在として扱い、
            // 呼び出し側で再インポートします。
            return nullptr;
        }
    }

    void Store(
        const std::uint64_t key,
        const SkeletalModel& model,
        const Recorder& recorder) noexcept
    {
        try
        {
            // 幾何とプリミティブの登録数が一致しない場合は、
            // 不完全なキャッシュを防ぐため保存しません。
            if (model.primitives.empty()
                || recorder.geometries.size()
                    != model.primitives.size())
            {
                return;
            }
            for (const auto& geometry : recorder.geometries)
            {
                if (geometry.vertexStride
                    != sizeof(ModelVertex))
                {
                    return;
                }
            }

            const auto path = EntryPath(key);
            if (path.empty())
            {
                return;
            }
            std::error_code error;
            std::filesystem::create_directories(
                path.parent_path(),
                error);
            if (error)
            {
                return;
            }

            std::size_t estimate = 4096;
            for (const auto& geometry : recorder.geometries)
            {
                estimate += geometry.vertexBytes.size()
                    + geometry.indices.size() * 4 + 64;
            }
            for (const auto& image : recorder.images)
            {
                estimate += image.bytes.size() + 64;
            }
            std::vector<std::uint8_t> output;
            output.reserve(estimate);

            AppendBytes(output, Magic, sizeof(Magic));
            AppendU32(output, FormatVersion);
            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    sizeof(ModelVertex)));

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    recorder.dependencies.size()));
            for (const auto& dependency : recorder.dependencies)
            {
                AppendString(
                    output,
                    LamaPon::PathToUtf8(dependency.path));
                AppendU8(output, dependency.exists ? 1 : 0);
                AppendU64(output, dependency.hash);
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    recorder.images.size()));
            for (const auto& image : recorder.images)
            {
                AppendU8(output, image.external ? 1 : 0);
                AppendU8(output, image.isDds ? 1 : 0);
                AppendU8(
                    output,
                    image.hasTransparency ? 1 : 0);
                AppendU8(
                    output,
                    static_cast<std::uint8_t>(image.usage));
                if (image.external)
                {
                    AppendString(
                        output,
                        LamaPon::PathToUtf8(
                            image.externalPath));
                    AppendU64(output, image.externalHash);
                }
                else
                {
                    AppendU64(output, image.bytes.size());
                    AppendBytes(
                        output,
                        image.bytes.data(),
                        image.bytes.size());
                }
            }

            AppendU8(output, model.hasLocalBounds ? 1 : 0);
            if (model.hasLocalBounds)
            {
                AppendBytes(
                    output,
                    &model.localBounds,
                    sizeof(model.localBounds));
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    model.nodes.size()));
            for (const auto& node : model.nodes)
            {
                AppendString(output, node.name);
                AppendI64(
                    output,
                    static_cast<std::int64_t>(node.parent));
                AppendBytes(
                    output,
                    &node.bindPose,
                    sizeof(node.bindPose));
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    model.skins.size()));
            for (const auto& skin : model.skins)
            {
                AppendString(output, skin.name);
                AppendU32(
                    output,
                    static_cast<std::uint32_t>(
                        skin.joints.size()));
                for (const auto joint : skin.joints)
                {
                    AppendU64(
                        output,
                        static_cast<std::uint64_t>(joint));
                }
                AppendBytes(
                    output,
                    skin.inverseBindMatrices.data(),
                    skin.inverseBindMatrices.size()
                        * sizeof(DirectX::XMFLOAT4X4));
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    model.animations.size()));
            for (const auto& clip : model.animations)
            {
                AppendString(output, clip.name);
                AppendF32(output, clip.duration);
                AppendU32(
                    output,
                    static_cast<std::uint32_t>(
                        clip.tracks.size()));
                for (const auto& track : clip.tracks)
                {
                    AppendU64(
                        output,
                        static_cast<std::uint64_t>(
                            track.node));
                    AppendVectorChannel(
                        output,
                        track.translation);
                    AppendQuaternionChannel(
                        output,
                        track.rotation);
                    AppendVectorChannel(output, track.scale);
                }
            }

            AppendU32(
                output,
                static_cast<std::uint32_t>(
                    model.primitives.size()));
            for (std::size_t index = 0;
                index < model.primitives.size();
                ++index)
            {
                const auto& primitive =
                    model.primitives[index];
                const auto& geometry =
                    recorder.geometries[index];
                AppendU64(
                    output,
                    static_cast<std::uint64_t>(
                        primitive.meshNode));
                AppendI64(
                    output,
                    static_cast<std::int64_t>(
                        primitive.skin));
                AppendU32(output, primitive.indexCount);
                AppendU8(
                    output,
                    primitive.hasLocalBounds ? 1 : 0);
                if (primitive.hasLocalBounds)
                {
                    AppendBytes(
                        output,
                        &primitive.localBounds,
                        sizeof(primitive.localBounds));
                }
                AppendBytes(
                    output,
                    &primitive.baseColor,
                    sizeof(primitive.baseColor));
                AppendF32(output, primitive.roughness);
                AppendF32(output, primitive.metallic);
                AppendF32(output, primitive.occlusionStrength);
                AppendBytes(
                    output,
                    &primitive.emissiveFactor,
                    sizeof(primitive.emissiveFactor));
                AppendU8(output, primitive.alpha ? 1 : 0);
                AppendU8(
                    output,
                    primitive.textureHasTransparency
                        ? 1
                        : 0);
                AppendU8(
                    output,
                    primitive.doubleSided ? 1 : 0);
                AppendU8(
                    output,
                    primitive.cutoutEffect != nullptr
                        ? 1
                        : 0);

                const std::int32_t slots[6] = {
                    recorder.ImageIndexFor(
                        primitive.texture.Get()),
                    recorder.ImageIndexFor(
                        primitive.normalTexture.Get()),
                    recorder.ImageIndexFor(
                        primitive.roughnessTexture.Get()),
                    recorder.ImageIndexFor(
                        primitive.metallicTexture.Get()),
                    recorder.ImageIndexFor(
                        primitive.occlusionTexture.Get()),
                    recorder.ImageIndexFor(
                        primitive.emissiveTexture.Get())
                };
                AppendBytes(output, slots, sizeof(slots));

                AppendU64(
                    output,
                    geometry.vertexBytes.size()
                        / sizeof(ModelVertex));
                AppendBytes(
                    output,
                    geometry.vertexBytes.data(),
                    geometry.vertexBytes.size());
                AppendU64(output, geometry.indices.size());
                AppendBytes(
                    output,
                    geometry.indices.data(),
                    geometry.indices.size()
                        * sizeof(std::uint32_t));
            }

            WriteFileAtomically(path, output);
        }
        catch (...)
        {
            // 保存できなくても読み込み自体は成功しているので
            // 何もしません。
        }
    }
}
