#include "ScriptRegistry.h"

#include "LamaPon/Scripting/GameModule.h"

#include <vector>

static_assert(
    LamaPon::GameModuleApiVersion == @LAMAPON_GAME_MODULE_API_VERSION@,
    "The Game Module SDK does not match the Runtime API version. "
    "Rebuild and reinstall LamaPon before building the game module.");

// DLLが保持する登録情報を返し、初回呼出し後はDataAsset登録を追加しない。
LAMAPON_GAME_MODULE_EXPORT
{
    // 初回に登録一覧を固定するScript記述子列
    static const std::vector<LamaPon::NativeScriptTypeDescriptor>
        registeredComponents = []
        {
            // 初回に複製する登録済みScript一覧
            const auto& scripts =
                LamaPon::GameModuleScripts::RegisteredScripts();
            return std::vector<LamaPon::NativeScriptTypeDescriptor>{
                scripts.begin(),
                scripts.end()
            };
        }();
    // Descriptorが借用するDataAsset一覧
    const auto& registeredDataAssets =
        LamaPon::GameModuleDataAssets::RegisteredDataAssets();
    // DLLの寿命まで保持するモジュール記述子
    static const LamaPon::GameModuleDescriptor module{
        @LAMAPON_GAME_MODULE_API_VERSION@,
        "LamaPon Project Game Module",
        registeredComponents.size(),
        registeredComponents.data(),
        registeredDataAssets.size(),
        registeredDataAssets.data()
    };
    return &module;
}
