#include "LamaPon/Editor/D3D11EditorGuiRenderer.h"

#include "LamaPon/Graphics/GraphicsDevice.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <stdexcept>

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
        ImGui_ImplDX11_NewFrame();
    }

    void D3D11EditorGuiRenderer::RenderDrawData(
        ImDrawData* const drawData)
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
        if (drawData == nullptr)
        {
            throw std::invalid_argument(
                "Dear ImGui draw data must not be null.");
        }
        ImGui_ImplDX11_RenderDrawData(drawData);
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
