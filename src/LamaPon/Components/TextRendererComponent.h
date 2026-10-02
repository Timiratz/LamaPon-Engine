#pragma once

#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextTextureAsset;

    // 数値には有限値を指定し、文字内容・書体・サイズが同値なら画像生成を省きます。
    // 画像生成失敗後は内容・書体・サイズの同値更新でも再試行します。
    class TextRendererComponent final : public Component
    {
    public:
        // 文字の表示と配置を設定します(text: UTF-8文字列, fontFamily: UTF-8書体名, fontSize: 1以上にする文字サイズDIP, color: アルファ乗算前のRGBA色, layoutSize: 0〜4096に制限する幅・高さ, wordWrap: 自動折り返しの指定, horizontalAlignment: 横位置合わせ, verticalAlignment: 縦位置合わせ)。
        explicit TextRendererComponent(
            std::string text = "日本語テキスト",
            std::string fontFamily = "Yu Gothic UI",
            float fontSize = 32.0f,
            DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f },
            DirectX::XMFLOAT2 layoutSize = { 0.0f, 0.0f },
            bool wordWrap = false,
            TextHorizontalAlignment horizontalAlignment =
                TextHorizontalAlignment::Left,
            TextVerticalAlignment verticalAlignment =
                TextVerticalAlignment::Top);

        // 文字列を設定して画像を更新します(text: UTF-8文字列で空は描画なし)。
        void SetText(std::string text);
        // 表示するUTF-8文字列を返します。
        [[nodiscard]] const std::string& Text() const noexcept { return m_text; }

        // 書体を設定して画像を更新します(fontFamily: UTF-8書体名で空は既定書体)。
        void SetFontFamily(std::string fontFamily);
        // 設定したUTF-8書体名を返します。
        [[nodiscard]] const std::string& FontFamily() const noexcept
        {
            return m_fontFamily;
        }

        // 文字サイズを1以上に制限して画像を更新します(fontSize: 文字サイズDIP)。
        void SetFontSize(float fontSize);
        // 設定した文字サイズをDIPで返します。
        [[nodiscard]] float FontSize() const noexcept { return m_fontSize; }

        // 描画色を変更し画像生成が保留中なら再試行します(color: アルファ乗算前のRGBA色)。
        void SetColor(const DirectX::XMFLOAT4& color);
        // アルファ乗算前の描画RGBA色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4& Color() const noexcept { return m_color; }

        // 配置サイズを0〜4096に制限して画像を更新します(size: 0は測定値の使用を示す幅・高さ)。
        void SetLayoutSize(const DirectX::XMFLOAT2& size);
        // 配置幅・高さを返し0の軸は測定値の使用を示します。
        [[nodiscard]] const DirectX::XMFLOAT2& LayoutSize() const noexcept
        {
            return m_layout.size;
        }

        // 折り返しを設定して画像を更新します(wordWrap: 幅指定時の自動折り返しの指定)。
        void SetWordWrap(bool wordWrap);
        // 自動折り返しの指定を返します。
        [[nodiscard]] bool WordWrap() const noexcept { return m_layout.wordWrap; }

        // 横位置合わせを設定して画像を更新します(alignment: 幅指定時の横位置合わせ)。
        void SetHorizontalAlignment(TextHorizontalAlignment alignment);
        // 横位置合わせの指定を返します。
        [[nodiscard]] TextHorizontalAlignment HorizontalAlignment() const noexcept
        {
            return m_layout.horizontalAlignment;
        }

        // 縦位置合わせを設定して画像を更新します(alignment: 高さ指定時の縦位置合わせ)。
        void SetVerticalAlignment(TextVerticalAlignment alignment);
        // 縦位置合わせの指定を返します。
        [[nodiscard]] TextVerticalAlignment VerticalAlignment() const noexcept
        {
            return m_layout.verticalAlignment;
        }

        // 2D描画の並び順を設定します(sortOrder: 小さいほど先に描く順序)。
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        // 設定した2D描画の並び順を返します。
        [[nodiscard]] int SortOrder() const noexcept { return m_sortOrder; }

        // 文字列・書体サイズ・並び順の描画説明を設定します(description: 出力する説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "TextRenderer";
        }
        // 2D描画の並び順を返します。
        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        // 資産と描画装置を借用して文字画像を生成します(graphics: 文字画像を管理する描画装置)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 白い文字画像に描画色と2D姿勢を反映します(sprites: スプライト描画の実行先)。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        // 文字画像を生成し未初期化・空文字列なら画像を消去します。
        // 生成例外では旧画像と新設定を保持し、生成保留の印を残して画像設定のSetterで再試行します。
        void RefreshTexture();

        // 表示するUTF-8文字列
        std::string m_text;
        // UTF-8書体名
        std::string m_fontFamily;
        // 文字サイズDIP
        float m_fontSize;
        // アルファ乗算前の描画RGBA
        DirectX::XMFLOAT4 m_color;
        // 文字画像の配置・折り返し設定
        TextLayoutOptions m_layout;
        // 2D描画の並び順
        int m_sortOrder{};
        // 借用する資産管理
        AssetManager* m_assets{};
        // 借用する描画装置
        GraphicsDevice* m_graphics{};
        // 共有する白い文字画像
        std::shared_ptr<const TextTextureAsset> m_texture;
        // 生成失敗後の再試行待ち
        bool m_textureRefreshPending{};
    };
}
