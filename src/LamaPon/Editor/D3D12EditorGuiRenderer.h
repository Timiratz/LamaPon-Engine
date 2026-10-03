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
        // 未初期化のD3D12 GUI描画処理を作る。
        D3D12EditorGuiRenderer() = default;
        // ShutdownでGUI資源と描画装置の使用権を解放する。
        ~D3D12EditorGuiRenderer() override;

        // 描画資源の共有を禁止する。
        D3D12EditorGuiRenderer(
            const D3D12EditorGuiRenderer&) = delete;
        // 描画資源の共有を禁止する。
        D3D12EditorGuiRenderer& operator=(
            const D3D12EditorGuiRenderer&) = delete;

        // 実験的DirectX12を返す。
        [[nodiscard]] RenderingApi Api() const noexcept override
        {
            return RenderingApi::DirectX12Experimental;
        }
        // GUI描画資源が初期化済みかを返す。
        [[nodiscard]] bool IsInitialized() const noexcept override
        {
            return m_initialized;
        }

        // 現在のImGui contextへdescriptor管理を含む描画資源を初期化する(graphics: Shutdownまで存続する描画装置)。
        void Initialize(GraphicsDevice& graphics) override;
        // 前フレームのGPU参照を解放して次のImGuiフレームを始める。
        void NewFrame() override;
        // 現フレームのtexture参照を返す(texture: 当該フレームで描画するtexture)。
        [[nodiscard]] ImTextureRef TextureReference(
            const TextureAsset& texture) override;
        // 描画先を表示する現フレームのtexture参照を返す(target: 表示する描画先の借用)。
        [[nodiscard]] ImTextureRef DisplayTextureReference(
            const RenderTarget& target) override;
        // 現在のフレームのcommand listへGUI描画を記録する(drawData: 非nullのフレーム描画命令)。
        void RenderDrawData(ImDrawData* drawData) override;
        // 初期化時のcontextが存続する間にGUI資源と所有descriptorを解放する。
        void Shutdown() noexcept override;

    private:
        // 所有記録に登録してImGui用SRV descriptorを割り当てる(info: rendererを含む初期化情報, cpu: CPU descriptorの出力先, gpu: GPU descriptorの出力先)。
        static void AllocateDescriptor(
            ImGui_ImplDX12_InitInfo* info,
            D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
        // 所有記録にあるSRV descriptorだけを解放する(info: rendererを含む初期化情報, cpu: 解放するCPU descriptor, gpu: callback互換用・未使用)。
        static void FreeDescriptor(
            ImGui_ImplDX12_InitInfo* info,
            D3D12_CPU_DESCRIPTOR_HANDLE cpu,
            D3D12_GPU_DESCRIPTOR_HANDLE gpu);
        // backendの初期化と初期化時contextがcurrentであることを要求する。
        void RequireCurrentContext() const;
        // 現世代のSRVを解決して次のNewFrameまでGPU参照を保持する(view: フレーム内で使うSRV参照)。
        [[nodiscard]] ImTextureRef TextureReference(
            const GraphicsViewHandle& view);
        // 全所有descriptorをbackendへ返して所有記録を空にする。
        void ReleaseOwnedDescriptors() noexcept;

        // 初期化中に借用する描画装置
        GraphicsDevice* m_graphics{};
        // 使用権で世代を固定したbackend
        D3D12Backend* m_backend{};
        // Shutdownまでの描画装置の使用権
        GraphicsDeviceResourceLease m_graphicsResourceLease;
        // 初期化時のImGui context
        ImGuiContext* m_imguiContext{};
        // 当該フレームのGPU参照の保持先
        std::vector<GraphicsViewHandle> m_frameTexturePins;
        // CPU参照と所有descriptor番号の対応
        std::unordered_map<std::uintptr_t, std::uint32_t>
            m_ownedDescriptorSlots;
        // GUI描画資源が初期化済みか
        bool m_initialized{};
    };
}
