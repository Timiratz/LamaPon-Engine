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
        // 引き継ぐアセットのルートパス
        std::optional<std::filesystem::path> assetRoot;
        // 段階アップロードの閾値バイト数
        std::optional<std::size_t> progressiveUploadThreshold;
        // 文字キャッシュの上限バイト数
        std::optional<std::size_t> textCacheBudgetBytes;
        // 引き継ぐ入力アクション定義
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
            // 現在のアセットルートへの参照
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
            // 終了処理を継続し、既に保存できた設定を保持します。
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
        // 生成に成功してから交換する管理
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
        // 未生成時だけ新規作成する音声
        auto audio = m_audio
            ? std::unique_ptr<AudioSystem>{}
            : std::make_unique<AudioSystem>();
        // ウィンドウへ結び付ける入力
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
                // 保存に失敗しても前回の入力設定を保持し、解放を続けます。
            }
        }
        m_input.reset();
        m_assets.reset();
    }

    void RuntimeServices::Shutdown() noexcept
    {
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
            // 生成に成功してから交換する管理
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
