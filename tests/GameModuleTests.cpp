#include "LamaPon/LamaPon.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace
{
    // 条件不成立ならテストを失敗させます。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(
        const bool condition,
        const char* message)
    {
        // assertion失敗を例外で通知
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

#ifdef NDEBUG
    // Release moduleにbuild machineのPDB pathが含まれないことを確認します。
    // RequireSafePdbReference(modulePath: 検査するGame Module DLL)
    void RequireSafePdbReference(
        const std::filesystem::path& modulePath)
    {
        // バイナリ形式を保って検査するmodule入力
        std::ifstream input(modulePath, std::ios::binary);
        Require(input.good(), "Could not inspect the Game Module.");
        // RSDS markerを検索するDLL bytes
        const std::string bytes(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>{});
        // CodeView PDB参照marker位置
        const auto marker = bytes.find("RSDS");
        // markerがないDLLはsymbols無効として扱う
        if (marker == std::string::npos)
        {
            // Release symbols may be disabled.
            return;
        }
        // RSDS固定部24 bytesの後がPDB path
        const auto pathBegin = marker + 24;
        // PDB path終端のNUL位置
        const auto pathEnd = bytes.find('\0', pathBegin);
        Require(
            pathEnd != std::string::npos
                && bytes.substr(pathBegin, pathEnd - pathBegin)
                    == "LamaPonGameModule.pdb",
            "Release Game Module leaked its build-machine PDB path.");
    }
#endif
}

