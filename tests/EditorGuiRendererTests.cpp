#include "LamaPon/Editor/EditorGuiRenderer.h"
#include "LamaPon/Editor/EditorModelPreviewRenderer.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/Scene.h"

#include <Windows.h>
#include <SpriteBatch.h>
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
            graphics.ResolveD3D11ShaderResourceView(view);
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
        auto& sprites = graphics.BeginSprites();
        sprites.Draw(
            graphics.WhiteTexture(),
            position,
            nullptr,
            DirectX::XMLoadFloat4(&color),
            0.0f,
            DirectX::XMFLOAT2{},
            size);
        graphics.EndSprites();
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
        {
            LamaPon::GraphicsDevice initiallyGuardedGraphics;
            auto* const fileOnlyAssets =
                &initiallyGuardedGraphics.Assets();
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
        }

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
        Require(
            graphics.WhiteTexture() != nullptr
                && graphics.AdditiveBlendPreservingAlpha() != nullptr,
            "DirectX 11 compatibility resources were not created");
        Microsoft::WRL::ComPtr<ID3D11Device> previousDevice =
            graphics.Device();
        const auto previousWhiteTexture =
            graphics.WhiteTextureHandle();
        const auto previousWhiteView =
            graphics.WhiteTextureViewHandle();
        const LamaPon::TextureResourceSnapshot
            previousWhiteResources{
                previousWhiteTexture,
                previousWhiteView,
                graphics.WhiteTexture()
            };
        const std::array<float, 4> instanceData{
            1.0f, 2.0f, 3.0f, 4.0f };
        const auto instanceBytes = std::as_bytes(
            std::span{ instanceData });
        const auto previousInstanceBuffer =
            graphics.AcquireInstanceBufferHandle(instanceBytes);
        Require(
            previousWhiteTexture
                && previousWhiteView
                && previousWhiteView.Kind()
                    == LamaPon::GraphicsViewKind::ShaderResource
                && previousInstanceBuffer
                && graphics.ResolveD3D11ShaderResourceView(
                    previousWhiteView) == graphics.WhiteTexture()
                && graphics.ResolveD3D11Buffer(
                    previousInstanceBuffer)
                    == graphics.AcquireInstanceBuffer(
                        instanceData.data(),
                        sizeof(instanceData)),
            "Neutral and DirectX 11 compatibility resources diverged");
        const LamaPon::TextureResourceSnapshot
            incompleteNeutralResources{
                previousWhiteTexture,
                {},
                graphics.WhiteTexture()
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
                    && graphics.ResolveD3D11ShaderResourceView(
                        previousWhiteView)
                        == graphics.WhiteTexture(),
                "Rejected reinitialization changed the active graphics state");
        }

        graphics.Initialize(
            window.Get(),
            Width,
            Height,
            LamaPon::RenderingApi::DirectX11);
        Require(
            graphics.StartupRenderingApi()
                    == LamaPon::RenderingApi::DirectX11
                && graphics.ActiveRenderingApi()
                    == LamaPon::RenderingApi::DirectX11
                && graphics.RenderingApiFallback()
                    == LamaPon::RenderingApiFallbackReason::None
                && graphics.WhiteTexture() != nullptr
                && graphics.AdditiveBlendPreservingAlpha() != nullptr
                && graphics.Device() != previousDevice.Get(),
            "GraphicsDevice reinitialization did not rebuild DirectX 11 resources");
        Require(
            previousWhiteTexture
                && previousWhiteView
                && previousInstanceBuffer,
            "Backend shutdown invalidated externally owned handle lifetimes");
        RequireThrowsExactly<std::invalid_argument>(
            [&]
            {
                static_cast<void>(
                    graphics.ResolveD3D11ShaderResourceView(
                        previousWhiteView));
            },
            "A shader view from the previous backend generation was accepted");
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
                static_cast<void>(
                    graphics.ResolveD3D11Buffer(
                        previousInstanceBuffer));
            },
            "A buffer from the previous backend generation was accepted");

        const auto rebuiltInstanceHandle =
            graphics.AcquireInstanceBufferHandle(instanceBytes);
        auto* const rebuiltInstanceBuffer =
            graphics.ResolveD3D11Buffer(rebuiltInstanceHandle);
        Require(
            rebuiltInstanceHandle
                && rebuiltInstanceHandle != previousInstanceBuffer
                && graphics.WhiteTextureHandle()
                    != previousWhiteTexture
                && graphics.WhiteTextureViewHandle()
                    != previousWhiteView
                && rebuiltInstanceBuffer != nullptr
                && graphics.AcquireInstanceBuffer(
                    instanceData.data(),
                    sizeof(instanceData)) == rebuiltInstanceBuffer,
            "GraphicsDevice reinitialization did not rebuild neutral resources");

        std::vector<std::byte> grownInstanceData(
            8192,
            std::byte{ 0x2a });
        const auto grownInstanceHandle =
            graphics.AcquireInstanceBufferHandle(grownInstanceData);
        auto* const grownInstanceBuffer =
            graphics.ResolveD3D11Buffer(grownInstanceHandle);
        Require(
            grownInstanceHandle
                && grownInstanceHandle != rebuiltInstanceHandle
                && grownInstanceBuffer != nullptr
                && graphics.ResolveD3D11Buffer(
                    rebuiltInstanceHandle) == rebuiltInstanceBuffer,
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
            graphics.ResolveD3D11ShaderResourceView(smallestMipView);
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
                && graphics.ResolveD3D11ShaderResourceView(
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
                && graphics.ResolveD3D11ShaderResourceView(
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
                && graphics.ResolveD3D11ShaderResourceView(
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
                && graphics.ResolveD3D11ShaderResourceView(
                    firstProgressiveResources->shaderResourceView)
                    == firstProgressiveResources
                        ->d3d11ShaderResourceView.Get()
                && graphics.ResolveD3D11ShaderResourceView(
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
                && graphics.ResolveD3D11ShaderResourceView(
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
        graphics.WhiteTexture()->GetDevice(
            whiteDevice.ReleaseAndGetAddressOf());
        graphics.AdditiveBlendPreservingAlpha()->GetDevice(
            blendDevice.ReleaseAndGetAddressOf());
        grownInstanceBuffer->GetDevice(
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
                && emptyClusteredLighting.clustered.lightCount == 0u,
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
        graphics.ResizeOffscreenTarget(displayTarget, 8, 4);
        Require(
            displayTarget.IsValid()
                && displayTarget.Width() == 8u
                && displayTarget.Height() == 4u,
            "Offscreen target resize must apply the requested dimensions");
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
        testResources.fill(graphics.WhiteTexture());
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
            displayTarget.ColorHistoryShaderResourceView() == nullptr,
            "Color history must be unavailable before its first capture");
        graphics.CaptureOffscreenTargetColorHistory(
            displayTarget,
            historyViewProjection);
        graphics.CaptureOffscreenTargetTemporalHistory(
            displayTarget,
            historyViewProjection);
        Require(
            displayTarget.ColorHistoryShaderResourceView() != nullptr,
            "Color history must become available after capture");
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

        // SpriteBatchもDrawをEndまで遅延します。Draw登録後にassetの
        // snapshotを差し替えても、登録時のviewがEndSpritesまで生存し、
        // その色で描かれることを確認します。
        auto deferredTextureAsset =
            CreateSolidTexture(graphics, assetColor);
        graphics.BeginFrame(clearColor);
        auto& deferredSprites = graphics.BeginSprites();
        auto* const deferredView =
            graphics.PinD3D11TextureForSpriteBatch(
                deferredTextureAsset.resources.Acquire());
        Require(deferredView != nullptr,
            "SpriteBatch texture pin must resolve an asset view");
        const DirectX::XMFLOAT4 opaqueWhite{
            1.0f, 1.0f, 1.0f, 1.0f };
        deferredSprites.Draw(
            deferredView,
            DirectX::XMFLOAT2{},
            nullptr,
            DirectX::XMLoadFloat4(&opaqueWhite),
            0.0f,
            DirectX::XMFLOAT2{},
            DirectX::XMFLOAT2{ 16.0f, 16.0f });
        PublishSolidTexture(
            deferredTextureAsset,
            graphics,
            replacementAssetColor);
        graphics.EndSprites();
        std::uint32_t deferredWidth{};
        std::uint32_t deferredHeight{};
        const auto deferredPixels = graphics.CaptureBackBuffer(
            deferredWidth,
            deferredHeight);
        graphics.EndFrame();
        Require(
            deferredWidth == Width && deferredHeight == Height,
            "SpriteBatch pin test must capture the active back buffer");
        RequirePixelNear(
            deferredPixels,
            8u,
            8u,
            { assetColor[0], assetColor[1], assetColor[2] },
            "SpriteBatch must retain the texture snapshot until EndSprites");

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
