#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace LamaPon
{
    // 回転の正本は単位クォータニオンで、Euler角での読み書きは専用関数を通します。
    struct Transform final
    {
        // 親を基準にした局所位置
        DirectX::XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
        // 単位回転クォータニオン
        DirectX::XMFLOAT4 rotationQuaternion{
            0.0f,
            0.0f,
            0.0f,
            1.0f
        };
        // 三軸の局所拡縮倍率
        DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };

        // 保持する回転クォータニオンをSIMD値で返します。
        [[nodiscard]] DirectX::XMVECTOR
            RotationVector() const noexcept
        {
            return DirectX::XMLoadFloat4(
                &rotationQuaternion);
        }

        // 回転クォータニオンを正規化して設定します(value: 回転値)。
        // 長さが非有限または微小なら単位回転へ戻します。
        void SetRotationVector(
            DirectX::FXMVECTOR value) noexcept
        {
            // 回転値の長さの二乗
            const float lengthSquared =
                DirectX::XMVectorGetX(
                    DirectX::XMVector4LengthSq(value));
            if (!std::isfinite(lengthSquared)
                || lengthSquared <= 1.0e-12f)
            {
                rotationQuaternion = {
                    0.0f,
                    0.0f,
                    0.0f,
                    1.0f
                };
                return;
            }
            DirectX::XMStoreFloat4(
                &rotationQuaternion,
                DirectX::XMQuaternionNormalize(value));
        }

        // Euler角から回転を設定します(pitch: X軸ラジアン, yaw: Y軸ラジアン, roll: Z軸ラジアン)。
        // DirectXMathのRollPitchYaw規約に従います。
        void SetEulerAngles(
            const float pitch,
            const float yaw,
            const float roll) noexcept
        {
            SetRotationVector(
                DirectX::XMQuaternionRotationRollPitchYaw(
                    pitch,
                    yaw,
                    roll));
        }

        // 三軸のEuler角から回転を設定します(radians: ピッチ・ヨー・ロールのラジアン)。
        void SetEulerAngles(
            const DirectX::XMFLOAT3& radians) noexcept
        {
            SetEulerAngles(
                radians.x,
                radians.y,
                radians.z);
        }

        // 回転をピッチ・ヨー・ロールのラジアンへ変換します。
        // 同じ回転を表す角度は複数あるため、設定値と数値が一致するとは限りません。
        [[nodiscard]] DirectX::XMFLOAT3
            EulerAngles() const noexcept
        {
            using namespace DirectX;

            // Euler角へ分解する回転行列
            XMFLOAT4X4 matrix{};
            XMStoreFloat4x4(
                &matrix,
                XMMatrixRotationQuaternion(
                    RotationVector()));

            // 行ベクトルのRz×Rx×Ryからsin(pitch)を取り出します。
            // ピッチの制限済み正弦値
            const float sinPitch =
                std::clamp(-matrix._32, -1.0f, 1.0f);
            // 抽出したピッチのラジアン
            const float pitch = std::asin(sinPitch);
            // 特異姿勢を判定するピッチ余弦
            const float cosPitch =
                std::sqrt(
                    std::max(
                        1.0f - sinPitch * sinPitch,
                        0.0f));

            // ピッチが±90度付近ではヨーとロールが縮退するため、ロールを0へ固定します。
            if (cosPitch < 1.0e-4f)
            {
                return {
                    pitch,
                    std::atan2(-matrix._13, matrix._11),
                    0.0f
                };
            }
            return {
                pitch,
                std::atan2(matrix._31, matrix._33),
                std::atan2(matrix._12, matrix._22)
            };
        }

        // ローカル軸まわりの回転を合成します(axis: 回転軸, radians: 回転ラジアン)。
        // 非有限の角度や微小・非有限の軸では変更しません。
        void Rotate(
            const DirectX::XMFLOAT3& axis,
            const float radians) noexcept
        {
            using namespace DirectX;
            // SIMD値へ変換した回転軸
            const XMVECTOR axisVector =
                XMLoadFloat3(&axis);
            // 回転軸の長さの二乗
            const float axisLengthSquared =
                XMVectorGetX(
                    XMVector3LengthSq(axisVector));
            if (!std::isfinite(axisLengthSquared)
                || axisLengthSquared <= 1.0e-12f
                || !std::isfinite(radians))
            {
                return;
            }
            SetRotationVector(
                XMQuaternionMultiply(
                    RotationVector(),
                    XMQuaternionRotationAxis(
                        XMVector3Normalize(axisVector),
                        radians)));
        }

        // Euler角ぶんの回転をクォータニオンで合成します(radians: 三軸の回転ラジアン)。
        // 非有限の角度では変更しません。
        void RotateEuler(
            const DirectX::XMFLOAT3& radians) noexcept
        {
            using namespace DirectX;
            if (!std::isfinite(radians.x)
                || !std::isfinite(radians.y)
                || !std::isfinite(radians.z))
            {
                return;
            }
            SetRotationVector(
                XMQuaternionMultiply(
                    RotationVector(),
                    XMQuaternionRotationRollPitchYaw(
                        radians.x,
                        radians.y,
                        radians.z)));
        }

        // 拡縮・回転・移動の順で局所変換行列を求めます。
        [[nodiscard]] DirectX::XMMATRIX
            LocalMatrix() const noexcept
        {
            using namespace DirectX;

            return XMMatrixScaling(
                    scale.x,
                    scale.y,
                    scale.z)
                * XMMatrixRotationQuaternion(
                    RotationVector())
                * XMMatrixTranslation(
                    position.x,
                    position.y,
                    position.z);
        }
    };
}
