#pragma once

#include "LamaPon/Graphics/SpriteRendering.h"
#include "LamaPon/Scene/Component.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace LamaPon
{
    class AssetManager;
    class GraphicsDevice;
    struct TextureAsset;
}

namespace LamaPon
{
    // 有効な最近傍1個のマスクに対する内外の表示を選びます。
    enum class SpriteMaskInteraction
    {
        // マスクに従わず描画します。
        None,
        // 最近傍のマスク内だけを描画します。
        VisibleInsideMask,
        // 最近傍のマスク外だけを描画します。
        VisibleOutsideMask
    };

    // サイズ・色・基準点・画像領域・描画定数には有限値を指定します。
    class SpriteRendererComponent final : public Component
    {
    public:
        // スプライトの描画定数の個数
        static constexpr std::size_t CustomParameterCount = 8;
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        // 画像を描く2D部品を作ります(size: 表示幅・高さ, color: アルファ乗算前のRGBA色, texturePath: 画像パスで空は単色描画)。
        explicit SpriteRendererComponent(
            DirectX::XMFLOAT2 size = { 128.0f, 128.0f },
            DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f },
            std::filesystem::path texturePath = {}) noexcept;

        // 表示幅・高さを設定します(size: 2D表示サイズ)。
        void SetSize(const DirectX::XMFLOAT2& size) noexcept { m_size = size; }
        // 描画時に乗算する色を設定します(color: アルファ乗算前のRGBA色)。
        void SetColor(const DirectX::XMFLOAT4& color) noexcept { m_color = color; }
        // ワールドSpriteの位置・回転の基準点を設定します(pivot: 左上0・右下1のXY比率)。
        // UI矩形がある場合はこの値を使わず、矩形中心を回転基準にします。
        void SetPivot(const DirectX::XMFLOAT2& pivot) noexcept
        {
            m_pivot = pivot;
        }
        // ワールドSpriteのXY基準点比率を返します。
        [[nodiscard]] const DirectX::XMFLOAT2&
            Pivot() const noexcept
        {
            return m_pivot;
        }
        // 画像を読み込んでパスと画像を置換します(texturePath: 画像パスで空は単色描画)。
        // 読み込み失敗時は現在のパスと画像を保持します。
        void SetTexturePath(std::filesystem::path texturePath);
        // 2D描画の並び順を設定します(sortOrder: 小さいほど先に描く順序)。
        void SetSortOrder(const int sortOrder) noexcept { m_sortOrder = sortOrder; }
        // 標準ワールドSpriteのマスク内外表示を設定します(interaction: マスクとの関係)。
        void SetMaskInteraction(
            const SpriteMaskInteraction interaction) noexcept
        {
            m_maskInteraction = interaction;
        }
        // マスク内外の表示指定を返します。
        [[nodiscard]] SpriteMaskInteraction
            MaskInteraction() const noexcept
        {
            return m_maskInteraction;
        }
        // 次の描画で使う独自シェーダーを設定します(shaderPath: シェーダーパスで空は標準描画)。
        void SetShaderPath(std::filesystem::path shaderPath)
        {
            m_shaderPath = std::move(shaderPath);
        }
        // 範囲内の描画定数だけを設定します(index: 0〜7の定数番号, value: 定数の4成分)。
        // 独自シェーダーの5〜7番は描画色・矩形・表示サイズ情報で上書きされます。
        void SetCustomParameter(
            const std::size_t index,
            const DirectX::XMFLOAT4& value) noexcept
        {
            if (index < m_customParameters.size())
            {
                m_customParameters[index] = value;
            }
        }
        // 初期化済みでパスがあれば独自シェーダーのキャッシュを無効化します。
        void ReloadShader();
        // 表示する画像領域を設定します(sourceRect: 正規化X・Y・幅・高さ)。
        // 通常画像を保持する場合に使い、全体は{0,0,1,1}で値の範囲は検証しません。
        void SetSourceRect(
            const DirectX::XMFLOAT4& sourceRect) noexcept
        {
            m_sourceRect = sourceRect;
        }
        // 正規化したX・Y・幅・高さの画像領域を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            SourceRect() const noexcept
        {
            return m_sourceRect;
        }
        // 描画済みのカメラ画像を指定します(name: カメラの出力名で空は通常画像)。
        // 名前に対応する有効な画像を取得できる場合に通常画像より優先します。
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
        // 設定した2D表示幅・高さを返します。
        [[nodiscard]] const DirectX::XMFLOAT2& Size() const noexcept { return m_size; }
        // アルファ乗算前の描画RGBA色を返します。
        [[nodiscard]] const DirectX::XMFLOAT4& Color() const noexcept { return m_color; }
        // 通常画像のパスを返します。
        [[nodiscard]] const std::filesystem::path& TexturePath() const noexcept
        {
            return m_texturePath;
        }
        // 設定した2D描画の並び順を返します。
        [[nodiscard]] int SortOrder() const noexcept { return m_sortOrder; }
        // 独自シェーダーのパスを返します。
        [[nodiscard]] const std::filesystem::path&
            ShaderPath() const noexcept
        {
            return m_shaderPath;
        }
        // 指定の描画定数を返し範囲外なら0番を返します(index: 定数番号)。
        [[nodiscard]] const DirectX::XMFLOAT4&
            CustomParameter(const std::size_t index) const noexcept
        {
            return m_customParameters[
                index < m_customParameters.size() ? index : 0];
        }
        // 設定した描画定数の全体を返します。
        [[nodiscard]] const CustomParameters&
            CustomParameterValues() const noexcept
        {
            return m_customParameters;
        }
        // 直近の独自描画パスで報告されたシェーダーエラーを返します。
        [[nodiscard]] const std::string&
            ShaderError() const noexcept
        {
            return m_shaderError;
        }
        // 直近の独自描画パスのシェーダー世代を返します。
        [[nodiscard]] std::uint64_t
            ShaderGeneration() const noexcept
        {
            return m_shaderGeneration;
        }
        // 予約定数を設定して独自描画パスを開始します(graphics: 描画装置)。
        [[nodiscard]] SpriteRenderPass BeginRenderPass(
            GraphicsDevice& graphics);
        // 画像とサイズ・並び順の描画説明を設定します(description: 出力する説明)。
        [[nodiscard]] bool DescribeDrawEvent(
            FrameDebugDrawDescription& description) const override;
        // 保存用のコンポーネント型名を返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override { return "SpriteRenderer"; }
        // 2D描画の並び順を返します。
        [[nodiscard]] int RenderSortOrder() const noexcept override
        {
            return m_sortOrder;
        }

