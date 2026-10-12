#include "LamaPon/Components/Blink2DComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <objbase.h>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

// 2D瞬きのコマ順・同期・間隔・保持・保存を検証します。
namespace
{
    // assertion失敗の件数
    int g_failures = 0;
    // 細かく進める1回の秒数
    constexpr float StepSeconds = 0.005f;

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

    // 1行のシートで表示中のコマ番号を返します(object: Sprite Rendererを持つ物体, columns: シートの列数)。
    [[nodiscard]] int ShownFrame(
        const LamaPon::GameObject& object,
        const int columns = 3)
    {
        // 物体のスプライト描画
        const auto* sprite =
            object.GetComponent<LamaPon::SpriteRendererComponent>();
        if (sprite == nullptr)
        {
            return -1;
        }
        return static_cast<int>(
            std::lround(
                sprite->SourceRect().x
                * static_cast<float>(columns)));
    }

    // 間隔を固定した基本の瞬き設定を返します。
    [[nodiscard]] LamaPon::Blink2DSettings FixedSettings()
    {
        // 0=開・1=半目・2=閉の3列シートで1秒ごとに瞬く設定
        LamaPon::Blink2DSettings settings{};
        settings.columns = 3;
        settings.rows = 1;
        settings.openFrame = 0;
        settings.closingStartFrame = 1;
        settings.closingFrameCount = 2;
        settings.frameSeconds = 0.05f;
        settings.closedSeconds = 0.1f;
        settings.intervalMinSeconds = 1.0f;
        settings.intervalMaxSeconds = 1.0f;
        settings.doubleBlinkChance = 0.0f;
        return settings;
    }

    // 左右の目を子に持つ顔を作ります(scene: 生成先, settings: 瞬き設定, left: 左目の出力, right: 右目の出力)。
    LamaPon::Blink2DComponent& CreateEyes(
        LamaPon::Scene& scene,
        const LamaPon::Blink2DSettings& settings,
        LamaPon::GameObject*& left,
        LamaPon::GameObject*& right)
    {
        // 両目をまとめる親
        auto& eyes = scene.CreateGameObject("Eyes");
        left = &scene.CreateGameObject("LeftEye");
        right = &scene.CreateGameObject("RightEye");
        left->SetParent(&eyes);
        right->SetParent(&eyes);
        left->AddComponent<LamaPon::SpriteRendererComponent>(
            DirectX::XMFLOAT2{ 32.0f, 16.0f });
        right->AddComponent<LamaPon::SpriteRendererComponent>(
            DirectX::XMFLOAT2{ 32.0f, 16.0f });
        return eyes.AddComponent<LamaPon::Blink2DComponent>(settings);
    }

