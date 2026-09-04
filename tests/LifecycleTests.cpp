#include "LamaPon/LamaPon.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class ThrowingDestroyScript final : public LamaPon::Script
    {
    public:
        static inline int destroyed{};
        void Awake() override
        {
            On("cleanup-test", [] {});
        }
        void OnDestroy() override
        {
            throw std::runtime_error("expected destroy failure");
        }
        ~ThrowingDestroyScript() override
        {
            ++destroyed;
        }
    };

    void CheckSceneReset()
    {
        LamaPon::GraphicsDevice graphics;
        LamaPon::Scene scene(graphics);
        const auto defaults = scene.SerializeToJson();
        const auto changeSettings = [&]
        {
            auto taa = scene.TemporalAntiAliasing();
            taa.enabled = !taa.enabled;
            scene.SetTemporalAntiAliasingSettings(taa);
            auto ssr = scene.ScreenSpaceReflection();
            ssr.enabled = !ssr.enabled;
            scene.SetScreenSpaceReflectionSettings(ssr);
            auto volume = scene.VolumetricLight();
            volume.enabled = !volume.enabled;
            scene.SetVolumetricLightSettings(volume);
        };
        changeSettings();
        scene.Clear();
        Require(scene.SerializeToJson() == defaults,
            "Clear must restore the complete default environment.");
        changeSettings();
        // 古いシーンや最小シーンには環境設定がありません。
        scene.LoadFromJson(R"({"format":"LamaPonScene","objects":[]})");
        Require(scene.SerializeToJson() == defaults,
            "Loading a scene without environment must restore defaults.");
    }

    void CheckScriptCleanup()
    {
        LamaPon::GraphicsDevice graphics;
        LamaPon::Scene scene(graphics);
        auto& object = scene.CreateGameObject("Cleanup");
        using Bridge = LamaPon::Detail::ScriptBridge<ThrowingDestroyScript>;
        auto* instance = Bridge::Create(&object, &graphics, "{}");
        Require(scene.Events().SubscriptionCount() == 1,
            "Script must subscribe before destruction.");
        bool threw{};
        try
        {
            Bridge::Destroy(instance);
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        Require(threw && ThrowingDestroyScript::destroyed == 1,
            "OnDestroy failure must propagate after destroying the instance.");
        Require(scene.Events().SubscriptionCount() == 0,
            "OnDestroy failure must not leave subscriptions behind.");
    }

    void CheckEventRecovery()
    {
        LamaPon::EventBus events;
        std::uint64_t outer{};
        outer = events.Subscribe("outer", [&](const LamaPon::EventArgs&)
        {
            events.Unsubscribe(outer);
            events.Publish("inner");
        });
        events.Subscribe("inner", [&](const LamaPon::EventArgs&)
        {
            events.Clear();
            throw std::runtime_error("expected nested event failure");
        });
        bool threw{};
        try
        {
            events.Publish("outer");
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        Require(threw && events.SubscriptionCount() == 0,
            "Nested publish failure must honor Clear and propagate.");
        int delivered{};
        const auto handle = events.Subscribe("outer",
            [&](const LamaPon::EventArgs&) { ++delivered; });
        events.Publish("outer");
        events.Unsubscribe(handle);
        events.Publish("outer");
        Require(delivered == 1 && events.SubscriptionCount() == 0,
            "EventBus must support later subscribe/publish/unsubscribe.");
    }
}

int main()
{
    try
    {
        CheckSceneReset();
        CheckScriptCleanup();
        CheckEventRecovery();
        std::cout << "Lifecycle recovery tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
