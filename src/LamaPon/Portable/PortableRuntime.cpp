#include "LamaPon/LamaPon.h"

#include "LamaPon/Web/WebAudioRuntime.h"
#include "LamaPon/Web/WebInput.h"
#include "LamaPon/Web/WebMath.h"
#include "LamaPon/Web/WebRenderer3D.h"

#if defined(LAMAPON_NATIVE_RUNTIME)
#include "LamaPon/Native/NativeBridge.h"
#else
#include <emscripten.h>
#endif

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <numbers>
#include <random>
#include <unordered_map>
#include <unordered_set>

#ifndef LAMAPON_WEB_AUDIO_ENABLED
#define LAMAPON_WEB_AUDIO_ENABLED 0
#endif

namespace
{
    using Json = nlohmann::json;
    using LamaPon::GameObject;
    using LamaPon::ProceduralMeshVertex;
    using LamaPon::Web::Mat4;
    using LamaPon::Web::Vec3;

    // Serialized assets and SDL/browser bridges always use UTF-8. A narrow
    // filesystem constructor would instead use the Windows ANSI code page.
    [[nodiscard]] std::filesystem::path PortablePathFromUtf8(const std::string_view text)
    {
        std::u8string utf8;
        utf8.reserve(text.size());
        for (const unsigned char byte : text) utf8.push_back(static_cast<char8_t>(byte));
        return std::filesystem::path(utf8);
    }

    [[nodiscard]] std::string PortablePathToUtf8(const std::filesystem::path& path)
    {
        const auto utf8 = path.generic_u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    struct PortableInputBinding final
    {
        // Input control名
        std::string control;
        // Control入力の倍率
        float scale{ 1.0f };
    };

    // Script別Input binding一覧を返します。
    std::unordered_map<std::string, std::vector<PortableInputBinding>>&
        PortableInputBindings()
    {
        // 登録済みのScript別binding
        static std::unordered_map<
            std::string,
            std::vector<PortableInputBinding>> value;
        return value;
    }

    // Script factory一覧を返します。
    std::unordered_map<std::string, LamaPon::ScriptFactory>& ScriptFactories()
    {
        // 登録済みのScript factory
        static std::unordered_map<std::string, LamaPon::ScriptFactory> value;
        return value;
    }

    // DirectX vectorをWeb vectorへ変換します(value: 変換元)。
    [[nodiscard]] Vec3 WebVector(const DirectX::XMFLOAT3& value) noexcept
    {
        return { value.x, value.y, value.z };
    }

    // 2D座標を0から1へ制限します(value: 制限前の座標)。
    [[nodiscard]] DirectX::XMFLOAT2 ClampUnit2(
        const DirectX::XMFLOAT2 value) noexcept
    {
        return {
            std::clamp(value.x, 0.0f, 1.0f),
            std::clamp(value.y, 0.0f, 1.0f)
        };
    }

    // Web vectorをDirectX vectorへ変換します(value: 変換元)。
    [[nodiscard]] DirectX::XMFLOAT3 DirectXVector(const Vec3& value) noexcept
    {
        return { value.x, value.y, value.z };
    }

    // DirectX colorをWeb colorへ変換します(value: 変換元)。
    [[nodiscard]] LamaPon::Web::Color WebColor(
        const DirectX::XMFLOAT4& value) noexcept
    {
        return { value.x, value.y, value.z, value.w };
    }

    // Local transformから行列を作ります(transform: 対象objectのtransform)。
    [[nodiscard]] Mat4 LocalMatrix(const LamaPon::Transform& transform)
    {
        return LamaPon::Web::Multiply(
            LamaPon::Web::Translation(WebVector(transform.position)),
            LamaPon::Web::Multiply(
                LamaPon::Web::RotationY(transform.rotation.y),
                LamaPon::Web::Multiply(
                    LamaPon::Web::RotationX(transform.rotation.x),
                    LamaPon::Web::Multiply(
                        LamaPon::Web::RotationZ(transform.rotation.z),
                        LamaPon::Web::Scale(WebVector(transform.scale))))));
    }

    // 親を含むworld行列を返します(object: 対象object)。
    [[nodiscard]] Mat4 ComputeWorldMatrix(const GameObject& object)
    {
        // 親を含まないlocal行列
        const Mat4 local = LocalMatrix(object.GetTransform());
        return object.Parent() != nullptr
            ? LamaPon::Web::Multiply(ComputeWorldMatrix(*object.Parent()), local)
            : local;
    }

    // 行列でpointを変換します(matrix: 変換行列, point: 入力座標)。
    [[nodiscard]] Vec3 TransformPoint(const Mat4& matrix, const Vec3& point)
    {
        return {
            matrix.values[0] * point.x + matrix.values[4] * point.y
                + matrix.values[8] * point.z + matrix.values[12],
            matrix.values[1] * point.x + matrix.values[5] * point.y
                + matrix.values[9] * point.z + matrix.values[13],
            matrix.values[2] * point.x + matrix.values[6] * point.y
                + matrix.values[10] * point.z + matrix.values[14],
        };
    }

    // 三角形面からvertex normalを再計算します(vertices: 頂点, indices: 三角形index)。
    void RecalculateNormals(
        std::vector<ProceduralMeshVertex>& vertices,
        const std::vector<std::uint32_t>& indices)
    {
        // 各vertexの蓄積済みnormalを初期化します(vertex: 対象vertex)。
        for (auto& vertex : vertices)
        {
            vertex.normal = {};
        }
        // Triangleを順にnormal計算します(index: triangle開始index)。
        for (std::size_t index{}; index + 2 < indices.size(); index += 3)
        {
            // Triangleの先頭vertex index
            const std::uint32_t a = indices[index];
            // Triangleの次vertex index
            const std::uint32_t b = indices[index + 1];
            // Triangleの末尾vertex index
            const std::uint32_t c = indices[index + 2];
            // 範囲外indexを含むTriangleを無視します。
            if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size())
            {
                continue;
            }
            // Triangleの先頭vertex位置
            const Vec3 first = WebVector(vertices[a].position);
            // Triangleの次vertex位置
            const Vec3 second = WebVector(vertices[b].position);
            // Triangleの末尾vertex位置
            const Vec3 third = WebVector(vertices[c].position);
            // Triangle面のnormal
            const Vec3 normal = LamaPon::Web::Cross(third - first, second - first);
            // 面normalを各vertexへ蓄積します(vertexIndex: 対象vertex)。
            for (const std::uint32_t vertexIndex : { a, b, c })
            {
                // 累積するvertex normal
                auto& value = vertices[vertexIndex].normal;
                value.x += normal.x;
                value.y += normal.y;
                value.z += normal.z;
            }
        }
        // 蓄積normalを単位化します(vertex: 対象vertex)。
        for (auto& vertex : vertices)
        {
            // 正規化後のvertex normal
            const Vec3 normal = LamaPon::Web::Normalize(WebVector(vertex.normal));
            vertex.normal = DirectXVector(
                LamaPon::Web::LengthSquared(normal) > 0.0f
                    ? normal
                    : Vec3{ 0.0f, 1.0f, 0.0f });
        }
    }

    // Shape別のmesh dataを作ります(shape: 形状, vertices: 頂点出力, indices: index出力)。
    void BuildPrimitive(
        LamaPon::PrimitiveShape shape,
        std::vector<ProceduralMeshVertex>& vertices,
        std::vector<std::uint32_t>& indices)
    {
        // Plane用のmeshを作ります。
        if (shape == LamaPon::PrimitiveShape::Plane)
        {
            vertices = {
                { { -0.5f, 0.0f, -0.5f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
                { {  0.5f, 0.0f, -0.5f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
                { { -0.5f, 0.0f,  0.5f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
                { {  0.5f, 0.0f,  0.5f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            };
            indices = { 0, 2, 1, 1, 2, 3 };
            return;
        }
        // 各面を分離したCube meshを作ります。
        if (shape == LamaPon::PrimitiveShape::Cube)
        {
            // Cube角のlocal座標
            constexpr std::array<DirectX::XMFLOAT3, 8> corners = {{
                { -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f },
                { -0.5f,  0.5f, -0.5f }, { 0.5f,  0.5f, -0.5f },
                { -0.5f, -0.5f,  0.5f }, { 0.5f, -0.5f,  0.5f },
                { -0.5f,  0.5f,  0.5f }, { 0.5f,  0.5f,  0.5f },
            }};
            // Cube面ごとのtriangle index
            constexpr std::array<std::uint32_t, 36> cubeIndices = {{
                0, 2, 1, 1, 2, 3, 5, 7, 4, 4, 7, 6,
                4, 6, 0, 0, 6, 2, 1, 3, 5, 5, 3, 7,
                2, 6, 3, 3, 6, 7, 4, 0, 5, 5, 0, 1,
            }};
            vertices.reserve(cubeIndices.size());
            indices.reserve(cubeIndices.size());
            // 面vertexを複製してindexを作ります(corner: 参照する角番号)。
            for (const std::uint32_t corner : cubeIndices)
            {
                indices.push_back(static_cast<std::uint32_t>(vertices.size()));
                vertices.push_back({ corners[corner], {}, {} });
            }
            RecalculateNormals(vertices, indices);
            return;
        }
        // Cylinder meshを側面と上下の面に分けて作ります。
        if (shape == LamaPon::PrimitiveShape::Cylinder)
        {
            // Cylinder側面の分割数
            constexpr int segmentCount = 32;
            // 側面ringを作ります(segment: ring分割番号)。
            for (int segment{}; segment <= segmentCount; ++segment)
            {
                // 横方向のtexture座標
                const float u = static_cast<float>(segment) / segmentCount;
                // ring上の角度radian
                const float angle = u * std::numbers::pi_v<float> * 2.0f;
                // ring位置のX成分
                const float x = std::cos(angle);
                // ring位置のZ成分
                const float z = std::sin(angle);
                vertices.push_back({
                    { x * 0.5f, -0.5f, z * 0.5f },
                    { x, 0.0f, z }, { u, 1.0f },
                });
                vertices.push_back({
                    { x * 0.5f, 0.5f, z * 0.5f },
                    { x, 0.0f, z }, { u, 0.0f },
                });
            }
            // 側面triangleを作ります(segment: ring分割番号)。
            for (int segment{}; segment < segmentCount; ++segment)
            {
                // 側面segment下端のvertex index
                const std::uint32_t bottom = static_cast<std::uint32_t>(
                    segment * 2);
                indices.insert(indices.end(), {
                    bottom, bottom + 1, bottom + 2,
                    bottom + 2, bottom + 1, bottom + 3,
                });
            }
            // 上面中心vertexのindex
            const std::uint32_t topCenter = static_cast<std::uint32_t>(
                vertices.size());
            vertices.push_back({
                { 0.0f, 0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f },
                { 0.5f, 0.5f },
            });
            // 下面中心vertexのindex
            const std::uint32_t bottomCenter = static_cast<std::uint32_t>(
                vertices.size());
            vertices.push_back({
                { 0.0f, -0.5f, 0.0f }, { 0.0f, -1.0f, 0.0f },
                { 0.5f, 0.5f },
            });
            // ring vertex列の開始index
            const std::uint32_t ringStart = static_cast<std::uint32_t>(
                vertices.size());
            // 上下面のring vertexを作ります(segment: ring分割番号)。
            for (int segment{}; segment <= segmentCount; ++segment)
            {
                // cap ring上の角度radian
                const float angle = static_cast<float>(segment) / segmentCount
                    * std::numbers::pi_v<float> * 2.0f;
                // ring位置のX成分
                const float x = std::cos(angle);
                // ring位置のZ成分
                const float z = std::sin(angle);
                vertices.push_back({
                    { x * 0.5f, 0.5f, z * 0.5f },
                    { 0.0f, 1.0f, 0.0f },
                    { x * 0.5f + 0.5f, z * 0.5f + 0.5f },
                });
                vertices.push_back({
                    { x * 0.5f, -0.5f, z * 0.5f },
                    { 0.0f, -1.0f, 0.0f },
                    { x * 0.5f + 0.5f, z * 0.5f + 0.5f },
                });
            }
            // 上下面のtriangleを作ります(segment: ring分割番号)。
            for (int segment{}; segment < segmentCount; ++segment)
            {
                // 上面segmentの開始vertex index
                const std::uint32_t top = ringStart
                    + static_cast<std::uint32_t>(segment * 2);
                indices.insert(indices.end(), {
                    topCenter, top, top + 2,
                    bottomCenter, top + 3, top + 1,
                });
            }
            return;
        }

        // Sphereの縦分割数
        constexpr int latitudeCount = 16;
        // Sphereの横分割数
        constexpr int longitudeCount = 32;
        // Sphere頂点を緯度ごとに作ります(latitude: 緯度番号)。
        for (int latitude{}; latitude <= latitudeCount; ++latitude)
        {
            // 緯度の正規化値
            const float v = static_cast<float>(latitude) / latitudeCount;
            // 緯度radian
            const float phi = v * std::numbers::pi_v<float>;
            // 各緯度の頂点を作ります(longitude: 経度番号)。
            for (int longitude{}; longitude <= longitudeCount; ++longitude)
            {
                // 経度の正規化値
                const float u = static_cast<float>(longitude) / longitudeCount;
                // 経度radian
                const float theta = u * std::numbers::pi_v<float> * 2.0f;
                // 球面上のunit normal
                const DirectX::XMFLOAT3 normal{
                    std::sin(phi) * std::cos(theta),
                    std::cos(phi),
                    std::sin(phi) * std::sin(theta),
                };
                vertices.push_back({
                    { normal.x * 0.5f, normal.y * 0.5f, normal.z * 0.5f },
                    normal,
                    { u, v },
                });
            }
        }
        // Sphereの各面へtriangle indexを作ります(latitude: 緯度番号)。
        for (int latitude{}; latitude < latitudeCount; ++latitude)
        {
            // 各緯度帯の面を作ります(longitude: 経度番号)。
            for (int longitude{}; longitude < longitudeCount; ++longitude)
            {
                // 現在のquad左上vertex index
                const std::uint32_t a = static_cast<std::uint32_t>(
                    latitude * (longitudeCount + 1) + longitude);
                // 次の緯度帯の対応vertex index
                const std::uint32_t b = a + longitudeCount + 1;
                indices.insert(indices.end(), {
                    a, b, a + 1,
                    a + 1, b, b + 1,
                });
            }
        }
    }

    // Ray交差を調べます(origin: 始点, direction: 方向, a: 先頭頂点, b: 次頂点, c: 末尾頂点, distance: 距離出力, normal: 法線出力)。
    [[nodiscard]] bool RayTriangle(
        const Vec3& origin,
        const Vec3& direction,
        const Vec3& a,
        const Vec3& b,
        const Vec3& c,
        float& distance,
        Vec3& normal)
    {
        // 退化triangle判定の許容誤差
        constexpr float epsilon = 0.000001f;
        // Triangleの2辺
        const Vec3 edge1 = b - a;
        // 第二辺のvectorです。
        const Vec3 edge2 = c - a;
        // Ray方向とedge2の外積
        const Vec3 p = LamaPon::Web::Cross(direction, edge2);
        // Triangle交差判定のdeterminant
        const float determinant = LamaPon::Web::Dot(edge1, p);
        // Rayがtriangle面と平行なら交差しません。
        if (std::abs(determinant) < epsilon)
        {
            return false;
        }
        // determinantの逆数
        const float inverse = 1.0f / determinant;
        // 始点からvertex aへのvector
        const Vec3 t = origin - a;
        // Triangle内の第一barycentric座標
        const float u = LamaPon::Web::Dot(t, p) * inverse;
        // 第一barycentric座標が範囲外なら交差しません。
        if (u < 0.0f || u > 1.0f)
        {
            return false;
        }
        // tとedge1の外積
        const Vec3 q = LamaPon::Web::Cross(t, edge1);
        // Triangle内の第二barycentric座標
        const float v = LamaPon::Web::Dot(direction, q) * inverse;
        // 第二座標または座標合計が範囲外なら交差しません。
        if (v < 0.0f || u + v > 1.0f)
        {
            return false;
        }
        // Ray上の交差距離
        const float result = LamaPon::Web::Dot(edge2, q) * inverse;
        // Rayの後方にある交差は除外します。
        if (result < 0.0f)
        {
            return false;
        }
        distance = result;
        normal = LamaPon::Web::Normalize(LamaPon::Web::Cross(edge1, edge2));
        // 法線をRayと逆向きへ揃えます。
        if (LamaPon::Web::Dot(normal, direction) > 0.0f)
        {
            normal = normal * -1.0f;
        }
        return true;
    }

    // Web renderer用にindex列を返します(source: mesh index列)。
    [[nodiscard]] std::vector<std::uint32_t> WebIndices(
        const std::vector<std::uint32_t>& source)
    {
        return source;
    }

    // Web renderer用のvertex列を作ります(source: Engine頂点列)。
    [[nodiscard]] std::vector<LamaPon::Web::Vertex3D> WebVertices(
        const std::vector<ProceduralMeshVertex>& source)
    {
        // 変換後のWeb頂点列
        std::vector<LamaPon::Web::Vertex3D> result;
        result.reserve(source.size());
        // 各Engine頂点をWeb形式へ変換します(vertex: 対象vertex)。
        for (const auto& vertex : source)
        {
            result.push_back({
                WebVector(vertex.position),
                WebVector(vertex.normal),
                { vertex.textureCoordinate.x, vertex.textureCoordinate.y },
            });
        }
        return result;
    }

    // Asset pathをWeb仮想pathへ変換します(path: source上のasset path)。
    [[nodiscard]] std::string VirtualAssetPath(
        const std::filesystem::path& path)
    {
        // 空pathには仮想pathを割り当てません。
        if (path.empty())
        {
            return {};
        }
        return "/assets/" + PortablePathToUtf8(path);
    }

    // glTF primitiveのattributeを探します(primitive: mesh primitive, type: attribute種別, index: attribute番号)。
    [[nodiscard]] const cgltf_accessor* FindModelAttribute(
        const cgltf_primitive& primitive,
        cgltf_attribute_type type,
        cgltf_int index = 0) noexcept
    {
        // primitiveのattributeを検索します(attributeIndex: 検索位置)。
        for (cgltf_size attributeIndex{};
             attributeIndex < primitive.attributes_count;
             ++attributeIndex)
        {
            // 検査中のglTF attribute
            const auto& attribute = primitive.attributes[attributeIndex];
            // 種別と番号が一致するattributeを返します。
            if (attribute.type == type && attribute.index == index)
            {
                return attribute.data;
            }
        }
        return nullptr;
    }

    // glTF accessorからfloat値を読みます(accessor: source, index: element番号, values: 出力先, count: 出力数)。
    [[nodiscard]] bool ReadModelFloat(
        const cgltf_accessor* accessor,
        cgltf_size index,
        float* values,
        cgltf_size count) noexcept
    {
        return accessor != nullptr
            && cgltf_accessor_read_float(
                accessor,
                index,
                values,
                count) != 0;
    }

    // glTF行列でpointを変換します(matrix: 4x4行列, value: 入力座標)。
    [[nodiscard]] DirectX::XMFLOAT3 TransformModelPoint(
        const float* matrix,
        const DirectX::XMFLOAT3& value) noexcept
    {
        return {
            matrix[0] * value.x + matrix[4] * value.y
                + matrix[8] * value.z + matrix[12],
            matrix[1] * value.x + matrix[5] * value.y
                + matrix[9] * value.z + matrix[13],
            matrix[2] * value.x + matrix[6] * value.y
                + matrix[10] * value.z + matrix[14],
        };
    }

    // glTF行列でnormalを変換します(matrix: 4x4行列, value: 入力normal)。
    [[nodiscard]] DirectX::XMFLOAT3 TransformModelNormal(
        const float* matrix,
        const DirectX::XMFLOAT3& value) noexcept
    {
        // 3x3部分行列のdeterminant
        const float determinant =
            matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6])
            - matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2])
            + matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
        // 逆行列を作れない場合は上向きnormalを返します。
        if (std::abs(determinant) <= 0.000001f)
        {
            return { 0.0f, 1.0f, 0.0f };
        }
        // 行列式の逆数
        const float inverse = 1.0f / determinant;
        // 逆転置行列で変換したnormal
        const Vec3 transformed{
            ((matrix[5] * matrix[10] - matrix[9] * matrix[6]) * value.x
                + (matrix[6] * matrix[8] - matrix[4] * matrix[10]) * value.y
                + (matrix[4] * matrix[9] - matrix[5] * matrix[8]) * value.z)
                * inverse,
            ((matrix[2] * matrix[9] - matrix[1] * matrix[10]) * value.x
                + (matrix[0] * matrix[10] - matrix[2] * matrix[8]) * value.y
                + (matrix[1] * matrix[8] - matrix[0] * matrix[9]) * value.z)
                * inverse,
            ((matrix[1] * matrix[6] - matrix[2] * matrix[5]) * value.x
                + (matrix[2] * matrix[4] - matrix[0] * matrix[6]) * value.y
                + (matrix[0] * matrix[5] - matrix[1] * matrix[4]) * value.z)
                * inverse,
        };
        return DirectXVector(LamaPon::Web::Normalize(transformed));
    }

    // 4x4行列の3x3 determinantを返します(matrix: 変換行列)。
    [[nodiscard]] float ModelTransformDeterminant(const float* matrix) noexcept
    {
        return matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6])
            - matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2])
            + matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
    }

    // float列をWeb行列へ変換します(values: 16要素の行列)。
    [[nodiscard]] Mat4 ModelMatrix(const std::array<float, 16>& values) noexcept
    {
        // Web行列形式の変換結果
        Mat4 result{};
        result.values = values;
        return result;
    }

    // Web行列からfloat列を返します(value: 変換行列)。
    [[nodiscard]] std::array<float, 16> ModelMatrix(const Mat4& value) noexcept
    {
        return value.values;
    }

    // Quaternionを単位長へ正規化します(value: 入力quaternion)。
    [[nodiscard]] DirectX::XMFLOAT4 NormalizeModelQuaternion(
        DirectX::XMFLOAT4 value) noexcept
    {
        // 入力quaternionの長さの二乗
        const float lengthSquared = value.x * value.x + value.y * value.y
            + value.z * value.z + value.w * value.w;
        // 零quaternionには単位quaternionを返します。
        if (lengthSquared <= 0.000001f)
        {
            return { 0.0f, 0.0f, 0.0f, 1.0f };
        }
        // 長さの逆数
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        value.x *= inverseLength;
        value.y *= inverseLength;
        value.z *= inverseLength;
        value.w *= inverseLength;
        return value;
    }

    // Quaternionを球面補間します(from: 開始値, to: 終了値, amount: 補間率)。
    [[nodiscard]] DirectX::XMFLOAT4 SlerpModelQuaternion(
        DirectX::XMFLOAT4 from,
        DirectX::XMFLOAT4 to,
        float amount) noexcept
    {
        from = NormalizeModelQuaternion(from);
        to = NormalizeModelQuaternion(to);
        // Quaternion間の内積
        float dot = from.x * to.x + from.y * to.y
            + from.z * to.z + from.w * to.w;
        // 最短回転側を選びます。
        if (dot < 0.0f)
        {
            dot = -dot;
            to = { -to.x, -to.y, -to.z, -to.w };
        }
        // 角度が小さい場合は線形補間を使います。
        if (dot > 0.9995f)
        {
            return NormalizeModelQuaternion({
                from.x + (to.x - from.x) * amount,
                from.y + (to.y - from.y) * amount,
                from.z + (to.z - from.z) * amount,
                from.w + (to.w - from.w) * amount,
            });
        }
        // Quaternion間の角度radian
        const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
        // 角度の正弦値
        const float sine = std::sin(angle);
        // 角度が退化した場合は開始値を返します。
        if (std::abs(sine) <= 0.000001f)
        {
            return from;
        }
        // 開始quaternionの補間係数
        const float fromWeight = std::sin((1.0f - amount) * angle) / sine;
        // 終了quaternionの補間係数
        const float toWeight = std::sin(amount * angle) / sine;
        return NormalizeModelQuaternion({
            from.x * fromWeight + to.x * toWeight,
            from.y * fromWeight + to.y * toWeight,
            from.z * fromWeight + to.z * toWeight,
            from.w * fromWeight + to.w * toWeight,
        });
    }

    // Quaternionからrotation matrixを作ります(value: 入力quaternion)。
    [[nodiscard]] Mat4 ModelQuaternionMatrix(
        DirectX::XMFLOAT4 value) noexcept
    {
        value = NormalizeModelQuaternion(value);
        // Quaternion各成分の二乗
        const float xx = value.x * value.x;
        // Quaternion各成分の二乗
        const float yy = value.y * value.y;
        // Quaternion各成分の二乗
        const float zz = value.z * value.z;
        // Quaternionのxy積
        const float xy = value.x * value.y;
        // Quaternionのxz積
        const float xz = value.x * value.z;
        // Quaternionのyz積
        const float yz = value.y * value.z;
        // Quaternionのwx積
        const float wx = value.w * value.x;
        // Quaternionのwy積
        const float wy = value.w * value.y;
        // Quaternionのwz積
        const float wz = value.w * value.z;
        // Identityを基準にしたrotation matrix
        Mat4 result = Mat4::Identity();
        result.values[0] = 1.0f - 2.0f * (yy + zz);
        result.values[1] = 2.0f * (xy + wz);
        result.values[2] = 2.0f * (xz - wy);
        result.values[4] = 2.0f * (xy - wz);
        result.values[5] = 1.0f - 2.0f * (xx + zz);
        result.values[6] = 2.0f * (yz + wx);
        result.values[8] = 2.0f * (xz + wy);
        result.values[9] = 2.0f * (yz - wx);
        result.values[10] = 1.0f - 2.0f * (xx + yy);
        return result;
    }

    // Translation・rotation・scaleを合成します(translation: 位置, rotation: 回転, scale: 拡大率)。
    [[nodiscard]] Mat4 ModelTrsMatrix(
        const DirectX::XMFLOAT3& translation,
        const DirectX::XMFLOAT4& rotation,
        const DirectX::XMFLOAT3& scale) noexcept
    {
        return LamaPon::Web::Multiply(
            LamaPon::Web::Translation(WebVector(translation)),
            LamaPon::Web::Multiply(
                ModelQuaternionMatrix(rotation),
                LamaPon::Web::Scale(WebVector(scale))));
    }

    // 2 vectorを線形補間します(from: 開始値, to: 終了値, amount: 補間率)。
    [[nodiscard]] DirectX::XMFLOAT4 LerpModelVector(
        const DirectX::XMFLOAT4& from,
        const DirectX::XMFLOAT4& to,
        float amount) noexcept
    {
        return {
            from.x + (to.x - from.x) * amount,
            from.y + (to.y - from.y) * amount,
            from.z + (to.z - from.z) * amount,
            from.w + (to.w - from.w) * amount,
        };
    }

    // 2 vectorをHermite補間します(from: 開始値, fromTangent: 開始接線, to: 終了値, toTangent: 終了接線, amount: 補間率, duration: 区間秒数)。
    [[nodiscard]] DirectX::XMFLOAT4 HermiteModelVector(
        const DirectX::XMFLOAT4& from,
        const DirectX::XMFLOAT4& fromTangent,
        const DirectX::XMFLOAT4& to,
        const DirectX::XMFLOAT4& toTangent,
        float amount,
        float duration) noexcept
    {
        // 補間率の二乗
        const float squared = amount * amount;
        // 補間率の三乗
        const float cubed = squared * amount;
        // 始点のHermite係数
        const float h00 = 2.0f * cubed - 3.0f * squared + 1.0f;
        // 始点接線のHermite係数
        const float h10 = cubed - 2.0f * squared + amount;
        // 終点のHermite係数
        const float h01 = -2.0f * cubed + 3.0f * squared;
        // 終点接線のHermite係数
        const float h11 = cubed - squared;
        return {
            h00 * from.x + h10 * duration * fromTangent.x
                + h01 * to.x + h11 * duration * toTangent.x,
            h00 * from.y + h10 * duration * fromTangent.y
                + h01 * to.y + h11 * duration * toTangent.y,
            h00 * from.z + h10 * duration * fromTangent.z
                + h01 * to.z + h11 * duration * toTangent.z,
            h00 * from.w + h10 * duration * fromTangent.w
                + h01 * to.w + h11 * duration * toTangent.w,
        };
    }

    using EncodedModelImages = std::unordered_map<const cgltf_image*,
        std::shared_ptr<const std::vector<unsigned char>>>;

    [[nodiscard]] std::filesystem::path ModelTexturePath(
        const std::filesystem::path& modelPath,
        const cgltf_texture_view& view,
        std::shared_ptr<const std::vector<unsigned char>>& encoded,
        EncodedModelImages& cache)
    {
        encoded.reset();
        if (!view.texture || !view.texture->image) return {};
        const auto* image = view.texture->image;
        if (const auto found = cache.find(image); found != cache.end())
        { encoded = found->second; return {}; }
        std::vector<unsigned char> bytes;
        if (image->buffer_view)
        {
            const auto& bufferView = *image->buffer_view;
            if (!image->mime_type || (std::string_view(image->mime_type) != "image/png"
                && std::string_view(image->mime_type) != "image/jpeg")
                || !bufferView.buffer || bufferView.offset > bufferView.buffer->size
                || bufferView.size > bufferView.buffer->size - bufferView.offset)
                throw std::runtime_error("Invalid embedded model image");
            const auto* data = cgltf_buffer_view_data(&bufferView);
            if (!data || !bufferView.size) throw std::runtime_error("Empty embedded model image");
            bytes.assign(data, data + bufferView.size);
        }
        else if (image->uri && std::string_view(image->uri).starts_with("data:"))
        {
            const std::string_view uri(image->uri);
            const auto comma = uri.find(',');
            if (comma == std::string_view::npos || (uri.substr(0, comma) != "data:image/png;base64"
                && uri.substr(0, comma) != "data:image/jpeg;base64"))
                throw std::runtime_error("Model image data URI must contain base64 PNG or JPEG");
            const auto payload = uri.substr(comma + 1);
            if (payload.empty() || payload.size() % 4)
                throw std::runtime_error("Invalid model image base64 length");
            std::size_t padding{};
            if (payload.back() == '=') ++padding;
            if (payload.size() > 1 && payload[payload.size() - 2] == '=') ++padding;
            for (std::size_t index = 0; index < payload.size() - padding; ++index)
            {
                const auto c = payload[index];
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                    || (c >= '0' && c <= '9') || c == '+' || c == '/'))
                    throw std::runtime_error("Invalid model image base64 character");
            }
            const auto size = payload.size() / 4 * 3 - padding;
            cgltf_options options{}; void* memory{};
            if (!size || cgltf_load_buffer_base64(&options, size, payload.data(), &memory) != cgltf_result_success)
                throw std::runtime_error("Cannot decode model image base64");
            struct Release { void* value; ~Release() { std::free(value); } } release{memory};
            const auto* first = static_cast<const unsigned char*>(memory);
            bytes.assign(first, first + size);
        }
        else
        {
            if (!image->uri) return {};
            std::string uri(image->uri);
            uri.resize(cgltf_decode_uri(uri.data()));
            return modelPath.parent_path() / PortablePathFromUtf8(uri);
        }
        encoded = std::make_shared<const std::vector<unsigned char>>(std::move(bytes));
        cache.emplace(image, encoded);
        return {};
    }

    // Web storageへtextを保存します(key: 項目名, value: 保存内容)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::SavePortableText;
#else
        EM_JS(int, SavePortableText,
          (const char* key, const char* value), {
        // DOM属性用のkey文字列
        const keyText = UTF8ToString(key);
        // DOM属性用の保存文字列
        const valueText = UTF8ToString(value);
        try {
            localStorage.setItem(
                "lamapon.portable." + keyText,
                valueText);
        }
        catch (error) {
            if (document.body) document.body.dataset.lamaponSaveError = String(error);
            return 0;
        }
        // DOMがある場合は保存結果を公開します。
        if (document.body) {
            document.body.dataset.lamaponSavedKey = keyText;
            document.body.dataset.lamaponSavedValue = valueText;
            delete document.body.dataset.lamaponSaveError;
        }
        return 1;
    });
#endif

    // Web storageのtextをmalloc領域で返します(key: 項目名)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::LoadPortableText;