    // コマ順・両目の同期・表示時間・次の間隔を検証します(graphics: シーン用の描画装置)。
    void TestSequenceAndSync(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 左目
        LamaPon::GameObject* left{};
        // 右目
        LamaPon::GameObject* right{};
        // 検証する瞬き
        auto& blink = CreateEyes(scene, FixedSettings(), left, right);

        scene.Update(0.0f);
        Require(
            ShownFrame(*left) == 0 && ShownFrame(*right) == 0,
            "Blink2D did not show the open frame after initializing.");

        // 表示したコマの変化順
        std::vector<int> sequence{ ShownFrame(*left) };
        // 閉じた目を表示した秒数
        float closedSeconds{};
        // 瞬き中だった秒数
        float blinkingSeconds{};
        // 両目のコマが常に一致したか
        bool synchronized = true;
        // 進めた回数
        for (int step = 0; step < 300; ++step)
        {
            scene.Update(StepSeconds);
            // 今回の左目のコマ
            const int frame = ShownFrame(*left);
            synchronized = synchronized && frame == ShownFrame(*right);
            if (frame != sequence.back())
            {
                sequence.push_back(frame);
            }
            closedSeconds += frame == 2 ? StepSeconds : 0.0f;
            blinkingSeconds += blink.IsBlinking() ? StepSeconds : 0.0f;
        }
        Require(
            sequence == std::vector<int>{ 0, 1, 2, 1, 0 },
            "Blink2D did not play open, half, closed, half, open.");
        Require(synchronized, "Blink2D eyes were not synchronized.");
        Require(
            NearlyEqual(closedSeconds, 0.1f, StepSeconds * 1.5f),
            "Blink2D closed frame duration was wrong.");
        Require(
            NearlyEqual(blinkingSeconds, 0.2f, StepSeconds * 1.5f),
            "Blink2D blink duration was wrong.");
        Require(
            !blink.IsBlinking()
                && NearlyEqual(
                    blink.SecondsUntilNextBlink(),
                    1.0f - (1.5f - 1.0f - 0.2f),
                    StepSeconds * 1.5f),
            "Blink2D did not schedule the next blink after its interval.");

        // 途中コマが2枚あると、開くときは閉じるときの逆順になります。
        // 4列シートで3コマかけて閉じる設定
        auto longSettings = FixedSettings();
        longSettings.columns = 4;
        longSettings.closingFrameCount = 3;
        blink.SetSettings(longSettings);
        // 4列シートで表示したコマの変化順
        std::vector<int> longSequence{ ShownFrame(*left, 4) };
        // 進めた回数
        for (int step = 0; step < 300; ++step)
        {
            scene.Update(StepSeconds);
            // 今回の左目のコマ
            const int frame = ShownFrame(*left, 4);
            if (frame != longSequence.back())
            {
                longSequence.push_back(frame);
            }
        }
        Require(
            longSequence == std::vector<int>{ 0, 1, 2, 3, 2, 1, 0 },
            "Blink2D did not reverse the in-between frames when opening.");
    }

    // 閉じたままの保持と、手動の瞬き・自動停止を検証します(graphics: シーン用の描画装置)。
    void TestHoldAndManual(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 左目
        LamaPon::GameObject* left{};
        // 右目
        LamaPon::GameObject* right{};
        // 自動で瞬かない設定
        auto settings = FixedSettings();
        settings.autoBlink = false;
        // 検証する瞬き
        auto& blink = CreateEyes(scene, settings, left, right);

        // 進めた回数
        for (int step = 0; step < 1000; ++step)
        {
            scene.Update(0.01f);
        }
        Require(
            !blink.IsBlinking() && ShownFrame(*left) == 0,
            "Blink2D blinked although autoBlink was off.");

        blink.SetHoldClosed(true);
        // 進めた回数
        for (int step = 0; step < 300; ++step)
        {
            scene.Update(0.01f);
        }
        Require(
            ShownFrame(*left) == 2 && ShownFrame(*right) == 2,
            "Blink2D did not stay closed while held.");
        blink.SetHoldClosed(false);
        scene.Update(0.05f);
        Require(
            ShownFrame(*left) == 2,
            "Blink2D opened before its closed time after release.");
        scene.Update(0.07f);
        Require(
            ShownFrame(*left) == 1,
            "Blink2D did not show the half frame while opening.");
        scene.Update(0.1f);
        Require(
            ShownFrame(*left) == 0 && !blink.IsBlinking(),
            "Blink2D did not open after being released.");

        blink.Blink();
        scene.Update(0.01f);
        Require(
            ShownFrame(*left) == 1 && blink.IsBlinking(),
            "Blink2D manual blink did not start.");
        scene.Update(1.0f);
        Require(
            ShownFrame(*left) == 0 && !blink.IsBlinking(),
            "Blink2D manual blink did not finish within one update.");

        blink.Blink();
        scene.Update(0.06f);
        blink.SetEnabled(false);
        Require(
            ShownFrame(*left) == 0,
            "Disabling Blink2D did not reopen the eyes.");
    }

