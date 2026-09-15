#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/SdkmeshImporter.h"
#include "LamaPon/Assets/TextureLoader.h"
#include "LamaPon/Assets/VboImporter.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/PointLightComponent.h"
#include "LamaPon/Components/ReflectionProbeComponent.h"
#include "LamaPon/Components/SpotLightComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Access.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShaderCompiler.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/SkeletalModel.h"
#include "LamaPon/Graphics/SpriteRendering.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/SceneManager.h"
#include "LamaPon/Scene/GameObject.h"

#include <Windows.h>
#include <objbase.h>
#include <DirectXPackedVector.h>
#include <CommonStates.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    // 2の累乗にしてviewport変換の係数を誤差なく表し、D3D11とD3D12の
    // 頂点位置が同じ丸めになるようにします。
    constexpr std::uint32_t CanvasWidth = 256u;
    constexpr std::uint32_t CanvasHeight = 128u;

    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    constexpr std::uint32_t MakeFourCc(
        const char a,
        const char b,
        const char c,
        const char d) noexcept
    {
        return static_cast<std::uint8_t>(a)
            | static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(b)) << 8u
            | static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(c)) << 16u
            | static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(d)) << 24u;
    }

    void WriteLittleEndian32(
        std::vector<std::uint8_t>& bytes,
        const std::size_t offset,
        const std::uint32_t value)
    {
        Require(
            offset <= bytes.size()
                && sizeof(value) <= bytes.size() - offset,
            "The DDS test header offset is invalid");
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    [[nodiscard]] std::vector<std::uint8_t> BuildClassicDds(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint32_t mipCount,
        const std::uint32_t fourCc,
        const std::vector<std::uint8_t>& payload)
    {
        std::vector<std::uint8_t> bytes(128u);
        WriteLittleEndian32(bytes, 0u, MakeFourCc('D', 'D', 'S', ' '));
        WriteLittleEndian32(bytes, 4u, 124u);
        WriteLittleEndian32(bytes, 12u, height);
        WriteLittleEndian32(bytes, 16u, width);
        WriteLittleEndian32(bytes, 28u, mipCount);
        WriteLittleEndian32(bytes, 76u, 32u);
        WriteLittleEndian32(bytes, 80u, 0x4u);
        WriteLittleEndian32(bytes, 84u, fourCc);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return bytes;
    }

    [[nodiscard]] std::vector<std::uint8_t> BuildRgbaDds()
    {
        std::vector<std::uint8_t> bytes(128u);
        WriteLittleEndian32(bytes, 0u, MakeFourCc('D', 'D', 'S', ' '));
        WriteLittleEndian32(bytes, 4u, 124u);
        WriteLittleEndian32(bytes, 12u, 4u);
        WriteLittleEndian32(bytes, 16u, 4u);
        WriteLittleEndian32(bytes, 28u, 2u);
        WriteLittleEndian32(bytes, 76u, 32u);
        WriteLittleEndian32(bytes, 80u, 0x41u);
        WriteLittleEndian32(bytes, 88u, 32u);
        WriteLittleEndian32(bytes, 92u, 0x000000ffu);
        WriteLittleEndian32(bytes, 96u, 0x0000ff00u);
        WriteLittleEndian32(bytes, 100u, 0x00ff0000u);
        WriteLittleEndian32(bytes, 104u, 0xff000000u);
        for (std::size_t pixel{}; pixel < 16u; ++pixel)
        {
            bytes.insert(bytes.end(), { 220u, 40u, 20u, 255u });
        }
        for (std::size_t pixel{}; pixel < 4u; ++pixel)
        {
            bytes.insert(bytes.end(), { 20u, 40u, 220u, 255u });
        }
        return bytes;
    }

    [[nodiscard]] std::vector<std::uint8_t> BuildDx10Dds(
        const DXGI_FORMAT format,
        const std::vector<std::uint8_t>& payload)
    {
        auto bytes = BuildClassicDds(
            4u,
            4u,
            1u,
            MakeFourCc('D', 'X', '1', '0'),
            {});
        bytes.resize(148u);
        WriteLittleEndian32(bytes, 128u, static_cast<std::uint32_t>(format));
        WriteLittleEndian32(bytes, 132u, 3u);
        WriteLittleEndian32(bytes, 140u, 1u);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return bytes;
    }

    // 4x4 RGBA8で1ミップのcube DDSです。面は+X、-X、+Y、-Y、+Z、-Zの
    // 順で、caps2へDDSCAPS2_CUBEMAPと6面すべての印を立てます。
    [[nodiscard]] std::vector<std::uint8_t> BuildCubeDds(
        const std::array<std::array<std::uint8_t, 3>, 6>& faceColors)
    {
        std::vector<std::uint8_t> bytes(128u);
        WriteLittleEndian32(bytes, 0u, MakeFourCc('D', 'D', 'S', ' '));
        WriteLittleEndian32(bytes, 4u, 124u);
        WriteLittleEndian32(bytes, 12u, 4u);
        WriteLittleEndian32(bytes, 16u, 4u);
        WriteLittleEndian32(bytes, 28u, 1u);
        WriteLittleEndian32(bytes, 76u, 32u);
        WriteLittleEndian32(bytes, 80u, 0x41u);
        WriteLittleEndian32(bytes, 88u, 32u);
        WriteLittleEndian32(bytes, 92u, 0x000000ffu);
        WriteLittleEndian32(bytes, 96u, 0x0000ff00u);
        WriteLittleEndian32(bytes, 100u, 0x00ff0000u);
        WriteLittleEndian32(bytes, 104u, 0xff000000u);
        WriteLittleEndian32(bytes, 112u, 0xfe00u);
        for (const auto& color : faceColors)
        {
            for (std::size_t pixel{}; pixel < 16u; ++pixel)
            {
                bytes.insert(
                    bytes.end(),
                    { color[0], color[1], color[2], 255u });
            }
        }
        return bytes;
    }

    class HiddenWindow final
    {
    public:
        HiddenWindow(
            const std::uint32_t width,
            const std::uint32_t height)
            : m_instance(GetModuleHandleW(nullptr))
        {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = DefWindowProcW;
            windowClass.hInstance = m_instance;
            windowClass.lpszClassName = ClassName;
            m_class = RegisterClassExW(&windowClass);
            if (m_class == 0)
            {
                throw std::runtime_error(
                    "The sprite rendering test window class could not be "
                    "registered");
            }

            m_window = CreateWindowExW(
                0,
                ClassName,
                L"LamaPonD3D12SpriteRenderingTests",
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
                    "The sprite rendering test window could not be created");
            }
        }

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

        HiddenWindow(const HiddenWindow&) = delete;
        HiddenWindow& operator=(const HiddenWindow&) = delete;

        [[nodiscard]] HWND Get() const noexcept
        {
            return m_window;
        }

    private:
        static constexpr const wchar_t* ClassName =
            L"LamaPonD3D12SpriteRenderingTests";

        HINSTANCE m_instance{};
        ATOM m_class{};
        HWND m_window{};
    };

    struct Capture final
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> pixels;
    };

    [[nodiscard]] DirectX::XMFLOAT4 Premultiplied(
        const DirectX::XMFLOAT4& color) noexcept
    {
        return {
            color.x * color.w,
            color.y * color.w,
            color.z * color.w,
            color.w
        };
    }

    void DrawRectangle(
        const LamaPon::SpriteRenderPass& pass,
        const float x,
        const float y,
        const float width,
        const float height,
        const DirectX::XMFLOAT4& color)
    {
        LamaPon::SpriteDrawRequest request;
        request.position = { x, y };
        request.scale = { width, height };
        request.tint = Premultiplied(color);
        Require(pass.Draw(request), "A sprite rectangle was rejected");
    }

    // D3D11とD3D12の両方で、同じ順序・同じ内容のSprite passを描きます。
    // 文字texture、source rectangle、原点、回転、flip、負のscale、入れ子の
    // scissor、各blend mode、End無しのpass破棄までを1枚へまとめます。
    void DrawSpriteScene(LamaPon::GraphicsDevice& graphics)
    {
        LamaPon::SceneLoadingScreenSettings loading;
        loading.enabled = true;
        loading.message = "Loading";
        loading.showPercentage = true;
        graphics.DrawLoadingScreen(
            0.4f,
            loading,
            CanvasWidth,
            CanvasHeight);

        const auto circle = graphics.Assets().LoadTexture("builtin/circle");
        const auto circleResources = circle != nullptr
            ? circle->resources.Acquire()
            : nullptr;
        Require(
            circleResources != nullptr
                && circleResources->shaderResourceView,
            "The built-in circle texture has no shader resource view");
        const auto circleView = circleResources->shaderResourceView;

        {
            auto pass = graphics.BeginSpritePass();
            DrawRectangle(
                pass,
                6.0f,
                8.0f,
                40.0f,
                18.0f,
                { 0.9f, 0.2f, 0.1f, 1.0f });
            DrawRectangle(
                pass,
                28.0f,
                14.0f,
                36.0f,
                30.0f,
                { 0.1f, 0.3f, 0.9f, 0.5f });

            LamaPon::SpriteDrawRequest rotated;
            rotated.texture = circleView;
            rotated.hasSourceRectangle = true;
            rotated.sourceRectangle = { 32, 48, 224, 208 };
            rotated.position = { 96.0f, 40.0f };
            rotated.origin = { 96.0f, 80.0f };
            rotated.scale = { 0.2f, 0.25f };
            rotated.rotation = 0.6f;
            rotated.flip = LamaPon::SpriteFlip::Horizontal;
            rotated.tint = { 0.2f, 0.9f, 0.3f, 1.0f };
            Require(
                pass.Draw(rotated),
                "A rotated source-rectangle sprite was rejected");

            LamaPon::SpriteDrawRequest whole;
            whole.texture = circleView;
            whole.position = { 150.0f, 6.0f };
            whole.origin = { 20.0f, 10.0f };
            whole.scale = { 0.18f, 0.12f };
            whole.flip = LamaPon::SpriteFlip::Both;
            whole.tint = { 1.0f, 0.8f, 0.2f, 0.8f };
            Require(pass.Draw(whole), "A whole-texture sprite was rejected");

            // 負のscaleは通常passでは裏向きとしてcullされ、UI clipping用の
            // scissor passではD3D11と同じく描かれます。
            auto mirrored = whole;
            mirrored.position = { 230.0f, 70.0f };
            mirrored.scale = { -0.1f, 0.1f };
            mirrored.flip = LamaPon::SpriteFlip::None;
            Require(pass.Draw(mirrored), "A mirrored sprite was rejected");

            Require(
                pass.PushScissor({ 12.5f, 60.0f, 120.0f, 118.0f }),
                "A sprite scissor was rejected");
            DrawRectangle(
                pass,
                0.0f,
                50.0f,
                256.0f,
                78.0f,
                { 0.8f, 0.8f, 0.2f, 0.6f });
            auto clipped = mirrored;
            clipped.position = { 100.0f, 70.0f };
            Require(pass.Draw(clipped), "A clipped sprite was rejected");
            Require(
                pass.PushScissor({ 30.0f, 40.0f, 200.0f, 90.0f }),
                "A nested sprite scissor was rejected");
            DrawRectangle(
                pass,
                0.0f,
                0.0f,
                256.0f,
                128.0f,
                { 0.1f, 0.9f, 0.9f, 0.4f });
            Require(pass.PopScissor(), "A nested sprite scissor was not popped");
            Require(pass.PopScissor(), "A sprite scissor was not popped");
            Require(
                !pass.PopScissor(),
                "An empty sprite scissor stack was popped");
            DrawRectangle(
                pass,
                200.0f,
                100.0f,
                50.0f,
                20.0f,
                { 0.5f, 0.1f, 0.6f, 1.0f });

            LamaPon::SpriteDrawRequest emptySource;
            emptySource.hasSourceRectangle = true;
            emptySource.sourceRectangle = { 4, 4, 4, 8 };
            Require(
                !pass.Draw(emptySource),
                "An empty source rectangle was accepted");
            pass.End();
        }

        const std::array blendModes{
            LamaPon::SpriteBlendMode::Additive,
            LamaPon::SpriteBlendMode::AlphaBlend,
            LamaPon::SpriteBlendMode::Opaque
        };
        for (std::size_t index{}; index < blendModes.size(); ++index)
        {
            LamaPon::SpritePassDescription description;
            description.blend = blendModes[index];
            auto pass = graphics.BeginSpritePass(description);
            const float offset = static_cast<float>(index) * 18.0f;
            LamaPon::SpriteDrawRequest request;
            request.texture = circleView;
            request.position = { 140.0f + offset, 64.0f + offset * 0.5f };
            request.scale = { 0.16f, 0.16f };
            request.tint = { 0.6f, 0.3f, 0.9f, 0.55f };
            Require(pass.Draw(request), "A blended sprite was rejected");
            DrawRectangle(
                pass,
                120.0f + offset,
                96.0f,
                14.0f,
                10.0f,
                { 0.9f, 0.9f, 0.9f, 0.35f });
            pass.End();
        }

        // EndせずにSpriteRenderPassを破棄しても、SpriteBatchと同じく積んだ
        // Spriteを描いてpassを閉じます。
        {
            auto pass = graphics.BeginSpritePass();
            DrawRectangle(
                pass,
                180.0f,
                30.0f,
                20.0f,
                20.0f,
                { 0.0f, 1.0f, 0.0f, 1.0f });
        }

        // b0のカスタム値とSV_Positionを使う同梱Shaderで、青い長方形の
        // 中央部分だけを描きます。D3D11 / D3D12のキャプチャ比較に
        // 入るため、単にcompileできるだけでなく定数bindも検証できます。
        {
            LamaPon::SpritePassDescription description;
            description.pixelShader =
                "shaders/LamaPonSpriteMask.hlsl";
            description.blend = LamaPon::SpriteBlendMode::Opaque;
            description.customParameters[0] = {
                0.0f, 0.0f, 0.0f, 0.0f };
            description.customParameters[1] = {
                206.0f, 24.0f, 12.0f, 7.0f };
            auto pass = graphics.BeginSpritePass(description);
            const auto status = pass.ShaderStatus();
            Require(
                status.fallback == LamaPon::SpriteShaderFallback::None
                    && status.error.empty()
                    && status.generation != 0,
                "The custom sprite shader did not prepare successfully "
                "(fallback "
                    + std::to_string(static_cast<int>(status.fallback))
                    + ", generation "
                    + std::to_string(status.generation)
                    + ", error: "
                    + status.error
                    + ")");
            DrawRectangle(
                pass,
                188.0f,
                12.0f,
                36.0f,
                24.0f,
                { 0.1f, 0.25f, 0.95f, 1.0f });
        }

        // b1のLight2D bufferも外部Shaderへ渡ることを画素で確かめます。
        {
            LamaPon::SpritePassDescription description;
            description.pixelShader =
                "shaders/LamaPonSpriteLit.hlsl";
            description.blend = LamaPon::SpriteBlendMode::Opaque;
            description.lighting.counts.x = 1u;
            description.lighting.lights[0].positionRadiusIntensity = {
                240.0f, 84.0f, 14.0f, 1.0f };
            description.lighting.lights[0].color = {
                0.8f, 0.15f, 0.05f, 0.0f };
            auto pass = graphics.BeginSpritePass(description);
            const auto status = pass.ShaderStatus();
            Require(
                status.fallback == LamaPon::SpriteShaderFallback::None
                    && status.error.empty()
                    && status.generation != 0,
                "The lit sprite shader did not prepare successfully");
            DrawRectangle(
                pass,
                228.0f,
                76.0f,
                24.0f,
                16.0f,
                { 0.25f, 0.25f, 0.25f, 1.0f });
        }
    }

    void RequireD3D12MissingCustomShaderFallback(
        LamaPon::GraphicsDevice& graphics)
    {
        LamaPon::SpritePassDescription description;
        description.pixelShader = "shaders/does-not-exist.hlsl";
        auto pass = graphics.BeginSpritePass(description);
        const auto status = pass.ShaderStatus();
        Require(
            status.fallback
                    == LamaPon::SpriteShaderFallback::DefaultPipeline
                && !status.error.empty(),
            "A missing DirectX 12 custom sprite shader did not report its "
            "default pipeline fallback");
        pass.End();
    }

    [[nodiscard]] Capture RenderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The requested rendering API did not start on WARP");
        Require(
            static_cast<bool>(graphics.WhiteTextureViewHandle()),
            "The graphics device has no sprite fallback texture");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);

        constexpr float ClearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        Capture capture;
        graphics.BeginFrame(ClearColor);
        DrawSpriteScene(graphics);
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        if (api == LamaPon::RenderingApi::DirectX12Experimental)
        {
            Require(
                graphics.IsD3D12ExperimentalBootstrap(),
                "The DirectX 12 sprite test did not use the bootstrap path");
            RequireD3D12MissingCustomShaderFallback(graphics);
        }
        graphics.EndFrame();

        // 2フレーム目は別のframe allocator、upload領域、遅延解放済みの
        // descriptorを通っても同じ画像になることを確かめます。
        graphics.BeginFrame(ClearColor);
        DrawSpriteScene(graphics);
        std::uint32_t secondWidth{};
        std::uint32_t secondHeight{};
        const auto secondPixels = graphics.CaptureBackBuffer(
            secondWidth,
            secondHeight);
        graphics.EndFrame();
        Require(
            secondWidth == capture.width
                && secondHeight == capture.height
                && secondPixels == capture.pixels,
            "A repeated sprite frame did not reproduce the first frame");
        return capture;
    }

    // bright-dotのScreenEffectをトーンマップ後へ掛け、D3D11と同じ
    // VSMain／PSMain、b0のカスタム値、t0のScene色で描くことを比べます。
    [[nodiscard]] Capture RenderCustomScreenEffectCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        // D3D11の合成はEnvironment shaderをasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);

        constexpr float clearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        {
            auto pass = graphics.BeginSpritePass();
            DrawRectangle(
                pass,
                24.0f,
                20.0f,
                80.0f,
                48.0f,
                { 0.15f, 0.35f, 0.7f, 1.0f });
        }

        LamaPon::ScreenEffectRequest request;
        request.shader =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/bright-dot.hlsl";
        request.customParameters[0] = {
            0.08f, 0.75f, 0.0f, 0.0f };
        request.point = LamaPon::ScreenEffectPoint::AfterToneMapping;
        std::uint64_t generation{};
        std::string error;
        Require(
            graphics.QueueScreenEffect(request, &generation, &error)
                && generation != 0
                && error.empty(),
            "The custom screen effect was not queued: " + error);

        LamaPon::PostProcessFrame frame;
        graphics.EndSceneComposition(frame);
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        const std::size_t center =
            (static_cast<std::size_t>(CanvasHeight / 2u) * CanvasWidth
                + CanvasWidth / 2u) * 4u;
        const auto describeCenter = [&capture, center]()
        {
            return capture.pixels.size() > center + 3u
                ? std::to_string(capture.pixels[center]) + ", "
                    + std::to_string(capture.pixels[center + 1u]) + ", "
                    + std::to_string(capture.pixels[center + 2u])
                : std::string("missing");
        };
        Require(
            capture.pixels.size() > center + 3u
                && capture.pixels[center] >= 185u
                && capture.pixels[center] <= 195u
                && capture.pixels[center + 1u] >= 185u
                && capture.pixels[center + 1u] <= 195u,
            std::string("The custom screen effect did not draw its center "
                "square on ")
                + (api == LamaPon::RenderingApi::DirectX11
                    ? "DirectX 11"
                    : "DirectX 12")
                + " (" + describeCenter() + ")");
        return capture;
    }

    void RequireNoD3D12DebugErrors()
    {
        for (const auto& entry : LamaPon::Logger::Instance().Snapshot())
        {
            if (entry.level == LamaPon::LogLevel::Error
                && entry.message.starts_with("D3D12:"))
            {
                throw std::runtime_error(
                    "The DirectX 12 debug layer reported an error: "
                    + entry.message);
            }
        }
    }

    void RequireD3D12OffscreenTarget()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);

        constexpr std::uint32_t targetWidth = 64u;
        constexpr std::uint32_t targetHeight = 32u;
        LamaPon::RenderTarget target;
        graphics.ResizeOffscreenTarget(
            target,
            targetWidth,
            targetHeight);
        Require(
            target.IsValid()
                && target.Width() == targetWidth
                && target.Height() == targetHeight
                && graphics.IsGraphicsViewCurrent(
                    target.CurrentColorViewHandle())
                && graphics.IsGraphicsViewCurrent(
                    target.DisplayViewHandle())
                && graphics.IsGraphicsViewCurrent(
                    target.DepthViewHandle()),
            "The DirectX 12 offscreen target did not publish its views");

        constexpr float backBufferClear[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(backBufferClear);
        auto primaryOutput = graphics.CaptureOutputState();

        constexpr float offscreenClear[4]{ 0.05f, 0.1f, 0.8f, 1.0f };
        graphics.BeginOffscreenTarget(target, offscreenClear);
        auto offscreenOutput = graphics.CaptureOutputState();
        graphics.BindOffscreenTargetDepthOnly(target);
        graphics.CaptureOffscreenTargetDepth(target);
        graphics.RestoreOutputState(*offscreenOutput);
        {
            auto pass = graphics.BeginSpritePass();
            DrawRectangle(
                pass,
                10.0f,
                8.0f,
                20.0f,
                12.0f,
                { 0.9f, 0.05f, 0.02f, 1.0f });
        }
        DirectX::XMFLOAT4X4 historyViewProjection{};
        DirectX::XMStoreFloat4x4(
            &historyViewProjection,
            DirectX::XMMatrixIdentity());
        graphics.CaptureOffscreenTargetColorHistory(
            target,
            historyViewProjection);
        graphics.CaptureOffscreenTargetTemporalHistory(
            target,
            historyViewProjection);
        Require(
            target.ColorHistoryViewHandle()
                && target.TemporalHistoryViewHandle()
                && graphics.IsGraphicsViewCurrent(
                    target.ColorHistoryViewHandle())
                && graphics.IsGraphicsViewCurrent(
                    target.TemporalHistoryViewHandle())
                && target.ColorHistoryViewProjection()._11 == 1.0f,
            "The DirectX 12 offscreen color histories were not published");
        graphics.RestoreOutputState(*primaryOutput);
        graphics.RestoreOutputState(*offscreenOutput);
        graphics.PublishOffscreenTarget(target);
        graphics.RestoreOutputState(*primaryOutput);
        {
            auto pass = graphics.BeginSpritePass();
            LamaPon::SpriteDrawRequest request;
            request.texture = target.DisplayViewHandle();
            request.position = { 20.0f, 15.0f };
            request.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(request),
                "The DirectX 12 offscreen display view was rejected");

            LamaPon::SpriteDrawRequest depthRequest;
            depthRequest.texture = target.DepthViewHandle();
            depthRequest.position = { 100.0f, 15.0f };
            depthRequest.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(depthRequest),
                "The DirectX 12 offscreen depth view was rejected");

            LamaPon::SpriteDrawRequest colorHistoryRequest;
            colorHistoryRequest.texture = target.ColorHistoryViewHandle();
            colorHistoryRequest.position = { 170.0f, 15.0f };
            colorHistoryRequest.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(colorHistoryRequest),
                "The DirectX 12 color history view was rejected");

            LamaPon::SpriteDrawRequest temporalHistoryRequest;
            temporalHistoryRequest.texture =
                target.TemporalHistoryViewHandle();
            temporalHistoryRequest.position = { 100.0f, 55.0f };
            temporalHistoryRequest.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(temporalHistoryRequest),
                "The DirectX 12 temporal history view was rejected");
        }
        std::uint32_t width{};
        std::uint32_t height{};
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 offscreen composition capture has unexpected "
            "dimensions");

        const auto pixel = [&pixels, width](
            const std::uint32_t x,
            const std::uint32_t y)
        {
            const auto offset = (
                static_cast<std::size_t>(y) * width + x) * 4u;
            return std::array<std::uint8_t, 4>{
                pixels[offset],
                pixels[offset + 1u],
                pixels[offset + 2u],
                pixels[offset + 3u] };
        };
        const auto blue = pixel(24u, 18u);
        const auto red = pixel(35u, 28u);
        const auto outside = pixel(90u, 70u);
        const auto depth = pixel(105u, 20u);
        const auto colorHistory = pixel(174u, 18u);
        const auto temporalHistory = pixel(115u, 68u);
        Require(
            blue[2] > 150u && blue[0] < 50u,
            "The DirectX 12 offscreen clear color was not sampled");
        Require(
            red[0] > 170u && red[2] < 80u,
            "The DirectX 12 offscreen sprite was not sampled");
        Require(
            outside[0] < 8u && outside[1] < 8u && outside[2] < 8u,
            "The DirectX 12 offscreen image escaped its destination bounds");
        Require(
            depth[0] > 230u && depth[1] < 12u && depth[2] < 12u,
            "The DirectX 12 offscreen depth copy was not sampled");
        Require(
            colorHistory[2] > 150u && colorHistory[0] < 50u,
            "The DirectX 12 color history copy was not sampled");
        Require(
            temporalHistory[0] > 170u && temporalHistory[2] < 80u,
            "The DirectX 12 temporal history copy was not sampled");

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("RenderTextureCamera");
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        camera.SetTargetTexture("d3d12-camera-target");
        camera.SetTargetTextureSize(40u, 20u);
        camera.SetTargetClearColor({ 0.05f, 0.8f, 0.1f, 1.0f });

        graphics.BeginFrame(backBufferClear);
        scene.RenderTargetTextures();
        const auto cameraView = graphics.RenderTextureViewHandle(
            "d3d12-camera-target");
        Require(
            cameraView
                && graphics.IsGraphicsViewCurrent(cameraView),
            "The DirectX 12 camera did not publish its render texture");
        {
            auto pass = graphics.BeginSpritePass();
            LamaPon::SpriteDrawRequest request;
            request.texture = cameraView;
            request.position = { 5.0f, 5.0f };
            request.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(request),
                "The DirectX 12 camera render texture was rejected");
        }
        std::uint32_t cameraWidth{};
        std::uint32_t cameraHeight{};
        const auto cameraPixels = graphics.CaptureBackBuffer(
            cameraWidth,
            cameraHeight);
        graphics.EndFrame();
        const auto cameraOffset = (
            static_cast<std::size_t>(10u) * cameraWidth + 10u) * 4u;
        Require(
            cameraWidth == CanvasWidth
                && cameraHeight == CanvasHeight
                && cameraPixels[cameraOffset + 1u] > 150u
                && cameraPixels[cameraOffset] < 50u
                && cameraPixels[cameraOffset + 2u] < 60u,
            "The DirectX 12 camera render texture was not restored and "
            "sampled");

        constexpr float compositionClear[4]{
            2.0f, 0.25f, 0.0f, 1.0f };
        graphics.BeginFrame(backBufferClear);
        graphics.BeginSceneComposition(compositionClear);
        scene.RenderMainCamera(
            graphics.AspectRatio(),
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        std::uint32_t compositionWidth{};
        std::uint32_t compositionHeight{};
        const auto compositionPixels = graphics.CaptureBackBuffer(
            compositionWidth,
            compositionHeight);
        graphics.EndFrame();
        const auto compositionOffset = (
            static_cast<std::size_t>(64u) * compositionWidth + 128u)
            * 4u;
        // 既定のカラーグレーディング（露出0.15、コントラスト1.05、彩度1.08、
        // 色温度0.02）とACESをD3D11のPSToneMapと同じ式で掛けると、
        // (2.0, 0.25, 0.0)はgamma変換なしで約(250, 101, 0)になります。
        Require(
            compositionWidth == CanvasWidth
                && compositionHeight == CanvasHeight
                && compositionPixels[compositionOffset] > 240u
                && compositionPixels[compositionOffset + 1u] > 90u
                && compositionPixels[compositionOffset + 1u] < 115u
                && compositionPixels[compositionOffset + 2u] < 8u,
            "The DirectX 12 HDR scene was not tone-mapped to the back "
            "buffer");

        auto lowExposureFrame = scene.PostProcessFrameData();
        lowExposureFrame.colorGrading.exposure = -2.0f;
        lowExposureFrame.colorGrading.contrast = 1.0f;
        lowExposureFrame.colorGrading.saturation = 1.0f;
        lowExposureFrame.colorGrading.temperature = 0.0f;
        lowExposureFrame.colorGrading.tint = 0.0f;
        lowExposureFrame.colorGrading.vignette = 0.0f;
        graphics.BeginFrame(backBufferClear);
        graphics.BeginSceneComposition(compositionClear);
        scene.RenderMainCamera(
            graphics.AspectRatio(),
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(lowExposureFrame);
        const auto lowExposurePixels = graphics.CaptureBackBuffer(
            compositionWidth,
            compositionHeight);
        graphics.EndFrame();
        Require(
            lowExposurePixels[compositionOffset] + 35u
                    < compositionPixels[compositionOffset]
                && lowExposurePixels[compositionOffset + 1u] + 35u
                    < compositionPixels[compositionOffset + 1u],
            "The DirectX 12 compositor ignored the color grading "
            "exposure");

        auto untonemappedFrame = lowExposureFrame;
        untonemappedFrame.colorGrading.toneMappingEnabled = false;
        graphics.BeginFrame(backBufferClear);
        graphics.BeginSceneComposition(compositionClear);
        scene.RenderMainCamera(
            graphics.AspectRatio(),
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(untonemappedFrame);
        const auto untonemappedPixels = graphics.CaptureBackBuffer(
            compositionWidth,
            compositionHeight);
        graphics.EndFrame();
        Require(
            untonemappedPixels[compositionOffset] > 250u
                && untonemappedPixels[compositionOffset + 1u] > 55u
                && untonemappedPixels[compositionOffset + 1u] < 75u,
            "The DirectX 12 compositor ignored the disabled tone "
            "mapping setting");

        // Bloomはトーンマップの前にHDRのまま掛かります。均一な(2.0, 0.25, 0.0)
        // でも高輝度分が足され、トーンマップ後の緑が約101から約126へ上がります。
        auto bloomFrame = scene.PostProcessFrameData();
        bloomFrame.bloom.enabled = true;
        graphics.BeginFrame(backBufferClear);
        graphics.BeginSceneComposition(compositionClear);
        scene.RenderMainCamera(
            graphics.AspectRatio(),
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(bloomFrame);
        const auto bloomPixels = graphics.CaptureBackBuffer(
            compositionWidth,
            compositionHeight);
        graphics.EndFrame();
        Require(
            bloomPixels[compositionOffset + 1u]
                > compositionPixels[compositionOffset + 1u] + 15u,
            "The DirectX 12 scene composition did not apply bloom");
    }

    void RequireD3D12PrimitiveScene()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        auto shadowSettings = graphics.Settings();
        shadowSettings.shadowResolution = 256u;
        shadowSettings.shadowCascadeLimit = 2u;
        graphics.SetGraphicsSettings(shadowSettings);
        Require(
            graphics.Shadows().IsValid()
                && graphics.Shadows().Resolution() == 256u
                && graphics.Shadows().CascadeCount() == 2u
                && graphics.IsGraphicsViewCurrent(
                    graphics.Shadows().ViewHandle()),
            "The DirectX 12 directional shadow map was not initialized");
        Require(
            graphics.SpotShadows().IsValid()
                && graphics.SpotShadows().Resolution() == 256u
                && graphics.SpotShadows().CascadeCount() == 4u
                && graphics.IsGraphicsViewCurrent(
                    graphics.SpotShadows().ViewHandle()),
            "The DirectX 12 spot shadow map was not initialized");
        Require(
            graphics.PointShadows().IsValid()
                && graphics.PointShadows().Resolution() == 256u
                && graphics.PointShadows().CascadeCount() == 6u
                && graphics.IsGraphicsViewCurrent(
                    graphics.PointShadows().ViewHandle()),
            "The DirectX 12 point shadow cube was not initialized");
        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 0.2f, 0.25f, 0.35f });
        scene.SetAmbientLightIntensity(0.2f);
        auto& lightObject = scene.CreateGameObject("Sun");
        lightObject.AddComponent<LamaPon::DirectionalLightComponent>();

        const std::array shapes{
            LamaPon::PrimitiveShape::Cube,
            LamaPon::PrimitiveShape::Sphere,
            LamaPon::PrimitiveShape::Cylinder,
            LamaPon::PrimitiveShape::Plane };
        for (std::size_t index{}; index < shapes.size(); ++index)
        {
            auto& object = scene.CreateGameObject("Primitive");
            object.GetTransform().position = {
                -1.35f + static_cast<float>(index) * 0.9f,
                0.0f,
                0.0f };
            object.GetTransform().scale = { 0.7f, 0.7f, 0.7f };
            if (shapes[index] == LamaPon::PrimitiveShape::Plane)
            {
                object.GetTransform().SetEulerAngles(
                    DirectX::XM_PIDIV2, 0.0f, 0.0f);
            }
            object.AddComponent<LamaPon::MeshRendererComponent>(
                shapes[index],
                DirectX::XMFLOAT4{ 0.95f, 0.2f, 0.12f, 1.0f });
        }

        auto& proceduralObject = scene.CreateGameObject("Procedural");
        proceduralObject.GetTransform().position = { 0.0f, 0.85f, 0.0f };
        auto& procedural = proceduralObject.AddComponent<
            LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.1f, 0.85f, 0.25f, 1.0f });
        procedural.SetProceduralMesh(
            {
                { { -0.4f, -0.3f, 0.0f }, { 0, 0, 1 }, { 0, 1 } },
                { { 0.0f, 0.4f, 0.0f }, { 0, 0, 1 }, { 0.5f, 0 } },
                { { 0.4f, -0.3f, 0.0f }, { 0, 0, 1 }, { 1, 1 } }
            },
            { 0, 1, 2 });

        constexpr float clearColor[4]{ 0.02f, 0.03f, 0.05f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        std::uint32_t width{};
        std::uint32_t height{};
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            graphics.DepthPass() == LamaPon::DepthPassKind::None
                && graphics.Lighting().directionalShadow.enabled
                && graphics.IsGraphicsViewCurrent(
                    graphics.Lighting().directionalShadow.texture),
            "The DirectX 12 scene did not complete its shadow depth pass");
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 primitive capture has unexpected dimensions");
        std::size_t coloredPixels{};
        for (std::size_t offset{}; offset + 3u < pixels.size(); offset += 4u)
        {
            if (pixels[offset] > 45u || pixels[offset + 1u] > 45u)
            {
                ++coloredPixels;
            }
        }
        Require(
            coloredPixels > 300u,
            "The DirectX 12 scene did not render its primitive meshes");

        // ゲーム実行と同じScene Composition経路でも3Dがtargetへ入り、
        // その深度を使うScreen Outlineが実際の画素へ反映されることを
        // 確認します。以前はRenderMainCameraがD3D12時だけtargetを捨て、
        // 直接描いた3Dが合成時に消えていました。
        const auto captureComposition = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            std::uint32_t captureWidth{};
            std::uint32_t captureHeight{};
            auto capture = graphics.CaptureBackBuffer(
                captureWidth,
                captureHeight);
            graphics.EndFrame();
            Require(
                captureWidth == CanvasWidth
                    && captureHeight == CanvasHeight,
                "The DirectX 12 scene composition capture has unexpected "
                "dimensions");
            return capture;
        };

        const auto compositionPixels = captureComposition();
        std::size_t composedGeometryPixels{};
        for (std::size_t offset{};
             offset + 3u < compositionPixels.size();
             offset += 4u)
        {
            if (compositionPixels[offset] > 55u
                || compositionPixels[offset + 1u] > 55u)
            {
                ++composedGeometryPixels;
            }
        }
        Require(
            composedGeometryPixels > 300u,
            "The DirectX 12 main camera did not render its primitive "
            "meshes into the scene composition target");

        auto outline = scene.ScreenOutline();
        outline.enabled = true;
        outline.color = { 0.0f, 1.0f, 0.0f };
        outline.intensity = 1.0f;
        outline.thickness = 2.0f;
        outline.depthThreshold = 0.01f;
        outline.normalThreshold = 0.1f;
        scene.SetScreenOutlineSettings(outline);
        const auto outlinedPixels = captureComposition();
        std::size_t greenOutlinePixels{};
        for (std::size_t offset{};
             offset + 3u < outlinedPixels.size();
             offset += 4u)
        {
            const auto green = outlinedPixels[offset + 1u];
            if (green > compositionPixels[offset + 1u] + 40u
                && green > outlinedPixels[offset] + 30u
                && green > outlinedPixels[offset + 2u] + 30u)
            {
                ++greenOutlinePixels;
            }
        }
        Require(
            greenOutlinePixels > 20u,
            "The DirectX 12 screen outline did not mark scene depth "
            "edges");

        outline.enabled = false;
        scene.SetScreenOutlineSettings(outline);
        auto depthOfField = scene.DepthOfField();
        depthOfField.enabled = true;
        depthOfField.focusDistance = 20.0f;
        depthOfField.focusRange = 0.0f;
        depthOfField.blurStrength = 8.0f;
        depthOfField.maximumRadius = 8.0f;
        scene.SetDepthOfFieldSettings(depthOfField);
        auto postProcessSettings = graphics.Settings();
        postProcessSettings.depthOfFieldEnabled = true;
        postProcessSettings.depthOfFieldSampleCount = 16u;
        graphics.SetGraphicsSettings(postProcessSettings);
        const auto depthOfFieldPixels = captureComposition();
        std::size_t depthOfFieldChangedPixels{};
        std::uint64_t depthOfFieldDifference{};
        for (std::size_t offset{};
             offset + 3u < depthOfFieldPixels.size();
             offset += 4u)
        {
            int difference{};
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                difference += std::abs(
                    static_cast<int>(depthOfFieldPixels[offset + channel])
                    - static_cast<int>(compositionPixels[offset + channel]));
            }
            if (difference > 12)
            {
                ++depthOfFieldChangedPixels;
                depthOfFieldDifference += static_cast<std::uint64_t>(
                    difference);
            }
        }
        Require(
            depthOfFieldChangedPixels > 100u
                && depthOfFieldDifference > 3000u,
            "The DirectX 12 depth of field did not blur out-of-focus "
            "scene geometry");
    }

    void RequireD3D12DirectionalShadows()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        auto settings = graphics.Settings();
        settings.shadowResolution = 256u;
        settings.shadowCascadeLimit = 1u;
        graphics.SetGraphicsSettings(settings);

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.08f);

        auto& wall = scene.CreateGameObject("ShadowReceiver");
        wall.GetTransform().scale = { 4.0f, 2.2f, 0.1f };
        wall.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });

        auto& blocker = scene.CreateGameObject("ShadowCaster");
        blocker.GetTransform().position = { 0.2f, 0.0f, 1.15f };
        blocker.GetTransform().scale = { 0.7f, 0.7f, 0.7f };
        blocker.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.55f, 0.55f, 0.55f, 1.0f });

        auto& lightObject = scene.CreateGameObject("ShadowSun");
        lightObject.GetTransform().SetEulerAngles(0.0f, 0.65f, 0.0f);
        auto& light = lightObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        light.SetShadowCascadeCount(1u);
        light.SetShadowBias(0.0005f);
        light.SetShadowNormalBias(0.001f);
        light.SetShadowStrength(1.0f);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto capture = [&]()
        {
            graphics.BeginFrame(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                nullptr);
            std::uint32_t width{};
            std::uint32_t height{};
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 shadow capture has unexpected dimensions");
            return pixels;
        };

        const auto shadowed = capture();

        // Scene Compositionのpost-process列から、同じDirectional Shadowを
        // 空気中の散乱へ使えることを確認します。無効へ戻したときは
        // 余分なping-pongや履歴を残さず元の画像へ戻る必要があります。
        const auto captureComposition = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            std::uint32_t width{};
            std::uint32_t height{};
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 volumetric capture has unexpected "
                "dimensions");
            return pixels;
        };
        const auto withoutVolumetric = captureComposition();
        auto volumetric = scene.VolumetricLight();
        volumetric.enabled = true;
        volumetric.intensity = 4.0f;
        volumetric.sampleCount = 12u;
        volumetric.scattering = 0.0f;
        scene.SetVolumetricLightSettings(volumetric);
        const auto withVolumetric = captureComposition();
        std::size_t volumetricPixels{};
        std::uint64_t volumetricBrightening{};
        std::uint64_t totalVolumetricDifference{};
        int maximumVolumetricDifference{};
        for (std::size_t offset{};
            offset + 3u < withVolumetric.size();
            offset += 4u)
        {
            const int difference =
                static_cast<int>(withVolumetric[offset])
                    - static_cast<int>(withoutVolumetric[offset])
                + static_cast<int>(withVolumetric[offset + 1u])
                    - static_cast<int>(withoutVolumetric[offset + 1u])
                + static_cast<int>(withVolumetric[offset + 2u])
                    - static_cast<int>(withoutVolumetric[offset + 2u]);
            maximumVolumetricDifference = std::max(
                maximumVolumetricDifference,
                difference);
            totalVolumetricDifference += static_cast<std::uint64_t>(
                std::max(difference, 0));
            if (difference > 0)
            {
                ++volumetricPixels;
                volumetricBrightening += static_cast<std::uint64_t>(
                    difference);
            }
        }
        Require(
            volumetricPixels > 800u
                && volumetricBrightening > 2000u
                && maximumVolumetricDifference >= 2,
            "The DirectX 12 volumetric light did not brighten the camera "
            "rays ("
                + std::to_string(volumetricPixels)
                + " pixels, total "
                + std::to_string(totalVolumetricDifference)
                + ", maximum "
                + std::to_string(maximumVolumetricDifference)
                + ")");
        volumetric.enabled = false;
        scene.SetVolumetricLightSettings(volumetric);
        Require(
            captureComposition() == withoutVolumetric,
            "Disabling DirectX 12 volumetric light did not restore the "
            "original frame");

        light.SetCastsShadows(false);
        const auto unshadowed = capture();
        std::size_t darkerPixels{};
        std::uint64_t totalDarkening{};
        for (std::size_t offset{};
            offset + 3u < shadowed.size();
            offset += 4u)
        {
            const auto shadowedBrightness = std::max({
                shadowed[offset],
                shadowed[offset + 1u],
                shadowed[offset + 2u] });
            const auto unshadowedBrightness = std::max({
                unshadowed[offset],
                unshadowed[offset + 1u],
                unshadowed[offset + 2u] });
            if (unshadowedBrightness
                > static_cast<unsigned int>(shadowedBrightness) + 12u)
            {
                ++darkerPixels;
                totalDarkening += static_cast<std::uint64_t>(
                    unshadowedBrightness - shadowedBrightness);
            }
        }
        Require(
            darkerPixels > 80u && totalDarkening > 4000u,
            "The DirectX 12 directional shadow was not visible on the "
            "receiver");

        settings.shadowsEnabled = false;
        graphics.SetGraphicsSettings(settings);
        const auto globallyDisabled = capture();
        Require(
            globallyDisabled == unshadowed
                && !graphics.Shadows().IsValid(),
            "Disabling DirectX 12 shadows did not preserve the unshadowed "
            "rendering path");
    }

    void RequireD3D12SpotShadows()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        auto settings = graphics.Settings();
        settings.shadowResolution = 256u;
        graphics.SetGraphicsSettings(settings);

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.03f);

        auto& wall = scene.CreateGameObject("SpotShadowReceiver");
        wall.GetTransform().scale = { 4.0f, 2.2f, 0.1f };
        wall.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.85f, 0.85f, 0.85f, 1.0f });

        auto& blocker = scene.CreateGameObject("SpotShadowCaster");
        blocker.GetTransform().position = { 0.4f, 0.0f, 1.35f };
        blocker.GetTransform().scale = { 0.65f, 0.65f, 0.65f };
        blocker.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.5f, 0.5f, 0.5f, 1.0f });

        auto& lightObject = scene.CreateGameObject("ShadowSpot");
        lightObject.GetTransform().position = { 1.6f, 0.0f, 3.0f };
        lightObject.GetTransform().SetEulerAngles(0.0f, 0.49f, 0.0f);
        auto& light = lightObject.AddComponent<
            LamaPon::SpotLightComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                6.0f,
                8.0f,
                DirectX::XMConvertToRadians(24.0f),
                DirectX::XMConvertToRadians(32.0f));
        light.SetCastsShadows(true);
        light.SetShadowBias(0.0005f);
        light.SetShadowNormalBias(0.001f);
        light.SetShadowStrength(1.0f);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto capture = [&]()
        {
            graphics.BeginFrame(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                nullptr);
            std::uint32_t width{};
            std::uint32_t height{};
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 spot shadow capture has unexpected "
                "dimensions");
            return pixels;
        };

        const auto shadowed = capture();
        Require(
            graphics.Lighting().spotShadows[0].enabled
                && graphics.IsGraphicsViewCurrent(
                    graphics.Lighting().spotShadowTexture),
            "The DirectX 12 scene did not publish its spot shadow");
        light.SetCastsShadows(false);
        const auto unshadowed = capture();
        std::size_t darkerPixels{};
        std::uint64_t totalDarkening{};
        for (std::size_t offset{};
            offset + 3u < shadowed.size();
            offset += 4u)
        {
            const auto shadowedBrightness = std::max({
                shadowed[offset],
                shadowed[offset + 1u],
                shadowed[offset + 2u] });
            const auto unshadowedBrightness = std::max({
                unshadowed[offset],
                unshadowed[offset + 1u],
                unshadowed[offset + 2u] });
            if (unshadowedBrightness
                > static_cast<unsigned int>(shadowedBrightness) + 12u)
            {
                ++darkerPixels;
                totalDarkening += static_cast<std::uint64_t>(
                    unshadowedBrightness - shadowedBrightness);
            }
        }
        Require(
            darkerPixels > 60u && totalDarkening > 3000u,
            "The DirectX 12 spot shadow was not visible on the receiver");
    }

    void RequireD3D12PointShadows()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        auto settings = graphics.Settings();
        settings.shadowResolution = 256u;
        graphics.SetGraphicsSettings(settings);

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.03f);

        auto& wall = scene.CreateGameObject("PointShadowReceiver");
        wall.GetTransform().scale = { 4.0f, 2.2f, 0.1f };
        wall.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.85f, 0.85f, 0.85f, 1.0f });

        auto& blocker = scene.CreateGameObject("PointShadowCaster");
        blocker.GetTransform().position = { 0.7f, 0.0f, 1.5f };
        blocker.GetTransform().scale = { 0.65f, 0.65f, 0.65f };
        blocker.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.5f, 0.5f, 0.5f, 1.0f });

        auto& lightObject = scene.CreateGameObject("ShadowPoint");
        lightObject.GetTransform().position = { 1.4f, 0.0f, 3.0f };
        auto& light = lightObject.AddComponent<
            LamaPon::PointLightComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                15.0f,
                8.0f);
        light.SetCastsShadows(true);
        light.SetShadowBias(0.0005f);
        light.SetShadowStrength(1.0f);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto capture = [&]()
        {
            graphics.BeginFrame(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                nullptr);
            std::uint32_t width{};
            std::uint32_t height{};
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 point shadow capture has unexpected "
                "dimensions");
            return pixels;
        };

        const auto shadowed = capture();
        Require(
            graphics.Lighting().pointShadow.enabled
                && graphics.IsGraphicsViewCurrent(
                    graphics.Lighting().pointShadow.texture),
            "The DirectX 12 scene did not publish its point shadow");
        light.SetCastsShadows(false);
        const auto unshadowed = capture();
        std::size_t darkerPixels{};
        std::uint64_t totalDarkening{};
        for (std::size_t offset{};
            offset + 3u < shadowed.size();
            offset += 4u)
        {
            const auto shadowedBrightness = std::max({
                shadowed[offset],
                shadowed[offset + 1u],
                shadowed[offset + 2u] });
            const auto unshadowedBrightness = std::max({
                unshadowed[offset],
                unshadowed[offset + 1u],
                unshadowed[offset + 2u] });
            if (unshadowedBrightness
                > static_cast<unsigned int>(shadowedBrightness) + 12u)
            {
                ++darkerPixels;
                totalDarkening += static_cast<std::uint64_t>(
                    unshadowedBrightness - shadowedBrightness);
            }
        }
        Require(
            darkerPixels > 60u && totalDarkening > 3000u,
            "The DirectX 12 point shadow was not visible on the receiver");
    }

    // Point LightとSpot Lightが、D3D11の従来経路と同じ距離・コーン減衰で
    // 壁を照らすことを、各光源の当たる点と届かない点の画素で確かめます。
    void RequireD3D12PointAndSpotLights()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        // 環境光もDirectional Lightも置かず、局所光源だけの明るさを見ます。
        scene.SetAmbientLightIntensity(0.0f);

        // 正面（+Z面）がz=0.05にある薄い灰色の壁です。
        auto& wall = scene.CreateGameObject("Wall");
        wall.GetTransform().scale = { 3.0f, 1.6f, 0.1f };
        wall.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });

        auto& pointObject = scene.CreateGameObject("PointLight");
        pointObject.GetTransform().position = { -0.8f, 0.0f, 0.6f };
        pointObject.AddComponent<LamaPon::PointLightComponent>(
            DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
            3.0f,
            2.0f);

        // 回転の無いSpot Lightは-Zを向くため、壁の正面へ当たります。
        auto& spotObject = scene.CreateGameObject("SpotLight");
        spotObject.GetTransform().position = { 0.9f, 0.0f, 1.2f };
        spotObject.AddComponent<LamaPon::SpotLightComponent>(
            DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
            3.0f,
            3.0f,
            DirectX::XMConvertToRadians(8.0f),
            DirectX::XMConvertToRadians(12.0f));

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const float aspectRatio =
            static_cast<float>(CanvasWidth) / CanvasHeight;
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(aspectRatio, false, nullptr);
        std::uint32_t width{};
        std::uint32_t height{};
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 local light capture has unexpected dimensions");

        const auto viewProjection = DirectX::XMMatrixMultiply(
            camera.ViewMatrix(),
            camera.ProjectionMatrix(aspectRatio));
        const auto brightnessAt = [&](const DirectX::XMFLOAT3& world)
        {
            const auto clip = DirectX::XMVector3TransformCoord(
                DirectX::XMLoadFloat3(&world),
                viewProjection);
            const int x = std::clamp(
                static_cast<int>(
                    (DirectX::XMVectorGetX(clip) * 0.5f + 0.5f)
                    * static_cast<float>(CanvasWidth)),
                0,
                static_cast<int>(CanvasWidth) - 1);
            const int y = std::clamp(
                static_cast<int>(
                    (0.5f - DirectX::XMVectorGetY(clip) * 0.5f)
                    * static_cast<float>(CanvasHeight)),
                0,
                static_cast<int>(CanvasHeight) - 1);
            const auto offset =
                (static_cast<std::size_t>(y) * CanvasWidth
                    + static_cast<std::size_t>(x)) * 4u;
            return std::max({
                pixels[offset],
                pixels[offset + 1u],
                pixels[offset + 2u] });
        };

        // D3D11のLamaPonLit.hlslと同じGGXの拡散（albedo/π）なので、灰色の
        // 壁の正面は点光源で約110、スポットで約80の明るさになります。
        Require(
            brightnessAt({ -0.8f, 0.0f, 0.05f }) > 90u,
            "The DirectX 12 point light did not light the wall");
        Require(
            brightnessAt({ 0.9f, 0.0f, 0.05f }) > 60u,
            "The DirectX 12 spot light did not light the wall");
        Require(
            brightnessAt({ 0.9f, 0.6f, 0.05f }) < 20u,
            "The DirectX 12 spot light leaked outside its cone");
        Require(
            brightnessAt({ 1.4f, -0.7f, 0.05f }) < 20u,
            "The DirectX 12 local lights leaked beyond their range");
    }

    void RequireD3D12MaterialFactors()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightIntensity(0.0f);

        auto& object = scene.CreateGameObject("EmissiveMesh");
        auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 1.0f });
        mesh.SetRoughness(0.2f);
        mesh.SetMetallic(0.8f);
        mesh.SetEmissiveColor({ 0.05f, 0.8f, 0.15f });

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        std::uint32_t width{};
        std::uint32_t height{};
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 material capture has unexpected dimensions");
        const auto center =
            (static_cast<std::size_t>(CanvasHeight / 2u) * CanvasWidth
                + CanvasWidth / 2u) * 4u;
        Require(
            pixels[center + 1u] > 170u
                && pixels[center] < 40u
                && pixels[center + 2u] < 70u,
            "The DirectX 12 pipeline did not apply the material emissive factor");
    }

    void RequireD3D12AnimatedGltfModel()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 10.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.9f);

        const auto modelPath = std::filesystem::path(
            LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "TexturedRiggedSimple.gltf";
        // This fixture references an existing JPEG outside the model file.
        // D3D12 must retain it as a backend handle without creating a D3D11 SRV.
        const auto texturedAsset = graphics.Assets().LoadModel(modelPath);
        Require(
            texturedAsset != nullptr
                && texturedAsset->skeletalModel != nullptr
                && !texturedAsset->skeletalModel->primitives.empty()
                && texturedAsset->skeletalModel->primitives.front()
                    .embeddedTextures.albedo
                && texturedAsset->skeletalModel->primitives.front().texture
                    == nullptr,
            "The DirectX 12 glTF texture was not retained as a backend handle");
        auto& object = scene.CreateGameObject("AnimatedModel");
        auto& model = object.AddComponent<
            LamaPon::ModelRendererComponent>(modelPath);
        model.SetAnimationPlayOnStart(false);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        std::uint32_t width{};
        std::uint32_t height{};
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            model.AnimationCount() > 0,
            "The DirectX 12 ModelRenderer did not load the glTF animation");
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 model capture has unexpected dimensions");
        std::size_t modelPixels{};
        for (std::size_t offset{}; offset + 3u < pixels.size(); offset += 4u)
        {
            if (pixels[offset] > 20u
                || pixels[offset + 1u] > 20u
                || pixels[offset + 2u] > 20u)
            {
                ++modelPixels;
            }
        }
        Require(
            modelPixels > 100u,
            "The DirectX 12 ModelRenderer did not draw the glTF mesh");

        model.SetAnimationTime(model.AnimationDuration() * 0.5f);
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        std::uint32_t animatedWidth{};
        std::uint32_t animatedHeight{};
        const auto animatedPixels = graphics.CaptureBackBuffer(
            animatedWidth,
            animatedHeight);
        graphics.EndFrame();
        Require(
            animatedWidth == width && animatedHeight == height,
            "The animated DirectX 12 model capture changed dimensions");
        std::size_t changedPixels{};
        for (std::size_t offset{};
            offset + 3u < pixels.size();
            offset += 4u)
        {
            if (pixels[offset] != animatedPixels[offset]
                || pixels[offset + 1u] != animatedPixels[offset + 1u]
                || pixels[offset + 2u] != animatedPixels[offset + 2u])
            {
                ++changedPixels;
            }
        }
        Require(
            changedPixels > 10u,
            "The DirectX 12 ModelRenderer did not apply its animated pose");
    }

    void RequireD3D12AnimatedFbxModel()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto modelPath = std::filesystem::path(
            LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "AnimatedSausage.fbx";
        const auto asset = graphics.Assets().LoadModel(modelPath);
        Require(
            asset != nullptr
                && asset->skeletalModel != nullptr
                && asset->skeletalModel->hasLocalBounds
                && !asset->skeletalModel->animations.empty()
                && !asset->skeletalModel->primitives.empty(),
            "The DirectX 12 FBX import did not retain its CPU model");
        for (const auto& primitive : asset->skeletalModel->primitives)
        {
            Require(
                !primitive.cpuVertexData.empty()
                    && !primitive.cpuIndices.empty()
                    && !primitive.vertexBuffer
                    && !primitive.indexBuffer
                    && !primitive.effect,
                "The DirectX 12 FBX import created D3D11 resources");
        }

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.9f);

        const auto& bounds = asset->skeletalModel->localBounds;
        const DirectX::XMFLOAT3 center{
            (bounds.minimum.x + bounds.maximum.x) * 0.5f,
            (bounds.minimum.y + bounds.maximum.y) * 0.5f,
            (bounds.minimum.z + bounds.maximum.z) * 0.5f };
        const float maximumExtent = std::max({
            bounds.maximum.x - bounds.minimum.x,
            bounds.maximum.y - bounds.minimum.y,
            bounds.maximum.z - bounds.minimum.z,
            0.001f });
        const float scale = 3.0f / maximumExtent;
        auto& object = scene.CreateGameObject("AnimatedFbxModel");
        object.GetTransform().scale = { scale, scale, scale };
        object.GetTransform().position = {
            -center.x * scale,
            -center.y * scale,
            -center.z * scale };
        auto& model = object.AddComponent<
            LamaPon::ModelRendererComponent>(modelPath);
        model.SetAnimationPlayOnStart(false);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto capture = [&]()
        {
            graphics.BeginFrame(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                nullptr);
            std::uint32_t width{};
            std::uint32_t height{};
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 FBX capture has unexpected dimensions");
            return pixels;
        };
        const auto pixels = capture();
        Require(
            model.AnimationCount() > 0,
            "The DirectX 12 ModelRenderer did not load the FBX animation");
        std::size_t modelPixels{};
        for (std::size_t offset{};
            offset + 3u < pixels.size();
            offset += 4u)
        {
            if (pixels[offset] > 20u
                || pixels[offset + 1u] > 20u
                || pixels[offset + 2u] > 20u)
            {
                ++modelPixels;
            }
        }
        Require(
            modelPixels > 100,
            "The DirectX 12 ModelRenderer did not draw the FBX mesh");
    }

    void RequireD3D12CmoModel()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto modelPath = std::filesystem::path(
            LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "arrow.cmo";
        const auto asset = graphics.Assets().LoadModel(modelPath);
        Require(
            asset != nullptr
                && asset->model == nullptr
                && asset->skeletalModel != nullptr
                && asset->skeletalModel->hasLocalBounds
                && !asset->skeletalModel->primitives.empty(),
            "The DirectX 12 CMO import did not retain its CPU model");
        for (const auto& primitive : asset->skeletalModel->primitives)
        {
            Require(
                !primitive.cpuVertexData.empty()
                    && !primitive.cpuIndices.empty()
                    && !primitive.vertexBuffer
                    && !primitive.indexBuffer
                    && !primitive.effect,
                "The DirectX 12 CMO import created D3D11 resources");
        }

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 5.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        const auto& bounds = asset->skeletalModel->localBounds;
        const DirectX::XMFLOAT3 center{
            (bounds.minimum.x + bounds.maximum.x) * 0.5f,
            (bounds.minimum.y + bounds.maximum.y) * 0.5f,
            (bounds.minimum.z + bounds.maximum.z) * 0.5f };
        const float maximumExtent = std::max({
            bounds.maximum.x - bounds.minimum.x,
            bounds.maximum.y - bounds.minimum.y,
            bounds.maximum.z - bounds.minimum.z,
            0.001f });
        const float scale = 3.0f / maximumExtent;
        auto& object = scene.CreateGameObject("CmoModel");
        auto& transform = object.GetTransform();
        transform.scale = { scale, scale, scale };
        transform.SetEulerAngles(0.45f, 0.65f, 0.0f);
        DirectX::XMFLOAT3 transformedCenter{};
        DirectX::XMStoreFloat3(
            &transformedCenter,
            DirectX::XMVector3TransformCoord(
                DirectX::XMLoadFloat3(&center),
                DirectX::XMMatrixScaling(scale, scale, scale)
                    * DirectX::XMMatrixRotationQuaternion(
                        transform.RotationVector())));
        transform.position = {
            -transformedCenter.x,
            -transformedCenter.y,
            -transformedCenter.z };
        static_cast<void>(object.AddComponent<
            LamaPon::ModelRendererComponent>(modelPath));

        constexpr float clearColor[4]{ 0.1f, 0.2f, 0.3f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        std::uint32_t width{};
        std::uint32_t height{};
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 CMO capture has unexpected dimensions");
        // arrow.cmoの内蔵albedoは真っ黒です。D3D11のDirectXTK Model経路と
        // 同じくコンポーネントの白へ内蔵textureが掛かり、背景と違う形が
        // 環境光の下でも黒く描かれることを確かめます。
        const std::array<int, 3> background{
            pixels[0],
            pixels[1],
            pixels[2] };
        std::size_t modelPixels{};
        std::size_t darkPixels{};
        for (std::size_t offset{};
            offset + 3u < pixels.size();
            offset += 4u)
        {
            bool differs{};
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                differs = differs
                    || std::abs(
                        static_cast<int>(pixels[offset + channel])
                        - background[channel]) > 8;
            }
            if (!differs)
            {
                continue;
            }
            ++modelPixels;
            if (pixels[offset] < 8u
                && pixels[offset + 1u] < 8u
                && pixels[offset + 2u] < 8u)
            {
                ++darkPixels;
            }
        }
        Require(
            modelPixels > 100u && darkPixels * 10u >= modelPixels * 9u,
            "The DirectX 12 ModelRenderer did not draw the CMO mesh with its "
            "embedded albedo ("
                + std::to_string(modelPixels) + " visible, "
                + std::to_string(darkPixels) + " dark pixels)");
    }

    // 同じCMOをD3D11ではDirectXTK Model、D3D12ではCPU幾何として描きます。
    // 内蔵の黒いalbedoへコンポーネントの発光色だけが乗る合成画像を比べ、
    // 形、内蔵textureの適用、コンポーネントmaterialの使用を確かめます。
    [[nodiscard]] Capture RenderCmoModelCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The CMO capture did not start the requested rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 5.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        auto& object = scene.CreateGameObject("CmoModel");
        object.GetTransform().position = { 0.0f, 0.2f, 0.0f };
        object.GetTransform().scale = { 1.6f, 1.6f, 1.6f };
        object.GetTransform().SetEulerAngles(0.45f, 0.65f, 0.0f);
        auto& renderer = object.AddComponent<
            LamaPon::ModelRendererComponent>(
                std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
                    / "models"
                    / "arrow.cmo");
        // Material上書き中はD3D11もDirectXTK EffectではなくLamaPon Litで
        // 描くため、D3D12と同じ合成規則で比べられます。
        renderer.SetMaterialOverrideEnabled(true);
        renderer.SetEmissiveColor({ 0.2f, 0.6f, 0.3f });

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        std::size_t emissivePixels{};
        for (std::size_t offset{};
            offset + 3u < capture.pixels.size();
            offset += 4u)
        {
            if (capture.pixels[offset + 1u]
                > capture.pixels[offset] + 30u)
            {
                ++emissivePixels;
            }
        }
        Require(
            capture.width == CanvasWidth
                && capture.height == CanvasHeight
                && emissivePixels > 100u,
            "The CMO capture did not draw the emissive model ("
                + std::to_string(emissivePixels) + " pixels)");
        return capture;
    }

    // 3Dを含む合成画像をD3D11とD3D12で画素ごとに比べます。
    void RequireMatchingFrameCaptures(
        const std::string& name,
        const Capture& d3d11,
        const Capture& d3d12)
    {
        const std::size_t expectedBytes =
            static_cast<std::size_t>(CanvasWidth) * CanvasHeight * 4u;
        for (const auto* const capture : { &d3d11, &d3d12 })
        {
            Require(
                capture->width == CanvasWidth
                    && capture->height == CanvasHeight
                    && capture->pixels.size() == expectedBytes,
                "The " + name + " captures have unexpected dimensions");
        }

        // WARP上の同じ演算でも、最終丸めの1段差だけは許容します。
        constexpr int ChannelTolerance = 2;
        std::size_t mismatchedPixels{};
        std::size_t firstMismatch =
            std::numeric_limits<std::size_t>::max();
        int largestDifference{};
        for (std::size_t pixel{}; pixel < expectedBytes / 4u; ++pixel)
        {
            int pixelDifference{};
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                const auto offset = pixel * 4u + channel;
                pixelDifference = std::max(
                    pixelDifference,
                    std::abs(
                        static_cast<int>(d3d11.pixels[offset])
                        - static_cast<int>(d3d12.pixels[offset])));
            }
            largestDifference = std::max(largestDifference, pixelDifference);
            if (pixelDifference > ChannelTolerance)
            {
                ++mismatchedPixels;
                firstMismatch = std::min(firstMismatch, pixel);
            }
        }
        if (mismatchedPixels == 0)
        {
            return;
        }
        const auto describe = [&](const Capture& capture)
        {
            const auto offset = firstMismatch * 4u;
            return std::to_string(capture.pixels[offset]) + ","
                + std::to_string(capture.pixels[offset + 1u]) + ","
                + std::to_string(capture.pixels[offset + 2u]);
        };
        throw std::runtime_error(
            "DirectX 12 " + name + " differed from DirectX 11 in "
            + std::to_string(mismatchedPixels)
            + " pixels (largest channel difference "
            + std::to_string(largestDifference)
            + "; first at "
            + std::to_string(firstMismatch % CanvasWidth)
            + ","
            + std::to_string(firstMismatch / CanvasWidth)
            + " D3D11="
            + describe(d3d11)
            + " D3D12="
            + describe(d3d12)
            + ")");
    }

