#include "LamaPon/LamaPon.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

int main()
{
    try
    {
        // シャドウコピーの置き場所: ローカルのモジュールは従来どおり隣、
        // ネットワーク(UNC/WebDAV)上のモジュールはユーザーローカルへ退避する。
        // 共有側へ作ろうとするとWebDAVでディレクトリ作成やLoadLibraryが
        // 失敗し、エディター再生でC++ Scriptが黙って動かなくなるため。
        const auto localShadow =
            LamaPon::GameModuleHost::HotReloadDirectoryFor(
                L"C:/Games/Sample/.lamapon/bin/LamaPonGameModule.dll");
        Require(
            localShadow.filename() == L".lamapon-hot-reload"
                && localShadow.parent_path()
                    == std::filesystem::path(
                        L"C:/Games/Sample/.lamapon/bin"),
            "Local Game Modules should shadow-copy next to the module.");
        const auto remoteShadow =
            LamaPon::GameModuleHost::HotReloadDirectoryFor(
                LR"(\\server\share\Game\.lamapon\bin\LamaPonGameModule.dll)");
        Require(
            remoteShadow.native().find(L"\\\\server") == std::wstring::npos
                && remoteShadow.native().find(L"HotReload")
                    != std::wstring::npos,
            "Network Game Modules must shadow-copy into the local cache.");

        LamaPon::GameModuleHost host;
        Require(
            host.Load(
                std::filesystem::current_path()
                / "LamaPonGameModule.dll"),
            host.LastError().c_str());
        Require(
            host.IsLoaded(),
            "Game Module was not loaded.");
        Require(
            host.ModuleName()
                == "LamaPon Sample Game",
            "Unexpected Game Module name.");
        // 登録数と並び順を固定すると、assets配下へ公式パッケージや
        // ユーザーのスクリプトを1つ足すだけでこのテストが壊れます
        // （CMakeがassets/*.cppをGame Moduleへ含めるため、実際に
        // Easing & Tweenパッケージの追加で壊れました）。
        // サンプルが持つ1種と、ScriptRegistry経由で足される
        // TargetRange.Gameが引けることだけを確認します。
        for (const char* expected : {
            "Sample.FloatingAccent",
            "TargetRange.Game" })
        {
            Require(
                host.FindComponent(expected) != nullptr,
                "Native components were not registered.");
        }
        const auto* floatingDescriptor =
            host.FindComponent("Sample.FloatingAccent");
        Require(
            floatingDescriptor != nullptr
                && floatingDescriptor->propertiesSchemaJson != nullptr,
            "Native component Inspector schema was not registered.");
        const auto inspectorSchema = nlohmann::json::parse(
            floatingDescriptor->propertiesSchemaJson);
        Require(
            inspectorSchema.at("fields").is_array()
                && inspectorSchema.at("fields").size() == 2
                && inspectorSchema.at("fields").at(0).at("name")
                    == "amplitude",
            "Native component Inspector schema was invalid.");

        LamaPon::GraphicsDevice graphics;
        LamaPon::Scene scene(graphics);
        auto& object =
            scene.CreateGameObject("Module Test");
        auto& script =
            object.AddComponent<
                LamaPon::NativeScriptComponent>(
                    "Sample.FloatingAccent",
                    R"({"amplitude":0.5,"frequency":2.0})");
        scene.Update(0.125f);
        Require(
            object.GetTransform().position.y > 0.49f
                && object.GetTransform().EulerAngles().y > 0.09f,
            "Native component update was not invoked.");

        const auto serialized =
            nlohmann::json::parse(
                scene.SerializeToJson());
        const auto& component =
            serialized.at("objects").at(0)
                .at("components").at(0);
        Require(
            component.at("type")
                    .get<std::string>()
                == "NativeScript"
                && component.at("script")
                    .get<std::string>()
                    == "Sample.FloatingAccent"
                && component.at("properties")
                    .at("amplitude").get<float>()
                    == 0.5f
                && component.at("properties")
                    .at("fixedTicks").get<int>()
                    == 6,
            "Native component did not serialize.");

        LamaPon::Scene loaded(graphics);
        loaded.LoadFromJson(serialized.dump());
        const auto* loadedScript =
            loaded.GameObjects().at(0)
                ->GetComponent<
                    LamaPon::NativeScriptComponent>();
        Require(
            loadedScript != nullptr
                && loadedScript->IsResolved()
                && loadedScript->ScriptType()
                    == script.ScriptType(),
            "Native component did not round-trip.");

        // GetScript<T>()で自作インターフェースを引けること。
        // 実際の呼び出しはGame Module内のDamageDealerProbeが行い、
        // GetScript<IDamageable>()で相手を見つけて25ダメージ与えます。
        // DamageableProbeはScriptを「先頭以外」に継承しているため、
        // void*から素にstatic_cast<Script*>する実装だとポインタ調整が
        // 入らず壊れます。descriptorのasScript経由になっているかの検査
        // でもあります。
        {
            LamaPon::Scene interfaceScene(graphics);
            auto& probeObject =
                interfaceScene.CreateGameObject("Interface Probe");
            auto& damageable =
                probeObject.AddComponent<
                    LamaPon::NativeScriptComponent>(
                        "Sample.DamageableProbe");
            auto& dealer =
                probeObject.AddComponent<
                    LamaPon::NativeScriptComponent>(
                        "Sample.DamageDealerProbe");
            interfaceScene.Update(0.016f);

            const auto dealerState = nlohmann::json::parse(
                dealer.SerializedProperties());
            Require(
                dealerState.value("dealt", false),
                "GetScript<T>() did not find the interface.");

            const auto health = nlohmann::json::parse(
                damageable.SerializedProperties());
            Require(
                health.value("health", 0) == 75,
                "Damage through the interface was not applied.");
        }

        Require(
            host.Reload(),
            host.LastError().c_str());
        Require(
            host.FindComponent(
                "Sample.FloatingAccent") != nullptr,
            "Component registration was lost after reload.");
        const float heightBeforeReloadedUpdate =
            object.GetTransform().position.y;
        scene.Update(0.05f);
        Require(
            object.GetTransform().position.y
                != heightBeforeReloadedUpdate,
            "Active Native component was not restored after reload.");

        const auto originalModulePath = host.ModulePath();
        const auto missingModulePath =
            std::filesystem::current_path()
            / "missing-project-module.dll";
        Require(
            !host.Load(missingModulePath)
                && !host.IsLoaded()
                && host.ModulePath() == missingModulePath
                && !script.IsResolved(),
            "Switching to a missing project module did not unload the previous module.");
        Require(
            host.Load(originalModulePath)
                && host.IsLoaded()
                && script.IsResolved(),
            "Native components were not restored after switching modules.");

        std::cout
            << "Game Module tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
