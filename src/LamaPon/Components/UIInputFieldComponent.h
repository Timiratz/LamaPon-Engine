#pragma once

#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <cstddef>
#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextTextureAsset;

    // クリックでフォーカスし、確定文字入力とEnterによる確定を受け付ける。
    // 本文は有効なUTF-8、数値設定は有限値を前提とし、フォーカス取得は他の入力欄を解除しない。
    class UIInputFieldComponent final : public Component
    {
    public:
        // UTF-8の本文と空欄時の案内を持つ入力欄を作る(text: 初期本文, placeholder: 空欄時の案内)。
        explicit UIInputFieldComponent(
            std::string text = {},
            std::string placeholder =
                "テキストを入力...");

        // 本文を置き換え、変更通知と文字画像の更新を行う(text: UTF-8本文)。
        // 最大文字数は適用せず、画像生成失敗時も本文と変更通知は更新済みとなる。
        void SetText(std::string text);
        // 空欄時の案内を設定して文字画像を更新する(placeholder: UTF-8の案内文)。
        void SetPlaceholder(std::string placeholder);
        // 書体を設定して文字画像を更新する(family: UTF-8書体名で空は既定書体)。
        void SetFontFamily(std::string family);
        // 文字画像のDIP単位の文字サイズを1〜256に収めて更新する(size: 文字サイズ)。
        void SetFontSize(float size);
        // 入力時の最大コードポイント数を1〜4096に収める(maxLength: 最大文字数)。
        // 既存の本文やSetTextによる本文は切り詰めない。
        void SetMaxLength(std::size_t maxLength) noexcept;
        // 操作可否を設定し、操作禁止時はフォーカスを外す(interactable: 操作を許可する指定)。
        void SetInteractable(bool interactable) noexcept;
        // 非フォーカス時の背景色を設定する(color: アルファ乗算前のRGBA)。
        void SetBackgroundColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_backgroundColor = color;
        }
        // フォーカス時の背景色を設定する(color: アルファ乗算前のRGBA)。
        void SetFocusedColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_focusedColor = color;
        }
        // 本文の描画色を設定して文字画像を更新する(color: アルファ乗算前のRGBA)。
        void SetTextColor(const DirectX::XMFLOAT4& color);
        // 案内文の描画色を設定して文字画像を更新する(color: アルファ乗算前のRGBA)。
        void SetPlaceholderColor(
            const DirectX::XMFLOAT4& color);
        // UI矩形がない場合の表示サイズを各軸1以上に収める(size: 表示幅と高さのピクセル数)。
        void SetFallbackSize(
            const DirectX::XMFLOAT2& size) noexcept;
        // 2D描画の並び順を設定する(sortOrder: 小さいほど先に描く順番)。
        void SetSortOrder(const int sortOrder) noexcept
        {
            m_sortOrder = sortOrder;
        }
        // 操作可能な場合に入力フォーカスを取得する。
        void Focus() noexcept
        {
            m_focused = m_interactable;
        }
        // 入力フォーカスを外す。
        void Blur() noexcept
        {
            m_focused = false;
        }

        // UTF-8の本文を取得する。
        [[nodiscard]] const std::string&
            Text() const noexcept
        {
            return m_text;
        }
        // 空欄時に表示するUTF-8の案内文を取得する。
        [[nodiscard]] const std::string&
            Placeholder() const noexcept
        {
            return m_placeholder;
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
        // 入力時に適用する最大コードポイント数を取得する。
        [[nodiscard]] std::size_t MaxLength() const noexcept
        {
            return m_maxLength;
        }
        // この入力欄が入力を受け付ける状態か確認する。
        [[nodiscard]] bool IsFocused() const noexcept
        {
            return m_focused;
        }
        // 操作を許可しているか確認する。
        [[nodiscard]] bool Interactable() const noexcept
        {
            return m_interactable;
        }
        // 本文更新の通知を取得し、その通知を消費する。
        [[nodiscard]] bool ConsumeValueChanged() noexcept
        {
            // 消費する本文更新通知
            const bool changed = m_valueChanged;
            m_valueChanged = false;
            return changed;
        }
        // Enterによる確定通知を取得し、その通知を消費する。
        [[nodiscard]] bool ConsumeSubmit() noexcept
        {
            // 消費するEnter確定通知
            const bool submitted = m_submitted;
            m_submitted = false;
            return submitted;
        }
        // 非フォーカス時のアルファ乗算前の背景色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            BackgroundColor() const noexcept
        {
            return m_backgroundColor;
        }
        // フォーカス時のアルファ乗算前の背景色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            FocusedColor() const noexcept
        {
            return m_focusedColor;
        }
        // 本文のアルファ乗算前の描画色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            TextColor() const noexcept
        {
            return m_textColor;
        }
        // 案内文のアルファ乗算前の描画色を取得する。
        [[nodiscard]] const DirectX::XMFLOAT4&
            PlaceholderColor() const noexcept
        {
            return m_placeholderColor;
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

        // 入力欄の描画情報を記録する(description: 書き込み先の描画説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // シリアライズ用のコンポーネント識別名を取得する。
        [[nodiscard]] std::string_view
            TypeName() const noexcept override
        {
            return "UIInputField";
        }
        // 2D描画を整列するための順番を取得する。
        [[nodiscard]] int
            RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        // 描画機器とアセットを借用し、文字画像を生成する(graphics: 所有者より長寿命の描画機器)。
        void OnInitialize(
            GraphicsDevice& graphics) override;
        // クリック、文字入力、確定とキャレット点滅を更新する(deltaTime: 経過秒数)。
        void OnUpdate(float deltaTime) override;
        // 背景、文字画像と右端のキャレットを描く(sprites: スプライト描画先)。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        // 本文と案内文の画像を512×64で生成し、表示時に入力欄へ拡縮する。
        // 生成失敗時は設定を保持し、本文と案内文の画像は個別に更新された段階で例外を返す。
        void RefreshText();
        // UTF-16のコード単位を受け、サロゲートペアを合成して本文に追加する(character: 入力のUTF-16コード単位)。
        void AppendCharacter(wchar_t character);
        // 本文末尾のUTF-8コードポイントを一つ削除する。
        void RemoveLastCodePoint();

        // UTF-8の本文
        std::string m_text;
        // 空欄時の案内文
        std::string m_placeholder;
        // 文字画像の書体名
        std::string m_fontFamily{ "Yu Gothic UI" };
        // 文字画像のDIPサイズ
        float m_fontSize{ 24.0f };
        // 入力時の最大文字数
        std::size_t m_maxLength{ 256 };
        // 操作を許可する指定
        bool m_interactable{ true };
        // 入力フォーカスの有無
        bool m_focused{};
        // 未消費の本文更新通知
        bool m_valueChanged{};
        // 未消費のEnter確定通知
        bool m_submitted{};
        // キャレット点滅の経過秒数
        float m_caretTimer{};
        // 保留中の上位サロゲート
        wchar_t m_pendingHighSurrogate{};
        // 通常背景のRGBA
        DirectX::XMFLOAT4 m_backgroundColor{
            0.10f, 0.12f, 0.16f, 0.96f };
        // フォーカス時背景のRGBA
        DirectX::XMFLOAT4 m_focusedColor{
            0.14f, 0.20f, 0.30f, 1.0f };
        // 本文のRGBA
        DirectX::XMFLOAT4 m_textColor{
            1.0f, 1.0f, 1.0f, 1.0f };
        // 案内文のRGBA
        DirectX::XMFLOAT4 m_placeholderColor{
            0.65f, 0.68f, 0.75f, 0.8f };
        // 矩形未指定時の表示サイズ
        DirectX::XMFLOAT2 m_fallbackSize{
            280.0f, 44.0f };
        // 2D描画の並び順
        int m_sortOrder{};
        // 本文の白色文字画像
        std::shared_ptr<const TextTextureAsset>
            m_textTexture;
        // 案内文の白色文字画像
        std::shared_ptr<const TextTextureAsset>
            m_placeholderTexture;
        // 借用した描画機器
        GraphicsDevice* m_graphics{};
        // 借用したアセット管理
        AssetManager* m_assets{};
    };
}
