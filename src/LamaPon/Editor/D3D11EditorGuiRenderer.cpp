#include "LamaPon/Editor/D3D11EditorGuiRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <cstdint>
#include <stdexcept>

namespace
{
    ImTextureRef MakeD3D11TextureReference(
        ID3D11ShaderResourceView* const texture)
    {
        return ImTextureRef{
            static_cast<ImTextureID>(
                reinterpret_cast<std::uintptr_t>(texture))
        };
    }
}

namespace LamaPon
{
    D3D11EditorGuiRenderer::~D3D11EditorGuiRenderer()
    {
        Shutdown();
    }

    void D3D11EditorGuiRenderer::Initialize(
        GraphicsDevice& graphics)
    {
        if (m_initialized)
        {
            throw std::logic_error(
                "The DirectX 11 editor GUI renderer is already initialized.");
        }

        auto* const imguiContext = ImGui::GetCurrentContext();
        if (imguiContext == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 11 editor GUI renderer requires an ImGui context.");
        }

        auto* const device = graphics.Device();
        auto* const context = graphics.Context();
        if (device == nullptr || context == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 11 editor GUI renderer requires an initialized "
                "DirectX 11 graphics device and context.");
        }

        if (!ImGui_ImplDX11_Init(device, context))
        {
            throw std::runtime_error(
                "Failed to initialize the DirectX 11 Dear ImGui renderer.");
        }
        m_imguiContext = imguiContext;
        m_initialized = true;
    }

    void D3D11EditorGuiRenderer::NewFrame()
    {
        RequireCurrentContext();
        ImGui_ImplDX11_NewFrame();
    }

    ImTextureRef D3D11EditorGuiRenderer::TextureReference(
        const TextureAsset& texture)
    {
        RequireCurrentContext();
        auto* const view = texture.view.Get();
        if (view == nullptr)
        {
            throw std::invalid_argument(
                "The editor GUI texture asset has no DirectX 11 shader "
                "resource view.");
        }
        return MakeD3D11TextureReference(view);
    }

    ImTextureRef D3D11EditorGuiRenderer::DisplayTextureReference(
        const RenderTarget& target)
    {
        RequireCurrentContext();
        auto* const view = target.DisplayShaderResourceView();
        if (view == nullptr)
        {
            throw std::invalid_argument(
                "The editor GUI render target has no DirectX 11 display "
                "shader resource view.");
        }
        return MakeD3D11TextureReference(view);
    }

    void D3D11EditorGuiRenderer::RenderDrawData(
        ImDrawData* const drawData)
    {
        RequireCurrentContext();
        if (drawData == nullptr)
        {
            throw std::invalid_argument(
                "Dear ImGui draw data must not be null.");
        }
        ImGui_ImplDX11_RenderDrawData(drawData);
    }

    void D3D11EditorGuiRenderer::RequireCurrentContext() const
    {
        if (!m_initialized)
        {
            throw std::logic_error(
                "The DirectX 11 editor GUI renderer is not initialized.");
        }
        if (ImGui::GetCurrentContext() != m_imguiContext)
        {
            throw std::logic_error(
                "The DirectX 11 editor GUI renderer requires its initializing "
                "ImGui context to be current.");
        }
    }

    void D3D11EditorGuiRenderer::Shutdown() noexcept
    {
        if (!m_initialized)
        {
            return;
        }

        auto* const previousContext = ImGui::GetCurrentContext();
        if (previousContext != m_imguiContext)
        {
            ImGui::SetCurrentContext(m_imguiContext);
        }
        ImGui_ImplDX11_Shutdown();
        if (previousContext != m_imguiContext)
        {
            ImGui::SetCurrentContext(previousContext);
        }
        m_imguiContext = nullptr;
        m_initialized = false;
    }
}
