#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextureAsset;

    // UI画像を描き、数値設定には有限値を指定します。
    class UIImageComponent final : public Component
    {
    public:
        // UI画像のパスと描画色を設定します(texturePath: 通常画像パスで空は単色描画, color: アルファ乗算前のRGBA色)。
        explicit UIImageComponent(
            std::filesystem::path texturePath = {},
            DirectX::XMFLOAT4 color =
                { 1.0f, 1.0f, 1.0f, 1.0f });

        // 通常画像を読み込んでパスと画像を置換します(path: 画像パスで空は単色描画)。
        // 読み込み失敗時は現在のパスと画像を保持します。
        void SetTexturePath(std::filesystem::path path);
        // 描画時に乗算する色を設定します(color: アルファ乗算前のRGBA色)。
        void SetColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_color = color;
        }
        // 9分割境界を非負に制限して設定します(border: 元画像ピクセルの左・上・右・下幅)。
        // すべて0なら通常描画で、表示領域が角より小さい場合は角も縮小します。
        void SetBorder(
            const DirectX::XMFLOAT4& border) noexcept;
        // UI矩形がない場合の幅・高さを1以上に制限します(size: 表示幅・高さピクセル)。
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        // 2D描画の並び順を設定します(sortOrder: 小さいほど先に描く順序)。
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        // カメラ出力の表示名を設定します(name: カメラの出力名で空は通常画像)。
        // 有効な出力画像を取得できれば通常画像より優先し、9分割を無効にします。
        void SetRenderTexture(std::string name)
        {
            m_renderTexture = std::move(name);
        }
        // 表示するカメラ出力の名前を返します。
        [[nodiscard]] const std::string&
            RenderTexture() const noexcept
        {
            return m_renderTexture;
        }

        // 通常画像のパスを返します。
        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        // アルファ乗算前の描画RGBA色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            Color() const noexcept
        {
            return m_color;
        }
        // 元画像ピクセルの左・上・右・下の分割幅を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            Border() const noexcept
        {
            return m_border;
        }
        // UI矩形がない場合の表示幅・高さを返します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            FallbackSize() const noexcept
        {
            return m_fallbackSize;
        }
        // 設定した2D描画の並び順を返します。
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }

        // 画像またはカメラ出力の描画説明を設定します(description: 出力する説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIImage";
        }
        // 2D描画の並び順を返します。
        [[nodiscard]] int
            RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        // 描画装置と資産を借用して通常画像を読み込みます(graphics: 画像を管理する描画装置)。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // 画像全体または9分割を表示矩形へ描画します(sprites: スプライト描画の実行先)。
        // UI矩形がある場合だけ所有物体のZ回転を矩形中心回りに適用します。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        // 通常画像のパス
        std::filesystem::path m_texturePath;
        // カメラ出力画像の参照名
        std::string m_renderTexture;
        // アルファ乗算前の描画RGBA
        DirectX::XMFLOAT4 m_color;
        // 元画像の左・上・右・下幅
        DirectX::XMFLOAT4 m_border{};
        // 矩形なしの表示幅・高さ
        DirectX::XMFLOAT2 m_fallbackSize{
            100.0f, 100.0f };
        // 2D描画の並び順
        int m_sortOrder{};
        // 共有する通常画像
        std::shared_ptr<const TextureAsset> m_texture;
        // 借用する描画装置
        GraphicsDevice* m_graphics{};
        // 借用する資産管理
        AssetManager* m_assets{};
    };
}
