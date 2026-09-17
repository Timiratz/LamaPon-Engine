#pragma once

#include "LamaPon/Editor/EditorGuiRenderer.h"
#include "LamaPon/Graphics/GraphicsDeviceResourceLease.h"
#include "LamaPon/Graphics/GraphicsResource.h"

#include <d3d12.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

struct ImGuiContext;
struct ImGui_ImplDX12_InitInfo;

namespace LamaPon
{
    class D3D12Backend;

    class D3D12EditorGuiRenderer final
        : public EditorGuiRenderer
    {
    public:
        D3D12EditorGuiRenderer() = default;
        ~D3D12EditorGuiRenderer() override;

        D3D12EditorGuiRenderer(
            const D3D12EditorGuiRenderer&) = delete;
        D3D12EditorGuiRenderer& operator=(
            const D3D12EditorGuiRenderer&) = delete;

        [[nodiscard]] RenderingApi Api() const noexcept override
        {
            return RenderingApi::DirectX12Experimental;
        }
        [[nodiscard]] bool IsInitialized() const noexcept override
        {
            return m_initialized;
        }

        void Initialize(GraphicsDevice& graphics) override;
        void NewFrame() override;
        [[nodiscard]] ImTextureRef TextureReference(
            const TextureAsset& texture) override;
        [[nodiscard]] ImTextureRef DisplayTextureReference(
            const RenderTarget& target) override;
        void RenderDrawData(ImDrawData* drawData) override;
        void Shutdown() noexcept override;

    private:
        static void AllocateDescriptor(
            ImGui_ImplDX12_InitInfo* info,
            D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
        static void FreeDescriptor(
            ImGui_ImplDX12_InitInfo* info,
            D3D12_CPU_DESCRIPTOR_HANDLE cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE gpu);
        void RequireCurrentContext() const;
        [[nodiscard]] ImTextureRef TextureReference(
            const GraphicsViewHandle& view);
        void ReleaseOwnedDescriptors() noexcept;

        GraphicsDevice* m_graphics{};
        D3D12Backend* m_backend{};
        GraphicsDeviceResourceLease m_graphicsResourceLease;
        ImGuiContext* m_imguiContext{};
        std::vector<GraphicsViewHandle> m_frameTexturePins;
        std::unordered_map<std::uintptr_t, std::uint32_t>
            m_ownedDescriptorSlots;
        bool m_initialized{};
    };
}
