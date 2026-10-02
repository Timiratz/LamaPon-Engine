#include "LamaPon/Editor/D3D11EditorGuiRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Access.h"
#include "LamaPon/Graphics/RenderTarget.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace
{
    // 生のSRVをImGuiのtexture IDへ変換する(texture: フレーム内で存続するSRV)。
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

        // 初期化する現在のImGui context
        auto* const imguiContext = ImGui::GetCurrentContext();
        if (imguiContext == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 11 editor GUI renderer requires an ImGui context.");
        }

        // 生のdevice・context取得前にleaseを取り、ImGui初期化中のbackend切替を防ぐ。
        // 描画装置の再初期化を防ぐ使用権
        auto resourceLease = graphics.AcquireResourceLease();
        // 初期化済みD3D11 deviceの借用
        auto* const device =
            Detail::GraphicsDeviceD3D11Access::Device(graphics);
        // 初期化済みD3D11 contextの借用
        auto* const context =
            Detail::GraphicsDeviceD3D11Access::Context(graphics);
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
        m_graphics = &graphics;
        m_graphicsResourceLease = std::move(resourceLease);
        m_imguiContext = imguiContext;
        m_initialized = true;
    }

    void D3D11EditorGuiRenderer::NewFrame()
    {
        RequireCurrentContext();
        // ImGuiのtexture IDは生のSRV参照なので次のNewFrameまで所有参照を保持し、描画命令を先に消費する。
        m_frameTexturePins.clear();
        ImGui_ImplDX11_NewFrame();
    }

    ImTextureRef D3D11EditorGuiRenderer::TextureReference(
        const TextureAsset& texture)
    {
        RequireCurrentContext();
        // textureのGPU資源の所有参照
        const auto resources = texture.resources.Acquire();
        // 当該フレームで参照するSRV
        auto* const view = resources != nullptr
            ? Detail::GraphicsDeviceD3D11Access::
                TryResolveD3D11ShaderResourceView(
                    *m_graphics,
                    *resources)
            : nullptr;
        if (view == nullptr)
        {
            throw std::invalid_argument(
                "The editor GUI texture asset has no DirectX 11 shader "
                "resource view.");
        }
        m_frameTexturePins.emplace_back(view);
        return MakeD3D11TextureReference(
            m_frameTexturePins.back().Get());
    }

    ImTextureRef D3D11EditorGuiRenderer::DisplayTextureReference(
        const RenderTarget& target)
    {
        RequireCurrentContext();
        // 当該フレームで参照するSRV
        auto* const view = Detail::GraphicsDeviceD3D11Access::
            TryResolveD3D11ShaderResourceView(
                *m_graphics,
                target.DisplayViewHandle());
        if (view == nullptr)
        {
            throw std::invalid_argument(
                "The editor GUI render target has no DirectX 11 display "
                "shader resource view.");
        }
        m_frameTexturePins.emplace_back(view);
        return MakeD3D11TextureReference(
            m_frameTexturePins.back().Get());
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
            m_frameTexturePins.clear();
            m_graphics = nullptr;
            m_graphicsResourceLease.Reset();
            return;
        }

        // 終了処理後に戻すImGui context
        auto* const previousContext = ImGui::GetCurrentContext();
        if (previousContext != m_imguiContext)
        {
            ImGui::SetCurrentContext(m_imguiContext);
        }
        m_frameTexturePins.clear();
        ImGui_ImplDX11_Shutdown();
        if (previousContext != m_imguiContext)
        {
            ImGui::SetCurrentContext(previousContext);
        }
        m_graphics = nullptr;
        m_imguiContext = nullptr;
        m_initialized = false;
        // ImGuiの内部GPU資源を解放してから描画装置の使用権を返す。
        m_graphicsResourceLease.Reset();
    }
}
