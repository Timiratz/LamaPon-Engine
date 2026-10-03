#include "LamaPon/Graphics/GraphicsBackendPackage.h"

#include <Windows.h>


// バックエンドパッケージのABI版を返す。
extern "C" __declspec(dllexport) std::uint32_t
    LamaPonGraphicsBackendAbiVersion() noexcept
{
    return LamaPon::GraphicsBackendPackageAbiVersion;
}

// パッケージが提供する描画APIの識別名を返す。
extern "C" __declspec(dllexport) const char*
    LamaPonGraphicsBackendApi() noexcept
{
    return "DirectX12Experimental";
}

// 常に成功を返すDLL入口で、無名の引数は使用しない。
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;
}
