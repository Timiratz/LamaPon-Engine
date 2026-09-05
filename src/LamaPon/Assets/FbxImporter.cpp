#include "LamaPon/Assets/FbxImporter.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/ModelCache.h"
#include "LamaPon/Assets/ModelLod.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <DDSTextureLoader.h>
#include <Effects.h>
#include <VertexTypes.h>
#include <WICTextureLoader.h>
#include <nlohmann/json.hpp>
#include <ufbx.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
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

    struct SceneDeleter final
    {
        void operator()(ufbx_scene* scene) const noexcept
        {
            ufbx_free_scene(scene);
        }
    };

    struct BakedAnimationDeleter final
    {
        void operator()(ufbx_baked_anim* animation) const noexcept
        {
            ufbx_free_baked_anim(animation);
        }
    };

    std::string ToString(const ufbx_string value)
    {
        return value.data != nullptr
            ? std::string(value.data, value.length)
            : std::string{};
    }

    std::string FormatError(const ufbx_error& error)
    {
        std::array<char, 4096> buffer{};
        ufbx_format_error(
            buffer.data(),
            buffer.size(),
            &error);
        return buffer.data();
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

    struct LoadedTexture final
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        bool hasTransparency{};
    };

    bool WicImageHasTransparency(IWICBitmapDecoder* decoder)
    {
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        ThrowIfFailed(
            decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()),
            "Reading the FBX texture frame");

        UINT width{};
        UINT height{};
        ThrowIfFailed(
            frame->GetSize(&width, &height),
            "Reading the FBX texture size");
        if (width == 0 || height == 0)
        {
            return false;
        }

        const std::uint64_t stride64 =
            static_cast<std::uint64_t>(width) * 4u;
        const std::uint64_t byteCount64 =
            stride64 * static_cast<std::uint64_t>(height);
        if (stride64 > std::numeric_limits<UINT>::max()
            || byteCount64 > std::numeric_limits<UINT>::max())
        {
            throw std::runtime_error(
                "FBX texture is too large to inspect its alpha channel.");
        }

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())),
            "Creating WIC for FBX texture inspection");
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        ThrowIfFailed(
            factory->CreateFormatConverter(
                converter.ReleaseAndGetAddressOf()),
            "Creating the FBX texture alpha converter");
        ThrowIfFailed(
            converter->Initialize(
                frame.Get(),
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom),
            "Converting the FBX texture alpha channel");

        const auto stride = static_cast<UINT>(stride64);
        const auto byteCount = static_cast<UINT>(byteCount64);
        std::vector<std::uint8_t> pixels(byteCount);
        ThrowIfFailed(
            converter->CopyPixels(
                nullptr,
                stride,
                byteCount,
                pixels.data()),
            "Reading the FBX texture alpha channel");
        for (std::size_t index = 3;
            index < pixels.size();
            index += 4)
        {
            if (pixels[index] < 255u)
            {
                return true;
            }
        }
        return false;
    }

    bool WicMemoryHasTransparency(
        const std::uint8_t* bytes,
        const std::size_t byteCount)
    {
        if (byteCount > std::numeric_limits<DWORD>::max())
        {
            throw std::runtime_error(
                "Embedded FBX texture is too large to inspect.");
        }

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        ThrowIfFailed(
            CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())),
            "Creating WIC for embedded FBX texture inspection");
        Microsoft::WRL::ComPtr<IWICStream> stream;
        ThrowIfFailed(
            factory->CreateStream(stream.ReleaseAndGetAddressOf()),
            "Creating the embedded FBX texture stream");
        ThrowIfFailed(
            stream->InitializeFromMemory(
                const_cast<BYTE*>(bytes),
                static_cast<DWORD>(byteCount)),
            "Opening the embedded FBX texture stream");
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        ThrowIfFailed(
            factory->CreateDecoderFromStream(
                stream.Get(),
                nullptr,
                WICDecodeMetadataCacheOnLoad,
                decoder.ReleaseAndGetAddressOf()),
            "Opening the embedded FBX texture alpha channel");
        return WicImageHasTransparency(decoder.Get());
    }

    DirectX::XMFLOAT3 ToFloat3(
        const ufbx_vec3 value) noexcept
    {
        return {
            static_cast<float>(value.x),
            static_cast<float>(value.y),
            static_cast<float>(value.z)
        };
    }

    DirectX::XMFLOAT4 ToFloat4(
        const ufbx_quat value) noexcept
    {
        return {
            static_cast<float>(value.x),
            static_cast<float>(value.y),
            static_cast<float>(value.z),
            static_cast<float>(value.w)
        };
    }

    DirectX::XMFLOAT4X4 ToMatrix(
        const ufbx_matrix& value) noexcept
    {
        return {
            value.m00,
            value.m10,
            value.m20,
            0.0f,
            value.m01,
            value.m11,
            value.m21,
            0.0f,
            value.m02,
            value.m12,
            value.m22,
            0.0f,
            value.m03,
            value.m13,
            value.m23,
            1.0f
        };
    }

    LamaPon::SkeletalPoseTransform ToPose(
        const ufbx_transform& transform) noexcept
    {
        LamaPon::SkeletalPoseTransform result;
        result.translation = ToFloat3(transform.translation);
        result.rotation = ToFloat4(transform.rotation);
        result.scale = ToFloat3(transform.scale);
        return result;
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
                "FBX mesh buffer has an invalid size.");
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
            "Creating FBX mesh buffer");
    }

    void EnableAlphaCutout(
        LamaPon::SkeletalPrimitive& primitive,
        ID3D11Device* device)
    {
        if (primitive.cutoutEffect != nullptr)
        {
            return;
        }

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
                Vertex::InputElements,
                Vertex::InputElementCount,
                shaderBytecode,
                shaderBytecodeSize,
                primitive.cutoutInputLayout.ReleaseAndGetAddressOf()),
            "Creating FBX alpha-cutout input layout");
    }

    bool IsDdsPath(const std::filesystem::path& path)
    {
        auto extension = path.extension().wstring();
        std::ranges::transform(
            extension,
            extension.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });
        return extension == L".dds";
    }

    LoadedTexture
        LoadTexture(
            LamaPon::AssetManager& assets,
            const std::filesystem::path& modelPath,
            const ufbx_texture& source,
            const LamaPon::TextureLoader::TextureUsage usage,
            LamaPon::ModelCache::Recorder& recorder)
    {
        LoadedTexture texture;
        if (source.content.data != nullptr
            && source.content.size > 0)
        {
            const auto* bytes =
                static_cast<const std::uint8_t*>(
                    source.content.data);
            const std::filesystem::path hint =
                LamaPon::PathFromUtf8(
                    !ToString(source.relative_filename).empty()
                        ? ToString(source.relative_filename)
                        : ToString(source.filename));
            texture.view = assets.CreateTextureViewFromMemory(
                std::span<const std::uint8_t>(
                    bytes,
                    source.content.size),
                IsDdsPath(hint),
                usage);
            if (!IsDdsPath(hint))
            {
                texture.hasTransparency =
                    WicMemoryHasTransparency(
                        bytes,
                        source.content.size);
            }
            // モデルキャッシュ用に画像そのものを控えます（埋め込みは
            // 元ファイルからしか取り出せないため）。
            recorder.RegisterEmbeddedImage(
                texture.view.Get(),
                std::span<const std::uint8_t>(
                    bytes,
                    source.content.size),
                IsDdsPath(hint),
                texture.hasTransparency,
                usage);
            return texture;
        }

        std::string filename =
            ToString(source.relative_filename);
        if (filename.empty())
        {
            filename = ToString(source.filename);
        }
        if (filename.empty())
        {
            return {};
        }
        std::ranges::replace(filename, '\\', '/');
        auto texturePath =
            LamaPon::PathFromUtf8(filename);
        if (!texturePath.is_absolute())
        {
            texturePath =
                modelPath.parent_path() / texturePath;
        }
        texturePath = texturePath.lexically_normal();
        if (!assets.FileExists(texturePath))
        {
            const auto absolute =
                LamaPon::PathFromUtf8(
                    ToString(source.absolute_filename));
            if (!absolute.empty()
                && assets.FileExists(absolute))
            {
                texturePath = absolute;
            }
            else
            {
                return {};
            }
        }

        const auto fileBytes = assets.ReadFileBytes(texturePath);
        texture.view = assets.CreateTextureViewFromMemory(
            fileBytes,
            IsDdsPath(texturePath),
            usage);
        if (!IsDdsPath(texturePath))
        {
            texture.hasTransparency =
                WicMemoryHasTransparency(
                    fileBytes.data(),
                    fileBytes.size());
        }
        // 外部ファイルはパス＋内容ハッシュだけ控えます（読み込み時に
        // 検証を兼ねて読み直すため、バイト列の複製は要りません）。
        recorder.RegisterExternalImage(
            texture.view.Get(),
            texturePath,
            fileBytes,
            IsDdsPath(texturePath),
            texture.hasTransparency,
            usage);
        return texture;
    }

    const ufbx_material* MaterialForPart(
        const ufbx_node& node,
        const ufbx_mesh& mesh,
        const ufbx_mesh_part& part)
    {
        if (part.index < node.materials.count)
        {
            return node.materials.data[part.index];
        }
        if (part.index < mesh.materials.count)
        {
            return mesh.materials.data[part.index];
        }
        return nullptr;
    }

    void ApplyMaterial(
        LamaPon::SkeletalPrimitive& primitive,
        const ufbx_material* material,
        ID3D11Device* device,
        LamaPon::AssetManager& assets,
        const std::filesystem::path& modelPath,
        std::map<
            std::pair<
                const ufbx_texture*,
                LamaPon::TextureLoader::TextureUsage>,
            LoadedTexture>& textureCache,
        LamaPon::ModelCache::Recorder& recorder)
    {
        if (material == nullptr)
        {
            primitive.baseColor = {
                0.8f,
                0.8f,
                0.8f,
                1.0f
            };
            return;
        }

        const auto& baseColor = material->pbr.base_color;
        primitive.baseColor = {
            static_cast<float>(baseColor.value_vec4.x),
            static_cast<float>(baseColor.value_vec4.y),
            static_cast<float>(baseColor.value_vec4.z),
            static_cast<float>(baseColor.value_vec4.w)
        };
        if (!baseColor.has_value)
        {
            primitive.baseColor = {
                0.8f,
                0.8f,
                0.8f,
                1.0f
            };
        }

        const auto& opacity = material->pbr.opacity;
        if (opacity.has_value)
        {
            primitive.baseColor.w *=
                static_cast<float>(opacity.value_real);
        }
        const auto& roughness = material->pbr.roughness;
        primitive.roughness = roughness.has_value
            ? std::clamp(
                static_cast<float>(roughness.value_real),
                0.0f,
                1.0f)
            : 0.5f;
        const auto& metalness = material->pbr.metalness;
        primitive.metallic = metalness.has_value
            ? std::clamp(
                static_cast<float>(metalness.value_real),
                0.0f,
                1.0f)
            : 0.0f;
        primitive.alpha = primitive.baseColor.w < 0.999f;
        primitive.doubleSided =
            material->features.double_sided.enabled;

        // 同じテクスチャを二重に読まないよう、キャッシュ経由で
        // 取り出します（未接続はnullptrのまま返ります）。
        const auto resolveTexture =
            [&](const ufbx_texture* source,
                const LamaPon::TextureLoader::TextureUsage usage)
            -> LoadedTexture
            {
                if (source == nullptr)
                {
                    return {};
                }
                const auto key = std::make_pair(source, usage);
                if (const auto existing =
                        textureCache.find(key);
                    existing != textureCache.end())
                {
                    return existing->second;
                }
                auto loaded = LoadTexture(
                    assets,
                    modelPath,
                    *source,
                    usage,
                    recorder);
                textureCache.emplace(key, loaded);
                return loaded;
            };

        // 法線マップとPBRマップはbaseColorのテクスチャが無くても
        // 使うので、ベースカラーより先に読み込みます。
        // FBXは粗さ・金属度・遮蔽がそれぞれ別画像ですが、いずれも
        // グレースケール（R=G=B）なので、glTFと同じチャンネル
        // （粗さ=G、金属度=B、遮蔽=R）で読めます。
        const auto resolveMap =
            [&](const ufbx_material_map& map,
                const LamaPon::TextureLoader::TextureUsage usage)
            -> Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            {
                if (!map.texture_enabled
                    || map.texture == nullptr)
                {
                    return {};
                }
                return resolveTexture(map.texture, usage).view;
            };

        using Usage = LamaPon::TextureLoader::TextureUsage;
        primitive.normalTexture =
            resolveMap(material->pbr.normal_map, Usage::NormalMap);
        primitive.roughnessTexture =
            resolveMap(material->pbr.roughness, Usage::DataMap);
        primitive.metallicTexture =
            resolveMap(material->pbr.metalness, Usage::DataMap);
        primitive.occlusionTexture =
            resolveMap(
                material->pbr.ambient_occlusion,
                Usage::DataMap);
        primitive.emissiveTexture =
            resolveMap(material->pbr.emission_color, Usage::Color);

        // 発光色は emission_color × emission_factor です。
        // 色が書かれていないマテリアルは発光なし（黒）にします。
        // ufbxは色が定義されていると factor に既定の1.0を入れる
        // ため、has_value だけでは発光の有無を判定できません。
        const auto& emissionColor = material->pbr.emission_color;
        if (emissionColor.has_value)
        {
            const auto& emissionFactor =
                material->pbr.emission_factor;
            const float strength = emissionFactor.has_value
                ? std::max(
                    static_cast<float>(
                        emissionFactor.value_real),
                    0.0f)
                : 1.0f;
            primitive.emissiveFactor = {
                std::max(
                    static_cast<float>(
                        emissionColor.value_vec4.x),
                    0.0f) * strength,
                std::max(
                    static_cast<float>(
                        emissionColor.value_vec4.y),
                    0.0f) * strength,
                std::max(
                    static_cast<float>(
                        emissionColor.value_vec4.z),
                    0.0f) * strength
            };
        }

        const auto* sourceTexture =
            baseColor.texture_enabled
                ? baseColor.texture
                : nullptr;
        if (sourceTexture == nullptr)
        {
            return;
        }

        // 古いFBXマテリアルでは、拡散テクスチャに最終色が焼き込まれていても、
        // ビューポート用の拡散色が保存されている場合があります。Blenderは
        // テクスチャを直接接続し、古い拡散RGBを乗算しません。同じ挙動にして
        // テクスチャ付きFBXへの二重着色を防ぎ、上で求めたアルファ値だけを残します。
        primitive.baseColor.x = 1.0f;
        primitive.baseColor.y = 1.0f;
        primitive.baseColor.z = 1.0f;

        const auto loadedTexture =
            resolveTexture(sourceTexture, Usage::Color);
        primitive.texture = loadedTexture.view;
        primitive.textureHasTransparency =
            loadedTexture.hasTransparency;
        if (primitive.textureHasTransparency)
        {
            EnableAlphaCutout(primitive, device);
        }
    }

    std::ptrdiff_t AddSkin(
        LamaPon::SkeletalModel& destination,
        const ufbx_skin_deformer& source)
    {
        if (source.clusters.count
            > static_cast<std::size_t>(
                DirectX::IEffectSkinning::MaxBones))
        {
            throw std::runtime_error(
                "FBX skin exceeds the DirectXTK limit of 72 joints.");
        }

        LamaPon::SkeletalSkin skin;
        skin.name = ToString(source.name);
        skin.joints.reserve(source.clusters.count);
        skin.inverseBindMatrices.reserve(
            source.clusters.count);
        for (std::size_t index = 0;
            index < source.clusters.count;
            ++index)
        {
            const auto* cluster = source.clusters.data[index];
            if (cluster == nullptr
                || cluster->bone_node == nullptr)
            {
                throw std::runtime_error(
                    "FBX skin contains a missing bone.");
            }
            skin.joints.push_back(
                cluster->bone_node->typed_id);
            skin.inverseBindMatrices.push_back(
                ToMatrix(cluster->geometry_to_bone));
        }
        destination.skins.emplace_back(std::move(skin));
        return static_cast<std::ptrdiff_t>(
            destination.skins.size() - 1);
    }

    Vertex MakeVertex(
        const ufbx_mesh& mesh,
        const ufbx_skin_deformer* skin,
        const std::uint32_t cornerIndex)
    {
        const auto position =
            ufbx_get_vertex_vec3(
                &mesh.vertex_position,
                cornerIndex);
        const auto normal =
            ufbx_get_vertex_vec3(
                &mesh.vertex_normal,
                cornerIndex);
        const ufbx_vec2 uv = mesh.vertex_uv.exists
            ? ufbx_get_vertex_vec2(
                &mesh.vertex_uv,
                cornerIndex)
            : ufbx_vec2{};
        const ufbx_vec3 tangent =
            mesh.vertex_tangent.exists
                ? ufbx_get_vertex_vec3(
                    &mesh.vertex_tangent,
                    cornerIndex)
                : ufbx_vec3{ 1.0f, 0.0f, 0.0f };
        const ufbx_vec4 color =
            mesh.vertex_color.exists
                ? ufbx_get_vertex_vec4(
                    &mesh.vertex_color,
                    cornerIndex)
                : ufbx_vec4{
                    1.0f,
                    1.0f,
                    1.0f,
                    1.0f
                };

        DirectX::XMUINT4 jointIndices{};
        DirectX::XMFLOAT4 blendWeights{
            1.0f,
            0.0f,
            0.0f,
            0.0f
        };
        if (skin != nullptr)
        {
            const auto vertexIndex =
                mesh.vertex_indices.data[cornerIndex];
            if (vertexIndex >= skin->vertices.count)
            {
                throw std::runtime_error(
                    "FBX skin vertex index is invalid.");
            }
            const auto& skinVertex =
                skin->vertices.data[vertexIndex];
            const std::size_t influenceCount =
                std::min<std::size_t>(
                    skinVertex.num_weights,
                    4);
            std::array<std::uint32_t, 4> joints{};
            std::array<float, 4> weights{};
            float total{};
            for (std::size_t influence = 0;
                influence < influenceCount;
                ++influence)
            {
                const std::size_t weightIndex =
                    skinVertex.weight_begin + influence;
                if (weightIndex >= skin->weights.count)
                {
                    throw std::runtime_error(
                        "FBX skin weight index is invalid.");
                }
                const auto& weight =
                    skin->weights.data[weightIndex];
                if (weight.cluster_index
                    >= skin->clusters.count)
                {
                    throw std::runtime_error(
                        "FBX skin joint index is invalid.");
                }
                joints[influence] = weight.cluster_index;
                weights[influence] =
                    static_cast<float>(weight.weight);
                total += weights[influence];
            }
            if (total > 0.000001f)
            {
                for (auto& weight : weights)
                {
                    weight /= total;
                }
                blendWeights = {
                    weights[0],
                    weights[1],
                    weights[2],
                    weights[3]
                };
            }
            jointIndices = {
                joints[0],
                joints[1],
                joints[2],
                joints[3]
            };
        }

        return Vertex{
            ToFloat3(position),
            ToFloat3(normal),
            DirectX::XMFLOAT4{
                tangent.x,
                tangent.y,
                tangent.z,
                1.0f
            },
            DirectX::XMFLOAT4{
                color.x,
                color.y,
                color.z,
                color.w
            },
            // FBXのUVはテクスチャの左下を原点としますが、Direct3Dは
            // WICテクスチャを左上原点としてサンプリングします。
            DirectX::XMFLOAT2{ uv.x, 1.0f - uv.y },
            jointIndices,
            blendWeights
        };
    }

    std::vector<Vertex> BuildPartVertices(
        const ufbx_mesh& mesh,
        const ufbx_mesh_part& part,
        const ufbx_skin_deformer* skin)
    {
        std::vector<std::uint32_t> triangleCorners(
            std::max<std::size_t>(
                mesh.max_face_triangles * 3,
                3));
        std::vector<Vertex> vertices;
        vertices.reserve(part.num_triangles * 3);
        for (std::size_t faceIndex = 0;
            faceIndex < part.face_indices.count;
            ++faceIndex)
        {
            const auto sourceFace =
                part.face_indices.data[faceIndex];
            if (sourceFace >= mesh.faces.count)
            {
                throw std::runtime_error(
                    "FBX material part references a missing face.");
            }
            const auto face = mesh.faces.data[sourceFace];
            const std::uint32_t triangleCount =
                ufbx_triangulate_face(
                    triangleCorners.data(),
                    triangleCorners.size(),
                    &mesh,
                    face);
            for (std::size_t corner = 0;
                corner < triangleCount * 3;
                ++corner)
            {
                const auto cornerIndex =
                    triangleCorners[corner];
                if (cornerIndex >= mesh.num_indices)
                {
                    throw std::runtime_error(
                        "FBX triangle references a missing vertex.");
                }
                vertices.push_back(
                    MakeVertex(
                        mesh,
                        skin,
                        cornerIndex));
            }
        }
        if (vertices.empty()
            || vertices.size()
                > std::numeric_limits<std::uint32_t>::max())
        {
            throw std::runtime_error(
                "FBX mesh part contains no supported triangles.");
        }
        return vertices;
    }

    // ApplyMaterialで材質(baseColor/texture/cutoutEffect等)を設定済みの
    // primitiveへ、頂点データからGPUバッファ・エフェクト・入力レイアウトを
    // 仕上げます。単独パーツにも、複数パーツを結合した頂点列にも使います。
    struct IndexedGeometry final
    {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    std::uint64_t HashVertex(const Vertex& vertex) noexcept
    {
        constexpr std::uint64_t offset = 14695981039346656037ull;
        constexpr std::uint64_t prime = 1099511628211ull;
        std::uint64_t hash = offset;
        const auto* bytes = reinterpret_cast<
            const std::uint8_t*>(&vertex);
        for (std::size_t index = 0;
            index < sizeof(Vertex);
            ++index)
        {
            hash ^= bytes[index];
            hash *= prime;
        }
        return hash;
    }

    IndexedGeometry BuildIndexedGeometry(
        const std::vector<Vertex>& expanded)
    {
        IndexedGeometry result;
        result.vertices.reserve(expanded.size());
        result.indices.reserve(expanded.size());
        std::unordered_multimap<
            std::uint64_t,
            std::uint32_t> verticesByHash;
        verticesByHash.reserve(expanded.size());

        for (const auto& vertex : expanded)
        {
            const auto hash = HashVertex(vertex);
            std::uint32_t vertexIndex =
                std::numeric_limits<std::uint32_t>::max();
            const auto [begin, end] =
                verticesByHash.equal_range(hash);
            for (auto candidate = begin;
                candidate != end;
                ++candidate)
            {
                if (std::memcmp(
                        &result.vertices[candidate->second],
                        &vertex,
                        sizeof(Vertex)) == 0)
                {
                    vertexIndex = candidate->second;
                    break;
                }
            }
            if (vertexIndex
                == std::numeric_limits<std::uint32_t>::max())
            {
                vertexIndex = static_cast<std::uint32_t>(
                    result.vertices.size());
                result.vertices.push_back(vertex);
                verticesByHash.emplace(hash, vertexIndex);
            }
            result.indices.push_back(vertexIndex);
        }
        return result;
    }

    LamaPon::SkeletalPrimitive FinalizeGpuPrimitive(
        ID3D11Device* device,
        LamaPon::AssetManager& assets,
        const std::vector<Vertex>& vertices,
        LamaPon::SkeletalPrimitive material,
        const std::size_t meshNode,
        const std::ptrdiff_t skin,
        LamaPon::ModelCache::Recorder& recorder)
    {
        const auto geometry = BuildIndexedGeometry(vertices);
        // モデルキャッシュ用に、共有化した頂点列と添字を控えます。
        // primitives.emplace_backと同じ順で呼ばれることが前提です。
        recorder.AddGeometry(
            geometry.vertices.data(),
            geometry.vertices.size(),
            sizeof(Vertex),
            geometry.indices.data(),
            geometry.indices.size());

        LamaPon::SkeletalPrimitive primitive =
            std::move(material);
        primitive.meshNode = meshNode;
        primitive.skin = skin;
        primitive.indexCount =
            static_cast<std::uint32_t>(geometry.indices.size());
        primitive.hasLocalBounds =
            CalculateLocalBounds(
                geometry.vertices,
                primitive.localBounds);
        CreateBuffer(
            device,
            assets,
            geometry.vertices.data(),
            geometry.vertices.size() * sizeof(Vertex),
            D3D11_BIND_VERTEX_BUFFER,
            primitive.vertexBuffer.ReleaseAndGetAddressOf());
        CreateBuffer(
            device,
            assets,
            geometry.indices.data(),
            geometry.indices.size() * sizeof(std::uint32_t),
            D3D11_BIND_INDEX_BUFFER,
            primitive.indexBuffer.ReleaseAndGetAddressOf());
        if (primitive.skin < 0 && primitive.hasLocalBounds)
        {
            const auto lodLevels =
                LamaPon::ModelLod::BuildLevels<Vertex>(
                    geometry.vertices,
                    geometry.indices,
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
            std::make_shared<DirectX::SkinnedEffect>(device);
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
                primitive.inputLayout.ReleaseAndGetAddressOf()),
            "Creating FBX input layout");
        return primitive;
    }

    // ボーンを持たない静的パーツを結合する前に、各パーツのバインド
    // ポーズ変換を頂点へ焼き込みます（位置は通常の変換、法線・接線は
    // 逆転置行列で変換して非一様スケールでも破綻しないようにします）。
    Vertex TransformStaticVertex(
        const Vertex& vertex,
        DirectX::FXMMATRIX transform,
        DirectX::CXMMATRIX normalTransform)
    {
        using namespace DirectX;
        Vertex result = vertex;
        XMStoreFloat3(
            &result.position,
            XMVector3Transform(
                XMLoadFloat3(&vertex.position),
                transform));
        XMStoreFloat3(
            &result.normal,
            XMVector3Normalize(
                XMVector3TransformNormal(
                    XMLoadFloat3(&vertex.normal),
                    normalTransform)));
        const XMFLOAT3 tangentDirection{
            vertex.tangent.x,
            vertex.tangent.y,
            vertex.tangent.z
        };
        XMFLOAT3 transformedTangent{};
        XMStoreFloat3(
            &transformedTangent,
            XMVector3Normalize(
                XMVector3TransformNormal(
                    XMLoadFloat3(&tangentDirection),
                    transform)));
        result.tangent = {
            transformedTangent.x,
            transformedTangent.y,
            transformedTangent.z,
            vertex.tangent.w
        };
        return result;
    }

    // FBXのメッシュパーツ1個分の、GPUアップロード前の生データ。
    struct StagingPart final
    {
        std::vector<Vertex> vertices;
        std::size_t meshNode{};
        std::ptrdiff_t skin{ -1 };
        const ufbx_material* material{};
    };

    LamaPon::SkeletalAnimationClip ImportAnimation(
        const ufbx_scene& scene,
        const ufbx_anim_stack& stack)
    {
        ufbx_bake_opts options{};
        options.trim_start_time = true;
        options.resample_rate = 30.0;
        options.maximum_sample_rate = 60.0;
        options.key_reduction_enabled = true;
        options.key_reduction_rotation = true;

        ufbx_error error{};
        std::unique_ptr<
            ufbx_baked_anim,
            BakedAnimationDeleter> baked(
                ufbx_bake_anim(
                    &scene,
                    stack.anim,
                    &options,
                    &error));
        if (!baked)
        {
            throw std::runtime_error(
                "Failed to bake FBX animation: "
                + FormatError(error));
        }

        LamaPon::SkeletalAnimationClip clip;
        clip.name = ToString(stack.name);
        if (clip.name.empty())
        {
            clip.name = "Animation";
        }
        clip.duration = static_cast<float>(
            std::max(baked->playback_duration, 0.0));
        clip.tracks.reserve(baked->nodes.count);
        for (std::size_t nodeIndex = 0;
            nodeIndex < baked->nodes.count;
            ++nodeIndex)
        {
            const auto& source = baked->nodes.data[nodeIndex];
            if (source.typed_id >= scene.nodes.count)
            {
                continue;
            }
            LamaPon::SkeletalNodeTrack track;
            track.node = source.typed_id;
            track.translation.interpolation =
                LamaPon::SkeletalInterpolation::Linear;
            track.rotation.interpolation =
                LamaPon::SkeletalInterpolation::Linear;
            track.scale.interpolation =
                LamaPon::SkeletalInterpolation::Linear;

            track.translation.keys.reserve(
                source.translation_keys.count);
            for (std::size_t keyIndex = 0;
                keyIndex < source.translation_keys.count;
                ++keyIndex)
            {
                const auto& key =
                    source.translation_keys.data[keyIndex];
                track.translation.keys.push_back({
                    static_cast<float>(key.time),
                    ToFloat3(key.value)
                });
                clip.duration = std::max(
                    clip.duration,
                    static_cast<float>(key.time));
            }
            track.rotation.keys.reserve(
                source.rotation_keys.count);
            for (std::size_t keyIndex = 0;
                keyIndex < source.rotation_keys.count;
                ++keyIndex)
            {
                const auto& key =
                    source.rotation_keys.data[keyIndex];
                track.rotation.keys.push_back({
                    static_cast<float>(key.time),
                    ToFloat4(key.value)
                });
                clip.duration = std::max(
                    clip.duration,
                    static_cast<float>(key.time));
            }
            track.scale.keys.reserve(
                source.scale_keys.count);
            for (std::size_t keyIndex = 0;
                keyIndex < source.scale_keys.count;
                ++keyIndex)
            {
                const auto& key =
                    source.scale_keys.data[keyIndex];
                track.scale.keys.push_back({
                    static_cast<float>(key.time),
                    ToFloat3(key.value)
                });
                clip.duration = std::max(
                    clip.duration,
                    static_cast<float>(key.time));
            }
            if (!track.translation.keys.empty()
                || !track.rotation.keys.empty()
                || !track.scale.keys.empty())
            {
                clip.tracks.emplace_back(std::move(track));
            }
        }
        return clip;
    }

    // モデルの.metaへ書き込まれたインポートスケール（Inspectorの
    // 「Model Asset」パネルから設定）を読み取ります。FBXはファイルごとに
    // 単位設定が食い違っていることがあり、実寸と大きく異なるサイズで
    // インポートされることがあるため、手動で補正できるようにします。
    float ReadModelImportScale(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& modelPath)
    {
        const std::filesystem::path metaPath(
            modelPath.wstring() + L".meta");
        if (!assets.FileExists(metaPath))
        {
            return 1.0f;
        }
        try
        {
            const auto bytes = assets.ReadFileBytes(metaPath);
            if (bytes.empty())
            {
                return 1.0f;
            }
            const auto document = nlohmann::json::parse(
                bytes.begin(),
                bytes.end());
            const double scale = document.value(
                "importScale",
                1.0);
            return scale > 0.0
                ? static_cast<float>(scale)
                : 1.0f;
        }
        catch (const std::exception&)
        {
            return 1.0f;
        }
    }
}

