#include "LamaPon/Components/Sway2DComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <objbase.h>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>

// 2D揺れ物の向き・収束・上限・親子順序・保存を検証します。
namespace
{
    // assertion失敗の件数
    int g_failures = 0;
    // 1フレームの秒数
    constexpr float FrameSeconds = 1.0f / 60.0f;

    // 条件不成立を失敗一覧へ追加します(condition: 成立条件, message: 失敗理由)。
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

    // 差が許容内か返します(left: 比較元, right: 比較先, tolerance: 許容差)。
    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right,
        const float tolerance = 1.0e-4f)
    {
        return std::abs(left - right) <= tolerance;
    }

    // 度をラジアンへ変換します(degrees: 角度の度)。
    [[nodiscard]] float ToRadians(const float degrees)
    {
        return degrees * (std::numbers::pi_v<float> / 180.0f);
    }

    // Z軸だけの回転クォータニオンから角度ラジアンを返します(object: 対象)。
    [[nodiscard]] float ZAngle(const LamaPon::GameObject& object)
    {
        // 対象のローカル回転
        const auto& rotation =
            object.GetTransform().rotationQuaternion;
        return 2.0f * std::atan2(rotation.z, rotation.w);
    }

    // 一定間隔でシーンを更新します(scene: 更新するシーン, count: フレーム数)。
    void RunFrames(
        LamaPon::Scene& scene,
        const int count)
    {
        // 更新したフレーム数
        for (int frame = 0; frame < count; ++frame)
        {
            scene.Update(FrameSeconds);
        }
    }

    // ローカルXYのワールドXYを返します(object: 対象, local: ローカルXY)。
    [[nodiscard]] DirectX::XMFLOAT2 WorldPoint(
        const LamaPon::GameObject& object,
        const DirectX::XMFLOAT2& local)
    {
        // 変換後のワールド座標
        DirectX::XMFLOAT3 world{};
        DirectX::XMStoreFloat3(
            &world,
            DirectX::XMVector3TransformCoord(
                DirectX::XMVectorSet(local.x, local.y, 0.0f, 1.0f),
                object.WorldMatrix()));
        return { world.x, world.y };
    }

    // 静止・移動への遅れ・収束・上限角度・瞬間移動を検証します(graphics: シーン用の描画装置)。
    void TestRestLagAndSettle(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 動かす親
        auto& body = scene.CreateGameObject("Body");
        // 下へ垂れる揺れパーツ
        auto& hair = scene.CreateGameObject("Hair");
        hair.SetParent(&body);
        // 上限を広げた揺れ設定
        LamaPon::Sway2DSettings settings{};
        settings.maxAngleDegrees = 60.0f;
        // 検証する揺れ物
        auto& sway =
            hair.AddComponent<LamaPon::Sway2DComponent>(settings);

        RunFrames(scene, 120);
        Require(
            NearlyEqual(sway.CurrentAngle(), 0.0f, 1.0e-5f)
                && NearlyEqual(ZAngle(hair), 0.0f, 1.0e-5f),
            "Sway2D moved without any motion or gravity.");

        // 右へ動くと、下に垂れた先端は左へ取り残され、正の角度になります。
        // 移動したフレーム数
        for (int frame = 0; frame < 10; ++frame)
        {
            body.GetTransform().position.x += 20.0f;
            scene.Update(FrameSeconds);
        }
        Require(
            sway.CurrentAngle() > ToRadians(1.0f),
            "Sway2D tip did not lag behind a rightward move.");
        Require(
            NearlyEqual(ZAngle(hair), sway.CurrentAngle(), 1.0e-4f),
            "Sway2D did not write its angle onto the rotation.");
        // 揺れパーツの回転中心のワールドXY
        const auto pivot = WorldPoint(hair, { 0.0f, 0.0f });
        // 揺れパーツの先端のワールドXY
        const auto tip = WorldPoint(hair, settings.tipOffset);
        Require(
            tip.x < pivot.x,
            "Sway2D tip is not on the trailing side.");

        RunFrames(scene, 600);
        Require(
            NearlyEqual(sway.CurrentAngle(), 0.0f, ToRadians(0.5f)),
            "Sway2D did not settle back to rest.");

        // 長さの10倍未満の大きな移動では上限角度で止まります。
        // 移動したフレーム数
        for (int frame = 0; frame < 5; ++frame)
        {
            body.GetTransform().position.x -= 400.0f;
            scene.Update(FrameSeconds);
            Require(
                std::abs(sway.CurrentAngle())
                    <= ToRadians(settings.maxAngleDegrees) + 1.0e-4f,
                "Sway2D exceeded its maximum angle.");
        }
        Require(
            sway.CurrentAngle() < -ToRadians(10.0f),
            "Sway2D did not swing the other way on a leftward move.");

        // 長さの10倍を超える移動は瞬間移動とみなし、揺れを作り直します。
        RunFrames(scene, 600);
        body.GetTransform().position.x += 5000.0f;
        scene.Update(FrameSeconds);
        Require(
            NearlyEqual(sway.CurrentAngle(), 0.0f, ToRadians(0.5f)),
            "Sway2D swung after a teleport.");
    }

    // 重力・一時停止・無効化を検証します(graphics: シーン用の描画装置)。
    void TestGravityPauseAndDisable(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 右へ伸びる揺れパーツ
        auto& ribbon = scene.CreateGameObject("Ribbon");
        // 重力で垂れる揺れ設定
        LamaPon::Sway2DSettings settings{};
        settings.tipOffset = { 100.0f, 0.0f };
        settings.gravity = { 0.0f, 2000.0f };
        settings.stiffness = 20.0f;
        settings.maxAngleDegrees = 80.0f;
        // 検証する揺れ物
        auto& sway =
            ribbon.AddComponent<LamaPon::Sway2DComponent>(settings);

        RunFrames(scene, 600);
        // 右向きの先端が下(+Y)へ垂れた落ち着き角度
        const float settled = sway.CurrentAngle();
        Require(
            settled > ToRadians(5.0f)
                && settled < ToRadians(80.0f),
            "Sway2D gravity did not pull the tip down.");
        RunFrames(scene, 60);
        Require(
            NearlyEqual(sway.CurrentAngle(), settled, 1.0e-3f),
            "Sway2D gravity did not reach a steady angle.");

        // 時間が止まっている間は揺れを保ち、静止姿勢へ戻しません。
        // 停止中に更新したフレーム数
        for (int frame = 0; frame < 3; ++frame)
        {
            scene.Update(0.0f);
        }
        Require(
            NearlyEqual(sway.CurrentAngle(), settled, 1.0e-4f)
                && NearlyEqual(ZAngle(ribbon), settled, 1.0e-4f),
            "Sway2D lost its pose while time was paused.");

        sway.SetEnabled(false);
        Require(
            NearlyEqual(ZAngle(ribbon), 0.0f, 1.0e-6f),
            "Disabling Sway2D did not restore the rest rotation.");
        scene.Update(FrameSeconds);
        Require(
            NearlyEqual(ZAngle(ribbon), 0.0f, 1.0e-6f),
            "Disabled Sway2D kept rotating.");
    }

    // 毎フレーム回転を書く処理と併用しても回転が積み重ならないことを検証します(graphics: シーン用の描画装置)。
    void TestExternalRotationWriter(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // アニメーションで回される揺れパーツ
        auto& ribbon = scene.CreateGameObject("Ribbon");
        // 重力で垂れる揺れ設定
        LamaPon::Sway2DSettings settings{};
        settings.tipOffset = { 100.0f, 0.0f };
        settings.gravity = { 0.0f, 2000.0f };
        settings.stiffness = 20.0f;
        settings.maxAngleDegrees = 80.0f;
        // 検証する揺れ物
        auto& sway =
            ribbon.AddComponent<LamaPon::Sway2DComponent>(settings);

        // アニメーションのように毎フレーム書き込む角度ラジアン
        const float animated = ToRadians(30.0f);
        // 更新したフレーム数
        for (int frame = 0; frame < 600; ++frame)
        {
            ribbon.GetTransform().SetEulerAngles(0.0f, 0.0f, animated);
            scene.Update(FrameSeconds);
        }
        // アニメーションに上乗せされた落ち着き角度
        const float settled = sway.CurrentAngle();
        Require(
            settled > ToRadians(1.0f),
            "Sway2D did not sway on top of an animated rotation.");
        Require(
            NearlyEqual(ZAngle(ribbon), animated + settled, 1.0e-4f),
            "Sway2D did not add its angle to the animated rotation.");
        // 追加で更新したフレーム数
        for (int frame = 0; frame < 120; ++frame)
        {
            ribbon.GetTransform().SetEulerAngles(0.0f, 0.0f, animated);
            scene.Update(FrameSeconds);
        }
        Require(
            NearlyEqual(ZAngle(ribbon), animated + settled, 1.0e-3f),
            "Sway2D accumulated rotation with an animated writer.");
    }

    // 2節の揺れを動かして下の節の角度を返します(graphics: シーン用の描画装置, childFirst: 子を先に生成するか)。
    [[nodiscard]] float RunChain(
        LamaPon::GraphicsDevice& graphics,
        const bool childFirst)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 動かす根元
        LamaPon::GameObject* root{};
        // 上の節
        LamaPon::GameObject* upper{};
        // 下の節
        LamaPon::GameObject* lower{};
        if (childFirst)
        {
            lower = &scene.CreateGameObject("Lower");
            upper = &scene.CreateGameObject("Upper");
            root = &scene.CreateGameObject("Root");
        }
        else
        {
            root = &scene.CreateGameObject("Root");
            upper = &scene.CreateGameObject("Upper");
            lower = &scene.CreateGameObject("Lower");
        }
        upper->SetParent(root);
        lower->SetParent(upper);
        lower->GetTransform().position = { 0.0f, 50.0f, 0.0f };
        // 各節の揺れ設定
        LamaPon::Sway2DSettings settings{};
        settings.tipOffset = { 0.0f, 50.0f };
        settings.maxAngleDegrees = 90.0f;
        upper->AddComponent<LamaPon::Sway2DComponent>(settings);
        // 下の節の揺れ物
        auto& lowerSway =
            lower->AddComponent<LamaPon::Sway2DComponent>(settings);

        RunFrames(scene, 5);
        // 移動したフレーム数
        for (int frame = 0; frame < 12; ++frame)
        {
            root->GetTransform().position.x += 15.0f;
            scene.Update(FrameSeconds);
        }
        RunFrames(scene, 7);
        return lowerSway.CurrentAngle();
    }

    // 親子でつないだ揺れが生成順に依存しないことを検証します(graphics: シーン用の描画装置)。
    void TestChainOrder(LamaPon::GraphicsDevice& graphics)
    {
        // 親から生成した場合の下の節の角度
        const float parentFirst = RunChain(graphics, false);
        // 子から生成した場合の下の節の角度
        const float childFirst = RunChain(graphics, true);
        Require(
            std::abs(parentFirst) > ToRadians(0.5f),
            "Chained Sway2D did not move.");
        Require(
            NearlyEqual(parentFirst, childFirst, 1.0e-5f),
            "Chained Sway2D depended on object creation order.");
    }

    // 親を左右反転しても先端が移動の後ろ側へ残ることを検証します(graphics: シーン用の描画装置)。
    void TestMirroredParent(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 左右反転した親
        auto& body = scene.CreateGameObject("Body");
        body.GetTransform().scale = { -1.0f, 1.0f, 1.0f };
        // 斜め下へ垂れる揺れパーツ
        auto& hair = scene.CreateGameObject("Hair");
        hair.SetParent(&body);
        // 斜めの先端を持つ揺れ設定
        LamaPon::Sway2DSettings settings{};
        settings.tipOffset = { 30.0f, 100.0f };
        // 検証する揺れ物
        auto& sway =
            hair.AddComponent<LamaPon::Sway2DComponent>(settings);

        RunFrames(scene, 2);
        // 静止時の回転中心から先端までのワールドX
        const float restTipX =
            WorldPoint(hair, settings.tipOffset).x
            - WorldPoint(hair, { 0.0f, 0.0f }).x;
        // 移動したフレーム数
        for (int frame = 0; frame < 10; ++frame)
        {
            body.GetTransform().position.x += 20.0f;
            scene.Update(FrameSeconds);
        }
        // 移動後の回転中心から先端までのワールドX
        const float movedTipX =
            WorldPoint(hair, settings.tipOffset).x
            - WorldPoint(hair, { 0.0f, 0.0f }).x;
        Require(
            std::abs(sway.CurrentAngle()) > ToRadians(1.0f)
                && movedTipX < restTipX - 1.0f,
            "Sway2D swung the wrong way under a mirrored parent.");
    }

    // 2つの揺れ設定の全項目が一致するか返します(left: 比較元, right: 比較先)。
    [[nodiscard]] bool SameSettings(
        const LamaPon::Sway2DSettings& left,
        const LamaPon::Sway2DSettings& right)
    {
        return NearlyEqual(left.tipOffset.x, right.tipOffset.x)
            && NearlyEqual(left.tipOffset.y, right.tipOffset.y)
            && NearlyEqual(left.stiffness, right.stiffness)
            && NearlyEqual(left.damping, right.damping)
            && NearlyEqual(left.inertia, right.inertia)
            && NearlyEqual(left.gravity.x, right.gravity.x)
            && NearlyEqual(left.gravity.y, right.gravity.y)
            && NearlyEqual(left.maxAngleDegrees, right.maxAngleDegrees)
            && NearlyEqual(
                left.windAmplitudeDegrees,
                right.windAmplitudeDegrees)
            && NearlyEqual(left.windFrequency, right.windFrequency)
            && NearlyEqual(
                left.windPhaseDegrees,
                right.windPhaseDegrees);
    }

    // 範囲外の値の補正と、保存・複製で設定が残ることを検証します(graphics: シーン用の描画装置)。
    void TestSettingsRoundTrip(LamaPon::GraphicsDevice& graphics)
    {
        // 範囲外と非有限値を含む設定
        LamaPon::Sway2DSettings invalid{};
        invalid.stiffness = std::numeric_limits<float>::quiet_NaN();
        invalid.damping = -5.0f;
        invalid.inertia = 5.0f;
        invalid.maxAngleDegrees = 500.0f;
        invalid.windFrequency = -1.0f;
        invalid.gravity = {
            std::numeric_limits<float>::infinity(),
            10.0f };
        // 補正後の設定
        const auto sanitized =
            LamaPon::Sway2DComponent::Sanitize(invalid);
        Require(
            sanitized.stiffness == LamaPon::Sway2DSettings{}.stiffness
                && sanitized.damping == 0.0f
                && sanitized.inertia == 1.0f
                && sanitized.maxAngleDegrees == 180.0f
                && sanitized.windFrequency == 0.0f
                && sanitized.gravity.x == 0.0f
                && sanitized.gravity.y == 10.0f,
            "Sway2D settings were not sanitized.");

        // 保存元のシーン
        LamaPon::Scene scene(graphics);
        // 揺れ物を持つ物体
        auto& cloth = scene.CreateGameObject("Cloth");
        // 既定値と異なる揺れ設定
        LamaPon::Sway2DSettings settings{};
        settings.tipOffset = { 12.0f, 80.0f };
        settings.stiffness = 45.0f;
        settings.damping = 3.5f;
        settings.inertia = 0.25f;
        settings.gravity = { 0.0f, 600.0f };
        settings.maxAngleDegrees = 70.0f;
        settings.windAmplitudeDegrees = 4.0f;
        settings.windFrequency = 1.25f;
        settings.windPhaseDegrees = 90.0f;
        cloth.AddComponent<LamaPon::Sway2DComponent>(settings);

        // JSONから復元したシーン
        LamaPon::Scene loaded(graphics);
        loaded.LoadFromJson(scene.SerializeToJson());
        // 復元した物体
        const auto* loadedCloth = loaded.FindGameObjectByName("Cloth");
        // 復元した揺れ物
        const auto* loadedSway =
            loadedCloth != nullptr
                ? loadedCloth->GetComponent<LamaPon::Sway2DComponent>()
                : nullptr;
        Require(
            loadedSway != nullptr
                && SameSettings(loadedSway->Settings(), settings),
            "Sway2D settings did not round-trip through JSON.");

        // 複製した物体
        auto& duplicate = scene.DuplicateGameObject(cloth);
        // 複製した揺れ物
        const auto* duplicateSway =
            duplicate.GetComponent<LamaPon::Sway2DComponent>();
        Require(
            duplicateSway != nullptr
                && SameSettings(duplicateSway->Settings(), settings),
            "Sway2D settings were not duplicated.");
    }
}

// 2D揺れ物の各検証を実行します。
int main()
{
    // COM初期化結果
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    try
    {
        // シーン用の描画装置
        LamaPon::GraphicsDevice graphics;
        TestRestLagAndSettle(graphics);
        TestGravityPauseAndDisable(graphics);
        TestExternalRotationWriter(graphics);
        TestChainOrder(graphics);
        TestMirroredParent(graphics);
        TestSettingsRoundTrip(graphics);
    }
    // exception: 検証中に送出された例外
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: exception: " << exception.what() << '\n';
        ++g_failures;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }
    if (g_failures != 0)
    {
        std::cerr << g_failures << " Sway2D check(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Sway2D checks passed.\n";
    return EXIT_SUCCESS;
}
