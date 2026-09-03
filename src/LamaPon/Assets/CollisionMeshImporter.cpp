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
        LamaPon::AssetManager* assets{};
    };

    // アーカイブ配布でも読めるよう、cgltfのファイル読み込みを
    // AssetManager経由にします（GltfImporterと同じ流儀）。
    cgltf_result CgltfFileRead(
        const cgltf_memory_options*,
        const cgltf_file_options* fileOptions,
        const char* path,
        cgltf_size* size,
        void** data)
    {
        auto* context = static_cast<CgltfReadContext*>(
            fileOptions->user_data);
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

    void CgltfFileRelease(
        const cgltf_memory_options*,
        const cgltf_file_options*,
        void* data,
        cgltf_size)
    {
        std::free(data);
    }

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
        const cgltf_accessor* positions{};
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

        const auto baseVertex =
            static_cast<std::uint32_t>(vertices.size());
        for (cgltf_size index = 0;
            index < positions->count;
            ++index)
        {
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
            const auto transformed =
                DirectX::XMVector3TransformCoord(
                    DirectX::XMVectorSet(
                        raw[0],
                        raw[1],
                        raw[2],
                        1.0f),
                    worldMatrix);
            XMFLOAT3 vertex{};
            DirectX::XMStoreFloat3(
                &vertex,
                transformed);
            vertices.push_back(vertex);
        }

        if (primitive.indices != nullptr)
        {
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

    void LoadGltf(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        std::vector<XMFLOAT3>& vertices,
        std::vector<std::uint32_t>& indices)
    {
        const std::string utf8Path =
            LamaPon::PathToUtf8(path);
        CgltfReadContext readContext{ &assets };
        cgltf_options options{};
        options.file.read = &CgltfFileRead;
        options.file.release = &CgltfFileRelease;
        options.file.user_data = &readContext;
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

        for (cgltf_size nodeIndex = 0;
            nodeIndex < data->nodes_count;
            ++nodeIndex)
        {
            const auto& node = data->nodes[nodeIndex];
            if (node.mesh == nullptr)
            {
                continue;
            }
            cgltf_float rawMatrix[16]{};
            cgltf_node_transform_world(
                &node,
                rawMatrix);
            // glTFの列優先行列は、行ベクトル規約のDirectXMathでは
            // そのまま16要素を読み込めば同じ変換になります。
            DirectX::XMFLOAT4X4 worldMatrixValues{};
            std::memcpy(
                &worldMatrixValues,
                rawMatrix,
                sizeof(rawMatrix));
            const auto worldMatrix =
                DirectX::XMLoadFloat4x4(
                    &worldMatrixValues);
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

    void LoadFbx(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        std::vector<XMFLOAT3>& vertices,
        std::vector<std::uint32_t>& indices)
    {
        const auto bytes = assets.ReadFileBytes(path);
        if (bytes.empty())
        {
            throw std::runtime_error(
                "FBX file is empty (collision mesh).");
        }

        const std::string utf8Path =
            LamaPon::PathToUtf8(path);
        // FbxImporterと同じ空間変換で読み込み、描画と一致させます。
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

        ufbx_error error{};
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
        const std::unique_ptr<
            ufbx_scene,
            decltype(&ufbx_free_scene)>
            scene(rawScene, &ufbx_free_scene);

        std::vector<std::uint32_t> triangleCorners;
        for (std::size_t nodeIndex = 0;
            nodeIndex < scene->nodes.count;
            ++nodeIndex)
        {
            const ufbx_node* node =
                scene->nodes.data[nodeIndex];
            if (node == nullptr
                || node->mesh == nullptr)
            {
                continue;
            }
            const ufbx_mesh& mesh = *node->mesh;
            triangleCorners.resize(
                std::max<std::size_t>(
                    mesh.max_face_triangles * 3,
                    3));
            const auto baseVertex =
                static_cast<std::uint32_t>(
                    vertices.size());
            for (std::size_t vertexIndex = 0;
                vertexIndex < mesh.num_vertices;
                ++vertexIndex)
            {
                const auto position =
                    ufbx_transform_position(
                        &node->geometry_to_world,
                        mesh.vertices.data[vertexIndex]);
                vertices.push_back({
                    static_cast<float>(position.x),
                    static_cast<float>(position.y),
                    static_cast<float>(position.z) });
            }
            for (std::size_t faceIndex = 0;
                faceIndex < mesh.faces.count;
                ++faceIndex)
            {
                const auto face =
                    mesh.faces.data[faceIndex];
                const std::uint32_t triangleCount =
                    ufbx_triangulate_face(
                        triangleCorners.data(),
                        triangleCorners.size(),
                        &mesh,
                        face);
                for (std::size_t corner = 0;
                    corner
                        < static_cast<std::size_t>(
                            triangleCount) * 3;
                    ++corner)
                {
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
        const auto resolved = assets.ResolvePath(path);
        const std::wstring key = resolved.wstring();
        if (const auto found = g_cache.find(key);
            found != g_cache.end())
        {
            return found->second;
        }

        std::wstring extension =
            resolved.extension().wstring();
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(
                    std::towlower(character));
            });

        std::vector<XMFLOAT3> vertices;
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
