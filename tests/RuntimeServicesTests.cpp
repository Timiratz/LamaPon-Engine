#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Input/InputSystem.h"

#include <iostream>
#include <stdexcept>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void CheckUnavailable(const LamaPon::RuntimeServices& services)
    {
        bool audioRejected{}, inputRejected{};
        try { static_cast<void>(services.Audio()); }
        catch (const std::logic_error&) { audioRejected = true; }
        try { static_cast<void>(services.Input()); }
        catch (const std::logic_error&) { inputRejected = true; }
        Require(audioRejected && inputRejected, "Inactive services must reject access");
    }
}

int main()
{
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result{};
    try
    {
        LamaPon::RuntimeServices services;
        CheckUnavailable(services);
        Require(services.TryAssets() == nullptr, "Services must start empty");
        auto& assets = services.EnsureAssets(nullptr, nullptr, false);
        Require(&assets == &services.EnsureAssets(nullptr, nullptr, false),
            "File-only asset access must reuse its cache");
        Require(!assets.ReadFileBytes("assets/shaders/LamaPonLit.hlsl").empty(),
            "File-only service must read engine assets without a GraphicsDevice");
        // 初期化前のファイル読み込みから通常サービスへ移行し、再開も確認。
        for (int cycle = 0; cycle < 2; ++cycle)
        {
            services.Initialize(nullptr, nullptr, nullptr, true);
            static_cast<void>(services.Audio());
            static_cast<void>(services.Input());
            Require(services.TryAssets() != nullptr, "Initialization must create assets");
            auto* input = &services.Input();
            bool repeatedRejected{};
            try { services.Initialize(nullptr, nullptr, nullptr, true); }
            catch (const std::logic_error&) { repeatedRejected = true; }
            Require(repeatedRejected && input == &services.Input(),
                "Repeated initialization must retain the active input owner");
            services.Shutdown();
            services.Shutdown();
            Require(services.TryAssets() == nullptr, "Shutdown must release assets");
            CheckUnavailable(services);
        }
        Require(!services.EnsureAssets(nullptr, nullptr, false).ReadFileBytes("assets/shaders/LamaPonLit.hlsl").empty(),
            "File-only access must recover after shutdown");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