#pragma pack(push, 4)
    // DirectXTKのSDKMesh.hと同じレイアウトです。Importerの定義を共有
    // せずに合成し、実ファイルのABIを独立に検証します。
    struct TestSdkmeshVertexElement final
    {
        std::uint16_t stream;
        std::uint16_t offset;
        std::uint8_t type;
        std::uint8_t method;
        std::uint8_t usage;
        std::uint8_t usageIndex;
    };
#pragma pack(pop)

#pragma pack(push, 8)
    struct TestSdkmeshHeader final
    {
        std::uint32_t version;
        std::uint8_t isBigEndian;
        std::uint64_t headerSize;
        std::uint64_t nonBufferDataSize;
        std::uint64_t bufferDataSize;
        std::uint32_t numVertexBuffers;
        std::uint32_t numIndexBuffers;
        std::uint32_t numMeshes;
        std::uint32_t numTotalSubsets;
        std::uint32_t numFrames;
        std::uint32_t numMaterials;
        std::uint64_t vertexStreamHeadersOffset;
        std::uint64_t indexStreamHeadersOffset;
        std::uint64_t meshDataOffset;
        std::uint64_t subsetDataOffset;
        std::uint64_t frameDataOffset;
        std::uint64_t materialDataOffset;
    };

    struct TestSdkmeshVertexBufferHeader final
    {
        std::uint64_t numVertices;
        std::uint64_t sizeBytes;
        std::uint64_t strideBytes;
        std::array<TestSdkmeshVertexElement, 32> declaration;
        std::uint64_t dataOffset;
    };

    struct TestSdkmeshIndexBufferHeader final
    {
        std::uint64_t numIndices;
        std::uint64_t sizeBytes;
        std::uint32_t indexType;
        std::uint64_t dataOffset;
    };

    struct TestSdkmeshMesh final
    {
        char name[100];
        std::uint8_t numVertexBuffers;
        std::uint32_t vertexBuffers[16];
        std::uint32_t indexBuffer;
        std::uint32_t numSubsets;
        std::uint32_t numFrameInfluences;
        DirectX::XMFLOAT3 boundingBoxCenter;
        DirectX::XMFLOAT3 boundingBoxExtents;
        std::uint64_t subsetOffset;
        std::uint64_t frameInfluenceOffset;
    };

    struct TestSdkmeshSubset final
    {
        char name[100];
        std::uint32_t materialId;
        std::uint32_t primitiveType;
        std::uint64_t indexStart;
        std::uint64_t indexCount;
        std::uint64_t vertexStart;
        std::uint64_t vertexCount;
    };

    struct TestSdkmeshFrame final
    {
        char name[100];
        std::uint32_t mesh;
        std::uint32_t parentFrame;
        std::uint32_t childFrame;
        std::uint32_t siblingFrame;
        DirectX::XMFLOAT4X4 matrix;
        std::uint32_t animationDataIndex;
    };

    struct TestSdkmeshMaterial final
    {
        char name[100];
        char materialInstancePath[260];
        char diffuseTexture[260];
        char normalTexture[260];
        char specularTexture[260];
        DirectX::XMFLOAT4 diffuse;
        DirectX::XMFLOAT4 ambient;
        DirectX::XMFLOAT4 specular;
        DirectX::XMFLOAT4 emissive;
        float power;
        std::uint64_t runtimeHandles[6];
    };