#else
        EM_JS(char*, LoadPortableText, (const char* key), {
        // UTF8文字列をmalloc領域へ変換します(value: 入力文字列)。
        const allocateUtf8 = value => {
            // UTF8 buffer長と終端文字
            const length = lengthBytesUTF8(value) + 1;
            // 呼び出し側へ返すmalloc buffer
            const result = _malloc(length);
            stringToUTF8(value, result, length);
            return result;
        };
        // Web storageから対象textを読みます。
        try {
            // nullは未登録、空文字列は保存済みの値です。
            const value = localStorage.getItem(
                "lamapon.portable." + UTF8ToString(key));
            return value === null ? 0 : allocateUtf8(value);
        }
        // Storage拒否時は空textを返します(error: browser例外)。
        catch (error) {
            return 0;
        }
    });
#endif

    // Asset textをmalloc領域で返します(path: 仮想path)。返却領域は呼び出し側がfreeします。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::LoadPortableAssetText;
#else
        EM_JS(char*, LoadPortableAssetText, (const char* path), {
        // Virtual filesystemからasset textを読みます。
        try {
            // UTF8形式のasset text
            const value = FS.readFile(
                UTF8ToString(path), { encoding: "utf8" });
            // UTF8 buffer長と終端文字
            const length = lengthBytesUTF8(value) + 1;
            // 呼び出し側へ返すmalloc buffer
            const result = _malloc(length);
            stringToUTF8(value, result, length);
            return result;
        }
        // 読み込み失敗はnull相当を返します(error: browser例外)。
        catch (error) {
            // DOMへasset失敗の診断情報を残します。
            if (document.body) {
                document.body.dataset.lamaponAssetError = String(error);
                document.body.dataset.lamaponAssetPath = UTF8ToString(path);
            }
            return 0;
        }
    });
#endif

    // Asset byte列をmalloc領域で返します(path: 仮想path, byteCount: byte数出力)。返却領域は呼び出し側がfreeします。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::LoadPortableAssetBytes;
#else
        EM_JS(unsigned char*, LoadPortableAssetBytes,
          (const char* path, std::uint32_t* byteCount), {
        // Virtual filesystemからasset byte列を読みます。
        try {
            // 読み込んだasset byte列
            const bytes = FS.readFile(UTF8ToString(path));
            // 呼び出し側へ返すmalloc buffer
            const result = _malloc(bytes.length);
            HEAPU8.set(bytes, result);
            HEAPU32[byteCount >> 2] = bytes.length;
            return result;
        }
        // 読み込み失敗時はbyte数を0にします(error: browser例外)。
        catch (error) {
            HEAPU32[byteCount >> 2] = 0;
            // model読込失敗をDOMへ通知します。
            if (document.body) {
                document.body.dataset.lamaponModelError = "asset-read";
                document.body.dataset.lamaponModelPath = UTF8ToString(path);
            }
            return 0;
        }
    });
#endif

    // Model load状態をDOMへ公開します(path: asset path, status: 状態, parts: part数)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::PublishPortableModelStatus;
#else
        EM_JS(void, PublishPortableModelStatus,
          (const char* path, const char* status, int parts), {
        // bodyがない場合は診断値を記録できません。
        if (!document.body) return;
        // UTF8状態名
        const statusText = UTF8ToString(status);
        document.body.dataset.lamaponModelPath = UTF8ToString(path);
        document.body.dataset.lamaponModelStatus = statusText;
        document.body.dataset.lamaponModelParts = String(parts);
        // 失敗状態だけerror属性へ残します。
        if (statusText !== "loaded") {
            document.body.dataset.lamaponModelError = statusText;
        } else {
            delete document.body.dataset.lamaponModelError;
        }
    });
#endif

    // Model animation状態をDOMへ公開します(name: 名称, index: 番号, count: 総数, time: 秒, playing: 再生状態)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::PublishPortableModelAnimation;
#else
        EM_JS(void, PublishPortableModelAnimation,
          (const char* name, int index, int count, float time, int playing), {
        // bodyがない場合は状態を公開できません。
        if (!document.body) return;
        document.body.dataset.lamaponModelAnimation = UTF8ToString(name);
        document.body.dataset.lamaponModelAnimationIndex = String(index);
        document.body.dataset.lamaponModelAnimationCount = String(count);
        document.body.dataset.lamaponModelAnimationTime = time.toFixed(4);
        document.body.dataset.lamaponModelAnimationPlaying = playing ? "1" : "0";
    });
#endif

    // Virtual asset JSONを読みます(assetPath: source上のpath, document: JSON出力先)。
    [[nodiscard]] bool LoadPortableJsonDocument(
        const std::filesystem::path& assetPath,
        Json& document)
    {
        // Web上のvirtual asset path
        const std::string path = VirtualAssetPath(assetPath);
        // JS bridgeが確保したJSON文字列
        char* loaded = LoadPortableAssetText(path.c_str());
        // assetを読み込めない場合は失敗を返します。
        if (loaded == nullptr)
        {
            return false;
        }
        // JSON parseに失敗してもruntimeを継続します。
        try
        {
            document = Json::parse(loaded);
        }
        // 不正なJSONを拒否してbridge bufferを解放します。
        catch (const Json::exception&)
        {
            std::free(loaded);
            return false;
        }
        std::free(loaded);
        return document.is_object();
    }

    // 登録済みaction数をDOMへ公開します(count: action数)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::PublishPortableInputActionCount;
#else
        EM_JS(void, PublishPortableInputActionCount, (int count), {
        // bodyがない場合は件数を公開できません。
        if (document.body) {
            document.body.dataset.lamaponInputActions = String(count);
        }
    });
#endif

    // Projectのinput action定義をScript bindingへ読み込みます。
    void LoadPortableInputBindings()
    {
        // Script別binding table
        auto& bindings = PortableInputBindings();
        bindings.clear();
        PublishPortableInputActionCount(0);
        // asset読込時にbridgeが確保したJSON文字列
        char* loaded = LoadPortableAssetText(
            "/assets/lamapon-input-actions.json");
        // 定義assetがない場合はbindingなしで続行します。
        if (loaded == nullptr)
        {
            return;
        }
        // 読み込んだaction定義
        Json document;
        // JSON parse failureを処理します。
        try
        {
            document = Json::parse(loaded);
        }
        // 不正な定義を読み飛ばします。
        catch (const Json::exception&)
        {
            std::free(loaded);
            return;
        }
        std::free(loaded);
        // action名からbinding値へのJSON object
        const auto actions = document.value("actions", Json::object());
        // actionsがobjectでない場合は何も登録しません。
        if (!actions.is_object())
        {
            return;
        }
        // Actionを順に登録します(name: action名, values: binding配列)。
        for (const auto& [name, values] : actions.items())
        {
            // binding配列がないactionは無視します。
            if (!values.is_array())
            {
                continue;
            }
            // 当該actionに登録するbinding列
            auto& target = bindings[name];
            // 各binding定義を検査します(value: binding object)。
            for (const auto& value : values)
            {
                // object以外のbindingを無視します。
                if (!value.is_object())
                {
                    continue;
                }
                // bindingが参照するinput control
                const std::string control = value.value("control", "");
                // control入力へ適用する倍率
                const float scale = value.value("scale", 1.0f);
                // 有効なcontrolと有限倍率だけ登録します。
                if (!control.empty() && std::isfinite(scale))
                {
                    target.push_back({ control, scale });
                }
            }
        }
        PublishPortableInputActionCount(static_cast<int>(bindings.size()));
    }

    // Portable UI textをDOM描画します(objectName: object名, objectId: object ID, text: 本文, font: font名, fontAsset: font path, size: 文字高, r: 赤, g: 緑, b: 青, a: 不透明度, x: 左位置, y: 上位置, width: 幅, height: 高さ, wordWrap: 折返し, horizontal: 横揃え, vertical: 縦揃え, sortOrder: 描画順)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::RenderPortableText;
#else
        EM_JS(void, RenderPortableText,
          (const char* objectName, double objectId,
           const char* text, const char* font, const char* fontAsset,
           float size, float r, float g, float b, float a,
           float x, float y, float width, float height,
           int wordWrap, int horizontal, int vertical, int sortOrder), {
        // DOM表示へ使うobject名
        const name = UTF8ToString(objectName);
        // 固定HUD名と既存DOM IDの対応
        const ids = {
            "HUD Time": "hud-time", "HUD Best": "hud-best",
            "HUD Gear": "hud-gear", "HUD Speed": "hud-speed",
            "HUD Speed Unit": "hud-speed-unit", "HUD Message": "hud-message"
        };
        // 固定HUD用のDOM ID
        const nativeId = ids[name] || "";
        // 固定HUD要素または新規DOM要素
        let element = nativeId ? document.getElementById(nativeId) : null;
        // 固定HUD要素かどうか
        const nativeElement = Boolean(element);
        // 固定HUDにないtextをPortable layerへ追加します。
        if (!element) {
            // object IDで一意にしたPortable UI要素ID
            const portableId = "lamapon-portable-text-"
                + String(Math.floor(objectId));
            element = document.getElementById(portableId);
            // 初回描画時だけDOM nodeを作ります。
            if (!element) {
            // HUD layerがなければbodyを使います。
            const layer = document.getElementById("hud") || document.body;
            element = document.createElement("div");
            element.id = portableId;
            element.dataset.lamaponPortableUi = name;
            element.style.position = "absolute";
            element.style.pointerEvents = "none";
            layer.appendChild(element);
            }
        }
        element.dataset.lamaponPortableFrame = String(
            document.body?.__lamaponPortableFrame || 0);
        element.textContent = UTF8ToString(text);
        // CSSへ設定するfont family
        const fontFamily = UTF8ToString(font);
        // Virtual filesystem上のfont path
        const fontPath = UTF8ToString(fontAsset);
        // 独自font assetがあれば読み込みます。
        if (fontPath) {
            globalThis.__lamaponPortableFonts ||= {};
            // 1つのfont pathを一度だけ読み込みます。
            if (!globalThis.__lamaponPortableFonts[fontPath]) {
                globalThis.__lamaponPortableFonts[fontPath] = "loading";
                // Font assetをbrowser FontFaceへ登録します。
                try {
                    // Virtual filesystem上のfont byte列
                    const bytes = FS.readFile(fontPath);
                    // Font assetの拡張子
                    const extension = fontPath.split(".").pop().toLowerCase();
                    // 拡張子から選ぶMIME type
                    const mime = extension === "woff2" ? "font/woff2"
                        : extension === "woff" ? "font/woff"
                        : extension === "otf" ? "font/otf" : "font/ttf";
                    // Browserで共有する一時font URL
                    const objectUrl = URL.createObjectURL(
                        new Blob([bytes], { type: mime }));
                    // Font loading request
                    const face = new FontFace(fontFamily, "url(" + objectUrl + ")");
                    // Load完了後にfontをdocumentへ登録します(loaded: 読み込み済みfont)。
                    face.load().then(loaded => {
                        document.fonts.add(loaded);
                        globalThis.__lamaponPortableFonts[fontPath] = "loaded";
                        // Font load成功をDOMへ公開します。
                        if (document.body) {
                            document.body.dataset.lamaponFontAsset = "loaded";
                            document.body.dataset.lamaponFontAssetPath = fontPath;
                        }
                    })
                    // Font loading failureをDOMへ記録します(error: browser例外)。
                    .catch(error => {
                        globalThis.__lamaponPortableFonts[fontPath] = "error";
                        // Font load失敗をDOMへ公開します。
                        if (document.body) {
                            document.body.dataset.lamaponFontAsset = "error";
                            document.body.dataset.lamaponFontAssetPath = fontPath;
                        }
                        console.warn("LamaPon Web font load failed", fontPath, error);
                    })
                    // Load完了後に一時URLを解放します。
                    .finally(() => URL.revokeObjectURL(objectUrl));
                }
                // Font fileが読めない場合は標準fontを使います(error: browser例外)。
                catch (error) {
                    globalThis.__lamaponPortableFonts[fontPath] = "error";
                    console.warn("LamaPon Web font unavailable", fontPath, error);
                }
            }
        }
        // 固定HUD要素には既定のlayoutを保ったまま色だけ反映します。
        if (nativeElement) {
            element.style.color = "rgba(" + (r*255) + "," + (g*255) + "," +
                (b*255) + "," + a + ")";
            // 空HUD messageを隠します。
            if (name === "HUD Message") element.style.opacity = text ? "1" : "0";
            return;
        }
        Object.assign(element.style, {
            left: x + "px", top: y + "px", width: width + "px",
            height: height + "px", fontFamily,
            fontSize: size + "px", color: "rgba(" + (r*255) + "," +
                (g*255) + "," + (b*255) + "," + a + ")",
            textAlign: horizontal === 1 ? "center" : horizontal === 2 ? "right" : "left",
            justifyContent: horizontal === 1 ? "center" : horizontal === 2 ? "flex-end" : "flex-start",
            alignItems: vertical === 1 ? "center" : vertical === 2 ? "flex-end" : "flex-start",
            whiteSpace: wordWrap ? "pre-wrap" : "pre",
            overflowWrap: wordWrap ? "anywhere" : "normal",
            zIndex: String(sortOrder), display: "flex"
        });
        // 空HUD messageを隠します。
        if (name === "HUD Message") element.style.opacity = text ? "1" : "0";
    });
#endif

    // Portable sprite maskをDOM stateへ記録します(objectId: object ID, x/y: 位置, width/height: 範囲, shape: 形状)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::RenderPortableMask;
#else
        EM_JS(void, RenderPortableMask,
          (double objectId, float x, float y, float width, float height,
           int shape), {
        // bodyがない場合はmaskを記録できません。
        if (!document.body) return;
        // frame内のmask一覧を用意します。
        document.body.__lamaponPortableMasks ||= {};
        document.body.__lamaponPortableMasks[String(Math.floor(objectId))] = {
            x, y, width, height, shape
        };
    });
#endif

    // Portable UI frameを開始します。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::BeginPortableUiFrame;
#else
        EM_JS(void, BeginPortableUiFrame, (), {
        // bodyがない場合はframe stateを更新できません。
        if (!document.body) return;
        document.body.__lamaponPortableFrame =
            (document.body.__lamaponPortableFrame || 0) + 1;
        document.body.__lamaponPortableMasks = {};
    });
#endif

    // 未更新のPortable UI要素を隠してframeを終了します。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::EndPortableUiFrame;
#else
        EM_JS(void, EndPortableUiFrame, (), {
        // bodyがない場合はframeを終了できません。
        if (!document.body) return;
        // 現在のPortable UI frame番号
        const frame = String(document.body.__lamaponPortableFrame || 0);
        // 今frameに描画されなかった要素を調べます(element: 検査中のUI要素)。
        for (const element of document.querySelectorAll(
                "[data-lamapon-portable-ui]")) {
            // 古いframeの表示要素を隠します。
            if (element.dataset.lamaponPortableFrame !== frame
                && element.style.display !== "none") {
                element.style.display = "none";
            }
        }
    });
#endif

    // ObjectのPortable UI表示を隠します(objectId: 対象ID)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::HidePortableObjectUi;
#else
        EM_JS(void, HidePortableObjectUi, (double objectId), {
        // DOM検索に使うobject ID
        const id = String(Math.floor(objectId));
        // 対象objectのUI prefixを調べます(prefix: UI種別prefix)。
        for (const prefix of [
                "lamapon-portable-text-",
                "lamapon-portable-sprite-"]) {
            // prefixとIDで特定したDOM element
            const element = document.getElementById(prefix + id);
            // 既存要素だけを隠します。
            if (element) {
                element.style.display = "none";
                element.dataset.lamaponPortableFrame = "hidden";
            }
        }
    });
#endif

    // Portable UI spriteをDOM描画します(objectName/objectId: 対象, texturePath: texture, r/g/b/a: 色, x/y: 中心位置, width/height: 寸法, pivotX/pivotY: 基準点, rotation: radian, sortOrder: 描画順, sourceX/sourceY/sourceWidth/sourceHeight: UV範囲, maskInteraction: mask設定)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::RenderPortableSprite;
#else
        EM_JS(void, RenderPortableSprite,
          (const char* objectName, double objectId, const char* texturePath,
           float r, float g, float b, float a, float x, float y,
           float width, float height, float pivotX, float pivotY,
           float rotation, int sortOrder,
           float sourceX, float sourceY, float sourceWidth, float sourceHeight,
           int maskInteraction), {
        // DOM表示へ使うobject名
        const name = UTF8ToString(objectName);
        // 固定HUD spriteまたはPortable UI要素
        let element = name === "HUD Tacho"
            ? document.getElementById("tacho-face")
            : name === "HUD Needle"
                ? document.getElementById("tacho-needle") : null;
        // 固定HUD要素かどうか
        const nativeElement = Boolean(element);
        // 固定HUDにないspriteをPortable layerへ追加します。
        if (!element) {
            // object IDで一意にしたPortable UI要素ID
            const portableId = "lamapon-portable-sprite-"
                + String(Math.floor(objectId));
            element = document.getElementById(portableId);
            // 初回描画時だけDOM nodeを作ります。
            if (!element) {
            // HUD layerがなければbodyを使います。
            const layer = document.getElementById("hud") || document.body;
            element = document.createElement("div");
            element.id = portableId;
            element.dataset.lamaponPortableUi = name;
            element.style.position = "absolute";
            element.style.pointerEvents = "none";
            layer.appendChild(element);
            }
        }
        // Portable UI要素のframe番号を更新します。
        if (!nativeElement) {
            element.dataset.lamaponPortableFrame = String(
                document.body?.__lamaponPortableFrame || 0);
        }
        // 固定needleは回転だけ更新します。
        if (nativeElement && name === "HUD Needle") {
            element.style.transform = "rotate(" + rotation + "rad)";
            return;
        }
        // DOMへ設定するtexture path
        const path = UTF8ToString(texturePath);
        // pivot反映後の左端位置
        const left = x - width * pivotX;
        // pivot反映後の上端位置
        const top = y - height * pivotY;
        // 零幅UVの除算を防ぐ幅
        const safeSourceWidth = Math.max(0.000001, sourceWidth);
        // 零高UVの除算を防ぐ高さ
        const safeSourceHeight = Math.max(0.000001, sourceHeight);
        // texture内UVの横offset
        const backgroundX = safeSourceWidth >= 0.999999
            ? 0 : sourceX / (1 - safeSourceWidth) * 100;
        // texture内UVの縦offset
        const backgroundY = safeSourceHeight >= 0.999999
            ? 0 : sourceY / (1 - safeSourceHeight) * 100;
        // 固定HUD以外のstyleを更新します。
        if (!nativeElement) Object.assign(element.style, {
            left: left + "px",
            top: top + "px",
            width: width + "px", height: height + "px",
            backgroundColor: path ? "transparent" : "rgba(" + (r*255) + "," +
                (g*255) + "," + (b*255) + "," + a + ")",
            backgroundSize: (100 / safeSourceWidth) + "% "
                + (100 / safeSourceHeight) + "%",
            backgroundPosition: backgroundX + "% " + backgroundY + "%",
            backgroundRepeat: "no-repeat",
            opacity: path ? String(a) : "1",
            transformOrigin: (pivotX*100) + "% " + (pivotY*100) + "%",
            transform: "rotate(" + rotation + "rad)", zIndex: String(sortOrder),
            display: "block"
        });
        // TextureのRGBへ指定色を乗算し、alphaはopacityで適用します。
        // filterはspriteの子として保持し、同じobjectの描画で再利用します。
        if (!nativeElement) {
            if (path && (r !== 1 || g !== 1 || b !== 1)) {
                if (!element.__lamaponTint) {
                    const ns = "http://www.w3.org/2000/svg";
                    const svg = document.createElementNS(ns, "svg");
                    svg.setAttribute("width", "0");
                    svg.setAttribute("height", "0");
                    svg.style.position = "absolute";
                    const filter = document.createElementNS(ns, "filter");
                    filter.id = element.id + "-tint";
                    filter.setAttribute("color-interpolation-filters", "sRGB");
                    const transfer = document.createElementNS(ns, "feComponentTransfer");
                    const channels = ["R", "G", "B"].map(channel => {
                        const node = document.createElementNS(ns, "feFunc" + channel);
                        node.setAttribute("type", "linear");
                        transfer.appendChild(node);
                        return node;
                    });
                    filter.appendChild(transfer);
                    svg.appendChild(filter);
                    element.appendChild(svg);
                    element.__lamaponTint = { filter, channels };
                }
                const tint = element.__lamaponTint;
                [r, g, b].forEach((value, index) =>
                    tint.channels[index].setAttribute("slope", String(value)));
                element.style.filter = "url(#" + tint.filter.id + ")";
            } else element.style.filter = "";
            if (!path) {
                element.style.backgroundImage = "";
                delete element.dataset.lamaponTextureLoaded;
            }
        }
        element.style.clipPath = "";
        element.style.maskImage = "";
        element.style.webkitMaskImage = "";
        // mask interactionが有効な場合は近いmaskを適用します。
        if (maskInteraction !== 0 && document.body) {
            // 現frameで登録されたmask一覧
            const masks = Object.values(document.body.__lamaponPortableMasks || {});
            // spriteに最も近いmask
            let nearest = null;
            // 最短mask距離の二乗
            let nearestDistance = Number.POSITIVE_INFINITY;
            // 最近傍maskを探します(mask: 登録済みmask)。
            for (const mask of masks) {
                // sprite中心とmask中心の距離二乗
                const distance = (mask.x - x) ** 2 + (mask.y - y) ** 2;
                // 最短距離だけ保持します。
                if (distance < nearestDistance) {
                    nearest = mask;
                    nearestDistance = distance;
                }
            }
            // 最寄りmaskと交差方式が有効な場合にclipします。
            if (nearest && maskInteraction === 1) {
                // Circle maskでは円形clipを使います。
                if (nearest.shape === 1) {
                    // Circle maskの半径
                    const radius = Math.min(nearest.width, nearest.height) * 0.5;
                    element.style.clipPath = "circle(" + radius + "px at "
                        + (nearest.x - left) + "px "
                        + (nearest.y - top) + "px)";
                }
                // Rectangle maskでは四辺のinsetを使います。
                else {
                    // Mask矩形の左上座標
                    const maskLeft = nearest.x - nearest.width * 0.5;
                    // Mask矩形の上端座標
                    const maskTop = nearest.y - nearest.height * 0.5;
                    // sprite上端からmask上端までの余白
                    const insetTop = Math.max(0, maskTop - top);
                    // sprite左端からmask左端までの余白
                    const insetLeft = Math.max(0, maskLeft - left);
                    // mask右端からsprite右端までの余白
                    const insetRight = Math.max(
                        0, left + width - maskLeft - nearest.width);
                    // mask下端からsprite下端までの余白
                    const insetBottom = Math.max(
                        0, top + height - maskTop - nearest.height);
                    element.style.clipPath = "inset(" + insetTop + "px "
                        + insetRight + "px " + insetBottom + "px "
                        + insetLeft + "px)";
                }
            }
        }
        // Textureが未読込ならvirtual filesystemから取得します。
        if (path && globalThis.FS && element.dataset.lamaponTextureLoaded !== path) {
            // Asset読込・Data URL変換の失敗を握り潰します。
            try {
                // Virtual filesystem上のtexture byte列
                const bytes = FS.readFile(path);
                // Base64変換用のbyte string
                let binary = "";
                // 巨大spreadを避けてbyte列を小分けにします(i: byte offset)。
                for (let i = 0; i < bytes.length; i += 0x8000) {
                    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
                }
                // Header byteからimage MIME typeを判定します。
                const imageMime = bytes.length >= 12
                    && bytes[0] === 0x52 && bytes[1] === 0x49
                    && bytes[2] === 0x46 && bytes[3] === 0x46
                    && bytes[8] === 0x57 && bytes[9] === 0x45
                    && bytes[10] === 0x42 && bytes[11] === 0x50
                        ? "image/webp"
                        : bytes.length >= 2
                            && bytes[0] === 0xff && bytes[1] === 0xd8
                                ? "image/jpeg"
                                : "image/png";
                // DOMへ渡すbase64 Data URL
                const url = "data:" + imageMime + ";base64," + btoa(binary);
                // Image nodeならsrcへtextureを設定します。
                if (element.tagName === "IMG") element.src = url;
                // その他はbackground imageとして設定します。
                else element.style.backgroundImage = "url(" + url + ")";
                element.dataset.lamaponTextureLoaded = path;
            }
            // texture不在時は無地spriteを表示します(error: browser例外)。
            catch (error) {}
        }
    });
#endif

    // 数値stateをDOMとJS mapへ公開します(key: state名, value: 数値)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::PublishPortableNumber;
#else
        EM_JS(void, PublishPortableNumber, (const char* key, double value), {
        // bodyがない場合はstateを公開できません。
        if (!document.body) return;
        // HTML attribute用のstate名
        const name = UTF8ToString(key).replaceAll("_", "-");
        document.body.setAttribute(
            "data-lamapon-state-" + name,
            String(value));
        document.body.__lamaponPortableState ||= {};
        document.body.__lamaponPortableState[UTF8ToString(key)] = value;
    });
#endif

    // 文字列stateをDOMとJS mapへ公開します(key: state名, value: 文字列)。
    #if defined(LAMAPON_NATIVE_RUNTIME)
        using LamaPon::Native::PublishPortableString;
#else
        EM_JS(void, PublishPortableString,
          (const char* key, const char* value), {
        // bodyがない場合はstateを公開できません。
        if (!document.body) return;
        // HTML attribute用のstate名
        const name = UTF8ToString(key).replaceAll("_", "-");
        // UTF8 state value
        const text = UTF8ToString(value);
        document.body.setAttribute("data-lamapon-state-" + name, text);
        document.body.__lamaponPortableState ||= {};
        document.body.__lamaponPortableState[UTF8ToString(key)] = text;
    });
#endif
}

namespace LamaPon
{
    struct Scene::Impl final
    {
        struct ContactKey final
        {
            // Contact pairの小さい側ID
            GameObjectId first{};
            // Contact pairの大きい側ID
            GameObjectId second{};

            bool operator==(const ContactKey&) const noexcept = default;
        };

        struct ContactHash final
        {
            // Contact pairのhash値を作ります(value: pair key)。
            std::size_t operator()(const ContactKey& value) const noexcept
            {
                return std::hash<GameObjectId>{}(value.first)
                    ^ (std::hash<GameObjectId>{}(value.second)
                        + 0x9e3779b9u
                        + (std::hash<GameObjectId>{}(value.first) << 6u)
                        + (std::hash<GameObjectId>{}(value.first) >> 2u));
            }
        };

        // Scene clear color
        DirectX::XMFLOAT4 clearColor{ 0.72f, 0.62f, 0.52f, 1.0f };
        // Main camera object
        GameObject* mainCamera{};
        // 現frameのcontactとtrigger種別
        std::unordered_map<ContactKey, bool, ContactHash> contacts;
        GameObjectId focusedButtonOwner{};
        UIButtonComponent* focusedButton{};
    };

    // 数値stateを更新します(key: state名, value: 数値)。
    void RuntimeState::SetNumber(std::string key, double value)
    {
        PublishPortableNumber(key.c_str(), value);
        m_numbers[std::move(key)] = value;
    }

    // 整数stateを更新します(key: state名, value: 整数)。
    void RuntimeState::SetInteger(std::string key, std::int64_t value)
    {
        PublishPortableNumber(key.c_str(), static_cast<double>(value));
        m_integers[std::move(key)] = value;
    }

    // 真偽stateを更新します(key: state名, value: 状態)。
    void RuntimeState::SetBoolean(std::string key, bool value)
    {
        PublishPortableNumber(key.c_str(), value ? 1.0 : 0.0);
        m_booleans[std::move(key)] = value;
    }

    // 文字列stateを更新します(key: state名, value: 文字列)。
    void RuntimeState::SetString(std::string key, std::string value)
    {
        PublishPortableString(key.c_str(), value.c_str());
        m_strings[std::move(key)] = std::move(value);
    }

    // 数値stateを返します(key: state名, fallback: 未登録時の値)。
    double RuntimeState::Number(std::string_view key, double fallback) const
    {
        // 該当stateの検索結果
        const auto found = m_numbers.find(std::string(key));
        return found != m_numbers.end() ? found->second : fallback;
    }

    // 整数stateを返します(key: state名, fallback: 未登録時の値)。
    std::int64_t RuntimeState::Integer(
        std::string_view key,
        std::int64_t fallback) const
    {
        // 該当stateの検索結果
        const auto found = m_integers.find(std::string(key));
        return found != m_integers.end() ? found->second : fallback;
    }

    // 真偽stateを返します(key: state名, fallback: 未登録時の値)。
    bool RuntimeState::Boolean(std::string_view key, bool fallback) const
    {
        // 該当stateの検索結果
        const auto found = m_booleans.find(std::string(key));
        return found != m_booleans.end() ? found->second : fallback;
    }

    // 文字列stateを返します(key: state名, fallback: 未登録時の値)。
    std::string RuntimeState::String(
        std::string_view key,
        std::string fallback) const
    {
        // 該当stateの検索結果
        const auto found = m_strings.find(std::string(key));
        return found != m_strings.end() ? found->second : std::move(fallback);
    }

    // Input actionの値を返します(action: action名)。
    float InputSystem::Value(std::string_view action) const
    {
        // Input deviceが未初期化なら0を返します。
        if (m_input == nullptr)
        {
            return 0.0f;
        }
        // 登録済みaction bindingを優先します(configured: 該当binding)。
        if (const auto configured = PortableInputBindings().find(
                std::string(action));
            configured != PortableInputBindings().end()
            && !configured->second.empty())
        {
            // bindingとtouch axisを合成したaction値
            float mapped{};
            // Action bindingを順に評価します(binding: 対象binding)。
            for (const auto& binding : configured->second)
            {
                // binding controlの入力値
                float value = m_input->ControlValue(binding.control);
                // 短いpress edgeはsimulation frameまで保持します。
                if (value == 0.0f
                    && m_input->ControlWasPressed(binding.control))
                {
                    value = 1.0f;
                }
                mapped += value * binding.scale;
            }
            // Horizontal actionへtouch axisを加えます。
            if (action == "MoveHorizontal")
            {
                mapped += m_input->TouchHorizontalAxis();
            }
            // Vertical actionへtouch axisを加えます。
            else if (action == "MoveVertical")
            {
                mapped += m_input->TouchVerticalAxis();
            }
            // Accelerate actionは最大値を使います。
            else if (action == "Accelerate")
            {
                mapped = std::max(mapped, m_input->TouchAccelerateAxis());
            }
            // Brake actionは最大値を使います。
            else if (action == "Brake")
            {
                mapped = std::max(mapped, m_input->TouchBrakeAxis());
            }
            return std::clamp(mapped, -1.0f, 1.0f);
        }
        // Keyまたはtouch buttonのdown値を返します(code: 検査するinput code)。
        const auto down = [this](const char* code)
        {
            return m_input->IsDown(code) || m_input->WasPressed(code)
                ? 1.0f : 0.0f;
        };
        // Arrow・WASD・axisを横移動値へ合成します。
        if (action == "MoveHorizontal")
        {
            return std::clamp(
                down("KeyD") + down("ArrowRight")
                    - down("KeyA") - down("ArrowLeft")
                    + m_input->HorizontalAxis(),
                -1.0f, 1.0f);
        }
        // Arrow・WASD・axisを縦移動値へ合成します。
        if (action == "MoveVertical")
        {
            return std::clamp(
                down("KeyW") + down("ArrowUp")
                    - down("KeyS") - down("ArrowDown")
                    + m_input->VerticalAxis(),
                -1.0f, 1.0f);
        }
        // 横視点入力はhorizontal axisを返します。
        if (action == "LookHorizontal")
        {
            return m_input->HorizontalAxis();
        }
        // 縦視点入力はvertical axisを返します。
        if (action == "LookVertical")
        {
            return m_input->VerticalAxis();
        }
        // 加速入力を返します。
        if (action == "Accelerate")
        {
            return std::max({
                down("KeyW"),
                down("ArrowUp"),
                m_input->AccelerateAxis(),
                std::max(m_input->VerticalAxis(), 0.0f),
            });
        }
        // 制動入力を返します。
        if (action == "Brake")
        {
            return std::max({
                down("KeyS"),
                down("ArrowDown"),
                m_input->BrakeAxis(),
                std::max(-m_input->VerticalAxis(), 0.0f),
            });
        }
        return 0.0f;
    }

