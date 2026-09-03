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

    // ライティングの計算方式です。品質プリセットとは独立した選択で、絵の
    // 作り方そのものが変わるので、プリセットを切り替えても
    // ここは引き継がれます。
    enum class RenderingPath
    {
        // 1回の描画で使えるポイント／スポットが品質設定の上限
        // （High=12/4）までの経路。クラスタの前計算が要らないぶん
        // 軽く、ライトの少ないシーンや非力な環境向けです。
        Forward,
        // クラスタライトカリング。視錐台を16x9x24の格子へ切って
        // 「そのクラスタへ届くライトの番号表」を毎フレーム作るので、
        // シーン全体でポイント＋スポット256灯まで置けます。
        ForwardPlus
    };

    struct GraphicsSettings final
    {
        GraphicsQualityPreset preset{
            GraphicsQualityPreset::High };
        float renderScale{ 1.0f };
        bool shadowsEnabled{ true };
        std::uint32_t shadowResolution{ 2048 };
        std::uint32_t shadowCascadeLimit{ 3 };
        bool bloomEnabled{ true };
        bool antiAliasingEnabled{ true };
        bool fogEnabled{ true };
        bool vSyncEnabled{ true };
        std::uint32_t pointLightLimit{ 12 };
        std::uint32_t spotLightLimit{ 4 };
        // SSAO（遮蔽による陰り）を使うか。プリセットではHigh以上で
        // 有効になります。Sceneの環境設定側でも個別にオンにする
        // 必要があります（Bloomと同じ扱い）。
        // 位置指定の初期化を壊さないよう、必ず末尾に足してください。
        bool ambientOcclusionEnabled{};
        // SSAOが遮蔽を探す回数。多いほど滑らかですが重くなります。
        // 半解像度で計算し、後段のブラーで均すため、少なめでも
        // 実用になります（Low=8, Medium=12, High=16, Ultra=24）。
        std::uint32_t ambientOcclusionSampleCount{ 16 };
        // 0 means unlimited. VSync can still impose the display refresh rate.
        std::uint32_t targetFrameRate{};
        // PNG/JPG等の読み込み時にBC1/BC3へランタイム圧縮して
        // VRAM使用量を約1/4〜1/8にします（画質は少し低下）。
        bool runtimeTextureCompression{};
        // Screen Space Lens Flare。ポスト処理のサンプル数が多いため、
        // High以上で有効にします。Scene側でも個別にオンにする必要があります。
        bool screenSpaceLensFlareEnabled{};
        // 被写界深度（DoF）。半解像度でぼかすので負荷は中程度です。
        // High以上で有効にします。Scene側でも個別にオンにする必要が
        // あります（Bloomと同じ扱い）。
        bool depthOfFieldEnabled{};
        // 被写界深度のぼけを作るサンプル数。多いほど滑らかですが
        // 重くなります。半解像度で円形に散らすため少なめでも実用に
        // なります（Low=10, Medium=16, High=22, Ultra=32）。
        std::uint32_t depthOfFieldSampleCount{ 22 };
        // モーションブラー（カメラの動きによるブレ）。フル解像度で
        // 線に沿ってサンプルするので、High以上で有効にします。
        // Scene側でも個別にオンにする必要があります。
        bool motionBlurEnabled{};
        // ブレの線に沿って何回サンプルするか（Low=4, Medium=6,
        // High=8, Ultra=16）。少ないとブレが縞に分かれて見えます。
        std::uint32_t motionBlurSampleCount{ 8 };
        // 自動露出（明順応・暗順応）。1/4解像度1パス＋ミップ生成
        // だけなので軽いほうですが、扱いを他と揃えてHigh以上で
        // 有効にします。Scene側でも個別にオンにする必要があります。
        bool autoExposureEnabled{};
        // ライティングの計算方式。既定はForward+です。
        // 位置指定の初期化を壊さないよう、必ず末尾に足してください。
        RenderingPath renderingPath{
            RenderingPath::ForwardPlus };
        // 自動LODの見た目優先度。1.0が基準で、小さいほど早く
        // 低LODへ切り替わり、大きいほど高LODを長く保ちます。
        float automaticLodQuality{ 1.0f };
    };

    [[nodiscard]] GraphicsSettings
        GraphicsSettingsForPreset(
            GraphicsQualityPreset preset) noexcept;
    [[nodiscard]] GraphicsSettings
        ClampGraphicsSettings(
            GraphicsSettings settings) noexcept;
    [[nodiscard]] std::string_view
        GraphicsQualityPresetName(
            GraphicsQualityPreset preset) noexcept;
    [[nodiscard]] GraphicsQualityPreset
        GraphicsQualityPresetFromName(
            std::string_view name);
    [[nodiscard]] std::string_view
        RenderingPathName(
            RenderingPath path) noexcept;
    // 知らない名前はForward+として読みます（設定ファイルが将来の
    // 名前を持っていても、絵が出ない状態にはしないため）。
    [[nodiscard]] RenderingPath
        RenderingPathFromName(
            std::string_view name);
}
