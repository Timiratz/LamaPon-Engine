#include "LamaPon/LamaPon.h"

#include "LamaPon/Web/WebAudioRuntime.h"
#include "LamaPon/Web/WebInput.h"
#include "LamaPon/Web/WebMath.h"
#include "LamaPon/Web/WebRenderer3D.h"

#include <emscripten.h>

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

    struct PortableInputBinding final
    {
        std::string control;
        float scale{ 1.0f };
    };

    std::unordered_map<std::string, std::vector<PortableInputBinding>>&
        PortableInputBindings()
    {
        static std::unordered_map<
            std::string,
            std::vector<PortableInputBinding>> value;
        return value;
    }

    std::unordered_map<std::string, LamaPon::ScriptFactory>& ScriptFactories()
    {
        static std::unordered_map<std::string, LamaPon::ScriptFactory> value;
        return value;
    }

    [[nodiscard]] Vec3 WebVector(const DirectX::XMFLOAT3& value) noexcept
    {
        return { value.x, value.y, value.z };
    }

    [[nodiscard]] DirectX::XMFLOAT2 ClampUnit2(
        const DirectX::XMFLOAT2 value) noexcept
    {
        return {
            std::clamp(value.x, 0.0f, 1.0f),
            std::clamp(value.y, 0.0f, 1.0f)
        };
    }

    [[nodiscard]] DirectX::XMFLOAT3 DirectXVector(const Vec3& value) noexcept
    {
        return { value.x, value.y, value.z };
    }

    [[nodiscard]] LamaPon::Web::Color WebColor(
        const DirectX::XMFLOAT4& value) noexcept
    {
        return { value.x, value.y, value.z, value.w };
    }

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

    [[nodiscard]] Mat4 WorldMatrix(const GameObject& object)
    {
        const Mat4 local = LocalMatrix(object.GetTransform());
        return object.Parent() != nullptr
            ? LamaPon::Web::Multiply(WorldMatrix(*object.Parent()), local)
            : local;
    }

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

    void RecalculateNormals(
        std::vector<ProceduralMeshVertex>& vertices,
        const std::vector<std::uint32_t>& indices)
    {
        for (auto& vertex : vertices)
        {
            vertex.normal = {};
        }
        for (std::size_t index{}; index + 2 < indices.size(); index += 3)
        {
            const std::uint32_t a = indices[index];
            const std::uint32_t b = indices[index + 1];
            const std::uint32_t c = indices[index + 2];
            if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size())
            {
                continue;
            }
            const Vec3 first = WebVector(vertices[a].position);
            const Vec3 second = WebVector(vertices[b].position);
            const Vec3 third = WebVector(vertices[c].position);
            const Vec3 normal = LamaPon::Web::Cross(third - first, second - first);
            for (const std::uint32_t vertexIndex : { a, b, c })
            {
                auto& value = vertices[vertexIndex].normal;
                value.x += normal.x;
                value.y += normal.y;
                value.z += normal.z;
            }
        }
        for (auto& vertex : vertices)
        {
            const Vec3 normal = LamaPon::Web::Normalize(WebVector(vertex.normal));
            vertex.normal = DirectXVector(
                LamaPon::Web::LengthSquared(normal) > 0.0f
                    ? normal
                    : Vec3{ 0.0f, 1.0f, 0.0f });
        }
    }

    void BuildPrimitive(
        LamaPon::PrimitiveShape shape,
        std::vector<ProceduralMeshVertex>& vertices,
        std::vector<std::uint32_t>& indices)
    {
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
        if (shape == LamaPon::PrimitiveShape::Cube)
        {
            constexpr std::array<DirectX::XMFLOAT3, 8> corners = {{
                { -0.5f, -0.5f, -0.5f }, { 0.5f, -0.5f, -0.5f },
                { -0.5f,  0.5f, -0.5f }, { 0.5f,  0.5f, -0.5f },
                { -0.5f, -0.5f,  0.5f }, { 0.5f, -0.5f,  0.5f },
                { -0.5f,  0.5f,  0.5f }, { 0.5f,  0.5f,  0.5f },
            }};
            constexpr std::array<std::uint32_t, 36> cubeIndices = {{
                0, 2, 1, 1, 2, 3, 5, 7, 4, 4, 7, 6,
                4, 6, 0, 0, 6, 2, 1, 3, 5, 5, 3, 7,
                2, 6, 3, 3, 6, 7, 4, 0, 5, 5, 0, 1,
            }};
            vertices.reserve(cubeIndices.size());
            indices.reserve(cubeIndices.size());
            for (const std::uint32_t corner : cubeIndices)
            {
                indices.push_back(static_cast<std::uint32_t>(vertices.size()));
                vertices.push_back({ corners[corner], {}, {} });
            }
            RecalculateNormals(vertices, indices);
            return;
        }
        if (shape == LamaPon::PrimitiveShape::Cylinder)
        {
            constexpr int segmentCount = 32;
            for (int segment{}; segment <= segmentCount; ++segment)
            {
                const float u = static_cast<float>(segment) / segmentCount;
                const float angle = u * std::numbers::pi_v<float> * 2.0f;
                const float x = std::cos(angle);
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
            for (int segment{}; segment < segmentCount; ++segment)
            {
                const std::uint32_t bottom = static_cast<std::uint32_t>(
                    segment * 2);
                indices.insert(indices.end(), {
                    bottom, bottom + 1, bottom + 2,
                    bottom + 2, bottom + 1, bottom + 3,
                });
            }
            const std::uint32_t topCenter = static_cast<std::uint32_t>(
                vertices.size());
            vertices.push_back({
                { 0.0f, 0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f },
                { 0.5f, 0.5f },
            });
            const std::uint32_t bottomCenter = static_cast<std::uint32_t>(
                vertices.size());
            vertices.push_back({
                { 0.0f, -0.5f, 0.0f }, { 0.0f, -1.0f, 0.0f },
                { 0.5f, 0.5f },
            });
            const std::uint32_t ringStart = static_cast<std::uint32_t>(
                vertices.size());
            for (int segment{}; segment <= segmentCount; ++segment)
            {
                const float angle = static_cast<float>(segment) / segmentCount
                    * std::numbers::pi_v<float> * 2.0f;
                const float x = std::cos(angle);
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
            for (int segment{}; segment < segmentCount; ++segment)
            {
                const std::uint32_t top = ringStart
                    + static_cast<std::uint32_t>(segment * 2);
                indices.insert(indices.end(), {
                    topCenter, top, top + 2,
                    bottomCenter, top + 3, top + 1,
                });
            }
            return;
        }

        constexpr int latitudeCount = 16;
        constexpr int longitudeCount = 32;
        for (int latitude{}; latitude <= latitudeCount; ++latitude)
        {
            const float v = static_cast<float>(latitude) / latitudeCount;
            const float phi = v * std::numbers::pi_v<float>;
            for (int longitude{}; longitude <= longitudeCount; ++longitude)
            {
                const float u = static_cast<float>(longitude) / longitudeCount;
                const float theta = u * std::numbers::pi_v<float> * 2.0f;
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
        for (int latitude{}; latitude < latitudeCount; ++latitude)
        {
            for (int longitude{}; longitude < longitudeCount; ++longitude)
            {
                const std::uint32_t a = static_cast<std::uint32_t>(
                    latitude * (longitudeCount + 1) + longitude);
                const std::uint32_t b = a + longitudeCount + 1;
                indices.insert(indices.end(), {
                    a, b, a + 1,
                    a + 1, b, b + 1,
                });
            }
        }
    }

    [[nodiscard]] bool RayTriangle(
        const Vec3& origin,
        const Vec3& direction,
        const Vec3& a,
        const Vec3& b,
        const Vec3& c,
        float& distance,
        Vec3& normal)
    {
        constexpr float epsilon = 0.000001f;
        const Vec3 edge1 = b - a;
        const Vec3 edge2 = c - a;
        const Vec3 p = LamaPon::Web::Cross(direction, edge2);
        const float determinant = LamaPon::Web::Dot(edge1, p);
        if (std::abs(determinant) < epsilon)
        {
            return false;
        }
        const float inverse = 1.0f / determinant;
        const Vec3 t = origin - a;
        const float u = LamaPon::Web::Dot(t, p) * inverse;
        if (u < 0.0f || u > 1.0f)
        {
            return false;
        }
        const Vec3 q = LamaPon::Web::Cross(t, edge1);
        const float v = LamaPon::Web::Dot(direction, q) * inverse;
        if (v < 0.0f || u + v > 1.0f)
        {
            return false;
        }
        const float result = LamaPon::Web::Dot(edge2, q) * inverse;
        if (result < 0.0f)
        {
            return false;
        }
        distance = result;
        normal = LamaPon::Web::Normalize(LamaPon::Web::Cross(edge1, edge2));
        if (LamaPon::Web::Dot(normal, direction) > 0.0f)
        {
            normal = normal * -1.0f;
        }
        return true;
    }

    [[nodiscard]] std::vector<std::uint32_t> WebIndices(
        const std::vector<std::uint32_t>& source)
    {
        return source;
    }

    [[nodiscard]] std::vector<LamaPon::Web::Vertex3D> WebVertices(
        const std::vector<ProceduralMeshVertex>& source)
    {
        std::vector<LamaPon::Web::Vertex3D> result;
        result.reserve(source.size());
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

    [[nodiscard]] std::string VirtualAssetPath(
        const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return {};
        }
        return "/assets/" + path.generic_string();
    }

    [[nodiscard]] const cgltf_accessor* FindModelAttribute(
        const cgltf_primitive& primitive,
        cgltf_attribute_type type,
        cgltf_int index = 0) noexcept
    {
        for (cgltf_size attributeIndex{};
             attributeIndex < primitive.attributes_count;
             ++attributeIndex)
        {
            const auto& attribute = primitive.attributes[attributeIndex];
            if (attribute.type == type && attribute.index == index)
            {
                return attribute.data;
            }
        }
        return nullptr;
    }

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

    [[nodiscard]] DirectX::XMFLOAT3 TransformModelNormal(
        const float* matrix,
        const DirectX::XMFLOAT3& value) noexcept
    {
        const float determinant =
            matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6])
            - matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2])
            + matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
        if (std::abs(determinant) <= 0.000001f)
        {
            return { 0.0f, 1.0f, 0.0f };
        }
        const float inverse = 1.0f / determinant;
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

    [[nodiscard]] float ModelTransformDeterminant(const float* matrix) noexcept
    {
        return matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6])
            - matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2])
            + matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
    }

    [[nodiscard]] Mat4 ModelMatrix(const std::array<float, 16>& values) noexcept
    {
        Mat4 result{};
        result.values = values;
        return result;
    }

    [[nodiscard]] std::array<float, 16> ModelMatrix(const Mat4& value) noexcept
    {
        return value.values;
    }

    [[nodiscard]] DirectX::XMFLOAT4 NormalizeModelQuaternion(
        DirectX::XMFLOAT4 value) noexcept
    {
        const float lengthSquared = value.x * value.x + value.y * value.y
            + value.z * value.z + value.w * value.w;
        if (lengthSquared <= 0.000001f)
        {
            return { 0.0f, 0.0f, 0.0f, 1.0f };
        }
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        value.x *= inverseLength;
        value.y *= inverseLength;
        value.z *= inverseLength;
        value.w *= inverseLength;
        return value;
    }

    [[nodiscard]] DirectX::XMFLOAT4 SlerpModelQuaternion(
        DirectX::XMFLOAT4 from,
        DirectX::XMFLOAT4 to,
        float amount) noexcept
    {
        from = NormalizeModelQuaternion(from);
        to = NormalizeModelQuaternion(to);
        float dot = from.x * to.x + from.y * to.y
            + from.z * to.z + from.w * to.w;
        if (dot < 0.0f)
        {
            dot = -dot;
            to = { -to.x, -to.y, -to.z, -to.w };
        }
        if (dot > 0.9995f)
        {
            return NormalizeModelQuaternion({
                from.x + (to.x - from.x) * amount,
                from.y + (to.y - from.y) * amount,
                from.z + (to.z - from.z) * amount,
                from.w + (to.w - from.w) * amount,
            });
        }
        const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
        const float sine = std::sin(angle);
        if (std::abs(sine) <= 0.000001f)
        {
            return from;
        }
        const float fromWeight = std::sin((1.0f - amount) * angle) / sine;
        const float toWeight = std::sin(amount * angle) / sine;
        return NormalizeModelQuaternion({
            from.x * fromWeight + to.x * toWeight,
            from.y * fromWeight + to.y * toWeight,
            from.z * fromWeight + to.z * toWeight,
            from.w * fromWeight + to.w * toWeight,
        });
    }

    [[nodiscard]] Mat4 ModelQuaternionMatrix(
        DirectX::XMFLOAT4 value) noexcept
    {
        value = NormalizeModelQuaternion(value);
        const float xx = value.x * value.x;
        const float yy = value.y * value.y;
        const float zz = value.z * value.z;
        const float xy = value.x * value.y;
        const float xz = value.x * value.z;
        const float yz = value.y * value.z;
        const float wx = value.w * value.x;
        const float wy = value.w * value.y;
        const float wz = value.w * value.z;
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

    [[nodiscard]] DirectX::XMFLOAT4 HermiteModelVector(
        const DirectX::XMFLOAT4& from,
        const DirectX::XMFLOAT4& fromTangent,
        const DirectX::XMFLOAT4& to,
        const DirectX::XMFLOAT4& toTangent,
        float amount,
        float duration) noexcept
    {
        const float squared = amount * amount;
        const float cubed = squared * amount;
        const float h00 = 2.0f * cubed - 3.0f * squared + 1.0f;
        const float h10 = cubed - 2.0f * squared + amount;
        const float h01 = -2.0f * cubed + 3.0f * squared;
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

    [[nodiscard]] std::filesystem::path ModelTexturePath(
        const std::filesystem::path& modelPath,
        const cgltf_texture_view& view)
    {
        if (view.texture == nullptr || view.texture->image == nullptr
            || view.texture->image->uri == nullptr)
        {
            return {};
        }
        std::string uri(view.texture->image->uri);
        uri.resize(cgltf_decode_uri(uri.data()));
        return modelPath.parent_path() / std::filesystem::path(uri);
    }

    EM_JS(void, SavePortableText,
          (const char* key, const char* value), {
        const keyText = UTF8ToString(key);
        const valueText = UTF8ToString(value);
        try {
            localStorage.setItem(
                "lamapon.portable." + keyText,
                valueText);
        } catch (error) {}
        if (document.body) {
            document.body.dataset.lamaponSavedKey = keyText;
            document.body.dataset.lamaponSavedValue = valueText;
        }
    });

    EM_JS(char*, LoadPortableText, (const char* key), {
        const allocateUtf8 = value => {
            const length = lengthBytesUTF8(value) + 1;
            const result = _malloc(length);
            stringToUTF8(value, result, length);
            return result;
        };
        try {
            const value = localStorage.getItem(
                "lamapon.portable." + UTF8ToString(key)) || "";
            return allocateUtf8(value);
        } catch (error) {
            return allocateUtf8("");
        }
    });

    EM_JS(char*, LoadPortableAssetText, (const char* path), {
        try {
            const value = FS.readFile(
                UTF8ToString(path), { encoding: "utf8" });
            const length = lengthBytesUTF8(value) + 1;
            const result = _malloc(length);
            stringToUTF8(value, result, length);
            return result;
        } catch (error) {
            if (document.body) {
                document.body.dataset.lamaponAssetError = String(error);
                document.body.dataset.lamaponAssetPath = UTF8ToString(path);
            }
            return 0;
        }
    });

    EM_JS(unsigned char*, LoadPortableAssetBytes,
          (const char* path, std::uint32_t* byteCount), {
        try {
            const bytes = FS.readFile(UTF8ToString(path));
            const result = _malloc(bytes.length);
            HEAPU8.set(bytes, result);
            HEAPU32[byteCount >> 2] = bytes.length;
            return result;
        } catch (error) {
            HEAPU32[byteCount >> 2] = 0;
            if (document.body) {
                document.body.dataset.lamaponModelError = "asset-read";
                document.body.dataset.lamaponModelPath = UTF8ToString(path);
            }
            return 0;
        }
    });

    EM_JS(void, PublishPortableModelStatus,
          (const char* path, const char* status, int parts), {
        if (!document.body) return;
        const statusText = UTF8ToString(status);
        document.body.dataset.lamaponModelPath = UTF8ToString(path);
        document.body.dataset.lamaponModelStatus = statusText;
        document.body.dataset.lamaponModelParts = String(parts);
        if (statusText !== "loaded") {
            document.body.dataset.lamaponModelError = statusText;
        } else {
            delete document.body.dataset.lamaponModelError;
        }
    });

    EM_JS(void, PublishPortableModelAnimation,
          (const char* name, int index, int count, float time, int playing), {
        if (!document.body) return;
        document.body.dataset.lamaponModelAnimation = UTF8ToString(name);
        document.body.dataset.lamaponModelAnimationIndex = String(index);
        document.body.dataset.lamaponModelAnimationCount = String(count);
        document.body.dataset.lamaponModelAnimationTime = time.toFixed(4);
        document.body.dataset.lamaponModelAnimationPlaying = playing ? "1" : "0";
    });

    [[nodiscard]] bool LoadPortableJsonDocument(
        const std::filesystem::path& assetPath,
        Json& document)
    {
        const std::string path = VirtualAssetPath(assetPath);
        char* loaded = LoadPortableAssetText(path.c_str());
        if (loaded == nullptr)
        {
            return false;
        }
        try
        {
            document = Json::parse(loaded);
        }
        catch (const Json::exception&)
        {
            std::free(loaded);
            return false;
        }
        std::free(loaded);
        return document.is_object();
    }

    EM_JS(void, PublishPortableInputActionCount, (int count), {
        if (document.body) {
            document.body.dataset.lamaponInputActions = String(count);
        }
    });

    void LoadPortableInputBindings()
    {
        auto& bindings = PortableInputBindings();
        bindings.clear();
        PublishPortableInputActionCount(0);
        char* loaded = LoadPortableAssetText(
            "/assets/lamapon-input-actions.json");
        if (loaded == nullptr)
        {
            return;
        }
        Json document;
        try
        {
            document = Json::parse(loaded);
        }
        catch (const Json::exception&)
        {
            std::free(loaded);
            return;
        }
        std::free(loaded);
        const auto actions = document.value("actions", Json::object());
        if (!actions.is_object())
        {
            return;
        }
        for (const auto& [name, values] : actions.items())
        {
            if (!values.is_array())
            {
                continue;
            }
            auto& target = bindings[name];
            for (const auto& value : values)
            {
                if (!value.is_object())
                {
                    continue;
                }
                const std::string control = value.value("control", "");
                const float scale = value.value("scale", 1.0f);
                if (!control.empty() && std::isfinite(scale))
                {
                    target.push_back({ control, scale });
                }
            }
        }
        PublishPortableInputActionCount(static_cast<int>(bindings.size()));
    }

    EM_JS(void, RenderPortableText,
          (const char* objectName, double objectId,
           const char* text, const char* font, const char* fontAsset,
           float size, float r, float g, float b, float a,
           float x, float y, float width, float height,
           int wordWrap, int horizontal, int vertical, int sortOrder), {
        const name = UTF8ToString(objectName);
        const ids = {
            "HUD Time": "hud-time", "HUD Best": "hud-best",
            "HUD Gear": "hud-gear", "HUD Speed": "hud-speed",
            "HUD Speed Unit": "hud-speed-unit", "HUD Message": "hud-message"
        };
        const nativeId = ids[name] || "";
        let element = nativeId ? document.getElementById(nativeId) : null;
        const nativeElement = Boolean(element);
        if (!element) {
            const portableId = "lamapon-portable-text-"
                + String(Math.floor(objectId));
            element = document.getElementById(portableId);
            if (!element) {
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
        const fontFamily = UTF8ToString(font);
        const fontPath = UTF8ToString(fontAsset);
        if (fontPath) {
            globalThis.__lamaponPortableFonts ||= {};
            if (!globalThis.__lamaponPortableFonts[fontPath]) {
                globalThis.__lamaponPortableFonts[fontPath] = "loading";
                try {
                    const bytes = FS.readFile(fontPath);
                    const extension = fontPath.split(".").pop().toLowerCase();
                    const mime = extension === "woff2" ? "font/woff2"
                        : extension === "woff" ? "font/woff"
                        : extension === "otf" ? "font/otf" : "font/ttf";
                    const objectUrl = URL.createObjectURL(
                        new Blob([bytes], { type: mime }));
                    const face = new FontFace(fontFamily, "url(" + objectUrl + ")");
                    face.load().then(loaded => {
                        document.fonts.add(loaded);
                        globalThis.__lamaponPortableFonts[fontPath] = "loaded";
                        if (document.body) {
                            document.body.dataset.lamaponFontAsset = "loaded";
                            document.body.dataset.lamaponFontAssetPath = fontPath;
                        }
                    }).catch(error => {
                        globalThis.__lamaponPortableFonts[fontPath] = "error";
                        if (document.body) {
                            document.body.dataset.lamaponFontAsset = "error";
                            document.body.dataset.lamaponFontAssetPath = fontPath;
                        }
                        console.warn("LamaPon Web font load failed", fontPath, error);
                    }).finally(() => URL.revokeObjectURL(objectUrl));
                } catch (error) {
                    globalThis.__lamaponPortableFonts[fontPath] = "error";
                    console.warn("LamaPon Web font unavailable", fontPath, error);
                }
            }
        }
        if (nativeElement) {
            element.style.color = "rgba(" + (r*255) + "," + (g*255) + "," +
                (b*255) + "," + a + ")";
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
        if (name === "HUD Message") element.style.opacity = text ? "1" : "0";
    });

    EM_JS(void, RenderPortableMask,
          (double objectId, float x, float y, float width, float height,
           int shape), {
        if (!document.body) return;
        document.body.__lamaponPortableMasks ||= {};
        document.body.__lamaponPortableMasks[String(Math.floor(objectId))] = {
            x, y, width, height, shape
        };
    });

    EM_JS(void, BeginPortableUiFrame, (), {
        if (!document.body) return;
        document.body.__lamaponPortableFrame =
            (document.body.__lamaponPortableFrame || 0) + 1;
        document.body.__lamaponPortableMasks = {};
    });

    EM_JS(void, EndPortableUiFrame, (), {
        if (!document.body) return;
        const frame = String(document.body.__lamaponPortableFrame || 0);
        for (const element of document.querySelectorAll(
                "[data-lamapon-portable-ui]")) {
            if (element.dataset.lamaponPortableFrame !== frame
                && element.style.display !== "none") {
                element.style.display = "none";
            }
        }
    });

    EM_JS(void, HidePortableObjectUi, (double objectId), {
        const id = String(Math.floor(objectId));
        for (const prefix of [
                "lamapon-portable-text-",
                "lamapon-portable-sprite-"]) {
            const element = document.getElementById(prefix + id);
            if (element) {
                element.style.display = "none";
                element.dataset.lamaponPortableFrame = "hidden";
            }
        }
    });

    EM_JS(void, RenderPortableSprite,
          (const char* objectName, double objectId, const char* texturePath,
           float r, float g, float b, float a, float x, float y,
           float width, float height, float pivotX, float pivotY,
           float rotation, int sortOrder,
           float sourceX, float sourceY, float sourceWidth, float sourceHeight,
           int maskInteraction), {
        const name = UTF8ToString(objectName);
        let element = name === "HUD Tacho"
            ? document.getElementById("tacho-face")
            : name === "HUD Needle"
                ? document.getElementById("tacho-needle") : null;
        const nativeElement = Boolean(element);
        if (!element) {
            const portableId = "lamapon-portable-sprite-"
                + String(Math.floor(objectId));
            element = document.getElementById(portableId);
            if (!element) {
            const layer = document.getElementById("hud") || document.body;
            element = document.createElement("div");
            element.id = portableId;
            element.dataset.lamaponPortableUi = name;
            element.style.position = "absolute";
            element.style.pointerEvents = "none";
            layer.appendChild(element);
            }
        }
        if (!nativeElement) {
            element.dataset.lamaponPortableFrame = String(
                document.body?.__lamaponPortableFrame || 0);
        }
        if (nativeElement && name === "HUD Needle") {
            element.style.transform = "rotate(" + rotation + "rad)";
            return;
        }
        const path = UTF8ToString(texturePath);
        const left = x - width * pivotX;
        const top = y - height * pivotY;
        const safeSourceWidth = Math.max(0.000001, sourceWidth);
        const safeSourceHeight = Math.max(0.000001, sourceHeight);
        const backgroundX = safeSourceWidth >= 0.999999
            ? 0 : sourceX / (1 - safeSourceWidth) * 100;
        const backgroundY = safeSourceHeight >= 0.999999
            ? 0 : sourceY / (1 - safeSourceHeight) * 100;
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
        element.style.clipPath = "";
        element.style.maskImage = "";
        element.style.webkitMaskImage = "";
        if (maskInteraction !== 0 && document.body) {
            const masks = Object.values(document.body.__lamaponPortableMasks || {});
            let nearest = null;
            let nearestDistance = Number.POSITIVE_INFINITY;
            for (const mask of masks) {
                const distance = (mask.x - x) ** 2 + (mask.y - y) ** 2;
                if (distance < nearestDistance) {
                    nearest = mask;
                    nearestDistance = distance;
                }
            }
            if (nearest && maskInteraction === 1) {
                if (nearest.shape === 1) {
                    const radius = Math.min(nearest.width, nearest.height) * 0.5;
                    element.style.clipPath = "circle(" + radius + "px at "
                        + (nearest.x - left) + "px "
                        + (nearest.y - top) + "px)";
                } else {
                    const maskLeft = nearest.x - nearest.width * 0.5;
                    const maskTop = nearest.y - nearest.height * 0.5;
                    const insetTop = Math.max(0, maskTop - top);
                    const insetLeft = Math.max(0, maskLeft - left);
                    const insetRight = Math.max(
                        0, left + width - maskLeft - nearest.width);
                    const insetBottom = Math.max(
                        0, top + height - maskTop - nearest.height);
                    element.style.clipPath = "inset(" + insetTop + "px "
                        + insetRight + "px " + insetBottom + "px "
                        + insetLeft + "px)";
                }
            }
        }
        if (path && globalThis.FS && !element.dataset.lamaponTextureLoaded) {
            try {
                const bytes = FS.readFile(path);
                let binary = "";
                for (let i = 0; i < bytes.length; i += 0x8000) {
                    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
                }
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
                const url = "data:" + imageMime + ";base64," + btoa(binary);
                if (element.tagName === "IMG") element.src = url;
                else element.style.backgroundImage = "url(" + url + ")";
                element.dataset.lamaponTextureLoaded = "1";
            } catch (error) {}
        }
    });

    EM_JS(void, PublishPortableNumber, (const char* key, double value), {
        if (!document.body) return;
        const name = UTF8ToString(key).replaceAll("_", "-");
        document.body.setAttribute(
            "data-lamapon-state-" + name,
            String(value));
        document.body.__lamaponPortableState ||= {};
        document.body.__lamaponPortableState[UTF8ToString(key)] = value;
    });

    EM_JS(void, PublishPortableString,
          (const char* key, const char* value), {
        if (!document.body) return;
        const name = UTF8ToString(key).replaceAll("_", "-");
        const text = UTF8ToString(value);
        document.body.setAttribute("data-lamapon-state-" + name, text);
        document.body.__lamaponPortableState ||= {};
        document.body.__lamaponPortableState[UTF8ToString(key)] = text;
    });
}

namespace LamaPon
{
    struct Scene::Impl final
    {
        struct ContactKey final
        {
            GameObjectId first{};
            GameObjectId second{};

            bool operator==(const ContactKey&) const noexcept = default;
        };

        struct ContactHash final
        {
            std::size_t operator()(const ContactKey& value) const noexcept
            {
                return std::hash<GameObjectId>{}(value.first)
                    ^ (std::hash<GameObjectId>{}(value.second)
                        + 0x9e3779b9u
                        + (std::hash<GameObjectId>{}(value.first) << 6u)
                        + (std::hash<GameObjectId>{}(value.first) >> 2u));
            }
        };

        DirectX::XMFLOAT4 clearColor{ 0.72f, 0.62f, 0.52f, 1.0f };
        GameObject* mainCamera{};
        std::unordered_set<ContactKey, ContactHash> contacts;
    };

    void RuntimeState::SetNumber(std::string key, double value)
    {
        PublishPortableNumber(key.c_str(), value);
        m_numbers[std::move(key)] = value;
    }

    void RuntimeState::SetInteger(std::string key, std::int64_t value)
    {
        PublishPortableNumber(key.c_str(), static_cast<double>(value));
        m_integers[std::move(key)] = value;
    }

    void RuntimeState::SetBoolean(std::string key, bool value)
    {
        PublishPortableNumber(key.c_str(), value ? 1.0 : 0.0);
        m_booleans[std::move(key)] = value;
    }

    void RuntimeState::SetString(std::string key, std::string value)
    {
        PublishPortableString(key.c_str(), value.c_str());
        m_strings[std::move(key)] = std::move(value);
    }

    double RuntimeState::Number(std::string_view key, double fallback) const
    {
        const auto found = m_numbers.find(std::string(key));
        return found != m_numbers.end() ? found->second : fallback;
    }

    std::int64_t RuntimeState::Integer(
        std::string_view key,
        std::int64_t fallback) const
    {
        const auto found = m_integers.find(std::string(key));
        return found != m_integers.end() ? found->second : fallback;
    }

    bool RuntimeState::Boolean(std::string_view key, bool fallback) const
    {
        const auto found = m_booleans.find(std::string(key));
        return found != m_booleans.end() ? found->second : fallback;
    }

    std::string RuntimeState::String(
        std::string_view key,
        std::string fallback) const
    {
        const auto found = m_strings.find(std::string(key));
        return found != m_strings.end() ? found->second : std::move(fallback);
    }

    float InputSystem::Value(std::string_view action) const
    {
        if (m_input == nullptr)
        {
            return 0.0f;
        }
        if (const auto configured = PortableInputBindings().find(
                std::string(action));
            configured != PortableInputBindings().end()
            && !configured->second.empty())
        {
            float mapped{};
            for (const auto& binding : configured->second)
            {
                float value = m_input->ControlValue(binding.control);
                // ごく短いクリックは描画フレーム間で押下と解放が完了します。
                // 1回操作を取りこぼさないよう、そのエッジ入力を現在の
                // シミュレーションフレームまで保持します。
                if (value == 0.0f
                    && m_input->ControlWasPressed(binding.control))
                {
                    value = 1.0f;
                }
                mapped += value * binding.scale;
            }
            if (action == "MoveHorizontal")
            {
                mapped += m_input->TouchHorizontalAxis();
            }
            else if (action == "MoveVertical")
            {
                mapped += m_input->TouchVerticalAxis();
            }
            else if (action == "Accelerate")
            {
                mapped = std::max(mapped, m_input->TouchAccelerateAxis());
            }
            else if (action == "Brake")
            {
                mapped = std::max(mapped, m_input->TouchBrakeAxis());
            }
            return std::clamp(mapped, -1.0f, 1.0f);
        }
        const auto down = [this](const char* code)
        {
            return m_input->IsDown(code) || m_input->WasPressed(code)
                ? 1.0f : 0.0f;
        };
        if (action == "MoveHorizontal")
        {
            return std::clamp(
                down("KeyD") + down("ArrowRight")
                    - down("KeyA") - down("ArrowLeft")
                    + m_input->HorizontalAxis(),
                -1.0f, 1.0f);
        }
        if (action == "MoveVertical")
        {
            return std::clamp(
                down("KeyW") + down("ArrowUp")
                    - down("KeyS") - down("ArrowDown")
                    + m_input->VerticalAxis(),
                -1.0f, 1.0f);
        }
        if (action == "LookHorizontal")
        {
            return m_input->HorizontalAxis();
        }
        if (action == "LookVertical")
        {
            return m_input->VerticalAxis();
        }
        if (action == "Accelerate")
        {
            return std::max({
                down("KeyW"),
                down("ArrowUp"),
                m_input->AccelerateAxis(),
                std::max(m_input->VerticalAxis(), 0.0f),
            });
        }
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

    bool InputSystem::WasPressed(std::string_view action) const
    {
        if (m_input == nullptr || !m_edgeEventsEnabled)
        {
            return false;
        }
        if (const auto configured = PortableInputBindings().find(
                std::string(action));
            configured != PortableInputBindings().end()
            && !configured->second.empty())
        {
            if (action == "ToggleView"
                && m_input->WasTouchToggleViewPressed())
            {
                return true;
            }
            return std::ranges::any_of(
                configured->second,
                [this](const PortableInputBinding& binding)
                {
                    return m_input->ControlWasPressed(binding.control);
                });
        }
        if (action == "Restart")
        {
            return m_input->WasPressed("KeyR")
                || m_input->WasGamepadPressed(9);
        }
        if (action == "ToggleView")
        {
            return m_input->WasPressed("KeyC")
                || m_input->WasGamepadPressed(3)
                || m_input->WasTouchToggleViewPressed();
        }
        if (action == "Jump" || action == "Submit")
        {
            return m_input->WasPressed("Space")
                || m_input->WasPressed("Enter")
                || m_input->WasGamepadPressed(0);
        }
        if (action == "Cancel")
        {
            return m_input->WasPressed("Escape")
                || m_input->WasGamepadPressed(1);
        }
        if (action == "Pause")
        {
            return m_input->WasPressed("Escape")
                || m_input->WasPressed("KeyP")
                || m_input->WasGamepadPressed(9);
        }
        return false;
    }

    bool InputSystem::WasReleased(
        std::string_view action,
        float threshold) const
    {
        (void)threshold;
        if (m_input == nullptr || !m_edgeEventsEnabled)
        {
            return false;
        }
        if (const auto configured = PortableInputBindings().find(
                std::string(action));
            configured != PortableInputBindings().end()
            && !configured->second.empty())
        {
            return std::ranges::any_of(
                configured->second,
                [this](const PortableInputBinding& binding)
                {
                    return m_input->ControlWasReleased(binding.control);
                });
        }
        const auto released = [this](const char* code)
        {
            return m_input->WasReleased(code);
        };
        if (action == "MoveHorizontal" || action == "LookHorizontal")
        {
            return released("KeyA") || released("KeyD")
                || released("ArrowLeft") || released("ArrowRight");
        }
        if (action == "MoveVertical" || action == "LookVertical")
        {
            return released("KeyW") || released("KeyS")
                || released("ArrowUp") || released("ArrowDown");
        }
        if (action == "Accelerate")
        {
            return released("KeyW") || released("ArrowUp");
        }
        if (action == "Brake")
        {
            return released("KeyS") || released("ArrowDown");
        }
        if (action == "Restart")
        {
            return released("KeyR");
        }
        if (action == "ToggleView")
        {
            return released("KeyC");
        }
        if (action == "Jump" || action == "Submit")
        {
            return released("Space") || released("Enter");
        }
        if (action == "Cancel" || action == "Pause")
        {
            return released("Escape");
        }
        return false;
    }

    const InputPointerState& InputSystem::Pointer() const noexcept
    {
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
        for (std::size_t index{}; index < m_pointer.buttons.size(); ++index)
        {
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

    const PortableKeyboardState& InputSystem::KeyboardState() const noexcept
    {
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

    Scene& Script::GetScene() const noexcept
    {
        return m_owner->GetScene();
    }

    GraphicsDevice& Script::Graphics() const noexcept
    {
        return GetScene().Graphics();
    }

    GameObject& Script::Owner() const noexcept
    {
        return *m_owner;
    }

    GameObject* Script::Find(const std::string_view name) const noexcept
    {
        return GetScene().FindGameObjectByName(name);
    }

    bool Script::Destroy(GameObject& gameObject)
    {
        return GetScene().DestroyGameObject(gameObject);
    }

    std::string Script::LoadText(
        std::string_view key,
        std::string fallback) const
    {
        const std::string keyText(key);
        char* loaded = LoadPortableText(keyText.c_str());
        if (loaded == nullptr)
        {
            return fallback;
        }
        std::string value(loaded);
        std::free(loaded);
        return value.empty() ? std::move(fallback) : value;
    }

    void Script::SaveText(
        std::string_view key,
        std::string_view value) const
    {
        const std::string keyText(key);
        const std::string valueText(value);
        SavePortableText(keyText.c_str(), valueText.c_str());
    }

    std::int64_t Script::LoadInteger(
        const std::string_view key,
        const std::int64_t fallback) const
    {
        const std::string value = LoadText(key);
        if (value.empty())
        {
            return fallback;
        }

        char* end{};
        const long long parsed = std::strtoll(value.c_str(), &end, 10);
        return end != value.c_str() && end != nullptr && *end == '\0'
            ? static_cast<std::int64_t>(parsed)
            : fallback;
    }

    void Script::SaveInteger(
        const std::string_view key,
        const std::int64_t value) const
    {
        SaveText(key, std::to_string(value));
    }

    bool RegisterPortableScript(
        std::string id,
        std::string,
        ScriptFactory factory)
    {
        return ScriptFactories().emplace(std::move(id), std::move(factory)).second;
    }

    std::unique_ptr<Script> CreatePortableScript(std::string_view id)
    {
        const auto found = ScriptFactories().find(std::string(id));
        return found != ScriptFactories().end() ? found->second() : nullptr;
    }

    GameObject::GameObject(Scene& scene, GameObjectId id, std::string name)
        : m_scene(&scene), m_id(id), m_name(std::move(name))
    {
        m_transform.m_owner = this;
    }

    void GameObject::SetEnabled(const bool enabled) noexcept
    {
        if (m_enabled == enabled)
        {
            return;
        }
        m_enabled = enabled;
        if (!enabled)
        {
            // Portable 2DのSpriteとTextはDOM要素です。Scene::Render()の
            // 後処理を待たず、GameObjectを無効化した時点で非表示にします。
            // RetryやHot Reloadで前フレームのUIが残ることを防ぎます。
            HidePortableObjectUi(static_cast<double>(m_id));
        }
    }

    DirectX::XMMATRIX GameObject::InterpolatedWorldMatrix(float) const
    {
        const Mat4 world = WorldMatrix(*this);
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

    MeshRendererComponent::MeshRendererComponent(
        PrimitiveShape shape,
        DirectX::XMFLOAT4 color,
        std::filesystem::path albedo)
        : m_color(color), m_albedo(std::move(albedo))
    {
        BuildPrimitive(shape, m_vertices, m_indices);
    }

    void MeshRendererComponent::SetProceduralMesh(
        std::vector<ProceduralMeshVertex> vertices,
        std::vector<std::uint32_t> indices,
        bool recalculateNormals)
    {
        if (recalculateNormals)
        {
            RecalculateNormals(vertices, indices);
        }
        m_vertices = std::move(vertices);
        m_indices = std::move(indices);
        m_dirty = true;
    }

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

    void ModelRendererComponent::SetAnimationIndex(std::size_t index) noexcept
    {
        m_animationIndex = m_animations.empty()
            ? index
            : std::min(index, m_animations.size() - 1);
        m_animationTime = 0.0f;
        if (m_loaded)
        {
            ApplyPortablePose();
        }
    }

    std::string_view ModelRendererComponent::AnimationName(
        std::size_t index) const noexcept
    {
        return index < m_animations.size()
            ? std::string_view(m_animations[index].name)
            : std::string_view{};
    }

    float ModelRendererComponent::AnimationDuration() const noexcept
    {
        return m_animationIndex < m_animations.size()
            ? m_animations[m_animationIndex].duration
            : 0.0f;
    }

    void ModelRendererComponent::StopAnimation() noexcept
    {
        m_animationPlaying = false;
        m_animationTime = 0.0f;
        if (m_loaded)
        {
            ApplyPortablePose();
        }
    }

    void ModelRendererComponent::SetAnimationTime(float value) noexcept
    {
        const float duration = AnimationDuration();
        if (!std::isfinite(value) || duration <= 0.0f)
        {
            m_animationTime = 0.0f;
        }
        else if (m_animationLoop)
        {
            m_animationTime = std::fmod(std::max(value, 0.0f), duration);
        }
        else
        {
            m_animationTime = std::clamp(value, 0.0f, duration);
        }
        if (m_loaded)
        {
            ApplyPortablePose();
        }
    }

    bool ModelRendererComponent::LoadPortableModel()
    {
        m_loaded = true;
        m_parts.clear();
        m_nodes.clear();
        m_poseNodes.clear();
        m_nodeWorldMatrices.clear();
        m_skins.clear();
        m_animations.clear();
        if (m_modelPath.empty())
        {
            return false;
        }
        const std::string virtualPath = VirtualAssetPath(m_modelPath);
        const std::string hostPath = virtualPath;
        std::uint32_t byteCount{};
        unsigned char* loadedBytes = LoadPortableAssetBytes(
            hostPath.c_str(),
            &byteCount);
        if (loadedBytes == nullptr || byteCount == 0)
        {
            PublishPortableModelStatus(hostPath.c_str(), "asset-read", 0);
            return false;
        }
        const std::unique_ptr<unsigned char, decltype(&std::free)>
            bytes(loadedBytes, &std::free);
        cgltf_options options{};
        cgltf_data* raw{};
        if (cgltf_parse(&options, bytes.get(), byteCount, &raw)
            != cgltf_result_success)
        {
            PublishPortableModelStatus(hostPath.c_str(), "parse", 0);
            return false;
        }
        const std::unique_ptr<cgltf_data, decltype(&cgltf_free)>
            document(raw, &cgltf_free);
        if (cgltf_load_buffers(&options, document.get(), hostPath.c_str())
            != cgltf_result_success)
        {
            PublishPortableModelStatus(hostPath.c_str(), "buffers", 0);
            return false;
        }
        if (cgltf_validate(document.get()) != cgltf_result_success)
        {
            PublishPortableModelStatus(hostPath.c_str(), "validate", 0);
            return false;
        }
        m_nodes.resize(document->nodes_count);
        for (cgltf_size nodeIndex{}; nodeIndex < document->nodes_count; ++nodeIndex)
        {
            const auto& sourceNode = document->nodes[nodeIndex];
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
            if (node.hasMatrix)
            {
                std::copy_n(sourceNode.matrix, 16, node.matrix.begin());
            }
            else
            {
                node.matrix = Mat4::Identity().values;
            }
        }
        m_poseNodes = m_nodes;
        m_nodeWorldMatrices.resize(
            m_nodes.size(), Mat4::Identity().values);

        m_skins.reserve(document->skins_count);
        for (cgltf_size skinIndex{}; skinIndex < document->skins_count; ++skinIndex)
        {
            const auto& sourceSkin = document->skins[skinIndex];
            ModelSkin skin;
            skin.joints.reserve(sourceSkin.joints_count);
            skin.inverseBindMatrices.reserve(sourceSkin.joints_count);
            for (cgltf_size jointIndex{};
                 jointIndex < sourceSkin.joints_count;
                 ++jointIndex)
            {
                if (sourceSkin.joints[jointIndex] == nullptr)
                {
                    PublishPortableModelStatus(
                        hostPath.c_str(), "skin-joint", 0);
                    return false;
                }
                skin.joints.push_back(static_cast<std::size_t>(
                    sourceSkin.joints[jointIndex] - document->nodes));
                std::array<float, 16> inverseBind = Mat4::Identity().values;
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

        m_animations.reserve(document->animations_count);
        for (cgltf_size animationIndex{};
             animationIndex < document->animations_count;
             ++animationIndex)
        {
            const auto& sourceAnimation = document->animations[animationIndex];
            ModelAnimation animation;
            animation.name = sourceAnimation.name != nullptr
                ? sourceAnimation.name
                : "Animation " + std::to_string(animationIndex + 1);
            for (cgltf_size channelIndex{};
                 channelIndex < sourceAnimation.channels_count;
                 ++channelIndex)
            {
                const auto& sourceChannel = sourceAnimation.channels[channelIndex];
                if (sourceChannel.target_node == nullptr
                    || sourceChannel.sampler == nullptr
                    || sourceChannel.sampler->input == nullptr
                    || sourceChannel.sampler->output == nullptr
                    || sourceChannel.target_path
                        == cgltf_animation_path_type_weights)
                {
                    continue;
                }
                ModelAnimationChannel channel;
                channel.nodeIndex = static_cast<std::size_t>(
                    sourceChannel.target_node - document->nodes);
                channel.path = sourceChannel.target_path
                        == cgltf_animation_path_type_translation
                    ? 0u
                    : sourceChannel.target_path
                        == cgltf_animation_path_type_rotation
                    ? 1u
                    : sourceChannel.target_path
                        == cgltf_animation_path_type_scale
                    ? 2u : 255u;
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
                const cgltf_size keyCount = sourceChannel.sampler->input->count;
                const cgltf_size expectedOutputCount = channel.interpolation == 2u
                    ? keyCount * 3 : keyCount;
                if (keyCount == 0
                    || sourceChannel.sampler->output->count < expectedOutputCount)
                {
                    PublishPortableModelStatus(
                        hostPath.c_str(), "animation-sampler", 0);
                    return false;
                }
                channel.times.resize(keyCount);
                channel.values.resize(keyCount);
                if (channel.interpolation == 2u)
                {
                    channel.inTangents.resize(keyCount);
                    channel.outTangents.resize(keyCount);
                }
                for (cgltf_size keyIndex{}; keyIndex < keyCount; ++keyIndex)
                {
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
                    const cgltf_size valueIndex = channel.interpolation == 2u
                        ? keyIndex * 3 + 1 : keyIndex;
                    float values[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
                    const cgltf_size componentCount = channel.path == 1u ? 4 : 3;
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
                    if (channel.path == 1u)
                    {
                        channel.values[keyIndex] = NormalizeModelQuaternion(
                            channel.values[keyIndex]);
                    }
                    if (channel.interpolation == 2u)
                    {
                        float inValues[4]{};
                        float outValues[4]{};
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
            if (!animation.channels.empty())
            {
                m_animations.emplace_back(std::move(animation));
            }
        }
        for (cgltf_size nodeIndex{}; nodeIndex < document->nodes_count; ++nodeIndex)
        {
            const auto& node = document->nodes[nodeIndex];
            if (node.mesh == nullptr)
            {
                continue;
            }
            float world[16]{};
            cgltf_node_transform_world(&node, world);
            // glTFの表面は反時計回りですが、LamaPon Rendererは時計回りです。
            // ミラー変換されたNodeでは向きが反転することも考慮します。
            const bool reverseWinding = ModelTransformDeterminant(world) >= 0.0f;
            for (cgltf_size primitiveIndex{};
                 primitiveIndex < node.mesh->primitives_count;
                 ++primitiveIndex)
            {
                const auto& primitive = node.mesh->primitives[primitiveIndex];
                if (primitive.type != cgltf_primitive_type_triangles)
                {
                    continue;
                }
                const auto* positions = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_position);
                if (positions == nullptr || positions->count == 0)
                {
                    continue;
                }
                const auto* normals = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_normal);
                const auto* coordinates = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_texcoord);
                const auto* jointIndices = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_joints);
                const auto* jointWeights = FindModelAttribute(
                    primitive,
                    cgltf_attribute_type_weights);
                Part part;
                part.meshNodeIndex = nodeIndex;
                if (node.skin != nullptr)
                {
                    part.skinIndex = static_cast<int>(
                        node.skin - document->skins);
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
                for (cgltf_size vertexIndex{};
                     vertexIndex < positions->count;
                     ++vertexIndex)
                {
                    float positionValues[3]{};
                    float normalValues[3]{ 0.0f, 1.0f, 0.0f };
                    float coordinateValues[2]{};
                    if (!ReadModelFloat(
                            positions,
                            vertexIndex,
                            positionValues,
                            3))
                    {
                        part.vertices.clear();
                        break;
                    }
                    const bool hasNormal = ReadModelFloat(
                        normals, vertexIndex, normalValues, 3);
                    const bool hasCoordinate = ReadModelFloat(
                        coordinates, vertexIndex, coordinateValues, 2);
                    (void)hasNormal;
                    (void)hasCoordinate;
                    ProceduralMeshVertex vertex{
                        { positionValues[0], positionValues[1], positionValues[2] },
                        { normalValues[0], normalValues[1], normalValues[2] },
                        { coordinateValues[0], coordinateValues[1] },
                    };
                    part.vertices.push_back(vertex);
                    part.bindVertices.push_back(vertex);
                    if (part.skinIndex >= 0)
                    {
                        cgltf_uint jointValues[4]{};
                        float weightValues[4]{};
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
                        const auto& skin = m_skins[
                            static_cast<std::size_t>(part.skinIndex)];
                        std::array<std::uint16_t, 4> packedJoints{};
                        for (std::size_t influence{}; influence < 4; ++influence)
                        {
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
                if (part.vertices.empty())
                {
                    continue;
                }
                const cgltf_size indexCount = primitive.indices != nullptr
                    ? primitive.indices->count
                    : positions->count;
                if (indexCount == 0 || indexCount % 3 != 0)
                {
                    continue;
                }
                part.indices.resize(indexCount);
                bool validIndices = true;
                for (cgltf_size index{}; index < indexCount; ++index)
                {
                    const cgltf_size value = primitive.indices != nullptr
                        ? cgltf_accessor_read_index(primitive.indices, index)
                        : index;
                    if (value >= part.vertices.size())
                    {
                        validIndices = false;
                        break;
                    }
                    part.indices[index] = static_cast<std::uint32_t>(value);
                }
                if (!validIndices)
                {
                    continue;
                }
                if (reverseWinding)
                {
                    for (std::size_t index{};
                         index + 2 < part.indices.size();
                         index += 3)
                    {
                        std::swap(part.indices[index + 1], part.indices[index + 2]);
                    }
                }
                if (normals == nullptr)
                {
                    RecalculateNormals(part.vertices, part.indices);
                    part.bindVertices = part.vertices;
                }
                if (primitive.material != nullptr)
                {
                    const auto& material = *primitive.material;
                    part.unlit = material.unlit != 0;
                    part.doubleSided = material.double_sided != 0;
                    part.alphaBlended =
                        material.alpha_mode == cgltf_alpha_mode_blend;
                    part.alphaCutoff = material.alpha_mode == cgltf_alpha_mode_mask
                        ? material.alpha_cutoff : -1.0f;
                    if (material.has_pbr_metallic_roughness)
                    {
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
                            pbr.base_color_texture);
                        part.metallicRoughnessTexture = ModelTexturePath(
                            m_modelPath,
                            pbr.metallic_roughness_texture);
                    }
                    part.normalTexture = ModelTexturePath(
                        m_modelPath,
                        material.normal_texture);
                    part.normalStrength = material.normal_texture.scale;
                    part.occlusionTexture = ModelTexturePath(
                        m_modelPath,
                        material.occlusion_texture);
                    part.occlusionStrength = material.occlusion_texture.scale;
                    part.emissiveTexture = ModelTexturePath(
                        m_modelPath,
                        material.emissive_texture);
                    const float emissiveStrength = material.has_emissive_strength
                        ? material.emissive_strength.emissive_strength
                        : 1.0f;
                    part.emissiveColor = {
                        material.emissive_factor[0] * emissiveStrength,
                        material.emissive_factor[1] * emissiveStrength,
                        material.emissive_factor[2] * emissiveStrength,
                    };
                    const float ior = material.has_ior
                        ? std::max(material.ior.ior, 1.0f)
                        : 1.5f;
                    const float f0 = std::pow(
                        (ior - 1.0f) / (ior + 1.0f), 2.0f);
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
                    if (material.alpha_mode == cgltf_alpha_mode_opaque)
                    {
                        part.color.w = 1.0f;
                    }
                }
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
                    if (!m_albedoTexture.empty())
                    {
                        part.albedoTexture = m_albedoTexture;
                    }
                    if (!m_normalTexture.empty())
                    {
                        part.normalTexture = m_normalTexture;
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

    void ModelRendererComponent::AdvancePortableAnimation(float deltaTime)
    {
        if (!m_animationPlaying || m_animationIndex >= m_animations.size()
            || !std::isfinite(deltaTime))
        {
            return;
        }
        const float duration = m_animations[m_animationIndex].duration;
        if (duration <= 0.0f)
        {
            m_animationPlaying = false;
            return;
        }
        m_animationTime += deltaTime * m_animationSpeed;
        if (m_animationLoop)
        {
            m_animationTime = std::fmod(m_animationTime, duration);
            if (m_animationTime < 0.0f)
            {
                m_animationTime += duration;
            }
        }
        else if (m_animationTime >= duration || m_animationTime <= 0.0f)
        {
            m_animationTime = std::clamp(m_animationTime, 0.0f, duration);
            m_animationPlaying = false;
        }
        ApplyPortablePose();
    }

    void ModelRendererComponent::ApplyPortablePose()
    {
        if (m_nodes.empty())
        {
            return;
        }
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
        if (m_animationIndex < m_animations.size())
        {
            const auto& animation = m_animations[m_animationIndex];
            for (const auto& channel : animation.channels)
            {
                if (channel.nodeIndex >= m_poseNodes.size()
                    || channel.times.empty()
                    || channel.values.size() != channel.times.size())
                {
                    continue;
                }
                std::size_t upper = static_cast<std::size_t>(
                    std::upper_bound(
                        channel.times.begin(),
                        channel.times.end(),
                        m_animationTime) - channel.times.begin());
                std::size_t first{};
                std::size_t second{};
                float amount{};
                float segmentDuration{};
                if (upper == 0)
                {
                    first = second = 0;
                }
                else if (upper >= channel.times.size())
                {
                    first = second = channel.times.size() - 1;
                }
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
                DirectX::XMFLOAT4 value = channel.values[first];
                if (first != second && channel.interpolation != 1u)
                {
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
                        if (channel.path == 1u)
                        {
                            value = NormalizeModelQuaternion(value);
                        }
                    }
                    else if (channel.path == 1u)
                    {
                        value = SlerpModelQuaternion(
                            channel.values[first],
                            channel.values[second],
                            amount);
                    }
                    else
                    {
                        value = LerpModelVector(
                            channel.values[first],
                            channel.values[second],
                            amount);
                    }
                }
                auto& node = m_poseNodes[channel.nodeIndex];
                node.hasMatrix = false;
                if (channel.path == 0u)
                {
                    node.translation = { value.x, value.y, value.z };
                }
                else if (channel.path == 1u)
                {
                    node.rotation = NormalizeModelQuaternion(value);
                }
                else if (channel.path == 2u)
                {
                    node.scale = { value.x, value.y, value.z };
                }
            }
        }

        std::vector<std::uint8_t> matrixStates(m_poseNodes.size());
        const auto calculateWorld = [&](const auto& self, std::size_t index) -> Mat4
        {
            if (index >= m_poseNodes.size())
            {
                return Mat4::Identity();
            }
            if (matrixStates[index] == 2u)
            {
                return ModelMatrix(m_nodeWorldMatrices[index]);
            }
            if (matrixStates[index] == 1u)
            {
                return Mat4::Identity();
            }
            matrixStates[index] = 1u;
            const auto& node = m_poseNodes[index];
            const Mat4 local = node.hasMatrix
                ? ModelMatrix(node.matrix)
                : ModelTrsMatrix(node.translation, node.rotation, node.scale);
            const Mat4 world = node.parent >= 0
                ? LamaPon::Web::Multiply(
                    self(self, static_cast<std::size_t>(node.parent)), local)
                : local;
            m_nodeWorldMatrices[index] = ModelMatrix(world);
            matrixStates[index] = 2u;
            return world;
        };
        for (std::size_t index{}; index < m_poseNodes.size(); ++index)
        {
            calculateWorld(calculateWorld, index);
        }

        for (auto& part : m_parts)
        {
            if (part.vertices.size() != part.bindVertices.size())
            {
                continue;
            }
            if (part.skinIndex < 0)
            {
                const auto& matrix = m_nodeWorldMatrices[part.meshNodeIndex];
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
            const auto& skin = m_skins[static_cast<std::size_t>(part.skinIndex)];
            std::vector<Mat4> jointMatrices;
            jointMatrices.reserve(skin.joints.size());
            for (std::size_t jointIndex{};
                 jointIndex < skin.joints.size();
                 ++jointIndex)
            {
                jointMatrices.push_back(LamaPon::Web::Multiply(
                    ModelMatrix(m_nodeWorldMatrices[skin.joints[jointIndex]]),
                    ModelMatrix(skin.inverseBindMatrices[jointIndex])));
            }
            for (std::size_t vertexIndex{};
                 vertexIndex < part.vertices.size();
                 ++vertexIndex)
            {
                const auto& source = part.bindVertices[vertexIndex];
                const auto& joints = part.joints[vertexIndex];
                const auto& weights = part.weights[vertexIndex];
                const std::array<float, 4> influenceWeights{
                    weights.x, weights.y, weights.z, weights.w };
                Vec3 position{};
                Vec3 normal{};
                float totalWeight{};
                for (std::size_t influence{}; influence < 4; ++influence)
                {
                    const float weight = influenceWeights[influence];
                    if (weight <= 0.000001f
                        || joints[influence] >= jointMatrices.size())
                    {
                        continue;
                    }
                    const auto& matrix = jointMatrices[joints[influence]].values;
                    position += WebVector(TransformModelPoint(
                        matrix.data(), source.position)) * weight;
                    normal += WebVector(TransformModelNormal(
                        matrix.data(), source.normal)) * weight;
                    totalWeight += weight;
                }
                if (totalWeight <= 0.000001f)
                {
                    position = WebVector(source.position);
                    normal = WebVector(source.normal);
                }
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

    void ParticleSystemComponent::Emit(int count)
    {
        const Mat4 world = WorldMatrix(Owner());
        const DirectX::XMFLOAT3 origin{
            world.values[12], world.values[13], world.values[14] };
        const auto nextRandom = [this]() noexcept
        {
            m_randomState = m_randomState * 1664525u + 1013904223u;
            return static_cast<float>(m_randomState >> 8)
                / static_cast<float>(0x00ffffffu);
        };
        for (int index{}; index < count && m_particles.size() < m_capacity; ++index)
        {
            const float random = nextRandom();
            const float randomY = nextRandom();
            const float randomZ = nextRandom();
            Vec3 localOffset{};
            Vec3 localDirection{ 0.0f, 1.0f, 0.0f };
            if (m_shape == ParticleEmitterShape::Sphere)
            {
                const Vec3 direction = LamaPon::Web::Normalize({
                    random * 2.0f - 1.0f,
                    randomY * 2.0f - 1.0f,
                    randomZ * 2.0f - 1.0f,
                });
                const float radius = nextRandom() * 0.5f;
                localOffset = {
                    direction.x * radius * m_emitterSize.x,
                    direction.y * radius * m_emitterSize.y,
                    direction.z * radius * m_emitterSize.z,
                };
                localDirection = direction;
            }
            else if (m_shape == ParticleEmitterShape::Box)
            {
                localOffset = {
                    (random - 0.5f) * m_emitterSize.x,
                    (randomY - 0.5f) * m_emitterSize.y,
                    (randomZ - 0.5f) * m_emitterSize.z,
                };
            }
            else
            {
                const float azimuth =
                    random * std::numbers::pi_v<float> * 2.0f;
                const float angle = std::clamp(
                    m_coneAngle, 0.0f,
                    std::numbers::pi_v<float> * 0.499f)
                    * std::sqrt(randomY);
                const float sine = std::sin(angle);
                localDirection = {
                    std::cos(azimuth) * sine,
                    std::cos(angle),
                    std::sin(azimuth) * sine,
                };
                const float radius = std::sqrt(randomZ) * 0.5f;
                localOffset = {
                    std::cos(azimuth) * radius * m_emitterSize.x,
                    0.0f,
                    std::sin(azimuth) * radius * m_emitterSize.z,
                };
            }
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
            const Vec3 worldOffset = transformDirection(localOffset);
            const Vec3 worldDirection = LamaPon::Web::Normalize(
                transformDirection(localDirection));
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

    void ParticleSystemComponent::Stop(bool clearParticles)
    {
        m_playing = false;
        if (clearParticles)
        {
            m_particles.clear();
        }
    }

    void AudioSourceComponent::SetPitch(float value)
    {
        m_pitch = value;
#if LAMAPON_WEB_AUDIO_ENABLED
        if (m_handle != 0)
        {
            Owner().GetScene().WebAudio().SetPitch(m_handle, value);
        }
#endif
    }

    void AudioSourceComponent::SetVolume(float value)
    {
        m_volume = value;
#if LAMAPON_WEB_AUDIO_ENABLED
        if (m_handle != 0)
        {
            Owner().GetScene().WebAudio().SetVolume(m_handle, value);
        }
#endif
    }

    void AudioSourceComponent::SetPan(float value)
    {
        m_pan = std::clamp(value, -1.0f, 1.0f);
#if LAMAPON_WEB_AUDIO_ENABLED
        if (m_handle != 0)
        {
            Owner().GetScene().WebAudio().SetPan(m_handle, m_pan);
        }
#endif
    }

    void AudioSourceComponent::Play()
    {
#if LAMAPON_WEB_AUDIO_ENABLED
        if (m_loop && m_handle == 0)
        {
            const std::string path = VirtualAssetPath(m_path);
            const auto& position = Owner().GetTransform().position;
            m_handle = Owner().GetScene().WebAudio().PlayLoop(
                path, m_volume, m_pan, m_spatial,
                position.x, position.y, position.z,
                m_minimumDistance, m_maximumDistance);
            Owner().GetScene().WebAudio().SetPitch(m_handle, m_pitch);
        }
        else if (!m_loop)
        {
            PlayOneShot();
        }
#endif
    }

    void AudioSourceComponent::PlayOneShot()
    {
#if LAMAPON_WEB_AUDIO_ENABLED
        const std::string path = VirtualAssetPath(m_path);
        const auto& position = Owner().GetTransform().position;
        Owner().GetScene().WebAudio().PlayWav(
            path, m_volume, false, m_pan, m_spatial,
            position.x, position.y, position.z,
            m_minimumDistance, m_maximumDistance);
#endif
    }

    void AudioSourceComponent::Stop()
    {
#if LAMAPON_WEB_AUDIO_ENABLED
        if (m_handle != 0)
        {
            Owner().GetScene().WebAudio().Stop(m_handle);
            m_handle = 0;
        }
#endif
    }

    void TransformAnimatorComponent::SetTime(float value) noexcept
    {
        if (!std::isfinite(value) || m_duration <= 0.0f)
        {
            m_time = 0.0f;
        }
        else if (m_loop)
        {
            m_time = std::fmod(std::max(value, 0.0f), m_duration);
        }
        else
        {
            m_time = std::clamp(value, 0.0f, m_duration);
        }
        ApplyPortableSample();
    }

    bool TransformAnimatorComponent::LoadPortableClip()
    {
        Json document;
        if (!LoadPortableJsonDocument(m_clipPath, document)
            || document.value("format", "") != "LamaPonAnimationClip"
            || document.value("version", 0) != 1
            || !document.contains("keyframes")
            || !document.at("keyframes").is_array())
        {
            return false;
        }
        const auto readFloat3 = [](const Json& value,
                                   DirectX::XMFLOAT3 fallback)
        {
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
        std::vector<Keyframe> loaded;
        loaded.reserve(document.at("keyframes").size());
        float previous = -1.0f;
        try
        {
            for (const auto& value : document.at("keyframes"))
            {
                const float time = value.at("time").get<float>();
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
        catch (const Json::exception&)
        {
            return false;
        }
        if (loaded.empty() || loaded.size() > 4096)
        {
            return false;
        }
        const float duration = document.value("duration", loaded.back().time);
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

    void TransformAnimatorComponent::AdvancePortableAnimation(float deltaTime)
    {
        if (!m_playing || m_keyframes.empty() || m_duration <= 0.0f)
        {
            return;
        }
        m_time += deltaTime * m_speed;
        if (m_loop)
        {
            m_time = std::fmod(m_time, m_duration);
            if (m_time < 0.0f)
            {
                m_time += m_duration;
            }
        }
        else if (m_time >= m_duration || m_time <= 0.0f)
        {
            m_time = std::clamp(m_time, 0.0f, m_duration);
            m_playing = false;
        }
        ApplyPortableSample();
    }

    void TransformAnimatorComponent::ApplyPortableSample()
    {
        if (m_keyframes.empty())
        {
            return;
        }
        const Keyframe* from = &m_keyframes.front();
        const Keyframe* to = from;
        for (std::size_t index = 1; index < m_keyframes.size(); ++index)
        {
            to = &m_keyframes[index];
            if (m_time <= to->time)
            {
                break;
            }
            from = to;
        }
        float amount{};
        if (to != from && to->time > from->time)
        {
            amount = std::clamp(
                (m_time - from->time) / (to->time - from->time),
                0.0f,
                1.0f);
        }
        const auto lerp = [amount](float left, float right)
        {
            return left + (right - left) * amount;
        };
        const auto lerpAngle = [amount](float left, float right)
        {
            constexpr float TwoPi = std::numbers::pi_v<float> * 2.0f;
            return left + std::remainder(right - left, TwoPi) * amount;
        };
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

    void UIRectTransformComponent::SetAnchorMin(
        const DirectX::XMFLOAT2 value) noexcept
    {
        m_anchorMin = ClampUnit2(value);
        m_anchorMax.x = std::max(m_anchorMax.x, m_anchorMin.x);
        m_anchorMax.y = std::max(m_anchorMax.y, m_anchorMin.y);
    }

    void UIRectTransformComponent::SetAnchorMax(
        const DirectX::XMFLOAT2 value) noexcept
    {
        m_anchorMax = ClampUnit2(value);
        m_anchorMin.x = std::min(m_anchorMin.x, m_anchorMax.x);
        m_anchorMin.y = std::min(m_anchorMin.y, m_anchorMax.y);
    }

    void UIRectTransformComponent::SetPivot(
        const DirectX::XMFLOAT2 value) noexcept
    {
        m_pivot = ClampUnit2(value);
    }

    UIRect UIRectTransformComponent::Resolve(
        const float viewportWidth,
        const float viewportHeight) const noexcept
    {
        UIRect parentRect{
            {},
            {
                std::max(viewportWidth, 1.0f),
                std::max(viewportHeight, 1.0f)
            }
        };
        for (const GameObject* ancestor = Owner().Parent();
             ancestor != nullptr;
             ancestor = ancestor->Parent())
        {
            if (const auto* parentTransform =
                    ancestor->GetComponent<UIRectTransformComponent>())
            {
                parentRect = parentTransform->Resolve(
                    viewportWidth,
                    viewportHeight);
                break;
            }
        }
        const auto parentSize = parentRect.Size();
        const DirectX::XMFLOAT2 anchorPixelsMin{
            parentRect.minimum.x + parentSize.x * m_anchorMin.x,
            parentRect.minimum.y + parentSize.y * m_anchorMin.y
        };
        const DirectX::XMFLOAT2 anchorPixelsMax{
            parentRect.minimum.x + parentSize.x * m_anchorMax.x,
            parentRect.minimum.y + parentSize.y * m_anchorMax.y
        };
        const DirectX::XMFLOAT2 size{
            std::max(anchorPixelsMax.x - anchorPixelsMin.x + m_sizeDelta.x,
                     0.0f),
            std::max(anchorPixelsMax.y - anchorPixelsMin.y + m_sizeDelta.y,
                     0.0f)
        };
        const DirectX::XMFLOAT2 pivotPosition{
            anchorPixelsMin.x
                + (anchorPixelsMax.x - anchorPixelsMin.x) * m_pivot.x
                + m_anchoredPosition.x,
            anchorPixelsMin.y
                + (anchorPixelsMax.y - anchorPixelsMin.y) * m_pivot.y
                + m_anchoredPosition.y
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
          m_fontSize(fontSize), m_color(color), m_bounds(bounds),
          m_wordWrap(wordWrap), m_horizontal(horizontal), m_vertical(vertical)
    {
    }

    SpriteRendererComponent::SpriteRendererComponent(
        DirectX::XMFLOAT2 size,
        DirectX::XMFLOAT4 color,
        std::filesystem::path texture)
        : m_size(size), m_color(color), m_texture(std::move(texture))
    {
    }

    SpriteAnimatorComponent::SpriteAnimatorComponent(
        const int columns,
        const int rows) noexcept
    {
        SetSheetGrid(columns, rows);
    }

    void SpriteAnimatorComponent::SetSheetGrid(
        const int columns,
        const int rows) noexcept
    {
        m_columns = std::max(columns, 1);
        m_rows = std::max(rows, 1);
    }

    void SpriteAnimatorComponent::AddClip(SpriteAnimationClip clip)
    {
        clip.startFrame = std::max(clip.startFrame, 0);
        clip.frameCount = std::max(clip.frameCount, 1);
        clip.framesPerSecond = std::max(clip.framesPerSecond, 0.01f);
        for (auto& existing : m_clips)
        {
            if (existing.name == clip.name)
            {
                existing = std::move(clip);
                return;
            }
        }
        m_clips.push_back(std::move(clip));
    }

    void SpriteAnimatorComponent::RemoveClip(const std::string_view name)
    {
        std::erase_if(
            m_clips,
            [name](const SpriteAnimationClip& clip)
            {
                return clip.name == name;
            });
        if (m_activeClip == name)
        {
            m_activeClip.clear();
            m_playing = false;
            m_currentFrame = -1;
        }
    }

    const SpriteAnimationClip* SpriteAnimatorComponent::FindClip(
        const std::string_view name) const noexcept
    {
        for (const auto& clip : m_clips)
        {
            if (clip.name == name)
            {
                return &clip;
            }
        }
        return nullptr;
    }

    bool SpriteAnimatorComponent::Play(const std::string_view clipName)
    {
        const auto* clip = FindClip(clipName);
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

    void SpriteAnimatorComponent::ApplyFrame(const int sheetFrame)
    {
        m_currentFrame = sheetFrame;
        auto* sprite = Owner().GetComponent<SpriteRendererComponent>();
        if (sprite == nullptr)
        {
            return;
        }
        const int totalFrames = m_columns * m_rows;
        int frame = totalFrames > 0 ? sheetFrame % totalFrames : 0;
        if (frame < 0)
        {
            frame += totalFrames;
        }
        const float width = 1.0f / static_cast<float>(m_columns);
        const float height = 1.0f / static_cast<float>(m_rows);
        sprite->SetSourceRect({
            static_cast<float>(frame % m_columns) * width,
            static_cast<float>(frame / m_columns) * height,
            width,
            height,
        });
    }

    void SpriteAnimatorComponent::Advance(const float deltaTime)
    {
        if (!m_started)
        {
            m_started = true;
            if (m_playOnStart && !m_clips.empty())
            {
                Play(m_defaultClip.empty()
                    ? m_clips.front().name
                    : m_defaultClip);
            }
        }
        const auto* clip = m_playing ? FindClip(m_activeClip) : nullptr;
        if (clip == nullptr)
        {
            return;
        }
        m_time += deltaTime * m_speed;
        const int advanced = static_cast<int>(std::floor(
            m_time * clip->framesPerSecond));
        int index = advanced;
        if (clip->loop)
        {
            index %= clip->frameCount;
            if (index < 0)
            {
                index += clip->frameCount;
            }
        }
        else if (index >= clip->frameCount)
        {
            index = clip->frameCount - 1;
            m_playing = false;
        }
        else
        {
            index = std::max(index, 0);
        }
        ApplyFrame(clip->startFrame + index);
    }

    void ParallaxLayerComponent::Advance(GameObject* mainCamera)
    {
        GameObject* reference = m_reference != nullptr ? m_reference : mainCamera;
        if (reference == nullptr || reference == &Owner())
        {
            return;
        }
        const auto& referencePosition = reference->GetTransform().position;
        auto& own = Owner().GetTransform();
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

    void NativeScriptComponent::OnAttached()
    {
        m_script = CreatePortableScript(m_scriptId);
        if (m_script != nullptr)
        {
            m_script->m_owner = &Owner();
        }
    }

    Scene::Scene(
        Web::Renderer3D& renderer,
        Web::WebAudioRuntime& audio,
        Web::WebInput& input)
        : m_impl(std::make_unique<Impl>()), m_renderer(&renderer), m_audio(&audio)
    {
        m_graphics.Input().Bind(&input);
    }

    Scene::~Scene() = default;

    GameObject& Scene::CreateGameObject(std::string name)
    {
        auto object = std::make_unique<GameObject>(*this, m_nextId++, std::move(name));
        GameObject& reference = *object;
        m_objects.push_back(std::move(object));
        return reference;
    }

    GameObject* Scene::FindGameObjectByName(
        const std::string_view name) noexcept
    {
        for (const auto& object : m_objects)
        {
            if (object != nullptr && object->Name() == name)
            {
                return object.get();
            }
        }
        return nullptr;
    }

    bool Scene::DestroyGameObject(GameObject& gameObject)
    {
        const auto found = std::find_if(
            m_objects.begin(), m_objects.end(),
            [&gameObject](const auto& object)
            {
                return object.get() == &gameObject;
            });
        if (found == m_objects.end())
        {
            return false;
        }

        for (const auto& object : m_objects)
        {
            bool belongsToTree = object.get() == &gameObject;
            for (const GameObject* parent = object->Parent();
                 !belongsToTree && parent != nullptr;
                 parent = parent->Parent())
            {
                belongsToTree = parent == &gameObject;
            }
            if (!belongsToTree)
            {
                continue;
            }
            object->SetEnabled(false);
            if (std::find(
                    m_pendingDestroy.begin(), m_pendingDestroy.end(),
                    object->Id()) == m_pendingDestroy.end())
            {
                m_pendingDestroy.push_back(object->Id());
            }
        }
        return true;
    }

    void Scene::FlushDestroyedObjects()
    {
        if (m_pendingDestroy.empty())
        {
            return;
        }

        const std::unordered_set<GameObjectId> destroyed{
            m_pendingDestroy.begin(), m_pendingDestroy.end()
        };
        for (const auto& object : m_objects)
        {
            if (object->Parent() != nullptr
                && destroyed.contains(object->Parent()->Id()))
            {
                object->SetParent(nullptr);
            }
        }
        std::erase_if(
            m_objects,
            [&destroyed](const auto& object)
            {
                return destroyed.contains(object->Id());
            });
        m_pendingDestroy.clear();
    }

    bool Scene::Load(const std::filesystem::path& virtualPath)
    {
        const std::string path = virtualPath.generic_string();
        char* loaded = LoadPortableAssetText(path.c_str());
        if (loaded == nullptr)
        {
            return false;
        }
        Json document;
        try
        {
            document = Json::parse(loaded);
        }
        catch (const Json::exception&)
        {
            std::free(loaded);
            return false;
        }
        std::free(loaded);
        LoadPortableInputBindings();
        std::unordered_map<std::int64_t, GameObject*> bySourceId;
        struct PendingParent final
        {
            GameObject* object{};
            std::int64_t parent{};
        };
        struct PendingDirectionalLight final
        {
            GameObject* object{};
            DirectX::XMFLOAT3 color{ 1.0f, 0.96f, 0.88f };
            float intensity{ 1.0f };
            bool enabled{ true };
        };
        std::vector<PendingParent> pendingParents;
        std::vector<PendingDirectionalLight> directionalLights;
        const auto float2 = [](const Json& value, DirectX::XMFLOAT2 fallback)
        {
            return value.is_array() && value.size() >= 2
                ? DirectX::XMFLOAT2{
                    value.at(0).get<float>(), value.at(1).get<float>() }
                : fallback;
        };
        const auto float3 = [](const Json& value, DirectX::XMFLOAT3 fallback)
        {
            return value.is_array() && value.size() >= 3
                ? DirectX::XMFLOAT3{
                    value.at(0).get<float>(), value.at(1).get<float>(),
                    value.at(2).get<float>() }
                : fallback;
        };
        const auto float4 = [](const Json& value, DirectX::XMFLOAT4 fallback)
        {
            return value.is_array() && value.size() >= 4
                ? DirectX::XMFLOAT4{
                    value.at(0).get<float>(), value.at(1).get<float>(),
                    value.at(2).get<float>(), value.at(3).get<float>() }
                : fallback;
        };
        for (const auto& objectJson : document.value("objects", Json::array()))
        {
            auto& object = CreateGameObject(objectJson.value("name", "GameObject"));
            const std::int64_t sourceId = objectJson.value("id", 0ll);
            bySourceId[sourceId] = &object;
            // parentを省略したオブジェクトはルートとして扱います。
            // const JSONへの[]は欠落時にassertで停止するため使いません。
            const auto parent = objectJson.find("parent");
            if (parent != objectJson.end() && !parent->is_null())
            {
                pendingParents.push_back({ &object, objectJson.value("parent", 0ll) });
            }
            const auto transform = objectJson.value("transform", Json::object());
            const auto position = transform.value("position", std::vector<float>{});
            const auto rotation = transform.value("rotation", std::vector<float>{});
            const auto scale = transform.value("scale", std::vector<float>{});
            if (position.size() >= 3)
                object.GetTransform().position = { position[0], position[1], position[2] };
            if (rotation.size() >= 3)
                object.GetTransform().rotation = { rotation[0], rotation[1], rotation[2] };
            if (scale.size() >= 3)
                object.GetTransform().scale = { scale[0], scale[1], scale[2] };
            object.SetEnabled(objectJson.value("enabled", true));
            for (const auto& component : objectJson.value("components", Json::array()))
            {
                const std::string type = component.value("type", "");
                if (type == "NativeScript")
                {
                    auto& script = object.AddComponent<NativeScriptComponent>(
                        component.value("script", ""));
                    script.SetEnabled(component.value("enabled", true));
                    if (script.Instance() != nullptr)
                    {
                        const Json properties = component.value(
                            "properties", Json::object());
                        script.Instance()->LoadProperties(properties.dump());
                    }
                }
                else if (type == "Camera")
                {
                    auto& camera = object.AddComponent<CameraComponent>();
                    camera.SetVerticalFieldOfView(
                        component.value("verticalFieldOfView", 1.0471976f));
                    camera.SetNearPlane(component.value("nearPlane", 0.1f));
                    camera.SetFarPlane(component.value("farPlane", 1500.0f));
                    camera.SetEnabled(component.value("enabled", true));
                }
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
                else if (type == "MeshRenderer")
                {
                    Json material = component;
                    const std::filesystem::path materialAsset(
                        component.value("materialAsset", ""));
                    if (!materialAsset.empty())
                    {
                        Json loadedMaterial;
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
                    const std::string shapeName = component.value(
                        "shape", "Cube");
                    const PrimitiveShape shape = shapeName == "Plane"
                        ? PrimitiveShape::Plane
                        : shapeName == "Sphere"
                        ? PrimitiveShape::Sphere
                        : shapeName == "Cylinder"
                        ? PrimitiveShape::Cylinder
                        : PrimitiveShape::Cube;
                    DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
                    if (material.contains("baseColor"))
                    {
                        color = float4(material.at("baseColor"), color);
                    }
                    else if (material.contains("color"))
                    {
                        color = float4(material.at("color"), color);
                    }
                    auto& mesh = object.AddComponent<MeshRendererComponent>(
                        shape,
                        color,
                        std::filesystem::path(
                            material.value("albedoTexture", "")));
                    mesh.SetNormalTexturePath(std::filesystem::path(
                        material.value("normalTexture", "")));
                    mesh.SetRoughness(material.value("roughness", 0.65f));
                    mesh.SetMetallic(material.value("metallic", 0.0f));
                    mesh.SetNormalStrength(material.value(
                        "normalStrength", 1.0f));
                    mesh.SetRoughnessTexturePath(std::filesystem::path(
                        material.value("roughnessTexture", "")));
                    mesh.SetMetallicTexturePath(std::filesystem::path(
                        material.value("metallicTexture", "")));
                    mesh.SetOcclusionTexturePath(std::filesystem::path(
                        material.value("occlusionTexture", "")));
                    mesh.SetOcclusionStrength(material.value(
                        "occlusionStrength", 1.0f));
                    mesh.SetEmissiveTexturePath(std::filesystem::path(
                        material.value("emissiveTexture", "")));
                    if (material.contains("emissiveColor"))
                    {
                        mesh.SetEmissiveColor(float3(
                            material.at("emissiveColor"), {}));
                    }
                    const std::string cullMode = component.value(
                        "cullMode", "Back");
                    mesh.SetCullMode(cullMode == "None"
                        ? ShaderCullMode::None
                        : cullMode == "Front"
                        ? ShaderCullMode::Front
                        : ShaderCullMode::Back);
                    mesh.SetEnabled(component.value("enabled", true));
                }
                else if (type == "ModelRenderer")
                {
                    Json material = component;
                    const std::filesystem::path materialAsset(
                        component.value("materialAsset", ""));
                    if (!materialAsset.empty())
                    {
                        Json loadedMaterial;
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
                    const auto color = material.value(
                        "color",
                        material.value(
                            "baseColor",
                            std::vector<float>{ 1.0f, 1.0f, 1.0f, 1.0f }));
                    auto& model = object.AddComponent<ModelRendererComponent>(
                        std::filesystem::path(component.value("model", "")),
                        component.value("wireframe", false),
                        !materialAsset.empty()
                            || component.value("materialOverride", false),
                        DirectX::XMFLOAT4{
                            color.size() > 0 ? color[0] : 1.0f,
                            color.size() > 1 ? color[1] : 1.0f,
                            color.size() > 2 ? color[2] : 1.0f,
                            color.size() > 3 ? color[3] : 1.0f,
                        },
                        std::filesystem::path(
                            material.value("albedoTexture", "")),
                        std::filesystem::path(
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
                    model.SetRoughnessTexturePath(std::filesystem::path(
                        material.value("roughnessTexture", "")));
                    model.SetMetallicTexturePath(std::filesystem::path(
                        material.value("metallicTexture", "")));
                    model.SetOcclusionTexturePath(std::filesystem::path(
                        material.value("occlusionTexture", "")));
                    model.SetOcclusionStrength(material.value(
                        "occlusionStrength", 1.0f));
                    model.SetEmissiveTexturePath(std::filesystem::path(
                        material.value("emissiveTexture", "")));
                    if (material.contains("emissiveColor"))
                    {
                        model.SetEmissiveColor(float3(
                            material.at("emissiveColor"), {}));
                    }
                    model.SetEnabled(component.value("enabled", true));
                }
                else if (type == "BoxCollider3D")
                {
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
                else if (type == "Rigidbody")
                {
                    auto& body = object.AddComponent<RigidbodyComponent>();
                    body.SetKinematic(component.value("kinematic", false));
                    body.SetUseGravity(component.value("useGravity", true));
                    if (component.contains("velocity"))
                    {
                        body.SetVelocity(float3(component.at("velocity"), {}));
                    }
                    body.SetEnabled(component.value("enabled", true));
                }
                else if (type == "ParticleSystem")
                {
                    const std::string shapeName = component.value(
                        "shape", "Point");
                    const ParticleEmitterShape shape = shapeName == "Sphere"
                        ? ParticleEmitterShape::Sphere
                        : shapeName == "Box"
                        ? ParticleEmitterShape::Box
                        : ParticleEmitterShape::Cone;
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
                        std::filesystem::path(component.value("texture", "")));
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
                    if (!component.value("playOnStart", true))
                    {
                        particles.SetPlayOnStart(false);
                    }
                    particles.SetEnabled(component.value("enabled", true));
                }
                else if (type == "AudioSource")
                {
                    auto& audio = object.AddComponent<AudioSourceComponent>(
                        std::filesystem::path(component.value("audio", "")),
                        component.value("volume", 1.0f));
                    audio.SetLoop(component.value("loop", false));
                    audio.SetPitch(component.value("pitch", 0.0f));
                    audio.SetPan(component.value("pan", 0.0f));
                    audio.SetSpatial(component.value("spatial", false));
                    audio.SetMinimumDistance(component.value(
                        "minimumDistance", 1.0f));
                    audio.SetMaximumDistance(component.value(
                        "maximumDistance", 20.0f));
                    audio.m_playOnStart = component.value("playOnStart", false);
                    audio.SetEnabled(component.value("enabled", true));
                }
                else if (type == "TransformAnimator")
                {
                    const std::filesystem::path controller(
                        component.value("controller", ""));
                    if (!controller.empty())
                    {
                        return false;
                    }
                    auto& animator =
                        object.AddComponent<TransformAnimatorComponent>(
                            std::filesystem::path(
                                component.value("clip", "")),
                            component.value("speed", 1.0f),
                            component.value("loop", true),
                            component.value("playOnStart", true));
                    if (!animator.LoadPortableClip())
                    {
                        return false;
                    }
                    animator.SetEnabled(component.value("enabled", true));
                }
                else if (type == "Rotator")
                {
                    auto& rotator = object.AddComponent<RotatorComponent>(
                        component.contains("angularVelocity")
                            ? float3(component.at("angularVelocity"),
                                { 0.0f, 1.0f, 0.0f })
                            : DirectX::XMFLOAT3{ 0.0f, 1.0f, 0.0f });
                    rotator.SetEnabled(component.value("enabled", true));
                }
                else if (type == "InputMover")
                {
                    auto& mover = object.AddComponent<InputMoverComponent>(
                        component.value("horizontalAction", "MoveHorizontal"),
                        component.value("verticalAction", "MoveVertical"),
                        component.value("speed", 3.0f));
                    mover.SetEnabled(component.value("enabled", true));
                }
                else if (type == "UIRectTransform")
                {
                    // Windows版のSceneに保存されたアンカー設定を読み込み、
                    // Web版でも同じビューポート基準のUI配置へ復元します。
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
                else if (type == "TextRenderer")
                {
                    const std::string horizontalName = component.value(
                        "horizontalAlignment", "Left");
                    const std::string verticalName = component.value(
                        "verticalAlignment", "Top");
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
                    text.SetFontAsset(std::filesystem::path(
                        component.value("fontAsset", "")));
                    text.SetEnabled(component.value("enabled", true));
                }
                else if (type == "SpriteRenderer")
                {
                    auto& sprite = object.AddComponent<SpriteRendererComponent>(
                        component.contains("size")
                            ? float2(component.at("size"), { 128.0f, 128.0f })
                            : DirectX::XMFLOAT2{ 128.0f, 128.0f },
                        component.contains("color")
                            ? float4(component.at("color"),
                                { 1.0f, 1.0f, 1.0f, 1.0f })
                            : DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f },
                        std::filesystem::path(component.value("texture", "")));
                    if (component.contains("pivot"))
                    {
                        sprite.SetPivot(float2(component.at("pivot"), {}));
                    }
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
                else if (type == "SpriteMask")
                {
                    auto& mask = object.AddComponent<SpriteMaskComponent>(
                        component.value("shape", "Rectangle") == "Circle"
                            ? SpriteMaskShape::Circle
                            : SpriteMaskShape::Rectangle,
                        component.contains("size")
                            ? float2(component.at("size"), { 128.0f, 128.0f })
                            : DirectX::XMFLOAT2{ 128.0f, 128.0f });
                    mask.SetEnabled(component.value("enabled", true));
                }
                else if (type == "SpriteAnimator")
                {
                    auto& animator = object.AddComponent<SpriteAnimatorComponent>(
                        component.value("columns", 1),
                        component.value("rows", 1));
                    animator.SetSpeed(component.value("speed", 1.0f));
                    animator.SetDefaultClip(component.value(
                        "defaultClip", std::string{}));
                    animator.SetPlayOnStart(component.value(
                        "playOnStart", true));
                    for (const auto& clip : component.value(
                        "clips", Json::array()))
                    {
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
                else if (type == "ParallaxLayer")
                {
                    auto& parallax = object.AddComponent<ParallaxLayerComponent>(
                        component.contains("factor")
                            ? float2(component.at("factor"), { 0.5f, 0.5f })
                            : DirectX::XMFLOAT2{ 0.5f, 0.5f },
                        component.value("referenceId", GameObjectId{}));
                    parallax.SetEnabled(component.value("enabled", true));
                }
                else if (type == "RenderCulling")
                {
                    auto& culling = object.AddComponent<RenderCullingComponent>(
                        component.value("alwaysVisible", false),
                        component.value("cullingMargin", 0.0f));
                    culling.SetEnabled(component.value("enabled", true));
                }
                else if (type == "AudioListener")
                {
                    // Web AudioはAudioContextごとにListenerを1つ管理します。
                }
            }
        }
        for (const PendingParent& pending : pendingParents)
        {
            const auto found = bySourceId.find(pending.parent);
            if (found != bySourceId.end())
            {
                pending.object->SetParent(found->second);
            }
        }

        const auto mainCameraId = document.value("mainCamera", 0ll);
        if (const auto camera = bySourceId.find(mainCameraId);
            camera != bySourceId.end()
            && camera->second->GetComponent<CameraComponent>() != nullptr)
        {
            m_impl->mainCamera = camera->second;
        }
        if (m_impl->mainCamera == nullptr)
        {
            for (const auto& object : m_objects)
            {
                if (object->GetComponent<CameraComponent>() != nullptr)
                {
                    m_impl->mainCamera = object.get();
                    break;
                }
            }
        }
        for (const auto& object : m_objects)
        {
            if (auto* parallax = object->GetComponent<ParallaxLayerComponent>();
                parallax != nullptr && parallax->m_referenceSourceId != 0)
            {
                const auto reference = bySourceId.find(
                    static_cast<std::int64_t>(parallax->m_referenceSourceId));
                parallax->m_reference = reference != bySourceId.end()
                    ? reference->second
                    : nullptr;
            }
        }

        const auto environment = document.value("environment", Json::object());
        const auto ambient = environment.value("ambientColor", std::vector<float>{});
        const auto fog = environment.value("fog", Json::object());
        const auto fogColor = fog.value("color", std::vector<float>{});
        m_renderer->SetFog({
            fog.value("enabled", false),
            fogColor.size() >= 3
                ? Web::Color{ fogColor[0], fogColor[1], fogColor[2], 1.0f }
                : Web::Color{},
            fog.value("startDistance", 110.0f),
            fog.value("endDistance", 520.0f),
        });
        Web::Vec3 lightDirection{ 0.6808f, -0.4078f, 0.6083f };
        Web::Color lightColor{ 1.0f, 0.76f, 0.52f, 1.0f };
        float lightIntensity = 1.35f;
        for (const auto& light : directionalLights)
        {
            if (!light.enabled || !light.object->IsEnabled())
            {
                continue;
            }
            const Mat4 world = WorldMatrix(*light.object);
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
            environment.value("ambientIntensity", 0.52f),
            lightDirection,
            lightColor,
            lightIntensity,
        });
        const auto sky = environment.value("sky", Json::object());
        const auto top = sky.value("topColor", std::vector<float>{});
        const auto horizon = sky.value("horizonColor", std::vector<float>{});
        m_renderer->SetSky({
            sky.value("enabled", false),
            top.size() >= 3
                ? Web::Color{ top[0], top[1], top[2], 1.0f }
                : Web::Color{},
            horizon.size() >= 3
                ? Web::Color{ horizon[0], horizon[1], horizon[2], 1.0f }
                : Web::Color{},
        });
        if (horizon.size() >= 3)
        {
            m_impl->clearColor = { horizon[0], horizon[1], horizon[2], 1.0f };
        }
        return true;
    }

    void Scene::StartScripts()
    {
        FlushDestroyedObjects();
        for (std::size_t objectIndex{}; objectIndex < m_objects.size(); ++objectIndex)
        {
            if (!m_objects[objectIndex]->IsEnabled())
            {
                continue;
            }
            for (const auto& component : m_objects[objectIndex]->Components())
            {
                if (!component->IsEnabled())
                {
                    continue;
                }
                auto* native = dynamic_cast<NativeScriptComponent*>(component.get());
                Script* script = native != nullptr ? native->Instance() : nullptr;
                if (script != nullptr && !script->m_started)
                {
                    script->m_started = true;
                    script->Start();
                }
                auto* audio = dynamic_cast<AudioSourceComponent*>(component.get());
                if (audio != nullptr && audio->IsEnabled()
                    && audio->m_playOnStart)
                {
                    audio->m_playOnStart = false;
                    audio->Play();
                }
            }
        }
    }

    void Scene::FixedUpdate(float deltaTime)
    {
        FlushDestroyedObjects();
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
                if (auto* native = dynamic_cast<NativeScriptComponent*>(component.get());
                    native != nullptr && native->Instance() != nullptr)
                {
                    native->Instance()->FixedUpdate(deltaTime);
                }
            }
        }
        for (const auto& object : m_objects)
        {
            if (auto* body = object->GetComponent<RigidbodyComponent>();
                object->IsEnabled() && body != nullptr && body->IsEnabled())
            {
                auto velocity = body->Velocity();
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
            Vec3 center{};
            Vec3 half{};
        };
        const auto boundsFor = [](const GameObject& object,
                                  const BoxCollider3DComponent& collider)
        {
            const Mat4 world = WorldMatrix(object);
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
        const auto dispatchCollision = [](GameObject& object,
                                          const CollisionEvent& event,
                                          const bool entered)
        {
            for (const auto& component : object.Components())
            {
                auto* native = dynamic_cast<NativeScriptComponent*>(
                    component.get());
                if (native == nullptr || !native->IsEnabled()
                    || native->Instance() == nullptr)
                {
                    continue;
                }
                if (entered)
                {
                    native->Instance()->OnCollisionEnter(event);
                }
                else
                {
                    native->Instance()->OnCollisionStay(event);
                }
            }
        };

        std::unordered_set<Impl::ContactKey, Impl::ContactHash> contacts;
        for (std::size_t firstIndex{}; firstIndex < m_objects.size(); ++firstIndex)
        {
            GameObject& first = *m_objects[firstIndex];
            auto* firstCollider = first.GetComponent<BoxCollider3DComponent>();
            if (!first.IsEnabled() || firstCollider == nullptr
                || !firstCollider->IsEnabled())
            {
                continue;
            }
            for (std::size_t secondIndex = firstIndex + 1;
                 secondIndex < m_objects.size(); ++secondIndex)
            {
                GameObject& second = *m_objects[secondIndex];
                auto* secondCollider = second.GetComponent<BoxCollider3DComponent>();
                if (!second.IsEnabled() || secondCollider == nullptr
                    || !secondCollider->IsEnabled())
                {
                    continue;
                }
                const std::uint32_t firstLayer = firstCollider->m_layer % 32u;
                const std::uint32_t secondLayer = secondCollider->m_layer % 32u;
                if ((firstCollider->m_mask & (1u << secondLayer)) == 0
                    || (secondCollider->m_mask & (1u << firstLayer)) == 0)
                {
                    continue;
                }
                const Bounds firstBounds = boundsFor(first, *firstCollider);
                const Bounds secondBounds = boundsFor(second, *secondCollider);
                const Vec3 delta = firstBounds.center - secondBounds.center;
                const Vec3 overlap{
                    firstBounds.half.x + secondBounds.half.x
                        - std::abs(delta.x),
                    firstBounds.half.y + secondBounds.half.y
                        - std::abs(delta.y),
                    firstBounds.half.z + secondBounds.half.z
                        - std::abs(delta.z),
                };
                if (overlap.x <= 0.0f || overlap.y <= 0.0f
                    || overlap.z <= 0.0f)
                {
                    continue;
                }

                Vec3 normal{ delta.x < 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f };
                float penetration = overlap.x;
                if (overlap.y < penetration)
                {
                    normal = { 0.0f, delta.y < 0.0f ? -1.0f : 1.0f, 0.0f };
                    penetration = overlap.y;
                }
                if (overlap.z < penetration)
                {
                    normal = { 0.0f, 0.0f, delta.z < 0.0f ? -1.0f : 1.0f };
                    penetration = overlap.z;
                }
                const Impl::ContactKey key{
                    std::min(first.Id(), second.Id()),
                    std::max(first.Id(), second.Id()),
                };
                contacts.insert(key);
                const bool entered = !m_impl->contacts.contains(key);
                const bool trigger = firstCollider->m_trigger
                    || secondCollider->m_trigger;
                const Vec3 point = (firstBounds.center + secondBounds.center)
                    * 0.5f;
                dispatchCollision(first, {
                    DirectXVector(normal), DirectXVector(point),
                    penetration, trigger,
                }, entered);
                dispatchCollision(second, {
                    DirectXVector(normal * -1.0f), DirectXVector(point),
                    penetration, trigger,
                }, entered);

                auto* firstBody = first.GetComponent<RigidbodyComponent>();
                auto* secondBody = second.GetComponent<RigidbodyComponent>();
                const bool firstDynamic = firstBody != nullptr
                    && firstBody->IsEnabled() && !firstBody->m_kinematic;
                const bool secondDynamic = secondBody != nullptr
                    && secondBody->IsEnabled() && !secondBody->m_kinematic;
                if (trigger || (!firstDynamic && !secondDynamic))
                {
                    continue;
                }
                const float firstDistance = secondDynamic
                    ? penetration * 0.5f
                    : penetration;
                const float secondDistance = firstDynamic
                    ? penetration * 0.5f
                    : penetration;
                if (firstDynamic)
                {
                    auto& position = first.GetTransform().position;
                    position.x += normal.x * firstDistance;
                    position.y += normal.y * firstDistance;
                    position.z += normal.z * firstDistance;
                    const float inward = firstBody->m_velocity.x * normal.x
                        + firstBody->m_velocity.y * normal.y
                        + firstBody->m_velocity.z * normal.z;
                    if (inward < 0.0f)
                    {
                        firstBody->m_velocity.x -= normal.x * inward;
                        firstBody->m_velocity.y -= normal.y * inward;
                        firstBody->m_velocity.z -= normal.z * inward;
                    }
                }
                if (secondDynamic)
                {
                    auto& position = second.GetTransform().position;
                    position.x -= normal.x * secondDistance;
                    position.y -= normal.y * secondDistance;
                    position.z -= normal.z * secondDistance;
                    const Vec3 secondNormal = normal * -1.0f;
                    const float inward = secondBody->m_velocity.x * secondNormal.x
                        + secondBody->m_velocity.y * secondNormal.y
                        + secondBody->m_velocity.z * secondNormal.z;
                    if (inward < 0.0f)
                    {
                        secondBody->m_velocity.x -= secondNormal.x * inward;
                        secondBody->m_velocity.y -= secondNormal.y * inward;
                        secondBody->m_velocity.z -= secondNormal.z * inward;
                    }
                }
            }
        }
        m_impl->contacts = std::move(contacts);
    }

    void Scene::Update(float deltaTime)
    {
        FlushDestroyedObjects();
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
                if (auto* native = dynamic_cast<NativeScriptComponent*>(component.get());
                    native != nullptr && native->Instance() != nullptr)
                {
                    native->Instance()->Update(deltaTime);
                }
                if (auto* particles = dynamic_cast<ParticleSystemComponent*>(component.get()))
                {
                    if (particles->m_playing && particles->m_duration > 0.0f)
                    {
                        particles->m_emittingTime += deltaTime;
                        if (particles->m_emittingTime >= particles->m_duration)
                        {
                            if (particles->m_looping)
                            {
                                particles->m_emittingTime = std::fmod(
                                    particles->m_emittingTime,
                                    particles->m_duration);
                            }
                            else
                            {
                                particles->m_playing = false;
                            }
                        }
                    }
                    if (particles->m_playing && particles->m_emissionRate > 0.0f)
                    {
                        particles->m_emissionAccumulator +=
                            particles->m_emissionRate * deltaTime;
                        const int emissionCount = static_cast<int>(
                            particles->m_emissionAccumulator);
                        if (emissionCount > 0)
                        {
                            particles->m_emissionAccumulator -=
                                static_cast<float>(emissionCount);
                            particles->Emit(emissionCount);
                        }
                    }
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
                    std::erase_if(
                        particles->m_particles,
                        [](const auto& particle)
                        {
                            return particle.age >= particle.lifetime;
                        });
                }
                if (auto* model = dynamic_cast<ModelRendererComponent*>(
                        component.get()))
                {
                    model->AdvancePortableAnimation(deltaTime);
                }
                if (auto* animator =
                        dynamic_cast<TransformAnimatorComponent*>(
                            component.get()))
                {
                    animator->AdvancePortableAnimation(deltaTime);
                }
                if (auto* rotator = dynamic_cast<RotatorComponent*>(
                        component.get()))
                {
                    auto& rotation = object->GetTransform().rotation;
                    rotation.x += rotator->m_angularVelocity.x * deltaTime;
                    rotation.y += rotator->m_angularVelocity.y * deltaTime;
                    rotation.z += rotator->m_angularVelocity.z * deltaTime;
                }
                if (auto* mover = dynamic_cast<InputMoverComponent*>(
                        component.get()))
                {
                    float horizontal = m_graphics.Input().Value(
                        mover->m_horizontalAction);
                    float vertical = m_graphics.Input().Value(
                        mover->m_verticalAction);
                    const float magnitude = std::sqrt(
                        horizontal * horizontal + vertical * vertical);
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
                if (auto* spriteAnimator =
                        dynamic_cast<SpriteAnimatorComponent*>(component.get()))
                {
                    spriteAnimator->Advance(deltaTime);
                }
                if (auto* parallax = dynamic_cast<ParallaxLayerComponent*>(
                        component.get()))
                {
                    parallax->Advance(m_impl->mainCamera);
                }
#if LAMAPON_WEB_AUDIO_ENABLED
                if (auto* audio = dynamic_cast<AudioSourceComponent*>(
                        component.get());
                    audio != nullptr && audio->m_spatial
                        && audio->m_handle != 0)
                {
                    const Mat4 world = WorldMatrix(*object);
                    m_audio->SetPosition(
                        audio->m_handle,
                        world.values[12],
                        world.values[13],
                        world.values[14]);
                }
#endif
            }
        }
    }

    void Scene::Render()
    {
        BeginPortableUiFrame();
        Vec3 cameraRight{ 1.0f, 0.0f, 0.0f };
        Vec3 cameraUp{ 0.0f, 1.0f, 0.0f };
        if (auto* camera = m_impl->mainCamera != nullptr
                ? m_impl->mainCamera->GetComponent<CameraComponent>()
                : FindComponentOfType<CameraComponent>())
        {
            const Mat4 world = WorldMatrix(camera->Owner());
            const Vec3 position{
                world.values[12], world.values[13], world.values[14] };
            const Vec3 forward = LamaPon::Web::Normalize({
                -world.values[8], -world.values[9], -world.values[10] });
            cameraRight = LamaPon::Web::Normalize({
                world.values[0], world.values[1], world.values[2] });
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
        for (const auto& object : m_objects)
        {
            if (!object->IsEnabled())
            {
                continue;
            }
            const Mat4 model = WorldMatrix(*object);
            for (const auto& component : object->Components())
            {
                if (!component->IsEnabled())
                {
                    continue;
                }
                if (auto* mesh = dynamic_cast<MeshRendererComponent*>(component.get()))
                {
                    if (mesh->m_dirty || mesh->m_webMesh == 0)
                    {
                        const auto vertices = WebVertices(mesh->m_vertices);
                        const auto indices = WebIndices(mesh->m_indices);
                        if (mesh->m_webMesh == 0)
                            mesh->m_webMesh = m_renderer->CreateMesh(vertices, indices);
                        else
                            m_renderer->UpdateMesh(mesh->m_webMesh, vertices, indices);
                        mesh->m_dirty = false;
                    }
                    if (mesh->m_webTexture == 0 && !mesh->m_albedo.empty())
                    {
                        const std::string path = VirtualAssetPath(mesh->m_albedo);
                        mesh->m_webTexture = m_renderer->CreateTexture(path.c_str());
                    }
                    if (mesh->m_webNormalTexture == 0 && !mesh->m_normal.empty())
                    {
                        const std::string path = VirtualAssetPath(mesh->m_normal);
                        mesh->m_webNormalTexture = m_renderer->CreateTexture(
                            path.c_str());
                    }
                    const auto loadMaterialTexture = [this](
                        std::uint32_t& id,
                        const std::filesystem::path& asset)
                    {
                        if (id == 0 && !asset.empty())
                        {
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
                else if (auto* imported =
                             dynamic_cast<ModelRendererComponent*>(component.get()))
                {
                    if (!imported->m_loaded)
                    {
                        const bool loaded = imported->LoadPortableModel();
                        (void)loaded;
                    }
                    // 不透明Geometryを先に描いてDepth Bufferを確定します。
                    // WindowやDecalのAlpha Blendにより、後続の不透明な車体が
                    // 半透明に見える問題を防ぎます。
                    for (int blendPass{}; blendPass < 2; ++blendPass)
                    {
                        for (auto& part : imported->m_parts)
                        {
                            const bool transparent = part.alphaBlended
                                || part.color.w < 0.999f;
                            if (transparent != (blendPass == 1))
                            {
                                continue;
                            }
                            if (part.webMesh == 0)
                            {
                            part.webMesh = m_renderer->CreateMesh(
                                WebVertices(part.vertices),
                                WebIndices(part.indices));
                            part.dirty = false;
                            }
                            else if (part.dirty)
                            {
                            m_renderer->UpdateMesh(
                                part.webMesh,
                                WebVertices(part.vertices),
                                WebIndices(part.indices));
                            part.dirty = false;
                            }
                            if (part.webTexture == 0 && !part.albedoTexture.empty())
                            {
                            const std::string path = VirtualAssetPath(
                                part.albedoTexture);
                            part.webTexture = m_renderer->CreateTexture(path.c_str());
                            }
                            if (part.webNormalTexture == 0
                                && !part.normalTexture.empty())
                            {
                            const std::string path = VirtualAssetPath(
                                part.normalTexture);
                            part.webNormalTexture = m_renderer->CreateTexture(
                                path.c_str());
                            }
                            if (part.webMetallicRoughnessTexture == 0
                                && !part.metallicRoughnessTexture.empty())
                            {
                            const std::string path = VirtualAssetPath(
                                part.metallicRoughnessTexture);
                            part.webMetallicRoughnessTexture =
                                m_renderer->CreateTexture(path.c_str());
                            }
                            const auto loadMaterialTexture = [this](
                            std::uint32_t& id,
                            const std::filesystem::path& asset)
                            {
                            if (id == 0 && !asset.empty())
                            {
                                const std::string path = VirtualAssetPath(asset);
                                id = m_renderer->CreateTexture(path.c_str());
                            }
                            };
                            loadMaterialTexture(
                            part.webRoughnessTexture, part.roughnessTexture);
                            loadMaterialTexture(
                            part.webMetallicTexture, part.metallicTexture);
                            loadMaterialTexture(
                            part.webOcclusionTexture, part.occlusionTexture);
                            loadMaterialTexture(
                            part.webEmissiveTexture, part.emissiveTexture);
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
                else if (auto* particles =
                             dynamic_cast<ParticleSystemComponent*>(component.get());
                         particles != nullptr && !particles->m_particles.empty())
                {
                    std::vector<Web::Vertex3D> vertices;
                    std::vector<std::uint32_t> indices;
                    vertices.reserve(particles->m_particles.size() * 4);
                    indices.reserve(particles->m_particles.size() * 6);
                    Web::Color color{ 0.0f, 0.0f, 0.0f, 0.0f };
                    for (const auto& particle : particles->m_particles)
                    {
                        const float cosine = std::cos(particle.rotation);
                        const float sine = std::sin(particle.rotation);
                        const Vec3 baseRight =
                            particles->m_renderMode
                                    == ParticleRenderMode::Horizontal
                                ? Vec3{ 1.0f, 0.0f, 0.0f }
                                : cameraRight;
                        const Vec3 baseUp =
                            particles->m_renderMode
                                    == ParticleRenderMode::Horizontal
                                ? Vec3{ 0.0f, 0.0f, 1.0f }
                                : cameraUp;
                        const Vec3 right =
                            baseRight * cosine + baseUp * sine;
                        const Vec3 up =
                            baseUp * cosine - baseRight * sine;
                        const float life = std::clamp(
                            particle.age / particle.lifetime, 0.0f, 1.0f);
                        const float displaySize = particle.size
                            * (1.0f
                                + (particles->m_endSizeMultiplier - 1.0f)
                                    * life);
                        const float halfSize = displaySize * 0.5f;
                        const Vec3 center = WebVector(particle.position);
                        const Vec3 horizontal = right * halfSize;
                        const Vec3 vertical = up * halfSize;
                        const std::uint32_t base = static_cast<std::uint32_t>(
                            vertices.size());
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
                    if (particles->m_webMesh == 0)
                    {
                        particles->m_webMesh =
                            m_renderer->CreateMesh(vertices, indices);
                    }
                    else
                    {
                        m_renderer->UpdateMesh(
                            particles->m_webMesh,
                            vertices,
                            indices);
                    }
                    if (particles->m_webTexture == 0
                        && !particles->m_texture.empty())
                    {
                        const std::string path =
                            VirtualAssetPath(particles->m_texture);
                        particles->m_webTexture =
                            m_renderer->CreateTexture(path.c_str());
                    }
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
                else if (auto* text = dynamic_cast<TextRendererComponent*>(component.get()))
                {
                    float textX = model.values[12];
                    float textY = model.values[13];
                    float textWidth = text->m_bounds.x;
                    float textHeight = text->m_bounds.y;
                    if (const auto* rect =
                            object->GetComponent<UIRectTransformComponent>())
                    {
                        const auto resolved = rect->Resolve(
                            static_cast<float>(m_graphics.UIWidth()),
                            static_cast<float>(m_graphics.UIHeight()));
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
                else if (auto* mask =
                             dynamic_cast<SpriteMaskComponent*>(component.get()))
                {
                    const float scaleX = std::hypot(
                        model.values[0], model.values[1]);
                    const float scaleY = std::hypot(
                        model.values[4], model.values[5]);
                    RenderPortableMask(
                        static_cast<double>(object->Id()),
                        model.values[12], model.values[13],
                        mask->m_size.x * scaleX,
                        mask->m_size.y * scaleY,
                        static_cast<int>(mask->m_shape));
                }
                else if (auto* sprite = dynamic_cast<SpriteRendererComponent*>(component.get()))
                {
                    const std::string path = VirtualAssetPath(sprite->m_texture);
                    const float scaleX = std::hypot(
                        model.values[0], model.values[1]);
                    const float scaleY = std::hypot(
                        model.values[4], model.values[5]);
                    const float rotation = std::atan2(
                        model.values[1], model.values[0]);
                    float spriteX = model.values[12];
                    float spriteY = model.values[13];
                    float spriteWidth = sprite->m_size.x * scaleX;
                    float spriteHeight = sprite->m_size.y * scaleY;
                    float pivotX = sprite->m_pivot.x;
                    float pivotY = sprite->m_pivot.y;
                    if (const auto* rect =
                            object->GetComponent<UIRectTransformComponent>())
                    {
                        // Rect Transform付きSpriteはWindows版と同じく、
                        // 解決済み矩形の中央を基準に配置して矩形全体へ伸縮します。
                        const auto resolved = rect->Resolve(
                            static_cast<float>(m_graphics.UIWidth()),
                            static_cast<float>(m_graphics.UIHeight()));
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

    bool Scene::Raycast(
        const Ray& ray,
        float maximumDistance,
        PhysicsHit& hit,
        const PhysicsQueryFilter& filter) const
    {
        const Vec3 origin = WebVector(ray.origin);
        const Vec3 direction = LamaPon::Web::Normalize(WebVector(ray.direction));
        float nearest = maximumDistance;
        bool found{};
        for (const auto& object : m_objects)
        {
            if (!object->IsEnabled()
                || object->Id() == filter.ignoredGameObjectId)
            {
                continue;
            }
            const Mat4 world = WorldMatrix(*object);
            if (const auto* box = object->GetComponent<BoxCollider3DComponent>();
                box != nullptr && box->IsEnabled()
                && (filter.layerMask & (1u << (box->m_layer % 32u))) != 0)
            {
                const Vec3 center = TransformPoint(
                    world, WebVector(box->m_offset));
                const Vec3 localHalf{
                    std::abs(box->m_size.x) * 0.5f,
                    std::abs(box->m_size.y) * 0.5f,
                    std::abs(box->m_size.z) * 0.5f,
                };
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
                float entry{};
                float exit = nearest;
                Vec3 entryNormal{};
                const auto intersectAxis = [&entry, &exit, &entryNormal](
                    const float rayOrigin,
                    const float rayDirection,
                    const float minimum,
                    const float maximum,
                    const Vec3& negativeNormal,
                    const Vec3& positiveNormal)
                {
                    if (std::abs(rayDirection) <= 0.000001f)
                    {
                        return rayOrigin >= minimum && rayOrigin <= maximum;
                    }
                    float nearDistance = (minimum - rayOrigin) / rayDirection;
                    float farDistance = (maximum - rayOrigin) / rayDirection;
                    Vec3 nearNormal = negativeNormal;
                    if (nearDistance > farDistance)
                    {
                        std::swap(nearDistance, farDistance);
                        nearNormal = positiveNormal;
                    }
                    if (nearDistance > entry)
                    {
                        entry = nearDistance;
                        entryNormal = nearNormal;
                    }
                    exit = std::min(exit, farDistance);
                    return entry <= exit;
                };
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
            const auto* collider = object->GetComponent<MeshCollider3DComponent>();
            if (collider == nullptr || !collider->IsEnabled()
                || (filter.layerMask & (1u << (collider->m_layer % 32u))) == 0)
            {
                continue;
            }
            for (std::size_t index{}; index + 2 < collider->m_indices.size(); index += 3)
            {
                const std::uint32_t ia = collider->m_indices[index];
                const std::uint32_t ib = collider->m_indices[index + 1];
                const std::uint32_t ic = collider->m_indices[index + 2];
                if (ia >= collider->m_vertices.size()
                    || ib >= collider->m_vertices.size()
                    || ic >= collider->m_vertices.size())
                {
                    continue;
                }
                const Vec3 a = TransformPoint(world, WebVector(collider->m_vertices[ia]));
                const Vec3 b = TransformPoint(world, WebVector(collider->m_vertices[ib]));
                const Vec3 c = TransformPoint(world, WebVector(collider->m_vertices[ic]));
                float distance{};
                Vec3 normal{};
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
