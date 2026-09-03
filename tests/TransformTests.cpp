// Transformの回転（クォータニオン正本）を検査します。
//
// 一番重要なのは「オイラー角→クォータニオン→行列」が、以前の
// 「オイラー角→行列」と同じ行列になること。ここがずれると、
// 既存シーンの見た目が全部変わります。
// あわせてジンバルロックが解消されたことも確認します。

#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Transform.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace
{
    int g_failures = 0;

    void Require(
        const bool condition,
        const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right,
        const float tolerance = 1.0e-4f) noexcept
    {
        return std::abs(left - right) <= tolerance;
    }

    // 2つの行列が（誤差の範囲で）同じか。
    [[nodiscard]] bool MatricesNearlyEqual(
        DirectX::FXMMATRIX left,
        DirectX::CXMMATRIX right,
        const float tolerance = 1.0e-4f)
    {
        DirectX::XMFLOAT4X4 a{};
        DirectX::XMFLOAT4X4 b{};
        DirectX::XMStoreFloat4x4(&a, left);
        DirectX::XMStoreFloat4x4(&b, right);
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                if (!NearlyEqual(
                        a.m[row][column],
                        b.m[row][column],
                        tolerance))
                {
                    return false;
                }
            }
        }
        return true;
    }
}

int main()
{
    using namespace DirectX;
    using LamaPon::Transform;

    // ① 既定は無回転。
    {
        Transform transform;
        Require(
            MatricesNearlyEqual(
                transform.LocalMatrix(),
                XMMatrixIdentity()),
            "A default transform must be the identity.");
        const auto euler = transform.EulerAngles();
        Require(
            NearlyEqual(euler.x, 0.0f)
                && NearlyEqual(euler.y, 0.0f)
                && NearlyEqual(euler.z, 0.0f),
            "A default transform must report zero"
            " Euler angles.");
    }

    // ② 行列の互換: SetEulerAnglesで作った回転行列が、以前の
    //    XMMatrixRotationRollPitchYawと一致すること。
    //    ここが崩れると既存シーンの見た目が変わります。
    {
        const float angles[]{
            -3.0f, -1.5f, -0.7f, 0.0f,
            0.3f, 1.1f, 2.4f, 3.0f
        };
        int checked = 0;
        for (const float pitch : angles)
        {
            for (const float yaw : angles)
            {
                for (const float roll : angles)
                {
                    Transform transform;
                    transform.SetEulerAngles(
                        pitch, yaw, roll);
                    const auto expected =
                        XMMatrixRotationRollPitchYaw(
                            pitch, yaw, roll);
                    if (!MatricesNearlyEqual(
                            XMMatrixRotationQuaternion(
                                transform
                                    .RotationVector()),
                            expected))
                    {
                        Require(
                            false,
                            "Quaternion rotation differs"
                            " from RollPitchYaw at "
                                + std::to_string(pitch)
                                + ","
                                + std::to_string(yaw)
                                + ","
                                + std::to_string(roll));
                    }
                    ++checked;
                }
            }
        }
        std::cout
            << "matrix compatibility checked: "
            << checked << " angle triples\n";
    }

    // ③ オイラー角の往復: 同じ「回転」に戻ること。数値が同じに
    //    戻るとは限らないので（-180度と180度など）、行列で比べます。
    {
        const float angles[]{
            -2.5f, -1.0f, -0.2f, 0.0f, 0.4f, 1.3f, 2.9f
        };
        for (const float pitch : angles)
        {
            for (const float yaw : angles)
            {
                for (const float roll : angles)
                {
                    Transform original;
                    original.SetEulerAngles(
                        pitch, yaw, roll);
                    Transform roundTripped;
                    roundTripped.SetEulerAngles(
                        original.EulerAngles());
                    if (!MatricesNearlyEqual(
                            XMMatrixRotationQuaternion(
                                original
                                    .RotationVector()),
                            XMMatrixRotationQuaternion(
                                roundTripped
                                    .RotationVector()),
                            1.0e-3f))
                    {
                        Require(
                            false,
                            "Euler round trip changed the"
                            " rotation at "
                                + std::to_string(pitch)
                                + ","
                                + std::to_string(yaw)
                                + ","
                                + std::to_string(roll));
                    }
                }
            }
        }
    }

    // ④ ジンバルロックが解消されていること。
    //    オイラー角ではピッチ90度でヨーとロールが縮退し、別々に
    //    回しても同じ姿勢になってしまいます。クォータニオンなら
    //    Rotateで独立に回せます。
    {
        Transform base;
        base.SetEulerAngles(XM_PIDIV2, 0.0f, 0.0f);

        Transform yawed = base;
        yawed.Rotate({ 0.0f, 1.0f, 0.0f }, 0.5f);
        Transform rolled = base;
        rolled.Rotate({ 0.0f, 0.0f, 1.0f }, 0.5f);

        Require(
            !MatricesNearlyEqual(
                XMMatrixRotationQuaternion(
                    yawed.RotationVector()),
                XMMatrixRotationQuaternion(
                    rolled.RotationVector()),
                1.0e-3f),
            "At 90 degrees pitch, yawing and rolling must"
            " still produce different orientations"
            " (no gimbal lock).");

        // どちらもベースからは変化していること。
        Require(
            !MatricesNearlyEqual(
                XMMatrixRotationQuaternion(
                    yawed.RotationVector()),
                XMMatrixRotationQuaternion(
                    base.RotationVector()),
                1.0e-3f),
            "Rotate must change the orientation.");
    }

    // ⑤ Rotateの合成が正しいこと: 90度を2回で180度。
    {
        Transform transform;
        transform.Rotate({ 0.0f, 1.0f, 0.0f }, XM_PIDIV2);
        transform.Rotate({ 0.0f, 1.0f, 0.0f }, XM_PIDIV2);
        Require(
            MatricesNearlyEqual(
                XMMatrixRotationQuaternion(
                    transform.RotationVector()),
                XMMatrixRotationY(XM_PI),
                1.0e-3f),
            "Two 90 degree yaws must equal one 180 degree"
            " yaw.");
    }

    // ⑥ 正規化されていること（合成を繰り返しても崩れない）。
    {
        Transform transform;
        for (int step = 0; step < 2000; ++step)
        {
            transform.Rotate(
                { 0.3f, 1.0f, 0.2f },
                0.05f);
        }
        const float length = XMVectorGetX(
            XMVector4Length(
                transform.RotationVector()));
        Require(
            NearlyEqual(length, 1.0f, 1.0e-3f),
            "Repeated rotation must keep the quaternion"
            " normalised, got length "
                + std::to_string(length));
    }

    // ⑦ 位置と大きさが回転と独立に効くこと。
    {
        Transform transform;
        transform.position = { 1.0f, 2.0f, 3.0f };
        transform.scale = { 2.0f, 2.0f, 2.0f };
        transform.SetEulerAngles(0.0f, XM_PIDIV2, 0.0f);
        const auto expected =
            XMMatrixScaling(2.0f, 2.0f, 2.0f)
            * XMMatrixRotationY(XM_PIDIV2)
            * XMMatrixTranslation(1.0f, 2.0f, 3.0f);
        Require(
            MatricesNearlyEqual(
                transform.LocalMatrix(),
                expected,
                1.0e-3f),
            "LocalMatrix must be scale * rotation *"
            " translation.");
    }

    // ⑧ 無効なクォータニオンを受け取っても、NaNをシーンへ広げず
    //    単位回転へ戻します。
    {
        Transform transform;
        transform.SetRotationVector(XMVectorZero());
        Require(
            MatricesNearlyEqual(
                transform.LocalMatrix(),
                XMMatrixIdentity()),
            "A zero quaternion must fall back to identity.");
        transform.SetRotationVector(XMVectorSet(
            std::numeric_limits<float>::quiet_NaN(),
            0.0f,
            0.0f,
            1.0f));
        Require(
            MatricesNearlyEqual(
                transform.LocalMatrix(),
                XMMatrixIdentity()),
            "A non-finite quaternion must fall back to identity.");
    }

    // ⑨ 親が回転していてもRotateWorldはEuler成分を近似せず、
    //    ワールド軸まわりの回転として適用します。
    {
        LamaPon::GameObject parent(1, "Parent");
        LamaPon::GameObject child(2, "Child");
        parent.GetTransform().SetEulerAngles(
            { 0.4f, 0.7f, -0.2f });
        child.GetTransform().SetEulerAngles(
            { -0.3f, 0.2f, 0.5f });
        child.SetParent(&parent);
        const auto before = child.WorldMatrix();
        child.RotateWorld({ 0.0f, 0.25f, 0.0f });
        const auto expected =
            before * XMMatrixRotationY(0.25f);
        Require(
            MatricesNearlyEqual(
                child.WorldMatrix(),
                expected,
                1.0e-3f),
            "RotateWorld must apply a world-axis quaternion delta.");
    }

    if (g_failures != 0)
    {
        std::cerr << g_failures
            << " transform assertion(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Transform tests passed.\n";
    return EXIT_SUCCESS;
}
