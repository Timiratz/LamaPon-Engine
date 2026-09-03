#include "ScriptRegistry.h"

#include "LamaPon/LamaPon.h"

namespace
{
    void* Create(
        LamaPon::GameObject*,
        LamaPon::GraphicsDevice*,
        const char*)
    {
        return new int{ 42 };
    }

    void Destroy(void* instance)
    {
        delete static_cast<int*>(instance);
    }

    const LamaPon::GameModuleScripts::AutoRegister Registration{
        LamaPon::NativeScriptTypeDescriptor{
            "Test.ExternalScript",
            "External Script",
            &Create,
            &Destroy
        }
    };
}