#pragma pack(pop)

    static_assert(sizeof(TestSdkmeshVertexElement) == 8u);
    static_assert(sizeof(TestSdkmeshHeader) == 104u);
    static_assert(sizeof(TestSdkmeshVertexBufferHeader) == 288u);
    static_assert(sizeof(TestSdkmeshIndexBufferHeader) == 32u);
    static_assert(sizeof(TestSdkmeshMesh) == 224u);
    static_assert(sizeof(TestSdkmeshSubset) == 144u);
    static_assert(sizeof(TestSdkmeshFrame) == 184u);
    static_assert(sizeof(TestSdkmeshMaterial) == 1256u);

    // 2つの頂点範囲を持つBoxを、VertexStartで範囲を指す2つのsubsetと
    // 赤／青の2つのMaterialで描くSDKMESH（version 101）へ組み立てます。
    // subset 0は+Z／-Z／+X面、subset 1は-X／+Y／-Y面です。
    [[nodiscard]] std::vector<std::uint8_t> BuildTestSdkmesh()
    {
        struct BoxVertex final
        {
            DirectX::XMFLOAT3 position;
            DirectX::XMFLOAT3 normal;
            DirectX::XMFLOAT2 textureCoordinate;
        };
        constexpr float h = 0.5f;
        std::vector<BoxVertex> vertices;
        std::vector<std::uint16_t> indices;
        const auto addFace = [&](
            const std::uint16_t rangeStart,
            const DirectX::XMFLOAT3& normal,
            const std::array<DirectX::XMFLOAT3, 4>& corners)
        {
            // cornersは外から見た左下、左上、右上、右下です。DirectXTKの
            // 既定（時計回りが表）で表になり、indexは範囲の先頭から数えます。
            const auto first = static_cast<std::uint16_t>(
                vertices.size() - rangeStart);
            const std::array<DirectX::XMFLOAT2, 4> textureCoordinates{ {
                { 0.0f, 1.0f },
                { 0.0f, 0.0f },
                { 1.0f, 0.0f },
                { 1.0f, 1.0f } } };
            for (std::size_t corner{}; corner < corners.size(); ++corner)
            {
                vertices.push_back({
                    corners[corner],
                    normal,
                    textureCoordinates[corner] });
            }
            constexpr std::array<std::uint16_t, 6> quad{
                0u, 1u, 2u, 0u, 2u, 3u };
            for (const auto offset : quad)
            {
                indices.push_back(
                    static_cast<std::uint16_t>(first + offset));
            }
        };
        addFace(0u, { 0.0f, 0.0f, 1.0f }, { {
            { -h, -h, h }, { -h, h, h }, { h, h, h }, { h, -h, h } } });
        addFace(0u, { 0.0f, 0.0f, -1.0f }, { {
            { h, -h, -h }, { h, h, -h }, { -h, h, -h }, { -h, -h, -h } } });
        addFace(0u, { 1.0f, 0.0f, 0.0f }, { {
            { h, -h, h }, { h, h, h }, { h, h, -h }, { h, -h, -h } } });
        addFace(12u, { -1.0f, 0.0f, 0.0f }, { {
            { -h, -h, -h }, { -h, h, -h }, { -h, h, h }, { -h, -h, h } } });
        addFace(12u, { 0.0f, 1.0f, 0.0f }, { {
            { -h, h, h }, { -h, h, -h }, { h, h, -h }, { h, h, h } } });
        addFace(12u, { 0.0f, -1.0f, 0.0f }, { {
            { -h, -h, -h }, { -h, -h, h }, { h, -h, h }, { h, -h, -h } } });

        const std::uint64_t vertexHeaderOffset = sizeof(TestSdkmeshHeader);
        const std::uint64_t indexHeaderOffset =
            vertexHeaderOffset + sizeof(TestSdkmeshVertexBufferHeader);
        const std::uint64_t meshOffset =
            indexHeaderOffset + sizeof(TestSdkmeshIndexBufferHeader);
        const std::uint64_t subsetOffset = meshOffset + sizeof(TestSdkmeshMesh);
        const std::uint64_t frameOffset =
            subsetOffset + 2u * sizeof(TestSdkmeshSubset);
        const std::uint64_t materialOffset =
            frameOffset + sizeof(TestSdkmeshFrame);
        const std::uint64_t subsetTableOffset =
            materialOffset + 2u * sizeof(TestSdkmeshMaterial);
        const std::uint64_t bufferOffset =
            subsetTableOffset + 2u * sizeof(std::uint32_t);
        const std::uint64_t vertexBytes = vertices.size() * sizeof(BoxVertex);
        const std::uint64_t indexBytes =
            indices.size() * sizeof(std::uint16_t);

        TestSdkmeshHeader header{};
        header.version = 101u;
        // DirectXTKは、header sizeにVB／IB headerまでを含めることを求めます。
        header.headerSize = meshOffset;
        header.nonBufferDataSize = bufferOffset - meshOffset;
        header.bufferDataSize = vertexBytes + indexBytes;
        header.numVertexBuffers = 1u;
        header.numIndexBuffers = 1u;
        header.numMeshes = 1u;
        header.numTotalSubsets = 2u;
        header.numFrames = 1u;
        header.numMaterials = 2u;
        header.vertexStreamHeadersOffset = vertexHeaderOffset;
        header.indexStreamHeadersOffset = indexHeaderOffset;
        header.meshDataOffset = meshOffset;
        header.subsetDataOffset = subsetOffset;
        header.frameDataOffset = frameOffset;
        header.materialDataOffset = materialOffset;

        TestSdkmeshVertexBufferHeader vertexBuffer{};
        vertexBuffer.numVertices = vertices.size();
        vertexBuffer.sizeBytes = vertexBytes;
        vertexBuffer.strideBytes = sizeof(BoxVertex);
        // D3DDECL_ENDで埋めてから、position／normal／UVを並べます。
        vertexBuffer.declaration.fill({ 0xffu, 0u, 17u, 0u, 0u, 0u });
        vertexBuffer.declaration[0] = { 0u, 0u, 2u, 0u, 0u, 0u };
        vertexBuffer.declaration[1] = { 0u, 12u, 2u, 0u, 3u, 0u };
        vertexBuffer.declaration[2] = { 0u, 24u, 1u, 0u, 5u, 0u };
        vertexBuffer.dataOffset = bufferOffset;

        TestSdkmeshIndexBufferHeader indexBuffer{};
        indexBuffer.numIndices = indices.size();
        indexBuffer.sizeBytes = indexBytes;
        indexBuffer.indexType = 0u;
        indexBuffer.dataOffset = bufferOffset + vertexBytes;

        TestSdkmeshMesh mesh{};
        strcpy_s(mesh.name, "TwoMaterialBox");
        mesh.numVertexBuffers = 1u;
        mesh.indexBuffer = 0u;
        mesh.numSubsets = 2u;
        mesh.boundingBoxExtents = { h, h, h };
        mesh.subsetOffset = subsetTableOffset;

        std::array<TestSdkmeshSubset, 2> subsets{};
        for (std::size_t index{}; index < subsets.size(); ++index)
        {
            auto& subset = subsets[index];
            strcpy_s(subset.name, index == 0u ? "Red" : "Blue");
            subset.materialId = static_cast<std::uint32_t>(index);
            subset.primitiveType = 0u;
            subset.indexStart = index * 18u;
            subset.indexCount = 18u;
            subset.vertexStart = index * 12u;
            subset.vertexCount = 12u;
        }

        TestSdkmeshFrame frame{};
        strcpy_s(frame.name, "Root");
        frame.mesh = 0u;
        frame.parentFrame = 0xffffffffu;
        frame.childFrame = 0xffffffffu;
        frame.siblingFrame = 0xffffffffu;
        DirectX::XMStoreFloat4x4(&frame.matrix, DirectX::XMMatrixIdentity());
        frame.animationDataIndex = 0xffffffffu;

        std::array<TestSdkmeshMaterial, 2> materials{};
        strcpy_s(materials[0].name, "Red");
        materials[0].diffuse = { 0.9f, 0.15f, 0.1f, 1.0f };
        materials[0].ambient = { 0.9f, 0.15f, 0.1f, 1.0f };
        strcpy_s(materials[1].name, "Blue");
        materials[1].diffuse = { 0.1f, 0.2f, 0.9f, 1.0f };
        materials[1].ambient = { 0.1f, 0.2f, 0.9f, 1.0f };

        const std::array<std::uint32_t, 2> subsetTable{ 0u, 1u };
        std::vector<std::uint8_t> file(
            static_cast<std::size_t>(bufferOffset + vertexBytes + indexBytes));
        const auto write = [&file](
            const std::uint64_t offset,
            const void* const data,
            const std::size_t size)
        {
            std::memcpy(file.data() + offset, data, size);
        };
        write(0u, &header, sizeof(header));
        write(vertexHeaderOffset, &vertexBuffer, sizeof(vertexBuffer));
        write(indexHeaderOffset, &indexBuffer, sizeof(indexBuffer));
        write(meshOffset, &mesh, sizeof(mesh));
        write(subsetOffset, subsets.data(), sizeof(subsets));
        write(frameOffset, &frame, sizeof(frame));
        write(materialOffset, materials.data(), sizeof(materials));
        write(subsetTableOffset, subsetTable.data(), sizeof(subsetTable));
        write(bufferOffset, vertices.data(), vertexBytes);
        write(bufferOffset + vertexBytes, indices.data(), indexBytes);

        return file;
    }

    // D3D12 Importerが、DirectXTKと同じくVertexStartで指した2つの頂点
    // 範囲をsubsetごとの赤／青のMaterialへ変換することを確かめます。
    // 合成データはメモリから直接渡し、検証用の一時ファイルを作りません。
    void RequireD3D12SdkmeshModel()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto file = BuildTestSdkmesh();
        const auto model = LamaPon::SdkmeshImporter::LoadFromMemory(
            graphics.Assets(),
            file,
            L"TwoMaterialBox.sdkmesh");
        Require(
            model != nullptr
                && model->hasLocalBounds
                && model->primitives.size() == 2u,
            "The DirectX 12 SDKMESH import did not retain its subsets");
        for (const auto& primitive : model->primitives)
        {
            Require(
                primitive.cpuIndices.size() == 18u
                    && primitive.cpuVertexStride != 0u
                    && primitive.cpuVertexData.size()
                        == 12u * primitive.cpuVertexStride
                    && !primitive.vertexBuffer
                    && !primitive.indexBuffer
                    && !primitive.effect,
                "The DirectX 12 SDKMESH import did not read its vertex "
                "ranges");
        }

        Require(
            model->primitives[0].baseColor.x > 0.8f
                && model->primitives[0].baseColor.z < 0.2f
                && model->primitives[1].baseColor.z > 0.8f
                && model->primitives[1].baseColor.x < 0.2f,
            "The DirectX 12 SDKMESH import did not retain subset materials");
    }

#pragma pack(push, 1)
    struct TestVboHeader final
    {
        std::uint32_t vertexCount;
        std::uint32_t indexCount;
    };

    struct TestVboVertex final
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 textureCoordinate;
    };
