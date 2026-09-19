#include "ScriptRegistry.h"

#include "LamaPon/Scripting/GameModule.h"

#include <vector>

static_assert(
    LamaPon::GameModuleApiVersion == @LAMAPON_GAME_MODULE_API_VERSION@,
    "The Game Module SDK does not match the Runtime API version. "
    "Rebuild and reinstall LamaPon before building the game module.");

LAMAPON_GAME_MODULE_EXPORT
{
    static const std::vector<LamaPon::NativeScriptTypeDescriptor>
        registeredComponents = []
        {
            const auto& scripts =
                LamaPon::GameModuleScripts::RegisteredScripts();
            return std::vector<LamaPon::NativeScriptTypeDescriptor>{
                scripts.begin(),
                scripts.end()
            };
        }();
    const auto& registeredDataAssets =
        LamaPon::GameModuleDataAssets::RegisteredDataAssets();
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
