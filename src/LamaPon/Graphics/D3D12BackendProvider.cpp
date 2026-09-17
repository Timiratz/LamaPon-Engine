#include "LamaPon/Graphics/GraphicsBackendPackage.h"

#include <Windows.h>

// D3D12実装を物理DLLへ移す途中の安定したABI境界です。現段階では
// capabilityとABIだけを提供し、描画処理はLamaPonRuntime内の実装へ
// 委譲します。後続段階で同じentry pointへBackend factoryを追加します。
extern "C" __declspec(dllexport) std::uint32_t
    LamaPonGraphicsBackendAbiVersion() noexcept
{
    return LamaPon::GraphicsBackendPackageAbiVersion;
}

extern "C" __declspec(dllexport) const char*
    LamaPonGraphicsBackendApi() noexcept
{
    return "DirectX12Experimental";
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
