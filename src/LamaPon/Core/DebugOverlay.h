#pragma once

#include <string>
#include <vector>

namespace LamaPon
{
    class GraphicsDevice;
    class Scene;

    // エクスポートしたゲームでF1キーで表示を切り替えるデバッグオーバーレイです。
    // FPS・描画/物理統計・直近の警告/エラーログを画面左上へ描画します。
    class DebugOverlay final
    {
    public:
        // F1の切り替えを検出し表示中なら描画します(graphics: 描画サービス, scene: 統計を読むシーン, deltaTime: 経過秒数)。
        // 本文は0.3秒ごとに更新し、失敗時は表示を閉じてゲームを継続します。
        void Update(
            GraphicsDevice& graphics,
            Scene& scene,
            float deltaTime);

        // オーバーレイの表示を設定します(visible: 表示するか)。
        void SetVisible(const bool visible) noexcept
        {
            m_visible = visible;
        }
        // オーバーレイを表示する設定かを返します。
        [[nodiscard]] bool IsVisible() const noexcept
        {
            return m_visible;
        }

    private:
        struct OverlayLine final
        {
            // 表示する1行の本文
            std::string text;
            // 重要度、0通常・1警告・2エラー
            int severity{};
        };

        // 統計と直近の警告・エラーで表示行を更新します(graphics: 描画統計の参照先, scene: シーン統計の参照先)。
        void RefreshLines(
            GraphicsDevice& graphics,
            Scene& scene);
        // 表示行と背景を画面左上へ描画します(graphics: 描画サービス)。
        void Draw(GraphicsDevice& graphics);

        // オーバーレイを表示するか
        bool m_visible{};
        // 前回のF1キー押下状態
        bool m_toggleHeld{};
        // 本文更新までの累積秒数
        float m_refreshTimer{};
        // 統計と診断の表示行
        std::vector<OverlayLine> m_lines;
    };
}
