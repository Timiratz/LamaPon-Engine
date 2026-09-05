#include "LamaPon/Assets/GltfImporter.h"

#include "LamaPon/Assets/ModelCache.h"
#include "LamaPon/Assets/ModelLod.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <DDSTextureLoader.h>
#include <Effects.h>
#include <VertexTypes.h>
#include <WICTextureLoader.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using Vertex =
        DirectX::VertexPositionNormalTangentColorTextureSkinning;

    void ExpandModelBounds(
        LamaPon::SkeletalModel& model,
        const std::span<const Vertex> vertices,
        DirectX::FXMMATRIX transform) noexcept
    {
        for (const auto& vertex : vertices)
        {
            DirectX::XMFLOAT3 position{};
            DirectX::XMStoreFloat3(
                &position,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&vertex.position),
                    transform));
            if (!model.hasLocalBounds)
            {
                model.localBounds = { position, position };
                model.hasLocalBounds = true;
                continue;
            }
            model.localBounds.minimum.x = std::min(
                model.localBounds.minimum.x, position.x);
            model.localBounds.minimum.y = std::min(
                model.localBounds.minimum.y, position.y);
            model.localBounds.minimum.z = std::min(
                model.localBounds.minimum.z, position.z);
            model.localBounds.maximum.x = std::max(
                model.localBounds.maximum.x, position.x);
            model.localBounds.maximum.y = std::max(
                model.localBounds.maximum.y, position.y);
            model.localBounds.maximum.z = std::max(
                model.localBounds.maximum.z, position.z);
        }
    }

    bool CalculateLocalBounds(
        const std::span<const Vertex> vertices,
        LamaPon::Bounds3D& bounds) noexcept
    {
        bool initialized{};
        for (const auto& vertex : vertices)
        {
            const auto& position = vertex.position;
            if (!initialized)
            {
                bounds = { position, position };
                initialized = true;
                continue;
            }
            bounds.minimum.x = std::min(
                bounds.minimum.x, position.x);
            bounds.minimum.y = std::min(
                bounds.minimum.y, position.y);
            bounds.minimum.z = std::min(
                bounds.minimum.z, position.z);
            bounds.maximum.x = std::max(
                bounds.maximum.x, position.x);
            bounds.maximum.y = std::max(
                bounds.maximum.y, position.y);
            bounds.maximum.z = std::max(
                bounds.maximum.z, position.z);
        }
        return initialized;
    }

    void ThrowIfFailed(
        const HRESULT result,
        const std::string& operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                operation
                + " failed with HRESULT "
                + std::to_string(
                    static_cast<unsigned long>(result)));
        }
    }

    std::string ResultName(const cgltf_result result)
    {
        switch (result)
        {
        case cgltf_result_data_too_short:
            return "data_too_short";
        case cgltf_result_unknown_format:
            return "unknown_format";
        case cgltf_result_invalid_json:
            return "invalid_json";
        case cgltf_result_invalid_gltf:
            return "invalid_gltf";
        case cgltf_result_invalid_options:
            return "invalid_options";
        case cgltf_result_file_not_found:
            return "file_not_found";
        case cgltf_result_io_error:
            return "io_error";
        case cgltf_result_out_of_memory:
            return "out_of_memory";
        case cgltf_result_legacy_gltf:
            return "legacy_gltf";
        default:
            return "unknown";
        }
    }

    const cgltf_accessor* FindAttribute(
        const cgltf_primitive& primitive,
        const cgltf_attribute_type type,
        const cgltf_int index = 0)
    {
        const auto found = std::ranges::find_if(
            std::span{
                primitive.attributes,
                primitive.attributes_count
            },
            [type, index](const cgltf_attribute& attribute)
            {
                return attribute.type == type
                    && attribute.index == index;
            });
        return found == std::span{
            primitive.attributes,
            primitive.attributes_count
        }.end()
            ? nullptr
            : found->data;
    }

    DirectX::XMFLOAT3 ReadFloat3(
        const cgltf_accessor& accessor,
        const cgltf_size index)
    {
        std::array<cgltf_float, 3> value{};
        if (!cgltf_accessor_read_float(
                &accessor,
                index,
                value.data(),
                value.size()))
        {
            throw std::runtime_error(
                "Failed to read a glTF vec3 accessor.");
        }
        return { value[0], value[1], value[2] };
    }

    DirectX::XMFLOAT4 ReadFloat4(
        const cgltf_accessor& accessor,
        const cgltf_size index)
    {
        std::array<cgltf_float, 4> value{};
        if (!cgltf_accessor_read_float(
                &accessor,
                index,
                value.data(),
                value.size()))
        {
            throw std::runtime_error(
                "Failed to read a glTF vec4 accessor.");
        }
        return { value[0], value[1], value[2], value[3] };
    }

    DirectX::XMFLOAT2 ReadFloat2(
        const cgltf_accessor& accessor,
        const cgltf_size index)
    {
        std::array<cgltf_float, 2> value{};
        if (!cgltf_accessor_read_float(
                &accessor,
                index,
                value.data(),
                value.size()))
        {
            throw std::runtime_error(
                "Failed to read a glTF vec2 accessor.");
        }
        return { value[0], value[1] };
    }

    LamaPon::SkeletalInterpolation ConvertInterpolation(
        const cgltf_interpolation_type interpolation)
    {
        if (interpolation == cgltf_interpolation_type_step)
        {
            return LamaPon::SkeletalInterpolation::Step;
        }
        if (interpolation
            == cgltf_interpolation_type_cubic_spline)
        {
            return LamaPon::SkeletalInterpolation::CubicSpline;
        }
        return LamaPon::SkeletalInterpolation::Linear;
    }

    std::vector<std::uint8_t> DecodeBase64(
        const std::string_view encoded)
    {
        static constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789+/";
        std::vector<std::uint8_t> result;
        result.reserve(encoded.size() * 3 / 4);
        unsigned value{};
        int bits{};
        for (const unsigned char character : encoded)
        {
            if (character == '=')
            {
                break;
            }
            if (std::isspace(character) != 0)
            {
                continue;
            }
            const auto position = alphabet.find(
                static_cast<char>(character));
            if (position == std::string_view::npos)
            {
                throw std::runtime_error(
                    "Invalid base64 image in glTF.");
            }
            value = (value << 6)
                | static_cast<unsigned>(position);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                result.push_back(
                    static_cast<std::uint8_t>(
                        (value >> bits) & 0xffu));
            }
        }
        return result;
    }

    struct CgltfReadContext final
    {
        LamaPon::AssetManager* assets{};
    };

    // cgltfによるファイル読み込み（本体の.gltf/.glbと、外部参照された
    // .binバッファー）をAssetManagerへ通します。これにより、ゲームが
    // アセットアーカイブを同梱している場合も透過的に復号できます。
    cgltf_result CgltfFileRead(
        const cgltf_memory_options*,
        const cgltf_file_options* fileOptions,
        const char* path,
        cgltf_size* size,
        void** data)
    {
        auto* context =
            static_cast<CgltfReadContext*>(fileOptions->user_data);
        try
        {
            auto bytes = context->assets->ReadFileBytes(
                LamaPon::PathFromUtf8(path));
            void* memory = std::malloc(
                bytes.empty() ? 1 : bytes.size());
            if (memory == nullptr)
            {
                return cgltf_result_out_of_memory;
            }
            if (!bytes.empty())
            {
                std::memcpy(memory, bytes.data(), bytes.size());
            }
            if (size != nullptr)
            {
                *size = bytes.size();
            }
            *data = memory;
            return cgltf_result_success;
        }
        catch (const std::exception&)
        {
            return cgltf_result_file_not_found;
        }
    }

    void CgltfFileRelease(
        const cgltf_memory_options*,
        const cgltf_file_options*,
        void* data,
        cgltf_size)
    {
        std::free(data);
    }

    bool IsDdsImage(const cgltf_image& image)
    {
        if (image.mime_type != nullptr
            && std::string_view(image.mime_type)
                == "image/vnd-ms.dds")
        {
            return true;
        }
        if (image.uri == nullptr)
        {
            return false;
        }
        std::string uri = image.uri;
        std::ranges::transform(
            uri,
            uri.begin(),
            [](const unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            });
        return uri.ends_with(".dds");
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        LoadImage(
            LamaPon::AssetManager& assets,
            const std::filesystem::path& modelPath,
            const cgltf_image& image,
            const LamaPon::TextureLoader::TextureUsage usage,
            LamaPon::ModelCache::Recorder& recorder)
    {
        const std::uint8_t* bytes{};
        std::size_t byteCount{};
        std::vector<std::uint8_t> ownedBytes;

        if (image.buffer_view != nullptr)
        {
            const auto& view = *image.buffer_view;
            if (view.data != nullptr)
            {
                bytes = static_cast<const std::uint8_t*>(
                    view.data);
            }
            else if (view.buffer != nullptr
                && view.buffer->data != nullptr)
            {
                bytes = static_cast<const std::uint8_t*>(
                    view.buffer->data) + view.offset;
            }
            byteCount = view.size;
        }
        else if (image.uri != nullptr)
        {
            const std::string_view uri(image.uri);
            if (uri.starts_with("data:"))
            {
                const auto comma = uri.find(',');
                if (comma == std::string_view::npos
                    || uri.substr(0, comma).find(";base64")
                        == std::string_view::npos)
                {
                    throw std::runtime_error(
                        "Only base64 data URI images are supported.");
                }
                ownedBytes = DecodeBase64(uri.substr(comma + 1));
                bytes = ownedBytes.data();
                byteCount = ownedBytes.size();
            }
            else
            {
                std::string decodedUri(uri);
                decodedUri.resize(
                    cgltf_decode_uri(decodedUri.data()));
                const auto imagePath =
                    modelPath.parent_path()
                    / LamaPon::PathFromUtf8(decodedUri);
                const auto imageBytes =
                    assets.ReadFileBytes(imagePath);
                auto texture =
                    assets.CreateTextureViewFromMemory(
                        imageBytes,
                        IsDdsImage(image),
                        usage);
                // モデルキャッシュには、外部ファイルのパスと内容ハッシュを
                // 記録します。読み込み時にファイルを再読込して検証します。
                recorder.RegisterExternalImage(
                    texture.Get(),
                    imagePath,
                    imageBytes,
                    IsDdsImage(image),
                    false,
                    usage);
                return texture;
            }
        }

        if (bytes == nullptr || byteCount == 0)
        {
            return {};
        }

        const std::span<const std::uint8_t> imageBytes(
            bytes,
            byteCount);
        auto texture = assets.CreateTextureViewFromMemory(
            imageBytes,
            IsDdsImage(image),
            usage);
        // 埋め込み（GLBのbufferViewやbase64）は元ファイルからしか
        // 取り出せないので、バイト列ごと控えます。
        recorder.RegisterEmbeddedImage(
            texture.Get(),
            imageBytes,
            IsDdsImage(image),
            false,
            usage);
        return texture;
    }

    void CreateBuffer(
        ID3D11Device* device,
        LamaPon::AssetManager& assets,
        const void* data,
        const std::size_t byteCount,
        const UINT bindFlags,
        ID3D11Buffer** output)
    {
        if (byteCount > std::numeric_limits<UINT>::max())
        {
            throw std::runtime_error(
                "glTF mesh buffer is too large for Direct3D 11.");
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
            "Creating glTF mesh buffer");
    }

    LamaPon::SkeletalPoseTransform ReadNodePose(
        const cgltf_node& node)
    {
        using namespace DirectX;
        LamaPon::SkeletalPoseTransform result;
        if (node.has_matrix)
        {
            XMFLOAT4X4 stored{};
            std::memcpy(
                &stored,
                node.matrix,
                sizeof(stored));
            XMVECTOR scale{};
            XMVECTOR rotation{};
            XMVECTOR translation{};
            if (!XMMatrixDecompose(
                    &scale,
                    &rotation,
                    &translation,
                    XMLoadFloat4x4(&stored)))
            {
                throw std::runtime_error(
                    "Unable to decompose a glTF node matrix.");
            }
            XMStoreFloat3(&result.scale, scale);
            XMStoreFloat4(
                &result.rotation,
                XMQuaternionNormalize(rotation));
            XMStoreFloat3(&result.translation, translation);
            return result;
        }

        if (node.has_translation)
        {
            result.translation = {
                node.translation[0],
                node.translation[1],
                node.translation[2]
            };
        }
        if (node.has_rotation)
        {
            result.rotation = {
                node.rotation[0],
                node.rotation[1],
                node.rotation[2],
                node.rotation[3]
            };
        }
        if (node.has_scale)
        {
            result.scale = {
                node.scale[0],
                node.scale[1],
                node.scale[2]
            };
        }
        return result;
    }

    void GenerateNormals(
        std::vector<Vertex>& vertices,
        const std::vector<std::uint32_t>& indices)
    {
        using namespace DirectX;
        std::vector<XMVECTOR> accumulated(
            vertices.size(),
            XMVectorZero());
        for (std::size_t index = 0;
            index + 2 < indices.size();
            index += 3)
        {
            const auto a = indices[index];
            const auto b = indices[index + 1];
            const auto c = indices[index + 2];
            if (a >= vertices.size()
                || b >= vertices.size()
                || c >= vertices.size())
            {
                continue;
            }
            const XMVECTOR pa =
                XMLoadFloat3(&vertices[a].position);
            const XMVECTOR pb =
                XMLoadFloat3(&vertices[b].position);
            const XMVECTOR pc =
                XMLoadFloat3(&vertices[c].position);
            const XMVECTOR normal = XMVector3Cross(
                XMVectorSubtract(pb, pa),
                XMVectorSubtract(pc, pa));
            accumulated[a] += normal;
            accumulated[b] += normal;
            accumulated[c] += normal;
        }
        for (std::size_t index = 0;
            index < vertices.size();
            ++index)
        {
            XMVECTOR normal = accumulated[index];
            if (XMVectorGetX(XMVector3LengthSq(normal))
                < 0.000001f)
            {
                normal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            }
            XMStoreFloat3(
                &vertices[index].normal,
                XMVector3Normalize(normal));
        }
    }

    LamaPon::SkeletalNodeTrack& FindOrCreateTrack(
        LamaPon::SkeletalAnimationClip& clip,
        const std::size_t node)
    {
        const auto found = std::ranges::find(
            clip.tracks,
            node,
            &LamaPon::SkeletalNodeTrack::node);
        if (found != clip.tracks.end())
        {
            return *found;
        }
        clip.tracks.push_back({});
        clip.tracks.back().node = node;
        return clip.tracks.back();
    }

    void ImportVectorChannel(
        LamaPon::SkeletalVectorChannel& destination,
        const cgltf_animation_sampler& sampler)
    {
        if (sampler.input == nullptr || sampler.output == nullptr)
        {
            return;
        }
        destination.interpolation =
            ConvertInterpolation(sampler.interpolation);
        destination.keys.resize(sampler.input->count);
        const bool cubic = sampler.interpolation
            == cgltf_interpolation_type_cubic_spline;
        for (cgltf_size index = 0;
            index < sampler.input->count;
            ++index)
        {
            cgltf_float time{};
            if (!cgltf_accessor_read_float(
                    sampler.input,
                    index,
                    &time,
                    1))
            {
                throw std::runtime_error(
                    "Failed to read glTF animation time.");
            }
            auto& key = destination.keys[index];
            key.time = time;
            const cgltf_size valueIndex =
                cubic ? index * 3 + 1 : index;
            key.value = ReadFloat3(
                *sampler.output,
                valueIndex);
            if (cubic)
            {
                key.inTangent = ReadFloat3(
                    *sampler.output,
                    index * 3);
                key.outTangent = ReadFloat3(
                    *sampler.output,
                    index * 3 + 2);
            }
        }
    }

    void ImportQuaternionChannel(
        LamaPon::SkeletalQuaternionChannel& destination,
        const cgltf_animation_sampler& sampler)
    {
        if (sampler.input == nullptr || sampler.output == nullptr)
        {
            return;
        }
        destination.interpolation =
            ConvertInterpolation(sampler.interpolation);
        destination.keys.resize(sampler.input->count);
        const bool cubic = sampler.interpolation
            == cgltf_interpolation_type_cubic_spline;
        for (cgltf_size index = 0;
            index < sampler.input->count;
            ++index)
        {
            cgltf_float time{};
            if (!cgltf_accessor_read_float(
                    sampler.input,
                    index,
                    &time,
                    1))
            {
                throw std::runtime_error(
                    "Failed to read glTF animation time.");
            }
            auto& key = destination.keys[index];
            key.time = time;
            const cgltf_size valueIndex =
                cubic ? index * 3 + 1 : index;
            key.value = ReadFloat4(
                *sampler.output,
                valueIndex);
            if (cubic)
            {
                key.inTangent = ReadFloat4(
                    *sampler.output,
                    index * 3);
                key.outTangent = ReadFloat4(
                    *sampler.output,
                    index * 3 + 2);
            }
        }
    }
}

