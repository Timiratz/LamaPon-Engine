#include "LamaPon/LamaPon.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    // Require(condition: 条件, message: 失敗理由)は不成立時に例外を送出する。
    void Require(const bool condition, const char* message)
    {
        // 条件違反を検出する
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class ThrowingDestroyScript final : public LamaPon::Script
    {
    public:
        // 破棄されたインスタンス数
        static inline int destroyed{};
        // テスト用イベントを購読する
        void Awake() override
        {
            On("cleanup-test", [] {});
        }
        // 破棄時の例外伝播を検証する
        void OnDestroy() override
        {
            throw std::runtime_error("expected destroy failure");
        }
        // 破棄されたインスタンスを記録する
        ~ThrowingDestroyScript() override
        {
            ++destroyed;
        }
    };

    class WindowSizeScript final : public LamaPon::Script
    {
    public:
        // Resize(width: 幅, height: 高さ)はウィンドウサイズを変更する。
        [[nodiscard]] bool Resize(
            const std::uint32_t width,
            const std::uint32_t height)
        {
            return SetWindowSize(width, height);
        }

        // Size()は現在のウィンドウサイズを返す。
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
            Size() const
        {
            return WindowSize();
        }
    };

    // スクリプトからのウィンドウサイズ連携を検証する
    void CheckWindowSizeBridge()
    {
        // テスト用描画デバイス
        LamaPon::GraphicsDevice graphics;
        // サイズ連携を検証するシーン
        LamaPon::Scene scene(graphics);
        // サイズ変更を試すゲームオブジェクト
        auto& object = scene.CreateGameObject("WindowSize");
        // サイズ連携テスト用ブリッジ
        using Bridge = LamaPon::Detail::ScriptBridge<WindowSizeScript>;
        // ブリッジが生成したスクリプト
        auto* script = static_cast<WindowSizeScript*>(
            Bridge::Create(&object, &graphics, "{}"));
        Require(!script->Resize(800, 600),
            "A scene without a window must reject resize requests.");

        // コールバックが受け取ったサイズ
        std::pair<std::uint32_t, std::uint32_t> size{ 640, 480 };
        // サイズ変更コールバック呼び出し数
        int calls{};
        scene.SetWindowSizeCallbacks(
            // 幅と高さを記録して変更成功を返す
            // onResize(width: 幅, height: 高さ)
            [&](const std::uint32_t width,
                const std::uint32_t height)
            {
                size = { width, height };
                ++calls;
                return true;
            },
            // 現在のサイズを返す
            [&] { return size; });
        // 現在のサイズをスクリプトから読めることを確認する
        Require(script->Size() == size,
            "Scripts must see the current game view size.");
        // 無効なサイズ要求を検証する
        Require(!script->Resize(0, 600)
                && !script->Resize(16385, 600)
                && calls == 0,
            "Invalid sizes must not reach the window handler.");
        // 有効なサイズ要求を検証する
        Require(script->Resize(1080, 1920)
                && script->Size()
                    == std::pair<std::uint32_t, std::uint32_t>{
                        1080, 1920 }
                && calls == 1,
            "Scripts must be able to change the game view size.");
        // テスト用スクリプトを破棄する
        Bridge::Destroy(script);
    }

    // 作成前のウィンドウサイズ設定を検証する
    void CheckWindowSizeBeforeCreation()
    {
        // 初期サイズを確認するアプリケーション
        LamaPon::Application application(L"WindowSize", 1280, 720);
        Require(application.WindowSize()
                == std::pair<std::uint32_t, std::uint32_t>{
                    1280, 720 },
            "The initial client size must be available before creation.");
        Require(!application.SetWindowSize(0, 720)
                && !application.SetWindowSize(16385, 720),
            "Invalid client sizes must be rejected.");
        Require(application.SetWindowSize(960, 540)
                && application.WindowSize()
                    == std::pair<std::uint32_t, std::uint32_t>{
                        960, 540 },
            "A client size can be configured before creation.");
    }

    // シーン初期化時の環境復元を検証する
    void CheckSceneReset()
    {
        // テスト用描画デバイス
        LamaPon::GraphicsDevice graphics;
        // 初期化動作を検証するシーン
        LamaPon::Scene scene(graphics);
        // 初期環境設定のJSON
        const auto defaults = scene.SerializeToJson();
        // 環境設定を初期状態から変更する
        const auto changeSettings = [&]
        {
            // 時間方向のアンチエイリアス設定
            auto taa = scene.TemporalAntiAliasing();
            taa.enabled = !taa.enabled;
            scene.SetTemporalAntiAliasingSettings(taa);
            // スクリーンスペース反射設定
            auto ssr = scene.ScreenSpaceReflection();
            ssr.enabled = !ssr.enabled;
            scene.SetScreenSpaceReflectionSettings(ssr);
            // ボリューメトリックライト設定
            auto volume = scene.VolumetricLight();
            volume.enabled = !volume.enabled;
            scene.SetVolumetricLightSettings(volume);
        };
        // 設定変更後にシーンを消去する
        changeSettings();
        scene.Clear();
        Require(scene.SerializeToJson() == defaults,
            "Clear must restore the complete default environment.");
        // 設定変更後に既定値なしシーンを読み込む
        changeSettings();
        // 古いシーンや最小シーンには環境設定がありません。
        scene.LoadFromJson(R"({"format":"LamaPonScene","objects":[]})");
        Require(scene.SerializeToJson() == defaults,
            "Loading a scene without environment must restore defaults.");
    }

    // スクリプト破棄失敗時の後始末を検証する
    void CheckScriptCleanup()
    {
        // テスト用描画デバイス
        LamaPon::GraphicsDevice graphics;
        // 破棄失敗を試すシーン
        LamaPon::Scene scene(graphics);
        // 破棄対象のゲームオブジェクト
        auto& object = scene.CreateGameObject("Cleanup");
        // テスト用スクリプトブリッジ
        using Bridge = LamaPon::Detail::ScriptBridge<ThrowingDestroyScript>;
        // ブリッジが生成したスクリプト
        auto* instance = Bridge::Create(&object, &graphics, "{}");
        Require(scene.Events().SubscriptionCount() == 1,
            "Script must subscribe before destruction.");
        // 破棄時の例外状態
        bool threw{};
        // 破棄処理の例外を捕捉する
        try
        {
            Bridge::Destroy(instance);
        }
        // 想定した破棄例外を記録する
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        Require(threw && ThrowingDestroyScript::destroyed == 1,
            "OnDestroy failure must propagate after destroying the instance.");
        Require(scene.Events().SubscriptionCount() == 0,
            "OnDestroy failure must not leave subscriptions behind.");
    }

    // 入れ子イベント失敗後の復旧を検証する
    void CheckEventRecovery()
    {
        // 復旧処理を試すイベントバス
        LamaPon::EventBus events;
        // 外側イベントの購読ID
        std::uint64_t outer{};
        // 外側イベント中に内側イベントを送る
        // outerHandler(args: 外側イベント引数)
        outer = events.Subscribe("outer", [&](const LamaPon::EventArgs&)
        {
            events.Unsubscribe(outer);
            events.Publish("inner");
        });
        // 内側イベントで購読解除後の例外を起こす
        // innerHandler(args: 内側イベント引数)
        events.Subscribe("inner", [&](const LamaPon::EventArgs&)
        {
            events.Clear();
            throw std::runtime_error("expected nested event failure");
        });
        // 入れ子イベント失敗の状態
        bool threw{};
        // イベント送信時の例外を捕捉する
        try
        {
            events.Publish("outer");
        }
        // 想定した送信例外を記録する
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        Require(threw && events.SubscriptionCount() == 0,
            "Nested publish failure must honor Clear and propagate.");
        // 外側イベントの配信回数
        int delivered{};
        // 再登録した購読ID
        const auto handle = events.Subscribe("outer",
            [&](const LamaPon::EventArgs&) { ++delivered; });
        events.Publish("outer");
        events.Unsubscribe(handle);
        events.Publish("outer");
        Require(delivered == 1 && events.SubscriptionCount() == 0,
            "EventBus must support later subscribe/publish/unsubscribe.");
    }
}

// ライフサイクル時の復旧動作を検証する
int main()
{
    // テスト失敗を終了コードへ変換する
    try
    {
        CheckSceneReset();
        CheckWindowSizeBridge();
        CheckWindowSizeBeforeCreation();
        CheckScriptCleanup();
        CheckEventRecovery();
        std::cout << "Lifecycle recovery tests passed.\n";
        // テスト成功を返す
        return 0;
    }
    // 例外内容を出力して失敗終了する
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        // テスト失敗を返す
        return 1;
    }
}
