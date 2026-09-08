#pragma once

#include "LamaPon/Editor/EditorGuiRenderer.h"

struct ImGuiContext;

namespace LamaPon
{
    class D3D11EditorGuiRenderer final
        : public EditorGuiRenderer
    {
    public:
        D3D11EditorGuiRenderer() = default;
        ~D3D11EditorGuiRenderer() override;

        D3D11EditorGuiRenderer(
            const D3D11EditorGuiRenderer&) = delete;
        D3D11EditorGuiRenderer& operator=(
            const D3D11EditorGuiRenderer&) = delete;

        [[nodiscard]] RenderingApi
            Api() const noexcept override
        {
            return RenderingApi::DirectX11;
        }
        [[nodiscard]] bool
            IsInitialized() const noexcept override
        {
            return m_initialized;
        }

        void Initialize(GraphicsDevice& graphics) override;
        void NewFrame() override;
        void RenderDrawData(ImDrawData* drawData) override;
        void Shutdown() noexcept override;

    private:
        ImGuiContext* m_imguiContext{};
        bool m_initialized{};
    };
}