    // 二度瞬きと、間隔が範囲内に収まることを検証します(graphics: シーン用の描画装置)。
    void TestIntervals(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 左目
        LamaPon::GameObject* left{};
        // 右目
        LamaPon::GameObject* right{};
        // 必ず二度瞬きする設定
        auto settings = FixedSettings();
        settings.intervalMinSeconds = 2.0f;
        settings.intervalMaxSeconds = 4.0f;
        settings.doubleBlinkChance = 1.0f;
        // 検証する瞬き
        auto& blink = CreateEyes(scene, settings, left, right);
        blink.SetRandomSeed(12345u);
        Require(
            blink.SecondsUntilNextBlink() >= 2.0f
                && blink.SecondsUntilNextBlink() <= 4.0f,
            "Blink2D first interval was outside its range.");

        // 開き終わった直後に引いた間隔
        std::vector<float> intervals;
        // 直前の更新で瞬き中だったか
        bool wasBlinking = false;
        // 進めた回数
        for (int step = 0; step < 6000; ++step)
        {
            scene.Update(StepSeconds);
            if (wasBlinking && !blink.IsBlinking())
            {
                intervals.push_back(blink.SecondsUntilNextBlink());
            }
            wasBlinking = blink.IsBlinking();
        }
        Require(
            intervals.size() >= 4,
            "Blink2D did not blink repeatedly.");
        // 二度瞬きと通常の間隔が交互になったか
        bool alternates = intervals.size() >= 4;
        // 確認した間隔の位置
        for (std::size_t index = 0; index < intervals.size(); ++index)
        {
            // 今回の間隔
            const float interval = intervals[index];
            alternates = alternates
                && (index % 2 == 0
                    ? interval >= 0.1f - StepSeconds
                        && interval <= 0.25f + 1.0e-4f
                    : interval >= 2.0f - StepSeconds
                        && interval <= 4.0f);
        }
        Require(
            alternates,
            "Blink2D double blinks did not alternate with normal intervals.");
    }

    // 別のBlink2Dを持つ子孫と、子孫を含めない設定を検証します(graphics: シーン用の描画装置)。
    void TestTargets(LamaPon::GraphicsDevice& graphics)
    {
        // 検証用のシーン
        LamaPon::Scene scene(graphics);
        // 親より先に更新されても上書きされないよう、子を先に生成する独自の瞬きを持つ子
        auto& wink = scene.CreateGameObject("Wink");
        // 自身もスプライトを持つ親
        auto& face = scene.CreateGameObject("Face");
        face.AddComponent<LamaPon::SpriteRendererComponent>();
        wink.SetParent(&face);
        wink.AddComponent<LamaPon::SpriteRendererComponent>();
        // 子に付けた独自の瞬き設定
        auto winkSettings = FixedSettings();
        winkSettings.openFrame = 1;
        winkSettings.autoBlink = false;
        wink.AddComponent<LamaPon::Blink2DComponent>(winkSettings);
        // 親の瞬き設定を受ける孫
        auto& lash = scene.CreateGameObject("Lash");
        lash.SetParent(&face);
        lash.AddComponent<LamaPon::SpriteRendererComponent>();
        // 親の瞬き
        auto& faceBlink =
            face.AddComponent<LamaPon::Blink2DComponent>(FixedSettings());

        scene.Update(0.0f);
        Require(
            ShownFrame(face) == 0
                && ShownFrame(lash) == 0
                && ShownFrame(wink) == 1,
            "Blink2D overrode a child that has its own Blink2D.");

        // 子孫を含めない設定
        auto ownOnly = FixedSettings();
        ownOnly.includeChildren = false;
        ownOnly.openFrame = 2;
        faceBlink.SetSettings(ownOnly);
        Require(
            ShownFrame(face) == 2 && ShownFrame(lash) == 0,
            "Blink2D includeChildren=false still changed children.");
    }

    // 2つの瞬き設定の全項目が一致するか返します(left: 比較元, right: 比較先)。
    [[nodiscard]] bool SameSettings(
        const LamaPon::Blink2DSettings& left,
        const LamaPon::Blink2DSettings& right)
    {
        return left.columns == right.columns
            && left.rows == right.rows
            && left.openFrame == right.openFrame
            && left.closingStartFrame == right.closingStartFrame
            && left.closingFrameCount == right.closingFrameCount
            && NearlyEqual(left.frameSeconds, right.frameSeconds)
            && NearlyEqual(left.closedSeconds, right.closedSeconds)
            && NearlyEqual(
                left.intervalMinSeconds,
                right.intervalMinSeconds)
            && NearlyEqual(
                left.intervalMaxSeconds,
                right.intervalMaxSeconds)
            && NearlyEqual(
                left.doubleBlinkChance,
                right.doubleBlinkChance)
            && left.autoBlink == right.autoBlink
            && left.includeChildren == right.includeChildren;
    }

