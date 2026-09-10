#include "LamaPon/Editor/EditorGuiRenderer.h"
#include "LamaPon/Editor/EditorModelPreviewRenderer.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Components/UIImageComponent.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Components/UIScrollViewComponent.h"
#include "LamaPon/Core/DebugOverlay.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/Component.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/SceneManager.h"

#include <Windows.h>
#include <CommonStates.h>
#include <imgui.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace
{
    class TestGraphicsOutputState final
        : public LamaPon::GraphicsOutputState
    {
    };

    // Public Component extensions receive the API-neutral draw context, and
    // Scene owns the surrounding pass and scissor state.
    class NeutralSceneSpriteComponent final
        : public LamaPon::Component
    {
    public:
        [[nodiscard]] std::size_t DrawCalls() const noexcept
        {
            return m_drawCalls;
        }

        [[nodiscard]] bool ContextWasActive() const noexcept
        {
            return m_contextWasActive;
        }

        [[nodiscard]] bool DrawWasAccepted() const noexcept
        {
            return m_drawWasAccepted;
        }

        void ThrowAfterNextDraw() noexcept
        {
            m_throwAfterDraw = true;
        }

        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return 1;
        }

    protected:
        void OnRender2D(
            const LamaPon::SpriteDrawContext& sprites) override
        {
            ++m_drawCalls;
            m_contextWasActive = static_cast<bool>(sprites);
            LamaPon::SpriteDrawRequest request;
            request.position = { 4.0f, 4.0f };
            request.scale = { 32.0f, 32.0f };
            request.tint = { 0.0f, 1.0f, 0.0f, 1.0f };
            m_drawWasAccepted = sprites.Draw(request);
            if (m_throwAfterDraw)
            {
                m_throwAfterDraw = false;
                throw std::runtime_error(
                    "Injected neutral Scene draw failure.");
            }
        }

    private:
        std::size_t m_drawCalls{};
        bool m_contextWasActive{};
        bool m_drawWasAccepted{};
        bool m_throwAfterDraw{};
    };

    void Require(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    template <typename Exception, typename Function>
    void RequireThrows(Function&& function, const char* message)
    {
        try
        {
            function();
        }
        catch (const Exception&)
        {
            return;
        }
        throw std::runtime_error(message);
    }

    template <typename Exception, typename Function>
    void RequireThrowsExactly(Function&& function, const char* message)
    {
        try
        {
            function();
        }
        catch (const Exception& exception)
        {
            if (typeid(exception) == typeid(Exception))
            {
                return;
            }
        }
        catch (...)
        {
        }
        throw std::runtime_error(message);
    }

    void RequireFactoryRejected(
        const LamaPon::RenderingApi activeApi,
        const char* message)
    {
        try
        {
            const auto renderer =
                LamaPon::CreateEditorGuiRenderer(activeApi);
            static_cast<void>(renderer);
        }
        catch (const std::logic_error&)
        {
            return;
        }
        throw std::runtime_error(message);
    }

    void RequireModelPreviewFactoryRejected(
        const LamaPon::RenderingApi activeApi,
        LamaPon::GraphicsDevice& graphics,
        const char* message)
    {
        try
        {
            const auto renderer =
                LamaPon::CreateEditorModelPreviewRenderer(
                    activeApi,
                    graphics);
            static_cast<void>(renderer);
        }
        catch (const std::logic_error&)
        {
            return;
        }
        throw std::runtime_error(message);
    }

    constexpr std::uint32_t Width = 96;
    constexpr std::uint32_t Height = 64;

    [[nodiscard]] ImTextureID ExpectedTextureId(
        const ID3D11ShaderResourceView* const view) noexcept
    {
        return static_cast<ImTextureID>(
            reinterpret_cast<std::uintptr_t>(view));
    }

    struct BoundVertexBuffer final
    {
        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        UINT stride{};
        UINT offset{};
    };

    [[nodiscard]] BoundVertexBuffer CaptureBoundVertexBuffer(
        LamaPon::GraphicsDevice& graphics,
        const UINT slot)
    {
        BoundVertexBuffer result;
        graphics.Context()->IAGetVertexBuffers(
            slot,
            1,
            result.buffer.ReleaseAndGetAddressOf(),
            &result.stride,
            &result.offset);
        return result;
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<
        ID3D11ShaderResourceView> CaptureBoundPixelShaderResource(
            LamaPon::GraphicsDevice& graphics,
            const UINT slot)
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> result;
        graphics.Context()->PSGetShaderResources(
            slot,
            1,
            result.ReleaseAndGetAddressOf());
        return result;
    }

    void PublishSolidTexture(
        LamaPon::TextureAsset& asset,
        LamaPon::GraphicsDevice& graphics,
        const std::array<std::uint8_t, 4>& color)
    {
        const std::array initialData{
            LamaPon::GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ color }),
                static_cast<std::uint32_t>(color.size()),
                static_cast<std::uint32_t>(color.size())
            }
        };
        auto texture = graphics.CreateTexture2D(
            LamaPon::GraphicsTexture2DDescription{
                1,
                1,
                1,
                LamaPon::GraphicsTextureFormat::Rgba8Unorm,
                LamaPon::GraphicsTextureUpdateMode::Immutable
            },
            initialData);
        auto view = graphics.CreateShaderResourceView(
            texture,
            LamaPon::GraphicsTextureViewDescription{ 0, 1 });
        auto* const d3d11View =
            graphics.TryResolveD3D11ShaderResourceView(view);
        Require(d3d11View != nullptr,
            "Editor GUI test texture view creation failed");

        LamaPon::TextureResourceSnapshot resources;
        resources.texture = std::move(texture);
        resources.shaderResourceView = std::move(view);
        resources.d3d11ShaderResourceView = d3d11View;
        asset.resources.Publish(std::move(resources));
        asset.width = 1;
        asset.height = 1;
    }

    [[nodiscard]] LamaPon::TextureAsset CreateSolidTexture(
        LamaPon::GraphicsDevice& graphics,
        const std::array<std::uint8_t, 4>& color)
    {
        LamaPon::TextureAsset asset;
        PublishSolidTexture(asset, graphics, color);
        return asset;
    }

    void DrawSolidRectangle(
        LamaPon::GraphicsDevice& graphics,
        const DirectX::XMFLOAT2 position,
        const DirectX::XMFLOAT2 size,
        const DirectX::XMFLOAT4 color)
    {
        auto pass = graphics.BeginSpritePass();
        LamaPon::SpriteDrawRequest request;
        request.position = position;
        request.scale = size;
        request.tint = color;
        Require(pass.Draw(request),
            "A solid rectangle was rejected by the sprite pass");
        pass.End();
    }

    void DrawImageWindow(
        const char* const title,
        const ImVec2 position,
        const ImTextureRef texture)
    {
        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoBackground
            | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoSavedSettings;
        ImGui::SetNextWindowPos(position, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2{ 40.0f, 48.0f }, ImGuiCond_Always);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2{ 0.0f, 0.0f });
        ImGui::Begin(title, nullptr, flags);
        ImGui::Image(texture, ImVec2{ 40.0f, 48.0f });
        ImGui::End();
        ImGui::PopStyleVar();
    }

    void RequirePixelNear(
        const std::vector<std::uint8_t>& pixels,
        const std::uint32_t x,
        const std::uint32_t y,
        const std::array<std::uint8_t, 3>& expected,
        const char* const message)
    {
        const auto offset =
            (static_cast<std::size_t>(y) * Width + x) * 4u;
        Require(offset + 2u < pixels.size(),
            "Editor GUI sampled pixel is outside the back buffer");
        constexpr int tolerance = 4;
        Require(
            std::abs(static_cast<int>(pixels[offset]) - expected[0])
                    <= tolerance
                && std::abs(
                    static_cast<int>(pixels[offset + 1u])
                    - expected[1]) <= tolerance
                && std::abs(
                    static_cast<int>(pixels[offset + 2u])
                    - expected[2]) <= tolerance,
            message);
    }

    class HiddenWindow final
    {
    public:
        HiddenWindow()
            : m_instance(GetModuleHandleW(nullptr))
        {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = DefWindowProcW;
            windowClass.hInstance = m_instance;
            windowClass.lpszClassName = ClassName;
            Require(RegisterClassExW(&windowClass) != 0,
                "Editor GUI test window registration failed");
            m_window = CreateWindowExW(
                0,
                ClassName,
                L"LamaPonEditorGuiRendererTests",
                WS_OVERLAPPEDWINDOW,
                0,
                0,
                static_cast<int>(Width),
                static_cast<int>(Height),
                nullptr,
                nullptr,
                m_instance,
                nullptr);
            Require(m_window != nullptr,
                "Editor GUI test window creation failed");
        }

        ~HiddenWindow()
        {
            if (m_window != nullptr)
            {
                DestroyWindow(m_window);
            }
            UnregisterClassW(ClassName, m_instance);
        }

        HiddenWindow(const HiddenWindow&) = delete;
        HiddenWindow& operator=(const HiddenWindow&) = delete;

        [[nodiscard]] HWND Get() const noexcept
        {
            return m_window;
        }

    private:
        inline static constexpr wchar_t ClassName[] =
            L"LamaPonEditorGuiRendererTests";
        HINSTANCE m_instance{};
        HWND m_window{};
    };

    class ImGuiContextScope final
    {
    public:
        ImGuiContextScope()
        {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(
                static_cast<float>(Width),
                static_cast<float>(Height));
            io.DeltaTime = 1.0f / 60.0f;
        }

        ~ImGuiContextScope()
        {
            ImGui::DestroyContext();
        }

        ImGuiContextScope(const ImGuiContextScope&) = delete;
        ImGuiContextScope& operator=(
            const ImGuiContextScope&) = delete;
    };

    void CheckD3D11Lifecycle()
    {
        HiddenWindow window;
        LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);

        // GPU初期化前でもfile-only AssetManagerとScene async loadは使える
        // ため、初回Initializeもlive ownerがいれば安全側で拒否します。
        LamaPon::SpriteRenderPass passPastDeviceLifetime;
        LamaPon::SpriteDrawContext contextPastDeviceLifetime;
        {
            LamaPon::GraphicsDevice initiallyGuardedGraphics;
            auto* const fileOnlyAssets =
                &initiallyGuardedGraphics.Assets();
            RequireThrowsExactly<std::logic_error>(
                [&]
                {
                    static_cast<void>(
                        initiallyGuardedGraphics.BeginSpritePass());
                },
                "An uninitialized graphics device accepted a sprite pass");
            {
                LamaPon::Scene initialScene(
                    initiallyGuardedGraphics);
                RequireThrowsExactly<std::logic_error>(
                    [&]
                    {
                        initiallyGuardedGraphics.Initialize(
                            window.Get(),
                            Width,
                            Height,
                            LamaPon::RenderingApi::DirectX11);
                    },
                    "Initial graphics setup accepted a live Scene");
                Require(
                    !initiallyGuardedGraphics.IsInitialized()
                        && initiallyGuardedGraphics.TryAssets()
                            == fileOnlyAssets,
                    "Rejected initial setup replaced the file-only AssetManager");
            }
            auto resourceLease =
                initiallyGuardedGraphics.AcquireResourceLease();
            auto movedResourceLease = std::move(resourceLease);
            Require(
                !resourceLease && movedResourceLease,
                "Moving a graphics resource lease changed its ownership count");
            RequireThrowsExactly<std::logic_error>(
                [&]
                {
                    initiallyGuardedGraphics.Initialize(
                        window.Get(),
                        Width,
                        Height,
                        LamaPon::RenderingApi::DirectX11);
                },
                "A moved graphics resource lease did not block initialization");
            movedResourceLease.Reset();
            movedResourceLease.Reset();
            initiallyGuardedGraphics.Initialize(
                window.Get(),
                Width,
                Height,
                LamaPon::RenderingApi::DirectX11);
            Require(
                initiallyGuardedGraphics.IsInitialized(),
                "Resetting the final resource lease did not reopen initialization");

            const D3D11_VIEWPORT spritePassViewport{
                0.0f,
                0.0f,
                static_cast<float>(Width),
                static_cast<float>(Height),
                0.0f,
                1.0f };
            initiallyGuardedGraphics.Context()->RSSetViewports(
                1,
                &spritePassViewport);
            LamaPon::SpritePassDescription invalidSpriteDescription;
            invalidSpriteDescription.blend =
                static_cast<LamaPon::SpriteBlendMode>(255);
            RequireThrowsExactly<std::invalid_argument>(
                [&]
                {
                    static_cast<void>(
                        initiallyGuardedGraphics.BeginSpritePass(
                            invalidSpriteDescription));
                },
                "An invalid sprite pass did not report its blend mode");
            auto passAfterRejectedBegin =
                initiallyGuardedGraphics.BeginSpritePass();
            Require(static_cast<bool>(passAfterRejectedBegin),
                "A rejected sprite pass retained its backend reservation");
            passAfterRejectedBegin.End();

            auto spritePass =
                initiallyGuardedGraphics.BeginSpritePass();
            auto spriteContext = spritePass.Context();
            auto movedSpritePass = std::move(spritePass);
            Require(
                !spritePass && movedSpritePass && spriteContext,
                "Moving a sprite render pass lost its active context");
            RequireThrowsExactly<std::logic_error>(
                [&]
                {
                    initiallyGuardedGraphics.Initialize(
                        window.Get(),
                        Width,
                        Height,
                        LamaPon::RenderingApi::DirectX11);
                },
                "An active sprite render pass did not block initialization");
            std::atomic_bool wrongThreadEndRejected{};
            std::thread wrongThreadEnd(
                [&]
                {
                    try
                    {
                        movedSpritePass.End();
                    }
                    catch (const std::logic_error&)
                    {
                        wrongThreadEndRejected.store(
                            true,
                            std::memory_order_relaxed);
                    }
                });
            wrongThreadEnd.join();
            Require(
                wrongThreadEndRejected.load(
                    std::memory_order_relaxed)
                    && movedSpritePass
                    && spriteContext,
                "A wrong-thread End corrupted the active sprite pass");
            movedSpritePass.End();
            movedSpritePass.End();
            movedSpritePass.Abort();
            Require(
                !movedSpritePass
                    && !spriteContext
                    && !spriteContext.Draw({})
                    && !spriteContext.PushScissor({})
                    && !spriteContext.PopScissor(),
                "A context remained usable after its sprite pass ended");

            {
                auto automaticSpritePass =
                    initiallyGuardedGraphics.BeginSpritePass();
                Require(static_cast<bool>(automaticSpritePass),
                    "A sprite render pass could not restart after End");
            }
            auto spritePassAfterAbort =
                initiallyGuardedGraphics.BeginSpritePass();
            Require(static_cast<bool>(spritePassAfterAbort),
                "The sprite pass destructor did not close its batch");
            spritePassAfterAbort.End();

            ImGuiContextScope initialRendererContext;
            auto initialRenderer =
                LamaPon::CreateEditorGuiRenderer(
                    initiallyGuardedGraphics.ActiveRenderingApi());
            initialRenderer->Initialize(
                initiallyGuardedGraphics);
            Microsoft::WRL::ComPtr<ID3D11Device>
                initialRendererDevice =
                    initiallyGuardedGraphics.Device();
            RequireThrowsExactly<std::logic_error>(
                [&]
                {
                    initiallyGuardedGraphics.Initialize(
                        window.Get(),
                        Width,
                        Height,
                        LamaPon::RenderingApi::DirectX11);
                },
                "Graphics initialization accepted an active editor renderer");
            initialRenderer->Shutdown();
            initiallyGuardedGraphics.Initialize(
                window.Get(),
                Width,
                Height,
                LamaPon::RenderingApi::DirectX11);
            Require(
                initiallyGuardedGraphics.Device()
                    != initialRendererDevice.Get(),
                "Editor renderer shutdown did not release its resource lease");

            const auto preservedAssetRoot =
                std::filesystem::path{ LAMAPON_TEST_ASSET_DIR };
            initiallyGuardedGraphics.Assets().SetAssetRoot(
                preservedAssetRoot);
            initiallyGuardedGraphics.Assets()
                .SetProgressiveUploadThreshold(4096);
            initiallyGuardedGraphics.Assets()
                .SetTextCacheBudgetBytes(8192);
            initiallyGuardedGraphics.Input().SetActions(
                {
                    {
                        "PreservedAction",
                        {
                            {
                                LamaPon::InputControl::KeyboardSpace,
                                0.75f
                            }
                        }
                    }
                });
            auto* const preservedAudio =
                &initiallyGuardedGraphics.Audio();
            preservedAudio->SetMasterVolume(0.65f);
            preservedAudio->SetBusVolume(
                LamaPon::AudioBus::Effects,
                0.4f);
            preservedAudio->SetSuspended(true);
            Require(
                initiallyGuardedGraphics.States().Opaque() != nullptr
                    && initiallyGuardedGraphics
                        .AdditiveBlendPreservingAlpha() != nullptr,
                "Graphics resources were not available before failed reinitialization");
            Microsoft::WRL::ComPtr<ID3D11Device>
                deviceBeforeFailedReinitialization =
                    initiallyGuardedGraphics.Device();
            RequireThrows<std::runtime_error>(
                [&]
                {
                    initiallyGuardedGraphics.Initialize(
                        nullptr,
                        Width,
                        Height,
                        LamaPon::RenderingApi::DirectX11);
                },
                "Invalid graphics reinitialization must report a failure");
            Require(
                !initiallyGuardedGraphics.IsInitialized()
                    && initiallyGuardedGraphics.Device() == nullptr
                    && &initiallyGuardedGraphics.Audio()
                        == preservedAudio
                    && std::abs(
                        preservedAudio->MasterVolume() - 0.65f)
                        < 0.0001f
                    && std::abs(
                        preservedAudio->BusVolume(
                            LamaPon::AudioBus::Effects) - 0.4f)
                        < 0.0001f
                    && preservedAudio->IsSuspended(),
                "Failed graphics reinitialization did not preserve audio");
            initiallyGuardedGraphics.Initialize(
                window.Get(),
                Width,
                Height,
                LamaPon::RenderingApi::DirectX11);
            auto* const recoveredOpaque =
                initiallyGuardedGraphics.States().Opaque();
            auto* const recoveredAdditive =
                initiallyGuardedGraphics
                    .AdditiveBlendPreservingAlpha();
            Microsoft::WRL::ComPtr<ID3D11Device>
                recoveredOpaqueDevice;
            Microsoft::WRL::ComPtr<ID3D11Device>
                recoveredAdditiveDevice;
            if (recoveredOpaque != nullptr)
            {
                recoveredOpaque->GetDevice(
                    recoveredOpaqueDevice.ReleaseAndGetAddressOf());
            }
            if (recoveredAdditive != nullptr)
            {
                recoveredAdditive->GetDevice(
                    recoveredAdditiveDevice.ReleaseAndGetAddressOf());
            }
            Require(
                initiallyGuardedGraphics.Device() != nullptr
                    && initiallyGuardedGraphics.Device()
                        != deviceBeforeFailedReinitialization.Get()
                    && recoveredOpaqueDevice.Get()
                        == initiallyGuardedGraphics.Device()
                    && recoveredAdditiveDevice.Get()
                        == initiallyGuardedGraphics.Device(),
                "Graphics recovery did not rebuild DirectX 11 frontend resources");
            Require(
                &initiallyGuardedGraphics.Audio()
                    == preservedAudio
                    && std::abs(
                        preservedAudio->MasterVolume() - 0.65f)
                        < 0.0001f
                    && std::abs(
                        preservedAudio->BusVolume(
                            LamaPon::AudioBus::Effects) - 0.4f)
                        < 0.0001f
                    && preservedAudio->IsSuspended(),
                "Graphics recovery recreated preserved audio");
            Require(
                initiallyGuardedGraphics.Assets().AssetRoot()
                        == preservedAssetRoot
                    && initiallyGuardedGraphics.Assets()
                        .ProgressiveUploadThreshold() == 4096
                    && initiallyGuardedGraphics.Assets()
                        .TextCacheBudgetBytes() == 8192,
                "Graphics recovery lost asset configuration");
            const auto& restoredActions =
                initiallyGuardedGraphics.Input().Actions();
            Require(
                restoredActions.size() == 1
                    && restoredActions.front().name
                        == "PreservedAction"
                    && restoredActions.front().bindings.size() == 1
                    && restoredActions.front().bindings.front().control
                        == LamaPon::InputControl::KeyboardSpace
                    && std::abs(
                        restoredActions.front().bindings.front().scale
                            - 0.75f) < 0.0001f,
                "Graphics recovery lost input action configuration");
            preservedAudio->SetSuspended(false);
            initiallyGuardedGraphics.Context()->RSSetViewports(
                1,
                &spritePassViewport);
            passPastDeviceLifetime =
                initiallyGuardedGraphics.BeginSpritePass();
            contextPastDeviceLifetime =
                passPastDeviceLifetime.Context();
        }
        Require(
            !passPastDeviceLifetime
                && !contextPastDeviceLifetime
                && !passPastDeviceLifetime.Draw({})
                && !contextPastDeviceLifetime.Draw({}),
            "A sprite pass accessed its GraphicsDevice after destruction");
        passPastDeviceLifetime.End();
        passPastDeviceLifetime.Abort();

        // A background model import owns the old Device beyond the initiating
        // call. Reinitialization must join it before stopping the backend,
        // then create a fresh AssetManager that remains usable.
        {
            LamaPon::GraphicsDevice asyncGraphics;
            asyncGraphics.Initialize(
                window.Get(),
                Width,
                Height,
                LamaPon::RenderingApi::DirectX11);
            const auto modelPath =
                std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }
                / L"models/arrow.cmo";
            Require(
                asyncGraphics.Assets().PrepareModelAsync(modelPath),
                "Background model preparation did not start");
            asyncGraphics.Initialize(
                window.Get(),
                Width,
                Height,
                LamaPon::RenderingApi::DirectX11);
            Require(
                asyncGraphics.IsInitialized()
                    && asyncGraphics.Assets().LoadModel(modelPath)
                        != nullptr,
                "Graphics reinitialization did not quiesce model work");
        }

        LamaPon::GraphicsDevice failedGraphics;
        RequireThrows<std::runtime_error>(
            [&]
            {
                failedGraphics.Initialize(
                    nullptr,
                    Width,
                    Height,
                    LamaPon::RenderingApi::DirectX11);
            },
            "Invalid graphics initialization must report a failure");
        Require(
            !failedGraphics.IsInitialized()
                && failedGraphics.Device() == nullptr
                && failedGraphics.Context() == nullptr
                && failedGraphics.TryAssets() == nullptr,
            "Failed graphics initialization retained partial resources");

        {
            LamaPon::GraphicsDevice fallbackGraphics;
            fallbackGraphics.Initialize(
                window.Get(),
                Width,
                Height,
                LamaPon::RenderingApi::DirectX12Experimental);
            Require(
                fallbackGraphics.StartupRenderingApi()
                        == LamaPon::RenderingApi::DirectX12Experimental
                    && fallbackGraphics.ActiveRenderingApi()
                        == LamaPon::RenderingApi::DirectX11
                    && fallbackGraphics.RenderingApiFallback()
                        == LamaPon::RenderingApiFallbackReason::NotImplemented,
                "Sprite smoke test requires the DirectX 11 fallback");
            constexpr float fallbackClearColor[]{
                0.0f, 0.0f, 0.0f, 1.0f };
            fallbackGraphics.BeginFrame(fallbackClearColor);
            auto fallbackNeutralPass =
                fallbackGraphics.BeginSpritePass();
            LamaPon::SpriteDrawRequest fallbackNeutralRequest;
            Require(fallbackNeutralPass.Draw(
                    fallbackNeutralRequest),
                "The DirectX 12 fallback rejected a neutral sprite draw");
            fallbackNeutralPass.End();
            fallbackGraphics.EndFrame();
        }

        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            Width,
            Height,
            LamaPon::RenderingApi::DirectX12Experimental);
        Require(
            graphics.StartupRenderingApi()
                    == LamaPon::RenderingApi::DirectX12Experimental
                && graphics.ActiveRenderingApi()
                    == LamaPon::RenderingApi::DirectX11
                && graphics.RenderingApiFallback()
                    == LamaPon::RenderingApiFallbackReason::NotImplemented,
            "Editor GUI smoke test requires the DirectX 11 fallback");

        // Backend差し替え前に遅延生成資源も作り、同じGraphicsDeviceを
        // 再初期化した後の描画で旧Device由来の資源が残らないことを
        // このテスト全体で確認します。
        Microsoft::WRL::ComPtr<ID3D11Device> previousDevice =
            graphics.Device();
        const auto previousWhiteTexture =
            graphics.WhiteTextureHandle();
        const auto previousWhiteView =
            graphics.WhiteTextureViewHandle();
        auto* const previousWhiteD3D11View =
            graphics.TryResolveD3D11ShaderResourceView(
                previousWhiteView);
        Require(
            previousWhiteD3D11View != nullptr
                && graphics.AdditiveBlendPreservingAlpha() != nullptr,
            "DirectX 11 compatibility resources were not created");
        constexpr UINT PixelShaderResourceTestSlot =
            D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1;
        const std::array<LamaPon::GraphicsViewHandle, 1>
            missingPixelShaderResource{};
        Require(
            graphics.TryBindPixelShaderResources(
                PixelShaderResourceTestSlot,
                missingPixelShaderResource,
                previousWhiteView)
                && CaptureBoundPixelShaderResource(
                    graphics,
                    PixelShaderResourceTestSlot).Get()
                    == previousWhiteD3D11View,
            "Neutral pixel shader resource fallback was not bound");
        Require(
            graphics.TryBindPixelShaderResources(
                PixelShaderResourceTestSlot,
                missingPixelShaderResource)
                && CaptureBoundPixelShaderResource(
                    graphics,
                    PixelShaderResourceTestSlot) == nullptr,
            "An empty neutral pixel shader resource did not unbind its slot");
        const LamaPon::TextureResourceSnapshot
            previousWhiteResources{
                previousWhiteTexture,
                previousWhiteView,
                previousWhiteD3D11View
            };
        const std::array<float, 4> instanceData{
            1.0f, 2.0f, 3.0f, 4.0f };
        const auto instanceBytes = std::as_bytes(
            std::span{ instanceData });
        const auto previousInstanceBuffer =
            graphics.AcquireInstanceBufferHandle(instanceBytes);
        const auto reusedPreviousInstanceBuffer =
            graphics.AcquireInstanceBufferHandle(instanceBytes);
        constexpr UINT InstanceBufferTestSlot =
            D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1;
        constexpr UINT InstanceBufferTestStride =
            sizeof(instanceData);
        graphics.BindVertexBuffer(
            previousInstanceBuffer,
            InstanceBufferTestSlot,
            InstanceBufferTestStride);
        const auto previousBoundInstanceBuffer =
            CaptureBoundVertexBuffer(
                graphics,
                InstanceBufferTestSlot);
        Require(
            previousWhiteTexture
                && previousWhiteView
                && previousWhiteView.Kind()
                    == LamaPon::GraphicsViewKind::ShaderResource
                && previousInstanceBuffer
                && graphics.TryResolveD3D11ShaderResourceView(
                    previousWhiteView) == previousWhiteD3D11View
                && reusedPreviousInstanceBuffer
                    == previousInstanceBuffer
                && previousBoundInstanceBuffer.buffer
                && previousBoundInstanceBuffer.stride
                    == InstanceBufferTestStride
                && previousBoundInstanceBuffer.offset == 0,
            "Neutral graphics resources were not reused and bound correctly");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                graphics.BindVertexBuffer(
                    {},
                    InstanceBufferTestSlot,
                    InstanceBufferTestStride);
            },
            "An empty vertex buffer handle was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                graphics.BindVertexBuffer(
                    previousInstanceBuffer,
                    InstanceBufferTestSlot,
                    0);
            },
            "A zero vertex buffer stride was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                graphics.BindVertexBuffer(
                    previousInstanceBuffer,
                    D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
                    InstanceBufferTestStride);
            },
            "An out-of-range vertex buffer slot was accepted");
        const LamaPon::TextureResourceSnapshot
            incompleteNeutralResources{
                previousWhiteTexture,
                {},
                previousWhiteD3D11View
            };
        Require(
            graphics.TryResolveD3D11ShaderResourceView(
                incompleteNeutralResources) == nullptr,
            "An incomplete neutral snapshot fell back to its raw D3D11 view");

        // SceneとそのComponentは旧Device世代のAssetManager / Effect等を
        // 保持します。再起動前提の設定を実行中に適用しようとしても、
        // 現在の状態を一切破棄する前に拒否されることを確認します。
        auto* const previousAssets = graphics.TryAssets();
        {
            LamaPon::Scene liveScene(graphics);
            RequireThrowsExactly<std::logic_error>(
                [&]
                {
                    graphics.Initialize(
                        window.Get(),
                        Width,
                        Height,
                        LamaPon::RenderingApi::DirectX11);
                },
                "Graphics reinitialization accepted a live Scene");
            Require(
                graphics.IsInitialized()
                    && graphics.Device() == previousDevice.Get()
                    && graphics.TryAssets() == previousAssets
                    && graphics.TryResolveD3D11ShaderResourceView(
                        previousWhiteView)
                        == previousWhiteD3D11View,
                "Rejected reinitialization changed the active graphics state");
        }

        graphics.Initialize(
            window.Get(),
            Width,
            Height,
            LamaPon::RenderingApi::DirectX11);
        const auto rebuiltWhiteView =
            graphics.WhiteTextureViewHandle();
        auto* const rebuiltWhiteD3D11View =
            graphics.TryResolveD3D11ShaderResourceView(
                rebuiltWhiteView);
        const std::array stalePixelShaderResource{
            previousWhiteView
        };
        Require(
            graphics.TryBindPixelShaderResources(
                PixelShaderResourceTestSlot,
                stalePixelShaderResource,
                rebuiltWhiteView)
                && CaptureBoundPixelShaderResource(
                    graphics,
                    PixelShaderResourceTestSlot).Get()
                    == rebuiltWhiteD3D11View,
            "A stale pixel shader resource did not use the current fallback");
        Require(
            graphics.TryBindPixelShaderResources(
                PixelShaderResourceTestSlot,
                stalePixelShaderResource,
                previousWhiteView)
                && CaptureBoundPixelShaderResource(
                    graphics,
                    PixelShaderResourceTestSlot) == nullptr,
            "Invalid pixel shader resource and fallback did not bind null");
        Require(
            graphics.TryBindPixelShaderResources(
                PixelShaderResourceTestSlot,
                missingPixelShaderResource,
                rebuiltWhiteView),
            "The pixel shader resource baseline could not be restored");
        const std::array overflowPixelShaderResources{
            rebuiltWhiteView,
            rebuiltWhiteView
        };
        Require(
            !graphics.TryBindPixelShaderResources(
                PixelShaderResourceTestSlot,
                overflowPixelShaderResources,
                rebuiltWhiteView)
                && CaptureBoundPixelShaderResource(
                    graphics,
                    PixelShaderResourceTestSlot).Get()
                    == rebuiltWhiteD3D11View,
            "An overflowing pixel shader resource range changed pipeline state");
        Require(
            graphics.TryBindPixelShaderResources(
                PixelShaderResourceTestSlot,
                std::span<const LamaPon::GraphicsViewHandle>{})
                && CaptureBoundPixelShaderResource(
                    graphics,
                    PixelShaderResourceTestSlot).Get()
                    == rebuiltWhiteD3D11View,
            "An empty pixel shader resource range was not a no-op");
        Require(
            graphics.TryBindPixelShaderResources(
                PixelShaderResourceTestSlot,
                missingPixelShaderResource)
                && CaptureBoundPixelShaderResource(
                    graphics,
                    PixelShaderResourceTestSlot) == nullptr,
            "Neutral pixel shader resource cleanup failed");
        Require(
            graphics.StartupRenderingApi()
                    == LamaPon::RenderingApi::DirectX11
                && graphics.ActiveRenderingApi()
                    == LamaPon::RenderingApi::DirectX11
                && graphics.RenderingApiFallback()
                    == LamaPon::RenderingApiFallbackReason::None
                && rebuiltWhiteD3D11View != nullptr
                && graphics.AdditiveBlendPreservingAlpha() != nullptr
                && graphics.Device() != previousDevice.Get(),
            "GraphicsDevice reinitialization did not rebuild DirectX 11 resources");
        Require(
            previousWhiteTexture
                && previousWhiteView
                && previousInstanceBuffer,
            "Backend shutdown invalidated externally owned handle lifetimes");
        Require(
            graphics.TryResolveD3D11ShaderResourceView(
                previousWhiteView) == nullptr,
            "The non-throwing shader view resolver accepted a stale handle");
        Require(
            graphics.TryResolveD3D11ShaderResourceView(
                previousWhiteResources) == nullptr,
            "A stale texture snapshot fell back to its old raw D3D11 view");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                graphics.BindVertexBuffer(
                    previousInstanceBuffer,
                    InstanceBufferTestSlot,
                    InstanceBufferTestStride);
            },
            "A buffer from the previous backend generation was accepted");

        const auto rebuiltInstanceHandle =
            graphics.AcquireInstanceBufferHandle(instanceBytes);
        const auto reusedRebuiltInstanceHandle =
            graphics.AcquireInstanceBufferHandle(instanceBytes);
        graphics.BindVertexBuffer(
            rebuiltInstanceHandle,
            InstanceBufferTestSlot,
            InstanceBufferTestStride);
        const auto rebuiltBoundInstanceBuffer =
            CaptureBoundVertexBuffer(
                graphics,
                InstanceBufferTestSlot);
        Require(
            rebuiltInstanceHandle
                && rebuiltInstanceHandle != previousInstanceBuffer
                && reusedRebuiltInstanceHandle
                    == rebuiltInstanceHandle
                && graphics.WhiteTextureHandle()
                    != previousWhiteTexture
                && rebuiltWhiteView
                    != previousWhiteView
                && rebuiltBoundInstanceBuffer.buffer
                && rebuiltBoundInstanceBuffer.buffer.Get()
                    != previousBoundInstanceBuffer.buffer.Get(),
            "GraphicsDevice reinitialization did not rebuild neutral resources");

        std::vector<std::byte> grownInstanceData(
            8192,
            std::byte{ 0x2a });
        const auto grownInstanceHandle =
            graphics.AcquireInstanceBufferHandle(grownInstanceData);
        graphics.BindVertexBuffer(
            grownInstanceHandle,
            InstanceBufferTestSlot,
            InstanceBufferTestStride);
        const auto grownBoundInstanceBuffer =
            CaptureBoundVertexBuffer(
                graphics,
                InstanceBufferTestSlot);
        graphics.BindVertexBuffer(
            rebuiltInstanceHandle,
            InstanceBufferTestSlot,
            InstanceBufferTestStride);
        const auto retainedRebuiltInstanceBuffer =
            CaptureBoundVertexBuffer(
                graphics,
                InstanceBufferTestSlot);
        graphics.BindVertexBuffer(
            grownInstanceHandle,
            InstanceBufferTestSlot,
            InstanceBufferTestStride);
        Require(
            grownInstanceHandle
                && grownInstanceHandle != rebuiltInstanceHandle
                && grownBoundInstanceBuffer.buffer
                && grownBoundInstanceBuffer.buffer.Get()
                    != rebuiltBoundInstanceBuffer.buffer.Get()
                && retainedRebuiltInstanceBuffer.buffer.Get()
                    == rebuiltBoundInstanceBuffer.buffer.Get(),
            "Growing a neutral buffer invalidated an externally held handle");

        // API非依存texture契約のinitial upload、mip範囲view、後続update、
        // 入力検証をD3D11/WARP実装に対して確認します。
        const std::array<std::uint8_t, 16> textureMip0{
            0xffu, 0x00u, 0x00u, 0xffu,
            0x00u, 0xffu, 0x00u, 0xffu,
            0x00u, 0x00u, 0xffu, 0xffu,
            0xffu, 0xffu, 0xffu, 0xffu
        };
        const std::array<std::uint8_t, 4> textureMip1{
            0x7fu, 0x7fu, 0x7fu, 0xffu
        };
        const std::array textureInitialData{
            LamaPon::GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ textureMip0 }),
                8,
                16
            },
            LamaPon::GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ textureMip1 }),
                4,
                4
            }
        };
        const LamaPon::GraphicsTexture2DDescription textureDescription{
            2,
            2,
            2,
            LamaPon::GraphicsTextureFormat::Rgba8Unorm,
            LamaPon::GraphicsTextureUpdateMode::Immutable
        };
        const auto neutralTexture = graphics.CreateTexture2D(
            textureDescription,
            textureInitialData);
        const auto fullTextureView = graphics.CreateShaderResourceView(
            neutralTexture,
            LamaPon::GraphicsTextureViewDescription{ 0, 2 });
        const auto smallestMipView = graphics.CreateShaderResourceView(
            neutralTexture,
            LamaPon::GraphicsTextureViewDescription{ 1, 1 });
        auto* const nativeSmallestMipView =
            graphics.TryResolveD3D11ShaderResourceView(smallestMipView);
        Require(
            nativeSmallestMipView != nullptr,
            "The neutral texture mip view did not resolve to D3D11");
        D3D11_SHADER_RESOURCE_VIEW_DESC smallestMipDescription{};
        nativeSmallestMipView->GetDesc(&smallestMipDescription);
        Require(
            neutralTexture
                && fullTextureView
                && fullTextureView.Kind()
                    == LamaPon::GraphicsViewKind::ShaderResource
                && smallestMipDescription.ViewDimension
                    == D3D11_SRV_DIMENSION_TEXTURE2D
                && smallestMipDescription.Texture2D.MostDetailedMip == 1
                && smallestMipDescription.Texture2D.MipLevels == 1,
            "The neutral texture mip view did not preserve its range");

        const std::array<std::uint8_t, 16> updatedTextureMip0{
            0x20u, 0x30u, 0x40u, 0xffu,
            0x20u, 0x30u, 0x40u, 0xffu,
            0x20u, 0x30u, 0x40u, 0xffu,
            0x20u, 0x30u, 0x40u, 0xffu
        };
        const auto updateableTexture = graphics.CreateTexture2D(
            LamaPon::GraphicsTexture2DDescription{
                2,
                2,
                1,
                LamaPon::GraphicsTextureFormat::Rgba8Unorm,
                LamaPon::GraphicsTextureUpdateMode::PerMipUpdate
            },
            {});
        graphics.UpdateTexture2D(
            updateableTexture,
            0,
            LamaPon::GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ updatedTextureMip0 }),
                8,
                16
            });
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                graphics.UpdateTexture2D(
                    neutralTexture,
                    0,
                    LamaPon::GraphicsTextureSubresourceData{
                        std::as_bytes(std::span{ updatedTextureMip0 }),
                        8,
                        16
                    });
            },
            "An immutable neutral texture accepted a later update");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                static_cast<void>(graphics.CreateTexture2D(
                    LamaPon::GraphicsTexture2DDescription{
                        0,
                        2,
                        1,
                        LamaPon::GraphicsTextureFormat::Rgba8Unorm
                    },
                    {}));
            },
            "A zero-width neutral texture was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                const std::array shortMip{
                    LamaPon::GraphicsTextureSubresourceData{
                        std::as_bytes(std::span{ textureMip1 }),
                        8,
                        4
                    }
                };
                static_cast<void>(graphics.CreateTexture2D(
                    LamaPon::GraphicsTexture2DDescription{
                        2,
                        2,
                        1,
                        LamaPon::GraphicsTextureFormat::Rgba8Unorm
                    },
                    shortMip));
            },
            "A truncated neutral texture subresource was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                static_cast<void>(graphics.CreateShaderResourceView(
                    neutralTexture,
                    LamaPon::GraphicsTextureViewDescription{ 1, 2 }));
            },
            "An out-of-range neutral texture view was accepted");

        // Baked GIで使うimmutable 3D textureも、生成・view・所有・転送内容を
        // API非依存handle経由で保ちます。
        const std::array<std::uint16_t, 32> texture3DVoxels{
            0x0000u, 0x0001u, 0x0002u, 0x0003u,
            0x0010u, 0x0011u, 0x0012u, 0x0013u,
            0x0020u, 0x0021u, 0x0022u, 0x0023u,
            0x0030u, 0x0031u, 0x0032u, 0x0033u,
            0x0100u, 0x0101u, 0x0102u, 0x0103u,
            0x0110u, 0x0111u, 0x0112u, 0x0113u,
            0x0120u, 0x0121u, 0x0122u, 0x0123u,
            0x0130u, 0x0131u, 0x0132u, 0x0133u
        };
        const std::array texture3DInitialData{
            LamaPon::GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ texture3DVoxels }),
                16,
                32
            }
        };
        auto neutralTexture3D = graphics.CreateTexture3D(
            LamaPon::GraphicsTexture3DDescription{
                2,
                2,
                2,
                1,
                LamaPon::GraphicsTextureFormat::Rgba16Float
            },
            texture3DInitialData);
        const auto texture3DView = graphics.CreateShaderResourceView(
            neutralTexture3D,
            LamaPon::GraphicsTextureViewDescription{ 0, 1 });
        auto* const nativeTexture3DView =
            graphics.TryResolveD3D11ShaderResourceView(texture3DView);
        Require(
            neutralTexture3D
                && texture3DView
                && texture3DView.Kind()
                    == LamaPon::GraphicsViewKind::ShaderResource
                && nativeTexture3DView != nullptr,
            "The neutral Texture3D view did not resolve to D3D11");

        D3D11_SHADER_RESOURCE_VIEW_DESC texture3DViewDescription{};
        nativeTexture3DView->GetDesc(&texture3DViewDescription);
        Microsoft::WRL::ComPtr<ID3D11Resource> texture3DResource;
        nativeTexture3DView->GetResource(
            texture3DResource.ReleaseAndGetAddressOf());
        Microsoft::WRL::ComPtr<ID3D11Texture3D> nativeTexture3D;
        Require(
            SUCCEEDED(texture3DResource.As(&nativeTexture3D)),
            "The neutral Texture3D view did not own a 3D texture");
        D3D11_TEXTURE3D_DESC nativeTexture3DDescription{};
        nativeTexture3D->GetDesc(&nativeTexture3DDescription);
        Require(
            texture3DViewDescription.Format
                    == DXGI_FORMAT_R16G16B16A16_FLOAT
                && texture3DViewDescription.ViewDimension
                    == D3D11_SRV_DIMENSION_TEXTURE3D
                && texture3DViewDescription.Texture3D.MostDetailedMip == 0
                && texture3DViewDescription.Texture3D.MipLevels == 1
                && nativeTexture3DDescription.Width == 2
                && nativeTexture3DDescription.Height == 2
                && nativeTexture3DDescription.Depth == 2
                && nativeTexture3DDescription.MipLevels == 1
                && nativeTexture3DDescription.Format
                    == DXGI_FORMAT_R16G16B16A16_FLOAT
                && nativeTexture3DDescription.Usage
                    == D3D11_USAGE_IMMUTABLE,
            "The neutral Texture3D description was not preserved");

        auto stagingTexture3DDescription = nativeTexture3DDescription;
        stagingTexture3DDescription.Usage = D3D11_USAGE_STAGING;
        stagingTexture3DDescription.BindFlags = 0;
        stagingTexture3DDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingTexture3DDescription.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture3D> stagingTexture3D;
        Require(
            SUCCEEDED(graphics.Device()->CreateTexture3D(
                &stagingTexture3DDescription,
                nullptr,
                stagingTexture3D.ReleaseAndGetAddressOf())),
            "The Texture3D readback resource could not be created");
        graphics.Context()->CopyResource(
            stagingTexture3D.Get(),
            nativeTexture3D.Get());
        D3D11_MAPPED_SUBRESOURCE mappedTexture3D{};
        Require(
            SUCCEEDED(graphics.Context()->Map(
                stagingTexture3D.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mappedTexture3D)),
            "The neutral Texture3D could not be mapped for verification");
        bool texture3DContentMatches = true;
        for (std::uint32_t z{}; z < 2; ++z)
        {
            for (std::uint32_t y{}; y < 2; ++y)
            {
                const auto* const source = reinterpret_cast<
                    const std::uint16_t*>(
                        static_cast<const std::byte*>(
                            mappedTexture3D.pData)
                        + z * mappedTexture3D.DepthPitch
                        + y * mappedTexture3D.RowPitch);
                const auto sourceOffset =
                    static_cast<std::size_t>((z * 2 + y) * 8);
                for (std::size_t value{}; value < 8; ++value)
                {
                    texture3DContentMatches =
                        texture3DContentMatches
                        && source[value]
                            == texture3DVoxels[sourceOffset + value];
                }
            }
        }
        graphics.Context()->Unmap(stagingTexture3D.Get(), 0);
        Require(
            texture3DContentMatches,
            "The neutral Texture3D upload changed voxel data");

        // depthだけが縮むmip chainも有効です。2D前提のmip上限計算へ
        // 戻らないことと、Texture3Dの部分viewを確認します。
        const std::array<std::uint16_t, 8> depthMip0{
            0x1000u, 0x1001u, 0x1002u, 0x1003u,
            0x1010u, 0x1011u, 0x1012u, 0x1013u
        };
        const std::array<std::uint16_t, 4> depthMip1{
            0x2000u, 0x2001u, 0x2002u, 0x2003u
        };
        const std::array depthMipInitialData{
            LamaPon::GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ depthMip0 }),
                8,
                8
            },
            LamaPon::GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ depthMip1 }),
                8,
                8
            }
        };
        const auto depthMipTexture3D = graphics.CreateTexture3D(
            LamaPon::GraphicsTexture3DDescription{
                1,
                1,
                2,
                2,
                LamaPon::GraphicsTextureFormat::Rgba16Float
            },
            depthMipInitialData);
        const auto depthMipView = graphics.CreateShaderResourceView(
            depthMipTexture3D,
            LamaPon::GraphicsTextureViewDescription{ 1, 1 });
        auto* const nativeDepthMipView =
            graphics.TryResolveD3D11ShaderResourceView(depthMipView);
        D3D11_SHADER_RESOURCE_VIEW_DESC depthMipViewDescription{};
        if (nativeDepthMipView != nullptr)
        {
            nativeDepthMipView->GetDesc(&depthMipViewDescription);
        }
        Require(
            nativeDepthMipView != nullptr
                && depthMipViewDescription.ViewDimension
                    == D3D11_SRV_DIMENSION_TEXTURE3D
                && depthMipViewDescription.Texture3D.MostDetailedMip == 1
                && depthMipViewDescription.Texture3D.MipLevels == 1,
            "A depth-only Texture3D mip range was not preserved");

        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                static_cast<void>(graphics.CreateTexture3D(
                    LamaPon::GraphicsTexture3DDescription{
                        2,
                        2,
                        0,
                        1,
                        LamaPon::GraphicsTextureFormat::Rgba16Float
                    },
                    texture3DInitialData));
            },
            "A zero-depth neutral Texture3D was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                static_cast<void>(graphics.CreateTexture3D(
                    LamaPon::GraphicsTexture3DDescription{
                        D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION + 1u,
                        1,
                        1,
                        1,
                        LamaPon::GraphicsTextureFormat::Rgba16Float
                    },
                    texture3DInitialData));
            },
            "An oversized neutral Texture3D was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                static_cast<void>(graphics.CreateTexture3D(
                    LamaPon::GraphicsTexture3DDescription{
                        2,
                        2,
                        2,
                        1,
                        LamaPon::GraphicsTextureFormat::Rgba16Float
                    },
                    {}));
            },
            "A neutral Texture3D without subresources was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                const std::array invalidInitialData{
                    LamaPon::GraphicsTextureSubresourceData{
                        std::as_bytes(std::span{ texture3DVoxels }),
                        15,
                        32
                    }
                };
                static_cast<void>(graphics.CreateTexture3D(
                    LamaPon::GraphicsTexture3DDescription{
                        2,
                        2,
                        2,
                        1,
                        LamaPon::GraphicsTextureFormat::Rgba16Float
                    },
                    invalidInitialData));
            },
            "A neutral Texture3D with a short row pitch was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                const std::array invalidInitialData{
                    LamaPon::GraphicsTextureSubresourceData{
                        std::as_bytes(std::span{ texture3DVoxels }),
                        16,
                        31
                    }
                };
                static_cast<void>(graphics.CreateTexture3D(
                    LamaPon::GraphicsTexture3DDescription{
                        2,
                        2,
                        2,
                        1,
                        LamaPon::GraphicsTextureFormat::Rgba16Float
                    },
                    invalidInitialData));
            },
            "A neutral Texture3D with a short slice pitch was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                const std::array invalidInitialData{
                    LamaPon::GraphicsTextureSubresourceData{
                        std::as_bytes(std::span{ texture3DVoxels })
                            .first(63),
                        16,
                        32
                    }
                };
                static_cast<void>(graphics.CreateTexture3D(
                    LamaPon::GraphicsTexture3DDescription{
                        2,
                        2,
                        2,
                        1,
                        LamaPon::GraphicsTextureFormat::Rgba16Float
                    },
                    invalidInitialData));
            },
            "A truncated neutral Texture3D volume was accepted");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                static_cast<void>(graphics.CreateShaderResourceView(
                    neutralTexture3D,
                    LamaPon::GraphicsTextureViewDescription{ 1, 1 }));
            },
            "An out-of-range neutral Texture3D view was accepted");
        neutralTexture3D.Reset();
        Require(
            graphics.TryResolveD3D11ShaderResourceView(texture3DView)
                == nativeTexture3DView,
            "A neutral Texture3D view did not retain its texture");

        const std::array<std::uint16_t, 12> bakedGiCoefficients{
            0x0000u, 0x0001u, 0x0002u, 0x0003u,
            0x0010u, 0x0011u, 0x0012u, 0x0013u,
            0x0020u, 0x0021u, 0x0022u, 0x0023u
        };
        const auto neutralBakedGiViews =
            graphics.UploadBakedGlobalIlluminationViews(
                1,
                1,
                1,
                bakedGiCoefficients);
        bool bakedGiViewsAreTexture3D = true;
        for (const auto& view : neutralBakedGiViews)
        {
            auto* const nativeView =
                graphics.TryResolveD3D11ShaderResourceView(view);
            D3D11_SHADER_RESOURCE_VIEW_DESC description{};
            if (nativeView == nullptr)
            {
                bakedGiViewsAreTexture3D = false;
                continue;
            }
            nativeView->GetDesc(&description);
            bakedGiViewsAreTexture3D = bakedGiViewsAreTexture3D
                && view.Kind()
                    == LamaPon::GraphicsViewKind::ShaderResource
                && description.Format
                    == DXGI_FORMAT_R16G16B16A16_FLOAT
                && description.ViewDimension
                    == D3D11_SRV_DIMENSION_TEXTURE3D;
        }
        Require(
            bakedGiViewsAreTexture3D,
            "Baked GI upload did not create three neutral Texture3D views");
        const auto invalidBakedGiViews =
            graphics.UploadBakedGlobalIlluminationViews(
                1,
                1,
                1,
                std::span{ bakedGiCoefficients }.first<11>());
        Require(
            !invalidBakedGiViews[0]
                && !invalidBakedGiViews[1]
                && !invalidBakedGiViews[2],
            "An invalid Baked GI payload returned partial neutral views");

        // RuntimeのAssetManagerはactive Backendを受け取り、通常画像・文字・
        // DDSの所有権をneutral handleへ置きます。raw SRVは同じhandleを
        // 解決したDirectX 11互換mirrorでなければなりません。
        auto& assets = graphics.Assets();
        const auto builtInTexture = assets.LoadTexture(
            L"builtin/circle");
        const auto builtInResources =
            builtInTexture->resources.Acquire();
        Require(
            builtInResources != nullptr
                && builtInResources->texture
                && builtInResources->shaderResourceView
                && builtInResources->d3d11ShaderResourceView != nullptr
                && graphics.TryResolveD3D11ShaderResourceView(
                    builtInResources->shaderResourceView)
                    == builtInResources->d3d11ShaderResourceView.Get(),
            "Built-in texture handles diverged from the D3D11 mirror");
        const auto textTexture = assets.LoadTextTexture(
            "Rendering API",
            "Yu Gothic UI",
            18.0f);
        const auto textResources =
            textTexture->resources.Acquire();
        Require(
            textResources != nullptr
                && textResources->texture
                && textResources->shaderResourceView
                && textResources->d3d11ShaderResourceView != nullptr
                && graphics.TryResolveD3D11ShaderResourceView(
                    textResources->shaderResourceView)
                    == textResources->d3d11ShaderResourceView.Get(),
            "Text texture handles diverged from the D3D11 mirror");

        // writerがA/Bを繰り返し公開している間も、readerにはtexture・view・
        // compatibility mirrorが必ず同じ世代の組として見えることを確認します。
        LamaPon::TextureResourceBinding concurrentBinding;
        const auto resourcesA = *builtInResources;
        const auto resourcesB = *textResources;
        concurrentBinding.Publish(resourcesA);
        std::atomic_int concurrentPhase{};
        std::atomic_bool writerFinished{};
        std::atomic_bool coherentSnapshots{ true };
        const auto matchesSnapshot =
            [](const std::shared_ptr<
                    const LamaPon::TextureResourceSnapshot>& current,
                const LamaPon::TextureResourceSnapshot& expected)
            {
                return current != nullptr
                    && current->texture == expected.texture
                    && current->shaderResourceView
                        == expected.shaderResourceView
                    && current->d3d11ShaderResourceView.Get()
                        == expected.d3d11ShaderResourceView.Get();
            };
        std::thread snapshotWriter(
            [&]
            {
                // 最初のB/Aはreaderのackを待ち、別threadで両世代を必ず
                // 観測させます。その後は同期せずpublishを繰り返します。
                concurrentBinding.Publish(resourcesB);
                concurrentPhase.store(1, std::memory_order_release);
                while (concurrentPhase.load(std::memory_order_acquire) < 2)
                {
                    std::this_thread::yield();
                }
                concurrentBinding.Publish(resourcesA);
                concurrentPhase.store(3, std::memory_order_release);
                while (concurrentPhase.load(std::memory_order_acquire) < 5)
                {
                    std::this_thread::yield();
                }
                for (int index = 0; index < 20000; ++index)
                {
                    concurrentBinding.Publish(
                        (index & 1) == 0
                            ? resourcesA
                            : resourcesB);
                }
                writerFinished.store(true, std::memory_order_release);
            });
        std::thread snapshotReader(
            [&]
            {
                while (concurrentPhase.load(std::memory_order_acquire) < 1)
                {
                    std::this_thread::yield();
                }
                if (!matchesSnapshot(
                    concurrentBinding.Acquire(),
                    resourcesB))
                {
                    coherentSnapshots.store(false);
                }
                concurrentPhase.store(2, std::memory_order_release);
                while (concurrentPhase.load(std::memory_order_acquire) < 3)
                {
                    std::this_thread::yield();
                }
                if (!matchesSnapshot(
                    concurrentBinding.Acquire(),
                    resourcesA))
                {
                    coherentSnapshots.store(false);
                }
                concurrentPhase.store(5, std::memory_order_release);
                while (!writerFinished.load(std::memory_order_acquire))
                {
                    const auto current = concurrentBinding.Acquire();
                    if (!matchesSnapshot(current, resourcesA)
                        && !matchesSnapshot(current, resourcesB))
                    {
                        coherentSnapshots.store(
                            false,
                            std::memory_order_relaxed);
                        break;
                    }
                }
            });
        snapshotWriter.join();
        snapshotReader.join();
        Require(
            coherentSnapshots.load(std::memory_order_relaxed),
            "Concurrent texture publication exposed a torn snapshot");

        const auto ddsTexture = assets.LoadTexture(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }
                / L"models/arrow.fbm_arrow.dds");
        const auto ddsResources =
            ddsTexture->resources.Acquire();
        Require(
            ddsResources != nullptr
                && ddsResources->texture
                && ddsResources->shaderResourceView
                && ddsResources->d3d11ShaderResourceView != nullptr
                && graphics.TryResolveD3D11ShaderResourceView(
                    ddsResources->shaderResourceView)
                    == ddsResources->d3d11ShaderResourceView.Get(),
            "DDS import did not enter the active backend generation");

        assets.SetProgressiveUploadThreshold(1);
        const auto progressiveTexture = assets.LoadTexture(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }
                / L"textures/LamaPonEngineLogo.png");
        const auto placeholderResources =
            progressiveTexture->resources.Acquire();
        Require(placeholderResources != nullptr,
            "Progressive loading did not publish a resource snapshot");
        const auto placeholderTextureHandle =
            placeholderResources->texture;
        const auto placeholderViewHandle =
            placeholderResources->shaderResourceView;
        Require(
            placeholderTextureHandle
                && placeholderViewHandle
                && assets.PendingTextureUploadCount() == 1u,
            "Backend-aware progressive loading did not publish a placeholder");
        assets.PumpTextureUploads(1);
        const auto firstProgressiveResources =
            progressiveTexture->resources.Acquire();
        Require(
            firstProgressiveResources != nullptr
                && firstProgressiveResources->texture
                    != placeholderTextureHandle
                && firstProgressiveResources->shaderResourceView
                    != placeholderViewHandle
                && graphics.TryResolveD3D11ShaderResourceView(
                    firstProgressiveResources->shaderResourceView)
                    == firstProgressiveResources
                        ->d3d11ShaderResourceView.Get()
                && graphics.TryResolveD3D11ShaderResourceView(
                    placeholderViewHandle) != nullptr,
            "The first progressive upload did not transactionally publish "
            "the final texture generation");
        for (int index = 0;
            index < 64
                && assets.PendingTextureUploadCount() != 0;
            ++index)
        {
            assets.PumpTextureUploads(1u << 30);
        }
        const auto finalProgressiveResources =
            progressiveTexture->resources.Acquire();
        Require(
            assets.PendingTextureUploadCount() == 0
                && finalProgressiveResources != nullptr
                && finalProgressiveResources->texture
                    != placeholderTextureHandle
                && graphics.TryResolveD3D11ShaderResourceView(
                    finalProgressiveResources->shaderResourceView)
                    == finalProgressiveResources
                        ->d3d11ShaderResourceView.Get(),
            "Backend-aware progressive loading did not publish its final view");
        assets.SetProgressiveUploadThreshold(
            LamaPon::AssetManager::DefaultProgressiveUploadThreshold);

        const auto colorVariant = assets.LoadTexture(
            L"builtin/triangle",
            LamaPon::TextureLoader::TextureUsage::Color);
        const auto normalVariant = assets.LoadTexture(
            L"builtin/triangle",
            LamaPon::TextureLoader::TextureUsage::NormalMap);
        const auto dataVariant = assets.LoadTexture(
            L"builtin/triangle",
            LamaPon::TextureLoader::TextureUsage::DataMap);
        assets.Invalidate(L"builtin/triangle");
        Require(
            assets.LoadTexture(
                L"builtin/triangle",
                LamaPon::TextureLoader::TextureUsage::Color)
                    != colorVariant
                && assets.LoadTexture(
                    L"builtin/triangle",
                    LamaPon::TextureLoader::TextureUsage::NormalMap)
                    != normalVariant
                && assets.LoadTexture(
                    L"builtin/triangle",
                    LamaPon::TextureLoader::TextureUsage::DataMap)
                    != dataVariant,
            "Texture invalidation retained a usage-specific cache variant");

        Microsoft::WRL::ComPtr<ID3D11Device> whiteDevice;
        Microsoft::WRL::ComPtr<ID3D11Device> blendDevice;
        Microsoft::WRL::ComPtr<ID3D11Device> instanceDevice;
        rebuiltWhiteD3D11View->GetDevice(
            whiteDevice.ReleaseAndGetAddressOf());
        graphics.AdditiveBlendPreservingAlpha()->GetDevice(
            blendDevice.ReleaseAndGetAddressOf());
        grownBoundInstanceBuffer.buffer->GetDevice(
            instanceDevice.ReleaseAndGetAddressOf());
        Require(
            whiteDevice.Get() == graphics.Device()
                && blendDevice.Get() == graphics.Device()
                && instanceDevice.Get() == graphics.Device(),
            "Reinitialized compatibility resources belong to the old device");
        TestGraphicsOutputState foreignOutputState;
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                graphics.RestoreOutputState(
                    foreignOutputState);
            },
            "Output state from another backend must be rejected");

        // ShadowMap::Begin/Endが従来から持つsilent no-opを、
        // Backend経由でも維持します。
        LamaPon::ShadowMap emptyShadowMap;
        graphics.BeginShadowMap(emptyShadowMap, 0);
        graphics.EndShadowMap(emptyShadowMap);

        // ライトが無いフレームは、前回値を残さず無効へ戻します。
        LamaPon::LightingState emptyClusteredLighting;
        emptyClusteredLighting.clustered.enabled = true;
        emptyClusteredLighting.clustered.lightCount = 123u;
        const auto emptyClusteredIdentity =
            DirectX::XMMatrixIdentity();
        graphics.UpdateClusteredLights(
            emptyClusteredLighting,
            emptyClusteredIdentity,
            emptyClusteredIdentity,
            Width,
            Height);
        Require(
            !emptyClusteredLighting.clustered.enabled
                && emptyClusteredLighting.clustered.lightCount == 0u
                && !emptyClusteredLighting.clustered.lights
                && !emptyClusteredLighting.clustered.lightIndices
                && !emptyClusteredLighting.clustered.clusterCounts,
            "Empty clustered lighting must clear the previous result");

        constexpr float displayColor[]{
            0.1f, 0.85f, 0.2f, 1.0f };
        const DirectX::XMFLOAT4X4 historyViewProjection{
            1.0f, 2.0f, 3.0f, 4.0f,
            5.0f, 6.0f, 7.0f, 8.0f,
            9.0f, 10.0f, 11.0f, 12.0f,
            13.0f, 14.0f, 15.0f, 16.0f };
        LamaPon::AutoExposureSettings autoExposureSettings{};
        autoExposureSettings.enabled = true;
        LamaPon::RenderTarget emptyOffscreenTarget;
        RequireThrows<std::invalid_argument>(
            [&]
            {
                graphics.BeginOffscreenTarget(
                    emptyOffscreenTarget,
                    displayColor);
            },
            "Beginning an empty offscreen target must be rejected");
        RequireThrows<std::invalid_argument>(
            [&]
            {
                graphics.BindOffscreenTarget(
                    emptyOffscreenTarget);
            },
            "Binding an empty offscreen target must be rejected");
        RequireThrows<std::invalid_argument>(
            [&]
            {
                graphics.BindOffscreenTargetDepthOnly(
                    emptyOffscreenTarget);
            },
            "Binding an empty offscreen depth target must be rejected");
        RequireThrows<std::invalid_argument>(
            [&]
            {
                graphics.CaptureOffscreenTargetDepth(
                    emptyOffscreenTarget);
            },
            "Capturing an empty offscreen depth target must be rejected");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                graphics.CaptureOffscreenTargetColorHistory(
                    emptyOffscreenTarget,
                    historyViewProjection);
            },
            "Capturing color history from an empty offscreen target must be rejected");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                graphics.CaptureOffscreenTargetTemporalHistory(
                    emptyOffscreenTarget,
                    historyViewProjection);
            },
            "Capturing temporal history from an empty offscreen target must be rejected");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                static_cast<void>(
                    graphics.UpdateOffscreenTargetAutoExposure(
                        emptyOffscreenTarget,
                        autoExposureSettings,
                        1.0f / 60.0f));
            },
            "Updating auto exposure for an empty offscreen target must be rejected");
        RequireThrows<std::invalid_argument>(
            [&]
            {
                graphics.PublishOffscreenTarget(
                    emptyOffscreenTarget);
            },
            "Publishing an empty offscreen target must be rejected");

        ImGuiContextScope imguiContext;
        auto renderer = LamaPon::CreateEditorGuiRenderer(
            graphics.ActiveRenderingApi());

        constexpr std::array<std::uint8_t, 4> assetColor{
            224u, 48u, 32u, 255u };
        auto textureAsset = CreateSolidTexture(graphics, assetColor);
        LamaPon::RenderTarget displayTarget;
        graphics.ResizeOffscreenTarget(displayTarget, 0, 0);
        Require(
            displayTarget.IsValid()
                && displayTarget.Width() == 1u
                && displayTarget.Height() == 1u,
            "Offscreen target dimensions must be clamped to at least one");
        const auto firstAmbientOcclusionView =
            displayTarget.AmbientOcclusionViewHandle();
        const auto firstReflectionDepthView =
            displayTarget.ReflectionDepthPyramidViewHandle();
        const auto firstDepthView = displayTarget.DepthViewHandle();
        Require(
            firstAmbientOcclusionView
                && firstReflectionDepthView
                && firstDepthView
                && !displayTarget.ColorHistoryViewHandle(),
            "Offscreen screen-space views were not published atomically");
        graphics.ResizeOffscreenTarget(displayTarget, 8, 4);
        Require(
            displayTarget.IsValid()
                && displayTarget.Width() == 8u
                && displayTarget.Height() == 4u,
            "Offscreen target resize must apply the requested dimensions");
        Require(
            displayTarget.AmbientOcclusionViewHandle()
                    != firstAmbientOcclusionView
                && displayTarget.ReflectionDepthPyramidViewHandle()
                    != firstReflectionDepthView
                && displayTarget.DepthViewHandle() != firstDepthView
                && !displayTarget.ColorHistoryViewHandle(),
            "Offscreen resize retained stale screen-space views");
        const auto resizedAmbientOcclusionView =
            displayTarget.AmbientOcclusionViewHandle();
        const auto resizedReflectionDepthView =
            displayTarget.ReflectionDepthPyramidViewHandle();
        const auto resizedDepthView = displayTarget.DepthViewHandle();
        graphics.ResizeOffscreenTarget(displayTarget, 8, 4);
        Require(
            displayTarget.AmbientOcclusionViewHandle()
                    == resizedAmbientOcclusionView
                && displayTarget.ReflectionDepthPyramidViewHandle()
                    == resizedReflectionDepthView
                && displayTarget.DepthViewHandle() == resizedDepthView,
            "A no-op offscreen resize rebuilt neutral views");
        RequireThrows<std::invalid_argument>(
            [&]
            {
                graphics.BeginOffscreenTarget(
                    displayTarget,
                    nullptr);
            },
            "Beginning an offscreen target with no clear color must be rejected");

        graphics.BeginOffscreenTarget(displayTarget, displayColor);
        // 深度専用bindはカラーRTVを外し、同じターゲットのDSVと
        // viewportだけを設定します。また、直前のLit描画で残り得る
        // t0〜t15を解除して、深度を次の描画先として安全に使える状態へ
        // 戻します。
        std::array<ID3D11ShaderResourceView*, 16> testResources{};
        testResources.fill(rebuiltWhiteD3D11View);
        graphics.Context()->PSSetShaderResources(
            0,
            static_cast<UINT>(testResources.size()),
            testResources.data());
        graphics.BindOffscreenTargetDepthOnly(displayTarget);

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            depthOnlyColorTarget;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            depthOnlyDepthTarget;
        graphics.Context()->OMGetRenderTargets(
            1,
            depthOnlyColorTarget.ReleaseAndGetAddressOf(),
            depthOnlyDepthTarget.ReleaseAndGetAddressOf());
        Require(
            depthOnlyColorTarget.Get() == nullptr
                && depthOnlyDepthTarget.Get() != nullptr,
            "Depth-only binding must keep a depth target without a color target");

        D3D11_VIEWPORT depthOnlyViewport{};
        UINT depthOnlyViewportCount = 1;
        graphics.Context()->RSGetViewports(
            &depthOnlyViewportCount,
            &depthOnlyViewport);
        Require(
            depthOnlyViewportCount == 1
                && depthOnlyViewport.Width == 8.0f
                && depthOnlyViewport.Height == 4.0f,
            "Depth-only binding must apply the offscreen target viewport");

        std::array<ID3D11ShaderResourceView*, 16>
            boundResources{};
        graphics.Context()->PSGetShaderResources(
            0,
            static_cast<UINT>(boundResources.size()),
            boundResources.data());
        bool allResourcesUnbound = true;
        for (auto*& resource : boundResources)
        {
            if (resource != nullptr)
            {
                allResourcesUnbound = false;
                resource->Release();
                resource = nullptr;
            }
        }
        Require(
            allResourcesUnbound,
            "Depth-only binding must unbind pixel shader resources t0 through t15");

        // SSRと同じ順序で、DSVが刺さっている深度を読取用資源へ控えます。
        // この操作と深度専用bindはいずれも描画内容を消さないため、カラーへ
        // 戻した後の既存画素検証がそのまま境界の回帰検証になります。
        graphics.CaptureOffscreenTargetDepth(displayTarget);
        graphics.BindOffscreenTarget(displayTarget);
        constexpr DirectX::XMFLOAT4 leftColor{
            0.9f, 0.1f, 0.05f, 1.0f };
        DrawSolidRectangle(
            graphics,
            { 0.0f, 0.0f },
            { 2.0f, 4.0f },
            leftColor);

        LamaPon::RenderTarget diversionTarget;
        graphics.ResizeOffscreenTarget(diversionTarget, 8, 4);
        constexpr float diversionColor[]{
            0.02f, 0.03f, 0.04f, 1.0f };
        graphics.BeginOffscreenTarget(
            diversionTarget,
            diversionColor);
        graphics.BindOffscreenTarget(displayTarget);
        constexpr DirectX::XMFLOAT4 rightColor{
            0.05f, 0.2f, 0.9f, 1.0f };
        DrawSolidRectangle(
            graphics,
            { 6.0f, 0.0f },
            { 2.0f, 4.0f },
            rightColor);

        // 履歴用コピーは現在の描画色を変えません。両履歴を控えた後に
        // publishし、下のImGui画像に対する左・中央・右の画素検証で
        // 描画済みの内容が保たれていることも確認します。
        Require(
            !displayTarget.ColorHistoryViewHandle(),
            "Color history must be unavailable before its first capture");
        graphics.CaptureOffscreenTargetColorHistory(
            displayTarget,
            historyViewProjection);
        graphics.CaptureOffscreenTargetTemporalHistory(
            displayTarget,
            historyViewProjection);
        const auto capturedColorHistory =
            displayTarget.ColorHistoryViewHandle();
        Require(
            capturedColorHistory
                && graphics.TryResolveD3D11ShaderResourceView(
                    capturedColorHistory) != nullptr,
            "Color history must become available after capture");
        graphics.ResizeOffscreenTarget(displayTarget, 8, 4);
        Require(
            displayTarget.ColorHistoryViewHandle()
                == capturedColorHistory,
            "A no-op offscreen resize discarded valid color history");
        const auto& storedHistoryViewProjection =
            displayTarget.ColorHistoryViewProjection();
        Require(
            storedHistoryViewProjection._11
                    == historyViewProjection._11
                && storedHistoryViewProjection._12
                    == historyViewProjection._12
                && storedHistoryViewProjection._13
                    == historyViewProjection._13
                && storedHistoryViewProjection._14
                    == historyViewProjection._14
                && storedHistoryViewProjection._21
                    == historyViewProjection._21
                && storedHistoryViewProjection._22
                    == historyViewProjection._22
                && storedHistoryViewProjection._23
                    == historyViewProjection._23
                && storedHistoryViewProjection._24
                    == historyViewProjection._24
                && storedHistoryViewProjection._31
                    == historyViewProjection._31
                && storedHistoryViewProjection._32
                    == historyViewProjection._32
                && storedHistoryViewProjection._33
                    == historyViewProjection._33
                && storedHistoryViewProjection._34
                    == historyViewProjection._34
                && storedHistoryViewProjection._41
                    == historyViewProjection._41
                && storedHistoryViewProjection._42
                    == historyViewProjection._42
                && storedHistoryViewProjection._43
                    == historyViewProjection._43
                && storedHistoryViewProjection._44
                    == historyViewProjection._44,
            "Color history capture must preserve its view-projection matrix");
        graphics.PublishOffscreenTarget(displayTarget);

        RequireThrows<std::logic_error>(
            [&]
            {
                static_cast<void>(
                    renderer->TextureReference(textureAsset));
            },
            "An uninitialized editor GUI renderer must reject asset textures");
        RequireThrows<std::logic_error>(
            [&]
            {
                static_cast<void>(
                    renderer->DisplayTextureReference(displayTarget));
            },
            "An uninitialized editor GUI renderer must reject display textures");

        renderer->Initialize(graphics);
        Require(renderer->IsInitialized(),
            "DirectX 11 editor GUI renderer initialization failed");
        Microsoft::WRL::ComPtr<ID3D11Device>
            rendererDevice = graphics.Device();
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.Initialize(
                    window.Get(),
                    Width,
                    Height,
                    LamaPon::RenderingApi::DirectX11);
            },
            "Graphics reinitialization accepted an active editor GUI renderer");
        Require(
            graphics.Device() == rendererDevice.Get()
                && renderer->IsInitialized(),
            "Rejected editor GUI reinitialization changed active state");

        renderer->NewFrame();
        ImGui::NewFrame();

        LamaPon::TextureAsset emptyTextureAsset;
        LamaPon::RenderTarget emptyDisplayTarget;
        RequireThrows<std::invalid_argument>(
            [&]
            {
                static_cast<void>(
                    renderer->TextureReference(emptyTextureAsset));
            },
            "Editor GUI renderer must reject an asset with no texture view");
        RequireThrows<std::invalid_argument>(
            [&]
            {
                static_cast<void>(
                    renderer->DisplayTextureReference(
                        emptyDisplayTarget));
            },
            "Editor GUI renderer must reject an empty display target");

        const auto assetTextureReference =
            renderer->TextureReference(textureAsset);
        const auto displayTextureReference =
            renderer->DisplayTextureReference(displayTarget);
        ID3D11ShaderResourceView* originalAssetView{};
        {
            const auto assetResources =
                textureAsset.resources.Acquire();
            Require(assetResources != nullptr,
                "The editor GUI test asset must publish a resource snapshot");
            originalAssetView =
                graphics.TryResolveD3D11ShaderResourceView(
                    *assetResources);
            Require(
                originalAssetView != nullptr
                    && assetTextureReference.GetTexID()
                        == ExpectedTextureId(originalAssetView),
                "Asset texture reference must contain its DirectX 11 SRV");
        }
        Require(
            displayTextureReference.GetTexID()
                == ExpectedTextureId(
                    displayTarget.DisplayShaderResourceView()),
            "Display texture reference must contain its DirectX 11 SRV");

        ImGui::SetNextWindowPos(ImVec2(4.0f, 4.0f));
        ImGui::SetNextWindowSize(ImVec2(88.0f, 56.0f));
        ImGui::Begin("Editor GUI backend smoke test");
        ImGui::TextUnformatted("DirectX 11 fallback");
        ImGui::End();
        DrawImageWindow(
            "Asset texture",
            ImVec2{ 4.0f, 8.0f },
            assetTextureReference);
        DrawImageWindow(
            "Render target texture",
            ImVec2{ 52.0f, 8.0f },
            displayTextureReference);
        ImGui::Render();

        // ImGui draw commands retain only the numeric texture ID. Replacing
        // the asset snapshot after command construction must not invalidate
        // that SRV before RenderDrawData consumes the commands.
        constexpr std::array<std::uint8_t, 4> replacementAssetColor{
            32u, 208u, 96u, 255u };
        PublishSolidTexture(
            textureAsset,
            graphics,
            replacementAssetColor);
        const auto replacementResources =
            textureAsset.resources.Acquire();
        Require(
            replacementResources != nullptr
                && graphics.TryResolveD3D11ShaderResourceView(
                    *replacementResources) != originalAssetView,
            "Replacing an asset snapshot must publish a distinct SRV");

        constexpr float clearColor[]{
            0.05f, 0.1f, 0.15f, 1.0f };
        graphics.BeginFrame(clearColor);
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            expectedRenderTarget;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            expectedDepthTarget;
        graphics.Context()->OMGetRenderTargets(
            1,
            expectedRenderTarget.ReleaseAndGetAddressOf(),
            expectedDepthTarget.ReleaseAndGetAddressOf());
        D3D11_VIEWPORT expectedViewport{};
        UINT expectedViewportCount = 1;
        graphics.Context()->RSGetViewports(
            &expectedViewportCount,
            &expectedViewport);
        auto backBufferOutputState =
            graphics.CaptureOutputState();
        graphics.BeginOffscreenTarget(
            diversionTarget,
            diversionColor);
        graphics.RestoreOutputState(
            *backBufferOutputState);
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            restoredRenderTarget;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            restoredDepthTarget;
        graphics.Context()->OMGetRenderTargets(
            1,
            restoredRenderTarget.ReleaseAndGetAddressOf(),
            restoredDepthTarget.ReleaseAndGetAddressOf());
        D3D11_VIEWPORT restoredViewport{};
        UINT restoredViewportCount = 1;
        graphics.Context()->RSGetViewports(
            &restoredViewportCount,
            &restoredViewport);
        Require(
            expectedRenderTarget.Get() != nullptr
                && expectedDepthTarget.Get() != nullptr
                && expectedViewportCount == 1
                && restoredRenderTarget.Get()
                    == expectedRenderTarget.Get()
                && restoredDepthTarget.Get()
                    == expectedDepthTarget.Get()
                && restoredViewportCount
                    == expectedViewportCount
                && restoredViewport.TopLeftX
                    == expectedViewport.TopLeftX
                && restoredViewport.TopLeftY
                    == expectedViewport.TopLeftY
                && restoredViewport.Width
                    == expectedViewport.Width
                && restoredViewport.Height
                    == expectedViewport.Height
                && restoredViewport.MinDepth
                    == expectedViewport.MinDepth
                && restoredViewport.MaxDepth
                    == expectedViewport.MaxDepth,
            "Output state restore must recover targets and viewport");
        renderer->RenderDrawData(ImGui::GetDrawData());
        std::uint32_t capturedWidth{};
        std::uint32_t capturedHeight{};
        const auto pixels = graphics.CaptureBackBuffer(
            capturedWidth,
            capturedHeight);
        graphics.EndFrame();

        Require(
            capturedWidth == Width
                && capturedHeight == Height
                && pixels.size()
                    == static_cast<std::size_t>(Width)
                        * Height * 4u,
            "Editor GUI renderer must draw to the active back buffer");
        bool containsGuiPixel{};
        for (std::size_t offset = 0;
            offset + 2u < pixels.size();
            offset += 4u)
        {
            // R8G8B8A8_UNORMへ書いたclear色は概ね(13, 26, 38)。
            // Dear ImGuiのwindow/textが描かれれば、この範囲外の画素が
            // 必ず現れます。
            if (pixels[offset] < 5u || pixels[offset] > 21u
                || pixels[offset + 1u] < 18u
                || pixels[offset + 1u] > 34u
                || pixels[offset + 2u] < 30u
                || pixels[offset + 2u] > 46u)
            {
                containsGuiPixel = true;
                break;
            }
        }
        Require(containsGuiPixel,
            "Dear ImGui draw data must change the cleared back buffer");
        RequirePixelNear(
            pixels,
            24u,
            32u,
            { assetColor[0], assetColor[1], assetColor[2] },
            "ImGui::Image must sample the asset texture reference");
        RequirePixelNear(
            pixels,
            59u,
            32u,
            { 230u, 26u, 13u },
            "Beginning an offscreen target must bind it for drawing");
        RequirePixelNear(
            pixels,
            72u,
            32u,
            { 26u, 217u, 51u },
            "Beginning an offscreen target must clear it before drawing");
        RequirePixelNear(
            pixels,
            84u,
            32u,
            { 13u, 51u, 230u },
            "Binding must restore and publishing must expose the offscreen target");

        // API非依存passはhandleを強所有し、nested Beginをpin破棄より
        // 前に拒否します。stale handleは白へ化けず、
        // 同じpassの後続Drawを壊しません。
        auto neutralSpriteAsset =
            CreateSolidTexture(graphics, assetColor);
        auto neutralSpriteResources =
            neutralSpriteAsset.resources.Acquire();
        Require(neutralSpriteResources != nullptr,
            "Neutral sprite test texture was not published");
        LamaPon::SpriteDrawRequest neutralRequest;
        neutralRequest.texture =
            neutralSpriteResources->shaderResourceView;
        neutralRequest.position = { 16.0f, 0.0f };
        neutralRequest.scale = { 16.0f, 16.0f };
        constexpr float neutralClearColor[]{
            0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(neutralClearColor);
        auto neutralPass = graphics.BeginSpritePass();
        auto neutralContext = neutralPass.Context();
        Require(neutralPass.Draw(neutralRequest),
            "A current neutral sprite view was rejected");
        neutralRequest.texture.Reset();
        neutralSpriteResources.reset();
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                static_cast<void>(graphics.BeginSpritePass());
            },
            "A nested neutral sprite pass was accepted");
        PublishSolidTexture(
            neutralSpriteAsset,
            graphics,
            replacementAssetColor);

        neutralRequest.texture = previousWhiteView;
        neutralRequest.position = { 32.0f, 0.0f };
        Require(!neutralContext.Draw(neutralRequest),
            "A stale sprite view was accepted by the current backend");
        const auto neutralReplacementResources =
            neutralSpriteAsset.resources.Acquire();
        Require(neutralReplacementResources != nullptr,
            "Replacement neutral sprite texture was not published");
        neutralRequest.texture =
            neutralReplacementResources->shaderResourceView;
        Require(neutralContext.Draw(neutralRequest),
            "A valid draw after a stale handle was rejected");

        neutralRequest.texture.Reset();
        neutralRequest.position = { 48.0f, 0.0f };
        neutralRequest.tint = { 0.0f, 0.0f, 1.0f, 1.0f };
        Require(neutralPass.Draw(neutralRequest),
            "An empty sprite view did not use the white texture fallback");
        neutralPass.End();
        Require(!neutralContext && !neutralContext.Draw(neutralRequest),
            "A neutral context remained active after End");
        std::uint32_t neutralWidth{};
        std::uint32_t neutralHeight{};
        const auto neutralPixels = graphics.CaptureBackBuffer(
            neutralWidth,
            neutralHeight);
        graphics.EndFrame();
        Require(
            neutralWidth == Width && neutralHeight == Height,
            "Neutral sprite test must capture the active back buffer");
        RequirePixelNear(
            neutralPixels,
            24u,
            8u,
            { assetColor[0], assetColor[1], assetColor[2] },
            "A neutral sprite pass did not retain its original view");
        RequirePixelNear(
            neutralPixels,
            40u,
            8u,
            {
                replacementAssetColor[0],
                replacementAssetColor[1],
                replacementAssetColor[2]
            },
            "A valid draw after a stale view did not reach the batch");
        RequirePixelNear(
            neutralPixels,
            56u,
            8u,
            { 0u, 0u, 255u },
            "The neutral white texture fallback did not preserve tint");

        constexpr std::array<std::uint8_t, 12> threePixelColors{
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u };
        const std::array threePixelData{
            LamaPon::GraphicsTextureSubresourceData{
                std::as_bytes(std::span{ threePixelColors }),
                12u,
                12u
            }
        };
        const auto threePixelTexture = graphics.CreateTexture2D(
            LamaPon::GraphicsTexture2DDescription{
                3u,
                1u,
                1u,
                LamaPon::GraphicsTextureFormat::Rgba8Unorm,
                LamaPon::GraphicsTextureUpdateMode::Immutable
            },
            threePixelData);
        const auto threePixelView =
            graphics.CreateShaderResourceView(
                threePixelTexture,
                { 0u, 1u });
        LamaPon::SpritePassDescription flipPassDescription;
        flipPassDescription.blend =
            LamaPon::SpriteBlendMode::Opaque;
        LamaPon::SpriteDrawRequest flipRequest;
        flipRequest.texture = threePixelView;
        flipRequest.hasSourceRectangle = true;
        flipRequest.sourceRectangle = { 0, 0, 2, 1 };
        flipRequest.scale = { 16.0f, 16.0f };
        flipRequest.flip = LamaPon::SpriteFlip::Horizontal;
        graphics.BeginFrame(neutralClearColor);
        auto flipPass = graphics.BeginSpritePass(
            flipPassDescription);
        Require(flipPass.Draw(flipRequest),
            "A neutral source rectangle and flip were rejected");
        flipPass.End();
        std::uint32_t flipWidth{};
        std::uint32_t flipHeight{};
        const auto flipPixels = graphics.CaptureBackBuffer(
            flipWidth,
            flipHeight);
        graphics.EndFrame();
        Require(
            flipWidth == Width && flipHeight == Height,
            "Neutral flip test must capture the active back buffer");
        const auto flippedLeft =
            (static_cast<std::size_t>(8u) * Width + 8u) * 4u;
        const auto flippedRight =
            (static_cast<std::size_t>(8u) * Width + 24u) * 4u;
        Require(
            flipPixels[flippedLeft + 1u]
                    > flipPixels[flippedLeft] + 64u
                && flipPixels[flippedRight]
                    > flipPixels[flippedRight + 1u] + 64u,
            "Sprite source selection and horizontal flip were not applied");

        // 入れ子のUIシザーは外側との交差だけを描画し、余分なPopは
        // 進行中のSprite passを壊さないことを実画素で確認します。
        constexpr float scissorClearColor[]{
            0.0f, 0.0f, 0.0f, 1.0f };
        const DirectX::XMFLOAT4 scissorRed{
            1.0f, 0.0f, 0.0f, 1.0f };
        const DirectX::XMFLOAT4 scissorGreen{
            0.0f, 1.0f, 0.0f, 1.0f };
        graphics.BeginFrame(scissorClearColor);
        auto scissorPass = graphics.BeginSpritePass();
        const auto drawScissorColor =
            [&scissorPass](
                const DirectX::XMFLOAT4& color)
            {
                LamaPon::SpriteDrawRequest request;
                request.scale = {
                    static_cast<float>(Width),
                    static_cast<float>(Height) };
                request.tint = color;
                Require(scissorPass.Draw(request),
                    "A scissored neutral sprite draw was rejected");
            };
        Require(scissorPass.PushScissor(
                { 8.0f, 8.0f, 56.0f, 48.0f }),
            "The outer sprite scissor was rejected");
        drawScissorColor(scissorRed);
        Require(scissorPass.PushScissor(
                { 24.0f, 16.0f, 72.0f, 32.0f }),
            "The inner sprite scissor was rejected");
        drawScissorColor(scissorGreen);
        Require(scissorPass.PopScissor(),
            "The inner sprite scissor could not be popped");
        Require(scissorPass.PopScissor(),
            "The outer sprite scissor could not be popped");
        Require(!scissorPass.PopScissor(),
            "An extra sprite scissor pop was accepted");
        scissorPass.End();
        std::uint32_t scissorWidth{};
        std::uint32_t scissorHeight{};
        const auto scissorPixels = graphics.CaptureBackBuffer(
            scissorWidth,
            scissorHeight);
        graphics.EndFrame();
        Require(
            scissorWidth == Width && scissorHeight == Height,
            "Nested UI scissor test must capture the active back buffer");
        RequirePixelNear(
            scissorPixels,
            12u,
            12u,
            { 255u, 0u, 0u },
            "The outer-only scissor region must retain the outer draw");
        RequirePixelNear(
            scissorPixels,
            32u,
            24u,
            { 0u, 255u, 0u },
            "The nested scissor intersection must contain the inner draw");
        RequirePixelNear(
            scissorPixels,
            64u,
            24u,
            { 0u, 0u, 0u },
            "The nested scissor must not draw outside its outer region");
        RequirePixelNear(
            scissorPixels,
            4u,
            4u,
            { 0u, 0u, 0u },
            "The outer UI scissor must preserve pixels outside its bounds");

        // Opaque blendを指定したneutral passでも、scissorの内部flush後に
        // blend設定がNonPremultipliedへ戻らないことを確認します。
        LamaPon::SpritePassDescription opaqueSpriteDescription;
        opaqueSpriteDescription.blend =
            LamaPon::SpriteBlendMode::Opaque;
        LamaPon::SpriteDrawRequest opaqueSpriteRequest;
        opaqueSpriteRequest.scale = {
            static_cast<float>(Width),
            static_cast<float>(Height) };
        opaqueSpriteRequest.tint = {
            1.0f, 0.0f, 0.0f, 0.25f };
        graphics.BeginFrame(scissorClearColor);
        auto opaqueSpritePass = graphics.BeginSpritePass(
            opaqueSpriteDescription);
        Require(opaqueSpritePass.PushScissor(
                { 8.0f, 8.0f, 56.0f, 48.0f })
                && opaqueSpritePass.Draw(opaqueSpriteRequest),
            "The neutral outer scissor rejected a draw");
        opaqueSpriteRequest.tint = {
            0.0f, 1.0f, 0.0f, 0.25f };
        Require(opaqueSpritePass.PushScissor(
                { 24.0f, 16.0f, 72.0f, 32.0f })
                && opaqueSpritePass.Draw(opaqueSpriteRequest)
                && opaqueSpritePass.PopScissor(),
            "The neutral inner scissor could not be restored");
        opaqueSpriteRequest.position = { 8.0f, 32.0f };
        opaqueSpriteRequest.scale = { 16.0f, 8.0f };
        opaqueSpriteRequest.tint = {
            0.0f, 0.0f, 1.0f, 1.0f };
        Require(opaqueSpritePass.Draw(opaqueSpriteRequest),
            "A draw after restoring the outer scissor was rejected");
        Require(
            opaqueSpritePass.PopScissor()
                && !opaqueSpritePass.PopScissor(),
            "The neutral outer scissor stack was not balanced");
        opaqueSpriteRequest.position = { 0.0f, 0.0f };
        opaqueSpriteRequest.scale = { 8.0f, 8.0f };
        opaqueSpriteRequest.tint = {
            1.0f, 1.0f, 0.0f, 1.0f };
        Require(opaqueSpritePass.Draw(opaqueSpriteRequest),
            "A draw after removing every scissor was rejected");
        opaqueSpritePass.End();
        std::uint32_t opaqueScissorWidth{};
        std::uint32_t opaqueScissorHeight{};
        const auto opaqueScissorPixels = graphics.CaptureBackBuffer(
            opaqueScissorWidth,
            opaqueScissorHeight);
        graphics.EndFrame();
        Require(
            opaqueScissorWidth == Width
                && opaqueScissorHeight == Height,
            "Neutral scissor test must capture the active back buffer");
        RequirePixelNear(
            opaqueScissorPixels,
            12u,
            12u,
            { 255u, 0u, 0u },
            "Neutral scissor restart lost the opaque blend mode");
        RequirePixelNear(
            opaqueScissorPixels,
            32u,
            24u,
            { 0u, 255u, 0u },
            "Neutral nested scissor did not preserve its intersection");
        RequirePixelNear(
            opaqueScissorPixels,
            64u,
            24u,
            { 0u, 0u, 0u },
            "Neutral nested scissor drew outside the outer rectangle");
        RequirePixelNear(
            opaqueScissorPixels,
            12u,
            36u,
            { 0u, 0u, 255u },
            "Popping the inner scissor did not restore the outer scissor");
        RequirePixelNear(
            opaqueScissorPixels,
            4u,
            4u,
            { 255u, 255u, 0u },
            "Popping every scissor did not restore unclipped drawing");

        // Deferred callbackは、同じshaderがpass中に利用・再compileされても
        // Begin時のeffect世代と定数をEndまで保持します。
        const auto spriteMaskShaderPath =
            std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / L"shaders/LamaPonSpriteMask.hlsl";
        LamaPon::SpritePassDescription retainedShaderDescription;
        retainedShaderDescription.pixelShader =
            spriteMaskShaderPath;
        retainedShaderDescription.blend =
            LamaPon::SpriteBlendMode::Opaque;
        retainedShaderDescription.customParameters[0] = {
            0.0f, 0.0f, 0.0f, 0.0f };
        retainedShaderDescription.customParameters[1] = {
            8.0f, 8.0f, 8.0f, 8.0f };
        LamaPon::SpriteDrawRequest retainedShaderRequest;
        retainedShaderRequest.scale = { 16.0f, 16.0f };
        retainedShaderRequest.tint = {
            0.0f, 1.0f, 1.0f, 1.0f };
        graphics.BeginFrame(scissorClearColor);
        auto retainedShaderPass = graphics.BeginSpritePass(
            retainedShaderDescription);
        const auto retainedShaderStatus =
            retainedShaderPass.ShaderStatus();
        Require(
            retainedShaderStatus.error.empty()
                && retainedShaderStatus.fallback
                    == LamaPon::SpriteShaderFallback::None
                && retainedShaderPass.Draw(retainedShaderRequest),
            "A valid custom sprite shader pass was not prepared");
        auto conflictingParameters =
            retainedShaderDescription.customParameters;
        conflictingParameters[0].y = 1.0f;
        std::uint64_t reusedShaderGeneration{};
        std::string reusedShaderError;
        Require(
            graphics.ApplyCustomPixelShader(
                spriteMaskShaderPath,
                conflictingParameters,
                &reusedShaderGeneration,
                &reusedShaderError)
                && reusedShaderError.empty()
                && reusedShaderGeneration
                    == retainedShaderStatus.generation,
            "The active sprite shader could not be reused with new values");
        graphics.InvalidateCustomPixelShader(
            spriteMaskShaderPath);
        std::uint64_t reloadedShaderGeneration{};
        std::string reloadedShaderError;
        Require(
            graphics.ApplyCustomPixelShader(
                spriteMaskShaderPath,
                conflictingParameters,
                &reloadedShaderGeneration,
                &reloadedShaderError)
                && reloadedShaderError.empty()
                && reloadedShaderGeneration
                    > retainedShaderStatus.generation,
            "The active sprite shader could not retain a hot-reloaded generation");
        retainedShaderPass.End();
        std::uint32_t retainedShaderWidth{};
        std::uint32_t retainedShaderHeight{};
        const auto retainedShaderPixels = graphics.CaptureBackBuffer(
            retainedShaderWidth,
            retainedShaderHeight);
        graphics.EndFrame();
        Require(
            retainedShaderWidth == Width
                && retainedShaderHeight == Height,
            "Retained sprite shader test did not capture the back buffer");
        RequirePixelNear(
            retainedShaderPixels,
            8u,
            8u,
            { 0u, 255u, 255u },
            "A sprite pass lost its shader generation or constants before End");

        const auto spriteLitShaderPath =
            std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / L"shaders/LamaPonSpriteLit.hlsl";
        LamaPon::SpritePassDescription retainedLightingDescription;
        retainedLightingDescription.pixelShader =
            spriteLitShaderPath;
        retainedLightingDescription.blend =
            LamaPon::SpriteBlendMode::Opaque;
        retainedLightingDescription.lighting.counts = {
            1u, 0u, 0u, 0u };
        retainedLightingDescription.lighting.lights[0]
            .positionRadiusIntensity = {
                32.5f, 8.5f, 4096.0f, 1.0f };
        retainedLightingDescription.lighting.lights[0].color = {
            0.0f, 0.0f, 1.0f, 0.0f };
        LamaPon::SpriteDrawRequest retainedLightingRequest;
        retainedLightingRequest.position = { 24.0f, 0.0f };
        retainedLightingRequest.scale = { 16.0f, 16.0f };
        retainedLightingRequest.tint = {
            0.25f, 0.25f, 0.25f, 1.0f };
        graphics.BeginFrame(scissorClearColor);
        auto retainedLightingPass = graphics.BeginSpritePass(
            retainedLightingDescription);
        const auto retainedLightingStatus =
            retainedLightingPass.ShaderStatus();
        Require(
            retainedLightingStatus.error.empty()
                && retainedLightingStatus.fallback
                    == LamaPon::SpriteShaderFallback::None
                && retainedLightingPass.Draw(
                    retainedLightingRequest),
            "A lit sprite pass was not prepared");
        graphics.InvalidateCustomPixelShader(
            spriteLitShaderPath);
        std::uint64_t reloadedLightingGeneration{};
        std::string reloadedLightingError;
        Require(
            graphics.ApplyCustomPixelShader(
                spriteLitShaderPath,
                {},
                &reloadedLightingGeneration,
                &reloadedLightingError)
                && reloadedLightingError.empty()
                && reloadedLightingGeneration
                    > retainedLightingStatus.generation,
            "The active lit sprite shader could not retain a reload");
        retainedLightingPass.End();
        std::uint32_t retainedLightingWidth{};
        std::uint32_t retainedLightingHeight{};
        const auto retainedLightingPixels = graphics.CaptureBackBuffer(
            retainedLightingWidth,
            retainedLightingHeight);
        graphics.EndFrame();
        Require(
            retainedLightingWidth == Width
                && retainedLightingHeight == Height,
            "Retained sprite lighting test did not capture the back buffer");
        RequirePixelNear(
            retainedLightingPixels,
            32u,
            8u,
            { 64u, 64u, 128u },
            "A sprite pass lost its lighting snapshot before End");

        LamaPon::SpritePassDescription missingShaderDescription;
        missingShaderDescription.pixelShader =
            L"shaders/definitely-missing-neutral-sprite.hlsl";
        missingShaderDescription.blend =
            LamaPon::SpriteBlendMode::Opaque;
        graphics.ResetShaderFallbackDraws();
        graphics.BeginFrame(scissorClearColor);
        auto fallbackSpritePass = graphics.BeginSpritePass(
            missingShaderDescription);
        Require(
            !fallbackSpritePass.ShaderStatus().error.empty()
                && fallbackSpritePass.ShaderStatus().fallback
                    == LamaPon::SpriteShaderFallback::ErrorPlaceholder
                && graphics.FrameStats().shaderFallbackDraws == 1u,
            "A missing neutral sprite shader did not report its fallback");
        Require(
            fallbackSpritePass.PushScissor(
                { 0.0f, 0.0f, 16.0f, 16.0f })
                && fallbackSpritePass.PopScissor(),
            "A fallback sprite pass could not restart around a scissor");
        LamaPon::SpriteDrawRequest fallbackSpriteRequest;
        fallbackSpriteRequest.scale = { 16.0f, 16.0f };
        Require(fallbackSpritePass.Draw(fallbackSpriteRequest),
            "The sprite error placeholder rejected the white fallback");
        fallbackSpritePass.End();
        std::uint32_t fallbackSpriteWidth{};
        std::uint32_t fallbackSpriteHeight{};
        const auto fallbackSpritePixels = graphics.CaptureBackBuffer(
            fallbackSpriteWidth,
            fallbackSpriteHeight);
        graphics.EndFrame();
        Require(
            fallbackSpriteWidth == Width
                && fallbackSpriteHeight == Height
                && graphics.FrameStats().shaderFallbackDraws == 1u,
            "Scissor restart requested the sprite fallback more than once");
        RequirePixelNear(
            fallbackSpritePixels,
            8u,
            8u,
            { 255u, 0u, 255u },
            "A missing sprite shader did not draw its error placeholder");

        LamaPon::SceneLoadingScreenSettings loadingScreenSettings;
        loadingScreenSettings.message.clear();
        loadingScreenSettings.showPercentage = false;
        loadingScreenSettings.backgroundColor = {
            1.0f, 0.0f, 0.0f, 0.5f };
        loadingScreenSettings.barBackgroundColor = {
            0.0f, 1.0f, 0.0f, 1.0f };
        loadingScreenSettings.barFillColor = {
            0.0f, 0.0f, 1.0f, 1.0f };
        graphics.BeginFrame(scissorClearColor);
        graphics.DrawLoadingScreen(
            0.5f,
            loadingScreenSettings,
            Width,
            Height);
        std::uint32_t loadingScreenWidth{};
        std::uint32_t loadingScreenHeight{};
        const auto loadingScreenPixels = graphics.CaptureBackBuffer(
            loadingScreenWidth,
            loadingScreenHeight);
        graphics.EndFrame();
        Require(
            loadingScreenWidth == Width
                && loadingScreenHeight == Height,
            "Loading screen test did not capture the back buffer");
        RequirePixelNear(
            loadingScreenPixels,
            2u,
            2u,
            { 64u, 0u, 0u },
            "The neutral loading screen lost its background color");
        RequirePixelNear(
            loadingScreenPixels,
            24u,
            48u,
            { 0u, 0u, 255u },
            "The neutral loading screen lost its progress fill");
        RequirePixelNear(
            loadingScreenPixels,
            72u,
            48u,
            { 0u, 255u, 0u },
            "The neutral loading screen lost its progress background");

        const auto startupLogoPath =
            std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / L"textures/LamaPonEngineLogo.png";
        graphics.BeginFrame(scissorClearColor);
        graphics.DrawStartupLogo(
            startupLogoPath,
            Width,
            Height);
        std::uint32_t startupLogoWidth{};
        std::uint32_t startupLogoHeight{};
        const auto startupLogoPixels = graphics.CaptureBackBuffer(
            startupLogoWidth,
            startupLogoHeight);
        graphics.EndFrame();
        std::size_t startupLogoColoredPixels{};
        for (std::size_t offset = 0;
            offset + 3u < startupLogoPixels.size();
            offset += 4u)
        {
            if (startupLogoPixels[offset]
                    + startupLogoPixels[offset + 1u]
                    + startupLogoPixels[offset + 2u] > 24u)
            {
                ++startupLogoColoredPixels;
            }
        }
        Require(
            startupLogoWidth == Width
                && startupLogoHeight == Height
                && startupLogoColoredPixels >= 16u,
            "The neutral startup logo did not reach the back buffer");
        RequirePixelNear(
            startupLogoPixels,
            48u,
            19u,
            { 132u, 214u, 255u },
            "The neutral startup logo lost its placement or texture color");
        RequirePixelNear(
            startupLogoPixels,
            20u,
            20u,
            { 0u, 0u, 0u },
            "The neutral startup logo exceeded its scaled bounds");

        constexpr float debugOverlayClearColor[]{
            1.0f, 0.0f, 0.0f, 1.0f };
        std::vector<std::uint8_t> debugOverlayPixels;
        std::uint32_t debugOverlayWidth{};
        std::uint32_t debugOverlayHeight{};
        {
            LamaPon::Scene debugOverlayScene(graphics);
            LamaPon::DebugOverlay debugOverlay;
            debugOverlay.SetVisible(true);
            graphics.BeginFrame(debugOverlayClearColor);
            debugOverlay.Update(
                graphics,
                debugOverlayScene,
                1.0f);
            Require(debugOverlay.IsVisible(),
                "The neutral debug overlay hid after drawing failed");
            debugOverlayPixels = graphics.CaptureBackBuffer(
                debugOverlayWidth,
                debugOverlayHeight);
            graphics.EndFrame();
        }
        Require(
            debugOverlayWidth == Width
                && debugOverlayHeight == Height,
            "Debug overlay test did not capture the back buffer");
        RequirePixelNear(
            debugOverlayPixels,
            8u,
            8u,
            { 82u, 0u, 0u },
            "The neutral debug overlay lost its translucent panel");
        std::size_t debugOverlayTextPixels{};
        for (std::size_t offset = 0;
            offset + 3u < debugOverlayPixels.size();
            offset += 4u)
        {
            if (debugOverlayPixels[offset + 1u] > 32u
                || debugOverlayPixels[offset + 2u] > 32u)
            {
                ++debugOverlayTextPixels;
            }
        }
        Require(debugOverlayTextPixels >= 16u,
            "The neutral debug overlay did not draw its text handles");

        graphics.BeginFrame(scissorClearColor);
        auto passAfterDebugOverlay = graphics.BeginSpritePass();
        passAfterDebugOverlay.End();
        graphics.EndFrame();

        // SceneはAPI非依存contextをComponentへ渡し、ScrollViewの子だけを
        // view矩形へclipします。後続の通常UIまでclipされたままなら、赤い
        // UIImageが消えるためPop漏れも同じframeで検出できます。
        std::vector<std::uint8_t> neutralScenePixels;
        std::uint32_t neutralSceneWidth{};
        std::uint32_t neutralSceneHeight{};
        {
            LamaPon::Scene neutralScene(graphics);
            auto& scrollObject =
                neutralScene.CreateGameObject(
                    "Neutral Scroll View");
            scrollObject.AddComponent<
                LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 8.0f, 8.0f },
                    DirectX::XMFLOAT2{ 16.0f, 16.0f });
            auto& scrollView = scrollObject.AddComponent<
                LamaPon::UIScrollViewComponent>();
            scrollView.SetBackgroundColor(
                { 0.0f, 0.0f, 0.0f, 0.0f });

            auto& clippedObject =
                neutralScene.CreateGameObject(
                    "Neutral Clipped Sprite");
            clippedObject.SetParent(&scrollObject);
            auto& clippedSprite = clippedObject.AddComponent<
                NeutralSceneSpriteComponent>();

            auto& fallbackObject =
                neutralScene.CreateGameObject(
                    "Neutral Fallback Image");
            fallbackObject.AddComponent<
                LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 0.0f, 0.0f },
                    DirectX::XMFLOAT2{ 40.0f, 8.0f },
                    DirectX::XMFLOAT2{ 8.0f, 8.0f });
            auto& fallbackImage = fallbackObject.AddComponent<
                LamaPon::UIImageComponent>();
            fallbackImage.SetColor(
                { 1.0f, 0.0f, 0.0f, 1.0f });
            fallbackImage.SetSortOrder(2);

            graphics.SetUIViewportSize(Width, Height);
            graphics.BeginFrame(scissorClearColor);
            neutralScene.Render2D();
            neutralScenePixels = graphics.CaptureBackBuffer(
                neutralSceneWidth,
                neutralSceneHeight);
            graphics.EndFrame();

            Require(
                clippedSprite.DrawCalls() == 1u,
                "Scene did not invoke the neutral Component render hook once");
            Require(
                clippedSprite.ContextWasActive(),
                "Scene passed an inactive neutral sprite context to a Component");
            Require(
                clippedSprite.DrawWasAccepted(),
                "The neutral Component draw was rejected by the Scene pass");

            // Component例外でもpassのRAII cleanupがactive scissorごと
            // batchを閉じ、同じframe中に次のpassを開始できること。
            clippedSprite.ThrowAfterNextDraw();
            graphics.BeginFrame(scissorClearColor);
            RequireThrowsExactly<std::runtime_error>(
                [&]
                {
                    neutralScene.Render2D();
                },
                "A neutral Scene Component failure did not propagate");
            auto recoveredPass = graphics.BeginSpritePass();
            recoveredPass.End();
            graphics.EndFrame();
        }
        Require(
            neutralSceneWidth == Width
                && neutralSceneHeight == Height,
            "Neutral Scene test did not capture the back buffer");
        RequirePixelNear(
            neutralScenePixels,
            12u,
            12u,
            { 0u, 255u, 0u },
            "The ScrollView clipped a neutral Component inside its view");
        RequirePixelNear(
            neutralScenePixels,
            28u,
            12u,
            { 0u, 0u, 0u },
            "The neutral Component drew outside its ScrollView clip");
        RequirePixelNear(
            neutralScenePixels,
            44u,
            12u,
            { 255u, 0u, 0u },
            "Scene did not remove the scissor or use UIImage's white fallback");

        auto modelPreviewRenderer =
            LamaPon::CreateEditorModelPreviewRenderer(
                graphics.ActiveRenderingApi(),
                graphics);
        Require(
            modelPreviewRenderer != nullptr
                && modelPreviewRenderer->Api()
                    == LamaPon::RenderingApi::DirectX11,
            "The active API must create the DirectX 11 model preview renderer");
        LamaPon::ModelAsset emptyModel;
        const LamaPon::LitMaterial previewMaterial{
            DirectX::XMFLOAT4{
                0.15f, 0.82f, 1.0f, 1.0f },
            {},
            {},
            0.8f };
        const auto identity = DirectX::XMMatrixIdentity();
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                modelPreviewRenderer->DrawModel(
                    emptyModel,
                    identity,
                    identity,
                    identity,
                    previewMaterial,
                    true);
            },
            "The model preview renderer must reject an empty model asset");

        const auto modelPath =
            std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "arrow.cmo";
        const auto model = graphics.Assets().LoadModel(modelPath);
        Require(
            model != nullptr
                && model->model != nullptr
                && model->hasLocalBounds,
            "The checked-in static preview model must load with bounds");

        const auto& bounds = model->localBounds;
        const DirectX::XMFLOAT3 modelCenter{
            (bounds.minimum.x + bounds.maximum.x) * 0.5f,
            (bounds.minimum.y + bounds.maximum.y) * 0.5f,
            (bounds.minimum.z + bounds.maximum.z) * 0.5f };
        const float modelSpan = std::max({
            bounds.maximum.x - bounds.minimum.x,
            bounds.maximum.y - bounds.minimum.y,
            bounds.maximum.z - bounds.minimum.z,
            0.1f });
        const float modelDistance = modelSpan * 3.0f + 1.0f;
        const auto modelFocus = DirectX::XMLoadFloat3(&modelCenter);
        const auto modelView = DirectX::XMMatrixLookAtLH(
            DirectX::XMVectorSet(
                modelCenter.x + modelDistance,
                modelCenter.y + modelDistance * 0.75f,
                modelCenter.z - modelDistance,
                1.0f),
            modelFocus,
            DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const auto modelProjection = DirectX::XMMatrixOrthographicLH(
            modelSpan * 2.2f,
            modelSpan * 2.2f,
            0.01f,
            modelDistance * 4.0f);

        LamaPon::RenderTarget modelTarget;
        graphics.ResizeOffscreenTarget(modelTarget, 64, 64);
        constexpr float modelClear[]{
            0.035f, 0.045f, 0.06f, 1.0f };
        graphics.BeginOffscreenTarget(modelTarget, modelClear);
        modelPreviewRenderer->DrawModel(
            *model,
            identity,
            modelView,
            modelProjection,
            previewMaterial,
            true);
        graphics.PublishOffscreenTarget(modelTarget);

        renderer->NewFrame();
        ImGui::NewFrame();
        const auto modelTextureReference =
            renderer->DisplayTextureReference(modelTarget);
        DrawImageWindow(
            "Model preview",
            ImVec2{ 28.0f, 8.0f },
            modelTextureReference);
        ImGui::Render();

        graphics.BeginFrame(clearColor);
        renderer->RenderDrawData(ImGui::GetDrawData());
        const auto modelPixels = graphics.CaptureBackBuffer(
            capturedWidth,
            capturedHeight);
        graphics.EndFrame();

        std::size_t modelPixelCount{};
        constexpr std::array<int, 3> expectedModelClear{
            9, 11, 15 };
        for (std::uint32_t y = 10; y < 54; ++y)
        {
            for (std::uint32_t x = 30; x < 66; ++x)
            {
                const auto offset =
                    (static_cast<std::size_t>(y) * Width + x)
                    * 4u;
                const int difference =
                    std::abs(
                        static_cast<int>(modelPixels[offset])
                        - expectedModelClear[0])
                    + std::abs(
                        static_cast<int>(modelPixels[offset + 1u])
                        - expectedModelClear[1])
                    + std::abs(
                        static_cast<int>(modelPixels[offset + 2u])
                        - expectedModelClear[2]);
                if (difference > 20)
                {
                    ++modelPixelCount;
                }
            }
        }
        Require(
            modelPixelCount >= 8u,
            "The DirectX 11 model preview renderer must draw the static model");

        auto* const ownerContext = ImGui::GetCurrentContext();
        auto* const alternateContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(alternateContext);
        RequireThrows<std::logic_error>(
            [&] { renderer->NewFrame(); },
            "Editor GUI renderer must reject a different ImGui context");
        RequireThrows<std::logic_error>(
            [&]
            {
                static_cast<void>(
                    renderer->TextureReference(textureAsset));
            },
            "Asset texture conversion must reject a different ImGui context");
        RequireThrows<std::logic_error>(
            [&]
            {
                static_cast<void>(
                    renderer->DisplayTextureReference(displayTarget));
            },
            "Display texture conversion must reject a different ImGui context");
        renderer->Shutdown();
        Require(
            !renderer->IsInitialized()
                && ImGui::GetCurrentContext() == alternateContext,
            "Editor GUI shutdown must restore the caller's ImGui context");
        ImGui::DestroyContext(alternateContext);
        ImGui::SetCurrentContext(ownerContext);

        {
            auto automaticRenderer =
                LamaPon::CreateEditorGuiRenderer(
                    graphics.ActiveRenderingApi());
            automaticRenderer->Initialize(graphics);
            Require(ImGui::GetIO().BackendRendererUserData != nullptr,
                "Initialized editor GUI renderer must register backend data");
        }
        Require(ImGui::GetIO().BackendRendererUserData == nullptr,
            "Editor GUI renderer destructor must release backend data");
    }
}

