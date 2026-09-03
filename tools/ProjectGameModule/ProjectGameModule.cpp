#include "ScriptRegistry.h"

#include "LamaPon/Scripting/GameModule.h"

#include <vector>

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
        LamaPon::GameModuleApiVersion,
        "LamaPon Project Game Module",
        registeredComponents.size(),
        registeredComponents.data(),
        registeredDataAssets.size(),
        registeredDataAssets.data()
    };
    return &module;
}