#pragma pack(pop)

    static_assert(sizeof(TestVboHeader) == 8u);
    static_assert(sizeof(TestVboVertex) == 32u);

    [[nodiscard]] std::vector<std::uint8_t> BuildTestVbo()
    {
        constexpr TestVboHeader header{ 4u, 6u };
        constexpr std::array<TestVboVertex, 4> vertices{ {
            { { -1.0f, -0.5f, 0.25f }, { 0.0f, 0.0f, -1.0f },
                { 0.0f, 1.0f } },
            { { -1.0f, 0.5f, 0.25f }, { 0.0f, 0.0f, -1.0f },
                { 0.0f, 0.0f } },
            { { 1.0f, 0.5f, 0.25f }, { 0.0f, 0.0f, -1.0f },
                { 1.0f, 0.0f } },
            { { 1.0f, -0.5f, 0.25f }, { 0.0f, 0.0f, -1.0f },
                { 1.0f, 1.0f } } } };
        constexpr std::array<std::uint16_t, 6> indices{
            0u, 1u, 2u, 0u, 2u, 3u };
        std::vector<std::uint8_t> bytes(
            sizeof(header) + sizeof(vertices) + sizeof(indices));
        std::size_t offset{};
        const auto append = [&bytes, &offset](
            const void* const data,
            const std::size_t size)
        {
            std::memcpy(bytes.data() + offset, data, size);
            offset += size;
        };
        append(&header, sizeof(header));
        append(vertices.data(), sizeof(vertices));
        append(indices.data(), sizeof(indices));
        return bytes;
    }

    void RequireD3D12VboModel()
    {
        const auto bytes = BuildTestVbo();
        const auto model = LamaPon::VboImporter::LoadFromMemory(
            bytes,
            L"Quad.vbo");
        Require(
            model != nullptr
                && model->hasLocalBounds
                && model->nodes.size() == 1u
                && model->primitives.size() == 1u,
            "The DirectX 12 VBO import did not create one CPU primitive");
        const auto& primitive = model->primitives.front();
        Require(
            primitive.cpuVertexStride == 60u
                && primitive.cpuVertexData.size() == 4u * 60u
                && primitive.cpuIndices
                    == std::vector<std::uint32_t>{ 0u, 1u, 2u, 0u, 2u, 3u }
                && !primitive.vertexBuffer
                && !primitive.indexBuffer
                && !primitive.effect,
            "The DirectX 12 VBO import did not retain its geometry");
        Require(
            model->localBounds.minimum.x == -1.0f
                && model->localBounds.minimum.y == -0.5f
                && model->localBounds.minimum.z == 0.25f
                && model->localBounds.maximum.x == 1.0f
                && model->localBounds.maximum.y == 0.5f
                && model->localBounds.maximum.z == 0.25f,
            "The DirectX 12 VBO import calculated incorrect bounds");
    }

    void RequireD3D12Particles()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        auto& particleObject = scene.CreateGameObject("Particles");
        auto& particles = particleObject.AddComponent<
            LamaPon::ParticleSystemComponent>();
        particles.SetPlayOnStart(false);
        particles.SetAdditive(false);
        particles.SetStartColor({ 0.1f, 0.8f, 0.25f, 1.0f });
        particles.SetEndColor({ 0.1f, 0.8f, 0.25f, 1.0f });
        particles.EmitParticle(
            { 0.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 0.0f },
            1.0f,
            1.2f);

        constexpr float clearColor[4]{ 0.01f, 0.02f, 0.03f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        std::uint32_t width{};
        std::uint32_t height{};
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 particle capture has unexpected dimensions");
        const auto center =
            (static_cast<std::size_t>(CanvasHeight / 2u) * CanvasWidth
                + CanvasWidth / 2u) * 4u;
        Require(
            pixels[center + 1u] > 150u
                && pixels[center] < 80u,
            "The DirectX 12 particle service did not draw its billboard");
    }

    void RequireMatchingCaptures(
        const Capture& d3d11,
        const Capture& d3d12)
    {
        const std::size_t expectedBytes =
            static_cast<std::size_t>(CanvasWidth) * CanvasHeight * 4u;
        Require(
            d3d11.width == CanvasWidth
                && d3d11.height == CanvasHeight
                && d3d12.width == CanvasWidth
                && d3d12.height == CanvasHeight
                && d3d11.pixels.size() == expectedBytes
                && d3d12.pixels.size() == expectedBytes,
            "The sprite captures have unexpected dimensions");

        // 不透明な赤い矩形の中心は、どちらのBackendでも赤くなります。
        const std::size_t rectangleCenter =
            (17u * static_cast<std::size_t>(CanvasWidth) + 26u) * 4u;
        Require(
            d3d12.pixels[rectangleCenter] > 180u
                && d3d12.pixels[rectangleCenter + 1u] < 90u,
            "The DirectX 12 sprite capture did not contain the red rectangle");

        // WARP上の同じ演算でも、最終丸めの1段差だけは許容します。
        constexpr int ChannelTolerance = 2;
        std::size_t mismatchedPixels{};
        std::size_t firstMismatch =
            std::numeric_limits<std::size_t>::max();
        int largestDifference{};
        for (std::size_t pixel{}; pixel < expectedBytes / 4u; ++pixel)
        {
            int pixelDifference{};
            for (std::size_t channel{}; channel < 4u; ++channel)
            {
                const auto offset = pixel * 4u + channel;
                pixelDifference = std::max(
                    pixelDifference,
                    std::abs(
                        static_cast<int>(d3d11.pixels[offset])
                        - static_cast<int>(d3d12.pixels[offset])));
            }
            largestDifference = std::max(
                largestDifference,
                pixelDifference);
            if (pixelDifference > ChannelTolerance)
            {
                ++mismatchedPixels;
                firstMismatch = std::min(firstMismatch, pixel);
            }
        }
        if (mismatchedPixels == 0)
        {
            return;
        }

        const auto describe = [&](const Capture& capture)
        {
            const auto offset = firstMismatch * 4u;
            return std::to_string(capture.pixels[offset]) + ","
                + std::to_string(capture.pixels[offset + 1u]) + ","
                + std::to_string(capture.pixels[offset + 2u]) + ","
                + std::to_string(capture.pixels[offset + 3u]);
        };
        throw std::runtime_error(
            "DirectX 12 sprite output differed from DirectX 11 in "
            + std::to_string(mismatchedPixels)
            + " pixels (largest channel difference "
            + std::to_string(largestDifference)
            + "; first at "
            + std::to_string(firstMismatch % CanvasWidth)
            + ","
            + std::to_string(firstMismatch / CanvasWidth)
            + " D3D11="
            + describe(d3d11)
            + " D3D12="
            + describe(d3d12)
            + ")");
    }

    enum class PostProcessCase : std::uint8_t
    {
        Bloom,
        LensFlare,
        ToneMapping,
        Fxaa,
        Temporal,
        MotionBlur
    };

    constexpr std::array<PostProcessCase, 6> PostProcessCases{
        PostProcessCase::Bloom,
        PostProcessCase::LensFlare,
        PostProcessCase::ToneMapping,
        PostProcessCase::Fxaa,
        PostProcessCase::Temporal,
        PostProcessCase::MotionBlur
    };

    [[nodiscard]] std::string PostProcessCaseName(
        const PostProcessCase postProcess)
    {
        switch (postProcess)
        {
        case PostProcessCase::Bloom:
            return "bloom";
        case PostProcessCase::LensFlare:
            return "screen-space lens flare";
        case PostProcessCase::ToneMapping:
            return "tone mapping";
        case PostProcessCase::Fxaa:
            return "FXAA";
        case PostProcessCase::Temporal:
            return "TAA";
        case PostProcessCase::MotionBlur:
            return "motion blur";
        }
        return "post-process";
    }

    // 64x32のHDR offscreenへ同じ絵を描いてpost-processを1つだけ掛け、
    // Spriteで画面へ写します。D3D11とD3D12の結果を画素で比べるための
    // captureです。
    [[nodiscard]] std::array<Capture, PostProcessCases.size()>
        RenderPostProcessCaptures(
            const LamaPon::RenderingApi api,
            const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The post-process capture did not start the requested "
            "rendering API");
        // D3D11のpost-processはEnvironment shaderをasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);

        LamaPon::RenderTarget target;
        graphics.ResizeOffscreenTarget(target, 64u, 32u);

        std::array<Capture, PostProcessCases.size()> captures;
        for (std::size_t index{}; index < PostProcessCases.size(); ++index)
        {
            const auto postProcess = PostProcessCases[index];
            constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
            DirectX::XMFLOAT4X4 identity{};
            DirectX::XMStoreFloat4x4(
                &identity,
                DirectX::XMMatrixIdentity());
            if (postProcess == PostProcessCase::Temporal)
            {
                // 1フレーム目の灰色を履歴へ控えます。次のフレームでは
                // 白黒境界の近傍範囲内へ履歴が収まり、実際に混ざります。
                graphics.BeginFrame(clearColor);
                const auto historyOutput = graphics.CaptureOutputState();
                graphics.BeginOffscreenTarget(target, clearColor);
                {
                    auto pass = graphics.BeginSpritePass();
                    DrawRectangle(
                        pass,
                        0.0f,
                        0.0f,
                        64.0f,
                        32.0f,
                        { 0.4f, 0.4f, 0.4f, 1.0f });
                }
                graphics.CaptureOffscreenTargetTemporalHistory(
                    target,
                    identity);
                graphics.RestoreOutputState(*historyOutput);
                graphics.EndFrame();
            }
            else if (postProcess == PostProcessCase::MotionBlur)
            {
                // 最初のフレームは描画せず、横へずれた前フレームの
                // ビュー射影だけをtargetへ保存します。次フレームとの
                // 差が8pxぶんのカメラ移動になります。
                DirectX::XMFLOAT4X4 previousViewProjection{};
                DirectX::XMStoreFloat4x4(
                    &previousViewProjection,
                    DirectX::XMMatrixTranslation(0.25f, 0.0f, 0.0f));
                graphics.BeginFrame(clearColor);
                const auto previousOutput = graphics.CaptureOutputState();
                graphics.BeginOffscreenTarget(target, clearColor);
                LamaPon::MotionBlurSettings motionBlur;
                motionBlur.enabled = true;
                motionBlur.intensity = 1.0f;
                motionBlur.maximumRadius = 8.0f;
                graphics.ApplyOffscreenTargetMotionBlur(
                    target,
                    motionBlur,
                    identity,
                    previousViewProjection,
                    8u);
                graphics.RestoreOutputState(*previousOutput);
                graphics.EndFrame();
            }
            graphics.BeginFrame(clearColor);
            const auto primaryOutput = graphics.CaptureOutputState();
            graphics.BeginOffscreenTarget(target, clearColor);
            {
                auto pass = graphics.BeginSpritePass();
                if (postProcess == PostProcessCase::Fxaa)
                {
                    // 回転した矩形の縁は階段状になり、FXAAが中間色で平します。
                    LamaPon::SpriteDrawRequest edge;
                    edge.position = { 32.0f, 16.0f };
                    edge.origin = { 0.5f, 0.5f };
                    edge.scale = { 30.0f, 12.0f };
                    edge.rotation = 0.5f;
                    edge.tint = { 0.9f, 0.9f, 0.9f, 1.0f };
                    Require(
                        pass.Draw(edge),
                        "The FXAA edge sprite was rejected");
                }
                else if (postProcess == PostProcessCase::Temporal)
                {
                    DrawRectangle(
                        pass,
                        32.0f,
                        0.0f,
                        32.0f,
                        32.0f,
                        { 1.0f, 1.0f, 1.0f, 1.0f });
                }
                else if (postProcess == PostProcessCase::MotionBlur)
                {
                    DrawRectangle(
                        pass,
                        28.0f,
                        0.0f,
                        8.0f,
                        32.0f,
                        { 1.0f, 1.0f, 1.0f, 1.0f });
                }
                else
                {
                    DrawRectangle(
                        pass,
                        28.0f,
                        12.0f,
                        8.0f,
                        8.0f,
                        { 4.0f, 2.0f, 0.5f, 1.0f });
                }
            }
            switch (postProcess)
            {
            case PostProcessCase::Bloom:
            {
                LamaPon::BloomSettings bloom;
                bloom.enabled = true;
                graphics.ApplyOffscreenTargetBloom(target, bloom);
                break;
            }
            case PostProcessCase::LensFlare:
            {
                LamaPon::ScreenSpaceLensFlareSettings lensFlare;
                lensFlare.enabled = true;
                lensFlare.threshold = 1.0f;
                lensFlare.intensity = 0.5f;
                lensFlare.streakIntensity = 0.4f;
                lensFlare.streakLength = 0.3f;
                lensFlare.streakDirections = 2u;
                lensFlare.streakAngleDegrees = 15.0f;
                graphics.ApplyOffscreenTargetScreenSpaceLensFlare(
                    target,
                    lensFlare);
                break;
            }
            case PostProcessCase::ToneMapping:
                graphics.ApplyOffscreenTargetToneMapping(
                    target,
                    LamaPon::ColorGradingSettings{});
                break;
            case PostProcessCase::Fxaa:
                graphics.ApplyOffscreenTargetFXAA(target);
                break;
            case PostProcessCase::Temporal:
            {
                LamaPon::TemporalAntiAliasingSettings temporal;
                temporal.enabled = true;
                temporal.historyWeight = 0.75f;
                temporal.clampTolerance = 4.0f;
                LamaPon::TemporalAntiAliasingInputs inputs;
                inputs.inverseViewProjection = identity;
                inputs.viewProjection = identity;
                graphics.ApplyOffscreenTargetTemporalAntiAliasing(
                    target,
                    temporal,
                    inputs);
                break;
            }
            case PostProcessCase::MotionBlur:
            {
                LamaPon::MotionBlurSettings motionBlur;
                motionBlur.enabled = true;
                motionBlur.intensity = 1.0f;
                motionBlur.maximumRadius = 8.0f;
                graphics.ApplyOffscreenTargetMotionBlur(
                    target,
                    motionBlur,
                    identity,
                    identity,
                    8u);
                break;
            }
            }
            graphics.PublishOffscreenTarget(target);
            graphics.RestoreOutputState(*primaryOutput);
            {
                auto pass = graphics.BeginSpritePass();
                LamaPon::SpriteDrawRequest request;
                request.texture = target.DisplayViewHandle();
                request.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
                Require(
                    pass.Draw(request),
                    "The post-processed offscreen display view was "
                    "rejected");
            }
            auto& capture = captures[index];
            capture.pixels = graphics.CaptureBackBuffer(
                capture.width,
                capture.height);
            graphics.EndFrame();
        }
        return captures;
    }

    void RequireMatchingPostProcessCaptures(
        const std::array<Capture, PostProcessCases.size()>& d3d11,
        const std::array<Capture, PostProcessCases.size()>& d3d12)
    {
        const std::size_t expectedBytes =
            static_cast<std::size_t>(CanvasWidth) * CanvasHeight * 4u;
        const auto offset = [](const std::uint32_t x, const std::uint32_t y)
        {
            return (static_cast<std::size_t>(y) * CanvasWidth + x) * 4u;
        };
        for (std::size_t index{}; index < PostProcessCases.size(); ++index)
        {
            const auto postProcess = PostProcessCases[index];
            const auto name = PostProcessCaseName(postProcess);
            for (const auto* const capture : { &d3d11[index], &d3d12[index] })
            {
                Require(
                    capture->width == CanvasWidth
                        && capture->height == CanvasHeight
                        && capture->pixels.size() == expectedBytes,
                    "The " + name + " captures have unexpected dimensions");
                const auto& pixels = capture->pixels;
                switch (postProcess)
                {
                case PostProcessCase::Bloom:
                    // 矩形はx=28..35、y=12..19です。半径2の9tapは縁から
                    // 2画素先まで届き、5画素離れると黒のままです。
                    Require(
                        pixels[offset(36u, 16u)] > 60u
                            && pixels[offset(36u, 16u) + 1u] > 20u,
                        "The bloom did not spread beyond the bright "
                        "rectangle");
                    Require(
                        pixels[offset(40u, 16u)] < 4u
                            && pixels[offset(5u, 5u)] < 4u,
                        "The bloom spread beyond its radius");
                    break;
                case PostProcessCase::LensFlare:
                {
                    // 元の8x8高輝度矩形の外側へ、ゴースト、ハロー、筋が
                    // 十分な範囲で広がっていることを確認します。
                    std::size_t flarePixels{};
                    for (std::uint32_t y{}; y < CanvasHeight; ++y)
                    {
                        for (std::uint32_t x{}; x < CanvasWidth; ++x)
                        {
                            if (x >= 28u && x < 36u
                                && y >= 12u && y < 20u)
                            {
                                continue;
                            }
                            const auto pixelOffset = offset(x, y);
                            flarePixels +=
                                pixels[pixelOffset] > 4u
                                    || pixels[pixelOffset + 1u] > 4u
                                    || pixels[pixelOffset + 2u] > 4u
                                ? 1u
                                : 0u;
                        }
                    }
                    Require(
                        flarePixels > 40u,
                        "The screen-space lens flare did not create "
                        "ghosts, a halo, and streaks");
                    break;
                }
                case PostProcessCase::ToneMapping:
                    // 既定のカラーグレーディングとACESで(4, 2, 0.5)は約
                    // (255, 242, 162)になり、単純clipの(255, 255, 128)とは
                    // 異なります。
                    Require(
                        pixels[offset(32u, 16u) + 1u] > 225u
                            && pixels[offset(32u, 16u) + 1u] < 252u
                            && pixels[offset(32u, 16u) + 2u] > 140u
                            && pixels[offset(32u, 16u) + 2u] < 180u,
                        "The tone mapping did not apply ACES and color "
                        "grading");
                    break;
                case PostProcessCase::Fxaa:
                {
                    // 回転した矩形の縁にだけ、FXAAが中間の明るさを作ります。
                    std::size_t blendedPixels{};
                    for (std::uint32_t y{}; y < 32u; ++y)
                    {
                        for (std::uint32_t x{}; x < 64u; ++x)
                        {
                            const auto value = pixels[offset(x, y)];
                            if (value > 16u && value < 200u)
                            {
                                ++blendedPixels;
                            }
                        }
                    }
                    Require(
                        blendedPixels > 8u,
                        "FXAA did not smooth the rotated edge");
                    break;
                }
                case PostProcessCase::Temporal:
                    // 現在色はx<32が黒、履歴は全面0.4です。境界の1画素は
                    // 近傍クランプを通過して履歴比率0.75で混ざり、その外側は
                    // 黒へクランプされます。
                    Require(
                        pixels[offset(30u, 16u)] < 4u
                            && pixels[offset(31u, 16u)] > 65u
                            && pixels[offset(31u, 16u)] < 90u
                            && pixels[offset(32u, 16u)] > 125u
                            && pixels[offset(32u, 16u)] < 155u,
                        "TAA did not reproject and clamp the temporal "
                        "history at the edge");
                    break;
                case PostProcessCase::MotionBlur:
                    // 元の白帯はx=28..35です。前フレームとの8pxの差を
                    // 中心から両側へ伸ばすため、外側に中間色ができます。
                    Require(
                        pixels[offset(26u, 16u)] > 20u
                            && pixels[offset(31u, 16u)] > 100u
                            && pixels[offset(31u, 16u)] < 250u,
                        "Motion blur did not spread the moving edge");
                    break;
                }
            }

            // WARP上の同じ演算でも、最終丸めの1段差だけは許容します。
            constexpr int ChannelTolerance = 2;
            for (std::size_t byte{}; byte < expectedBytes; ++byte)
            {
                const int difference = std::abs(
                    static_cast<int>(d3d11[index].pixels[byte])
                    - static_cast<int>(d3d12[index].pixels[byte]));
                if (difference > ChannelTolerance)
                {
                    const auto pixel = byte / 4u;
                    throw std::runtime_error(
                        "DirectX 12 " + name
                        + " differed from DirectX 11 at "
                        + std::to_string(pixel % CanvasWidth)
                        + ","
                        + std::to_string(pixel / CanvasWidth)
                        + " by "
                        + std::to_string(difference));
                }
            }
        }
    }

    void RequireD3D12DdsTextures()
    {
        const auto rgbaBytes = BuildRgbaDds();
        const auto rgbaPrepared =
            LamaPon::TextureLoader::PrepareDdsTextureData(rgbaBytes);
        Require(
            rgbaPrepared.format == DXGI_FORMAT_R8G8B8A8_UNORM
                && rgbaPrepared.levels.size() == 2u
                && rgbaPrepared.levels[0].width == 4u
                && rgbaPrepared.levels[0].height == 4u
                && rgbaPrepared.levels[0].rowPitch == 16u
                && rgbaPrepared.levels[1].width == 2u
                && rgbaPrepared.levels[1].height == 2u
                && rgbaPrepared.levels[1].bytes[2] == 220u,
            "The DDS parser did not preserve the RGBA8 mip chain");

        const std::vector<std::uint8_t> bc1Block{
            0x00u, 0xf8u, 0x00u, 0x00u,
            0x00u, 0x00u, 0x00u, 0x00u };
        std::vector<std::uint8_t> bc3Block(16u);
        bc3Block[0] = 255u;
        bc3Block[1] = 255u;
        bc3Block[8] = 0xe0u;
        bc3Block[9] = 0x07u;
        std::vector<std::uint8_t> bc5Block(16u);
        bc5Block[0] = 0u;
        bc5Block[1] = 0u;
        bc5Block[8] = 255u;
        bc5Block[9] = 255u;
        const auto bc1Bytes = BuildClassicDds(
            4u,
            4u,
            1u,
            MakeFourCc('D', 'X', 'T', '1'),
            bc1Block);
        const auto bc3Bytes = BuildClassicDds(
            4u,
            4u,
            1u,
            MakeFourCc('D', 'X', 'T', '5'),
            bc3Block);
        const auto bc5Bytes = BuildClassicDds(
            4u,
            4u,
            1u,
            MakeFourCc('A', 'T', 'I', '2'),
            bc5Block);
        std::vector<std::uint8_t> bgraPixels;
        for (std::size_t pixel{}; pixel < 16u; ++pixel)
        {
            bgraPixels.insert(bgraPixels.end(), { 30u, 60u, 210u, 255u });
        }
        const auto bgraBytes = BuildDx10Dds(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            bgraPixels);
        Require(
            LamaPon::TextureLoader::PrepareDdsTextureData(bc1Bytes).format
                    == DXGI_FORMAT_BC1_UNORM
                && LamaPon::TextureLoader::PrepareDdsTextureData(bc3Bytes)
                        .format == DXGI_FORMAT_BC3_UNORM
                && LamaPon::TextureLoader::PrepareDdsTextureData(bc5Bytes)
                        .format == DXGI_FORMAT_BC5_UNORM
                && LamaPon::TextureLoader::PrepareDdsTextureData(bgraBytes)
                        .format == DXGI_FORMAT_B8G8R8A8_UNORM,
            "The DDS parser did not map classic and DX10 formats");

        auto cubeBytes = bc1Bytes;
        WriteLittleEndian32(cubeBytes, 112u, 0x200u);
        bool rejectedCube{};
        try
        {
            static_cast<void>(
                LamaPon::TextureLoader::PrepareDdsTextureData(cubeBytes));
        }
        catch (const std::invalid_argument&)
        {
            rejectedCube = true;
        }
        Require(
            rejectedCube,
            "The 2D DDS parser accepted a cube texture as a flat image");
        auto arrayBytes = bgraBytes;
        WriteLittleEndian32(arrayBytes, 140u, 2u);
        bool rejectedArray{};
        try
        {
            static_cast<void>(
                LamaPon::TextureLoader::PrepareDdsTextureData(arrayBytes));
        }
        catch (const std::invalid_argument&)
        {
            rejectedArray = true;
        }
        Require(
            rejectedArray,
            "The 2D DDS parser accepted a DX10 texture array");

        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const std::array views{
            graphics.Assets().CreateTextureViewHandleFromMemory(
                bc1Bytes,
                true),
            graphics.Assets().CreateTextureViewHandleFromMemory(
                bc3Bytes,
                true),
            graphics.Assets().CreateTextureViewHandleFromMemory(
                bc5Bytes,
                true),
            graphics.Assets().CreateTextureViewHandleFromMemory(
                rgbaBytes,
                true),
            graphics.Assets().CreateTextureViewHandleFromMemory(
                bgraBytes,
                true) };
        for (const auto& view : views)
        {
            Require(
                graphics.IsGraphicsViewCurrent(view),
                "A parsed DDS view was not created by the DirectX 12 "
                "backend");
        }

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        {
            auto pass = graphics.BeginSpritePass();
            for (std::size_t index{}; index < views.size(); ++index)
            {
                LamaPon::SpriteDrawRequest request;
                request.texture = views[index];
                request.position = {
                    static_cast<float>(index * 48u),
                    0.0f };
                request.scale = { 12.0f, 32.0f };
                Require(pass.Draw(request), "A DDS sprite was rejected");
            }
        }
        std::uint32_t width{};
        std::uint32_t height{};
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        const auto channel = [&](const std::uint32_t x, const std::size_t c)
        {
            return pixels[(64u * CanvasWidth + x) * 4u + c];
        };
        Require(
            width == CanvasWidth
                && height == CanvasHeight
                && channel(24u, 0u) > 220u
                && channel(24u, 1u) < 20u
                && channel(72u, 1u) > 220u
                && channel(72u, 0u) < 20u
                && channel(120u, 1u) > 220u
                && channel(120u, 0u) < 20u
                && channel(168u, 0u) > 190u
                && channel(168u, 1u) > 25u
                && channel(168u, 1u) < 60u
                && channel(216u, 0u) > 190u
                && channel(216u, 1u) > 40u
                && channel(216u, 1u) < 80u
                && channel(216u, 2u) < 45u,
            "DirectX 12 did not sample RGBA8, BGRA8, BC1, BC3, and BC5 "
            "DDS textures with their expected colors");
    }

    // 左半分を輝度2.0、右半分を0.125で塗ると、対数平均（幾何平均）は0.5に
    // なり、keyValue 0.18の露出補正はlog2(0.18 / 0.5)段です。算術平均の
    // 約1.06とは大きく変わるため、PSLuminanceと同じ対数平均で測れているかを
    // 確かめられます。
    void RequireD3D12AutoExposure()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);

        LamaPon::RenderTarget target;
        graphics.ResizeOffscreenTarget(target, 64u, 32u);
        LamaPon::AutoExposureSettings settings;
        settings.enabled = true;
        // 測定値は次フレーム以降に非同期で読むため、数フレーム描きます。
        for (int frame{}; frame < 4; ++frame)
        {
            constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
            graphics.BeginFrame(clearColor);
            const auto primaryOutput = graphics.CaptureOutputState();
            graphics.BeginOffscreenTarget(target, clearColor);
            {
                auto pass = graphics.BeginSpritePass();
                DrawRectangle(
                    pass,
                    0.0f,
                    0.0f,
                    32.0f,
                    32.0f,
                    { 2.0f, 2.0f, 2.0f, 1.0f });
                DrawRectangle(
                    pass,
                    32.0f,
                    0.0f,
                    32.0f,
                    32.0f,
                    { 0.125f, 0.125f, 0.125f, 1.0f });
            }
            static_cast<void>(graphics.UpdateOffscreenTargetAutoExposure(
                target,
                settings,
                1.0f / 60.0f));
            graphics.RestoreOutputState(*primaryOutput);
            // 画面の読み出しでGPUの完了を待ち、次のフレームで測定値を
            // 確実に読めるようにします。
            std::uint32_t width{};
            std::uint32_t height{};
            static_cast<void>(graphics.CaptureBackBuffer(width, height));
            graphics.EndFrame();
        }

        const float expected = std::log2(0.18f / 0.5f);
        Require(
            std::abs(target.AdaptedLuminance() - 0.5f) < 0.01f
                && std::abs(target.AutoExposureStops() - expected) < 0.02f,
            "DirectX 12 auto exposure did not follow the geometric mean "
            "luminance (adapted "
                + std::to_string(target.AdaptedLuminance())
                + ", stops "
                + std::to_string(target.AutoExposureStops())
                + ")");
    }

    // 床へ置いたCubeを斜め上から見下ろすSceneです。接地部の周りだけに
    // 遮蔽が生まれ、平らな床の残りと空は遮蔽されません。床もCubeで作り、
    // D3D11とD3D12で同じ形の深度を書きます（PlaneはD3D11だけ厚みの
    // ある箱です）。近平面にかからない広さに収めます。
    void BuildAmbientOcclusionScene(LamaPon::Scene& scene)
    {
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 2.4f, 6.0f };
        cameraObject.GetTransform().SetEulerAngles(-0.42f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        auto& floor = scene.CreateGameObject("Floor");
        floor.GetTransform().position = { 0.0f, -1.7f, 0.0f };
        floor.GetTransform().scale = { 8.0f, 1.0f, 8.0f };
        floor.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });
        auto& cube = scene.CreateGameObject("Cube");
        cube.GetTransform().position = { 0.0f, -0.7f, 0.0f };
        cube.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });

        auto occlusion = scene.AmbientOcclusion();
        occlusion.enabled = true;
        occlusion.radius = 0.75f;
        occlusion.strength = 1.0f;
        scene.SetAmbientOcclusionSettings(occlusion);
    }

    void EnableAmbientOcclusionQuality(LamaPon::GraphicsDevice& graphics)
    {
        auto settings = graphics.Settings();
        settings.ambientOcclusionEnabled = true;
        settings.ambientOcclusionSampleCount = 16u;
        graphics.SetGraphicsSettings(settings);
    }

    // 同じSceneをD3D11とD3D12で描き、深度プリパスから求めた遮蔽texture
    // （半解像度）を画面左上へ等倍で写したcaptureです。
    [[nodiscard]] Capture RenderAmbientOcclusionCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The ambient occlusion capture did not start the requested "
            "rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        EnableAmbientOcclusionQuality(graphics);
        LamaPon::Scene scene(graphics);
        BuildAmbientOcclusionScene(scene);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        auto* const target = graphics.SceneCompositionTarget();
        Require(
            target != nullptr,
            "The ambient occlusion capture has no scene composition target");
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            target);
        const auto occlusionView = target->AmbientOcclusionViewHandle();
        const auto& occlusion = graphics.Lighting().screenAmbientOcclusion;
        Require(
            occlusion.enabled
                && occlusion.texture == occlusionView
                && graphics.IsGraphicsViewCurrent(occlusionView),
            "The scene did not resolve SSAO from its depth prepass");
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        {
            auto pass = graphics.BeginSpritePass();
            LamaPon::SpriteDrawRequest request;
            request.texture = occlusionView;
            request.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(request),
                "The ambient occlusion view was rejected");
        }
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();
        return capture;
    }

    void RequireMatchingAmbientOcclusionCaptures(
        const Capture& d3d11,
        const Capture& d3d12)
    {
        const std::size_t expectedBytes =
            static_cast<std::size_t>(CanvasWidth) * CanvasHeight * 4u;
        for (const auto* const capture : { &d3d11, &d3d12 })
        {
            Require(
                capture->width == CanvasWidth
                    && capture->height == CanvasHeight
                    && capture->pixels.size() == expectedBytes,
                "The ambient occlusion captures have unexpected dimensions");
        }

        // 遮蔽textureはR8なので、Spriteで写すと赤だけに値が入ります。
        constexpr std::uint32_t OcclusionWidth = CanvasWidth / 2u;
        constexpr std::uint32_t OcclusionHeight = CanvasHeight / 2u;
        constexpr int ChannelTolerance = 2;
        std::size_t d3d11OccludedPixels{};
        std::size_t d3d12OccludedPixels{};
        std::size_t mismatchedPixels{};
        std::size_t firstMismatch =
            std::numeric_limits<std::size_t>::max();
        int largestDifference{};
        for (std::uint32_t y{}; y < OcclusionHeight; ++y)
        {
            for (std::uint32_t x{}; x < OcclusionWidth; ++x)
            {
                const std::size_t pixel =
                    static_cast<std::size_t>(y) * CanvasWidth + x;
                const int d3d11Value = d3d11.pixels[pixel * 4u];
                const int d3d12Value = d3d12.pixels[pixel * 4u];
                // 接地部の遮蔽はブラー後で約0.8〜0.9の明るさです。
                d3d11OccludedPixels += d3d11Value < 240 ? 1u : 0u;
                d3d12OccludedPixels += d3d12Value < 240 ? 1u : 0u;
                const int difference = std::abs(d3d11Value - d3d12Value);
                largestDifference = std::max(largestDifference, difference);
                if (difference > ChannelTolerance)
                {
                    ++mismatchedPixels;
                    firstMismatch = std::min(firstMismatch, pixel);
                }
            }
        }
        Require(
            d3d11OccludedPixels > 20u && d3d12OccludedPixels > 20u,
            "The ambient occlusion captures did not occlude the cube "
            "contact (D3D11 "
                + std::to_string(d3d11OccludedPixels)
                + ", D3D12 "
                + std::to_string(d3d12OccludedPixels)
                + " pixels)");
        if (mismatchedPixels == 0)
        {
            return;
        }
        throw std::runtime_error(
            "DirectX 12 ambient occlusion differed from DirectX 11 in "
            + std::to_string(mismatchedPixels)
            + " pixels (largest difference "
            + std::to_string(largestDifference)
            + "; first at "
            + std::to_string(firstMismatch % CanvasWidth)
            + ","
            + std::to_string(firstMismatch / CanvasWidth)
            + " D3D11="
            + std::to_string(d3d11.pixels[firstMismatch * 4u])
            + " D3D12="
            + std::to_string(d3d12.pixels[firstMismatch * 4u])
            + ")");
    }

    // SSAOはD3D11と同じく環境光項だけへ掛かります。環境光だけで照らした
    // Sceneでは接地部が暗くなり、明るくなる画素はありません。環境光を0に
    // すると掛ける先が無くなるため、SSAOの有無で画像は1bitも変わりません。
    void RequireD3D12AmbientOcclusion()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        EnableAmbientOcclusionQuality(graphics);
        LamaPon::Scene scene(graphics);
        BuildAmbientOcclusionScene(scene);

        constexpr float clearColor[4]{ 0.02f, 0.03f, 0.05f, 1.0f };
        const auto captureComposition = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            std::uint32_t width{};
            std::uint32_t height{};
            auto capture = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 SSAO capture has unexpected dimensions");
            return capture;
        };
        const auto setOcclusionEnabled = [&](const bool enabled)
        {
            auto occlusion = scene.AmbientOcclusion();
            occlusion.enabled = enabled;
            scene.SetAmbientOcclusionSettings(occlusion);
        };

        setOcclusionEnabled(false);
        const auto withoutOcclusion = captureComposition();
        Require(
            !graphics.Lighting().screenAmbientOcclusion.enabled,
            "Disabled SSAO reached the DirectX 12 lighting state");
        setOcclusionEnabled(true);
        const auto withOcclusion = captureComposition();
        Require(
            graphics.Lighting().screenAmbientOcclusion.enabled,
            "The DirectX 12 scene did not apply its resolved SSAO");

        std::size_t darkenedPixels{};
        bool brightened{};
        for (std::size_t offset{};
             offset + 3u < withOcclusion.size();
             offset += 4u)
        {
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                brightened = brightened
                    || withOcclusion[offset + channel]
                        > withoutOcclusion[offset + channel] + 1u;
            }
            if (withOcclusion[offset + 1u] + 6u
                < withoutOcclusion[offset + 1u])
            {
                ++darkenedPixels;
            }
        }
        Require(
            darkenedPixels > 40u
                && darkenedPixels
                    < static_cast<std::size_t>(CanvasWidth) * CanvasHeight
                        / 4u
                && !brightened,
            "DirectX 12 SSAO did not darken only the ambient contact "
            "region ("
                + std::to_string(darkenedPixels)
                + " darkened pixels)");

        setOcclusionEnabled(false);
        Require(
            captureComposition() == withoutOcclusion,
            "Disabling DirectX 12 SSAO did not restore the original frame");

        scene.SetAmbientLightIntensity(0.0f);
        auto& lightObject = scene.CreateGameObject("Sun");
        lightObject.AddComponent<LamaPon::DirectionalLightComponent>();
        const auto directOnly = captureComposition();
        setOcclusionEnabled(true);
        const auto directOnlyWithOcclusion = captureComposition();
        std::size_t litPixels{};
        for (std::size_t offset{};
             offset + 3u < directOnly.size();
             offset += 4u)
        {
            if (directOnly[offset + 1u] > 40u)
            {
                ++litPixels;
            }
        }
        Require(
            litPixels > 300u && directOnlyWithOcclusion == directOnly,
            "DirectX 12 SSAO changed a scene without ambient light");
    }

    // 磨いた金属の床へ赤いCubeを置き、斜め上から見るSceneです。床の
    // 手前側の反射レイがCubeの前面へ当たります。環境光だけで照らすため、
    // D3D11とD3D12で同じ合成画像になります。
    void BuildScreenSpaceReflectionScene(LamaPon::Scene& scene)
    {
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 2.0f, 5.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.38f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // 床とCubeは頂点と添字を明示したProcedural Meshで作ります。
        // D3D11のGeometricPrimitiveとD3D12の基本形状は三角形の頂点順が
        // 異なり、SSRのように自分の面と深度を比べる判定では補間の丸めの
        // 差が面ごとの当たり外れになるためです。
        const auto addFace = [](
            std::vector<LamaPon::ProceduralMeshVertex>& vertices,
            std::vector<std::uint32_t>& indices,
            const DirectX::XMFLOAT3& normal,
            const std::array<DirectX::XMFLOAT3, 4>& corners)
        {
            // cornersは外から見た左下、左上、右上、右下です。D3D11が
            // 裏面として捨てないよう、画面上で時計回りの三角形にします。
            const auto first =
                static_cast<std::uint32_t>(vertices.size());
            for (const auto& corner : corners)
            {
                vertices.push_back({ corner, normal, { 0.0f, 0.0f } });
            }
            indices.insert(
                indices.end(),
                { first, first + 1u, first + 2u,
                    first, first + 2u, first + 3u });
        };

        std::vector<LamaPon::ProceduralMeshVertex> floorVertices;
        std::vector<std::uint32_t> floorIndices;
        addFace(floorVertices, floorIndices, { 0.0f, 1.0f, 0.0f }, { {
            { -4.0f, -1.2f, 4.0f }, { -4.0f, -1.2f, -4.0f },
            { 4.0f, -1.2f, -4.0f }, { 4.0f, -1.2f, 4.0f } } });
        auto& floor = scene.CreateGameObject("Floor");
        auto& floorMesh = floor.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.7f, 0.7f, 0.7f, 1.0f });
        floorMesh.SetProceduralMesh(floorVertices, floorIndices);
        floorMesh.SetMetallic(1.0f);
        floorMesh.SetRoughness(0.1f);

        constexpr float h = 0.5f;
        constexpr float y = -0.7f;
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        addFace(cubeVertices, cubeIndices, { 0.0f, 0.0f, 1.0f }, { {
            { -h, y - h, h }, { -h, y + h, h },
            { h, y + h, h }, { h, y - h, h } } });
        addFace(cubeVertices, cubeIndices, { 0.0f, 0.0f, -1.0f }, { {
            { h, y - h, -h }, { h, y + h, -h },
            { -h, y + h, -h }, { -h, y - h, -h } } });
        addFace(cubeVertices, cubeIndices, { 1.0f, 0.0f, 0.0f }, { {
            { h, y - h, h }, { h, y + h, h },
            { h, y + h, -h }, { h, y - h, -h } } });
        addFace(cubeVertices, cubeIndices, { -1.0f, 0.0f, 0.0f }, { {
            { -h, y - h, -h }, { -h, y + h, -h },
            { -h, y + h, h }, { -h, y - h, h } } });
        addFace(cubeVertices, cubeIndices, { 0.0f, 1.0f, 0.0f }, { {
            { -h, y + h, h }, { -h, y + h, -h },
            { h, y + h, -h }, { h, y + h, h } } });
        auto& cube = scene.CreateGameObject("Cube");
        auto& cubeMesh = cube.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.9f, 0.15f, 0.1f, 1.0f });
        cubeMesh.SetProceduralMesh(cubeVertices, cubeIndices);
        cubeMesh.SetMetallic(0.0f);
        cubeMesh.SetRoughness(0.8f);

        auto reflection = scene.ScreenSpaceReflection();
        reflection.enabled = true;
        scene.SetScreenSpaceReflectionSettings(reflection);
    }

    // 同じSceneをD3D11とD3D12で2フレーム描きます。SSRは前フレームの
    // HDRカラーを読むため、反射は2フレーム目の合成画像に映ります。
    [[nodiscard]] Capture RenderScreenSpaceReflectionCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The screen-space reflection capture did not start the "
            "requested rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        LamaPon::Scene scene(graphics);
        BuildScreenSpaceReflectionScene(scene);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        Capture capture;
        for (int frame{}; frame < 2; ++frame)
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            auto* const target = graphics.SceneCompositionTarget();
            Require(
                target != nullptr,
                "The screen-space reflection capture has no scene "
                "composition target");
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                target);
            if (frame == 1)
            {
                const auto& reflection =
                    graphics.Lighting().screenSpaceReflection;
                Require(
                    reflection.enabled
                        && reflection.texture
                            == target->ColorHistoryViewHandle()
                        && reflection.depth
                            == target->ReflectionDepthPyramidViewHandle()
                        && reflection.depthPyramidMaximumMip > 0u
                        && reflection.depthPyramidMaximumMip + 1u
                            == target->ReflectionDepthPyramidMipCount(),
                    "The scene did not resolve SSR from its color history "
                    "and depth pyramid");
            }
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            capture.pixels = graphics.CaptureBackBuffer(
                capture.width,
                capture.height);
            graphics.EndFrame();
        }
        return capture;
    }

    // depth-probeのScreenEffectをProcedural MeshのSceneへ掛けます。
    // 距離をR／Gへ書くため、D3D11と同じ深度（t3）と射影の係数（b0）を
    // 渡していれば合成画像が一致します。
    [[nodiscard]] Capture RenderScreenEffectDepthCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The screen effect depth capture did not start the requested "
            "rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        LamaPon::Scene scene(graphics);
        BuildScreenSpaceReflectionScene(scene);
        auto reflection = scene.ScreenSpaceReflection();
        reflection.enabled = false;
        scene.SetScreenSpaceReflectionSettings(reflection);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            graphics.SceneCompositionTarget());
        LamaPon::ScreenEffectRequest request;
        request.shader =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/depth-probe.hlsl";
        // 距離を12mで割った値を書きます。
        request.customParameters[0] = { 12.0f, 0.0f, 0.0f, 0.0f };
        request.point = LamaPon::ScreenEffectPoint::AfterToneMapping;
        std::uint64_t generation{};
        std::string error;
        Require(
            graphics.QueueScreenEffect(request, &generation, &error)
                && generation != 0
                && error.empty(),
            "The depth screen effect was not queued: " + error);
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        // 深度が欠けるとShaderは青を返します。Cubeと床と背景で距離が
        // 違うため、赤（距離の上位）は画面内で大きく変わります。
        std::size_t missingDepthPixels{};
        int minimumRed = 255;
        int maximumRed = 0;
        for (std::size_t offset{};
            offset + 3u < capture.pixels.size();
            offset += 4u)
        {
            if (capture.pixels[offset + 2u] > 128u)
            {
                ++missingDepthPixels;
            }
            minimumRed = std::min(
                minimumRed,
                static_cast<int>(capture.pixels[offset]));
            maximumRed = std::max(
                maximumRed,
                static_cast<int>(capture.pixels[offset]));
        }
        Require(
            capture.width == CanvasWidth
                && capture.height == CanvasHeight
                && missingDepthPixels == 0u
                && maximumRed > minimumRed + 40,
            "The screen effect did not read the scene depth ("
                + std::to_string(missingDepthPixels) + " blue pixels, red "
                + std::to_string(minimumRed) + "-"
                + std::to_string(maximumRed) + ")");
        return capture;
    }

    // auxiliary-probeのScreenEffectで、補助texture（t1／t2）が決まった
    // registerへ届き、指定しなかった側がD3D11と同じく白になることを
    // 確かめます。
    [[nodiscard]] Capture RenderScreenEffectAuxiliaryCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The screen effect texture capture did not start the requested "
            "rendering API");
        // Environment shaderと補助textureはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);

        constexpr float clearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        LamaPon::ScreenEffectRequest request;
        request.shader =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/auxiliary-probe.hlsl";
        request.auxiliaryTextures[0] = "textures/LamaPonLogo.png";
        request.customParameters[0] = { 0.8f, 0.0f, 0.0f, 0.0f };
        request.point = LamaPon::ScreenEffectPoint::AfterToneMapping;
        std::uint64_t generation{};
        std::string error;
        Require(
            graphics.QueueScreenEffect(request, &generation, &error)
                && generation != 0
                && error.empty(),
            "The texture screen effect was not queued: " + error);
        LamaPon::PostProcessFrame frame;
        graphics.EndSceneComposition(frame);
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        // 右半分はt2を指定していないため、白へ倍率0.8を掛けた色です。
        const std::size_t rightCenter =
            (static_cast<std::size_t>(CanvasHeight / 2u) * CanvasWidth
                + CanvasWidth * 3u / 4u) * 4u;
        bool whiteFallback = capture.pixels.size() > rightCenter + 3u;
        for (std::size_t channel{}; whiteFallback && channel < 3u; ++channel)
        {
            whiteFallback =
                std::abs(
                    static_cast<int>(capture.pixels[rightCenter + channel])
                    - 204) <= 2;
        }
        // 左半分はLogo画像を引き伸ばすため、一様な色になりません。
        int minimumLeft = 255;
        int maximumLeft = 0;
        for (std::uint32_t y{}; y < CanvasHeight; ++y)
        {
            for (std::uint32_t x{}; x < CanvasWidth / 2u; ++x)
            {
                const auto offset =
                    (static_cast<std::size_t>(y) * CanvasWidth + x) * 4u;
                if (offset + 3u >= capture.pixels.size())
                {
                    continue;
                }
                const int luminance = capture.pixels[offset]
                    + capture.pixels[offset + 1u]
                    + capture.pixels[offset + 2u];
                minimumLeft = std::min(minimumLeft, luminance);
                maximumLeft = std::max(maximumLeft, luminance);
            }
        }
        Require(
            capture.width == CanvasWidth
                && capture.height == CanvasHeight
                && whiteFallback
                && maximumLeft > minimumLeft + 60,
            "The screen effect did not bind its auxiliary textures (left "
                + std::to_string(minimumLeft) + "-"
                + std::to_string(maximumLeft) + ")");
        return capture;
    }

    // compileに失敗するScreenEffect／ComputeEffectと存在しないShader、
    // 寸法の無いCompute要求は、D3D11と同じく説明付きで拒否し、描画
    // キューへ積みません。
    void RequireD3D12CustomShaderFailures()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        LamaPon::ScreenEffectRequest request;
        request.shader = fixtures / "broken-shader.hlsl";
        std::uint64_t generation{ 1 };
        std::string error;
        Require(
            !graphics.QueueScreenEffect(request, &generation, &error)
                && generation == 0
                && !error.empty(),
            "The DirectX 12 screen effect accepted a shader that does not "
            "compile");

        request.shader = fixtures / "missing-screen-effect.hlsl";
        error.clear();
        Require(
            !graphics.QueueScreenEffect(request, &generation, &error)
                && error.find("not found") != std::string::npos,
            "The DirectX 12 screen effect did not report a missing shader: "
                + error);

        LamaPon::ComputeEffectRequest compute;
        compute.shader = fixtures / "broken-shader.hlsl";
        compute.outputTexture = "brokenCompute";
        compute.outputWidth = 16;
        compute.outputHeight = 16;
        error.clear();
        Require(
            !graphics.DispatchComputeEffect(compute, &error)
                && !error.empty(),
            "The DirectX 12 compute effect accepted a shader that does not "
            "compile");
        compute.shader = fixtures / "compute-probe.hlsl";
        compute.outputWidth = 0;
        error.clear();
        Require(
            !graphics.DispatchComputeEffect(compute, &error)
                && !error.empty(),
            "The DirectX 12 compute effect accepted an empty output size");

        // 失敗した要求は積まれていないため、合成しても何も起きません。
        constexpr float clearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        LamaPon::PostProcessFrame frame;
        graphics.EndSceneComposition(frame);
        graphics.EndFrame();
    }

    // ComputeEffectを8で割り切れない寸法の名前付きRenderTextureへ書き、
    // その表示用textureをSpriteで画面全体へ引き伸ばします。D3D11と同じ
    // CSMain、b0、t0／t1、s0、u0と切り上げたthread group数で書いていれば
    // 合成画像が一致します。readInputsはcompute-input-probeでLogo画像を
    // t0へ渡し、falseはcompute-probeの模様を書きます。
    [[nodiscard]] Capture RenderComputeEffectCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile,
        const bool readInputs)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The compute effect capture did not start the requested "
            "rendering API");
        // 入力textureはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        LamaPon::ComputeEffectRequest request;
        request.shader = fixtures
            / (readInputs ? "compute-input-probe.hlsl" : "compute-probe.hlsl");
        request.outputTexture =
            readInputs ? "computeInputProbe" : "computeProbe";
        request.outputWidth = 100;
        request.outputHeight = 60;
        request.customParameters[0] = { 0.75f, 0.0f, 0.0f, 0.0f };
        if (readInputs)
        {
            request.inputTextures[0] = "textures/LamaPonLogo.png";
        }

        constexpr float clearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        graphics.BeginFrame(clearColor);
        std::string error;
        Require(
            graphics.DispatchComputeEffect(request, &error) && error.empty(),
            "The compute effect did not run: " + error);
        const auto output =
            graphics.RenderTextureViewHandle(request.outputTexture);
        Require(
            static_cast<bool>(output),
            "The compute effect output texture is unavailable");
        {
            LamaPon::SpritePassDescription description;
            description.blend = LamaPon::SpriteBlendMode::Opaque;
            auto pass = graphics.BeginSpritePass(description);
            LamaPon::SpriteDrawRequest sprite;
            sprite.texture = output;
            sprite.scale = {
                static_cast<float>(CanvasWidth) / request.outputWidth,
                static_cast<float>(CanvasHeight) / request.outputHeight };
            Require(pass.Draw(sprite), "The compute output sprite was rejected");
        }
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();
        Require(
            capture.width == CanvasWidth
                && capture.height == CanvasHeight
                && capture.pixels.size()
                    == static_cast<std::size_t>(CanvasWidth)
                        * CanvasHeight * 4u,
            "The compute effect capture has unexpected dimensions");

        const auto pixel = [&capture](
            const std::uint32_t x,
            const std::uint32_t y)
        {
            const auto offset =
                (static_cast<std::size_t>(y) * CanvasWidth + x) * 4u;
            return std::array<int, 3>{
                capture.pixels[offset],
                capture.pixels[offset + 1u],
                capture.pixels[offset + 2u] };
        };
        const auto describe = [](const std::array<int, 3>& color)
        {
            return std::to_string(color[0]) + ", "
                + std::to_string(color[1]) + ", "
                + std::to_string(color[2]);
        };
        if (readInputs)
        {
            // 右半分はt1を指定していないため、白へ倍率0.75を掛けた色です。
            const auto right = pixel(CanvasWidth * 3u / 4u, CanvasHeight / 2u);
            bool whiteFallback = true;
            for (const int channel : right)
            {
                whiteFallback = whiteFallback && std::abs(channel - 191) <= 2;
            }
            // 左半分はLogo画像を引き伸ばすため、一様な色になりません。
            int minimumLeft = 765;
            int maximumLeft = 0;
            for (std::uint32_t y{}; y < CanvasHeight; ++y)
            {
                for (std::uint32_t x{}; x < CanvasWidth / 2u - 2u; ++x)
                {
                    const auto color = pixel(x, y);
                    const int luminance = color[0] + color[1] + color[2];
                    minimumLeft = std::min(minimumLeft, luminance);
                    maximumLeft = std::max(maximumLeft, luminance);
                }
            }
            Require(
                whiteFallback && maximumLeft > minimumLeft + 60,
                "The compute effect did not read its input textures (right "
                    + describe(right) + ", left "
                    + std::to_string(minimumLeft) + "-"
                    + std::to_string(maximumLeft) + ")");
        }
        else
        {
            // 左半分は赤0.75、右半分は上から下への緑のグラデーションです。
            const auto left = pixel(CanvasWidth / 4u, CanvasHeight / 2u);
            const auto top = pixel(CanvasWidth * 3u / 4u, 4u);
            const auto bottom = pixel(CanvasWidth * 3u / 4u, CanvasHeight - 5u);
            Require(
                std::abs(left[0] - 191) <= 2
                    && left[1] <= 2
                    && left[2] <= 2
                    && top[0] <= 2
                    && top[1] < 40
                    && bottom[1] > 215,
                "The compute effect did not write the probe pattern (left "
                    + describe(left) + ", top " + describe(top)
                    + ", bottom " + describe(bottom) + ")");
        }
        return capture;
    }

    // 1辺1の6面Procedural Cubeです。D3D11のGeometricPrimitiveとD3D12の
    // 組み込み形状は三角形の頂点順が異なるため、API間の画素比較には
    // 頂点と添字を明示した形を使います。
    void BuildProceduralCube(
        std::vector<LamaPon::ProceduralMeshVertex>& vertices,
        std::vector<std::uint32_t>& indices)
    {
        const auto addFace = [&vertices, &indices](
            const DirectX::XMFLOAT3& normal,
            const std::array<DirectX::XMFLOAT3, 4>& corners)
        {
            // cornersは外から見た左下、左上、右上、右下で、D3D11が裏面として
            // 捨てないよう画面上で時計回りの三角形にします。
            const auto first = static_cast<std::uint32_t>(vertices.size());
            const std::array<DirectX::XMFLOAT2, 4> textureCoordinates{ {
                { 0.0f, 1.0f },
                { 0.0f, 0.0f },
                { 1.0f, 0.0f },
                { 1.0f, 1.0f } } };
            for (std::size_t corner{}; corner < corners.size(); ++corner)
            {
                vertices.push_back({
                    corners[corner],
                    normal,
                    textureCoordinates[corner] });
            }
            indices.insert(
                indices.end(),
                { first, first + 1u, first + 2u,
                    first, first + 2u, first + 3u });
        };
        constexpr float h = 0.5f;
        addFace({ 0.0f, 0.0f, 1.0f }, { {
            { -h, -h, h }, { -h, h, h }, { h, h, h }, { h, -h, h } } });
        addFace({ 0.0f, 0.0f, -1.0f }, { {
            { h, -h, -h }, { h, h, -h }, { -h, h, -h }, { -h, -h, -h } } });
        addFace({ 1.0f, 0.0f, 0.0f }, { {
            { h, -h, h }, { h, h, h }, { h, h, -h }, { h, -h, -h } } });
        addFace({ -1.0f, 0.0f, 0.0f }, { {
            { -h, -h, -h }, { -h, h, -h }, { -h, h, h }, { -h, -h, h } } });
        addFace({ 0.0f, 1.0f, 0.0f }, { {
            { -h, h, h }, { -h, h, -h }, { h, h, -h }, { h, h, h } } });
        addFace({ 0.0f, -1.0f, 0.0f }, { {
            { -h, -h, -h }, { -h, -h, h }, { h, -h, h }, { h, -h, -h } } });
    }

    struct MaterialShaderCapture final
    {
        Capture baseline;
        Capture frame;
        std::array<std::string, 4> errors;
    };

    // Mesh RendererのMaterial custom shaderを4つのProcedural Cubeで描きます。
    // 雛形LamaPonCustomMaterial（t0とb0のカスタム値）、b1の光源・b3・t7・
    // 点サンプリング・半透明の宣言を読むmaterial-lighting-probe、keyword付きの
    // variant-probe、compileに失敗してマゼンタの代替表示になるbroken-shader
    // です。深度パス（影）も同じShaderの頂点シェーダーで描きます。
    [[nodiscard]] MaterialShaderCapture RenderMaterialShaderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The material shader capture did not start the requested "
            "rendering API");
        // D3D11のLit / Environment shaderと雛形はasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // D3D11の非同期compileは最初のフレームを標準Litで描くため止めます。
        graphics.SetAsyncShaderCompilationEnabled(false);

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.5f, 5.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.08f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.35f);
        auto& sun = scene.CreateGameObject("Sun");
        sun.GetTransform().SetEulerAngles(0.8f, -0.5f, 0.0f);
        sun.AddComponent<LamaPon::DirectionalLightComponent>();
        auto& bulb = scene.CreateGameObject("PointLight");
        bulb.GetTransform().position = { 0.0f, 1.2f, 1.6f };
        bulb.AddComponent<LamaPon::PointLightComponent>(
            DirectX::XMFLOAT3{ 1.0f, 0.8f, 0.6f },
            3.0f,
            2.0f);

        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        const auto addCube = [&](
            const char* const name,
            const float x,
            const DirectX::XMFLOAT4& color,
            std::filesystem::path albedo) -> LamaPon::MeshRendererComponent&
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.2f, 1.2f, 1.2f };
            object.GetTransform().SetEulerAngles(0.5f, 0.7f, 0.0f);
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                color,
                std::move(albedo));
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
            return mesh;
        };

        auto& templateMesh = addCube(
            "TemplateMaterial",
            -2.25f,
            { 1.0f, 1.0f, 1.0f, 1.0f },
            "textures/LamaPonLogo.png");
        templateMesh.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        templateMesh.SetCustomParameter(0, { 1.0f, 0.55f, 0.2f, 0.5f });
        templateMesh.SetCustomParameter(1, { 0.3f, 0.9f, 0.0f, 0.0f });
        templateMesh.SetCustomParameter(2, { 1.0f, 1.0f, 0.0f, 0.0f });

        auto& lightingMesh = addCube(
            "LightingProbe",
            -0.75f,
            { 0.9f, 0.9f, 0.9f, 0.7f },
            "textures/LamaPonLogo.png");
        lightingMesh.SetShaderPath(fixtures / "material-lighting-probe.hlsl");
        lightingMesh.SetCustomTexturePath(0, "textures/particle-glow.png");
        lightingMesh.SetCustomVector(0, { 0.05f, 0.1f, 0.0f, 0.0f });
        lightingMesh.SetCustomParameter(7, { 0.0f, 0.0f, 0.0f, 1.0f });

        auto& variantMesh = addCube(
            "VariantProbe",
            0.75f,
            { 1.0f, 1.0f, 1.0f, 1.0f },
            {});
        variantMesh.SetShaderPath(fixtures / "variant-probe.hlsl");
        variantMesh.EnableShaderKeyword("VARIANT_PROBE_GREEN");
        variantMesh.EnableShaderKeyword("VARIANT_PROBE_BRIGHT");

        auto& brokenMesh = addCube(
            "BrokenShader",
            2.25f,
            { 1.0f, 1.0f, 1.0f, 1.0f },
            {});
        brokenMesh.SetShaderPath(fixtures / "broken-shader.hlsl");

        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        MaterialShaderCapture capture;
        capture.frame.pixels = graphics.CaptureBackBuffer(
            capture.frame.width,
            capture.frame.height);
        graphics.EndFrame();
        capture.errors = {
            templateMesh.ShaderError(),
            lightingMesh.ShaderError(),
            variantMesh.ShaderError(),
            brokenMesh.ShaderError() };

        // keyword付きのvariantは緑、compileに失敗したShaderはマゼンタで
        // 描かれます。
        std::size_t magentaPixels{};
        std::size_t greenPixels{};
        for (std::size_t offset{};
            offset + 3u < capture.frame.pixels.size();
            offset += 4u)
        {
            const int red = capture.frame.pixels[offset];
            const int green = capture.frame.pixels[offset + 1u];
            const int blue = capture.frame.pixels[offset + 2u];
            if (red > 150 && blue > 150 && green < 80)
            {
                ++magentaPixels;
            }
            if (green > red + 60 && green > blue + 60)
            {
                ++greenPixels;
            }
        }
        Require(
            capture.frame.width == CanvasWidth
                && capture.frame.height == CanvasHeight
                && capture.errors[0].empty()
                && capture.errors[1].empty()
                && capture.errors[2].empty()
                && !capture.errors[3].empty()
                && magentaPixels > 30u
                && greenPixels > 30u,
            std::string("The material shaders did not draw as expected on ")
                + (api == LamaPon::RenderingApi::DirectX11
                    ? "DirectX 11"
                    : "DirectX 12")
                + " (" + std::to_string(magentaPixels) + " magenta, "
                + std::to_string(greenPixels) + " green pixels; errors: ["
                + capture.errors[0] + "] [" + capture.errors[1] + "] ["
                + capture.errors[2] + "])");
        return capture;
    }

    // FBX（AnimatedSausage）のModel RendererへMaterial custom shaderを
    // 割り当てて描きます。D3D11はDirectXTK SkinnedEffectの頂点シェーダーと
    // PSSkinnedMain、D3D12は同じ計算の内蔵頂点シェーダーとPSSkinnedMainで、
    // 骨を動かした姿勢、Material上書きのcustom値、VSSkinnedOutlineの輪郭、
    // PSSkinnedOccludedの遮蔽表示、compileに失敗したShaderのマゼンタ表示を
    // 比べます。
    [[nodiscard]] MaterialShaderCapture RenderSkinnedMaterialShaderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The skinned material shader capture did not start the requested "
            "rendering API");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        graphics.SetAsyncShaderCompilationEnabled(false);
        const auto templateShader = graphics.Assets().ResolvePath(
            "shaders/LamaPonCustomMaterial.hlsl");
        static_cast<void>(LamaPon::CompileShaderCached(
            graphics.Assets(),
            templateShader,
            "VSSkinnedOutline",
            "vs_5_0"));
        static_cast<void>(LamaPon::CompileShaderCached(
            graphics.Assets(),
            templateShader,
            "PSOutline",
            "ps_5_0"));
        static_cast<void>(LamaPon::CompileShaderCached(
            graphics.Assets(),
            templateShader,
            "PSSkinnedOccluded",
            "ps_5_0"));
        if (api == LamaPon::RenderingApi::DirectX11)
        {
            auto& states = LamaPon::Detail::GraphicsDeviceD3D11Access::States(
                graphics);
            D3D11_DEPTH_STENCIL_DESC depthDescription{};
            states.DepthDefault()->GetDesc(&depthDescription);
            D3D11_RASTERIZER_DESC rasterizerDescription{};
            states.CullCounterClockwise()->GetDesc(&rasterizerDescription);
            Require(
                depthDescription.DepthEnable
                    && depthDescription.DepthWriteMask
                        == D3D11_DEPTH_WRITE_MASK_ALL
                    && depthDescription.DepthFunc
                        == D3D11_COMPARISON_LESS_EQUAL
                    && rasterizerDescription.CullMode == D3D11_CULL_BACK
                    && !rasterizerDescription.FrontCounterClockwise,
                "The DirectXTK outline states no longer match the DirectX 12 "
                "material pipeline assumptions");
            std::uint64_t generation{};
            std::string error;
            auto* const effect = graphics.SkinnedMaterialShader(
                "shaders/LamaPonCustomMaterial.hlsl",
                generation,
                error);
            Require(
                effect != nullptr
                    && effect->HasOutline()
                    && effect->HasOccludedPass(),
                "The DirectX 11 skinned material shader did not expose its "
                "outline and occluded passes: " + error);
        }

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 8.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.9f);

        const auto modelPath = std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "AnimatedSausage.fbx";
        const auto modelAsset = graphics.Assets().LoadModel(modelPath);
        Require(
            modelAsset != nullptr
                && modelAsset->skeletalModel != nullptr
                && modelAsset->skeletalModel->hasLocalBounds,
            "The skinned material test model could not be loaded");
        const auto& bounds = modelAsset->skeletalModel->localBounds;
        const DirectX::XMFLOAT3 modelCenter{
            (bounds.minimum.x + bounds.maximum.x) * 0.5f,
            (bounds.minimum.y + bounds.maximum.y) * 0.5f,
            (bounds.minimum.z + bounds.maximum.z) * 0.5f };
        const float modelExtent = std::max({
            bounds.maximum.x - bounds.minimum.x,
            bounds.maximum.y - bounds.minimum.y,
            bounds.maximum.z - bounds.minimum.z,
            0.001f });
        const float modelScale = 2.5f / modelExtent;
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        const auto addModel = [&](
            const char* const name,
            const float x) -> LamaPon::ModelRendererComponent&
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = {
                x - modelCenter.x * modelScale,
                -modelCenter.y * modelScale,
                -modelCenter.z * modelScale };
            object.GetTransform().scale = {
                modelScale,
                modelScale,
                modelScale };
            auto& model =
                object.AddComponent<LamaPon::ModelRendererComponent>(modelPath);
            model.SetAnimationPlayOnStart(false);
            return model;
        };
        // モデルより先に描いて深度を作り、PSSkinnedOccludedが奥だけを
        // 通ることを実画素で確認します。
        auto& blocker = scene.CreateGameObject("SkinnedOccluder");
        blocker.GetTransform().position = { -2.5f, 0.0f, 1.5f };
        blocker.GetTransform().scale = { 0.35f, 0.35f, 0.35f };
        blocker.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.18f, 0.2f, 0.24f, 1.0f });
        auto& templateModel = addModel("TemplateModel", -2.5f);
        templateModel.SetMaterialOverrideEnabled(true);
        templateModel.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        templateModel.SetCustomParameter(0, { 1.0f, 0.5f, 0.2f, 0.6f });
        templateModel.SetCustomParameter(1, { 0.2f, 1.2f, 0.0f, 0.0f });
        templateModel.SetCustomParameter(2, { 1.0f, 1.0f, 0.0f, 0.0f });
        // まず追加passを無効にした画像を基準にし、その後だけ輪郭と遮蔽を
        // 有効にします。通常のskinned描画のAPI間一致と、D3D12の追加passが
        // 実画素を描くことを独立に検証できます。
        templateModel.SetCustomParameter(3, { 0.0f, 1.0f, 0.9f, 0.05f });
        templateModel.SetCustomParameter(4, { 0.05f, 0.85f, 1.0f, 0.0f });
        auto& brokenModel = addModel("BrokenModel", 2.5f);
        brokenModel.SetShaderPath(fixtures / "broken-shader.hlsl");

        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 最初のフレームでModelを読み込み、骨を動かした姿勢で描き直します。
        static_cast<void>(render());
        templateModel.SetAnimationTime(templateModel.AnimationDuration() * 0.5f);
        brokenModel.SetAnimationTime(brokenModel.AnimationDuration() * 0.5f);
        MaterialShaderCapture capture;
        capture.baseline = render();
        templateModel.SetCustomParameter(3, { 0.12f, 1.0f, 0.9f, 0.05f });
        templateModel.SetCustomParameter(4, { 0.05f, 0.85f, 1.0f, 1.0f });
        capture.frame = render();
        capture.errors = {
            templateModel.ShaderError(),
            {},
            {},
            brokenModel.ShaderError() };

        const std::array<int, 3> background{
            capture.frame.pixels[0],
            capture.frame.pixels[1],
            capture.frame.pixels[2] };
        std::size_t magentaPixels{};
        std::size_t shadedPixels{};
        std::size_t outlinePixels{};
        std::size_t occludedPixels{};
        for (std::size_t offset{};
            offset + 3u < capture.frame.pixels.size();
            offset += 4u)
        {
            const int red = capture.frame.pixels[offset];
            const int green = capture.frame.pixels[offset + 1u];
            const int blue = capture.frame.pixels[offset + 2u];
            if (red > 150 && blue > 150 && green < 80)
            {
                ++magentaPixels;
            }
            if (red > blue + 40 && green > blue + 40)
            {
                ++outlinePixels;
            }
            if (green > red + 40 && blue > red + 40)
            {
                ++occludedPixels;
            }
            else if (std::abs(red - background[0]) > 12
                || std::abs(green - background[1]) > 12
                || std::abs(blue - background[2]) > 12)
            {
                ++shadedPixels;
            }
        }
        Require(
            capture.frame.width == CanvasWidth
                && capture.frame.height == CanvasHeight
                && capture.errors[0].empty()
                && !capture.errors[3].empty()
                && magentaPixels > 30u
                && shadedPixels > 30u
                && (api == LamaPon::RenderingApi::DirectX11
                    || (outlinePixels > 20u && occludedPixels > 20u)),
            std::string("The skinned material shaders did not draw as expected on ")
                + (api == LamaPon::RenderingApi::DirectX11
                    ? "DirectX 11"
                    : "DirectX 12")
                + " (" + std::to_string(magentaPixels) + " magenta, "
                + std::to_string(shadedPixels) + " shaded, "
                + std::to_string(outlinePixels) + " outline, "
                + std::to_string(occludedPixels)
                + " occluded pixels; error: ["
                + capture.errors[0] + "])");
        return capture;
    }

    // VSInstancedMainを持つ同じMaterialのCubeを2個まとめ、D3D11とD3D12が
    // どちらも1回のinstance batchで同じ画像を描くことを確認します。非一様
    // スケールにして、まとめない描画と同じ画像になることも確かめます。
    [[nodiscard]] Capture RenderMaterialShaderInstancingCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The material shader instancing capture did not start the "
            "requested rendering API");
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        graphics.SetAsyncShaderCompilationEnabled(false);
        if (api == LamaPon::RenderingApi::DirectX11)
        {
            std::uint64_t generation{};
            std::string error;
            auto& effect = graphics.MaterialShader(
                "shaders/LamaPonCustomMaterial.hlsl",
                generation,
                error);
            Require(
                error.empty() && effect.SupportsInstancing(),
                "The DirectX 11 template material did not compile "
                "VSInstancedMain: " + error);
        }

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.2f, 6.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        const auto addCube = [&](const char* const name, const float x)
            -> LamaPon::MeshRendererComponent&
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.3f, 0.8f, 1.1f };
            object.GetTransform().SetEulerAngles(0.35f, 0.55f, 0.0f);
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.9f, 0.8f, 0.6f, 1.0f });
            mesh.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
            mesh.SetCustomParameter(0, { 0.3f, 0.8f, 1.0f, 0.55f });
            mesh.SetCustomParameter(1, { 0.15f, 0.0f, 0.0f, 0.0f });
            return mesh;
        };
        auto& left = addCube("InstancedLeft", -1.4f);
        auto& right = addCube("InstancedRight", 1.4f);

        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 初回描画でComponentとShader cacheを用意し、次のフレームでSceneの
        // batch収集へ入る状態にします。
        static_cast<void>(render());
        const auto capture = render();

        const auto& stats = scene.VisibilityStats();
        Require(
            stats.meshInstanceBatchCount == 1u
                && stats.meshInstancedRendererCount == 2u,
            "The material shader instance batch was not used on " + apiName
                + " (eligible " + std::to_string(left.CanBeInstanced())
                + "/" + std::to_string(right.CanBeInstanced())
                + ", keys " + std::to_string(left.InstanceBatchKey())
                + "/" + std::to_string(right.InstanceBatchKey()) + ")");
        const auto errors = left.ShaderError() + right.ShaderError();
        Require(
            errors.empty(),
            apiName + " reported a material shader instancing error: "
                + errors);

        // テンプレートが読まないcustom vectorだけを変えるとbatch keyが分かれ、
        // 見た目を変えずに1個ずつの描画になります。
        right.SetCustomVector(7, { 1.0f, 0.0f, 0.0f, 0.0f });
        const auto individual = render();
        Require(
            scene.VisibilityStats().meshInstanceBatchCount == 0u,
            "The material shader instancing capture could not split the "
            "batch on " + apiName);
        std::size_t differentPixels{};
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size()
                 && offset + 3u < individual.pixels.size();
             offset += 4u)
        {
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                if (std::abs(
                        static_cast<int>(capture.pixels[offset + channel])
                        - static_cast<int>(
                            individual.pixels[offset + channel])) > 2)
                {
                    ++differentPixels;
                    break;
                }
            }
        }
        Require(
            differentPixels == 0u,
            apiName + " drew the material shader instance batch differently "
                "from individual draws in "
                + std::to_string(differentPixels) + " pixels");
        return capture;
    }

    // 組み込みLitのインスタンス描画をD3D11と比べます。色と非一様スケールの
    // 違うCube 2個はMesh Rendererの1 batchに、アニメーションの無いglTFの箱
    // 2個はModel Rendererの1 batchになり、D3D11のLamaPonLit.hlslの
    // VSInstancedMainと同じく、instanceの色とworldで変換した法線で描きます。
    [[nodiscard]] Capture RenderBuiltInInstancingCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // アニメーションもskinも無い、色付きの箱です（bufferはbase64で内蔵）。
        const auto staticModelPath =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/static-box.gltf";
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The built-in instancing capture did not start the requested "
            "rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.3f, 6.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.25f);
        // 法線の違いが明暗に出るよう、影の無い斜めの光を当てます。
        auto& sunObject = scene.CreateGameObject("Sun");
        sunObject.GetTransform().SetEulerAngles(-0.6f, 0.35f, 0.0f);
        auto& sun = sunObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        sun.SetCastsShadows(false);

        const auto addCube = [&](
            const char* const name,
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& scale,
            const DirectX::XMFLOAT4& color)
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = position;
            object.GetTransform().scale = scale;
            object.GetTransform().SetEulerAngles(0.5f, 0.7f, 0.0f);
            static_cast<void>(
                object.AddComponent<LamaPon::MeshRendererComponent>(
                    LamaPon::PrimitiveShape::Cube,
                    color));
        };
        addCube(
            "LitCubeA",
            { -2.3f, 0.7f, 0.0f },
            { 1.2f, 0.6f, 0.9f },
            { 0.9f, 0.4f, 0.3f, 1.0f });
        addCube(
            "LitCubeB",
            { -0.8f, -0.6f, 0.0f },
            { 0.7f, 1.1f, 0.8f },
            { 0.3f, 0.6f, 0.9f, 1.0f });
        const auto addModel = [&](
            const char* const name,
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& scale)
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = position;
            object.GetTransform().scale = scale;
            object.GetTransform().SetEulerAngles(0.4f, 0.6f, 0.0f);
            static_cast<void>(
                object.AddComponent<LamaPon::ModelRendererComponent>(
                    staticModelPath));
        };
        addModel("StaticBoxA", { 0.9f, 0.7f, 0.0f }, { 1.3f, 0.7f, 1.0f });
        addModel("StaticBoxB", { 2.4f, -0.6f, 0.0f }, { 0.8f, 1.2f, 0.9f });

        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 最初のフレームでComponentとModelを用意し、次のフレームでSceneの
        // batch収集へ入れます。
        static_cast<void>(render());
        const auto capture = render();

        const auto& stats = scene.VisibilityStats();
        Require(
            stats.meshInstanceBatchCount == 1u
                && stats.meshInstancedRendererCount == 2u
                && stats.modelInstanceBatchCount == 1u
                && stats.modelInstancedRendererCount == 2u,
            apiName + " did not batch the built-in Lit renderers ("
                + std::to_string(stats.meshInstanceBatchCount) + " mesh / "
                + std::to_string(stats.modelInstanceBatchCount)
                + " model batches)");

        const std::array<int, 3> background{
            capture.pixels[0],
            capture.pixels[1],
            capture.pixels[2] };
        std::size_t leftPixels{};
        std::size_t rightPixels{};
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            int difference{};
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                difference = std::max(
                    difference,
                    std::abs(
                        static_cast<int>(capture.pixels[offset + channel])
                        - background[channel]));
            }
            if (difference <= 12)
            {
                continue;
            }
            if ((offset / 4u) % capture.width < capture.width / 2u)
            {
                ++leftPixels;
            }
            else
            {
                ++rightPixels;
            }
        }
        Require(
            leftPixels > 300u && rightPixels > 300u,
            apiName + " did not draw the batched built-in Lit renderers ("
                + std::to_string(leftPixels) + " mesh, "
                + std::to_string(rightPixels) + " model pixels)");
        return capture;
    }

    // ParticleSystemのcustom pixel shaderをD3D11と比べます。左は既定の
    // particle、中央は頂点色・UV・t0／t1・カスタム値を読むparticle-probe、
    // 右はcompileに失敗したShaderのマゼンタの代替表示です。
    [[nodiscard]] Capture RenderCustomParticleShaderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The custom particle shader capture did not start the requested "
            "rendering API");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        const auto addParticles = [&](
            const char* const name,
            const float x) -> LamaPon::ParticleSystemComponent&
        {
            auto& object = scene.CreateGameObject(name);
            auto& particles = object.AddComponent<
                LamaPon::ParticleSystemComponent>();
            particles.SetPlayOnStart(false);
            particles.SetAdditive(false);
            particles.SetStartColor({ 0.9f, 0.75f, 0.4f, 1.0f });
            particles.SetEndColor({ 0.9f, 0.75f, 0.4f, 1.0f });
            particles.EmitParticle(
                { x, 0.0f, 0.0f },
                { 0.0f, 0.0f, 0.0f },
                1.0f,
                1.1f);
            return particles;
        };
        static_cast<void>(addParticles("DefaultParticles", -1.4f));
        auto& probe = addParticles("ProbeParticles", 0.0f);
        probe.SetShaderPath(fixtures / "particle-probe.hlsl");
        probe.SetAuxiliaryTexturePath("textures/LamaPonLogo.png");
        probe.SetCustomParameter(0, { 0.35f, 1.0f, 0.6f, 0.0f });
        auto& broken = addParticles("BrokenParticles", 1.4f);
        broken.SetShaderPath(fixtures / "broken-shader.hlsl");

        constexpr float clearColor[4]{ 0.05f, 0.05f, 0.08f, 1.0f };
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 最初のフレームでtextureとShaderを読み込みます。
        static_cast<void>(render());
        const auto capture = render();
        const std::string probeError(probe.ShaderError());
        const std::string brokenError(broken.ShaderError());

        std::size_t magentaPixels{};
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            if (capture.pixels[offset] > 150u
                && capture.pixels[offset + 2u] > 150u
                && capture.pixels[offset + 1u] < 80u)
            {
                ++magentaPixels;
            }
        }
        Require(
            probeError.empty()
                && !brokenError.empty()
                && magentaPixels > 100u,
            apiName + " did not draw the custom particle shaders ("
                + std::to_string(magentaPixels) + " placeholder pixels; "
                "errors: [" + probeError + "] [" + brokenError + "])");
        return capture;
    }

    // SSRは前フレームのカラーを読むため、有効にした最初のフレームは
    // SSR無しと同じ画像です。2フレーム目から床へCubeの赤が映り、SSRを
    // 切ると元の画像へ戻ります。
    void RequireD3D12ScreenSpaceReflection()
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        LamaPon::Scene scene(graphics);
        BuildScreenSpaceReflectionScene(scene);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto captureComposition = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            std::uint32_t width{};
            std::uint32_t height{};
            auto capture = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 SSR capture has unexpected dimensions");
            return capture;
        };
        const auto setReflectionEnabled = [&](const bool enabled)
        {
            auto reflection = scene.ScreenSpaceReflection();
            reflection.enabled = enabled;
            scene.SetScreenSpaceReflectionSettings(reflection);
        };

        setReflectionEnabled(false);
        const auto withoutReflection = captureComposition();
        setReflectionEnabled(true);
        Require(
            captureComposition() == withoutReflection,
            "The first DirectX 12 SSR frame read a missing color history");
        const auto withReflection = captureComposition();
        const auto& reflection = graphics.Lighting().screenSpaceReflection;
        Require(
            reflection.enabled
                && graphics.IsGraphicsViewCurrent(reflection.texture)
                && graphics.IsGraphicsViewCurrent(reflection.depth)
                && reflection.depthPyramidMaximumMip == 8u,
            "The DirectX 12 scene did not apply SSR with a full Hi-Z "
            "pyramid");

        std::size_t reflectedPixels{};
        for (std::size_t offset{};
             offset + 3u < withReflection.size();
             offset += 4u)
        {
            if (withReflection[offset] > withoutReflection[offset] + 12u
                && withReflection[offset]
                    > withReflection[offset + 1u] + 20u)
            {
                ++reflectedPixels;
            }
        }
        Require(
            reflectedPixels > 40u,
            "DirectX 12 SSR did not reflect the red cube onto the floor ("
                + std::to_string(reflectedPixels)
                + " reflected pixels)");

        setReflectionEnabled(false);
        Require(
            captureComposition() == withoutReflection,
            "Disabling DirectX 12 SSR did not restore the original frame");
    }

    // Sceneのグラデーション空を朝昼夜モードで描きます。天頂・地平線・
    // 地面の3色と太陽円盤、にじみが画面に入るようカメラを少し上へ向け、
    // 手前のProcedural Cubeで空が深度を書かず3Dの背後に残ることも同じ
    // 画像で比べます。
    [[nodiscard]] Capture RenderSkyCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The sky capture did not start the requested rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        LamaPon::Scene scene(graphics);

        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        // 正のピッチで少し上を向き、下端に地面、上端に天頂が入ります。
        cameraObject.GetTransform().SetEulerAngles(0.25f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        // Directional Lightは自分の-Z軸の向きへ進みます。180度振り返らせて
        // 少し下へ傾け、カメラの正面やや上に太陽が見えるようにします。
        auto& sunObject = scene.CreateGameObject("Sun");
        sunObject.GetTransform().SetEulerAngles(
            -0.3f,
            DirectX::XM_PI,
            0.0f);
        auto& sun = sunObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        sun.SetCastsShadows(false);
        // トーンマップ後も円盤がほぼ白く残り、数十画素を占めるように
        // 実際の太陽より明るく大きくします。
        sun.SetIntensity(4.0f);
        sun.SetAngularDiameterDegrees(8.0f);

        LamaPon::SkySettings sky;
        sky.enabled = true;
        sky.sunDriven = true;
        scene.SetSkySettings(sky);

        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        auto& cube = scene.CreateGameObject("Cube");
        cube.GetTransform().position = { -1.2f, 0.3f, 0.0f };
        auto& cubeMesh = cube.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.9f, 0.15f, 0.1f, 1.0f });
        cubeMesh.SetProceduralMesh(cubeVertices, cubeIndices);

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        auto* const target = graphics.SceneCompositionTarget();
        Require(
            target != nullptr,
            "The sky capture has no scene composition target");
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            target);
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        // 空は画面全体を塗るためclearの黒は残らず、太陽円盤とにじみは
        // ほぼ白い画素になります。両APIとも空を描かずに一致する誤検出を
        // 防ぎます。
        std::size_t clearPixels{};
        std::size_t sunPixels{};
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            const auto red = capture.pixels[offset];
            const auto green = capture.pixels[offset + 1u];
            const auto blue = capture.pixels[offset + 2u];
            if (red == 0u && green == 0u && blue == 0u)
            {
                ++clearPixels;
            }
            if (red > 230u && green > 230u && blue > 230u)
            {
                ++sunPixels;
            }
        }
        Require(
            clearPixels == 0u && sunPixels > 40u,
            std::string{ api == LamaPon::RenderingApi::DirectX11
                    ? "DirectX 11"
                    : "DirectX 12" }
                + " did not draw the gradient sky and sun disk ("
                + std::to_string(clearPixels) + " clear pixels, "
                + std::to_string(sunPixels) + " sun pixels)");
        return capture;
    }

    // cube DDSのパーサーが面ごとのミップ列へ分けることと、2Dとcube、
    // 欠けた面のDDSを取り違えないことを確かめます。
    void RequireDdsCubeParser(const std::vector<std::uint8_t>& cubeBytes)
    {
        const auto prepared =
            LamaPon::TextureLoader::PrepareDdsCubeTextureData(cubeBytes);
        Require(
            LamaPon::TextureLoader::IsDdsCubeTexture(cubeBytes)
                && !LamaPon::TextureLoader::IsDdsCubeTexture(BuildRgbaDds())
                && prepared.format == DXGI_FORMAT_R8G8B8A8_UNORM
                && prepared.levels.size() == 6u
                && prepared.levels[0].width == 4u
                && prepared.levels[5].height == 4u
                && prepared.levels[5].bytes.size() == 64u
                && prepared.levels[1].bytes[0] == cubeBytes[128u + 64u],
            "The DDS parser did not split the cube into six faces");

        const auto rejects = [](const auto& prepare)
        {
            try
            {
                static_cast<void>(prepare());
            }
            catch (const std::invalid_argument&)
            {
                return true;
            }
            return false;
        };
        auto partialCube = cubeBytes;
        WriteLittleEndian32(partialCube, 112u, 0x0600u);
        Require(
            rejects([&cubeBytes]
            {
                return LamaPon::TextureLoader::PrepareDdsTextureData(
                    cubeBytes);
            })
                && rejects([]
                {
                    return LamaPon::TextureLoader::PrepareDdsCubeTextureData(
                        BuildRgbaDds());
                })
                && rejects([&partialCube]
                {
                    return LamaPon::TextureLoader::PrepareDdsCubeTextureData(
                        partialCube);
                }),
            "The DDS parser accepted a cube as 2D, a 2D texture as a cube, "
            "or a cube with missing faces");
    }

    // DDS cubeのSkyを描きます。カメラを斜め上へ向けて複数の面をまたぎ、
    // D3D11と同じくTextureCubeとして読めていることと、同じSceneの
    // グラデーション空とは違う画像になることを確かめます。
    [[nodiscard]] Capture RenderCubemapSkyCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile,
        const std::span<const std::uint8_t> cubemapBytes)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The cubemap sky capture did not start the requested rendering "
            "API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        const auto cubemapView =
            graphics.Assets().CreateTextureViewHandleFromMemory(
                cubemapBytes,
                true);
        Require(
            graphics.IsSampleableCubeView(cubemapView),
            apiName + " did not load the DDS cube as a sampleable "
                "TextureCube");

        LamaPon::SkySettings sky;
        sky.enabled = true;
        sky.intensity = 1.5f;
        sky.iblIntensity = 0.0f;

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto view = DirectX::XMMatrixLookAtLH(
            DirectX::XMVectorZero(),
            DirectX::XMVectorSet(0.65f, 0.34f, -0.68f, 0.0f),
            DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const auto projection = DirectX::XMMatrixPerspectiveFovLH(
            DirectX::XMConvertToRadians(60.0f),
            static_cast<float>(CanvasWidth) / CanvasHeight,
            0.1f,
            100.0f);
        const auto captureFrame = [
            &graphics,
            &clearColor,
            &sky,
            &view,
            &projection](const LamaPon::GraphicsViewHandle& texture)
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            graphics.DrawSky(view, projection, sky, texture);
            graphics.EndSceneComposition({});
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            return frame;
        };
        const auto cubemapFrame = captureFrame(cubemapView);
        const auto gradientFrame = captureFrame({});

        graphics.BeginFrame(clearColor);
        const auto prefiltered =
            graphics.TryGetPrefilteredEnvironmentViews(cubemapView, 1u);
        graphics.EndFrame();
        Require(
            prefiltered.IsValid()
                && graphics.IsSampleableCubeView(prefiltered.specular)
                && graphics.IsSampleableCubeView(prefiltered.irradiance)
                && prefiltered.specularMaximumMip == 7.0f,
            apiName + " did not prefilter the in-memory cubemap");

        std::size_t changedPixels{};
        for (std::size_t offset{};
             offset + 3u < cubemapFrame.pixels.size()
                 && offset + 3u < gradientFrame.pixels.size();
             offset += 4u)
        {
            if (!std::equal(
                    cubemapFrame.pixels.begin()
                        + static_cast<std::ptrdiff_t>(offset),
                    cubemapFrame.pixels.begin()
                        + static_cast<std::ptrdiff_t>(offset + 3u),
                    gradientFrame.pixels.begin()
                        + static_cast<std::ptrdiff_t>(offset)))
            {
                ++changedPixels;
            }
        }
        Require(
            changedPixels > cubemapFrame.pixels.size() / 8u,
            apiName + " drew the cubemap sky like the gradient sky ("
                + std::to_string(changedPixels) + " changed pixels)");
        return cubemapFrame;
    }

    // 2枚の合成画像で、RGBのどれかが違う画素を数えます。
    [[nodiscard]] std::size_t CountChangedPixels(
        const Capture& first,
        const Capture& second)
    {
        std::size_t changedPixels{};
        for (std::size_t offset{};
             offset + 3u < first.pixels.size()
                 && offset + 3u < second.pixels.size();
             offset += 4u)
        {
            if (!std::equal(
                    first.pixels.begin()
                        + static_cast<std::ptrdiff_t>(offset),
                    first.pixels.begin()
                        + static_cast<std::ptrdiff_t>(offset + 3u),
                    second.pixels.begin()
                        + static_cast<std::ptrdiff_t>(offset)))
            {
                ++changedPixels;
            }
        }
        return changedPixels;
    }

    // 霧を掛けたSceneを描きます。距離の違うProcedural CubeとMaterial上書き中の
    // CMOはLitEffectの範囲霧と指数霧で、この画像をD3D11と比べます。上書きを
    // 外したCMOはDirectXTK Effectの線形霧です。開始0と終了0.01でどちらの式も
    // 霧の色へ振り切れるので、霧が掛かったことだけを確かめます（内蔵partの
    // 半透明の扱いはこの段階の比較対象外です）。
    [[nodiscard]] Capture RenderFogCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The fog capture did not start the requested rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.6f, 6.0f };
        cameraObject.GetTransform().SetEulerAngles(-0.08f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.8f);

        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        constexpr std::array<const char*, 4> cubeNames{
            "NearCube", "MiddleCube", "FarCube", "FarthestCube" };
        constexpr std::array<DirectX::XMFLOAT3, 4> cubePositions{ {
            { -2.0f, 0.8f, 2.5f },
            { -0.8f, 0.8f, -1.0f },
            { 0.8f, 0.8f, -5.0f },
            { 2.8f, 0.8f, -10.0f } } };
        for (std::size_t index{}; index < cubeNames.size(); ++index)
        {
            auto& object = scene.CreateGameObject(cubeNames[index]);
            object.GetTransform().position = cubePositions[index];
            object.GetTransform().SetEulerAngles(0.4f, 0.6f, 0.0f);
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.9f, 0.2f, 0.15f, 1.0f });
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
        }

        auto& modelObject = scene.CreateGameObject("CmoModel");
        modelObject.GetTransform().position = { 0.0f, -0.9f, 1.0f };
        modelObject.GetTransform().scale = { 1.2f, 1.2f, 1.2f };
        modelObject.GetTransform().SetEulerAngles(0.45f, 0.65f, 0.0f);
        auto& model = modelObject.AddComponent<
            LamaPon::ModelRendererComponent>(
                std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
                    / "models"
                    / "arrow.cmo");
        // Material上書き中はD3D11もDirectXTK EffectではなくLamaPon Litで
        // 描くため、D3D12と同じ霧の式で比べられます。
        model.SetMaterialOverrideEnabled(true);
        model.SetEmissiveColor({ 0.2f, 0.6f, 0.3f });

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto captureFrame = [&graphics, &scene, &clearColor]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            auto* const target = graphics.SceneCompositionTarget();
            Require(
                target != nullptr,
                "The fog capture has no scene composition target");
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                target);
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            return frame;
        };

        LamaPon::FogSettings fog;
        fog.enabled = true;
        fog.color = { 0.55f, 0.65f, 0.8f };
        fog.startDistance = 4.0f;
        fog.endDistance = 18.0f;
        fog.density = 0.04f;
        scene.SetFogSettings(fog);
        const auto litFogFrame = captureFrame();
        fog.enabled = false;
        scene.SetFogSettings(fog);
        const auto litClearFrame = captureFrame();

        model.SetMaterialOverrideEnabled(false);
        fog.enabled = true;
        fog.startDistance = 0.0f;
        fog.endDistance = 0.01f;
        fog.density = 0.0f;
        scene.SetFogSettings(fog);
        const auto fullFogFrame = captureFrame();
        fog.enabled = false;
        scene.SetFogSettings(fog);
        const auto unfoggedFrame = captureFrame();

        const auto litFogPixels =
            CountChangedPixels(litFogFrame, litClearFrame);
        const auto fullFogPixels =
            CountChangedPixels(fullFogFrame, unfoggedFrame);
        Require(
            litFogPixels > 400u && fullFogPixels > 800u,
            apiName + " did not apply the scene fog ("
                + std::to_string(litFogPixels) + " lit fog pixels, "
                + std::to_string(fullFogPixels) + " full fog pixels)");
        return litFogFrame;
    }

    // Directional／Point／Spot LightでProcedural Cubeを照らします。D3D11の
    // LamaPonLit.hlslと同じCook-Torrance GGX、太陽の見かけの大きさ、法線
    // マップの強さの掛け方で、D3D12の基本3D描画が一致することを比べます。
    // 影の比較はこの段階の対象外なので、光源の影は切ります。
    [[nodiscard]] Capture RenderDirectLightingCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The direct lighting capture did not start the requested "
            "rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        const bool directX11 = api == LamaPon::RenderingApi::DirectX11;
        const std::string apiName = directX11 ? "DirectX 11" : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.3f, 5.0f };
        cameraObject.GetTransform().SetEulerAngles(-0.05f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.05f);

        // 光は自分の-Z軸の向きへ進むので、上から手前へ差し込むよう傾けます。
        auto& sunObject = scene.CreateGameObject("Sun");
        sunObject.GetTransform().SetEulerAngles(-0.6f, 0.35f, 0.0f);
        auto& sun = sunObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        sun.SetCastsShadows(false);
        sun.SetAngularDiameterDegrees(3.0f);

        auto& pointObject = scene.CreateGameObject("PointLight");
        pointObject.GetTransform().position = { -1.2f, 0.9f, 1.6f };
        auto& pointLight = pointObject.AddComponent<
            LamaPon::PointLightComponent>(
                DirectX::XMFLOAT3{ 1.0f, 0.7f, 0.4f },
                4.0f,
                4.0f);
        pointLight.SetCastsShadows(false);

        auto& spotObject = scene.CreateGameObject("SpotLight");
        spotObject.GetTransform().position = { 1.4f, 0.4f, 2.2f };
        auto& spotLight = spotObject.AddComponent<
            LamaPon::SpotLightComponent>(
                DirectX::XMFLOAT3{ 0.4f, 0.7f, 1.0f },
                6.0f,
                6.0f,
                DirectX::XMConvertToRadians(18.0f),
                DirectX::XMConvertToRadians(28.0f));
        spotLight.SetCastsShadows(false);

        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        const auto addCube = [&](
            const char* const name,
            const float x,
            const DirectX::XMFLOAT4& color,
            const float metallic,
            const float roughness) -> LamaPon::MeshRendererComponent&
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 0.9f, 0.9f, 0.9f };
            object.GetTransform().SetEulerAngles(0.5f, 0.7f, 0.0f);
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                color);
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
            mesh.SetMetallic(metallic);
            mesh.SetRoughness(roughness);
            return mesh;
        };
        addCube("GoldMetal", -1.6f, { 1.0f, 0.78f, 0.35f, 1.0f }, 0.9f, 0.15f);
        auto& mapped = addCube(
            "NormalMapped",
            0.0f,
            { 0.6f, 0.7f, 0.9f, 1.0f },
            0.0f,
            0.55f);
        // 既存の画像を法線として読み、法線textureとstrengthの経路を
        // D3D11 / D3D12で同じ入力にします。
        mapped.SetNormalTexturePath("textures/LamaPonLogo.png");
        mapped.SetNormalStrength(1.5f);
        auto& plastic = addCube(
            "RoughPlastic",
            1.6f,
            { 0.9f, 0.3f, 0.25f, 1.0f },
            0.25f,
            0.8f);
        // 非一様スケールでも、D3D11と同じ逆転置行列で法線を変換します。
        plastic.Owner().GetTransform().scale = { 1.2f, 0.6f, 0.9f };

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto captureFrame = [&graphics, &scene, &clearColor]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            auto* const target = graphics.SceneCompositionTarget();
            Require(
                target != nullptr,
                "The direct lighting capture has no scene composition "
                "target");
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                target);
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 法線マップの読み込みを待たずに比べないよう、2フレーム目を使います。
        static_cast<void>(captureFrame());
        const auto capture = captureFrame();
        std::size_t litPixels{};
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            if (std::max({
                    capture.pixels[offset],
                    capture.pixels[offset + 1u],
                    capture.pixels[offset + 2u] }) > 40u)
            {
                ++litPixels;
            }
        }
        Require(
            litPixels > 1500u,
            apiName + " did not light the cubes with the direct lights ("
                + std::to_string(litPixels) + " lit pixels)");
        return capture;
    }

    // Forward+のクラスタライトを、固定配列の16灯を超える24灯のPoint Lightと
    // 10灯のSpot Lightで確かめます。D3D11と同じカリングの番号表とGGXで
    // 床が照らされることを比べます（影は対象外）。
    [[nodiscard]] Capture RenderClusteredLightingCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The clustered lighting capture did not start the requested "
            "rendering API");
        // Lit、Environment、Light culling shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 4.0f, 7.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.5f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.02f);

        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        auto& floorObject = scene.CreateGameObject("Floor");
        floorObject.GetTransform().position = { 0.0f, -0.1f, 0.0f };
        floorObject.GetTransform().scale = { 12.0f, 0.2f, 10.0f };
        auto& floor = floorObject.AddComponent<
            LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });
        floor.SetProceduralMesh(cubeVertices, cubeIndices);
        floor.SetRoughness(0.7f);

        constexpr std::array<DirectX::XMFLOAT3, 4> pointColors{ {
            { 1.0f, 0.35f, 0.3f },
            { 0.35f, 1.0f, 0.4f },
            { 0.35f, 0.5f, 1.0f },
            { 1.0f, 0.9f, 0.35f } } };
        std::size_t pointIndex{};
        for (int row{}; row < 4; ++row)
        {
            for (int column{}; column < 6; ++column)
            {
                auto& object = scene.CreateGameObject("ClusterPoint");
                object.GetTransform().position = {
                    -5.0f + 2.0f * static_cast<float>(column),
                    0.6f,
                    -3.0f + 2.0f * static_cast<float>(row) };
                auto& light = object.AddComponent<
                    LamaPon::PointLightComponent>(
                        pointColors[pointIndex % pointColors.size()],
                        2.0f,
                        2.2f);
                light.SetCastsShadows(false);
                ++pointIndex;
            }
        }
        // 回転の無いSpot Lightは-Zを向くため、ピッチ-90度で真下を照らします。
        for (int index{}; index < 10; ++index)
        {
            auto& object = scene.CreateGameObject("ClusterSpot");
            object.GetTransform().position = {
                -4.5f + static_cast<float>(index),
                2.5f,
                -4.4f };
            object.GetTransform().SetEulerAngles(
                -DirectX::XM_PIDIV2,
                0.0f,
                0.0f);
            auto& light = object.AddComponent<LamaPon::SpotLightComponent>(
                DirectX::XMFLOAT3{ 0.9f, 0.8f, 1.0f },
                3.0f,
                4.0f,
                DirectX::XMConvertToRadians(20.0f),
                DirectX::XMConvertToRadians(30.0f));
            light.SetCastsShadows(false);
        }

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        auto* const target = graphics.SceneCompositionTarget();
        Require(
            target != nullptr,
            "The clustered lighting capture has no scene composition target");
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            target);
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        const auto& clustered = graphics.Lighting().clustered;
        const bool clusteredActive =
            clustered.enabled && clustered.lightCount == 34u;
        std::size_t litPixels{};
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            if (std::max({
                    capture.pixels[offset],
                    capture.pixels[offset + 1u],
                    capture.pixels[offset + 2u] }) > 40u)
            {
                ++litPixels;
            }
        }
        Require(
            clusteredActive && litPixels > 3000u,
            apiName + " did not light the floor with Forward+ clustered "
                "lights (clustered: " + (clusteredActive ? "yes" : "no")
                + ", " + std::to_string(litPixels) + " lit pixels)");
        return capture;
    }

    // Sceneへ復元した2×2×2のベイクした間接光（L1球面調和）を確かめます。
    // 左ほど青く右ほど赤く、上向きの面ほど緑が強い係数で、Litの2つのCubeと
    // t23〜t25を読むbaked-gi-probeを照らし、GIを切った画像と違うことと、
    // D3D11と一致することを比べます。
    [[nodiscard]] Capture RenderBakedGlobalIlluminationCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The baked global illumination capture did not start the "
            "requested rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // D3D11の非同期compileは最初のフレームを標準Litで描くため止めます。
        graphics.SetAsyncShaderCompilationEnabled(false);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.4f, 4.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.06f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.3f);

        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        const auto addCube = [&](
            const char* const name,
            const float x,
            const float metallic,
            const float roughness) -> LamaPon::MeshRendererComponent&
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.1f, 1.1f, 1.1f };
            object.GetTransform().SetEulerAngles(0.45f, 0.6f, 0.0f);
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.9f, 0.9f, 0.9f, 1.0f });
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
            mesh.SetMetallic(metallic);
            mesh.SetRoughness(roughness);
            return mesh;
        };
        addCube("GiDielectric", -1.7f, 0.0f, 0.8f);
        addCube("GiMetal", 0.0f, 0.6f, 0.4f);
        auto& probe = addCube("BakedGiProbe", 1.7f, 0.0f, 0.5f);
        probe.SetShaderPath(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/baked-gi-probe.hlsl");

        LamaPon::BakedGlobalIlluminationSettings settings;
        settings.enabled = true;
        settings.center = { 0.0f, 0.0f, 0.0f };
        settings.size = { 6.0f, 4.0f, 4.0f };
        settings.resolutionX = 2;
        settings.resolutionY = 2;
        settings.resolutionZ = 2;
        settings.intensity = 1.0f;
        scene.SetBakedGlobalIlluminationSettings(settings);

        // 並びは[R,G,B]×[z,y,x]×(x係数, y係数, z係数, 定数項)のfp16です。
        constexpr std::size_t ProbeCount = 8u;
        std::vector<std::uint16_t> payload(
            ProbeCount
            * LamaPon::BakedGlobalIlluminationCoefficientsPerProbe);
        for (std::size_t z{}; z < 2u; ++z)
        {
            for (std::size_t y{}; y < 2u; ++y)
            {
                for (std::size_t x{}; x < 2u; ++x)
                {
                    const std::size_t probeIndex = (z * 2u + y) * 2u + x;
                    const float side = static_cast<float>(x);
                    const float height = static_cast<float>(y);
                    const std::array<std::array<float, 4>, 3> channels{ {
                        { 0.3f, 0.0f, 0.0f, 0.2f + 0.8f * side },
                        { 0.0f, 0.4f, 0.0f, 0.3f + 0.3f * height },
                        { 0.0f, 0.0f, 0.25f, 1.0f - 0.8f * side } } };
                    for (std::size_t channel{}; channel < 3u; ++channel)
                    {
                        for (std::size_t term{}; term < 4u; ++term)
                        {
                            payload[(channel * ProbeCount + probeIndex) * 4u
                                + term] =
                                DirectX::PackedVector::XMConvertFloatToHalf(
                                    channels[channel][term]);
                        }
                    }
                }
            }
        }
        scene.RestoreBakedGlobalIllumination(settings, payload);
        Require(
            scene.HasBakedGlobalIllumination(),
            apiName + " rejected the baked global illumination payload");

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto captureFrame = [&graphics, &scene, &clearColor]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            auto* const target = graphics.SceneCompositionTarget();
            Require(
                target != nullptr,
                "The baked global illumination capture has no scene "
                "composition target");
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                target);
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            return frame;
        };
        const auto giFrame = captureFrame();
        const auto shaderError = probe.ShaderError();
        // 3本のTexture3Dを作れずにフラットな環境光へ落ちていないことを
        // 確かめます。
        const auto& bakedGi = graphics.Lighting().bakedGlobalIllumination;
        const bool giActive = bakedGi.enabled
            && bakedGi.redCoefficients
            && bakedGi.greenCoefficients
            && bakedGi.blueCoefficients;
        settings.enabled = false;
        scene.SetBakedGlobalIlluminationSettings(settings);
        const auto flatFrame = captureFrame();
        const auto changedPixels = CountChangedPixels(giFrame, flatFrame);
        Require(
            giActive && shaderError.empty() && changedPixels > 1500u,
            apiName + " did not light the cubes with baked global "
                "illumination (active: " + (giActive ? "yes" : "no") + ", "
                + std::to_string(changedPixels)
                + " changed pixels; error: [" + shaderError + "])");
        return giFrame;
    }

    // リフレクションプローブを、Scene読み込み後と同じ実行中のベイクで
    // 確かめます。赤・緑・青の発光壁に囲まれた場所へ、ボックス射影付きの
    // プローブと、境界で混ぜる2個目のプローブを置きます。滑らかな金属、
    // 粗い誘電体、t3／t6を読むenvironment-probeのCubeへ映り込みが入り、
    // プローブを切った画像と違うことと、D3D11と一致することを比べます。
    [[nodiscard]] Capture RenderReflectionProbeCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The reflection probe capture did not start the requested "
            "rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // D3D11の非同期compileは最初のフレームを標準Litで描くため止めます。
        graphics.SetAsyncShaderCompilationEnabled(false);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.4f, 4.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.06f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.1f);

        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        const auto addWall = [&](
            const char* const name,
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& scale,
            const DirectX::XMFLOAT3& emissive)
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = position;
            object.GetTransform().scale = scale;
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.2f, 0.2f, 0.2f, 1.0f });
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
            mesh.SetRoughness(0.9f);
            mesh.SetEmissiveColor(emissive);
        };
        addWall("RedWall", { -6.0f, 1.0f, 0.0f }, { 0.5f, 8.0f, 14.0f },
            { 1.5f, 0.1f, 0.1f });
        addWall("GreenWall", { 6.0f, 1.0f, 0.0f }, { 0.5f, 8.0f, 14.0f },
            { 0.1f, 1.5f, 0.2f });
        addWall("BlueCeiling", { 0.0f, 5.0f, 0.0f }, { 12.0f, 0.5f, 14.0f },
            { 0.1f, 0.2f, 1.5f });
        addWall("Floor", { 0.0f, -1.5f, 0.0f }, { 12.0f, 0.5f, 14.0f },
            { 0.35f, 0.35f, 0.35f });
        addWall("BackWall", { 0.0f, 1.0f, -7.0f }, { 12.0f, 8.0f, 0.5f },
            { 0.6f, 0.45f, 0.15f });

        const auto addCube = [&](
            const char* const name,
            const float x,
            const float metallic,
            const float roughness) -> LamaPon::MeshRendererComponent&
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.1f, 1.1f, 1.1f };
            object.GetTransform().SetEulerAngles(0.45f, 0.6f, 0.0f);
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.9f, 0.9f, 0.9f, 1.0f });
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
            mesh.SetMetallic(metallic);
            mesh.SetRoughness(roughness);
            return mesh;
        };
        addCube("ProbeMetal", -1.7f, 1.0f, 0.15f);
        addCube("ProbeRough", 0.0f, 0.0f, 0.9f);
        auto& shaderCube = addCube("ProbeShader", 1.7f, 0.0f, 0.5f);
        shaderCube.SetShaderPath(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/environment-probe.hlsl");

        // 左のプローブはボックス射影付きです。範囲4、混ぜ始め2なので、中央と
        // 右のCubeは2個目のプローブと比率で混ざります。
        auto& primaryObject = scene.CreateGameObject("PrimaryProbe");
        primaryObject.GetTransform().position = { -1.0f, 1.5f, 0.0f };
        auto& primaryProbe = primaryObject.AddComponent<
            LamaPon::ReflectionProbeComponent>(4.0f, 1.0f);
        primaryProbe.SetBoxExtents({ 5.0f, 2.75f, 6.5f });
        primaryProbe.SetBlendDistance(2.0f);
        primaryProbe.RequestBake();
        auto& secondaryObject = scene.CreateGameObject("SecondaryProbe");
        secondaryObject.GetTransform().position = { 2.5f, 1.5f, 0.0f };
        auto& secondaryProbe = secondaryObject.AddComponent<
            LamaPon::ReflectionProbeComponent>(4.0f, 1.2f);
        secondaryProbe.SetBlendDistance(2.0f);
        secondaryProbe.RequestBake();

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto captureFrame = [&graphics, &scene, &clearColor]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            auto* const target = graphics.SceneCompositionTarget();
            Require(
                target != nullptr,
                "The reflection probe capture has no scene composition "
                "target");
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                target);
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 最初のフレームの頭で2つのプローブを焼き、同じフレームから使います。
        const auto probeFrame = captureFrame();
        const auto shaderError = shaderCube.ShaderError();
        const bool baked = primaryProbe.IsBaked()
            && secondaryProbe.IsBaked()
            && graphics.IsSampleableCubeView(
                primaryProbe.BakedEnvironment().specular)
            && graphics.IsSampleableCubeView(
                secondaryProbe.BakedEnvironment().irradiance);
        primaryProbe.SetEnabled(false);
        secondaryProbe.SetEnabled(false);
        const auto flatFrame = captureFrame();
        const auto changedPixels = CountChangedPixels(probeFrame, flatFrame);
        Require(
            baked && shaderError.empty() && changedPixels > 1500u,
            apiName + " did not light the cubes with baked reflection "
                "probes (baked: " + (baked ? "yes" : "no") + ", "
                + std::to_string(changedPixels)
                + " changed pixels; error: [" + shaderError + "])");
        return probeFrame;
    }

    // Material上書き中のCMOをMaterial custom shaderで描きます。左は描画状態を
    // 宣言しロゴを貼ったテンプレートで、宣言による上書きとテクスチャ座標を、
    // 右は宣言の無いfacing-probeで、DirectXTKのModelMeshと同じ既定の合成・
    // 深度・カリング（表面と裏面のどちらを描くか）を、D3D11との一致で確かめます。
    [[nodiscard]] Capture RenderCmoMaterialShaderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The CMO material shader capture did not start the requested "
            "rendering API");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // D3D11の非同期compileは最初のフレームを標準Litで描くため止めます。
        graphics.SetAsyncShaderCompilationEnabled(false);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        const auto addModel = [&](
            const char* const name,
            const float x) -> LamaPon::ModelRendererComponent&
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.4f, 1.4f, 1.4f };
            object.GetTransform().SetEulerAngles(0.45f, 0.65f, 0.0f);
            auto& renderer = object.AddComponent<
                LamaPon::ModelRendererComponent>(
                    std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
                        / "models"
                        / "arrow.cmo");
            renderer.SetMaterialOverrideEnabled(true);
            return renderer;
        };
        // arrow.cmoの内蔵albedoは黒いため、テンプレートにはロゴを貼ります。
        auto& declared = addModel("DeclaredCmo", -1.3f);
        declared.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        declared.SetAlbedoTexturePath("textures/LamaPonLogo.png");
        declared.SetCustomParameter(0, { 1.0f, 0.4f, 0.2f, 0.7f });
        declared.SetCustomParameter(1, { 0.3f, 1.0f, 0.0f, 0.0f });
        auto& defaultState = addModel("DefaultStateCmo", 1.3f);
        defaultState.SetShaderPath(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/facing-probe.hlsl");

        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 最初のフレームでModelとShaderを読み込みます。
        static_cast<void>(render());
        const auto capture = render();
        const auto declaredError = declared.ShaderError();
        const auto defaultStateError = defaultState.ShaderError();

        std::size_t leftPixels{};
        std::size_t rightPixels{};
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            if (std::max({
                    capture.pixels[offset],
                    capture.pixels[offset + 1u],
                    capture.pixels[offset + 2u] }) <= 12u)
            {
                continue;
            }
            const auto x = (offset / 4u) % capture.width;
            if (x < capture.width / 2u)
            {
                ++leftPixels;
            }
            else
            {
                ++rightPixels;
            }
        }
        Require(
            declaredError.empty()
                && defaultStateError.empty()
                && leftPixels > 100u
                && rightPixels > 100u,
            apiName + " did not draw the CMO material shaders ("
                + std::to_string(leftPixels) + " declared, "
                + std::to_string(rightPixels) + " default-state pixels; "
                "errors: [" + declaredError + "] [" + defaultStateError
                + "])");
        return capture;
    }

    // Mesh RendererのテセレーションShader（LamaPonTessellatedTerrain）を
    // D3D11と比べます。PlaneとCubeはD3D11と同じ四角パッチの制御点を
    // HSMain／DSMainで割って波打たせ、Procedural MeshのCubeは四角パッチへ
    // 分けないため、D3D11と同じ説明付きの代替表示になります。
    [[nodiscard]] Capture RenderTessellationCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The tessellation capture did not start the requested rendering "
            "API");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // D3D11の非同期compileは最初のフレームを標準Litで描くため止めます。
        graphics.SetAsyncShaderCompilationEnabled(false);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 2.2f, 5.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.35f, 0.0f, 0.0f);
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.6f);

        const auto addMesh = [&](
            const char* const name,
            const LamaPon::PrimitiveShape shape,
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& scale,
            const float yaw) -> LamaPon::MeshRendererComponent&
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = position;
            object.GetTransform().scale = scale;
            object.GetTransform().SetEulerAngles(0.0f, yaw, 0.0f);
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                shape,
                DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f });
            mesh.SetShaderPath(
                std::filesystem::path{ "shaders" }
                / "LamaPonTessellatedTerrain.hlsl");
            // 0=地面の色、1=起伏の高さ／細かさ／速さ、3.x=分割数です。
            mesh.SetCustomParameter(0, { 0.3f, 0.85f, 0.35f, 1.0f });
            mesh.SetCustomParameter(1, { 0.25f, 2.0f, 0.0f, 0.0f });
            mesh.SetCustomParameter(3, { 12.0f, 0.0f, 0.0f, 0.0f });
            return mesh;
        };
        auto& plane = addMesh(
            "TessellatedPlane",
            LamaPon::PrimitiveShape::Plane,
            { -1.4f, 0.0f, 0.0f },
            { 2.6f, 1.0f, 2.6f },
            0.3f);
        auto& cube = addMesh(
            "TessellatedCube",
            LamaPon::PrimitiveShape::Cube,
            { 1.5f, 0.7f, 0.0f },
            { 1.3f, 1.3f, 1.3f },
            0.6f);
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        auto& procedural = addMesh(
            "ProceduralCube",
            LamaPon::PrimitiveShape::Cube,
            { 0.3f, 1.9f, -1.2f },
            { 0.7f, 0.7f, 0.7f },
            0.4f);
        procedural.SetProceduralMesh(cubeVertices, cubeIndices);

        constexpr float clearColor[4]{ 0.05f, 0.05f, 0.08f, 1.0f };
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 最初のフレームでShaderを読み込みます。
        static_cast<void>(render());
        const auto capture = render();
        const auto planeError = plane.ShaderError();
        const auto cubeError = cube.ShaderError();
        const auto proceduralError = procedural.ShaderError();

        std::size_t terrainPixels{};
        std::size_t magentaPixels{};
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            const int red = capture.pixels[offset];
            const int green = capture.pixels[offset + 1u];
            const int blue = capture.pixels[offset + 2u];
            if (green > red + 40 && green > blue + 40)
            {
                ++terrainPixels;
            }
            else if (red > 150 && blue > 150 && green < 80)
            {
                ++magentaPixels;
            }
        }
        Require(
            planeError.empty()
                && cubeError.empty()
                && proceduralError.find("quad patches") != std::string::npos
                && terrainPixels > 1000u
                && magentaPixels > 50u,
            apiName + " did not draw the tessellation shaders ("
                + std::to_string(terrainPixels) + " terrain, "
                + std::to_string(magentaPixels) + " placeholder pixels; "
                "errors: [" + planeError + "] [" + cubeError + "] ["
                + proceduralError + "])");
        return capture;
    }

    // Material上書き中のCMOで、custom shaderの輪郭（VSOutline／PSOutline）と
    // 遮蔽表示（PSOccluded）をD3D11と比べます。手前の箱に隠れた部分は
    // 遮蔽表示の青、シルエットの外側は輪郭の黄色になります。
    [[nodiscard]] Capture RenderCmoOutlineCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The CMO outline capture did not start the requested rendering "
            "API");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // D3D11の非同期compileは最初のフレームを標準Litで描くため止めます。
        graphics.SetAsyncShaderCompilationEnabled(false);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 5.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // モデルより先に描かれて深度を書く、手前の箱です。
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        auto& occluderObject = scene.CreateGameObject("Occluder");
        occluderObject.GetTransform().position = { 0.0f, -0.3f, 2.0f };
        occluderObject.GetTransform().scale = { 0.8f, 0.5f, 0.2f };
        auto& occluder = occluderObject.AddComponent<
            LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.35f, 0.35f, 0.35f, 1.0f });
        occluder.SetProceduralMesh(cubeVertices, cubeIndices);

        auto& object = scene.CreateGameObject("OutlinedCmo");
        object.GetTransform().position = { 0.0f, 0.2f, 0.0f };
        object.GetTransform().scale = { 1.6f, 1.6f, 1.6f };
        object.GetTransform().SetEulerAngles(0.45f, 0.65f, 0.0f);
        auto& renderer = object.AddComponent<
            LamaPon::ModelRendererComponent>(
                std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
                    / "models"
                    / "arrow.cmo");
        renderer.SetMaterialOverrideEnabled(true);
        renderer.SetShaderPath(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/outline-probe.hlsl");
        // 3: x = 輪郭の太さ、yzw = 輪郭の色。4: rgb = 遮蔽表示の色、
        // w > 0で遮蔽表示を有効にします。
        renderer.SetCustomParameter(3, { 0.08f, 1.0f, 0.85f, 0.1f });
        renderer.SetCustomParameter(4, { 0.2f, 0.6f, 1.0f, 1.0f });

        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 最初のフレームでModelとShaderを読み込みます。
        static_cast<void>(render());
        const auto capture = render();
        const auto shaderError = renderer.ShaderError();

        std::size_t outlinePixels{};
        std::size_t occludedPixels{};
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            const int red = capture.pixels[offset];
            const int green = capture.pixels[offset + 1u];
            const int blue = capture.pixels[offset + 2u];
            if (red > 170 && green > 140 && blue < 90)
            {
                ++outlinePixels;
            }
            else if (blue > red + 50 && blue > green + 15)
            {
                ++occludedPixels;
            }
        }
        Require(
            shaderError.empty()
                && outlinePixels > 20u
                && occludedPixels > 150u,
            apiName + " did not draw the CMO outline and occluded passes ("
                + std::to_string(outlinePixels) + " outline, "
                + std::to_string(occludedPixels) + " occluded pixels; "
                "error: [" + shaderError + "])");
        return capture;
    }

    // Model Rendererのワイヤーフレーム表示をD3D11と比べます。上の段はMaterial
    // 上書きの無いCMO（DirectXTK Effect）、上書き中のLit、宣言の無いcustom
    // shaderです。下の段は宣言付きのcustom shaderのCMO（D3D11と同じく塗り
    // つぶしに戻ります）、glTFの既定Lit、glTFのcustom shader（宣言より
    // ワイヤーフレームを優先します）です。
    [[nodiscard]] Capture RenderWireframeCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The wireframe capture did not start the requested rendering API");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // D3D11の非同期compileは最初のフレームを標準Litで描くため止めます。
        graphics.SetAsyncShaderCompilationEnabled(false);
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        LamaPon::Scene scene(graphics);
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 8.0f };
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // 暗い線も見分けられるよう、奥に明るい板を置きます。
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        auto& backdropObject = scene.CreateGameObject("Backdrop");
        backdropObject.GetTransform().position = { 0.0f, 0.0f, -3.0f };
        backdropObject.GetTransform().scale = { 24.0f, 12.0f, 0.2f };
        auto& backdrop = backdropObject.AddComponent<
            LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.55f, 0.55f, 0.6f, 1.0f });
        backdrop.SetProceduralMesh(cubeVertices, cubeIndices);

        const auto models = std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / "models";
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        std::vector<LamaPon::ModelRendererComponent*> renderers;
        const auto addModel = [&](
            const char* const name,
            const std::filesystem::path& path,
            const DirectX::XMFLOAT3& position,
            const float scale,
            const float roll = 0.0f) -> LamaPon::ModelRendererComponent&
        {
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = position;
            object.GetTransform().scale = { scale, scale, scale };
            object.GetTransform().SetEulerAngles(0.45f, 0.65f, roll);
            auto& renderer =
                object.AddComponent<LamaPon::ModelRendererComponent>(path);
            renderer.SetAnimationPlayOnStart(false);
            renderer.SetWireframe(true);
            renderers.push_back(&renderer);
            return renderer;
        };
        static_cast<void>(addModel(
            "EffectCmo",
            models / "arrow.cmo",
            { -3.0f, 1.6f, 0.0f },
            1.6f));
        auto& litCmo = addModel(
            "LitCmo",
            models / "arrow.cmo",
            { 0.0f, 1.6f, 0.0f },
            1.6f);
        litCmo.SetMaterialOverrideEnabled(true);
        litCmo.SetAlbedoTexturePath("textures/LamaPonLogo.png");
        auto& undeclaredCmo = addModel(
            "UndeclaredShaderCmo",
            models / "arrow.cmo",
            { 3.0f, 1.6f, 0.0f },
            1.6f);
        undeclaredCmo.SetMaterialOverrideEnabled(true);
        undeclaredCmo.SetShaderPath(fixtures / "facing-probe.hlsl");
        auto& declaredCmo = addModel(
            "DeclaredShaderCmo",
            models / "arrow.cmo",
            { -3.0f, -1.6f, 0.0f },
            1.6f);
        declaredCmo.SetMaterialOverrideEnabled(true);
        declaredCmo.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        declaredCmo.SetAlbedoTexturePath("textures/LamaPonLogo.png");
        static_cast<void>(addModel(
            "LitGltf",
            models / "TexturedRiggedSimple.gltf",
            { 0.0f, -1.6f, 0.0f },
            0.4f,
            1.1f));
        auto& shaderGltf = addModel(
            "ShaderGltf",
            models / "TexturedRiggedSimple.gltf",
            { 3.0f, -1.6f, 0.0f },
            0.4f,
            1.1f);
        shaderGltf.SetMaterialOverrideEnabled(true);
        shaderGltf.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        shaderGltf.SetCustomParameter(0, { 1.0f, 0.5f, 0.2f, 0.6f });

        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            return frame;
        };
        // 最初のフレームでModelとShaderを読み込みます。
        static_cast<void>(render());
        const auto wireframe = render();
        std::string errors;
        for (auto* const renderer : renderers)
        {
            if (!renderer->ShaderError().empty())
            {
                errors += "[" + renderer->ShaderError() + "] ";
            }
            renderer->SetWireframe(false);
        }
        const auto solid = render();
        const auto changedPixels = CountChangedPixels(wireframe, solid);
        Require(
            errors.empty() && changedPixels > 1000u,
            apiName + " did not draw the Model Renderer wireframes ("
                + std::to_string(changedPixels)
                + " pixels changed from the solid frame; errors: " + errors
                + ")");
        return wireframe;
    }
}

