#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/SpriteRendering.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// 2Dメッシュ描画を矩形描画・描画順・クリップ・Sprite Rendererの格子で検証します。
namespace
{
    // 描画先の横幅
    constexpr std::uint32_t CanvasWidth = 128u;
    // 描画先の高さ
    constexpr std::uint32_t CanvasHeight = 64u;
    // フレームを塗りつぶす背景色
    constexpr float ClearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };

    // 条件が偽なら失敗理由を送出します(condition: 判定, message: 失敗理由)。
    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class HiddenWindow final
    {
    public:
        // 描画用の不可視ウィンドウを作ります(width: 幅, height: 高さ)。
        HiddenWindow(
            const std::uint32_t width,
            const std::uint32_t height)
            : m_instance(GetModuleHandleW(nullptr))
        {
            // 登録するウィンドウクラス
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = DefWindowProcW;
            windowClass.hInstance = m_instance;
            windowClass.lpszClassName = ClassName;
            m_class = RegisterClassExW(&windowClass);
            if (m_class == 0)
            {
                throw std::runtime_error(
                    "The sprite mesh test window class could not be "
                    "registered");
            }
            m_window = CreateWindowExW(
                0,
                ClassName,
                ClassName,
                WS_OVERLAPPEDWINDOW,
                0,
                0,
                static_cast<int>(width),
                static_cast<int>(height),
                nullptr,
                nullptr,
                m_instance,
                nullptr);
            if (m_window == nullptr)
            {
                UnregisterClassW(ClassName, m_instance);
                m_class = 0;
                throw std::runtime_error(
                    "The sprite mesh test window could not be created");
            }
        }

        // 所有するウィンドウとクラス登録を解放します。
        ~HiddenWindow()
        {
            if (m_window != nullptr)
            {
                DestroyWindow(m_window);
            }
            if (m_class != 0)
            {
                UnregisterClassW(ClassName, m_instance);
            }
        }

        // ウィンドウの複製を禁止します。
        HiddenWindow(const HiddenWindow&) = delete;
        // ウィンドウの複製代入を禁止します。
        HiddenWindow& operator=(const HiddenWindow&) = delete;

        // 所有するウィンドウを返します。
        [[nodiscard]] HWND Get() const noexcept
        {
            return m_window;
        }

    private:
        // 登録するクラス名
        static constexpr const wchar_t* ClassName =
            L"LamaPonSpriteMeshRenderingTests";
        // 登録元のモジュール
        HINSTANCE m_instance{};
        // 登録済みのクラス
        ATOM m_class{};
        // 所有するウィンドウ
        HWND m_window{};
    };

    struct Capture final
    {
        // 画像の横幅
        std::uint32_t width{};
        // 画像の高さ
        std::uint32_t height{};
        // RGBA画素列
        std::vector<std::uint8_t> pixels;
    };

    // 指定画素のRGBAを返します(capture: 画像, x: 横位置, y: 縦位置)。
    [[nodiscard]] std::array<int, 4> Pixel(
        const Capture& capture,
        const std::uint32_t x,
        const std::uint32_t y)
    {
        // 画素の先頭バイト位置
        const std::size_t offset =
            (static_cast<std::size_t>(y) * capture.width + x) * 4u;
        return {
            capture.pixels[offset],
            capture.pixels[offset + 1u],
            capture.pixels[offset + 2u],
            capture.pixels[offset + 3u] };
    }

    // 指定画素がRGBの目安に近いか確認します(capture: 画像, x: 横位置, y: 縦位置, red: 赤, green: 緑, blue: 青, message: 失敗理由)。
    void RequireColor(
        const Capture& capture,
        const std::uint32_t x,
        const std::uint32_t y,
        const int red,
        const int green,
        const int blue,
        const std::string& message)
    {
        // 確認する画素
        const auto pixel = Pixel(capture, x, y);
        Require(
            std::abs(pixel[0] - red) <= 8
                && std::abs(pixel[1] - green) <= 8
                && std::abs(pixel[2] - blue) <= 8,
            message + " (got " + std::to_string(pixel[0]) + ","
                + std::to_string(pixel[1]) + ","
                + std::to_string(pixel[2]) + ")");
    }

    // 2枚の画像が許容差内で一致するか確認します(left: 比較元, right: 比較先, message: 失敗理由, tolerance: 成分ごとの許容差)。
    void RequireMatching(
        const Capture& left,
        const Capture& right,
        const std::string& message,
        const int tolerance = 2)
    {
        Require(
            left.width == right.width
                && left.height == right.height
                && left.pixels.size() == right.pixels.size()
                && !left.pixels.empty(),
            message + ": capture sizes differ");
        // 許容差を超えた画素数
        std::size_t mismatched{};
        // 最初に差が出た画素番号
        std::size_t first = std::numeric_limits<std::size_t>::max();
        // 比較する画素番号
        for (std::size_t pixel = 0; pixel < left.pixels.size() / 4u; ++pixel)
        {
            // 画素内の成分番号
            for (std::size_t channel = 0; channel < 4u; ++channel)
            {
                // 成分のバイト位置
                const std::size_t offset = pixel * 4u + channel;
                if (std::abs(
                        static_cast<int>(left.pixels[offset])
                        - static_cast<int>(right.pixels[offset]))
                    > tolerance)
                {
                    ++mismatched;
                    first = std::min(first, pixel);
                    break;
                }
            }
        }
        if (mismatched == 0)
        {
            return;
        }
        // 最初に差が出た画素の左側
        const auto leftPixel = Pixel(
            left,
            static_cast<std::uint32_t>(first % left.width),
            static_cast<std::uint32_t>(first / left.width));
        // 最初に差が出た画素の右側
        const auto rightPixel = Pixel(
            right,
            static_cast<std::uint32_t>(first % left.width),
            static_cast<std::uint32_t>(first / left.width));
        throw std::runtime_error(
            message + ": " + std::to_string(mismatched)
                + " pixels differ, first at ("
                + std::to_string(first % left.width) + ","
                + std::to_string(first / left.width) + ") "
                + std::to_string(leftPixel[0]) + ","
                + std::to_string(leftPixel[1]) + ","
                + std::to_string(leftPixel[2]) + " vs "
                + std::to_string(rightPixel[0]) + ","
                + std::to_string(rightPixel[1]) + ","
                + std::to_string(rightPixel[2]));
    }

    // RGBへアルファを掛けた色を返します(color: 元のRGBA)。
    [[nodiscard]] DirectX::XMFLOAT4 Premultiplied(
        const DirectX::XMFLOAT4& color) noexcept
    {
        return {
            color.x * color.w,
            color.y * color.w,
            color.z * color.w,
            color.w };
    }

    struct QuadMesh final
    {
        // 左上・右上・左下・右下の頂点
        std::array<LamaPon::SpriteMeshVertex, 4> vertices{};
        // 2枚の三角形の頂点番号
        std::array<std::uint16_t, 6> indices{ 0, 1, 2, 2, 1, 3 };

        // 頂点と索引を参照する描画要求を返します(texture: 描画画像)。
        [[nodiscard]] LamaPon::SpriteMeshDrawRequest Request(
            const LamaPon::GraphicsViewHandle& texture = {}) const
        {
            // メッシュの描画要求
            LamaPon::SpriteMeshDrawRequest request;
            request.texture = texture;
            request.vertices = vertices;
            request.indices = indices;
            return request;
        }
    };

    // 矩形の2Dメッシュを作ります(x: 左端, y: 上端, width: 幅, height: 高さ, color: RGBA色)。
    [[nodiscard]] QuadMesh MakeQuad(
        const float x,
        const float y,
        const float width,
        const float height,
        const DirectX::XMFLOAT4& color)
    {
        // 作成する矩形
        QuadMesh quad;
        // 事前乗算した頂点色
        const auto tint = Premultiplied(color);
        quad.vertices = { {
            { { x, y }, { 0.0f, 0.0f }, tint },
            { { x + width, y }, { 1.0f, 0.0f }, tint },
            { { x, y + height }, { 0.0f, 1.0f }, tint },
            { { x + width, y + height }, { 1.0f, 1.0f }, tint }
        } };
        return quad;
    }

    // 白画像の矩形を積みます(pass: 送信先, x: 左端, y: 上端, width: 幅, height: 高さ, color: RGBA色)。
    void DrawRectangle(
        const LamaPon::SpriteRenderPass& pass,
        const float x,
        const float y,
        const float width,
        const float height,
        const DirectX::XMFLOAT4& color)
    {
        // 矩形の描画要求
        LamaPon::SpriteDrawRequest request;
        request.position = { x, y };
        request.scale = { width, height };
        request.tint = Premultiplied(color);
        Require(pass.Draw(request), "A sprite rectangle was rejected");
    }

    // 1フレームを描いて撮影します(graphics: 描画装置, draw: フレーム内の描画処理)。
    [[nodiscard]] Capture CaptureFrame(
        LamaPon::GraphicsDevice& graphics,
        const std::function<void()>& draw)
    {
        // 撮影した画像
        Capture capture;
        graphics.BeginFrame(ClearColor);
        draw();
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();
        return capture;
    }

    // 円画像のビューを返します(graphics: 描画装置, width: 画像幅の出力, height: 画像高の出力)。
    [[nodiscard]] LamaPon::GraphicsViewHandle CircleTexture(
        LamaPon::GraphicsDevice& graphics,
        float& width,
        float& height)
    {
        // 内蔵の円画像
        const auto circle = graphics.Assets().LoadTexture("builtin/circle");
        // 円画像のGPU資源
        const auto resources = circle != nullptr
            ? circle->resources.Acquire()
            : nullptr;
        Require(
            resources != nullptr && resources->shaderResourceView,
            "The built-in circle texture has no shader resource view");
        width = static_cast<float>(circle->width);
        height = static_cast<float>(circle->height);
        return resources->shaderResourceView;
    }

    // 2Dライト付きの不透明パスの設定を返します。
    [[nodiscard]] LamaPon::SpritePassDescription LitPass()
    {
        // 自作シェーダーと1灯のライトを使うパス設定
        LamaPon::SpritePassDescription description;
        description.pixelShader = "shaders/LamaPonSpriteLit.hlsl";
        description.blend = LamaPon::SpriteBlendMode::Opaque;
        description.lighting.counts.x = 1u;
        description.lighting.lights[0].positionRadiusIntensity = {
            28.0f, 48.0f, 24.0f, 1.0f };
        description.lighting.lights[0].color = {
            0.8f, 0.3f, 0.05f, 0.0f };
        return description;
    }

    struct ApiCaptures final
    {
        // 矩形で描いた基準画像
        Capture quads;
        // 同じ形をメッシュで描いた画像
        Capture meshes;
        // 描画順・両面・クリップの画像
        Capture ordering;
        // Sprite Rendererを矩形で描いた画像
        Capture spriteQuad;
        // Sprite Rendererを格子で描いた画像
        Capture spriteGrid;
        // Sprite Rendererの格子を変形した画像
        Capture spriteDeformed;
    };

    // 不正なメッシュが拒否されることを確認します(pass: 送信先)。
    void RequireRejectedMeshes(const LamaPon::SpriteRenderPass& pass)
    {
        // 正常な基準の矩形
        auto quad = MakeQuad(0.0f, 0.0f, 4.0f, 4.0f, { 1, 1, 1, 1 });
        // 範囲外の頂点番号
        const std::array<std::uint16_t, 3> outOfRange{ 0, 1, 4 };
        // 3の倍数でない索引列
        const std::array<std::uint16_t, 4> partial{ 0, 1, 2, 3 };
        // 検証する描画要求
        auto request = quad.Request();
        request.indices = outOfRange;
        Require(!pass.DrawMesh(request), "An out-of-range index was accepted");
        request.indices = partial;
        Require(!pass.DrawMesh(request), "A partial triangle was accepted");
        request.indices = {};
        Require(!pass.DrawMesh(request), "An empty mesh was accepted");
        quad.vertices[0].position.x =
            std::numeric_limits<float>::quiet_NaN();
        Require(
            !pass.DrawMesh(quad.Request()),
            "A non-finite vertex was accepted");
    }

    // 指定APIで全画像を撮影します(api: 描画API)。
    [[nodiscard]] ApiCaptures RenderCaptures(const LamaPon::RenderingApi api)
    {
        // 描画用の不可視ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 撮影する描画装置
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The requested rendering API did not start on WARP");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);

        // 円画像の幅
        float circleWidth{};
        // 円画像の高さ
        float circleHeight{};
        // 円画像のビュー
        const auto circle =
            CircleTexture(graphics, circleWidth, circleHeight);
        // 撮影結果
        ApiCaptures captures;

        captures.quads = CaptureFrame(graphics, [&]()
        {
            {
                // 標準のパス
                auto pass = graphics.BeginSpritePass();
                DrawRectangle(pass, 8.0f, 8.0f, 40.0f, 24.0f,
                    { 0.9f, 0.2f, 0.1f, 1.0f });
                // 半透明の円画像
                LamaPon::SpriteDrawRequest request;
                request.texture = circle;
                request.position = { 64.0f, 4.0f };
                request.scale = {
                    48.0f / circleWidth,
                    48.0f / circleHeight };
                request.tint = Premultiplied({ 0.2f, 0.6f, 1.0f, 0.75f });
                Require(pass.Draw(request), "The circle sprite was rejected");
                pass.End();
            }
            {
                // 自作シェーダーのパス
                auto pass = graphics.BeginSpritePass(LitPass());
                DrawRectangle(pass, 8.0f, 40.0f, 40.0f, 16.0f,
                    { 0.3f, 0.3f, 0.3f, 1.0f });
                pass.End();
            }
        });

        captures.meshes = CaptureFrame(graphics, [&]()
        {
            {
                // 標準のパス
                auto pass = graphics.BeginSpritePass();
                // 赤い矩形のメッシュ
                const auto red = MakeQuad(8.0f, 8.0f, 40.0f, 24.0f,
                    { 0.9f, 0.2f, 0.1f, 1.0f });
                Require(pass.DrawMesh(red.Request()), "The red mesh was rejected");
                // 半透明の円画像のメッシュ
                const auto disc = MakeQuad(64.0f, 4.0f, 48.0f, 48.0f,
                    { 0.2f, 0.6f, 1.0f, 0.75f });
                Require(
                    pass.DrawMesh(disc.Request(circle)),
                    "The circle mesh was rejected");
                pass.End();
            }
            {
                // 自作シェーダーのパス
                auto pass = graphics.BeginSpritePass(LitPass());
                // 照らされる灰色のメッシュ
                const auto grey = MakeQuad(8.0f, 40.0f, 40.0f, 16.0f,
                    { 0.3f, 0.3f, 0.3f, 1.0f });
                Require(pass.DrawMesh(grey.Request()), "The lit mesh was rejected");
                pass.End();
            }
        });

        captures.ordering = CaptureFrame(graphics, [&]()
        {
            // 標準のパス
            auto pass = graphics.BeginSpritePass();
            DrawRectangle(pass, 10.0f, 10.0f, 40.0f, 40.0f, { 0, 0, 1, 1 });
            // 間に描く赤いメッシュ
            const auto red = MakeQuad(20.0f, 20.0f, 40.0f, 40.0f, { 1, 0, 0, 1 });
            Require(pass.DrawMesh(red.Request()), "The ordered mesh was rejected");
            DrawRectangle(pass, 30.0f, 30.0f, 40.0f, 40.0f, { 0, 1, 0, 1 });

            // 頂点の並びを逆にした黄色いメッシュ
            auto mirrored = MakeQuad(90.0f, 6.0f, 20.0f, 20.0f, { 1, 1, 0, 1 });
            mirrored.indices = { 0, 2, 1, 1, 2, 3 };
            Require(
                pass.DrawMesh(mirrored.Request()),
                "The mirrored mesh was rejected");

            Require(
                pass.PushScissor({ 90.0f, 40.0f, 110.0f, 60.0f }),
                "The scissor was rejected");
            // クリップされる白いメッシュ
            const auto clipped =
                MakeQuad(76.0f, 34.0f, 48.0f, 30.0f, { 1, 1, 1, 1 });
            Require(
                pass.DrawMesh(clipped.Request()),
                "The clipped mesh was rejected");
            Require(pass.PopScissor(), "The scissor was not popped");
            RequireRejectedMeshes(pass);
            pass.End();
        });

        // Sprite Rendererを描くシーン
        LamaPon::Scene scene(graphics);
        // 回転・拡縮した円のSprite
        auto& rotated = scene.CreateGameObject("Rotated");
        rotated.GetTransform().position = { 40.0f, 32.0f, 0.0f };
        rotated.GetTransform().scale = { 1.2f, 0.8f, 1.0f };
        rotated.GetTransform().SetEulerAngles(0.0f, 0.0f, 0.3f);
        // 円のSprite Renderer
        auto& rotatedSprite =
            rotated.AddComponent<LamaPon::SpriteRendererComponent>(
                DirectX::XMFLOAT2{ 48.0f, 32.0f },
                DirectX::XMFLOAT4{ 0.9f, 0.8f, 0.2f, 1.0f },
                "builtin/circle");
        rotatedSprite.SetPivot({ 0.5f, 0.5f });
        // 変形させる単色のSprite
        auto& stretched = scene.CreateGameObject("Stretched");
        stretched.GetTransform().position = { 80.0f, 8.0f, 0.0f };
        // 単色のSprite Renderer
        auto& stretchedSprite =
            stretched.AddComponent<LamaPon::SpriteRendererComponent>(
                DirectX::XMFLOAT2{ 16.0f, 16.0f },
                DirectX::XMFLOAT4{ 0.1f, 0.9f, 0.3f, 1.0f });

        captures.spriteQuad = CaptureFrame(graphics, [&]() { scene.Render2D(); });
        Require(!rotatedSprite.UsesMesh(), "A 1x1 sprite used the mesh path");
        rotatedSprite.SetMeshGrid(4, 3);
        stretchedSprite.SetMeshGrid(2, 2);
        Require(
            rotatedSprite.UsesMesh()
                && rotatedSprite.MeshVertexCount() == 20u,
            "The sprite grid did not switch to the mesh path");
        captures.spriteGrid = CaptureFrame(graphics, [&]() { scene.Render2D(); });

        // 右の列だけ右へ伸ばした格子
        auto deformed = stretchedSprite.MeshRestPositions();
        // 変形する頂点番号
        for (std::size_t index = 2; index < deformed.size(); index += 3)
        {
            deformed[index].x += 24.0f;
        }
        Require(
            stretchedSprite.SetMeshDeformation(deformed),
            "A matching deformation was rejected");
        Require(
            !stretchedSprite.SetMeshDeformation({ { 0.0f, 0.0f } })
                && stretchedSprite.MeshDeformation().empty(),
            "A mismatched deformation was accepted");
        Require(
            stretchedSprite.SetMeshDeformation(deformed),
            "A matching deformation was rejected again");
        captures.spriteDeformed =
            CaptureFrame(graphics, [&]() { scene.Render2D(); });

        // 保存・読み込みで格子が残り、変形は実行時の状態として保存しません。
        // JSONから復元したシーン
        LamaPon::Scene loaded(graphics);
        loaded.LoadFromJson(scene.SerializeToJson());
        // 復元した円のSprite
        const auto* loadedRotated = loaded.FindGameObjectByName("Rotated");
        // 復元した円のSprite Renderer
        const auto* loadedSprite =
            loadedRotated != nullptr
                ? loadedRotated->GetComponent<
                    LamaPon::SpriteRendererComponent>()
                : nullptr;
        Require(
            loadedSprite != nullptr
                && loadedSprite->MeshColumns() == 4
                && loadedSprite->MeshRows() == 3
                && loadedSprite->MeshDeformation().empty(),
            "The sprite mesh grid did not round-trip through JSON");

        // 複製したSprite
        auto& duplicate = scene.DuplicateGameObject(rotated);
        // 複製したSprite Renderer
        const auto* duplicateSprite =
            duplicate.GetComponent<LamaPon::SpriteRendererComponent>();
        Require(
            duplicateSprite != nullptr
                && duplicateSprite->MeshColumns() == 4
                && duplicateSprite->MeshRows() == 3
                && duplicateSprite->Pivot().x == 0.5f
                && duplicateSprite->Pivot().y == 0.5f,
            "Duplicating a sprite lost its mesh grid or pivot");
        return captures;
    }

    // 1つのAPIの撮影結果を検証します(captures: 撮影結果, name: API名)。
    void RequireApiCaptures(
        const ApiCaptures& captures,
        const std::string& name)
    {
        RequireColor(captures.quads, 20, 20, 230, 51, 26,
            name + ": the reference red rectangle is missing");
        RequireMatching(
            captures.quads,
            captures.meshes,
            name + ": meshes did not match the sprite rectangles");

        RequireColor(captures.ordering, 15, 15, 0, 0, 255,
            name + ": the first rectangle was not drawn");
        RequireColor(captures.ordering, 25, 25, 255, 0, 0,
            name + ": the mesh was not drawn over the earlier rectangle");
        RequireColor(captures.ordering, 55, 25, 255, 0, 0,
            name + ": the mesh was not drawn");
        RequireColor(captures.ordering, 40, 40, 0, 255, 0,
            name + ": the later rectangle was not drawn over the mesh");
        RequireColor(captures.ordering, 100, 16, 255, 255, 0,
            name + ": a reversed mesh was culled");
        RequireColor(captures.ordering, 100, 50, 255, 255, 255,
            name + ": the clipped mesh was not drawn inside the scissor");
        RequireColor(captures.ordering, 84, 50, 0, 0, 0,
            name + ": the mesh was drawn outside the scissor");

        // 格子の内側の辺ではUV補間の丸めが矩形とわずかに異なるため、半透明の縁だけ差を広く許します。
        RequireMatching(
            captures.spriteQuad,
            captures.spriteGrid,
            name + ": a sprite grid did not match its rectangle",
            4);
        RequireColor(captures.spriteDeformed, 86, 14, 26, 230, 77,
            name + ": the deformed sprite lost its original area");
        RequireColor(captures.spriteDeformed, 110, 14, 26, 230, 77,
            name + ": the deformed sprite was not stretched");
        RequireColor(captures.spriteGrid, 110, 14, 0, 0, 0,
            name + ": the undeformed sprite covered the stretched area");
    }
}

// D3D11とD3D12の2Dメッシュ描画を検証します。
int main()
{
    // COM初期化結果
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // テストの終了コード
    int result = EXIT_SUCCESS;
    try
    {
        LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);
        // D3D11の撮影結果
        const auto d3d11 =
            RenderCaptures(LamaPon::RenderingApi::DirectX11);
        RequireApiCaptures(d3d11, "DirectX 11");
        // D3D12の撮影結果
        const auto d3d12 =
            RenderCaptures(LamaPon::RenderingApi::DirectX12Experimental);
        RequireApiCaptures(d3d12, "DirectX 12");
        RequireMatching(
            d3d11.meshes,
            d3d12.meshes,
            "DirectX 12 meshes differed from DirectX 11");
        RequireMatching(
            d3d11.ordering,
            d3d12.ordering,
            "DirectX 12 mesh ordering differed from DirectX 11");
        RequireMatching(
            d3d11.spriteDeformed,
            d3d12.spriteDeformed,
            "DirectX 12 deformed sprites differed from DirectX 11");
        std::cout << "Sprite mesh rendering checks passed.\n";
    }
    // exception: 検証中に送出された例外
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << '\n';
        result = EXIT_FAILURE;
    }
    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }
    return result;
}
