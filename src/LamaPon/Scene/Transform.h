#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>

namespace LamaPon
{
    // GameObjectの位置・回転・大きさ。
    //
    // 回転の正本はクォータニオン（rotationQuaternion）です。オイラー角は
    // EulerAngles() / SetEulerAngles() を
    // 通した「読み書きできる窓」として扱います。
    //
    // なぜクォータニオンを正本にするか:
    //   - ジンバルロックが無い。オイラー角ではピッチ±90度でヨーと
    //     ロールが縮退し、独立に回せなくなる
    //   - 補間が正しい。軸ごとに角度を補間すると、回転の軌跡が
    //     最短経路から外れて途中で振れる（wobble）
    //   - 回転の合成が素直。角度の足し算は順序依存で誤差が溜まる
    //
    // オイラー角で扱いたいときは EulerAngles() / SetEulerAngles() を
    // 使ってください（Inspectorの表示・編集はこちらです）。
    // 「Y軸まわりに少し回す」のような操作は Rotate() を使うと、
    // オイラー角を経由せずに合成できます。
    struct Transform final
    {
        DirectX::XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
        // 単位クォータニオン（x, y, z, w）。既定は無回転。
        DirectX::XMFLOAT4 rotationQuaternion{
            0.0f,
            0.0f,
            0.0f,
            1.0f
        };
        DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };

        [[nodiscard]] DirectX::XMVECTOR
            RotationVector() const noexcept
        {
            return DirectX::XMLoadFloat4(
                &rotationQuaternion);
        }

        void SetRotationVector(
            DirectX::FXMVECTOR value) noexcept
        {
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

        // オイラー角（ラジアン。x=ピッチ, y=ヨー, z=ロール）。
        // DirectXMathのRollPitchYaw規約と同じ並びです。
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

        void SetEulerAngles(
            const DirectX::XMFLOAT3& radians) noexcept
        {
            SetEulerAngles(
                radians.x,
                radians.y,
                radians.z);
        }

        // クォータニオンからオイラー角へ戻します。
        //
        // 同じ回転を表すオイラー角は複数あるため、往復して必ず同じ
        // 数値に戻るとは限りません（-180度と180度など）。表す回転は
        // 同じです。Inspectorでの編集は、この揺れを避けるために
        // 編集中の入力値を別に保持しています。
        [[nodiscard]] DirectX::XMFLOAT3
            EulerAngles() const noexcept
        {
            using namespace DirectX;

            XMFLOAT4X4 matrix{};
            XMStoreFloat4x4(
                &matrix,
                XMMatrixRotationQuaternion(
                    RotationVector()));

            // M = Rz(roll) * Rx(pitch) * Ry(yaw) を展開すると
            //   _32 = -sin(pitch)
            //   _12 =  sin(roll) * cos(pitch)
            //   _22 =  cos(roll) * cos(pitch)
            //   _31 =  cos(pitch) * sin(yaw)
            //   _33 =  cos(pitch) * cos(yaw)
            // になります（DirectXは行ベクトル規約）。
            const float sinPitch =
                std::clamp(-matrix._32, -1.0f, 1.0f);
            const float pitch = std::asin(sinPitch);
            const float cosPitch =
                std::sqrt(
                    std::max(
                        1.0f - sinPitch * sinPitch,
                        0.0f));

            // ピッチが±90度に近いとcos(pitch)が0へ落ち、ヨーと
            // ロールが縮退します（ジンバルロック）。その場合は
            // ロールを0に固定してヨーへ寄せます。
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

        // 任意軸まわりの回転を後から合成します（ローカル基準）。
        // オイラー角の成分を足す代わりにこちらを使ってください。
        void Rotate(
            const DirectX::XMFLOAT3& axis,
            const float radians) noexcept
        {
            using namespace DirectX;
            const XMVECTOR axisVector =
                XMLoadFloat3(&axis);
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

        // オイラー角ぶんの回転を合成します。既存の
        // 「rotation.y += delta」のような書き方の置き換えです。
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