int main()
{
    // AssetManagerのWIC / DirectWrite factoryと文字textureはCOMを使います。
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    int result = 0;
    try
    {
        LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);
        // D3D11のDirectXTKとD3D12の読み込みで同じcube DDSを使います。
        const auto cubeSkyBytes = BuildCubeDds({ {
            { 230u, 60u, 40u },
            { 40u, 180u, 70u },
            { 60u, 90u, 230u },
            { 230u, 210u, 50u },
            { 200u, 60u, 200u },
            { 50u, 200u, 210u } } });
        RequireDdsCubeParser(cubeSkyBytes);
        const auto d3d11 = RenderCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11PostProcess = RenderPostProcessCaptures(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11AmbientOcclusion = RenderAmbientOcclusionCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11ScreenSpaceReflection =
            RenderScreenSpaceReflectionCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11CmoModel = RenderCmoModelCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11CustomScreenEffect =
            RenderCustomScreenEffectCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11ScreenEffectDepth = RenderScreenEffectDepthCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11ScreenEffectTextures =
            RenderScreenEffectAuxiliaryCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11ComputeEffect = RenderComputeEffectCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer,
            false);
        const auto d3d11ComputeEffectInputs = RenderComputeEffectCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer,
            true);
        const auto d3d11MaterialShaders = RenderMaterialShaderCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11MaterialShaderInstancing =
            RenderMaterialShaderInstancingCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11BuiltInInstancing = RenderBuiltInInstancingCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11CustomParticleShaders =
            RenderCustomParticleShaderCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11SkinnedMaterialShaders =
            RenderSkinnedMaterialShaderCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11Sky = RenderSkyCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11CubemapSky = RenderCubemapSkyCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer,
            cubeSkyBytes);
        const auto d3d11Fog = RenderFogCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11DirectLighting = RenderDirectLightingCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11ClusteredLighting = RenderClusteredLightingCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11BakedGlobalIllumination =
            RenderBakedGlobalIlluminationCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11ReflectionProbes = RenderReflectionProbeCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11CmoMaterialShaders = RenderCmoMaterialShaderCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11Tessellation = RenderTessellationCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11CmoOutline = RenderCmoOutlineCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        const auto d3d11Wireframe = RenderWireframeCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);

        // D3D12側だけdebug layerを有効にし、描画中のvalidation errorを
        // 描画結果の一致とは別に検出します。
        LamaPon::Logger::Instance().Clear();
        LamaPon::GraphicsDevice::SetEnableDebugLayer(true);
        const auto d3d12 = RenderCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        const auto d3d12PostProcess = RenderPostProcessCaptures(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        const auto d3d12AmbientOcclusion = RenderAmbientOcclusionCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        const auto d3d12ScreenSpaceReflection =
            RenderScreenSpaceReflectionCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        const auto d3d12CmoModel = RenderCmoModelCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        const auto d3d12CustomScreenEffect =
            RenderCustomScreenEffectCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        const auto d3d12ScreenEffectDepth = RenderScreenEffectDepthCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        const auto d3d12ScreenEffectTextures =
            RenderScreenEffectAuxiliaryCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        const auto d3d12ComputeEffect = RenderComputeEffectCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap,
            false);
        const auto d3d12ComputeEffectInputs = RenderComputeEffectCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap,
            true);
        const auto d3d12MaterialShaders = RenderMaterialShaderCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12MaterialShaderInstancing =
            RenderMaterialShaderInstancingCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        const auto d3d12BuiltInInstancing = RenderBuiltInInstancingCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12CustomParticleShaders =
            RenderCustomParticleShaderCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        const auto d3d12SkinnedMaterialShaders =
            RenderSkinnedMaterialShaderCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        const auto d3d12Sky = RenderSkyCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12CubemapSky = RenderCubemapSkyCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap,
            cubeSkyBytes);
        const auto d3d12Fog = RenderFogCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12DirectLighting = RenderDirectLightingCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12ClusteredLighting = RenderClusteredLightingCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12BakedGlobalIllumination =
            RenderBakedGlobalIlluminationCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        const auto d3d12ReflectionProbes = RenderReflectionProbeCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12CmoMaterialShaders = RenderCmoMaterialShaderCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12Tessellation = RenderTessellationCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12CmoOutline = RenderCmoOutlineCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        const auto d3d12Wireframe = RenderWireframeCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        RequireD3D12AutoExposure();
        RequireD3D12DdsTextures();
        RequireD3D12PrimitiveScene();
        RequireD3D12AmbientOcclusion();
        RequireD3D12ScreenSpaceReflection();
        RequireD3D12OffscreenTarget();
        RequireD3D12DirectionalShadows();
        RequireD3D12SpotShadows();
        RequireD3D12PointShadows();
        RequireD3D12PointAndSpotLights();
        RequireD3D12MaterialFactors();
        RequireD3D12AnimatedGltfModel();
        RequireD3D12AnimatedFbxModel();
        RequireD3D12CmoModel();
        RequireD3D12SdkmeshModel();
        RequireD3D12VboModel();
        RequireD3D12Particles();
        LamaPon::GraphicsDevice::SetEnableDebugLayer(false);
        RequireNoD3D12DebugErrors();
        // compile errorの診断はdebug layerの検査と分けて確かめます。
        RequireD3D12CustomShaderFailures();
        RequireMatchingCaptures(d3d11, d3d12);
        RequireMatchingPostProcessCaptures(
            d3d11PostProcess,
            d3d12PostProcess);
        RequireMatchingAmbientOcclusionCaptures(
            d3d11AmbientOcclusion,
            d3d12AmbientOcclusion);
        RequireMatchingFrameCaptures(
            "screen-space reflections",
            d3d11ScreenSpaceReflection,
            d3d12ScreenSpaceReflection);
        RequireMatchingFrameCaptures(
            "CMO model",
            d3d11CmoModel,
            d3d12CmoModel);
        RequireMatchingFrameCaptures(
            "custom screen effect",
            d3d11CustomScreenEffect,
            d3d12CustomScreenEffect);
        RequireMatchingFrameCaptures(
            "screen effect depth",
            d3d11ScreenEffectDepth,
            d3d12ScreenEffectDepth);
        RequireMatchingFrameCaptures(
            "screen effect textures",
            d3d11ScreenEffectTextures,
            d3d12ScreenEffectTextures);
        RequireMatchingFrameCaptures(
            "compute effect",
            d3d11ComputeEffect,
            d3d12ComputeEffect);
        RequireMatchingFrameCaptures(
            "compute effect inputs",
            d3d11ComputeEffectInputs,
            d3d12ComputeEffectInputs);
        RequireMatchingFrameCaptures(
            "material shaders",
            d3d11MaterialShaders.frame,
            d3d12MaterialShaders.frame);
        RequireMatchingFrameCaptures(
            "material shader instancing",
            d3d11MaterialShaderInstancing,
            d3d12MaterialShaderInstancing);
        RequireMatchingFrameCaptures(
            "built-in Lit instancing",
            d3d11BuiltInInstancing,
            d3d12BuiltInInstancing);
        RequireMatchingFrameCaptures(
            "custom particle shaders",
            d3d11CustomParticleShaders,
            d3d12CustomParticleShaders);
        RequireMatchingFrameCaptures(
            "skinned material shader baseline",
            d3d11SkinnedMaterialShaders.baseline,
            d3d12SkinnedMaterialShaders.baseline);
        RequireMatchingFrameCaptures("sky", d3d11Sky, d3d12Sky);
        RequireMatchingFrameCaptures(
            "cubemap sky",
            d3d11CubemapSky,
            d3d12CubemapSky);
        RequireMatchingFrameCaptures("fog", d3d11Fog, d3d12Fog);
        RequireMatchingFrameCaptures(
            "direct lighting",
            d3d11DirectLighting,
            d3d12DirectLighting);
        RequireMatchingFrameCaptures(
            "clustered lighting",
            d3d11ClusteredLighting,
            d3d12ClusteredLighting);
        RequireMatchingFrameCaptures(
            "baked global illumination",
            d3d11BakedGlobalIllumination,
            d3d12BakedGlobalIllumination);
        RequireMatchingFrameCaptures(
            "reflection probes",
            d3d11ReflectionProbes,
            d3d12ReflectionProbes);
        RequireMatchingFrameCaptures(
            "CMO material shaders",
            d3d11CmoMaterialShaders,
            d3d12CmoMaterialShaders);
        RequireMatchingFrameCaptures(
            "tessellation shaders",
            d3d11Tessellation,
            d3d12Tessellation);
        RequireMatchingFrameCaptures(
            "CMO outline and occluded passes",
            d3d11CmoOutline,
            d3d12CmoOutline);
        RequireMatchingFrameCaptures(
            "model wireframes",
            d3d11Wireframe,
            d3d12Wireframe);
        std::cout << "D3D12 sprite rendering tests passed.\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    LamaPon::GraphicsDevice::SetEnableDebugLayer(false);
    LamaPon::GraphicsDevice::SetPreferWarpAdapter(false);
    if (uninitialize)
    {
        CoUninitialize();
    }
    return result;
}
