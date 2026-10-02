#include "ScriptRegistry.h"

#include "LamaPon/LamaPon.h"

namespace
{
    // 登録した外部Scriptの生成確認用に整数42を確保する。
    void* Create(
        LamaPon::GameObject*,
        LamaPon::GraphicsDevice*,
        const char*)
    {
        return new int{ 42 };
    }

    // Createで確保した検査用の整数を解放する(instance: Createが返した整数ポインター)。
    void Destroy(void* instance)
    {
        delete static_cast<int*>(instance);
    }

    // 外部Scriptの自動登録を保持する静的変数
    const LamaPon::GameModuleScripts::AutoRegister Registration{
        LamaPon::NativeScriptTypeDescriptor{
            "Test.ExternalScript",
            "External Script",
            &Create,
            &Destroy
        }
    };
}