namespace LamaPon
{
    std::shared_ptr<SkeletalModel> FbxImporter::Load(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& path)
    {
        if (device == nullptr)
        {
            throw std::invalid_argument(
                "FbxImporter requires a Direct3D 11 device.");
        }

        if (!assets.FileExists(path))
        {
            throw std::runtime_error(
                "Unable to open FBX file: "
                + PathToUtf8(path));
        }
        std::vector<std::uint8_t> bytes = assets.ReadFileBytes(path);
        if (bytes.empty())
        {
            throw std::runtime_error("FBX file is empty.");
        }

        // インポートキャッシュ。前回と同じ内容（本体・外部テクスチャ・
        // .meta）なら、パースと組み立てを全部飛ばして組み立て済みの
        // 材料から復元します。
        const std::uint64_t cacheKey =
            ModelCache::ComputeKey(bytes, 1);
        if (auto cached = ModelCache::TryLoad(
                device,
                context,
                assets,
                cacheKey))
        {
            Logger::Instance().Info(
                "FBX cache: " + PathToUtf8(path)
                + " nodes="
                + std::to_string(cached->nodes.size())
                + " primitives="
                + std::to_string(cached->primitives.size()));
            return cached;
        }
        ModelCache::Recorder recorder;

        const std::string utf8Path = PathToUtf8(path);
        ufbx_load_opts options{};
        options.filename = {
            utf8Path.data(),
            utf8Path.size()
        };
        options.target_axes =
            ufbx_axes_right_handed_y_up;
        options.target_unit_meters = 1.0f;
        options.space_conversion =
            UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
        options.geometry_transform_handling =
            UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
        options.inherit_mode_handling =
            UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
        options.pivot_handling =
            UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT;
        options.clean_skin_weights = true;
        options.generate_missing_normals = true;
        options.normalize_normals = true;
        options.normalize_tangents = true;
        options.use_blender_pbr_material = true;
        options.node_depth_limit = 512;

        ufbx_error error{};
        std::unique_ptr<ufbx_scene, SceneDeleter> scene(
            ufbx_load_memory(
                bytes.data(),
                bytes.size(),
                &options,
                &error));
        if (!scene)
        {
            throw std::runtime_error(
                "Failed to load FBX: "
                + FormatError(error));
        }

        auto model = std::make_shared<SkeletalModel>();
        model->nodes.reserve(scene->nodes.count);
        for (std::size_t index = 0;
            index < scene->nodes.count;
            ++index)
        {
            const auto* source = scene->nodes.data[index];
            SkeletalNode node;
            node.name = ToString(source->name);
            if (node.name.empty())
            {
                node.name =
                    "Node " + std::to_string(index);
            }
            node.parent = source->parent != nullptr
                ? static_cast<std::ptrdiff_t>(
                    source->parent->typed_id)
                : -1;
            node.bindPose = ToPose(source->local_transform);
            model->nodes.emplace_back(std::move(node));
        }

        // .metaで指定されたインポートスケールをルートノードへ適用します。
        // 子ノードへは通常の階層変換で伝播するため、ここではルート
        // （parentを持たないノード）だけを補正すれば十分です。
        //
        // キャッシュには適用後のノードが入るので、.metaを依存として
        // 記録します（スケールを変えたら作り直しになるように。
        // 「無い」ことも記録します。後から.metaを作ったときも
        // 作り直しが要るためです）。
        {
            const std::filesystem::path metaPath(
                path.wstring() + L".meta");
            const bool metaExists =
                assets.FileExists(metaPath);
            std::vector<std::uint8_t> metaBytes;
            if (metaExists)
            {
                metaBytes = assets.ReadFileBytes(metaPath);
            }
            recorder.RegisterDependency(
                metaPath,
                metaExists,
                metaBytes);
        }
        const float importScale =
            ReadModelImportScale(assets, path);
        if (importScale != 1.0f)
        {
            for (auto& node : model->nodes)
            {
                if (node.parent < 0)
                {
                    node.bindPose.translation.x *= importScale;
                    node.bindPose.translation.y *= importScale;
                    node.bindPose.translation.z *= importScale;
                    node.bindPose.scale.x *= importScale;
                    node.bindPose.scale.y *= importScale;
                    node.bindPose.scale.z *= importScale;
                }
            }
        }

        // 用途で圧縮フォーマットが変わるので、鍵に用途を含めます。
        std::map<
            std::pair<
                const ufbx_texture*,
                LamaPon::TextureLoader::TextureUsage>,
            LoadedTexture> textureCache;
        std::vector<StagingPart> stagingParts;
        for (std::size_t nodeIndex = 0;
            nodeIndex < scene->nodes.count;
            ++nodeIndex)
        {
            const auto* node = scene->nodes.data[nodeIndex];
            const auto* mesh = node->mesh;
            if (mesh == nullptr || !node->visible)
            {
                continue;
            }

            const ufbx_skin_deformer* skin{};
            std::ptrdiff_t skinIndex = -1;
            if (mesh->skin_deformers.count > 0)
            {
                skin = mesh->skin_deformers.data[0];
                skinIndex = AddSkin(*model, *skin);
            }

            for (std::size_t partIndex = 0;
                partIndex < mesh->material_parts.count;
                ++partIndex)
            {
                const auto& part =
                    mesh->material_parts.data[partIndex];
                if (part.num_triangles == 0)
                {
                    continue;
                }
                StagingPart staging;
                staging.vertices =
                    BuildPartVertices(*mesh, part, skin);
                staging.meshNode = node->typed_id;
                staging.skin = skinIndex;
                staging.material =
                    MaterialForPart(*node, *mesh, part);
                stagingParts.push_back(std::move(staging));
            }
        }

        model->animations.reserve(
            scene->anim_stacks.count);
        for (std::size_t index = 0;
            index < scene->anim_stacks.count;
            ++index)
        {
            auto clip = ImportAnimation(
                *scene,
                *scene->anim_stacks.data[index]);
            if (!clip.tracks.empty())
            {
                model->animations.emplace_back(
                    std::move(clip));
            }
        }

        std::vector<SkeletalPoseTransform> localBindPose;
        std::vector<DirectX::XMFLOAT4X4> globalBindPose;
        SkeletalModel::SamplePose(
            model->nodes,
            nullptr,
            0.0f,
            localBindPose,
            globalBindPose);
        for (const auto& staging : stagingParts)
        {
            if (staging.meshNode >= globalBindPose.size())
            {
                continue;
            }
            ExpandModelBounds(
                *model,
                staging.vertices,
                DirectX::XMLoadFloat4x4(
                    &globalBindPose[staging.meshNode]));
        }

        // ボーンやアニメーションを持たない静的パーツは、同じマテリアルを
        // 使うものどうしを1つの頂点/インデックスバッファへ結合します。
        // DCCツール側でパーツが結合されないままFBX出力されることがあり
        // （ボディパネル・ボルト・トリムなどが数百個の別プリミティブに
        // なるケース）、パーツ単位のドローコール・状態変更がCPU負荷の
        // 支配要因になるため。スキン付き、またはアニメーションを含む
        // モデルは対象外とし、既存の描画経路をそのまま使います。
        const std::size_t originalPartCount = stagingParts.size();
        std::size_t mergedGroupCount = 0;
        if (model->animations.empty())
        {
            std::vector<const ufbx_material*> groupMaterials;
            std::vector<std::vector<Vertex>> groupVertices;
            for (auto& staging : stagingParts)
            {
                if (staging.skin >= 0
                    || staging.meshNode >= globalBindPose.size())
                {
                    continue;
                }

                std::size_t groupIndex = groupMaterials.size();
                for (std::size_t index = 0;
                    index < groupMaterials.size();
                    ++index)
                {
                    if (groupMaterials[index]
                        == staging.material)
                    {
                        groupIndex = index;
                        break;
                    }
                }
                if (groupIndex == groupMaterials.size())
                {
                    groupMaterials.push_back(staging.material);
                    groupVertices.emplace_back();
                }

                using namespace DirectX;
                const XMMATRIX bind = XMLoadFloat4x4(
                    &globalBindPose[staging.meshNode]);
                const XMMATRIX normalMatrix =
                    XMMatrixTranspose(
                        XMMatrixInverse(nullptr, bind));
                auto& target = groupVertices[groupIndex];
                target.reserve(
                    target.size() + staging.vertices.size());
                for (const auto& vertex : staging.vertices)
                {
                    target.push_back(
                        TransformStaticVertex(
                            vertex,
                            bind,
                            normalMatrix));
                }
                staging.vertices.clear();
            }

            if (!groupMaterials.empty())
            {
                SkeletalNode mergedRoot;
                mergedRoot.name = "MergedStaticGeometry";
                mergedRoot.parent = -1;
                model->nodes.push_back(std::move(mergedRoot));
                const std::size_t mergedRootIndex =
                    model->nodes.size() - 1;

                for (std::size_t index = 0;
                    index < groupMaterials.size();
                    ++index)
                {
                    SkeletalPrimitive material;
                    ApplyMaterial(
                        material,
                        groupMaterials[index],
                        device,
                        assets,
                        path,
                        textureCache,
                        recorder);
                    model->primitives.emplace_back(
                        FinalizeGpuPrimitive(
                            device,
                            assets,
                            groupVertices[index],
                            std::move(material),
                            mergedRootIndex,
                            -1,
                            recorder));
                }
                mergedGroupCount = groupMaterials.size();
            }
        }

        for (auto& staging : stagingParts)
        {
            if (staging.vertices.empty())
            {
                // 上の結合処理ですでに統合済み。
                continue;
            }
            SkeletalPrimitive material;
            ApplyMaterial(
                material,
                staging.material,
                device,
                assets,
                path,
                textureCache,
                recorder);
            model->primitives.emplace_back(
                FinalizeGpuPrimitive(
                    device,
                    assets,
                    staging.vertices,
                    std::move(material),
                    staging.meshNode,
                    staging.skin,
                    recorder));
        }

        if (model->nodes.empty()
            || model->primitives.empty())
        {
            throw std::runtime_error(
                "FBX does not contain a supported triangle mesh.");
        }

        {
            std::size_t totalIndices{};
            for (const auto& primitive : model->primitives)
            {
                totalIndices += primitive.indexCount;
            }
            std::string summary =
                "FBX imported: " + PathToUtf8(path)
                + " nodes=" + std::to_string(model->nodes.size())
                + " parts=" + std::to_string(originalPartCount);
            if (mergedGroupCount > 0)
            {
                summary += "->"
                    + std::to_string(model->primitives.size())
                    + " (merged)";
            }
            summary += " skins="
                + std::to_string(model->skins.size())
                + " triangles="
                + std::to_string(totalIndices / 3);
            // 結合後もドローコールがまだ多い場合は、DCCツール側で
            // マテリアルをまとめる余地があることが多いので知らせます。
            constexpr std::size_t primitiveCountWarningThreshold = 64;
            if (model->primitives.size()
                > primitiveCountWarningThreshold)
            {
                Logger::Instance().Warning(
                    summary
                    + " -- ドローコール数が多いモデルです。"
                        "DCCツール側で同一マテリアルのメッシュを"
                        "結合してから書き出すことを検討してください。");
            }
            else
            {
                Logger::Instance().Info(summary);
            }
        }

        // 次回のためにインポート結果を保存します（失敗しても無害）。
        ModelCache::Store(cacheKey, *model, recorder);
        return model;
    }
}
