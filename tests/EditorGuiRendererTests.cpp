#include "LamaPon/Editor/EditorGuiRenderer.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <Windows.h>
#include <imgui.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
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

    constexpr std::uint32_t Width = 96;
    constexpr std::uint32_t Height = 64;

    [[nodiscard]] ImTextureID ExpectedTextureId(
        const ID3D11ShaderResourceView* const view) noexcept
    {
        return static_cast<ImTextureID>(
            reinterpret_cast<std::uintptr_t>(view));
    }

    [[nodiscard]] LamaPon::TextureAsset CreateSolidTexture(
        LamaPon::GraphicsDevice& graphics,
        const std::array<std::uint8_t, 4>& color)
    {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 1;
        description.Height = 1;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        const D3D11_SUBRESOURCE_DATA initialData{
            color.data(),
            static_cast<UINT>(color.size()),
            0
        };
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Require(
            SUCCEEDED(graphics.Device()->CreateTexture2D(
                &description,
                &initialData,
                texture.ReleaseAndGetAddressOf())),
            "Editor GUI test texture creation failed");

        LamaPon::TextureAsset asset;
        asset.width = 1;
        asset.height = 1;
        Require(
            SUCCEEDED(graphics.Device()->CreateShaderResourceView(
                texture.Get(),
                nullptr,
                asset.view.ReleaseAndGetAddressOf())),
            "Editor GUI test texture view creation failed");
        return asset;
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

        ImGuiContextScope imguiContext;
        auto renderer = LamaPon::CreateEditorGuiRenderer(
            graphics.ActiveRenderingApi());

        constexpr std::array<std::uint8_t, 4> assetColor{
            224u, 48u, 32u, 255u };
        auto textureAsset = CreateSolidTexture(graphics, assetColor);
        LamaPon::RenderTarget displayTarget;
        displayTarget.Resize(graphics.Device(), 1, 1);
        constexpr float displayColor[]{
            0.1f, 0.85f, 0.2f, 1.0f };
        displayTarget.Clear(graphics.Context(), displayColor);
        displayTarget.CopyToDisplay(graphics.Context());

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
        Require(
            assetTextureReference.GetTexID()
                == ExpectedTextureId(textureAsset.view.Get()),
            "Asset texture reference must contain its DirectX 11 SRV");
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

        constexpr float clearColor[]{
            0.05f, 0.1f, 0.15f, 1.0f };
        graphics.BeginFrame(clearColor);
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
            72u,
            32u,
            { 26u, 217u, 51u },
            "ImGui::Image must sample the render target texture reference");

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

int main()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    int result{};
    try
    {
        LamaPon::GraphicsDevice graphics;
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
