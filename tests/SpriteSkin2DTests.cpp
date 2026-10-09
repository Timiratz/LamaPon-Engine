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

// 2Dスキンの曲げ・追従・重み・揺れ物との連携・保存・複製を検証します。
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

    // 2点が許容差内で一致するか返します(actual: 実際の位置, expected: 期待する位置, tolerance: 許容差)。
    [[nodiscard]] bool Near(
        const DirectX::XMFLOAT2& actual,
        const DirectX::XMFLOAT2& expected,
        const float tolerance)
    {
        return std::abs(actual.x - expected.x) <= tolerance
            && std::abs(actual.y - expected.y) <= tolerance;
    }

    // 全頂点が許容差内で一致するか返します(actual: 実際の頂点列, expected: 期待する頂点列, tolerance: 許容差)。
    [[nodiscard]] bool AllNear(
        const std::vector<DirectX::XMFLOAT2>& actual,
        const std::vector<DirectX::XMFLOAT2>& expected,
        const float tolerance)
    {
        if (actual.size() != expected.size())
        {
            return false;
        }
        // 比較する頂点番号
        for (std::size_t index = 0; index < actual.size(); ++index)
        {
            if (!Near(actual[index], expected[index], tolerance))
            {
                return false;
            }
        }
        return true;
    }

    // 幅20・高さ100で上辺中央を基準点にした縦長の髪を作ります(scene: 生成先)。
    // 格子は1列4行で、頂点は上の行から(-10,0),(10,0),(-10,25)...(10,100)と並びます。
    LamaPon::GameObject& CreateHair(LamaPon::Scene& scene)
    {
        // 髪のGameObject
        auto& hair = scene.CreateGameObject("Hair");
        // 髪のSprite Renderer
        auto& sprite =
            hair.AddComponent<LamaPon::SpriteRendererComponent>(
                DirectX::XMFLOAT2{ 20.0f, 100.0f });
        sprite.SetPivot({ 0.5f, 0.0f });
        sprite.SetMeshGrid(1, 4);
        return hair;
    }

    // 髪のSprite Rendererを返します(hair: 髪のGameObject)。
    [[nodiscard]] LamaPon::SpriteRendererComponent& Sprite(
        LamaPon::GameObject& hair)
    {
        return *hair.GetComponent<LamaPon::SpriteRendererComponent>();
    }

    // ボーンを曲げると下側の頂点だけが回り、元に戻すと静止頂点へ戻ることを検証します(graphics: シーン用の描画装置)。
    void TestBendAndFollow(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 髪
        auto& hair = CreateHair(scene);
        // 髪のスキン
        auto& skin = hair.AddComponent<LamaPon::SpriteSkin2DComponent>();
        // 根元と中ほどのボーン
        const auto bones = skin.CreateBoneChain(2, false);
        Require(
            bones.size() == 2
                && skin.IsBound()
                && bones[0]->Parent() == &hair
                && bones[1]->Parent() == bones[0]
                && Near(
                    { bones[1]->GetTransform().position.x,
                        bones[1]->GetTransform().position.y },
                    { 0.0f, 50.0f },
                    1.0e-4f),
            "CreateBoneChain did not build a two-bone chain.");
        Require(
            Sprite(hair).UsesMesh(),
            "A skinned sprite did not use the mesh path.");
        // 変形前の静止頂点
        const auto rest = Sprite(hair).MeshRestPositions();
        Require(
            AllNear(Sprite(hair).DeformedMeshPositions(), rest, 1.0e-3f),
            "A freshly bound skin moved its vertices.");

        // 自動の重みでは上端は根元、下端は中ほどのボーンに強く結び付きます。
        // 頂点ごとの重み
        const auto& weights = skin.Weights();
        Require(
            weights.size() == rest.size()
                && weights[0].bones[0] == 0
                && weights[0].weights[0] > 0.9f
                && weights[9].bones[0] == 1
                && weights[9].weights[0] > 0.9f,
            "Automatic weights did not follow the nearest bones.");

        bones[1]->GetTransform().SetEulerAngles(
            0.0f,
            0.0f,
            std::numbers::pi_v<float> * 0.5f);
        // 中ほどで90度曲げた頂点
        const auto bent = Sprite(hair).DeformedMeshPositions();
        Require(
            Near(bent[0], rest[0], 0.5f) && Near(bent[1], rest[1], 0.5f),
            "Bending the lower bone moved the root vertices.");
        Require(
            Near(bent[8], { -50.0f, 40.0f }, 1.5f)
                && Near(bent[9], { -50.0f, 60.0f }, 1.5f),
            "The tip vertices did not rotate around the lower bone.");

        // 髪全体を動かしても、子のボーンが一緒に動くので形は変わりません。
        bones[1]->GetTransform().SetEulerAngles(0.0f, 0.0f, 0.0f);
        hair.GetTransform().position = { 200.0f, 120.0f, 0.0f };
        hair.GetTransform().SetEulerAngles(0.0f, 0.0f, 0.7f);
        hair.GetTransform().scale = { 1.5f, 0.8f, 1.0f };
        Require(
            AllNear(Sprite(hair).DeformedMeshPositions(), rest, 1.0e-2f),
            "The skin did not follow the sprite rigidly.");

        // 重みは格子と同数で正規化して設定し、不正な数は拒否します。
        Require(
            !skin.SetWeights({ LamaPon::SpriteSkinWeight{} }),
            "Weights with the wrong count were accepted.");
        // 全頂点を根元へ結び付ける重み
        std::vector<LamaPon::SpriteSkinWeight> rootWeights(rest.size());
        // 設定する頂点の重み
        for (auto& weight : rootWeights)
        {
            weight.weights = { 2.0f, 0.0f, 0.0f, 0.0f };
        }
        Require(
            skin.SetWeights(rootWeights)
                && skin.Weights()[9].weights[0] == 1.0f,
            "Valid weights were not normalized and accepted.");
    }

    // 揺れ物を付けたボーンの鎖で、先ほど大きく遅れてしなることを検証します(graphics: シーン用の描画装置)。
    void TestSwayingChain(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 動かす頭
        auto& head = scene.CreateGameObject("Head");
        // 頭の子の髪
        auto& hair = CreateHair(scene);
        hair.SetParent(&head);
        // 髪のスキン
        auto& skin = hair.AddComponent<LamaPon::SpriteSkin2DComponent>();
        // 揺れ物付きの3本のボーン
        const auto bones = skin.CreateBoneChain(3, true);
        Require(
            bones.size() == 3
                && bones[0]->GetComponent<LamaPon::Sway2DComponent>()
                    == nullptr
                && bones[1]->GetComponent<LamaPon::Sway2DComponent>()
                    != nullptr
                && bones[2]->GetComponent<LamaPon::Sway2DComponent>()
                    != nullptr,
            "CreateBoneChain did not add Sway2D below the root bone.");
        // 変形前の静止頂点
        const auto rest = Sprite(hair).MeshRestPositions();

        // 移動したフレーム数
        for (int frame = 0; frame < 10; ++frame)
        {
            head.GetTransform().position.x += 20.0f;
            scene.Update(FrameSeconds);
        }
        // 揺れで曲がった頂点
        const auto swayed = Sprite(hair).DeformedMeshPositions();
        Require(
            Near(swayed[0], rest[0], 0.5f),
            "The root vertex moved while the hair swayed.");
        // 中ほどの頂点の遅れ
        const float middleLag = rest[4].x - swayed[4].x;
        // 先端の頂点の遅れ
        const float tipLag = rest[8].x - swayed[8].x;
        Require(
            middleLag > 1.0f && tipLag > middleLag + 1.0f,
            "The swaying chain did not bend more toward the tip.");

        // 長く待つと静止姿勢へ戻ります。
        // 待機したフレーム数
        for (int frame = 0; frame < 900; ++frame)
        {
            scene.Update(FrameSeconds);
        }
        Require(
            AllNear(Sprite(hair).DeformedMeshPositions(), rest, 1.0f),
            "The swaying chain did not settle back to rest.");
    }

    // 保存・読み込み・複製でボーンとバインドが引き継がれることを検証します(graphics: シーン用の描画装置)。
    void TestPersistence(LamaPon::GraphicsDevice& graphics)
    {
        // 保存元のシーン
        LamaPon::Scene scene(graphics);
        // 髪
        auto& hair = CreateHair(scene);
        // 髪のスキン
        auto& skin = hair.AddComponent<LamaPon::SpriteSkin2DComponent>();
        skin.SetWeightFalloff(3.0f);
        // 根元と中ほどのボーン
        const auto bones = skin.CreateBoneChain(2, false);
        bones[1]->GetTransform().SetEulerAngles(0.0f, 0.0f, 0.8f);
        // 保存前の曲がった頂点
        const auto bent = Sprite(hair).DeformedMeshPositions();

        // JSONから復元したシーン
        LamaPon::Scene loaded(graphics);
        loaded.LoadFromJson(scene.SerializeToJson());
        // 復元した髪
        auto* loadedHair = loaded.FindGameObjectByName("Hair");
        // 復元したスキン
        const auto* loadedSkin = loadedHair != nullptr
            ? loadedHair->GetComponent<LamaPon::SpriteSkin2DComponent>()
            : nullptr;
        Require(
            loadedSkin != nullptr
                && loadedSkin->IsBound()
                && loadedSkin->Bones() == skin.Bones()
                && loadedSkin->WeightFalloff() == 3.0f
                && loadedSkin->Weights().size() == skin.Weights().size()
                && AllNear(
                    Sprite(*loadedHair).DeformedMeshPositions(),
                    bent,
                    1.0e-3f),
            "The skin did not round-trip through JSON.");

        // 子のボーンごと複製した髪
        auto& duplicate = scene.DuplicateGameObject(hair);
        // 複製したスキン
        const auto* duplicateSkin =
            duplicate.GetComponent<LamaPon::SpriteSkin2DComponent>();
        // 複製したスキンのボーンがすべて複製側の子孫か
        bool remapped = duplicateSkin != nullptr
            && duplicateSkin->IsBound()
            && duplicateSkin->Bones().size() == 2;
        // 確認するボーンID
        for (const auto id : duplicateSkin != nullptr
                ? duplicateSkin->Bones()
                : std::vector<std::uint64_t>{})
        {
            // 祖先をたどる物体
            const auto* ancestor = scene.FindGameObject(id);
            while (ancestor != nullptr && ancestor != &duplicate)
            {
                ancestor = ancestor->Parent();
            }
            remapped = remapped && ancestor == &duplicate;
        }
        Require(remapped, "Duplicated skin bones were not remapped.");
        Require(
            AllNear(Sprite(duplicate).DeformedMeshPositions(), bent, 1.0e-3f),
            "The duplicated skin did not keep its pose.");
        // プリハブとして保存し配置した髪
        auto& instance = scene.InstantiatePrefabFromJson(
            scene.SerializePrefabToJson(hair));
        // 配置した髪のスキン
        const auto* instanceSkin =
            instance.GetComponent<LamaPon::SpriteSkin2DComponent>();
        // 配置した髪のボーンがすべて配置側の子孫か
        bool instanced = instanceSkin != nullptr
            && instanceSkin->IsBound()
            && instanceSkin->Bones().size() == 2;
        // 確認するボーンID
        for (const auto id : instanceSkin != nullptr
                ? instanceSkin->Bones()
                : std::vector<std::uint64_t>{})
        {
            // 祖先をたどる物体
            const auto* ancestor = scene.FindGameObject(id);
            while (ancestor != nullptr && ancestor != &instance)
            {
                ancestor = ancestor->Parent();
            }
            instanced = instanced && ancestor == &instance;
        }
        Require(instanced, "Prefab skin bones were not remapped.");
        Require(
            AllNear(Sprite(instance).DeformedMeshPositions(), bent, 1.0e-3f),
            "The prefab skin did not keep its pose.");

        bones[1]->GetTransform().SetEulerAngles(0.0f, 0.0f, 0.0f);
        Require(
            AllNear(Sprite(duplicate).DeformedMeshPositions(), bent, 1.0e-3f),
            "Posing the original bone changed the duplicate.");
    }

    // ボーンの削除や格子の変更があっても安全に描けることを検証します(graphics: シーン用の描画装置)。
    void TestInvalidStates(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // Sprite Rendererのない物体
        auto& empty = scene.CreateGameObject("Empty");
        // バインドできないスキン
        auto& lonely = empty.AddComponent<LamaPon::SpriteSkin2DComponent>();
        Require(
            !lonely.Bind() && lonely.CreateBoneChain(2, false).empty(),
            "A skin without a sprite was bound.");

        // 髪
        auto& hair = CreateHair(scene);
        // 髪のスキン
        auto& skin = hair.AddComponent<LamaPon::SpriteSkin2DComponent>();
        // 根元と中ほどのボーン
        const auto bones = skin.CreateBoneChain(2, false);
        // 変形前の静止頂点
        const auto rest = Sprite(hair).MeshRestPositions();
        hair.GetTransform().position = { 30.0f, 40.0f, 0.0f };
        Require(
            scene.DestroyGameObject(*bones[0]),
            "The bones could not be destroyed.");
        Require(
            AllNear(Sprite(hair).DeformedMeshPositions(), rest, 1.0e-3f),
            "Missing bones did not follow the sprite rigidly.");

        Sprite(hair).SetMeshGrid(2, 2);
        Require(
            AllNear(
                Sprite(hair).DeformedMeshPositions(),
                Sprite(hair).MeshRestPositions(),
                1.0e-5f),
            "A skin bound to another grid deformed the new grid.");
        Require(
            !skin.ComputeAutomaticWeights(),
            "Weights were computed for a different grid.");
    }
}

// 2Dスキンの各検証を実行します。
int main()
{
    // COM初期化結果
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    try
    {
        // シーン用の描画装置
        LamaPon::GraphicsDevice graphics;
        TestBendAndFollow(graphics);
        TestSwayingChain(graphics);
        TestPersistence(graphics);
        TestInvalidStates(graphics);
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
        std::cerr << g_failures << " SpriteSkin2D check(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "SpriteSkin2D checks passed.\n";
    return EXIT_SUCCESS;
}
