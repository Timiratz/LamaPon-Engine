#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Transform.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

// クォータニオンを正本とするTransformの回転を検証します。
namespace
{
    // 失敗したTransform検証の件数
    int g_failures = 0;

    // Require(condition: 検証条件, message: 失敗理由)は不成立時に失敗数を増やす。
    void Require(
        const bool condition,
        const std::string& message)
    {
        // 条件違反を記録する
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    // NearlyEqual(left: 左値, right: 右値, tolerance: 許容誤差)は誤差内か判定する。
    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right,
        const float tolerance = 1.0e-4f) noexcept
    {
        return std::abs(left - right) <= tolerance;
    }

    // MatricesNearlyEqual(left: 左行列, right: 右行列, tolerance: 許容誤差)は誤差内の一致を判定する。
    [[nodiscard]] bool MatricesNearlyEqual(
        DirectX::FXMMATRIX left,
        DirectX::CXMMATRIX right,
        const float tolerance = 1.0e-4f)
    {
        // 左行列を格納するCPU行列
        DirectX::XMFLOAT4X4 a{};
        // 右行列を格納するCPU行列
        DirectX::XMFLOAT4X4 b{};
        DirectX::XMStoreFloat4x4(&a, left);
        DirectX::XMStoreFloat4x4(&b, right);
        // row: 行列の行番号
        for (int row = 0; row < 4; ++row)
        {
            // column: 行列の列番号
            for (int column = 0; column < 4; ++column)
            {
                // 対応要素が許容誤差内か確認する
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

// Transformの回転・階層・行列互換性を検証する
int main()
{
    using namespace DirectX;
    using LamaPon::Transform;

    // (1) 既定は無回転。
    {
        // 初期状態のTransform
        Transform transform;
        Require(
            MatricesNearlyEqual(
                transform.LocalMatrix(),
                XMMatrixIdentity()),
            "A default transform must be the identity.");
        // 初期回転のオイラー角
        const auto euler = transform.EulerAngles();
        Require(
            NearlyEqual(euler.x, 0.0f)
                && NearlyEqual(euler.y, 0.0f)
                && NearlyEqual(euler.z, 0.0f),
            "A default transform must report zero"
            " Euler angles.");
    }

    // SetEulerAnglesの結果とDirectXのRollPitchYaw行列を比較する。
    {
        // 比較対象の角度一覧
        const float angles[]{
            -3.0f, -1.5f, -0.7f, 0.0f,
            0.3f, 1.1f, 2.4f, 3.0f
        };
        // 比較済み角度組の件数
        int checked = 0;
        // pitch: ピッチ角候補
        for (const float pitch : angles)
        {
            // yaw: ヨー角候補
            for (const float yaw : angles)
            {
                // roll: ロール角候補
                for (const float roll : angles)
                {
                    // 角度を設定するTransform
                    Transform transform;
                    transform.SetEulerAngles(
                        pitch, yaw, roll);
                    // DirectXの期待回転行列
                    const auto expected =
                        XMMatrixRotationRollPitchYaw(
                            pitch, yaw, roll);
                    // 回転結果に差があれば失敗として記録する
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

    // オイラー角は非一意なため、往復結果は数値でなく回転行列で比較する。
    {
        // 往復変換を試す角度一覧
        const float angles[]{
            -2.5f, -1.0f, -0.2f, 0.0f, 0.4f, 1.3f, 2.9f
        };
        // pitch: ピッチ角候補
        for (const float pitch : angles)
        {
            // yaw: ヨー角候補
            for (const float yaw : angles)
            {
                // roll: ロール角候補
                for (const float roll : angles)
                {
                    // 変換前のTransform
                    Transform original;
                    original.SetEulerAngles(
                        pitch, yaw, roll);
                    // オイラー角を往復したTransform
                    Transform roundTripped;
                    roundTripped.SetEulerAngles(
                        original.EulerAngles());
                    // 回転往復で差があれば失敗として記録する
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

    // Quaternionがpitch=90度でもヨーとロールを独立回転させる。
    {
        // ピッチ90度の回転基準
        Transform base;
        base.SetEulerAngles(XM_PIDIV2, 0.0f, 0.0f);

        // 基準からヨー回転した姿勢
        Transform yawed = base;
        yawed.Rotate({ 0.0f, 1.0f, 0.0f }, 0.5f);
        // 基準からロール回転した姿勢
        Transform rolled = base;
        rolled.Rotate({ 0.0f, 0.0f, 1.0f }, 0.5f);

        // それぞれ異なる回転になることを確認する
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

        // ヨー回転が基準姿勢から変化したことを確認する
        Require(
            !MatricesNearlyEqual(
                XMMatrixRotationQuaternion(
                    yawed.RotationVector()),
                XMMatrixRotationQuaternion(
                    base.RotationVector()),
                1.0e-3f),
            "Rotate must change the orientation.");
    }

    // (5) Rotateの合成が正しいこと: 90度を2回で180度。
    {
        // 合成回転を試すTransform
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

    // (6) 正規化されていること（合成を繰り返しても崩れない）。
    {
        // 反復回転を試すTransform
        Transform transform;
        // step: 合成回転の反復番号
        for (int step = 0; step < 2000; ++step)
        {
            transform.Rotate(
                { 0.3f, 1.0f, 0.2f },
                0.05f);
        }
        // 合成後のクォータニオン長
        const float length = XMVectorGetX(
            XMVector4Length(
                transform.RotationVector()));
        Require(
            NearlyEqual(length, 1.0f, 1.0e-3f),
            "Repeated rotation must keep the quaternion"
            " normalised, got length "
                + std::to_string(length));
    }

    // (7) 位置と大きさが回転と独立に効くこと。
    {
        // 位置・拡大・回転を設定するTransform
        Transform transform;
        transform.position = { 1.0f, 2.0f, 3.0f };
        transform.scale = { 2.0f, 2.0f, 2.0f };
        transform.SetEulerAngles(0.0f, XM_PIDIV2, 0.0f);
        // 位置・拡大・回転から作る期待行列
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

    // (8) 無効なクォータニオンを受け取っても、NaNをシーンへ広げず
    // 単位回転へ戻します。
    {
        // 無効回転の補正を試すTransform
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

    // RotateWorldは親回転下でもワールド軸の回転差分を適用する。
    {
        // 親階層の回転
        LamaPon::GameObject parent(1, "Parent");
        // 親の子になる回転対象
        LamaPon::GameObject child(2, "Child");
        parent.GetTransform().SetEulerAngles(
            { 0.4f, 0.7f, -0.2f });
        child.GetTransform().SetEulerAngles(
            { -0.3f, 0.2f, 0.5f });
        child.SetParent(&parent);
        // RotateWorld適用前のワールド行列
        const auto before = child.WorldMatrix();
        child.RotateWorld({ 0.0f, 0.25f, 0.0f });
        // ワールドY軸回転後の期待行列
        const auto expected =
            before * XMMatrixRotationY(0.25f);
        Require(
            MatricesNearlyEqual(
                child.WorldMatrix(),
                expected,
                1.0e-3f),
            "RotateWorld must apply a world-axis quaternion delta.");
    }

    // 検証失敗があれば件数を出力する
    if (g_failures != 0)
    {
        std::cerr << g_failures
            << " transform assertion(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Transform tests passed.\n";
    return EXIT_SUCCESS;
}
