#include "LamaPon/Components/BillboardComponent.h"

#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/Transform.h"

#include <cmath>

namespace
{
    // 長さが取れないベクトルは向きとして使えません。
    [[nodiscard]] bool TryNormalize(
        DirectX::FXMVECTOR value,
        DirectX::XMVECTOR& normalized) noexcept
    {
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

    // 直交していない2本から、3本目を作って正規直交系へ整えます。
    // firstは必ず保たれ、referenceは向きの基準にだけ使います。
    // referenceがfirstと平行だと外積が0になるので、そのときは
    // 別の軸へ逃がします。
    [[nodiscard]] DirectX::XMVECTOR PickReference(
        DirectX::FXMVECTOR first,
        DirectX::FXMVECTOR reference) noexcept
    {
        using namespace DirectX;
        const float alignment = std::abs(
            XMVectorGetX(XMVector3Dot(first, reference)));
        if (alignment < 0.999f)
        {
            return reference;
        }
        // 平行なので、firstと平行になりにくい軸を選び直します。
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
            // 座標を向くモードはカメラを使いません。基準の上方向は
            // ワールドの上のままにします。
            return true;
        }

        auto* camera = Owner().GetScene().MainCamera();
        if (camera == nullptr
            || !camera->IsEnabled()
            || !camera->Owner().IsActiveInHierarchy())
        {
            return false;
        }
        const auto cameraWorld =
            camera->Owner().WorldMatrix();
        XMStoreFloat3(&frame.position, cameraWorld.r[3]);
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

        CameraFrame cameraFrame{};
        if (!ResolveCamera(cameraFrame))
        {
            return;
        }

        // 向ける方向を決めます。
        XMVECTOR desired{};
        if (UsesViewDirection())
        {
            // 画面と平行にする系。カメラが見ている向きの逆へ面を
            // 向けます（カメラへ正面を見せるため）。板の位置に
            // よらず同じ向きになるので、画面の端でも歪みません。
            desired = XMVectorNegate(
                XMLoadFloat3(&cameraFrame.forward));
        }
        else
        {
            // 位置を向く系。自分から向く先へのベクトルです。
            const auto& targetPosition =
                m_mode == BillboardMode::LookAtPosition
                    ? m_targetPosition
                    : cameraFrame.position;
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

        XMVECTOR facing{};
        if (!TryNormalize(desired, facing))
        {
            // 真上／真下から見られている、あるいは向く先と重なって
            // いる状態です。前の向きを保ちます。
            return;
        }

        // 基準の上方向。上下を起こすモードはワールドの上を使います
        // （カメラが傾いていても立ったままにするため）。
        const XMVECTOR reference = IsUpright()
            ? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
            : XMLoadFloat3(&cameraFrame.up);

        // 指定した軸をfacingへ向ける正規直交系を作ります。
        XMVECTOR right{};
        XMVECTOR up{};
        XMVECTOR forward{};
        if (m_facingAxis == BillboardFacingAxis::Up)
        {
            up = facing;
            right = XMVector3Normalize(
                XMVector3Cross(
                    PickReference(up, reference),
                    up));
            // 3本目は必ず right × up で作ります。up × right にすると
            // 行列式が-1（左手系）になり、XMQuaternionRotationMatrixが
            // 回転として解釈できず、向きが壊れます。DirectXの規約は
            // X × Y = Z です。
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

        // 行ベクトル規約なので、行0=右, 行1=上, 行2=前 です。
        XMMATRIX basis = XMMatrixIdentity();
        basis.r[0] = right;
        basis.r[1] = up;
        basis.r[2] = forward;
        XMVECTOR rotation =
            XMQuaternionRotationMatrix(basis);

        // Transformが持つのはローカル回転なので、親の回転を打ち消し
        // ます。これをしないと、親を回した瞬間に子のビルボードが
        // 一緒に回ってカメラから外れます。
        if (const auto* parent = Owner().Parent())
        {
            XMVECTOR parentScale{};
            XMVECTOR parentRotation{};
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
