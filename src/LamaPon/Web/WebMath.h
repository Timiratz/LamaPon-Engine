#pragma once

#include <array>
#include <cmath>

namespace LamaPon::Web
{
    struct Vec2 final
    {
        float x{};
        float y{};
    };

    struct Vec3 final
    {
        float x{};
        float y{};
        float z{};

        constexpr Vec3 operator+(const Vec3& other) const noexcept
        {
            return { x + other.x, y + other.y, z + other.z };
        }

        constexpr Vec3 operator-(const Vec3& other) const noexcept
        {
            return { x - other.x, y - other.y, z - other.z };
        }

        constexpr Vec3 operator*(float scalar) const noexcept
        {
            return { x * scalar, y * scalar, z * scalar };
        }

        constexpr Vec3& operator+=(const Vec3& other) noexcept
        {
            x += other.x;
            y += other.y;
            z += other.z;
            return *this;
        }
    };

    [[nodiscard]] inline float Dot(
        const Vec3& left,
        const Vec3& right) noexcept
    {
        return left.x * right.x + left.y * right.y + left.z * right.z;
    }

    [[nodiscard]] inline Vec3 Cross(
        const Vec3& left,
        const Vec3& right) noexcept
    {
        return {
            left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x,
        };
    }

    [[nodiscard]] inline float LengthSquared(const Vec3& value) noexcept
    {
        return Dot(value, value);
    }

    [[nodiscard]] inline float Length(const Vec3& value) noexcept
    {
        return std::sqrt(std::max(LengthSquared(value), 0.0f));
    }

    [[nodiscard]] inline Vec3 Normalize(const Vec3& value) noexcept
    {
        const float lengthSquared = LengthSquared(value);
        if (lengthSquared <= 0.000001f)
        {
            return {};
        }
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        return value * inverseLength;
    }

    struct Mat4 final
    {
        // GLSLのmat4 Uniformと同じColumn-major形式で保持します。
        std::array<float, 16> values{};

        [[nodiscard]] static constexpr Mat4 Identity() noexcept
        {
            Mat4 result{};
            result.values[0] = 1.0f;
            result.values[5] = 1.0f;
            result.values[10] = 1.0f;
            result.values[15] = 1.0f;
            return result;
        }
    };

    [[nodiscard]] inline Mat4 Multiply(
        const Mat4& left,
        const Mat4& right) noexcept
    {
        Mat4 result{};
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
            {
                float value = 0.0f;
                for (int index = 0; index < 4; ++index)
                {
                    value += left.values[index * 4 + row]
                        * right.values[column * 4 + index];
                }
                result.values[column * 4 + row] = value;
            }
        }
        return result;
    }

    [[nodiscard]] inline Mat4 Translation(const Vec3& position) noexcept
    {
        Mat4 result = Mat4::Identity();
        result.values[12] = position.x;
        result.values[13] = position.y;
        result.values[14] = position.z;
        return result;
    }

    [[nodiscard]] inline Mat4 Scale(const Vec3& scale) noexcept
    {
        Mat4 result{};
        result.values[0] = scale.x;
        result.values[5] = scale.y;
        result.values[10] = scale.z;
        result.values[15] = 1.0f;
        return result;
    }

    [[nodiscard]] inline Mat4 RotationY(float radians) noexcept
    {
        Mat4 result = Mat4::Identity();
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        result.values[0] = cosine;
        result.values[2] = -sine;
        result.values[8] = sine;
        result.values[10] = cosine;
        return result;
    }

    [[nodiscard]] inline Mat4 RotationX(float radians) noexcept
    {
        Mat4 result = Mat4::Identity();
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        result.values[5] = cosine;
        result.values[6] = sine;
        result.values[9] = -sine;
        result.values[10] = cosine;
        return result;
    }

    [[nodiscard]] inline Mat4 RotationZ(float radians) noexcept
    {
        Mat4 result = Mat4::Identity();
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        result.values[0] = cosine;
        result.values[1] = sine;
        result.values[4] = -sine;
        result.values[5] = cosine;
        return result;
    }

    [[nodiscard]] inline Mat4 Perspective(
        float verticalFieldOfView,
        float aspectRatio,
        float nearPlane,
        float farPlane) noexcept
    {
        const float tangent = std::tan(verticalFieldOfView * 0.5f);
        const float yScale = tangent > 0.000001f ? 1.0f / tangent : 1.0f;
        const float xScale = yScale / (aspectRatio > 0.000001f ? aspectRatio : 1.0f);
        Mat4 result{};
        result.values[0] = xScale;
        result.values[5] = yScale;
        result.values[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
        result.values[11] = -1.0f;
        result.values[14] = (2.0f * farPlane * nearPlane)
            / (nearPlane - farPlane);
        return result;
    }

    [[nodiscard]] inline Mat4 LookAt(
        const Vec3& eye,
        const Vec3& target,
        const Vec3& upDirection) noexcept
    {
        const Vec3 forward = Normalize(target - eye);
        const Vec3 side = Normalize(Cross(forward, upDirection));
        const Vec3 up = Cross(side, forward);

        Mat4 result = Mat4::Identity();
        result.values[0] = side.x;
        result.values[4] = side.y;
        result.values[8] = side.z;
        result.values[12] = -Dot(side, eye);
        result.values[1] = up.x;
        result.values[5] = up.y;
        result.values[9] = up.z;
        result.values[13] = -Dot(up, eye);
        result.values[2] = -forward.x;
        result.values[6] = -forward.y;
        result.values[10] = -forward.z;
        result.values[14] = Dot(forward, eye);
        return result;
    }
}