static_assert(!std::is_copy_constructible_v<
    LamaPon::GraphicsDeviceResourceLease>);
static_assert(!std::is_copy_assignable_v<
    LamaPon::GraphicsDeviceResourceLease>);
static_assert(std::is_nothrow_move_constructible_v<
    LamaPon::GraphicsDeviceResourceLease>);
static_assert(std::is_nothrow_move_assignable_v<
    LamaPon::GraphicsDeviceResourceLease>);
static_assert(std::is_nothrow_destructible_v<
    LamaPon::GraphicsDeviceResourceLease>);
static_assert(!std::is_copy_constructible_v<
    LamaPon::SpriteRenderPass>);
static_assert(!std::is_copy_assignable_v<
    LamaPon::SpriteRenderPass>);
static_assert(std::is_nothrow_move_constructible_v<
    LamaPon::SpriteRenderPass>);
static_assert(std::is_nothrow_move_assignable_v<
    LamaPon::SpriteRenderPass>);
static_assert(std::is_nothrow_destructible_v<
    LamaPon::SpriteRenderPass>);

int main()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    int result{};
    try
    {
        LamaPon::GraphicsDevice graphics;
        LamaPon::ShadowMap shadowMap;
        LamaPon::RenderTarget offscreenTarget;
        LamaPon::LightingState clusteredLighting;
        TestGraphicsOutputState foreignOutputState;
        const auto clusteredIdentity =
            DirectX::XMMatrixIdentity();
        constexpr float offscreenClear[]{
            0.0f, 0.0f, 0.0f, 1.0f };
        const DirectX::XMFLOAT4X4 historyViewProjection{
            1.0f, 2.0f, 3.0f, 4.0f,
            5.0f, 6.0f, 7.0f, 8.0f,
            9.0f, 10.0f, 11.0f, 12.0f,
            13.0f, 14.0f, 15.0f, 16.0f };
        LamaPon::AutoExposureSettings autoExposureSettings{};
        autoExposureSettings.enabled = true;
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.BeginShadowMap(shadowMap, 0);
            },
            "Beginning a shadow map requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.EndShadowMap(shadowMap);
            },
            "Ending a shadow map requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.UpdateClusteredLights(
                    clusteredLighting,
                    clusteredIdentity,
                    clusteredIdentity,
                    1,
                    1);
            },
            "Updating clustered lights requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                static_cast<void>(
                    graphics.CaptureOutputState());
            },
            "Capturing output state requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.RestoreOutputState(
                    foreignOutputState);
            },
            "Restoring output state requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.ResizeOffscreenTarget(
                    offscreenTarget,
                    1,
                    1);
            },
            "Resizing an offscreen target requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.BeginOffscreenTarget(
                    offscreenTarget,
                    offscreenClear);
            },
            "Beginning an offscreen target requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.BindOffscreenTarget(offscreenTarget);
            },
            "Binding an offscreen target requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.PublishOffscreenTarget(
                    offscreenTarget);
            },
            "Publishing an offscreen target requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.BindOffscreenTargetDepthOnly(
                    offscreenTarget);
            },
            "Binding an offscreen depth target requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.CaptureOffscreenTargetDepth(
                    offscreenTarget);
            },
            "Capturing offscreen depth requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.CaptureOffscreenTargetColorHistory(
                    offscreenTarget,
                    historyViewProjection);
            },
            "Capturing offscreen color history requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                graphics.CaptureOffscreenTargetTemporalHistory(
                    offscreenTarget,
                    historyViewProjection);
            },
            "Capturing offscreen temporal history requires an initialized device");
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                static_cast<void>(
                    graphics.UpdateOffscreenTargetAutoExposure(
                        offscreenTarget,
                        autoExposureSettings,
                        1.0f / 60.0f));
            },
            "Updating offscreen auto exposure requires an initialized device");
        const auto modelPreviewRenderer =
            LamaPon::CreateEditorModelPreviewRenderer(
                LamaPon::RenderingApi::DirectX11,
                graphics);
        Require(
            modelPreviewRenderer != nullptr
                && modelPreviewRenderer->Api()
                    == LamaPon::RenderingApi::DirectX11,
            "DirectX 11 model preview factory must return a renderer");
        const LamaPon::ModelAsset emptyModel;
        const LamaPon::LitMaterial previewMaterial;
        const auto identity = DirectX::XMMatrixIdentity();
        RequireThrowsExactly<std::logic_error>(
            [&]
            {
                modelPreviewRenderer->DrawModel(
                    emptyModel,
                    identity,
                    identity,
                    identity,
                    previewMaterial,
                    true);
            },
            "Model preview drawing requires an initialized graphics device");
        RequireModelPreviewFactoryRejected(
            LamaPon::RenderingApi::Auto,
            graphics,
            "Model preview factory must reject unresolved Auto");
        RequireModelPreviewFactoryRejected(
            LamaPon::RenderingApi::DirectX12Experimental,
            graphics,
            "Model preview factory must reject unimplemented DirectX 12");
        RequireModelPreviewFactoryRejected(
            static_cast<LamaPon::RenderingApi>(-1),
            graphics,
            "Model preview factory must reject an unknown rendering API");
        const auto renderer = LamaPon::CreateEditorGuiRenderer(
            LamaPon::RenderingApi::DirectX11);
        Require(renderer != nullptr,
            "DirectX 11 editor GUI factory must return a renderer");
        Require(renderer->Api() == LamaPon::RenderingApi::DirectX11,
            "DirectX 11 editor GUI renderer must report DirectX 11");
        Require(!renderer->IsInitialized(),
            "A newly created editor GUI renderer must not be initialized");

        {
            ImGuiContextScope imguiContext;
            try
            {
                renderer->Initialize(graphics);
                throw std::runtime_error(
                    "Editor GUI initialization must reject an uninitialized device");
            }
            catch (const std::invalid_argument&)
            {
            }
        }
        Require(!renderer->IsInitialized(),
            "Rejected initialization must leave the renderer uninitialized");
        renderer->Shutdown();

        RequireFactoryRejected(
            LamaPon::RenderingApi::Auto,
            "Editor GUI factory must reject unresolved Auto");
        RequireFactoryRejected(
            LamaPon::RenderingApi::DirectX12Experimental,
            "Editor GUI factory must reject unimplemented DirectX 12");
        RequireFactoryRejected(
            static_cast<LamaPon::RenderingApi>(-1),
            "Editor GUI factory must reject an unknown rendering API");

        CheckD3D11Lifecycle();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (uninitialize)
    {
        CoUninitialize();
    }
    return result;
}
