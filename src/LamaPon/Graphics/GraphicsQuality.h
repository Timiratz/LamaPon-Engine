#pragma once

#include <cstdint>
#include <string_view>

namespace LamaPon
{
    enum class GraphicsQualityPreset
    {
        Low,
        Medium,
        High,
        Ultra,
        Custom
    };

    // 品質プリセットと独立に選ぶライト計算の方式。
    enum class RenderingPath
    {
        // 品質設定の上限内で点・スポットを直接照明する。
        Forward,
        // クラスタへ届くライトを前計算し、点・スポットの合計256灯を扱う。
        ForwardPlus
    };

    enum class RenderingApi
    {
        Auto,
        DirectX11,
        DirectX12Experimental
    };

    // 既定構築の点ライト上限は12、Highプリセットは8なので、プリセットの適用には生成関数を使う。
    // 公開項目の追加は末尾に置き、既存の位置指定初期化との互換性を保つ。
    struct GraphicsSettings final
    {
        // 各ポスト効果の利用には品質側の許可とScene側の有効設定の両方が必要になる。
        // 品質プリセットの識別子
        GraphicsQualityPreset preset{
            GraphicsQualityPreset::High };
        // 描画解像度の倍率
        float renderScale{ 1.0f };
        // 影の描画の有効有無
        bool shadowsEnabled{ true };
        // 方向影の解像度
        std::uint32_t shadowResolution{ 2048 };
        // 影カスケード数の上限
        std::uint32_t shadowCascadeLimit{ 3 };
        // ブルームの利用許可
        bool bloomEnabled{ true };
        // 平滑化の利用許可
        bool antiAliasingEnabled{ true };
        // 霧の利用許可
        bool fogEnabled{ true };
        // 垂直同期の有効有無
        bool vSyncEnabled{ true };
        // 直接照明する点ライト数上限
        std::uint32_t pointLightLimit{ 12 };
        // 直接照明するスポット数上限
        std::uint32_t spotLightLimit{ 4 };
        // 画面空間遮蔽の利用許可
        bool ambientOcclusionEnabled{};
        // 遮蔽を求めるサンプル数
        std::uint32_t ambientOcclusionSampleCount{ 16 };
        // フレームレート上限、0は無制限
        std::uint32_t targetFrameRate{};
        // 読込画像のBC圧縮の有効有無
        bool runtimeTextureCompression{};
        // レンズフレアの利用許可
        bool screenSpaceLensFlareEnabled{};
        // 被写界深度の利用許可
        bool depthOfFieldEnabled{};
        // 被写界深度のサンプル数
        std::uint32_t depthOfFieldSampleCount{ 22 };
        // カメラブラーの利用許可
        bool motionBlurEnabled{};
        // カメラブラーのサンプル数
        std::uint32_t motionBlurSampleCount{ 8 };
        // 自動露出の利用許可
        bool autoExposureEnabled{};
        // ライト計算の方式
        RenderingPath renderingPath{
            RenderingPath::ForwardPlus };
        // 高いLODを維持する優先度倍率
        float automaticLodQuality{ 1.0f };
        // 起動時に使う描画API
        // APIの切替はプロセス再起動後に反映する。
        RenderingApi renderingApi{ RenderingApi::DirectX11 };
    };

    // 指定プリセットの設定を作り、CustomはHigh相当、不明な値はHighとする(preset: 品質プリセット)。
    [[nodiscard]] GraphicsSettings
        GraphicsSettingsForPreset(
            GraphicsQualityPreset preset) noexcept;
    // 数値の範囲と不明な描画方式を補正して設定を返す(settings: 補正する描画設定)。
    // プリセット識別子は保持し、浮動小数点のNaNは補正しない。
    [[nodiscard]] GraphicsSettings
        ClampGraphicsSettings(
            GraphicsSettings settings) noexcept;
    // プリセットの保存名を返し、不明な値はHighとする(preset: 品質プリセット)。
    [[nodiscard]] std::string_view
        GraphicsQualityPresetName(
            GraphicsQualityPreset preset) noexcept;
    // 大文字小文字を区別してプリセット名を読み、不明ならinvalid_argumentを送出する(name: プリセットの保存名)。
    [[nodiscard]] GraphicsQualityPreset
        GraphicsQualityPresetFromName(
            std::string_view name);
    // 描画APIの保存名を返し、不明な値はDirectX11とする(api: 描画APIの種別)。
    [[nodiscard]] std::string_view
        RenderingApiName(
            RenderingApi api) noexcept;
    // 大文字小文字を区別して描画API名を読み、不明ならDirectX11とする(name: 描画APIの保存名)。
    [[nodiscard]] RenderingApi
        RenderingApiFromName(
            std::string_view name) noexcept;
    // 描画方式の保存名を返し、不明な値はForwardPlusとする(path: ライト計算の方式)。
    [[nodiscard]] std::string_view
        RenderingPathName(
            RenderingPath path) noexcept;
    // 大文字小文字を区別して描画方式名を読み、不明ならForwardPlusとする(name: 描画方式の保存名)。
    [[nodiscard]] RenderingPath
        RenderingPathFromName(
            std::string_view name);
}
