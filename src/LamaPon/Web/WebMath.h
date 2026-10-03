#pragma once

#include <array>
#include <cmath>

namespace LamaPon::Web
{
    struct Vec2 final
    {
        // X成分
        float x{};
        // Y成分
        float y{};
    };

    struct Vec3 final
    {
        // X成分
        float x{};
        // Y成分
        float y{};
        // Z成分
        float z{};

        // 成分ごとの和を返す(other: 加えるベクトル)。
        constexpr Vec3 operator+(const Vec3& other) const noexcept
        {
            return { x + other.x, y + other.y, z + other.z };
        }

        // 成分ごとの差を返す(other: 引くベクトル)。
        constexpr Vec3 operator-(const Vec3& other) const noexcept
        {
            return { x - other.x, y - other.y, z - other.z };
        }

        // 全成分を倍率で乗算する(scalar: 倍率)。
        constexpr Vec3 operator*(float scalar) const noexcept
        {
            return { x * scalar, y * scalar, z * scalar };
        }

        // 他のベクトルを加算する(other: 加えるベクトル)。
        constexpr Vec3& operator+=(const Vec3& other) noexcept
        {
            x += other.x;
            y += other.y;
            z += other.z;
            return *this;
        }
    };

    // 内積を求める(left: 左ベクトル, right: 右ベクトル)。
    [[nodiscard]] inline float Dot(
        const Vec3& left,
        const Vec3& right) noexcept
    {
        return left.x * right.x + left.y * right.y + left.z * right.z;
    }

    // 外積を求める(left: 左ベクトル, right: 右ベクトル)。
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

    // ベクトルの長さの二乗を返す(value: 対象ベクトル)。
    [[nodiscard]] inline float LengthSquared(const Vec3& value) noexcept
    {
        return Dot(value, value);
    }

    // ベクトルの長さを返す(value: 対象ベクトル)。
    [[nodiscard]] inline float Length(const Vec3& value) noexcept
    {
        return std::sqrt(std::max(LengthSquared(value), 0.0f));
    }

    // 長さの二乗が1e-6以下なら零、それ以外は単位ベクトルを返す(value: 対象ベクトル)。
    [[nodiscard]] inline Vec3 Normalize(const Vec3& value) noexcept
    {
        // ベクトル長の二乗
        const float lengthSquared = LengthSquared(value);
        if (lengthSquared <= 0.000001f)
        {
            return {};
        }
        // ベクトル長の逆数
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        return value * inverseLength;
    }

    struct Mat4 final
    {
        // GLSLと同じ列優先の16成分
        std::array<float, 16> values{};

        // 単位行列を返す。
        [[nodiscard]] static constexpr Mat4 Identity() noexcept
        {
            // 生成する行列
            Mat4 result{};
            result.values[0] = 1.0f;
            result.values[5] = 1.0f;
            result.values[10] = 1.0f;
            result.values[15] = 1.0f;
            return result;
        }
    };

    // 左行列と右行列の積を求める(left: 左行列, right: 右行列)。
    [[nodiscard]] inline Mat4 Multiply(
        const Mat4& left,
        const Mat4& right) noexcept
    {
        // 生成する行列
        Mat4 result{};
        // 結果行列の列番号
        for (int column = 0; column < 4; ++column)
        {
            // 結果行列の行番号
            for (int row = 0; row < 4; ++row)
            {
                // 行列積の成分累積値
                float value = 0.0f;
                // 積和を取る成分番号
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

    // 平行移動行列を生成する(position: 移動量)。
    [[nodiscard]] inline Mat4 Translation(const Vec3& position) noexcept
    {
        // 生成する行列
        Mat4 result = Mat4::Identity();
        result.values[12] = position.x;
        result.values[13] = position.y;
        result.values[14] = position.z;
        return result;
    }

    // 拡縮行列を生成する(scale: 各軸の倍率)。
    [[nodiscard]] inline Mat4 Scale(const Vec3& scale) noexcept
    {
        // 生成する行列
        Mat4 result{};
        result.values[0] = scale.x;
        result.values[5] = scale.y;
        result.values[10] = scale.z;
        result.values[15] = 1.0f;
        return result;
    }

    // Y軸まわりの回転行列を生成する(radians: 回転角のラジアン)。
    [[nodiscard]] inline Mat4 RotationY(float radians) noexcept
    {
        // 生成する行列
        Mat4 result = Mat4::Identity();
        // 回転角の余弦
        const float cosine = std::cos(radians);
        // 回転角の正弦
        const float sine = std::sin(radians);
        result.values[0] = cosine;
        result.values[2] = -sine;
        result.values[8] = sine;
        result.values[10] = cosine;
        return result;
    }

    // X軸まわりの回転行列を生成する(radians: 回転角のラジアン)。
    [[nodiscard]] inline Mat4 RotationX(float radians) noexcept
    {
        // 生成する行列
        Mat4 result = Mat4::Identity();
        // 回転角の余弦
        const float cosine = std::cos(radians);
        // 回転角の正弦
        const float sine = std::sin(radians);
        result.values[5] = cosine;
        result.values[6] = sine;
        result.values[9] = -sine;
        result.values[10] = cosine;
        return result;
    }

    // Z軸まわりの回転行列を生成する(radians: 回転角のラジアン)。
    [[nodiscard]] inline Mat4 RotationZ(float radians) noexcept
    {
        // 生成する行列
        Mat4 result = Mat4::Identity();
        // 回転角の余弦
        const float cosine = std::cos(radians);
        // 回転角の正弦
        const float sine = std::sin(radians);
        result.values[0] = cosine;
        result.values[1] = sine;
        result.values[4] = -sine;
        result.values[5] = cosine;
        return result;
    }

    // OpenGLの深度範囲に対応する透視行列を生成する(verticalFieldOfView: 縦画角のラジアン, aspectRatio: 幅と高さの比, nearPlane: 正の近クリップ距離, farPlane: 近距離より大きい遠距離)。
    [[nodiscard]] inline Mat4 Perspective(
        float verticalFieldOfView,
        float aspectRatio,
        float nearPlane,
        float farPlane) noexcept
    {
        // 縦画角の半角の正接
        const float tangent = std::tan(verticalFieldOfView * 0.5f);
        // 透視投影のY軸倍率
        const float yScale = tangent > 0.000001f ? 1.0f / tangent : 1.0f;
        // 透視投影のX軸倍率
        const float xScale = yScale / (aspectRatio > 0.000001f ? aspectRatio : 1.0f);
        // 生成する行列
        Mat4 result{};
        result.values[0] = xScale;
        result.values[5] = yScale;
        result.values[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
        result.values[11] = -1.0f;
        result.values[14] = (2.0f * farPlane * nearPlane)
            / (nearPlane - farPlane);
        return result;
    }

    // eyeとtargetを一致させず、upDirectionは視線と平行にしない。
    // 右手系のビュー行列を生成する(eye: 視点位置, target: 注視位置, upDirection: 上方向)。
    [[nodiscard]] inline Mat4 LookAt(
        const Vec3& eye,
        const Vec3& target,
        const Vec3& upDirection) noexcept
    {
        // 視点から注視点への単位方向
        const Vec3 forward = Normalize(target - eye);
        // ビューの右方向
        const Vec3 side = Normalize(Cross(forward, upDirection));
        // ビューの上方向
        const Vec3 up = Cross(side, forward);

        // 生成する行列
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
