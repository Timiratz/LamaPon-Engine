#include "LamaPon/Assets/CollisionMeshImporter.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Physics/CollisionMesh.h"

#include "cgltf.h"
#include <ufbx.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using DirectX::XMFLOAT3;

    struct CgltfReadContext final
    {
        // コールバック中に借りる取得元
        LamaPon::AssetManager* assets{};
    };

    // アーカイブ対応の入力を確保し、例外を結果コードに変換する(fileOptions: 読み取り設定, path: UTF8パス, size: 読取バイト数の任意出力, data: malloc領域の出力)。
    cgltf_result CgltfFileRead(
        const cgltf_memory_options*,
        const cgltf_file_options* fileOptions,
        const char* path,
        cgltf_size* size,
        void** data)
    {
        // cgltfの読み取り委譲先
        auto* context = static_cast<CgltfReadContext*>(
            fileOptions->user_data);
        try
        {
            // アセットから取得した全バイト
            auto bytes = context->assets->ReadFileBytes(
                LamaPon::PathFromUtf8(path));
            // cgltfへ所有権を渡す入力領域
            void* memory = std::malloc(
                bytes.empty() ? 1 : bytes.size());
            if (memory == nullptr)
            {
                return cgltf_result_out_of_memory;
            }
            if (!bytes.empty())
            {
                std::memcpy(
                    memory,
                    bytes.data(),
                    bytes.size());
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

    // cgltfへ渡した入力領域を解放する(data: mallocで確保した領域)。
    void CgltfFileRelease(
        const cgltf_memory_options*,
        const cgltf_file_options*,
        void* data,
        cgltf_size)
    {
        std::free(data);
    }

    // 三角形の位置をノード変換して配列へ追記する(primitive: 元プリミティブ, worldMatrix: ノードからワールド変換, vertices: 頂点の追記先, indices: 索引の追記先)。
    void AppendGltfPrimitive(
        const cgltf_primitive& primitive,
        const DirectX::XMMATRIX worldMatrix,
        std::vector<XMFLOAT3>& vertices,
        std::vector<std::uint32_t>& indices)
    {
        if (primitive.type
            != cgltf_primitive_type_triangles)
        {
            return;
        }
        // 位置属性のアクセサー
        const cgltf_accessor* positions{};
        // 位置属性を探す属性番号
        for (cgltf_size attribute = 0;
            attribute < primitive.attributes_count;
            ++attribute)
        {
            if (primitive.attributes[attribute].type
                == cgltf_attribute_type_position)
            {
                positions =
                    primitive.attributes[attribute].data;
                break;
            }
        }
        if (positions == nullptr
            || positions->count == 0)
        {
            return;
        }

        // 追記するメッシュの頂点開始番号
        const auto baseVertex =
            static_cast<std::uint32_t>(vertices.size());
        // 読み込む頂点・索引の番号
        for (cgltf_size index = 0;
            index < positions->count;
            ++index)
        {
            // アクセサーから読んだXYZ位置
            cgltf_float raw[3]{};
            if (!cgltf_accessor_read_float(
                    positions,
                    index,
                    raw,
                    3))
            {
                throw std::runtime_error(
                    "Failed to read glTF positions for collision.");
            }
            // ノード変換を適用した頂点位置
            const auto transformed =
                DirectX::XMVector3TransformCoord(
                    DirectX::XMVectorSet(
                        raw[0],
                        raw[1],
                        raw[2],
                        1.0f),
                    worldMatrix);
            // 衝突形状へ追加する頂点
            XMFLOAT3 vertex{};
            DirectX::XMStoreFloat3(
                &vertex,
                transformed);
            vertices.push_back(vertex);
        }

        if (primitive.indices != nullptr)
        {
            // 読み込む頂点・索引の番号
            for (cgltf_size index = 0;
                index < primitive.indices->count;
                ++index)
            {
                indices.push_back(
                    baseVertex
                    + static_cast<std::uint32_t>(
                        cgltf_accessor_read_index(
                            primitive.indices,
                            index)));
            }
        }
        else
        {
            // 読み込む頂点・索引の番号
            for (cgltf_size index = 0;
                index < positions->count;
                ++index)
            {
                indices.push_back(
                    baseVertex
                    + static_cast<std::uint32_t>(index));
            }
        }
    }

    // 外部バッファーも読み全ノードから三角形を集める(assets: ファイルの取得元, path: glTFのパス, vertices: 頂点の追記先, indices: 索引の追記先)。
    void LoadGltf(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        std::vector<XMFLOAT3>& vertices,
        std::vector<std::uint32_t>& indices)
    {
        // 外部ライブラリー用UTF8パス
        const std::string utf8Path =
            LamaPon::PathToUtf8(path);
        // cgltfの取得元を保持する設定
        CgltfReadContext readContext{ &assets };
        // アーカイブ対応のglTF読取設定
        cgltf_options options{};
        options.file.read = &CgltfFileRead;
        options.file.release = &CgltfFileRelease;
        options.file.user_data = &readContext;
        // cgltfが返した解析結果
        cgltf_data* rawData{};
        if (cgltf_parse_file(
                &options,
                utf8Path.c_str(),
                &rawData)
            != cgltf_result_success)
        {
            throw std::runtime_error(
                "Failed to parse glTF for collision mesh.");
        }
        // glTF解析結果の解放付き所有参照
        const std::unique_ptr<
            cgltf_data,
            decltype(&cgltf_free)>
            data(rawData, &cgltf_free);
        if (cgltf_load_buffers(
                &options,
                data.get(),
                utf8Path.c_str())
            != cgltf_result_success)
        {
            throw std::runtime_error(
                "Failed to load glTF buffers for collision mesh.");
        }

        // 三角形を抽出するノード番号
        for (cgltf_size nodeIndex = 0;
            nodeIndex < data->nodes_count;
            ++nodeIndex)
        {
            // メッシュを持つ対象ノード
            const auto& node = data->nodes[nodeIndex];
            if (node.mesh == nullptr)
            {
                continue;
            }
            // glTFの列優先ワールド行列
            cgltf_float rawMatrix[16]{};
            cgltf_node_transform_world(
                &node,
                rawMatrix);

            // DirectX行列へそのまま写す16値
            DirectX::XMFLOAT4X4 worldMatrixValues{};
            // glTFの列優先値は、DirectXの行ベクトル規約では転置せずに同じ変換として読める。
            std::memcpy(
                &worldMatrixValues,
                rawMatrix,
                sizeof(rawMatrix));
            // ノードからワールドへの変換
            const auto worldMatrix =
                DirectX::XMLoadFloat4x4(
                    &worldMatrixValues);
            // 抽出するプリミティブ番号
            for (cgltf_size primitiveIndex = 0;
                primitiveIndex
                    < node.mesh->primitives_count;
                ++primitiveIndex)
            {
                AppendGltfPrimitive(
                    node.mesh->primitives[
                        primitiveIndex],
                    worldMatrix,
                    vertices,
                    indices);
            }
        }
    }

    // 描画と同じ空間設定でFBXを読み三角形を集める(assets: ファイルの取得元, path: FBXのパス, vertices: 頂点の追記先, indices: 索引の追記先)。
    void LoadFbx(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        std::vector<XMFLOAT3>& vertices,
        std::vector<std::uint32_t>& indices)
    {
        // アセットから取得した全バイト
        const auto bytes = assets.ReadFileBytes(path);
        if (bytes.empty())
        {
            throw std::runtime_error(
                "FBX file is empty (collision mesh).");
        }

        // 外部ライブラリー用UTF8パス
        const std::string utf8Path =
            LamaPon::PathToUtf8(path);

        // 描画と同じ右手Y軸・メートル設定
        ufbx_load_opts options{};
        options.filename = {
            utf8Path.data(),
            utf8Path.size() };
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
        options.node_depth_limit = 512;

        // FBXの解析失敗情報
        ufbx_error error{};
        // ufbxが返した解析結果
        ufbx_scene* rawScene = ufbx_load_memory(
            bytes.data(),
            bytes.size(),
            &options,
            &error);
        if (rawScene == nullptr)
        {
            throw std::runtime_error(
                "Failed to load FBX for collision mesh.");
        }
        // FBX解析結果の解放付き所有参照
        const std::unique_ptr<
            ufbx_scene,
            decltype(&ufbx_free_scene)>
            scene(rawScene, &ufbx_free_scene);

        // 三角形化した面のコーナー番号
        std::vector<std::uint32_t> triangleCorners;
        // 三角形を抽出するノード番号
        for (std::size_t nodeIndex = 0;
            nodeIndex < scene->nodes.count;
            ++nodeIndex)
        {
            // メッシュを持つ対象ノード
            const ufbx_node* node =
                scene->nodes.data[nodeIndex];
            if (node == nullptr
                || node->mesh == nullptr)
            {
                continue;
            }
            // 変換元メッシュ・衝突形状
            const ufbx_mesh& mesh = *node->mesh;
            triangleCorners.resize(
                std::max<std::size_t>(
                    mesh.max_face_triangles * 3,
                    3));
            // 追記するメッシュの頂点開始番号
            const auto baseVertex =
                static_cast<std::uint32_t>(
                    vertices.size());
            // 変換するFBX頂点の番号
            for (std::size_t vertexIndex = 0;
                vertexIndex < mesh.num_vertices;
                ++vertexIndex)
            {
                // ノード変換済みのFBX位置
                const auto position =
                    ufbx_transform_position(
                        &node->geometry_to_world,
                        mesh.vertices.data[vertexIndex]);
                vertices.push_back({
                    static_cast<float>(position.x),
                    static_cast<float>(position.y),
                    static_cast<float>(position.z) });
            }
            // 三角形化するFBX面の番号
            for (std::size_t faceIndex = 0;
                faceIndex < mesh.faces.count;
                ++faceIndex)
            {
                // 三角形化するFBX面
                const auto face =
                    mesh.faces.data[faceIndex];
                // 面から得られた三角形数
                const std::uint32_t triangleCount =
                    ufbx_triangulate_face(
                        triangleCorners.data(),
                        triangleCorners.size(),
                        &mesh,
                        face);
                // 三角形のコーナー走査位置
                for (std::size_t corner = 0;
                    corner
                        < static_cast<std::size_t>(
                            triangleCount) * 3;
                    ++corner)
                {
                    // 元メッシュ内のコーナー番号
                    const auto cornerIndex =
                        triangleCorners[corner];
                    if (cornerIndex
                        >= mesh.num_indices)
                    {
                        continue;
                    }
                    indices.push_back(
                        baseVertex
                        + mesh.vertex_indices.data[
                            cornerIndex]);
                }
            }
        }
    }

    // 直列操作するパス別共有索引
    std::unordered_map<
        std::wstring,
        std::shared_ptr<const LamaPon::CollisionMesh>>
        g_cache;
}

namespace LamaPon::CollisionMeshImporter
{
    std::shared_ptr<const CollisionMesh> Load(
        AssetManager& assets,
        const std::filesystem::path& path)
    {
        // 取得元が解決したモデルのパス
        const auto resolved = assets.ResolvePath(path);
        // 解決済みパスのキャッシュキー
        const std::wstring key = resolved.wstring();
        // 同じパスの共有衝突形状
        if (const auto found = g_cache.find(key);
            found != g_cache.end())
        {
            return found->second;
        }

        // 形式判定用の拡張子
        std::wstring extension =
            resolved.extension().wstring();
        // 拡張子を小文字にする(character: 変換する文字)。
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });

        // 変換済み位置の頂点配列
        std::vector<XMFLOAT3> vertices;
        // 各三角形の頂点索引
        std::vector<std::uint32_t> indices;
        if (extension == L".gltf"
            || extension == L".glb")
        {
            LoadGltf(assets, resolved, vertices, indices);
        }
        else if (extension == L".fbx")
        {
            LoadFbx(assets, resolved, vertices, indices);
        }
        else
        {
            throw std::runtime_error(
                "Mesh Colliderが対応していないモデル形式です: "
                + PathToUtf8(resolved));
        }

        // 変換元メッシュ・衝突形状
        auto mesh = std::make_shared<CollisionMesh>();
        mesh->Build(
            std::move(vertices),
            std::move(indices));
        if (mesh->IsEmpty())
        {
            throw std::runtime_error(
                "モデルから衝突用の三角形を抽出できませんでした: "
                + PathToUtf8(resolved));
        }
        g_cache.emplace(key, mesh);
        return mesh;
    }

    void ClearCache()
    {
        g_cache.clear();
    }
}
