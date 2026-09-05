// ビルボード（カメラを向き続けるコンポーネント）の向きを検査します。
//
// 指定した軸が期待する方向を向くことを検査します。クォータニオンを
// 回転行列へ戻し、軸ベクトルと期待方向の内積が1になることを確認します。
//
// GPUは要りません。GraphicsDeviceも作らずにSceneを組み、Updateを
// 1回呼ぶだけです。

#include "LamaPon/Components/BillboardComponent.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Graphics/GraphicsDevice.h"

#include <objbase.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
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

    // 回転後のローカル軸（0=右, 1=上, 2=前）を取り出します。
    [[nodiscard]] DirectX::XMVECTOR WorldAxis(
        const LamaPon::GameObject& object,
        const int axis)
    {
        return DirectX::XMVector3Normalize(
            object.WorldMatrix().r[axis]);
    }

    [[nodiscard]] float Alignment(
        DirectX::FXMVECTOR left,
        DirectX::FXMVECTOR right)
    {
        return DirectX::XMVectorGetX(
            DirectX::XMVector3Dot(
                DirectX::XMVector3Normalize(left),
                DirectX::XMVector3Normalize(right)));
    }

    [[nodiscard]] DirectX::XMVECTOR Direction(
        const DirectX::XMFLOAT3& from,
        const DirectX::XMFLOAT3& to)
    {
        return DirectX::XMVector3Normalize(
            DirectX::XMVectorSubtract(
                DirectX::XMLoadFloat3(&to),
                DirectX::XMLoadFloat3(&from)));
    }
}