namespace LamaPon
{
    std::shared_ptr<SkeletalModel> GltfImporter::Load(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& path)
    {
        if (device == nullptr)
        {
            throw std::invalid_argument(
                "GltfImporter requires a Direct3D 11 device.");
        }

        // インポートキャッシュ。本体・外部の.bin・外部テクスチャが
        // 前回と同じなら、パースと組み立てを全部飛ばします。
        const auto sourceBytes = assets.ReadFileBytes(path);
        const std::uint64_t cacheKey =
            ModelCache::ComputeKey(sourceBytes, 2);
        if (auto cached = ModelCache::TryLoad(
                device,
                context,
                assets,
                cacheKey))
        {
            return cached;
        }
        ModelCache::Recorder recorder;

        const std::string utf8Path = PathToUtf8(path);
        CgltfReadContext readContext{ &assets };
        cgltf_options options{};
        options.file.read = &CgltfFileRead;
        options.file.release = &CgltfFileRelease;
        options.file.user_data = &readContext;
        cgltf_data* rawData{};
        const cgltf_result parseResult =
            cgltf_parse_file(
                &options,
                utf8Path.c_str(),
                &rawData);
        if (parseResult != cgltf_result_success)
        {
            throw std::runtime_error(
                "Failed to parse glTF: "
                + ResultName(parseResult));
        }
        const std::unique_ptr<cgltf_data, decltype(&cgltf_free)>
            data(rawData, &cgltf_free);

        const cgltf_result bufferResult =
            cgltf_load_buffers(
                &options,
                data.get(),
                utf8Path.c_str());
        if (bufferResult != cgltf_result_success)
        {
            throw std::runtime_error(
                "Failed to load glTF buffers: "
                + ResultName(bufferResult));
        }
        // 外部の.binバッファを依存として記録します（変わったら
        // キャッシュを作り直すため）。GLBの内蔵バッファ（uri無し）と
        // data: URIは本体のバイト列に含まれるので鍵が既に守っています。
        for (cgltf_size bufferIndex = 0;
            bufferIndex < data->buffers_count;
            ++bufferIndex)
        {
            const auto& buffer = data->buffers[bufferIndex];
            if (buffer.uri == nullptr
                || std::string_view(buffer.uri)
                    .starts_with("data:")
                || buffer.data == nullptr)
            {
                continue;
            }
            std::string decodedUri(buffer.uri);
            decodedUri.resize(
                cgltf_decode_uri(decodedUri.data()));
            const auto bufferPath =
                path.parent_path()
                / LamaPon::PathFromUtf8(decodedUri);
            recorder.RegisterDependency(
                bufferPath,
                true,
                std::span<const std::uint8_t>(
                    static_cast<const std::uint8_t*>(
                        buffer.data),
                    buffer.size));
        }

        const cgltf_result validationResult =
            cgltf_validate(data.get());
        if (validationResult != cgltf_result_success)
        {
            throw std::runtime_error(
                "Invalid glTF data: "
                + ResultName(validationResult));
        }

        auto model = std::make_shared<SkeletalModel>();
        model->nodes.reserve(data->nodes_count);
        for (cgltf_size index = 0;
            index < data->nodes_count;
            ++index)
        {
            const auto& source = data->nodes[index];
            SkeletalNode node;
            node.name = source.name != nullptr
                ? source.name
                : "Node " + std::to_string(index);
            node.parent = source.parent != nullptr
                ? static_cast<std::ptrdiff_t>(
                    cgltf_node_index(data.get(), source.parent))
                : -1;
            node.bindPose = ReadNodePose(source);
            model->nodes.emplace_back(std::move(node));
        }

        model->skins.reserve(data->skins_count);
        for (cgltf_size index = 0;
            index < data->skins_count;
            ++index)
        {
            const auto& source = data->skins[index];
            if (source.joints_count
                > static_cast<cgltf_size>(
                    DirectX::IEffectSkinning::MaxBones))
            {
                throw std::runtime_error(
                    "glTF skin exceeds the DirectXTK limit of 72 joints.");
            }
            SkeletalSkin skin;
            skin.name = source.name != nullptr
                ? source.name
                : "Skin " + std::to_string(index);
            skin.joints.reserve(source.joints_count);
            skin.inverseBindMatrices.resize(
                source.joints_count);
            for (cgltf_size joint = 0;
                joint < source.joints_count;
                ++joint)
            {
                skin.joints.push_back(
                    cgltf_node_index(
                        data.get(),
                        source.joints[joint]));
                DirectX::XMStoreFloat4x4(
                    &skin.inverseBindMatrices[joint],
                    DirectX::XMMatrixIdentity());
                if (source.inverse_bind_matrices != nullptr)
                {
                    std::array<cgltf_float, 16> matrix{};
                    if (!cgltf_accessor_read_float(
                            source.inverse_bind_matrices,
                            joint,
                            matrix.data(),
                            matrix.size()))
                    {
                        throw std::runtime_error(
                            "Failed to read glTF inverse bind matrix.");
                    }
                    std::memcpy(
                        &skin.inverseBindMatrices[joint],
                        matrix.data(),
                        sizeof(DirectX::XMFLOAT4X4));
                }
            }
            model->skins.emplace_back(std::move(skin));
        }

        std::vector<SkeletalPoseTransform> localBindPose;
        std::vector<DirectX::XMFLOAT4X4> globalBindPose;
        SkeletalModel::SamplePose(
            model->nodes,
            nullptr,
            0.0f,
            localBindPose,
            globalBindPose);

        // 鍵は画像だけでなく用途も含みます。用途で圧縮フォーマットが
        // 変わるので、同じ画像を色と法線の両方に使うモデルでは別々の
        // テクスチャが要ります（実際にはまず無い組み合わせですが、
        // 混ざると法線として読んだBC5を色として貼ることになります）。
        std::map<
            std::pair<
                const cgltf_image*,
                LamaPon::TextureLoader::TextureUsage>,
            Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView>> imageCache;

        // 同じ画像を何度も読まないよう、キャッシュ経由で
        // テクスチャを取り出します（未使用や欠損はnullptr）。
        const auto resolveImage =
            [&](const cgltf_texture_view& view,
                const LamaPon::TextureLoader::TextureUsage usage)
            -> Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView>
            {
                if (view.texture == nullptr
                    || view.texture->image == nullptr)
                {
                    return {};
                }
                const auto* image = view.texture->image;
                const auto key = std::make_pair(image, usage);
                if (const auto existing = imageCache.find(key);
                    existing != imageCache.end())
                {
                    return existing->second;
                }
                auto loaded = LoadImage(
                    assets,
                    path,
                    *image,
                    usage,
                    recorder);
                imageCache.emplace(key, loaded);
                return loaded;
            };

        for (cgltf_size nodeIndex = 0;
            nodeIndex < data->nodes_count;
            ++nodeIndex)
        {
            const auto& node = data->nodes[nodeIndex];
            if (node.mesh == nullptr)
            {
                continue;
            }
            for (cgltf_size primitiveIndex = 0;
                primitiveIndex < node.mesh->primitives_count;
                ++primitiveIndex)
            {
                const auto& source =
                    node.mesh->primitives[primitiveIndex];
                if (source.type != cgltf_primitive_type_triangles)
                {
                    continue;
                }
                const auto* positions = FindAttribute(
                    source,
                    cgltf_attribute_type_position);
                if (positions == nullptr || positions->count == 0)
                {
                    continue;
                }
                const auto* normals = FindAttribute(
                    source,
                    cgltf_attribute_type_normal);
                const auto* texcoords = FindAttribute(
                    source,
                    cgltf_attribute_type_texcoord);
                const auto* tangents = FindAttribute(
                    source,
                    cgltf_attribute_type_tangent);
                const auto* joints = FindAttribute(
                    source,
                    cgltf_attribute_type_joints);
                const auto* weights = FindAttribute(
                    source,
                    cgltf_attribute_type_weights);

                std::vector<std::uint32_t> indices;
                if (source.indices != nullptr)
                {
                    indices.resize(source.indices->count);
                    for (cgltf_size index = 0;
                        index < source.indices->count;
                        ++index)
                    {
                        indices[index] =
                            static_cast<std::uint32_t>(
                                cgltf_accessor_read_index(
                                    source.indices,
                                    index));
                    }
                }
                else
                {
                    indices.resize(positions->count);
                    for (std::uint32_t index = 0;
                        index < indices.size();
                        ++index)
                    {
                        indices[index] = index;
                    }
                }
                if (indices.size() % 3 != 0)
                {
                    throw std::runtime_error(
                        "glTF triangle index count is not divisible by three.");
                }
                if (std::ranges::any_of(
                        indices,
                        [count = positions->count](
                            const std::uint32_t index)
                        {
                            return index >= count;
                        }))
                {
                    throw std::runtime_error(
                        "glTF index references a missing vertex.");
                }

                std::vector<Vertex> vertices;
                vertices.reserve(positions->count);
                for (cgltf_size index = 0;
                    index < positions->count;
                    ++index)
                {
                    const auto position =
                        ReadFloat3(*positions, index);
                    const DirectX::XMFLOAT3 normal =
                        normals != nullptr
                            ? ReadFloat3(*normals, index)
                            : DirectX::XMFLOAT3{};
                    const DirectX::XMFLOAT2 uv =
                        texcoords != nullptr
                            ? ReadFloat2(*texcoords, index)
                            : DirectX::XMFLOAT2{};
                    const DirectX::XMFLOAT4 tangent =
                        tangents != nullptr
                            ? ReadFloat4(*tangents, index)
                            : DirectX::XMFLOAT4{
                                1.0f,
                                0.0f,
                                0.0f,
                                1.0f
                            };

                    DirectX::XMUINT4 jointIndices{};
                    DirectX::XMFLOAT4 blendWeights{
                        1.0f,
                        0.0f,
                        0.0f,
                        0.0f
                    };
                    if (node.skin != nullptr
                        && joints != nullptr
                        && weights != nullptr)
                    {
                        std::array<cgltf_uint, 4> sourceJoints{};
                        std::array<cgltf_float, 4> sourceWeights{};
                        if (!cgltf_accessor_read_uint(
                                joints,
                                index,
                                sourceJoints.data(),
                                sourceJoints.size())
                            || !cgltf_accessor_read_float(
                                weights,
                                index,
                                sourceWeights.data(),
                                sourceWeights.size()))
                        {
                            throw std::runtime_error(
                                "Failed to read glTF skin weights.");
                        }
                        jointIndices = {
                            sourceJoints[0],
                            sourceJoints[1],
                            sourceJoints[2],
                            sourceJoints[3]
                        };
                        if (std::ranges::any_of(
                                sourceJoints,
                                [count = node.skin->joints_count](
                                    const cgltf_uint joint)
                                {
                                    return joint >= count;
                                }))
                        {
                            throw std::runtime_error(
                                "glTF vertex references a missing skin joint.");
                        }
                        const float total =
                            sourceWeights[0]
                            + sourceWeights[1]
                            + sourceWeights[2]
                            + sourceWeights[3];
                        if (total > 0.000001f)
                        {
                            blendWeights = {
                                sourceWeights[0] / total,
                                sourceWeights[1] / total,
                                sourceWeights[2] / total,
                                sourceWeights[3] / total
                            };
                        }
                    }
                    vertices.emplace_back(
                        position,
                        normal,
                        tangent,
                        0xffffffffu,
                        uv,
                        jointIndices,
                        blendWeights);
                }
                if (normals == nullptr)
                {
                    GenerateNormals(vertices, indices);
                }
                ExpandModelBounds(
                    *model,
                    vertices,
                    DirectX::XMLoadFloat4x4(
                        &globalBindPose[nodeIndex]));

                SkeletalPrimitive primitive;
                primitive.meshNode =
                    static_cast<std::size_t>(nodeIndex);
                primitive.skin = node.skin != nullptr
                    ? static_cast<std::ptrdiff_t>(
                        cgltf_skin_index(data.get(), node.skin))
                    : -1;
                primitive.indexCount =
                    static_cast<std::uint32_t>(indices.size());
                primitive.hasLocalBounds =
                    CalculateLocalBounds(
                        vertices,
                        primitive.localBounds);
                // モデルキャッシュ用に幾何を控えます
                // （primitives.emplace_backと同じ順で呼ぶこと）。
                recorder.AddGeometry(
                    vertices.data(),
                    vertices.size(),
                    sizeof(Vertex),
                    indices.data(),
                    indices.size());
                CreateBuffer(
                    device,
                    assets,
                    vertices.data(),
                    vertices.size() * sizeof(Vertex),
                    D3D11_BIND_VERTEX_BUFFER,
                    primitive.vertexBuffer.
                        ReleaseAndGetAddressOf());
                CreateBuffer(
                    device,
                    assets,
                    indices.data(),
                    indices.size() * sizeof(std::uint32_t),
                    D3D11_BIND_INDEX_BUFFER,
                    primitive.indexBuffer.
                        ReleaseAndGetAddressOf());
                if (primitive.skin < 0
                    && primitive.hasLocalBounds)
                {
                    const auto lodLevels = ModelLod::BuildLevels<Vertex>(
                        vertices,
                        indices,
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

                primitive.effect =
                    std::make_shared<DirectX::SkinnedEffect>(
                        device);
                primitive.effect->SetWeightsPerVertex(4);
                const void* shaderBytecode{};
                std::size_t shaderBytecodeSize{};
                primitive.effect->GetVertexShaderBytecode(
                    &shaderBytecode,
                    &shaderBytecodeSize);
                ThrowIfFailed(
                    device->CreateInputLayout(
                        Vertex::InputElements,
                        Vertex::InputElementCount,
                        shaderBytecode,
                        shaderBytecodeSize,
                        primitive.inputLayout.
                            ReleaseAndGetAddressOf()),
                    "Creating glTF input layout");

                if (source.material != nullptr)
                {
                    const auto& material = *source.material;
                    primitive.alpha =
                        material.alpha_mode
                        == cgltf_alpha_mode_blend;
                    primitive.doubleSided =
                        material.double_sided != 0;
                    if (material.has_pbr_metallic_roughness)
                    {
                        const auto& pbr =
                            material.pbr_metallic_roughness;
                        primitive.baseColor = {
                            pbr.base_color_factor[0],
                            pbr.base_color_factor[1],
                            pbr.base_color_factor[2],
                            pbr.base_color_factor[3]
                        };
                        primitive.roughness =
                            pbr.roughness_factor;
                        primitive.metallic = std::clamp(
                            pbr.metallic_factor,
                            0.0f,
                            1.0f);
                        primitive.texture = resolveImage(
                            pbr.base_color_texture,
                            LamaPon::TextureLoader::
                                TextureUsage::Color);
                        // glTFのmetallicRoughnessTextureは1枚に
                        // G=粗さ・B=金属度が入っているので、同じ
                        // 画像を両方の枠へ渡します（シェーダー側で
                        // 読むチャンネルが違うだけです）。
                        auto metallicRoughness = resolveImage(
                            pbr.metallic_roughness_texture,
                            LamaPon::TextureLoader::
                                TextureUsage::DataMap);
                        primitive.roughnessTexture =
                            metallicRoughness;
                        primitive.metallicTexture =
                            std::move(metallicRoughness);
                    }
                    // 法線マップと遮蔽マップはpbrMetallicRoughnessの
                    // 外側にあるので、has_pbr_metallic_roughnessに
                    // 関係なく読み込みます。
                    primitive.normalTexture = resolveImage(
                        material.normal_texture,
                        LamaPon::TextureLoader::
                            TextureUsage::NormalMap);
                    primitive.occlusionTexture = resolveImage(
                        material.occlusion_texture,
                        LamaPon::TextureLoader::
                            TextureUsage::DataMap);
                    if (primitive.occlusionTexture)
                    {
                        // scaleがocclusionTextureのstrengthです。
                        primitive.occlusionStrength = std::clamp(
                            material.occlusion_texture.scale,
                            0.0f,
                            1.0f);
                    }

                    primitive.emissiveTexture = resolveImage(
                        material.emissive_texture,
                        LamaPon::TextureLoader::
                            TextureUsage::Color);
                    // KHR_materials_emissive_strengthがあれば、
                    // 1を超える発光もそのまま反映します（Bloomが
                    // 拾えるように、ここでは飽和させません）。
                    const float emissiveStrength =
                        material.has_emissive_strength != 0
                            ? std::max(
                                material.emissive_strength
                                    .emissive_strength,
                                0.0f)
                            : 1.0f;
                    primitive.emissiveFactor = {
                        std::max(
                            material.emissive_factor[0], 0.0f)
                            * emissiveStrength,
                        std::max(
                            material.emissive_factor[1], 0.0f)
                            * emissiveStrength,
                        std::max(
                            material.emissive_factor[2], 0.0f)
                            * emissiveStrength
                    };
                }
                model->primitives.emplace_back(
                    std::move(primitive));
            }
        }

        model->animations.reserve(data->animations_count);
        for (cgltf_size index = 0;
            index < data->animations_count;
            ++index)
        {
            const auto& source = data->animations[index];
            SkeletalAnimationClip clip;
            clip.name = source.name != nullptr
                ? source.name
                : "Animation " + std::to_string(index + 1);
            for (cgltf_size channelIndex = 0;
                channelIndex < source.channels_count;
                ++channelIndex)
            {
                const auto& channel =
                    source.channels[channelIndex];
                if (channel.target_node == nullptr
                    || channel.sampler == nullptr
                    || channel.sampler->input == nullptr)
                {
                    continue;
                }
                auto& track = FindOrCreateTrack(
                    clip,
                    cgltf_node_index(
                        data.get(),
                        channel.target_node));
                if (channel.target_path
                    == cgltf_animation_path_type_translation)
                {
                    ImportVectorChannel(
                        track.translation,
                        *channel.sampler);
                }
                else if (channel.target_path
                    == cgltf_animation_path_type_rotation)
                {
                    ImportQuaternionChannel(
                        track.rotation,
                        *channel.sampler);
                }
                else if (channel.target_path
                    == cgltf_animation_path_type_scale)
                {
                    ImportVectorChannel(
                        track.scale,
                        *channel.sampler);
                }

                const auto* input = channel.sampler->input;
                for (cgltf_size timeIndex = 0;
                    timeIndex < input->count;
                    ++timeIndex)
                {
                    cgltf_float keyTime{};
                    if (cgltf_accessor_read_float(
                            input,
                            timeIndex,
                            &keyTime,
                            1))
                    {
                        clip.duration =
                            std::max(clip.duration, keyTime);
                    }
                }
            }
            model->animations.emplace_back(std::move(clip));
        }

        if (model->nodes.empty() || model->primitives.empty())
        {
            throw std::runtime_error(
                "glTF does not contain a supported triangle mesh.");
        }

        // 次回のためにインポート結果を保存します（失敗しても無害）。
        ModelCache::Store(cacheKey, *model, recorder);
        return model;
    }
}
