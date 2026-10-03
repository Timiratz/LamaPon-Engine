#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextTextureAsset;

    // 表示矩形内で押して離すと切り替わる、ラベル付きオン／オフUI。
    // 数値設定は有限値を前提とし、描画と入力判定は所有者の回転・拡縮を適用しない。
    class UIToggleComponent final : public Component
    {
    public:
        // ラベル付きのオン／オフUIを作る(label: UTF-8の表示ラベル, isOn: 初期のオン状態)。
        explicit UIToggleComponent(
            std::string label = "トグル",
            bool isOn = false);

        // オン状態を設定し、状態が変わった場合だけ変更通知を記録する(isOn: 設定するオン状態)。
        void SetIsOn(bool isOn) noexcept;
        // 表示ラベルを設定して文字画像を更新する(label: UTF-8の表示ラベル)。
        void SetLabel(std::string label);
        // 書体を設定して文字画像を更新する(family: UTF-8書体名で空は既定書体)。
        void SetFontFamily(std::string family);
        // 文字画像のDIP単位の文字サイズを1〜256に収めて更新する(size: 文字サイズ)。
        void SetFontSize(float size);
        // 操作可否を設定し、操作禁止時は押下状態を解除する(interactable: 操作を許可する指定)。
        void SetInteractable(bool interactable) noexcept;
        // チェック枠の背景色を設定する(color: アルファ乗算前のRGBA)。
        void SetBoxColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_boxColor = color;
        }
        // オン時の枠内の塗りつぶし色を設定する(color: アルファ乗算前のRGBA)。
        void SetCheckColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_checkColor = color;
        }
        // ラベルの描画色を設定して文字画像を更新する(color: アルファ乗算前のRGBA)。
        void SetTextColor(const DirectX::XMFLOAT4& color);
        // UI矩形がない場合の表示サイズを各軸1以上に収める(size: 表示幅と高さのピクセル数)。
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        // 2D描画の並び順を設定する(sortOrder: 小さいほど先に描く順番)。
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }

        // 現在のオン状態を取得する。
        [[nodiscard]] bool IsOn() const noexcept
        {
            return m_isOn;
        }
        // 値の変更通知を取得し、その通知を消費する。
        [[nodiscard]] bool ConsumeValueChanged() noexcept
        {
            // 消費する値変更通知
            const bool changed = m_valueChanged;
            m_valueChanged = false;
            return changed;
        }
        // 表示するUTF-8のラベルを取得する。
        [[nodiscard]] const std::string&
            Label() const noexcept
        {
            return m_label;
        }
        // 文字画像に使うUTF-8の書体名を取得する。
        [[nodiscard]] const std::string&
            FontFamily() const noexcept
        {
            return m_fontFamily;
        }
        // 文字画像生成時のDIP単位の文字サイズを取得する。
        [[nodiscard]] float FontSize() const noexcept
        {
            return m_fontSize;
        }
        // 操作を許可しているか確認する。
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }
        // チェック枠背景のアルファ乗算前の色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            BoxColor() const noexcept
        {
            return m_boxColor;
        }
        // オン時の塗りつぶしのアルファ乗算前の色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            CheckColor() const noexcept
        {
            return m_checkColor;
        }
        // ラベルのアルファ乗算前の描画色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            TextColor() const noexcept
        {
            return m_textColor;
        }
        // UI矩形がない場合の表示幅と高さを取得する。
        [[nodiscard]] const DirectX::XMFLOAT2&
            FallbackSize() const noexcept
        {
            return m_fallbackSize;
        }
        // 設定された2D描画順を取得する。
        [[nodiscard]] int SortOrder() const noexcept
        {
            return m_sortOrder;
        }

        // UI部品の描画情報を記録する(description: 書き込み先の描画説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIToggle";
        }
        // 2D描画を整列するための順番を取得する。
        [[nodiscard]] int
            RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        // 描画機器とアセットを借用してラベル画像を生成する(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // 表示矩形内で押して離した場合にオン状態を切り替える(deltaTime: 未使用の経過秒数)。
        void OnUpdate(float deltaTime) override;
        // UI部品の背景と表示内容を描く(sprites: スプライト描画先)。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        // 512×128のラベル画像を生成し、表示時にラベル領域へ拡縮する。
        // 生成失敗時は更新済みの設定と以前の画像が残り、文字画像に関わるSetterで再試行できる。
        void RefreshText();

        // UTF-8の表示ラベル
        std::string m_label;
        // 文字画像の書体名
        std::string m_fontFamily{ "Yu Gothic UI" };
        // 文字画像のDIPサイズ
        float m_fontSize{ 24.0f };
        // 現在のオン状態
        bool m_isOn{};
        // 操作を許可する指定
        bool m_interactable{ true };
        // ポインターが矩形内か
        bool m_hovered{};
        // 矩形内で始まった押下
        bool m_pressedInside{};
        // 未消費の値変更通知
        bool m_valueChanged{};
        // チェック枠背景のRGBA
        DirectX::XMFLOAT4 m_boxColor{
            0.16f, 0.20f, 0.28f, 0.96f };
        // オン時の塗りつぶしRGBA
        DirectX::XMFLOAT4 m_checkColor{
            0.30f, 0.75f, 0.40f, 1.0f };
        // ラベルのRGBA
        DirectX::XMFLOAT4 m_textColor{
            1.0f, 1.0f, 1.0f, 1.0f };
        // 矩形未指定時の表示サイズ
        DirectX::XMFLOAT2 m_fallbackSize{
            220.0f, 40.0f };
        // 2D描画の並び順
        int m_sortOrder{};
        // ラベルの白色文字画像
        std::shared_ptr<const TextTextureAsset>
            m_textTexture;
        // 借用した描画機器
        GraphicsDevice* m_graphics{};
        // 借用したアセット管理
        AssetManager* m_assets{};
    };
}
