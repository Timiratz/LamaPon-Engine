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
    struct TextTextureAsset;
    struct TextureAsset;

    // サイズ・文字サイズ・色には有限値を指定し、描画と入力は回転・スケールを使わない矩形で行います。
    class UIButtonComponent final : public Component
    {
    public:
        // ボタンの文字とサイズ・画像を設定します(label: UTF-8表示文字列, fallbackSize: 矩形なしの表示幅・高さ, texturePath: 背景画像パスで空は単色)。
        explicit UIButtonComponent(
            std::string label = "ボタン",
            DirectX::XMFLOAT2 fallbackSize =
                { 220.0f, 56.0f },
            std::filesystem::path texturePath = {});

        // 表示文字列を変更して文字画像を更新します(label: UTF-8文字列で空は表示なし)。
        void SetLabel(std::string label);
        // 書体を変更して文字画像を更新します(family: UTF-8書体名で空は既定書体)。
        void SetFontFamily(std::string family);
        // 文字画像のサイズを1〜256に制限して更新します(size: 文字サイズDIP)。
        void SetFontSize(float size);
        // 矩形なしの表示サイズを各軸1以上に制限します(size: 表示幅・高さピクセル)。
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        // 通常状態の背景色を設定します(color: アルファ乗算前のRGBA色)。
        void SetNormalColor(
            const DirectX::XMFLOAT4& color) noexcept;
        // ポインターが重なる背景色を設定します(color: アルファ乗算前のRGBA色)。
        void SetHoveredColor(
            const DirectX::XMFLOAT4& color) noexcept;
        // 内側から押し始めた状態の背景色を設定します(color: アルファ乗算前のRGBA色)。
        void SetPressedColor(
            const DirectX::XMFLOAT4& color) noexcept;
        // 入力無効時の背景色を設定します(color: アルファ乗算前のRGBA色)。
        void SetDisabledColor(
            const DirectX::XMFLOAT4& color) noexcept;
        // 文字色を変更して文字画像の取得を更新します(color: アルファ乗算前のRGBA色)。
        void SetTextColor(
            const DirectX::XMFLOAT4& color);
        // 入力を設定し無効時は重なりと押下状態を解除します(interactable: 入力を受ける指定)。
        void SetInteractable(bool interactable) noexcept;
        // 背景画像を読み込んでパスと画像を置換します(path: 画像パスで空は単色)。
        // 読み込み失敗時は現在のパスと画像を保持します。
        void SetTexturePath(
            std::filesystem::path path);
        // 2D描画の並び順を設定します(sortOrder: 小さいほど先に描く順序)。
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        // クリック時に要求するシーンを設定します(path: シーン参照パスで空は要求なし)。
        void SetTargetScene(
            std::filesystem::path path)
        {
            m_targetScene = std::move(path);
        }
        // クリック時の現在シーン再読込を設定します(reload: 再読込する指定)。
        // 再読込の指定はTargetSceneの指定より優先します。
        void SetReloadCurrentScene(
            bool reload) noexcept
        {
            m_reloadCurrentScene = reload;
        }
        // クリック先シーンの読み込み方式を設定します(additive: 現在シーンに追加する指定)。
        void SetLoadTargetAdditive(
            bool additive) noexcept
        {
            m_loadTargetAdditive = additive;
        }

        // 表示するUTF-8文字列を返します。
        [[nodiscard]] const std::string&
            Label() const noexcept { return m_label; }
        // 設定したUTF-8書体名を返します。
        [[nodiscard]] const std::string&
            FontFamily() const noexcept
        {
            return m_fontFamily;
        }
        // 文字画像内の文字サイズをDIPで返します。
        [[nodiscard]] float FontSize() const noexcept
        {
            return m_fontSize;
        }
        // UI矩形がない場合の表示幅・高さを返します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            FallbackSize() const noexcept
        {
            return m_fallbackSize;
        }
        // 通常状態のアルファ乗算前の背景色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            NormalColor() const noexcept
        {
            return m_normalColor;
        }
        // 重なり状態のアルファ乗算前の背景色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            HoveredColor() const noexcept
        {
            return m_hoveredColor;
        }
        // 押下状態のアルファ乗算前の背景色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            PressedColor() const noexcept
        {
            return m_pressedColor;
        }
        // 入力無効状態のアルファ乗算前の背景色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            DisabledColor() const noexcept
        {
            return m_disabledColor;
        }
        // アルファ乗算前の文字色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            TextColor() const noexcept
        {
            return m_textColor;
        }
        // 入力を受ける指定を返します。
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }
        // 入力領域を矩形か内接楕円に設定します(circular: 内接楕円を使う指定)。
        void SetCircularHitArea(
            const bool circular) noexcept
        {
            m_circularHitArea = circular;
        }
        // 入力に内接楕円を使う指定を返します。
        [[nodiscard]] bool
            CircularHitArea() const noexcept
        {
            return m_circularHitArea;
        }
        // 直近の更新でポインターが入力領域内だったか返します。
        [[nodiscard]] bool IsHovered() const noexcept
        {
            return m_hovered;
        }
        // 領域内から押し始めてまだ解放していない状態か返します。
        [[nodiscard]] bool IsPressed() const noexcept
        {
            return m_pressedInside;
        }
        // 今回の更新で領域内から押して内側で解放したか返します。
        [[nodiscard]] bool WasClicked() const noexcept
        {
            return m_clicked;
        }
        // 保持するクリック状態を返して消去します。
        [[nodiscard]] bool ConsumeClick() noexcept
        {
            // 消費するクリック状態
            const bool clicked = m_clicked;
            m_clicked = false;
            return clicked;
        }
        // クリック時のScene EventBus発行名を設定します(name: 発行名で空は発行なし)。
        void SetClickEventName(std::string name)
        {
            m_clickEventName = std::move(name);
        }
        // クリック時に発行するイベント名を返します。
        [[nodiscard]] const std::string&
            ClickEventName() const noexcept
        {
            return m_clickEventName;
        }
        // 背景画像のパスを返します。
        [[nodiscard]] const std::filesystem::path&
            TexturePath() const noexcept
        {
            return m_texturePath;
        }
        // クリック時に要求するシーンのパスを返します。
        [[nodiscard]] const std::filesystem::path&
            TargetScene() const noexcept
        {
            return m_targetScene;
        }
        // クリック時に現在シーンを再読込する指定を返します。
        [[nodiscard]] bool
            ReloadCurrentScene() const noexcept
        {
            return m_reloadCurrentScene;
        }
        // クリック先シーンを追加する指定を返します。
        [[nodiscard]] bool
            LoadTargetAdditive() const noexcept
        {
            return m_loadTargetAdditive;
        }
        // 設定した2D描画の並び順を返します。
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }

        // 画像とラベルの描画説明を設定します(description: 出力する説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIButton";
        }
        // 2D描画の並び順を返します。
        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        // 資産と描画装置を借用して背景と文字を読み込みます(graphics: 入力と画像を管理する描画装置)。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // 入力からクリックを判定してイベント発行とシーン要求を処理します(deltaTime: 使用しない経過秒数)。
        // クリック状態を毎回消去し、シーン入力停止中は押下を解除し、イベントをシーン要求より先に発行します。
        void OnUpdate(float deltaTime) override;
        // 状態別の背景と文字画像を矩形へ伸縮して描画します(sprites: スプライト描画の実行先)。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        // 512×128の固定領域に中央揃えの文字画像を生成し未初期化・空文字なら消去します。
        // 生成失敗時は旧画像と変更済み設定を保持して例外を伝播します。
        void RefreshText();

        // 表示するUTF-8文字列
        std::string m_label;
        // UTF-8書体名
        std::string m_fontFamily{
            "Yu Gothic UI" };
        // 文字画像内の文字サイズDIP
        float m_fontSize{ 24.0f };
        // 矩形なしの表示幅・高さ
        DirectX::XMFLOAT2 m_fallbackSize;
        // 通常時の乗算前の背景RGBA
        DirectX::XMFLOAT4 m_normalColor{
            0.08f, 0.28f, 0.52f, 0.96f };
        // 重なり時の乗算前の背景RGBA
        DirectX::XMFLOAT4 m_hoveredColor{
            0.12f, 0.42f, 0.76f, 1.0f };
        // 押下時の乗算前の背景RGBA
        DirectX::XMFLOAT4 m_pressedColor{
            0.04f, 0.20f, 0.40f, 1.0f };
        // 無効時の乗算前の背景RGBA
        DirectX::XMFLOAT4 m_disabledColor{
            0.18f, 0.20f, 0.24f, 0.65f };
        // アルファ乗算前の文字RGBA
        DirectX::XMFLOAT4 m_textColor{
            1.0f, 1.0f, 1.0f, 1.0f };
        // 入力を受ける指定
        bool m_interactable{ true };
        // 内接楕円を入力に使う指定
        bool m_circularHitArea{};
        // ポインターが入力領域内
        bool m_hovered{};
        // 内側で押し始め解放前の状態
        bool m_pressedInside{};
        // 今回の更新のクリック状態
        bool m_clicked{};
        // クリック時の発行名
        std::string m_clickEventName;
        // 背景画像のパス
        std::filesystem::path m_texturePath;
        // クリック先のシーンパス
        std::filesystem::path m_targetScene;
        // クリック時の再読込指定
        bool m_reloadCurrentScene{};
        // クリック先の追加読込指定
        bool m_loadTargetAdditive{};
        // 2D描画の並び順
        int m_sortOrder{};
        // 共有する背景画像
        std::shared_ptr<const TextureAsset>
            m_texture;
        // 共有する白い文字画像
        std::shared_ptr<const TextTextureAsset>
            m_textTexture;
        // 借用する描画装置
        GraphicsDevice* m_graphics{};
        // 借用する資産管理
        AssetManager* m_assets{};
    };
}
