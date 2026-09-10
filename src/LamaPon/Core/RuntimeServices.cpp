#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Input/InputSystem.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace LamaPon
{
    struct RuntimeServices::ReinitializationState final
    {
        std::optional<std::filesystem::path> assetRoot;
        std::optional<std::size_t> progressiveUploadThreshold;
        std::optional<std::size_t> textCacheBudgetBytes;
        std::optional<std::vector<InputActionDefinition>> inputActions;
    };

    RuntimeServices::RuntimeServices()
        : m_reinitializationState(
            std::make_unique<ReinitializationState>())
    {
    }
    RuntimeServices::~RuntimeServices() = default;

    void RuntimeServices::CaptureAssetSettings() noexcept
    {
        if (!m_assets || !m_reinitializationState)
        {
            return;
        }
        try
        {
            const auto& root = m_assets->AssetRoot();
            if (root.empty())
            {
                m_reinitializationState->assetRoot.reset();
            }
            else
            {
                m_reinitializationState->assetRoot = root;
            }
            m_reinitializationState->progressiveUploadThreshold =
                m_assets->ProgressiveUploadThreshold();
            m_reinitializationState->textCacheBudgetBytes =
                m_assets->TextCacheBudgetBytes();
        }
        catch (...)
        {
            // A failed snapshot must not make noexcept teardown fail. Values
            // captured by an earlier successful transition remain available.
        }
    }

    void RuntimeServices::RestoreAssetSettings(
        AssetManager& assets) const
    {
        if (!m_reinitializationState)
        {
            return;
        }
        if (m_reinitializationState->assetRoot)
        {
            assets.SetAssetRoot(
                *m_reinitializationState->assetRoot);
        }
        if (m_reinitializationState->progressiveUploadThreshold)
        {
            assets.SetProgressiveUploadThreshold(
                *m_reinitializationState
                    ->progressiveUploadThreshold);
        }
        if (m_reinitializationState->textCacheBudgetBytes)
        {
            assets.SetTextCacheBudgetBytes(
                *m_reinitializationState
                    ->textCacheBudgetBytes);
        }
    }

    void RuntimeServices::Initialize(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        const HWND window,
        const bool textureCompression)
    {
        InitializeImpl(
            device,
            context,
            window,
            textureCompression,
            nullptr);
    }

    void RuntimeServices::Initialize(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        const HWND window,
        const bool textureCompression,
        GraphicsBackend& backend)
    {
        InitializeImpl(
            device,
            context,
            window,
            textureCompression,
            &backend);
    }

    void RuntimeServices::InitializeImpl(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const HWND window,
        const bool textureCompression,
        GraphicsBackend* const backend)
    {
        if (m_input)
        {
            throw std::logic_error(
                "Runtime services are already initialized; call Shutdown "
                "or PrepareForGraphicsReinitialization first.");
        }
        CaptureAssetSettings();
        auto assets = backend != nullptr
            ? std::make_unique<AssetManager>(
                device,
                context,
                *backend)
            : std::make_unique<AssetManager>(
                device,
                context);
        assets->SetRuntimeTextureCompressionEnabled(textureCompression);
        RestoreAssetSettings(*assets);
        auto audio = m_audio
            ? std::unique_ptr<AudioSystem>{}
            : std::make_unique<AudioSystem>();
        auto input = std::make_unique<InputSystem>(window);
        if (m_reinitializationState
            && m_reinitializationState->inputActions)
        {
            input->SetActions(
                *m_reinitializationState->inputActions);
        }

        m_assets = std::move(assets);
        if (audio)
        {
            m_audio = std::move(audio);
        }
        m_input = std::move(input);
    }

    void RuntimeServices::QuiesceGraphicsWork() noexcept
    {
        if (m_assets)
        {
            m_assets->QuiesceGraphicsWork();
        }
    }

    void RuntimeServices::PrepareForGraphicsReinitialization() noexcept
    {
        // AssetManagerは旧Device / Contextを借りています。Inputも
        // Windowへ登録されるため作り直しますが、Audioは再利用します。
        QuiesceGraphicsWork();
        CaptureAssetSettings();
        if (m_input
            && m_reinitializationState)
        {
            try
            {
                m_reinitializationState->inputActions =
                    m_input->Actions();
            }
            catch (...)
            {
                // Keep the previous snapshot when allocation fails during
                // noexcept teardown.
            }
        }
        m_input.reset();
        m_assets.reset();
    }

    void RuntimeServices::Shutdown() noexcept
    {
        // 入力と再生を終了してから、それらが参照するアセットを解放します。
        // 複数回呼べるため、明示終了後のデストラクタとも共存できます。
        QuiesceGraphicsWork();
        m_input.reset();
        m_audio.reset();
        m_assets.reset();
        if (m_reinitializationState)
        {
            m_reinitializationState->assetRoot.reset();
            m_reinitializationState
                ->progressiveUploadThreshold.reset();
            m_reinitializationState
                ->textCacheBudgetBytes.reset();
            m_reinitializationState->inputActions.reset();
        }
    }

    AssetManager& RuntimeServices::EnsureAssets(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        const bool textureCompression)
    {
        return EnsureAssetsImpl(
            device,
            context,
            textureCompression,
            nullptr);
    }

    AssetManager& RuntimeServices::EnsureAssets(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        const bool textureCompression,
        GraphicsBackend& backend)
    {
        return EnsureAssetsImpl(
            device,
            context,
            textureCompression,
            &backend);
    }

    AssetManager& RuntimeServices::EnsureAssetsImpl(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const bool textureCompression,
        GraphicsBackend* const backend)
    {
        if (!m_assets)
        {
            auto assets = backend != nullptr
                ? std::make_unique<AssetManager>(
                    device,
                    context,
                    *backend)
                : std::make_unique<AssetManager>(
                    device,
                    context);
            assets->SetRuntimeTextureCompressionEnabled(textureCompression);
            RestoreAssetSettings(*assets);
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