    // 操作の押下状態を返します(action: 操作名)
    bool InputSystem::WasPressed(std::string_view action) const
    {
        // 入力系統が無効なら押下扱いにしません。
        if (m_input == nullptr || !m_edgeEventsEnabled)
        {
            return false;
        }
        // 明示設定がある操作は割り当てだけを評価します。
        if (const auto configured = PortableInputBindings().find(
                std::string(action));
            configured != PortableInputBindings().end()
            && !configured->second.empty())
        {
            // タッチ専用の視点切替も押下として扱います。
            if (action == "ToggleView"
                && m_input->WasTouchToggleViewPressed())
            {
                return true;
            }
            return std::ranges::any_of(
                configured->second,
                // 割り当てた入力の押下を確認します(binding: 入力割り当て)
                [this](const PortableInputBinding& binding)
                {
                    return m_input->ControlWasPressed(binding.control);
                });
        }
        // UI方向は十字キー、左スティックまたは矢印キー。明示設定を優先します。
        if (action == "UIUp") return m_input->WasPressed("ArrowUp") || m_input->WasGamepadPressed(12) || m_input->WasGamepadNavigationPressed(0);
        if (action == "UIDown") return m_input->WasPressed("ArrowDown") || m_input->WasGamepadPressed(13) || m_input->WasGamepadNavigationPressed(1);
        if (action == "UILeft") return m_input->WasPressed("ArrowLeft") || m_input->WasGamepadPressed(14) || m_input->WasGamepadNavigationPressed(2);
        if (action == "UIRight") return m_input->WasPressed("ArrowRight") || m_input->WasGamepadPressed(15) || m_input->WasGamepadNavigationPressed(3);
        if (action == "UINext" || action == "UIPrevious")
        {
            const bool shift = m_input->IsDown("ShiftLeft") || m_input->IsDown("ShiftRight");
            return m_input->WasPressed("Tab") && shift == (action == "UIPrevious");
        }
        // 再起動の既定キーを確認します。
        if (action == "Restart")
        {
            return m_input->WasPressed("KeyR")
                || m_input->WasGamepadPressed(9);
        }
        // 視点切替の既定入力を確認します。
        if (action == "ToggleView")
        {
            return m_input->WasPressed("KeyC")
                || m_input->WasGamepadPressed(3)
                || m_input->WasTouchToggleViewPressed();
        }
        // ジャンプと決定の既定入力を共有します。
        if (action == "Jump" || action == "Submit")
        {
            return m_input->WasPressed("Space")
                || m_input->WasPressed("Enter")
                || m_input->WasGamepadPressed(0);
        }
        // キャンセルの既定入力を確認します。
        if (action == "Cancel")
        {
            return m_input->WasPressed("Escape")
                || m_input->WasGamepadPressed(1);
        }
        // 一時停止の既定入力を確認します。
        if (action == "Pause")
        {
            return m_input->WasPressed("Escape")
                || m_input->WasPressed("KeyP")
                || m_input->WasGamepadPressed(9);
        }
        return false;
    }

    // 操作の解放状態を返します(action: 操作名, threshold: 未使用閾値)
    bool InputSystem::WasReleased(
        std::string_view action,
        float threshold) const
    {
        // API互換のため閾値引数を保持します。
        (void)threshold;
        // 入力系統が無効なら解放扱いにしません。
        if (m_input == nullptr || !m_edgeEventsEnabled)
        {
            return false;
        }
        // 明示設定がある操作は割り当てだけを評価します。
        if (const auto configured = PortableInputBindings().find(
                std::string(action));
            configured != PortableInputBindings().end()
            && !configured->second.empty())
        {
            return std::ranges::any_of(
                configured->second,
                // 割り当てた入力の解放を確認します(binding: 入力割り当て)
                [this](const PortableInputBinding& binding)
                {
                    return m_input->ControlWasReleased(binding.control);
                });
        }
        // 既定キーの解放確認を共通化します(code: キーコード)
        const auto released = [this](const char* code)
        {
            return m_input->WasReleased(code);
        };
        // 横移動と視点操作の既定キーを確認します。
        if (action == "MoveHorizontal" || action == "LookHorizontal")
        {
            return released("KeyA") || released("KeyD")
                || released("ArrowLeft") || released("ArrowRight");
        }
        // 縦移動と視点操作の既定キーを確認します。
        if (action == "MoveVertical" || action == "LookVertical")
        {
            return released("KeyW") || released("KeyS")
                || released("ArrowUp") || released("ArrowDown");
        }
        // 加速の既定キーを確認します。
        if (action == "Accelerate")
        {
            return released("KeyW") || released("ArrowUp");
        }
        // ブレーキの既定キーを確認します。
        if (action == "Brake")
        {
            return released("KeyS") || released("ArrowDown");
        }
        // 再起動の既定キーを確認します。
        if (action == "Restart")
        {
            return released("KeyR");
        }
        // 視点切替の既定キーを確認します。
        if (action == "ToggleView")
        {
            return released("KeyC");
        }
        // ジャンプと決定の既定キーを共有します。
        if (action == "Jump" || action == "Submit")
        {
            return released("Space") || released("Enter");
        }
        // キャンセルと一時停止の既定キーを共有します。
        if (action == "Cancel" || action == "Pause")
        {
            return released("Escape");
        }
        return false;
    }

    // 現在のポインター状態を返します。
    const InputPointerState& InputSystem::Pointer() const noexcept
    {
        // 入力系統がない場合は空状態を返します。
        if (m_input == nullptr)
        {
            m_pointer = {};
            return m_pointer;
        }
        m_pointer.position = {
            m_input->PointerX(),
            m_input->PointerY(),
        };
        m_pointer.delta = {
            m_input->PointerDeltaX(),
            m_input->PointerDeltaY(),
        };
        m_pointer.valid = m_input->PointerValid();
        m_pointer.wheel = m_input->PointerWheel();
        m_pointer.wheelHorizontal = 0.0f;
        m_pointer.down = false;
        m_pointer.pressed = false;
        m_pointer.released = false;
        // 各ボタンの状態を集約します。
        for (std::size_t index{}; index < m_pointer.buttons.size(); ++index)
        {
            // 更新対象のボタン状態です。
            auto& button = m_pointer.buttons[index];
            button.down = m_input->PointerButtonDown(
                static_cast<int>(index));
            button.pressed = m_edgeEventsEnabled
                && m_input->PointerButtonPressed(static_cast<int>(index));
            button.released = m_edgeEventsEnabled
                && m_input->PointerButtonReleased(static_cast<int>(index));
            m_pointer.down = m_pointer.down || button.down;
            m_pointer.pressed = m_pointer.pressed || button.pressed;
            m_pointer.released = m_pointer.released || button.released;
        }
        return m_pointer;
    }

    // 現在のキーボード状態を返します。
    const PortableKeyboardState& InputSystem::KeyboardState() const noexcept
    {
        // 入力系統がない場合は空状態を返します。
        if (m_input == nullptr)
        {
            m_keyboardState = {};
            return m_keyboardState;
        }
        m_keyboardState.Space = m_input->IsDown("Space")
            || m_input->WasPressed("Space");
        m_keyboardState.R = m_input->IsDown("KeyR")
            || m_input->WasPressed("KeyR");
        return m_keyboardState;
    }

    // 所属シーンを返します。
    Scene& Script::GetScene() const noexcept
    {
        return m_owner->GetScene();
    }

    std::uint64_t Script::On(
        const std::string_view eventName,
        std::function<void(const EventArgs&)> handler)
    {
        auto& events = GetScene().Events();
        const auto handle = events.Subscribe(eventName, std::move(handler));
        if (handle != 0) m_eventSubscriptions.emplace_back(&events, handle);
        return handle;
    }

    std::uint64_t Script::On(
        const std::string_view eventName,
        std::function<void()> handler)
    {
        return On(eventName, [callback = std::move(handler)](const EventArgs&)
        {
            if (callback) callback();
        });
    }

    void Script::Off(const std::uint64_t handle)
    {
        for (const auto& subscription : m_eventSubscriptions)
        {
            if (subscription.second == handle)
            {
                subscription.first->Unsubscribe(handle);
                break;
            }
        }
        std::erase_if(m_eventSubscriptions, [handle](const auto& subscription)
        {
            return subscription.second == handle;
        });
    }

    void Script::Emit(const std::string_view eventName)
    {
        EventArgs eventArgs;
        eventArgs.sender = &Owner();
        GetScene().Events().Publish(eventName, eventArgs);
    }

    void Script::Emit(
        const std::string_view eventName,
        EventArgs eventArgs)
    {
        if (eventArgs.sender == nullptr) eventArgs.sender = &Owner();
        GetScene().Events().Publish(eventName, eventArgs);
    }

    // 所属シーンの描画デバイスを返します。
    GraphicsDevice& Script::Graphics() const noexcept
    {
        return GetScene().Graphics();
    }

    // 所有GameObjectを返します。
    GameObject& Script::Owner() const noexcept
    {
        return *m_owner;
    }

    // 名前からGameObjectを探します(name: 検索名)
    GameObject* Script::Find(const std::string_view name) const noexcept
    {
        return GetScene().FindGameObjectByName(name);
    }

    // タグからGameObjectを探します(tag: 検索するタグ)
    GameObject* Script::FindWithTag(const std::string_view tag) const noexcept
    {
        return GetScene().FindGameObjectByTag(tag);
    }

    // タグから全GameObjectを探します(tag: 検索するタグ)
    std::vector<GameObject*> Script::FindObjectsWithTag(
        const std::string_view tag) const
    {
        return GetScene().FindGameObjectsByTag(tag);
    }

    // 所属シーンからGameObjectを削除します(gameObject: 削除対象)
    bool Script::Destroy(GameObject& gameObject)
    {
        return GetScene().DestroyGameObject(gameObject);
    }

    // ScriptからPrefabを生成します(prefabPath: アセットpath, parent: 任意の親)
    GameObject& Script::Instantiate(
        const std::filesystem::path& prefabPath,
        GameObject* parent)
    {
        return GetScene().InstantiatePrefab(prefabPath, parent);
    }

    // 保存文字列を読み込みます(key: 保存キー, fallback: 既定値)
    std::string Script::LoadText(
        std::string_view key,
        std::string fallback) const
    {
        // 保存APIへ渡すキー文字列です。
        const std::string keyText(key);
        // JavaScript側から確保された保存文字列です。
        char* loaded = LoadPortableText(keyText.c_str());
        // 未保存なら既定値を返します。
        if (loaded == nullptr)
        {
            return fallback;
        }
        // 解放前に標準文字列へ複製します。
        std::string value(loaded);
        std::free(loaded);
        return value;
    }

    // 文字列を保存します(key: 保存キー, value: 保存値)
    void Script::SaveText(
        std::string_view key,
        std::string_view value) const
    {
        // 保存APIへ渡すキー文字列です。
        const std::string keyText(key);
        // 保存APIへ渡す値文字列です。
        const std::string valueText(value);
        if (!SavePortableText(keyText.c_str(), valueText.c_str()))
            throw std::runtime_error("Cannot persist game save data");
    }

    // 保存整数を読み込みます(key: 保存キー, fallback: 既定値)
    std::int64_t Script::LoadInteger(
        const std::string_view key,
        const std::int64_t fallback) const
    {
        // 文字列として読み込んだ値です。
        const std::string value = LoadText(key);
        // 空文字列は既定値へ置き換えます。
        if (value.empty())
        {
            return fallback;
        }

        // 整数変換の終端位置です。
        char* end{};
        // 10進整数への変換結果です。
        const long long parsed = std::strtoll(value.c_str(), &end, 10);
        return end != value.c_str() && end != nullptr && *end == '\0'
            ? static_cast<std::int64_t>(parsed)
            : fallback;
    }

    // 整数を文字列化して保存します(key: 保存キー, value: 保存値)
    void Script::SaveInteger(
        const std::string_view key,
        const std::int64_t value) const
    {
        SaveText(key, std::to_string(value));
    }

    // Portable向けスクリプトを登録します(id: 登録名, factory: 生成関数)
    bool RegisterPortableScript(
        std::string id,
        std::string,
        ScriptFactory factory)
    {
        return ScriptFactories().emplace(std::move(id), std::move(factory)).second;
    }

    // 登録名からスクリプトを生成します(id: 登録名)
    std::unique_ptr<Script> CreatePortableScript(std::string_view id)
    {
        // 登録済み生成関数の検索結果です。
        const auto found = ScriptFactories().find(std::string(id));
        return found != ScriptFactories().end() ? found->second() : nullptr;
    }

    // シーン所有のGameObjectを生成します(scene: 所属先, id: 識別子, name: 表示名)
    GameObject::GameObject(Scene& scene, GameObjectId id, std::string name)
        : m_scene(&scene), m_id(id), m_name(std::move(name))
    {
        m_transform.m_owner = this;
    }

    // componentの有効状態を更新し、所有Scriptへ有効状態を反映します(enabled: 新しい状態)
    void Component::SetEnabled(const bool enabled)
    {
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        if (m_owner != nullptr)
        {
            m_owner->RefreshScriptActiveState();
        }
    }

    // 所有GameObjectのTransformを返します。
    Transform& Component::GetTransform() const noexcept
    {
        return m_owner->GetTransform();
    }

    // 自身と所有GameObjectの有効状態を返します。
    bool Component::IsActiveAndEnabled() const noexcept
    {
        return m_owner != nullptr
            && m_enabled
            && m_owner->IsActiveInHierarchy();
    }

    // 有効状態を更新します(enabled: 新しい有効状態)
    void GameObject::SetEnabled(const bool enabled)
    {
        // 状態が変わらなければ副作用を起こしません。
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        // 無効化時はPortable UIを即座に隠します。
        if (!enabled)
        {
            // Portable 2DのSpriteとTextはDOM要素です。
            // Scene::Render()の後処理を待たず、GameObjectを無効化した時点で非表示にします。
            // RetryやHot Reloadで前フレームのUIが残ることを防ぎます。
            HidePortableObjectUi(static_cast<double>(m_id));
        }
        RefreshScriptActiveState();
    }

    // 自身と子孫のScript有効状態遷移を通知します。
    void GameObject::RefreshScriptActiveState()
    {
        const bool activeInHierarchy = IsActiveInHierarchy();
        for (const auto& component : m_components)
        {
            auto* native = dynamic_cast<NativeScriptComponent*>(
                component.get());
            auto* script = native != nullptr
                ? native->Instance()
                : nullptr;
            if (script == nullptr || !script->m_awake)
            {
                continue;
            }
            const bool active = activeInHierarchy && native->IsEnabled();
            if (script->m_active == active)
            {
                continue;
            }
            script->m_active = active;
            if (active)
            {
                script->OnEnable();
            }
            else
            {
                script->OnDisable();
            }
        }
        for (auto* child : m_children)
        {
            child->RefreshScriptActiveState();
        }
    }

    // 名前をたどって子孫を検索します(path: slash区切りの名前)
    GameObject* GameObject::FindChild(const std::string_view path) const noexcept
    {
        if (path.empty())
        {
            return nullptr;
        }

        const std::vector<GameObject*>* children = &m_children;
        std::size_t start{};
        for (;;)
        {
            const auto separator = path.find('/', start);
            const auto name = separator == std::string_view::npos
                ? path.substr(start)
                : path.substr(start, separator - start);
            if (name.empty())
            {
                return nullptr;
            }

            GameObject* found{};
            for (auto* child : *children)
            {
                if (child != nullptr && child->Name() == name)
                {
                    found = child;
                    break;
                }
            }
            if (found == nullptr || separator == std::string_view::npos)
            {
                return found;
            }
            children = &found->m_children;
            start = separator + 1;
        }
    }

    // 所有コンポーネントの順序を変更します(moved: 移動対象, reference: 基準)
    bool GameObject::ReorderComponent(
        const Component& moved,
        const Component& reference,
        const bool insertAfter)
    {
        if (&moved == &reference)
        {
            return false;
        }
        const auto owns = [this](const Component& component)
        {
            return std::find_if(m_components.begin(), m_components.end(),
                [&component](const auto& candidate)
                {
                    return candidate.get() == &component;
                });
        };
        const auto movedPosition = owns(moved);
        if (movedPosition == m_components.end() || owns(reference) == m_components.end())
        {
            return false;
        }

        auto moving = std::move(*movedPosition);
        m_components.erase(movedPosition);
        auto referencePosition = owns(reference);
        if (insertAfter)
        {
            ++referencePosition;
        }
        m_components.insert(referencePosition, std::move(moving));
        return true;
    }

    // ワールド行列をDirectX形式へ変換します。
    DirectX::XMMATRIX GameObject::InterpolatedWorldMatrix(float) const
    {
        // Portable形式のワールド行列です。
        const Mat4 world = ComputeWorldMatrix(*this);
        // DirectX APIへ返す行列です。
        DirectX::XMMATRIX result;
        result._11 = world.values[0]; result._12 = world.values[1];
        result._13 = world.values[2]; result._14 = world.values[3];
        result._21 = world.values[4]; result._22 = world.values[5];
        result._23 = world.values[6]; result._24 = world.values[7];
        result._31 = world.values[8]; result._32 = world.values[9];
        result._33 = world.values[10]; result._34 = world.values[11];
        result._41 = world.values[12]; result._42 = world.values[13];
        result._43 = world.values[14]; result._44 = world.values[15];
        return result;
    }

    // 現在のワールド行列をDirectX形式へ変換します。
    DirectX::XMMATRIX GameObject::WorldMatrix() const noexcept
    {
        return InterpolatedWorldMatrix(0.0f);
    }

    // 有効なカリング設定から常時表示状態を返します。
    bool GameObject::IsAlwaysVisible() const noexcept
    {
        const auto* culling = GetComponent<RenderCullingComponent>();
        return culling != nullptr && culling->IsEnabled()
            && culling->AlwaysVisible();
    }

    // 有効なカリング設定から境界余白を返します。
    float GameObject::CullingMargin() const noexcept
    {
        const auto* culling = GetComponent<RenderCullingComponent>();
        return culling != nullptr && culling->IsEnabled()
            ? culling->CullingMargin() : 0.0f;
    }

    // プリミティブ描画要素を生成します(shape: 形状, color: 色, albedo: 画像)
    MeshRendererComponent::MeshRendererComponent(
        PrimitiveShape shape,
        DirectX::XMFLOAT4 color,
        std::filesystem::path albedo)
        : m_color(color), m_albedo(std::move(albedo))
    {
        BuildPrimitive(shape, m_vertices, m_indices);
    }

    // 頂点とindexを設定します(vertices: 頂点, indices: index, recalculateNormals: 法線再計算)
    void MeshRendererComponent::SetProceduralMesh(
        std::vector<ProceduralMeshVertex> vertices,
        std::vector<std::uint32_t> indices,
        bool recalculateNormals)
    {
        // 要求された場合のみ法線を再計算します。
        if (recalculateNormals)
        {
            RecalculateNormals(vertices, indices);
        }
        m_vertices = std::move(vertices);
        m_indices = std::move(indices);
        m_dirty = true;
    }

    // モデル描画要素を生成します(modelPath: モデル, wireframe: 線表示, materialOverrideEnabled: 材質上書き, color: 色, albedoTexture: 色画像, normalTexture: 法線画像, roughness: 粗さ, normalStrength: 法線強度)
    ModelRendererComponent::ModelRendererComponent(
        std::filesystem::path modelPath,
        bool wireframe,
        bool materialOverrideEnabled,
        DirectX::XMFLOAT4 color,
        std::filesystem::path albedoTexture,
        std::filesystem::path normalTexture,
        float roughness,
        float normalStrength)
        : m_modelPath(std::move(modelPath)),
          m_color(color),
          m_albedoTexture(std::move(albedoTexture)),
          m_normalTexture(std::move(normalTexture)),
          m_roughness(roughness),
          m_normalStrength(normalStrength),
          m_wireframe(wireframe),
          m_materialOverrideEnabled(materialOverrideEnabled)
    {
    }

    // 読み込むモデルを切り替えます(path: モデルファイル)
    void ModelRendererComponent::SetModelPath(std::filesystem::path path)
    {
        m_modelPath = std::move(path);
        m_parts.clear();
        m_nodes.clear();
        m_poseNodes.clear();
        m_nodeWorldMatrices.clear();
        m_skins.clear();
        m_animations.clear();
        m_animationTime = 0.0f;
        m_animationPlaying = false;
        m_loaded = false;
    }

    // 再生対象のanimationを選びます(index: animation番号)
    void ModelRendererComponent::SetAnimationIndex(std::size_t index) noexcept
    {
        m_animationIndex = m_animations.empty()
            ? index
            : std::min(index, m_animations.size() - 1);
        m_animationTime = 0.0f;
        // 読み込み済みなら選択した姿勢を反映します。
        if (m_loaded)
        {
            ApplyPortablePose();
        }
    }

    // 番号に対応するanimation名を返します(index: animation番号)
    std::string_view ModelRendererComponent::AnimationName(
        std::size_t index) const noexcept
    {
        return index < m_animations.size()
            ? std::string_view(m_animations[index].name)
            : std::string_view{};
    }

    // 選択中animationの長さを返します。
    float ModelRendererComponent::AnimationDuration() const noexcept
    {
        return m_animationIndex < m_animations.size()
            ? m_animations[m_animationIndex].duration
            : 0.0f;
    }

    // animationを先頭姿勢で停止します。
    void ModelRendererComponent::StopAnimation() noexcept
    {
        m_animationPlaying = false;
        m_animationTime = 0.0f;
        // 読み込み済みなら停止姿勢を反映します。
        if (m_loaded)
        {
            ApplyPortablePose();
        }
    }

    // 再生位置を更新します(value: 秒)
    void ModelRendererComponent::SetAnimationTime(float value) noexcept
    {
        // 選択中animationの再生長です。
        const float duration = AnimationDuration();
        // 不正値や空animationは先頭へ戻します。
        if (!std::isfinite(value) || duration <= 0.0f)
        {
            m_animationTime = 0.0f;
        }
        // loop再生ではanimation長で折り返します。
        else if (m_animationLoop)
        {
            m_animationTime = std::fmod(std::max(value, 0.0f), duration);
        }
        // 非loop再生ではanimation長に収めます。
        else
        {
            m_animationTime = std::clamp(value, 0.0f, duration);
        }
        // 読み込み済みなら更新位置の姿勢を反映します。
        if (m_loaded)
        {
            ApplyPortablePose();
        }
    }

