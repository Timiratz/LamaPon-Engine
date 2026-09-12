#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Components/CameraComponent.h"
#include "LamaPon/Components/DirectionalLightComponent.h"
#include "LamaPon/Components/MeshRendererComponent.h"
#include "LamaPon/Components/ModelRendererComponent.h"
#include "LamaPon/Components/ParticleSystemComponent.h"
#include "LamaPon/Components/PointLightComponent.h"
#include "LamaPon/Components/SpotLightComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/RenderTarget.h"
#include "LamaPon/Graphics/ShadowMap.h"
#include "LamaPon/Graphics/SkeletalModel.h"
#include "LamaPon/Graphics/SpriteRendering.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scene/SceneManager.h"
#include "LamaPon/Scene/GameObject.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
    }

    void RequireD3D12CustomShaderFallback(
        LamaPon::GraphicsDevice& graphics)
    {
        LamaPon::SpritePassDescription description;
        description.pixelShader = "shaders/LamaPonSpriteLit.hlsl";
        auto pass = graphics.BeginSpritePass(description);
        const auto status = pass.ShaderStatus();
        Require(
            status.fallback
                    == LamaPon::SpriteShaderFallback::DefaultPipeline
                && !status.error.empty(),
            "A DirectX 12 custom sprite shader did not report its default "
            "pipeline fallback");
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
            RequireD3D12CustomShaderFallback(graphics);
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
            0.7f, 0.15f, 0.05f, 1.0f };
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
        Require(
            compositionWidth == CanvasWidth
                && compositionHeight == CanvasHeight
                && compositionPixels[compositionOffset] > 150u
                && compositionPixels[compositionOffset + 1u] < 70u
                && compositionPixels[compositionOffset + 2u] < 40u,
            "The DirectX 12 main scene composition was not copied to the "
            "back buffer");
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

        Require(
            brightnessAt({ -0.8f, 0.0f, 0.05f }) > 200u,
            "The DirectX 12 point light did not light the wall");
        Require(
            brightnessAt({ 0.9f, 0.0f, 0.05f }) > 180u,
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
        const auto d3d11 = RenderCapture(
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
        RequireD3D12PrimitiveScene();
        RequireD3D12OffscreenTarget();
        RequireD3D12DirectionalShadows();
        RequireD3D12SpotShadows();
        RequireD3D12PointShadows();
        RequireD3D12PointAndSpotLights();
        RequireD3D12MaterialFactors();
        RequireD3D12AnimatedGltfModel();
        RequireD3D12AnimatedFbxModel();
        RequireD3D12Particles();
        LamaPon::GraphicsDevice::SetEnableDebugLayer(false);
        RequireNoD3D12DebugErrors();
        RequireMatchingCaptures(d3d11, d3d12);
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
