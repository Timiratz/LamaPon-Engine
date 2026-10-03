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
    // 描画イベントを発行した経路。
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

    // GraphicsDeviceのDepthPassKindに対応する描画先の用途。
    enum class FrameDebugPass : std::uint8_t
    {
        Color,
        ShadowDepth,
        DepthPrepass
    };

    // 描画内容の説明で、不明な項目は空または0とする。
    struct FrameDebugDrawDescription final
    {
        // 形状名または画像・モデルのパス
        std::string geometry;
        // 材質・シェーダー・画像の指定
        std::string material;
        // 合成・深度・カリングの要約
        std::string state;
        // 描画する頂点数
        std::uint64_t vertexCount{};
        // 描画する三角形数
        std::uint64_t triangleCount{};
        // 描画するインスタンス数
        std::uint32_t instanceCount{ 1 };
    };

    struct FrameDebugEvent final
    {
        // フレーム内の0始まりの番号
        std::uint32_t index{};
        // イベントを発行した経路
        FrameDebugEventKind kind{ FrameDebugEventKind::Draw3D };
        // 描画先の用途
        FrameDebugPass pass{ FrameDebugPass::Color };
        // 開いたGPU区間を結ぶ経路
        std::string sectionPath;
        // 描画物体の識別子
        std::uint64_t objectId{};
        // 描画物体の名前
        std::string objectName;
        // 描画コンポーネントの型名
        std::string componentType;
        // 描画内容の説明
        FrameDebugDrawDescription description;
        // 上限以降で描画を省く有無
        bool skipped{};
    };

    // GPU区間に沿って描画イベントを記録し、上限以降の描画を省いて途中の画像を表示する。
    // 無効時は記録せず、フレームの確定はGraphicsDeviceのPresent時に行う。
    class FrameDebugger final : public GpuSectionListener
    {
    public:
        // 記録の有効状態を切り替え、処理中の記録を消去する(enabled: 有効にするフラグ)。
        // 無効化で確定済み記録も消すが、上限と完了フレーム数は保持する。
        void SetEnabled(bool enabled) noexcept;
        // 記録の有効状態を返す。
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_enabled;
        }
        // 有効時に描画する最終番号を設定する(limit: 最終描画番号、空なら無制限)。
        void SetEventLimit(std::optional<std::uint32_t> limit) noexcept;
        // 設定済みの描画イベント上限を返す。
        [[nodiscard]] std::optional<std::uint32_t>
            EventLimit() const noexcept
        {
            return m_limit;
        }

        // イベントを登録して描画すべきか返す(kind: 発行経路, pass: 描画先の用途, objectId: 物体の識別子, objectName: 物体名, componentType: コンポーネント名, description: 描画内容の説明)。
        // falseなら呼出側で描画を省き、記録失敗でも番号の消費と上限判定は維持する。
        [[nodiscard]] bool SubmitDrawEvent(
            FrameDebugEventKind kind,
            FrameDebugPass pass,
            std::uint64_t objectId,
            std::string_view objectName,
            std::string_view componentType,
            FrameDebugDrawDescription description) noexcept;

        // 有効時の記録を確定して次フレーム用に初期化する。
        void EndFrame() noexcept;
        // 直近に確定したイベント一覧を借用する。
        [[nodiscard]] const std::vector<FrameDebugEvent>&
            LastFrameEvents() const noexcept
        {
            return m_lastFrame;
        }
        // 有効中に確定したフレームの累積数を返す。
        [[nodiscard]] std::uint64_t CompletedFrames() const noexcept
        {
            return m_completedFrames;
        }

        // 有効時にGPU区間名を積み、記録失敗なら欠落数を増やす(name: 開始する区間名)。
        void OnGpuSectionBegin(std::string_view name) noexcept override;
        // 欠落した開始を考慮してGPU区間の入れ子を閉じる。
        void OnGpuSectionEnd() noexcept override;

    private:
        // 開いているGPU区間をスラッシュで連結した経路を返す。
        [[nodiscard]] std::string CurrentSectionPath() const;

        // 開いているGPU区間名の一覧
        std::vector<std::string> m_sections;
        // 処理中のフレームのイベント
        std::vector<FrameDebugEvent> m_currentFrame;
        // 確定した直前フレームのイベント
        std::vector<FrameDebugEvent> m_lastFrame;
        // 描画する最終イベント番号
        std::optional<std::uint32_t> m_limit;
        // 確定済みの累積フレーム数
        std::uint64_t m_completedFrames{};
        // 次のイベント番号、欠落も消費
        std::uint32_t m_nextIndex{};
        // 記録失敗で省いた区間開始の数
        std::size_t m_droppedSections{};
        // 描画イベント記録の有効有無
        bool m_enabled{};
    };
}
