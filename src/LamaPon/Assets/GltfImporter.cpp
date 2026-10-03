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

    // 変換後の頂点位置でモデル全体の境界を広げる(model: 更新するモデル, vertices: 元の局所頂点列, transform: モデル座標への変換)。
    void ExpandModelBounds(
        LamaPon::SkeletalModel& model,
        const std::span<const Vertex> vertices,
        DirectX::FXMMATRIX transform) noexcept
    {
        // 境界を求める元頂点
        for (const auto& vertex : vertices)
        {
            // 頂点の局所または変換後位置
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

    // 局所頂点の境界を計算し、頂点の有無を返す(vertices: 境界を求める頂点列, bounds: 境界の返却先)。
    bool CalculateLocalBounds(
        const std::span<const Vertex> vertices,
        LamaPon::Bounds3D& bounds) noexcept
    {
        // 最初の頂点の取り込み済み状態
        bool initialized{};
        // 境界を求める元頂点
        for (const auto& vertex : vertices)
        {
            // 頂点の局所または変換後位置
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

    // HRESULTが失敗なら操作名付き例外を送出する(result: APIの結果, operation: 失敗した操作名)。
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

    // cgltfの失敗結果を診断名へ変換する(result: 解析の結果)。
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

    // 用途と番号に一致する属性を返し、無ければ空を返す(primitive: 元の描画部分, type: 属性の用途, index: 用途内の番号)。
    const cgltf_accessor* FindAttribute(
        // 元データ列の要素番号
        const cgltf_primitive& primitive,
        const cgltf_attribute_type type,
        const cgltf_int index = 0)
    {
        // 用途と番号が合う属性を探す(attribute: 照合する元属性)。
        // 属性またはトラックの検索結果
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

    // アクセサーから実数3成分を読み、失敗時は例外を送出する(accessor: 元データの配置, index: 要素番号)。
    DirectX::XMFLOAT3 ReadFloat3(
        const cgltf_accessor& accessor,
        const cgltf_size index)
    {
        // 読み取った実数成分または値
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

    // アクセサーから実数4成分を読み、失敗時は例外を送出する(accessor: 元データの配置, index: 要素番号)。
    DirectX::XMFLOAT4 ReadFloat4(
        const cgltf_accessor& accessor,
        const cgltf_size index)
    {
        // 読み取った実数成分または値
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

    // アクセサーから実数2成分を読み、失敗時は例外を送出する(accessor: 元データの配置, index: 要素番号)。
    DirectX::XMFLOAT2 ReadFloat2(
        const cgltf_accessor& accessor,
        const cgltf_size index)
    {
        // 読み取った実数成分または値
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

    // 補間形式を変換し、段階・三次以外は線形にする(interpolation: glTFの補間形式)。
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

    // 空白を飛ばしてBase64をバイト列へ復号する(encoded: 符号化された画像内容)。
    std::vector<std::uint8_t> DecodeBase64(
        const std::string_view encoded)
    {
        // Base64の64文字
        static constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789+/";
        // 復号した画像のバイト列
        std::vector<std::uint8_t> result;
        result.reserve(encoded.size() * 3 / 4);
        // 復号中のビット蓄積値
        // 読み取った実数成分または値
        unsigned value{};
        // 未出力のビット数
        int bits{};
        // 符号化内容の1文字
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
            // 頂点の局所または変換後位置
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
        // アーカイブ対応の取得元
        LamaPon::AssetManager* assets{};
    };

    // アーカイブ対応の取得元からcgltf用のコピーを確保する(fileOptions: 取得元のユーザーデータ, path: UTF-8の取得パス, size: バイト数の任意返却先, data: malloc領域の返却先)。
    cgltf_result CgltfFileRead(
        const cgltf_memory_options*,
        const cgltf_file_options* fileOptions,
        const char* path,
        cgltf_size* size,
        void** data)
    {
        // ファイル取得元の利用情報
        auto* context =
            static_cast<CgltfReadContext*>(fileOptions->user_data);
        try
        {
            // 画像またはファイルのバイト列
            auto bytes = context->assets->ReadFileBytes(
                LamaPon::PathFromUtf8(path));
            // cgltfへ渡すmalloc領域
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

    // cgltf読取コールバックのmalloc領域を解放する(data: CgltfFileReadが確保した領域)。
    void CgltfFileRelease(
        const cgltf_memory_options*,
        const cgltf_file_options*,
        void* data,
        cgltf_size)
    {
        std::free(data);
    }

    // MIMEまたはURI拡張子からDDS形式かを調べる(image: 元画像の記述)。
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
        // 元画像のURI
        std::string uri = image.uri;
        // URIの各文字を小文字へ揃える(value: URIの1バイト)。
        std::ranges::transform(
            uri,
            uri.begin(),
            [](const unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            });
        return uri.ends_with(".dds");
    }

    // 内蔵・Base64・外部画像をD3D11ビューにし、復元情報を記録する(assets: 画像の取得元, modelPath: 元モデルのパス, image: 元画像の記述, usage: 画像用途, recorder: キャッシュ記録先)。
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
        LoadImage(
            LamaPon::AssetManager& assets,
            const std::filesystem::path& modelPath,
            const cgltf_image& image,
            const LamaPon::TextureLoader::TextureUsage usage,
            LamaPon::ModelCache::Recorder& recorder)
    {
        // 画像またはファイルのバイト列
        const std::uint8_t* bytes{};
        // 画像のバイト数
        std::size_t byteCount{};
        // 復号して保持する画像の列
        std::vector<std::uint8_t> ownedBytes;

        if (image.buffer_view != nullptr)
        {
            // 画像を格納するバッファー範囲
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
            // 元画像のURI
            const std::string_view uri(image.uri);
            if (uri.starts_with("data:"))
            {
                // データURIの内容開始区切り
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
                // 展開する外部画像のURI
                std::string decodedUri(uri);
                decodedUri.resize(
                    cgltf_decode_uri(decodedUri.data()));
                // 外部画像の解決先パス
                const auto imagePath =
                    modelPath.parent_path()
                    / LamaPon::PathFromUtf8(decodedUri);
                // 画像の元バイト列
                const auto imageBytes =
                    assets.ReadFileBytes(imagePath);
                // 生成したD3D11画像ビュー
                auto texture =
                    assets.CreateTextureViewFromMemory(
                        imageBytes,
                        IsDdsImage(image),
                        usage);
                // 外部画像はパスと内容ハッシュを記録し、キャッシュ復元時に照合する。
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

        // 内蔵画像の元バイト列
        const std::span<const std::uint8_t> imageBytes(
            bytes,
            byteCount);
        // 生成したD3D11画像ビュー
        auto texture = assets.CreateTextureViewFromMemory(
            imageBytes,
            IsDdsImage(image),
            usage);
        // 内蔵画像とBase64画像は、キャッシュへバイト列をコピーする。
        recorder.RegisterEmbeddedImage(
            texture.Get(),
            imageBytes,
            IsDdsImage(image),
            false,
            usage);
        return texture;
    }

    // 内蔵・Base64・外部画像のバイト列を取得する(assets: ファイルの取得元, modelPath: 元モデルのパス, image: 元画像の記述)。
    [[nodiscard]] std::vector<std::uint8_t> ReadImageBytes(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& modelPath,
        const cgltf_image& image)
    {
        if (image.buffer_view != nullptr)
        {
            // 画像を格納するバッファー範囲
            const auto& view = *image.buffer_view;
            // 画像またはファイルのバイト列
            const auto* const bytes = view.data != nullptr
                ? static_cast<const std::uint8_t*>(view.data)
                : view.buffer != nullptr && view.buffer->data != nullptr
                    ? static_cast<const std::uint8_t*>(
                        view.buffer->data) + view.offset
                    : nullptr;
            if (bytes == nullptr || view.size == 0)
            {
                return {};
            }
            return { bytes, bytes + view.size };
        }
        if (image.uri == nullptr)
        {
            return {};
        }

        // 元画像のURI
        const std::string_view uri(image.uri);
        if (uri.starts_with("data:"))
        {
            // データURIの内容開始区切り
            const auto comma = uri.find(',');
            if (comma == std::string_view::npos
                || uri.substr(0, comma).find(";base64")
                    == std::string_view::npos)
            {
                throw std::runtime_error(
                    "Only base64 data URI images are supported.");
            }
            return DecodeBase64(uri.substr(comma + 1));
        }
        // 展開する外部画像のURI
        std::string decodedUri(uri);
        decodedUri.resize(
            cgltf_decode_uri(decodedUri.data()));
        return assets.ReadFileBytes(
            modelPath.parent_path()
            / LamaPon::PathFromUtf8(decodedUri));
    }

    // 任意のD3D11デバイスへ不変バッファーを作る(device: 空なら作成を省略, assets: 転送予算の管理元, data: 初期データの先頭, byteCount: 初期データのバイト数, bindFlags: バインド用途, output: COM参照の返却先)。
    void CreateBuffer(
        ID3D11Device* device,
        LamaPon::AssetManager& assets,
        const void* data,
        const std::size_t byteCount,
        const UINT bindFlags,
        ID3D11Buffer** output)
    {
        if (device == nullptr)
        {
            return;
        }
        if (byteCount > std::numeric_limits<UINT>::max())
        {
            throw std::runtime_error(
                "glTF mesh buffer is too large for Direct3D 11.");
        }
        // 不変バッファーの設定
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(byteCount);
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = bindFlags;
        // バッファーの初期データ
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

    // 行列またはTRSから局所姿勢を取り込む(node: 元ノードの変換情報)。
    LamaPon::SkeletalPoseTransform ReadNodePose(
        const cgltf_node& node)
    {
        using namespace DirectX;
        // ノードの局所姿勢
        LamaPon::SkeletalPoseTransform result;
        if (node.has_matrix)
        {
            // コピーしたノード行列
            XMFLOAT4X4 stored{};
            // glTFの列優先配列をDirectXの行ベクトル用配置へ転置せずコピーする。
            std::memcpy(
                &stored,
                node.matrix,
                sizeof(stored));
            // 分解した拡大倍率
            XMVECTOR scale{};
            // 分解した回転
            XMVECTOR rotation{};
            // 分解した移動量
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

    // 三角形面積で重み付けした法線を生成する(vertices: 法線の更新先, indices: 三角形の索引列)。
    void GenerateNormals(
        std::vector<Vertex>& vertices,
        const std::vector<std::uint32_t>& indices)
    {
        using namespace DirectX;
        // 頂点ごとの面法線の累積
        std::vector<XMVECTOR> accumulated(
            vertices.size(),
            XMVectorZero());
        // 元データ列の要素番号
        for (std::size_t index = 0;
            index + 2 < indices.size();
            index += 3)
        {
            // 三角形の第1頂点番号
            const auto a = indices[index];
            // 三角形の第2頂点番号
            const auto b = indices[index + 1];
            // 三角形の第3頂点番号
            const auto c = indices[index + 2];
            if (a >= vertices.size()
                || b >= vertices.size()
                || c >= vertices.size())
            {
                continue;
            }
            // 第1頂点の位置
            const XMVECTOR pa =
                XMLoadFloat3(&vertices[a].position);
            // 第2頂点の位置
            const XMVECTOR pb =
                XMLoadFloat3(&vertices[b].position);
            // 第3頂点の位置
            const XMVECTOR pc =
                XMLoadFloat3(&vertices[c].position);
            // 元法線または生成する法線
            const XMVECTOR normal = XMVector3Cross(
                XMVectorSubtract(pb, pa),
                XMVectorSubtract(pc, pa));
            accumulated[a] += normal;
            accumulated[b] += normal;
            accumulated[c] += normal;
        }
        // 元データ列の要素番号
        for (std::size_t index = 0;
            index < vertices.size();
            ++index)
        {
            // 元法線または生成する法線
            XMVECTOR normal = accumulated[index];
            // 面法線がほぼゼロなら、既定の上向き法線を使う。
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

    // ノードのトラックを返し、無ければ追加する(clip: 更新するクリップ, node: 対象ノード番号)。
    LamaPon::SkeletalNodeTrack& FindOrCreateTrack(
        LamaPon::SkeletalAnimationClip& clip,
        const std::size_t node)
    {
        // 属性またはトラックの検索結果
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

    // 補間形式とベクトルキーを取り込む(destination: 更新するチャンネル, sampler: 時刻と変換値の配置)。
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
        // 三次スプライン補間か
        const bool cubic = sampler.interpolation
            == cgltf_interpolation_type_cubic_spline;
        // 元データ列の要素番号
        for (cgltf_size index = 0;
            index < sampler.input->count;
            ++index)
        {
            // 変換キーの時刻（秒）
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
            // 取り込む変換キー
            auto& key = destination.keys[index];
            key.time = time;
            // 出力値列の変換値の位置
            // 三次スプラインの出力は入接線・値・出接線の3要素で並ぶ。
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

    // 補間形式と回転キーを取り込む(destination: 更新するチャンネル, sampler: 時刻と回転値の配置)。
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
        // 三次スプライン補間か
        const bool cubic = sampler.interpolation
            == cgltf_interpolation_type_cubic_spline;
        // 元データ列の要素番号
        for (cgltf_size index = 0;
            index < sampler.input->count;
            ++index)
        {
            // 変換キーの時刻（秒）
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
            // 取り込む変換キー
            auto& key = destination.keys[index];
            key.time = time;
            // 出力値列の変換値の位置
            // 三次スプラインの出力は入接線・値・出接線の3要素で並ぶ。
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
    bool GltfImporter::RequiresSkinning(
        AssetManager& assets,
        const std::filesystem::path& path,
        bool* const requiresForwardRole)
    {
        if (requiresForwardRole != nullptr)
        {
            *requiresForwardRole = false;
        }
        // 元モデルのバイト列
        const auto sourceBytes = assets.ReadFileBytes(path);
        // cgltfの解析・読取設定
        cgltf_options options{};
        // 所有権を引き取る解析結果
        cgltf_data* rawData{};
        // glTF解析の結果
        const auto parseResult = cgltf_parse(
            &options,
            sourceBytes.data(),
            sourceBytes.size(),
            &rawData);
        if (parseResult != cgltf_result_success)
        {
            throw std::runtime_error(
                "Failed to inspect glTF skinning: "
                + ResultName(parseResult));
        }
        // cgltf_freeで解放する解析結果
        const std::unique_ptr<cgltf_data, decltype(&cgltf_free)>
            data(rawData, &cgltf_free);

        // Loadと同じく、位置属性がある三角形だけから必要な描画役割を判定する。
        // スキン描画を要する部分の有無
        bool requiresSkinnedRole{};
        // 通常描画を要する部分の有無
        bool foundForwardRole{};
        // 元ノードの番号
        for (cgltf_size nodeIndex = 0;
            nodeIndex < data->nodes_count;
            ++nodeIndex)
        {
            // 取り込むノードまたは元ノード
            const auto& node = data->nodes[nodeIndex];
            if (node.mesh == nullptr)
            {
                continue;
            }
            // 元メッシュの描画部分番号
            for (cgltf_size primitiveIndex = 0;
                primitiveIndex < node.mesh->primitives_count;
                ++primitiveIndex)
            {
                // 描画部分の記述または取込先
                const auto& primitive =
                    node.mesh->primitives[primitiveIndex];
                // 先頭の位置属性の配置
                const auto* positions = FindAttribute(
                    primitive,
                    cgltf_attribute_type_position);
                if (primitive.type == cgltf_primitive_type_triangles
                    && positions != nullptr
                    && positions->count != 0)
                {
                    if (node.skin != nullptr)
                    {
                        requiresSkinnedRole = true;
                    }
                    else
                    {
                        foundForwardRole = true;
                    }
                }
            }
        }
        if (requiresForwardRole != nullptr)
        {
            *requiresForwardRole = foundForwardRole;
        }
        return requiresSkinnedRole;
    }

    std::shared_ptr<SkeletalModel> GltfImporter::Load(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        AssetManager& assets,
        const std::filesystem::path& path)
    {

        // 元モデルのバイト列
        const auto sourceBytes = assets.ReadFileBytes(path);
        // モデル内容から作るキャッシュキー
        const std::uint64_t cacheKey =
            ModelCache::ComputeKey(sourceBytes, 2);
        if (device != nullptr)
        {
            // 復元できたモデルキャッシュ
            if (auto cached = ModelCache::TryLoad(
                    device,
                    context,
                    assets,
                    cacheKey))
            {
                return cached;
            }
        }
        // キャッシュ復元情報の記録先
        ModelCache::Recorder recorder;

        // モデルのUTF-8パス
        const std::string utf8Path = PathToUtf8(path);
        // ファイル取得元の利用情報
        CgltfReadContext readContext{ &assets };
        // cgltfの解析・読取設定
        cgltf_options options{};
        options.file.read = &CgltfFileRead;
        options.file.release = &CgltfFileRelease;
        options.file.user_data = &readContext;
        // 所有権を引き取る解析結果
        cgltf_data* rawData{};
        // glTF解析の結果
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
        // cgltf_freeで解放する解析結果
        const std::unique_ptr<cgltf_data, decltype(&cgltf_free)>
            data(rawData, &cgltf_free);

        // 外部バッファー読込の結果
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
        // 外部バッファーは依存先として記録し、内蔵データはモデル本体のキーで照合する。
        // 外部バッファーの番号
        for (cgltf_size bufferIndex = 0;
            bufferIndex < data->buffers_count;
            ++bufferIndex)
        {
            // 元バッファーの記述
            const auto& buffer = data->buffers[bufferIndex];
            if (buffer.uri == nullptr
                || std::string_view(buffer.uri)
                    .starts_with("data:")
                || buffer.data == nullptr)
            {
                continue;
            }
            // 展開する外部バッファーのURI
            std::string decodedUri(buffer.uri);
            decodedUri.resize(
                cgltf_decode_uri(decodedUri.data()));
            // 外部バッファーの解決先パス
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

        // glTF構造検証の結果
        const cgltf_result validationResult =
            cgltf_validate(data.get());
        if (validationResult != cgltf_result_success)
        {
            throw std::runtime_error(
                "Invalid glTF data: "
                + ResultName(validationResult));
        }

        // 読み込み先のCPUモデル
        auto model = std::make_shared<SkeletalModel>();
        model->nodes.reserve(data->nodes_count);
        // 元データ列の要素番号
        for (cgltf_size index = 0;
            index < data->nodes_count;
            ++index)
        {
            // 取り込む元データの記述
            const auto& source = data->nodes[index];
            // 取り込むノードまたは元ノード
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
        // 元データ列の要素番号
        for (cgltf_size index = 0;
            index < data->skins_count;
            ++index)
        {
            // 取り込む元データの記述
            const auto& source = data->skins[index];
            if (source.joints_count
                > static_cast<cgltf_size>(
                    DirectX::IEffectSkinning::MaxBones))
            {
                throw std::runtime_error(
                    "glTF skin exceeds the DirectXTK limit of 72 joints.");
            }
            // 取り込むスキン
            SkeletalSkin skin;
            skin.name = source.name != nullptr
                ? source.name
                : "Skin " + std::to_string(index);
            skin.joints.reserve(source.joints_count);
            skin.inverseBindMatrices.resize(
                source.joints_count);
            // スキン内のボーン番号
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
                    // 元の逆バインド行列の16成分
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
                    // 逆バインド行列も列優先配列から転置せずコピーする。
                    std::memcpy(
                        &skin.inverseBindMatrices[joint],
                        matrix.data(),
                        sizeof(DirectX::XMFLOAT4X4));
                }
            }
            model->skins.emplace_back(std::move(skin));
        }

        // ノードごとの局所バインド姿勢
        std::vector<SkeletalPoseTransform> localBindPose;
        // ノードごとの全体バインド行列
        std::vector<DirectX::XMFLOAT4X4> globalBindPose;
        SkeletalModel::SamplePose(
            model->nodes,
            nullptr,
            0.0f,
            localBindPose,
            globalBindPose);


        // 画像と用途別のD3D11ビュー
        std::map<
            std::pair<
                const cgltf_image*,
                LamaPon::TextureLoader::TextureUsage>,
            Microsoft::WRL::ComPtr<
                ID3D11ShaderResourceView>> imageCache;


        // D3D11画像を画像と用途別に再利用する(view: 元の画像参照, usage: 画像用途)。
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
                // D3D11デバイスが無ければ、画像はresolveGraphicsImage側で取り込む。
                if (device == nullptr)
                {
                    return {};
                }
                // 元画像の記述
                const auto* image = view.texture->image;
                // 画像と用途の再利用キー
                const auto key = std::make_pair(image, usage);
                // 同じ画像と用途の既存ビュー
                if (const auto existing = imageCache.find(key);
                    existing != imageCache.end())
                {
                    return existing->second;
                }
                // 生成した画像ビュー
                auto loaded = LoadImage(
                    assets,
                    path,
                    *image,
                    usage,
                    recorder);
                imageCache.emplace(key, loaded);
                return loaded;
            };

        // 画像と用途別の画像ハンドル
        std::map<
            std::pair<
                const cgltf_image*,
                LamaPon::TextureLoader::TextureUsage>,
            GraphicsViewHandle> graphicsImageCache;

        // D3D11以外の画像ハンドルを再利用する(view: 元の画像参照, usage: 画像用途)。
        const auto resolveGraphicsImage =
            [&](const cgltf_texture_view& view,
                const LamaPon::TextureLoader::TextureUsage usage)
            -> GraphicsViewHandle
            {
                if (device != nullptr
                    || view.texture == nullptr
                    || view.texture->image == nullptr)
                {
                    return {};
                }
                // 元画像の記述
                const auto* image = view.texture->image;
                // 画像と用途の再利用キー
                const auto key = std::make_pair(image, usage);
                // 同じ画像と用途の既存ビュー
                if (const auto existing = graphicsImageCache.find(key);
                    existing != graphicsImageCache.end())
                {
                    return existing->second;
                }
                // 画像の元バイト列
                const auto imageBytes = ReadImageBytes(
                    assets,
                    path,
                    *image);
                // 生成した画像ビュー
                auto loaded = assets.CreateTextureViewHandleFromMemory(
                    imageBytes,
                    IsDdsImage(*image),
                    usage);
                graphicsImageCache.emplace(key, loaded);
                return loaded;
            };

        // 元ノードの番号
        for (cgltf_size nodeIndex = 0;
            nodeIndex < data->nodes_count;
            ++nodeIndex)
        {
            // 取り込むノードまたは元ノード
            const auto& node = data->nodes[nodeIndex];
            if (node.mesh == nullptr)
            {
                continue;
            }
            // 元メッシュの描画部分番号
            for (cgltf_size primitiveIndex = 0;
                primitiveIndex < node.mesh->primitives_count;
                ++primitiveIndex)
            {
                // 取り込む元データの記述
                const auto& source =
                    node.mesh->primitives[primitiveIndex];
                if (source.type != cgltf_primitive_type_triangles)
                {
                    continue;
                }
                // 先頭の位置属性の配置
                const auto* positions = FindAttribute(
                    source,
                    cgltf_attribute_type_position);
                if (positions == nullptr || positions->count == 0)
                {
                    continue;
                }
                // 先頭の法線属性の配置
                const auto* normals = FindAttribute(
                    source,
                    cgltf_attribute_type_normal);
                // 先頭のUV属性の配置
                const auto* texcoords = FindAttribute(
                    source,
                    cgltf_attribute_type_texcoord);
                // 先頭の接線属性の配置
                const auto* tangents = FindAttribute(
                    source,
                    cgltf_attribute_type_tangent);
                // 先頭のボーン番号属性の配置
                const auto* joints = FindAttribute(
                    source,
                    cgltf_attribute_type_joints);
                // 先頭の影響度属性の配置
                const auto* weights = FindAttribute(
                    source,
                    cgltf_attribute_type_weights);

                // 三角形の索引列
                std::vector<std::uint32_t> indices;
                if (source.indices != nullptr)
                {
                    indices.resize(source.indices->count);
                    // 元データ列の要素番号
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
                    // 元データ列の要素番号
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
                // 索引が頂点数を超えるか調べる(count: 頂点数, index: 判定する索引)。
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

                // 取り込んだ共通CPU頂点列
                std::vector<Vertex> vertices;
                vertices.reserve(positions->count);
                // 元データ列の要素番号
                for (cgltf_size index = 0;
                    index < positions->count;
                    ++index)
                {
                    // 頂点の局所または変換後位置
                    const auto position =
                        ReadFloat3(*positions, index);
                    // 元法線または生成する法線
                    const DirectX::XMFLOAT3 normal =
                        normals != nullptr
                            ? ReadFloat3(*normals, index)
                            : DirectX::XMFLOAT3{};
                    // 頂点の最初のUV座標
                    const DirectX::XMFLOAT2 uv =
                        texcoords != nullptr
                            ? ReadFloat2(*texcoords, index)
                            : DirectX::XMFLOAT2{};
                    // 頂点の接線と従法線の向き
                    const DirectX::XMFLOAT4 tangent =
                        tangents != nullptr
                            ? ReadFloat4(*tangents, index)
                            : DirectX::XMFLOAT4{
                                1.0f,
                                0.0f,
                                0.0f,
                                1.0f
                            };

                    // 頂点が参照する4ボーン番号
                    DirectX::XMUINT4 jointIndices{};
                    // 頂点の4ボーンの影響度
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
                        // 元の4ボーン番号
                        std::array<cgltf_uint, 4> sourceJoints{};
                        // 元の4ボーンの影響度
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
                        // ボーン参照が範囲外か調べる(count: スキンのボーン数, joint: 判定する番号)。
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
                        // 元の4影響度の合計
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
                    // UVは最初の組を使い、元の頂点色は取り込まず白を設定する。
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

                // 描画部分の記述または取込先
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
                // キャッシュ幾何はmodel->primitivesへの追加と同じ順で記録する。
                recorder.AddGeometry(
                    vertices.data(),
                    vertices.size(),
                    sizeof(Vertex),
                    indices.data(),
                    indices.size());
                // 共通CPU頂点列のバイト先頭
                const auto* vertexBytes =
                    reinterpret_cast<const std::uint8_t*>(
                        vertices.data());
                primitive.cpuVertexData.assign(
                    vertexBytes,
                    vertexBytes + vertices.size() * sizeof(Vertex));
                primitive.cpuVertexStride = sizeof(Vertex);
                primitive.cpuIndices = indices;
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
                    // 生成した詳細度別の索引列
                    const auto lodLevels = ModelLod::BuildLevels<Vertex>(
                        vertices,
                        indices,
                        primitive.localBounds);
                    // 詳細度の段階番号
                    for (std::size_t level = 0;
                        level < lodLevels.size();
                        ++level)
                    {
                        if (lodLevels[level].empty())
                        {
                            continue;
                        }
                        primitive.cpuLodIndices[level] =
                            lodLevels[level];
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

                if (device != nullptr)
                {
                    primitive.effect =
                        std::make_shared<DirectX::SkinnedEffect>(
                            device);
                    primitive.effect->SetWeightsPerVertex(4);
                    // 入力配置用の頂点シェーダー
                    const void* shaderBytecode{};
                    // シェーダーのバイト数
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
                }

                if (source.material != nullptr)
                {
                    // 描画部分の元材質
                    const auto& material = *source.material;
                    primitive.alpha =
                        material.alpha_mode
                        == cgltf_alpha_mode_blend;
                    primitive.doubleSided =
                        material.double_sided != 0;
                    if (material.has_pbr_metallic_roughness)
                    {
                        // 金属度・粗さ形式の材質
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
                        primitive.embeddedTextures.albedo =
                            resolveGraphicsImage(
                                pbr.base_color_texture,
                                LamaPon::TextureLoader::
                                    TextureUsage::Color);
                        // 金属度・粗さ画像はBが金属度、Gが粗さなので同じ画像を両スロットへ渡す。
                        // 金属度・粗さのD3D11画像
                        auto metallicRoughness = resolveImage(
                            pbr.metallic_roughness_texture,
                            LamaPon::TextureLoader::
                                TextureUsage::DataMap);
                        primitive.roughnessTexture =
                            metallicRoughness;
                        primitive.metallicTexture =
                            std::move(metallicRoughness);
                        // 金属度・粗さの画像ハンドル
                        auto graphicsMetallicRoughness =
                            resolveGraphicsImage(
                                pbr.metallic_roughness_texture,
                                LamaPon::TextureLoader::
                                    TextureUsage::DataMap);
                        primitive.embeddedTextures.roughness =
                            graphicsMetallicRoughness;
                        primitive.embeddedTextures.metallic =
                            std::move(graphicsMetallicRoughness);
                    }
                    // 法線と遮蔽画像はPBRの金属度・粗さ情報が無い材質でも読み込む。
                    primitive.normalTexture = resolveImage(
                        material.normal_texture,
                        LamaPon::TextureLoader::
                            TextureUsage::NormalMap);
                    primitive.embeddedTextures.normal =
                        resolveGraphicsImage(
                            material.normal_texture,
                            LamaPon::TextureLoader::
                                TextureUsage::NormalMap);
                    primitive.occlusionTexture = resolveImage(
                        material.occlusion_texture,
                        LamaPon::TextureLoader::
                            TextureUsage::DataMap);
                    primitive.embeddedTextures.occlusion =
                        resolveGraphicsImage(
                            material.occlusion_texture,
                            LamaPon::TextureLoader::
                                TextureUsage::DataMap);
                    if (primitive.occlusionTexture
                        || primitive.embeddedTextures.occlusion)
                    {
                        // cgltfのscaleに入った遮蔽強度を0〜1へ制限する。
                        primitive.occlusionStrength = std::clamp(
                            material.occlusion_texture.scale,
                            0.0f,
                            1.0f);
                    }

                    primitive.emissiveTexture = resolveImage(
                        material.emissive_texture,
                        LamaPon::TextureLoader::
                            TextureUsage::Color);
                    primitive.embeddedTextures.emissive =
                        resolveGraphicsImage(
                            material.emissive_texture,
                            LamaPon::TextureLoader::
                                TextureUsage::Color);
                    // 発光強度は非負に制限するが、1を超える値も保持する。
                    // 非負で上限無しの発光強度
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
                    primitive.embeddedTextures.occlusionStrength =
                        primitive.occlusionStrength;
                    primitive.embeddedTextures.emissiveFactor =
                        primitive.emissiveFactor;
                }
                model->primitives.emplace_back(
                    std::move(primitive));
            }
        }

        model->animations.reserve(data->animations_count);
        // 元データ列の要素番号
        for (cgltf_size index = 0;
            index < data->animations_count;
            ++index)
        {
            // 取り込む元データの記述
            const auto& source = data->animations[index];
            // 取り込むアニメーションクリップ
            SkeletalAnimationClip clip;
            clip.name = source.name != nullptr
                ? source.name
                : "Animation " + std::to_string(index + 1);
            // 元の変換チャンネル番号
            for (cgltf_size channelIndex = 0;
                channelIndex < source.channels_count;
                ++channelIndex)
            {
                // 元の変換チャンネル
                const auto& channel =
                    source.channels[channelIndex];
                if (channel.target_node == nullptr
                    || channel.sampler == nullptr
                    || channel.sampler->input == nullptr)
                {
                    continue;
                }
                // 更新するノードの変換トラック
                auto& track = FindOrCreateTrack(
                    clip,
                    cgltf_node_index(
                        data.get(),
                        channel.target_node));
                // 位置・回転・倍率だけを取り込み、モーフの重みは変換しない。
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

                // チャンネルの時刻列
                const auto* input = channel.sampler->input;
                // 変換キーの時刻番号
                for (cgltf_size timeIndex = 0;
                    timeIndex < input->count;
                    ++timeIndex)
                {
                    // クリップ内のキー時刻（秒）
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

        // D3D11の復元情報だけをモデルキャッシュへ保存する。
        if (device != nullptr)
        {
            ModelCache::Store(cacheKey, *model, recorder);
        }
        return model;
    }
}