    // 範囲外の値の補正と、保存・複製で設定が残ることを検証します(graphics: シーン用の描画装置)。
    void TestSettingsRoundTrip(LamaPon::GraphicsDevice& graphics)
    {
        // 範囲外と非有限値を含む設定
        LamaPon::Blink2DSettings invalid{};
        invalid.columns = 0;
        invalid.rows = 1000;
        invalid.openFrame = -3;
        invalid.closingFrameCount = 0;
        invalid.frameSeconds = std::numeric_limits<float>::quiet_NaN();
        invalid.intervalMinSeconds = 5.0f;
        invalid.intervalMaxSeconds = 1.0f;
        invalid.doubleBlinkChance = 2.0f;
        // 補正後の設定
        const auto sanitized =
            LamaPon::Blink2DComponent::Sanitize(invalid);
        Require(
            sanitized.columns == 1
                && sanitized.rows == 256
                && sanitized.openFrame == 0
                && sanitized.closingFrameCount == 1
                && sanitized.frameSeconds
                    == LamaPon::Blink2DSettings{}.frameSeconds
                && sanitized.intervalMinSeconds == 5.0f
                && sanitized.intervalMaxSeconds == 5.0f
                && sanitized.doubleBlinkChance == 1.0f,
            "Blink2D settings were not sanitized.");

        // 保存元のシーン
        LamaPon::Scene scene(graphics);
        // 瞬きを持つ物体
        auto& eye = scene.CreateGameObject("Eye");
        // 既定値と異なる瞬き設定
        LamaPon::Blink2DSettings settings{};
        settings.columns = 4;
        settings.rows = 2;
        settings.openFrame = 4;
        settings.closingStartFrame = 5;
        settings.closingFrameCount = 3;
        settings.frameSeconds = 0.03f;
        settings.closedSeconds = 0.08f;
        settings.intervalMinSeconds = 1.5f;
        settings.intervalMaxSeconds = 3.5f;
        settings.doubleBlinkChance = 0.3f;
        settings.autoBlink = false;
        settings.includeChildren = false;
        eye.AddComponent<LamaPon::Blink2DComponent>(settings);

        // JSONから復元したシーン
        LamaPon::Scene loaded(graphics);
        loaded.LoadFromJson(scene.SerializeToJson());
        // 復元した物体
        const auto* loadedEye = loaded.FindGameObjectByName("Eye");
        // 復元した瞬き
        const auto* loadedBlink =
            loadedEye != nullptr
                ? loadedEye->GetComponent<LamaPon::Blink2DComponent>()
                : nullptr;
        Require(
            loadedBlink != nullptr
                && SameSettings(loadedBlink->Settings(), settings),
            "Blink2D settings did not round-trip through JSON.");

        // 複製した物体
        auto& duplicate = scene.DuplicateGameObject(eye);
        // 複製した瞬き
        const auto* duplicateBlink =
            duplicate.GetComponent<LamaPon::Blink2DComponent>();
        Require(
            duplicateBlink != nullptr
                && SameSettings(duplicateBlink->Settings(), settings),
            "Blink2D settings were not duplicated.");
    }
}

// 2D瞬きの各検証を実行します。
int main()
{
    // COM初期化結果
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    try
    {
        // シーン用の描画装置
        LamaPon::GraphicsDevice graphics;
        TestSequenceAndSync(graphics);
        TestHoldAndManual(graphics);
        TestIntervals(graphics);
        TestTargets(graphics);
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
        std::cerr << g_failures << " Blink2D check(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Blink2D checks passed.\n";
    return EXIT_SUCCESS;
}