    protected:
        // 描画装置と資産を借用して通常画像を読み込みます(graphics: 画像を管理する描画装置)。
        void OnInitialize(GraphicsDevice& graphics) override;
        // 位置・回転・画像領域とUI矩形を反映して描画します(sprites: スプライト描画の実行先)。
        void OnRender2D(
            const SpriteDrawContext& sprites) override;

    private:
        // 設定した2D表示幅・高さ
        DirectX::XMFLOAT2 m_size;
        // アルファ乗算前の描画RGBA
        DirectX::XMFLOAT4 m_color;
        // 既存シーンの配置を保つため、既定の基準点は左上を維持します。
        // ワールドSpriteのXY基準点
        DirectX::XMFLOAT2 m_pivot{ 0.0f, 0.0f };
        // 正規化X・Y・幅・高さ
        DirectX::XMFLOAT4 m_sourceRect{
            0.0f, 0.0f, 1.0f, 1.0f };
        // 通常画像のパス
        std::filesystem::path m_texturePath;
        // 独自シェーダーのパス
        std::filesystem::path m_shaderPath;
        // 設定したスプライト描画定数
        CustomParameters m_customParameters{};
        // 2D描画の並び順
        int m_sortOrder{};
        // マスク内外の表示指定
        SpriteMaskInteraction m_maskInteraction{
            SpriteMaskInteraction::None };
        // カメラ出力画像の参照名
        std::string m_renderTexture;
        // 共有する通常画像
        std::shared_ptr<const TextureAsset> m_texture;
        // 借用する資産管理
        AssetManager* m_assets{};
        // 借用する描画装置
        GraphicsDevice* m_graphics{};
        // 直近のシェーダー世代
        std::uint64_t m_shaderGeneration{};
        // 直近のシェーダーエラー
        std::string m_shaderError;
    };
}
