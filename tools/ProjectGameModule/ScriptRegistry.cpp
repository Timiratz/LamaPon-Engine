#include "ScriptRegistry.h"

#include <utility>

namespace
{
    std::vector<LamaPon::NativeScriptTypeDescriptor>& ScriptStorage()
    {
        static std::vector<LamaPon::NativeScriptTypeDescriptor> scripts;
        return scripts;
    }

    std::vector<LamaPon::NativeDataAssetTypeDescriptor>&
        DataAssetStorage()
    {
        static std::vector<LamaPon::NativeDataAssetTypeDescriptor>
            dataAssets;
        return dataAssets;
    }
}

namespace LamaPon::GameModuleScripts
{
    void Register(NativeScriptTypeDescriptor descriptor)
    {
        ScriptStorage().push_back(std::move(descriptor));
    }

    const std::vector<NativeScriptTypeDescriptor>&
        RegisteredScripts() noexcept
    {
        return ScriptStorage();
    }

    AutoRegister::AutoRegister(NativeScriptTypeDescriptor descriptor)
    {
        Register(std::move(descriptor));
    }
}

namespace LamaPon::GameModuleDataAssets
{
    void Register(NativeDataAssetTypeDescriptor descriptor)
    {
        DataAssetStorage().push_back(std::move(descriptor));
    }

    const std::vector<NativeDataAssetTypeDescriptor>&
        RegisteredDataAssets() noexcept
    {
        return DataAssetStorage();
    }

    AutoRegister::AutoRegister(
        NativeDataAssetTypeDescriptor descriptor)
    {
        Register(std::move(descriptor));
    }
}
