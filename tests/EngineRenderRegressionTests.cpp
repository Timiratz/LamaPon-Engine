// エンジンの実描画パス（シェーダーコンパイル、ライティング、
// GPUインスタンシング、カスケード影、スカイ）をWARPデバイス＋
// 非表示ウィンドウで実行し、ピクセルの性質で検証します。
// ゴールデンイメージ比較ではなく性質検証なので、環境差や
// ラスタライズ差に対して頑健です。
#include "LamaPon/LamaPon.h"

// レンダーテクスチャの解像度を確認するため、RenderTargetの実体が必要です
// （LamaPon.hはGraphicsDevice経由の前方宣言しか持ちません）。
#include "LamaPon/Graphics/EnvironmentCache.h"
#include "LamaPon/Graphics/PngWriter.h"
#include "LamaPon/Graphics/RenderPipeline.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include <Windows.h>
#include <objbase.h>
// Compute Shaderの出力（R16G16B16A16_FLOAT）をCPUで読み戻すため。
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    constexpr std::uint32_t Width = 320;
    constexpr std::uint32_t Height = 180;

    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // 失敗時にどこまで進んだかCTestログで分かるようにします。
    void Stage(const char* name)
    {
        std::cout << "stage: " << name << std::endl;
    }

    // 非表示のWin32ウィンドウ（スワップチェーン用）。
    [[nodiscard]] HWND CreateHiddenWindow()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance =
            GetModuleHandleW(nullptr);
        windowClass.lpszClassName =
            L"LamaPonRenderTests";
        Require(
            RegisterClassExW(&windowClass) != 0,
            "RegisterClassExW failed.");

        const HWND window = CreateWindowExW(
            0,
            windowClass.lpszClassName,
            L"LamaPonRenderTests",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            static_cast<int>(Width),
            static_cast<int>(Height),
            nullptr,
            nullptr,
            windowClass.hInstance,
            nullptr);
        Require(
            window != nullptr,
            "CreateWindowExW failed.");
        return window;
    }

    struct Pixel final
    {
        std::uint8_t red{};
        std::uint8_t green{};
        std::uint8_t blue{};
    };

    [[nodiscard]] Pixel At(
        const std::vector<std::uint8_t>& pixels,
        const std::uint32_t x,
        const std::uint32_t y)
    {
        const std::size_t offset =
            (static_cast<std::size_t>(y) * Width + x)
            * 4;
        return {
            pixels[offset],
            pixels[offset + 1],
            pixels[offset + 2] };
    }

    [[nodiscard]] bool NearColor(
        const Pixel& pixel,
        const int red,
        const int green,
        const int blue,
        const int tolerance)
    {
        return std::abs(pixel.red - red) <= tolerance
            && std::abs(pixel.green - green)
                <= tolerance
            && std::abs(pixel.blue - blue) <= tolerance;
    }

    // 指定領域に背景色から大きく外れたピクセルがあるか。
    [[nodiscard]] bool RegionHasForeground(
        const std::vector<std::uint8_t>& pixels,
        const std::uint32_t minimumX,
        const std::uint32_t maximumX,
        const std::uint32_t minimumY,
        const std::uint32_t maximumY,
        const Pixel& background)
    {
        for (std::uint32_t y = minimumY;
            y < maximumY;
            ++y)
        {
            for (std::uint32_t x = minimumX;
                x < maximumX;
                ++x)
            {
                const auto pixel = At(pixels, x, y);
                const int difference =
                    std::abs(
                        pixel.red - background.red)
                    + std::abs(
                        pixel.green - background.green)
                    + std::abs(
                        pixel.blue - background.blue);
                if (difference > 60)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // 指定領域の輝度合計（R+G+B）。
    [[nodiscard]] long long RegionBrightness(
        const std::vector<std::uint8_t>& pixels,
        const std::uint32_t minimumX,
        const std::uint32_t maximumX,
        const std::uint32_t minimumY,
        const std::uint32_t maximumY)
    {
        long long total = 0;
        for (std::uint32_t y = minimumY;
            y < maximumY;
            ++y)
        {
            for (std::uint32_t x = minimumX;
                x < maximumX;
                ++x)
            {
                const auto pixel = At(pixels, x, y);
                total += pixel.red;
                total += pixel.green;
                total += pixel.blue;
            }
        }
        return total;
    }

    // 指定領域の「鋭さ」。隣どうしの差の**2乗**の平均です。
    //
    // 絶対値の平均では駄目です。単調な段差の総変動量はぼかしても
    // 保存されるので（高さHをN画素へ広げてもΣ|Δ|はHのまま）、窓を
    // 固定するとまったく同じ値になります。2乗ならH^2/Nになり、
    // 広がったぶんだけ確実に下がります。
    [[nodiscard]] double RegionSharpness(
        const std::vector<std::uint8_t>& pixels,
        const std::uint32_t minimumX,
        const std::uint32_t maximumX,
        const std::uint32_t minimumY,
        const std::uint32_t maximumY)
    {
        double total{};
        int count{};
        for (std::uint32_t y = minimumY; y < maximumY; ++y)
        {
            for (std::uint32_t x = minimumX;
                x + 1 < maximumX;
                ++x)
            {
                const auto left = At(pixels, x, y);
                const auto right = At(pixels, x + 1, y);
                const double deltaRed =
                    static_cast<double>(left.red)
                    - static_cast<double>(right.red);
                const double deltaGreen =
                    static_cast<double>(left.green)
                    - static_cast<double>(right.green);
                const double deltaBlue =
                    static_cast<double>(left.blue)
                    - static_cast<double>(right.blue);
                total += deltaRed * deltaRed
                    + deltaGreen * deltaGreen
                    + deltaBlue * deltaBlue;
                ++count;
            }
        }
        return count > 0 ? total / count : 0.0;
    }

    // --dump <フォルダー> を付けると、各段の描画結果をPPM
    // （P3。RenderRegressionTestsと同じ形式）で書き出します。
    // 目視確認用で、通常のテスト実行では使いません。
    std::filesystem::path g_dumpDirectory;
    bool g_runBenchmarks{};

    void DumpFrame(
        const char* name,
        const std::vector<std::uint8_t>& pixels)
    {
        if (g_dumpDirectory.empty())
        {
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(
            g_dumpDirectory,
            error);
        const auto path = g_dumpDirectory
            / (std::string{ name } + ".ppm");
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc);
        output << "P3\n"
               << Width << ' ' << Height
               << "\n255\n";
        for (std::uint32_t y{}; y < Height; ++y)
        {
            for (std::uint32_t x{}; x < Width; ++x)
            {
                const auto pixel = At(pixels, x, y);
                output
                    << static_cast<unsigned>(pixel.red) << ' '
                    << static_cast<unsigned>(pixel.green) << ' '
                    << static_cast<unsigned>(pixel.blue) << ' ';
            }
            output << '\n';
        }
        std::cout
            << "dumped: "
            << path.string()
            << '\n';
    }
}

int main(const int argumentCount, char** arguments)
{
    try
    {
        // --dump <dir> のときは、実GPUを優先します（Windows on ARMの
        // x64エミュレーションにはx64版WARPが無いため）。
        bool preferWarp = true;
        bool debugLayer = false;
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::string argument{ arguments[index] };
            if (argument == "--dump"
                && index + 1 < argumentCount)
            {
                g_dumpDirectory = arguments[index + 1];
                g_runBenchmarks = true;
                preferWarp = false;
                ++index;
                continue;
            }
            if (argument == "--benchmark")
            {
                g_runBenchmarks = true;
                preferWarp = false;
                continue;
            }
            // --d3ddebug: D3D11のデバッグレイヤーを有効にして、
            // 不正な描画をログへ出します。全機能を一度に掃くのに
            // 使えます（テスト自体の判定は変わりません）。
            if (argument == "--d3ddebug")
            {
                debugLayer = true;
            }
        }

        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        static_cast<void>(comResult);

        // 環境キャッシュ（プローブのベイク結果）は専用の置き場で
        // 走らせます。本物の%LOCALAPPDATA%を汚さないためと、前回の
        // 実行結果が残っていて表示が変わるのを防ぐためです。
        LamaPon::EnvironmentCache::SetCacheDirectoryOverride(
            std::filesystem::current_path()
            / "test-output"
            / "environment-cache");

        Stage("window");
        const HWND window = CreateHiddenWindow();

        // CIランナーにはGPUがないためWARPを明示します。
        LamaPon::GraphicsDevice::SetPreferWarpAdapter(
            preferWarp);
        LamaPon::GraphicsDevice::SetEnableDebugLayer(
            debugLayer);
        // このexeが立てたフラグが、LamaPonRuntime.dllの中まで
        // 届いていること。ヘッダで`inline static`にすると
        // EXE側とDLL側で別々の実体になり、**無言で効かなくなり**
        // ます（2026-08-07に--warpと--d3ddebugの両方が effectively
        // 無効になっていました）。
        Require(
            LamaPon::GraphicsDevice::IsDebugLayerEnabled()
                == debugLayer,
            "A flag set from the executable must reach the"
            " runtime DLL.");
        if (debugLayer)
        {
            // Loggerはファイルを指定しないと画面にも出ません
            // （メモリへ溜めるだけ）。行き先を作ってから知らせます。
            const auto logPath =
                (g_dumpDirectory.empty()
                    ? std::filesystem::current_path()
                    : g_dumpDirectory)
                / "d3d-debug.log";
            static_cast<void>(
                LamaPon::Logger::Instance().SetFilePath(
                    logPath));
            std::cout
                << "d3d debug layer requested; messages go to "
                << logPath.string()
                << std::endl;
        }
        LamaPon::GraphicsDevice graphics;
        Stage("initialize");
        graphics.Initialize(window, Width, Height);
        Stage("asset-root");
        graphics.Assets().SetAssetRoot(
            LAMAPON_TEST_ASSET_DIR);
        graphics.RefreshMemoryStatistics(true);
        Require(
            graphics.MemoryStats().processWorkingSetBytes > 0
                && graphics.MemoryStats().processPrivateBytes > 0
                && graphics.MemoryStats().systemPhysicalTotalBytes > 0,
            "runtime RAM statistics must be available");

        // 検証を単純にするため後処理と垂直同期を切ります。
        auto settings = graphics.Settings();
        settings.preset =
            LamaPon::GraphicsQualityPreset::Custom;
        settings.renderScale = 1.0f;
        settings.antiAliasingEnabled = false;
        settings.bloomEnabled = false;
        settings.fogEnabled = false;
        settings.vSyncEnabled = false;
        settings.shadowsEnabled = true;
        settings.shadowResolution = 512;
        settings.shadowCascadeLimit = 1;
        Stage("settings");
        graphics.SetGraphicsSettings(settings);
        // このテストは1段につき1フレームしか描かないので、非同期
        // コンパイルを入れたままだと「まだ焼けていないので標準Lit」
        // の絵を撮ってしまいます。オフラインの決め打ち描画では
        // 同期にします。
        // 非同期そのものは下の専用の段で確かめます。
        graphics.SetAsyncShaderCompilationEnabled(false);

        Stage("scene-build");
        LamaPon::Scene scene(graphics);
        auto& cameraObject =
            scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position =
            { 0.0f, 0.0f, 8.0f };
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        auto& subject =
            scene.CreateGameObject("Subject");
        subject.GetTransform().scale =
            { 2.0f, 2.0f, 2.0f };
        subject.AddComponent<
            LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{
                0.9f, 0.08f, 0.08f, 1.0f },
            std::filesystem::path{},
            std::filesystem::path{},
            1.0f,
            1.0f);

        auto& sunObject =
            scene.CreateGameObject("Sun");
        sunObject.GetTransform().SetEulerAngles({ -0.7f, 0.3f, 0.0f });
        auto& sun = sunObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        sun.SetColor({ 1.0f, 1.0f, 1.0f });
        sun.SetIntensity(2.0f);
        sunObject.SetEnabled(false);

        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.35f);

        constexpr float clearColor[4]{
            0.05f, 0.10f, 0.30f, 1.0f };
        const Pixel background{
            static_cast<std::uint8_t>(0.05f * 255.0f),
            static_cast<std::uint8_t>(0.10f * 255.0f),
            static_cast<std::uint8_t>(0.30f * 255.0f) };

        const auto renderFrame =
            [&graphics, &scene, &clearColor]
        {
            graphics.BeginFrame(clearColor);
            scene.Render();
            std::uint32_t width{};
            std::uint32_t height{};
            auto pixels = graphics.CaptureBackBuffer(
                width,
                height);
            graphics.EndFrame();
            Require(
                width == Width && height == Height,
                "Captured size must match the swap chain.");
            return pixels;
        };

        // ① クリアカラーと環境光のみの被写体
        Stage("frame-ambient");
        const auto ambientFrame = renderFrame();
        DumpFrame("ambient", ambientFrame);
        Require(
            NearColor(
                At(ambientFrame, 2, 2),
                background.red,
                background.green,
                background.blue,
                14),
            "Corner pixel must match the clear color.");
        const auto centerPixel = At(
            ambientFrame,
            Width / 2,
            Height / 2);
        Require(
            centerPixel.red
                    > centerPixel.green + 25
                && centerPixel.red
                    > centerPixel.blue + 25,
            "Center pixel must be dominated by the red cube.");


        // ② 平行光源を有効化すると被写体が明るくなる
        sunObject.SetEnabled(true);
        Stage("frame-lit");
        const auto litFrame = renderFrame();
        DumpFrame("lit", litFrame);
        const auto litBrightness = RegionBrightness(
            litFrame,
            Width / 2 - 8,
            Width / 2 + 8,
            Height / 2 - 8,
            Height / 2 + 8);
        const auto ambientBrightness = RegionBrightness(
            ambientFrame,
            Width / 2 - 8,
            Width / 2 + 8,
            Height / 2 - 8,
            Height / 2 + 8);
        Require(
            litBrightness
                > ambientBrightness
                    + ambientBrightness / 10,
            "Directional light must brighten the cube.");

        // ③ 同一マテリアルの2個はGPUインスタンシング経路で描画
        subject.GetTransform().position =
            { -2.5f, 0.0f, 0.0f };
        subject.GetTransform().scale =
            { 1.5f, 1.5f, 1.5f };
        auto& clone = scene.CreateGameObject("SubjectClone");
        clone.GetTransform().position =
            { 2.5f, 0.0f, 0.0f };
        clone.GetTransform().scale =
            { 1.5f, 1.5f, 1.5f };
        clone.AddComponent<
            LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{
                0.9f, 0.08f, 0.08f, 1.0f },
            std::filesystem::path{},
            std::filesystem::path{},
            1.0f,
            1.0f);
        Stage("frame-instanced");
        const auto instancedFrame = renderFrame();
        Require(
            RegionHasForeground(
                instancedFrame,
                Width / 8,
                Width / 2 - 20,
                Height / 2 - 20,
                Height / 2 + 20,
                background),
            "Left instanced cube must be visible.");
        Require(
            RegionHasForeground(
                instancedFrame,
                Width / 2 + 20,
                Width - Width / 8,
                Height / 2 - 20,
                Height / 2 + 20,
                background),
            "Right instanced cube must be visible.");
        Require(
            !RegionHasForeground(
                instancedFrame,
                Width / 2 - 6,
                Width / 2 + 6,
                Height / 2 - 6,
                Height / 2 + 6,
                background),
            "Center must return to the clear color.");

        // インスタンシングはGameObject::Render3Dを迂回するため、
        // 無効オブジェクトをバッチへ混ぜると通常経路と違って描画
        // されてしまいます。無効化した右側だけが消えることを固定します。
        clone.SetEnabled(false);
        Stage("frame-instanced-disabled-object");
        const auto disabledInstanceFrame = renderFrame();
        Require(
            RegionHasForeground(
                disabledInstanceFrame,
                Width / 8,
                Width / 2 - 20,
                Height / 2 - 20,
                Height / 2 + 20,
                background),
            "Enabled instance must remain visible.");
        Require(
            !RegionHasForeground(
                disabledInstanceFrame,
                Width / 2 + 20,
                Width - Width / 8,
                Height / 2 - 20,
                Height / 2 + 20,
                background),
            "Disabled instance must not be rendered by batching.");
        clone.SetEnabled(true);

        // ④ カスケード影：影の有無で床の明るさが変わる
        auto& ground = scene.CreateGameObject("Ground");
        ground.GetTransform().position =
            { 0.0f, -1.8f, 0.0f };
        ground.GetTransform().scale =
            { 26.0f, 0.2f, 26.0f };
        ground.AddComponent<
            LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{
                0.75f, 0.75f, 0.75f, 1.0f },
            std::filesystem::path{},
            std::filesystem::path{},
            1.0f,
            1.0f);
        sunObject.GetTransform().SetEulerAngles({ -1.1f, 0.25f, 0.0f });

        Stage("frame-shadowed");
        const auto shadowedFrame = renderFrame();
        settings.shadowsEnabled = false;
        graphics.SetGraphicsSettings(settings);
        Stage("frame-unshadowed");
        const auto unshadowedFrame = renderFrame();
        settings.shadowsEnabled = true;
        graphics.SetGraphicsSettings(settings);

        const auto shadowedFloor = RegionBrightness(
            shadowedFrame,
            0,
            Width,
            Height / 2 + 10,
            Height);
        const auto unshadowedFloor = RegionBrightness(
            unshadowedFrame,
            0,
            Width,
            Height / 2 + 10,
            Height);
        Require(
            shadowedFloor + unshadowedFloor / 300
                < unshadowedFloor,
            "Shadows must darken part of the floor.");

        // ⑤ スカイ：背景がクリアカラーからグラデーションに変わる
        auto sky = scene.Sky();
        sky.enabled = true;
        sky.topColor = { 0.05f, 0.55f, 0.15f };
        sky.horizonColor = { 0.75f, 0.45f, 0.15f };
        scene.SetSkySettings(sky);
        Stage("frame-sky");
        const auto skyFrame = renderFrame();
        DumpFrame("sky", skyFrame);
        Require(
            !NearColor(
                At(skyFrame, Width / 2, 4),
                background.red,
                background.green,
                background.blue,
                14),
            "Sky gradient must replace the clear color.");

        // ⑥ Light2D：組み込みシェーダー（LamaPonSpriteLit.hlsl）が
        // 実際にコンパイルでき、加算式ライティングとして機能することを
        // 確認します。暗めのグレーを土台にすることで、白飽和で
        // チャンネル差が消えないようにしています。
        constexpr std::uint32_t LitSpriteSampleX = 260;
        constexpr std::uint32_t LitSpriteSampleY = 150;
        auto& litSpriteObject =
            scene.CreateGameObject("LitSprite");
        litSpriteObject.GetTransform().position =
            { 240.0f, 130.0f, 0.0f };
        litSpriteObject.AddComponent<
            LamaPon::SpriteRendererComponent>(
                DirectX::XMFLOAT2{ 40.0f, 40.0f },
                DirectX::XMFLOAT4{
                    0.15f, 0.15f, 0.15f, 1.0f });

        Stage("frame-light2d-none");
        const auto unlitFrame = renderFrame();
        DumpFrame("light2d-none", unlitFrame);
        const auto unlitPixel = At(
            unlitFrame,
            LitSpriteSampleX,
            LitSpriteSampleY);
        Require(
            unlitPixel.red < 60
                && unlitPixel.green < 60
                && unlitPixel.blue < 60,
            "The dark sprite must render at its own color"
                " before any Light2D exists.");

        auto& lightObject =
            scene.CreateGameObject("TestLight2D");
        lightObject.GetTransform().position =
            { -5000.0f, -5000.0f, 0.0f };
        auto& testLight2D =
            lightObject.AddComponent<
                LamaPon::Light2DComponent>();
        testLight2D.SetColor({ 0.0f, 0.4f, 1.0f });
        testLight2D.SetIntensity(1.0f);
        testLight2D.SetRadius(50.0f);

        Stage("frame-light2d-out-of-range");
        const auto farFrame = renderFrame();
        DumpFrame("light2d-far", farFrame);
        const auto farPixel = At(
            farFrame,
            LitSpriteSampleX,
            LitSpriteSampleY);
        Require(
            std::abs(
                static_cast<int>(farPixel.red)
                    - static_cast<int>(unlitPixel.red))
                    <= 2
                && std::abs(
                    static_cast<int>(farPixel.green)
                        - static_cast<int>(
                            unlitPixel.green))
                    <= 2
                && std::abs(
                    static_cast<int>(farPixel.blue)
                        - static_cast<int>(
                            unlitPixel.blue))
                    <= 2,
            "A Light2D outside its radius must not"
                " visibly affect the sprite.");

        lightObject.GetTransform().position =
            {
                static_cast<float>(LitSpriteSampleX),
                static_cast<float>(LitSpriteSampleY),
                0.0f
            };
        Stage("frame-light2d-near");
        const auto light2DNearFrame = renderFrame();
        DumpFrame("light2d-near", light2DNearFrame);
        const auto light2DNearPixel = At(
            light2DNearFrame,
            LitSpriteSampleX,
            LitSpriteSampleY);
        Require(
            light2DNearPixel.blue
                    > unlitPixel.blue + 15
                && light2DNearPixel.green
                    > unlitPixel.green + 5
                && light2DNearPixel.blue
                    > light2DNearPixel.green
                && light2DNearPixel.green
                    > light2DNearPixel.red,
            "A nearby blue-tinted Light2D must brighten"
                " and tint the sprite toward blue without"
                " affecting its red channel.");

        // ⑥-2 Light2DがTilemapへ届くこと。
        // 灯りをSprite単位ではなく画面単位で評価するようにした本題
        // です。以前は「オブジェクトの原点に近い2灯」だったので、
        // 画面いっぱいに広がるTilemapでは成立しませんでした。
        constexpr std::uint32_t TilemapSampleX = 90;
        constexpr std::uint32_t TilemapSampleY = 90;
        auto& tilemapObject =
            scene.CreateGameObject("LitTilemap");
        tilemapObject.GetTransform().position =
            { 60.0f, 60.0f, 0.0f };
        auto& tilemap =
            tilemapObject.AddComponent<
                LamaPon::TilemapComponent>(
                DirectX::XMFLOAT2{ 32.0f, 32.0f },
                1u,
                1u,
                DirectX::XMFLOAT4{
                    0.15f, 0.15f, 0.15f, 1.0f });
        static_cast<void>(tilemap.SetCell(0, 0, 0));
        static_cast<void>(tilemap.SetCell(1, 0, 0));
        static_cast<void>(tilemap.SetCell(0, 1, 0));
        static_cast<void>(tilemap.SetCell(1, 1, 0));

        Stage("frame-tilemap-unlit");
        const auto tilemapUnlitFrame = renderFrame();
        DumpFrame("tilemap-unlit", tilemapUnlitFrame);
        const auto tilemapUnlitPixel = At(
            tilemapUnlitFrame,
            TilemapSampleX,
            TilemapSampleY);
        Require(
            tilemapUnlitPixel.red < 60
                && tilemapUnlitPixel.green < 60
                && tilemapUnlitPixel.blue < 60,
            "The dark tilemap must render at its own color"
                " while every Light2D is out of range.");

        auto& tilemapLightObject =
            scene.CreateGameObject("TilemapLight2D");
        tilemapLightObject.GetTransform().position =
            {
                static_cast<float>(TilemapSampleX),
                static_cast<float>(TilemapSampleY),
                0.0f
            };
        auto& tilemapLight =
            tilemapLightObject.AddComponent<
                LamaPon::Light2DComponent>();
        tilemapLight.SetColor({ 0.0f, 0.4f, 1.0f });
        tilemapLight.SetIntensity(1.0f);
        tilemapLight.SetRadius(50.0f);

        Stage("frame-tilemap-lit");
        const auto tilemapLitFrame = renderFrame();
        DumpFrame("tilemap-lit", tilemapLitFrame);
        const auto tilemapLitPixel = At(
            tilemapLitFrame,
            TilemapSampleX,
            TilemapSampleY);
        Require(
            tilemapLitPixel.blue
                    > tilemapUnlitPixel.blue + 15
                && tilemapLitPixel.blue
                    > tilemapLitPixel.green
                && tilemapLitPixel.green
                    > tilemapLitPixel.red,
            "A nearby blue-tinted Light2D must brighten and"
                " tint a Tilemap, not only Sprite Renderers.");

        // ⑥-3 UIは既定では照らさず、「UIも照らす」を入れたときだけ
        // 照らすこと。特定の画素ではなく「変わった画素の数」で見ます
        // （UIの矩形はアンカー解決で決まるので、位置を決め打ちすると
        // 解決規則を変えたときに空振りするため）。
        auto& uiSpriteObject =
            scene.CreateGameObject("LitUISprite");
        uiSpriteObject.AddComponent<
            LamaPon::UIRectTransformComponent>(
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            DirectX::XMFLOAT2{
                static_cast<float>(TilemapSampleX) - 20.0f,
                static_cast<float>(TilemapSampleY) - 20.0f },
            DirectX::XMFLOAT2{ 40.0f, 40.0f });
        uiSpriteObject.AddComponent<
            LamaPon::SpriteRendererComponent>(
                DirectX::XMFLOAT2{ 40.0f, 40.0f },
                DirectX::XMFLOAT4{
                    0.15f, 0.15f, 0.15f, 1.0f });

        Stage("frame-ui-light-off");
        const auto uiUnlitFrame = renderFrame();
        DumpFrame("ui-light-off", uiUnlitFrame);

        tilemapLight.SetAffectsUI(true);
        Stage("frame-ui-light-on");
        const auto uiLitFrame = renderFrame();
        DumpFrame("ui-light-on", uiLitFrame);

        int changedPixels{};
        for (std::uint32_t y = 0; y < Height; ++y)
        {
            for (std::uint32_t x = 0; x < Width; ++x)
            {
                const auto& before = At(uiUnlitFrame, x, y);
                const auto& after = At(uiLitFrame, x, y);
                if (std::abs(
                        static_cast<int>(after.blue)
                        - static_cast<int>(before.blue))
                    > 8)
                {
                    ++changedPixels;
                }
            }
        }
        std::cout
            << "ui light2d changed pixels: "
            << changedPixels << std::endl;
        // UIスプライトは40x40＝1600画素。灯りは中心が最も強く、
        // 縁では0になるので全部は変わりません。実測で900前後です。
        // 「UIへ届いていない」ときは0になるので、そこと明確に離した
        // 400を閾値にしています。
        Require(
            changedPixels > 400,
            "Turning on a Light2D's AffectsUI must light the"
                " UI sprite.");

        tilemapLight.SetAffectsUI(false);
        static_cast<void>(
            scene.DestroyGameObject(uiSpriteObject));
        static_cast<void>(
            scene.DestroyGameObject(tilemapLightObject));
        static_cast<void>(
            scene.DestroyGameObject(tilemapObject));

        // ⑦ Sprite Mask：組み込みシェーダー（LamaPonSpriteMask.hlsl）が
        // 実際にコンパイルでき、円マスクの内側／外側で正しくクリップ
        // されることを確認します。マスク円の中心は必ず可視、円から
        // 十分離れた（しかしスプライト矩形内の）点は必ず不可視という、
        // 形状に依存しない判定にしています。
        constexpr std::uint32_t MaskInsideX = 70;
        constexpr std::uint32_t MaskInsideY = 70;
        constexpr std::uint32_t MaskOutsideX = 110;
        constexpr std::uint32_t MaskOutsideY = 30;
        auto& maskedSpriteObject =
            scene.CreateGameObject("MaskedSprite");
        maskedSpriteObject.GetTransform().position =
            { 20.0f, 20.0f, 0.0f };
        auto& maskedSprite =
            maskedSpriteObject.AddComponent<
                LamaPon::SpriteRendererComponent>(
                DirectX::XMFLOAT2{ 100.0f, 100.0f },
                DirectX::XMFLOAT4{
                    0.15f, 0.15f, 0.15f, 1.0f });
        maskedSprite.SetMaskInteraction(
            LamaPon::SpriteMaskInteraction::
                VisibleInsideMask);

        auto& maskObject =
            scene.CreateGameObject("TestSpriteMask");
        maskObject.GetTransform().position =
            { 70.0f, 70.0f, 0.0f };
        auto& spriteMask =
            maskObject.AddComponent<
                LamaPon::SpriteMaskComponent>();
        spriteMask.SetShape(
            LamaPon::SpriteMaskShape::Circle);
        spriteMask.SetSize({ 60.0f, 60.0f });

        Stage("frame-sprite-mask-inside");
        const auto maskFrame = renderFrame();
        DumpFrame("sprite-mask", maskFrame);
        const auto insidePixel = At(
            maskFrame, MaskInsideX, MaskInsideY);
        Require(
            insidePixel.red < 60
                && insidePixel.green < 60
                && insidePixel.blue < 60,
            "The mask center must show the sprite's own"
                " dark color (VisibleInsideMask).");
        const auto outsidePixel = At(
            maskFrame, MaskOutsideX, MaskOutsideY);
        Require(
            std::abs(
                static_cast<int>(outsidePixel.red)
                    - static_cast<int>(insidePixel.red))
                > 20
                || std::abs(
                    static_cast<int>(outsidePixel.green)
                        - static_cast<int>(
                            insidePixel.green))
                > 20
                || std::abs(
                    static_cast<int>(outsidePixel.blue)
                        - static_cast<int>(
                            insidePixel.blue))
                > 20,
            "A point outside the mask circle but inside"
                " the sprite's rect must not show the"
                " sprite's own color (VisibleInsideMask"
                " should clip it away).");

        // 2Dの壊れたShader。3Dと同じくマゼンタで知らせます。
        // マスクを外した素の矩形で見ます（マスクが効いたままだと
        // 「クリップされた」のか「色が出ていない」のか分かりません）。
        {
            maskedSprite.SetMaskInteraction(
                LamaPon::SpriteMaskInteraction::None);
            const auto sampleSprite =
                [&](const char* name) -> Pixel
            {
                Stage(name);
                const auto frame = renderFrame();
                DumpFrame(name, frame);
                return At(frame, MaskInsideX, MaskInsideY);
            };

            const auto plain =
                sampleSprite("sprite-error-plain");
            maskedSprite.SetShaderPath(
                std::filesystem::path{
                    LAMAPON_TEST_FIXTURE_DIR }
                / "broken-shader.hlsl");
            const auto broken =
                sampleSprite("sprite-error-broken");
            maskedSprite.SetShaderPath({});
            const auto repaired =
                sampleSprite("sprite-error-repaired");

            std::cout
                << "sprite error: plain=("
                << static_cast<int>(plain.red)
                << "," << static_cast<int>(plain.green)
                << "," << static_cast<int>(plain.blue)
                << ") broken=("
                << static_cast<int>(broken.red)
                << "," << static_cast<int>(broken.green)
                << "," << static_cast<int>(broken.blue)
                << ") repaired=("
                << static_cast<int>(repaired.red)
                << "," << static_cast<int>(repaired.green)
                << "," << static_cast<int>(repaired.blue)
                << ")" << std::endl;

            Require(
                broken.red > 90
                    && broken.blue > 90
                    && broken.green + 60 < broken.red
                    && broken.green + 60 < broken.blue,
                "A 2D shader that fails to compile must be"
                " drawn with the magenta placeholder.");
            // スプライト自身は暗い色。代役が居座っていないこと。
            Require(
                repaired.red < 60
                    && repaired.green < 60
                    && repaired.blue < 60,
                "Clearing the broken 2D shader must bring the"
                " sprite's own colour back.");
            Require(
                plain.red < 60,
                "The sprite must start from its own colour.");
        }

        maskedSprite.SetMaskInteraction(
            LamaPon::SpriteMaskInteraction::
                VisibleOutsideMask);
        Stage("frame-sprite-mask-outside");
        const auto invertedFrame = renderFrame();
        DumpFrame("sprite-mask-inverted", invertedFrame);
        const auto invertedInsidePixel = At(
            invertedFrame, MaskInsideX, MaskInsideY);
        const auto invertedOutsidePixel = At(
            invertedFrame, MaskOutsideX, MaskOutsideY);
        Require(
            invertedOutsidePixel.red < 60
                && invertedOutsidePixel.green < 60
                && invertedOutsidePixel.blue < 60,
            "A point outside the mask circle must show the"
                " sprite's own dark color with"
                " VisibleOutsideMask.");
        Require(
            std::abs(
                static_cast<int>(invertedInsidePixel.red)
                    - static_cast<int>(
                        invertedOutsidePixel.red))
                > 20
                || std::abs(
                    static_cast<int>(
                        invertedInsidePixel.green)
                        - static_cast<int>(
                            invertedOutsidePixel.green))
                > 20
                || std::abs(
                    static_cast<int>(
                        invertedInsidePixel.blue)
                        - static_cast<int>(
                            invertedOutsidePixel.blue))
                > 20,
            "The mask center must not show the sprite's"
                " own color with VisibleOutsideMask.");

        // ⑧ レンダーテクスチャ：サブカメラの絵をSpriteで画面へ出す。
        // 何も無い方向を向かせるので、テクスチャは背景色一色に
        // なります。左上に貼ったSpriteがその色になれば成功です。
        constexpr std::uint32_t RenderTextureSize = 128;
        constexpr float SpriteSize = 64.0f;
        auto& subCameraObject =
            scene.CreateGameObject("MinimapCamera");
        subCameraObject.GetTransform().position =
            { 0.0f, 40.0f, 0.0f };
        subCameraObject.GetTransform().SetEulerAngles({ 1.5f, 0.0f, 0.0f });
        auto& subCamera =
            subCameraObject.AddComponent<
                LamaPon::CameraComponent>();
        subCamera.SetTargetTexture("minimap");
        subCamera.SetTargetTextureSize(
            RenderTextureSize,
            RenderTextureSize);
        subCamera.SetTargetClearColor(
            { 0.0f, 0.85f, 0.15f, 1.0f });

        auto& minimapSprite =
            scene.CreateGameObject("MinimapSprite");
        auto& minimapRenderer =
            minimapSprite.AddComponent<
                LamaPon::SpriteRendererComponent>(
                    DirectX::XMFLOAT2{
                        SpriteSize,
                        SpriteSize });
        minimapRenderer.SetRenderTexture("minimap");

        Stage("frame-render-texture");
        const auto renderTextureFrame = renderFrame();
        DumpFrame("render-texture", renderTextureFrame);
        Require(
            graphics.RenderTextureView("minimap")
                != nullptr,
            "The named render texture must exist after a frame.");
        const auto* minimapTarget =
            graphics.FindRenderTexture("minimap");
        Require(
            minimapTarget != nullptr
                && minimapTarget->Width()
                    == RenderTextureSize
                && minimapTarget->Height()
                    == RenderTextureSize,
            "The render texture must use the requested resolution.");
        // レンダーテクスチャにもスカイとシーンのカラーグレーディング
        // （トーンマップ・ビネット）がかかるため、サブカメラの
        // クリア色がそのままピクセルへ出てくるわけではありません。
        // 一方でこの直後のアサーションはメインビューにスカイが
        // 出ていることを要求しており、同じフレームなのでスカイを
        // 切って厳密な色にすることもできません。
        // そこで「Spriteの位置に背景と明確に違う絵が出ていて、
        // かつ緑優勢である（サブカメラの緑いクリア色／スカイ上部の
        // 緑）」ことで、レンダーテクスチャが表示されていることを
        // 確認します。後処理の係数に依存しません。
        Require(
            RegionHasForeground(
                renderTextureFrame,
                4,
                static_cast<std::uint32_t>(SpriteSize) - 4,
                4,
                static_cast<std::uint32_t>(SpriteSize) - 4,
                background),
            "The sprite must display the render texture contents.");
        const auto minimapPixel =
            At(renderTextureFrame, 24, 24);
        Require(
            minimapPixel.green > minimapPixel.red
                && minimapPixel.green > minimapPixel.blue,
            "The sprite must display the render texture contents.");
        // 描画先の復元漏れがあると、メインカメラの絵がテクスチャ側へ
        // 流れて画面が変わってしまいます。空の帯で確認します。
        Require(
            !NearColor(
                At(
                    renderTextureFrame,
                    Width / 2,
                    4),
                background.red,
                background.green,
                background.blue,
                14),
            "The main view must still be rendered after the render texture pass.");
        Require(
            graphics.RenderTextureNames().size() == 1,
            "Only the requested render texture should exist.");
        Require(
            graphics.ReleaseRenderTexture("minimap")
                && graphics.RenderTextureView("minimap")
                    == nullptr,
            "Releasing a render texture must remove it.");

        // GIの保存→読み込み検証をテストの最後で行うための受け渡し
        // （ベイクする節と検証する節が離れているため、両方から
        // 見えるこの階層で宣言します）。
        std::string giRoundTripJson;
        long long giRoundTripExpectedNear = 0;

        // 上のrenderFrameはBeginFrame→Scene::Render→Captureで、
        // ポスト処理（SSAO・光の筋・Bloom・トーンマップ）を通りません。
        // それらはApplicationがBeginSceneComposition〜
        // EndSceneCompositionの間で行うため、ここでは実ゲームと
        // 同じ順序を再現します。
        const auto renderComposedFrame =
            [&graphics, &scene, &clearColor]
            {
                graphics.BeginFrame(clearColor);
                graphics.BeginSceneComposition(clearColor);
                scene.RenderMainCamera(
                    graphics.AspectRatio(),
                    false,
                    graphics.SceneCompositionTarget());
                // Application.cppやEditorLayer.cppと同じ引数で
                // 呼びます。ここが古い呼び方のままだと、ポスト処理を
                // 1つ足すたびに「テストだけ通っていない経路」が
                // 静かに増えていきます。
                graphics.EndSceneComposition(
                    scene.PostProcessFrameData());
                std::uint32_t width{};
                std::uint32_t height{};
                auto pixels = graphics.CaptureBackBuffer(
                    width,
                    height);
                graphics.EndFrame();
                return pixels;
            };

        const auto composedBaseFrame = renderComposedFrame();
        auto outlineSettings = scene.ScreenOutline();
        outlineSettings.enabled = true;
        outlineSettings.color = { 0.0f, 0.0f, 0.0f };
        outlineSettings.intensity = 1.0f;
        outlineSettings.thickness = 1.0f;
        outlineSettings.depthThreshold = 0.025f;
        outlineSettings.normalThreshold = 0.25f;
        scene.SetScreenOutlineSettings(outlineSettings);
        Stage("frame-outline");
        const auto outlineFrame = renderComposedFrame();
        DumpFrame("outline", outlineFrame);
        std::size_t outlineChangedPixels{};
        for (std::uint32_t y = 1; y + 1 < Height; ++y)
        {
            for (std::uint32_t x = 1; x + 1 < Width; ++x)
            {
                const auto before = At(composedBaseFrame, x, y);
                const auto after = At(outlineFrame, x, y);
                const int difference =
                    std::abs(
                        static_cast<int>(before.red)
                        - static_cast<int>(after.red))
                    + std::abs(
                        static_cast<int>(before.green)
                        - static_cast<int>(after.green))
                    + std::abs(
                        static_cast<int>(before.blue)
                        - static_cast<int>(after.blue));
                if (difference > 8)
                {
                    ++outlineChangedPixels;
                }
            }
        }
        std::cout
            << "screen outline changed pixels: "
            << outlineChangedPixels
            << '\n';
        Require(
            outlineChangedPixels > 20,
            "Screen outline must change visible boundary pixels.");
        scene.SetScreenOutlineSettings({});

        // ⑨ ボリュメトリックライト（光の筋）。
        //
        // 太陽をカメラの正面奥へ置き、間に「中央だけ隙間のある壁」を
        // 立てます。隙間からは空が見え、レイは最後まで光の中を通ります。
        //
        // 影の判定が効いているかは、壁の上の2箇所を比べて確かめます。
        // どちらも同じ壁（＝カメラからの距離が同じ）なので、距離による
        // 減衰は揃います。違うのは「レイのどこから壁の影に入るか」です。
        //   ・隙間に近い側は、視線が中央寄りなので長い区間を隙間越しの
        //     光の中で進み、途中から壁の影へ入ります
        //   ・画面の端に近い側は、すぐ壁の影へ入ります
        // 前方散乱を0（全方向へ均一）にすると位相関数の角度差も消える
        // ので、この2箇所に差が出る理由は影の判定しか残りません。
        // ただの霧を足しているだけの実装なら、2箇所は同じだけ明るく
        // なってしまいます。
        {
            const auto savedSunRotation =
                sunObject.GetTransform().EulerAngles();
            const auto savedCameraPosition =
                cameraObject.GetTransform().position;
            const auto savedCameraRotation =
                cameraObject.GetTransform().EulerAngles();
            const auto savedSubjectPosition =
                subject.GetTransform().position;
            const auto savedClonePosition =
                clone.GetTransform().position;

            // 光がカメラへ向かって進む向き（前方散乱が最大になる）。
            // yaw=πで奥（-Z）から手前（+Z）へ、pitchで少し下向き。
            sunObject.GetTransform().SetEulerAngles(
                { -0.5236f, 3.14159265f, 0.0f });
            // キューブは視界の外へ退かせ、壁と空気だけを見ます
            // （壁の前に立っていると測る帯の奥行きが変わります）。
            subject.GetTransform().position =
                { 0.0f, 60.0f, 0.0f };
            clone.GetTransform().position =
                { 6.0f, 60.0f, 0.0f };
            cameraObject.GetTransform().position =
                { 0.0f, 0.0f, 8.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { 0.0f, 0.0f, 0.0f });

            const auto createBlocker =
                [&scene](const float centerX)
                -> LamaPon::GameObject&
                {
                    auto& wall = scene.CreateGameObject(
                        "VolumetricBlocker");
                    wall.GetTransform().position =
                        { centerX, 0.0f, -4.0f };
                    wall.GetTransform().scale =
                        { 8.0f, 24.0f, 1.0f };
                    wall.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Cube,
                        DirectX::XMFLOAT4{
                            0.18f, 0.18f, 0.20f, 1.0f },
                        std::filesystem::path{},
                        std::filesystem::path{},
                        1.0f,
                        1.0f);
                    return wall;
                };
            auto& leftWall = createBlocker(-6.0f);
            auto& rightWall = createBlocker(6.0f);

            // 縦画角45度・カメラz=8・壁z=-4なので、画面に映る半幅は
            // 約8.8。壁の内側の縁（x=±2）は320px幅のx=124とx=196
            // あたりに来ます。床の地平線はy=121付近なので、帯は
            // y=60〜120に取れば壁だけを見ます。
            constexpr std::uint32_t gapMinimumX = 135;
            constexpr std::uint32_t gapMaximumX = 185;
            // 右の壁の2箇所（隙間寄りと画面端寄り）。奥行きは同じ。
            constexpr std::uint32_t wallNearMinimumX = 205;
            constexpr std::uint32_t wallNearMaximumX = 240;
            constexpr std::uint32_t wallFarMinimumX = 280;
            constexpr std::uint32_t wallFarMaximumX = 315;
            constexpr std::uint32_t bandMinimumY = 60;
            constexpr std::uint32_t bandMaximumY = 120;

            const auto bandBrightness =
                [&](const std::vector<std::uint8_t>& frame,
                    const std::uint32_t minimumX,
                    const std::uint32_t maximumX)
                {
                    return RegionBrightness(
                        frame,
                        minimumX,
                        maximumX,
                        bandMinimumY,
                        bandMaximumY);
                };

            auto volumetric = scene.VolumetricLight();
            volumetric.enabled = false;
            scene.SetVolumetricLightSettings(volumetric);
            Stage("frame-volumetric-off");
            const auto volumetricOffFrame =
                renderComposedFrame();
            DumpFrame(
                "volumetric-off",
                volumetricOffFrame);

            volumetric.enabled = true;
            volumetric.intensity = 1.2f;
            volumetric.sampleCount = 48;
            volumetric.maximumDistance = 24.0f;
            volumetric.scattering = 0.75f;
            scene.SetVolumetricLightSettings(volumetric);
            Stage("frame-volumetric-on");
            const auto volumetricOnFrame =
                renderComposedFrame();
            DumpFrame(
                "volumetric-on",
                volumetricOnFrame);

            const auto gapBefore = bandBrightness(
                volumetricOffFrame,
                gapMinimumX,
                gapMaximumX);
            const auto gapAfter = bandBrightness(
                volumetricOnFrame,
                gapMinimumX,
                gapMaximumX);
            std::cout
                << "volumetric gap: "
                << gapBefore << " -> " << gapAfter
                << std::endl;
            // 隙間は明るくなること。効いていなければ差は完全に0です。
            Require(
                gapAfter
                    > gapBefore + gapBefore / 100 + 1,
                "Volumetric light must brighten the gap between the blockers.");

            // 前方散乱を0にして、位相関数の角度差を消した状態で
            // 壁の2箇所を比べます。強度は最大まで上げて、8bitの
            // 読み出しで差が丸まらないようにします。
            volumetric.scattering = 0.0f;
            volumetric.intensity = 4.0f;
            scene.SetVolumetricLightSettings(volumetric);
            Stage("frame-volumetric-isotropic");
            const auto volumetricIsotropicFrame =
                renderComposedFrame();
            DumpFrame(
                "volumetric-isotropic",
                volumetricIsotropicFrame);

            const auto wallNearGain =
                bandBrightness(
                    volumetricIsotropicFrame,
                    wallNearMinimumX,
                    wallNearMaximumX)
                - bandBrightness(
                    volumetricOffFrame,
                    wallNearMinimumX,
                    wallNearMaximumX);
            const auto wallFarGain =
                bandBrightness(
                    volumetricIsotropicFrame,
                    wallFarMinimumX,
                    wallFarMaximumX)
                - bandBrightness(
                    volumetricOffFrame,
                    wallFarMinimumX,
                    wallFarMaximumX);
            std::cout
                << "volumetric isotropic gain: near="
                << wallNearGain
                << " far=" << wallFarGain
                << std::endl;
            Require(
                wallNearGain > 0,
                "Isotropic volumetric light must still add light to the wall.");
            // 端寄りの方が早く影へ入るので、はっきり少なく光ります。
            // 一律に霧を足しているだけなら差は出ません。
            Require(
                wallFarGain * 10 < wallNearGain * 9,
                "The band that enters the blocker's shadow sooner must gain less light.");

            // 元へ戻すとオフの絵と1バイトも変わらないこと。
            // 設定の切り替えでターゲットの入れ替えが崩れていないか
            // （光の筋はカラーターゲットをswapするため）の確認です。
            volumetric.enabled = false;
            scene.SetVolumetricLightSettings(volumetric);
            Stage("frame-volumetric-off-again");
            const auto volumetricRestoredFrame =
                renderComposedFrame();
            Require(
                volumetricRestoredFrame
                    == volumetricOffFrame,
                "Disabling volumetric light must restore the exact original frame.");

            static_cast<void>(
                scene.DestroyGameObject(rightWall));
            static_cast<void>(
                scene.DestroyGameObject(leftWall));
            sunObject.GetTransform().SetEulerAngles(
                savedSunRotation);
            cameraObject.GetTransform().position =
                savedCameraPosition;
            cameraObject.GetTransform().SetEulerAngles(
                savedCameraRotation);
            subject.GetTransform().position =
                savedSubjectPosition;
            clone.GetTransform().position =
                savedClonePosition;
            scene.SetVolumetricLightSettings(
                LamaPon::VolumetricLightSettings{});
        }

        // ⑩ リフレクションプローブのブレンド。
        //
        // カメラの画角の外（左右x=±10）へ緑と赤の壁を置き、その
        // すぐ内側にプローブを1つずつ焼きます。壁は画面に映らない
        // ので、鏡面の球に出る色は「どちらのプローブを読んだか」
        // だけで決まります。
        //
        // 球は2つのプローブの中間（＝影響度が同じ）に置きます。
        //   ・混ぜ始める距離0 … 近い方だけを読む（従来の挙動）。
        //     片方だけを有効にした絵とバイト単位で一致するはず
        //   ・混ぜ始める距離あり … 半々で混ざるので、緑と赤の差
        //     （G−R）が単独のときよりはっきり小さくなるはず
        {
            const auto savedCameraPosition =
                cameraObject.GetTransform().position;
            const auto savedCameraRotation =
                cameraObject.GetTransform().EulerAngles();
            const auto savedSubjectPosition =
                subject.GetTransform().position;
            const auto savedClonePosition =
                clone.GetTransform().position;

            subject.GetTransform().position =
                { 0.0f, 60.0f, 0.0f };
            clone.GetTransform().position =
                { 6.0f, 60.0f, 0.0f };
            cameraObject.GetTransform().position =
                { 0.0f, 0.5f, 6.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { -0.08f, 0.0f, 0.0f });

            auto& mirrorSphere =
                scene.CreateGameObject("BlendMirror");
            mirrorSphere.GetTransform().position =
                { 0.0f, 0.0f, 0.0f };
            mirrorSphere.GetTransform().scale =
                { 2.0f, 2.0f, 2.0f };
            auto& mirrorRenderer =
                mirrorSphere.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Sphere,
                    DirectX::XMFLOAT4{
                        1.0f, 1.0f, 1.0f, 1.0f },
                    std::filesystem::path{},
                    std::filesystem::path{},
                    0.05f,
                    1.0f);
            mirrorRenderer.SetMetallic(1.0f);

            const auto createColoredWall =
                [&scene](
                    const char* name,
                    const float centerX,
                    const DirectX::XMFLOAT4& color)
                -> LamaPon::GameObject&
                {
                    auto& wall =
                        scene.CreateGameObject(name);
                    wall.GetTransform().position =
                        { centerX, 0.0f, 0.0f };
                    // 奥行きを詰めて、画角の外に収めます
                    // （13m先の画面の半幅は9.6なので、x=9.8から
                    // 始まる壁が入り込まないようにするため）。
                    wall.GetTransform().scale =
                        { 0.4f, 14.0f, 10.0f };
                    wall.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Cube,
                        color,
                        std::filesystem::path{},
                        std::filesystem::path{},
                        1.0f,
                        1.0f);
                    return wall;
                };
            auto& greenWall = createColoredWall(
                "BlendWallGreen",
                -10.0f,
                { 0.05f, 0.9f, 0.1f, 1.0f });
            auto& redWall = createColoredWall(
                "BlendWallRed",
                10.0f,
                { 0.9f, 0.06f, 0.05f, 1.0f });

            const auto createProbe =
                [&scene](
                    const char* name,
                    const float centerX)
                -> LamaPon::GameObject&
                {
                    auto& probeObject =
                        scene.CreateGameObject(name);
                    probeObject.GetTransform().position =
                        { centerX, 0.0f, 0.0f };
                    probeObject.AddComponent<
                        LamaPon::
                            ReflectionProbeComponent>(
                        20.0f,
                        1.0f);
                    return probeObject;
                };
            auto& greenProbe = createProbe(
                "BlendProbeGreen",
                -5.0f);
            auto& redProbe = createProbe(
                "BlendProbeRed",
                5.0f);

            // 球が映っている中央の帯。
            constexpr std::uint32_t sphereMinimumX = 145;
            constexpr std::uint32_t sphereMaximumX = 175;
            constexpr std::uint32_t sphereMinimumY = 75;
            constexpr std::uint32_t sphereMaximumY = 105;
            const auto sphereSkew =
                [&](const std::vector<std::uint8_t>& frame)
                {
                    long long green = 0;
                    long long red = 0;
                    for (std::uint32_t y = sphereMinimumY;
                        y < sphereMaximumY;
                        ++y)
                    {
                        for (std::uint32_t x =
                                sphereMinimumX;
                            x < sphereMaximumX;
                            ++x)
                        {
                            const auto pixel =
                                At(frame, x, y);
                            green += pixel.green;
                            red += pixel.red;
                        }
                    }
                    return green - red;
                };

            // 緑のプローブだけ。
            redProbe.SetEnabled(false);
            Stage("frame-probe-blend-green-only");
            const auto greenOnlyFrame =
                renderComposedFrame();
            DumpFrame(
                "probe-blend-green",
                greenOnlyFrame);

            // 赤のプローブだけ。
            redProbe.SetEnabled(true);
            greenProbe.SetEnabled(false);
            Stage("frame-probe-blend-red-only");
            const auto redOnlyFrame =
                renderComposedFrame();
            DumpFrame(
                "probe-blend-red",
                redOnlyFrame);

            const auto greenOnlySkew =
                sphereSkew(greenOnlyFrame);
            const auto redOnlySkew =
                sphereSkew(redOnlyFrame);
            std::cout
                << "probe blend skew(G-R): greenOnly="
                << greenOnlySkew
                << " redOnly=" << redOnlySkew
                << std::endl;
            // まず、プローブごとに色が違うことを確かめます
            // （これが成り立たないと以降の比較に意味がありません）。
            //
            // 符号が反転するとは限りません。鏡面の球は周囲すべてを
            // 映すので、どちらのプローブも両方の壁を見ています
            // （近い方が大きく映る、という差にしかなりません）。
            // 見るのは「緑寄りの度合いに差がある」ことです。
            Require(
                greenOnlySkew - redOnlySkew > 1000,
                "The two probes must tint the mirror sphere differently.");

            // 両方を有効にして、混ぜ始める距離0（従来の挙動）。
            greenProbe.SetEnabled(true);
            Stage("frame-probe-blend-off");
            const auto hardSwitchFrame =
                renderComposedFrame();
            DumpFrame(
                "probe-blend-off",
                hardSwitchFrame);
            // 近い方だけを読むので、単独で描いたどちらかの絵と
            // 1バイトも違わないはずです。既定値のままの既存シーンが
            // 変わらないことの証明になります。
            Require(
                hardSwitchFrame == greenOnlyFrame
                    || hardSwitchFrame == redOnlyFrame,
                "With no blend distance the nearest probe must win outright.");

            // 混ぜ始める距離を球までの距離より大きく取ると、影響度が
            // どちらも1になり半々で混ざります。
            {
                auto* greenComponent =
                    greenProbe.GetComponent<
                        LamaPon::
                            ReflectionProbeComponent>();
                auto* redComponent =
                    redProbe.GetComponent<
                        LamaPon::
                            ReflectionProbeComponent>();
                Require(
                    greenComponent != nullptr
                        && redComponent != nullptr,
                    "Reflection probe components must exist.");
                greenComponent->SetBlendDistance(20.0f);
                redComponent->SetBlendDistance(20.0f);
            }
            Stage("frame-probe-blend-on");
            const auto blendedFrame =
                renderComposedFrame();
            DumpFrame(
                "probe-blend-on",
                blendedFrame);

            const auto blendedSkew =
                sphereSkew(blendedFrame);
            const auto middleSkew =
                (greenOnlySkew + redOnlySkew) / 2;
            const auto allowedOffset =
                (greenOnlySkew - redOnlySkew) / 3;
            std::cout
                << "probe blend skew(G-R): blended="
                << blendedSkew
                << " (middle=" << middleSkew
                << " allowed=+-" << allowedOffset << ")"
                << std::endl;
            // 半々で混ざるなら、単独2枚のちょうど中間付近へ来ます。
            // 混ざっていなければどちらかの端の値のままになるので、
            // 許容幅（差の1/3）を外れて落ちます。
            Require(
                std::llabs(blendedSkew - middleSkew)
                    < allowedOffset,
                "Blending two probes must land between the two single-probe tints.");

            static_cast<void>(
                scene.DestroyGameObject(redProbe));
            static_cast<void>(
                scene.DestroyGameObject(greenProbe));
            static_cast<void>(
                scene.DestroyGameObject(redWall));
            static_cast<void>(
                scene.DestroyGameObject(greenWall));
            static_cast<void>(
                scene.DestroyGameObject(mirrorSphere));
            cameraObject.GetTransform().position =
                savedCameraPosition;
            cameraObject.GetTransform().SetEulerAngles(
                savedCameraRotation);
            subject.GetTransform().position =
                savedSubjectPosition;
            clone.GetTransform().position =
                savedClonePosition;
        }

        // ⑪ SSR（画面空間反射）。
        //
        // 鏡面の床の上へ緑の箱を置いて、床に緑が映るかを見ます。
        // 箱は床より手前（カメラ寄り）に置くので画面に写っており、
        // SSRが映せる条件を満たします。
        //
        // 決め手は2つです。
        //   ・有効にすると、床の「箱の真下」だけが緑寄りになる。
        //     箱から離れた床は変わらない（＝画面全体を緑にする
        //     ような実装では通らない）
        //   ・切り戻すと元の絵とバイト単位で一致する。既定はオフ
        //     なので、既存のシーンが変わらないことの証明になります
        //
        // なお前フレームのカラーを読むため、有効化した直後の1枚は
        // まだ映りません。2枚目から比べます。
        {
            const auto savedCameraPosition =
                cameraObject.GetTransform().position;
            const auto savedCameraRotation =
                cameraObject.GetTransform().EulerAngles();
            const auto savedSubjectPosition =
                subject.GetTransform().position;
            const auto savedClonePosition =
                clone.GetTransform().position;

            subject.GetTransform().position =
                { 0.0f, 60.0f, 0.0f };
            clone.GetTransform().position =
                { 6.0f, 60.0f, 0.0f };
            cameraObject.GetTransform().position =
                { 0.0f, 2.2f, 7.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { -0.30f, 0.0f, 0.0f });

            // 鏡のように磨いた床。粗さを小さくしないとSSRは
            // 掛かりません（ぼやけた反射は1本のレイで表せないため）。
            auto& mirrorFloor =
                scene.CreateGameObject("ReflectiveFloor");
            mirrorFloor.GetTransform().position =
                { 0.0f, -1.0f, 0.0f };
            mirrorFloor.GetTransform().scale =
                { 30.0f, 0.3f, 30.0f };
            auto& floorRenderer =
                mirrorFloor.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Cube,
                    DirectX::XMFLOAT4{
                        0.35f, 0.35f, 0.38f, 1.0f },
                    std::filesystem::path{},
                    std::filesystem::path{},
                    0.03f,
                    1.0f);
            floorRenderer.SetMetallic(1.0f);
            floorRenderer.SetRoughness(0.03f);

            // 床の上に浮かぶ緑の箱。これが床へ映るはずです。
            auto& greenBox =
                scene.CreateGameObject("ReflectedBox");
            greenBox.GetTransform().position =
                { 0.0f, 0.6f, 0.0f };
            greenBox.GetTransform().scale =
                { 2.0f, 1.2f, 2.0f };
            greenBox.AddComponent<
                LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{
                    0.05f, 0.95f, 0.1f, 1.0f },
                std::filesystem::path{},
                std::filesystem::path{},
                1.0f,
                1.0f);

            // 床のうち、箱の真下あたり（映り込みが出る場所）と、
            // 箱から左へ大きく離れた場所（出ない場所）。
            constexpr std::uint32_t underMinimumX = 145;
            constexpr std::uint32_t underMaximumX = 175;
            constexpr std::uint32_t asideMinimumX = 20;
            constexpr std::uint32_t asideMaximumX = 50;
            constexpr std::uint32_t floorMinimumY = 132;
            constexpr std::uint32_t floorMaximumY = 150;
            const auto greenSkew =
                [&](const std::vector<std::uint8_t>& frame,
                    const std::uint32_t minimumX,
                    const std::uint32_t maximumX)
                {
                    long long skew = 0;
                    for (std::uint32_t y = floorMinimumY;
                        y < floorMaximumY;
                        ++y)
                    {
                        for (std::uint32_t x = minimumX;
                            x < maximumX;
                            ++x)
                        {
                            const auto pixel =
                                At(frame, x, y);
                            skew += pixel.green;
                            skew -= pixel.red;
                        }
                    }
                    return skew;
                };

            auto reflection =
                scene.ScreenSpaceReflection();
            reflection.enabled = false;
            scene.SetScreenSpaceReflectionSettings(
                reflection);
            Stage("frame-ssr-off");
            const auto ssrOffFrame =
                renderComposedFrame();
            DumpFrame("ssr-off", ssrOffFrame);

            reflection.enabled = true;
            reflection.intensity = 1.0f;
            reflection.maximumDistance = 12.0f;
            reflection.stepCount = 48;
            reflection.thickness = 0.6f;
            reflection.roughnessCutoff = 0.4f;
            scene.SetScreenSpaceReflectionSettings(
                reflection);
            // 1枚目は前フレームのカラーがまだ無いので捨てます。
            Stage("frame-ssr-warmup");
            static_cast<void>(renderComposedFrame());
            Stage("frame-ssr-on");
            const auto ssrOnFrame = renderComposedFrame();
            DumpFrame("ssr-on", ssrOnFrame);

            const auto underBefore = greenSkew(
                ssrOffFrame,
                underMinimumX,
                underMaximumX);
            const auto underAfter = greenSkew(
                ssrOnFrame,
                underMinimumX,
                underMaximumX);
            const auto asideBefore = greenSkew(
                ssrOffFrame,
                asideMinimumX,
                asideMaximumX);
            const auto asideAfter = greenSkew(
                ssrOnFrame,
                asideMinimumX,
                asideMaximumX);
            std::cout
                << "ssr green skew under box: "
                << underBefore << " -> " << underAfter
                << ", aside: "
                << asideBefore << " -> " << asideAfter
                << std::endl;
            // 箱の真下は緑寄りになること。
            //
            // 閾値は「1画素あたり」で決めます。合計値へ小さな定数を
            // 置くと、実質何も起きていなくても通ってしまうためです
            // （2026-08-05に閾値+200で空振りを見逃しかけました。
            // 30x18=540画素なので1画素あたり0.4未満でした）。
            constexpr long long bandPixels =
                static_cast<long long>(
                    underMaximumX - underMinimumX)
                * static_cast<long long>(
                    floorMaximumY - floorMinimumY);
            Require(
                underAfter - underBefore
                    > bandPixels * 4,
                "SSR must reflect the green box onto the mirror floor.");
            // 離れた床の変化は、真下の変化よりはっきり小さいこと。
            // 画面全体へ色を足すだけの実装では通りません。
            Require(
                (asideAfter - asideBefore) * 2
                    < underAfter - underBefore,
                "SSR must not tint floor that has nothing above it.");

            // 距離フェード。最大距離のところで反射が急に途切れると、
            // カメラが少し動くだけで現れたり消えたりします。
            //
            // 同じ当たりに対して**最大距離の設定だけ**を変えて測ります。
            // 幾何は一切動かさないので、差が出るなら距離フェードだけが
            // 原因です。フェードが無ければ、当たりが見つかる限りどの
            // 設定でも同じ値が返ります。
            //
            // 反復数は全距離で128（上限）に固定します。画面空間の
            // トラバーサルでは反復数は「歩幅」ではなく「予算」なので、
            // 十分に与えれば当たり位置は最大距離に依らず同一になり、
            // 変わるのはフェードだけになります（ワールド等間隔で
            // 歩いていた頃は、距離に比例させないと刻み幅が変わって
            // しまうため密度を固定していました）。
            {
                // 測る場所は狭い帯にします。広い帯だと画素ごとに
                // レイの長さが違うため、最大距離を縮めていくと画素が
                // 順番に脱落し、**per-pixelのフェードが無くても**
                // 合計は緩やかに落ちます。それでは検証になりません。
                constexpr std::uint32_t narrowMinimumX = 157;
                constexpr std::uint32_t narrowMaximumX = 165;
                constexpr std::uint32_t narrowMinimumY = 137;
                constexpr std::uint32_t narrowMaximumY = 142;
                const auto narrowSkew =
                    [&](const std::vector<std::uint8_t>& frame)
                {
                    long long skew = 0;
                    for (std::uint32_t y = narrowMinimumY;
                        y < narrowMaximumY;
                        ++y)
                    {
                        for (std::uint32_t x = narrowMinimumX;
                            x < narrowMaximumX;
                            ++x)
                        {
                            const auto pixel = At(frame, x, y);
                            skew += pixel.green;
                            skew -= pixel.red;
                        }
                    }
                    return skew;
                };
                const long long narrowBefore =
                    narrowSkew(ssrOffFrame);

                const auto measureAtDistance =
                    [&](const float maximumDistance)
                {
                    auto sweep = reflection;
                    sweep.enabled = true;
                    sweep.maximumDistance = maximumDistance;
                    sweep.stepCount = 128;
                    scene.SetScreenSpaceReflectionSettings(
                        sweep);
                    // 前フレームのカラーを読むので1枚捨てます。
                    static_cast<void>(renderComposedFrame());
                    const auto frame = renderComposedFrame();
                    return narrowSkew(frame) - narrowBefore;
                };

                Stage("frame-ssr-distance-fade");
                // 診断: 12mと1.8mの絵を残します（数字が崩れたとき、
                // 「当たっていない」のか「変な所に当たっている」のかを
                // 絵で切り分けるため）。
                {
                    auto diagnostic = reflection;
                    diagnostic.enabled = true;
                    diagnostic.maximumDistance = 12.0f;
                    diagnostic.stepCount = 128;
                    scene.SetScreenSpaceReflectionSettings(
                        diagnostic);
                    static_cast<void>(renderComposedFrame());
                    DumpFrame(
                        "ssr-fade-12m",
                        renderComposedFrame());
                    diagnostic.maximumDistance = 1.8f;
                    scene.SetScreenSpaceReflectionSettings(
                        diagnostic);
                    static_cast<void>(renderComposedFrame());
                    DumpFrame(
                        "ssr-fade-1p8m",
                        renderComposedFrame());
                }
                // この帯のレイの長さは約1.35mなので、フェードが効く
                // 範囲（最後の1/4＝最大距離が1.35〜1.8mのとき）を
                // またぐように選んでいます。
                const long long fullRange =
                    measureAtDistance(12.0f);
                const long long highRange =
                    measureAtDistance(1.8f);
                const long long midRange =
                    measureAtDistance(1.6f);
                const long long lowRange =
                    measureAtDistance(1.45f);
                const long long cutRange =
                    measureAtDistance(1.3f);
                std::cout
                    << "ssr distance fade: 12m=" << fullRange
                    << " 1.8m=" << highRange
                    << " 1.6m=" << midRange
                    << " 1.45m=" << lowRange
                    << " 1.3m=" << cutRange
                    << std::endl;

                // 実測: 2474 / 2307 / 1402 / 402 / 0
                //      （全強度比 100% / 93% / 57% / 16% / 0%）
                //
                // **中間の値が並ぶことが決め手です。** フェードが無い
                // 実装（当たれば全強度、届かなければ0）だと、レイが
                // 届く1.45mまでは2474のままで、1.3mで一気に0へ落ちる
                // 崖になります。93%・57%・16%はどれもその形では
                // 出てきません。
                Require(
                    fullRange > 500,
                    "The narrow band must show a reflection to"
                    " measure the fade against.");
                // 最大距離を縮めるほど単調に弱くなること。
                Require(
                    fullRange >= highRange
                        && highRange > midRange
                        && midRange > lowRange
                        && lowRange > cutRange,
                    "SSR must fade monotonically as the ray"
                    " approaches the maximum distance.");
                // 途中が「全強度でも0でもない」こと。崖ではなく
                // フェードであることの証明です。
                Require(
                    midRange * 4 > fullRange
                        && midRange * 4 < fullRange * 3,
                    "The mid-range reflection must be partially"
                    " faded, not full or gone.");
                Require(
                    lowRange > 0
                        && lowRange * 3 < fullRange,
                    "The near-cutoff reflection must still be"
                    " present but weak.");
                Require(
                    cutRange == 0,
                    "Beyond the maximum distance the reflection"
                    " must be gone.");

                // 反復の上限を変えたときの様子。
                //
                // Hi-Zでは、予算が足りてさえいれば当たり位置は
                // 予算に依りません（トラバーサルは決定的で、余った
                // 反復は使われないだけ）。実測で48以上は完全に同じ
                // 値になります（DDA時代は128=1795 / 64=1785 /
                // 48=1556 と頭打ちにならず、この性質は主張できません
                // でした）。下の判定はこの飽和を固定します。
                // 32以下は床を這う区間で予算が尽きて0になります
                // （これは診断のみ。値は判定しません）。
                const auto measureAtSteps =
                    [&](const std::uint32_t steps)
                {
                    auto sweep = reflection;
                    sweep.enabled = true;
                    sweep.maximumDistance = 12.0f;
                    sweep.stepCount = steps;
                    scene.SetScreenSpaceReflectionSettings(
                        sweep);
                    static_cast<void>(renderComposedFrame());
                    const auto frame = renderComposedFrame();
                    return narrowSkew(frame) - narrowBefore;
                };
                std::cout << "ssr step budget:";
                for (const std::uint32_t steps : {
                    128u, 96u, 64u, 48u, 32u, 24u, 16u, 8u })
                {
                    std::cout
                        << " " << steps << "="
                        << measureAtSteps(steps);
                }
                std::cout << std::endl;
                {
                    const long long generous =
                        measureAtSteps(128u);
                    const long long saturated =
                        measureAtSteps(48u);
                    Require(
                        saturated == generous
                            && generous > 0,
                        "Above the saturation budget the"
                        " Hi-Z traversal must return the"
                        " exact same reflection.");
                }

                // 「物の厚み」が実際に効いていること。
                //
                // 深度→距離の式が壊れていた間、差の大きさが桁違いに
                // 小さくなるせいで厚みの判定が常に成立し、この設定は
                // **何も変えませんでした**。効くようになったことを
                // 押さえておきます（薄くすれば当たりが減ります）。
                const auto measureAtThickness =
                    [&](const float value)
                {
                    auto sweep = reflection;
                    sweep.enabled = true;
                    sweep.maximumDistance = 12.0f;
                    sweep.thickness = value;
                    scene.SetScreenSpaceReflectionSettings(
                        sweep);
                    static_cast<void>(renderComposedFrame());
                    const auto frame = renderComposedFrame();
                    return narrowSkew(frame) - narrowBefore;
                };
                const long long thickHit =
                    measureAtThickness(6.0f);
                const long long thinHit =
                    measureAtThickness(0.02f);
                std::cout
                    << "ssr thickness: 6.0=" << thickHit
                    << " 0.02=" << thinHit
                    << std::endl;
                // 実測: 6.0=2137 / 0.02=0。厚みを薄くすると当たりが
                // 完全に無くなります（壊れていた間はどちらも同じ値で、
                // この設定は何も変えませんでした）。
                Require(
                    thickHit > 0 && thinHit == 0,
                    "The thickness setting must affect which"
                    " surfaces the ray hits.");

                // 厚みの既定値は「鏡像と見比べて」決めます。
                //
                // 厚みには両側に失敗があります。薄すぎるとレイが
                // 物の側面から入った歩を捨てるので反射が途中で切れ、
                // 厚すぎると物の裏まで通り抜けたレイを当たり扱いに
                // するので、映るべきでない床まで映り込みます。
                // どちらに寄せるかは「正解の絵」が無いと決められ
                // ません。
                //
                // そこで、床の面で折り返した位置へ同じ箱をもう1つ
                // 置き、床をどけて**同じカメラで**描きます。立方体は
                // 上下対称なので、折り返した形はそのまま立方体です。
                // それが写った行が、そのまま「反射が出るべき行」に
                // なります。
                //
                // カメラを面で反転させるやり方は採りません。
                // 2026-08-06に試したところ、反転したカメラからは
                // 箱が1画素も写らず（切り分けに時間を取られました）、
                // しかも比べる相手のカメラが変わってしまうので、
                // ずれたときに「反射がおかしい」のか「参照が
                // おかしい」のか分かりません。同じカメラのままに
                // できるこちらが確実です。
                //
                // 見方は床の縦1列を**行ごと**に。合計値だけだと反射が
                // どこまで伸びたかが分からず、既定値の判断はまさに
                // そこだからです。箱そのものの下端はy=100なので、床
                // だけを見るためにその下から始めます（入れると箱の緑が
                // 参照にも反射にも同じだけ乗り、一致率が水増しされます）。
                constexpr std::uint32_t profileMinimumY = 102;
                constexpr std::uint32_t profileMaximumY = 180;
                const auto rowGreen =
                    [&](const std::vector<std::uint8_t>& frame,
                        const std::uint32_t y)
                {
                    long long skew = 0;
                    for (std::uint32_t x = underMinimumX;
                        x < underMaximumX;
                        ++x)
                    {
                        const auto pixel = At(frame, x, y);
                        skew += pixel.green;
                        skew -= pixel.red;
                    }
                    return skew
                        / static_cast<long long>(
                            underMaximumX - underMinimumX);
                };
                // 鏡像が出ている行のうち、反射が届いている行の数。
                // しきい値が2つあるのは、鏡像は箱そのもの（明るい）で、
                // 反射は床の反射率を通した後（暗い）だからです。同じ
                // 値では比べられません。
                const auto countRows =
                    [&](const std::vector<std::uint8_t>& mirror,
                        const std::vector<std::uint8_t>* reflected,
                        std::uint32_t& firstRow)
                {
                    std::uint32_t rows = 0;
                    firstRow = 0;
                    for (std::uint32_t y = profileMinimumY;
                        y < profileMaximumY;
                        ++y)
                    {
                        if (rowGreen(mirror, y) <= 30)
                        {
                            continue;
                        }
                        if (reflected != nullptr
                            && rowGreen(*reflected, y) <= 20)
                        {
                            continue;
                        }
                        if (rows == 0)
                        {
                            firstRow = y;
                        }
                        ++rows;
                    }
                    return rows;
                };
                const auto printRowProfile =
                    [&](const char* label,
                        const std::vector<std::uint8_t>& frame)
                {
                    std::cout
                        << "ssr row profile " << label
                        << " from y=" << profileMinimumY << ":";
                    for (std::uint32_t y = profileMinimumY;
                        y < profileMaximumY;
                        ++y)
                    {
                        std::cout << ' ' << rowGreen(frame, y);
                    }
                    std::cout << std::endl;
                };

                // 鏡像そのものを置いた「正解の絵」。
                Stage("frame-ssr-mirror-reference");
                std::vector<std::uint8_t> referenceFrame;
                {
                    auto disabled = reflection;
                    disabled.enabled = false;
                    scene.SetScreenSpaceReflectionSettings(
                        disabled);
                    const auto savedFloorPosition =
                        mirrorFloor.GetTransform().position;
                    // 床の上面。立方体プリミティブは1辺1なので
                    // スケールの半分だけ持ち上がります。
                    const float mirrorPlaneY =
                        savedFloorPosition.y
                        + mirrorFloor.GetTransform().scale.y
                            * 0.5f;
                    // 鏡像の前に立つ物は2つとも退けます。
                    //   ・鏡の床そのもの
                    //   ・④で置いた影用の地面（y=-1.8、上面が-1.7）
                    // 地面のほうを忘れると、**上面が鏡像の上面と
                    // ぴったり同一平面**（どちらも-1.7）になり、
                    // Zファイティングで鏡像が半分しか出ません。
                    // 2026-08-06にこれで「鏡像は26行」と読み違え、
                    // 既定値を1つ小さく決めてしまいました。
                    const auto savedGroundPosition =
                        ground.GetTransform().position;
                    mirrorFloor.GetTransform().position =
                        { 0.0f, -10000.0f, 0.0f };
                    ground.GetTransform().position =
                        { 0.0f, -10000.0f, 0.0f };
                    const auto boxPosition =
                        greenBox.GetTransform().position;
                    const auto boxScale =
                        greenBox.GetTransform().scale;
                    auto& mirroredBox =
                        scene.CreateGameObject("MirroredBox");
                    mirroredBox.GetTransform().position = {
                        boxPosition.x,
                        2.0f * mirrorPlaneY - boxPosition.y,
                        boxPosition.z
                    };
                    mirroredBox.GetTransform().scale = boxScale;
                    mirroredBox.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Cube,
                        DirectX::XMFLOAT4{
                            0.05f, 0.95f, 0.1f, 1.0f },
                        std::filesystem::path{},
                        std::filesystem::path{},
                        1.0f,
                        1.0f);
                    referenceFrame = renderComposedFrame();
                    DumpFrame(
                        "ssr-mirror-reference",
                        referenceFrame);
                    static_cast<void>(
                        scene.DestroyGameObject(mirroredBox));
                    mirrorFloor.GetTransform().position =
                        savedFloorPosition;
                    ground.GetTransform().position =
                        savedGroundPosition;
                }

                // 既定値を選ぶための掃引（--dump のときだけ）。
                // 絵はPPMで残すので、数字だけでなく切れ方・漏れ方の
                // 形も見られます。
                if (!g_dumpDirectory.empty())
                {
                    printRowProfile(
                        "mirror-reference",
                        referenceFrame);
                    printRowProfile("ssr-off", ssrOffFrame);

                    // ファイル名は固定の文字列にします。値から作ると
                    // <sstream>を足すことになり、そのためだけに
                    // インクルードを増やしたくないためです。
                    struct ThicknessSample final
                    {
                        const char* name;
                        float value;
                    };
                    static constexpr ThicknessSample samples[] = {
                        { "ssr-thickness-005", 0.05f },
                        { "ssr-thickness-010", 0.10f },
                        { "ssr-thickness-020", 0.20f },
                        { "ssr-thickness-040", 0.40f },
                        { "ssr-thickness-080", 0.80f },
                        { "ssr-thickness-120", 1.20f },
                        { "ssr-thickness-160", 1.60f },
                        { "ssr-thickness-240", 2.40f },
                        { "ssr-thickness-320", 3.20f },
                        { "ssr-thickness-640", 6.40f },
                    };
                    Stage("frame-ssr-thickness-sweep");
                    for (const auto& sample : samples)
                    {
                        auto sweep = reflection;
                        sweep.enabled = true;
                        sweep.maximumDistance = 12.0f;
                        sweep.thickness = sample.value;
                        scene.SetScreenSpaceReflectionSettings(
                            sweep);
                        static_cast<void>(
                            renderComposedFrame());
                        const auto frame =
                            renderComposedFrame();
                        DumpFrame(sample.name, frame);
                        std::cout
                            << "ssr thickness sweep "
                            << sample.value
                            << ": under="
                            << greenSkew(
                                frame,
                                underMinimumX,
                                underMaximumX)
                                - underBefore
                            << " aside="
                            << greenSkew(
                                frame,
                                asideMinimumX,
                                asideMaximumX)
                                - asideBefore
                            << " narrow="
                            << narrowSkew(frame)
                                - narrowBefore;
                        std::uint32_t sweepFirstRow = 0;
                        std::cout
                            << " mirrorRowsCovered="
                            << countRows(
                                referenceFrame,
                                &frame,
                                sweepFirstRow)
                            << " from y=" << sweepFirstRow
                            << std::endl;
                        printRowProfile(sample.name, frame);
                    }
                }

                // 既定の厚みで、鏡像の行のどれだけを埋められるか。
                //
                // しきい値が2つあるのは、鏡像は箱そのもの（明るい）で、
                // 反射は床の反射率を通した後（暗い）だからです。同じ
                // 値では比べられません。
                {
                    Stage("frame-ssr-default-thickness");
                    auto standard = reflection;
                    standard.enabled = true;
                    standard.maximumDistance = 12.0f;
                    standard.thickness =
                        LamaPon::ScreenSpaceReflectionSettings{}
                            .thickness;
                    scene.SetScreenSpaceReflectionSettings(
                        standard);
                    static_cast<void>(renderComposedFrame());
                    const auto defaultFrame =
                        renderComposedFrame();
                    DumpFrame("ssr-default-thickness", defaultFrame);
                    std::uint32_t firstReferenceRow = 0;
                    std::uint32_t firstCoveredRow = 0;
                    const std::uint32_t referenceRows = countRows(
                        referenceFrame,
                        nullptr,
                        firstReferenceRow);
                    const std::uint32_t coveredRows = countRows(
                        referenceFrame,
                        &defaultFrame,
                        firstCoveredRow);
                    std::cout
                        << "ssr default thickness "
                        << standard.thickness
                        << ": mirror rows " << referenceRows
                        << " from y=" << firstReferenceRow
                        << ", covered " << coveredRows
                        << " from y=" << firstCoveredRow
                        << std::endl;
                    // 実測: 鏡像はy124から画面下端まで56行。既定1.2で
                    // **56行＝100%**を埋めます。掃引では0.4=39行(70%)、
                    // 0.8=48行(86%)、1.0=52行(93%)、1.2で満杯になり、
                    // 1.6や2.4へ上げても増えません（3.2まで行くと
                    // 鏡像の外へ漏れ始め、真下の帯が22821→30035へ
                    // 増えます）。
                    //
                    // しきい値は9割。1.2は5行ぶん余裕があり、0.8以下は
                    // 落ちます。
                    Require(
                        referenceRows >= 40,
                        "The mirrored box must be visible to"
                        " measure the reflection against.");
                    Require(
                        coveredRows * 10 >= referenceRows * 9,
                        "The default thickness must cover the rows"
                        " where the mirror image is.");
                }
            }

            // 切り戻すと元の絵と一致すること。
            reflection.enabled = false;
            scene.SetScreenSpaceReflectionSettings(
                reflection);
            Stage("frame-ssr-off-again");
            const auto ssrRestoredFrame =
                renderComposedFrame();
            Require(
                ssrRestoredFrame == ssrOffFrame,
                "Disabling SSR must restore the exact original frame.");

            static_cast<void>(
                scene.DestroyGameObject(greenBox));
            static_cast<void>(
                scene.DestroyGameObject(mirrorFloor));
            cameraObject.GetTransform().position =
                savedCameraPosition;
            cameraObject.GetTransform().SetEulerAngles(
                savedCameraRotation);
            subject.GetTransform().position =
                savedSubjectPosition;
            clone.GetTransform().position =
                savedClonePosition;
            scene.SetScreenSpaceReflectionSettings(
                LamaPon::ScreenSpaceReflectionSettings{});
        }

        // ⑫ TAA（時間的アンチエイリアス）。
        //
        // 斜めに傾けた板を1枚置いて、その輪郭を見ます。TAAは毎フレーム
        // サブピクセルだけずらして描いて混ぜるので、輪郭に「前景でも
        // 背景でもない中間色」の画素が増えるはずです。これがギザギザが
        // 減ったことの直接的な証拠になります（見た目の判断が要らない）。
        //
        // 性能はWARPのVMでは測れませんが、画質はこうして数値で出せます。
        {
            const auto savedCameraPosition =
                cameraObject.GetTransform().position;
            const auto savedCameraRotation =
                cameraObject.GetTransform().EulerAngles();
            const auto savedSubjectPosition =
                subject.GetTransform().position;
            const auto savedClonePosition =
                clone.GetTransform().position;

            clone.GetTransform().position =
                { 6.0f, 60.0f, 0.0f };
            cameraObject.GetTransform().position =
                { 0.0f, 0.0f, 6.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { 0.0f, 0.0f, 0.0f });
            // 斜めの輪郭を作るため、被写体を軸から外して回します。
            subject.GetTransform().position =
                { 0.0f, 0.0f, 0.0f };
            subject.GetTransform().SetEulerAngles(
                { 0.0f, 0.0f, 0.37f });

            // 輪郭の「段差」を測ります。
            //
            // 中間色の画素を数える方法は使えません。キューブ面の
            // 陰影まで中間色として拾ってしまい、2万画素中5500以上が
            // 常に該当して輪郭の変化が埋もれます（2026-08-05に
            // 5535→5569しか動かず判定に使えませんでした）。
            //
            // 代わりに横方向の隣接差の最大値を行ごとに取り、その
            // 平均を見ます。ギザギザした輪郭は1画素で全コントラスト
            // 跳ぶので段差が大きく、均されると小さくなります。
            // つまりTAAを有効にすると**下がる**のが正しい向きです。
            const auto edgeStepAverage =
                [&](const std::vector<std::uint8_t>& frame)
                {
                    long long total = 0;
                    int rows = 0;
                    for (std::uint32_t y = 40;
                        y < 140;
                        ++y)
                    {
                        int rowMaximum = 0;
                        for (std::uint32_t x = 60;
                            x + 1 < 260;
                            ++x)
                        {
                            const auto left =
                                At(frame, x, y);
                            const auto right =
                                At(frame, x + 1, y);
                            const int step = std::max(
                                std::abs(
                                    left.red - right.red),
                                std::max(
                                    std::abs(
                                        left.green
                                        - right.green),
                                    std::abs(
                                        left.blue
                                        - right.blue)));
                            rowMaximum = std::max(
                                rowMaximum,
                                step);
                        }
                        total += rowMaximum;
                        ++rows;
                    }
                    return static_cast<double>(total)
                        / static_cast<double>(
                            std::max(rows, 1));
                };

            auto temporal =
                scene.TemporalAntiAliasing();
            temporal.enabled = false;
            scene.SetTemporalAntiAliasingSettings(
                temporal);
            Stage("frame-taa-off");
            const auto taaOffFrame = renderComposedFrame();
            DumpFrame("taa-off", taaOffFrame);

            temporal.enabled = true;
            temporal.historyWeight = 0.9f;
            temporal.jitterScale = 1.0f;
            temporal.clampTolerance = 2.0f;
            scene.SetTemporalAntiAliasingSettings(
                temporal);
            // 積算されるまで何枚か回します（1枚目は履歴が無い）。
            Stage("frame-taa-accumulate");
            std::vector<std::uint8_t> taaOnFrame;
            for (int warmup = 0; warmup < 12; ++warmup)
            {
                taaOnFrame = renderComposedFrame();
            }
            DumpFrame("taa-on", taaOnFrame);
            // 収束の確認用にもう1枚。
            const auto taaSettledFrame =
                renderComposedFrame();
            DumpFrame("taa-settled", taaSettledFrame);

            // 「段差が大きい行の数」も数えます。硬い輪郭は1画素で
            // 全コントラスト跳ぶので、しきい値を超える行が多く
            // なります。均されると跳びが分割されて減ります。
            // 平均より鋭敏なので、判定にはこちらを使います。
            const auto hardRowCount =
                [&](const std::vector<std::uint8_t>& frame,
                    const int threshold)
                {
                    int rows = 0;
                    for (std::uint32_t y = 40;
                        y < 140;
                        ++y)
                    {
                        int rowMaximum = 0;
                        for (std::uint32_t x = 60;
                            x + 1 < 260;
                            ++x)
                        {
                            const auto left =
                                At(frame, x, y);
                            const auto right =
                                At(frame, x + 1, y);
                            const int step = std::max(
                                std::abs(
                                    left.red - right.red),
                                std::max(
                                    std::abs(
                                        left.green
                                        - right.green),
                                    std::abs(
                                        left.blue
                                        - right.blue)));
                            rowMaximum = std::max(
                                rowMaximum,
                                step);
                        }
                        if (rowMaximum > threshold)
                        {
                            ++rows;
                        }
                    }
                    return rows;
                };

            const auto stepBefore =
                edgeStepAverage(taaOffFrame);
            const auto stepAfter =
                edgeStepAverage(taaOnFrame);
            std::cout
                << "taa edge step average: "
                << stepBefore << " -> " << stepAfter
                << std::endl;
            std::cout
                << "taa hard rows(>200): "
                << hardRowCount(taaOffFrame, 200)
                << " -> " << hardRowCount(taaOnFrame, 200)
                << ", (>150): "
                << hardRowCount(taaOffFrame, 150)
                << " -> " << hardRowCount(taaOnFrame, 150)
                << ", (>100): "
                << hardRowCount(taaOffFrame, 100)
                << " -> " << hardRowCount(taaOnFrame, 100)
                << std::endl;
            // 段差が小さくなること＝ギザギザが均されたこと。
            // 実測は15%減（131.2→111.3）。12%を要求します。
            //
            // この12%は「空側を混ぜ忘れた版で落ちる」値です。
            // シェーダーで深度1の画素を早期returnしていた頃は
            // 8%減（120.8）しか出ませんでした。輪郭の空側にAAが
            // かからない状態なので、通してはいけません。
            Require(
                stepAfter < stepBefore * 0.88,
                "TAA must reduce the hard step across the diagonal edge.");
            // こちらの方が鋭敏です。実測は80→13（84%減）なので、
            // 半減を要求します（空側を混ぜ忘れた版は56で落ちます）。
            const auto hardBefore =
                hardRowCount(taaOffFrame, 150);
            const auto hardAfter =
                hardRowCount(taaOnFrame, 150);
            Require(
                hardBefore > 0
                    && hardAfter * 2 < hardBefore,
                "TAA must cut the number of rows that still jump in one pixel.");

            // 静止シーンでは収束して、隣り合うフレームがほぼ同じ絵に
            // なること。ここが暴れると画面がちらつきます。
            long long settleDifference = 0;
            int settleMaximum = 0;
            for (std::uint32_t y = 40; y < 140; ++y)
            {
                for (std::uint32_t x = 60; x < 260; ++x)
                {
                    const auto left =
                        At(taaOnFrame, x, y);
                    const auto right =
                        At(taaSettledFrame, x, y);
                    const int difference = std::max(
                        std::abs(left.red - right.red),
                        std::max(
                            std::abs(
                                left.green - right.green),
                            std::abs(
                                left.blue - right.blue)));
                    settleDifference += difference;
                    settleMaximum = std::max(
                        settleMaximum,
                        difference);
                }
            }
            constexpr long long settlePixels = 200LL * 100LL;
            const double settleAverage =
                static_cast<double>(settleDifference)
                / static_cast<double>(settlePixels);
            std::cout
                << "taa settle: max=" << settleMaximum
                << " average=" << settleAverage
                << std::endl;
            // 判定は平均で行います。輪郭の1画素はジッターの位置に
            // よって最後まで多少振れるので、最大値で締めると
            // 正しく動いていても落ちます。平均が十分小さければ
            // 「絵として静止している」と言えます。
            //
            // 閾値0.3は「直す前のコードで落ちる」値です。再投影に
            // ずらし込みの行列を使っていた版は1.12、空側を混ぜ忘れて
            // いた版は0.32まで悪化していました（正しい版は0.15）。
            // ここを緩くすると、同じ間違いを入れても気付けません。
            Require(
                settleAverage < 0.3,
                "TAA must converge on a static scene instead of flickering.");

            // 切り戻すと元の絵とバイト単位で一致すること。
            temporal.enabled = false;
            scene.SetTemporalAntiAliasingSettings(
                temporal);
            Stage("frame-taa-off-again");
            const auto taaRestoredFrame =
                renderComposedFrame();
            Require(
                taaRestoredFrame == taaOffFrame,
                "Disabling TAA must restore the exact original frame.");

            cameraObject.GetTransform().position =
                savedCameraPosition;
            cameraObject.GetTransform().SetEulerAngles(
                savedCameraRotation);
            subject.GetTransform().position =
                savedSubjectPosition;
            subject.GetTransform().SetEulerAngles(
                { 0.0f, 0.0f, 0.0f });
            clone.GetTransform().position =
                savedClonePosition;
            scene.SetTemporalAntiAliasingSettings(
                LamaPon::TemporalAntiAliasingSettings{});
        }

        // ⑦ SSAO（--dump のときだけ）。
        // 陰りの見た目は数値で判定しにくいので、ここではアサーション
        // をせず、有効・無効の2枚を書き出して目視で比べられるように
        // します。キューブが床に接しているので、接地部分に陰りが
        // 出ているか、ザラつきや輪郭の白い縁が無いかを確認できます。
        if (!g_dumpDirectory.empty())
        {
            auto aoGraphics = graphics.Settings();
            aoGraphics.ambientOcclusionEnabled = true;
            graphics.SetGraphicsSettings(aoGraphics);

            auto occlusion = scene.AmbientOcclusion();
            occlusion.enabled = false;
            scene.SetAmbientOcclusionSettings(occlusion);
            Stage("frame-ssao-off");
            DumpFrame("ssao-off", renderComposedFrame());

            occlusion.enabled = true;
            occlusion.radius = 0.5f;
            occlusion.strength = 0.6f;
            scene.SetAmbientOcclusionSettings(occlusion);
            Stage("frame-ssao-on");
            DumpFrame("ssao-on", renderComposedFrame());

            // 強めにかけた版も出します（効いているかの判定用）。
            occlusion.strength = 1.0f;
            scene.SetAmbientOcclusionSettings(occlusion);
            Stage("frame-ssao-strong");
            DumpFrame("ssao-strong", renderComposedFrame());

            // 半径を広げた版。既定の0.5がシーンの寸法に対して
            // 小さすぎないかを見るための比較用です。
            occlusion.radius = 2.0f;
            scene.SetAmbientOcclusionSettings(occlusion);
            Stage("frame-ssao-wide");
            DumpFrame("ssao-wide", renderComposedFrame());

            // 接地部の陰りを見るための専用の段。
            //
            // 既定のシーンはキューブの底が-1.0、床の上面が-1.7で
            // 0.7だけ浮いているため、SSAOがほとんど効きません
            // （2026-08-04に上のoff/onを比べたところ、差は最大
            // 1/255・全体の1%以下しか出ず、判定に使えませんでした）。
            // キューブを床へ置き、カメラを上げて見下ろします。
            //
            // 影を出したまま比べるのが要点です。SSAOを完成した色へ
            // 掛けていた頃は影の中がさらに暗くなっていたので、
            // 「接地部には陰りが出て、影の中は変わらない」ことを
            // この2枚で確認します。
            const auto savedSubjectPosition =
                subject.GetTransform().position;
            const auto savedCameraPosition =
                cameraObject.GetTransform().position;
            const auto savedCameraRotation =
                cameraObject.GetTransform().EulerAngles();

            subject.GetTransform().position =
                { 0.0f, -0.7f, 0.0f };
            cameraObject.GetTransform().position =
                { 0.0f, 2.4f, 6.0f };
            cameraObject.GetTransform().SetEulerAngles({ -0.42f, 0.0f, 0.0f });

            occlusion.enabled = false;
            scene.SetAmbientOcclusionSettings(occlusion);
            Stage("frame-ssao-contact-off");
            DumpFrame(
                "ssao-contact-off",
                renderComposedFrame());

            occlusion.enabled = true;
            occlusion.radius = 0.75f;
            occlusion.strength = 1.0f;
            scene.SetAmbientOcclusionSettings(occlusion);
            Stage("frame-ssao-contact-on");
            DumpFrame(
                "ssao-contact-on",
                renderComposedFrame());

            // SSAOが環境光項だけへ掛かっていることの決定的な確認。
            // 環境光を0にすると掛ける先が無くなるので、SSAOの有無で
            // 絵が1ビットも変わらないはずです。完成した色へ掛けて
            // いた頃は、環境光が0でも全体が暗くなっていました。
            const auto savedAmbientIntensity =
                scene.AmbientLightIntensity();
            scene.SetAmbientLightIntensity(0.0f);

            occlusion.enabled = false;
            scene.SetAmbientOcclusionSettings(occlusion);
            Stage("frame-ssao-noambient-off");
            DumpFrame(
                "ssao-noambient-off",
                renderComposedFrame());

            occlusion.enabled = true;
            scene.SetAmbientOcclusionSettings(occlusion);
            Stage("frame-ssao-noambient-on");
            DumpFrame(
                "ssao-noambient-on",
                renderComposedFrame());

            scene.SetAmbientLightIntensity(
                savedAmbientIntensity);

            subject.GetTransform().position =
                savedSubjectPosition;
            cameraObject.GetTransform().position =
                savedCameraPosition;
            cameraObject.GetTransform().SetEulerAngles(savedCameraRotation);

            // クラスタライトカリング（Forward+）の確認。ポイント
            // ライトを24灯並べます。従来経路は品質設定の上限
            // （High=12灯）で打ち切られるので、右半分のライトが
            // 消えます。クラスタ経路が動いていれば24灯全部の
            // 光だまりが床に並びます。
            {
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 4.5f, 9.0f };
                cameraObject.GetTransform().SetEulerAngles({ -0.42f, 0.0f, 0.0f });

                std::vector<LamaPon::GameObject*>
                    clusterLightObjects;
                for (int lightIndex = 0;
                    lightIndex < 24;
                    ++lightIndex)
                {
                    auto& lightObject =
                        scene.CreateGameObject(
                            "ClusterLight"
                            + std::to_string(lightIndex));
                    lightObject.GetTransform().position = {
                        -11.5f
                            + static_cast<float>(
                                lightIndex),
                        -1.1f,
                        0.0f
                    };
                    lightObject.AddComponent<
                        LamaPon::PointLightComponent>(
                        DirectX::XMFLOAT3{
                            lightIndex % 3 == 0
                                ? 1.0f : 0.15f,
                            lightIndex % 3 == 1
                                ? 1.0f : 0.15f,
                            lightIndex % 3 == 2
                                ? 1.0f : 0.15f },
                        6.0f,
                        1.7f);
                    clusterLightObjects.push_back(
                        &lightObject);
                }

                Stage("frame-clustered-lights");
                const auto clusteredFrame =
                    renderComposedFrame();
                DumpFrame(
                    "clustered-lights",
                    clusteredFrame);

                // 描画方式の切り替えが本当に効いているかを絵で
                // 確かめます。24灯はForward+でしか全部点きません。
                // Forwardでは品質設定の上限で打ち切られるので、床の
                // 光だまりが減ります。
                //
                // 見るのは「変わった画素の数」です。画面全体の合計
                // 輝度で測ろうとして一度失敗しました——環境光と
                // 平行光が支配的なので、24灯が8灯へ減っても合計は
                // 0.9%しか動かず、正しく動いているのに判定が落ちます。
                // 一番ありがちな壊れ方は「切り替えても1ビットも
                // 変わらない」なので、数える方が素直で厳しい。
                auto forwardSettings = graphics.Settings();
                forwardSettings.renderingPath =
                    LamaPon::RenderingPath::Forward;
                graphics.SetGraphicsSettings(
                    forwardSettings);
                Stage("frame-forward-path");
                const auto forwardFrame =
                    renderComposedFrame();
                DumpFrame("forward-path", forwardFrame);
                forwardSettings.renderingPath =
                    LamaPon::RenderingPath::ForwardPlus;
                graphics.SetGraphicsSettings(
                    forwardSettings);

                std::size_t pathChangedPixels = 0;
                int pathMaximumDelta = 0;
                std::size_t pathRightHalfChanged = 0;
                for (std::uint32_t y = 0; y < Height; ++y)
                {
                    for (std::uint32_t x = 0;
                        x < Width;
                        ++x)
                    {
                        const auto plus =
                            At(clusteredFrame, x, y);
                        const auto plain =
                            At(forwardFrame, x, y);
                        const int delta = std::max(
                            {
                                std::abs(
                                    plus.red - plain.red),
                                std::abs(
                                    plus.green
                                    - plain.green),
                                std::abs(
                                    plus.blue - plain.blue)
                            });
                        pathMaximumDelta = std::max(
                            pathMaximumDelta,
                            delta);
                        // 8はディザ等の誤差を落とすためのしきい値。
                        // 実測の最大差は84なので十分下です。
                        if (delta > 8)
                        {
                            ++pathChangedPixels;
                            if (x >= Width / 2)
                            {
                                ++pathRightHalfChanged;
                            }
                        }
                    }
                }
                std::cout
                    << "rendering path changed pixels: "
                    << pathChangedPixels
                    << " (right half "
                    << pathRightHalfChanged
                    << ", max delta "
                    << pathMaximumDelta
                    << ", point light limit "
                    << graphics.Settings().pointLightLimit
                    << " of 24 lights)"
                    << std::endl;

                // 上限が24灯以上だと打ち切りが起きず、この段が
                // 何も確かめない段になってしまいます。
                Require(
                    graphics.Settings().pointLightLimit
                        < 24,
                    "The point light limit must cut the"
                    " 24 lights for this stage to mean"
                    " anything.");
                // 実測5243画素。5倍の余裕を見て1000にします。
                // 配線が外れれば0になるので、空振りはしません。
                Require(
                    pathChangedPixels > 1000,
                    "Switching the rendering path must"
                    " change the picture when the scene"
                    " has more lights than the limit.");

                for (auto* lightObject :
                    clusterLightObjects)
                {
                    static_cast<void>(
                        scene.DestroyGameObject(
                            *lightObject));
                }
                subject.GetTransform().position =
                    savedSubjectPosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(savedCameraRotation);
            }

            // 半透明の前後ソートの確認。
            //
            // 重なった半透明を2枚置き、**作る順番だけ変えて**2回
            // 描きます。並べ替えが効いていれば結果は作成順に
            // よらないので、2枚の絵は一致するはずです。色の合成式を
            // 当てにしないので、ブレンドの設定を将来変えても
            // この段は生き残ります。
            {
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.0f, 6.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { 0.0f, 0.0f, 0.0f });

                // nearZが手前。奥（farZ）を赤、手前を青にします。
                constexpr float nearZ = 1.0f;
                constexpr float farZ = -1.0f;
                const auto buildPane =
                    [&scene](
                        const char* name,
                        const float z,
                        const DirectX::XMFLOAT4& color)
                {
                    auto& pane =
                        scene.CreateGameObject(name);
                    pane.GetTransform().position =
                        { 0.0f, 0.0f, z };
                    pane.GetTransform().SetEulerAngles(
                        { -1.5707963f, 0.0f, 0.0f });
                    pane.GetTransform().scale =
                        { 2.5f, 1.0f, 2.5f };
                    pane.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Plane,
                        color);
                    return &pane;
                };
                constexpr DirectX::XMFLOAT4 farColor{
                    1.0f, 0.15f, 0.15f, 0.5f };
                constexpr DirectX::XMFLOAT4 nearColor{
                    0.15f, 0.3f, 1.0f, 0.5f };

                // ①手前を先に作る（並べ替えが無いと手前が先に出る）
                auto* firstNear = buildPane(
                    "AlphaNearFirst", nearZ, nearColor);
                auto* firstFar = buildPane(
                    "AlphaFarSecond", farZ, farColor);
                Stage("frame-alpha-sort");
                const auto nearFirstFrame =
                    renderComposedFrame();
                DumpFrame("alpha-sort", nearFirstFrame);
                static_cast<void>(
                    scene.DestroyGameObject(*firstNear));
                static_cast<void>(
                    scene.DestroyGameObject(*firstFar));

                // ②奥を先に作る（同じ絵になるはず）
                auto* secondFar = buildPane(
                    "AlphaFarFirst", farZ, farColor);
                auto* secondNear = buildPane(
                    "AlphaNearSecond", nearZ, nearColor);
                const auto farFirstFrame =
                    renderComposedFrame();
                static_cast<void>(
                    scene.DestroyGameObject(*secondFar));
                static_cast<void>(
                    scene.DestroyGameObject(*secondNear));

                // ③板が無い絵。上の2枚が「そもそも何も写って
                // いないから一致した」を弾くために要ります。
                const auto emptyFrame = renderComposedFrame();

                const auto countDifferences =
                    [](const std::vector<std::uint8_t>& left,
                        const std::vector<std::uint8_t>& right)
                {
                    std::size_t changed = 0;
                    for (std::uint32_t y = 0;
                        y < Height;
                        ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            ++x)
                        {
                            const auto a = At(left, x, y);
                            const auto b = At(right, x, y);
                            if (std::max({
                                    std::abs(a.red - b.red),
                                    std::abs(
                                        a.green - b.green),
                                    std::abs(a.blue - b.blue)
                                }) > 8)
                            {
                                ++changed;
                            }
                        }
                    }
                    return changed;
                };
                const auto orderDifference =
                    countDifferences(
                        nearFirstFrame, farFirstFrame);
                const auto paneCoverage =
                    countDifferences(
                        nearFirstFrame, emptyFrame);
                std::cout
                    << "alpha sort: order difference="
                    << orderDifference
                    << " pane coverage=" << paneCoverage
                    << std::endl;

                Require(
                    paneCoverage > 1000,
                    "The translucent panes must actually be"
                    " visible for this stage to mean"
                    " anything.");
                Require(
                    orderDifference == 0,
                    "Translucent objects must composite the"
                    " same regardless of the order they were"
                    " created in.");

                // 読み込みモデル（ModelRenderer）でも同じことを
                // 見ます。中身がパーツの集まりなので、MeshRenderer
                // が通っても素通りする可能性があります。
                const auto buildModel =
                    [&scene](
                        const char* name,
                        const float z,
                        const DirectX::XMFLOAT4& color)
                {
                    auto& object =
                        scene.CreateGameObject(name);
                    object.GetTransform().position =
                        { 0.0f, -0.5f, z };
                    object.GetTransform().scale =
                        { 1.5f, 1.5f, 1.5f };
                    auto& renderer =
                        object.AddComponent<
                            LamaPon::
                                ModelRendererComponent>(
                            std::filesystem::path{ "models" }
                                / "RiggedSimple.glb");
                    // アルファを1未満にすると、モデル全体が
                    // 半透明として扱われます（forceAlpha）。
                    renderer.SetColor(color);
                    return &object;
                };
                auto* modelNearFirst = buildModel(
                    "AlphaModelNear", nearZ, nearColor);
                auto* modelFarSecond = buildModel(
                    "AlphaModelFar", farZ, farColor);
                Stage("frame-alpha-sort-model");
                const auto modelNearFirstFrame =
                    renderComposedFrame();
                DumpFrame(
                    "alpha-sort-model",
                    modelNearFirstFrame);
                static_cast<void>(
                    scene.DestroyGameObject(
                        *modelNearFirst));
                static_cast<void>(
                    scene.DestroyGameObject(
                        *modelFarSecond));

                auto* modelFarFirst = buildModel(
                    "AlphaModelFar2", farZ, farColor);
                auto* modelNearSecond = buildModel(
                    "AlphaModelNear2", nearZ, nearColor);
                const auto modelFarFirstFrame =
                    renderComposedFrame();
                static_cast<void>(
                    scene.DestroyGameObject(*modelFarFirst));
                static_cast<void>(
                    scene.DestroyGameObject(
                        *modelNearSecond));

                const auto modelEmptyFrame =
                    renderComposedFrame();
                const auto modelOrderDifference =
                    countDifferences(
                        modelNearFirstFrame,
                        modelFarFirstFrame);
                const auto modelCoverage =
                    countDifferences(
                        modelNearFirstFrame,
                        modelEmptyFrame);
                std::cout
                    << "alpha sort (model): order"
                       " difference="
                    << modelOrderDifference
                    << " coverage=" << modelCoverage
                    << std::endl;

                Require(
                    modelCoverage > 500,
                    "The translucent models must actually be"
                    " visible for this stage to mean"
                    " anything.");
                Require(
                    modelOrderDifference == 0,
                    "Translucent models must composite the"
                    " same regardless of the order they were"
                    " created in.");

                subject.GetTransform().position =
                    savedSubjectPosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraRotation);
            }

            // テセレーションの確認。
            //
            // 見るのは「不透明として深度を書けること」です。以前は
            // この経路だけ描画状態が固定で深度を書けず、地形のような
            // 不透明な面が作れませんでした。
            //
            // 板の**後ろ**に箱を置き、板より**後に**作ります。深度を
            // 書けていれば箱は隠れます。書けていなければ、後から
            // 描かれる箱が板を上書きして見えてしまいます。
            {
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                // 板は水平（XZ）なので、真上から見下ろします。
                cameraObject.GetTransform().position =
                    { 0.0f, 3.0f, 0.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { -1.5707963f, 0.0f, 0.0f });

                auto& terrain =
                    scene.CreateGameObject("TessTerrain");
                terrain.GetTransform().position =
                    { 0.0f, 0.0f, 0.0f };
                terrain.GetTransform().scale =
                    { 8.0f, 1.0f, 8.0f };
                auto& terrainRenderer =
                    terrain.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Plane,
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f, 1.0f });
                terrainRenderer.SetShaderPath(
                    std::filesystem::path{ "shaders" }
                        / "LamaPonTessellatedTerrain.hlsl");
                // 地面は緑、隠れるべき箱は青。取り違えないよう
                // はっきり分けます。
                terrainRenderer.SetCustomParameter(
                    0,
                    DirectX::XMFLOAT4{
                        0.2f, 0.9f, 0.2f, 1.0f });
                terrainRenderer.SetCustomParameter(
                    1,
                    DirectX::XMFLOAT4{
                        0.1f, 4.0f, 0.0f, 0.0f });
                terrainRenderer.SetCustomParameter(
                    3,
                    DirectX::XMFLOAT4{
                        16.0f, 0.0f, 0.0f, 0.0f });

                auto& buried =
                    scene.CreateGameObject("BuriedBox");
                buried.GetTransform().position =
                    { 0.0f, -2.0f, 0.0f };
                buried.GetTransform().scale =
                    { 3.0f, 1.0f, 3.0f };
                buried.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Cube,
                    DirectX::XMFLOAT4{
                        0.05f, 0.1f, 1.0f, 1.0f });

                Stage("frame-tessellation");
                const auto tessellationFrame =
                    renderComposedFrame();
                DumpFrame(
                    "tessellation",
                    tessellationFrame);

                // 対照。板を消して同じ絵を撮ります。ここで箱が
                // 見えていなければ、隠しているのは板ではなく別の
                // 何か（前の段で置いた地面など）で、上の判定は
                // 空振りです。検証シーンは使い回しなので、
                // 「隠れた＝成功」を信じる前に必ずこれを見ます。
                static_cast<void>(
                    scene.DestroyGameObject(terrain));
                const auto withoutTerrainFrame =
                    renderComposedFrame();
                static_cast<void>(
                    scene.DestroyGameObject(buried));

                const auto countColour =
                    [](const std::vector<std::uint8_t>& frame,
                        const bool wantGreen)
                {
                    std::size_t total = 0;
                    for (std::uint32_t y = 0;
                        y < Height;
                        ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            ++x)
                        {
                            const auto pixel =
                                At(frame, x, y);
                            const int primary = wantGreen
                                ? pixel.green
                                : pixel.blue;
                            const int otherA = wantGreen
                                ? pixel.red
                                : pixel.red;
                            const int otherB = wantGreen
                                ? pixel.blue
                                : pixel.green;
                            if (primary > 80
                                && primary > otherA + 30
                                && primary > otherB + 30)
                            {
                                ++total;
                            }
                        }
                    }
                    return total;
                };
                const auto terrainPixels =
                    countColour(tessellationFrame, true);
                const auto buriedPixels =
                    countColour(tessellationFrame, false);
                const auto buriedWithoutTerrain =
                    countColour(withoutTerrainFrame, false);
                std::cout
                    << "tessellation: terrain="
                    << terrainPixels
                    << " buried=" << buriedPixels
                    << " buried without terrain="
                    << buriedWithoutTerrain
                    << std::endl;

                // 板が出ていること。HSMain/DSMainが無視されていたり
                // コンパイルに失敗していたら、ここで落ちます。
                Require(
                    terrainPixels > 2000,
                    "The tessellated plane must be visible.");
                // 対照が先。板が無いときに箱が見えていなければ、
                // 下の判定は何も確かめていません。
                Require(
                    buriedWithoutTerrain > 500,
                    "The buried box must be visible without"
                    " the tessellated plane, otherwise the"
                    " occlusion check proves nothing.");
                // 深度が書けていれば、後から描く裏の箱は隠れます。
                Require(
                    buriedPixels < 100,
                    "An opaque tessellated surface must write"
                    " depth so later geometry behind it stays"
                    " hidden.");

                // テセレーションShaderを、四角パッチに割れない形状
                // （Sphere）へ割り当てた場合。
                // ハル／ドメインを束ねたまま三角形リストを描くのは
                // D3D11では不正で、利用者の環境ではエディターが
                // ドライバーごと落ちた（2026-08-07）。描けること
                // 自体が回帰の目印になります。
                {
                    auto& wrongShape =
                        scene.CreateGameObject("TessOnSphere");
                    wrongShape.GetTransform().position =
                        { 0.0f, 0.0f, 0.0f };
                    auto& wrongRenderer =
                        wrongShape.AddComponent<
                            LamaPon::MeshRendererComponent>(
                            LamaPon::PrimitiveShape::Sphere,
                            DirectX::XMFLOAT4{
                                1.0f, 1.0f, 1.0f, 1.0f });
                    wrongRenderer.SetShaderPath(
                        std::filesystem::path{ "shaders" }
                        / "LamaPonTessellatedTerrain.hlsl");

                    Stage("frame-tessellation-wrong-shape");
                    const auto wrongFrame =
                        renderComposedFrame();
                    DumpFrame(
                        "tessellation-wrong-shape",
                        wrongFrame);
                    const auto shaderError =
                        wrongRenderer.ShaderError();
                    std::size_t magenta = 0;
                    for (std::uint32_t y = 0; y < Height; ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            ++x)
                        {
                            const auto pixel =
                                At(wrongFrame, x, y);
                            if (pixel.red > 90
                                && pixel.blue > 90
                                && pixel.green + 60 < pixel.red
                                && pixel.green + 60 < pixel.blue)
                            {
                                ++magenta;
                            }
                        }
                    }

                    // 箱を消した絵と見比べます。「落ちなかった」
                    // だけでは、描画そのものが捨てられていても
                    // 気付けません。変化した画素を数えます。
                    static_cast<void>(
                        scene.DestroyGameObject(wrongShape));
                    const auto withoutCube =
                        renderComposedFrame();
                    std::size_t changed = 0;
                    for (std::uint32_t y = 0; y < Height; ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            ++x)
                        {
                            const auto before =
                                At(wrongFrame, x, y);
                            const auto after =
                                At(withoutCube, x, y);
                            if (std::abs(
                                    before.red - after.red)
                                    > 8
                                || std::abs(
                                    before.green - after.green)
                                    > 8
                                || std::abs(
                                    before.blue - after.blue)
                                    > 8)
                            {
                                ++changed;
                            }
                        }
                    }
                    std::cout
                        << "tessellation on a sphere: changed="
                        << changed
                        << " magenta=" << magenta
                        << " error=\""
                        << shaderError
                        << "\""
                        << std::endl;
                    Require(
                        changed > 500,
                        "A tessellation shader on a shape that"
                        " cannot tessellate must still draw the"
                        " mesh.");
                    // テセレーション前提の頂点シェーダーは単体では
                    // 成立せず、そのまま描くと何も出ません。消える
                    // より、壊れている色で見えている方が探しやすい。
                    Require(
                        magenta > 500,
                        "A tessellation shader on a shape that"
                        " cannot tessellate must fall back to"
                        " the magenta placeholder.");
                    // 黙って無視すると「書いたのに効かない」に
                    // なるので、理由が読めること。
                    Require(
                        !shaderError.empty(),
                        "Assigning a tessellation shader to a"
                        " shape that cannot tessellate must be"
                        " reported.");
                }

                // Cubeは6面それぞれを四角パッチにできるので、
                // テセレーションが**効きます**（以前は代役でした）。
                //
                // 見るのは2つです。
                //  (1) 6面ぜんぶがパッチになっていること。1面しか
                //      描いていなくても「マゼンタが出ない」だけなら
                //      通ってしまうので、**普通のCubeと同じ大きさに
                //      見えるか**を対照に置きます。
                //  (2) 変位が効いていること。**真上から見ると上面は
                //      +Yへ動くだけで輪郭が変わらない**ので、箱を
                //      傾けて側面の膨らみが写るようにします。
                {
                    auto& tessCube =
                        scene.CreateGameObject("TessCube");
                    tessCube.GetTransform().position =
                        { 0.0f, 0.0f, 0.0f };
                    tessCube.GetTransform().scale =
                        { 1.2f, 1.2f, 1.2f };
                    tessCube.GetTransform().SetEulerAngles(
                        { 0.6f, 0.5f, 0.0f });
                    auto& cubeRenderer =
                        tessCube.AddComponent<
                            LamaPon::MeshRendererComponent>(
                            LamaPon::PrimitiveShape::Cube,
                            DirectX::XMFLOAT4{
                                1.0f, 1.0f, 1.0f, 1.0f });

                    Stage("frame-tessellation-cube-plain");
                    const auto plainCubeFrame =
                        renderComposedFrame();
                    DumpFrame(
                        "tessellation-cube-plain",
                        plainCubeFrame);

                    cubeRenderer.SetShaderPath(
                        std::filesystem::path{ "shaders" }
                        / "LamaPonTessellatedTerrain.hlsl");
                    cubeRenderer.SetCustomParameter(
                        0,
                        DirectX::XMFLOAT4{
                            0.2f, 0.9f, 0.2f, 1.0f });
                    cubeRenderer.SetCustomParameter(
                        3,
                        DirectX::XMFLOAT4{
                            8.0f, 0.0f, 0.0f, 0.0f });
                    // 周波数は分割数と**割り切れない**値にします。
                    // 分割8に対して周波数4だと sin(u*4*2pi) が
                    // u=k/8 のすべてでちょうど0になり、頂点が1つも
                    // 動きません（傾きだけ変わるので陰影は変わり、
                    // 「効いているように見えて動いていない」絵に
                    // なります。2026-08-07に実際に踏みました）。
                    cubeRenderer.SetCustomParameter(
                        1,
                        DirectX::XMFLOAT4{
                            0.0f, 2.5f, 0.0f, 0.0f });
                    Stage("frame-tessellation-cube-flat");
                    const auto flatFrame =
                        renderComposedFrame();
                    DumpFrame(
                        "tessellation-cube-flat",
                        flatFrame);
                    const auto cubeError =
                        cubeRenderer.ShaderError();

                    cubeRenderer.SetCustomParameter(
                        1,
                        DirectX::XMFLOAT4{
                            0.35f, 2.5f, 0.0f, 0.0f });
                    Stage("frame-tessellation-cube");
                    const auto bumpFrame =
                        renderComposedFrame();
                    DumpFrame(
                        "tessellation-cube",
                        bumpFrame);

                    // 箱を消した絵を基準に、覆っている画素を数えます。
                    static_cast<void>(
                        scene.DestroyGameObject(tessCube));
                    const auto emptyFrame =
                        renderComposedFrame();

                    const auto coverage =
                        [&emptyFrame](
                            const std::vector<std::uint8_t>&
                                frame)
                    {
                        std::size_t total = 0;
                        for (std::uint32_t y = 0;
                            y < Height;
                            ++y)
                        {
                            for (std::uint32_t x = 0;
                                x < Width;
                                ++x)
                            {
                                const auto with =
                                    At(frame, x, y);
                                const auto without =
                                    At(emptyFrame, x, y);
                                if (std::abs(
                                        with.red - without.red)
                                        > 8
                                    || std::abs(
                                        with.green
                                        - without.green) > 8
                                    || std::abs(
                                        with.blue
                                        - without.blue) > 8)
                                {
                                    ++total;
                                }
                            }
                        }
                        return total;
                    };
                    std::size_t magenta = 0;
                    for (std::uint32_t y = 0; y < Height; ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            ++x)
                        {
                            const auto pixel =
                                At(flatFrame, x, y);
                            if (pixel.red > 90
                                && pixel.blue > 90
                                && pixel.green + 60 < pixel.red
                                && pixel.green + 60 < pixel.blue)
                            {
                                ++magenta;
                            }
                        }
                    }

                    const auto plainCoverage =
                        coverage(plainCubeFrame);
                    const auto flatCoverage =
                        coverage(flatFrame);
                    const auto bumpCoverage =
                        coverage(bumpFrame);
                    std::cout
                        << "tessellation on a cube: plain="
                        << plainCoverage
                        << " flat=" << flatCoverage
                        << " bumped=" << bumpCoverage
                        << " magenta=" << magenta
                        << " error=\"" << cubeError << "\""
                        << std::endl;
                    Require(
                        cubeError.empty(),
                        "A cube can be split into quad patches,"
                        " so a tessellation shader must be"
                        " accepted on it.");
                    Require(
                        magenta == 0,
                        "A tessellated cube must not fall back"
                        " to the magenta placeholder.");
                    Require(
                        plainCoverage > 2000,
                        "The plain cube must be visible,"
                        " otherwise the comparison below means"
                        " nothing.");
                    // 6面ぜんぶがパッチになっていれば、変位なしの
                    // テセレーションは普通のCubeと同じ形になります。
                    // 1面しか描いていなければ、ここで大きく減ります。
                    Require(
                        flatCoverage * 10 > plainCoverage * 8
                            && flatCoverage * 10
                                < plainCoverage * 12,
                        "An undisplaced tessellated cube must"
                        " cover the same area as a plain cube"
                        " (all six faces must become patches).");
                    // 側面が法線方向（外向き）へ膨らむので広がります。
                    Require(
                        bumpCoverage > flatCoverage + 300,
                        "Displacing along the face normal must"
                        " change the cube's silhouette,"
                        " otherwise nothing was displaced.");
                }

                // ジオメトリシェーダー。三角形を面の向きへ押し出す
                // 見本を割り当てて、**形が変わること**を見ます。
                // 「マゼンタが出ない」だけでは、GSが1度も走らなくても
                // 通ってしまいます。
                {
                    // 置く前の絵。あとで「消したら元に戻ったか」を
                    // 見るための対照です。ずっと前の段の絵を使うと、
                    // 途中で変わった分まで差として出ます
                    // （最初これで23143画素の誤検出を出しました）。
                    const auto beforeGeometryFrame =
                        renderComposedFrame();

                    auto& gsObject =
                        scene.CreateGameObject("GeometryShader");
                    gsObject.GetTransform().position =
                        { 0.0f, 0.0f, 0.0f };
                    gsObject.GetTransform().scale =
                        { 1.2f, 1.2f, 1.2f };
                    gsObject.GetTransform().SetEulerAngles(
                        { 0.6f, 0.5f, 0.0f });
                    auto& gsRenderer =
                        gsObject.AddComponent<
                            LamaPon::MeshRendererComponent>(
                            LamaPon::PrimitiveShape::Cube,
                            DirectX::XMFLOAT4{
                                1.0f, 1.0f, 1.0f, 1.0f });
                    gsRenderer.SetShaderPath(
                        std::filesystem::path{ "shaders" }
                        / "LamaPonGeometryExplode.hlsl");
                    gsRenderer.SetCustomParameter(
                        0,
                        DirectX::XMFLOAT4{
                            0.85f, 0.45f, 0.2f, 1.0f });

                    // 押し出し0。GSは走りますが形は元のままです。
                    gsRenderer.SetCustomParameter(
                        1,
                        DirectX::XMFLOAT4{
                            0.0f, 0.0f, 0.0f, 0.0f });
                    Stage("frame-geometry-shader-flat");
                    const auto gsFlatFrame =
                        renderComposedFrame();
                    DumpFrame(
                        "geometry-shader-flat",
                        gsFlatFrame);
                    const auto gsError =
                        gsRenderer.ShaderError();

                    // 押し出しあり。面がばらけるので輪郭が広がります。
                    gsRenderer.SetCustomParameter(
                        1,
                        DirectX::XMFLOAT4{
                            0.5f, 0.0f, 0.0f, 0.0f });
                    Stage("frame-geometry-shader");
                    const auto gsFrame =
                        renderComposedFrame();
                    DumpFrame("geometry-shader", gsFrame);

                    static_cast<void>(
                        scene.DestroyGameObject(gsObject));
                    const auto gsEmptyFrame =
                        renderComposedFrame();
                    DumpFrame(
                        "geometry-shader-empty",
                        gsEmptyFrame);
                    {
                        // 同じ画素を3枚から並べます。「消えた」のか
                        // 「暗く描かれた」のかは、対照と見比べないと
                        // 区別できません。
                        const auto sample =
                            [](const std::vector<std::uint8_t>&
                                    frame)
                        {
                            const auto pixel =
                                At(frame, 150, 90);
                            return std::to_string(pixel.red)
                                + ","
                                + std::to_string(pixel.green)
                                + ","
                                + std::to_string(pixel.blue);
                        };
                        std::cout
                            << "geometry shader pixel(150,90):"
                            << " flat=" << sample(gsFlatFrame)
                            << " exploded=" << sample(gsFrame)
                            << " empty=" << sample(gsEmptyFrame)
                            << std::endl;
                    }

                    const auto gsCoverage =
                        [&gsEmptyFrame](
                            const std::vector<std::uint8_t>&
                                frame)
                    {
                        std::size_t total = 0;
                        for (std::uint32_t y = 0;
                            y < Height;
                            ++y)
                        {
                            for (std::uint32_t x = 0;
                                x < Width;
                                ++x)
                            {
                                const auto with =
                                    At(frame, x, y);
                                const auto without =
                                    At(gsEmptyFrame, x, y);
                                if (std::abs(
                                        with.red - without.red)
                                        > 8
                                    || std::abs(
                                        with.green
                                        - without.green) > 8
                                    || std::abs(
                                        with.blue
                                        - without.blue) > 8)
                                {
                                    ++total;
                                }
                            }
                        }
                        return total;
                    };
                    const auto gsFlatCoverage =
                        gsCoverage(gsFlatFrame);
                    const auto gsBurstCoverage =
                        gsCoverage(gsFrame);
                    std::cout
                        << "geometry shader: flat="
                        << gsFlatCoverage
                        << " exploded=" << gsBurstCoverage
                        << " error=\"" << gsError << "\""
                        << std::endl;
                    Require(
                        gsError.empty(),
                        "A geometry shader with triangle input"
                        " must be accepted.");
                    Require(
                        gsFlatCoverage > 2000,
                        "The cube must be visible with the"
                        " geometry shader bound, otherwise the"
                        " comparison below means nothing.");
                    // 面がばらけると、外へ広がる分と、隙間から
                    // 向こうが見える分の両方が起きます。**増える
                    // とは限らない**ので、増減ではなく変化量を見ます。
                    const auto silhouetteChange =
                        gsBurstCoverage > gsFlatCoverage
                            ? gsBurstCoverage - gsFlatCoverage
                            : gsFlatCoverage - gsBurstCoverage;
                    Require(
                        silhouetteChange > 1000,
                        "Pushing each triangle along its face"
                        " normal must change the silhouette,"
                        " otherwise the geometry shader never"
                        " ran.");
                    // 束ねたまま抜けていないこと。GSが残っていると、
                    // この後のスプライトやポスト処理まで巻き込みます。
                    // オブジェクトを消した後の絵が、テセレーションの
                    // 段を始める前と同じであることで確かめます。
                    std::size_t leaked = 0;
                    for (std::uint32_t y = 0; y < Height; ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            ++x)
                        {
                            const auto after =
                                At(gsEmptyFrame, x, y);
                            const auto before =
                                At(beforeGeometryFrame, x, y);
                            if (std::abs(
                                    after.red - before.red) > 8
                                || std::abs(
                                    after.green
                                    - before.green) > 8
                                || std::abs(
                                    after.blue
                                    - before.blue) > 8)
                            {
                                ++leaked;
                            }
                        }
                    }
                    std::cout
                        << "geometry shader leak check: "
                        << leaked << " pixels" << std::endl;
                    Require(
                        leaked < 500,
                        "A geometry shader must not stay bound"
                        " after the object that used it is"
                        " gone.");
                }

                // 同じことを読み込みモデルでも見ます。MeshRenderer
                // だけ塞いでもModelRendererは素通りしていました
                // （2026-08-07）。パッチで描く経路が無いのは同じ
                // なので、テセレーション前提の頂点シェーダーが刺さり
                // **何も描かれない**まま黙って消えます。
                //
                // 使うのはCMO（スキニングなし）です。glTF/FBXは
                // VSSkinnedMainが無くてコンパイルの時点で落ちるため、
                // 差し替えの手前で止まってしまい**この経路を通りません**。
                // マテリアル上書きを有効にしないと共通Litで描かれない
                // （＝カスタムShaderが効かない）ので、そこも合わせます。
                {
                    auto& tessModel =
                        scene.CreateGameObject("TessOnModel");
                    tessModel.GetTransform().position =
                        { 0.0f, -0.5f, 0.0f };
                    tessModel.GetTransform().scale =
                        { 1.5f, 1.5f, 1.5f };
                    auto& modelRenderer =
                        tessModel.AddComponent<
                            LamaPon::ModelRendererComponent>(
                            std::filesystem::path{ "models" }
                                / "arrow.cmo");
                    modelRenderer.SetMaterialOverrideEnabled(
                        true);
                    modelRenderer.SetShaderPath(
                        std::filesystem::path{ "shaders" }
                        / "LamaPonTessellatedTerrain.hlsl");

                    Stage("frame-tessellation-on-model");
                    const auto modelFrame =
                        renderComposedFrame();
                    DumpFrame(
                        "tessellation-on-model",
                        modelFrame);
                    const auto modelShaderError =
                        modelRenderer.ShaderError();
                    // 共通Litで描かれていないと、この段は「代役が
                    // 出ている」ことを何も確かめていません。
                    const bool usesLamaPonLit =
                        modelRenderer.UsesLamaPonLit();
                    std::size_t modelMagenta = 0;
                    for (std::uint32_t y = 0; y < Height; ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            ++x)
                        {
                            const auto pixel =
                                At(modelFrame, x, y);
                            if (pixel.red > 90
                                && pixel.blue > 90
                                && pixel.green + 60 < pixel.red
                                && pixel.green + 60 < pixel.blue)
                            {
                                ++modelMagenta;
                            }
                        }
                    }

                    // モデルを消した絵と見比べます。「落ちなかった」
                    // では、描画が丸ごと捨てられていても通ります。
                    static_cast<void>(
                        scene.DestroyGameObject(tessModel));
                    const auto withoutModel =
                        renderComposedFrame();
                    std::size_t modelChanged = 0;
                    for (std::uint32_t y = 0; y < Height; ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            ++x)
                        {
                            const auto before =
                                At(modelFrame, x, y);
                            const auto after =
                                At(withoutModel, x, y);
                            if (std::abs(
                                    before.red - after.red)
                                    > 8
                                || std::abs(
                                    before.green - after.green)
                                    > 8
                                || std::abs(
                                    before.blue - after.blue)
                                    > 8)
                            {
                                ++modelChanged;
                            }
                        }
                    }
                    std::cout
                        << "tessellation on a model: changed="
                        << modelChanged
                        << " magenta=" << modelMagenta
                        << " lamaponLit="
                        << (usesLamaPonLit ? "yes" : "no")
                        << " error=\""
                        << modelShaderError
                        << "\""
                        << std::endl;
                    Require(
                        usesLamaPonLit,
                        "The model must actually be drawn with"
                        " the common Lit path, otherwise this"
                        " stage checks nothing.");
                    Require(
                        modelChanged > 200,
                        "A tessellation shader on a loaded model"
                        " must still draw the model.");
                    Require(
                        modelMagenta > 200,
                        "A tessellation shader on a loaded model"
                        " must fall back to the magenta"
                        " placeholder.");
                    // コンパイル失敗による代役と区別します。CMOは
                    // VSMain/PSMainがそろっていてコンパイルは通るので、
                    // 理由がテセレーションでなければ差し替えは
                    // 効いていません。
                    Require(
                        modelShaderError.find("tessellation")
                            != std::string::npos,
                        "Assigning a tessellation shader to a"
                        " loaded model must be reported as a"
                        " tessellation problem.");
                }

                subject.GetTransform().position =
                    savedSubjectPosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraRotation);
            }

            // 自作Compute Shaderの確認。
            //
            // 計算した絵を名前付きテクスチャへ書き、その名前を
            // SpriteRendererへ入れて表示します。表示側は無改造で
            // 使える（レンダーテクスチャと同じ登録簿に入る）ことも
            // ここで一緒に確かめています。
            {
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.0f, 6.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { 0.0f, 0.0f, 0.0f });

                constexpr float computeRed = 0.75f;
                LamaPon::ComputeEffectRequest compute;
                compute.shader =
                    std::filesystem::path{
                        LAMAPON_TEST_FIXTURE_DIR }
                    / "compute-probe.hlsl";
                compute.outputTexture = "computeProbe";
                compute.outputWidth = 128;
                compute.outputHeight = 128;
                compute.customParameters[0] = {
                    computeRed, 0.0f, 0.0f, 0.0f
                };
                std::string computeError;
                Require(
                    graphics.DispatchComputeEffect(
                        compute,
                        &computeError),
                    ("The compute effect must run: "
                        + computeError).c_str());

                // 出力テクスチャの中身をCPUへ読み戻して確かめます。
                //
                // 最初はスプライトで表示した絵を判定していましたが、
                // シーンの床まで「赤が優勢」に数えてしまい、さらに
                // 表示経路（フィルタやUV）が挟まるせいで、
                // 「Computeが書けていない」のか「表示が違う」のかを
                // 切り分けられませんでした。書いた本人を直接読みます。
                const auto* computeTarget =
                    graphics.FindRenderTexture(
                        "computeProbe");
                Require(
                    computeTarget != nullptr
                        && computeTarget->DisplayTexture()
                            != nullptr,
                    "The compute output texture must exist.");
                std::cout
                    << "compute effect: output "
                    << computeTarget->Width()
                    << "x" << computeTarget->Height()
                    << std::endl;

                D3D11_TEXTURE2D_DESC outputDescription{};
                computeTarget->DisplayTexture()->GetDesc(
                    &outputDescription);
                outputDescription.Usage =
                    D3D11_USAGE_STAGING;
                outputDescription.BindFlags = 0;
                outputDescription.CPUAccessFlags =
                    D3D11_CPU_ACCESS_READ;
                outputDescription.MiscFlags = 0;
                Microsoft::WRL::ComPtr<ID3D11Texture2D>
                    staging;
                Require(
                    SUCCEEDED(graphics.Device()
                        ->CreateTexture2D(
                            &outputDescription,
                            nullptr,
                            staging.GetAddressOf())),
                    "The staging copy of the compute output"
                    " must be created.");
                graphics.Context()->CopyResource(
                    staging.Get(),
                    computeTarget->DisplayTexture());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                Require(
                    SUCCEEDED(graphics.Context()->Map(
                        staging.Get(),
                        0,
                        D3D11_MAP_READ,
                        0,
                        &mapped)),
                    "The compute output must be readable.");

                // R16G16B16A16_FLOAT なので half から戻します。
                const auto texel =
                    [&](const std::uint32_t x,
                        const std::uint32_t y)
                {
                    const auto* row =
                        static_cast<const std::uint8_t*>(
                            mapped.pData)
                        + static_cast<std::size_t>(y)
                            * mapped.RowPitch;
                    const auto* halves =
                        reinterpret_cast<
                            const DirectX::PackedVector::
                                HALF*>(row)
                        + static_cast<std::size_t>(x) * 4;
                    return DirectX::XMFLOAT3{
                        DirectX::PackedVector::
                            XMConvertHalfToFloat(halves[0]),
                        DirectX::PackedVector::
                            XMConvertHalfToFloat(halves[1]),
                        DirectX::PackedVector::
                            XMConvertHalfToFloat(halves[2])
                    };
                };
                const auto left =
                    texel(compute.outputWidth / 4,
                        compute.outputHeight / 2);
                const auto rightTop =
                    texel(compute.outputWidth * 3 / 4, 1);
                const auto rightBottom =
                    texel(compute.outputWidth * 3 / 4,
                        compute.outputHeight - 1);
                std::cout
                    << "compute effect texels: left r="
                    << left.x
                    << " right top g=" << rightTop.y
                    << " right bottom g=" << rightBottom.y
                    << std::endl;
                graphics.Context()->Unmap(staging.Get(), 0);

                Require(
                    std::abs(left.x - computeRed) < 0.01f
                        && left.y < 0.01f,
                    "The compute shader must write the"
                    " requested colour into the left half.");
                // 下ほど明るい縦のグラデーション。配線だけ通って
                // 中身が一様だと、ここで落ちます。
                Require(
                    rightBottom.y > rightTop.y + 0.5f,
                    "The compute output must keep the"
                    " vertical gradient it wrote.");

                // 表示側。同じ名前をSpriteRendererへ入れるだけで
                // 出る、という導線をドキュメントに書いているので、
                // 実際に出ることも見ます。テクスチャの右半分は
                // 明るい緑なので、画面のどこかに緑が優勢な画素が
                // 無ければ表示できていません。
                // スプライトの位置とサイズは**画面のピクセル**です
                // （ワールド座標ではありません）。サイズを省くと
                // 数ピクセルになって見えず、判定が空振りします。
                auto& computeSprite =
                    scene.CreateGameObject("ComputeSprite");
                computeSprite.GetTransform().position =
                    { 80.0f, 30.0f, 0.0f };
                auto& computeRenderer =
                    computeSprite.AddComponent<
                        LamaPon::SpriteRendererComponent>(
                        DirectX::XMFLOAT2{ 160.0f, 120.0f });
                computeRenderer.SetRenderTexture(
                    "computeProbe");
                Stage("frame-compute-effect");
                // 2Dを描くのはscene.Render()を通るrenderFrameの方
                // です。renderComposedFrameは3Dの合成だけなので、
                // スプライトを置いても一生出てきません。
                const auto computeFrame = renderFrame();
                DumpFrame("compute-effect", computeFrame);
                static_cast<void>(
                    scene.DestroyGameObject(
                        computeSprite));

                std::size_t spriteGreen = 0;
                std::size_t spriteRed = 0;
                for (std::uint32_t y = 0; y < Height; ++y)
                {
                    for (std::uint32_t x = 0;
                        x < Width;
                        ++x)
                    {
                        const auto pixel =
                            At(computeFrame, x, y);
                        // 背景の床は(191,151,48)のような暖色で、
                        // 青がはっきり低い。混ざらないよう、
                        // 「他の2色がどちらも低い」ものだけ数えます。
                        if (pixel.green > 80
                            && pixel.red < 60
                            && pixel.blue < 60)
                        {
                            ++spriteGreen;
                        }
                        if (pixel.red > 80
                            && pixel.green < 60
                            && pixel.blue < 60)
                        {
                            ++spriteRed;
                        }
                    }
                }
                std::cout
                    << "compute sprite: red=" << spriteRed
                    << " green=" << spriteGreen
                    << std::endl;
                Require(
                    spriteRed > 200 && spriteGreen > 200,
                    "A SpriteRenderer pointed at the compute"
                    " output must show both halves of it.");

                subject.GetTransform().position =
                    savedSubjectPosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraRotation);
            }

            // リフレクションプローブの確認。鏡面の球のそばに
            // 緑の壁を置いてプローブを焼く。テストシーンには
            // Skyboxが無いので、プローブ無しでは球へ緑が映る
            // 経路が存在しない＝球に緑が出ればプローブが機能
            // している証拠になる。
            {
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.5f, 6.0f };
                cameraObject.GetTransform().SetEulerAngles({ -0.15f, 0.0f, 0.0f });

                auto& mirrorSphere =
                    scene.CreateGameObject(
                        "ProbeMirrorSphere");
                mirrorSphere.GetTransform().position =
                    { 0.0f, 0.0f, 0.0f };
                mirrorSphere.GetTransform().scale =
                    { 2.0f, 2.0f, 2.0f };
                auto& mirrorRenderer =
                    mirrorSphere.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Sphere,
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f, 1.0f },
                        std::filesystem::path{},
                        std::filesystem::path{},
                        0.05f,
                        1.0f);
                mirrorRenderer.SetMetallic(1.0f);

                // 壁はカメラ（z=+6）の後ろ。カメラには映らず、
                // 球の反射にだけ現れます。
                auto& greenWall =
                    scene.CreateGameObject("ProbeWall");
                greenWall.GetTransform().position =
                    { 0.0f, 0.0f, 10.0f };
                greenWall.GetTransform().scale =
                    { 20.0f, 12.0f, 0.3f };
                greenWall.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Cube,
                    DirectX::XMFLOAT4{
                        0.05f, 0.9f, 0.1f, 1.0f },
                    std::filesystem::path{},
                    std::filesystem::path{},
                    1.0f,
                    1.0f);

                // プローブは鏡面球の中心に置きます。球の面は内側
                // から見ると全部裏面（カリングされる）なので、
                // ベイクには球自身が映らず周囲だけが焼けます。
                auto& probeObject =
                    scene.CreateGameObject(
                        "ReflectionProbe");
                probeObject.GetTransform().position =
                    { 0.0f, 0.0f, 0.0f };
                probeObject.AddComponent<
                    LamaPon::ReflectionProbeComponent>(
                    20.0f,
                    1.0f);

                Stage("frame-reflection-probe-on");
                DumpFrame(
                    "reflection-probe-on",
                    renderComposedFrame());
                // 診断: ベイクが走ったか、検索で見つかるかを出す
                // （--dump時のみの出力。効いていないときの切り分け用）。
                {
                    auto& probeComponent =
                        *probeObject.GetComponent<
                            LamaPon::
                                ReflectionProbeComponent>();
                    std::cout
                        << "probe baked: "
                        << (probeComponent.IsBaked()
                            ? "yes" : "no")
                        << ", bake requested: "
                        << (probeComponent
                                .IsBakeRequested()
                            ? "yes" : "no")
                        << ", lookup: "
                        << (scene.NearestReflectionProbe(
                                DirectX::XMFLOAT3{
                                    0.0f, 0.0f, 0.0f })
                                != nullptr
                            ? "hit" : "miss")
                        << std::endl;
                }

                // ボックス射影。箱の大きさを与えると、反射先が箱との
                // 交点へ補正されて壁が正しい距離で映ります。指定なしの
                // ときは無限遠として映るため、球の上に映る壁の
                // 位置と大きさが変わります。
                {
                    auto& probeComponent =
                        *probeObject.GetComponent<
                            LamaPon::
                                ReflectionProbeComponent>();
                    probeComponent.SetBoxExtents(
                        { 12.0f, 6.0f, 11.0f });
                    Stage("frame-reflection-box-on");
                    DumpFrame(
                        "reflection-box-on",
                        renderComposedFrame());
                    std::cout
                        << "box projection active: "
                        << (probeComponent
                                .UsesBoxProjection()
                            ? "yes" : "no")
                        << std::endl;
                    probeComponent.SetBoxExtents(
                        { 0.0f, 0.0f, 0.0f });
                }

                // プローブを無効にした比較用（球は環境反射なし）。
                probeObject.SetEnabled(false);
                Stage("frame-reflection-probe-off");
                DumpFrame(
                    "reflection-probe-off",
                    renderComposedFrame());

                static_cast<void>(
                    scene.DestroyGameObject(probeObject));
                static_cast<void>(
                    scene.DestroyGameObject(greenWall));
                static_cast<void>(
                    scene.DestroyGameObject(mirrorSphere));
                subject.GetTransform().position =
                    savedSubjectPosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(savedCameraRotation);
            }

            // リフレクションプローブの永続化。ベイク結果はディスクへ
            // 保存され、シーン由来のプローブは次にシーンを開いたとき
            // ベイクの代わりに復元されます。ここでは
            //   ・復元した絵がベイク直後の絵とバイト単位で一致する
            //   ・本当にディスクから来ている（黙って再ベイクして
            //     いない）
            // の2つを確かめます。後者は「壁の色を変えたのに、復元は
            // 古い色の反射を返す」ことで見分けます。再ベイクなら新しい
            // 色が映るはずなので、この2つは両立しません。
            {
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.5f, 6.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { -0.15f, 0.0f, 0.0f });

                // 前の節と同じ配置（鏡面球＋カメラの後ろの緑の壁）。
                auto& mirrorSphere =
                    scene.CreateGameObject(
                        "PersistMirrorSphere");
                mirrorSphere.GetTransform().position =
                    { 0.0f, 0.0f, 0.0f };
                mirrorSphere.GetTransform().scale =
                    { 2.0f, 2.0f, 2.0f };
                auto& mirrorRenderer =
                    mirrorSphere.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Sphere,
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f, 1.0f },
                        std::filesystem::path{},
                        std::filesystem::path{},
                        0.05f,
                        1.0f);
                mirrorRenderer.SetMetallic(1.0f);
                auto& wall =
                    scene.CreateGameObject("PersistWall");
                wall.GetTransform().position =
                    { 0.0f, 0.0f, 10.0f };
                wall.GetTransform().scale =
                    { 20.0f, 12.0f, 0.3f };
                auto& wallRenderer = wall.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Cube,
                    DirectX::XMFLOAT4{
                        0.05f, 0.9f, 0.1f, 1.0f },
                    std::filesystem::path{},
                    std::filesystem::path{},
                    1.0f,
                    1.0f);

                // 決定性のため、空のキャッシュから始めます
                // （置き場はmainでtest-output配下へ差し替え済み）。
                const auto environmentCacheDirectory =
                    LamaPon::EnvironmentCache::
                        CacheDirectory();
                std::error_code cleanupError;
                std::filesystem::remove_all(
                    environmentCacheDirectory,
                    cleanupError);

                const auto createProbe =
                    [&scene](const char* name)
                    -> LamaPon::ReflectionProbeComponent&
                {
                    auto& probeObject =
                        scene.CreateGameObject(name);
                    probeObject.GetTransform().position =
                        { 0.0f, 0.0f, 0.0f };
                    auto& component =
                        probeObject.AddComponent<
                            LamaPon::
                                ReflectionProbeComponent>(
                            20.0f,
                            1.0f);
                    // シーンファイル由来の印。実際のシーン読み込み
                    // ではデシリアライザが立てます。これが無い
                    // プローブは保存も復元もされません。
                    component.MarkLoadedFromScene();
                    return component;
                };

                // ①ベイク（初回はキャッシュが無いので普通に焼き、
                // 結果がディスクへ書かれる）。
                auto& bakedProbe =
                    createProbe("PersistedProbe");
                Stage("frame-probe-persist-bake");
                const auto bakedFrame =
                    renderComposedFrame();
                Require(
                    bakedProbe.IsBaked(),
                    "The probe must bake on the first frame.");
                Require(
                    std::filesystem::exists(
                        environmentCacheDirectory)
                        && !std::filesystem::is_empty(
                            environmentCacheDirectory),
                    "Baking a scene-loaded probe must write"
                    " an environment cache entry.");

                // ②作り直して復元（シーンを開き直したのと同じ）。
                // 絵はベイク直後とバイト単位で一致すること。
                static_cast<void>(
                    scene.DestroyGameObject(
                        bakedProbe.Owner()));
                auto& restoredProbe =
                    createProbe("PersistedProbeReloaded");
                Stage("frame-probe-persist-restore");
                const auto restoredFrame =
                    renderComposedFrame();
                Require(
                    restoredProbe.IsBaked(),
                    "The reloaded probe must restore from"
                    " the disk cache.");
                Require(
                    restoredFrame == bakedFrame,
                    "The restored probe must render the exact"
                    " same frame as the fresh bake.");

                // ③壁を赤へ変えて作り直すと、復元は**緑のまま**の
                // 反射を返すこと。ここが「本当にディスクから来て
                // いる」ことの証明です（黙って再ベイクしていたら
                // 赤くなってこの一致が壊れます）。壁はカメラの
                // 後ろなので、反射以外にこの色変更は写りません。
                wallRenderer.SetColor(
                    { 0.9f, 0.06f, 0.05f, 1.0f });
                static_cast<void>(
                    scene.DestroyGameObject(
                        restoredProbe.Owner()));
                auto& staleProbe =
                    createProbe("PersistedProbeStale");
                Stage("frame-probe-persist-stale");
                const auto staleFrame =
                    renderComposedFrame();
                DumpFrame("probe-persist-stale", staleFrame);
                Require(
                    staleFrame == bakedFrame,
                    "The restored probe must serve the stored"
                    " bake even after the wall changed.");

                // ④キャッシュを消せば普通のベイクへ落ち、今度は
                // 赤い壁が映ること（復元へ固執しないことの確認）。
                std::filesystem::remove_all(
                    environmentCacheDirectory,
                    cleanupError);
                static_cast<void>(
                    scene.DestroyGameObject(
                        staleProbe.Owner()));
                auto& rebakedProbe =
                    createProbe("PersistedProbeRebaked");
                Stage("frame-probe-persist-rebake");
                const auto rebakedFrame =
                    renderComposedFrame();
                Require(
                    rebakedProbe.IsBaked(),
                    "With no cache the probe must fall back"
                    " to a real bake.");
                Require(
                    rebakedFrame != bakedFrame,
                    "The fresh bake must show the new wall"
                    " color instead of the stored one.");

                static_cast<void>(
                    scene.DestroyGameObject(
                        rebakedProbe.Owner()));
                static_cast<void>(
                    scene.DestroyGameObject(wall));
                static_cast<void>(
                    scene.DestroyGameObject(mirrorSphere));
                subject.GetTransform().position =
                    savedSubjectPosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraRotation);
            }

            // ベイクした間接光（照度ボリューム）。赤い壁のそばの
            // 床に、壁からのはね返り（バウンス光）が出ることを
            // 確かめます。
            //
            // 決め手は3つ。
            //   ・有効でも**データが無ければ**絵は1ビットも変わらない
            //   ・ベイク後、壁のそばの床は赤寄りになり、壁から離れた
            //     床の変化はそれよりはっきり小さい（＝画面全体を
            //     一様に赤くする実装では通らない）
            //   ・JSONへ保存→別のSceneへ読み込みで、同じ間接光が
            //     再現される
            {
                // 前回、cloneを退かし忘れて「離れた床」の帯に赤い
                // キューブがそのまま写り、検査が壁のはね返りではなく
                // キューブを測っていました。両方とも退かします。
                const auto giSavedClonePosition =
                    clone.GetTransform().position;
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                clone.GetTransform().position =
                    { 6.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 2.5f, 8.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { -0.35f, 0.0f, 0.0f });

                // 赤い壁。カメラから見て左側に立てます。
                auto& redWall =
                    scene.CreateGameObject("GiRedWall");
                redWall.GetTransform().position =
                    { -4.0f, 0.2f, 0.0f };
                redWall.GetTransform().scale =
                    { 0.3f, 4.0f, 12.0f };
                redWall.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Cube,
                    DirectX::XMFLOAT4{
                        0.9f, 0.05f, 0.05f, 1.0f },
                    std::filesystem::path{},
                    std::filesystem::path{},
                    1.0f,
                    1.0f);

                // 床の帯の「赤寄り」。壁のそば（画面左）と、壁から
                // 離れた場所（画面右）を同じ大きさで測ります。
                const auto redSkew =
                    [&](const std::vector<std::uint8_t>& frame,
                        const std::uint32_t minimumX,
                        const std::uint32_t maximumX)
                {
                    long long skew = 0;
                    for (std::uint32_t y = 130; y < 165; ++y)
                    {
                        for (std::uint32_t x = minimumX;
                            x < maximumX;
                            ++x)
                        {
                            const auto pixel = At(frame, x, y);
                            skew += pixel.red;
                            skew -= pixel.blue;
                        }
                    }
                    return skew;
                };
                // 帯はダンプ画像から決めています（壁の右が床になる
                // のはx≈105から。遠い帯はボリュームの外の床です）。
                constexpr std::uint32_t nearMinimumX = 110;
                constexpr std::uint32_t nearMaximumX = 150;
                constexpr std::uint32_t farMinimumX = 270;
                constexpr std::uint32_t farMaximumX = 310;
                constexpr long long bandPixels =
                    static_cast<long long>(
                        nearMaximumX - nearMinimumX)
                    * 35;

                Stage("frame-gi-off");
                const auto giOffFrame = renderComposedFrame();
                DumpFrame("gi-off", giOffFrame);

                // ①有効でもデータが無ければ何も変わらないこと。
                auto giSettings =
                    scene.BakedGlobalIllumination();
                giSettings.enabled = true;
                // ボリュームは**壁のある左半分だけ**を覆います。
                // 「箱の外の床は1画素も変わらない」を遠い帯の検査に
                // するためです（縁のフェードの検証も兼ねます）。
                giSettings.center = { -3.0f, 1.0f, 0.0f };
                giSettings.size = { 10.0f, 6.0f, 16.0f };
                giSettings.resolutionX = 4;
                giSettings.resolutionY = 2;
                giSettings.resolutionZ = 4;
                giSettings.intensity = 1.0f;
                scene.SetBakedGlobalIlluminationSettings(
                    giSettings);
                const auto giNoDataFrame =
                    renderComposedFrame();
                Require(
                    giNoDataFrame == giOffFrame,
                    "Enabling GI without baked data must not"
                    " change the frame.");

                // ②ベイク。毎フレーム数点ずつ進むので、終わるまで
                // 描画を回します（4x2x4=32点なので数フレーム）。
                scene.RequestBakedGlobalIlluminationBake();
                Stage("frame-gi-bake");
                int bakeFrames = 0;
                while (
                    scene.BakedGlobalIlluminationBakeProgress()
                        >= 0.0f)
                {
                    static_cast<void>(renderComposedFrame());
                    Require(
                        ++bakeFrames < 64,
                        "The GI bake must finish within a"
                        " bounded number of frames.");
                }
                Require(
                    scene.HasBakedGlobalIllumination(),
                    "The GI bake must produce data.");
                Stage("frame-gi-on");
                const auto giOnFrame = renderComposedFrame();
                DumpFrame("gi-on", giOnFrame);

                const long long nearBefore =
                    redSkew(giOffFrame,
                        nearMinimumX, nearMaximumX);
                const long long nearAfter =
                    redSkew(giOnFrame,
                        nearMinimumX, nearMaximumX);
                const long long farBefore =
                    redSkew(giOffFrame,
                        farMinimumX, farMaximumX);
                const long long farAfter =
                    redSkew(giOnFrame,
                        farMinimumX, farMaximumX);
                std::cout
                    << "gi red skew near wall: "
                    << nearBefore << " -> " << nearAfter
                    << ", far (outside volume): "
                    << farBefore << " -> " << farAfter
                    << std::endl;
                // どこで効いているかの診断（8画素刻みの列プロファイル。
                // 帯の位置がずれたときに、この数字から選び直します）。
                std::cout << "gi column profile:";
                for (std::uint32_t column = 0;
                    column < 320;
                    column += 8)
                {
                    std::cout
                        << ' '
                        << (redSkew(giOnFrame,
                                column, column + 8)
                            - redSkew(giOffFrame,
                                column, column + 8))
                            / (8 * 35);
                }
                std::cout << std::endl;
                // 壁のそばは1画素あたり2以上赤くなること
                // （閾値は1画素あたりで決めます）。
                Require(
                    nearAfter - nearBefore > bandPixels * 2,
                    "GI must bounce red light onto the floor"
                    " near the wall.");
                // ボリュームの外の床は（1画素あたり1未満まで）
                // 変わらないこと。縁のフェードの外では重みが
                // ちょうど0になり、従来のAmbientがそのまま
                // 使われるためです。
                Require(
                    std::abs(farAfter - farBefore)
                        < bandPixels,
                    "The floor outside the GI volume must"
                    " stay unchanged.");

                // ③保存→読み込みで同じ間接光が再現されること。
                // この場でシーンを丸ごとJSONにし、テストの最後に
                // 別のSceneへ読み込んで見比べます（今のSceneを
                // 壊すと以降の節が使えなくなるため、検証は最後です）。
                giRoundTripJson = scene.SerializeToJson();
                giRoundTripExpectedNear =
                    nearAfter - nearBefore;

                // 後片付け。表示は無効へ戻します（焼き込みデータは
                // 残りますが、enabledが偽なら描画へ影響しません）。
                giSettings.enabled = false;
                scene.SetBakedGlobalIlluminationSettings(
                    giSettings);
                static_cast<void>(
                    scene.DestroyGameObject(redWall));
                subject.GetTransform().position =
                    savedSubjectPosition;
                clone.GetTransform().position =
                    giSavedClonePosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraRotation);
                const auto giRestoredFrame =
                    renderComposedFrame();
                static_cast<void>(giRestoredFrame);
            }

            // レトロ3D（PS1風）Shaderの確認。テクスチャの泳ぎは
            // 「大きなポリゴンを浅い角度で見る」ときに一番出るので、
            // 巨大なPlaneを1枚敷いて低い視点から見ます。UVを細かく
            // 繰り返して市松模様にし、直線が折れる様子を見えるように
            // します（アルベド画像が無くてもUVスケールで格子は作れ
            // ないため、色の量子化と頂点スナップが主な判定材料）。
            {
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.6f, 6.0f };
                cameraObject.GetTransform().SetEulerAngles({ -0.08f, 0.0f, 0.0f });

                auto& retroFloor =
                    scene.CreateGameObject("RetroFloor");
                retroFloor.GetTransform().position =
                    { 0.0f, -1.0f, -10.0f };
                retroFloor.GetTransform().scale =
                    { 40.0f, 1.0f, 60.0f };
                auto& retroRenderer =
                    retroFloor.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Plane,
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f, 1.0f },
                        // テクスチャが無いと泳ぎは目に見えません
                        // （歪むのはUVなので、模様が必要です）。
                        std::filesystem::path{ "textures" }
                            / "directxtk.jpg");
                retroRenderer.SetShaderPath(
                    std::filesystem::path{ "shaders" }
                        / "LamaPonRetro3D.hlsl");
                // 泳ぎ1.0 / スナップ1.0 / 格子80 / 色8段
                retroRenderer.SetCustomParameter(
                    0,
                    DirectX::XMFLOAT4{
                        1.0f, 1.0f, 80.0f, 8.0f });
                // ディザ0.5 / 明るさ1.0
                retroRenderer.SetCustomParameter(
                    1,
                    DirectX::XMFLOAT4{
                        0.5f, 1.0f, 0.0f, 0.0f });
                // UVを4回繰り返して、折れ目が見えるようにします。
                retroRenderer.SetCustomParameter(
                    2,
                    DirectX::XMFLOAT4{
                        4.0f, 4.0f, 0.0f, 0.0f });

                auto& retroBlock =
                    scene.CreateGameObject("RetroBlock");
                retroBlock.GetTransform().position =
                    { 1.4f, -0.4f, -3.0f };
                retroBlock.GetTransform().scale =
                    { 1.2f, 1.2f, 1.2f };
                auto& retroBlockRenderer =
                    retroBlock.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Cube,
                        DirectX::XMFLOAT4{
                            0.9f, 0.3f, 0.25f, 1.0f });
                retroBlockRenderer.SetShaderPath(
                    std::filesystem::path{ "shaders" }
                        / "LamaPonRetro3D.hlsl");
                retroBlockRenderer.SetCustomParameter(
                    0,
                    DirectX::XMFLOAT4{
                        1.0f, 1.0f, 80.0f, 8.0f });
                retroBlockRenderer.SetCustomParameter(
                    1,
                    DirectX::XMFLOAT4{
                        0.5f, 1.0f, 0.0f, 0.0f });

                Stage("frame-retro3d-on");
                DumpFrame(
                    "retro3d-on",
                    renderComposedFrame());

                // どのパラメーターが効いているかを切り分けます。
                // 泳ぎだけ切る／スナップだけ切る／両方切る の3枚。
                const auto setRetroParameters =
                    [&retroRenderer, &retroBlockRenderer](
                        const float warp,
                        const float snap)
                    {
                        const DirectX::XMFLOAT4 value{
                            warp, snap, 80.0f, 8.0f };
                        retroRenderer.SetCustomParameter(
                            0, value);
                        retroBlockRenderer
                            .SetCustomParameter(0, value);
                    };

                setRetroParameters(0.0f, 1.0f);
                Stage("frame-retro3d-nowarp");
                DumpFrame(
                    "retro3d-nowarp",
                    renderComposedFrame());

                setRetroParameters(1.0f, 0.0f);
                Stage("frame-retro3d-nosnap");
                DumpFrame(
                    "retro3d-nosnap",
                    renderComposedFrame());

                setRetroParameters(0.0f, 0.0f);
                Stage("frame-retro3d-off");
                DumpFrame(
                    "retro3d-off",
                    renderComposedFrame());

                // 最後にONへ戻して、描画が復活するか確認します
                // （復活すればパラメーター依存、しなければ状態依存）。
                setRetroParameters(1.0f, 1.0f);
                Stage("frame-retro3d-onagain");
                DumpFrame(
                    "retro3d-onagain",
                    renderComposedFrame());

                static_cast<void>(
                    scene.DestroyGameObject(retroBlock));
                static_cast<void>(
                    scene.DestroyGameObject(retroFloor));
                subject.GetTransform().position =
                    savedSubjectPosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(savedCameraRotation);
            }

            // ノイズのCPU/GPU一致。C++（LamaPon::Noise）とHLSL
            // （LamaPonNoise.hlsli）は同じ値を返す約束なので、
            // 実際に突き合わせます。ここがずれると「C++で地形を
            // 作り、シェーダーで同じノイズを使う」が成立しません。
            //
            // 画面全体へノイズの値を書き出す専用Shaderを1枚かけて、
            // 中央のピクセルを読み取ります。8bitバッファなので
            // R=上位、G=下位に分けて精度を確保しています。
            {
                struct NoiseCase final
                {
                    const char* name;
                    float x;
                    float y;
                    float kind;
                    float octaves;
                    float z;
                    float expected;
                };
                const NoiseCase noiseCases[]{
                    { "Value2D", 3.25f, -7.5f, 0.0f, 1.0f,
                        0.0f,
                        LamaPon::Noise::Value2D(
                            3.25f, -7.5f) },
                    { "Value2D(negative)", -12.75f, -0.25f,
                        0.0f, 1.0f, 0.0f,
                        LamaPon::Noise::Value2D(
                            -12.75f, -0.25f) },
                    { "Perlin2D", 5.5f, 2.125f, 1.0f, 1.0f,
                        0.0f,
                        LamaPon::Noise::Perlin2D(
                            5.5f, 2.125f) },
                    { "Fractal(5)", 1.5f, 9.25f, 2.0f, 5.0f,
                        0.0f,
                        LamaPon::Noise::FractalValue2D(
                            1.5f, 9.25f, 5) },
                    { "Worley2D", 4.125f, 6.75f, 3.0f, 1.0f,
                        0.0f,
                        LamaPon::Noise::Worley2D(
                            4.125f, 6.75f) },
                    { "Value1D", 17.375f, 0.0f, 4.0f, 1.0f,
                        0.0f,
                        LamaPon::Noise::Value1D(17.375f) },
                    { "Value3D", 2.5f, -3.25f, 5.0f, 1.0f,
                        8.125f,
                        LamaPon::Noise::Value3D(
                            2.5f, -3.25f, 8.125f) }
                };

                const auto probeShader =
                    std::filesystem::path{
                        LAMAPON_TEST_FIXTURE_DIR }
                    / "noise-probe.hlsl";
                bool probeUsable = true;
                for (const auto& noiseCase : noiseCases)
                {
                    LamaPon::ScreenEffectRequest request;
                    request.shader = probeShader;
                    request.customParameters[0] = {
                        noiseCase.x,
                        noiseCase.y,
                        noiseCase.kind,
                        noiseCase.octaves
                    };
                    request.customParameters[1] = {
                        noiseCase.z, 0.0f, 0.0f, 0.0f
                    };
                    std::string shaderError;
                    if (!graphics.QueueScreenEffect(
                            request,
                            nullptr,
                            &shaderError))
                    {
                        std::cout
                            << "noise probe unavailable: "
                            << shaderError << std::endl;
                        probeUsable = false;
                        break;
                    }

                    const auto frame =
                        renderComposedFrame();
                    const auto& pixel = At(
                        frame,
                        Width / 2,
                        Height / 2);
                    // R=上位（1/255刻み）、G=下位。
                    const float measured =
                        (static_cast<float>(pixel.red)
                            + static_cast<float>(
                                pixel.green) / 255.0f)
                        / 255.0f;
                    const float difference = std::abs(
                        measured - noiseCase.expected);
                    std::cout
                        << "noise " << noiseCase.name
                        << ": cpu="
                        << noiseCase.expected
                        << " gpu=" << measured
                        << " diff=" << difference
                        << std::endl;
                    // ここのRequireはconst char*を取るため、
                    // 失敗した種類は上のログ行から読み取ります。
                    Require(
                        difference < 0.01f,
                        "CPU and GPU noise implementations"
                        " disagree (see the noise log lines"
                        " above for which one).");
                }
                static_cast<void>(probeUsable);
            }

            // ScreenEffectがシーンの深度（t3）を読めることの確認。
            // 手前に箱を置き、その画素と背景（何も無い＝遠平面）の
            // 画素で距離が違うことを見ます。
            //
            // 深度が刺さっていないとShaderは0を読み、距離はニア
            // クリップ面に張り付いて画面が一様になります。つまり
            // 「手前と奥の差が出ない」が配線が外れたときの症状で、
            // ここが0に近ければ落ちます。
            {
                subject.GetTransform().position =
                    savedSubjectPosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraRotation);

                constexpr float depthProbeRange = 40.0f;
                const auto probeFrame =
                    [&](const float rawMode)
                {
                    LamaPon::ScreenEffectRequest request;
                    request.shader =
                        std::filesystem::path{
                            LAMAPON_TEST_FIXTURE_DIR }
                        / "depth-probe.hlsl";
                    request.customParameters[0] = {
                        depthProbeRange,
                        rawMode,
                        0.0f,
                        0.0f
                    };
                    std::string shaderError;
                    Require(
                        graphics.QueueScreenEffect(
                            request,
                            nullptr,
                            &shaderError),
                        "The depth probe screen effect must"
                        " compile.");
                    return renderComposedFrame();
                };
                const auto decode =
                    [](const std::vector<std::uint8_t>& frame,
                        const std::uint32_t x,
                        const std::uint32_t y)
                {
                    const auto pixel = At(frame, x, y);
                    // 青一色は「深度が無効」の合図（fixture参照）。
                    // 値はR・Gにしか書かないので、Bが立っていたら
                    // それしかありません。
                    Require(
                        pixel.blue < 128,
                        "The screen effect reported that no"
                        " scene depth was bound.");
                    return (static_cast<float>(pixel.red)
                            + static_cast<float>(pixel.green)
                                / 255.0f)
                        / 255.0f;
                };

                Stage("frame-screeneffect-depth");
                const auto rawFrame = probeFrame(1.0f);
                const float rawCenter =
                    decode(rawFrame, Width / 2, Height / 2);
                const float rawCorner = decode(rawFrame, 4, 4);
                const auto depthFrame = probeFrame(0.0f);
                DumpFrame(
                    "screeneffect-depth",
                    depthFrame);
                // 特定の画素を決め打ちすると、カメラの向きが変わった
                // だけで「たまたま両方とも背景」になり、何も検査
                // しない段に化けます（実際そうなりました）。画面
                // 全体の最小・最大で見ます。
                float nearDistance = depthProbeRange;
                float farDistance = 0.0f;
                for (std::uint32_t y = 0; y < Height; ++y)
                {
                    for (std::uint32_t x = 0;
                        x < Width;
                        ++x)
                    {
                        const float value =
                            decode(depthFrame, x, y)
                            * depthProbeRange;
                        nearDistance =
                            std::min(nearDistance, value);
                        farDistance =
                            std::max(farDistance, value);
                    }
                }
                // 2点だけ見ると「たまたま両方とも背景だった」のか
                // 「深度が空」なのか区別できません。全画素の幅を
                // 見れば一発で分かります。
                float rawMinimum = 1.0f;
                float rawMaximum = 0.0f;
                for (std::uint32_t y = 0; y < Height; ++y)
                {
                    for (std::uint32_t x = 0;
                        x < Width;
                        ++x)
                    {
                        const auto pixel = At(rawFrame, x, y);
                        const float value =
                            (static_cast<float>(pixel.red)
                                + static_cast<float>(
                                    pixel.green) / 255.0f)
                            / 255.0f;
                        rawMinimum =
                            std::min(rawMinimum, value);
                        rawMaximum =
                            std::max(rawMaximum, value);
                    }
                }
                const auto& probeProjection =
                    graphics.SceneProjection();
                std::cout
                    << "screen effect depth: raw "
                    << rawMinimum << ".." << rawMaximum
                    << " (center " << rawCenter
                    << ", corner " << rawCorner
                    << ") | distance "
                    << nearDistance
                    << "m.." << farDistance
                    << "m | projection _33="
                    << probeProjection._33
                    << " _43=" << probeProjection._43
                    << std::endl;

                // ①深度が読めていること。画面のどこかに遠平面(1.0)
                // より手前の画素があれば配線は通っています。
                // 全面1.0なら深度が空です。
                Require(
                    rawMinimum < 0.99f,
                    "The screen effect must read scene"
                    " geometry depth, not an empty buffer.");
                // ②手前の物と背景が区別できていること。
                Require(
                    nearDistance < depthProbeRange * 0.5f
                        && farDistance
                            > depthProbeRange * 0.95f,
                    "The screen effect must separate near"
                    " geometry from the far background.");
                // ③距離の式そのものの検査。ドキュメントに載せた式で
                // CPU側でも同じ距離になることを見ます。符号を1つ
                // 間違えるだけで落ちるので、ここが式の正解表です
                // （射影は右手系なので_33も_43も負、分母は普段
                // マイナスで遠平面で0へ近づきます）。
                const float expectedNearest =
                    probeProjection._43
                    / (rawMinimum + probeProjection._33);
                std::cout
                    << "screen effect depth formula: shader="
                    << nearDistance
                    << "m cpu=" << expectedNearest
                    << "m" << std::endl;
                Require(
                    std::abs(nearDistance - expectedNearest)
                        < expectedNearest * 0.05f,
                    "The documented depth linearisation must"
                    " reproduce what the shader computed.");

                // ④深度から組み立てた法線。単位ベクトルになって
                // いること、かつ画面内で向きが変わっていることを
                // 見ます。共有実装が壊れると長さが1から外れるか、
                // 全画素が同じ向きになります。
                const auto normalFrame = probeFrame(2.0f);
                DumpFrame(
                    "screeneffect-normal",
                    normalFrame);
                std::size_t normalSamples = 0;
                double lengthTotal = 0.0;
                float normalMinimumZ = 1.0f;
                float normalMaximumZ = -1.0f;
                for (std::uint32_t y = 1;
                    y + 1 < Height;
                    ++y)
                {
                    for (std::uint32_t x = 1;
                        x + 1 < Width;
                        ++x)
                    {
                        // ジオメトリのある画素だけ見ます（空は
                        // 遠平面どうしなので法線が定義できません）。
                        const auto rawPixel =
                            At(rawFrame, x, y);
                        const float rawValue =
                            (static_cast<float>(rawPixel.red)
                                + static_cast<float>(
                                    rawPixel.green) / 255.0f)
                            / 255.0f;
                        if (rawValue > 0.999f)
                        {
                            continue;
                        }
                        const auto pixel =
                            At(normalFrame, x, y);
                        const float nx =
                            static_cast<float>(pixel.red)
                                / 255.0f * 2.0f - 1.0f;
                        const float ny =
                            static_cast<float>(pixel.green)
                                / 255.0f * 2.0f - 1.0f;
                        const float nz =
                            static_cast<float>(pixel.blue)
                                / 255.0f * 2.0f - 1.0f;
                        ++normalSamples;
                        lengthTotal += std::sqrt(
                            static_cast<double>(
                                nx * nx + ny * ny + nz * nz));
                        normalMinimumZ =
                            std::min(normalMinimumZ, nz);
                        normalMaximumZ =
                            std::max(normalMaximumZ, nz);
                    }
                }
                const double averageLength =
                    normalSamples > 0
                    ? lengthTotal
                        / static_cast<double>(normalSamples)
                    : 0.0;
                std::cout
                    << "screen effect normal: samples="
                    << normalSamples
                    << " average length=" << averageLength
                    << " z range " << normalMinimumZ
                    << ".." << normalMaximumZ
                    << std::endl;

                Require(
                    normalSamples > 500,
                    "The scene must cover enough pixels for"
                    " the normal check to mean anything.");
                // 8bitへ丸めるので0.02ほどはずれます。
                Require(
                    std::abs(averageLength - 1.0) < 0.1,
                    "Normals reconstructed from depth must"
                    " be unit length.");
                Require(
                    normalMaximumZ - normalMinimumZ > 0.05f,
                    "Reconstructed normals must vary across"
                    " the scene, not be a constant vector.");
            }

            // ノイズの見本Shaderを5種類ぶん焼いて目視できるように
            // します（数値の一致は上の段で見たので、ここは
            // 「意図した模様になっているか」の確認用）。
            {
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.0f, 3.2f };
                cameraObject.GetTransform().SetEulerAngles({ 0.0f, 0.0f, 0.0f });

                auto& noisePlane =
                    scene.CreateGameObject("NoisePlane");
                noisePlane.GetTransform().position =
                    { 0.0f, 0.0f, 0.0f };
                // Planeは寝ているので、立ててカメラへ向けます。
                noisePlane.GetTransform().SetEulerAngles({ -1.5707963f, 0.0f, 0.0f });
                noisePlane.GetTransform().scale =
                    { 4.0f, 1.0f, 2.4f };
                auto& noiseRenderer =
                    noisePlane.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Plane,
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f, 1.0f });
                noiseRenderer.SetShaderPath(
                    std::filesystem::path{ "shaders" }
                        / "LamaPonNoiseSample.hlsl");
                noiseRenderer.SetCustomParameter(
                    1,
                    DirectX::XMFLOAT4{
                        1.0f, 1.0f, 0.0f, 0.0f });
                noiseRenderer.SetCustomParameter(
                    2,
                    DirectX::XMFLOAT4{
                        0.05f, 0.12f, 0.25f, 1.0f });
                noiseRenderer.SetCustomParameter(
                    3,
                    DirectX::XMFLOAT4{
                        0.95f, 0.9f, 0.7f, 1.0f });

                const char* const noiseNames[]{
                    "noise-value",
                    "noise-perlin",
                    "noise-fractal",
                    "noise-worley",
                    "noise-curl"
                };
                for (int kind = 0; kind < 5; ++kind)
                {
                    noiseRenderer.SetCustomParameter(
                        0,
                        DirectX::XMFLOAT4{
                            static_cast<float>(kind),
                            8.0f,
                            5.0f,
                            0.0f });
                    Stage(noiseNames[kind]);
                    DumpFrame(
                        noiseNames[kind],
                        renderComposedFrame());
                }

                static_cast<void>(
                    scene.DestroyGameObject(noisePlane));
                subject.GetTransform().position =
                    savedSubjectPosition;
                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(savedCameraRotation);
            }

            // 水面の見本Shader。ノイズで作った波の傾きで太陽と空を
            // 反射するので、「面全体が一様でないこと」が最低条件です
            // （一様なら波の法線が出ていない＝計算が死んでいる）。
            {
                const auto savedSubject =
                    subject.GetTransform().position;
                const auto savedCamera =
                    cameraObject.GetTransform().position;
                const auto savedCameraAngles =
                    cameraObject.GetTransform().EulerAngles();
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                // SubjectCloneは最初の段で作った赤いキューブで、
                // 誰も片付けないまま残っています。水面の絵に写り
                // 込むと「これは何だ」となるので一緒に退かします。
                const auto savedClonePosition =
                    clone.GetTransform().position;
                clone.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                // 太陽の反射（光の帯）が画面の真ん中に来る配置に
                // します。太陽の高度とカメラの伏せ角を揃えるのが
                // 条件で、どちらも約30度にしてあります。ずれると
                // 帯は水平線の向こうへ行き、絵に写りません。
                cameraObject.GetTransform().position =
                    { 0.0f, 3.0f, 7.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { -0.38f, 0.0f, 0.0f });
                sunObject.SetEnabled(true);
                // 太陽をカメラの向こう側・低い位置へ置きます。
                // 水面の反射は「太陽が自分の向こうにあるとき」しか
                // 見えません（背中側にあると光の帯は水平線の裏へ
                // 行きます）。WorldDirection()は光の進む向きで、
                // yaw=πのとき (0, sin(pitch), cos(pitch)) です。
                const auto savedSunAngles =
                    sunObject.GetTransform().EulerAngles();
                sunObject.GetTransform().SetEulerAngles(
                    { -0.5236f, 3.14159265f, 0.0f });

                auto& waterPlane =
                    scene.CreateGameObject("WaterPlane");
                waterPlane.GetTransform().position =
                    { 0.0f, 0.0f, 0.0f };
                waterPlane.GetTransform().scale =
                    { 12.0f, 1.0f, 12.0f };
                auto& waterRenderer =
                    waterPlane.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Plane,
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f, 1.0f });
                waterRenderer.SetShaderPath(
                    std::filesystem::path{ "shaders" }
                        / "LamaPonWater.hlsl");
                // LAMAPON_PROPERTIESの既定値を流し込むのは
                // Inspector（EditorLayer::ApplyShaderPropertyDefaults）
                // なので、コードから割り当てるここでは自分で渡します。
                // 渡さないと全部0＝真っ黒になり、しかも「絵が出て
                // いる」ので気付きにくい状態になります。
                waterRenderer.SetCustomParameter(
                    0,
                    DirectX::XMFLOAT4{
                        0.25f, 1.1f, 1.0f, 0.6f });
                waterRenderer.SetCustomParameter(
                    1,
                    DirectX::XMFLOAT4{
                        0.35f, 1.0f, 0.35f, 0.0f });
                waterRenderer.SetCustomParameter(
                    2,
                    DirectX::XMFLOAT4{
                        0.10f, 0.42f, 0.45f, 0.72f });
                waterRenderer.SetCustomParameter(
                    3,
                    DirectX::XMFLOAT4{
                        0.01f, 0.09f, 0.16f, 1.0f });
                waterRenderer.SetCustomParameter(
                    4,
                    DirectX::XMFLOAT4{
                        0.24f, 0.45f, 0.85f, 1.0f });
                waterRenderer.SetCustomParameter(
                    5,
                    DirectX::XMFLOAT4{
                        0.72f, 0.82f, 0.93f, 1.0f });

                Stage("frame-water");
                const auto waterFrame = renderComposedFrame();
                DumpFrame("water", waterFrame);

                // 水面が確実に写っている範囲だけを見ます。画面の
                // 下半分ぜんぶを測ると、水と空の境目の段差だけで
                // ばらつきが出てしまい、波が死んでいても閾値を
                // 超えてしまいます（実際に一度これで見逃しました）。
                double sum{};
                double sumSquared{};
                double peak{};
                int samples{};
                for (std::uint32_t y = 110; y < 175; y += 2)
                {
                    for (std::uint32_t x = 40; x < 280; x += 2)
                    {
                        const auto& pixel = At(waterFrame, x, y);
                        const double luminance =
                            0.299 * pixel.red
                            + 0.587 * pixel.green
                            + 0.114 * pixel.blue;
                        sum += luminance;
                        sumSquared += luminance * luminance;
                        peak = std::max(peak, luminance);
                        ++samples;
                    }
                }
                const double mean = sum / samples;
                const double variance =
                    sumSquared / samples - mean * mean;
                const double deviation =
                    variance > 0.0 ? std::sqrt(variance) : 0.0;
                std::cout
                    << "water: mean=" << mean
                    << " stddev=" << deviation
                    << " peak=" << peak
                    << std::endl;
                // 太陽の反射（光の帯）が出ていること。これが
                // この見本の主役なので、水色が出ているだけでは
                // 通さないようにします。空の映り込みだけなら
                // 200前後までしか上がりません。
                // 実測値: 太陽の反射が出ているとき peak=226 /
                // stddev=26。法線が裏返って反射が消えていたときは
                // peak=179 / stddev=7.1 でした。その間に置きます。
                Require(
                    peak > 205.0,
                    "The water sample shader must show the"
                    " sun's reflection as a bright highlight.");
                // 2つ見ます。meanは「そもそも色が出ているか」
                // （パラメーターが0のままだと真っ黒＝mean 2以下に
                // なります）。deviationは「波が効いているか」
                // （法線が全部真上なら映り込みが一様になります）。
                Require(
                    mean > 20.0,
                    "The water sample shader must render a"
                    " visible surface, not near-black.");
                Require(
                    deviation > 15.0,
                    "The water sample shader must produce"
                    " a varying surface (waves), not a flat"
                    " single tone.");

                static_cast<void>(
                    scene.DestroyGameObject(waterPlane));
                clone.GetTransform().position =
                    savedClonePosition;
                sunObject.GetTransform().SetEulerAngles(
                    savedSunAngles);
                sunObject.SetEnabled(false);
                subject.GetTransform().position = savedSubject;
                cameraObject.GetTransform().position =
                    savedCamera;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraAngles);
            }

            // 太陽の角度サイズ。つるつるの球で、見かけの大きさを
            // 0度（点）／0.53度（本物の太陽）／8度（曇り）と振り、
            // ハイライトが広がることを画素数で測ります。
            {
                const auto savedSubject =
                    subject.GetTransform().position;
                const auto savedCamera =
                    cameraObject.GetTransform().position;
                const auto savedCameraAngles =
                    cameraObject.GetTransform().EulerAngles();
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                const auto savedCloneForSize =
                    clone.GetTransform().position;
                clone.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                // 測る相手は球ではなく「寝かせた平面」です。球だと
                // 法線が画素ごとに大きく回るので、0.53度ぶんの反射は
                // 1画素未満に潰れて何も測れません（実際に球で試して
                // 3つとも0になりました）。平面なら反射の向きが
                // ゆっくり変わるので、太陽の円盤が横に伸びた光の帯
                // として広い面積に写ります。
                cameraObject.GetTransform().position =
                    { 0.0f, 1.2f, 6.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { -0.16f, 0.0f, 0.0f });
                sunObject.SetEnabled(true);
                scene.SetAmbientLightIntensity(0.02f);
                // 水面と同じ理由で、太陽はカメラの向こう側・低い
                // 位置に置きます。背中側にあると鏡の面には太陽が
                // 一切写らず、角度を変えても3つとも真っ暗な同じ絵に
                // なります（実際にこれで空振りしました）。
                const auto savedSunAnglesForSize =
                    sunObject.GetTransform().EulerAngles();
                sunObject.GetTransform().SetEulerAngles(
                    { -0.305f, 3.14159265f, 0.0f });

                auto& mirror =
                    scene.CreateGameObject("SunSizeMirror");
                mirror.GetTransform().scale =
                    { 12.0f, 1.0f, 12.0f };
                auto& mirrorRenderer =
                    mirror.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Plane,
                        // 磨いた金属です。暗い金属にすると反射率
                        // （F0＝albedo）ごと下がってハイライトまで
                        // 暗くなり、広がりが測れなくなります。
                        DirectX::XMFLOAT4{
                            0.95f, 0.95f, 0.95f, 1.0f },
                        std::filesystem::path{},
                        std::filesystem::path{},
                        // 粗さ0.2です。完全な鏡（0.04）にすると、
                        // 点光源のハイライトが1画素より細くなって
                        // サンプリングから漏れ、比べる相手が「たまたま
                        // 拾えた10」のような値になってしまいます。
                        // 少しざらつかせて、両方が確実に画素へ乗る
                        // ようにしています。
                        0.2f,
                        1.0f);
                // 6番目の引数はnormalStrengthでmetallicではありません。
                // 金属度を上げないと拡散反射が残り、面全体がN･Lの
                // 明るさで埋まってハイライトの広がりを測れません。
                mirrorRenderer.SetMetallic(1.0f);

                // ハイライトの「広がり」を画素数で測ります。合計
                // 輝度ではないのは、代表点法がピークの高さではなく
                // 広がりを変える手法だからです（エネルギーは保存
                // するので、合計はあまり動きません）。
                const auto countHighlight =
                    [&](const char* name,
                        int& maximumLuminance) -> int
                {
                    const auto frame = renderComposedFrame();
                    DumpFrame(name, frame);
                    int bright{};
                    maximumLuminance = 0;
                    // 鏡の面が写っている範囲だけを見ます。画面全体
                    // だと明るい空が数万画素ぶん混ざって、
                    // ハイライトの差が完全に埋もれます（一度これで
                    // 3つとも同じ43188という数字になりました）。
                    for (std::uint32_t y = 100; y < Height; ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            ++x)
                        {
                            const auto& pixel =
                                At(frame, x, y);
                            const int luminance =
                                (static_cast<int>(pixel.red)
                                    * 299
                                + static_cast<int>(
                                    pixel.green) * 587
                                + static_cast<int>(
                                    pixel.blue) * 114)
                                / 1000;
                            maximumLuminance = std::max(
                                maximumLuminance,
                                luminance);
                            // 金属面は太陽の反射以外ほぼ真っ暗に
                            // なるので、境目は低めで足ります。
                            if (luminance > 40)
                            {
                                ++bright;
                            }
                        }
                    }
                    return bright;
                };
                int pointPeak{};
                int realPeak{};
                int widePeak{};

                sun.SetAngularDiameterDegrees(0.0f);
                Stage("frame-sun-point");
                const int pointHighlight =
                    countHighlight("sun-size-point", pointPeak);

                sun.SetAngularDiameterDegrees(0.53f);
                Stage("frame-sun-real");
                const int realHighlight =
                    countHighlight("sun-size-real", realPeak);

                sun.SetAngularDiameterDegrees(8.0f);
                Stage("frame-sun-wide");
                const int wideHighlight =
                    countHighlight("sun-size-wide", widePeak);

                std::cout
                    << "sun highlight pixels: point="
                    << pointHighlight
                    << " real=" << realHighlight
                    << " wide=" << wideHighlight
                    << " | peak: point=" << pointPeak
                    << " real=" << realPeak
                    << " wide=" << widePeak
                    << std::endl;
                // 角度を広げるほどハイライトは広がります。実測は
                // point=5860 / wide=6360 で、差は約500です。角半径が
                // シェーダーへ届いていないと3つとも完全に同じ数に
                // なる（差0）ので、その間に200を置いています。
                //
                // realがpointとほぼ同じ（5818対5860）なのは正しい
                // 挙動です。粗さ0.2の面では材質のざらつきのほうが
                // 太陽の0.53度よりずっと広く、円盤の大きさは埋もれ
                // ます。太陽の大きさが効くのは磨かれた面だけです。
                Require(
                    wideHighlight > pointHighlight + 200,
                    "A larger angular diameter must widen the"
                    " specular highlight.");

                static_cast<void>(
                    scene.DestroyGameObject(mirror));
                clone.GetTransform().position =
                    savedCloneForSize;
                sun.SetAngularDiameterDegrees(0.53f);
                sunObject.GetTransform().SetEulerAngles(
                    savedSunAnglesForSize);
                sunObject.SetEnabled(false);
                scene.SetAmbientLightIntensity(0.35f);
                subject.GetTransform().position = savedSubject;
                cameraObject.GetTransform().position =
                    savedCamera;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraAngles);
            }

            // Screen Space Lens Flare。明るい点から出るゴーストと
            // ハローなので、「切ったときとの差」で確かめます。
            // 太陽を画面へ入れて光源を作ります。
            {
                const auto savedSubject =
                    subject.GetTransform().position;
                const auto savedClone =
                    clone.GetTransform().position;
                const auto savedCamera =
                    cameraObject.GetTransform().position;
                const auto savedCameraAngles =
                    cameraObject.GetTransform().EulerAngles();
                const auto savedSunAngles =
                    sunObject.GetTransform().EulerAngles();
                const auto savedSky = scene.Sky();
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                clone.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.0f, 6.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { 0.12f, 0.0f, 0.0f });
                sunObject.SetEnabled(true);
                // 太陽をカメラの正面・少し上へ置いて空に描かせます。
                sunObject.GetTransform().SetEulerAngles(
                    { -0.22f, 3.14159265f, 0.0f });
                auto flareSky = scene.Sky();
                flareSky.enabled = true;
                flareSky.sunDriven = true;
                flareSky.cubemapPath.clear();
                scene.SetSkySettings(flareSky);

                auto flare = scene.ScreenSpaceLensFlare();
                flare.enabled = false;
                scene.SetScreenSpaceLensFlareSettings(flare);
                Stage("frame-lensflare-off");
                const auto offFrame = renderComposedFrame();
                DumpFrame("lensflare-off", offFrame);

                flare.enabled = true;
                scene.SetScreenSpaceLensFlareSettings(flare);
                Stage("frame-lensflare-on");
                const auto onFrame = renderComposedFrame();
                DumpFrame("lensflare-on", onFrame);

                // 1画素あたりの増分で見ます。合計値へ定数を置くと、
                // 効いていなくても画素数で越えてしまいます。
                double added{};
                int changed{};
                for (std::uint32_t y = 0; y < Height; ++y)
                {
                    for (std::uint32_t x = 0; x < Width; ++x)
                    {
                        const auto& before = At(offFrame, x, y);
                        const auto& after = At(onFrame, x, y);
                        const int delta =
                            (static_cast<int>(after.red)
                                - static_cast<int>(before.red))
                            + (static_cast<int>(after.green)
                                - static_cast<int>(
                                    before.green))
                            + (static_cast<int>(after.blue)
                                - static_cast<int>(
                                    before.blue));
                        if (delta > 6)
                        {
                            ++changed;
                            added += delta;
                        }
                    }
                }
                const double perPixel = changed > 0
                    ? added / changed
                    : 0.0;
                std::cout
                    << "lens flare: changed=" << changed
                    << " perPixel=" << perPixel
                    << std::endl;
                // レンズフレアは画面の一部にだけ出るものなので、
                // 「広い面積が薄く変わる」ではなく「狭い面積が
                // はっきり明るくなる」ことを見ます。
                //
                // 実測: changed=44 / perPixel=41。320x180で太陽が
                // 小さいため面積は狭く出ます。切ると両方0になるので、
                // その間に置けば空振りしません。
                Require(
                    changed > 20,
                    "Screen space lens flare must brighten a"
                    " visible area of the frame.");
                Require(
                    perPixel > 20.0,
                    "Screen space lens flare must be visible"
                    " per pixel, not a faint wash.");

                // アナモルフィック風（十字に長く伸びる筋）。
                // 多段パスが効いていないと筋が出ないので、ゴースト
                // だけの上の状態と比べて差が出ることを見ます。
                auto anamorphic = flare;
                anamorphic.enabled = true;
                anamorphic.threshold = 1.0f;
                anamorphic.intensity = 1.0f;
                anamorphic.streakIntensity = 2.5f;
                anamorphic.streakLength = 0.7f;
                anamorphic.streakDirections = 2;
                anamorphic.streakAngleDegrees = 0.0f;
                anamorphic.chromaticAberration = 0.5f;
                scene.SetScreenSpaceLensFlareSettings(
                    anamorphic);
                Stage("frame-lensflare-anamorphic");
                const auto anamorphicFrame =
                    renderComposedFrame();
                DumpFrame(
                    "lensflare-anamorphic",
                    anamorphicFrame);

                int streakChanged{};
                for (std::uint32_t y = 0; y < Height; ++y)
                {
                    for (std::uint32_t x = 0; x < Width; ++x)
                    {
                        const auto& before = At(onFrame, x, y);
                        const auto& after =
                            At(anamorphicFrame, x, y);
                        const int delta =
                            (static_cast<int>(after.red)
                                - static_cast<int>(before.red))
                            + (static_cast<int>(after.green)
                                - static_cast<int>(
                                    before.green))
                            + (static_cast<int>(after.blue)
                                - static_cast<int>(
                                    before.blue));
                        if (delta > 6)
                        {
                            ++streakChanged;
                        }
                    }
                }
                std::cout
                    << "lens flare streaks: changed="
                    << streakChanged << std::endl;
                // 筋は画面を横切るので、ゴーストだけの44画素とは
                // 桁が違う面積になるはずです。多段パスが効かないと
                // ここが伸びません。
                Require(
                    streakChanged > 500,
                    "Multi-pass streaks must cover far more"
                    " of the frame than ghosts alone.");

                flare.enabled = false;
                scene.SetScreenSpaceLensFlareSettings(flare);
                scene.SetSkySettings(savedSky);
                sunObject.GetTransform().SetEulerAngles(
                    savedSunAngles);
                sunObject.SetEnabled(false);
                subject.GetTransform().position = savedSubject;
                clone.GetTransform().position = savedClone;
                cameraObject.GetTransform().position =
                    savedCamera;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraAngles);
            }

            // 被写界深度（DoF）。
            //
            // 「絵が変わったか」では検証になりません（画面全体を
            // ぼかす実装でも通ってしまう）。手前と奥に同じ立方体を
            // 置き、**ピント位置だけ**を入れ替えた2枚を撮って、
            // 「ピントを合わせた側の輪郭が鋭く、もう一方が甘い」を
            // 両方向で確かめます。この形なら
            //   ・何もしていない  → 2枚が同じで両方の比が1になる
            //   ・全体をぼかす    → 同じく比が1になる
            //   ・ピント位置を無視 → 同じく比が1になる
            // のいずれも落ちます。
            {
                const auto savedSubject =
                    subject.GetTransform().position;
                const auto savedSubjectScale =
                    subject.GetTransform().scale;
                const auto savedClone =
                    clone.GetTransform().position;
                const auto savedCloneScale =
                    clone.GetTransform().scale;
                const auto savedCamera =
                    cameraObject.GetTransform().position;
                const auto savedCameraAngles =
                    cameraObject.GetTransform().EulerAngles();
                const auto savedSky = scene.Sky();
                const auto savedTemporal =
                    scene.TemporalAntiAliasing();
                const auto savedQuality = graphics.Settings();

                // 背景を単色にします。空を描くとぼけても模様が
                // 変わらない領域が増えて、測る値が薄まります。
                auto flatSky = scene.Sky();
                flatSky.enabled = false;
                scene.SetSkySettings(flatSky);
                // TAAは前フレームと混ぜるので、1枚ずつ撮るこの検証
                // では2枚が互いに汚染します。ここだけ切ります。
                auto noTemporal = scene.TemporalAntiAliasing();
                noTemporal.enabled = false;
                scene.SetTemporalAntiAliasingSettings(
                    noTemporal);
                // 品質側のDoFを明示的に有効にします（プリセットの
                // 既定に依存させると、既定を変えた日に無言で
                // 空振りするテストになります）。
                auto depthOfFieldQuality = graphics.Settings();
                depthOfFieldQuality.depthOfFieldEnabled = true;
                depthOfFieldQuality.depthOfFieldSampleCount = 24;
                graphics.SetGraphicsSettings(
                    depthOfFieldQuality);

                // カメラは原点から-Zを向きます（既定の回転）。
                // 立方体は一辺2（scale 2の単位立方体）なので、
                // 手前の1個は深度5〜7、奥の1個は29〜31を占めます。
                cameraObject.GetTransform().position =
                    { 0.0f, 0.0f, 0.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { 0.0f, 0.0f, 0.0f });
                subject.GetTransform().position =
                    { -3.0f, 0.0f, -6.0f };
                subject.GetTransform().scale =
                    { 2.0f, 2.0f, 2.0f };
                clone.GetTransform().position =
                    { 3.5f, 0.0f, -30.0f };
                clone.GetTransform().scale =
                    { 2.0f, 2.0f, 2.0f };

                // 測る帯は実際のダンプ画像から決めました。縦画角45度・
                // 320x180で、手前の立方体は左端が画面外へ出て**右の
                // 輪郭が x=98 付近**に来ます（一番右へ出るのは手前の
                // 面の角ではなく**奥の面の角**です。中心軸から見て
                // 奥のほうが軸に近いため、投影は外側へ回ります）。
                // 奥の立方体は x=178〜194・y=83〜97 の小さな四角です。
                //
                // どちらの帯も**地面（y=118付近から下）と空の境目を
                // 含めない**ようにしています。地面は奥へ向かって深度が
                // 連続的に変わるため、ピント位置を動かすと地平線の
                // ぼけ具合まで一緒に動いて、立方体の寄与と混ざります。
                constexpr std::uint32_t NearMinimumX = 78;
                constexpr std::uint32_t NearMaximumX = 124;
                constexpr std::uint32_t NearMinimumY = 58;
                constexpr std::uint32_t NearMaximumY = 104;
                constexpr std::uint32_t FarMinimumX = 164;
                constexpr std::uint32_t FarMaximumX = 208;
                constexpr std::uint32_t FarMinimumY = 84;
                constexpr std::uint32_t FarMaximumY = 96;

                LamaPon::DepthOfFieldSettings depthOfField{};
                depthOfField.enabled = true;
                depthOfField.focusRange = 2.0f;
                depthOfField.blurStrength = 1.0f;
                depthOfField.maximumRadius = 12.0f;

                // ①手前にピント（深度5〜7が鋭い帯）。
                depthOfField.focusDistance = 6.0f;
                scene.SetDepthOfFieldSettings(depthOfField);
                Stage("frame-dof-focus-near");
                const auto focusNearFrame =
                    renderComposedFrame();
                DumpFrame("dof-focus-near", focusNearFrame);

                // ②奥にピント（深度29〜31が鋭い帯）。
                depthOfField.focusDistance = 30.0f;
                scene.SetDepthOfFieldSettings(depthOfField);
                Stage("frame-dof-focus-far");
                const auto focusFarFrame =
                    renderComposedFrame();
                DumpFrame("dof-focus-far", focusFarFrame);

                const double nearWhenFocusedNear =
                    RegionSharpness(
                        focusNearFrame,
                        NearMinimumX,
                        NearMaximumX,
                        NearMinimumY,
                        NearMaximumY);
                const double nearWhenFocusedFar =
                    RegionSharpness(
                        focusFarFrame,
                        NearMinimumX,
                        NearMaximumX,
                        NearMinimumY,
                        NearMaximumY);
                const double farWhenFocusedFar =
                    RegionSharpness(
                        focusFarFrame,
                        FarMinimumX,
                        FarMaximumX,
                        FarMinimumY,
                        FarMaximumY);
                const double farWhenFocusedNear =
                    RegionSharpness(
                        focusNearFrame,
                        FarMinimumX,
                        FarMaximumX,
                        FarMinimumY,
                        FarMaximumY);
                std::cout
                    << "depth of field: near sharpness "
                    << nearWhenFocusedNear
                    << " -> " << nearWhenFocusedFar
                    << " / far sharpness "
                    << farWhenFocusedFar
                    << " -> " << farWhenFocusedNear
                    << std::endl;

                // まず両方の帯に輪郭があること。帯の置き場所を間違えて
                // 立方体の内側（真っ平ら）だけを測っていると比の判定が
                // 無意味になるので、先に押さえます。実際に最初はこれで
                // 空振りし、帯が立方体の中に丸ごと入っていました
                // （鋭さ0.06＝ほぼ完全に平ら）。
                //
                // 実測: ピントの合っている側は手前438・奥904。外した
                // 側の39・52とは桁が違うので、100はその間に置けます。
                Require(
                    nearWhenFocusedNear > 100.0
                        && farWhenFocusedFar > 100.0,
                    "Both cubes must show an edge when they"
                    " are in focus.");
                // ピントを外した側は鋭さが落ちること。実測の落ち方は
                // 手前が11.1倍、奥が17.5倍です。3.0倍に置いているのは、
                // 「何もしない」「全体を均一にぼかす」「ピント位置を
                // 無視する」のどれでも比が1.0付近に張り付くのに対し、
                // 正しく効いていれば10倍以上出るためで、その間で
                // いちばん広い余裕を取れる位置です。
                Require(
                    nearWhenFocusedNear
                        > nearWhenFocusedFar * 3.0,
                    "The near cube must lose sharpness when the"
                    " focus moves to the far cube.");
                Require(
                    farWhenFocusedFar
                        > farWhenFocusedNear * 3.0,
                    "The far cube must lose sharpness when the"
                    " focus moves to the near cube.");

                depthOfField.enabled = false;
                scene.SetDepthOfFieldSettings(depthOfField);
                graphics.SetGraphicsSettings(savedQuality);
                scene.SetTemporalAntiAliasingSettings(
                    savedTemporal);
                scene.SetSkySettings(savedSky);
                subject.GetTransform().position = savedSubject;
                subject.GetTransform().scale =
                    savedSubjectScale;
                clone.GetTransform().position = savedClone;
                clone.GetTransform().scale = savedCloneScale;
                cameraObject.GetTransform().position =
                    savedCamera;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraAngles);
            }

            // モーションブラー（カメラの動きによるブレ）。
            //
            // 「動かして撮った絵が甘い」では検証になりません（動かせば
            // 絵は変わるので当たり前です）。**まったく同じカメラの動き**
            // に対して、ブラーのオン/オフだけを変えた2枚を撮って比べます。
            // さらに「カメラを動かさなければオンでもオフと同じ」ことも
            // 見ます。効果がゼロになる条件を用意するのがいちばん強い
            // 確かめ方です。
            {
                const auto savedSubject =
                    subject.GetTransform().position;
                const auto savedSubjectScale =
                    subject.GetTransform().scale;
                const auto savedClone =
                    clone.GetTransform().position;
                const auto savedCamera =
                    cameraObject.GetTransform().position;
                const auto savedCameraAngles =
                    cameraObject.GetTransform().EulerAngles();
                const auto savedSky = scene.Sky();
                const auto savedTemporal =
                    scene.TemporalAntiAliasing();
                const auto savedQuality = graphics.Settings();

                auto flatSky = scene.Sky();
                flatSky.enabled = false;
                scene.SetSkySettings(flatSky);
                // TAAは前フレームと混ぜるので、フレームを跨いで比べる
                // この検証では2枚が互いに汚染します。ここだけ切ります。
                auto noTemporal = scene.TemporalAntiAliasing();
                noTemporal.enabled = false;
                scene.SetTemporalAntiAliasingSettings(
                    noTemporal);
                auto motionQuality = graphics.Settings();
                motionQuality.motionBlurEnabled = true;
                motionQuality.motionBlurSampleCount = 12;
                graphics.SetGraphicsSettings(motionQuality);

                // 立方体を1つだけ正面に置きます。輪郭が縦なので、
                // 横へ動かしたときのブレがそのまま横の甘さになります。
                cameraObject.GetTransform().SetEulerAngles(
                    { 0.0f, 0.0f, 0.0f });
                subject.GetTransform().position =
                    { 0.0f, 0.0f, -6.0f };
                subject.GetTransform().scale =
                    { 2.0f, 2.0f, 2.0f };
                // 2個目は画面外へ出します（1個だけを見たいので）。
                clone.GetTransform().position =
                    { 0.0f, 100.0f, 0.0f };

                LamaPon::MotionBlurSettings motionBlur{};
                motionBlur.intensity = 1.0f;
                motionBlur.maximumRadius = 24.0f;

                // 立方体は画面中央に幅約73画素で写るので、左の輪郭
                // （x≒124）をまたぐ帯を測ります。上限24画素ぶんの
                // 余白を両側に取っています。
                constexpr std::uint32_t EdgeMinimumX = 96;
                constexpr std::uint32_t EdgeMaximumX = 152;
                constexpr std::uint32_t EdgeMinimumY = 78;
                constexpr std::uint32_t EdgeMaximumY = 102;

                // 「1枚目で前フレームの行列を控え、2枚目で動かす」を
                // 毎回同じ手順で踏みます。オン/オフで動きが変わって
                // しまうと比較になりません。
                const auto renderAfterPan =
                    [&](const bool enabled, const float shiftX)
                {
                    auto settings = motionBlur;
                    settings.enabled = enabled;
                    scene.SetMotionBlurSettings(settings);
                    cameraObject.GetTransform().position =
                        { 0.0f, 0.0f, 0.0f };
                    static_cast<void>(renderComposedFrame());
                    cameraObject.GetTransform().position =
                        { shiftX, 0.0f, 0.0f };
                    return renderComposedFrame();
                };

                Stage("frame-motionblur-off");
                const auto panOffFrame =
                    renderAfterPan(false, 0.35f);
                DumpFrame("motionblur-off", panOffFrame);
                Stage("frame-motionblur-on");
                const auto panOnFrame =
                    renderAfterPan(true, 0.35f);
                DumpFrame("motionblur-on", panOnFrame);
                // カメラを動かさない場合。オンでも何も起きないはず
                // （半画素も動いていないなら早期に抜ける作りです）。
                Stage("frame-motionblur-still");
                const auto stillOnFrame =
                    renderAfterPan(true, 0.0f);
                DumpFrame("motionblur-still", stillOnFrame);

                const double panOffSharpness = RegionSharpness(
                    panOffFrame,
                    EdgeMinimumX,
                    EdgeMaximumX,
                    EdgeMinimumY,
                    EdgeMaximumY);
                const double panOnSharpness = RegionSharpness(
                    panOnFrame,
                    EdgeMinimumX,
                    EdgeMaximumX,
                    EdgeMinimumY,
                    EdgeMaximumY);
                const double stillSharpness = RegionSharpness(
                    stillOnFrame,
                    EdgeMinimumX,
                    EdgeMaximumX,
                    EdgeMinimumY,
                    EdgeMaximumY);
                std::cout
                    << "motion blur: pan off " << panOffSharpness
                    << " -> pan on " << panOnSharpness
                    << " / still on " << stillSharpness
                    << std::endl;

                // 実測: 振ったときの鋭さは 604 → 229（2.63倍落ちる）、
                // 動かさなければ 605 でオフとほぼ同一（差0.2%）。
                // 効いていなければ3つとも604付近に揃うので、閾値は
                // その間に置いています。
                Require(
                    panOffSharpness > 100.0,
                    "The cube must show an edge when motion blur"
                    " is off.");
                Require(
                    panOffSharpness > panOnSharpness * 2.0,
                    "Motion blur must soften the edge when the"
                    " camera pans.");
                // 静止時はオフの絵と同じ鋭さに戻ること。ここが緩むと
                // 「常に少しぼかしている」実装でも上の判定を通せます。
                Require(
                    stillSharpness > panOffSharpness * 0.9,
                    "Motion blur must do nothing when the camera"
                    " does not move.");

                LamaPon::MotionBlurSettings disabled{};
                scene.SetMotionBlurSettings(disabled);
                graphics.SetGraphicsSettings(savedQuality);
                scene.SetTemporalAntiAliasingSettings(
                    savedTemporal);
                scene.SetSkySettings(savedSky);
                subject.GetTransform().position = savedSubject;
                subject.GetTransform().scale =
                    savedSubjectScale;
                clone.GetTransform().position = savedClone;
                cameraObject.GetTransform().position =
                    savedCamera;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraAngles);
            }

            // 自動露出（明順応・暗順応）。
            //
            // 「絵が明るくなった」だけでは、露出を上げるだけの実装でも
            // 通ります。**暗いシーンでは開き、明るいシーンでは絞る**の
            // 両方向を見ます。どちらもオフの絵との比で測るので、
            // シーンの明るさそのものの差は約分されます。
            {
                const auto savedAmbient =
                    scene.AmbientLightIntensity();
                const auto savedSky = scene.Sky();
                const auto savedTemporal =
                    scene.TemporalAntiAliasing();
                const auto savedQuality = graphics.Settings();

                auto flatSky = scene.Sky();
                flatSky.enabled = false;
                scene.SetSkySettings(flatSky);
                auto noTemporal = scene.TemporalAntiAliasing();
                noTemporal.enabled = false;
                scene.SetTemporalAntiAliasingSettings(
                    noTemporal);
                auto exposureQuality = graphics.Settings();
                exposureQuality.autoExposureEnabled = true;
                graphics.SetGraphicsSettings(exposureQuality);

                LamaPon::AutoExposureSettings autoExposure{};
                autoExposure.keyValue = 0.18f;
                autoExposure.minimumLuminance = 0.001f;
                autoExposure.maximumLuminance = 20.0f;

                // 明るさを測る帯は**空の側**（画面上部）にします。
                // 画面全体の平均では検証になりませんでした。明るい
                // シーンでは地面がACESの肩に乗って圧縮されるため、
                // 露出を1段以上絞っても合計が7%しか動かず、実際に
                // 効いているのに落ちる判定になりました（絵を見ると
                // 地面は白飛び側から灰へ、空も暗くなっていました）。
                // 空は輝度0.1程度で肩から遠いので、露出の変化が
                // そのまま画素に出ます。
                constexpr std::uint32_t SkyMinimumY = 4;
                constexpr std::uint32_t SkyMaximumY = 56;

                // 露出の段数そのものも見ます。画素は色調整曲線を
                // 通った後の値なので、「測れているか」「符号が
                // 合っているか」はこちらのほうが直接確かめられます。
                float measuredStops{};
                float measuredLuminance{};
                const auto measureBrightness =
                    [&](const bool enabled,
                        const float ambientIntensity)
                {
                    auto settings = autoExposure;
                    settings.enabled = enabled;
                    scene.SetAutoExposureSettings(settings);
                    scene.SetAmbientLightIntensity(
                        ambientIntensity);
                    // 測定は1フレーム遅れで効くので数枚回します
                    // （読めなかったフレームがあっても落ちないよう
                    // 余裕を持たせています）。
                    std::vector<std::uint8_t> frame;
                    for (int index = 0; index < 4; ++index)
                    {
                        frame = renderComposedFrame();
                    }
                    const auto* const target =
                        graphics.SceneCompositionTarget();
                    measuredStops = target != nullptr
                        ? target->AutoExposureStops()
                        : 0.0f;
                    measuredLuminance = target != nullptr
                        ? target->AdaptedLuminance()
                        : 0.0f;
                    return frame;
                };

                Stage("frame-autoexposure-dark-off");
                const auto darkOffFrame =
                    measureBrightness(false, 0.05f);
                DumpFrame("autoexposure-dark-off", darkOffFrame);
                Stage("frame-autoexposure-dark-on");
                const auto darkOnFrame =
                    measureBrightness(true, 0.05f);
                DumpFrame("autoexposure-dark-on", darkOnFrame);
                const float darkStops = measuredStops;
                const float darkLuminance = measuredLuminance;
                Stage("frame-autoexposure-bright-off");
                const auto brightOffFrame =
                    measureBrightness(false, 2.5f);
                DumpFrame(
                    "autoexposure-bright-off",
                    brightOffFrame);
                Stage("frame-autoexposure-bright-on");
                const auto brightOnFrame =
                    measureBrightness(true, 2.5f);
                DumpFrame(
                    "autoexposure-bright-on",
                    brightOnFrame);
                const float brightStops = measuredStops;
                const float brightLuminance = measuredLuminance;

                const double darkOff = static_cast<double>(
                    RegionBrightness(
                        darkOffFrame,
                        0,
                        Width,
                        SkyMinimumY,
                        SkyMaximumY));
                const double darkOn = static_cast<double>(
                    RegionBrightness(
                        darkOnFrame,
                        0,
                        Width,
                        SkyMinimumY,
                        SkyMaximumY));
                const double brightOff = static_cast<double>(
                    RegionBrightness(
                        brightOffFrame,
                        0,
                        Width,
                        SkyMinimumY,
                        SkyMaximumY));
                const double brightOn = static_cast<double>(
                    RegionBrightness(
                        brightOnFrame,
                        0,
                        Width,
                        SkyMinimumY,
                        SkyMaximumY));
                std::cout
                    << "auto exposure: sky dark " << darkOff
                    << " -> " << darkOn
                    << " / sky bright " << brightOff
                    << " -> " << brightOn
                    << " / measured dark L=" << darkLuminance
                    << " stops=" << darkStops
                    << " / bright L=" << brightLuminance
                    << " stops=" << brightStops
                    << std::endl;

                // 実測（320x180の検証シーン）:
                //   暗いシーン L=0.0713 → 露出 +1.34段、
                //     空の帯 2.63e6 → 5.64e6（2.14倍）
                //   明るいシーン L=0.218 → 露出 -0.28段、
                //     空の帯 2.63e6 → 2.13e6（0.81倍）
                // 効いていなければ段数は両方0、画素の比は両方1.0に
                // 張り付くので、閾値はその間に置いています。
                //
                // **明るい側の段数が小さいのは正しい挙動です。** 露出は
                // 画面の**幾何平均**（対数平均）に対して働き、この検証
                // シーンは上半分がクリアカラー（環境光を上げても変わらない
                // 固定色、輝度0.1程度）で占められているので、平均が
                // なかなか上がりません。実際のゲームでは空も明るくなる
                // ので、もっと大きく振れます。ここを「弱い」と見て
                // シェーダーを触らないでください。
                //
                // まず測った明るさがシーンの明るさに追随していること。
                // ここが動かないと、以下の判定は全部たまたまです。
                Require(
                    brightLuminance > darkLuminance * 2.0,
                    "The measured luminance must follow the"
                    " scene brightness.");
                // 露出の段数が両方向へ振れること。暗いシーンでは
                // 開き（正）、明るいシーンでは絞ります（負）。
                Require(
                    darkStops > 0.8f && brightStops < -0.15f,
                    "Auto exposure must open up on a dark scene"
                    " and close down on a bright one.");
                // 振れ幅も見ます。片方だけ動く実装（明るい側で
                // 飽和して止まるなど）を通さないためです。
                Require(
                    darkStops - brightStops > 1.0f,
                    "The exposure must swing by more than a stop"
                    " between the two scenes.");
                // 画素にも出ていること。
                Require(
                    darkOn > darkOff * 1.5,
                    "Auto exposure must brighten the pixels of a"
                    " dark scene.");
                Require(
                    brightOn < brightOff * 0.9,
                    "Auto exposure must darken the pixels of a"
                    " bright scene.");

                LamaPon::AutoExposureSettings disabled{};
                scene.SetAutoExposureSettings(disabled);
                graphics.SetGraphicsSettings(savedQuality);
                scene.SetTemporalAntiAliasingSettings(
                    savedTemporal);
                scene.SetSkySettings(savedSky);
                scene.SetAmbientLightIntensity(savedAmbient);
                // 露出を戻した状態で1枚回し、この後の段が前の露出を
                // 引き継がないようにします。
                static_cast<void>(renderComposedFrame());
            }

            // 非同期コンパイル。コンパイル中は標準Litで描いて処理を
            // 止めず、焼き上がったら本来のシェーダーへ差し替わる
            // ことを確かめます。ここだけ非同期へ戻します。
            {
                const auto savedSubject =
                    subject.GetTransform().position;
                const auto savedClone =
                    clone.GetTransform().position;
                const auto savedCamera =
                    cameraObject.GetTransform().position;
                const auto savedCameraAngles =
                    cameraObject.GetTransform().EulerAngles();
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                clone.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.0f, 3.2f };
                cameraObject.GetTransform().SetEulerAngles(
                    { 0.0f, 0.0f, 0.0f });

                graphics.SetAsyncShaderCompilationEnabled(true);
                // 焼き上がり済みだと一発で出てしまい非同期の道を
                // 通らないので、キャッシュを消してから始めます。
                LamaPon::ClearShaderCache();

                auto& asyncPlane =
                    scene.CreateGameObject("AsyncPlane");
                asyncPlane.GetTransform().SetEulerAngles(
                    { -1.5707963f, 0.0f, 0.0f });
                asyncPlane.GetTransform().scale =
                    { 4.0f, 1.0f, 2.4f };
                auto& asyncRenderer =
                    asyncPlane.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Plane,
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f, 1.0f });
                asyncRenderer.SetShaderPath(
                    std::filesystem::path{
                        LAMAPON_TEST_FIXTURE_DIR }
                    / "variant-probe.hlsl");

                Stage("frame-async-compiling");
                const auto firstFrame = renderComposedFrame();
                DumpFrame("async-compiling", firstFrame);
                const auto whileCompiling = At(
                    firstFrame,
                    Width / 2,
                    Height / 2);

                // 焼き上がるまで描き続けます。非同期なので何
                // フレームかかるかは環境次第です。
                Pixel settled{};
                int frames = 0;
                for (; frames < 600; ++frames)
                {
                    const auto frame = renderComposedFrame();
                    settled = At(frame, Width / 2, Height / 2);
                    if (settled.red > 60
                        && settled.green < 40)
                    {
                        break;
                    }
                }
                DumpFrame(
                    "async-settled",
                    renderComposedFrame());

                std::cout
                    << "async shader: compiling=("
                    << static_cast<int>(whileCompiling.red)
                    << "," << static_cast<int>(
                        whileCompiling.green)
                    << ") settled=("
                    << static_cast<int>(settled.red)
                    << "," << static_cast<int>(settled.green)
                    << ") frames=" << frames
                    << std::endl;

                Require(
                    frames < 600,
                    "An asynchronously compiled shader must"
                    " eventually replace the placeholder.");
                // variant-probeはキーワード無しで暗い赤。標準Lit
                // （コンパイル中の代役）は白い板なので緑成分が
                // 残ります。ここが同じなら差し替わっていません。
                Require(
                    settled.red > settled.green + 20,
                    "The settled frame must show the custom"
                    " shader, not the placeholder.");

                static_cast<void>(
                    scene.DestroyGameObject(asyncPlane));
                graphics.SetAsyncShaderCompilationEnabled(
                    false);
                subject.GetTransform().position = savedSubject;
                clone.GetTransform().position = savedClone;
                cameraObject.GetTransform().position =
                    savedCamera;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraAngles);
            }

            // 壊れたShaderの知らせ方。コンパイルに失敗したものを
            // 標準Litで代役すると、動いているように見えて実は
            // 壊れている状態になります。マゼンタで描かれること、
            // そして直したら本来の色へ戻ることを確かめます。
            {
                const auto savedSubject =
                    subject.GetTransform().position;
                const auto savedClone =
                    clone.GetTransform().position;
                const auto savedCamera =
                    cameraObject.GetTransform().position;
                const auto savedCameraAngles =
                    cameraObject.GetTransform().EulerAngles();
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                clone.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.0f, 3.2f };
                cameraObject.GetTransform().SetEulerAngles(
                    { 0.0f, 0.0f, 0.0f });

                auto& errorPlane =
                    scene.CreateGameObject("ShaderErrorPlane");
                errorPlane.GetTransform().SetEulerAngles(
                    { -1.5707963f, 0.0f, 0.0f });
                errorPlane.GetTransform().scale =
                    { 4.0f, 1.0f, 2.4f };
                auto& errorRenderer =
                    errorPlane.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Plane,
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f, 1.0f });

                const auto sampleShaderError =
                    [&](const char* name) -> Pixel
                {
                    Stage(name);
                    // 1枚捨ててから撮ります。前の段が時間方向の
                    // 蓄積（TAA・自動露出）を残していると、
                    // 差し替えた直後の1枚に前の絵が混ざります。
                    static_cast<void>(renderComposedFrame());
                    const auto frame = renderComposedFrame();
                    DumpFrame(name, frame);
                    return At(frame, Width / 2, Height / 2);
                };

                // コンパイルに失敗するShader。
                errorRenderer.SetShaderPath(
                    std::filesystem::path{
                        LAMAPON_TEST_FIXTURE_DIR }
                    / "broken-shader.hlsl");
                const auto broken =
                    sampleShaderError("shader-error-broken");

                // 指定したファイルが無い場合（消した・移動した）。
                errorRenderer.SetShaderPath(
                    std::filesystem::path{
                        LAMAPON_TEST_FIXTURE_DIR }
                    / "no-such-shader.hlsl");
                const auto missing =
                    sampleShaderError("shader-error-missing");

                // 2D用のShaderを3Dマテリアルへ入れた場合。利用者が
                // 実際にやった操作で、HLSLは
                // 「'VSMain': entrypoint not found」としか言いません。
                // 説明が足されて届くところまで（配線）を見ます。
                errorRenderer.SetShaderPath(
                    std::filesystem::path{ "shaders" }
                    / "LamaPonSpriteError.hlsl");
                const auto wrongKind =
                    sampleShaderError("shader-error-wrong-kind");
                const auto wrongKindMessage =
                    errorRenderer.ShaderError();
                std::cout
                    << "shader error hint: "
                    << wrongKindMessage
                    << std::endl;

                // 動いていたShaderを書き間違えた場合。直前に成功
                // したものを描き続けると「編集しても見た目が
                // 変わらない」になります。ここが本命です。
                const auto probePath =
                    std::filesystem::temp_directory_path()
                    / "lamapon-shader-error-probe.hlsl";
                const auto writeProbe =
                    [&](const char* source)
                {
                    std::filesystem::copy_file(
                        std::filesystem::path{
                            LAMAPON_TEST_FIXTURE_DIR }
                        / source,
                        probePath,
                        std::filesystem::copy_options::
                            overwrite_existing);
                };

                writeProbe("variant-probe.hlsl");
                errorRenderer.SetShaderPath(probePath);
                const auto working =
                    sampleShaderError("shader-error-working");

                writeProbe("broken-shader.hlsl");
                errorRenderer.ReloadShader();
                const auto edited =
                    sampleShaderError("shader-error-edited");

                // 直したときに元へ戻ること。代役が居座ると、今度は
                // 「直したのに直らない」に見えます。
                writeProbe("variant-probe.hlsl");
                errorRenderer.ReloadShader();
                const auto repaired =
                    sampleShaderError("shader-error-repaired");

                std::error_code removeError;
                std::filesystem::remove(probePath, removeError);

                std::cout
                    << "shader error: broken=("
                    << static_cast<int>(broken.red)
                    << "," << static_cast<int>(broken.green)
                    << "," << static_cast<int>(broken.blue)
                    << ") missing=("
                    << static_cast<int>(missing.red)
                    << "," << static_cast<int>(missing.green)
                    << "," << static_cast<int>(missing.blue)
                    << ") working=("
                    << static_cast<int>(working.red)
                    << "," << static_cast<int>(working.green)
                    << "," << static_cast<int>(working.blue)
                    << ") edited=("
                    << static_cast<int>(edited.red)
                    << "," << static_cast<int>(edited.green)
                    << "," << static_cast<int>(edited.blue)
                    << ") repaired=("
                    << static_cast<int>(repaired.red)
                    << "," << static_cast<int>(repaired.green)
                    << "," << static_cast<int>(repaired.blue)
                    << ")" << std::endl;

                // 赤と青が立って緑が沈んでいればマゼンタです。
                // 具体的な数値はトーンマップ次第なので比で見ます。
                const auto isMagenta =
                    [](const Pixel& pixel) noexcept
                {
                    return pixel.red > 90
                        && pixel.blue > 90
                        && pixel.green + 60 < pixel.red
                        && pixel.green + 60 < pixel.blue;
                };
                Require(
                    isMagenta(broken),
                    "A shader that fails to compile must be"
                    " drawn with the magenta placeholder.");
                Require(
                    isMagenta(missing),
                    "A missing shader file must be drawn with"
                    " the magenta placeholder.");
                Require(
                    isMagenta(wrongKind),
                    "A 2D shader assigned to a 3D material must"
                    " be drawn with the magenta placeholder.");
                // 「2D」はASCIIなので、文字コードに関係なく探せます。
                Require(
                    wrongKindMessage.find("2D")
                        != std::string::npos,
                    "The shader error must explain that this is"
                    " a 2D shader, not just repeat X3501.");
                Require(
                    wrongKindMessage.find("X3501")
                        != std::string::npos,
                    "The original compiler message must survive"
                    " alongside the explanation.");
                // variant-probeはキーワード無しで暗い赤です。
                Require(
                    !isMagenta(working)
                        && working.red > working.blue + 20,
                    "The probe shader must render normally"
                    " before it is broken.");
                Require(
                    isMagenta(edited),
                    "Breaking a shader that used to compile"
                    " must show the magenta placeholder, not"
                    " keep drawing the last good version.");
                Require(
                    !isMagenta(repaired)
                        && repaired.red > repaired.blue + 20,
                    "Repairing the shader must bring the real"
                    " shader back.");

                static_cast<void>(
                    scene.DestroyGameObject(errorPlane));
                subject.GetTransform().position = savedSubject;
                clone.GetTransform().position = savedClone;
                cameraObject.GetTransform().position =
                    savedCamera;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraAngles);
            }

            // Shaderバリアント。キーワードごとに別々のシェーダーと
            // してコンパイルされ、マテリアルの指定で選ばれることを
            // 色で確かめます。プリプロセッサで色を変える見本Shaderを
            // 使うので、届いていなければ色が変わりません。
            {
                const auto savedSubject =
                    subject.GetTransform().position;
                const auto savedClone =
                    clone.GetTransform().position;
                const auto savedCamera =
                    cameraObject.GetTransform().position;
                const auto savedCameraAngles =
                    cameraObject.GetTransform().EulerAngles();
                subject.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                clone.GetTransform().position =
                    { 0.0f, 50.0f, 0.0f };
                cameraObject.GetTransform().position =
                    { 0.0f, 0.0f, 3.2f };
                cameraObject.GetTransform().SetEulerAngles(
                    { 0.0f, 0.0f, 0.0f });

                auto& variantPlane =
                    scene.CreateGameObject("VariantPlane");
                variantPlane.GetTransform().SetEulerAngles(
                    { -1.5707963f, 0.0f, 0.0f });
                variantPlane.GetTransform().scale =
                    { 4.0f, 1.0f, 2.4f };
                auto& variantRenderer =
                    variantPlane.AddComponent<
                        LamaPon::MeshRendererComponent>(
                        LamaPon::PrimitiveShape::Plane,
                        DirectX::XMFLOAT4{
                            1.0f, 1.0f, 1.0f, 1.0f });
                variantRenderer.SetShaderPath(
                    std::filesystem::path{
                        LAMAPON_TEST_FIXTURE_DIR }
                    / "variant-probe.hlsl");

                const auto sampleVariant =
                    [&](const char* name) -> Pixel
                {
                    Stage(name);
                    const auto frame = renderComposedFrame();
                    DumpFrame(name, frame);
                    return At(frame, Width / 2, Height / 2);
                };

                // キーワード無し: 暗い赤。
                const auto plain =
                    sampleVariant("variant-none");
                // GREENだけ: 暗い緑。
                variantRenderer.EnableShaderKeyword(
                    "VARIANT_PROBE_GREEN");
                const auto green =
                    sampleVariant("variant-green");
                // GREEN + BRIGHT: 明るい緑。
                variantRenderer.EnableShaderKeyword(
                    "VARIANT_PROBE_BRIGHT");
                const auto brightGreen =
                    sampleVariant("variant-green-bright");
                // 宣言に無いキーワードは無視されること。
                variantRenderer.EnableShaderKeyword(
                    "VARIANT_PROBE_NOT_DECLARED");
                const auto unknown =
                    sampleVariant("variant-unknown-keyword");

                std::cout
                    << "variant: none=("
                    << static_cast<int>(plain.red) << ","
                    << static_cast<int>(plain.green) << ")"
                    << " green=("
                    << static_cast<int>(green.red) << ","
                    << static_cast<int>(green.green) << ")"
                    << " bright=("
                    << static_cast<int>(brightGreen.red) << ","
                    << static_cast<int>(brightGreen.green) << ")"
                    << std::endl;

                Require(
                    plain.red > plain.green + 20,
                    "Without keywords the variant probe must"
                    " render red.");
                Require(
                    green.green > green.red + 20,
                    "Enabling VARIANT_PROBE_GREEN must select"
                    " the green variant.");
                Require(
                    brightGreen.green > green.green + 40,
                    "Enabling VARIANT_PROBE_BRIGHT must select"
                    " the brighter variant.");
                Require(
                    unknown.red == brightGreen.red
                        && unknown.green == brightGreen.green
                        && unknown.blue == brightGreen.blue,
                    "A keyword the shader never declared must"
                    " be ignored, not change the variant.");

                static_cast<void>(
                    scene.DestroyGameObject(variantPlane));
                subject.GetTransform().position = savedSubject;
                clone.GetTransform().position = savedClone;
                cameraObject.GetTransform().position =
                    savedCamera;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraAngles);
            }

            // 朝昼夜。Directional Lightを回すだけで空と環境光が
            // 変わることを見ます。太陽の高度を真上から真下まで
            // 振って、明るさが単調に落ちることを確かめます。
            {
                const auto savedSunAnglesForSky =
                    sunObject.GetTransform().EulerAngles();
                const auto savedSky = scene.Sky();
                sunObject.SetEnabled(true);
                auto daySky = scene.Sky();
                daySky.enabled = true;
                daySky.sunDriven = true;
                daySky.cubemapPath.clear();
                scene.SetSkySettings(daySky);

                struct TimeOfDay final
                {
                    const char* name;
                    // 太陽の高度（度）。90=真上, 0=地平線, -20=夜。
                    float elevationDegrees;
                };
                const TimeOfDay times[]{
                    { "daynight-noon", 78.0f },
                    { "daynight-afternoon", 30.0f },
                    { "daynight-sunset", 2.0f },
                    { "daynight-night", -25.0f }
                };
                double previousSkyLuminance = 1.0e9;
                bool descending = true;
                for (const auto& time : times)
                {
                    // yaw=piのとき、光の進む向きは
                    // (0, sin(pitch), cos(pitch)) です。太陽を
                    // 高度eへ置くには光が下向きに sin(e) 進めば
                    // よいので pitch = -e になります。
                    const float pitch =
                        -DirectX::XMConvertToRadians(
                            time.elevationDegrees);
                    sunObject.GetTransform().SetEulerAngles(
                        { pitch, 3.14159265f, 0.0f });
                    Stage(time.name);
                    const auto frame = renderComposedFrame();
                    DumpFrame(time.name, frame);
                    // 空だけを見ます（画面上部は必ず空）。
                    double sum{};
                    int samples{};
                    for (std::uint32_t y = 5; y < 30; ++y)
                    {
                        for (std::uint32_t x = 0;
                            x < Width;
                            x += 4)
                        {
                            const auto& pixel = At(frame, x, y);
                            sum += 0.299 * pixel.red
                                + 0.587 * pixel.green
                                + 0.114 * pixel.blue;
                            ++samples;
                        }
                    }
                    const double luminance = sum / samples;
                    std::cout
                        << time.name << ": sky luminance="
                        << luminance << std::endl;
                    if (luminance > previousSkyLuminance)
                    {
                        descending = false;
                    }
                    previousSkyLuminance = luminance;
                }
                // 太陽が下がるほど空は暗くなること。追従して
                // いなければ4枚とも同じ明るさになり、ここで
                // 落ちます。
                Require(
                    descending,
                    "The sun-driven sky must get darker as the"
                    " directional light rotates below the"
                    " horizon.");

                scene.SetSkySettings(savedSky);
                sunObject.GetTransform().SetEulerAngles(
                    savedSunAnglesForSky);
                sunObject.SetEnabled(false);
            }

            // SSAOを戻してから、読み込みモデルの描画を確認します。
            occlusion.enabled = false;
            scene.SetAmbientOcclusionSettings(occlusion);

            // ⑧ 読み込みモデル（--dump のときだけ）。
            // スキニングモデルはDirectXTKの頂点シェーダーで変形し、
            // ピクセルシェーダーだけをLamaPon Litへ差し替える構造の
            // ため、PSSkinnedMainの入力セマンティクスがずれていると
            // UVと法線が入れ替わって明確に破綻します。Lit（既定）と
            // 従来のDirectXTK描画の2枚を出して見比べられるように
            // します。
            auto& modelObject =
                scene.CreateGameObject("DumpModel");
            modelObject.GetTransform().position =
                { 0.0f, 0.0f, 0.0f };
            modelObject.GetTransform().scale =
                { 1.5f, 1.5f, 1.5f };
            auto& modelRenderer =
                modelObject.AddComponent<
                    LamaPon::ModelRendererComponent>(
                        std::filesystem::path{ "models" }
                            / "RiggedSimple.glb");

            Stage("frame-model-lit");
            DumpFrame("model-lit", renderComposedFrame());

            modelRenderer.SetUseLegacyShading(true);
            Stage("frame-model-legacy");
            DumpFrame("model-legacy", renderComposedFrame());
            modelRenderer.SetUseLegacyShading(false);

            // Litでは横縞（面ごとの段差）が見える。GGXの鏡面が面の
            // 向きの差を強調しているだけなら、粗くすれば消えるはず。
            // 粗さを両端に振って切り分けます。
            modelRenderer.SetMaterialOverrideEnabled(true);
            modelRenderer.SetRoughness(1.0f);
            Stage("frame-model-rough");
            DumpFrame("model-rough", renderComposedFrame());
            modelRenderer.SetRoughness(0.05f);
            Stage("frame-model-glossy");
            DumpFrame("model-glossy", renderComposedFrame());
            modelRenderer.SetRoughness(0.5f);
            modelRenderer.SetMaterialOverrideEnabled(false);

            // ⑨ 発光とBloom。発光はライティングと無関係に加算され、
            // 1を超える値はBloomが滲ませます。
            auto emissiveBloom = graphics.Settings();
            emissiveBloom.bloomEnabled = true;
            graphics.SetGraphicsSettings(emissiveBloom);
            auto bloom = scene.Bloom();
            bloom.enabled = true;
            scene.SetBloomSettings(bloom);
            modelRenderer.SetMaterialOverrideEnabled(true);
            modelRenderer.SetEmissiveColor(
                { 3.0f, 0.6f, 0.1f });
            Stage("frame-emissive-bloom");
            DumpFrame("emissive-bloom", renderComposedFrame());
            modelRenderer.SetEmissiveColor({});
            modelRenderer.SetMaterialOverrideEnabled(false);
        }

        // ベイクした間接光の保存→読み込み。GIの節で丸ごとJSONへ
        // したシーンを**別のScene**へ読み込み、同じ場所に同じ
        // はね返りが出ることを確かめます（焼き込みデータはbase64の
        // fp16なので、復元はビット単位で同じテクスチャになるはず）。
        // 主のsceneを壊さないよう、専用のSceneで行います。
        if (!giRoundTripJson.empty())
        {
            Stage("frame-gi-roundtrip");
            LamaPon::Scene reloadedScene(graphics);
            reloadedScene.LoadFromJson(giRoundTripJson);
            Require(
                reloadedScene.HasBakedGlobalIllumination(),
                "The baked GI data must survive the JSON"
                " round trip.");
            // 保存時はenabled=trueのまま書いているので、復元後も
            // 有効のはず。
            Require(
                reloadedScene.BakedGlobalIllumination()
                    .enabled,
                "The GI enable flag must survive the JSON"
                " round trip.");
            const auto renderReloaded =
                [&graphics, &reloadedScene, &clearColor]
                {
                    graphics.BeginFrame(clearColor);
                    graphics.BeginSceneComposition(
                        clearColor);
                    reloadedScene.RenderMainCamera(
                        graphics.AspectRatio(),
                        false,
                        graphics.SceneCompositionTarget());
                    graphics.EndSceneComposition(
                        reloadedScene
                            .PostProcessFrameData());
                    std::uint32_t width{};
                    std::uint32_t height{};
                    auto pixels =
                        graphics.CaptureBackBuffer(
                            width,
                            height);
                    graphics.EndFrame();
                    return pixels;
                };
            const auto reloadedOnFrame = renderReloaded();
            DumpFrame("gi-roundtrip", reloadedOnFrame);
            auto reloadedSettings =
                reloadedScene.BakedGlobalIllumination();
            reloadedSettings.enabled = false;
            reloadedScene
                .SetBakedGlobalIlluminationSettings(
                    reloadedSettings);
            const auto reloadedOffFrame = renderReloaded();
            const auto reloadedSkew =
                [&](const std::vector<std::uint8_t>& frame)
            {
                long long skew = 0;
                for (std::uint32_t y = 130; y < 165; ++y)
                {
                    for (std::uint32_t x = 110; x < 150; ++x)
                    {
                        const auto pixel = At(frame, x, y);
                        skew += pixel.red;
                        skew -= pixel.blue;
                    }
                }
                return skew;
            };
            const long long reloadedDelta =
                reloadedSkew(reloadedOnFrame)
                - reloadedSkew(reloadedOffFrame);
            std::cout
                << "gi roundtrip red skew delta: "
                << reloadedDelta
                << " (baked: "
                << giRoundTripExpectedNear
                << ")" << std::endl;
            // 読み込んだGIの効きが、焼いた直後の効きとほぼ同じ
            // こと（fp16の往復なので本来は完全一致。帯の外の
            // 描画差に引きずられないよう、1割の揺れまで許します）。
            Require(
                reloadedDelta * 10
                        > giRoundTripExpectedNear * 9
                    && reloadedDelta * 10
                        < giRoundTripExpectedNear * 11,
                "The reloaded GI must reproduce the same"
                " bounce light.");
        }

        // 画面エフェクトの差し込み地点。**Bloomの前へ置いた光は
        // 滲み、後ろへ置いた光は滲みません。** 同じShaderを同じ
        // パラメーターで2回かけ、滲みの差だけを見ます。
        // 「落ちない」「色が出る」だけでは、指定した位置に入ったのか
        // 従来どおり末尾に入ったのかを区別できません。
        //
        // この段も**絵を見る判定の最後**へ置いています（後ろは時間しか
        // 測らないベンチだけ）。途中へ差すと後ろの段の測定値が動きます。
        {
            const auto savedGraphicsSettings =
                graphics.Settings();
            const auto savedBloom = scene.Bloom();
            auto bloomSettings = graphics.Settings();
            bloomSettings.bloomEnabled = true;
            graphics.SetGraphicsSettings(bloomSettings);
            auto sceneBloom = scene.Bloom();
            sceneBloom.enabled = true;
            // 既定（radius 2）の滲みは1〜2画素しかなく、四角の縁と
            // 見分けが付きません。判定の余白より確実に外へ出るよう、
            // ここでは広く強くします。
            sceneBloom.threshold = 0.5f;
            sceneBloom.intensity = 2.0f;
            sceneBloom.radius = 8.0f;
            scene.SetBloomSettings(sceneBloom);

            const auto dotShader =
                std::filesystem::path{
                    LAMAPON_TEST_FIXTURE_DIR }
                / "bright-dot.hlsl";
            constexpr float DotRadius = 0.05f;
            const auto renderWithPoint =
                [&](const LamaPon::ScreenEffectPoint point,
                    const char* stageName,
                    std::vector<std::uint8_t>& frame)
            {
                LamaPon::ScreenEffectRequest request;
                request.shader = dotShader;
                request.point = point;
                request.customParameters[0] = {
                    DotRadius, 6.0f, 0.0f, 0.0f
                };
                std::string shaderError;
                const bool queued = graphics.QueueScreenEffect(
                    request,
                    nullptr,
                    &shaderError);
                Require(
                    queued,
                    shaderError.empty()
                        ? "the bright dot effect must queue"
                        : shaderError.c_str());
                Stage(stageName);
                frame = renderComposedFrame();
                DumpFrame(stageName + 6, frame);
            };

            std::vector<std::uint8_t> beforeBloomFrame;
            std::vector<std::uint8_t> afterToneMapFrame;
            renderWithPoint(
                LamaPon::ScreenEffectPoint::BeforeBloom,
                "frame-inject-before-bloom",
                beforeBloomFrame);
            renderWithPoint(
                LamaPon::ScreenEffectPoint::AfterToneMapping,
                "frame-inject-after-tonemap",
                afterToneMapFrame);

            // 四角の中は数えません。滲みは**外側**に出ます。
            const auto insideDot =
                [](const std::uint32_t x,
                    const std::uint32_t y)
            {
                const float u =
                    (static_cast<float>(x) + 0.5f)
                    / static_cast<float>(Width);
                const float v =
                    (static_cast<float>(y) + 0.5f)
                    / static_cast<float>(Height);
                return std::abs(u - 0.5f) < DotRadius + 0.01f
                    && std::abs(v - 0.5f) < DotRadius + 0.01f;
            };
            std::size_t dotPixels = 0;
            std::size_t halo = 0;
            for (std::uint32_t y = 0; y < Height; ++y)
            {
                for (std::uint32_t x = 0; x < Width; ++x)
                {
                    const auto bloomed =
                        At(beforeBloomFrame, x, y);
                    const auto plain =
                        At(afterToneMapFrame, x, y);
                    if (insideDot(x, y))
                    {
                        if (bloomed.red > 150
                            && plain.red > 150)
                        {
                            ++dotPixels;
                        }
                        continue;
                    }
                    const int bloomedSum = bloomed.red
                        + bloomed.green + bloomed.blue;
                    const int plainSum = plain.red
                        + plain.green + plain.blue;
                    if (bloomedSum - plainSum > 24)
                    {
                        ++halo;
                    }
                }
            }
            std::cout
                << "screen effect injection: dot="
                << dotPixels
                << " halo=" << halo
                << std::endl;
            // 対照が先。どちらの位置でも四角そのものは出ること。
            Require(
                dotPixels > 200,
                "The bright square must be drawn at both"
                " injection points, otherwise the comparison"
                " below means nothing.");
            // Bloomの前へ入っていれば、四角の外へ光がにじみます。
            Require(
                halo > 500,
                "An effect injected before bloom must bleed"
                " outside the square; if it does not, it was"
                " applied at the old fixed position.");

            graphics.SetGraphicsSettings(savedGraphicsSettings);
            scene.SetBloomSettings(savedBloom);
        }

        // テセレーションした形で影を落とせること。
        //
        // **この段は一番後ろに置いています。** 検証シーンは全段で
        // 使い回すので、途中へ差し込むと後ろの段の測定値が動きます
        // （実際、GIの数値が3.5倍に動きました）。絵を見る判定は
        // ここまでで終わっているので、時間しか測らないベンチの手前が
        // 一番安全です。
        {
            const auto benchSavedCameraPosition =
                cameraObject.GetTransform().position;
            const auto benchSavedCameraRotation =
                cameraObject.GetTransform().EulerAngles();
            // 板は水平（XZ）なので真上から。影の落ち先が見えるよう
            // 引いて撮ります。
            cameraObject.GetTransform().position =
                { 0.0f, 14.0f, 0.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { -1.5707963f, 0.0f, 0.0f });
            sunObject.SetEnabled(true);

            auto& receiver =
                scene.CreateGameObject("ShadowFloor");
            receiver.GetTransform().position =
                { 0.0f, -3.0f, 0.0f };
            receiver.GetTransform().scale =
                { 30.0f, 1.0f, 30.0f };
            receiver.AddComponent<
                LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Plane,
                DirectX::XMFLOAT4{
                    1.0f, 1.0f, 1.0f, 1.0f });

            // 対照が先です。**同じ大きさ・同じ位置の普通の板**
            // が影を落とせているかを見ておかないと、下の判定が
            // 0でも「テセレーションが原因」とは言えません
            // （カメラの向きや太陽の角度で影が画面外へ出て
            // いるだけかもしれない）。
            auto& plainCaster =
                scene.CreateGameObject("PlainCaster");
            plainCaster.GetTransform().position =
                { 0.0f, 0.0f, 0.0f };
            plainCaster.GetTransform().scale =
                { 8.0f, 1.0f, 8.0f };
            plainCaster.AddComponent<
                LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Plane,
                DirectX::XMFLOAT4{
                    1.0f, 1.0f, 1.0f, 1.0f });
            Stage("frame-tessellation-shadow-plain");
            const auto plainFrame =
                renderComposedFrame();
            DumpFrame(
                "tessellation-shadow-plain",
                plainFrame);
            static_cast<void>(
                scene.DestroyGameObject(plainCaster));

            auto& caster =
                scene.CreateGameObject("TessCaster");
            caster.GetTransform().position =
                { 0.0f, 0.0f, 0.0f };
            caster.GetTransform().scale =
                { 8.0f, 1.0f, 8.0f };
            auto& casterRenderer =
                caster.AddComponent<
                    LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Plane,
                    DirectX::XMFLOAT4{
                        1.0f, 1.0f, 1.0f, 1.0f });
            casterRenderer.SetShaderPath(
                std::filesystem::path{ "shaders" }
                    / "LamaPonTessellatedTerrain.hlsl");
            casterRenderer.SetCustomParameter(
                0,
                DirectX::XMFLOAT4{
                    0.2f, 0.9f, 0.2f, 1.0f });
            casterRenderer.SetCustomParameter(
                1,
                DirectX::XMFLOAT4{
                    0.4f, 4.0f, 0.0f, 0.0f });
            casterRenderer.SetCustomParameter(
                3,
                DirectX::XMFLOAT4{
                    16.0f, 0.0f, 0.0f, 0.0f });

            Stage("frame-tessellation-shadow");
            const auto shadowFrame =
                renderComposedFrame();
            DumpFrame(
                "tessellation-shadow",
                shadowFrame);

            // 対照。落とす側を消して同じ絵を撮ります。
            static_cast<void>(
                scene.DestroyGameObject(caster));
            const auto litFrame = renderComposedFrame();
            DumpFrame(
                "tessellation-shadow-none",
                litFrame);

            // 影＝「落とす側が無いときは明るく、あるときは
            // 暗い」画素。板そのものが写っている場所は緑に
            // なるので数えません（そこは影ではなく遮蔽）。
            // 普通の板は受け面と同じ白＆同じ向きなので、
            // 板の写る場所は明るさがほぼ変わりません。つまり
            // 対照側で数えられるのは影だけです。
            const auto countShadowed =
                [&litFrame](
                    const std::vector<std::uint8_t>&
                        frame)
            {
                std::size_t total = 0;
                for (std::uint32_t y = 0;
                    y < Height;
                    ++y)
                {
                    for (std::uint32_t x = 0;
                        x < Width;
                        ++x)
                    {
                        const auto with =
                            At(frame, x, y);
                        const auto without =
                            At(litFrame, x, y);
                        const bool greenish =
                            with.green > with.red + 20
                            && with.green
                                > with.blue + 20;
                        const int lit =
                            without.red
                            + without.green
                            + without.blue;
                        const int now = with.red
                            + with.green
                            + with.blue;
                        if (!greenish
                            && lit > 200
                            && lit - now > 60)
                        {
                            ++total;
                        }
                    }
                }
                return total;
            };
            const auto plainShadowed =
                countShadowed(plainFrame);
            const auto shadowed =
                countShadowed(shadowFrame);
            std::cout
                << "tessellation shadow: plain="
                << plainShadowed
                << " tessellated=" << shadowed
                << std::endl;
            // 対照が先。普通の板でも影が見えないなら、
            // カメラか太陽の置き方の問題です。
            Require(
                plainShadowed > 300,
                "A plain plane of the same size must"
                " cast a visible shadow here, otherwise"
                " this stage proves nothing.");
            Require(
                shadowed > 300,
                "A tessellated surface must cast a"
                " shadow onto what is below it.");

            static_cast<void>(
                scene.DestroyGameObject(receiver));
            sunObject.SetEnabled(false);
            cameraObject.GetTransform().position =
                benchSavedCameraPosition;
            cameraObject.GetTransform().SetEulerAngles(
                benchSavedCameraRotation);
        }

        // エディターの「シーンビュー経路」と「ゲームビュー経路」の
        // A/B計測（--benchmark/--dump時のみの診断。時間は判定しません——WARPの
        // 絶対値は環境依存ですが、同じマシンでの相対比較はできます）。
        //
        // エディターでゲームビューのFPSがシーンビューより低い、という
        // 報告の切り分け用です。両経路は同じターゲット・同じポスト
        // 処理・同じ2Dを通るはずなので、ここで差が出るなら経路の中、
        // 出ないなら解像度など経路の外に原因があります。
        if (g_runBenchmarks)
        {
            Stage("frame-gameview-bench");
            auto* benchCamera = scene.MainCamera();
            if (benchCamera != nullptr)
            {
                LamaPon::RenderTarget benchTarget;

                // GPUがコマンドを消化し終えるまで待ちます。待たずに
                // 計ると「積んだ時間」だけになり、描画の重さが
                // 見えません。
                Microsoft::WRL::ComPtr<ID3D11Query> fence;
                D3D11_QUERY_DESC queryDescription{};
                queryDescription.Query = D3D11_QUERY_EVENT;
                Require(
                    SUCCEEDED(graphics.Device()->CreateQuery(
                        &queryDescription,
                        fence.ReleaseAndGetAddressOf())),
                    "bench query creation must succeed");
                const auto waitForGpu = [&]
                {
                    graphics.Context()->End(fence.Get());
                    BOOL done = FALSE;
                    while (graphics.Context()->GetData(
                            fence.Get(),
                            &done,
                            sizeof(done),
                            0) != S_OK
                        || done == FALSE)
                    {
                    }
                };

                // 1枚ごとにフェンスで待つと、1回の提出にかかる固定費が
                // 結果をまるごと覆い隠します。実際、320x180から
                // 1280x720までどの解像度も16.6msに張り付いていました
                // （表示周期そのものの値です）。これでは解像度の差が
                // 見えないので、何枚かまとめて積んでから1度だけ待ち、
                // 1枚あたりへ割り戻します。
                constexpr int benchBatch = 8;
                constexpr int benchSamples = 5;

                Microsoft::WRL::ComPtr<ID3D11Query>
                    disjointQuery;
                Microsoft::WRL::ComPtr<ID3D11Query>
                    timestampBeginQuery;
                Microsoft::WRL::ComPtr<ID3D11Query>
                    timestampEndQuery;
                Microsoft::WRL::ComPtr<ID3D11Query>
                    pipelineStatisticsQuery;
                D3D11_QUERY_DESC disjointDescription{};
                disjointDescription.Query =
                    D3D11_QUERY_TIMESTAMP_DISJOINT;
                D3D11_QUERY_DESC timestampDescription{};
                timestampDescription.Query =
                    D3D11_QUERY_TIMESTAMP;
                D3D11_QUERY_DESC pipelineDescription{};
                pipelineDescription.Query =
                    D3D11_QUERY_PIPELINE_STATISTICS;
                Require(
                    SUCCEEDED(graphics.Device()->CreateQuery(
                        &disjointDescription,
                        disjointQuery.ReleaseAndGetAddressOf()))
                        && SUCCEEDED(graphics.Device()->CreateQuery(
                            &timestampDescription,
                            timestampBeginQuery
                                .ReleaseAndGetAddressOf()))
                        && SUCCEEDED(graphics.Device()->CreateQuery(
                            &timestampDescription,
                            timestampEndQuery
                                .ReleaseAndGetAddressOf()))
                        && SUCCEEDED(graphics.Device()->CreateQuery(
                            &pipelineDescription,
                            pipelineStatisticsQuery
                                .ReleaseAndGetAddressOf())),
                    "bench timestamp query creation must succeed");

                struct BenchMeasurement final
                {
                    double cpuMilliseconds{};
                    double gpuMilliseconds{};
                    std::uint64_t inputAssemblerVertices{};
                    std::uint64_t inputAssemblerPrimitives{};
                    std::uint64_t vertexShaderInvocations{};
                    std::uint64_t pixelShaderInvocations{};
                };

                const auto measure =
                    [&](const std::uint32_t width,
                        const std::uint32_t height,
                        const bool gamePath,
                        const int batch,
                        const int sampleCount)
                {
                    benchTarget.Resize(
                        graphics.Device(),
                        width,
                        height);
                    constexpr float benchClear[]{
                        0.0f, 0.0f, 0.0f, 1.0f };
                    const auto drawOnce = [&]
                    {
                        graphics.SetUIViewportSize(
                            width,
                            height);
                        benchTarget.Bind(graphics.Context());
                        benchTarget.Clear(
                            graphics.Context(),
                            benchClear);
                        if (gamePath)
                        {
                            scene.RenderMainCamera(
                                benchTarget.AspectRatio(),
                                false,
                                &benchTarget);
                        }
                        else
                        {
                            scene.RenderWithMatrices(
                                benchCamera->ViewMatrix(),
                                benchCamera->ProjectionMatrix(
                                    benchTarget
                                        .AspectRatio()),
                                false,
                                false,
                                &benchTarget);
                        }
                        LamaPon::RunPostProcess(
                            graphics,
                            benchTarget,
                            scene.PostProcessFrameData());
                        scene.Render2D();
                        benchTarget.CopyToDisplay(
                            graphics.Context());
                    };

                    // 1周目はシェーダーのバリアント切り替え等で
                    // 揺れるので捨てます。
                    drawOnce();
                    waitForGpu();

                    std::vector<double> cpuSamples;
                    std::vector<double> gpuSamples;
                    D3D11_QUERY_DATA_PIPELINE_STATISTICS
                        pipelineStatistics{};
                    for (int sample = 0;
                        sample < sampleCount;
                        ++sample)
                    {
                        graphics.Context()->Begin(
                            disjointQuery.Get());
                        graphics.Context()->Begin(
                            pipelineStatisticsQuery.Get());
                        graphics.Context()->End(
                            timestampBeginQuery.Get());
                        const auto cpuBegin =
                            std::chrono::steady_clock::now();
                        for (int frame = 0;
                            frame < batch;
                            ++frame)
                        {
                            drawOnce();
                        }
                        const auto cpuEnd =
                            std::chrono::steady_clock::now();
                        graphics.Context()->End(
                            timestampEndQuery.Get());
                        graphics.Context()->End(
                            pipelineStatisticsQuery.Get());
                        graphics.Context()->End(
                            disjointQuery.Get());

                        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT
                            disjointData{};
                        while (graphics.Context()->GetData(
                                disjointQuery.Get(),
                                &disjointData,
                                sizeof(disjointData),
                                0) != S_OK)
                        {
                            std::this_thread::yield();
                        }
                        std::uint64_t gpuBegin{};
                        std::uint64_t gpuEnd{};
                        while (graphics.Context()->GetData(
                                timestampBeginQuery.Get(),
                                &gpuBegin,
                                sizeof(gpuBegin),
                                0) != S_OK
                            || graphics.Context()->GetData(
                                timestampEndQuery.Get(),
                                &gpuEnd,
                                sizeof(gpuEnd),
                                0) != S_OK)
                        {
                            std::this_thread::yield();
                        }
                        while (graphics.Context()->GetData(
                                pipelineStatisticsQuery.Get(),
                                &pipelineStatistics,
                                sizeof(pipelineStatistics),
                                0) != S_OK)
                        {
                            std::this_thread::yield();
                        }
                        cpuSamples.push_back(
                            std::chrono::duration<
                                double,
                                std::milli>(
                                cpuEnd - cpuBegin).count()
                            / batch);
                        gpuSamples.push_back(
                            !disjointData.Disjoint
                                && disjointData.Frequency > 0
                                && gpuEnd >= gpuBegin
                            ? static_cast<double>(
                                gpuEnd - gpuBegin)
                                * 1000.0
                                / static_cast<double>(
                                    disjointData.Frequency)
                                / batch
                            : 0.0);
                    }
                    std::sort(
                        cpuSamples.begin(),
                        cpuSamples.end());
                    std::sort(
                        gpuSamples.begin(),
                        gpuSamples.end());
                    return BenchMeasurement{
                        cpuSamples[cpuSamples.size() / 2],
                        gpuSamples[gpuSamples.size() / 2],
                        pipelineStatistics.IAVertices
                            / static_cast<std::uint64_t>(batch),
                        pipelineStatistics.IAPrimitives
                            / static_cast<std::uint64_t>(batch),
                        pipelineStatistics.VSInvocations
                            / static_cast<std::uint64_t>(batch),
                        pipelineStatistics.PSInvocations
                            / static_cast<std::uint64_t>(batch)
                    };
                };

                const auto sceneSmall =
                    measure(
                        320, 180, false,
                        benchBatch, benchSamples);
                const auto gameSmall =
                    measure(
                        320, 180, true,
                        benchBatch, benchSamples);
                std::cout
                    << "gameview bench 320x180:"
                    << " scene-path cpu="
                    << sceneSmall.cpuMilliseconds << "ms gpu="
                    << sceneSmall.gpuMilliseconds << "ms"
                    << " game-path cpu="
                    << gameSmall.cpuMilliseconds << "ms gpu="
                    << gameSmall.gpuMilliseconds << "ms"
                    << std::endl;
                const auto game360 =
                    measure(
                        640, 360, true,
                        benchBatch, benchSamples);
                const auto game720 =
                    measure(
                        1280, 720, true,
                        benchBatch, benchSamples);
                const auto game1080 =
                    measure(
                        1920, 1080, true,
                        benchBatch, benchSamples);
                std::cout
                    << "gameview bench resolution sweep"
                    << " (game-path GPU):"
                    << " 640x360=" << game360.gpuMilliseconds << "ms"
                    << " 1280x720=" << game720.gpuMilliseconds << "ms"
                    << " 1920x1080=" << game1080.gpuMilliseconds << "ms"
                    << std::endl;

                Stage("frame-many-objects-bench");
                const auto savedCameraPosition =
                    cameraObject.GetTransform().position;
                const auto savedCameraRotation =
                    cameraObject.GetTransform().EulerAngles();
                const bool savedFrustumCulling =
                    scene.FrustumCullingEnabled();
                const bool savedOcclusionCulling =
                    scene.OcclusionCullingEnabled();
                const auto savedGraphicsSettings =
                    graphics.Settings();
                graphics.RefreshMemoryStatistics(true);
                const auto memoryBeforeStress =
                    graphics.MemoryStats();
                cameraObject.GetTransform().position =
                    { 0.0f, 0.0f, 8.0f };
                cameraObject.GetTransform().SetEulerAngles(
                    { 0.0f, 0.0f, 0.0f });
                scene.SetFrustumCullingEnabled(true);
                scene.SetOcclusionCullingEnabled(false);

                std::vector<LamaPon::GameObject*> stressObjects;
                constexpr int stressObjectCount = 2048;
                constexpr int stressVisibleCount = 256;
                stressObjects.reserve(stressObjectCount);
                for (int index = 0;
                    index < stressObjectCount;
                    ++index)
                {
                    auto& object = scene.CreateGameObject(
                        "Stress object " + std::to_string(index));
                    if (index < stressVisibleCount)
                    {
                        const int column = index % 32;
                        const int row = index / 32;
                        object.GetTransform().position = {
                            (static_cast<float>(column) - 15.5f)
                                * 1.15f,
                            (static_cast<float>(row) - 3.5f)
                                * 1.15f,
                            -24.0f - static_cast<float>(row)
                        };
                    }
                    else
                    {
                        object.GetTransform().position = {
                            1000.0f
                                + static_cast<float>(index) * 2.0f,
                            static_cast<float>(index % 32),
                            -24.0f
                        };
                    }
                    object.AddComponent<
                        LamaPon::MeshRendererComponent>();
                    stressObjects.push_back(&object);
                }
                static_cast<void>(scene.EvaluateRenderVisibility(
                    benchCamera->ViewMatrix(),
                    benchCamera->ProjectionMatrix(16.0f / 9.0f)));
                const auto visibilityBegin =
                    std::chrono::steady_clock::now();
                const auto stressVisibility =
                    scene.EvaluateRenderVisibility(
                        benchCamera->ViewMatrix(),
                        benchCamera->ProjectionMatrix(16.0f / 9.0f));
                const auto visibilityEnd =
                    std::chrono::steady_clock::now();
                const auto stressFrame =
                    measure(
                        1280, 720, true,
                        benchBatch, benchSamples);
                const auto& stressDrawStats =
                    scene.VisibilityStats();
                std::cout
                    << "many-object bench objects="
                    << stressObjectCount
                    << " visibility-cpu="
                    << std::chrono::duration<
                        double,
                        std::milli>(
                            visibilityEnd - visibilityBegin).count()
                    << "ms frame-cpu="
                    << stressFrame.cpuMilliseconds
                    << "ms frame-gpu="
                    << stressFrame.gpuMilliseconds
                    << "ms visible="
                    << stressVisibility.visibleRendererCount
                    << " bvh-nodes="
                    << stressVisibility.spatialNodeCount
                    << " bvh-tests="
                    << stressVisibility.spatialNodeTestCount
                    << " instanced="
                    << stressDrawStats.meshInstancedRendererCount
                    << std::endl;

                for (auto* object : stressObjects)
                {
                    static_cast<void>(
                        scene.DestroyGameObject(*object));
                }

                Stage("frame-all-visible-high-poly-bench");
                const auto baselineVisibility =
                    scene.EvaluateRenderVisibility(
                        benchCamera->ViewMatrix(),
                        benchCamera->ProjectionMatrix(
                            16.0f / 9.0f));
                std::vector<LamaPon::GameObject*>
                    highPolygonObjects;
                constexpr int highPolygonObjectCount = 2048;
                constexpr int highPolygonColumns = 64;
                highPolygonObjects.reserve(
                    highPolygonObjectCount);
                for (int index = 0;
                    index < highPolygonObjectCount;
                    ++index)
                {
                    const int column =
                        index % highPolygonColumns;
                    const int row =
                        index / highPolygonColumns;
                    auto& object = scene.CreateGameObject(
                        "High polygon sphere "
                        + std::to_string(index));
                    object.GetTransform().position = {
                        (static_cast<float>(column) - 31.5f)
                            * 2.0f,
                        (static_cast<float>(row) - 15.5f)
                            * 2.0f,
                        -150.0f
                    };
                    object.AddComponent<
                        LamaPon::MeshRendererComponent>(
                            LamaPon::PrimitiveShape::Sphere);
                    highPolygonObjects.push_back(&object);
                }
                const auto highVisibility =
                    scene.EvaluateRenderVisibility(
                        benchCamera->ViewMatrix(),
                        benchCamera->ProjectionMatrix(
                            16.0f / 9.0f));
                Require(
                    highVisibility.visibleRendererCount
                        >= baselineVisibility.visibleRendererCount
                            + highPolygonObjectCount,
                    "all-visible high-polygon benchmark must keep every sphere visible");
                const auto highFrame =
                    measure(1280, 720, true, 3, 3);
                Require(
                    highFrame.inputAssemblerPrimitives
                        >= static_cast<std::uint64_t>(
                            highPolygonObjectCount),
                    "high-polygon benchmark must submit geometry to the GPU");
                graphics.RefreshMemoryStatistics(true);
                const auto memoryWithHighPolygon =
                    graphics.MemoryStats();
                constexpr double bytesPerMiB =
                    1024.0 * 1024.0;
                const auto signedMiB = [](
                    const std::uint64_t after,
                    const std::uint64_t before)
                {
                    return static_cast<double>(
                        static_cast<std::int64_t>(after)
                        - static_cast<std::int64_t>(before))
                        / (1024.0 * 1024.0);
                };
                std::cout
                    << "all-visible high-poly bench objects="
                    << highPolygonObjectCount
                    << " cpu=" << highFrame.cpuMilliseconds << "ms"
                    << " gpu=" << highFrame.gpuMilliseconds << "ms"
                    << " ia-primitives="
                    << highFrame.inputAssemblerPrimitives
                    << " vs=" << highFrame.vertexShaderInvocations
                    << " ps=" << highFrame.pixelShaderInvocations
                    << " ram-delta="
                    << signedMiB(
                        memoryWithHighPolygon.processPrivateBytes,
                        memoryBeforeStress.processPrivateBytes)
                    << "MiB"
                    << std::endl;

                Stage("frame-quality-preset-bench");
                constexpr std::array qualityPresets{
                    LamaPon::GraphicsQualityPreset::Low,
                    LamaPon::GraphicsQualityPreset::Medium,
                    LamaPon::GraphicsQualityPreset::High,
                    LamaPon::GraphicsQualityPreset::Ultra
                };
                for (const auto preset : qualityPresets)
                {
                    auto presetSettings =
                        LamaPon::GraphicsSettingsForPreset(preset);
                    presetSettings.vSyncEnabled = false;
                    presetSettings.targetFrameRate = 0;
                    presetSettings.renderingPath =
                        savedGraphicsSettings.renderingPath;
                    graphics.SetGraphicsSettings(presetSettings);
                    const auto presetFrame =
                        measure(1280, 720, true, 2, 3);
                    graphics.RefreshMemoryStatistics(true);
                    const auto& presetMemory =
                        graphics.MemoryStats();
                    std::cout
                        << "quality bench preset="
                        << LamaPon::GraphicsQualityPresetName(
                            preset)
                        << " scale=" << presetSettings.renderScale
                        << " lod="
                        << presetSettings.automaticLodQuality
                        << " cpu=" << presetFrame.cpuMilliseconds
                        << "ms gpu=" << presetFrame.gpuMilliseconds
                        << "ms primitives="
                        << presetFrame.inputAssemblerPrimitives
                        << " vram="
                        << static_cast<double>(
                            presetMemory.localVideoMemoryUsageBytes)
                            / bytesPerMiB
                        << "/"
                        << static_cast<double>(
                            presetMemory.localVideoMemoryBudgetBytes)
                            / bytesPerMiB
                        << "MiB"
                        << std::endl;
                }
                graphics.SetGraphicsSettings(
                    savedGraphicsSettings);

                for (auto* object : highPolygonObjects)
                {
                    static_cast<void>(
                        scene.DestroyGameObject(*object));
                }

                Stage("frame-texture-memory-bench");
                graphics.RefreshMemoryStatistics(true);
                const auto textureMemoryBefore =
                    graphics.MemoryStats();
                std::vector<
                    Microsoft::WRL::ComPtr<ID3D11Texture2D>>
                    stressTextures;
                std::vector<
                    Microsoft::WRL::ComPtr<
                        ID3D11ShaderResourceView>>
                    stressTextureViews;
                constexpr std::size_t textureStressCount = 128;
                constexpr std::uint32_t textureStressSize = 512;
                stressTextures.reserve(textureStressCount);
                stressTextureViews.reserve(textureStressCount);
                D3D11_TEXTURE2D_DESC textureDescription{};
                textureDescription.Width = textureStressSize;
                textureDescription.Height = textureStressSize;
                textureDescription.MipLevels = 1;
                textureDescription.ArraySize = 1;
                textureDescription.Format =
                    DXGI_FORMAT_R8G8B8A8_UNORM;
                textureDescription.SampleDesc.Count = 1;
                textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
                textureDescription.BindFlags =
                    D3D11_BIND_SHADER_RESOURCE;
                std::vector<std::uint32_t> texturePixels(
                    static_cast<std::size_t>(textureStressSize)
                        * textureStressSize,
                    0xff7f3f1fu);
                D3D11_SUBRESOURCE_DATA initialTextureData{};
                initialTextureData.pSysMem = texturePixels.data();
                initialTextureData.SysMemPitch =
                    textureStressSize * sizeof(std::uint32_t);
                for (std::size_t index = 0;
                    index < textureStressCount;
                    ++index)
                {
                    texturePixels[
                        index % texturePixels.size()] =
                        0xff000000u
                        | static_cast<std::uint32_t>(
                            index * 2654435761u);
                    Microsoft::WRL::ComPtr<ID3D11Texture2D>
                        texture;
                    if (FAILED(graphics.Device()->CreateTexture2D(
                            &textureDescription,
                            &initialTextureData,
                            texture.ReleaseAndGetAddressOf())))
                    {
                        break;
                    }
                    Microsoft::WRL::ComPtr<
                        ID3D11ShaderResourceView> view;
                    if (FAILED(graphics.Device()->
                            CreateShaderResourceView(
                                texture.Get(),
                                nullptr,
                                view.ReleaseAndGetAddressOf())))
                    {
                        break;
                    }
                    stressTextures.push_back(std::move(texture));
                    stressTextureViews.push_back(std::move(view));
                }
                Require(
                    stressTextures.size() >= 16,
                    "texture memory benchmark must allocate at least 16 textures");
                Microsoft::WRL::ComPtr<ID3D11Texture2D>
                    textureResidencyProbe;
                D3D11_TEXTURE2D_DESC residencyProbeDescription =
                    textureDescription;
                residencyProbeDescription.Usage = D3D11_USAGE_DEFAULT;
                Require(
                    SUCCEEDED(graphics.Device()->CreateTexture2D(
                        &residencyProbeDescription,
                        nullptr,
                        textureResidencyProbe.ReleaseAndGetAddressOf())),
                    "texture memory benchmark residency probe creation must succeed");
                for (const auto& texture : stressTextures)
                {
                    graphics.Context()->CopyResource(
                        textureResidencyProbe.Get(),
                        texture.Get());
                }
                waitForGpu();
                graphics.RefreshMemoryStatistics(true);
                const auto textureMemoryAfter =
                    graphics.MemoryStats();
                std::cout
                    << "texture memory bench textures="
                    << stressTextures.size()
                    << " size=" << textureStressSize << "x"
                    << textureStressSize
                    << " logical="
                    << static_cast<double>(
                        stressTextures.size()
                        * textureStressSize
                        * textureStressSize
                        * 4ull) / bytesPerMiB
                    << "MiB ram-delta="
                    << signedMiB(
                        textureMemoryAfter.processPrivateBytes,
                        textureMemoryBefore.processPrivateBytes)
                    << "MiB vram-delta="
                    << signedMiB(
                        textureMemoryAfter.localVideoMemoryUsageBytes,
                        textureMemoryBefore.localVideoMemoryUsageBytes)
                    << "MiB shared-delta="
                    << signedMiB(
                        textureMemoryAfter.nonLocalVideoMemoryUsageBytes,
                        textureMemoryBefore.nonLocalVideoMemoryUsageBytes)
                    << "MiB"
                    << std::endl;

                cameraObject.GetTransform().position =
                    savedCameraPosition;
                cameraObject.GetTransform().SetEulerAngles(
                    savedCameraRotation);
                scene.SetFrustumCullingEnabled(
                    savedFrustumCulling);
                scene.SetOcclusionCullingEnabled(
                    savedOcclusionCulling);
            }
        }

        // ---- 法線マップ: BC5でも同じ絵になるか ----
        //
        // 法線マップはBC5（RGの2チャンネル）で読み込まれるように
        // なり、シェーダーはZをxyから復元します。ここでは同じ構図で
        // 3枚撮って突き合わせます。背景の物体は3枚とも同じなので、
        // 差はこの平面の陰影にだけ出ます。
        //   ①法線マップなし ②非圧縮の法線マップ ③BC5の法線マップ
        // ①と②が違えば法線マップが効いていて、②と③が近ければ
        // BC5にしても絵が変わっていない、と言えます。
        {
            const auto normalMapDirectory =
                std::filesystem::temp_directory_path()
                / L"LamaPonNormalMapTest";
            std::error_code directoryError;
            std::filesystem::create_directories(
                normalMapDirectory,
                directoryError);
            // 2枚に分けるのは、読み込み済みテクスチャがパスで
            // キャッシュされるためです。同じパスだと圧縮設定を
            // 切り替えても前の結果が返ってきます。
            const auto rawNormalPath =
                normalMapDirectory / L"normal-raw.png";
            const auto compressedNormalPath =
                normalMapDirectory / L"normal-bc5.png";

            // xとyで別々に波打たせます。片方だけだとZの復元を
            // 間違えても気付けません。
            constexpr std::uint32_t normalMapSize = 64;
            std::vector<std::uint8_t> normalPixels(
                static_cast<std::size_t>(normalMapSize)
                    * normalMapSize * 4);
            for (std::uint32_t y = 0; y < normalMapSize; ++y)
            {
                for (std::uint32_t x = 0; x < normalMapSize; ++x)
                {
                    const float horizontal = std::sin(
                        static_cast<float>(x)
                        * 6.28318530718f / 16.0f);
                    const float vertical = std::cos(
                        static_cast<float>(y)
                        * 6.28318530718f / 12.0f);
                    const float normalX = 0.55f * horizontal;
                    const float normalY = 0.55f * vertical;
                    const float normalZ = std::sqrt(
                        std::max(
                            0.0f,
                            1.0f
                                - normalX * normalX
                                - normalY * normalY));
                    const auto encode =
                        [](const float value)
                        {
                            return static_cast<std::uint8_t>(
                                std::clamp(
                                    (value * 0.5f + 0.5f)
                                        * 255.0f,
                                    0.0f,
                                    255.0f));
                        };
                    const std::size_t offset =
                        (static_cast<std::size_t>(y)
                            * normalMapSize + x) * 4;
                    normalPixels[offset] = encode(normalX);
                    normalPixels[offset + 1] = encode(normalY);
                    normalPixels[offset + 2] = encode(normalZ);
                    normalPixels[offset + 3] = 255;
                }
            }
            LamaPon::SavePng(
                rawNormalPath,
                normalMapSize,
                normalMapSize,
                normalPixels);
            LamaPon::SavePng(
                compressedNormalPath,
                normalMapSize,
                normalMapSize,
                normalPixels);

            subject.SetEnabled(false);
            sunObject.SetEnabled(true);
            cameraObject.GetTransform().position =
                { 0.0f, 0.0f, 6.0f };
            cameraObject.GetTransform().SetEulerAngles(
                { 0.0f, 0.0f, 0.0f });
            auto& normalObject =
                scene.CreateGameObject("NormalMapSubject");
            normalObject.GetTransform().position =
                { 0.0f, 0.0f, 0.0f };
            normalObject.GetTransform().scale =
                { 5.0f, 5.0f, 0.5f };
            auto& normalRenderer = normalObject.AddComponent<
                LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.85f, 0.85f, 0.85f, 1.0f },
                std::filesystem::path{},
                std::filesystem::path{},
                0.35f,
                1.0f);

            Stage("frame-normalmap-none");
            const auto normalNoneFrame = renderFrame();
            DumpFrame("normalmap-none", normalNoneFrame);

            graphics.Assets()
                .SetRuntimeTextureCompressionEnabled(false);
            normalRenderer.SetNormalTexturePath(rawNormalPath);
            Stage("frame-normalmap-raw");
            const auto normalRawFrame = renderFrame();
            DumpFrame("normalmap-raw", normalRawFrame);

            graphics.Assets()
                .SetRuntimeTextureCompressionEnabled(true);
            normalRenderer.SetNormalTexturePath(
                compressedNormalPath);
            Stage("frame-normalmap-bc5");
            const auto normalBc5Frame = renderFrame();
            DumpFrame("normalmap-bc5", normalBc5Frame);

            // 本当にBC5になっているかをフォーマットで確かめます。
            // 絵が同じでも「圧縮されていないから同じ」では意味が
            // ありません。
            const auto compressedAsset =
                graphics.Assets().LoadTexture(
                    compressedNormalPath,
                    LamaPon::TextureLoader::TextureUsage::
                        NormalMap);
            Require(
                compressedAsset != nullptr
                    && compressedAsset->view != nullptr,
                "the compressed normal map must load");
            Microsoft::WRL::ComPtr<ID3D11Resource>
                compressedResource;
            compressedAsset->view->GetResource(
                compressedResource.GetAddressOf());
            Microsoft::WRL::ComPtr<ID3D11Texture2D>
                compressedTexture;
            Require(
                SUCCEEDED(compressedResource.As(
                    &compressedTexture)),
                "the normal map must be a 2D texture");
            D3D11_TEXTURE2D_DESC compressedDescription{};
            compressedTexture->GetDesc(&compressedDescription);
            Require(
                compressedDescription.Format
                    == DXGI_FORMAT_BC5_UNORM,
                "the normal map must actually be BC5");

            const auto changedPixelCount =
                [](const std::vector<std::uint8_t>& left,
                    const std::vector<std::uint8_t>& right,
                    const int tolerance)
                {
                    std::size_t changed = 0;
                    for (std::size_t index = 0;
                        index + 3 < left.size()
                            && index + 3 < right.size();
                        index += 4)
                    {
                        int worst = 0;
                        for (std::size_t channel = 0;
                            channel < 3;
                            ++channel)
                        {
                            worst = std::max(
                                worst,
                                std::abs(
                                    static_cast<int>(
                                        left[index + channel])
                                    - static_cast<int>(
                                        right[index + channel])));
                        }
                        if (worst > tolerance)
                        {
                            ++changed;
                        }
                    }
                    return changed;
                };
            // 平均は薄まるので2乗差で見ます（1画素あたり）。
            const auto meanSquaredDifference =
                [](const std::vector<std::uint8_t>& left,
                    const std::vector<std::uint8_t>& right)
                {
                    double total = 0.0;
                    std::size_t samples = 0;
                    for (std::size_t index = 0;
                        index + 3 < left.size()
                            && index + 3 < right.size();
                        index += 4)
                    {
                        for (std::size_t channel = 0;
                            channel < 3;
                            ++channel)
                        {
                            const double delta =
                                static_cast<double>(
                                    left[index + channel])
                                - static_cast<double>(
                                    right[index + channel]);
                            total += delta * delta;
                            ++samples;
                        }
                    }
                    return samples == 0
                        ? 0.0
                        : total / static_cast<double>(samples);
                };

            const auto mapEffect = changedPixelCount(
                normalNoneFrame,
                normalRawFrame,
                8);
            const auto compressionEffect = changedPixelCount(
                normalRawFrame,
                normalBc5Frame,
                8);
            const double compressionError =
                meanSquaredDifference(
                    normalRawFrame,
                    normalBc5Frame);
            std::cout
                << "normal map bench changed-by-map="
                << mapEffect
                << " changed-by-bc5=" << compressionEffect
                << " bc5-mse=" << compressionError
                << std::endl;
            // 実測は2209画素（320x180のうち3.8%）。GPUが変われば
            // 多少動くので、半分に落ちても気付ける値にしています。
            Require(
                mapEffect > 1000,
                "the normal map must visibly change the shading");
            Require(
                compressionEffect * 8 < mapEffect,
                "BC5 must change far less than the normal map itself");
            Require(
                compressionError < 12.0,
                "BC5 shading error must stay small per pixel");
        }
    }
    catch (const std::system_error& error)
    {
        std::cerr
            << "Engine render tests failed (system_error "
            << error.code().value()
            << "): "
            << error.what()
            << '\n';
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "Engine render tests failed: "
            << error.what()
            << '\n';
        return 1;
    }

    // シェーダーのコンパイル状況。キャッシュが効いているかを
    // 数字で確かめるために出します（1回目はcompiled中心、2回目は
    // cacheHit中心になるのが正しい姿）。
    {
        const auto stats = LamaPon::ShaderCompileStatistics();
        std::cout
            << "shader compile: compiled="
            << stats.compiledCount
            << " (" << stats.compileMilliseconds << "ms)"
            << " cacheHit=" << stats.cacheHitCount
            << " (" << stats.cacheReadMilliseconds << "ms)"
            << std::endl;
    }
    std::cout << "Engine render tests passed.\n";
    return 0;
}