    // Portable用glTFモデルを読み込みます。
    bool ModelRendererComponent::LoadPortableModel()
    {
        m_loaded = true;
        m_parts.clear();
        m_nodes.clear();
        m_poseNodes.clear();
        m_nodeWorldMatrices.clear();
        m_skins.clear();
        m_animations.clear();
        // 未指定モデルは読み込み失敗として返します。
        if (m_modelPath.empty())
        {
            return false;
        }
        // 仮想asset名をglTF読み込みに使うpathです。
        const std::string virtualPath = VirtualAssetPath(m_modelPath);
        // Emscriptenのasset APIへ渡すpathです。
        const std::string hostPath = virtualPath;
        // 読み込んだbyte数です。
        std::uint32_t byteCount{};
        // JavaScript側から確保されたモデルbufferです。
        unsigned char* loadedBytes = LoadPortableAssetBytes(
            hostPath.c_str(),
            &byteCount);
        // 空または取得不能なassetを拒否します。
        if (loadedBytes == nullptr || byteCount == 0)
        {
            PublishPortableModelStatus(hostPath.c_str(), "asset-read", 0);
            return false;
        }
        // C++の所有期間終了時にasset bufferを解放します。
        const std::unique_ptr<unsigned char, decltype(&std::free)>
            bytes(loadedBytes, &std::free);
        // cgltfのparse設定です。
        cgltf_options options{};
#if defined(LAMAPON_NATIVE_RUNTIME)
        // 外部glTF bufferも、APKを含む同じアセット読み込み経路を通す。
        options.file.read = [](const cgltf_memory_options*, const cgltf_file_options*,
            const char* path, cgltf_size* size, void** data) -> cgltf_result
        {
            std::uint32_t count{};
            auto* bytes = LamaPon::Native::LoadPortableAssetBytes(path, &count);
            if (!bytes) return cgltf_result_file_not_found;
            if (*size != 0 && count < *size) { std::free(bytes); return cgltf_result_io_error; }
            *size = count;
            *data = bytes;
            return cgltf_result_success;
        };
        options.file.release = [](const cgltf_memory_options*, const cgltf_file_options*, void* data, cgltf_size)
        { std::free(data); };
#endif
        // cgltfから受け取る未所有documentです。
        cgltf_data* raw{};
        // 不正なglTF documentを拒否します。
        if (cgltf_parse(&options, bytes.get(), byteCount, &raw)
            != cgltf_result_success)
        {
            PublishPortableModelStatus(hostPath.c_str(), "parse", 0);
            return false;
        }
        // parse後はcgltf_freeでdocumentを解放します。
        const std::unique_ptr<cgltf_data, decltype(&cgltf_free)>
            document(raw, &cgltf_free);
        // 外部bufferを解決できないdocumentを拒否します。
        if (cgltf_load_buffers(&options, document.get(), hostPath.c_str())
            != cgltf_result_success)
        {
            PublishPortableModelStatus(hostPath.c_str(), "buffers", 0);
            return false;
        }
        // glTF仕様に適合しないdocumentを拒否します。
        if (cgltf_validate(document.get()) != cgltf_result_success)
        {
            PublishPortableModelStatus(hostPath.c_str(), "validate", 0);
            return false;
        }
        EncodedModelImages encodedImages;
        m_nodes.resize(document->nodes_count);
        // glTF nodeをPortable形式へ変換します。
        for (cgltf_size nodeIndex{}; nodeIndex < document->nodes_count; ++nodeIndex)
        {
            // cgltf側のnodeです。
            const auto& sourceNode = document->nodes[nodeIndex];
            // Portable側の変換先nodeです。
            auto& node = m_nodes[nodeIndex];
            node.parent = sourceNode.parent != nullptr
                ? static_cast<int>(sourceNode.parent - document->nodes)
                : -1;
            node.translation = sourceNode.has_translation
                ? DirectX::XMFLOAT3{
                    sourceNode.translation[0],
                    sourceNode.translation[1],
                    sourceNode.translation[2] }
                : DirectX::XMFLOAT3{};
            node.rotation = sourceNode.has_rotation
                ? NormalizeModelQuaternion({
                    sourceNode.rotation[0],
                    sourceNode.rotation[1],
                    sourceNode.rotation[2],
                    sourceNode.rotation[3] })
                : DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 1.0f };
            node.scale = sourceNode.has_scale
                ? DirectX::XMFLOAT3{
                    sourceNode.scale[0],
                    sourceNode.scale[1],
                    sourceNode.scale[2] }
                : DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f };
            node.hasMatrix = sourceNode.has_matrix != 0;
            // glTFで行列を指定したnodeだけ値を複製します。
            if (node.hasMatrix)
            {
                std::copy_n(sourceNode.matrix, 16, node.matrix.begin());
            }
            // TRS指定nodeは単位行列から姿勢を組み立てます。
            else
            {
                node.matrix = Mat4::Identity().values;
            }
        }
        // 描画時に更新する姿勢nodeを初期化します。
        m_poseNodes = m_nodes;
        // 各nodeのworld行列を単位行列で初期化します。
        m_nodeWorldMatrices.resize(
            m_nodes.size(), Mat4::Identity().values);

        // glTF skinをPortable形式へ変換します。
        m_skins.reserve(document->skins_count);
        // document内の全skinを順に複製します。
        for (cgltf_size skinIndex{}; skinIndex < document->skins_count; ++skinIndex)
        {
            // cgltf側のskinです。
            const auto& sourceSkin = document->skins[skinIndex];
            // Portable側の変換先skinです。
            ModelSkin skin;
            skin.joints.reserve(sourceSkin.joints_count);
            skin.inverseBindMatrices.reserve(sourceSkin.joints_count);
            // skin内のjoint参照を検証して複製します。
            for (cgltf_size jointIndex{};
                 jointIndex < sourceSkin.joints_count;
                 ++jointIndex)
            {
                // null jointはskinを無効とします。
                if (sourceSkin.joints[jointIndex] == nullptr)
                {
                    PublishPortableModelStatus(
                        hostPath.c_str(), "skin-joint", 0);
                    return false;
                }
                skin.joints.push_back(static_cast<std::size_t>(
                    sourceSkin.joints[jointIndex] - document->nodes));
                // 未指定inverse bind matrix用の初期値です。
                std::array<float, 16> inverseBind = Mat4::Identity().values;
                // inverse bind matrixを読む必要がある場合のみ検証します。
                if (sourceSkin.inverse_bind_matrices != nullptr
                    && !ReadModelFloat(
                        sourceSkin.inverse_bind_matrices,
                        jointIndex,
                        inverseBind.data(),
                        16))
                {
                    PublishPortableModelStatus(
                        hostPath.c_str(), "inverse-bind-matrix", 0);
                    return false;
                }
                skin.inverseBindMatrices.push_back(inverseBind);
            }
            m_skins.emplace_back(std::move(skin));
        }

        // glTF animationをPortable形式へ変換します。
        m_animations.reserve(document->animations_count);
        // document内の全animationを順に変換します。
        for (cgltf_size animationIndex{};
             animationIndex < document->animations_count;
             ++animationIndex)
        {
            // cgltf側のanimationです。
            const auto& sourceAnimation = document->animations[animationIndex];
            // Portable側の変換先animationです。
            ModelAnimation animation;
            animation.name = sourceAnimation.name != nullptr
                ? sourceAnimation.name
                : "Animation " + std::to_string(animationIndex + 1);
            // 対象nodeとsamplerを持つchannelだけを変換します。
            for (cgltf_size channelIndex{};
                 channelIndex < sourceAnimation.channels_count;
                 ++channelIndex)
            {
                // cgltf側の変換元channelです。
                const auto& sourceChannel = sourceAnimation.channels[channelIndex];
                // 不完全または未対応のchannelを読み飛ばします。
                if (sourceChannel.target_node == nullptr
                    || sourceChannel.sampler == nullptr
                    || sourceChannel.sampler->input == nullptr
                    || sourceChannel.sampler->output == nullptr
                    || sourceChannel.target_path
                        == cgltf_animation_path_type_weights)
                {
                    continue;
                }
                // Portable側の変換先channelです。
                ModelAnimationChannel channel;
                channel.nodeIndex = static_cast<std::size_t>(
                    sourceChannel.target_node - document->nodes);
                // weights以外のpathを位置・回転・scaleへ対応させます。
                channel.path = sourceChannel.target_path
                        == cgltf_animation_path_type_translation
                    ? 0u
                    : sourceChannel.target_path
                        == cgltf_animation_path_type_rotation
                    ? 1u
                    : sourceChannel.target_path
                        == cgltf_animation_path_type_scale
                    ? 2u : 255u;
                // 対応しないpathは読み飛ばします。
                if (channel.path == 255u)
                {
                    continue;
                }
                channel.interpolation = sourceChannel.sampler->interpolation
                        == cgltf_interpolation_type_step
                    ? 1u
                    : sourceChannel.sampler->interpolation
                        == cgltf_interpolation_type_cubic_spline
                    ? 2u : 0u;
                // sampler key数です。
                const cgltf_size keyCount = sourceChannel.sampler->input->count;
                // cubic splineではkeyあたり3値を要求します。
                const cgltf_size expectedOutputCount = channel.interpolation == 2u
                    ? keyCount * 3 : keyCount;
                // 空または値不足のsamplerを拒否します。
                if (keyCount == 0
                    || sourceChannel.sampler->output->count < expectedOutputCount)
                {
                    PublishPortableModelStatus(
                        hostPath.c_str(), "animation-sampler", 0);
                    return false;
                }
                channel.times.resize(keyCount);
                channel.values.resize(keyCount);
                // cubic spline用tangent配列を確保します。
                if (channel.interpolation == 2u)
                {
                    channel.inTangents.resize(keyCount);
                    channel.outTangents.resize(keyCount);
                }
                // sampler keyをPortable形式へ変換します。
                for (cgltf_size keyIndex{}; keyIndex < keyCount; ++keyIndex)
                {
                    // key時刻の読み込みに失敗したら中断します。
                    if (!ReadModelFloat(
                            sourceChannel.sampler->input,
                            keyIndex,
                            &channel.times[keyIndex],
                            1))
                    {
                        PublishPortableModelStatus(
                            hostPath.c_str(), "animation-time", 0);
                        return false;
                    }
                    // cubic splineでは中央値がkey値です。
                    const cgltf_size valueIndex = channel.interpolation == 2u
                        ? keyIndex * 3 + 1 : keyIndex;
                    // samplerから読む値です。
                    float values[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
                    // 回転は4成分、位置とscaleは3成分です。
                    const cgltf_size componentCount = channel.path == 1u ? 4 : 3;
                    // key値の読み込みに失敗したら中断します。
                    if (!ReadModelFloat(
                            sourceChannel.sampler->output,
                            valueIndex,
                            values,
                            componentCount))
                    {
                        PublishPortableModelStatus(
                            hostPath.c_str(), "animation-value", 0);
                        return false;
                    }
                    channel.values[keyIndex] = {
                        values[0], values[1], values[2], values[3] };
                    // quaternion値は単位長へ正規化します。
                    if (channel.path == 1u)
                    {
                        channel.values[keyIndex] = NormalizeModelQuaternion(
                            channel.values[keyIndex]);
                    }
                    // cubic splineは入出力tangentも読み込みます。
                    if (channel.interpolation == 2u)
                    {
                        // keyの入射tangentです。
                        float inValues[4]{};
                        // keyの出射tangentです。
                        float outValues[4]{};
                        // いずれかのtangentが欠けたchannelを拒否します。
                        if (!ReadModelFloat(
                                sourceChannel.sampler->output,
                                keyIndex * 3,
                                inValues,
                                componentCount)
                            || !ReadModelFloat(
                                sourceChannel.sampler->output,
                                keyIndex * 3 + 2,
                                outValues,
                                componentCount))
                        {
                            PublishPortableModelStatus(
                                hostPath.c_str(), "animation-tangent", 0);
                            return false;
                        }
                        channel.inTangents[keyIndex] = {
                            inValues[0], inValues[1], inValues[2], inValues[3] };
                        channel.outTangents[keyIndex] = {
                            outValues[0], outValues[1], outValues[2], outValues[3] };
                    }
                    animation.duration = std::max(
                        animation.duration, channel.times[keyIndex]);
                }
                animation.channels.emplace_back(std::move(channel));
            }
            // 有効なchannelを含むanimationだけを保持します。
            if (!animation.channels.empty())
            {
                m_animations.emplace_back(std::move(animation));
            }
        }
        // meshを持つnodeごとにprimitiveを変換します。
        for (cgltf_size nodeIndex{}; nodeIndex < document->nodes_count; ++nodeIndex)
        {
            // cgltf側のnodeです。
            const auto& node = document->nodes[nodeIndex];
            // meshを持たないnodeは描画対象外です。
            if (node.mesh == nullptr)
            {
                continue;
            }
            // node階層を含むglTF world変換です。
            float world[16]{};
            cgltf_node_transform_world(&node, world);
            // glTFの表面は反時計回りですが、LamaPon Rendererは時計回りです。
            // ミラー変換されたNodeでは向きが反転することも考慮します。
            const bool reverseWinding = ModelTransformDeterminant(world) >= 0.0f;
            // node内のprimitiveを描画用partへ変換します。
            for (cgltf_size primitiveIndex{};
                 primitiveIndex < node.mesh->primitives_count;
                 ++primitiveIndex)
            {
                // cgltf側の変換元primitiveです。
                const auto& primitive = node.mesh->primitives[primitiveIndex];
                // 三角形以外のprimitiveは描画しません。
                if (primitive.type != cgltf_primitive_type_triangles)
                {
                    continue;
                }
                // 頂点位置attributeです。
                const auto* positions = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_position);
                // 頂点位置がないprimitiveは描画できません。
                if (positions == nullptr || positions->count == 0)
                {
                    continue;
                }
                // 任意の頂点法線attributeです。
                const auto* normals = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_normal);
                // 任意のtexture coordinate attributeです。
                const auto* coordinates = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_texcoord);
                // 任意のskin joint index attributeです。
                const auto* jointIndices = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_joints);
                // 任意のskin joint weight attributeです。
                const auto* jointWeights = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_weights);
                // 描画とskin適用に使う変換先partです。
                Part part;
                part.meshNodeIndex = nodeIndex;
                // skin付きnodeは参照と頂点attributeを検証します。
                if (node.skin != nullptr)
                {
                    part.skinIndex = static_cast<int>(
                        node.skin - document->skins);
                    // skinまたはjoint attributeが不正ならモデルを拒否します。
                    if (part.skinIndex < 0
                        || static_cast<std::size_t>(part.skinIndex)
                            >= m_skins.size()
                        || jointIndices == nullptr
                        || jointWeights == nullptr
                        || jointIndices->count != positions->count
                        || jointWeights->count != positions->count)
                    {
                        PublishPortableModelStatus(
                            hostPath.c_str(), "skin-attributes", 0);
                        return false;
                    }
                    part.joints.reserve(positions->count);
                    part.weights.reserve(positions->count);
                }
                part.vertices.reserve(positions->count);
                part.bindVertices.reserve(positions->count);
                // primitive頂点をPortable形式へ変換します。
                for (cgltf_size vertexIndex{};
                     vertexIndex < positions->count;
                     ++vertexIndex)
                {
                    // 頂点位置の読込先です。
                    float positionValues[3]{};
                    // 法線がない場合に使う既定値です。
                    float normalValues[3]{ 0.0f, 1.0f, 0.0f };
                    // texture coordinateの読込先です。
                    float coordinateValues[2]{};
                    // 頂点位置を読めなければこのprimitiveを破棄します。
                    if (!ReadModelFloat(
                            positions,
                            vertexIndex,
                            positionValues,
                            3))
                    {
                        part.vertices.clear();
                        break;
                    }
                    // 任意の法線とtexture coordinateを読み込みます。
                    const bool hasNormal = ReadModelFloat(
                        normals, vertexIndex, normalValues, 3);
                    // 任意のtexture coordinateを読み込めたか示します。
                    const bool hasCoordinate = ReadModelFloat(
                        coordinates, vertexIndex, coordinateValues, 2);
                    (void)hasNormal;
                    (void)hasCoordinate;
                    // Portable vertexへ詰める変換結果です。
                    ProceduralMeshVertex vertex{
                        { positionValues[0], positionValues[1], positionValues[2] },
                        { normalValues[0], normalValues[1], normalValues[2] },
                        { coordinateValues[0], coordinateValues[1] },
                    };
                    part.vertices.push_back(vertex);
                    part.bindVertices.push_back(vertex);
                    // skin indexを持つ頂点だけweightを読み込みます。
                    if (part.skinIndex >= 0)
                    {
                        // glTF形式の4 joint indexです。
                        cgltf_uint jointValues[4]{};
                        // 対応する4 joint weightです。
                        float weightValues[4]{};
                        // skin頂点attributeが読めなければモデルを拒否します。
                        if (!cgltf_accessor_read_uint(
                                jointIndices,
                                vertexIndex,
                                jointValues,
                                4)
                            || !ReadModelFloat(
                                jointWeights,
                                vertexIndex,
                                weightValues,
                                4))
                        {
                            PublishPortableModelStatus(
                                hostPath.c_str(), "skin-vertex", 0);
                            return false;
                        }
                        // このpartが参照するskinです。
                        const auto& skin = m_skins[
                            static_cast<std::size_t>(part.skinIndex)];
                        // GPUへ渡す圧縮joint indexです。
                        std::array<std::uint16_t, 4> packedJoints{};
                        // 4 influenceを検証して圧縮します。
                        for (std::size_t influence{}; influence < 4; ++influence)
                        {
                            // skin外jointは無効なモデルとして扱います。
                            if (jointValues[influence] >= skin.joints.size())
                            {
                                PublishPortableModelStatus(
                                    hostPath.c_str(), "skin-joint-index", 0);
                                return false;
                            }
                            packedJoints[influence] = static_cast<std::uint16_t>(
                                jointValues[influence]);
                        }
                        part.joints.push_back(packedJoints);
                        part.weights.push_back({
                            weightValues[0], weightValues[1],
                            weightValues[2], weightValues[3] });
                    }
                }
                // 頂点を持たないprimitiveは読み飛ばします。
                if (part.vertices.empty())
                {
                    continue;
                }
                // index未指定なら頂点順をそのまま使います。
                const cgltf_size indexCount = primitive.indices != nullptr
                    ? primitive.indices->count
                    : positions->count;
                // 三角形を構成しないindex列は読み飛ばします。
                if (indexCount == 0 || indexCount % 3 != 0)
                {
                    continue;
                }
                // primitive indexを保存します。
                part.indices.resize(indexCount);
                // 範囲外indexがないかを記録します。
                bool validIndices = true;
                // index列を読み込み頂点範囲を検証します。
                for (cgltf_size index{}; index < indexCount; ++index)
                {
                    // accessor値または暗黙の連番indexです。
                    const cgltf_size value = primitive.indices != nullptr
                        ? cgltf_accessor_read_index(primitive.indices, index)
                        : index;
                    // 頂点数を超えるindexはprimitiveを無効化します。
                    if (value >= part.vertices.size())
                    {
                        validIndices = false;
                        break;
                    }
                    part.indices[index] = static_cast<std::uint32_t>(value);
                }
                // 不正indexを含むprimitiveは読み飛ばします。
                if (!validIndices)
                {
                    continue;
                }
                // rendererの頂点規約に合わせて面の向きを反転します。
                if (reverseWinding)
                {
                    // 三角形ごとに2頂点を交換します。
                    for (std::size_t index{};
                         index + 2 < part.indices.size();
                         index += 3)
                    {
                        std::swap(part.indices[index + 1], part.indices[index + 2]);
                    }
                }
                // 法線がなければ面から再計算します。
                if (normals == nullptr)
                {
                    RecalculateNormals(part.vertices, part.indices);
                    part.bindVertices = part.vertices;
                }
                // glTF materialが指定されたpartへ材質を反映します。
                if (primitive.material != nullptr)
                {
                    // cgltf側のmaterialです。
                    const auto& material = *primitive.material;
                    part.unlit = material.unlit != 0;
                    part.doubleSided = material.double_sided != 0;
                    part.alphaBlended =
                        material.alpha_mode == cgltf_alpha_mode_blend;
                    part.alphaCutoff = material.alpha_mode == cgltf_alpha_mode_mask
                        ? material.alpha_cutoff : -1.0f;
                    // PBR metallic-roughness情報を反映します。
                    if (material.has_pbr_metallic_roughness)
                    {
                        // glTF metallic-roughness値です。
                        const auto& pbr = material.pbr_metallic_roughness;
                        part.color = {
                            pbr.base_color_factor[0],
                            pbr.base_color_factor[1],
                            pbr.base_color_factor[2],
                            pbr.base_color_factor[3],
                        };
                        part.roughness = pbr.roughness_factor;
                        part.metallic = pbr.metallic_factor;
                        part.albedoTexture = ModelTexturePath(
                            m_modelPath,
                            pbr.base_color_texture, part.albedoImage, encodedImages);
                        part.metallicRoughnessTexture = ModelTexturePath(
                            m_modelPath,
                            pbr.metallic_roughness_texture, part.metallicRoughnessImage, encodedImages);
                    }
                    part.normalTexture = ModelTexturePath(
                        m_modelPath,
                        material.normal_texture, part.normalImage, encodedImages);
                    part.normalStrength = material.normal_texture.scale;
                    part.occlusionTexture = ModelTexturePath(
                        m_modelPath,
                        material.occlusion_texture, part.occlusionImage, encodedImages);
                    part.occlusionStrength = material.occlusion_texture.scale;
                    part.emissiveTexture = ModelTexturePath(
                        m_modelPath,
                        material.emissive_texture, part.emissiveImage, encodedImages);
                    // emissive intensityの既定値です。
                    const float emissiveStrength = material.has_emissive_strength
                        ? material.emissive_strength.emissive_strength
                        : 1.0f;
                    part.emissiveColor = {
                        material.emissive_factor[0] * emissiveStrength,
                        material.emissive_factor[1] * emissiveStrength,
                        material.emissive_factor[2] * emissiveStrength,
                    };
                    // 未指定IORの既定値を使います。
                    const float ior = material.has_ior
                        ? std::max(material.ior.ior, 1.0f)
                        : 1.5f;
                    // IORからdielectric反射率F0を算出します。
                    const float f0 = std::pow(
                        (ior - 1.0f) / (ior + 1.0f), 2.0f);
                    // 未指定specular factorの既定値です。
                    const float specularFactor = material.has_specular
                        ? material.specular.specular_factor
                        : 1.0f;
                    part.dielectricSpecular = {
                        f0 * specularFactor
                            * (material.has_specular
                                ? material.specular.specular_color_factor[0]
                                : 1.0f),
                        f0 * specularFactor
                            * (material.has_specular
                                ? material.specular.specular_color_factor[1]
                                : 1.0f),
                        f0 * specularFactor
                            * (material.has_specular
                                ? material.specular.specular_color_factor[2]
                                : 1.0f),
                    };
                    // opaque materialはalphaを常に1にします。
                    if (material.alpha_mode == cgltf_alpha_mode_opaque)
                    {
                        part.color.w = 1.0f;
                    }
                }
                // 有効なmaterial overrideでglTF値を上書きします。
                if (m_materialOverrideEnabled)
                {
                    part.color = m_color;
                    part.roughness = m_roughness;
                    part.metallic = m_metallic;
                    part.normalStrength = m_normalStrength;
                    part.roughnessTexture = m_roughnessTexture;
                    part.metallicTexture = m_metallicTexture;
                    part.occlusionTexture = m_occlusionTexture;
                    part.occlusionStrength = m_occlusionStrength;
                    part.emissiveTexture = m_emissiveTexture;
                    part.emissiveColor = m_emissiveColor;
                    part.alphaBlended = m_color.w < 0.999f;
                    part.alphaCutoff = -1.0f;
                    // override画像が指定されている場合だけ差し替えます。
                    if (!m_albedoTexture.empty())
                    {
                        part.albedoTexture = m_albedoTexture;
                        part.albedoImage.reset();
                    }
                    // override法線画像が指定されている場合だけ差し替えます。
                    if (!m_normalTexture.empty())
                    {
                        part.normalTexture = m_normalTexture;
                        part.normalImage.reset();
                    }
                }
                m_parts.emplace_back(std::move(part));
            }
        }
        m_animationIndex = m_animations.empty()
            ? 0u
            : std::min(m_animationIndex, m_animations.size() - 1);
        m_animationTime = 0.0f;
        m_animationPlaying = m_animationPlayOnStart && !m_animations.empty();
        ApplyPortablePose();
        PublishPortableModelStatus(
            hostPath.c_str(),
            m_parts.empty() ? "no-renderable-parts" : "loaded",
            static_cast<int>(m_parts.size()));
        return !m_parts.empty();
    }

    // 再生位置をdelta分進めて姿勢を反映します(deltaTime: 経過秒)
    void ModelRendererComponent::AdvancePortableAnimation(float deltaTime)
    {
        // 停止中・対象外・不正deltaでは更新しません。
        if (!m_animationPlaying || m_animationIndex >= m_animations.size()
            || !std::isfinite(deltaTime))
        {
            return;
        }
        // 選択中animationの再生長です。
        const float duration = m_animations[m_animationIndex].duration;
        // 長さがないanimationは停止します。
        if (duration <= 0.0f)
        {
            m_animationPlaying = false;
            return;
        }
        m_animationTime += deltaTime * m_animationSpeed;
        // loop再生は範囲外位置を折り返します。
        if (m_animationLoop)
        {
            m_animationTime = std::fmod(m_animationTime, duration);
            // 負方向再生をanimation長内へ戻します。
            if (m_animationTime < 0.0f)
            {
                m_animationTime += duration;
            }
        }
        // 非loop再生は終端で停止します。
        else if (m_animationTime >= duration || m_animationTime <= 0.0f)
        {
            m_animationTime = std::clamp(m_animationTime, 0.0f, duration);
            m_animationPlaying = false;
        }
        ApplyPortablePose();
    }

    // 選択中animationをnode姿勢とworld行列へ反映します。
    void ModelRendererComponent::ApplyPortablePose()
    {
        // node未読込なら姿勢更新を行いません。
        if (m_nodes.empty())
        {
            return;
        }
        // 外部へ通知する選択中animation名です。
        const char* animationName = m_animationIndex < m_animations.size()
            ? m_animations[m_animationIndex].name.c_str()
            : "";
        PublishPortableModelAnimation(
            animationName,
            static_cast<int>(m_animationIndex),
            static_cast<int>(m_animations.size()),
            m_animationTime,
            m_animationPlaying ? 1 : 0);
        m_poseNodes = m_nodes;
        // 選択animationのchannelをnodeへ適用します。
        if (m_animationIndex < m_animations.size())
        {
            // 現在選択されているanimationです。
            const auto& animation = m_animations[m_animationIndex];
            // 各channelの再生値を計算します。
            for (const auto& channel : animation.channels)
            {
                // 対象nodeとkey配列の整合性を確認します。
                if (channel.nodeIndex >= m_poseNodes.size()
                    || channel.times.empty()
                    || channel.values.size() != channel.times.size())
                {
                    continue;
                }
                // 再生時刻を越える最初のkey位置です。
                std::size_t upper = static_cast<std::size_t>(
                    std::upper_bound(
                        channel.times.begin(),
                        channel.times.end(),
                        m_animationTime) - channel.times.begin());
                // 補間元key indexです。
                std::size_t first{};
                // 補間先key indexです。
                std::size_t second{};
                // key間の正規化補間率です。
                float amount{};
                // key間の時間幅です。
                float segmentDuration{};
                // 再生時刻が最初のkey以前なら先頭値を使います。
                if (upper == 0)
                {
                    first = second = 0;
                }
                // 最終key以降なら末尾値を使います。
                else if (upper >= channel.times.size())
                {
                    first = second = channel.times.size() - 1;
                }
                // 区間内は隣接key間の補間率を計算します。
                else
                {
                    first = upper - 1;
                    second = upper;
                    segmentDuration = channel.times[second] - channel.times[first];
                    amount = segmentDuration > 0.000001f
                        ? std::clamp(
                            (m_animationTime - channel.times[first])
                                / segmentDuration,
                            0.0f,
                            1.0f)
                        : 0.0f;
                }
                // 補間前のkey値です。
                DirectX::XMFLOAT4 value = channel.values[first];
                // STEP以外で異なるkey間を補間します。
                if (first != second && channel.interpolation != 1u)
                {
                    // cubic splineはtangent付きHermite補間を使います。
                    if (channel.interpolation == 2u
                        && channel.inTangents.size() == channel.values.size()
                        && channel.outTangents.size() == channel.values.size())
                    {
                        value = HermiteModelVector(
                            channel.values[first],
                            channel.outTangents[first],
                            channel.values[second],
                            channel.inTangents[second],
                            amount,
                            segmentDuration);
                        // 回転値は再正規化してから保持します。
                        if (channel.path == 1u)
                        {
                            value = NormalizeModelQuaternion(value);
                        }
                    }
                    // 回転はquaternion球面補間を使います。
                    else if (channel.path == 1u)
                    {
                        value = SlerpModelQuaternion(
                            channel.values[first],
                            channel.values[second],
                            amount);
                    }
                    // 位置とscaleは線形補間します。
                    else
                    {
                        value = LerpModelVector(
                            channel.values[first],
                            channel.values[second],
                            amount);
                    }
                }
                // channelの対象nodeを更新します。
                auto& node = m_poseNodes[channel.nodeIndex];
                node.hasMatrix = false;
                // translation channelをnodeへ反映します。
                if (channel.path == 0u)
                {
                    node.translation = { value.x, value.y, value.z };
                }
                // rotation channelをnodeへ反映します。
                else if (channel.path == 1u)
                {
                    node.rotation = NormalizeModelQuaternion(value);
                }
                // scale channelをnodeへ反映します。
                else if (channel.path == 2u)
                {
                    node.scale = { value.x, value.y, value.z };
                }
            }
        }

        // node world行列計算の未訪問・訪問中・完了状態です。
        std::vector<std::uint8_t> matrixStates(m_poseNodes.size());
        // 親nodeを先に評価しworld行列をmemoizeします(self: 再帰参照, index: node番号)
        const auto calculateWorld = [&](const auto& self, std::size_t index) -> Mat4
        {
            // 範囲外nodeは単位行列とします。
            if (index >= m_poseNodes.size())
            {
                return Mat4::Identity();
            }
            // 計算済みnodeのworld行列を再利用します。
            if (matrixStates[index] == 2u)
            {
                return ModelMatrix(m_nodeWorldMatrices[index]);
            }
            // 循環参照では再帰を止めます。
            if (matrixStates[index] == 1u)
            {
                return Mat4::Identity();
            }
            // 現在のnodeが計算中であることを記録します。
            matrixStates[index] = 1u;
            // 評価対象のpose nodeです。
            const auto& node = m_poseNodes[index];
            // nodeのローカル変換行列です。
            const Mat4 local = node.hasMatrix
                ? ModelMatrix(node.matrix)
                : ModelTrsMatrix(node.translation, node.rotation, node.scale);
            // 親world行列へローカル行列を合成します。
            const Mat4 world = node.parent >= 0
                ? LamaPon::Web::Multiply(
                    self(self, static_cast<std::size_t>(node.parent)), local)
                : local;
            // 算出済みworld行列を保存します。
            m_nodeWorldMatrices[index] = ModelMatrix(world);
            // nodeの計算完了を記録します。
            matrixStates[index] = 2u;
            return world;
        };
        // 全nodeのworld行列を解決します。
        for (std::size_t index{}; index < m_poseNodes.size(); ++index)
        {
            calculateWorld(calculateWorld, index);
        }

        // animation姿勢を各mesh partの頂点へ反映します。
        for (auto& part : m_parts)
        {
            // bind頂点と描画頂点の数が違うpartは更新しません。
            if (part.vertices.size() != part.bindVertices.size())
            {
                continue;
            }
            // skinのないpartはnode world行列だけで変換します。
            if (part.skinIndex < 0)
            {
                // mesh nodeのworld行列です。
                const auto& matrix = m_nodeWorldMatrices[part.meshNodeIndex];
                // bind頂点へnode変換を適用します。
                for (std::size_t vertexIndex{};
                     vertexIndex < part.vertices.size();
                     ++vertexIndex)
                {
                    part.vertices[vertexIndex].position = TransformModelPoint(
                        matrix.data(), part.bindVertices[vertexIndex].position);
                    part.vertices[vertexIndex].normal = TransformModelNormal(
                        matrix.data(), part.bindVertices[vertexIndex].normal);
                }
                part.dirty = true;
                continue;
            }
            // このpartが参照するskinです。
            const auto& skin = m_skins[static_cast<std::size_t>(part.skinIndex)];
            // jointごとのworldとinverse bind行列です。
            std::vector<Mat4> jointMatrices;
            jointMatrices.reserve(skin.joints.size());
            // 各jointのskin行列を計算します。
            for (std::size_t jointIndex{};
                 jointIndex < skin.joints.size();
                 ++jointIndex)
            {
                jointMatrices.push_back(LamaPon::Web::Multiply(
                    ModelMatrix(m_nodeWorldMatrices[skin.joints[jointIndex]]),
                    ModelMatrix(skin.inverseBindMatrices[jointIndex])));
            }
            // bind頂点へweighted joint変換を適用します。
            for (std::size_t vertexIndex{};
                 vertexIndex < part.vertices.size();
                 ++vertexIndex)
            {
                // skin前の頂点属性です。
                const auto& source = part.bindVertices[vertexIndex];
                // 4つのjoint indexです。
                const auto& joints = part.joints[vertexIndex];
                // 4つのjoint weightです。
                const auto& weights = part.weights[vertexIndex];
                // jointごとの重み配列です。
                const std::array<float, 4> influenceWeights{
                    weights.x, weights.y, weights.z, weights.w };
                // 変換後の合成位置です。
                Vec3 position{};
                // 変換後の合成法線です。
                Vec3 normal{};
                // 正規化に使うweight合計です。
                float totalWeight{};
                // 有効なjoint influenceを合成します。
                for (std::size_t influence{}; influence < 4; ++influence)
                {
                    // 現在のjointのweightです。
                    const float weight = influenceWeights[influence];
                    // 寄与しないweightと範囲外jointを除外します。
                    if (weight <= 0.000001f
                        || joints[influence] >= jointMatrices.size())
                    {
                        continue;
                    }
                    // 対象jointのskin行列値です。
                    const auto& matrix = jointMatrices[joints[influence]].values;
                    position += WebVector(TransformModelPoint(
                        matrix.data(), source.position)) * weight;
                    normal += WebVector(TransformModelNormal(
                        matrix.data(), source.normal)) * weight;
                    totalWeight += weight;
                }
                // 有効weightがない頂点はbind属性を維持します。
                if (totalWeight <= 0.000001f)
                {
                    position = WebVector(source.position);
                    normal = WebVector(source.normal);
                }
                // weight合計の誤差を位置へ補正します。
                else if (std::abs(totalWeight - 1.0f) > 0.0001f)
                {
                    position = position * (1.0f / totalWeight);
                }
                part.vertices[vertexIndex].position = DirectXVector(position);
                part.vertices[vertexIndex].normal = DirectXVector(
                    LamaPon::Web::Normalize(normal));
            }
            part.dirty = true;
        }
    }

    // 粒子発生設定を初期化します(capacity: 上限, emissionRate: 発生率, lifetime: 寿命範囲, speed: 速度範囲, size: 大きさ範囲, startColor: 開始色, endColor: 終了色, shape: 発生形状, texture: 画像)
    ParticleSystemComponent::ParticleSystemComponent(
        std::uint32_t capacity,
        float emissionRate,
        DirectX::XMFLOAT2 lifetime,
        DirectX::XMFLOAT2 speed,
        DirectX::XMFLOAT2 size,
        DirectX::XMFLOAT4 startColor,
        DirectX::XMFLOAT4 endColor,
        ParticleEmitterShape shape,
        std::filesystem::path texture)
        : m_capacity(capacity), m_emissionRate(emissionRate),
          m_lifetime(lifetime), m_speed(speed), m_size(size),
          m_startColor(startColor), m_endColor(endColor), m_shape(shape),
          m_texture(std::move(texture))
    {
    }

    // 指定数の粒子を発生させます(count: 発生数)
    void ParticleSystemComponent::Emit(int count)
    {
        // ownerのworld行列です。
        const Mat4 world = ComputeWorldMatrix(Owner());
        // 発生器のworld位置です。
        const DirectX::XMFLOAT3 origin{
            world.values[12], world.values[13], world.values[14] };
        // 乱数状態を進めて0から1の値を返します。
        const auto nextRandom = [this]() noexcept
        {
            m_randomState = m_randomState * 1664525u + 1013904223u;
            return static_cast<float>(m_randomState >> 8)
                / static_cast<float>(0x00ffffffu);
        };
        // 容量に達するまで粒子を生成します。
        for (int index{}; index < count && m_particles.size() < m_capacity; ++index)
        {
            // 球形状用の乱数です。
            const float random = nextRandom();
            // 形状用の第2乱数です。
            const float randomY = nextRandom();
            // 形状用の第3乱数です。
            const float randomZ = nextRandom();
            // 発生器内の粒子位置です。
            Vec3 localOffset{};
            // 発生器内の初期進行方向です。
            Vec3 localDirection{ 0.0f, 1.0f, 0.0f };
            // 球面上の方向と半径で配置します。
            if (m_shape == ParticleEmitterShape::Sphere)
            {
                // 球面上の正規化方向です。
                const Vec3 direction = LamaPon::Web::Normalize({
                    random * 2.0f - 1.0f,
                    randomY * 2.0f - 1.0f,
                    randomZ * 2.0f - 1.0f,
                });
                // 球内の発生半径です。
                const float radius = nextRandom() * 0.5f;
                localOffset = {
                    direction.x * radius * m_emitterSize.x,
                    direction.y * radius * m_emitterSize.y,
                    direction.z * radius * m_emitterSize.z,
                };
                localDirection = direction;
            }
            // 箱の各軸へ一様に配置します。
            else if (m_shape == ParticleEmitterShape::Box)
            {
                localOffset = {
                    (random - 0.5f) * m_emitterSize.x,
                    (randomY - 0.5f) * m_emitterSize.y,
                    (randomZ - 0.5f) * m_emitterSize.z,
                };
            }
            // cone方向と底面半径で配置します。
            else
            {
                // cone周方向の角度です。
                const float azimuth =
                    random * std::numbers::pi_v<float> * 2.0f;
                // cone軸からの偏角です。
                const float angle = std::clamp(
                    m_coneAngle, 0.0f,
                    std::numbers::pi_v<float> * 0.499f)
                    * std::sqrt(randomY);
                // 偏角の正弦を方向と位置に共有します。
                const float sine = std::sin(angle);
                localDirection = {
                    std::cos(azimuth) * sine,
                    std::cos(angle),
                    std::sin(azimuth) * sine,
                };
                // cone底面内の発生半径です。
                const float radius = std::sqrt(randomZ) * 0.5f;
                localOffset = {
                    std::cos(azimuth) * radius * m_emitterSize.x,
                    0.0f,
                    std::sin(azimuth) * radius * m_emitterSize.z,
                };
            }
            // 発生器内方向をworld方向へ変換します(value: ローカル方向)
            const auto transformDirection = [&world](const Vec3& value)
            {
                return Vec3{
                    world.values[0] * value.x
                        + world.values[4] * value.y
                        + world.values[8] * value.z,
                    world.values[1] * value.x
                        + world.values[5] * value.y
                        + world.values[9] * value.z,
                    world.values[2] * value.x
                        + world.values[6] * value.y
                        + world.values[10] * value.z,
                };
            };
            // 発生位置offsetのworld変換です。
            const Vec3 worldOffset = transformDirection(localOffset);
            // 正規化済みのworld進行方向です。
            const Vec3 worldDirection = LamaPon::Web::Normalize(
                transformDirection(localDirection));
            // 設定範囲内で選んだ粒子速度です。
            const float speed =
                m_speed.x + (m_speed.y - m_speed.x) * nextRandom();
            m_particles.push_back({
                { origin.x + worldOffset.x,
                  origin.y + worldOffset.y,
                  origin.z + worldOffset.z },
                DirectXVector(worldDirection * speed),
                0.0f,
                m_lifetime.x + (m_lifetime.y - m_lifetime.x) * random,
                m_size.x + (m_size.y - m_size.x) * random,
                random * std::numbers::pi_v<float> * 2.0f,
            });
        }
    }

    // 発生を止め必要なら既存粒子を消します(clearParticles: 粒子削除)
    void ParticleSystemComponent::Stop(bool clearParticles)
    {
        m_playing = false;
        // 要求時は画面上の粒子も消します。
        if (clearParticles)
        {
            m_particles.clear();
        }
    }

    // 再生pitchを設定します(value: pitch倍率)
    void AudioSourceComponent::SetPitch(float value)
    {
        m_pitch = std::clamp(value, -1.0f, 1.0f);
#if LAMAPON_WEB_AUDIO_ENABLED
        // Web audio handleがあるとき再生中音源へ反映します。
        if (m_handle != 0)
        {
            Owner().GetScene().WebAudio().SetPitch(m_handle, m_pitch);
        }
#endif
    }

    // 再生音量を設定します(value: 音量)
    void AudioSourceComponent::SetVolume(float value)
    {
        m_volume = std::clamp(value, 0.0f, 1.0f);
#if LAMAPON_WEB_AUDIO_ENABLED
        // Web audio handleがあるとき再生中音源へ反映します。
        if (m_handle != 0)
        {
            Owner().GetScene().WebAudio().SetVolume(m_handle, m_volume);
        }
#endif
    }

    // 左右panを設定します(value: -1から1の位置)
    void AudioSourceComponent::SetPan(float value)
    {
        m_pan = std::clamp(value, -1.0f, 1.0f);
#if LAMAPON_WEB_AUDIO_ENABLED
        // Web audio handleがあるとき再生中音源へ反映します。
        if (m_handle != 0)
        {
            Owner().GetScene().WebAudio().SetPan(m_handle, m_pan);
        }
#endif
    }

    // 空間音声の距離をWindows runtimeと同じ下限で整えます。
    void AudioSourceComponent::SetMinimumDistance(const float value)
    {
        const bool wasPlaying = m_handle != 0;
        if (wasPlaying) Stop();
        m_minimumDistance = std::max(value, 0.01f);
        m_maximumDistance = std::max(
            m_maximumDistance, m_minimumDistance + 0.01f);
        if (wasPlaying) Play();
    }

    // 最大距離を最小距離より0.01以上離します。
    void AudioSourceComponent::SetMaximumDistance(const float value)
    {
        const bool wasPlaying = m_handle != 0;
        if (wasPlaying) Stop();
        m_maximumDistance = std::max(
            value, m_minimumDistance + 0.01f);
        if (wasPlaying) Play();
    }

    // loop音源を開始しone-shot音源を委譲再生します。
    void AudioSourceComponent::Play()
    {
#if LAMAPON_WEB_AUDIO_ENABLED
        // 未作成のloop音源だけを生成します。
        if (m_loop && m_handle == 0)
        {
            // Web audio APIへ渡す仮想asset pathです。
            const std::string path = VirtualAssetPath(m_path);
            // 音源位置に使うowner transformです。
            const auto& position = Owner().GetTransform().position;
            m_handle = Owner().GetScene().WebAudio().PlayLoop(
                path, m_volume, m_pan, m_spatial,
                position.x, position.y, position.z,
                m_minimumDistance, m_maximumDistance);
            Owner().GetScene().WebAudio().SetPitch(m_handle, m_pitch);
        }
        // loopしない場合はone-shot再生へ委譲します。
        else if (!m_loop)
        {
            PlayOneShot();
        }
#endif
    }

    // Web audioでone-shot音源を再生します。
    void AudioSourceComponent::PlayOneShot()
    {
#if LAMAPON_WEB_AUDIO_ENABLED
        // Web audio APIへ渡す仮想asset pathです。
        const std::string path = VirtualAssetPath(m_path);
        // 音源位置に使うowner transformです。
        const auto& position = Owner().GetTransform().position;
        Owner().GetScene().WebAudio().PlayWav(
            path, m_volume, false, m_pan, m_spatial,
            position.x, position.y, position.z,
            m_minimumDistance, m_maximumDistance);
#endif
    }

    // loop音源を停止してhandleを破棄します。
    void AudioSourceComponent::Stop()
    {
#if LAMAPON_WEB_AUDIO_ENABLED
        // 有効な音源handleだけを停止します。
        if (m_handle != 0)
        {
            Owner().GetScene().WebAudio().Stop(m_handle);
            m_handle = 0;
        }
#endif
    }

    // 再生位置を更新します(value: 秒)
    void TransformAnimatorComponent::SetTime(float value) noexcept
    {
        // 不正値や空clipは先頭位置へ戻します。
        if (!std::isfinite(value) || m_duration <= 0.0f)
        {
            m_time = 0.0f;
        }
        // loop再生ではclip長で折り返します。
        else if (m_loop)
        {
            m_time = std::fmod(std::max(value, 0.0f), m_duration);
        }
        // 非loop再生ではclip範囲へ収めます。
        else
        {
            m_time = std::clamp(value, 0.0f, m_duration);
        }
        ApplyPortableSample();
    }

    // Portable用animation clipを読み込みます。
    bool TransformAnimatorComponent::LoadPortableClip()
    {
        // JSON形式のclip documentです。
        Json document;
        // 未対応形式やkeyframe配列がないclipを拒否します。
        if (!LoadPortableJsonDocument(m_clipPath, document)
            || document.value("format", "") != "LamaPonAnimationClip"
            || document.value("version", 0) != 1
            || !document.contains("keyframes")
            || !document.at("keyframes").is_array())
        {
            return false;
        }
        // JSON配列を3成分値へ変換します(value: JSON値, fallback: 既定値)
        const auto readFloat3 = [](const Json& value,
                                   DirectX::XMFLOAT3 fallback)
        {
            // 不足した成分は既定値で補います。
            if (!value.is_array() || value.size() < 3)
            {
                return fallback;
            }
            return DirectX::XMFLOAT3{
                value.at(0).get<float>(),
                value.at(1).get<float>(),
                value.at(2).get<float>(),
            };
        };
        // 検証済みkeyframeの一時領域です。
        std::vector<Keyframe> loaded;
        loaded.reserve(document.at("keyframes").size());
        // 時刻の昇順と非負条件を検証する直前の値です。
        float previous = -1.0f;
        // JSON型不一致を読み込み失敗として扱います。
        try
        {
            // keyframeを検証して読み込みます。
            for (const auto& value : document.at("keyframes"))
            {
                // 現在keyframeの再生時刻です。
                const float time = value.at("time").get<float>();
                // 時刻は有限・非負かつ厳密昇順である必要があります。
                if (!std::isfinite(time) || time < 0.0f || time <= previous)
                {
                    return false;
                }
                previous = time;
                loaded.push_back({
                    time,
                    readFloat3(value.at("position"), {}),
                    readFloat3(value.at("rotation"), {}),
                    readFloat3(
                        value.at("scale"),
                        { 1.0f, 1.0f, 1.0f }),
                });
            }
        }
        // JSON parse/access exceptionを失敗として返します。
        catch (const Json::exception&)
        {
            return false;
        }
        // 空clipと過大なkeyframe配列を拒否します。
        if (loaded.empty() || loaded.size() > 4096)
        {
            return false;
        }
        // JSON durationまたは最後のkey時刻をclip長にします。
        const float duration = document.value("duration", loaded.back().time);
        // clip長は有限かつ最後のkey以降である必要があります。
        if (!std::isfinite(duration)
            || duration <= 0.0f
            || duration < loaded.back().time)
        {
            return false;
        }
        m_keyframes = std::move(loaded);
        m_duration = duration;
        m_time = 0.0f;
        m_playing = m_playOnStart;
        ApplyPortableSample();
        return true;
    }

    // 再生位置をdelta分進めてsampleを反映します(deltaTime: 経過秒)
    void TransformAnimatorComponent::AdvancePortableAnimation(float deltaTime)
    {
        // 停止中・空clip・長さ未設定では更新しません。
        if (!m_playing || m_keyframes.empty() || m_duration <= 0.0f)
        {
            return;
        }
        m_time += deltaTime * m_speed;
        // loop再生はclip範囲外の位置を折り返します。
        if (m_loop)
        {
            m_time = std::fmod(m_time, m_duration);
            // 負方向再生をclip長内へ戻します。
            if (m_time < 0.0f)
            {
                m_time += m_duration;
            }
        }
        // 非loop再生は端点で停止します。
        else if (m_time >= m_duration || m_time <= 0.0f)
        {
            m_time = std::clamp(m_time, 0.0f, m_duration);
            m_playing = false;
        }
        ApplyPortableSample();
    }

    // 現在時刻のkeyframeをtransformへ補間反映します。
    void TransformAnimatorComponent::ApplyPortableSample()
    {
        // keyframe未読込ならtransformを変更しません。
        if (m_keyframes.empty())
        {
            return;
        }
        // 補間元keyframeです。
        const Keyframe* from = &m_keyframes.front();
        // 補間先keyframeです。
        const Keyframe* to = from;
        // 現在時刻を含むkey区間を探します。
        for (std::size_t index = 1; index < m_keyframes.size(); ++index)
        {
            to = &m_keyframes[index];
            // 再生時刻を覆うkeyへ到達したら探索を終えます。
            if (m_time <= to->time)
            {
                break;
            }
            from = to;
        }
        // 区間内の正規化補間率です。
        float amount{};
        // 異なる時刻のkey間だけ補間率を計算します。
        if (to != from && to->time > from->time)
        {
            amount = std::clamp(
                (m_time - from->time) / (to->time - from->time),
                0.0f,
                1.0f);
        }
        // 任意の数値を線形補間します(left: 開始値, right: 終了値)
        const auto lerp = [amount](float left, float right)
        {
            return left + (right - left) * amount;
        };
        // 回転角を最短経路で線形補間します(left: 開始角, right: 終了角)
        const auto lerpAngle = [amount](float left, float right)
        {
            // 角度差を一周未満へ正規化する定数です。
            constexpr float TwoPi = std::numbers::pi_v<float> * 2.0f;
            return left + std::remainder(right - left, TwoPi) * amount;
        };
        // animationを適用するowner transformです。
        auto& transform = Owner().GetTransform();
        transform.position = {
            lerp(from->position.x, to->position.x),
            lerp(from->position.y, to->position.y),
            lerp(from->position.z, to->position.z),
        };
        transform.rotation = {
            lerpAngle(from->rotation.x, to->rotation.x),
            lerpAngle(from->rotation.y, to->rotation.y),
            lerpAngle(from->rotation.z, to->rotation.z),
        };
        transform.scale = {
            lerp(from->scale.x, to->scale.x),
            lerp(from->scale.y, to->scale.y),
            lerp(from->scale.z, to->scale.z),
        };
    }

    // UIアンカーと配置値を初期化します(anchorMin: 最小anchor, anchorMax: 最大anchor, pivot: 基準点, anchoredPosition: 相対位置, sizeDelta: サイズ差)
    DirectX::XMFLOAT3 PortableLocalLightComponent::WorldPosition() const noexcept
    {
        const auto world = ComputeWorldMatrix(Owner());
        return { world.values[12], world.values[13], world.values[14] };
    }

    DirectX::XMFLOAT3 SpotLightComponent::WorldDirection() const noexcept
    {
        const auto world = ComputeWorldMatrix(Owner());
        const Web::Vec3 forward{-world.values[8], -world.values[9], -world.values[10]};
        const auto direction = Web::Length(forward) > 0.0001f
            ? Web::Normalize(forward) : Web::Vec3{0,-1,0};
        return { direction.x, direction.y, direction.z };
    }

    UIRectTransformComponent::UIRectTransformComponent(
        const DirectX::XMFLOAT2 anchorMin,
        const DirectX::XMFLOAT2 anchorMax,
        const DirectX::XMFLOAT2 pivot,
        const DirectX::XMFLOAT2 anchoredPosition,
        const DirectX::XMFLOAT2 sizeDelta) noexcept
        : m_anchorMin(ClampUnit2(anchorMin)),
          m_anchorMax(ClampUnit2(anchorMax)),
          m_pivot(ClampUnit2(pivot)),
          m_anchoredPosition(anchoredPosition),
          m_sizeDelta(sizeDelta)
    {
        m_anchorMax.x = std::max(m_anchorMax.x, m_anchorMin.x);
        m_anchorMax.y = std::max(m_anchorMax.y, m_anchorMin.y);
    }

    // 最小anchorを更新します(value: 0から1の座標)
    void UIRectTransformComponent::SetAnchorMin(
        const DirectX::XMFLOAT2 value) noexcept
    {
        m_anchorMin = ClampUnit2(value);
        m_anchorMax.x = std::max(m_anchorMax.x, m_anchorMin.x);
        m_anchorMax.y = std::max(m_anchorMax.y, m_anchorMin.y);
    }

    // 最大anchorを更新します(value: 0から1の座標)
    void UIRectTransformComponent::SetAnchorMax(
        const DirectX::XMFLOAT2 value) noexcept
    {
        m_anchorMax = ClampUnit2(value);
        m_anchorMin.x = std::min(m_anchorMin.x, m_anchorMax.x);
        m_anchorMin.y = std::min(m_anchorMin.y, m_anchorMax.y);
    }

    // pivotを更新します(value: 0から1の基準点)
    void UIRectTransformComponent::SetPivot(
        const DirectX::XMFLOAT2 value) noexcept
    {
        m_pivot = ClampUnit2(value);
    }

    // 親矩形からpixel座標を解決します(viewportWidth: 幅, viewportHeight: 高さ)
    UIRect UIRectTransformComponent::Resolve(
        const float viewportWidth,
        const float viewportHeight) const noexcept
    {
        // 親がない場合に使うviewport矩形です。
        UIRect parentRect{
            {},
            {
                std::max(viewportWidth, 1.0f),
                std::max(viewportHeight, 1.0f)
            }
        };
        // 最も近い親UI rectのworld矩形を探します。
        for (const GameObject* ancestor = Owner().Parent();
             ancestor != nullptr;
             ancestor = ancestor->Parent())
        {
            // 親にrectがあればその矩形を基準にします。
            if (const auto* parentTransform =
                    ancestor->GetComponent<UIRectTransformComponent>())
            {
                parentRect = parentTransform->Resolve(
                    viewportWidth,
                    viewportHeight);
                break;
            }
        }
        float canvasScale = 1.0f;
        for (const GameObject* ancestor = &Owner(); ancestor; ancestor = ancestor->Parent())
            if (const auto* canvas = ancestor->GetComponent<UICanvasComponent>())
            { canvasScale = canvas->ScaleFactor(viewportWidth, viewportHeight); break; }
        // 親矩形の幅と高さです。
        const auto parentSize = parentRect.Size();
        // 最小anchorのpixel位置です。
        const DirectX::XMFLOAT2 anchorPixelsMin{
            parentRect.minimum.x + parentSize.x * m_anchorMin.x,
            parentRect.minimum.y + parentSize.y * m_anchorMin.y
        };
        // 最大anchorのpixel位置です。
        const DirectX::XMFLOAT2 anchorPixelsMax{
            parentRect.minimum.x + parentSize.x * m_anchorMax.x,
            parentRect.minimum.y + parentSize.y * m_anchorMax.y
        };
        // anchor範囲とsizeDeltaから算出したサイズです。
        const DirectX::XMFLOAT2 size{
            std::max(anchorPixelsMax.x - anchorPixelsMin.x + m_sizeDelta.x * canvasScale,
                     0.0f),
            std::max(anchorPixelsMax.y - anchorPixelsMin.y + m_sizeDelta.y * canvasScale,
                     0.0f)
        };
        // anchor範囲とanchoredPositionから算出したpivot位置です。
        const DirectX::XMFLOAT2 pivotPosition{
            anchorPixelsMin.x
                + (anchorPixelsMax.x - anchorPixelsMin.x) * m_pivot.x
                + m_anchoredPosition.x * canvasScale,
            anchorPixelsMin.y
                + (anchorPixelsMax.y - anchorPixelsMin.y) * m_pivot.y
                + m_anchoredPosition.y * canvasScale
        };
        return {
            {
                pivotPosition.x - size.x * m_pivot.x,
                pivotPosition.y - size.y * m_pivot.y
            },
            {
                pivotPosition.x + size.x * (1.0f - m_pivot.x),
                pivotPosition.y + size.y * (1.0f - m_pivot.y)
            }
        };
    }

    // 文字表示要素を生成します(text: 本文, fontFamily: font名, fontSize: 文字サイズ, color: 色, bounds: 表示範囲, wordWrap: 折返し, horizontal: 横揃え, vertical: 縦揃え)
    TextRendererComponent::TextRendererComponent(
        std::string text,
        std::string fontFamily,
        float fontSize,
        DirectX::XMFLOAT4 color,
        DirectX::XMFLOAT2 bounds,
        bool wordWrap,
        TextHorizontalAlignment horizontal,
        TextVerticalAlignment vertical)
        : m_text(std::move(text)), m_fontFamily(std::move(fontFamily)),
          m_fontSize(std::max(fontSize, 1.0f)), m_color(color),
          m_bounds{
              std::clamp(bounds.x, 0.0f, 4096.0f),
              std::clamp(bounds.y, 0.0f, 4096.0f)
          },
          m_wordWrap(wordWrap), m_horizontal(horizontal), m_vertical(vertical)
    {
    }

    // Sprite表示要素を生成します(size: サイズ, color: 色, texture: 画像path)
    SpriteRendererComponent::SpriteRendererComponent(
        DirectX::XMFLOAT2 size,
        DirectX::XMFLOAT4 color,
        std::filesystem::path texture)
        : m_size(size), m_color(color), m_texture(std::move(texture))
    {
    }

    // Sprite sheetの列数と行数を初期化します(columns: 列数, rows: 行数)
    SpriteAnimatorComponent::SpriteAnimatorComponent(
        const int columns,
        const int rows) noexcept
    {
        SetSheetGrid(columns, rows);
    }

    // Sprite sheetのgridを設定します(columns: 列数, rows: 行数)
    void SpriteAnimatorComponent::SetSheetGrid(
        const int columns,
        const int rows) noexcept
    {
        m_columns = std::max(columns, 1);
        m_rows = std::max(rows, 1);
    }

    // clipを追加または同名clipと置換します(clip: animation設定)
    void SpriteAnimatorComponent::AddClip(SpriteAnimationClip clip)
    {
        clip.startFrame = std::max(clip.startFrame, 0);
        clip.frameCount = std::max(clip.frameCount, 1);
        clip.framesPerSecond = std::max(clip.framesPerSecond, 0.01f);
        // 同名clipがあれば差し替えて終了します。
        for (auto& existing : m_clips)
        {
            // 登録名が一致する既存clipを更新します。
            if (existing.name == clip.name)
            {
                existing = std::move(clip);
                return;
            }
        }
        m_clips.push_back(std::move(clip));
    }

    // 登録clipを削除します(name: clip名)
    void SpriteAnimatorComponent::RemoveClip(const std::string_view name)
    {
        // 指定名のclipを全件削除します。
        std::erase_if(
            m_clips,
            // 削除対象の登録名を比較します(clip: 登録clip)
            [name](const SpriteAnimationClip& clip)
            {
                return clip.name == name;
            });
        // 再生中clipを削除した場合は再生状態を解除します。
        if (m_activeClip == name)
        {
            m_activeClip.clear();
            m_playing = false;
            m_currentFrame = -1;
        }
    }

    // 登録名からclipを探します(name: clip名)
    const SpriteAnimationClip* SpriteAnimatorComponent::FindClip(
        const std::string_view name) const noexcept
    {
        // 登録済みclipから一致するものを探します。
        for (const auto& clip : m_clips)
        {
            // 名前一致したclipを返します。
            if (clip.name == name)
            {
                return &clip;
            }
        }
        return nullptr;
    }

    // clipを先頭frameから再生します(clipName: clip名)
    bool SpriteAnimatorComponent::Play(const std::string_view clipName)
    {
        // 再生対象clipの検索結果です。
        const auto* clip = FindClip(clipName);
        // 未登録clipは再生できません。
        if (clip == nullptr)
        {
            return false;
        }
        m_activeClip = clip->name;
        m_time = 0.0f;
        m_playing = true;
        ApplyFrame(clip->startFrame);
        return true;
    }

    // Sprite sheetのframeをsource rectへ反映します(sheetFrame: frame番号)
    void SpriteAnimatorComponent::ApplyFrame(const int sheetFrame)
    {
        m_currentFrame = sheetFrame;
        // 描画対象のSprite componentです。
        auto* sprite = Owner().GetComponent<SpriteRendererComponent>();
        // Spriteがなければsource rectを更新できません。
        if (sprite == nullptr)
        {
            return;
        }
        // sheet内の総frame数です。
        const int totalFrames = m_columns * m_rows;
        // frame数内へ折り返した番号です。
        int frame = totalFrames > 0 ? sheetFrame % totalFrames : 0;
        // 負のframe番号を正の範囲へ戻します。
        if (frame < 0)
        {
            frame += totalFrames;
        }
        // 1 cellあたりのUV幅です。
        const float width = 1.0f / static_cast<float>(m_columns);
        // 1 cellあたりのUV高さです。
        const float height = 1.0f / static_cast<float>(m_rows);
        sprite->SetSourceRect({
            static_cast<float>(frame % m_columns) * width,
            static_cast<float>(frame / m_columns) * height,
            width,
            height,
        });
    }

    // 再生状態をdelta分進めてframeを反映します(deltaTime: 経過秒)
    void SpriteAnimatorComponent::Advance(const float deltaTime)
    {
        // 初回更新でplay-on-startを処理します。
        if (!m_started)
        {
            m_started = true;
            // 再生対象があれば既定clipを開始します。
            if (m_playOnStart && !m_clips.empty())
            {
                Play(m_defaultClip.empty()
                    ? m_clips.front().name
                    : m_defaultClip);
            }
        }
        // 現在再生中clipの検索結果です。
        const auto* clip = m_playing ? FindClip(m_activeClip) : nullptr;
        // 再生対象が消えていれば更新を終えます。
        if (clip == nullptr)
        {
            return;
        }
        // 経過秒と再生速度を加算します。
        m_time += deltaTime * m_speed;
        // 経過時間から進んだframe数です。
        const int advanced = static_cast<int>(std::floor(
            m_time * clip->framesPerSecond));
        // clip内の相対frame番号です。
        int index = advanced;
        // loop clipではframeを循環させます。
        if (clip->loop)
        {
            index %= clip->frameCount;
            // 負方向再生をclip範囲内へ戻します。
            if (index < 0)
            {
                index += clip->frameCount;
            }
        }
        // 非loop clipは最終frameで停止します。
        else if (index >= clip->frameCount)
        {
            index = clip->frameCount - 1;
            m_playing = false;
        }
        // 再生開始前は先頭frameに留めます。
        else
        {
            index = std::max(index, 0);
        }
        ApplyFrame(clip->startFrame + index);
    }

    // 参照objectの移動に応じて位置をずらします(mainCamera: 既定camera)
    void ParallaxLayerComponent::Advance(GameObject* mainCamera)
    {
        // 明示referenceを優先しなければcameraを使います。
        GameObject* reference = m_reference != nullptr ? m_reference : mainCamera;
        // 自分自身や参照先なしでは移動しません。
        if (reference == nullptr || reference == &Owner())
        {
            return;
        }
        // 参照先objectのpositionです。
        const auto& referencePosition = reference->GetTransform().position;
        // parallaxを適用するowner transformです。
        auto& own = Owner().GetTransform();
        // 初回は参照位置とowner位置の基準を記録します。
        if (!m_initialized)
        {
            m_referenceOrigin = { referencePosition.x, referencePosition.y };
            m_ownOrigin = { own.position.x, own.position.y };
            m_initialized = true;
            return;
        }
        own.position.x = m_ownOrigin.x
            + (referencePosition.x - m_referenceOrigin.x) * m_factor.x;
        own.position.y = m_ownOrigin.y
            + (referencePosition.y - m_referenceOrigin.y) * m_factor.y;
    }

    // Owner設定後に登録済みnative scriptを生成します。
    void NativeScriptComponent::OnAttached()
    {
        m_script = CreatePortableScript(m_scriptId);
        // script生成に成功した場合だけownerを関連付けます。
        if (m_script != nullptr)
        {
            m_script->m_owner = &Owner();
        }
    }

    // Portable sceneの描画・audio・input依存を接続します(renderer: 描画器, audio: 音声, input: 入力)
    Scene::Scene(
        Web::Renderer3D& renderer,
        Web::WebAudioRuntime& audio,
        Web::WebInput& input)
        : m_impl(std::make_unique<Impl>()), m_renderer(&renderer), m_audio(&audio)
    {
        m_graphics.Input().Bind(&input);
    }

    // scene所有scriptへ終了通知を送り、objectと実装状態を破棄します。
    Scene::~Scene()
    {
        for (auto object = m_objects.rbegin();
             object != m_objects.rend();
             ++object)
        {
            if (*object != nullptr)
            {
                DestroyScripts(**object);
            }
        }
    }

    // scene所有のGameObjectを追加します(name: 表示名)
    GameObject& Scene::CreateGameObject(std::string name)
    {
        // 一意IDを割り当てた新規objectです。
        auto object = std::make_unique<GameObject>(*this, m_nextId++, std::move(name));
        // 所有vectorへ移動する前の参照です。
        GameObject& reference = *object;
        m_objects.push_back(std::move(object));
        return reference;
    }

    // 名前からscene内のGameObjectを探します(name: 検索名)
    GameObject* Scene::FindGameObjectByName(
        const std::string_view name) noexcept
    {
        // 所有objectから名前一致を探します。
        for (const auto& object : m_objects)
        {
            // 有効なobjectの名前が一致したら返します。
            if (object != nullptr && object->Name() == name)
            {
                return object.get();
            }
        }
        return nullptr;
    }

    // タグからscene内のGameObjectを探します(tag: 検索タグ)
    GameObject* Scene::FindGameObjectByTag(
        const std::string_view tag) noexcept
    {
        for (const auto& object : m_objects)
        {
            if (object != nullptr && object->CompareTag(tag))
            {
                return object.get();
            }
        }
        return nullptr;
    }

    // タグが一致するscene内のGameObjectを登録順に収集します(tag: 検索タグ)
    std::vector<GameObject*> Scene::FindGameObjectsByTag(
        const std::string_view tag) const
    {
        std::vector<GameObject*> results;
        for (const auto& object : m_objects)
        {
            if (object != nullptr && object->CompareTag(tag))
            {
                results.push_back(object.get());
            }
        }
        return results;
    }

    // scene所有vectorからGameObjectを削除します(gameObject: 削除対象)
    bool Scene::DestroyGameObject(GameObject& gameObject)
    {
        // 参照先objectの所有位置を検索します。
        const auto found = std::find_if(
            m_objects.begin(), m_objects.end(),
            // 所有pointerが参照先と一致するか確認します(object: 所有要素)
            [&gameObject](const auto& object)
            {
                return object.get() == &gameObject;
            });
        // scene所有objectに含まれない参照は削除できません。
        if (found == m_objects.end())
        {
            return false;
        }

        // 対象objectと子孫objectを無効化します。
        for (const auto& object : m_objects)
        {
            // 対象自身なら削除treeに含めます。
            bool belongsToTree = object.get() == &gameObject;
            // 親階層をたどり対象の子孫か確認します。
            for (const GameObject* parent = object->Parent();
                 !belongsToTree && parent != nullptr;
                 parent = parent->Parent())
            {
                belongsToTree = parent == &gameObject;
            }
            // 対象tree外のobjectは保持します。
            if (!belongsToTree)
            {
                continue;
            }
            // 削除予約前にobjectとPortable UIを無効化します。
            object->SetEnabled(false);
            // IDが未登録の場合だけpending destroyへ追加します。
            if (std::find(
                    m_pendingDestroy.begin(), m_pendingDestroy.end(),
                    object->Id()) == m_pendingDestroy.end())
            {
                m_pendingDestroy.push_back(object->Id());
            }
        }
        return true;
    }

    // 削除予約を反映してobject所有領域を整理します。
    void Scene::FlushDestroyedObjects()
    {
        // 削除予約がなければ処理を省略します。
        if (m_pendingDestroy.empty())
        {
            return;
        }

        // 削除対象IDの重複を排除します。
        const std::unordered_set<GameObjectId> destroyed{
            m_pendingDestroy.begin(), m_pendingDestroy.end()
        };
        for (const auto& object : m_objects)
        {
            if (destroyed.contains(object->Id()))
            {
                if (m_impl->mainCamera == object.get())
                {
                    m_impl->mainCamera = nullptr;
                }
                DestroyScripts(*object);
            }
        }
        // 削除対象を親に持つobjectをrootへ切り離します。
        for (const auto& object : m_objects)
        {
            const auto* parent = object->Parent();
            // 親の子一覧から削除対象を外し、残る子も削除済みの親から切り離します。
            if (destroyed.contains(object->Id())
                || (parent != nullptr
                    && destroyed.contains(parent->Id())))
            {
                object->SetParent(nullptr);
            }
        }
        // 削除対象IDのGameObjectを所有vectorから消します。
        std::erase_if(
            m_objects,
            // objectのIDが削除集合にあるかを判定します(object: 所有要素)
            [&destroyed](const auto& object)
            {
                return destroyed.contains(object->Id());
            });
        m_pendingDestroy.clear();
    }

    // 予約された主Sceneを読み込み、成功時だけ旧object群を破棄します。
    void Scene::ProcessPendingSceneLoad()
    {
        if (!m_scenes.HasPendingLoad())
        {
            return;
        }
        const auto path = m_scenes.TakePendingPath();
        std::vector<GameObjectId> previousIds;
        previousIds.reserve(m_objects.size());
        for (const auto& object : m_objects)
        {
            previousIds.push_back(object->Id());
        }
        GameObject* const previousMainCamera = m_impl->mainCamera;
        m_impl->mainCamera = nullptr;
        bool loaded{};
        try
        {
            loaded = Load(path);
        }
        catch (const std::exception& error)
        {
            m_scenes.RecordLoadFailure(error.what());
        }
        if (!loaded)
        {
            m_impl->mainCamera = previousMainCamera;
            return;
        }
        for (const auto id : previousIds)
        {
            const auto object = std::find_if(
                m_objects.begin(), m_objects.end(),
                [id](const auto& candidate)
                {
                    return candidate->Id() == id;
                });
            if (object != m_objects.end())
            {
                static_cast<void>(DestroyGameObject(**object));
            }
        }
        FlushDestroyedObjects();
    }

    // 仮想pathのscene JSONを読み込みます(virtualPath: scene asset)
    bool Scene::Load(const std::filesystem::path& virtualPath)
    {
        // asset APIへ渡すUTF-8 pathです。
        std::string path = PortablePathToUtf8(virtualPath);
        if (path.empty())
        {
            m_scenes.RecordLoadFailure("Scene asset path is empty.");
            return false;
        }
        if (path.starts_with("/assets/"))
        {
        }
        else if (path.starts_with("assets/"))
        {
            path.insert(path.begin(), '/');
        }
        else if (path.front() == '/')
        {
            m_scenes.RecordLoadFailure(
                "Scene path must be relative to assets or start with /assets/.");
            return false;
        }
        else
        {
            path.insert(0, "/assets/");
        }
        for (const auto& part : PortablePathFromUtf8(path))
        {
            if (part == "..")
            {
                m_scenes.RecordLoadFailure(
                    "Scene path cannot escape the assets directory.");
                return false;
            }
        }
        // JavaScript側から確保されたJSON bufferです。
        char* loaded = LoadPortableAssetText(path.c_str());
        // assetが取得できなければ読み込みを中断します。
        if (loaded == nullptr)
        {
            m_scenes.RecordLoadFailure("Could not open scene: " + path);
            return false;
        }
        // JSON bufferの所有権を自動で解放します。
        const std::unique_ptr<char, decltype(&std::free)> loadedOwner(
            loaded, &std::free);
        // Scene JSONはportable runtimeが読む間だけ所有します。
        const std::string document(loaded);
        if (!LoadDocument(document, nullptr, nullptr, true))
        {
            m_scenes.RecordLoadFailure("Could not load scene: " + path);
            return false;
        }
        m_scenes.RecordLoadSuccess(PortablePathFromUtf8(path));
        return true;
    }

    // 仮想Prefab pathからPrefabを読み込みます(prefabPath: assets相対path)
    GameObject& Scene::InstantiatePrefab(
        const std::filesystem::path& prefabPath,
        GameObject* parent)
    {
        // VFSから読み込むUTF-8 pathです。
        std::string assetPath = PortablePathToUtf8(prefabPath);
        if (assetPath.empty())
        {
            throw std::invalid_argument("Prefab asset path is empty.");
        }
        if (assetPath.starts_with("/assets/"))
        {
        }
        else if (assetPath.starts_with("assets/"))
        {
            assetPath.insert(assetPath.begin(), '/');
        }
        else if (assetPath.front() == '/')
        {
            throw std::invalid_argument(
                "Prefab path must be relative to assets or start with /assets/.");
        }
        else
        {
            assetPath.insert(0, "/assets/");
        }
        for (const auto& part : PortablePathFromUtf8(assetPath))
        {
            if (part == "..")
            {
                throw std::invalid_argument(
                    "Prefab path cannot escape the assets directory.");
            }
        }
        // VFSのPrefab文書を読み込みます。
        char* loaded = LoadPortableAssetText(assetPath.c_str());
        if (loaded == nullptr)
        {
            throw std::runtime_error("Could not open prefab: " + assetPath);
        }
        const std::unique_ptr<char, decltype(&std::free)> loadedOwner(
            loaded, &std::free);
        GameObject* root{};
        if (!LoadDocument(std::string_view(loaded), parent, &root, false)
            || root == nullptr)
        {
            throw std::runtime_error("Could not instantiate portable prefab.");
        }
        return *root;
    }

    // JSON文書をSceneへトランザクション復元します。
    bool Scene::LoadDocument(
        const std::string_view json,
        GameObject* prefabParent,
        GameObject** prefabRoot,
        const bool restoreEnvironment) try
    {
        if (prefabRoot != nullptr)
        {
            *prefabRoot = nullptr;
        }
        // 指定parentが同じSceneに属することを確認します。
        if (prefabParent != nullptr
            && std::none_of(
                m_objects.begin(), m_objects.end(),
                [prefabParent](const auto& object)
                {
                    return object.get() == prefabParent;
                }))
        {
            return false;
        }
        // SceneまたはPrefab JSONをparseします。
        const Json document = Json::parse(json.begin(), json.end());
        std::int64_t prefabRootId{};
        if (prefabRoot != nullptr)
        {
            const auto objects = document.find("objects");
            if (!document.is_object()
                || document.value("format", std::string{}) != "LamaPonPrefab"
                || document.value("version", 0) != 1
                || objects == document.end()
                || !objects->is_array()
                || objects->empty()
                || objects->size() > 4096)
            {
                return false;
            }
            prefabRootId = document.value("root", std::int64_t{});
            std::unordered_set<std::int64_t> sourceIds;
            std::unordered_set<std::int64_t> parentIds;
            std::unordered_map<std::int64_t, std::int64_t> parentById;
            std::size_t rootCount{};
            bool declaredRootIsTopLevel{};
            for (const auto& object : *objects)
            {
                if (!object.is_object())
                {
                    return false;
                }
                const auto idValue = object.find("id");
                if (idValue == object.end() || !idValue->is_number_integer())
                {
                    return false;
                }
                const auto sourceId = idValue->get<std::int64_t>();
                if (sourceId <= 0 || !sourceIds.insert(sourceId).second)
                {
                    return false;
                }
                const auto parentValue = object.find("parent");
                if (parentValue == object.end() || parentValue->is_null())
                {
                    ++rootCount;
                    declaredRootIsTopLevel = declaredRootIsTopLevel
                        || sourceId == prefabRootId;
                }
                else if (parentValue->is_number_integer())
                {
                    const auto parentId = parentValue->get<std::int64_t>();
                    parentIds.insert(parentId);
                    parentById.emplace(sourceId, parentId);
                }
                else
                {
                    return false;
                }
            }
            if (!sourceIds.contains(prefabRootId) || rootCount != 1
                || !declaredRootIsTopLevel)
            {
                return false;
            }
            for (const auto parentId : parentIds)
            {
                if (!sourceIds.contains(parentId))
                {
                    return false;
                }
            }
            for (const auto& [sourceId, parentId] : parentById)
            {
                (void)parentId;
                std::unordered_set<std::int64_t> visited;
                auto cursor = sourceId;
                while (true)
                {
                    const auto parent = parentById.find(cursor);
                    if (parent == parentById.end())
                    {
                        break;
                    }
                    if (!visited.insert(cursor).second)
                    {
                        return false;
                    }
                    cursor = parent->second;
                }
            }
        }

        // Scene restoration is transactional so a malformed value does not
        // leave half of its objects in the running game.
        const auto objectCountBeforeLoad = m_objects.size();
        const auto nextIdBeforeLoad = m_nextId;
        GameObject* const mainCameraBeforeLoad = m_impl->mainCamera;
        const auto rollback = [
            this, objectCountBeforeLoad, nextIdBeforeLoad,
            mainCameraBeforeLoad](Scene*) noexcept
        {
            for (std::size_t index = objectCountBeforeLoad;
                 index < m_objects.size(); ++index)
            {
                auto& object = *m_objects[index];
                object.SetParent(nullptr);
                if (auto* parallax = object.GetComponent<ParallaxLayerComponent>())
                {
                    parallax->m_reference = nullptr;
                }
            }
            m_objects.resize(objectCountBeforeLoad);
            m_nextId = nextIdBeforeLoad;
            m_impl->mainCamera = mainCameraBeforeLoad;
        };
        std::unique_ptr<Scene, decltype(rollback)> loadTransaction(this, rollback);

        if (restoreEnvironment)
        {
            LoadPortableInputBindings();
        }
        // 読み込み中に使うsource IDとobjectの対応表です。
        std::unordered_map<std::int64_t, GameObject*> bySourceId;
        // parent参照をobject生成後に解決する一時情報です。
        struct PendingParent final
        {
            // parentを設定するobjectです。
            GameObject* object{};
            // scene JSON上のparent IDです。
            std::int64_t parent{};
        };
        // light component生成をobject反復後に行う一時情報です。
        struct PendingDirectionalLight final
        {
            // lightを追加するobjectです。
            GameObject* object{};
            // directional light colorです。
            DirectX::XMFLOAT3 color{ 1.0f, 0.96f, 0.88f };
            // light intensityです。
            float intensity{ 1.0f };
            // 読み込み後の有効状態です。
            bool enabled{ true };
        };
        // object生成後に設定するparent一覧です。
        std::vector<PendingParent> pendingParents;
        // object生成後に追加するdirectional light一覧です。
        std::vector<PendingDirectionalLight> directionalLights;
        // Prefab rootの復元先を保持します。
        GameObject* loadedPrefabRoot{};
        // JSON配列から2成分値を読みます(value: JSON値, fallback: 既定値)
        const auto float2 = [](const Json& value, DirectX::XMFLOAT2 fallback)
        {
            // 成分数が足りない場合は既定値を使います。
            return value.is_array() && value.size() >= 2
                ? DirectX::XMFLOAT2{
                    value.at(0).get<float>(), value.at(1).get<float>() }
                : fallback;
        };
        // JSON配列から3成分値を読みます(value: JSON値, fallback: 既定値)
        const auto float3 = [](const Json& value, DirectX::XMFLOAT3 fallback)
        {
            // 成分数が足りない場合は既定値を使います。
            return value.is_array() && value.size() >= 3
                ? DirectX::XMFLOAT3{
                    value.at(0).get<float>(), value.at(1).get<float>(),
                    value.at(2).get<float>() }
                : fallback;
        };
        // JSON配列から4成分値を読みます(value: JSON値, fallback: 既定値)
        const auto float4 = [](const Json& value, DirectX::XMFLOAT4 fallback)
        {
            // 成分数が足りない場合は既定値を使います。
            return value.is_array() && value.size() >= 4
                ? DirectX::XMFLOAT4{
                    value.at(0).get<float>(), value.at(1).get<float>(),
                    value.at(2).get<float>(), value.at(3).get<float>() }
                : fallback;
        };
        // scene JSONのobjectを順に生成します。
        for (const auto& objectJson : document.value("objects", Json::array()))
        {
            // object名は省略時に既定名を使います。
            auto& object = CreateGameObject(objectJson.value("name", "GameObject"));
            object.SetTag(objectJson.value("tag", std::string{}));
            // scene JSON内のsource IDです。
            const std::int64_t sourceId = objectJson.value("id", 0ll);
            bySourceId[sourceId] = &object;
            // parentを省略したオブジェクトはルートとして扱います。
            // const JSONへの[]は欠落時にassertで停止するため使いません。
            const auto parent = objectJson.find("parent");
            // parent IDが明示されたobjectを後で接続します。
            if (parent != objectJson.end() && !parent->is_null())
            {
                pendingParents.push_back({ &object, objectJson.value("parent", 0ll) });
            }
            // transformと各属性のJSON値です。
            const auto transform = objectJson.value("transform", Json::object());
            // position成分の一時配列です。
            const auto position = transform.value("position", std::vector<float>{});
            // rotation成分の一時配列です。
            const auto rotation = transform.value("rotation", std::vector<float>{});
            // scale成分の一時配列です。
            const auto scale = transform.value("scale", std::vector<float>{});
            // 3成分ある場合だけpositionを復元します。
            if (position.size() >= 3)
                object.GetTransform().position = { position[0], position[1], position[2] };
            // 3成分ある場合だけrotationを復元します。
            if (rotation.size() >= 3)
                object.GetTransform().rotation = { rotation[0], rotation[1], rotation[2] };
            // 3成分ある場合だけscaleを復元します。
            if (scale.size() >= 3)
                object.GetTransform().scale = { scale[0], scale[1], scale[2] };
            object.SetEnabled(objectJson.value("enabled", true));
            // 旧形式で物体直下に保存された可視設定を互換コンポーネントへ移します。
            const bool legacyAlwaysVisible =
                objectJson.value("alwaysVisible", false);
            const float legacyCullingMargin =
                objectJson.value("cullingMargin", 0.0f);
            if (legacyAlwaysVisible || legacyCullingMargin > 0.0f)
            {
                object.AddComponent<RenderCullingComponent>(
                    legacyAlwaysVisible,
                    legacyCullingMargin);
            }
            // object componentをtypeごとに復元します。
            for (const auto& component : objectJson.value("components", Json::array()))
            {
                // component種別名です。
                const std::string type = component.value("type", "");
                // NativeScript componentを復元します。
                if (type == "NativeScript")
                {
                    // 登録script IDからcomponentを追加します。
                    auto& script = object.AddComponent<NativeScriptComponent>(
                        component.value("script", ""));
                    script.SetEnabled(component.value("enabled", true));
                    if (prefabRoot != nullptr && script.Instance() == nullptr)
                    {
                        return false;
                    }
                    // script生成に成功した場合だけpropertiesを読み込みます。
                    if (script.Instance() != nullptr)
                    {
                        // 任意properties objectです。
                        const Json properties = component.value(
                            "properties", Json::object());
                        script.Instance()->LoadProperties(properties.dump());
                    }
                }
                // Camera componentを復元します。
                else if (type == "Camera")
                {
                    // Windowsランタイムと同じ既定値でcameraを追加します。
                    auto& camera = object.AddComponent<CameraComponent>(
                        component.value(
                            "verticalFieldOfView", DirectX::XM_PI / 4.0f),
                        component.value("nearPlane", 0.1f),
                        component.value("farPlane", 1000.0f));
                    camera.SetEnabled(component.value("enabled", true));
                }
                // directional light設定を後段へ保存します。
                else if (type == "DirectionalLight")
                {
                    directionalLights.push_back({
                        &object,
                        component.contains("color")
                            ? float3(component.at("color"),
                                { 1.0f, 0.96f, 0.88f })
                            : DirectX::XMFLOAT3{ 1.0f, 0.96f, 0.88f },
                        component.value("intensity", 1.0f),
                        component.value("enabled", true),
                    });
                }
                else if (type == "PointLight" || type == "SpotLight")
                {
                    const auto color = component.contains("color")
                        ? float3(component.at("color"), {1.0f, 1.0f, 1.0f})
                        : type == "SpotLight" ? DirectX::XMFLOAT3{1.0f, 0.88f, 0.68f}
                                              : DirectX::XMFLOAT3{1.0f, 0.72f, 0.42f};
                    PortableLocalLightComponent* light{};
                    if (type == "SpotLight")
                        light = &object.AddComponent<SpotLightComponent>(color,
                            component.value("intensity", 5.0f), component.value("range", 12.0f),
                            component.value("innerConeAngle", 0.3926991f), component.value("outerConeAngle", 0.6108652f));
                    else
                        light = &object.AddComponent<PointLightComponent>(color,
                            component.value("intensity", 3.0f), component.value("range", 8.0f));
                    light->SetEnabled(component.value("enabled", true));
                }
                // primitive mesh componentを復元します。
                else if (type == "MeshRenderer")
                {
                    // inline値またはmaterial assetを読むJSONです。
                    Json material = component;
                    // 任意のmaterial asset pathです。
                    const std::filesystem::path materialAsset(
                        component.value("materialAsset", ""));
                    // material asset指定時は外部JSONを読み込みます。
                    if (!materialAsset.empty())
                    {
                        // assetから読み込むmaterial JSONです。
                        Json loadedMaterial;
                        // 欠損または異なる型のassetはsceneを拒否します。
                        if (!LoadPortableJsonDocument(
                                materialAsset,
                                loadedMaterial)
                            || loadedMaterial.value("type", "")
                                != "LamaPonLitMaterial")
                        {
                            return false;
                        }
                        material = std::move(loadedMaterial);
                    }
                    // primitive名から対応する形状を選びます。
                    const std::string shapeName = component.value(
                        "shape", "Cube");
                    // 形状が省略または未知ならCubeを使います。
                    const PrimitiveShape shape = shapeName == "Plane"
                        ? PrimitiveShape::Plane
                        : shapeName == "Sphere"
                        ? PrimitiveShape::Sphere
                        : shapeName == "Cylinder"
                        ? PrimitiveShape::Cylinder
                        : PrimitiveShape::Cube;
                    // material未指定時の白色です。
                    DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
                    // baseColorを優先して色を読みます。
                    if (material.contains("baseColor"))
                    {
                        color = float4(material.at("baseColor"), color);
                    }
                    // baseColorがなければcolorを読みます。
                    else if (material.contains("color"))
                    {
                        color = float4(material.at("color"), color);
                    }
                    // scene material設定でmesh componentを生成します。
                    auto& mesh = object.AddComponent<MeshRendererComponent>(
                        shape,
                        color,
                        PortablePathFromUtf8(
                            material.value("albedoTexture", "")));
                    mesh.SetNormalTexturePath(PortablePathFromUtf8(
                        material.value("normalTexture", "")));
                    mesh.SetRoughness(material.value("roughness", 0.65f));
                    mesh.SetMetallic(material.value("metallic", 0.0f));
                    mesh.SetNormalStrength(material.value(
                        "normalStrength", 1.0f));
                    mesh.SetRoughnessTexturePath(PortablePathFromUtf8(
                        material.value("roughnessTexture", "")));
                    mesh.SetMetallicTexturePath(PortablePathFromUtf8(
                        material.value("metallicTexture", "")));
                    mesh.SetOcclusionTexturePath(PortablePathFromUtf8(
                        material.value("occlusionTexture", "")));
                    mesh.SetOcclusionStrength(material.value(
                        "occlusionStrength", 1.0f));
                    mesh.SetEmissiveTexturePath(PortablePathFromUtf8(
                        material.value("emissiveTexture", "")));
                    // emissive colorが指定された場合だけ反映します。
                    if (material.contains("emissiveColor"))
                    {
                        mesh.SetEmissiveColor(float3(
                            material.at("emissiveColor"), {}));
                    }
                    // culling mode名をrenderer enumへ変換します。
                    const std::string cullMode = component.value(
                        "cullMode", "Back");
                    mesh.SetCullMode(cullMode == "None"
                        ? ShaderCullMode::None
                        : cullMode == "Front"
                        ? ShaderCullMode::Front
                        : ShaderCullMode::Back);
                    mesh.SetEnabled(component.value("enabled", true));
                }
                // skeletal model componentと材質を復元します。
                else if (type == "ModelRenderer")
                {
                    // inline設定またはmaterial assetの内容です。
                    Json material = component;
                    // 任意のmaterial asset pathです。
                    const std::filesystem::path materialAsset(
                        component.value("materialAsset", ""));
                    // material asset指定時は外部JSONを読み込みます。
                    if (!materialAsset.empty())
                    {
                        // assetから読み込むmaterial JSONです。
                        Json loadedMaterial;
                        // 欠損または異なる型のassetはsceneを拒否します。
                        if (!LoadPortableJsonDocument(
                                materialAsset,
                                loadedMaterial)
                            || loadedMaterial.value("type", "")
                                != "LamaPonLitMaterial")
                        {
                            return false;
                        }
                        material = std::move(loadedMaterial);
                    }
                    // material色または白の既定配列です。
                    const auto color = material.value(
                        "color",
                        material.value(
                            "baseColor",
                            std::vector<float>{ 1.0f, 1.0f, 1.0f, 1.0f }));
                    // scene設定からmodel componentを生成します。
                    auto& model = object.AddComponent<ModelRendererComponent>(
                        PortablePathFromUtf8(component.value("model", "")),
                        component.value("wireframe", false),
                        !materialAsset.empty()
                            || component.value("materialOverride", false),
                        DirectX::XMFLOAT4{
                            color.size() > 0 ? color[0] : 1.0f,
                            color.size() > 1 ? color[1] : 1.0f,
                            color.size() > 2 ? color[2] : 1.0f,
                            color.size() > 3 ? color[3] : 1.0f,
                        },
                        PortablePathFromUtf8(
                            material.value("albedoTexture", "")),
                        PortablePathFromUtf8(
                            material.value("normalTexture", "")),
                        material.value("roughness", 0.5f),
                        material.value("normalStrength", 1.0f));
                    model.SetAnimationIndex(component.value(
                        "animationIndex", std::size_t{}));
                    model.SetAnimationSpeed(component.value(
                        "animationSpeed", 1.0f));
                    model.SetAnimationLoop(component.value(
                        "animationLoop", true));
                    model.SetAnimationPlayOnStart(component.value(
                        "animationPlayOnStart", true));
                    model.SetMetallic(material.value("metallic", 0.0f));
                    model.SetRoughnessTexturePath(PortablePathFromUtf8(
                        material.value("roughnessTexture", "")));
                    model.SetMetallicTexturePath(PortablePathFromUtf8(
                        material.value("metallicTexture", "")));
                    model.SetOcclusionTexturePath(PortablePathFromUtf8(
                        material.value("occlusionTexture", "")));
                    model.SetOcclusionStrength(material.value(
                        "occlusionStrength", 1.0f));
                    model.SetEmissiveTexturePath(PortablePathFromUtf8(
                        material.value("emissiveTexture", "")));
                    // emissive color指定がある場合だけ上書きします。
                    if (material.contains("emissiveColor"))
                    {
                        model.SetEmissiveColor(float3(
                            material.at("emissiveColor"), {}));
                    }
                    model.SetEnabled(component.value("enabled", true));
                }
                // 3D box collider componentを復元します。
                else if (type == "BoxCollider3D")
                {
                    // collider shapeとoffsetを持つcomponentです。
                    auto& collider = object.AddComponent<BoxCollider3DComponent>(
                        component.contains("size")
                            ? float3(component.at("size"), { 1.0f, 1.0f, 1.0f })
                            : DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                        component.contains("offset")
                            ? float3(component.at("offset"), {})
                            : DirectX::XMFLOAT3{});
                    collider.SetLayer(component.value("layer", 0u));
                    collider.SetCollisionMask(component.value(
                        "mask", 0xffffffffu));
                    collider.SetTrigger(component.value("trigger", false));
                    collider.SetEnabled(component.value("enabled", true));
                }
                // Rigidbody componentを復元します。
                else if (type == "Rigidbody")
                {
                    // objectへ追加したbodyです。
                    auto& body = object.AddComponent<RigidbodyComponent>();
                    body.SetKinematic(component.value("kinematic", false));
                    body.SetUseGravity(component.value("useGravity", true));
                    // velocityが指定された場合だけ初期値を反映します。
                    if (component.contains("velocity"))
                    {
                        body.SetVelocity(float3(component.at("velocity"), {}));
                    }
                    body.SetEnabled(component.value("enabled", true));
                }
                // ParticleSystem componentを復元します。
                else if (type == "ParticleSystem")
                {
                    // emitter shape名です。
                    const std::string shapeName = component.value(
                        "shape", "Point");
                    // 不明または未指定shapeはconeを使います。
                    const ParticleEmitterShape shape = shapeName == "Sphere"
                        ? ParticleEmitterShape::Sphere
                        : shapeName == "Box"
                        ? ParticleEmitterShape::Box
                        : ParticleEmitterShape::Cone;
                    // component設定からparticle systemを生成します。
                    auto& particles = object.AddComponent<ParticleSystemComponent>(
                        component.value("maxParticles", 256u),
                        component.value("emissionRate", 0.0f),
                        component.contains("lifetime")
                            ? float2(component.at("lifetime"), { 1.0f, 1.0f })
                            : DirectX::XMFLOAT2{ 1.0f, 1.0f },
                        component.contains("startSpeed")
                            ? float2(component.at("startSpeed"), {})
                            : DirectX::XMFLOAT2{},
                        component.contains("startSize")
                            ? float2(component.at("startSize"), { 1.0f, 1.0f })
                            : DirectX::XMFLOAT2{ 1.0f, 1.0f },
                        component.contains("startColor")
                            ? float4(component.at("startColor"),
                                { 1.0f, 1.0f, 1.0f, 1.0f })
                            : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
                        component.contains("endColor")
                            ? float4(component.at("endColor"),
                                { 1.0f, 1.0f, 1.0f, 0.0f })
                            : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 0.0f },
                        shape,
                        PortablePathFromUtf8(component.value("texture", "")));
                    // gravity指定がある場合だけ反映します。
                    if (component.contains("gravity"))
                    {
                        particles.SetGravity(float3(component.at("gravity"), {}));
                    }
                    particles.SetEndSizeMultiplier(component.value(
                        "endSizeMultiplier", 0.15f));
                    particles.SetRenderMode(
                        component.value("renderMode", "Billboard")
                                == "Horizontal"
                            ? ParticleRenderMode::Horizontal
                            : ParticleRenderMode::Billboard);
                    // emitter size指定がある場合だけ反映します。
                    if (component.contains("emitterSize"))
                    {
                        particles.SetEmitterSize(float3(
                            component.at("emitterSize"),
                            { 1.0f, 1.0f, 1.0f }));
                    }
                    particles.SetConeAngle(component.value(
                        "coneAngle", 0.4363323f));
                    particles.SetDuration(component.value(
                        "duration", 5.0f));
                    particles.SetLooping(component.value(
                        "looping", true));
                    particles.SetAdditive(component.value(
                        "additive", true));
                    // playOnStart=falseの場合は自動開始を止めます。
                    if (!component.value("playOnStart", true))
                    {
                        particles.SetPlayOnStart(false);
                    }
                    particles.SetEnabled(component.value("enabled", true));
                }
                // AudioSource componentを復元します。
                else if (type == "AudioSource")
                {
                    // scene設定からaudio sourceを生成します。
                    auto& audio = object.AddComponent<AudioSourceComponent>(
                        PortablePathFromUtf8(component.value("audio", "")),
                        component.value("volume", 1.0f));
                    audio.SetLoop(component.value("loop", false));
                    audio.SetPitch(component.value("pitch", 0.0f));
                    audio.SetPan(component.value("pan", 0.0f));
                    // 保存した音声バスをcomponentへ保持します。
                    const int audioBus = component.value(
                        "bus", static_cast<int>(AudioBus::Effects));
                    if (audioBus < 0
                        || audioBus >= static_cast<int>(AudioBus::Count))
                    {
                        return false;
                    }
                    audio.SetBus(static_cast<AudioBus>(audioBus));
                    audio.SetSpatial(component.value("spatial", false));
                    audio.SetMinimumDistance(component.value(
                        "minimumDistance", 1.0f));
                    audio.SetMaximumDistance(component.value(
                        "maximumDistance", 20.0f));
                    audio.m_playOnStart = component.value("playOnStart", false);
                    audio.SetEnabled(component.value("enabled", true));
                }
                // TransformAnimator componentを復元します。
                else if (type == "TransformAnimator")
                {
                    // Portable未対応controller pathです。
                    const std::filesystem::path controller(
                        component.value("controller", ""));
                    // controller指定はPortableでは読み込めません。
                    if (!controller.empty())
                    {
                        return false;
                    }
                    // scene設定からtransform animatorを生成します。
                    auto& animator =
                        object.AddComponent<TransformAnimatorComponent>(
                            PortablePathFromUtf8(
                                component.value("clip", "")),
                            component.value("speed", 1.0f),
                            component.value("loop", true),
                            component.value("playOnStart", true));
                    // 読み込みに失敗したclipを持つsceneは拒否します。
                    if (!animator.LoadPortableClip())
                    {
                        return false;
                    }
                    animator.SetEnabled(component.value("enabled", true));
                }
                // Rotator componentを復元します。
                else if (type == "Rotator")
                {
                    // angular velocityを持つrotator componentです。
                    auto& rotator = object.AddComponent<RotatorComponent>(
                        component.contains("angularVelocity")
                            ? float3(component.at("angularVelocity"),
                                { 0.0f, 1.0f, 0.0f })
                            : DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f });
                    rotator.SetEnabled(component.value("enabled", true));
                }
                // InputMover componentを復元します。
                else if (type == "InputMover")
                {
                    // action名と速度を持つmover componentです。
                    auto& mover = object.AddComponent<InputMoverComponent>(
                        component.value("horizontalAction", "MoveHorizontal"),
                        component.value("verticalAction", "MoveVertical"),
                        component.value("speed", 3.0f));
                    mover.SetEnabled(component.value("enabled", true));
                }
                // UI rect anchor設定を復元します。
                else if (type == "UIRectTransform")
                {
                    // Windows版のSceneに保存されたアンカー設定を読み込み、Web版でも同じビューポート基準のUI配置へ復元します。
                    // scene設定からrect transformを生成します。
                    auto& rect = object.AddComponent<UIRectTransformComponent>(
                        component.contains("anchorMin")
                            ? float2(component.at("anchorMin"),
                                { 0.5f, 0.5f })
                            : DirectX::XMFLOAT2{ 0.5f, 0.5f },
                        component.contains("anchorMax")
                            ? float2(component.at("anchorMax"),
                                { 0.5f, 0.5f })
                            : DirectX::XMFLOAT2{ 0.5f, 0.5f },
                        component.contains("pivot")
                            ? float2(component.at("pivot"),
                                { 0.5f, 0.5f })
                            : DirectX::XMFLOAT2{ 0.5f, 0.5f },
                        component.contains("anchoredPosition")
                            ? float2(component.at("anchoredPosition"), {})
                            : DirectX::XMFLOAT2{},
                        component.contains("sizeDelta")
                            ? float2(component.at("sizeDelta"),
                                { 220.0f, 56.0f })
                            : DirectX::XMFLOAT2{ 220.0f, 56.0f });
                    rect.SetEnabled(component.value("enabled", true));
                }
                else if (type == "UICanvas")
                {
                    auto& canvas = object.AddComponent<UICanvasComponent>(
                        component.contains("referenceResolution") ? float2(component.at("referenceResolution"), {1280,720}) : DirectX::XMFLOAT2{1280,720},
                        component.value("matchWidthOrHeight", 0.5f));
                    canvas.SetEnabled(component.value("enabled", true));
                }
                else if (type == "UIImage" || type == "UIButton")
                {
                    PortableUIVisualComponent* image{};
                    if (type == "UIButton")
                    {
                        auto& button = object.AddComponent<UIButtonComponent>(component.value("label", std::string{"ボタン"}));
                        button.SetFontFamily(component.value("fontFamily", std::string{"Yu Gothic UI"}));
                        button.SetFontSize(component.value("fontSize", 24.0f));
                        button.SetInteractable(component.value("interactable", true));
                        button.SetNavigationEnabled(component.value("navigationEnabled", true));
                        button.SetCircularHitArea(component.value("circularHitArea", false));
                        button.SetReloadCurrentScene(component.value("reloadCurrentScene", false));
                        button.SetClickEventName(
                            component.value("clickEvent", std::string{}));
                        button.SetTargetScene(PortablePathFromUtf8(
                            component.value("targetScene", std::string{})));
                        if (component.contains("normalColor")) button.SetNormalColor(float4(component.at("normalColor"), {1,1,1,1}));
                        if (component.contains("hoveredColor")) button.SetHoveredColor(float4(component.at("hoveredColor"), {1,1,1,1}));
                        if (component.contains("pressedColor")) button.SetPressedColor(float4(component.at("pressedColor"), {1,1,1,1}));
                        if (component.contains("disabledColor")) button.SetDisabledColor(float4(component.at("disabledColor"), {1,1,1,1}));
                        if (component.contains("textColor")) button.SetTextColor(float4(component.at("textColor"), {1,1,1,1}));
                        image = &button;
                    }
                    else image = &object.AddComponent<UIImageComponent>();
                    image->SetTexturePath(PortablePathFromUtf8(component.value("texture", "")));
                    image->SetSortOrder(component.value("sortOrder", 0));
                    image->SetEnabled(component.value("enabled", true));
                    if (component.contains("fallbackSize")) image->SetFallbackSize(float2(component.at("fallbackSize"), {220,56}));
                    if (component.contains("color")) image->SetColor(float4(component.at("color"), {1,1,1,1}));
                }
                // TextRenderer componentを復元します。
                else if (type == "TextRenderer")
                {
                    // 横alignment名です。
                    const std::string horizontalName = component.value(
                        "horizontalAlignment", "Left");
                    // 縦alignment名です。
                    const std::string verticalName = component.value(
                        "verticalAlignment", "Top");
                    // scene設定からtext componentを生成します。
                    auto& text = object.AddComponent<TextRendererComponent>(
                        component.value("text", ""),
                        component.value("fontFamily", "sans-serif"),
                        component.value("fontSize", 24.0f),
                        component.contains("color")
                            ? float4(component.at("color"),
                                { 1.0f, 1.0f, 1.0f, 1.0f })
                            : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
                        component.contains("layoutSize")
                            ? float2(component.at("layoutSize"), {})
                            : DirectX::XMFLOAT2{},
                        component.value("wordWrap", false),
                        horizontalName == "Center"
                            ? TextHorizontalAlignment::Center
                            : horizontalName == "Right"
                            ? TextHorizontalAlignment::Right
                            : TextHorizontalAlignment::Left,
                        verticalName == "Center"
                            ? TextVerticalAlignment::Center
                            : verticalName == "Bottom"
                            ? TextVerticalAlignment::Bottom
                            : TextVerticalAlignment::Top);
                    text.SetSortOrder(component.value("sortOrder", 0));
                    text.SetFontAsset(PortablePathFromUtf8(
                        component.value("fontAsset", "")));
                    text.SetEnabled(component.value("enabled", true));
                }
                // SpriteRenderer componentを復元します。
                else if (type == "SpriteRenderer")
                {
                    // scene設定からsprite componentを生成します。
                    auto& sprite = object.AddComponent<SpriteRendererComponent>(
                        component.contains("size")
                            ? float2(component.at("size"), { 128.0f, 128.0f })
                            : DirectX::XMFLOAT2{ 128.0f, 128.0f },
                        component.contains("color")
                            ? float4(component.at("color"),
                                { 1.0f, 1.0f, 1.0f, 1.0f })
                            : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
                        PortablePathFromUtf8(component.value("texture", "")));
                    // pivot指定がある場合だけ反映します。
                    if (component.contains("pivot"))
                    {
                        sprite.SetPivot(float2(component.at("pivot"), {}));
                    }
                    // source rect指定がある場合だけ反映します。
                    if (component.contains("sourceRect"))
                    {
                        sprite.SetSourceRect(float4(
                            component.at("sourceRect"),
                            { 0.0f, 0.0f, 1.0f, 1.0f }));
                    }
                    sprite.SetMaskInteraction(static_cast<SpriteMaskInteraction>(
                        std::clamp(component.value("maskInteraction", 0), 0, 2)));
                    sprite.SetSortOrder(component.value("sortOrder", 0));
                    sprite.SetEnabled(component.value("enabled", true));
                }
                // SpriteMask componentを復元します。
                else if (type == "SpriteMask")
                {
                    // shapeとsizeを持つmask componentです。
                    auto& mask = object.AddComponent<SpriteMaskComponent>(
                        component.value("shape", "Rectangle") == "Circle"
                            ? SpriteMaskShape::Circle
                            : SpriteMaskShape::Rectangle,
                        component.contains("size")
                            ? float2(component.at("size"), { 128.0f, 128.0f })
                            : DirectX::XMFLOAT2{ 128.0f, 128.0f });
                    mask.SetEnabled(component.value("enabled", true));
                }
                // SpriteAnimator componentを復元します。
                else if (type == "SpriteAnimator")
                {
                    // sheet gridを持つsprite animatorです。
                    auto& animator = object.AddComponent<SpriteAnimatorComponent>(
                        component.value("columns", 1),
                        component.value("rows", 1));
                    animator.SetSpeed(component.value("speed", 1.0f));
                    animator.SetDefaultClip(component.value(
                        "defaultClip", std::string{}));
                    animator.SetPlayOnStart(component.value(
                        "playOnStart", true));
                    // JSONのclip配列をsprite animatorへ登録します。
                    for (const auto& clip : component.value(
                        "clips", Json::array()))
                    {
                        // clipのframe範囲と再生設定です。
                        animator.AddClip({
                            clip.value("name", std::string{}),
                            clip.value("startFrame", 0),
                            clip.value("frameCount", 1),
                            clip.value("framesPerSecond", 10.0f),
                            clip.value("loop", true),
                        });
                    }
                    animator.SetEnabled(component.value("enabled", true));
                }
                // ParallaxLayer componentを復元します。
                else if (type == "ParallaxLayer")
                {
                    // 移動係数と参照IDを持つcomponentです。
                    auto& parallax = object.AddComponent<ParallaxLayerComponent>(
                        component.contains("factor")
                            ? float2(component.at("factor"), { 0.5f, 0.5f })
                            : DirectX::XMFLOAT2{ 0.5f, 0.5f },
                        component.value("referenceId", GameObjectId{}));
                    parallax.SetEnabled(component.value("enabled", true));
                }
                // RenderCulling componentを復元します。
                else if (type == "RenderCulling")
                {
                    // 可視設定とculling余白を持つcomponentです。
                    auto& culling = object.AddComponent<RenderCullingComponent>(
                        component.value("alwaysVisible", false),
                        component.value("cullingMargin", 0.0f));
                    culling.SetEnabled(component.value("enabled", true));
                }
                // Web Audio listenerはruntime側の単一listenerを使います。
                else if (type == "AudioListener")
                {
                    // Web AudioはAudioContextごとにListenerを1つ管理します。
                }
            }
        }
        // object生成後にsource IDからparent参照を解決します。
        for (const PendingParent& pending : pendingParents)
        {
            // parent source IDの検索結果です。
            const auto found = bySourceId.find(pending.parent);
            // parent objectが存在する場合だけ接続します。
            if (found != bySourceId.end())
            {
                pending.object->SetParent(found->second);
            }
        }
        if (prefabRoot != nullptr)
        {
            const auto found = bySourceId.find(prefabRootId);
            if (found == bySourceId.end())
            {
                return false;
            }
            loadedPrefabRoot = found->second;
            if (prefabParent != nullptr)
            {
                loadedPrefabRoot->SetParent(prefabParent);
            }
        }

        if (restoreEnvironment)
        {
            // scene JSONに指定されたmain camera source IDです。
            const auto mainCameraId = document.value("mainCamera", 0ll);
            // 指定IDがcamera componentを持つ場合に採用します。
            if (const auto camera = bySourceId.find(mainCameraId);
                camera != bySourceId.end()
                && camera->second->GetComponent<CameraComponent>() != nullptr)
            {
                m_impl->mainCamera = camera->second;
            }
            // main camera未指定なら最初のcamera componentを選びます。
            if (m_impl->mainCamera == nullptr)
            {
                // この文書で新しく読んだobjectからcameraを探します。
                for (std::size_t objectIndex = objectCountBeforeLoad;
                     objectIndex < m_objects.size();
                     ++objectIndex)
                {
                    // 最初に見つかったcameraをmain cameraにします。
                    if (m_objects[objectIndex]->GetComponent<CameraComponent>()
                        != nullptr)
                    {
                        m_impl->mainCamera = m_objects[objectIndex].get();
                        break;
                    }
                }
            }
        }
        // parallaxの参照source IDをobject pointerへ解決します。
        for (std::size_t objectIndex = objectCountBeforeLoad;
             objectIndex < m_objects.size();
             ++objectIndex)
        {
            const auto& object = m_objects[objectIndex];
            // 外部参照を持つparallax componentだけ解決します。
            if (auto* parallax = object->GetComponent<ParallaxLayerComponent>();
                parallax != nullptr && parallax->m_referenceSourceId != 0)
            {
                // source IDから参照objectを検索します。
                const auto reference = bySourceId.find(
                    static_cast<std::int64_t>(parallax->m_referenceSourceId));
                parallax->m_reference = reference != bySourceId.end()
                    ? reference->second
                    : nullptr;
            }
        }

        if (restoreEnvironment)
        {
        // scene環境設定のJSON objectです。
        const auto environment = document.value("environment", Json::object());
        // ambient color成分の配列です。
        const auto ambient = environment.value("ambientColor", std::vector<float>{});
        // fog設定のJSON objectです。
        const auto fog = environment.value("fog", Json::object());
        // fog color成分の配列です。
        const auto fogColor = fog.value("color", std::vector<float>{});
        // 環境値をすべて読み終えてからrenderer stateを変更します。
        const bool fogEnabled = fog.value("enabled", false);
        const float fogStartDistance = fog.value("startDistance", 110.0f);
        const float fogEndDistance = fog.value("endDistance", 520.0f);
        const float ambientIntensity = environment.value("ambientIntensity", 0.52f);
        // sky設定のJSON objectです。
        const auto sky = environment.value("sky", Json::object());
        // sky top color成分の配列です。
        const auto top = sky.value("topColor", std::vector<float>{});
        // sky horizon color成分の配列です。
        const auto horizon = sky.value("horizonColor", std::vector<float>{});
        const bool skyEnabled = sky.value("enabled", false);
        m_renderer->SetFog({
            fogEnabled,
            fogColor.size() >= 3
                ? Web::Color{ fogColor[0], fogColor[1], fogColor[2], 1.0f }
                : Web::Color{},
            fogStartDistance,
            fogEndDistance,
        });
        // scene directional lightの既定方向です。
        Web::Vec3 lightDirection{ 0.6808f, -0.4078f, 0.6083f };
        // scene directional lightの既定色です。
        Web::Color lightColor{ 1.0f, 0.76f, 0.52f, 1.0f };
        // scene directional lightの既定強度です。
        float lightIntensity = 1.35f;
        // 有効なdirectional lightをlighting設定へ反映します。
        for (const auto& light : directionalLights)
        {
            // componentまたはownerが無効なら飛ばします。
            if (!light.enabled || !light.object->IsEnabled())
            {
                continue;
            }
            // light objectのworld transformです。
            const Mat4 world = ComputeWorldMatrix(*light.object);
            lightDirection = LamaPon::Web::Normalize({
                -world.values[8], -world.values[9], -world.values[10] });
            lightColor = {
                std::clamp(light.color.x, 0.0f, 1.0f),
                std::clamp(light.color.y, 0.0f, 1.0f),
                std::clamp(light.color.z, 0.0f, 1.0f),
                1.0f,
            };
            lightIntensity = std::clamp(light.intensity, 0.0f, 16.0f);
            break;
        }
        m_renderer->SetLighting({
            ambient.size() >= 3
                ? Web::Color{ ambient[0], ambient[1], ambient[2], 1.0f }
                : Web::Color{ 1.0f, 1.0f, 1.0f, 1.0f },
            ambientIntensity,
            lightDirection,
            lightColor,
            lightIntensity,
        });
        m_renderer->SetSky({
            skyEnabled,
            top.size() >= 3
                ? Web::Color{ top[0], top[1], top[2], 1.0f }
                : Web::Color{},
            horizon.size() >= 3
                ? Web::Color{ horizon[0], horizon[1], horizon[2], 1.0f }
                : Web::Color{},
        });
        // horizon colorが指定されていればclear colorへ反映します。
        if (horizon.size() >= 3)
        {
            m_impl->clearColor = { horizon[0], horizon[1], horizon[2], 1.0f };
        }
        }
        loadTransaction.release();
        if (prefabRoot != nullptr)
        {
            *prefabRoot = loadedPrefabRoot;
            PrepareScripts(false, false);
        }
        return true;
    }
    catch (const Json::exception&)
    {
        return false;
    }

    // Awake・有効状態通知・必要なStartをScene内Scriptへ適用します。
    void Scene::PrepareScripts(
        const bool startActiveScripts,
        const bool flushDestroyedObjects)
    {
        if (flushDestroyedObjects)
        {
            FlushDestroyedObjects();
        }
        for (std::size_t objectIndex{};
             objectIndex < m_objects.size();
             ++objectIndex)
        {
            GameObject& object = *m_objects[objectIndex];
            for (std::size_t componentIndex{};
                 componentIndex < object.Components().size();
                 ++componentIndex)
            {
                Component* component =
                    object.Components()[componentIndex].get();
                auto* native = dynamic_cast<NativeScriptComponent*>(component);
                Script* script = native != nullptr
                    ? native->Instance()
                    : nullptr;
                if (script != nullptr)
                {
                    if (!script->m_awake)
                    {
                        script->m_awake = true;
                        script->Awake();
                    }
                    const bool active = object.IsActiveInHierarchy()
                        && native->IsEnabled();
                    if (script->m_active != active)
                    {
                        script->m_active = active;
                        if (active)
                        {
                            script->OnEnable();
                        }
                        else
                        {
                            script->OnDisable();
                        }
                    }
                    const bool activeAfterNotification =
                        object.IsActiveInHierarchy() && native->IsEnabled();
                    if (startActiveScripts && activeAfterNotification
                        && !script->m_started)
                    {
                        script->m_started = true;
                        script->Start();
                    }
                }
                if (startActiveScripts
                    && object.IsActiveInHierarchy())
                {
                    if (auto* audio = dynamic_cast<AudioSourceComponent*>(
                        component);
                    audio != nullptr && audio->IsEnabled()
                            && audio->m_playOnStart
                            && !audio->m_playOnStartConsumed)
                    {
                        audio->m_playOnStartConsumed = true;
                        audio->Play();
                    }
                }
            }
        }
    }

    // Script instanceへ有効解除と破棄を通知します。
    void Scene::DestroyScripts(GameObject& gameObject) noexcept
    {
        for (const auto& component : gameObject.Components())
        {
            auto* native = dynamic_cast<NativeScriptComponent*>(
                component.get());
            auto* script = native != nullptr
                ? native->Instance()
                : nullptr;
            if (script == nullptr)
            {
                continue;
            }
            if (script->m_active)
            {
                script->m_active = false;
                try
                {
                    script->OnDisable();
                }
                catch (...)
                {
                    Logger::Instance().Error(
                        "Portable Script OnDisable threw during destruction.");
                }
            }
            if (script->m_awake)
            {
                script->m_awake = false;
                try
                {
                    script->OnDestroy();
                }
                catch (...)
                {
                    Logger::Instance().Error(
                        "Portable Script OnDestroy threw during destruction.");
                }
            }
        }
    }

    // enabled object上のscriptとplay-on-start音声を開始します。
    void Scene::StartScripts()
    {
        PrepareScripts(true);
    }

    // script fixed update、rigidbodyと3D接触を処理します(deltaTime: 固定step秒)
    void Scene::FixedUpdate(float deltaTime)
    {
        PrepareScripts(false);
        // enabled objectのnative script fixed updateを呼びます。
        for (const auto& object : m_objects)
        {
            // 無効objectのscriptは更新しません。
            if (!object->IsEnabled())
            {
                continue;
            }
            // enabled componentのfixed updateを呼びます。
            for (const auto& component : object->Components())
            {
                // 無効componentは更新しません。
                if (!component->IsEnabled())
                {
                    continue;
                }
                // native scriptが有効な場合にfixed updateを呼びます。
                if (auto* native = dynamic_cast<NativeScriptComponent*>(component.get());
                    native != nullptr && native->Instance() != nullptr)
                {
                    native->Instance()->FixedUpdate(deltaTime);
                }
            }
        }
        // enabled rigidbodyの速度とpositionを積分します。
        for (const auto& object : m_objects)
        {
            // 有効なobjectとrigidbodyだけを積分します。
            if (auto* body = object->GetComponent<RigidbodyComponent>();
                object->IsEnabled() && body != nullptr && body->IsEnabled())
            {
                // rigidbodyの現在速度です。
                auto velocity = body->Velocity();
                // 非kinematic gravity bodyへ重力を積分します。
                if (!body->m_kinematic && body->m_useGravity)
                {
                    velocity.y -= 9.80665f * deltaTime;
                    body->m_velocity = velocity;
                }
                object->GetTransform().position.x += velocity.x * deltaTime;
                object->GetTransform().position.y += velocity.y * deltaTime;
                object->GetTransform().position.z += velocity.z * deltaTime;
            }
        }

        struct Bounds final
        {
            // world spaceでのbox中心です。
            Vec3 center{};
            // world spaceでの各軸半径です。
            Vec3 half{};
        };
        // box colliderをworld AABBへ変換します(object: 所有者, collider: box形状)
        const auto boundsFor = [](const GameObject& object,
                                  const BoxCollider3DComponent& collider)
        {
            // 所有者のworld transformです。
            const Mat4 world = ComputeWorldMatrix(object);
            // collider sizeのローカル半径です。
            const Vec3 localHalf{
                std::abs(collider.m_size.x) * 0.5f,
                std::abs(collider.m_size.y) * 0.5f,
                std::abs(collider.m_size.z) * 0.5f,
            };
            return Bounds{
                TransformPoint(world, WebVector(collider.m_offset)),
                {
                    std::abs(world.values[0]) * localHalf.x
                        + std::abs(world.values[4]) * localHalf.y
                        + std::abs(world.values[8]) * localHalf.z,
                    std::abs(world.values[1]) * localHalf.x
                        + std::abs(world.values[5]) * localHalf.y
                        + std::abs(world.values[9]) * localHalf.z,
                    std::abs(world.values[2]) * localHalf.x
                        + std::abs(world.values[6]) * localHalf.y
                        + std::abs(world.values[10]) * localHalf.z,
                },
            };
        };
        enum class ContactPhase
        {
            Enter,
            Stay,
            Exit,
        };
        // 接触イベントを有効なnative scriptへ通知します(object: 所有者, other: 相手, 接触情報)
        const auto dispatchContact = [](GameObject& object,
                                        GameObject& other,
                                        const DirectX::XMFLOAT3& normal,
                                        const DirectX::XMFLOAT3& point,
                                        const float penetration,
                                        const bool isTrigger,
                                        const ContactPhase phase)
        {
            const CollisionEvent event{
                other,
                normal,
                point,
                penetration,
                isTrigger,
            };
            // owner componentへcollision callbackを通知します。
            for (const auto& component : object.Components())
            {
                // componentがnative scriptか確認します。
                auto* native = dynamic_cast<NativeScriptComponent*>(
                    component.get());
                // 無効・未生成scriptはcallback対象外です。
                if (native == nullptr || !native->IsEnabled()
                    || native->Instance() == nullptr)
                {
                    continue;
                }
                if (isTrigger)
                {
                    if (phase == ContactPhase::Enter)
                    {
                        native->Instance()->OnTriggerEnter(event);
                    }
                    else if (phase == ContactPhase::Stay)
                    {
                        native->Instance()->OnTriggerStay(event);
                    }
                    else
                    {
                        native->Instance()->OnTriggerExit(event);
                    }
                }
                else
                {
                    if (phase == ContactPhase::Enter)
                    {
                        native->Instance()->OnCollisionEnter(event);
                    }
                    else if (phase == ContactPhase::Stay)
                    {
                        native->Instance()->OnCollisionStay(event);
                    }
                    else
                    {
                        native->Instance()->OnCollisionExit(event);
                    }
                }
            }
        };

        // このfixed stepで検出した接触pair集合です。
        std::unordered_map<Impl::ContactKey, bool, Impl::ContactHash> contacts;
        // 1つ目のcollider候補を走査します。
        for (std::size_t firstIndex{}; firstIndex < m_objects.size(); ++firstIndex)
        {
            // 1つ目の接触候補objectです。
            GameObject& first = *m_objects[firstIndex];
            // 1つ目の3D box colliderです。
            auto* firstCollider = first.GetComponent<BoxCollider3DComponent>();
            // 無効objectまたはcolliderは判定しません。
            if (!first.IsEnabled() || firstCollider == nullptr
                || !firstCollider->IsEnabled())
            {
                continue;
            }
            // 2つ目のcollider候補を重複しない組合せで走査します。
            for (std::size_t secondIndex = firstIndex + 1;
                 secondIndex < m_objects.size(); ++secondIndex)
            {
                // 2つ目の接触候補objectです。
                GameObject& second = *m_objects[secondIndex];
                // 2つ目の3D box colliderです。
                auto* secondCollider = second.GetComponent<BoxCollider3DComponent>();
                // 無効objectまたはcolliderは判定しません。
                if (!second.IsEnabled() || secondCollider == nullptr
                    || !secondCollider->IsEnabled())
                {
                    continue;
                }
                // 1つ目colliderのbitmask indexです。
                const std::uint32_t firstLayer = firstCollider->m_layer % 32u;
                // 2つ目colliderのbitmask indexです。
                const std::uint32_t secondLayer = secondCollider->m_layer % 32u;
                // 双方のcollision maskが許可しないpairは除外します。
                if ((firstCollider->m_mask & (1u << secondLayer)) == 0
                    || (secondCollider->m_mask & (1u << firstLayer)) == 0)
                {
                    continue;
                }
                // 1つ目colliderのworld AABBです。
                const Bounds firstBounds = boundsFor(first, *firstCollider);
                // 2つ目colliderのworld AABBです。
                const Bounds secondBounds = boundsFor(second, *secondCollider);
                // box中心間のdeltaです。
                const Vec3 delta = firstBounds.center - secondBounds.center;
                // 各軸の重なり深さです。
                const Vec3 overlap{
                    firstBounds.half.x + secondBounds.half.x
                        - std::abs(delta.x),
                    firstBounds.half.y + secondBounds.half.y
                        - std::abs(delta.y),
                    firstBounds.half.z + secondBounds.half.z
                        - std::abs(delta.z),
                };
                // いずれかの軸で離れていれば非接触です。
                if (overlap.x <= 0.0f || overlap.y <= 0.0f
                    || overlap.z <= 0.0f)
                {
                    continue;
                }

                // 最小重なり軸で決める接触法線です。
                Vec3 normal{ delta.x < 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f };
                // 初期penetrationはx軸重なりです。
                float penetration = overlap.x;
                // y軸の重なりが小さければ法線をyへ移します。
                if (overlap.y < penetration)
                {
                    normal = { 0.0f, delta.y < 0.0f ? -1.0f : 1.0f, 0.0f };
                    penetration = overlap.y;
                }
                // z軸の重なりが小さければ法線をzへ移します。
                if (overlap.z < penetration)
                {
                    normal = { 0.0f, 0.0f, delta.z < 0.0f ? -1.0f : 1.0f };
                    penetration = overlap.z;
                }
                // object ID順で安定した接触識別子を作ります。
                const Impl::ContactKey key{
                    std::min(first.Id(), second.Id()),
                    std::max(first.Id(), second.Id()),
                };
                // いずれかがtriggerなら物理補正しません。
                const bool trigger = firstCollider->m_trigger
                    || secondCollider->m_trigger;
                contacts.insert_or_assign(key, trigger);
                // 直前stepの接触集合からenter/stayを判定します。
                const auto previous = m_impl->contacts.find(key);
                const auto phase = previous == m_impl->contacts.end()
                    ? ContactPhase::Enter
                    : ContactPhase::Stay;
                // 2つのbox中心の中間点です。
                const Vec3 point = (firstBounds.center + secondBounds.center)
                    * 0.5f;
                // 1つ目objectへ接触法線向きのeventを送ります。
                dispatchContact(
                    first,
                    second,
                    DirectXVector(normal),
                    DirectXVector(point),
                    penetration,
                    trigger,
                    phase);
                // 2つ目objectへ反対向きのeventを送ります。
                dispatchContact(
                    second,
                    first,
                    DirectXVector(normal * -1.0f),
                    DirectXVector(point),
                    penetration,
                    trigger,
                    phase);

                // 接触解決対象のrigidbodyです。
                auto* firstBody = first.GetComponent<RigidbodyComponent>();
                // 接触解決対象のもう一方のrigidbodyです。
                auto* secondBody = second.GetComponent<RigidbodyComponent>();
                // 1つ目bodyが有効なdynamic bodyかを示します。
                const bool firstDynamic = firstBody != nullptr
                    && firstBody->IsEnabled() && !firstBody->m_kinematic;
                // 2つ目bodyが有効なdynamic bodyかを示します。
                const bool secondDynamic = secondBody != nullptr
                    && secondBody->IsEnabled() && !secondBody->m_kinematic;
                // triggerまたは両方staticなら物理補正しません。
                if (trigger || (!firstDynamic && !secondDynamic))
                {
                    continue;
                }
                // 1つ目bodyへ配分する補正距離です。
                const float firstDistance = secondDynamic
                    ? penetration * 0.5f
                    : penetration;
                // 2つ目bodyへ配分する補正距離です。
                const float secondDistance = firstDynamic
                    ? penetration * 0.5f
                    : penetration;
                // dynamicな1つ目bodyを押し戻し法線速度を除去します。
                if (firstDynamic)
                {
                    // 1つ目body ownerのpositionです。
                    auto& position = first.GetTransform().position;
                    position.x += normal.x * firstDistance;
                    position.y += normal.y * firstDistance;
                    position.z += normal.z * firstDistance;
                    // 接触面へ向かう1つ目body速度成分です。
                    const float inward = firstBody->m_velocity.x * normal.x
                        + firstBody->m_velocity.y * normal.y
                        + firstBody->m_velocity.z * normal.z;
                    // 接触面へ進入する速度だけを除去します。
                    if (inward < 0.0f)
                    {
                        firstBody->m_velocity.x -= normal.x * inward;
                        firstBody->m_velocity.y -= normal.y * inward;
                        firstBody->m_velocity.z -= normal.z * inward;
                    }
                }
                // dynamicな2つ目bodyを反対向きに補正します。
                if (secondDynamic)
                {
                    // 2つ目body ownerのpositionです。
                    auto& position = second.GetTransform().position;
                    position.x -= normal.x * secondDistance;
                    position.y -= normal.y * secondDistance;
                    position.z -= normal.z * secondDistance;
                    // 2つ目body向きの接触法線です。
                    const Vec3 secondNormal = normal * -1.0f;
                    // 接触面へ向かう2つ目body速度成分です。
                    const float inward = secondBody->m_velocity.x * secondNormal.x
                        + secondBody->m_velocity.y * secondNormal.y
                        + secondBody->m_velocity.z * secondNormal.z;
                    // 接触面へ進入する速度だけを除去します。
                    if (inward < 0.0f)
                    {
                        secondBody->m_velocity.x -= secondNormal.x * inward;
                        secondBody->m_velocity.y -= secondNormal.y * inward;
                        secondBody->m_velocity.z -= secondNormal.z * inward;
                    }
                }
            }
        }
        // 前stepにはあった接触が終わった場合、現存objectへexitを通知します。
        for (const auto& [previous, wasTrigger] : m_impl->contacts)
        {
            if (contacts.contains(previous))
            {
                continue;
            }
            GameObject* first{};
            GameObject* second{};
            for (const auto& object : m_objects)
            {
                if (object->Id() == previous.first)
                {
                    first = object.get();
                }
                else if (object->Id() == previous.second)
                {
                    second = object.get();
                }
            }
            if (first == nullptr || second == nullptr)
            {
                continue;
            }
            const DirectX::XMFLOAT3 zero{};
            dispatchContact(
                *first,
                *second,
                zero,
                zero,
                0.0f,
                wasTrigger,
                ContactPhase::Exit);
            dispatchContact(
                *second,
                *first,
                zero,
                zero,
                0.0f,
                wasTrigger,
                ContactPhase::Exit);
        }
        m_impl->contacts = std::move(contacts);
    }

    // script・particle・animation・入力componentを更新します(deltaTime: frame秒)
    void Scene::Update(float deltaTime)
    {
        ProcessPendingSceneLoad();
        PrepareScripts(true);
        struct ButtonCandidate final
        {
            UIButtonComponent* button;
            GameObjectId owner;
            DirectX::XMFLOAT2 center;
        };
        std::vector<ButtonCandidate> navigation;
        // Script::Updateから同じフレームのクリックを参照できるよう先に更新します。
        for (const auto& object : m_objects)
            for (const auto& component : object->Components())
                if (auto* button = dynamic_cast<UIButtonComponent*>(component.get()))
                {
                    button->m_clicked = false;
                    button->m_focused = false;
                    if (!object->IsEnabled() || !button->IsEnabled() || !button->m_interactable)
                    { button->m_pressed = button->m_hovered = false; continue; }
                    const auto& pointer = m_graphics.Input().Pointer();
                    UIRect area;
                    if (const auto* rect = object->GetComponent<UIRectTransformComponent>())
                        area = rect->Resolve(m_graphics.UIWidth(), m_graphics.UIHeight());
                    else
                    {
                        const auto world = ComputeWorldMatrix(*object);
                        const auto size = button->FallbackSize();
                        area = {{world.values[12]-size.x*0.5f, world.values[13]-size.y*0.5f},
                                {world.values[12]+size.x*0.5f, world.values[13]+size.y*0.5f}};
                    }
                    const auto size = area.Size();
                    if (button->m_navigationEnabled && size.x > 0 && size.y > 0
                        && std::isfinite(area.minimum.x) && std::isfinite(area.minimum.y)
                        && std::isfinite(area.maximum.x) && std::isfinite(area.maximum.y))
                        navigation.push_back({button, object->Id(),
                            {area.minimum.x + size.x * 0.5f, area.minimum.y + size.y * 0.5f}});
                    const float x = pointer.position.x, y = pointer.position.y;
                    button->m_hovered = pointer.valid && size.x > 0 && size.y > 0
                        && x >= area.minimum.x && x <= area.maximum.x && y >= area.minimum.y && y <= area.maximum.y;
                    if (button->m_hovered && button->m_circular)
                    {
                        const float dx = (x - (area.minimum.x+size.x*0.5f)) / (size.x*0.5f);
                        const float dy = (y - (area.minimum.y+size.y*0.5f)) / (size.y*0.5f);
                        button->m_hovered = dx*dx + dy*dy <= 1.0f;
                    }
                    const auto& left = pointer.Button(PointerButton::Left);
                    if (left.pressed) button->m_pressed = button->m_hovered;
                    if (left.released) { button->m_clicked = button->m_pressed && button->m_hovered; button->m_pressed = false; }
                    if (!left.down && !left.released) button->m_pressed = false;
                }
        auto focused = std::find_if(navigation.begin(), navigation.end(), [&](const ButtonCandidate& candidate) {
            return candidate.owner == m_impl->focusedButtonOwner && candidate.button == m_impl->focusedButton;
        });
        const auto& input = m_graphics.Input();
        const auto& pointer = input.Pointer();
        if (pointer.pressed || std::abs(pointer.delta.x) > 1 || std::abs(pointer.delta.y) > 1
            || input.WasPressed("Cancel"))
            focused = navigation.end();
        else if (!navigation.empty())
        {
            const bool next = input.WasPressed("UINext"), previous = input.WasPressed("UIPrevious");
            const float dx = static_cast<float>(input.WasPressed("UIRight")) - static_cast<float>(input.WasPressed("UILeft"));
            const float dy = static_cast<float>(input.WasPressed("UIDown")) - static_cast<float>(input.WasPressed("UIUp"));
            if (next || previous)
            {
                if (focused == navigation.end()) focused = previous ? navigation.end() - 1 : navigation.begin();
                else
                {
                    const auto index = focused - navigation.begin();
                    const auto count = static_cast<std::ptrdiff_t>(navigation.size());
                    focused = navigation.begin() + (index + count + (previous ? -1 : 1)) % count;
                }
            }
            else if (dx != 0 || dy != 0)
            {
                if (focused == navigation.end()) focused = navigation.begin();
                else
                {
                    auto nearest = focused;
                    float best = std::numeric_limits<float>::infinity();
                    for (auto candidate = navigation.begin(); candidate != navigation.end(); ++candidate)
                    {
                        const float x = candidate->center.x - focused->center.x;
                        const float y = candidate->center.y - focused->center.y;
                        if (x * dx + y * dy <= 0.01f) continue;
                        const float lateral = x * dy - y * dx;
                        const float score = x*x + y*y + 3*lateral*lateral;
                        if (score < best) { best = score; nearest = candidate; }
                    }
                    focused = nearest;
                }
            }
            if (input.WasPressed("Submit") && focused != navigation.end())
            {
                focused->button->m_clicked = true;
            }
        }
        m_impl->focusedButton = focused != navigation.end() ? focused->button : nullptr;
        m_impl->focusedButtonOwner = focused != navigation.end() ? focused->owner : 0;
        if (focused != navigation.end()) focused->button->m_focused = true;
        // Pointer・keyboard・gamepadで確定したクリックを共通イベントとScene要求へ反映します。
        for (const auto& object : m_objects)
        {
            for (const auto& component : object->Components())
            {
                auto* button = dynamic_cast<UIButtonComponent*>(component.get());
                if (button == nullptr || !button->m_clicked) continue;
                if (!button->m_clickEventName.empty())
                {
                    EventArgs eventArgs;
                    eventArgs.sender = object.get();
                    m_events.Publish(button->m_clickEventName, eventArgs);
                }
                if (button->m_reloadCurrentScene)
                {
                    if (!m_scenes.RequestReload())
                    {
                        Logger::Instance().Error(
                            "UI Button scene reload failed: "
                            + m_scenes.LastError());
                    }
                }
                else if (!button->m_targetScene.empty()
                    && !m_scenes.RequestLoad(button->m_targetScene))
                {
                    Logger::Instance().Error(
                        "UI Button scene transition failed: "
                        + PortablePathToUtf8(button->m_targetScene)
                        + " | " + m_scenes.LastError());
                }
            }
        }
        // enabled scene objectだけを更新します。
        for (const auto& object : m_objects)
        {
            // 無効objectのcomponentは更新しません。
            if (!object->IsEnabled())
            {
                continue;
            }
            // object componentを順番に更新します。
            for (const auto& component : object->Components())
            {
                // 無効componentは更新しません。
                if (!component->IsEnabled())
                {
                    continue;
                }
                // native script instanceがある場合にUpdateを呼びます。
                if (auto* native = dynamic_cast<NativeScriptComponent*>(component.get());
                    native != nullptr && native->Instance() != nullptr)
                {
                    native->Instance()->Update(deltaTime);
                }
                // particle componentを更新します。
                if (auto* particles = dynamic_cast<ParticleSystemComponent*>(component.get()))
                {
                    // 発生中で有限durationがあるemitterを進めます。
                    if (particles->m_playing && particles->m_duration > 0.0f)
                    {
                        particles->m_emittingTime += deltaTime;
                        // duration到達時にloopまたは停止を処理します。
                        if (particles->m_emittingTime >= particles->m_duration)
                        {
                            // loop emitterはduration内へ折り返します。
                            if (particles->m_looping)
                            {
                                particles->m_emittingTime = std::fmod(
                                    particles->m_emittingTime,
                                    particles->m_duration);
                            }
                            // 非loop emitterは発生を停止します。
                            else
                            {
                                particles->m_playing = false;
                            }
                        }
                    }
                    // 発生中でrateが正なら整数個の粒子を発生します。
                    if (particles->m_playing && particles->m_emissionRate > 0.0f)
                    {
                        particles->m_emissionAccumulator +=
                            particles->m_emissionRate * deltaTime;
                        // accumulator内の整数部分を発生個数にします。
                        const int emissionCount = static_cast<int>(
                            particles->m_emissionAccumulator);
                        // 1個以上溜まった分をparticle poolへ追加します。
                        if (emissionCount > 0)
                        {
                            particles->m_emissionAccumulator -=
                                static_cast<float>(emissionCount);
                            particles->Emit(emissionCount);
                        }
                    }
                    // 各particleのage、速度、位置を積分します。
                    for (auto& particle : particles->m_particles)
                    {
                        particle.age += deltaTime;
                        particle.velocity.x += particles->m_gravity.x * deltaTime;
                        particle.velocity.y += particles->m_gravity.y * deltaTime;
                        particle.velocity.z += particles->m_gravity.z * deltaTime;
                        particle.position.x += particle.velocity.x * deltaTime;
                        particle.position.y += particle.velocity.y * deltaTime;
                        particle.position.z += particle.velocity.z * deltaTime;
                    }
                    // lifetimeに達したparticleを削除します。
                    std::erase_if(
                        particles->m_particles,
                        // 寿命切れparticleを判定します(particle: 粒子)
                        [](const auto& particle)
                        {
                            return particle.age >= particle.lifetime;
                        });
                }
                // model animationを進めます。
                if (auto* model = dynamic_cast<ModelRendererComponent*>(
                        component.get()))
                {
                    model->AdvancePortableAnimation(deltaTime);
                }
                // transform animation clipを進めます。
                if (auto* animator =
                        dynamic_cast<TransformAnimatorComponent*>(
                            component.get()))
                {
                    animator->AdvancePortableAnimation(deltaTime);
                }
                // rotatorの角速度をrotationへ積分します。
                if (auto* rotator = dynamic_cast<RotatorComponent*>(
                        component.get()))
                {
                    // 更新対象transformのrotationです。
                    auto& rotation = object->GetTransform().rotation;
                    rotation.x += rotator->m_angularVelocity.x * deltaTime;
                    rotation.y += rotator->m_angularVelocity.y * deltaTime;
                    rotation.z += rotator->m_angularVelocity.z * deltaTime;
                }
                // input moverのaction値をpositionへ反映します。
                if (auto* mover = dynamic_cast<InputMoverComponent*>(
                        component.get()))
                {
                    // 水平actionの値です。
                    float horizontal = m_graphics.Input().Value(
                        mover->m_horizontalAction);
                    // 垂直actionの値です。
                    float vertical = m_graphics.Input().Value(
                        mover->m_verticalAction);
                    // 2軸入力の合成magnitudeです。
                    const float magnitude = std::sqrt(
                        horizontal * horizontal + vertical * vertical);
                    // 斜め入力の移動速度を正規化します。
                    if (magnitude > 1.0f)
                    {
                        horizontal /= magnitude;
                        vertical /= magnitude;
                    }
                    object->GetTransform().position.x +=
                        horizontal * mover->m_speed * deltaTime;
                    object->GetTransform().position.z -=
                        vertical * mover->m_speed * deltaTime;
                }
                // Sprite sheet animationを進めます。
                if (auto* spriteAnimator =
                        dynamic_cast<SpriteAnimatorComponent*>(component.get()))
                {
                    spriteAnimator->Advance(deltaTime);
                }
                // parallax位置を参照objectに同期します。
                if (auto* parallax = dynamic_cast<ParallaxLayerComponent*>(
                        component.get()))
                {
                    parallax->Advance(m_impl->mainCamera);
                }
#if LAMAPON_WEB_AUDIO_ENABLED
                // 空間audio sourceの位置を更新します。
                if (auto* audio = dynamic_cast<AudioSourceComponent*>(
                        component.get());
                    audio != nullptr && audio->m_spatial
                        && audio->m_handle != 0)
                {
                    // ownerのworld位置を取得します。
                    const Mat4 world = ComputeWorldMatrix(*object);
                    m_audio->SetPosition(
                        audio->m_handle,
                        world.values[12],
                        world.values[13],
                        world.values[14]);
                }
#endif
            }
        }
        // 通常更新とフレーム内コンポーネント処理をすべて終えてからLateUpdateを呼びます。
        for (const auto& object : m_objects)
        {
            if (!object->IsEnabled())
            {
                continue;
            }
            for (const auto& component : object->Components())
            {
                if (!component->IsEnabled())
                {
                    continue;
                }
                auto* native = dynamic_cast<NativeScriptComponent*>(
                    component.get());
                auto* script = native != nullptr
                    ? native->Instance()
                    : nullptr;
                if (script != nullptr && script->m_started)
                {
                    script->LateUpdate(deltaTime);
                }
            }
        }
    }

    // scene objectをworld描画しPortable UIを同期します。
    void Scene::Render()
    {
        std::vector<Web::LocalLight3D> localLights;
        localLights.reserve(8);
        for (const auto& object : m_objects)
        {
            if (!object->IsEnabled()) continue;
            for (const auto& component : object->Components())
            {
                auto* light = dynamic_cast<PortableLocalLightComponent*>(component.get());
                if (!light || !light->IsEnabled() || localLights.size() >= 8) continue;
                Web::LocalLight3D value;
                value.position = WebVector(light->WorldPosition());
                const auto color = light->Color();
                value.color = {color.x, color.y, color.z, 1.0f};
                value.intensity = light->Intensity();
                value.range = light->Range();
                if (auto* spot = dynamic_cast<SpotLightComponent*>(light))
                {
                    value.spot = true;
                    value.direction = WebVector(spot->WorldDirection());
                    value.innerConeAngle = spot->InnerConeAngle();
                    value.outerConeAngle = spot->OuterConeAngle();
                }
                localLights.push_back(value);
            }
        }
        m_renderer->SetLocalLights(localLights);
        BeginPortableUiFrame();
        // camera未指定時の右方向です。
        Vec3 cameraRight{ 1.0f, 0.0f, 0.0f };
        // camera未指定時の上方向です。
        Vec3 cameraUp{ 0.0f, 1.0f, 0.0f };
        // main cameraまたはscene内の最初のcameraです。
        if (auto* camera = m_impl->mainCamera != nullptr
                ? m_impl->mainCamera->GetComponent<CameraComponent>()
                : FindComponentOfType<CameraComponent>())
        {
            // camera ownerのworld transformです。
            const Mat4 world = ComputeWorldMatrix(camera->Owner());
            // camera world位置です。
            const Vec3 position{
                world.values[12], world.values[13], world.values[14] };
            // cameraが向くworld方向です。
            const Vec3 forward = LamaPon::Web::Normalize({
                -world.values[8], -world.values[9], -world.values[10] });
            // cameraのworld右方向です。
            cameraRight = LamaPon::Web::Normalize({
                world.values[0], world.values[1], world.values[2] });
            // cameraのworld上方向です。
            cameraUp = LamaPon::Web::Normalize({
                world.values[4], world.values[5], world.values[6] });
#if LAMAPON_WEB_AUDIO_ENABLED
            m_audio->SetListener(
                position.x, position.y, position.z,
                forward.x, forward.y, forward.z,
                cameraUp.x, cameraUp.y, cameraUp.z);
#endif
            m_renderer->SetCamera({
                position,
                position + forward,
                { 0.0f, 1.0f, 0.0f },
                camera->m_fieldOfView,
                camera->m_nearPlane,
                camera->m_farPlane,
            });
        }
        m_renderer->BeginFrame(WebColor(m_impl->clearColor));
        // 有効objectをworld transformで描画します。
        for (const auto& object : m_objects)
        {
            // 無効objectは描画しません。
            if (!object->IsEnabled())
            {
                continue;
            }
            // objectのworld model matrixです。
            const Mat4 model = ComputeWorldMatrix(*object);
            // 各componentの描画処理を行います。
            for (const auto& component : object->Components())
            {
                // 無効componentは描画しません。
                if (!component->IsEnabled())
                {
                    continue;
                }
                // procedural mesh componentを描画します。
                if (auto* mesh = dynamic_cast<MeshRendererComponent*>(component.get()))
                {
                    // CPU頂点変更時またはGPU mesh未作成時にbufferを更新します。
                    if (mesh->m_dirty || mesh->m_webMesh == 0)
                    {
                        // GPU vertex buffer形式へ変換します。
                        const auto vertices = WebVertices(mesh->m_vertices);
                        // GPU index buffer形式へ変換します。
                        const auto indices = WebIndices(mesh->m_indices);
                        // 初回はmeshを生成します。
                        if (mesh->m_webMesh == 0)
                            mesh->m_webMesh = m_renderer->CreateMesh(vertices, indices);
                        // 既存meshはbuffer内容を更新します。
                        else
                            m_renderer->UpdateMesh(mesh->m_webMesh, vertices, indices);
                        mesh->m_dirty = false;
                    }
                    // 指定albedo textureが未作成なら読み込みます。
                    if (mesh->m_webTexture == 0 && !mesh->m_albedo.empty())
                    {
                        // Web rendererへ渡す仮想asset pathです。
                        const std::string path = VirtualAssetPath(mesh->m_albedo);
                        mesh->m_webTexture = m_renderer->CreateTexture(path.c_str());
                    }
                    // 指定normal textureが未作成なら読み込みます。
                    if (mesh->m_webNormalTexture == 0 && !mesh->m_normal.empty())
                    {
                        // Web rendererへ渡す仮想asset pathです。
                        const std::string path = VirtualAssetPath(mesh->m_normal);
                        mesh->m_webNormalTexture = m_renderer->CreateTexture(
                            path.c_str());
                    }
                    // 任意material textureを初回だけ生成します(id: GPU texture, asset: source path)
                    const auto loadMaterialTexture = [this](
                        std::uint32_t& id,
                        const std::filesystem::path& asset)
                    {
                        // handle未作成でasset指定がある場合だけ読み込みます。
                        if (id == 0 && !asset.empty())
                        {
                            // Web rendererへ渡す仮想asset pathです。
                            const std::string path = VirtualAssetPath(asset);
                            id = m_renderer->CreateTexture(path.c_str());
                        }
                    };
                    loadMaterialTexture(
                        mesh->m_webRoughnessTexture,
                        mesh->m_roughnessTexture);
                    loadMaterialTexture(
                        mesh->m_webMetallicTexture,
                        mesh->m_metallicTexture);
                    loadMaterialTexture(
                        mesh->m_webOcclusionTexture,
                        mesh->m_occlusionTexture);
                    loadMaterialTexture(
                        mesh->m_webEmissiveTexture,
                        mesh->m_emissiveTexture);
                    m_renderer->DrawMesh(
                        mesh->m_webMesh,
                        model,
                        WebColor(mesh->m_color),
                        mesh->m_roughness,
                        mesh->m_webTexture,
                        mesh->m_cullMode == ShaderCullMode::None,
                        mesh->m_color.w < 0.999f,
                        -1.0f,
                        mesh->m_webNormalTexture,
                        mesh->m_normalStrength,
                        mesh->m_metallic,
                        0,
                        mesh->m_webRoughnessTexture,
                        mesh->m_webMetallicTexture,
                        mesh->m_webOcclusionTexture,
                        mesh->m_occlusionStrength,
                        mesh->m_webEmissiveTexture,
                        WebColor({
                            mesh->m_emissiveColor.x,
                            mesh->m_emissiveColor.y,
                            mesh->m_emissiveColor.z,
                            1.0f }));
                }
                // imported modelを読み込み透明順で描画します。
                else if (auto* imported =
                             dynamic_cast<ModelRendererComponent*>(component.get()))
                {
                    // 初回render時にPortable用modelを読み込みます。
                    if (!imported->m_loaded)
                    {
                        // 読み込み結果はcomponent状態へ反映されます。
                        const bool loaded = imported->LoadPortableModel();
                        (void)loaded;
                    }
                    // 不透明Geometryを先に描いてDepth Bufferを確定します。
                    // WindowやDecalのAlpha Blendにより、後続の不透明な車体が半透明に見える問題を防ぎます。
                    // opaque pass後にtransparent passを描画します。
                    for (int blendPass{}; blendPass < 2; ++blendPass)
                    {
                        // model primitiveごとにGPU資源とdrawを処理します。
                        for (auto& part : imported->m_parts)
                        {
                            // alpha blendまたはalpha値からpassを選びます。
                            const bool transparent = part.alphaBlended
                                || part.color.w < 0.999f;
                            // 異なるpassに属するpartを飛ばします。
                            if (transparent != (blendPass == 1))
                            {
                                continue;
                            }
                            // GPU mesh未作成ならvertex/index bufferを生成します。
                            if (part.webMesh == 0)
                            {
                                // primitiveのvertex/indexからGPU meshを作ります。
                                part.webMesh = m_renderer->CreateMesh(
                                WebVertices(part.vertices),
                                WebIndices(part.indices));
                            part.dirty = false;
                            }
                            // dirty頂点を既存GPU meshへ更新します。
                            else if (part.dirty)
                            {
                                // 変更済みprimitive bufferをGPUへ転送します。
                                m_renderer->UpdateMesh(
                                part.webMesh,
                                WebVertices(part.vertices),
                                WebIndices(part.indices));
                            part.dirty = false;
                            }
                            const auto loadMaterialTexture = [this](
                                std::uint32_t& id, const std::filesystem::path& asset,
                                const std::shared_ptr<const std::vector<unsigned char>>& encoded = {})
                            {
                                if (id != 0) return;
                                if (encoded) id = m_renderer->CreateTextureEncoded(*encoded);
                                else if (!asset.empty())
                                {
                                    const std::string path = VirtualAssetPath(asset);
                                    id = m_renderer->CreateTexture(path.c_str());
                                }
                            };
                            loadMaterialTexture(part.webTexture, part.albedoTexture, part.albedoImage);
                            loadMaterialTexture(part.webNormalTexture, part.normalTexture, part.normalImage);
                            loadMaterialTexture(part.webMetallicRoughnessTexture,
                                part.metallicRoughnessTexture, part.metallicRoughnessImage);
                            loadMaterialTexture(part.webRoughnessTexture, part.roughnessTexture);
                            loadMaterialTexture(part.webMetallicTexture, part.metallicTexture);
                            loadMaterialTexture(part.webOcclusionTexture, part.occlusionTexture, part.occlusionImage);
                            loadMaterialTexture(part.webEmissiveTexture, part.emissiveTexture, part.emissiveImage);
                            m_renderer->DrawMesh(
                            part.webMesh,
                            model,
                            WebColor(part.color),
                            part.roughness,
                            part.webTexture,
                            part.doubleSided,
                            part.alphaBlended,
                            part.alphaCutoff,
                            part.webNormalTexture,
                            part.normalStrength,
                            part.metallic,
                            part.webMetallicRoughnessTexture,
                            part.webRoughnessTexture,
                            part.webMetallicTexture,
                            part.webOcclusionTexture,
                            part.occlusionStrength,
                            part.webEmissiveTexture,
                            WebColor({
                                part.emissiveColor.x,
                                part.emissiveColor.y,
                                part.emissiveColor.z,
                                1.0f }),
                            part.unlit,
                            WebColor({
                                part.dielectricSpecular.x,
                                part.dielectricSpecular.y,
                                part.dielectricSpecular.z,
                                1.0f }));
                        }
                    }
                }
                // active particleをcamera-facing meshへまとめて描画します。
                else if (auto* particles =
                             dynamic_cast<ParticleSystemComponent*>(component.get());
                         particles != nullptr && !particles->m_particles.empty())
                {
                    // particle quad頂点をまとめるGPU upload前bufferです。
                    std::vector<Web::Vertex3D> vertices;
                    // particle quad indexをまとめるbufferです。
                    std::vector<std::uint32_t> indices;
                    vertices.reserve(particles->m_particles.size() * 4);
                    indices.reserve(particles->m_particles.size() * 6);
                    // particle色の平均値を集計します。
                    Web::Color color{ 0.0f, 0.0f, 0.0f, 0.0f };
                    // particleごとのbillboard quadを生成します。
                    for (const auto& particle : particles->m_particles)
                    {
                        // particle quadの回転角cosです。
                        const float cosine = std::cos(particle.rotation);
                        // particle quadの回転角sinです。
                        const float sine = std::sin(particle.rotation);
                        // billboardまたは水平描画の基準rightです。
                        const Vec3 baseRight =
                            particles->m_renderMode
                                    == ParticleRenderMode::Horizontal
                                ? Vec3{ 1.0f, 0.0f, 0.0f }
                                : cameraRight;
                        // billboardまたは水平描画の基準upです。
                        const Vec3 baseUp =
                            particles->m_renderMode
                                    == ParticleRenderMode::Horizontal
                                ? Vec3{ 0.0f, 0.0f, 1.0f }
                                : cameraUp;
                        // particle回転後のquad right方向です。
                        const Vec3 right =
                            baseRight * cosine + baseUp * sine;
                        // particle回転後のquad up方向です。
                        const Vec3 up =
                            baseUp * cosine - baseRight * sine;
                        // 寿命区間内の正規化進行率です。
                        const float life = std::clamp(
                            particle.age / particle.lifetime, 0.0f, 1.0f);
                        // 寿命に応じて変化させた表示sizeです。
                        const float displaySize = particle.size
                            * (1.0f
                                + (particles->m_endSizeMultiplier - 1.0f)
                                    * life);
                        // quad頂点へ使う半径です。
                        const float halfSize = displaySize * 0.5f;
                        // particleのworld中心です。
                        const Vec3 center = WebVector(particle.position);
                        // quad横方向のhalf-size offsetです。
                        const Vec3 horizontal = right * halfSize;
                        // quad縦方向のhalf-size offsetです。
                        const Vec3 vertical = up * halfSize;
                        // このquadの先頭vertex indexです。
                        const std::uint32_t base = static_cast<std::uint32_t>(
                            vertices.size());
                        // billboard quadのworld normalです。
                        const Vec3 normal = LamaPon::Web::Normalize(
                            LamaPon::Web::Cross(right, up));
                        vertices.insert(vertices.end(), {
                            { center - horizontal - vertical, normal, { 0.0f, 1.0f } },
                            { center + horizontal - vertical, normal, { 1.0f, 1.0f } },
                            { center - horizontal + vertical, normal, { 0.0f, 0.0f } },
                            { center + horizontal + vertical, normal, { 1.0f, 0.0f } },
                        });
                        indices.insert(indices.end(), {
                            base, base + 2,
                            base + 1, base + 1,
                            base + 2, base + 3,
                        });
                        color.r += particles->m_startColor.x
                            + (particles->m_endColor.x
                               - particles->m_startColor.x) * life;
                        color.g += particles->m_startColor.y
                            + (particles->m_endColor.y
                               - particles->m_startColor.y) * life;
                        color.b += particles->m_startColor.z
                            + (particles->m_endColor.z
                               - particles->m_startColor.z) * life;
                        color.a += particles->m_startColor.w
                            + (particles->m_endColor.w
                               - particles->m_startColor.w) * life;
                    }
                    // GPU meshが未作成ならparticle quad meshを生成します。
                    if (particles->m_webMesh == 0)
                    {
                        particles->m_webMesh =
                            m_renderer->CreateMesh(vertices, indices);
                    }
                    // 既存particle meshのvertex/indexを更新します。
                    else
                    {
                        m_renderer->UpdateMesh(
                            particles->m_webMesh,
                            vertices,
                            indices);
                    }
                    // particle texture指定がある場合だけ初回読み込みします。
                    if (particles->m_webTexture == 0
                        && !particles->m_texture.empty())
                    {
                        // Web rendererへ渡すvirtual asset pathです。
                        const std::string path =
                            VirtualAssetPath(particles->m_texture);
                        particles->m_webTexture =
                            m_renderer->CreateTexture(path.c_str());
                    }
                    // 平均色をparticle数で割る係数です。
                    const float inverseParticleCount = 1.0f
                        / static_cast<float>(particles->m_particles.size());
                    color.r *= inverseParticleCount;
                    color.g *= inverseParticleCount;
                    color.b *= inverseParticleCount;
                    color.a *= inverseParticleCount;
                    m_renderer->DrawMesh(
                        particles->m_webMesh,
                        Mat4::Identity(),
                        color,
                        1.0f,
                        particles->m_webTexture,
                        true,
                        true,
                        -1.0f,
                        0,
                        1.0f,
                        0.0f,
                        0,
                        0,
                        0,
                        0,
                        1.0f,
                        0,
                        {},
                        true,
                        { 0.04f, 0.04f, 0.04f, 1.0f },
                        particles->m_additive);
                }
                else if (auto* image = dynamic_cast<PortableUIVisualComponent*>(component.get()))
                {
                    UIRect area;
                    if (const auto* rect = object->GetComponent<UIRectTransformComponent>())
                        area = rect->Resolve(m_graphics.UIWidth(), m_graphics.UIHeight());
                    else area = {{model.values[12]-image->m_size.x*0.5f, model.values[13]-image->m_size.y*0.5f},
                                 {model.values[12]+image->m_size.x*0.5f, model.values[13]+image->m_size.y*0.5f}};
                    const auto size = area.Size();
                    auto color = image->m_color;
                    auto* button = dynamic_cast<UIButtonComponent*>(image);
                    if (button) color = !button->m_interactable ? button->m_disabled
                        : button->m_pressed ? button->m_press : (button->m_hovered || button->m_focused) ? button->m_hover : button->m_normal;
                    RenderPortableSprite(object->Name().c_str(), static_cast<double>(object->Id()),
                        VirtualAssetPath(image->m_texture).c_str(), color.x,color.y,color.z,color.w,
                        area.minimum.x,area.minimum.y,size.x,size.y,0,0,0,image->m_sortOrder,0,0,1,1,0);
                    if (button)
                        RenderPortableText(object->Name().c_str(), static_cast<double>(object->Id()),
                            button->m_label.c_str(),button->m_fontFamily.c_str(),"",button->m_fontSize,
                            button->m_textColor.x,button->m_textColor.y,button->m_textColor.z,button->m_textColor.w,
                            area.minimum.x,area.minimum.y,size.x,size.y,0,1,1,image->m_sortOrder);
                }
                // TextRenderer componentをDOMへ反映します。
                else if (auto* text = dynamic_cast<TextRendererComponent*>(component.get()))
                {
                    // rect transform未指定時のworld x位置です。
                    float textX = model.values[12];
                    // rect transform未指定時のworld y位置です。
                    float textY = model.values[13];
                    // text component既定の表示幅です。
                    float textWidth = text->m_bounds.x;
                    // text component既定の表示高さです。
                    float textHeight = text->m_bounds.y;
                    // rect transformがあれば解決済み矩形を使います。
                    if (const auto* rect =
                            object->GetComponent<UIRectTransformComponent>())
                    {
                        // viewport基準で解決したtext矩形です。
                        const auto resolved = rect->Resolve(
                            static_cast<float>(m_graphics.UIWidth()),
                            static_cast<float>(m_graphics.UIHeight()));
                        // 解決済み矩形の幅と高さです。
                        const auto resolvedSize = resolved.Size();
                        textX = resolved.minimum.x;
                        textY = resolved.minimum.y;
                        textWidth = resolvedSize.x;
                        textHeight = resolvedSize.y;
                    }
                    RenderPortableText(
                        object->Name().c_str(), static_cast<double>(object->Id()),
                        text->m_text.c_str(),
                        text->m_fontFamily.c_str(),
                        VirtualAssetPath(text->m_fontAsset).c_str(),
                        text->m_fontSize,
                        text->m_color.x, text->m_color.y, text->m_color.z,
                        text->m_color.w, textX, textY,
                        textWidth, textHeight,
                        text->m_wordWrap ? 1 : 0,
                        static_cast<int>(text->m_horizontal),
                        static_cast<int>(text->m_vertical), text->m_sortOrder);
                }
                // SpriteMask componentをDOMへ反映します。
                else if (auto* mask =
                             dynamic_cast<SpriteMaskComponent*>(component.get()))
                {
                    // model matrixのx軸scaleです。
                    const float scaleX = std::hypot(
                        model.values[0], model.values[1]);
                    // model matrixのy軸scaleです。
                    const float scaleY = std::hypot(
                        model.values[4], model.values[5]);
                    RenderPortableMask(
                        static_cast<double>(object->Id()),
                        model.values[12], model.values[13],
                        mask->m_size.x * scaleX,
                        mask->m_size.y * scaleY,
                        static_cast<int>(mask->m_shape));
                }
                // SpriteRenderer componentをDOMへ反映します。
                else if (auto* sprite = dynamic_cast<SpriteRendererComponent*>(component.get()))
                {
                    // Web rendererへ渡すvirtual texture pathです。
                    const std::string path = VirtualAssetPath(sprite->m_texture);
                    // model matrixのx軸scaleです。
                    const float scaleX = std::hypot(
                        model.values[0], model.values[1]);
                    // model matrixのy軸scaleです。
                    const float scaleY = std::hypot(
                        model.values[4], model.values[5]);
                    // model matrixから抽出したz回転角です。
                    const float rotation = std::atan2(
                        model.values[1], model.values[0]);
                    // rect transform未指定時のworld x位置です。
                    float spriteX = model.values[12];
                    // rect transform未指定時のworld y位置です。
                    float spriteY = model.values[13];
                    // transform scale適用後のsprite幅です。
                    float spriteWidth = sprite->m_size.x * scaleX;
                    // transform scale適用後のsprite高さです。
                    float spriteHeight = sprite->m_size.y * scaleY;
                    // sprite componentに設定されたpivot xです。
                    float pivotX = sprite->m_pivot.x;
                    // sprite componentに設定されたpivot yです。
                    float pivotY = sprite->m_pivot.y;
                    // rect transformがあれば解決矩形全体へ配置します。
                    if (const auto* rect =
                            object->GetComponent<UIRectTransformComponent>())
                    {
                        // Rect Transform付きSpriteはWindows版と同じく、解決済み矩形の中央を基準に配置して矩形全体へ伸縮します。
                        // viewport基準で解決したsprite矩形です。
                        const auto resolved = rect->Resolve(
                            static_cast<float>(m_graphics.UIWidth()),
                            static_cast<float>(m_graphics.UIHeight()));
                        // 解決済み矩形の幅と高さです。
                        const auto resolvedSize = resolved.Size();
                        spriteX = resolved.minimum.x + resolvedSize.x * 0.5f;
                        spriteY = resolved.minimum.y + resolvedSize.y * 0.5f;
                        spriteWidth = resolvedSize.x * scaleX;
                        spriteHeight = resolvedSize.y * scaleY;
                        pivotX = 0.5f;
                        pivotY = 0.5f;
                    }
                    RenderPortableSprite(
                        object->Name().c_str(), static_cast<double>(object->Id()),
                        path.c_str(),
                        sprite->m_color.x, sprite->m_color.y, sprite->m_color.z,
                        sprite->m_color.w, spriteX, spriteY,
                        spriteWidth, spriteHeight,
                        pivotX, pivotY,
                        rotation, sprite->m_sortOrder,
                        sprite->m_sourceRect.x, sprite->m_sourceRect.y,
                        sprite->m_sourceRect.z, sprite->m_sourceRect.w,
                        static_cast<int>(sprite->m_maskInteraction));
                }
            }
        }
        EndPortableUiFrame();
        m_renderer->EndFrame();
    }

    // filter内のcolliderとrayの最短交差を返します(ray: ray, maximumDistance: 距離上限, hit: 出力hit, filter: query条件)
    bool Scene::Raycast(
        const Ray& ray,
        float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        // scene座標系のray起点です。
        const Vec3 origin = WebVector(ray.origin);
        // 正規化済みscene座標系ray方向です。
        const Vec3 direction = LamaPon::Web::Normalize(WebVector(ray.direction));
        // 現時点の最短交差距離です。
        float nearest = maximumDistance;
        // 有効交差を見つけたかを示します。
        bool found{};
        // scene内objectごとにcolliderとの交差を調べます。
        for (const auto& object : m_objects)
        {
            // 無効objectと除外指定objectは判定しません。
            if (!object->IsEnabled()
                || object->Id() == filter.ignoredGameObjectId)
            {
                continue;
            }
            // collider交差で使うobject world matrixです。
            const Mat4 world = ComputeWorldMatrix(*object);
            // 有効なlayerのbox colliderと交差判定します。
            if (const auto* box = object->GetComponent<BoxCollider3DComponent>();
                box != nullptr && box->IsEnabled()
                && (filter.layerMask & (1u << (box->m_layer % 32u))) != 0)
            {
                // box centerのworld位置です。
                const Vec3 center = TransformPoint(
                    world, WebVector(box->m_offset));
                // box sizeのlocal half extentです。
                const Vec3 localHalf{
                    std::abs(box->m_size.x) * 0.5f,
                    std::abs(box->m_size.y) * 0.5f,
                    std::abs(box->m_size.z) * 0.5f,
                };
                // rotationとscaleを反映したworld half extentです。
                const Vec3 half{
                    std::abs(world.values[0]) * localHalf.x
                        + std::abs(world.values[4]) * localHalf.y
                        + std::abs(world.values[8]) * localHalf.z,
                    std::abs(world.values[1]) * localHalf.x
                        + std::abs(world.values[5]) * localHalf.y
                        + std::abs(world.values[9]) * localHalf.z,
                    std::abs(world.values[2]) * localHalf.x
                        + std::abs(world.values[6]) * localHalf.y
                        + std::abs(world.values[10]) * localHalf.z,
                };
                // ray entry距離の初期値です。
                float entry{};
                // ray exit距離の初期値です。
                float exit = nearest;
                // box entry時の法線です。
                Vec3 entryNormal{};
                // rayを1軸のslabへ交差させます(rayOrigin: 起点軸, rayDirection: 方向軸, minimum: slab下端, maximum: slab上端, negativeNormal: 負側法線, positiveNormal: 正側法線)
                const auto intersectAxis = [&entry, &exit, &entryNormal](
                    const float rayOrigin,
                    const float rayDirection,
                    const float minimum,
                    const float maximum,
                    const Vec3& negativeNormal,
                    const Vec3& positiveNormal)
                {
                    // rayが平行なら起点がslab内かだけ判定します。
                    if (std::abs(rayDirection) <= 0.000001f)
                    {
                        return rayOrigin >= minimum && rayOrigin <= maximum;
                    }
                    // slab下端との交差距離です。
                    float nearDistance = (minimum - rayOrigin) / rayDirection;
                    // slab上端との交差距離です。
                    float farDistance = (maximum - rayOrigin) / rayDirection;
                    // 初期near交差面の法線です。
                    Vec3 nearNormal = negativeNormal;
                    // rayが逆向きなら距離と法線を入れ替えます。
                    if (nearDistance > farDistance)
                    {
                        std::swap(nearDistance, farDistance);
                        nearNormal = positiveNormal;
                    }
                    // 最も後ろのentry面と法線を保持します。
                    if (nearDistance > entry)
                    {
                        entry = nearDistance;
                        entryNormal = nearNormal;
                    }
                    exit = std::min(exit, farDistance);
                    return entry <= exit;
                };
                // 3軸すべてで交差し、最短距離内ならhit更新します。
                if (intersectAxis(
                        origin.x, direction.x,
                        center.x - half.x, center.x + half.x,
                        { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f })
                    && intersectAxis(
                        origin.y, direction.y,
                        center.y - half.y, center.y + half.y,
                        { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f })
                    && intersectAxis(
                        origin.z, direction.z,
                        center.z - half.z, center.z + half.z,
                        { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, 1.0f })
                    && entry <= nearest)
                {
                    nearest = entry;
                    found = true;
                    hit.gameObject = object.get();
                    hit.distance = entry;
                    hit.point = DirectXVector(origin + direction * entry);
                    hit.normal = DirectXVector(entryNormal);
                }
            }
            // mesh colliderがなければ次のobjectへ進みます。
            const auto* collider = object->GetComponent<MeshCollider3DComponent>();
            // 無効colliderまたはlayer対象外を飛ばします。
            if (collider == nullptr || !collider->IsEnabled()
                || (filter.layerMask & (1u << (collider->m_layer % 32u))) == 0)
            {
                continue;
            }
            // mesh triangleごとにray交差を調べます。
            for (std::size_t index{}; index + 2 < collider->m_indices.size(); index += 3)
            {
                // triangleの1頂点indexです。
                const std::uint32_t ia = collider->m_indices[index];
                // triangleの2頂点indexです。
                const std::uint32_t ib = collider->m_indices[index + 1];
                // triangleの3頂点indexです。
                const std::uint32_t ic = collider->m_indices[index + 2];
                // 頂点配列外のindexを持つtriangleを飛ばします。
                if (ia >= collider->m_vertices.size()
                    || ib >= collider->m_vertices.size()
                    || ic >= collider->m_vertices.size())
                {
                    continue;
                }
                // world座標へ変換したtriangle頂点です。
                const Vec3 a = TransformPoint(world, WebVector(collider->m_vertices[ia]));
                // world座標へ変換したtriangle頂点です。
                const Vec3 b = TransformPoint(world, WebVector(collider->m_vertices[ib]));
                // world座標へ変換したtriangle頂点です。
                const Vec3 c = TransformPoint(world, WebVector(collider->m_vertices[ic]));
                // triangle交差までの距離です。
                float distance{};
                // triangle交差法線です。
                Vec3 normal{};
                // 最短距離内のtriangle交差をhitへ保存します。
                if (RayTriangle(origin, direction, a, b, c, distance, normal)
                    && distance <= nearest)
                {
                    nearest = distance;
                    found = true;
                    hit.gameObject = object.get();
                    hit.distance = distance;
                    hit.point = DirectXVector(origin + direction * distance);
                    hit.normal = DirectXVector(normal);
                }
            }
        }
        return found;
    }
}
