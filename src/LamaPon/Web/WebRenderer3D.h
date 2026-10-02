#pragma once

#include "LamaPon/Web/WebMath.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace LamaPon::Web
{
    struct Color final
    {
        // 赤成分
        float r{};
        // 緑成分
        float g{};
        // 青成分
        float b{};
        // Alpha成分
        float a{ 1.0f };
    };

    struct Vertex3D final
    {
        // ローカル空間の頂点位置
        Vec3 position{};
        // ローカル空間の法線
        Vec3 normal{ 0.0f, 1.0f, 0.0f };
        // テクスチャ座標
        Vec2 uv{};
    };

    struct Camera3D final
    {
        // 視点のWorld位置
        Vec3 position{ 0.0f, 3.0f, 8.0f };
        // 注視点のWorld位置
        Vec3 target{};
        // カメラの上方向
        Vec3 up{ 0.0f, 1.0f, 0.0f };
        // 縦画角のラジアン
        float verticalFieldOfViewRadians{ 0.8f };
        // 近クリップ距離
        float nearPlane{ 0.05f };
        // 遠クリップ距離
        float farPlane{ 500.0f };
    };

    struct Fog3D final
    {
        // 距離フォグを使うか
        bool enabled{};
        // フォグの色
        Color color{ 0.74f, 0.60f, 0.52f, 1.0f };
        // フォグの開始距離
        float startDistance{ 110.0f };
        // フォグの終了距離
        float endDistance{ 520.0f };
    };

    struct Lighting3D final
    {
        // 環境光の色
        Color ambientColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        // 環境光の強度
        float ambientIntensity{ 0.42f };
        // 光が進むWorld方向
        Vec3 directionalDirection{ -0.25f, -0.75f, -0.90f };
        // 平行光の色
        Color directionalColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        // 平行光の強度
        float directionalIntensity{ 0.80f };
    };

    struct Sky3D final
    {
        // 空の階調を背景に使うか
        bool enabled{};
        // 空の上端の色
        Color topColor{ 0.722f, 0.620f, 0.572f, 1.0f };
        // 空の水平線の色
        Color horizonColor{ 0.879f, 0.780f, 0.561f, 1.0f };
    };

    using MeshId = std::uint32_t;
    using TextureId = std::uint32_t;

    // WebGLまたはCanvas2D/SVGで描画し、APIへGPU固有型を公開しない。
    class Renderer3D final
    {
    public:
        // 描画状態を用意し、WebGL初期化はInitializeで行う。
        Renderer3D();
        // 所有するMeshバッファ・Program・WebGL Contextを破棄する。
        ~Renderer3D();

        // 複製を禁止する。
        Renderer3D(const Renderer3D&) = delete;
        // 複製代入を禁止する。
        Renderer3D& operator=(const Renderer3D&) = delete;

        // WebGL2・WebGL1・Canvas2D/SVGの順に初期化する(canvasSelector: CanvasのCSS指定, width: 要求する描画幅, height: 要求する描画高さ)。
        [[nodiscard]] bool Initialize(
            const char* canvasSelector = "#canvas",
            std::uint32_t width = 1280,
            std::uint32_t height = 720) noexcept;
        // 描画サイズを更新し、ソフトウェア描画は幅960以内に縮小する(width: 要求する幅, height: 要求する高さ)。
        void Resize(std::uint32_t width, std::uint32_t height) noexcept;
        // 描画先を背景色または空の階調で消去する(clearColor: 背景RGBA)。
        void BeginFrame(Color clearColor) noexcept;
        // ソフトウェアの保留描画を実行する。
        void EndFrame() noexcept;
        // Resize後はSetCameraを呼び、画面比率を行列へ反映する。
        // 現在の描画サイズでビューと透視行列を更新する(camera: 視点とクリップ設定)。
        void SetCamera(const Camera3D& camera) noexcept;
        // 開始を0以上・終了を開始より先へ補正して設定する(fog: 距離フォグ設定)。
        void SetFog(const Fog3D& fog) noexcept;
        // 強度を0以上・光の方向を単位ベクトルへ補正する(lighting: 環境光と平行光)。
        void SetLighting(const Lighting3D& lighting) noexcept;
        // 背景に使う空の階調を設定する(sky: 空の表示設定)。
        void SetSky(const Sky3D& sky) noexcept;

        // 頂点とIndexを複製して登録し、未初期化・空データなら0を返す(vertices: 頂点配列, indices: 三角形の頂点番号列)。
        [[nodiscard]] MeshId CreateMesh(
            const std::vector<Vertex3D>& vertices,
            const std::vector<std::uint32_t>& indices) noexcept;
        // IDは読込完了を保証せず、画像の管理表はブラウザー全体で共有する。
        // 画像の非同期読込を予約し、開始不可なら0を返す(virtualPath: 仮想FS内の画像パス)。
        [[nodiscard]] TextureId CreateTexture(
            const char* virtualPath) noexcept;
        // 既存Meshの全データを置き換える(mesh: 登録済みID, vertices: 頂点配列, indices: 三角形の頂点番号列)。
        void UpdateMesh(
            MeshId mesh,
            const std::vector<Vertex3D>& vertices,
            const std::vector<std::uint32_t>& indices) noexcept;
        // 登録済みMeshとGPUバッファを破棄する(mesh: 対象ID)。
        void DestroyMesh(MeshId mesh) noexcept;
        // WebGLの透過Meshは呼出側で奥から順に渡す。
        // 登録済みMeshを描画する(mesh: 対象ID, model: 列優先のWorld変換, color: 表面のRGBA, roughness: 粗さ0～1, texture: 表面色テクスチャID, doubleSided: 両面描画するか, alphaBlended: 透過合成するか, alphaCutoff: 負値で無効のAlphaしきい値, normalTexture: 法線テクスチャID, normalStrength: 法線の強さ, metallic: 金属度0～1, metallicRoughnessTexture: G粗さ・B金属度のID, roughnessTexture: G粗さテクスチャID, metallicTexture: B金属度テクスチャID, occlusionTexture: R遮蔽テクスチャID, occlusionStrength: 遮蔽の強さ0～1, emissiveTexture: 発光テクスチャID, emissiveColor: 発光RGB, unlit: 照明を無効にするか, dielectricSpecular: 非金属の反射RGB, additiveBlend: 透過時に加算合成するか)。
        void DrawMesh(
            MeshId mesh,
            const Mat4& model,
            Color color,
            float roughness = 0.65f,
            TextureId texture = 0,
            bool doubleSided = false,
            bool alphaBlended = false,
            float alphaCutoff = -1.0f,
            TextureId normalTexture = 0,
            float normalStrength = 1.0f,
            float metallic = 0.0f,
            TextureId metallicRoughnessTexture = 0,
            TextureId roughnessTexture = 0,
            TextureId metallicTexture = 0,
            TextureId occlusionTexture = 0,
            float occlusionStrength = 1.0f,
            TextureId emissiveTexture = 0,
            Color emissiveColor = {},
            bool unlit = false,
            Color dielectricSpecular = {
                0.04f, 0.04f, 0.04f, 1.0f },
            bool additiveBlend = false) noexcept;

        // 内部の描画幅を返す。
        [[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }
        // 内部の描画高さを返す。
        [[nodiscard]] std::uint32_t Height() const noexcept { return m_height; }
        // Canvas2D/SVGの互換描画を使っているか返す。
        [[nodiscard]] bool UsesCanvas2DFallback() const noexcept;

    private:
        struct Impl;
        // バックエンドと描画資源の所有者
        std::unique_ptr<Impl> m_impl;
        // 内部の描画幅
        std::uint32_t m_width{};
        // 内部の描画高さ
        std::uint32_t m_height{};
        // Worldからビューへの行列
        Mat4 m_view{ Mat4::Identity() };
        // ビューからクリップへの行列
        Mat4 m_projection{ Mat4::Identity() };
        // 視点のWorld位置
        Vec3 m_cameraPosition{};
        // 距離フォグ設定
        Fog3D m_fog{};
        // 環境光と平行光の設定
        Lighting3D m_lighting{};
        // 背景に使う空の階調
        Sky3D m_sky{};
        // 描画バックエンドの初期化済み
        bool m_initialized{};
    };
}
