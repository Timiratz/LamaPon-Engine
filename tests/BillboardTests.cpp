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

// ビルボードがカメラ方向を向くことを検証します。
namespace
{
    // g_failures: assertion失敗の件数。
    int g_failures = 0;

    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を失敗一覧へ追加します。
    void Require(
        const bool condition,
        const std::string& message)
    {
        // assertion失敗を集計します。
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    // WorldAxis(object: 対象, axis: 0右・1上・2前): objectの回転後のaxisを返します。
    [[nodiscard]] DirectX::XMVECTOR WorldAxis(
        const LamaPon::GameObject& object,
        const int axis)
    {
        return DirectX::XMVector3Normalize(
            object.WorldMatrix().r[axis]);
    }

    // Alignment(left: 左方向, right: 右方向): 2方向の向きの内積を返します。
    [[nodiscard]] float Alignment(
        DirectX::FXMVECTOR left,
        DirectX::FXMVECTOR right)
    {
        return DirectX::XMVectorGetX(
            DirectX::XMVector3Dot(
                DirectX::XMVector3Normalize(left),
                DirectX::XMVector3Normalize(right)));
    }

    // Direction(from: 始点, to: 終点): 2点間の単位方向を返します。
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

// Billboardの各向き・親回転・camera不在時を検証します。
int main()
{
    // comResult: COM初期化結果。
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<void>(comResult);

    // Billboardの方向制御と例外を検査します。
    try
    {
    // graphics: 描画確認用device。
    LamaPon::GraphicsDevice graphics;
    // scene: billboardの更新対象。
    LamaPon::Scene scene(graphics);

        // cameraPosition: cameraのworld座標。
        const DirectX::XMFLOAT3 cameraPosition{
            6.0f, 4.0f, 8.0f };
        // cameraObject: cameraを持つscene object。
        auto& cameraObject =
            scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position =
            cameraPosition;
        // camera: sceneのmain camera component。
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        // subject: 検証対象のscene object。
        auto& subject = scene.CreateGameObject("Billboard");
        // subjectPosition: 検証対象のworld座標。
        const DirectX::XMFLOAT3 subjectPosition{
            0.0f, 0.0f, 0.0f };
        subject.GetTransform().position = subjectPosition;
        // billboard: 各modeを検証するcomponent。
        auto& billboard = subject.AddComponent<
            LamaPon::BillboardComponent>();

        // toCamera: subjectからcameraへの単位方向。
        const auto toCamera = Direction(
            subjectPosition,
            cameraPosition);

        // FaceCameraPositionのUp軸を検証します。
        billboard.SetMode(
            LamaPon::BillboardMode::FaceCameraPosition);
        scene.Update(0.016f);
        Require(
            Alignment(WorldAxis(subject, 1), toCamera)
                > 0.9999f,
            "FaceCameraPosition with the Up axis must point +Y at the camera.");

        // Forward軸をcameraへ向けるmodeを検証します。
        billboard.SetFacingAxis(
            LamaPon::BillboardFacingAxis::Forward);
        scene.Update(0.016f);
        Require(
            Alignment(WorldAxis(subject, 2), toCamera)
                > 0.9999f,
            "FaceCameraPosition with the Forward axis must point +Z at the camera.");

        // camera view方向の逆を向くScreenAlignedを検証します。
        // cameraForward: cameraのworld前方向。
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

        // ScreenAlignedとFaceCameraPositionの位置依存差を検証します。
        // farSubject: 位置差を比較するscene object。
        auto& farSubject =
            scene.CreateGameObject("BillboardFar");
        farSubject.GetTransform().position =
            { -14.0f, 3.0f, 5.0f };
        // farBillboard: 離れたobjectのcomponent。
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

        // Upright modeが水平を保ってcameraへ向くことを確認します。
        billboard.SetMode(
            LamaPon::BillboardMode
                ::UprightFaceCameraPosition);
        scene.Update(0.016f);
        // uprightForward: upright modeのworld前方向。
        const auto uprightForward = WorldAxis(subject, 2);
        Require(
            std::abs(
                DirectX::XMVectorGetY(uprightForward))
                < 1.0e-4f,
            "Upright modes must keep the facing axis horizontal.");
        // flatCamera: subjectの高さへ平面化したcamera位置。
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

        // UprightScreenAlignedが水平と上方向を保つことを確認します。
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

        // LookAtPositionが指定点を向くことを確認します。
        // lookTarget: billboardが向くworld座標。
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

        // 親の回転後もcamera方向を保つことを確認します。
        billboard.SetMode(
            LamaPon::BillboardMode::FaceCameraPosition);
        // parent: subjectへ回転を与えるobject。
        auto& parent = scene.CreateGameObject("Parent");
        parent.GetTransform().SetEulerAngles(
            { 0.3f, 1.1f, -0.4f });
        subject.SetParent(&parent);
        scene.Update(0.016f);
        // movedPosition: 親回転後のsubject world位置。
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

        // main camera削除後も現在の回転を保つことを確認します。
        subject.SetParent(nullptr);
        billboard.SetFacingAxis(
            LamaPon::BillboardFacingAxis::Up);
        scene.Update(0.016f);
        // beforeClear: camera削除直前のrotation。
        const auto beforeClear =
            subject.GetTransform().rotationQuaternion;
        scene.ClearMainCamera();
        scene.Update(0.016f);
        // afterClear: camera削除後のrotation。
        const auto afterClear =
            subject.GetTransform().rotationQuaternion;
        Require(
            beforeClear.x == afterClear.x
                && beforeClear.y == afterClear.y
                && beforeClear.z == afterClear.z
                && beforeClear.w == afterClear.w,
            "Without a main camera the rotation must stay untouched.");

        // assertion成功時だけ成功メッセージを表示します。
        if (g_failures == 0)
        {
            std::cout << "Billboard tests passed." << '\n';
        }
    }
    // テスト例外を標準エラーと失敗状態へ変換します。
    catch (const std::exception& error)
    {
        std::cerr
            << "Billboard tests threw: "
            << error.what()
            << '\n';
        ++g_failures;
    }

    // COM初期化成功時だけCOMを解放します。
    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
