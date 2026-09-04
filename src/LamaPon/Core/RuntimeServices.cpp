#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Input/InputSystem.h"

#include <stdexcept>
#include <utility>

namespace LamaPon
{
    RuntimeServices::RuntimeServices() = default;
    RuntimeServices::~RuntimeServices() = default;

    void RuntimeServices::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
        const HWND window, const bool textureCompression)
    {
        if (m_audio || m_input)
        {
            throw std::logic_error("Runtime services are already initialized; call Shutdown first.");
        }
        auto assets = std::make_unique<AssetManager>(device, context);
        assets->SetRuntimeTextureCompressionEnabled(textureCompression);
        auto audio = std::make_unique<AudioSystem>();
        auto input = std::make_unique<InputSystem>(window);

        Shutdown();
        m_assets = std::move(assets);
        m_audio = std::move(audio);
        m_input = std::move(input);
    }

    void RuntimeServices::Shutdown() noexcept
    {
        // 入力と再生を終了してから、それらが参照するアセットを解放します。
        // 複数回呼べるため、明示終了後のデストラクタとも共存できます。
        m_input.reset();
        m_audio.reset();
        m_assets.reset();
    }

    AssetManager& RuntimeServices::EnsureAssets(ID3D11Device* device,
        ID3D11DeviceContext* context, const bool textureCompression)
    {
        if (!m_assets)
        {
            auto assets = std::make_unique<AssetManager>(device, context);
            assets->SetRuntimeTextureCompressionEnabled(textureCompression);
            m_assets = std::move(assets);
        }
        return *m_assets;
    }

    AudioSystem& RuntimeServices::Audio() const
    {
        if (!m_audio) throw std::logic_error("Audio system is not initialized.");
        return *m_audio;
    }

    InputSystem& RuntimeServices::Input() const
    {
        if (!m_input) throw std::logic_error("Input system is not initialized.");
        return *m_input;
    }
}
