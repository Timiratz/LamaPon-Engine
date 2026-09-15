#include "LamaPon/Editor/D3D12EditorGuiRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D12Access.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>

#include <stdexcept>
#include <utility>

namespace LamaPon
{
    D3D12EditorGuiRenderer::~D3D12EditorGuiRenderer()
    {
        Shutdown();
    }

    void D3D12EditorGuiRenderer::Initialize(
        GraphicsDevice& graphics)
    {
        if (m_initialized)
        {
            throw std::logic_error(
                "The DirectX 12 editor GUI renderer is already initialized.");
        }
        auto* const imguiContext = ImGui::GetCurrentContext();
        if (imguiContext == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 12 editor GUI renderer requires an ImGui "
                "context.");
        }

        auto resourceLease = graphics.AcquireResourceLease();
        auto* const backend =
            Detail::GraphicsDeviceD3D12Access::Backend(graphics);
        if (backend == nullptr
            || !backend->IsInitialized()
            || backend->Device() == nullptr
            || backend->CommandQueue() == nullptr
            || backend->ShaderResourceDescriptorHeap() == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 12 editor GUI renderer requires an initialized "
                "DirectX 12 graphics backend.");
        }

        m_graphics = &graphics;
        m_backend = backend;
        m_imguiContext = imguiContext;
        try
        {
            ImGui_ImplDX12_InitInfo info{};
            info.Device = backend->Device();
            info.CommandQueue = backend->CommandQueue();
            info.NumFramesInFlight = static_cast<int>(
                backend->FramesInFlight());
            info.RTVFormat = D3D12Backend::PrimaryColorFormat;
            info.DSVFormat = DXGI_FORMAT_UNKNOWN;
            info.UserData = this;
            info.SrvDescriptorHeap =
                backend->ShaderResourceDescriptorHeap();
            info.SrvDescriptorAllocFn = &AllocateDescriptor;
            info.SrvDescriptorFreeFn = &FreeDescriptor;
            if (!ImGui_ImplDX12_Init(&info))
            {
                throw std::runtime_error(
                    "Failed to initialize the DirectX 12 Dear ImGui "
                    "renderer.");
            }
        }
        catch (...)
        {
            ReleaseOwnedDescriptors();
            m_graphics = nullptr;
            m_backend = nullptr;
            m_imguiContext = nullptr;
            throw;
        }
        m_graphicsResourceLease = std::move(resourceLease);
        m_initialized = true;
    }

    void D3D12EditorGuiRenderer::NewFrame()
    {
        RequireCurrentContext();
        m_frameTexturePins.clear();
        ImGui_ImplDX12_NewFrame();
    }

    ImTextureRef D3D12EditorGuiRenderer::TextureReference(
        const TextureAsset& texture)
    {
        RequireCurrentContext();
        const auto resources = texture.resources.Acquire();
        if (resources == nullptr)
        {
            throw std::invalid_argument(
                "The editor GUI texture asset has no DirectX 12 shader "
                "resource view.");
        }
        return TextureReference(resources->shaderResourceView);
    }

    ImTextureRef D3D12EditorGuiRenderer::DisplayTextureReference(
        const RenderTarget& target)
    {
        RequireCurrentContext();
        return TextureReference(target.DisplayViewHandle());
    }

    ImTextureRef D3D12EditorGuiRenderer::TextureReference(
        const GraphicsViewHandle& view)
    {
        const auto binding = m_backend->TryResolveShaderResource(view);
        if (!binding.has_value())
        {
            throw std::invalid_argument(
                "The editor GUI texture has no current DirectX 12 shader "
                "resource view.");
        }
        m_frameTexturePins.push_back(view);
        return ImTextureRef{
            static_cast<ImTextureID>(binding->descriptor.ptr)
        };
    }

    void D3D12EditorGuiRenderer::RenderDrawData(
        ImDrawData* const drawData)
    {
        RequireCurrentContext();
        if (drawData == nullptr)
        {
            throw std::invalid_argument(
                "Dear ImGui draw data must not be null.");
        }
        auto* const commands = m_backend->BeginFrameCommands();
        auto* const heap = m_backend->ShaderResourceDescriptorHeap();
        if (commands == nullptr || heap == nullptr)
        {
            throw std::logic_error(
                "The DirectX 12 editor GUI renderer has no active command "
                "list or descriptor heap.");
        }
        commands->SetDescriptorHeaps(1u, &heap);
        ImGui_ImplDX12_RenderDrawData(drawData, commands);
    }

    void D3D12EditorGuiRenderer::AllocateDescriptor(
        ImGui_ImplDX12_InitInfo* const info,
        D3D12_CPU_DESCRIPTOR_HANDLE* const cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE* const gpu)
    {
        if (info == nullptr || cpu == nullptr || gpu == nullptr
            || info->UserData == nullptr)
        {
            throw std::invalid_argument(
                "Dear ImGui requested an invalid DirectX 12 descriptor.");
        }
        auto& renderer = *static_cast<D3D12EditorGuiRenderer*>(
            info->UserData);
        if (renderer.m_backend == nullptr)
        {
            throw std::logic_error(
                "The DirectX 12 editor GUI backend is unavailable.");
        }
        const auto allocation =
            renderer.m_backend->AllocateExternalShaderResourceDescriptor();
        try
        {
            renderer.m_ownedDescriptorSlots.emplace(
                allocation.cpu.ptr,
                allocation.slot);
        }
        catch (...)
        {
            renderer.m_backend->ReleaseExternalShaderResourceDescriptor(
                allocation.slot);
            throw;
        }
        *cpu = allocation.cpu;
        *gpu = allocation.gpu;
    }

    void D3D12EditorGuiRenderer::FreeDescriptor(
        ImGui_ImplDX12_InitInfo* const info,
        const D3D12_CPU_DESCRIPTOR_HANDLE cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        if (info == nullptr || info->UserData == nullptr)
        {
            return;
        }
        auto& renderer = *static_cast<D3D12EditorGuiRenderer*>(
            info->UserData);
        const auto found = renderer.m_ownedDescriptorSlots.find(cpu.ptr);
        if (found == renderer.m_ownedDescriptorSlots.end())
        {
            return;
        }
        if (renderer.m_backend != nullptr)
        {
            renderer.m_backend->ReleaseExternalShaderResourceDescriptor(
                found->second);
        }
        renderer.m_ownedDescriptorSlots.erase(found);
    }

    void D3D12EditorGuiRenderer::RequireCurrentContext() const
    {
        if (!m_initialized || m_backend == nullptr
            || !m_backend->IsInitialized())
        {
            throw std::logic_error(
                "The DirectX 12 editor GUI renderer is not initialized.");
        }
        if (ImGui::GetCurrentContext() != m_imguiContext)
        {
            throw std::logic_error(
                "The DirectX 12 editor GUI renderer requires its "
                "initializing ImGui context to be current.");
        }
    }

    void D3D12EditorGuiRenderer::ReleaseOwnedDescriptors() noexcept
    {
        if (m_backend != nullptr)
        {
            for (const auto& [cpu, slot] : m_ownedDescriptorSlots)
            {
                static_cast<void>(cpu);
                m_backend->ReleaseExternalShaderResourceDescriptor(slot);
            }
        }
        m_ownedDescriptorSlots.clear();
    }

    void D3D12EditorGuiRenderer::Shutdown() noexcept
    {
        if (m_initialized)
        {
            auto* const previousContext = ImGui::GetCurrentContext();
            if (previousContext != m_imguiContext)
            {
                ImGui::SetCurrentContext(m_imguiContext);
            }
            m_frameTexturePins.clear();
            ImGui_ImplDX12_Shutdown();
            if (previousContext != m_imguiContext)
            {
                ImGui::SetCurrentContext(previousContext);
            }
        }
        ReleaseOwnedDescriptors();
        m_graphics = nullptr;
        m_backend = nullptr;
        m_imguiContext = nullptr;
        m_initialized = false;
        m_graphicsResourceLease.Reset();
    }
}
