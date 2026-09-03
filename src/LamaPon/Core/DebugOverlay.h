#pragma once

#include <string>
#include <vector>

namespace LamaPon
{
    class GraphicsDevice;
    class Scene;

    // エクスポートしたゲームでF1キーで表示を切り替える
    // デバッグオーバーレイです。FPS・描画/物理統計・直近の
    // 警告/エラーログを画面左上へ描画します。
    class DebugOverlay final
    {
    public:
        // 毎フレーム呼びます（F1の切り替え検出と、表示中なら描画）。
        void Update(
            GraphicsDevice& graphics,
            Scene& scene,
            float deltaTime);

        void SetVisible(const bool visible) noexcept
        {
            m_visible = visible;
        }
        [[nodiscard]] bool IsVisible() const noexcept
        {
            return m_visible;
        }

    private:
        struct OverlayLine final
        {
            std::string text;
            // 0=通常、1=警告、2=エラー（表示色に使います）。
            int severity{};
        };

        // テキストテクスチャのキャッシュ増加を抑えるため、
        // 表示文字列は一定間隔でだけ作り直します。
        void RefreshLines(
            GraphicsDevice& graphics,
            Scene& scene);
        void Draw(GraphicsDevice& graphics);

        bool m_visible{};
        bool m_toggleHeld{};
        float m_refreshTimer{};
        std::vector<OverlayLine> m_lines;
    };
}
