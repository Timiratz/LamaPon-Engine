#include "LamaPon/Components/Keyform2DComponent.h"
#include "LamaPon/Components/Rig2DComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/SpriteSkin2DComponent.h"
#include "LamaPon/Components/Sway2DComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <objbase.h>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

// 2Dリグのパラメータとキーフォームの補間・合成・記録・保存を検証します。
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
        const float tolerance = 1.0e-3f)
    {
        return std::abs(left - right) <= tolerance;
    }

    // Z軸の回転を度で返します(object: 対象)。
    [[nodiscard]] float ZDegrees(const LamaPon::GameObject& object)
    {
        // 対象のローカル回転
        const auto& rotation = object.GetTransform().rotationQuaternion;
        return 2.0f * std::atan2(rotation.z, rotation.w)
            * (180.0f / std::numbers::pi_v<float>);
    }

    // 移動・回転・拡縮のキーを作ります(value: パラメータ値, x: X移動, y: Y移動, degrees: 回転, scaleX: X倍率)。
    [[nodiscard]] LamaPon::Keyform2DKey PoseKey(
        const float value,
        const float x,
        const float y,
        const float degrees,
        const float scaleX = 1.0f)
    {
        // 作るキー
        LamaPon::Keyform2DKey key;
        key.value = value;
        key.positionOffset = { x, y };
        key.rotationDegrees = degrees;
        key.scale = { scaleX, 1.0f };
        return key;
    }

    // 角度Xと目の開きを持つキャラクターと鼻・まぶたを作ります(scene: 生成先)。
    LamaPon::GameObject& CreateCharacter(LamaPon::Scene& scene)
    {
        // パラメータを持つ根元
        auto& character = scene.CreateGameObject("Character");
        // 根元のリグ
        auto& rig = character.AddComponent<LamaPon::Rig2DComponent>();
        rig.AddParameter({ "AngleX", -30.0f, 30.0f, 0.0f, 0.0f });
        rig.AddParameter({ "AngleY", -30.0f, 30.0f, 0.0f, 0.0f });
        rig.AddParameter({ "EyeOpen", 0.0f, 1.0f, 1.0f, 1.0f });

        // 顔の向きで動く鼻
        auto& nose = scene.CreateGameObject("Nose");
        nose.SetParent(&character);
        nose.GetTransform().position = { 100.0f, 50.0f, 0.0f };
        nose.AddComponent<LamaPon::SpriteRendererComponent>(
            DirectX::XMFLOAT2{ 10.0f, 10.0f });
        // 鼻のキーフォーム
        auto& noseKeys = nose.AddComponent<LamaPon::Keyform2DComponent>();
        noseKeys.SetKey("AngleX", PoseKey(-30.0f, -12.0f, -2.0f, -6.0f));
        noseKeys.SetKey("AngleX", PoseKey(0.0f, 0.0f, 0.0f, 0.0f));
        noseKeys.SetKey("AngleX", PoseKey(30.0f, 12.0f, 2.0f, 6.0f, 1.1f));
        noseKeys.SetKey("AngleY", PoseKey(-30.0f, 0.0f, -8.0f, 0.0f));
        noseKeys.SetKey("AngleY", PoseKey(30.0f, 0.0f, 8.0f, 0.0f));

        // 目の開きで薄くなるまぶた
        auto& lid = scene.CreateGameObject("Lid");
        lid.SetParent(&character);
        lid.AddComponent<LamaPon::SpriteRendererComponent>(
            DirectX::XMFLOAT2{ 10.0f, 4.0f },
            DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 0.8f });
        // まぶたのキーフォーム
        auto& lidKeys = lid.AddComponent<LamaPon::Keyform2DComponent>();
        // 閉じた目のキー
        LamaPon::Keyform2DKey closed;
        closed.value = 0.0f;
        closed.opacity = 0.25f;
        lidKeys.SetKey("EyeOpen", closed);
        // 開いた目のキー
        LamaPon::Keyform2DKey open;
        open.value = 1.0f;
        lidKeys.SetKey("EyeOpen", open);
        return character;
    }

    // パラメータの登録・範囲・自動の揺れを検証します(graphics: シーン用の描画装置)。
    void TestParameters(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // パラメータを持つ物体
        auto& owner = scene.CreateGameObject("Owner");
        // 検証するリグ
        auto& rig = owner.AddComponent<LamaPon::Rig2DComponent>();
        Require(
            !rig.AddParameter({ "" })
                && !rig.AddParameter({ std::string(65, 'a') }),
            "Invalid parameter names were accepted.");
        Require(
            rig.AddParameter({ "Breath", 1.0f, -1.0f, 5.0f, 0.0f, 0.5f, 1.0f })
                && rig.FindParameter("Breath") != nullptr
                && rig.FindParameter("Breath")->minimum == -1.0f
                && rig.FindParameter("Breath")->defaultValue == 1.0f,
            "A parameter was not sanitized.");
        Require(
            rig.SetParameter("Breath", 7.0f)
                && rig.ParameterValue("Breath") == 1.0f
                && !rig.SetParameter("Missing", 0.0f)
                && rig.ParameterValue("Missing") == 0.0f,
            "Parameter values were not clamped or looked up.");
        rig.SetParameter("Breath", 0.0f);
        // 4分の1周期まで進めたフレーム数
        for (int frame = 0; frame < 15; ++frame)
        {
            scene.Update(FrameSeconds);
        }
        Require(
            NearlyEqual(rig.ParameterValue("Breath"), 0.5f, 1.0e-2f),
            "The automatic wave did not move the parameter.");
        rig.SetParameters({
            { "A", 0.0f, 1.0f },
            { "A", 0.0f, 2.0f },
            { "B", 0.0f, 1.0f } });
        Require(
            rig.Parameters().size() == 2
                && rig.Parameters()[0].name == "A"
                && rig.Parameters()[0].maximum == 1.0f
                && rig.Parameters()[1].name == "B",
            "SetParameters did not keep order and drop duplicates.");
        rig.SetParameter("A", 1.0f);
        rig.ResetParameters();
        Require(
            rig.ParameterValue("A") == 0.0f && rig.RemoveParameter("B")
                && rig.Parameters().size() == 1,
            "Reset or remove did not work.");
    }

    // 姿勢と不透明度の補間・合成・無効化・プレビューを検証します(graphics: シーン用の描画装置)。
    void TestPoseKeys(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // キャラクターの根元
        auto& character = CreateCharacter(scene);
        // 根元のリグ
        auto& rig = *character.GetComponent<LamaPon::Rig2DComponent>();
        // 鼻
        auto& nose = *scene.FindGameObjectByName("Nose");
        // まぶた
        auto& lid = *scene.FindGameObjectByName("Lid");
        // まぶたのSprite Renderer
        auto& lidSprite =
            *lid.GetComponent<LamaPon::SpriteRendererComponent>();

        scene.Update(FrameSeconds);
        Require(
            NearlyEqual(nose.GetTransform().position.x, 100.0f)
                && NearlyEqual(lidSprite.Color().w, 0.8f),
            "Default parameters moved the parts.");
        Require(
            !nose.GetComponent<LamaPon::SpriteRendererComponent>()->UsesMesh(),
            "A pose-only keyform switched its sprite to the mesh path.");

        rig.SetParameter("AngleX", 15.0f);
        rig.SetParameter("AngleY", -15.0f);
        rig.SetParameter("EyeOpen", 0.5f);
        scene.Update(FrameSeconds);
        Require(
            NearlyEqual(nose.GetTransform().position.x, 106.0f)
                && NearlyEqual(nose.GetTransform().position.y, 47.0f)
                && NearlyEqual(ZDegrees(nose), 3.0f, 1.0e-2f)
                && NearlyEqual(nose.GetTransform().scale.x, 1.05f),
            "Pose keys were not interpolated and added together.");
        Require(
            NearlyEqual(lidSprite.Color().w, 0.8f * 0.625f),
            "Opacity keys were not interpolated.");

        rig.SetParameter("AngleX", -45.0f);
        scene.Update(FrameSeconds);
        Require(
            NearlyEqual(nose.GetTransform().position.x, 88.0f)
                && NearlyEqual(ZDegrees(nose), -6.0f, 1.0e-2f),
            "A clamped parameter did not use the end key.");

        // 毎フレーム書いても回転は積み重なりません。
        // 待機したフレーム数
        for (int frame = 0; frame < 30; ++frame)
        {
            scene.Update(FrameSeconds);
        }
        Require(
            NearlyEqual(ZDegrees(nose), -6.0f, 1.0e-2f),
            "Pose keys accumulated over frames.");

        nose.GetComponent<LamaPon::Keyform2DComponent>()->SetEnabled(false);
        Require(
            NearlyEqual(nose.GetTransform().position.x, 100.0f)
                && NearlyEqual(ZDegrees(nose), 0.0f, 1.0e-3f),
            "Disabling a keyform did not restore its rest pose.");
        nose.GetComponent<LamaPon::Keyform2DComponent>()->SetEnabled(true);

        // 更新しない編集中でも、リグからプレビューと復元ができます。
        rig.SetParameter("AngleX", 30.0f);
        rig.SetParameter("AngleY", 0.0f);
        rig.ApplyToHierarchy();
        Require(
            NearlyEqual(nose.GetTransform().position.x, 112.0f),
            "ApplyToHierarchy did not preview the pose.");
        rig.RestoreHierarchyRestPose();
        Require(
            NearlyEqual(nose.GetTransform().position.x, 100.0f)
                && NearlyEqual(lidSprite.Color().w, 0.8f),
            "RestoreHierarchyRestPose did not restore the parts.");
    }

    // 揺れ物と同じ部品で使っても回転が積み重ならないことを検証します(graphics: シーン用の描画装置)。
    void TestWithSway(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // キャラクターの根元
        auto& character = CreateCharacter(scene);
        // 根元のリグ
        auto& rig = *character.GetComponent<LamaPon::Rig2DComponent>();
        // 鼻
        auto& nose = *scene.FindGameObjectByName("Nose");
        // 重力で垂れる揺れ設定
        LamaPon::Sway2DSettings settings{};
        settings.tipOffset = { 30.0f, 0.0f };
        settings.gravity = { 0.0f, 2000.0f };
        settings.stiffness = 20.0f;
        // 鼻の揺れ物
        auto& sway = nose.AddComponent<LamaPon::Sway2DComponent>(settings);
        rig.SetParameter("AngleX", 30.0f);
        // 更新したフレーム数
        for (int frame = 0; frame < 600; ++frame)
        {
            scene.Update(FrameSeconds);
        }
        // キーと揺れを合わせた角度
        const float settled = ZDegrees(nose);
        Require(
            sway.CurrentAngle() > 0.01f
                && NearlyEqual(
                    settled,
                    6.0f
                        + sway.CurrentAngle()
                            * (180.0f / std::numbers::pi_v<float>),
                    1.0e-2f),
            "Sway did not add on top of the keyform rotation.");
        // 追加で更新したフレーム数
        for (int frame = 0; frame < 120; ++frame)
        {
            scene.Update(FrameSeconds);
        }
        Require(
            NearlyEqual(ZDegrees(nose), settled, 1.0e-2f),
            "Keyform and sway accumulated rotation.");
    }

    // 頂点の移動キーと、スキンで曲げた形の記録を検証します(graphics: シーン用の描画装置)。
    void TestMeshKeys(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // キャラクターの根元
        auto& character = CreateCharacter(scene);
        // 根元のリグ
        auto& rig = *character.GetComponent<LamaPon::Rig2DComponent>();
        rig.AddParameter({ "MouthOpen", 0.0f, 1.0f, 0.0f, 0.0f });
        // 口
        auto& mouth = scene.CreateGameObject("Mouth");
        mouth.SetParent(&character);
        // 口のSprite Renderer
        auto& sprite = mouth.AddComponent<LamaPon::SpriteRendererComponent>(
            DirectX::XMFLOAT2{ 20.0f, 10.0f });
        // 口のキーフォーム
        auto& keys = mouth.AddComponent<LamaPon::Keyform2DComponent>();
        // 閉じた口のキー
        LamaPon::Keyform2DKey closed;
        closed.value = 0.0f;
        keys.SetKey("MouthOpen", closed);
        // 下の2頂点を下げた開いた口のキー
        LamaPon::Keyform2DKey open;
        open.value = 1.0f;
        open.vertexOffsets = {
            { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 10.0f }, { 0.0f, 10.0f } };
        keys.SetKey("MouthOpen", open);
        Require(sprite.UsesMesh(), "A keyform sprite did not use the mesh path.");
        // 変形前の静止頂点
        const auto rest = sprite.MeshRestPositions();
        rig.SetParameter("MouthOpen", 0.5f);
        // 半分開いた頂点
        const auto half = sprite.DeformedMeshPositions();
        Require(
            NearlyEqual(half[0].y, rest[0].y)
                && NearlyEqual(half[2].y, rest[2].y + 5.0f)
                && NearlyEqual(half[3].y, rest[3].y + 5.0f),
            "Vertex keys were not interpolated.");

        // スキンで曲げた形を記録し、ボーンを戻してもキーで同じ形になります。
        // 縦に分けた髪
        auto& hair = scene.CreateGameObject("Hair");
        hair.SetParent(&character);
        // 髪のSprite Renderer
        auto& hairSprite = hair.AddComponent<LamaPon::SpriteRendererComponent>(
            DirectX::XMFLOAT2{ 20.0f, 100.0f });
        hairSprite.SetPivot({ 0.5f, 0.0f });
        hairSprite.SetMeshGrid(1, 4);
        // 髪のキーフォーム
        auto& hairKeys = hair.AddComponent<LamaPon::Keyform2DComponent>();
        // 髪のスキン
        auto& skin = hair.AddComponent<LamaPon::SpriteSkin2DComponent>();
        // 髪のボーン
        const auto bones = skin.CreateBoneChain(2, false);
        bones[1]->GetTransform().SetEulerAngles(0.0f, 0.0f, 0.6f);
        // ボーンで曲げた形
        const auto bent = hairSprite.DeformedMeshPositions();
        rig.SetParameter("AngleX", 30.0f);
        Require(
            hairKeys.RecordMeshKey("AngleX", 30.0f)
                && hairKeys.Channels().size() == 1
                && hairKeys.Channels()[0].keys[0].vertexOffsets.size()
                    == bent.size(),
            "RecordMeshKey did not store the bent shape.");
        bones[1]->GetTransform().SetEulerAngles(0.0f, 0.0f, 0.0f);
        // キーだけで曲げた形
        const auto recorded = hairSprite.DeformedMeshPositions();
        // 形が一致したか
        bool matches = recorded.size() == bent.size();
        // 比べる頂点番号
        for (std::size_t index = 0; matches && index < bent.size(); ++index)
        {
            matches = NearlyEqual(recorded[index].x, bent[index].x, 1.0e-2f)
                && NearlyEqual(recorded[index].y, bent[index].y, 1.0e-2f);
        }
        Require(matches, "The recorded mesh key did not reproduce the shape.");
    }

    // ボーンに付けたキーフォームがパラメータでボーンを回し、メッシュを曲げることを検証します(graphics: シーン用の描画装置)。
    void TestKeyformOnBone(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // キャラクターの根元
        auto& character = CreateCharacter(scene);
        // 根元のリグ
        auto& rig = *character.GetComponent<LamaPon::Rig2DComponent>();
        // 縦に分けた前髪
        auto& bangs = scene.CreateGameObject("Bangs");
        bangs.SetParent(&character);
        // 前髪のSprite Renderer
        auto& sprite = bangs.AddComponent<LamaPon::SpriteRendererComponent>(
            DirectX::XMFLOAT2{ 20.0f, 100.0f });
        sprite.SetPivot({ 0.5f, 0.0f });
        sprite.SetMeshGrid(1, 4);
        // 前髪のボーン
        const auto bones =
            bangs.AddComponent<LamaPon::SpriteSkin2DComponent>()
                .CreateBoneChain(2, false);
        // 下のボーンを角度Xで回すキーフォーム
        auto& boneKeys = bones[1]->AddComponent<LamaPon::Keyform2DComponent>();
        boneKeys.SetKey("AngleX", PoseKey(0.0f, 0.0f, 0.0f, 0.0f));
        boneKeys.SetKey("AngleX", PoseKey(30.0f, 0.0f, 0.0f, 90.0f));

        scene.Update(FrameSeconds);
        rig.SetParameter("AngleX", 30.0f);
        scene.Update(FrameSeconds);
        // パラメータで曲げた頂点
        const auto bent = sprite.DeformedMeshPositions();
        Require(
            NearlyEqual(bent[8].x, -50.0f, 1.5f)
                && NearlyEqual(bent[8].y, 40.0f, 1.5f),
            "A keyform on a bone did not bend the skinned mesh.");
    }

    // 姿勢の記録・保存・複製を検証します(graphics: シーン用の描画装置)。
    void TestRecordingAndPersistence(LamaPon::GraphicsDevice& graphics)
    {
        // 保存元のシーン
        LamaPon::Scene scene(graphics);
        // キャラクターの根元
        auto& character = CreateCharacter(scene);
        // 鼻
        auto& nose = *scene.FindGameObjectByName("Nose");
        // 鼻のキーフォーム
        auto& keys = *nose.GetComponent<LamaPon::Keyform2DComponent>();
        scene.Update(FrameSeconds);
        Require(keys.HasRestPose(), "The rest pose was not captured.");

        nose.GetTransform().position = { 110.0f, 52.0f, 0.0f };
        nose.GetTransform().SetEulerAngles(
            0.0f,
            0.0f,
            10.0f * (std::numbers::pi_v<float> / 180.0f));
        Require(
            keys.RecordPoseKey("AngleX", 30.0f),
            "RecordPoseKey failed.");
        // 記録した角度Xのキー
        const auto& recorded = keys.Channels()[0].keys.back();
        Require(
            keys.Channels()[0].parameter == "AngleX"
                && NearlyEqual(recorded.positionOffset.x, 10.0f)
                && NearlyEqual(recorded.positionOffset.y, 2.0f)
                && NearlyEqual(recorded.rotationDegrees, 10.0f, 1.0e-2f),
            "RecordPoseKey did not store the difference from rest.");
        Require(
            keys.RemoveKey("AngleX", -30.0f)
                && keys.Channels()[0].keys.size() == 2,
            "RemoveKey did not remove the key.");

        // JSONから復元したシーン
        LamaPon::Scene loaded(graphics);
        loaded.LoadFromJson(scene.SerializeToJson());
        // 復元した根元
        const auto* loadedCharacter = loaded.FindGameObjectByName("Character");
        // 復元したリグ
        const auto* loadedRig = loadedCharacter != nullptr
            ? loadedCharacter->GetComponent<LamaPon::Rig2DComponent>()
            : nullptr;
        // 復元した鼻
        const auto* loadedNose = loaded.FindGameObjectByName("Nose");
        // 復元した鼻のキーフォーム
        const auto* loadedKeys = loadedNose != nullptr
            ? loadedNose->GetComponent<LamaPon::Keyform2DComponent>()
            : nullptr;
        Require(
            loadedRig != nullptr
                && loadedRig->Parameters().size() == 3
                && loadedRig->Parameters()[2].name == "EyeOpen"
                && loadedRig->Parameters()[2].defaultValue == 1.0f,
            "Rig parameters did not round-trip.");
        Require(
            loadedKeys != nullptr
                && loadedKeys->HasRestPose()
                && NearlyEqual(loadedKeys->RestPosition().x, 100.0f)
                && loadedKeys->Channels().size() == 2
                && loadedKeys->Channels()[0].keys.size() == 2
                && NearlyEqual(
                    loadedKeys->Channels()[0].keys.back().rotationDegrees,
                    10.0f,
                    1.0e-2f),
            "Keyforms did not round-trip.");

        // 複製したキャラクター
        auto& duplicate = scene.DuplicateGameObject(character);
        // 複製した鼻のキーフォーム
        const auto* duplicateKeys = duplicate.Children().empty()
            ? nullptr
            : duplicate.Children().front()
                ->GetComponent<LamaPon::Keyform2DComponent>();
        Require(
            duplicate.GetComponent<LamaPon::Rig2DComponent>() != nullptr
                && duplicateKeys != nullptr
                && duplicateKeys->HasRestPose()
                && duplicateKeys->Channels().size() == 2,
            "Duplicating kept neither the rig nor the keyforms.");
    }
}

// 2Dリグの各検証を実行します。
int main()
{
    // COM初期化結果
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    try
    {
        // シーン用の描画装置
        LamaPon::GraphicsDevice graphics;
        TestParameters(graphics);
        TestPoseKeys(graphics);
        TestWithSway(graphics);
        TestMeshKeys(graphics);
        TestKeyformOnBone(graphics);
        TestRecordingAndPersistence(graphics);
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
        std::cerr << g_failures << " Rig2D check(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Rig2D checks passed.\n";
    return EXIT_SUCCESS;
}