int main()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<void>(comResult);

    try
    {
        LamaPon::GraphicsDevice graphics;
        LamaPon::Scene scene(graphics);

        // カメラは斜め上、被写体は原点。
        const DirectX::XMFLOAT3 cameraPosition{
            6.0f, 4.0f, 8.0f };
        auto& cameraObject =
            scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position =
            cameraPosition;
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        auto& subject = scene.CreateGameObject("Billboard");
        const DirectX::XMFLOAT3 subjectPosition{
            0.0f, 0.0f, 0.0f };
        subject.GetTransform().position = subjectPosition;
        auto& billboard = subject.AddComponent<
            LamaPon::BillboardComponent>();

        const auto toCamera = Direction(
            subjectPosition,
            cameraPosition);

        // (1) カメラの位置を向く＋面は上（+Y）。Planeの画像を
        // カメラへ正対させる組み合わせです。
        billboard.SetMode(
            LamaPon::BillboardMode::FaceCameraPosition);
        scene.Update(0.016f);
        Require(
            Alignment(WorldAxis(subject, 1), toCamera)
                > 0.9999f,
            "FaceCameraPosition with the Up axis must point +Y at the camera.");

        // (2) 面を前（+Z）へ変えると、今度は前がカメラを向きます。
        billboard.SetFacingAxis(
            LamaPon::BillboardFacingAxis::Forward);
        scene.Update(0.016f);
        Require(
            Alignment(WorldAxis(subject, 2), toCamera)
                > 0.9999f,
            "FaceCameraPosition with the Forward axis must point +Z at the camera.");

        // (3) 画面と平行にする系は、カメラの座標ではなく「カメラが
        // 見ている向き」の逆を向きます。カメラは無回転なので前は
        // +Zで、板の面は-Zへ向くはずです。
        DirectX::XMFLOAT3 cameraForward{};
        DirectX::XMStoreFloat3(
            &cameraForward,
            DirectX::XMVector3Normalize(
                cameraObject.WorldMatrix().r[2]));
        billboard.SetMode(
            LamaPon::BillboardMode::ScreenAligned);
        scene.Update(0.016f);
        Require(
            Alignment(
                WorldAxis(subject, 2),
                DirectX::XMVectorNegate(
                    DirectX::XMLoadFloat3(
                        &cameraForward)))
                > 0.9999f,
            "ScreenAligned must face against the camera's view direction.");

        // (4) 2種類の違いを直接見ます。離れた場所へもう1枚置くと、
        //   ・画面と平行の系 … 位置に関係なく同じ向き
        //   ・位置を向く系 … 位置ごとに違う向き
        // になります。ここが2種類を分ける性質そのものです。
        auto& farSubject =
            scene.CreateGameObject("BillboardFar");
        farSubject.GetTransform().position =
            { -14.0f, 3.0f, 5.0f };
        auto& farBillboard =
            farSubject.AddComponent<
                LamaPon::BillboardComponent>(
                LamaPon::BillboardMode::ScreenAligned,
                LamaPon::BillboardFacingAxis::Forward);
        scene.Update(0.016f);
        Require(
            Alignment(
                WorldAxis(subject, 2),
                WorldAxis(farSubject, 2))
                > 0.9999f,
            "ScreenAligned billboards must all share one orientation.");

        farBillboard.SetMode(
            LamaPon::BillboardMode::FaceCameraPosition);
        billboard.SetMode(
            LamaPon::BillboardMode::FaceCameraPosition);
        scene.Update(0.016f);
        Require(
            Alignment(
                WorldAxis(subject, 2),
                WorldAxis(farSubject, 2))
                < 0.99f,
            "FaceCameraPosition billboards at different places must differ.");
        Require(
            Alignment(
                WorldAxis(farSubject, 2),
                Direction(
                    farSubject.GetTransform().position,
                    cameraPosition))
                > 0.9999f,
            "The far billboard must aim at the camera on its own.");
        static_cast<void>(
            scene.DestroyGameObject(farSubject));

        // (5) 立ったままのモードは上下に傾きません。カメラは上に4だけ
        // 高いので、傾く実装ならここで上を向いてしまいます。
        billboard.SetMode(
            LamaPon::BillboardMode
                ::UprightFaceCameraPosition);
        scene.Update(0.016f);
        const auto uprightForward = WorldAxis(subject, 2);
        Require(
            std::abs(
                DirectX::XMVectorGetY(uprightForward))
                < 1.0e-4f,
            "Upright modes must keep the facing axis horizontal.");
        const DirectX::XMFLOAT3 flatCamera{
            cameraPosition.x,
            subjectPosition.y,
            cameraPosition.z };
        Require(
            Alignment(
                uprightForward,
                Direction(subjectPosition, flatCamera))
                > 0.9999f,
            "UprightFaceCameraPosition must still turn toward the camera horizontally.");
        Require(
            DirectX::XMVectorGetY(WorldAxis(subject, 1))
                > 0.9999f,
            "Upright modes must keep the object standing upright.");

        // (6) 立ったまま画面の向きに合わせるモードも、水平のままで
        // あることを見ます（向く先はカメラの向きの逆の水平成分）。
        billboard.SetMode(
            LamaPon::BillboardMode::UprightScreenAligned);
        scene.Update(0.016f);
        Require(
            std::abs(
                DirectX::XMVectorGetY(
                    WorldAxis(subject, 2)))
                < 1.0e-4f,
            "UprightScreenAligned must keep the facing axis horizontal.");
        Require(
            DirectX::XMVectorGetY(WorldAxis(subject, 1))
                > 0.9999f,
            "UprightScreenAligned must keep the object standing upright.");

        // (7) 座標を指定するモードは、カメラではなくその点を向きます。
        const DirectX::XMFLOAT3 lookTarget{
            -5.0f, 0.0f, 0.0f };
        billboard.SetMode(
            LamaPon::BillboardMode::LookAtPosition);
        billboard.SetTargetPosition(lookTarget);
        scene.Update(0.016f);
        Require(
            Alignment(
                WorldAxis(subject, 2),
                Direction(subjectPosition, lookTarget))
                > 0.9999f,
            "LookAtPosition must aim at the given point, not the camera.");

        // (8) 親を回してもカメラを向いたまま。Transformが持つのは
        // ローカル回転なので、親の回転を打ち消していないとここで
        // 一緒に回ってしまいます。
        billboard.SetMode(
            LamaPon::BillboardMode::FaceCameraPosition);
        auto& parent = scene.CreateGameObject("Parent");
        parent.GetTransform().SetEulerAngles(
            { 0.3f, 1.1f, -0.4f });
        subject.SetParent(&parent);
        scene.Update(0.016f);
        // 親に回されて位置が変わるので、向く先も測り直します。
        DirectX::XMFLOAT3 movedPosition{};
        DirectX::XMStoreFloat3(
            &movedPosition,
            subject.WorldMatrix().r[3]);
        Require(
            Alignment(
                WorldAxis(subject, 2),
                Direction(movedPosition, cameraPosition))
                > 0.9999f,
            "A rotated parent must not drag the billboard off the camera.");

        // (9) メインカメラが無いときは向きを変えません（例外も
        // 投げません）。付けた向きのまま残ります。
        subject.SetParent(nullptr);
        billboard.SetFacingAxis(
            LamaPon::BillboardFacingAxis::Up);
        scene.Update(0.016f);
        const auto beforeClear =
            subject.GetTransform().rotationQuaternion;
        scene.ClearMainCamera();
        scene.Update(0.016f);
        const auto afterClear =
            subject.GetTransform().rotationQuaternion;
        Require(
            beforeClear.x == afterClear.x
                && beforeClear.y == afterClear.y
                && beforeClear.z == afterClear.z
                && beforeClear.w == afterClear.w,
            "Without a main camera the rotation must stay untouched.");

        if (g_failures == 0)
        {
            std::cout << "Billboard tests passed." << '\n';
        }
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "Billboard tests threw: "
            << error.what()
            << '\n';
        ++g_failures;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
