#include "ScriptRegistry.h"

#include <utility>

namespace
{
    // DLL内で共有するScript登録の保存領域を返す。
    std::vector<LamaPon::NativeScriptTypeDescriptor>& ScriptStorage()
    {
        // DLLの寿命まで保持するScript記述子列
        static std::vector<LamaPon::NativeScriptTypeDescriptor> scripts;
        return scripts;
    }

    // DLL内で共有するDataAsset登録の保存領域を返す。
    std::vector<LamaPon::NativeDataAssetTypeDescriptor>&
        DataAssetStorage()
    {
        // DLLの寿命まで保持するDataAsset記述子列
        static std::vector<LamaPon::NativeDataAssetTypeDescriptor>
            dataAssets;
        return dataAssets;
    }
}

namespace LamaPon::GameModuleScripts
{
    // Scriptの記述子を登録順に保存する(descriptor: 追加するScript登録情報)。
    void Register(NativeScriptTypeDescriptor descriptor)
    {
        ScriptStorage().push_back(std::move(descriptor));
    }

    // Scriptの登録一覧を借用で返し、要素への参照は後の追加で無効になり得る。
    const std::vector<NativeScriptTypeDescriptor>&
        RegisteredScripts() noexcept
    {
        return ScriptStorage();
    }

    // 静的初期化時にScript記述子を登録する(descriptor: 追加するScript登録情報)。
    AutoRegister::AutoRegister(NativeScriptTypeDescriptor descriptor)
    {
        Register(std::move(descriptor));
    }
}

namespace LamaPon::GameModuleDataAssets
{
    // DataAssetの記述子を登録順に保存する(descriptor: 追加するDataAsset情報)。
    void Register(NativeDataAssetTypeDescriptor descriptor)
    {
        DataAssetStorage().push_back(std::move(descriptor));
    }

    // DataAssetの登録一覧を借用で返し、要素への参照は後の追加で無効になり得る。
    const std::vector<NativeDataAssetTypeDescriptor>&
        RegisteredDataAssets() noexcept
    {
        return DataAssetStorage();
    }

    // 静的初期化時にDataAsset記述子を登録する(descriptor: 追加するDataAsset情報)。
    AutoRegister::AutoRegister(
        NativeDataAssetTypeDescriptor descriptor)
    {
        Register(std::move(descriptor));
    }
}