// Game Moduleのshadow copy・registration・reloadを検証します。
int main()
{
    // テスト例外を失敗終了コードへ変換
    try
    {
        // UNC/WebDAV moduleはlocal cacheへ退避する
        const auto localShadow =
            LamaPon::GameModuleHost::HotReloadDirectoryFor(
                L"C:/Games/Sample/.lamapon/bin/LamaPonGameModule.dll");
        Require(
            localShadow.filename() == L".lamapon-hot-reload"
                && localShadow.parent_path()
                    == std::filesystem::path(
                        L"C:/Games/Sample/.lamapon/bin"),
            "Local Game Modules should shadow-copy next to the module.");
        // remote moduleを退避するユーザーlocal cache
        const auto remoteShadow =
            LamaPon::GameModuleHost::HotReloadDirectoryFor(
                LR"(\\server\share\Game\.lamapon\bin\LamaPonGameModule.dll)");
        Require(
            remoteShadow.native().find(L"\\\\server") == std::wstring::npos
                && remoteShadow.native().find(L"HotReload")
                    != std::wstring::npos,
            "Network Game Modules must shadow-copy into the local cache.");

        // load・reload対象のGame Module host
        LamaPon::GameModuleHost host;
#ifdef NDEBUG
        // Release PDB参照の検査結果
        RequireSafePdbReference(
            std::filesystem::current_path()
                / "LamaPonGameModule.dll");
#endif
        // Game Module DLLのload結果
        const bool moduleLoaded = host.Load(
            std::filesystem::current_path() / "LamaPonGameModule.dll");
        Require(moduleLoaded, host.LastError().c_str());
        Require(
            host.IsLoaded(),
            "Game Module was not loaded.");
        Require(
            host.ModuleName()
                == "LamaPon Sample Game",
            "Unexpected Game Module name.");
        // 必須のNativeScript登録だけを確認
        // expected: load後に見つかるべきcomponent名
        for (const char* expected : {
            "Sample.FloatingAccent",
            "TargetRange.Game" })
        {
            Require(
                host.FindComponent(expected) != nullptr,
                "Native components were not registered.");
        }
        // Inspector schemaを公開するFloatingAccent定義
        const auto* floatingDescriptor =
            host.FindComponent("Sample.FloatingAccent");
        Require(
            floatingDescriptor != nullptr
                && floatingDescriptor->propertiesSchemaJson != nullptr,
            "Native component Inspector schema was not registered.");
        // Inspectorへ公開されたcomponent schema
        const auto inspectorSchema = nlohmann::json::parse(
            floatingDescriptor->propertiesSchemaJson);
        Require(
            inspectorSchema.at("fields").is_array()
                && inspectorSchema.at("fields").size() == 2
                && inspectorSchema.at("fields").at(0).at("name")
                    == "amplitude",
            "Native component Inspector schema was invalid.");

        {
            LamaPon::GraphicsDevice liveGraphics;
            LamaPon::Scene liveScene(liveGraphics);
            auto& liveObject = liveScene.CreateGameObject("Live properties");
            auto& live = liveObject.AddComponent<LamaPon::NativeScriptComponent>(
                "Sample.LivePropertyProbe", R"({"speed":1.0})");
            liveScene.Update(0.25f);
            auto* instance = live.ScriptInstance();
            live.ApplyPropertiesJsonLive(R"({"speed":2.0})");
            Require(live.ScriptInstance() == instance, "Live editing recreated the script instance.");
            liveScene.Update(0.25f);
            const auto values = nlohmann::json::parse(live.SerializedProperties());
            Require(values.at("phase").get<float>() == 0.75f && values.at("starts").get<int>() == 1,
                "Live editing reset state, repeated Start, or failed to update the value.");
            bool rejected{};
            try { live.ApplyPropertiesJsonLive(R"({"speed":"invalid"})"); }
            catch (const std::exception&) { rejected = true; }
            Require(rejected && live.ScriptInstance() == instance
                && nlohmann::json::parse(live.PropertiesJson()).at("speed") == 2.0,
                "Invalid live properties changed the committed values or the instance.");
            auto& legacyObject = liveScene.CreateGameObject("Not opted in");
            auto& legacy = legacyObject.AddComponent<LamaPon::NativeScriptComponent>(
                "Sample.FloatingAccent", "{}");
            liveScene.Update(0.01f);
            rejected = false;
            try { legacy.ApplyPropertiesJsonLive("{}"); }
            catch (const std::exception&) { rejected = true; }
            Require(rejected, "Live editing was enabled for a script without opt-in.");
        }

        // NativeScriptの実行先となるgraphics device
        LamaPon::GraphicsDevice graphics;
        // Module Test scene
        LamaPon::Scene scene(graphics);
        // NativeScriptを持たせるgame object
        auto& object =
            scene.CreateGameObject("Module Test");
        // updateとserialize対象のNativeScript component
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

        // componentを含むscene JSON
        const auto serialized =
            nlohmann::json::parse(
                scene.SerializeToJson());
        // NativeScript componentの保存JSON
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

        // JSONから復元したscene
        LamaPon::Scene loaded(graphics);
        loaded.LoadFromJson(serialized.dump());
        // 復元sceneのNativeScript component
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

        // GetScript<T>がasScript経由でbase offsetを保つことを確認
        {
            // interface lookupを行うModule probe scene
            LamaPon::Scene interfaceScene(graphics);
            // 二つのNativeScriptを置くgame object
            auto& probeObject =
                interfaceScene.CreateGameObject("Interface Probe");
            // IDamageableとして解決するscript
            auto& damageable =
                probeObject.AddComponent<
                    LamaPon::NativeScriptComponent>(
                        "Sample.DamageableProbe");
            // interfaceを通じてdamageを与えるscript
            auto& dealer =
                probeObject.AddComponent<
                    LamaPon::NativeScriptComponent>(
                        "Sample.DamageDealerProbe");
            interfaceScene.Update(0.016f);

            // dealerのinterface call状態
            const auto dealerState = nlohmann::json::parse(
                dealer.SerializedProperties());
            Require(
                dealerState.value("dealt", false),
                "GetScript<T>() did not find the interface.");

            // damage適用後の対象health
            const auto health = nlohmann::json::parse(
                damageable.SerializedProperties());
            Require(
                health.value("health", 0) == 75,
                "Damage through the interface was not applied.");
        }

        // Module DLLのreload結果
        const bool reloaded = host.Reload();
        Require(reloaded, host.LastError().c_str());
        Require(
            host.FindComponent(
                "Sample.FloatingAccent") != nullptr,
            "Component registration was lost after reload.");
        // reload直後のgame object高さ
        const float heightBeforeReloadedUpdate =
            object.GetTransform().position.y;
        scene.Update(0.05f);
        Require(
            object.GetTransform().position.y
                != heightBeforeReloadedUpdate,
            "Active Native component was not restored after reload.");

        // reload前に読み込んだDLLのpath
        const auto originalModulePath = host.ModulePath();
        // 存在しないproject moduleのpath
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
    // テスト例外を標準エラーと失敗終了コードへ変換
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
