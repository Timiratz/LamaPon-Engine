#include "LamaPon/Components/BillboardComponent.h"

#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/Transform.h"

#include <cmath>

namespace
{
    // 方向を正規化し短すぎればfalseを返します(value: 有限の入力方向, normalized: 成功時の単位方向出力)。
    [[nodiscard]] bool TryNormalize(
        DirectX::FXMVECTOR value,
        DirectX::XMVECTOR& normalized) noexcept
    {
        // 入力方向の長さの二乗
        const float lengthSquared =
            DirectX::XMVectorGetX(
                DirectX::XMVector3LengthSq(value));
        if (lengthSquared < 1.0e-12f)
        {
            return false;
        }
        normalized = DirectX::XMVector3Normalize(value);
        return true;
    }

    // 平行に近い基準方向を別軸へ置き換えます(first: 保つ単位軸, reference: 基準の単位方向)。
    [[nodiscard]] DirectX::XMVECTOR PickReference(
        DirectX::FXMVECTOR first,
        DirectX::FXMVECTOR reference) noexcept
    {
        using namespace DirectX;
        // 両単位方向の内積絶対値
        const float alignment = std::abs(
            XMVectorGetX(XMVector3Dot(first, reference)));
        if (alignment < 0.999f)
        {
            return reference;
        }

        // 方向のY成分の絶対値
        const float towardUp = std::abs(
            XMVectorGetY(first));
        return towardUp < 0.9f
            ? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
            : XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    }
}

namespace LamaPon
{
    BillboardComponent::BillboardComponent(
        const BillboardMode mode,
        const BillboardFacingAxis facingAxis) noexcept
        : m_mode(mode)
        , m_facingAxis(facingAxis)
    {
    }

    bool BillboardComponent::UsesViewDirection()
        const noexcept
    {
        return m_mode == BillboardMode::ScreenAligned
            || m_mode
                == BillboardMode::UprightScreenAligned;
    }

    bool BillboardComponent::IsUpright() const noexcept
    {
        return m_mode
                == BillboardMode::UprightFaceCameraPosition
            || m_mode
                == BillboardMode::UprightScreenAligned;
    }

    bool BillboardComponent::ResolveCamera(
        CameraFrame& frame) const noexcept
    {
        using namespace DirectX;

        if (m_mode == BillboardMode::LookAtPosition)
        {

            return true;
        }

        // 有効性を確認する主カメラ
        auto* camera = Owner().GetScene().MainCamera();
        if (camera == nullptr
            || !camera->IsEnabled()
            || !camera->Owner().IsActiveInHierarchy())
        {
            return false;
        }
        // カメラのワールド変換
        const auto cameraWorld =
            camera->Owner().WorldMatrix();
        XMStoreFloat3(&frame.position, cameraWorld.r[3]);
        // 正規化するカメラの軸
        XMVECTOR axis{};
        if (TryNormalize(cameraWorld.r[2], axis))
        {
            XMStoreFloat3(&frame.forward, axis);
        }
        if (TryNormalize(cameraWorld.r[1], axis))
        {
            XMStoreFloat3(&frame.up, axis);
        }
        return true;
    }

    void BillboardComponent::OnLateUpdate(
        const float deltaTime)
    {
        static_cast<void>(deltaTime);

        using namespace DirectX;

        // 向きを決めるカメラ情報
        CameraFrame cameraFrame{};
        if (!ResolveCamera(cameraFrame))
        {
            return;
        }


        // 対象へ向けたい方向
        XMVECTOR desired{};
        if (UsesViewDirection())
        {
            // カメラのワールド+Z軸の反対を向けます。
            desired = XMVectorNegate(
                XMLoadFloat3(&cameraFrame.forward));
        }
        else
        {

            // 向く対象のワールド位置
            const auto& targetPosition =
                m_mode == BillboardMode::LookAtPosition
                    ? m_targetPosition
                    : cameraFrame.position;
            // 自身のワールド位置
            XMFLOAT3 selfPosition{};
            XMStoreFloat3(
                &selfPosition,
                Owner().WorldMatrix().r[3]);
            desired = XMVectorSubtract(
                XMLoadFloat3(&targetPosition),
                XMLoadFloat3(&selfPosition));
        }

        // 上下を起こしたままにするモードでは水平成分だけを使います。
        if (IsUpright())
        {
            desired = XMVectorSetY(desired, 0.0f);
        }

        // 対象へ向ける単位方向
        XMVECTOR facing{};
        if (!TryNormalize(desired, facing))
        {

            return;
        }


        // 姿勢の上方向の基準
        const XMVECTOR reference = IsUpright()
            ? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
            : XMLoadFloat3(&cameraFrame.up);


        // 姿勢のワールド+X単位軸
        XMVECTOR right{};
        // 姿勢のワールド+Y単位軸
        XMVECTOR up{};
        // 姿勢のワールド+Z単位軸
        XMVECTOR forward{};
        if (m_facingAxis == BillboardFacingAxis::Up)
        {
            up = facing;
            right = XMVector3Normalize(
                XMVector3Cross(
                    PickReference(up, reference),
                    up));
            // X×Y=Zの向きを保ち、クォータニオンが扱える回転基底を作ります。
            forward = XMVector3Cross(right, up);
        }
        else
        {
            forward = facing;
            right = XMVector3Normalize(
                XMVector3Cross(
                    PickReference(forward, reference),
                    forward));
            up = XMVector3Cross(forward, right);
        }

        // 行0・1・2にワールドのX・Y・Z軸を入れます。
        // 姿勢の正規直交基底行列
        XMMATRIX basis = XMMatrixIdentity();
        basis.r[0] = right;
        basis.r[1] = up;
        basis.r[2] = forward;
        // 適用する回転クォータニオン
        XMVECTOR rotation =
            XMQuaternionRotationMatrix(basis);

        // 保存するローカル回転から親の回転を打ち消します。
        // 回転を打ち消す親物体
        if (const auto* parent = Owner().Parent())
        {
            // 分解した親の拡大倍率
            XMVECTOR parentScale{};
            // 分解した親の回転
            XMVECTOR parentRotation{};
            // 分解した親の平行移動
            XMVECTOR parentTranslation{};
            if (XMMatrixDecompose(
                    &parentScale,
                    &parentRotation,
                    &parentTranslation,
                    parent->WorldMatrix()))
            {
                rotation = XMQuaternionMultiply(
                    rotation,
                    XMQuaternionInverse(parentRotation));
            }
        }

        GetTransform().SetRotationVector(rotation);
    }
}
