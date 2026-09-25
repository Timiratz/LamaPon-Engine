#pragma once

#include "LamaPon/Graphics/GpuProfiler.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    // 描画イベントをどの経路から出したかです。
    enum class FrameDebugEventKind : std::uint8_t
    {
        // 通常の3D描画（GameObject::Render3D）。
        Draw3D,
        // 遮蔽シルエットなどの事前パス（GameObject::RenderPre3D）。
        PreRender3D,
        // スプライト・UIの描画（GameObject::Render2D）。
        Draw2D,
        // 同じ形状・マテリアルをまとめたインスタンス描画。
        InstancedBatch
    };

    // 描画先の用途です。GraphicsDeviceのDepthPassKindと対応します。
    enum class FrameDebugPass : std::uint8_t
    {
        Color,
        ShadowDepth,
        DepthPrepass
    };

    // Componentが自分の描画内容を説明するための入れ物です。
    // 分からない項目は空や0のままで構いません。
    struct FrameDebugDrawDescription final
    {
        // 描く形状です（モデルのパス、プリミティブ名、画像など）。
        std::string geometry;
        // マテリアル・シェーダー・テクスチャなどの見た目の指定です。
        std::string material;
        // 合成方式、深度、カリング、ワイヤーフレームなどの状態の要約です。
        std::string state;
        std::uint64_t vertexCount{};
        std::uint64_t triangleCount{};
        std::uint32_t instanceCount{ 1 };
    };

    struct FrameDebugEvent final
    {
        std::uint32_t index{};
        FrameDebugEventKind kind{ FrameDebugEventKind::Draw3D };
        FrameDebugPass pass{ FrameDebugPass::Color };
        // 開いていたGPU区間を"/"で連結した経路です（例: 3D描画）。
        std::string sectionPath;
        std::uint64_t objectId{};
        std::string objectName;
        std::string componentType;
        FrameDebugDrawDescription description;
        // 表示上限より後ろにあり、このフレームでは描かなかったイベントです。
        bool skipped{};
    };

    // フレームを構成する描画イベントを記録し、指定したイベントより後ろの
    // 描画を飛ばすことで「そこまで描いた途中の絵」を作ります（Unityの
    // Frame Debuggerに相当）。GPU区間の通知を受けてイベントをパスごとに
    // 分類します。フレームの区切りはGraphicsDevice::EndFrame（Present）です。
    //
    // 無効の間は何も記録せず、SubmitDrawEventは常にtrueを返します。
    class FrameDebugger final : public GpuSectionListener
    {
    public:
        void SetEnabled(bool enabled) noexcept;
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_enabled;
        }
        // limit番目（0始まり、この番号を含む）より後ろの描画を飛ばします。
        // nulloptで全て描きます。無効の間は適用しません。
        void SetEventLimit(std::optional<std::uint32_t> limit) noexcept;
        [[nodiscard]] std::optional<std::uint32_t>
            EventLimit() const noexcept
        {
            return m_limit;
        }

        // 描画イベントを登録します。falseを返したときは、呼び出し側は
        // その描画を飛ばしてください。記録に失敗した場合も描画は止めません。
        [[nodiscard]] bool SubmitDrawEvent(
            FrameDebugEventKind kind,
            FrameDebugPass pass,
            std::uint64_t objectId,
            std::string_view objectName,
            std::string_view componentType,
            FrameDebugDrawDescription description) noexcept;

        // 現在のフレームのイベントを確定し、次のフレームの記録を始めます。
        void EndFrame() noexcept;
        // 直前に確定したフレームのイベントです。
        [[nodiscard]] const std::vector<FrameDebugEvent>&
            LastFrameEvents() const noexcept
        {
            return m_lastFrame;
        }
        // 確定したフレームの数です。表示側が更新を検出するために使います。
        [[nodiscard]] std::uint64_t CompletedFrames() const noexcept
        {
            return m_completedFrames;
        }

        void OnGpuSectionBegin(std::string_view name) noexcept override;
        void OnGpuSectionEnd() noexcept override;

    private:
        [[nodiscard]] std::string CurrentSectionPath() const;

        std::vector<std::string> m_sections;
        std::vector<FrameDebugEvent> m_currentFrame;
        std::vector<FrameDebugEvent> m_lastFrame;
        std::optional<std::uint32_t> m_limit;
        std::uint64_t m_completedFrames{};
        // 記録に失敗したイベントも番号を消費し、上限との対応を保ちます。
        std::uint32_t m_nextIndex{};
        // 記録に失敗して入れ子が崩れたときに、対応するEndを捨てる数です。
        std::size_t m_droppedSections{};
        bool m_enabled{};
    };
}
