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
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/UIRectTransformComponent.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
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
#include "../packages/src/scene-transition-showcase/SceneTransitionAssets.h"

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
    // 2の累乗値を使い、D3D11/12のviewport変換を一致させます。
    // CanvasWidth: 描画canvasの横幅。
    constexpr std::uint32_t CanvasWidth = 256u;
    // CanvasHeight: 描画canvasの高さ。
    constexpr std::uint32_t CanvasHeight = 128u;

    // Require(condition: 判定, message: 失敗理由): 条件が偽なら検証を失敗させます。
    void Require(const bool condition, const std::string& message)
    {
        // 期待を満たさない場合は失敗理由を送出します。
        if (!condition)
        {
            // 条件不成立をテスト失敗として通知します。
            throw std::runtime_error(message);
        }
    }

    // MakeFourCc(a: 1文字目, b: 2文字目, c: 3文字目, d: 4文字目): 4文字コードをlittle-endian値にします。
    constexpr std::uint32_t MakeFourCc(
        const char a,
        const char b,
        const char c,
        const char d) noexcept
    {
        // 4文字を32bit little-endian FourCCへまとめて返します。
        return static_cast<std::uint8_t>(a)
            | static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(b)) << 8u
            | static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(c)) << 16u
            | static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(d)) << 24u;
    }

    // WriteLittleEndian32(bytes: 出力buffer, offset: byte位置, value: 書込値): 32bit値を書き込みます。
    void WriteLittleEndian32(
        std::vector<std::uint8_t>& bytes,
        const std::size_t offset,
        const std::uint32_t value)
    {
        Require(
            offset <= bytes.size()
                && sizeof(value) <= bytes.size() - offset,
            "The DDS test header offset is invalid");
        // bytesへ32bit値をlittle-endianで格納します。
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    // BuildClassicDds(width: 幅, height: 高さ, mipCount: mip数, fourCc: pixel形式, payload: pixel bytes): DDSを組み立てます。
    [[nodiscard]] std::vector<std::uint8_t> BuildClassicDds(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::uint32_t mipCount,
        const std::uint32_t fourCc,
        const std::vector<std::uint8_t>& payload)
    {
        // bytes: DDS headerとpayloadの出力buffer。
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
        // 構築したfixture byte列を返します。
        return bytes;
    }

    // BuildLegacyDds(flags: pixel flags, bitCount: 色bit数, redMask: 赤mask, greenMask: 緑mask, blueMask: 青mask, alphaMask: alpha mask, payload: pixel bytes): mask形式DDSを組み立てます。
    [[nodiscard]] std::vector<std::uint8_t> BuildLegacyDds(
        const std::uint32_t flags,
        const std::uint32_t bitCount,
        const std::uint32_t redMask,
        const std::uint32_t greenMask,
        const std::uint32_t blueMask,
        const std::uint32_t alphaMask,
        const std::vector<std::uint8_t>& payload)
    {
        // bytes: DDS headerとpayloadの出力buffer。
        std::vector<std::uint8_t> bytes(128u);
        WriteLittleEndian32(bytes, 0u, MakeFourCc('D', 'D', 'S', ' '));
        WriteLittleEndian32(bytes, 4u, 124u);
        WriteLittleEndian32(bytes, 12u, 4u);
        WriteLittleEndian32(bytes, 16u, 4u);
        WriteLittleEndian32(bytes, 28u, 1u);
        WriteLittleEndian32(bytes, 76u, 32u);
        WriteLittleEndian32(bytes, 80u, flags);
        WriteLittleEndian32(bytes, 88u, bitCount);
        WriteLittleEndian32(bytes, 92u, redMask);
        WriteLittleEndian32(bytes, 96u, greenMask);
        WriteLittleEndian32(bytes, 100u, blueMask);
        WriteLittleEndian32(bytes, 104u, alphaMask);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        // 構築したfixture byte列を返します。
        return bytes;
    }

    // BuildRgbaDds(): 2 mip levelsを持つRGBA DDSを作ります。
    [[nodiscard]] std::vector<std::uint8_t> BuildRgbaDds()
    {
        // bytes: DDS headerとmip pixelの出力buffer。
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
        // pixel: base mipの0始まりpixel位置。
        for (std::size_t pixel{}; pixel < 16u; ++pixel)
        {
            bytes.insert(bytes.end(), { 220u, 40u, 20u, 255u });
        }
        // pixel: 小さいmipの0始まりpixel位置。
        for (std::size_t pixel{}; pixel < 4u; ++pixel)
        {
            bytes.insert(bytes.end(), { 20u, 40u, 220u, 255u });
        }
        // 構築したfixture byte列を返します。
        return bytes;
    }

    // BuildDx10Dds(format: DXGI形式, payload: pixel bytes): DX10拡張DDSを組み立てます。
    [[nodiscard]] std::vector<std::uint8_t> BuildDx10Dds(
        const DXGI_FORMAT format,
        const std::vector<std::uint8_t>& payload)
    {
        // bytes: DX10 headerとpayloadの出力buffer。
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
        // 構築したfixture byte列を返します。
        return bytes;
    }

    // BuildCubeDds(faceColors: +X,-X,+Y,-Y,+Z,-Z順): 6面cap付き4x4 RGBA8・1 mipのcube DDSを作ります。
    [[nodiscard]] std::vector<std::uint8_t> BuildCubeDds(
        const std::array<std::array<std::uint8_t, 3>, 6>& faceColors)
    {
        // bytes: 6面DDS headerとpixelの出力buffer。
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
        // color: 現在のcube面のRGB値。
        for (const auto& color : faceColors)
        {
            // pixel: 現在の面の0始まり位置。
            for (std::size_t pixel{}; pixel < 16u; ++pixel)
            {
                bytes.insert(
                    bytes.end(),
                    { color[0], color[1], color[2], 255u });
            }
        }
        // 構築したfixture byte列を返します。
        return bytes;
    }

    class HiddenWindow final
    {
    public:
        // HiddenWindow(width: 幅, height: 高さ): 描画test用の不可視windowを作ります。
        HiddenWindow(
            const std::uint32_t width,
            const std::uint32_t height)
            : m_instance(GetModuleHandleW(nullptr))
        {
        // 登録用window class
        WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = DefWindowProcW;
            windowClass.hInstance = m_instance;
            windowClass.lpszClassName = ClassName;
            m_class = RegisterClassExW(&windowClass);
            // 登録に失敗した場合はwindowを作れません。
            if (m_class == 0)
            {
                // 描画fixtureの失敗を例外として通知します。
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
            // 作成失敗時は登録済みclassを戻します。
            if (m_window == nullptr)
            {
                UnregisterClassW(ClassName, m_instance);
                m_class = 0;
                // 描画fixtureの失敗を例外として通知します。
                throw std::runtime_error(
                    "The sprite rendering test window could not be created");
            }
        }

        // ~HiddenWindow(): 所有するwindowとclass登録を解放します。
        ~HiddenWindow()
        {
            // window生成済みならnative handleを解放します。
            if (m_window != nullptr)
            {
                DestroyWindow(m_window);
            }
            // class登録済みならWin32から解除します。
            if (m_class != 0)
            {
                UnregisterClassW(ClassName, m_instance);
            }
        }

        // HiddenWindow(copy): native windowの所有権複製を禁止します。
        HiddenWindow(const HiddenWindow&) = delete;
        // operator=(copy): native windowの所有権代入を禁止します。
        HiddenWindow& operator=(const HiddenWindow&) = delete;

        // Get(): 所有するnative window handleを返します。
        [[nodiscard]] HWND Get() const noexcept
        {
            // 保持中のnative window handleを返します。
            return m_window;
        }

    private:
        // Win32へ登録する固有class名。
        static constexpr const wchar_t* ClassName =
            L"LamaPonD3D12SpriteRenderingTests";

        // 登録元module。
        HINSTANCE m_instance{};
        // 登録済みclass識別子。
        ATOM m_class{};
        // 所有するnative window。
        HWND m_window{};
    };

    struct Capture final
    {
        // width: capture画像の横幅。
        std::uint32_t width{};
        // height: capture画像の高さ。
        std::uint32_t height{};
        // pixels: RGBA画像のbyte列。
        std::vector<std::uint8_t> pixels;
    };

    // Premultiplied(color: 元のRGBA色): RGB各channelへalphaを乗算します。
    [[nodiscard]] DirectX::XMFLOAT4 Premultiplied(
        const DirectX::XMFLOAT4& color) noexcept
    {
        // alphaでpremultiplyしたRGBA色を返します。
        return {
            color.x * color.w,
            color.y * color.w,
            color.z * color.w,
            color.w
        };
    }

    // DrawRectangle(pass: sprite pass, x: 左位置, y: 上位置, width: 幅, height: 高さ, color: RGBA色): 矩形spriteを追加します。
    void DrawRectangle(
        const LamaPon::SpriteRenderPass& pass,
        const float x,
        const float y,
        const float width,
        const float height,
        const DirectX::XMFLOAT4& color)
    {
        // request: 矩形spriteの描画設定。
        LamaPon::SpriteDrawRequest request;
        request.position = { x, y };
        request.scale = { width, height };
        request.tint = Premultiplied(color);
        Require(pass.Draw(request), "A sprite rectangle was rejected");
    }

    // DrawSpriteScene(graphics: 描画device): D3D11/12で共通のsprite描画passを検証します。
    // 文字texture、座標変換、scissor、blend、未完了pass破棄の描画結果を照合します。
    void DrawSpriteScene(LamaPon::GraphicsDevice& graphics)
    {
        // loading: 描画する読み込み画面設定。
        LamaPon::SceneLoadingScreenSettings loading;
        loading.enabled = true;
        loading.message = "Loading";
        loading.showPercentage = true;
        graphics.DrawLoadingScreen(
            0.4f,
            loading,
            CanvasWidth,
            CanvasHeight);

        // circle: 円形の内蔵texture。
        const auto circle = graphics.Assets().LoadTexture("builtin/circle");
        // circleResources: 円textureのresource lease。
        const auto circleResources = circle != nullptr
            ? circle->resources.Acquire()
            : nullptr;
        Require(
            circleResources != nullptr
                && circleResources->shaderResourceView,
            "The built-in circle texture has no shader resource view");
        // circleView: 円textureのshader resource view。
        const auto circleView = circleResources->shaderResourceView;

        {
            // pass: 最初のsprite描画pass。
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

            // rotated: source rectangleと回転を使うsprite。
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

            // whole: texture全体を使うsprite。
        LamaPon::SpriteDrawRequest whole;
            whole.texture = circleView;
            whole.position = { 150.0f, 6.0f };
            whole.origin = { 20.0f, 10.0f };
            whole.scale = { 0.18f, 0.12f };
            whole.flip = LamaPon::SpriteFlip::Both;
            whole.tint = { 1.0f, 0.8f, 0.2f, 0.8f };
            Require(pass.Draw(whole), "A whole-texture sprite was rejected");

            // mirrored: 負scaleで反転する比較用sprite。
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
            // clipped: 入れ子scissor内で描くsprite。
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

            // emptySource: 空のsource rectangleを持つrequest。
        LamaPon::SpriteDrawRequest emptySource;
            emptySource.hasSourceRectangle = true;
            emptySource.sourceRectangle = { 4, 4, 4, 8 };
            Require(
                !pass.Draw(emptySource),
                "An empty source rectangle was accepted");
            pass.End();
        }

        // blendModes: 個別に検証する描画blend mode。
        const std::array blendModes{
            LamaPon::SpriteBlendMode::Additive,
            LamaPon::SpriteBlendMode::AlphaBlend,
            LamaPon::SpriteBlendMode::Opaque
        };
        // index: blendModesの現在位置。
        for (std::size_t index{}; index < blendModes.size(); ++index)
        {
            // description: 現在のblend modeを指定するpass設定。
            LamaPon::SpritePassDescription description;
            description.blend = blendModes[index];
            // pass: 現在のblend modeで描くpass。
            auto pass = graphics.BeginSpritePass(description);
            // offset: blend modeごとの描画位置補正。
            const float offset = static_cast<float>(index) * 18.0f;
            // request: blend modeを比較するsprite。
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

        // EndせずにSpriteRenderPassを破棄しても、SpriteBatchと同じく積んだSpriteを描いてpassを閉じます。
        {
        // fallback確認pass
        auto pass = graphics.BeginSpritePass();
            DrawRectangle(
                pass,
                180.0f,
                30.0f,
                20.0f,
                20.0f,
                { 0.0f, 1.0f, 0.0f, 1.0f });
        }

        // b0のカスタム値とSV_Positionを使う同梱Shaderで、青い長方形の中央部分だけを描きます。
        // D3D11 / D3D12のキャプチャ比較に入るため、単にcompileできるだけでなく定数bindも検証できます。
        {
        // 不明Shaderのpass設定
        LamaPon::SpritePassDescription description;
            description.pixelShader =
                "shaders/LamaPonSpriteMask.hlsl";
            description.blend = LamaPon::SpriteBlendMode::Opaque;
            description.customParameters[0] = {
                0.0f, 0.0f, 0.0f, 0.0f };
            description.customParameters[1] = {
                206.0f, 24.0f, 12.0f, 7.0f };
        // fallback描画pass
        auto pass = graphics.BeginSpritePass(description);
        // Shader状態
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
        // compile失敗pass設定
        LamaPon::SpritePassDescription description;
            description.pixelShader =
                "shaders/LamaPonSpriteLit.hlsl";
            description.blend = LamaPon::SpriteBlendMode::Opaque;
            description.lighting.counts.x = 1u;
            description.lighting.lights[0].positionRadiusIntensity = {
                240.0f, 84.0f, 14.0f, 1.0f };
            description.lighting.lights[0].color = {
                0.8f, 0.15f, 0.05f, 0.0f };
        // compile失敗pass
        auto pass = graphics.BeginSpritePass(description);
        // compile失敗状態
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

    // RequireD3D12MissingCustomShaderFallback(graphics: 描画device): 未登録shaderのD3D12 fallbackを検証します。
    void RequireD3D12MissingCustomShaderFallback(
        LamaPon::GraphicsDevice& graphics)
    {
        // description: 未登録pixel shaderを指定するpass設定。
        LamaPon::SpritePassDescription description;
        description.pixelShader = "shaders/does-not-exist.hlsl";
        // pass: default shader fallbackを使う描画pass。
        auto pass = graphics.BeginSpritePass(description);
        // status: shader解決結果。
        const auto status = pass.ShaderStatus();
        Require(
            status.fallback
                    == LamaPon::SpriteShaderFallback::DefaultPipeline
                && !status.error.empty(),
            "A missing DirectX 12 custom sprite shader did not report its "
            "default pipeline fallback");
        pass.End();
    }

    // RenderCapture(api: rendering API, profile: 起動policy): 共通sprite sceneを描画しframeを撮影します。
    [[nodiscard]] Capture RenderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // window: 描画用の不可視window。
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // graphics: sprite sceneを描くdevice。
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

        // ClearColor: frameとcompositionのclear色。
        constexpr float ClearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        // capture: 1フレーム目の画像。
        Capture capture;
        graphics.BeginFrame(ClearColor);
        DrawSpriteScene(graphics);
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        // D3D12 bootstrap経路だけcustom shader fallbackを確認します。
        if (api == LamaPon::RenderingApi::DirectX12Experimental)
        {
            Require(
                graphics.IsD3D12ExperimentalBootstrap(),
                "The DirectX 12 sprite test did not activate its renderer");
            RequireD3D12MissingCustomShaderFallback(graphics);
        }
        graphics.EndFrame();

        // allocator・upload・遅延解放descriptorを跨ぐ2フレーム目の再現性を確認します。
        graphics.BeginFrame(ClearColor);
        DrawSpriteScene(graphics);
        // secondWidth: 2フレーム目の画像幅。
        std::uint32_t secondWidth{};
        // secondHeight: 2フレーム目の画像高さ。
        std::uint32_t secondHeight{};
        // secondPixels: 2フレーム目のRGBA画像。
        const auto secondPixels = graphics.CaptureBackBuffer(
            secondWidth,
            secondHeight);
        graphics.EndFrame();
        Require(
            secondWidth == capture.width
                && secondHeight == capture.height
                && secondPixels == capture.pixels,
            "A repeated sprite frame did not reproduce the first frame");
        // 撮影したframe captureを返します。
        return capture;
    }

    // RenderCustomScreenEffectCapture(api: rendering API, profile: 起動policy): tone mapping後のeffectを撮影します。
    // D3D11と同じVSMain/PSMain、b0の値、t0のscene色で描きます。
    [[nodiscard]] Capture RenderCustomScreenEffectCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // window: effect描画用の不可視window。
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // graphics: screen effectを描くdevice。
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        // D3D11の合成はEnvironment shaderをasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);

        // clearColor: scene compositionのclear色。
        constexpr float clearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        {
            // pass: effect前にscene色を描くpass。
        auto pass = graphics.BeginSpritePass();
            DrawRectangle(
                pass,
                24.0f,
                20.0f,
                80.0f,
                48.0f,
                { 0.15f, 0.35f, 0.7f, 1.0f });
        }

        // request: queueするcustom screen effect。
        LamaPon::ScreenEffectRequest request;
        request.shader =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/bright-dot.hlsl";
        request.customParameters[0] = {
            0.08f, 0.75f, 0.0f, 0.0f };
        request.point = LamaPon::ScreenEffectPoint::AfterToneMapping;
        // generation: queueされたshader generation。
        std::uint64_t generation{};
        // error: effect登録時の診断内容。
        std::string error;
        Require(
            graphics.QueueScreenEffect(request, &generation, &error)
                && generation != 0
                && error.empty(),
            "The custom screen effect was not queued: " + error);

        // frame: scene compositionを終了するpost-process設定。
        LamaPon::PostProcessFrame frame;
        graphics.EndSceneComposition(frame);
        // capture: effect適用後の画面。
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        // center: 画像中央のRGBA byte index。
        const std::size_t center =
            (static_cast<std::size_t>(CanvasHeight / 2u) * CanvasWidth
                + CanvasWidth / 2u) * 4u;
        // describeCenter(): 画像中央pixelのRGBを文字列で返します。
        const auto describeCenter = [&capture, center]()
        {
            // capture中央pixelの表示文字列を返します。
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // RequireNoD3D12DebugErrors(): D3D12 debug layerのerror記録を検査します。
    void RequireNoD3D12DebugErrors()
    {
        // entry: 確認中のlogger entry。
        for (const auto& entry : LamaPon::Logger::Instance().Snapshot())
        {
            // D3D12 debug errorなら失敗として通知します。
            if (entry.level == LamaPon::LogLevel::Error
                && entry.message.starts_with("D3D12:"))
            {
                // DirectX 12 debug layerのerrorをテスト失敗として通知します。
                throw std::runtime_error(
                    "The DirectX 12 debug layer reported an error: "
                    + entry.message);
            }
        }
    }

    // RequireD3D12OffscreenTarget(): offscreen描画、history view、camera textureを検証します。
    void RequireD3D12OffscreenTarget()
    {
        // window: 描画用の不可視window。
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // graphics: offscreen描画を行うdevice。
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);

        // targetWidth: offscreen targetの横幅。
        constexpr std::uint32_t targetWidth = 64u;
        // targetHeight: offscreen targetの高さ。
        constexpr std::uint32_t targetHeight = 32u;
        // target: view更新を検証するoffscreen target。
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

        // backBufferClear: swapchain画像のclear色。
        constexpr float backBufferClear[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(backBufferClear);
        // primaryOutput: frame開始時のoutput state。
        auto primaryOutput = graphics.CaptureOutputState();

        // offscreenClear: offscreen targetのclear色。
        constexpr float offscreenClear[4]{ 0.05f, 0.1f, 0.8f, 1.0f };
        graphics.BeginOffscreenTarget(target, offscreenClear);
        // offscreenOutput: offscreen描画時のoutput state。
        auto offscreenOutput = graphics.CaptureOutputState();
        graphics.BindOffscreenTargetDepthOnly(target);
        graphics.CaptureOffscreenTargetDepth(target);
        graphics.RestoreOutputState(*offscreenOutput);
        {
            // pass: offscreen targetへ色を描くpass。
        auto pass = graphics.BeginSpritePass();
            DrawRectangle(
                pass,
                10.0f,
                8.0f,
                20.0f,
                12.0f,
                { 0.9f, 0.05f, 0.02f, 1.0f });
        }
        // History view matrix.
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
        // View test pass.
        auto pass = graphics.BeginSpritePass();
        // Display-view sprite.
        LamaPon::SpriteDrawRequest request;
            request.texture = target.DisplayViewHandle();
            request.position = { 20.0f, 15.0f };
            request.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(request),
                "The DirectX 12 offscreen display view was rejected");

        // Depth-view sprite.
        LamaPon::SpriteDrawRequest depthRequest;
            depthRequest.texture = target.DepthViewHandle();
            depthRequest.position = { 100.0f, 15.0f };
            depthRequest.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(depthRequest),
                "The DirectX 12 offscreen depth view was rejected");

        // Color-history view.
        LamaPon::SpriteDrawRequest colorHistoryRequest;
            colorHistoryRequest.texture = target.ColorHistoryViewHandle();
            colorHistoryRequest.position = { 170.0f, 15.0f };
            colorHistoryRequest.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(colorHistoryRequest),
                "The DirectX 12 color history view was rejected");

        // Temporal-history view.
        LamaPon::SpriteDrawRequest temporalHistoryRequest;
            temporalHistoryRequest.texture =
                target.TemporalHistoryViewHandle();
            temporalHistoryRequest.position = { 100.0f, 55.0f };
            temporalHistoryRequest.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(temporalHistoryRequest),
                "The DirectX 12 temporal history view was rejected");
        }
        // Capture width.
        std::uint32_t width{};
        // Capture height.
        std::uint32_t height{};
        // RGBA pixels.
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 offscreen composition capture has unexpected "
            "dimensions");

        // Reads an RGBA pixel.
        // pixel(x: 横位置, y: 縦位置)
        const auto pixel = [&pixels, width](
            const std::uint32_t x,
            const std::uint32_t y)
        {
            // Pixel byte offset.
            const auto offset = (
                static_cast<std::size_t>(y) * width + x) * 4u;
            // pixelのRGBA channel値を返します。
            return std::array<std::uint8_t, 4>{
                pixels[offset],
                pixels[offset + 1u],
                pixels[offset + 2u],
                pixels[offset + 3u] };
        };
        // Clear sample.
        const auto blue = pixel(24u, 18u);
        // Drawn target pixel.
        const auto red = pixel(35u, 28u);
        // Outside sample.
        const auto outside = pixel(90u, 70u);
        // Depth-view pixel.
        const auto depth = pixel(105u, 20u);
        // Color-history pixel.
        const auto colorHistory = pixel(174u, 18u);
        // Temporal sample.
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

        // Camera scene.
        LamaPon::Scene scene(graphics);
        // Camera object.
        auto& cameraObject = scene.CreateGameObject("RenderTextureCamera");
        // Camera component.
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        camera.SetTargetTexture("d3d12-camera-target");
        camera.SetTargetTextureSize(40u, 20u);
        camera.SetTargetClearColor({ 0.05f, 0.8f, 0.1f, 1.0f });

        graphics.BeginFrame(backBufferClear);
        scene.RenderTargetTextures();
        // Camera output view.
        const auto cameraView = graphics.RenderTextureViewHandle(
            "d3d12-camera-target");
        Require(
            cameraView
                && graphics.IsGraphicsViewCurrent(cameraView),
            "The DirectX 12 camera did not publish its render texture");
        {
            // Camera-view pass.
            auto pass = graphics.BeginSpritePass();
            // Camera-view sprite.
            LamaPon::SpriteDrawRequest request;
            request.texture = cameraView;
            request.position = { 5.0f, 5.0f };
            request.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(request),
                "The DirectX 12 camera render texture was rejected");
        }
        // Camera capture width.
        std::uint32_t cameraWidth{};
        // Camera capture height.
        std::uint32_t cameraHeight{};
        // Camera capture pixels.
        const auto cameraPixels = graphics.CaptureBackBuffer(
            cameraWidth,
            cameraHeight);
        graphics.EndFrame();
        // Camera offset.
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

        // HDR test color.
        constexpr float compositionClear[4]{
            2.0f, 0.25f, 0.0f, 1.0f };
        graphics.BeginFrame(backBufferClear);
        graphics.BeginSceneComposition(compositionClear);
        scene.RenderMainCamera(
            graphics.AspectRatio(),
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        // Composition width.
        std::uint32_t compositionWidth{};
        // Composition height.
        std::uint32_t compositionHeight{};
        // Composition pixels.
        const auto compositionPixels = graphics.CaptureBackBuffer(
            compositionWidth,
            compositionHeight);
        graphics.EndFrame();
        // HDR sample offset.
        const auto compositionOffset = (
            static_cast<std::size_t>(64u) * compositionWidth + 128u)
            * 4u;
        // 既定のカラーグレーディング（露出0.15、コントラスト1.05、彩度1.08、色温度0.02）とACESをD3D11のPSToneMapと同じ式で掛けると、(2.0, 0.25, 0.0)はgamma変換なしで約(250, 101, 0)になります。
        Require(
            compositionWidth == CanvasWidth
                && compositionHeight == CanvasHeight
                && compositionPixels[compositionOffset] > 240u
                && compositionPixels[compositionOffset + 1u] > 90u
                && compositionPixels[compositionOffset + 1u] < 115u
                && compositionPixels[compositionOffset + 2u] < 8u,
            "The DirectX 12 HDR scene was not tone-mapped to the back "
            "buffer");

        // Low-exposure frame.
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
        // Low-exposure pixels.
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

        // Un-tonemapped frame.
        auto untonemappedFrame = lowExposureFrame;
        untonemappedFrame.colorGrading.toneMappingEnabled = false;
        graphics.BeginFrame(backBufferClear);
        graphics.BeginSceneComposition(compositionClear);
        scene.RenderMainCamera(
            graphics.AspectRatio(),
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(untonemappedFrame);
        // Raw pixels.
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

        // Bloomはトーンマップの前にHDRのまま掛かります。
        // 均一な(2.0, 0.25, 0.0)でも高輝度分が足され、トーンマップ後の緑が約101から約126へ上がります。
        // Bloom検証フレーム
        auto bloomFrame = scene.PostProcessFrameData();
        bloomFrame.bloom.enabled = true;
        graphics.BeginFrame(backBufferClear);
        graphics.BeginSceneComposition(compositionClear);
        scene.RenderMainCamera(
            graphics.AspectRatio(),
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(bloomFrame);
        // Bloom pixels.
        const auto bloomPixels = graphics.CaptureBackBuffer(
            compositionWidth,
            compositionHeight);
        graphics.EndFrame();
        Require(
            bloomPixels[compositionOffset + 1u]
                > compositionPixels[compositionOffset + 1u] + 15u,
            "The DirectX 12 scene composition did not apply bloom");
    }

    // Verifies primitive meshes, outlines, and depth-of-field composition.
    void RequireD3D12PrimitiveScene()
    {
        // D3D12 test window.
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Test device.
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // Shadow settings.
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
        // Primitive scene.
        LamaPon::Scene scene(graphics);
        // Main camera object.
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        // Camera component.
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 0.2f, 0.25f, 0.35f });
        scene.SetAmbientLightIntensity(0.2f);
        // Sun light.
        auto& lightObject = scene.CreateGameObject("Sun");
        lightObject.AddComponent<LamaPon::DirectionalLightComponent>();

        // Primitive shape set.
        const std::array shapes{
            LamaPon::PrimitiveShape::Cube,
            LamaPon::PrimitiveShape::Sphere,
            LamaPon::PrimitiveShape::Cylinder,
            LamaPon::PrimitiveShape::Plane };
        // Create one scene object for each primitive shape.
        for (std::size_t index{}; index < shapes.size(); ++index)
        {
            // Primitive object.
            auto& object = scene.CreateGameObject("Primitive");
            object.GetTransform().position = {
                -1.35f + static_cast<float>(index) * 0.9f,
                0.0f,
                0.0f };
            object.GetTransform().scale = { 0.7f, 0.7f, 0.7f };
            // Rotate the plane so its front faces the camera.
            if (shapes[index] == LamaPon::PrimitiveShape::Plane)
            {
                object.GetTransform().SetEulerAngles(
                    DirectX::XM_PIDIV2, 0.0f, 0.0f);
            }
            object.AddComponent<LamaPon::MeshRendererComponent>(
                shapes[index],
                DirectX::XMFLOAT4{ 0.95f, 0.2f, 0.12f, 1.0f });
        }

        // Procedural object.
        auto& proceduralObject = scene.CreateGameObject("Procedural");
        proceduralObject.GetTransform().position = { 0.0f, 0.85f, 0.0f };
        // Procedural renderer.
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

        // Frame clear color.
        constexpr float clearColor[4]{ 0.02f, 0.03f, 0.05f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        // Capture width.
        std::uint32_t width{};
        // Frame height.
        std::uint32_t height{};
        // Primitive pixels.
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
        // Visible pixel count.
        std::size_t coloredPixels{};
        // 描画結果の色画素を集計
        for (std::size_t offset{}; offset + 3u < pixels.size(); offset += 4u)
        {
            // Count visible red or green primitive pixels.
            if (pixels[offset] > 45u || pixels[offset + 1u] > 45u)
            {
                ++coloredPixels;
            }
        }
        Require(
            coloredPixels > 300u,
            "The DirectX 12 scene did not render its primitive meshes");

        // 3D合成後の画像を取得します。
        // 合成先の深度を使う輪郭描画も、同じ経路で検証します。
        const auto captureComposition = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 合成画像の幅
            std::uint32_t captureWidth{};
            // 合成画像の高さ
            std::uint32_t captureHeight{};
            // 合成後のRGBA画像
            auto capture = graphics.CaptureBackBuffer(
                captureWidth,
                captureHeight);
            graphics.EndFrame();
            Require(
                captureWidth == CanvasWidth
                    && captureHeight == CanvasHeight,
                "The DirectX 12 scene composition capture has unexpected "
                "dimensions");
            // 撮影したframe captureを返します。
            return capture;
        };

        // 輪郭処理前のRGBA画像
        const auto compositionPixels = captureComposition();
        // 合成先の3D画素数
        std::size_t composedGeometryPixels{};
        // RGBA画素の先頭位置
        for (std::size_t offset{};
             offset + 3u < compositionPixels.size();
             offset += 4u)
        {
            // 合成された形状画素を集計
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

        // 輪郭描画の設定。
        auto outline = scene.ScreenOutline();
        outline.enabled = true;
        outline.color = { 0.0f, 1.0f, 0.0f };
        outline.intensity = 1.0f;
        outline.thickness = 2.0f;
        outline.depthThreshold = 0.01f;
        outline.normalThreshold = 0.1f;
        scene.SetScreenOutlineSettings(outline);
        // 輪郭処理後のRGBA画像
        const auto outlinedPixels = captureComposition();
        // 緑色の輪郭画素数
        std::size_t greenOutlinePixels{};
        // RGBA画素の先頭位置
        for (std::size_t offset{};
             offset + 3u < outlinedPixels.size();
             offset += 4u)
        {
            // 確認画素の緑成分
            const auto green = outlinedPixels[offset + 1u];
            // Count pixels where the outline adds a green edge.
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
        // Depth-of-field settings.
        auto depthOfField = scene.DepthOfField();
        depthOfField.enabled = true;
        depthOfField.focusDistance = 20.0f;
        depthOfField.focusRange = 0.0f;
        depthOfField.blurStrength = 8.0f;
        depthOfField.maximumRadius = 8.0f;
        scene.SetDepthOfFieldSettings(depthOfField);
        // Post-process settings.
        auto postProcessSettings = graphics.Settings();
        postProcessSettings.depthOfFieldEnabled = true;
        postProcessSettings.depthOfFieldSampleCount = 16u;
        graphics.SetGraphicsSettings(postProcessSettings);
        // Depth-of-field pixels.
        const auto depthOfFieldPixels = captureComposition();
        // Changed pixel count.
        std::size_t depthOfFieldChangedPixels{};
        // RGB difference.
        std::uint64_t depthOfFieldDifference{};
        // Compare each captured pixel with the unblurred frame.
        for (std::size_t offset{};
             offset + 3u < depthOfFieldPixels.size();
             offset += 4u)
        {
                // Pixel RGB difference.
            int difference{};
            // Sum differences across the three color channels.
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                difference += std::abs(
                    static_cast<int>(depthOfFieldPixels[offset + channel])
                    - static_cast<int>(compositionPixels[offset + channel]));
            }
            // Count pixels with a visible blur difference.
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

    // Verifies directional shadowing and its volumetric-light composition.
    void RequireD3D12DirectionalShadows()
    {
        // D3D12 test window.
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Shadow test device.
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        // Shadow map settings.
        auto settings = graphics.Settings();
        settings.shadowResolution = 256u;
        settings.shadowCascadeLimit = 1u;
        graphics.SetGraphicsSettings(settings);

        // Shadow scene.
        LamaPon::Scene scene(graphics);
        // Main camera object.
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        // Shadow camera.
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.08f);

        // Receiver wall.
        auto& wall = scene.CreateGameObject("ShadowReceiver");
        wall.GetTransform().scale = { 4.0f, 2.2f, 0.1f };
        wall.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });

        // Shadow caster cube.
        auto& blocker = scene.CreateGameObject("ShadowCaster");
        blocker.GetTransform().position = { 0.2f, 0.0f, 1.15f };
        blocker.GetTransform().scale = { 0.7f, 0.7f, 0.7f };
        blocker.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.55f, 0.55f, 0.55f, 1.0f });

        // Shadow light object.
        auto& lightObject = scene.CreateGameObject("ShadowSun");
        lightObject.GetTransform().SetEulerAngles(0.0f, 0.65f, 0.0f);
        // Directional light.
        auto& light = lightObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        light.SetShadowCascadeCount(1u);
        light.SetShadowBias(0.0005f);
        light.SetShadowNormalBias(0.001f);
        light.SetShadowStrength(1.0f);

        // Shadow clear color.
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // Shadow capture.
        const auto capture = [&]()
        {
            graphics.BeginFrame(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                nullptr);
            // Capture width.
            std::uint32_t width{};
            // Captured shadow-frame height.
            std::uint32_t height{};
            // Shadow pixels.
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 shadow capture has unexpected dimensions");
            // 撮影したpixel配列を返します。
            return pixels;
        };

        // Shadowed frame.
        const auto shadowed = capture();

        // Captures volumetric scattering and verifies disabling it restores the image.
        // Composition capture.
        const auto captureComposition = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // Capture width.
            std::uint32_t width{};
            // Captured composition height.
            std::uint32_t height{};
            // Composition pixels.
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 volumetric capture has unexpected "
                "dimensions");
            // 撮影したpixel配列を返します。
            return pixels;
        };
        // Baseline pixels.
        const auto withoutVolumetric = captureComposition();
        // Volumetric settings.
        auto volumetric = scene.VolumetricLight();
        volumetric.enabled = true;
        volumetric.intensity = 4.0f;
        volumetric.sampleCount = 12u;
        volumetric.scattering = 0.0f;
        scene.SetVolumetricLightSettings(volumetric);
        // Volumetric pixels.
        const auto withVolumetric = captureComposition();
        // Bright pixel count.
        std::size_t volumetricPixels{};
        // Total brightening.
        std::uint64_t volumetricBrightening{};
        // RGB difference.
        std::uint64_t totalVolumetricDifference{};
        // Max RGB difference.
        int maximumVolumetricDifference{};
        // Compare enabled and disabled volumetric captures.
        for (std::size_t offset{};
            offset + 3u < withVolumetric.size();
            offset += 4u)
        {
                // Pixel RGB difference.
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
            // Record pixels brightened by volumetric scattering.
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
        // Unshadowed frame.
        const auto unshadowed = capture();
        // Dark pixel count.
        std::size_t darkerPixels{};
        // Total darkening.
        std::uint64_t totalDarkening{};
        // Compare shadowed and unshadowed pixel brightness.
        for (std::size_t offset{};
            offset + 3u < shadowed.size();
            offset += 4u)
        {
            // Shadowed brightness.
            const auto shadowedBrightness = std::max({
                shadowed[offset],
                shadowed[offset + 1u],
                shadowed[offset + 2u] });
            // Unshadowed max.
            const auto unshadowedBrightness = std::max({
                unshadowed[offset],
                unshadowed[offset + 1u],
                unshadowed[offset + 2u] });
            // Count pixels visibly darkened by the shadow.
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
        // No-shadow frame.
        const auto globallyDisabled = capture();
        Require(
            globallyDisabled == unshadowed
                && !graphics.Shadows().IsValid(),
            "Disabling DirectX 12 shadows did not preserve the unshadowed "
            "rendering path");
    }

    // Verifies that spot-light shadows affect the receiver.
    void RequireD3D12SpotShadows()
    {
        // D3D12 test window.
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Spot-shadow device.
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        // Shadow map settings.
        auto settings = graphics.Settings();
        settings.shadowResolution = 256u;
        graphics.SetGraphicsSettings(settings);

        // Spot-shadow scene.
        LamaPon::Scene scene(graphics);
        // Main camera object.
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        // Spot-shadow camera.
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.03f);

        // Spot receiver.
        auto& wall = scene.CreateGameObject("SpotShadowReceiver");
        wall.GetTransform().scale = { 4.0f, 2.2f, 0.1f };
        wall.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.85f, 0.85f, 0.85f, 1.0f });

        // Spot-light blocker.
        auto& blocker = scene.CreateGameObject("SpotShadowCaster");
        blocker.GetTransform().position = { 0.4f, 0.0f, 1.35f };
        blocker.GetTransform().scale = { 0.65f, 0.65f, 0.65f };
        blocker.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.5f, 0.5f, 0.5f, 1.0f });

        // Spot-light object.
        auto& lightObject = scene.CreateGameObject("ShadowSpot");
        lightObject.GetTransform().position = { 1.6f, 0.0f, 3.0f };
        lightObject.GetTransform().SetEulerAngles(0.0f, 0.49f, 0.0f);
        // Shadowed spot.
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

        // Shadow clear color.
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // Shadow capture.
        const auto capture = [&]()
        {
            graphics.BeginFrame(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                nullptr);
            // Capture width.
            std::uint32_t width{};
            // Frame height.
            std::uint32_t height{};
            // Shadow pixels.
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 spot shadow capture has unexpected "
                "dimensions");
            // 撮影したpixel配列を返します。
            return pixels;
        };

        // Shadowed frame.
        const auto shadowed = capture();
        Require(
            graphics.Lighting().spotShadows[0].enabled
                && graphics.IsGraphicsViewCurrent(
                    graphics.Lighting().spotShadowTexture),
            "The DirectX 12 scene did not publish its spot shadow");
        light.SetCastsShadows(false);
        // Unshadowed frame.
        const auto unshadowed = capture();
        // Dark pixel count.
        std::size_t darkerPixels{};
        // Total darkening.
        std::uint64_t totalDarkening{};
        // Compare shadowed and unshadowed pixels.
        for (std::size_t offset{};
            offset + 3u < shadowed.size();
            offset += 4u)
        {
            // Shadowed brightness.
            const auto shadowedBrightness = std::max({
                shadowed[offset],
                shadowed[offset + 1u],
                shadowed[offset + 2u] });
            // Unshadowed max.
            const auto unshadowedBrightness = std::max({
                unshadowed[offset],
                unshadowed[offset + 1u],
                unshadowed[offset + 2u] });
            // Count pixels visibly darkened by the shadow.
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

    // Verifies that point-light shadows affect the receiver.
    void RequireD3D12PointShadows()
    {
        // Hidden window for the D3D12 swap chain.
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Point-shadow device.
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        // Settings controlling shadow map resolution.
        auto settings = graphics.Settings();
        settings.shadowResolution = 256u;
        graphics.SetGraphicsSettings(settings);

        // Point-shadow scene.
        LamaPon::Scene scene(graphics);
        // Main camera object.
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        // Point-shadow camera.
        auto& camera = cameraObject.AddComponent<
            LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.03f);

        // Point-shadow receiver.
        auto& wall = scene.CreateGameObject("PointShadowReceiver");
        wall.GetTransform().scale = { 4.0f, 2.2f, 0.1f };
        wall.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.85f, 0.85f, 0.85f, 1.0f });

        // Point-light blocker.
        auto& blocker = scene.CreateGameObject("PointShadowCaster");
        blocker.GetTransform().position = { 0.7f, 0.0f, 1.5f };
        blocker.GetTransform().scale = { 0.65f, 0.65f, 0.65f };
        blocker.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.5f, 0.5f, 0.5f, 1.0f });

        // Point-light object.
        auto& lightObject = scene.CreateGameObject("ShadowPoint");
        lightObject.GetTransform().position = { 1.4f, 0.0f, 3.0f };
        // Shadow-casting point light.
        auto& light = lightObject.AddComponent<
            LamaPon::PointLightComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                15.0f,
                8.0f);
        light.SetCastsShadows(true);
        light.SetShadowBias(0.0005f);
        light.SetShadowStrength(1.0f);

        // Shadow clear color.
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // Shadow capture.
        const auto capture = [&]()
        {
            graphics.BeginFrame(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                nullptr);
            // Capture width.
            std::uint32_t width{};
            // Frame height.
            std::uint32_t height{};
            // Shadow pixels.
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 point shadow capture has unexpected "
                "dimensions");
            // 撮影したpixel配列を返します。
            return pixels;
        };

        // Shadowed frame.
        const auto shadowed = capture();
        Require(
            graphics.Lighting().pointShadow.enabled
                && graphics.IsGraphicsViewCurrent(
                    graphics.Lighting().pointShadow.texture),
            "The DirectX 12 scene did not publish its point shadow");
        light.SetCastsShadows(false);
        // Unshadowed frame.
        const auto unshadowed = capture();
        // Dark pixel count.
        std::size_t darkerPixels{};
        // Total darkening.
        std::uint64_t totalDarkening{};
        // Compare shadowed and unshadowed pixels.
        for (std::size_t offset{};
            offset + 3u < shadowed.size();
            offset += 4u)
        {
            // Shadowed brightness.
            const auto shadowedBrightness = std::max({
                shadowed[offset],
                shadowed[offset + 1u],
                shadowed[offset + 2u] });
            // Unshadowed max.
            const auto unshadowedBrightness = std::max({
                unshadowed[offset],
                unshadowed[offset + 1u],
                unshadowed[offset + 2u] });
            // Count pixels visibly darkened by the shadow.
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

    // Point LightとSpot Lightが、D3D11の従来経路と同じ距離・コーン減衰で壁を照らすことを、各光源の当たる点と届かない点の画素で確かめます。
    // Draws each local light and samples lit and unlit world positions.
    void RequireD3D12PointAndSpotLights()
    {
        // D3D12 test window.
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Local-light device.
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // Local-light scene.
        LamaPon::Scene scene(graphics);
        // Main camera object.
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        // Projection camera.
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        // 環境光もDirectional Lightも置かず、局所光源だけの明るさを見ます。
        scene.SetAmbientLightIntensity(0.0f);

        // 正面（+Z面）がz=0.05にある薄い灰色の壁です。
        // Light receiver.
        auto& wall = scene.CreateGameObject("Wall");
        wall.GetTransform().scale = { 3.0f, 1.6f, 0.1f };
        wall.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });

        // Left point light.
        auto& pointObject = scene.CreateGameObject("PointLight");
        pointObject.GetTransform().position = { -0.8f, 0.0f, 0.6f };
        pointObject.AddComponent<LamaPon::PointLightComponent>(
            DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
            3.0f,
            2.0f);

        // 回転の無いSpot Lightは-Zを向くため、壁の正面へ当たります。
        // Right spot light.
        auto& spotObject = scene.CreateGameObject("SpotLight");
        spotObject.GetTransform().position = { 0.9f, 0.0f, 1.2f };
        spotObject.AddComponent<LamaPon::SpotLightComponent>(
            DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
            3.0f,
            3.0f,
            DirectX::XMConvertToRadians(8.0f),
            DirectX::XMConvertToRadians(12.0f));

        // Light clear.
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // Camera aspect ratio.
        const float aspectRatio =
            static_cast<float>(CanvasWidth) / CanvasHeight;
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(aspectRatio, false, nullptr);
        // Capture width.
        std::uint32_t width{};
        // Frame height.
        std::uint32_t height{};
        // Frame pixels.
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 local light capture has unexpected dimensions");

        // Projection matrix.
        const auto viewProjection = DirectX::XMMatrixMultiply(
            camera.ViewMatrix(),
            camera.ProjectionMatrix(aspectRatio));
        // Returns the brightest captured channel at a world-space point.
        // brightnessAt(world: world-space sample point)
        const auto brightnessAt = [&](const DirectX::XMFLOAT3& world)
        {
        // Project world point.
            const auto clip = DirectX::XMVector3TransformCoord(
                DirectX::XMLoadFloat3(&world),
                viewProjection);
        // Clamped pixel x.
            const int x = std::clamp(
                static_cast<int>(
                    (DirectX::XMVectorGetX(clip) * 0.5f + 0.5f)
                    * static_cast<float>(CanvasWidth)),
                0,
                static_cast<int>(CanvasWidth) - 1);
        // Clamped pixel y.
            const int y = std::clamp(
                static_cast<int>(
                    (0.5f - DirectX::XMVectorGetY(clip) * 0.5f)
                    * static_cast<float>(CanvasHeight)),
                0,
                static_cast<int>(CanvasHeight) - 1);
        // Sample byte offset.
            const auto offset =
                (static_cast<std::size_t>(y) * CanvasWidth
                    + static_cast<std::size_t>(x)) * 4u;
            // channelごとの差分の最大値を返します。
            return std::max({
                pixels[offset],
                pixels[offset + 1u],
                pixels[offset + 2u] });
        };

        // D3D11のLamaPonLit.hlslと同じGGXの拡散（albedo/π）なので、灰色の壁の正面は点光源で約110、スポットで約80の明るさになります。
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

    // Verifies that the D3D12 material pipeline applies emissive factors.
    void RequireD3D12MaterialFactors()
    {
        // D3D12 test window.
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Emissive device.
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // Emissive test scene.
        LamaPon::Scene scene(graphics);
        // Main camera object.
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        // Material camera.
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightIntensity(0.0f);

        // Emissive cube.
        auto& object = scene.CreateGameObject("EmissiveMesh");
        // Emissive renderer.
        auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.0f, 0.0f, 0.0f, 1.0f });
        mesh.SetRoughness(0.2f);
        mesh.SetMetallic(0.8f);
        mesh.SetEmissiveColor({ 0.05f, 0.8f, 0.15f });

        // Material clear.
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        // Capture width.
        std::uint32_t width{};
        // Captured material height.
        std::uint32_t height{};
        // Material pixels.
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 material capture has unexpected dimensions");
        // Center pixel offset.
        const auto center =
            (static_cast<std::size_t>(CanvasHeight / 2u) * CanvasWidth
                + CanvasWidth / 2u) * 4u;
        Require(
            pixels[center + 1u] > 170u
                && pixels[center] < 40u
                && pixels[center + 2u] < 70u,
            "The DirectX 12 pipeline did not apply the material emissive factor");
    }

    // Verifies textured glTF loading and animated model rendering.
    void RequireD3D12AnimatedGltfModel()
    {
        // D3D12 test window.
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Model test device.
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // Model scene.
        LamaPon::Scene scene(graphics);
        // Main camera object.
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 10.0f };
        // Model test camera.
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.9f);

        // glTF fixture path.
        const auto modelPath = std::filesystem::path(
            LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "TexturedRiggedSimple.gltf";
        // This fixture references an existing JPEG outside the model file.
        // D3D12 must retain it as a backend handle without creating a D3D11 SRV.
        // Backend model data.
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
        // Model object.
        auto& object = scene.CreateGameObject("AnimatedModel");
        // Model renderer.
        auto& model = object.AddComponent<
            LamaPon::ModelRendererComponent>(modelPath);
        model.SetAnimationPlayOnStart(false);

        // Model clear color.
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        // Capture width.
        std::uint32_t width{};
        // Initial frame height.
        std::uint32_t height{};
        // Model pixels.
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            model.AnimationCount() > 0,
            "The DirectX 12 ModelRenderer did not load the glTF animation");
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 model capture has unexpected dimensions");
        // Model pixel count.
        std::size_t modelPixels{};
        // Inspect the initial model image pixel by pixel.
        for (std::size_t offset{}; offset + 3u < pixels.size(); offset += 4u)
        {
            // Count pixels whose RGB channels differ from black.
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
        // Animated width.
        std::uint32_t animatedWidth{};
        // Animated height.
        std::uint32_t animatedHeight{};
        // Animated pixels.
        const auto animatedPixels = graphics.CaptureBackBuffer(
            animatedWidth,
            animatedHeight);
        graphics.EndFrame();
        Require(
            animatedWidth == width && animatedHeight == height,
            "The animated DirectX 12 model capture changed dimensions");
        // Changed pixel count.
        std::size_t changedPixels{};
        // Compare each pixel to the initial pose.
        for (std::size_t offset{};
            offset + 3u < pixels.size();
             offset += 4u)
        {
            // アニメーション前後の差分を検出
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

    // FBXアニメーションをD3D12で検証します。
    void RequireD3D12AnimatedFbxModel()
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 描画デバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // FBXモデルのパス
        const auto modelPath = std::filesystem::path(
            LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "AnimatedSausage.fbx";
        // 読み込み結果
        const auto asset = graphics.Assets().LoadModel(modelPath);
        Require(
            asset != nullptr
                && asset->skeletalModel != nullptr
                && asset->skeletalModel->hasLocalBounds
                && !asset->skeletalModel->animations.empty()
                && !asset->skeletalModel->primitives.empty(),
            "The DirectX 12 FBX import did not retain its CPU model");
        // 各メッシュのD3D11資源を検証
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

        // 描画シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.9f);

        // モデルの境界
        const auto& bounds = asset->skeletalModel->localBounds;
        // 境界の中心
        const DirectX::XMFLOAT3 center{
            (bounds.minimum.x + bounds.maximum.x) * 0.5f,
            (bounds.minimum.y + bounds.maximum.y) * 0.5f,
            (bounds.minimum.z + bounds.maximum.z) * 0.5f };
        // 最大の辺長
        const float maximumExtent = std::max({
            bounds.maximum.x - bounds.minimum.x,
            bounds.maximum.y - bounds.minimum.y,
            bounds.maximum.z - bounds.minimum.z,
            0.001f });
        // 描画用の倍率
        const float scale = 3.0f / maximumExtent;
        // モデルのオブジェクト
        auto& object = scene.CreateGameObject("AnimatedFbxModel");
        object.GetTransform().scale = { scale, scale, scale };
        object.GetTransform().position = {
            -center.x * scale,
            -center.y * scale,
            -center.z * scale };
        // モデル描画部品
        auto& model = object.AddComponent<
            LamaPon::ModelRendererComponent>(modelPath);
        model.SetAnimationPlayOnStart(false);

        // 背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // FBXモデルを撮影
        const auto capture = [&]()
        {
            graphics.BeginFrame(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                nullptr);
            // 画像の幅
            std::uint32_t width{};
            // 画像の高さ
            std::uint32_t height{};
            // 描画画像
            auto pixels = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 FBX capture has unexpected dimensions");
            // 撮影したpixel配列を返します。
            return pixels;
        };
        // 初回の撮影画像
        const auto pixels = capture();
        Require(
            model.AnimationCount() > 0,
            "The DirectX 12 ModelRenderer did not load the FBX animation");
        // 描画された画素数
        std::size_t modelPixels{};
        // モデル画素を数える
        for (std::size_t offset{};
            offset + 3u < pixels.size();
            offset += 4u)
        {
            // 黒以外の画素を数える
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

    // CMOの取込と描画を検証します。
    void RequireD3D12CmoModel()
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 描画デバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // CMOモデルのパス
        const auto modelPath = std::filesystem::path(
            LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "arrow.cmo";
        // 読み込み結果
        const auto asset = graphics.Assets().LoadModel(modelPath);
        Require(
            asset != nullptr
                && asset->model == nullptr
                && asset->skeletalModel != nullptr
                && asset->skeletalModel->hasLocalBounds
                && !asset->skeletalModel->primitives.empty(),
            "The DirectX 12 CMO import did not retain its CPU model");
        // 各メッシュのD3D11資源を検証
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

        // 描画シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 5.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // モデルの境界
        const auto& bounds = asset->skeletalModel->localBounds;
        // 境界の中心
        const DirectX::XMFLOAT3 center{
            (bounds.minimum.x + bounds.maximum.x) * 0.5f,
            (bounds.minimum.y + bounds.maximum.y) * 0.5f,
            (bounds.minimum.z + bounds.maximum.z) * 0.5f };
        // 最大の辺長
        const float maximumExtent = std::max({
            bounds.maximum.x - bounds.minimum.x,
            bounds.maximum.y - bounds.minimum.y,
            bounds.maximum.z - bounds.minimum.z,
            0.001f });
        // 描画用の倍率
        const float scale = 3.0f / maximumExtent;
        // モデルのオブジェクト
        auto& object = scene.CreateGameObject("CmoModel");
        // 配置と姿勢
        auto& transform = object.GetTransform();
        transform.scale = { scale, scale, scale };
        transform.SetEulerAngles(0.45f, 0.65f, 0.0f);
        // 回転後の中心
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
        // CMO描画部品
        static_cast<void>(object.AddComponent<
            LamaPon::ModelRendererComponent>(modelPath));

        // 背景色
        constexpr float clearColor[4]{ 0.1f, 0.2f, 0.3f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        // 画像の幅
        std::uint32_t width{};
        // 画像の高さ
        std::uint32_t height{};
        // 描画画像
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 CMO capture has unexpected dimensions");
        // arrow.cmoの内蔵albedoは真っ黒です。
        // D3D11のDirectXTK Model経路と同じくコンポーネントの白へ内蔵textureが掛かり、背景と違う形が環境光の下でも黒く描かれることを確かめます。
        // 背景色のRGB
        const std::array<int, 3> background{
            pixels[0],
            pixels[1],
            pixels[2] };
        // モデルの画素数
        std::size_t modelPixels{};
        // 黒い画素数
        std::size_t darkPixels{};
        // 背景とモデルを分類
        for (std::size_t offset{};
            offset + 3u < pixels.size();
            offset += 4u)
        {
            // 背景との差
            bool differs{};
            // RGB各成分を比較
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                differs = differs
                    || std::abs(
                        static_cast<int>(pixels[offset + channel])
                        - background[channel]) > 8;
            }
            // 背景画素を除外
            if (!differs)
            {
                // 次の画素へ
                continue;
            }
            ++modelPixels;
            // 黒い画素を数える
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

    // CMOの合成画像を指定APIで取得します。
    // RenderCmoModelCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderCmoModelCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 指定APIの描画デバイス
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
        // D3D11シェーダー用のasset root
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // 描画シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 5.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // CMOモデルのオブジェクト
        auto& object = scene.CreateGameObject("CmoModel");
        object.GetTransform().position = { 0.0f, 0.2f, 0.0f };
        object.GetTransform().scale = { 1.6f, 1.6f, 1.6f };
        object.GetTransform().SetEulerAngles(0.45f, 0.65f, 0.0f);
        // CMOを描くモデル部品
        auto& renderer = object.AddComponent<
            LamaPon::ModelRendererComponent>(
                std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
                    / "models"
                    / "arrow.cmo");
        // 共通のLit合成経路を使う
        renderer.SetMaterialOverrideEnabled(true);
        renderer.SetEmissiveColor({ 0.2f, 0.6f, 0.3f });

        // 合成先の背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        // 合成画像の取得結果
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        // 発光色が現れた画素数
        std::size_t emissivePixels{};
        // 緑成分の強いモデル画素を数える
        for (std::size_t offset{};
            offset + 3u < capture.pixels.size();
            offset += 4u)
        {
            // 発光の緑成分を判定
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // D3D11とD3D12の画像差を検証します。
    // RequireMatchingFrameCaptures(name: 比較名, d3d11: D3D11画像, d3d12: D3D12画像)
    void RequireMatchingFrameCaptures(
        const std::string& name,
        const Capture& d3d11,
        const Capture& d3d12)
    {
        // 比較対象の画像サイズ
        const std::size_t expectedBytes =
            static_cast<std::size_t>(CanvasWidth) * CanvasHeight * 4u;
        // 両APIの画像寸法を検証
        for (const auto* const capture : { &d3d11, &d3d12 })
        {
            Require(
                capture->width == CanvasWidth
                    && capture->height == CanvasHeight
                    && capture->pixels.size() == expectedBytes,
                "The " + name + " captures have unexpected dimensions");
        }

        // WARP上の同じ演算でも、最終丸めの1段差だけは許容します。
        // WARPの丸め誤差許容値
        constexpr int ChannelTolerance = 2;
        // 差が見つかった画素数
        std::size_t mismatchedPixels{};
        // 最初に差が出た画素
        std::size_t firstMismatch =
            std::numeric_limits<std::size_t>::max();
        // 最大チャンネル差
        int largestDifference{};
        // 各画素のRGB差を調べる
        for (std::size_t pixel{}; pixel < expectedBytes / 4u; ++pixel)
        {
            // 現在画素の最大成分差
            int pixelDifference{};
            // RGB成分ごとの差を集計
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                // 画像内の現在成分位置
                const auto offset = pixel * 4u + channel;
                pixelDifference = std::max(
                    pixelDifference,
                    std::abs(
                        static_cast<int>(d3d11.pixels[offset])
                        - static_cast<int>(d3d12.pixels[offset])));
            }
            largestDifference = std::max(largestDifference, pixelDifference);
            // 許容差を超えた画素を記録
            if (pixelDifference > ChannelTolerance)
            {
                ++mismatchedPixels;
                firstMismatch = std::min(firstMismatch, pixel);
            }
        }
        // 差がなければ検証成功
        if (mismatchedPixels == 0)
        {
            // pixel差分がないため比較処理を終了します。
            return;
        }
        // 最初の不一致画素のRGB表記を作成
        // describe(capture: 表示する画像)
        const auto describe = [&](const Capture& capture)
        {
            // 不一致画素の先頭成分位置
            const auto offset = firstMismatch * 4u;
            // pixel値を比較診断用文字列にします。
            return std::to_string(capture.pixels[offset]) + ","
                + std::to_string(capture.pixels[offset + 1u]) + ","
                + std::to_string(capture.pixels[offset + 2u]);
        };
        // 描画fixtureの失敗を例外として通知します。
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
    // DirectXTKのSDKMesh.hと同じレイアウトです。
    // Importerの定義を共有せずに合成し、実ファイルのABIを独立に検証します。
    struct TestSdkmeshVertexElement final
    {
        // 頂点ストリーム番号
        std::uint16_t stream;
        // 頂点内の要素位置
        std::uint16_t offset;
        // 要素のデータ形式
        std::uint8_t type;
        // 宣言の変換方式
        std::uint8_t method;
        // 頂点要素の用途
        std::uint8_t usage;
        // 用途の識別番号
        std::uint8_t usageIndex;
    };
#pragma pack(pop)

#pragma pack(push, 8)
    struct TestSdkmeshHeader final
    {
        // SDKMESH形式の版番号
        std::uint32_t version;
        // ビッグエンディアン形式か
        std::uint8_t isBigEndian;
        // ヘッダー領域のサイズ
        std::uint64_t headerSize;
        // バッファ以外の領域サイズ
        std::uint64_t nonBufferDataSize;
        // 頂点・索引データのサイズ
        std::uint64_t bufferDataSize;
        // 頂点バッファ数
        std::uint32_t numVertexBuffers;
        // 索引バッファ数
        std::uint32_t numIndexBuffers;
        // メッシュ数
        std::uint32_t numMeshes;
        // subset総数
        std::uint32_t numTotalSubsets;
        // フレーム数
        std::uint32_t numFrames;
        // マテリアル数
        std::uint32_t numMaterials;
        // 頂点ストリームヘッダー位置
        std::uint64_t vertexStreamHeadersOffset;
        // 索引ストリームヘッダー位置
        std::uint64_t indexStreamHeadersOffset;
        // メッシュデータ位置
        std::uint64_t meshDataOffset;
        // subsetデータ位置
        std::uint64_t subsetDataOffset;
        // フレームデータ位置
        std::uint64_t frameDataOffset;
        // マテリアルデータ位置
        std::uint64_t materialDataOffset;
    };

    struct TestSdkmeshVertexBufferHeader final
    {
        // 頂点数
        std::uint64_t numVertices;
        // 頂点データのサイズ
        std::uint64_t sizeBytes;
        // 頂点1個あたりのサイズ
        std::uint64_t strideBytes;
        // 頂点属性の宣言
        std::array<TestSdkmeshVertexElement, 32> declaration;
        // 頂点データの位置
        std::uint64_t dataOffset;
    };

    struct TestSdkmeshIndexBufferHeader final
    {
        // 索引数
        std::uint64_t numIndices;
        // 索引データのサイズ
        std::uint64_t sizeBytes;
        // 索引のデータ形式
        std::uint32_t indexType;
        // 索引データの位置
        std::uint64_t dataOffset;
    };

    struct TestSdkmeshMesh final
    {
        // メッシュ名
        char name[100];
        // 使用する頂点バッファ数
        std::uint8_t numVertexBuffers;
        // 頂点バッファ番号一覧
        std::uint32_t vertexBuffers[16];
        // 使用する索引バッファ番号
        std::uint32_t indexBuffer;
        // メッシュ内のsubset数
        std::uint32_t numSubsets;
        // フレーム影響数
        std::uint32_t numFrameInfluences;
        // 境界ボックスの中心
        DirectX::XMFLOAT3 boundingBoxCenter;
        // 境界ボックスの半径
        DirectX::XMFLOAT3 boundingBoxExtents;
        // subset番号一覧の位置
        std::uint64_t subsetOffset;
        // フレーム影響一覧の位置
        std::uint64_t frameInfluenceOffset;
    };

    struct TestSdkmeshSubset final
    {
        // subset名
        char name[100];
        // 適用するマテリアル番号
        std::uint32_t materialId;
        // プリミティブ形式
        std::uint32_t primitiveType;
        // 描画開始索引
        std::uint64_t indexStart;
        // 描画する索引数
        std::uint64_t indexCount;
        // 頂点範囲の開始番号
        std::uint64_t vertexStart;
        // 使用する頂点数
        std::uint64_t vertexCount;
    };

    struct TestSdkmeshFrame final
    {
        // フレーム名
        char name[100];
        // 関連メッシュ番号
        std::uint32_t mesh;
        // 親フレーム番号
        std::uint32_t parentFrame;
        // 子フレーム番号
        std::uint32_t childFrame;
        // 兄弟フレーム番号
        std::uint32_t siblingFrame;
        // フレーム変換行列
        DirectX::XMFLOAT4X4 matrix;
        // アニメーションデータ番号
        std::uint32_t animationDataIndex;
    };

    struct TestSdkmeshMaterial final
    {
        // マテリアル名
        char name[100];
        // マテリアル定義ファイル
        char materialInstancePath[260];
        // diffuseテクスチャ名
        char diffuseTexture[260];
        // normalテクスチャ名
        char normalTexture[260];
        // specularテクスチャ名
        char specularTexture[260];
        // diffuse色
        DirectX::XMFLOAT4 diffuse;
        // ambient色
        DirectX::XMFLOAT4 ambient;
        // specular色
        DirectX::XMFLOAT4 specular;
        // emissive色
        DirectX::XMFLOAT4 emissive;
        // 光沢度
        float power;
        // 実行時ハンドル領域
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

    // 2材質BoxのSDKMESH version 101データを生成します。
    // 頂点範囲とsubset材質のImporter検証用です。
    [[nodiscard]] std::vector<std::uint8_t> BuildTestSdkmesh()
    {
        struct BoxVertex final
        {
            // 頂点座標
            DirectX::XMFLOAT3 position;
            // 頂点法線
            DirectX::XMFLOAT3 normal;
            // 頂点UV
            DirectX::XMFLOAT2 textureCoordinate;
        };
        // Boxの半辺長
        constexpr float h = 0.5f;
        // 面ごとの頂点配列
        std::vector<BoxVertex> vertices;
        // 面ごとの索引配列
        std::vector<std::uint16_t> indices;
        // addFace(rangeStart: 頂点範囲, normal: 面法線, corners: 面頂点)
        const auto addFace = [&](
            const std::uint16_t rangeStart,
            const DirectX::XMFLOAT3& normal,
            const std::array<DirectX::XMFLOAT3, 4>& corners)
        {
            // cornersは外から見た左下、左上、右上、右下です。
            // DirectXTKの既定（時計回りが表）で表になり、indexは範囲の先頭から数えます。
            // 面範囲内での最初の頂点番号
            const auto first = static_cast<std::uint16_t>(
                vertices.size() - rangeStart);
            // 面の頂点に対応するUV
            const std::array<DirectX::XMFLOAT2, 4> textureCoordinates{ {
                { 0.0f, 1.0f },
                { 0.0f, 0.0f },
                { 1.0f, 0.0f },
                { 1.0f, 1.0f } } };
            // 面の四頂点を登録
            for (std::size_t corner{}; corner < corners.size(); ++corner)
            {
                vertices.push_back({
                    corners[corner],
                    normal,
                    textureCoordinates[corner] });
            }
            // 四角形を構成する三角形索引
            constexpr std::array<std::uint16_t, 6> quad{
                0u, 1u, 2u, 0u, 2u, 3u };
            // 面の頂点番号を追加
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

        // 頂点バッファヘッダーの位置
        const std::uint64_t vertexHeaderOffset = sizeof(TestSdkmeshHeader);
        // 索引バッファヘッダーの位置
        const std::uint64_t indexHeaderOffset =
            vertexHeaderOffset + sizeof(TestSdkmeshVertexBufferHeader);
        // メッシュデータの位置
        const std::uint64_t meshOffset =
            indexHeaderOffset + sizeof(TestSdkmeshIndexBufferHeader);
        // subsetデータの位置
        const std::uint64_t subsetOffset = meshOffset + sizeof(TestSdkmeshMesh);
        // フレームデータの位置
        const std::uint64_t frameOffset =
            subsetOffset + 2u * sizeof(TestSdkmeshSubset);
        // マテリアルデータの位置
        const std::uint64_t materialOffset =
            frameOffset + sizeof(TestSdkmeshFrame);
        // subset番号一覧の位置
        const std::uint64_t subsetTableOffset =
            materialOffset + 2u * sizeof(TestSdkmeshMaterial);
        // 頂点・索引データの位置
        const std::uint64_t bufferOffset =
            subsetTableOffset + 2u * sizeof(std::uint32_t);
        // 頂点データのサイズ
        const std::uint64_t vertexBytes = vertices.size() * sizeof(BoxVertex);
        // 索引データのサイズ
        const std::uint64_t indexBytes =
            indices.size() * sizeof(std::uint16_t);

        // SDKMESHファイルヘッダー
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

        // 頂点バッファの記述
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

        // 索引バッファの記述
        TestSdkmeshIndexBufferHeader indexBuffer{};
        indexBuffer.numIndices = indices.size();
        indexBuffer.sizeBytes = indexBytes;
        indexBuffer.indexType = 0u;
        indexBuffer.dataOffset = bufferOffset + vertexBytes;

        // Boxメッシュの記述
        TestSdkmeshMesh mesh{};
        strcpy_s(mesh.name, "TwoMaterialBox");
        mesh.numVertexBuffers = 1u;
        mesh.indexBuffer = 0u;
        mesh.numSubsets = 2u;
        mesh.boundingBoxExtents = { h, h, h };
        mesh.subsetOffset = subsetTableOffset;

        // 赤青のsubset記述
        std::array<TestSdkmeshSubset, 2> subsets{};
        // 各subsetの範囲と材質を設定
        for (std::size_t index{}; index < subsets.size(); ++index)
        {
            // 設定対象subset
            auto& subset = subsets[index];
            strcpy_s(subset.name, index == 0u ? "Red" : "Blue");
            subset.materialId = static_cast<std::uint32_t>(index);
            subset.primitiveType = 0u;
            subset.indexStart = index * 18u;
            subset.indexCount = 18u;
            subset.vertexStart = index * 12u;
            subset.vertexCount = 12u;
        }

        // ルートフレーム
        TestSdkmeshFrame frame{};
        strcpy_s(frame.name, "Root");
        frame.mesh = 0u;
        frame.parentFrame = 0xffffffffu;
        frame.childFrame = 0xffffffffu;
        frame.siblingFrame = 0xffffffffu;
        DirectX::XMStoreFloat4x4(&frame.matrix, DirectX::XMMatrixIdentity());
        frame.animationDataIndex = 0xffffffffu;

        // 赤青のマテリアル記述
        std::array<TestSdkmeshMaterial, 2> materials{};
        strcpy_s(materials[0].name, "Red");
        materials[0].diffuse = { 0.9f, 0.15f, 0.1f, 1.0f };
        materials[0].ambient = { 0.9f, 0.15f, 0.1f, 1.0f };
        strcpy_s(materials[1].name, "Blue");
        materials[1].diffuse = { 0.1f, 0.2f, 0.9f, 1.0f };
        materials[1].ambient = { 0.1f, 0.2f, 0.9f, 1.0f };

        // メッシュから参照するsubset番号
        const std::array<std::uint32_t, 2> subsetTable{ 0u, 1u };
        // 完成するSDKMESHファイル領域
        std::vector<std::uint8_t> file(
            static_cast<std::size_t>(bufferOffset + vertexBytes + indexBytes));
        // write(offset: 書込位置, data: 元データ, size: 書込サイズ)
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

        // 生成したfixture fileを返します。
        return file;
    }

    // SDKMESH subsetの頂点範囲と材質変換を検証します。
    // fixtureはメモリから渡してファイル生成を避けます。
    void RequireD3D12SdkmeshModel()
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // SDKMESHを読み込む描画デバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // 検証用のSDKMESHデータ
        const auto file = BuildTestSdkmesh();
        // インポート後のCPUモデル
        const auto model = LamaPon::SdkmeshImporter::LoadFromMemory(
            graphics.Assets(),
            file,
            L"TwoMaterialBox.sdkmesh");
        Require(
            model != nullptr
                && model->hasLocalBounds
                && model->primitives.size() == 2u,
            "The DirectX 12 SDKMESH import did not retain its subsets");
        // 各subsetが頂点範囲を保持するか検証
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
        // 頂点数
        std::uint32_t vertexCount;
        // 索引数
        std::uint32_t indexCount;
    };

    struct TestVboVertex final
    {
        // 頂点座標
        DirectX::XMFLOAT3 position;
        // 頂点法線
        DirectX::XMFLOAT3 normal;
        // 頂点UV
        DirectX::XMFLOAT2 textureCoordinate;
    };
#pragma pack(pop)

    static_assert(sizeof(TestVboHeader) == 8u);
    static_assert(sizeof(TestVboVertex) == 32u);

    // 四頂点のVBO fixtureを生成します。
    [[nodiscard]] std::vector<std::uint8_t> BuildTestVbo()
    {
        // VBOヘッダー
        constexpr TestVboHeader header{ 4u, 6u };
        // 四角形を構成する頂点
        constexpr std::array<TestVboVertex, 4> vertices{ {
            { { -1.0f, -0.5f, 0.25f }, { 0.0f, 0.0f, -1.0f },
                { 0.0f, 1.0f } },
            { { -1.0f, 0.5f, 0.25f }, { 0.0f, 0.0f, -1.0f },
                { 0.0f, 0.0f } },
            { { 1.0f, 0.5f, 0.25f }, { 0.0f, 0.0f, -1.0f },
                { 1.0f, 0.0f } },
            { { 1.0f, -0.5f, 0.25f }, { 0.0f, 0.0f, -1.0f },
                { 1.0f, 1.0f } } } };
        // 四角形を構成する三角形索引
        constexpr std::array<std::uint16_t, 6> indices{
            0u, 1u, 2u, 0u, 2u, 3u };
        // VBOデータ格納領域
        std::vector<std::uint8_t> bytes(
            sizeof(header) + sizeof(vertices) + sizeof(indices));
        // 書込先の現在位置
        std::size_t offset{};
        // append(data: 元データ, size: 書込サイズ)
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
        // 構築したfixture byte列を返します。
        return bytes;
    }

    // VBOの頂点形式と境界を検証します。
    void RequireD3D12VboModel()
    {
        // VBOのメモリデータ
        const auto bytes = BuildTestVbo();
        // 読み込んだCPUモデル
        const auto model = LamaPon::VboImporter::LoadFromMemory(
            bytes,
            L"Quad.vbo");
        Require(
            model != nullptr
                && model->hasLocalBounds
                && model->nodes.size() == 1u
                && model->primitives.size() == 1u,
            "The DirectX 12 VBO import did not create one CPU primitive");
        // 読み込んだ唯一のプリミティブ
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

    // D3D12パーティクルの描画を検証します。
    void RequireD3D12Particles()
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // パーティクルを描くデバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // パーティクルを含むシーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        // パーティクルのオブジェクト
        auto& particleObject = scene.CreateGameObject("Particles");
        // 検証対象のパーティクル部品
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

        // 背景色
        constexpr float clearColor[4]{ 0.01f, 0.02f, 0.03f, 1.0f };
        graphics.BeginFrame(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            nullptr);
        // 撮影画像の幅
        std::uint32_t width{};
        // 撮影画像の高さ
        std::uint32_t height{};
        // パーティクルの撮影画像
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        Require(
            width == CanvasWidth && height == CanvasHeight,
            "The DirectX 12 particle capture has unexpected dimensions");
        // 画面中心のRGBA位置
        const auto center =
            (static_cast<std::size_t>(CanvasHeight / 2u) * CanvasWidth
                + CanvasWidth / 2u) * 4u;
        Require(
            pixels[center + 1u] > 150u
                && pixels[center] < 80u,
            "The DirectX 12 particle service did not draw its billboard");
    }

    // D3D11とD3D12のスプライト画像を比較します。
    // RequireMatchingCaptures(d3d11: D3D11画像, d3d12: D3D12画像)
    void RequireMatchingCaptures(
        const Capture& d3d11,
        const Capture& d3d12)
    {
        // 比較する画像のバイト数
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

        // 赤い矩形中心のRGBA位置
        const std::size_t rectangleCenter =
            (17u * static_cast<std::size_t>(CanvasWidth) + 26u) * 4u;
        Require(
            d3d12.pixels[rectangleCenter] > 180u
                && d3d12.pixels[rectangleCenter + 1u] < 90u,
            "The DirectX 12 sprite capture did not contain the red rectangle");

        // WARPで許容する丸め誤差
        constexpr int ChannelTolerance = 2;
        // 許容差を超えた画素数
        std::size_t mismatchedPixels{};
        // 最初に差が出た画素
        std::size_t firstMismatch =
            std::numeric_limits<std::size_t>::max();
        // 最大チャンネル差
        int largestDifference{};
        // 各画素の色差を比較
        for (std::size_t pixel{}; pixel < expectedBytes / 4u; ++pixel)
        {
            // 現在画素の最大成分差
            int pixelDifference{};
            // RGBA成分ごとの差を集計
            for (std::size_t channel{}; channel < 4u; ++channel)
            {
                // 画像内の現在成分位置
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
            // 許容差を超えた画素を記録
            if (pixelDifference > ChannelTolerance)
            {
                ++mismatchedPixels;
                firstMismatch = std::min(firstMismatch, pixel);
            }
        }
        // 差がなければ検証成功
        if (mismatchedPixels == 0)
        {
            // pixel差分がないため比較処理を終了します。
            return;
        }

        // 最初の差分画素のRGBA表記を作成
        // describe(capture: 表示する画像)
        const auto describe = [&](const Capture& capture)
        {
            // 差分画素の先頭成分位置
            const auto offset = firstMismatch * 4u;
            // pixel値を比較診断用文字列にします。
            return std::to_string(capture.pixels[offset]) + ","
                + std::to_string(capture.pixels[offset + 1u]) + ","
                + std::to_string(capture.pixels[offset + 2u]) + ","
                + std::to_string(capture.pixels[offset + 3u]);
        };
        // 描画fixtureの失敗を例外として通知します。
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

    // 比較対象の後処理一覧
    constexpr std::array<PostProcessCase, 6> PostProcessCases{
        PostProcessCase::Bloom,
        PostProcessCase::LensFlare,
        PostProcessCase::ToneMapping,
        PostProcessCase::Fxaa,
        PostProcessCase::Temporal,
        PostProcessCase::MotionBlur
    };

    // PostProcessCaseName(postProcess: 後処理の種類)
    // 後処理の種類を診断用の名前へ変換します。
    [[nodiscard]] std::string PostProcessCaseName(
        const PostProcessCase postProcess)
    {
        // 種類ごとの表示名を選択
        switch (postProcess)
        {
        case PostProcessCase::Bloom:
            // post-process種別の表示名を返します。
            return "bloom";
        case PostProcessCase::LensFlare:
            // post-process種別の表示名を返します。
            return "screen-space lens flare";
        case PostProcessCase::ToneMapping:
            // post-process種別の表示名を返します。
            return "tone mapping";
        case PostProcessCase::Fxaa:
            // post-process種別の表示名を返します。
            return "FXAA";
        case PostProcessCase::Temporal:
            // post-process種別の表示名を返します。
            return "TAA";
        case PostProcessCase::MotionBlur:
            // post-process種別の表示名を返します。
            return "motion blur";
        }
        // post-process種別の表示名を返します。
        return "post-process";
    }

    // 後処理ごとのD3D11/12比較画像を生成します。
    // 同一HDR入力へ各効果を単独で適用します。
    // RenderPostProcessCaptures(api: 描画API, profile: 起動設定)
    [[nodiscard]] std::array<Capture, PostProcessCases.size()>
        RenderPostProcessCaptures(
            const LamaPon::RenderingApi api,
            const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 後処理を実行する描画デバイス
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
        // D3D11シェーダー用のasset root
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);

        // 後処理を適用するHDR描画先
        LamaPon::RenderTarget target;
        graphics.ResizeOffscreenTarget(target, 64u, 32u);

        // 各後処理の撮影結果
        std::array<Capture, PostProcessCases.size()> captures;
        // 定義済み後処理を順に撮影
        for (std::size_t index{}; index < PostProcessCases.size(); ++index)
        {
            // 今回検証する後処理
            const auto postProcess = PostProcessCases[index];
            // 描画先の初期色
            constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
            // 後処理用の単位行列
            DirectX::XMFLOAT4X4 identity{};
            DirectX::XMStoreFloat4x4(
                &identity,
                DirectX::XMMatrixIdentity());
            // TAA履歴を用意する専用フレーム
            if (postProcess == PostProcessCase::Temporal)
            {
                // 1フレーム目の灰色を履歴へ控えます。
                // 次のフレームでは白黒境界の近傍範囲内へ履歴が収まり、実際に混ざります。
                graphics.BeginFrame(clearColor);
                // 履歴描画前の出力状態
                const auto historyOutput = graphics.CaptureOutputState();
                graphics.BeginOffscreenTarget(target, clearColor);
                {
                    // 履歴用描画パス
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
            // モーションブラー履歴を用意する専用フレーム
            else if (postProcess == PostProcessCase::MotionBlur)
            {
                // 最初のフレームは描画せず、横へずれた前フレームのビュー射影だけをtargetへ保存します。
                // 次フレームとの差が8pxぶんのカメラ移動になります。
                // 前フレームのビュー射影
                DirectX::XMFLOAT4X4 previousViewProjection{};
                DirectX::XMStoreFloat4x4(
                    &previousViewProjection,
                    DirectX::XMMatrixTranslation(0.25f, 0.0f, 0.0f));
                graphics.BeginFrame(clearColor);
                // 履歴描画前の出力状態
                const auto previousOutput = graphics.CaptureOutputState();
                graphics.BeginOffscreenTarget(target, clearColor);
                // 初期履歴へ適用するブラー設定
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
            // 主描画前の出力状態
            const auto primaryOutput = graphics.CaptureOutputState();
            graphics.BeginOffscreenTarget(target, clearColor);
            {
                // 今回の入力画像を描くパス
                auto pass = graphics.BeginSpritePass();
                // FXAA検証用の斜め境界
                if (postProcess == PostProcessCase::Fxaa)
                {
                    // 回転した矩形の縁は階段状になり、FXAAが中間色で平します。
                    // FXAA境界を描く要求
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
                // TAA検証用の白黒境界
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
                // ブラー検証用の白帯
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
                // その他の後処理に使う高輝度矩形
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
            // 選択した後処理を適用
            switch (postProcess)
            {
            case PostProcessCase::Bloom:
            {
                // Bloom検証用の設定
                LamaPon::BloomSettings bloom;
                bloom.enabled = true;
                graphics.ApplyOffscreenTargetBloom(target, bloom);
                // この描画検証caseを終えて次caseへのfallthroughを防ぎます。
                break;
            }
            case PostProcessCase::LensFlare:
            {
                // Lens flare検証用の設定
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
                // この描画検証caseを終えて次caseへのfallthroughを防ぎます。
                break;
            }
            case PostProcessCase::ToneMapping:
                graphics.ApplyOffscreenTargetToneMapping(
                    target,
                    LamaPon::ColorGradingSettings{});
                // この描画検証caseを終えて次caseへのfallthroughを防ぎます。
                break;
            case PostProcessCase::Fxaa:
                graphics.ApplyOffscreenTargetFXAA(target);
                // この描画検証caseを終えて次caseへのfallthroughを防ぎます。
                break;
            case PostProcessCase::Temporal:
            {
                // TAA検証用の設定
                LamaPon::TemporalAntiAliasingSettings temporal;
                temporal.enabled = true;
                temporal.historyWeight = 0.75f;
                temporal.clampTolerance = 4.0f;
                // TAA用の行列入力
                LamaPon::TemporalAntiAliasingInputs inputs;
                inputs.inverseViewProjection = identity;
                inputs.viewProjection = identity;
                graphics.ApplyOffscreenTargetTemporalAntiAliasing(
                    target,
                    temporal,
                    inputs);
                // この描画検証caseを終えて次caseへのfallthroughを防ぎます。
                break;
            }
            case PostProcessCase::MotionBlur:
            {
                // Motion blur検証用の設定
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
                // このpost-process分岐の検証を終えます。
                break;
            }
            }
            graphics.PublishOffscreenTarget(target);
            graphics.RestoreOutputState(*primaryOutput);
            {
                // 後処理画像の表示パス
                auto pass = graphics.BeginSpritePass();
                // offscreen画像を画面へ写す要求
                LamaPon::SpriteDrawRequest request;
                request.texture = target.DisplayViewHandle();
                request.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
                Require(
                    pass.Draw(request),
                    "The post-processed offscreen display view was "
                    "rejected");
            }
            // 今回の撮影結果を格納
            auto& capture = captures[index];
            capture.pixels = graphics.CaptureBackBuffer(
                capture.width,
                capture.height);
            graphics.EndFrame();
        }
        // 撮影したframe captureを返します。
        return captures;
    }

    // D3D11/12の各後処理画像を検証します。
    // RequireMatchingPostProcessCaptures(d3d11: D3D11画像, d3d12: D3D12画像)
    void RequireMatchingPostProcessCaptures(
        const std::array<Capture, PostProcessCases.size()>& d3d11,
        const std::array<Capture, PostProcessCases.size()>& d3d12)
    {
        // 期待するRGBA画像サイズ
        const std::size_t expectedBytes =
            static_cast<std::size_t>(CanvasWidth) * CanvasHeight * 4u;
        // offset(x:横,y:縦)
        const auto offset = [](const std::uint32_t x, const std::uint32_t y)
        {
            // 座標に対応するRGBA byte offsetを返します。
            return (static_cast<std::size_t>(y) * CanvasWidth + x) * 4u;
        };
        // 全後処理の画像を検証
        for (std::size_t index{}; index < PostProcessCases.size(); ++index)
        {
            // 現在の後処理
            const auto postProcess = PostProcessCases[index];
            // エラー表示用の後処理名
            const auto name = PostProcessCaseName(postProcess);
            // 両APIの画像寸法と効果を検証
            for (const auto* const capture : { &d3d11[index], &d3d12[index] })
            {
                Require(
                    capture->width == CanvasWidth
                        && capture->height == CanvasHeight
                        && capture->pixels.size() == expectedBytes,
                    "The " + name + " captures have unexpected dimensions");
                // 検査対象画像の画素配列
                const auto& pixels = capture->pixels;
                // 後処理固有の出力を検査
                switch (postProcess)
                {
                case PostProcessCase::Bloom:
                    // 矩形はx=28..35、y=12..19です。
                    // 半径2の9tapは縁から2画素先まで届き、5画素離れると黒のままです。
                    Require(
                        pixels[offset(36u, 16u)] > 60u
                            && pixels[offset(36u, 16u) + 1u] > 20u,
                        "The bloom did not spread beyond the bright "
                        "rectangle");
                    Require(
                        pixels[offset(40u, 16u)] < 4u
                            && pixels[offset(5u, 5u)] < 4u,
                        "The bloom spread beyond its radius");
                    // 最初の差分pixelが見つかったため探索を終えます。
                    break;
                case PostProcessCase::LensFlare:
                {
                    // 元の8x8高輝度矩形の外側へ、ゴースト、ハロー、筋が十分な範囲で広がっていることを確認します。
                    // 元矩形外の発光画素数
                    std::size_t flarePixels{};
                    // 画面全体から発光画素を集計
                    for (std::uint32_t y{}; y < CanvasHeight; ++y)
                    {
                        // 各行の画素を走査
                        for (std::uint32_t x{}; x < CanvasWidth; ++x)
                        {
                            // 元矩形の画素を除外
                            if (x >= 28u && x < 36u
                                && y >= 12u && y < 20u)
                            {
                                // 発光範囲の外側を調べる
                                continue;
                            }
                            // 現在画素のRGBA位置
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
                    // この描画検証caseを終えて次caseへのfallthroughを防ぎます。
                    break;
                }
                case PostProcessCase::ToneMapping:
                    // 既定のカラーグレーディングとACESで(4, 2, 0.5)は約(255, 242, 162)になり、単純clipの(255, 255, 128)とは異なります。
                    Require(
                        pixels[offset(32u, 16u) + 1u] > 225u
                            && pixels[offset(32u, 16u) + 1u] < 252u
                            && pixels[offset(32u, 16u) + 2u] > 140u
                            && pixels[offset(32u, 16u) + 2u] < 180u,
                        "The tone mapping did not apply ACES and color "
                        "grading");
                    // この描画検証caseを終えて次caseへのfallthroughを防ぎます。
                    break;
                case PostProcessCase::Fxaa:
                {
                    // 回転した矩形の縁にだけ、FXAAが中間の明るさを作ります。
                    // FXAA境界の中間色画素数
                    std::size_t blendedPixels{};
                    // 対象範囲を行ごとに走査
                    for (std::uint32_t y{}; y < 32u; ++y)
                    {
                        // 対象行の画素を走査
                        for (std::uint32_t x{}; x < 64u; ++x)
                        {
                            // 現在画素の赤成分
                            const auto value = pixels[offset(x, y)];
                            // 中間色の境界画素を数える
                            if (value > 16u && value < 200u)
                            {
                                ++blendedPixels;
                            }
                        }
                    }
                    Require(
                        blendedPixels > 8u,
                        "FXAA did not smooth the rotated edge");
                    // この描画検証caseを終えて次caseへのfallthroughを防ぎます。
                    break;
                }
                case PostProcessCase::Temporal:
                    // 現在色はx<32が黒、履歴は全面0.4です。
                    // 境界の1画素は近傍クランプを通過して履歴比率0.75で混ざり、その外側は黒へクランプされます。
                    Require(
                        pixels[offset(30u, 16u)] < 4u
                            && pixels[offset(31u, 16u)] > 65u
                            && pixels[offset(31u, 16u)] < 90u
                            && pixels[offset(32u, 16u)] > 125u
                            && pixels[offset(32u, 16u)] < 155u,
                        "TAA did not reproject and clamp the temporal "
                        "history at the edge");
                    // この描画検証caseを終えて次caseへのfallthroughを防ぎます。
                    break;
                case PostProcessCase::MotionBlur:
                    // 元の白帯はx=28..35です。
                    // 前フレームとの8pxの差を中心から両側へ伸ばすため、外側に中間色ができます。
                    Require(
                        pixels[offset(26u, 16u)] > 20u
                            && pixels[offset(31u, 16u)] > 100u
                            && pixels[offset(31u, 16u)] < 250u,
                        "Motion blur did not spread the moving edge");
                    // このpost-process分岐の検証を終えます。
                    break;
                }
            }

            // WARPで許容する丸め誤差
            constexpr int ChannelTolerance = 2;
            // RGBA全成分を画素単位で比較
            for (std::size_t byte{}; byte < expectedBytes; ++byte)
            {
                // 対応成分間の絶対差
                const int difference = std::abs(
                    static_cast<int>(d3d11[index].pixels[byte])
                    - static_cast<int>(d3d12[index].pixels[byte]));
                // 許容差を超えた成分を報告
                if (difference > ChannelTolerance)
                {
                    // 不一致成分が属する画素
                    const auto pixel = byte / 4u;
                    // 描画fixtureの失敗を例外として通知します。
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

    // DDS形式の変換とGPU view生成を検証します。
    void RequireD3D12DdsTextures()
    {
        // RGBA8のDDS fixture
        const auto rgbaBytes = BuildRgbaDds();
        // RGBA8のmip構造
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

        // BC1の圧縮ブロック
        const std::vector<std::uint8_t> bc1Block{
            0x00u, 0xf8u, 0x00u, 0x00u,
            0x00u, 0x00u, 0x00u, 0x00u };
        // BC3の圧縮ブロック
        std::vector<std::uint8_t> bc3Block(16u);
        bc3Block[0] = 255u;
        bc3Block[1] = 255u;
        bc3Block[8] = 0xe0u;
        bc3Block[9] = 0x07u;
        // BC5の圧縮ブロック
        std::vector<std::uint8_t> bc5Block(16u);
        bc5Block[0] = 0u;
        bc5Block[1] = 0u;
        bc5Block[8] = 255u;
        bc5Block[9] = 255u;
        // BC1形式のDDS fixture
        const auto bc1Bytes = BuildClassicDds(
            4u,
            4u,
            1u,
            MakeFourCc('D', 'X', 'T', '1'),
            bc1Block);
        // BC3形式のDDS fixture
        const auto bc3Bytes = BuildClassicDds(
            4u,
            4u,
            1u,
            MakeFourCc('D', 'X', 'T', '5'),
            bc3Block);
        // BC5形式のDDS fixture
        const auto bc5Bytes = BuildClassicDds(
            4u,
            4u,
            1u,
            MakeFourCc('A', 'T', 'I', '2'),
            bc5Block);
        // BGRA形式の画素データ
        std::vector<std::uint8_t> bgraPixels;
        // BGRA fixture用の画素を生成
        for (std::size_t pixel{}; pixel < 16u; ++pixel)
        {
            bgraPixels.insert(bgraPixels.end(), { 30u, 60u, 210u, 255u });
        }
        // BGRA DDS fixture
        const auto bgraBytes = BuildDx10Dds(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            bgraPixels);
        struct ExtendedDdsCase final
        {
            // 入力DDSの形式
            DXGI_FORMAT format;
            // 画像データのサイズ
            std::size_t bytes;
            // 1行あたりのデータ量
            std::uint32_t rowPitch;
        };
        // 対応形式と期待する配置
        const std::array extendedCases{
            ExtendedDdsCase{ DXGI_FORMAT_R8_UNORM, 16u, 4u },
            ExtendedDdsCase{ DXGI_FORMAT_R8G8_UNORM, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_B8G8R8X8_UNORM, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_BC1_UNORM_SRGB, 8u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_BC2_UNORM, 16u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_BC2_UNORM_SRGB, 16u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_BC3_UNORM_SRGB, 16u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_BC4_UNORM, 8u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_BC4_SNORM, 8u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_BC5_SNORM, 16u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_BC6H_UF16, 16u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_BC6H_SF16, 16u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_BC7_UNORM, 16u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_BC7_UNORM_SRGB, 16u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_R16_FLOAT, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_R16G16_FLOAT, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_R16G16B16A16_FLOAT, 128u, 32u },
            ExtendedDdsCase{ DXGI_FORMAT_R32_FLOAT, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_R32G32_FLOAT, 128u, 32u },
            ExtendedDdsCase{ DXGI_FORMAT_R32G32B32A32_FLOAT, 256u, 64u },
            ExtendedDdsCase{ DXGI_FORMAT_R10G10B10A2_UNORM, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_R16G16_UNORM, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_B5G5R5A1_UNORM, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_B5G6R5_UNORM, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_B4G4R4A4_UNORM, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_R16_UNORM, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_A8_UNORM, 16u, 4u },
            ExtendedDdsCase{ DXGI_FORMAT_R8G8_SNORM, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_R8G8B8A8_SNORM, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_R16G16_SNORM, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_R16G16B16A16_UNORM, 128u, 32u },
            ExtendedDdsCase{ DXGI_FORMAT_R16G16B16A16_SNORM, 128u, 32u },
            ExtendedDdsCase{ DXGI_FORMAT_R8G8_B8G8_UNORM, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_G8R8_G8B8_UNORM, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_R8_SNORM, 16u, 4u },
            ExtendedDdsCase{ DXGI_FORMAT_R16_SNORM, 32u, 8u },
            ExtendedDdsCase{ DXGI_FORMAT_R11G11B10_FLOAT, 64u, 16u },
            ExtendedDdsCase{ DXGI_FORMAT_R9G9B9E5_SHAREDEXP, 64u, 16u }
        };
        // 各DX10形式のfixture
        std::vector<std::vector<std::uint8_t>> extendedDdsBytes;
        extendedDdsBytes.reserve(extendedCases.size());
        // 対応形式ごとに読み込み配置を検証
        for (const auto& testCase : extendedCases)
        {
            extendedDdsBytes.push_back(BuildDx10Dds(
                testCase.format,
                std::vector<std::uint8_t>(testCase.bytes)));
            // fixtureの解析結果
            // 型なしfixtureの解析結果
            const auto prepared =
                LamaPon::TextureLoader::PrepareDdsTextureData(
                    extendedDdsBytes.back());
            Require(
                prepared.format == testCase.format
                    && prepared.levels.size() == 1u
                    && prepared.levels.front().rowPitch
                        == testCase.rowPitch
                    && prepared.levels.front().bytes.size()
                        == testCase.bytes,
                "An extended DX10 DDS format lost its upload layout");
        }
        struct TypelessDdsCase final
        {
            // DDS内の型なし形式
            DXGI_FORMAT storageFormat;
            // shaderから参照する形式
            DXGI_FORMAT viewFormat;
            // 画像データのサイズ
            std::size_t bytes;
            // 1行あたりのデータ量
            std::uint32_t rowPitch;
        };
        // 型なし形式の既定view対応表
        const std::array typelessCases{
            TypelessDdsCase{ DXGI_FORMAT_R32G32B32A32_TYPELESS,
                DXGI_FORMAT_R32G32B32A32_FLOAT, 256u, 64u },
            TypelessDdsCase{ DXGI_FORMAT_R16G16B16A16_TYPELESS,
                DXGI_FORMAT_R16G16B16A16_UNORM, 128u, 32u },
            TypelessDdsCase{ DXGI_FORMAT_R32G32_TYPELESS,
                DXGI_FORMAT_R32G32_FLOAT, 128u, 32u },
            TypelessDdsCase{ DXGI_FORMAT_R10G10B10A2_TYPELESS,
                DXGI_FORMAT_R10G10B10A2_UNORM, 64u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_R8G8B8A8_TYPELESS,
                DXGI_FORMAT_R8G8B8A8_UNORM, 64u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_R16G16_TYPELESS,
                DXGI_FORMAT_R16G16_UNORM, 64u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_R32_TYPELESS,
                DXGI_FORMAT_R32_FLOAT, 64u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_R8G8_TYPELESS,
                DXGI_FORMAT_R8G8_UNORM, 32u, 8u },
            TypelessDdsCase{ DXGI_FORMAT_R16_TYPELESS,
                DXGI_FORMAT_R16_UNORM, 32u, 8u },
            TypelessDdsCase{ DXGI_FORMAT_R8_TYPELESS,
                DXGI_FORMAT_R8_UNORM, 16u, 4u },
            TypelessDdsCase{ DXGI_FORMAT_BC1_TYPELESS,
                DXGI_FORMAT_BC1_UNORM, 8u, 8u },
            TypelessDdsCase{ DXGI_FORMAT_BC2_TYPELESS,
                DXGI_FORMAT_BC2_UNORM, 16u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_BC3_TYPELESS,
                DXGI_FORMAT_BC3_UNORM, 16u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_BC4_TYPELESS,
                DXGI_FORMAT_BC4_UNORM, 8u, 8u },
            TypelessDdsCase{ DXGI_FORMAT_BC5_TYPELESS,
                DXGI_FORMAT_BC5_UNORM, 16u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_BC6H_TYPELESS,
                DXGI_FORMAT_BC6H_UF16, 16u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_BC7_TYPELESS,
                DXGI_FORMAT_BC7_UNORM, 16u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_B8G8R8A8_TYPELESS,
                DXGI_FORMAT_B8G8R8A8_UNORM, 64u, 16u },
            TypelessDdsCase{ DXGI_FORMAT_B8G8R8X8_TYPELESS,
                DXGI_FORMAT_B8G8R8X8_UNORM, 64u, 16u }
        };
        // 型なし形式のview変換を検証
        for (const auto& testCase : typelessCases)
        {
                // typeless DDS view
            const auto prepared =
                LamaPon::TextureLoader::PrepareDdsTextureData(
                    BuildDx10Dds(
                        testCase.storageFormat,
                        std::vector<std::uint8_t>(testCase.bytes)));
            Require(
                prepared.format == testCase.viewFormat
                    && prepared.levels.front().rowPitch
                        == testCase.rowPitch
                    && prepared.levels.front().bytes.size()
                        == testCase.bytes,
                "A typeless DDS format was not normalized to its default "
                "shader-readable view");
        }
        // 表示専用形式が拒否されたか
        bool rejectedDisplayOnlyFormat{};
        // view非対応形式の拒否を確認
        try
        {
            static_cast<void>(
                LamaPon::TextureLoader::PrepareDdsTextureData(
                    BuildDx10Dds(
                        DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM,
                        std::vector<std::uint8_t>(64u))));
        }
        // 想定した形式エラーを記録
        catch (const std::invalid_argument&)
        {
            rejectedDisplayOnlyFormat = true;
        }
        Require(
            rejectedDisplayOnlyFormat,
            "A display-only DDS format without portable SRV support was "
            "accepted");
        // YUY2の画素ペア列
        std::vector<std::uint8_t> yuy2Payload;
        // 8組のYUY2画素を生成
        for (std::size_t pair{}; pair < 8u; ++pair)
        {
            yuy2Payload.insert(yuy2Payload.end(), { 81u, 90u, 145u, 240u });
        }
        // YUY2 DDS fixture
        auto yuy2Bytes = BuildDx10Dds(DXGI_FORMAT_YUY2, yuy2Payload);
        // RGBAへ変換したYUY2の解析結果
        const auto yuy2Prepared =
            LamaPon::TextureLoader::PrepareDdsTextureData(yuy2Bytes);
        Require(
            yuy2Prepared.format == DXGI_FORMAT_R8G8B8A8_UNORM
                && yuy2Prepared.levels.front().rowPitch == 16u
                && yuy2Prepared.levels.front().bytes.size() == 64u
                && yuy2Prepared.levels.front().bytes[0] >= 253u
                && yuy2Prepared.levels.front().bytes[1] == 0u
                && yuy2Prepared.levels.front().bytes[2] == 0u
                && yuy2Prepared.levels.front().bytes[3] == 255u
                && yuy2Prepared.levels.front().bytes[4] == 255u
                && yuy2Prepared.levels.front().bytes[5] >= 72u
                && yuy2Prepared.levels.front().bytes[5] <= 76u
                && yuy2Prepared.levels.front().bytes[6] >= 71u
                && yuy2Prepared.levels.front().bytes[6] <= 75u,
            "YUY2 DDS data was not converted to portable RGBA8 data");
        extendedDdsBytes.push_back(std::move(yuy2Bytes));

        // BC2形式のDDS fixture
        const auto bc2Bytes = BuildClassicDds(
            4u,
            4u,
            1u,
            MakeFourCc('D', 'X', 'T', '3'),
            std::vector<std::uint8_t>(16u));
        // BC4形式のDDS fixture
        const auto bc4Bytes = BuildClassicDds(
            4u,
            4u,
            1u,
            MakeFourCc('A', 'T', 'I', '1'),
            std::vector<std::uint8_t>(8u));
        const std::vector<std::pair<std::vector<std::uint8_t>, DXGI_FORMAT>>
            legacyCases{
                { BuildLegacyDds(0x41u, 32u, 0x3ff00000u, 0x000ffc00u,
                    0x000003ffu, 0xc0000000u,
                    std::vector<std::uint8_t>(64u)),
                    DXGI_FORMAT_R10G10B10A2_UNORM },
                { BuildLegacyDds(0x40u, 32u, 0x0000ffffu, 0xffff0000u,
                    0u, 0u, std::vector<std::uint8_t>(64u)),
                    DXGI_FORMAT_R16G16_UNORM },
                { BuildLegacyDds(0x41u, 16u, 0x7c00u, 0x03e0u,
                    0x001fu, 0x8000u, std::vector<std::uint8_t>(32u)),
                    DXGI_FORMAT_B5G5R5A1_UNORM },
                { BuildLegacyDds(0x40u, 16u, 0xf800u, 0x07e0u,
                    0x001fu, 0u, std::vector<std::uint8_t>(32u)),
                    DXGI_FORMAT_B5G6R5_UNORM },
                { BuildLegacyDds(0x41u, 16u, 0x0f00u, 0x00f0u,
                    0x000fu, 0xf000u, std::vector<std::uint8_t>(32u)),
                    DXGI_FORMAT_B4G4R4A4_UNORM },
                { BuildLegacyDds(0x20000u, 16u, 0xffffu, 0u,
                    0u, 0u, std::vector<std::uint8_t>(32u)),
                    DXGI_FORMAT_R16_UNORM },
                { BuildLegacyDds(0x2u, 8u, 0u, 0u,
                    0u, 0xffu, std::vector<std::uint8_t>(16u)),
                    DXGI_FORMAT_A8_UNORM },
                { BuildLegacyDds(0x80000u, 16u, 0x00ffu, 0xff00u,
                    0u, 0u, std::vector<std::uint8_t>(32u)),
                    DXGI_FORMAT_R8G8_SNORM },
                { BuildLegacyDds(0x80000u, 32u, 0x000000ffu,
                    0x0000ff00u, 0x00ff0000u, 0xff000000u,
                    std::vector<std::uint8_t>(64u)),
                    DXGI_FORMAT_R8G8B8A8_SNORM },
                { BuildLegacyDds(0x80000u, 32u, 0x0000ffffu,
                    0xffff0000u, 0u, 0u,
                    std::vector<std::uint8_t>(64u)),
                    DXGI_FORMAT_R16G16_SNORM },
                { BuildClassicDds(4u, 4u, 1u, 36u,
                    std::vector<std::uint8_t>(128u)),
                    DXGI_FORMAT_R16G16B16A16_UNORM },
                { BuildClassicDds(4u, 4u, 1u, 110u,
                    std::vector<std::uint8_t>(128u)),
                    DXGI_FORMAT_R16G16B16A16_SNORM },
                { BuildClassicDds(4u, 4u, 1u,
                    MakeFourCc('R', 'G', 'B', 'G'),
                    std::vector<std::uint8_t>(32u)),
                    DXGI_FORMAT_R8G8_B8G8_UNORM },
                { BuildClassicDds(4u, 4u, 1u,
                    MakeFourCc('G', 'R', 'G', 'B'),
                    std::vector<std::uint8_t>(32u)),
                    DXGI_FORMAT_G8R8_G8B8_UNORM },
                { BuildClassicDds(4u, 4u, 1u,
                    MakeFourCc('Y', 'U', 'Y', '2'),
                    std::vector<std::uint8_t>(32u)),
                    DXGI_FORMAT_R8G8B8A8_UNORM }
            };
        // 旧形式ごとのDXGI変換結果を検証
        for (const auto& [bytes, expectedFormat] : legacyCases)
        {
            Require(
                LamaPon::TextureLoader::PrepareDdsTextureData(bytes).format
                    == expectedFormat,
                "A DirectXTK-compatible legacy DDS format was not mapped");
        }
        Require(
            LamaPon::TextureLoader::PrepareDdsTextureData(bc1Bytes).format
                    == DXGI_FORMAT_BC1_UNORM
                && LamaPon::TextureLoader::PrepareDdsTextureData(bc3Bytes)
                        .format == DXGI_FORMAT_BC3_UNORM
                && LamaPon::TextureLoader::PrepareDdsTextureData(bc5Bytes)
                        .format == DXGI_FORMAT_BC5_UNORM
                && LamaPon::TextureLoader::PrepareDdsTextureData(bgraBytes)
                        .format == DXGI_FORMAT_B8G8R8A8_UNORM
                && LamaPon::TextureLoader::PrepareDdsTextureData(bc2Bytes)
                        .format == DXGI_FORMAT_BC2_UNORM
                && LamaPon::TextureLoader::PrepareDdsTextureData(bc4Bytes)
                        .format == DXGI_FORMAT_BC4_UNORM,
            "The DDS parser did not map classic and DX10 formats");

        // BC1 cube DDS
        auto cubeBytes = bc1Bytes;
        WriteLittleEndian32(cubeBytes, 112u, 0x200u);
        // Cube fixtureが拒否されたか
        bool rejectedCube{};
        // 2D loaderのcube拒否を確認
        try
        {
            static_cast<void>(
                LamaPon::TextureLoader::PrepareDdsTextureData(cubeBytes));
        }
        // 想定した形式エラーを記録
        catch (const std::invalid_argument&)
        {
            rejectedCube = true;
        }
        Require(
            rejectedCube,
            "The 2D DDS parser accepted a cube texture as a flat image");
        // 配列枚数を持つBGRA fixture
        auto arrayBytes = bgraBytes;
        WriteLittleEndian32(arrayBytes, 140u, 2u);
        arrayBytes.insert(
            arrayBytes.end(),
            bgraPixels.begin(),
            bgraPixels.end());
        // 配列fixtureが拒否されたか
        bool rejectedArray{};
        // 2D loaderの配列拒否を確認
        try
        {
            static_cast<void>(
                LamaPon::TextureLoader::PrepareDdsTextureData(arrayBytes));
        }
        // 想定した形式エラーを記録
        catch (const std::invalid_argument&)
        {
            rejectedArray = true;
        }
        Require(
            rejectedArray,
            "The single-2D DDS parser accepted a DX10 texture array");
        // texture arrayの解析結果
        const auto arrayPrepared =
            LamaPon::TextureLoader::PrepareDdsResourceData(arrayBytes);

        // Volume texture pixels
        std::vector<std::uint8_t> rgbaPixels;
        // volume画像の画素を生成
        for (std::size_t pixel{}; pixel < 16u; ++pixel)
        {
            rgbaPixels.insert(
                rgbaPixels.end(),
                { 40u, 170u, 230u, 255u });
        }
        // Volume DDS fixture
        auto volumeBytes = BuildDx10Dds(
            DXGI_FORMAT_R8G8B8A8_UNORM,
            rgbaPixels);
        WriteLittleEndian32(volumeBytes, 24u, 2u);
        WriteLittleEndian32(volumeBytes, 112u, 0x200000u);
        WriteLittleEndian32(volumeBytes, 132u, 4u);
        volumeBytes.insert(
            volumeBytes.end(),
            rgbaPixels.begin(),
            rgbaPixels.end());
        // volume textureの解析結果
        const auto volumePrepared =
            LamaPon::TextureLoader::PrepareDdsResourceData(volumeBytes);
        Require(
            arrayPrepared.dimension
                    == LamaPon::TextureLoader::
                        PreparedDdsTextureDimension::Texture2DArray
                && arrayPrepared.arraySize == 2u
                && arrayPrepared.mipLevels == 1u
                && arrayPrepared.subresources.size() == 2u
                && volumePrepared.dimension
                    == LamaPon::TextureLoader::
                        PreparedDdsTextureDimension::Texture3D
                && volumePrepared.depth == 2u
                && volumePrepared.subresources.size() == 1u
                && volumePrepared.subresources.front().bytes.size()
                    == 128u,
            "The DDS resource parser did not preserve array and volume "
            "dimensions");

        // Shared test window
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        {
        // DX11 test device
            LamaPon::GraphicsDevice d3d11Graphics;
            d3d11Graphics.Initialize(
                window.Get(),
                CanvasWidth,
                CanvasHeight,
                LamaPon::RenderingApi::DirectX11);
            // DX11 DDS views
            for (const auto& bytes : extendedDdsBytes)
            {
                // 現在fixtureの解析結果
                const auto prepared =
                    LamaPon::TextureLoader::PrepareDdsTextureData(bytes);
                // DirectX 11でネイティブview生成を確認
                try
                {
                    // 生成したDirectX 11 view
                    const auto view = d3d11Graphics.Assets()
                        .CreateTextureViewHandleFromMemory(bytes, true);
                    Require(
                        view && d3d11Graphics.IsGraphicsViewCurrent(view),
                        "An extended DDS format did not create a DirectX 11 "
                        "view");
                }
                // native生成失敗を形式名付きで報告
                catch (const std::exception& exception)
                {
                    // 描画fixtureの失敗を例外として通知します。
                    throw std::runtime_error(
                        "DirectX 11 DDS format "
                        + std::to_string(
                            static_cast<unsigned>(prepared.format))
                        + " failed native creation: " + exception.what());
                }
            }
        }
        // DX12 test device
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // DX12 DDS views
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
        // 生成viewの所有backendを検証
        for (const auto& view : views)
        {
            Require(
                graphics.IsGraphicsViewCurrent(view),
                "A parsed DDS view was not created by the DirectX 12 "
                "backend");
        }
            // DX12 DDS data
        for (const auto& bytes : extendedDdsBytes)
        {
            // 現在fixtureの解析結果
            const auto prepared =
                LamaPon::TextureLoader::PrepareDdsTextureData(bytes);
            // DirectX 12でネイティブview生成を確認
            try
            {
                // 生成したDirectX 12 view
                const auto view =
                    graphics.Assets().CreateTextureViewHandleFromMemory(
                        bytes,
                        true);
                Require(
                    view && graphics.IsGraphicsViewCurrent(view),
                    "An extended DDS format did not create a DirectX 12 view");
            }
            // native生成失敗を形式名付きで報告
            catch (const std::exception& exception)
            {
                // 描画fixtureの失敗を例外として通知します。
                throw std::runtime_error(
                    "DirectX 12 DDS format "
                    + std::to_string(static_cast<unsigned>(prepared.format))
                    + " failed native creation: " + exception.what());
            }
        }
        // DX12 array view
        const auto arrayView =
            graphics.Assets().CreateTextureViewHandleFromMemory(
                arrayBytes,
                true);
        // DX12 volume view
        const auto volumeView =
            graphics.Assets().CreateTextureViewHandleFromMemory(
                volumeBytes,
                true);
        Require(
            graphics.IsGraphicsViewCurrent(arrayView)
                && graphics.IsGraphicsViewCurrent(volumeView),
            "DirectX 12 did not create DDS array and volume views");

        // 比較用フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        {
            // DDS viewを描画するパス
            auto pass = graphics.BeginSpritePass();
            // 各viewを順に画面へ描画
            for (std::size_t index{}; index < views.size(); ++index)
            {
                // 現在viewの描画要求
                LamaPon::SpriteDrawRequest request;
                request.texture = views[index];
                request.position = {
                    static_cast<float>(index * 48u),
                    0.0f };
                request.scale = { 12.0f, 32.0f };
                Require(pass.Draw(request), "A DDS sprite was rejected");
            }
        }
        // 撮影画像の幅
        std::uint32_t width{};
        // 撮影画像の高さ
        std::uint32_t height{};
        // DDSスプライトの撮影画像
        const auto pixels = graphics.CaptureBackBuffer(width, height);
        graphics.EndFrame();
        // channel(x:横,c:色)
        const auto channel = [&](const std::uint32_t x, const std::size_t c)
        {
            // 撮影したpixel配列を返します。
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

    // 対数平均に基づく自動露出値を検証します。
    // 左右の幾何平均0.5を4フレーム後に確認します。
    void RequireD3D12AutoExposure()
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 自動露出を計測する描画デバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);

        // 自動露出を適用する描画先
        LamaPon::RenderTarget target;
        graphics.ResizeOffscreenTarget(target, 64u, 32u);
        // 自動露出設定
        LamaPon::AutoExposureSettings settings;
        settings.enabled = true;
        // 測定値は次フレーム以降に非同期で読むため、数フレーム描きます。
        for (int frame{}; frame < 4; ++frame)
        {
            // 各計測フレームの背景色
            constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
            graphics.BeginFrame(clearColor);
            // 主描画前の出力状態
            const auto primaryOutput = graphics.CaptureOutputState();
            graphics.BeginOffscreenTarget(target, clearColor);
            {
                // 輝度サンプルを描くパス
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
            // 画面の読み出しでGPUの完了を待ち、次のフレームで測定値を確実に読めるようにします。
            // GPU完了待ち用の画像寸法
            std::uint32_t width{};
            // 読み出し先の高さ
            std::uint32_t height{};
            static_cast<void>(graphics.CaptureBackBuffer(width, height));
            graphics.EndFrame();
        }

        // 期待する露出補正段数
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

    // Cubeと床でAO差を測るSceneを構築します。
    // 床もCubeにして深度形状を揃え、近平面に収めます。
    // BuildAmbientOcclusionScene(scene: 構築先Scene)
    void BuildAmbientOcclusionScene(LamaPon::Scene& scene)
    {
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 2.4f, 6.0f };
        cameraObject.GetTransform().SetEulerAngles(-0.42f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // 接地面の床オブジェクト
        auto& floor = scene.CreateGameObject("Floor");
        floor.GetTransform().position = { 0.0f, -1.7f, 0.0f };
        floor.GetTransform().scale = { 8.0f, 1.0f, 8.0f };
        floor.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });
        // 遮蔽を作るCube
        auto& cube = scene.CreateGameObject("Cube");
        cube.GetTransform().position = { 0.0f, -0.7f, 0.0f };
        cube.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });

        // シーンの遮蔽設定
        auto occlusion = scene.AmbientOcclusion();
        occlusion.enabled = true;
        occlusion.radius = 0.75f;
        occlusion.strength = 1.0f;
        scene.SetAmbientOcclusionSettings(occlusion);
    }

    // EnableAmbientOcclusionQuality(graphics: 描画デバイス)
    // AOを有効にして検証用の標本数を設定します。
    void EnableAmbientOcclusionQuality(LamaPon::GraphicsDevice& graphics)
    {
        // 変更対象の描画設定
        auto settings = graphics.Settings();
        settings.ambientOcclusionEnabled = true;
        settings.ambientOcclusionSampleCount = 16u;
        graphics.SetGraphicsSettings(settings);
    }

    // 深度から求めたAO画像を指定APIで撮影します。
    // RenderAmbientOcclusionCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderAmbientOcclusionCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // AOを描くグラフィックスデバイス
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
        // AO検証シーン
        LamaPon::Scene scene(graphics);
        BuildAmbientOcclusionScene(scene);

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        // scene composition描画先
        auto* const target = graphics.SceneCompositionTarget();
        Require(
            target != nullptr,
            "The ambient occlusion capture has no scene composition target");
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            target);
        // 深度から生成したAO view
        const auto occlusionView = target->AmbientOcclusionViewHandle();
        // Lightingへ反映されたAO状態
        const auto& occlusion = graphics.Lighting().screenAmbientOcclusion;
        Require(
            occlusion.enabled
                && occlusion.texture == occlusionView
                && graphics.IsGraphicsViewCurrent(occlusionView),
            "The scene did not resolve SSAO from its depth prepass");
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        {
            // AO画像の表示パス
            auto pass = graphics.BeginSpritePass();
            // AO viewを画面へ写す要求
            LamaPon::SpriteDrawRequest request;
            request.texture = occlusionView;
            request.tint = { 1.0f, 1.0f, 1.0f, 1.0f };
            Require(
                pass.Draw(request),
                "The ambient occlusion view was rejected");
        }
        // AO viewの撮影結果
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();
        // 撮影したframe captureを返します。
        return capture;
    }

    // D3D11/12の遮蔽画像を画素単位で比較します。
    // RequireMatchingAmbientOcclusionCaptures(d3d11: D3D11画像, d3d12: D3D12画像)
    void RequireMatchingAmbientOcclusionCaptures(
        const Capture& d3d11,
        const Capture& d3d12)
    {
        // 期待するRGBA画像サイズ
        const std::size_t expectedBytes =
            static_cast<std::size_t>(CanvasWidth) * CanvasHeight * 4u;
        // 両API画像の寸法と格納量を検証
        for (const auto* const capture : { &d3d11, &d3d12 })
        {
            Require(
                capture->width == CanvasWidth
                    && capture->height == CanvasHeight
                    && capture->pixels.size() == expectedBytes,
                "The ambient occlusion captures have unexpected dimensions");
        }

        // 遮蔽textureはR8なので、Spriteで写すと赤だけに値が入ります。
        // 半解像度AO画像の幅
        constexpr std::uint32_t OcclusionWidth = CanvasWidth / 2u;
        // 半解像度AO画像の高さ
        constexpr std::uint32_t OcclusionHeight = CanvasHeight / 2u;
        // WARPで許容する丸め誤差
        constexpr int ChannelTolerance = 2;
        // D3D11で遮蔽された画素数
        std::size_t d3d11OccludedPixels{};
        // D3D12で遮蔽された画素数
        std::size_t d3d12OccludedPixels{};
        // 許容差を超えた画素数
        std::size_t mismatchedPixels{};
        // 最初に差が出た画素
        std::size_t firstMismatch =
            std::numeric_limits<std::size_t>::max();
        // 最大チャンネル差
        int largestDifference{};
        // 半解像度AO画像を走査
        for (std::uint32_t y{}; y < OcclusionHeight; ++y)
        {
            // 現在行の画素を走査
            for (std::uint32_t x{}; x < OcclusionWidth; ++x)
            {
                // RGBA画像内の現在画素番号
                const std::size_t pixel =
                    static_cast<std::size_t>(y) * CanvasWidth + x;
                // D3D11のAO値
                const int d3d11Value = d3d11.pixels[pixel * 4u];
                // D3D12のAO値
                const int d3d12Value = d3d12.pixels[pixel * 4u];
                // 接地部の遮蔽はブラー後で約0.8〜0.9の明るさです。
                d3d11OccludedPixels += d3d11Value < 240 ? 1u : 0u;
                d3d12OccludedPixels += d3d12Value < 240 ? 1u : 0u;
                // 両APIのAO値の差
                const int difference = std::abs(d3d11Value - d3d12Value);
                largestDifference = std::max(largestDifference, difference);
                // 許容差を超えた画素を記録
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
        // 差がなければ検証成功
        if (mismatchedPixels == 0)
        {
            // pixel差分がないため比較処理を終了します。
            return;
        }
        // 描画fixtureの失敗を例外として通知します。
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

    // SSAOが環境光へだけ影響することを検証します。
    // 環境光なしでは有効・無効の画像が一致します。
    void RequireD3D12AmbientOcclusion()
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // SSAO検証用の描画デバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        EnableAmbientOcclusionQuality(graphics);
        // SSAO検証シーン
        LamaPon::Scene scene(graphics);
        BuildAmbientOcclusionScene(scene);

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.02f, 0.03f, 0.05f, 1.0f };
        // 合成画像を撮影
        const auto captureComposition = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 撮影画像の幅
            std::uint32_t width{};
            // 撮影画像の高さ
            std::uint32_t height{};
            // 取得した合成画像
            auto capture = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 SSAO capture has unexpected dimensions");
            // 撮影したframe captureを返します。
            return capture;
        };
        // setOcclusionEnabled(enabled:AO有効)
        const auto setOcclusionEnabled = [&](const bool enabled)
        {
            // 変更対象のAO設定
            auto occlusion = scene.AmbientOcclusion();
            occlusion.enabled = enabled;
            scene.SetAmbientOcclusionSettings(occlusion);
        };

        setOcclusionEnabled(false);
        // SSAO無効時の合成画像
        const auto withoutOcclusion = captureComposition();
        Require(
            !graphics.Lighting().screenAmbientOcclusion.enabled,
            "Disabled SSAO reached the DirectX 12 lighting state");
        setOcclusionEnabled(true);
        // SSAO有効時の合成画像
        const auto withOcclusion = captureComposition();
        Require(
            graphics.Lighting().screenAmbientOcclusion.enabled,
            "The DirectX 12 scene did not apply its resolved SSAO");

        // 暗くなった画素数
        std::size_t darkenedPixels{};
        // 明るくなった画素があるか
        bool brightened{};
        // SSAO前後の各画素を比較
        for (std::size_t offset{};
             offset + 3u < withOcclusion.size();
             offset += 4u)
        {
            // RGBの明るさ変化を確認
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                brightened = brightened
                    || withOcclusion[offset + channel]
                        > withoutOcclusion[offset + channel] + 1u;
            }
            // 緑成分が暗くなった画素を数える
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
        // 直接光だけを加えるライト
        auto& lightObject = scene.CreateGameObject("Sun");
        lightObject.AddComponent<LamaPon::DirectionalLightComponent>();
        // 環境光なし・SSAOなしの画像
        const auto directOnly = captureComposition();
        setOcclusionEnabled(true);
        // 環境光なし・SSAOありの画像
        const auto directOnlyWithOcclusion = captureComposition();
        // 直接光で明るい画素数
        std::size_t litPixels{};
        // 直接光の画素を走査
        for (std::size_t offset{};
             offset + 3u < directOnly.size();
             offset += 4u)
        {
            // 明るい画素を数える
            if (directOnly[offset + 1u] > 40u)
            {
                ++litPixels;
            }
        }
        Require(
            litPixels > 300u && directOnlyWithOcclusion == directOnly,
            "DirectX 12 SSAO changed a scene without ambient light");
    }

    // 鏡面床と赤いCubeを使うSSR検証Sceneを構築します。
    // 床とCubeの頂点順を両backendで揃えます。
    // BuildScreenSpaceReflectionScene(scene: 構築先Scene)
    void BuildScreenSpaceReflectionScene(LamaPon::Scene& scene)
    {
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 2.0f, 5.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.38f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // 床とCubeは頂点と添字を明示したProcedural Meshで作ります。
        // D3D11のGeometricPrimitiveとD3D12の基本形状は三角形の頂点順が異なり、SSRのように自分の面と深度を比べる判定では補間の丸めの差が面ごとの当たり外れになるためです。
        // addFace(vertices: 頂点列, indices: 索引列, normal: 面法線, corners: 面頂点)
        const auto addFace = [](
            std::vector<LamaPon::ProceduralMeshVertex>& vertices,
            std::vector<std::uint32_t>& indices,
            const DirectX::XMFLOAT3& normal,
            const std::array<DirectX::XMFLOAT3, 4>& corners)
        {
            // cornersは外から見た左下、左上、右上、右下です。
            // D3D11が裏面として捨てないよう、画面上で時計回りの三角形にします。
            // 追加する面の先頭頂点
            const auto first =
                static_cast<std::uint32_t>(vertices.size());
            // 面の頂点を登録
            for (const auto& corner : corners)
            {
                vertices.push_back({ corner, normal, { 0.0f, 0.0f } });
            }
            indices.insert(
                indices.end(),
                { first, first + 1u, first + 2u,
                    first, first + 2u, first + 3u });
        };

        // 床の頂点列
        std::vector<LamaPon::ProceduralMeshVertex> floorVertices;
        // 床の索引列
        std::vector<std::uint32_t> floorIndices;
        addFace(floorVertices, floorIndices, { 0.0f, 1.0f, 0.0f }, { {
            { -4.0f, -1.2f, 4.0f }, { -4.0f, -1.2f, -4.0f },
            { 4.0f, -1.2f, -4.0f }, { 4.0f, -1.2f, 4.0f } } });
        // 床オブジェクト
        auto& floor = scene.CreateGameObject("Floor");
        // 床のメッシュ描画部品
        auto& floorMesh = floor.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.7f, 0.7f, 0.7f, 1.0f });
        floorMesh.SetProceduralMesh(floorVertices, floorIndices);
        floorMesh.SetMetallic(1.0f);
        floorMesh.SetRoughness(0.1f);

        // Cubeの半辺長
        constexpr float h = 0.5f;
        // Cubeの中心高
        constexpr float y = -0.7f;
        // Cubeの頂点列
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // Cubeの索引列
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
        // 赤い反射対象Cube
        auto& cube = scene.CreateGameObject("Cube");
        // Cubeのメッシュ描画部品
        auto& cubeMesh = cube.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.9f, 0.15f, 0.1f, 1.0f });
        cubeMesh.SetProceduralMesh(cubeVertices, cubeIndices);
        cubeMesh.SetMetallic(0.0f);
        cubeMesh.SetRoughness(0.8f);

        // シーンのSSR設定
        auto reflection = scene.ScreenSpaceReflection();
        reflection.enabled = true;
        scene.SetScreenSpaceReflectionSettings(reflection);
    }

    // SSRの履歴を更新して指定APIの合成画像を撮影します。
    // 前フレームのHDR色を使うため2フレーム描画します。
    // RenderScreenSpaceReflectionCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderScreenSpaceReflectionCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // SSRを描くグラフィックスデバイス
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
        // SSR検証シーン
        LamaPon::Scene scene(graphics);
        BuildScreenSpaceReflectionScene(scene);

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 最終フレームの撮影結果
        Capture capture;
        // SSR履歴を作る2フレームを描画
        for (int frame{}; frame < 2; ++frame)
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            // scene composition描画先
            auto* const target = graphics.SceneCompositionTarget();
            Require(
                target != nullptr,
                "The screen-space reflection capture has no scene "
                "composition target");
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                target);
            // 履歴が揃う2フレーム目を検証
            if (frame == 1)
            {
                // 解決済みSSR状態
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // depth-probeでシーン深度の受け渡しを検証します。
    // shaderは距離をRGへ出力し、欠損時は青を出します。
    // RenderScreenEffectDepthCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderScreenEffectDepthCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Depth probe device
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
        // SSR用の深度を持つシーン
        LamaPon::Scene scene(graphics);
        BuildScreenSpaceReflectionScene(scene);
        // SSR出力を無効化して深度だけを検査
        auto reflection = scene.ScreenSpaceReflection();
        reflection.enabled = false;
        scene.SetScreenSpaceReflectionSettings(reflection);

        // 深度probe合成先の背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            graphics.SceneCompositionTarget());
        // 深度probe要求
        LamaPon::ScreenEffectRequest request;
        request.shader =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/depth-probe.hlsl";
        // 距離を12mで割った値を書きます。
        request.customParameters[0] = { 12.0f, 0.0f, 0.0f, 0.0f };
        request.point = LamaPon::ScreenEffectPoint::AfterToneMapping;
        // queue結果の世代番号
        std::uint64_t generation{};
        // queue失敗時の診断文字列
        std::string error;
        Require(
            graphics.QueueScreenEffect(request, &generation, &error)
                && generation != 0
                && error.empty(),
            "The depth screen effect was not queued: " + error);
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        // 深度probeの撮影結果
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        // 深度が欠けるとShaderは青を返します。
        // Cubeと床と背景で距離が違うため、赤（距離の上位）は画面内で大きく変わります。
        // 欠損深度の青画素数
        std::size_t missingDepthPixels{};
        // 最小の赤成分
        int minimumRed = 255;
        // 最大の赤成分
        int maximumRed = 0;
        // 撮影画像の深度出力を走査
        for (std::size_t offset{};
            offset + 3u < capture.pixels.size();
            offset += 4u)
        {
            // 青出力を深度欠損として数える
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // auxiliary-probeで補助textureのbindingを検証します。
    // 未指定のtexture slotは白として扱われます。
    // RenderScreenEffectAuxiliaryCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderScreenEffectAuxiliaryCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Auxiliary probe device
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
        // probe用シェーダーとtextureのasset root
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        // auxiliary-probe要求
        LamaPon::ScreenEffectRequest request;
        request.shader =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/auxiliary-probe.hlsl";
        request.auxiliaryTextures[0] = "textures/LamaPonLogo.png";
        request.customParameters[0] = { 0.8f, 0.0f, 0.0f, 0.0f };
        request.point = LamaPon::ScreenEffectPoint::AfterToneMapping;
        // queue結果の世代番号
        std::uint64_t generation{};
        // queue失敗時の診断文字列
        std::string error;
        Require(
            graphics.QueueScreenEffect(request, &generation, &error)
                && generation != 0
                && error.empty(),
            "The texture screen effect was not queued: " + error);
        // 空のpost-process設定
        LamaPon::PostProcessFrame frame;
        graphics.EndSceneComposition(frame);
        // auxiliary-probeの撮影結果
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        // 右半分はt2を指定していないため、白へ倍率0.8を掛けた色です。
        // 右中央のRGBA位置
        const std::size_t rightCenter =
            (static_cast<std::size_t>(CanvasHeight / 2u) * CanvasWidth
                + CanvasWidth * 3u / 4u) * 4u;
        // 右側が白fallbackの色か
        bool whiteFallback = capture.pixels.size() > rightCenter + 3u;
        // 右中央のRGB成分を確認
        for (std::size_t channel{}; whiteFallback && channel < 3u; ++channel)
        {
            whiteFallback =
                std::abs(
                    static_cast<int>(capture.pixels[rightCenter + channel])
                    - 204) <= 2;
        }
        // 左半分はLogo画像を引き伸ばすため、一様な色になりません。
        // 左領域の最小輝度
        int minimumLeft = 255;
        // 左領域の最大輝度
        int maximumLeft = 0;
        // Logo領域を行ごとに走査
        for (std::uint32_t y{}; y < CanvasHeight; ++y)
        {
            // 左半分の画素を走査
            for (std::uint32_t x{}; x < CanvasWidth / 2u; ++x)
            {
                // 現在画素のRGBA位置
                const auto offset =
                    (static_cast<std::size_t>(y) * CanvasWidth + x) * 4u;
                // 格納外の画素を除外
                if (offset + 3u >= capture.pixels.size())
                {
                    // 次の画素へ
                    continue;
                }
                // 現在画素のRGB輝度
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // 不正なScreenEffect/ComputeEffectの拒否を検証します。
    // 失敗時は診断を返し、要求をqueueしません。
    void RequireD3D12CustomShaderFailures()
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // shader検証対象の描画デバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // Shader fixtures
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        // 失敗するscreen effect要求
        LamaPon::ScreenEffectRequest request;
        request.shader = fixtures / "broken-shader.hlsl";
        // queue失敗を表す世代番号
        std::uint64_t generation{ 1 };
        // queue失敗時の診断文字列
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

        // 失敗するcompute effect要求
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

        // 失敗時は要求を積みません。
        // Clear color.
        constexpr float clearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        // ポストプロセスなしの合成設定
        LamaPon::PostProcessFrame frame;
        graphics.EndSceneComposition(frame);
        graphics.EndFrame();
    }

    // ComputeEffectの出力を全画面へ表示して撮影します。
    // 100x60の出力で端数thread groupと未指定入力を検証します。
    // RenderComputeEffectCapture(api: 描画API, profile: 起動設定, readInputs: 入力textureを読むか)
    [[nodiscard]] Capture RenderComputeEffectCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile,
        const bool readInputs)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Compute test device
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
        // Compute fixtures
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        // compute probe要求
        LamaPon::ComputeEffectRequest request;
        request.shader = fixtures
            / (readInputs ? "compute-input-probe.hlsl" : "compute-probe.hlsl");
        request.outputTexture =
            readInputs ? "computeInputProbe" : "computeProbe";
        request.outputWidth = 100;
        request.outputHeight = 60;
        request.customParameters[0] = { 0.75f, 0.0f, 0.0f, 0.0f };
        // 入力texture付きprobeだけLogoを登録
        if (readInputs)
        {
            request.inputTextures[0] = "textures/LamaPonLogo.png";
        }

        // 出力表示用の背景色
        constexpr float clearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };
        graphics.BeginFrame(clearColor);
        // Compute error text
        std::string error;
        Require(
            graphics.DispatchComputeEffect(request, &error) && error.empty(),
            "The compute effect did not run: " + error);
        // compute出力の表示用view
        const auto output =
            graphics.RenderTextureViewHandle(request.outputTexture);
        Require(
            static_cast<bool>(output),
            "The compute effect output texture is unavailable");
        {
            // 置換描画のpass設定
            LamaPon::SpritePassDescription description;
            description.blend = LamaPon::SpriteBlendMode::Opaque;
            // compute出力を表示するpass
            auto pass = graphics.BeginSpritePass(description);
            // 出力textureを全画面へ描く要求
            LamaPon::SpriteDrawRequest sprite;
            sprite.texture = output;
            sprite.scale = {
                static_cast<float>(CanvasWidth) / request.outputWidth,
                static_cast<float>(CanvasHeight) / request.outputHeight };
            Require(pass.Draw(sprite), "The compute output sprite was rejected");
        }
        // compute出力の撮影結果
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

        // pixel(x:横,y:縦)
        const auto pixel = [&capture](
            const std::uint32_t x,
            const std::uint32_t y)
        {
            // RGBA画像内の現在画素位置
            const auto offset =
                (static_cast<std::size_t>(y) * CanvasWidth + x) * 4u;
            // pixelのRGB channel値を返します。
            return std::array<int, 3>{
                capture.pixels[offset],
                capture.pixels[offset + 1u],
                capture.pixels[offset + 2u] };
        };
        // describe(color:RGB)
        const auto describe = [](const std::array<int, 3>& color)
        {
            // pixel値を比較診断用文字列にします。
            return std::to_string(color[0]) + ", "
                + std::to_string(color[1]) + ", "
                + std::to_string(color[2]);
        };
        // 入力textureの反映結果を検証
        if (readInputs)
        {
            // 右半分はt1を指定していないため、白へ倍率0.75を掛けた色です。
            // 右側の中央画素
            const auto right = pixel(CanvasWidth * 3u / 4u, CanvasHeight / 2u);
            // 右側の未指定slotが白か
            bool whiteFallback = true;
            // 白fallbackのRGB成分を確認
            for (const int channel : right)
            {
                whiteFallback = whiteFallback && std::abs(channel - 191) <= 2;
            }
            // 左半分はLogo画像を引き伸ばすため、一様な色になりません。
            // Logo領域の最小輝度
            int minimumLeft = 765;
            // Logo領域の最大輝度
            int maximumLeft = 0;
            // Logo領域を行ごとに走査
            for (std::uint32_t y{}; y < CanvasHeight; ++y)
            {
                // 左側の画素を走査
                for (std::uint32_t x{}; x < CanvasWidth / 2u - 2u; ++x)
                {
                    // 現在画素のRGB色
                    const auto color = pixel(x, y);
                    // 現在画素の輝度
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
        // 固定probe模様の出力を検証
        else
        {
            // 左半分は赤0.75、右半分は上から下への緑のグラデーションです。
            // 赤い領域の中央画素
            const auto left = pixel(CanvasWidth / 4u, CanvasHeight / 2u);
            // 緑グラデーション上端の画素
            const auto top = pixel(CanvasWidth * 3u / 4u, 4u);
            // 緑グラデーション下端の画素
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // API間で頂点順を揃えた単位Cubeを生成します。
    // BuildProceduralCube(vertices: 頂点出力, indices: 索引出力)
    void BuildProceduralCube(
        std::vector<LamaPon::ProceduralMeshVertex>& vertices,
        std::vector<std::uint32_t>& indices)
    {
        // addFace(normal:法線,corners:面)
        const auto addFace = [&vertices, &indices](
            const DirectX::XMFLOAT3& normal,
            const std::array<DirectX::XMFLOAT3, 4>& corners)
        {
            // cornersは外から見た左下、左上、右上、右下で、D3D11が裏面として捨てないよう画面上で時計回りの三角形にします。
            // 面の先頭頂点番号
            const auto first = static_cast<std::uint32_t>(vertices.size());
            // 四頂点に対応するUV
            const std::array<DirectX::XMFLOAT2, 4> textureCoordinates{ {
                { 0.0f, 1.0f },
                { 0.0f, 0.0f },
                { 1.0f, 0.0f },
                { 1.0f, 1.0f } } };
            // 面の頂点を追加
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
        // Cubeの半辺長
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
        // shader適用前の基準画像
        Capture baseline;
        // shader適用後の画像
        Capture frame;
        // 各shaderの診断文字列
        std::array<std::string, 4> errors;
    };

    // Material shaderと特殊passの出力を指定APIで検証します。
    // 雛形・lighting probe・variant・compile失敗shaderを比較します。
    // RenderMaterialShaderCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] MaterialShaderCapture RenderMaterialShaderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Material test device
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

        // 複数のテスト材質を含むシーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.5f, 5.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.08f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.35f);
        // シーンの太陽光
        auto& sun = scene.CreateGameObject("Sun");
        sun.GetTransform().SetEulerAngles(0.8f, -0.5f, 0.0f);
        sun.AddComponent<LamaPon::DirectionalLightComponent>();
        // 追加する点光源
        auto& bulb = scene.CreateGameObject("PointLight");
        bulb.GetTransform().position = { 0.0f, 1.2f, 1.6f };
        bulb.AddComponent<LamaPon::PointLightComponent>(
            DirectX::XMFLOAT3{ 1.0f, 0.8f, 0.6f },
            3.0f,
            2.0f);

        // 全Cubeで共有する頂点列
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 全Cubeで共有する索引列
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // Shader fixtures
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        // addCube(name:名前,x:X,color:tint,albedo:画像)
        const auto addCube = [&](
            const char* const name,
            const float x,
            const DirectX::XMFLOAT4& color,
            std::filesystem::path albedo) -> LamaPon::MeshRendererComponent&
        {
            // 生成するCube object
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.2f, 1.2f, 1.2f };
            object.GetTransform().SetEulerAngles(0.5f, 0.7f, 0.0f);
            // CubeのMeshRenderer
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                color,
                std::move(albedo));
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
            // 構築したmesh fixtureを返します。
            return mesh;
        };

        // 雛形shader用のCube
        auto& templateMesh = addCube(
            "TemplateMaterial",
            -2.25f,
            { 1.0f, 1.0f, 1.0f, 1.0f },
            "textures/LamaPonLogo.png");
        templateMesh.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        templateMesh.SetCustomParameter(0, { 1.0f, 0.55f, 0.2f, 0.5f });
        templateMesh.SetCustomParameter(1, { 0.3f, 0.9f, 0.0f, 0.0f });
        templateMesh.SetCustomParameter(2, { 1.0f, 1.0f, 0.0f, 0.0f });

        // Lighting probe cube
        auto& lightingMesh = addCube(
            "LightingProbe",
            -0.75f,
            { 0.9f, 0.9f, 0.9f, 0.7f },
            "textures/LamaPonLogo.png");
        lightingMesh.SetShaderPath(fixtures / "material-lighting-probe.hlsl");
        lightingMesh.SetCustomTexturePath(0, "textures/particle-glow.png");
        lightingMesh.SetCustomVector(0, { 0.05f, 0.1f, 0.0f, 0.0f });
        lightingMesh.SetCustomParameter(7, { 0.0f, 0.0f, 0.0f, 1.0f });

        // Variant probe cube
        auto& variantMesh = addCube(
            "VariantProbe",
            0.75f,
            { 1.0f, 1.0f, 1.0f, 1.0f },
            {});
        variantMesh.SetShaderPath(fixtures / "variant-probe.hlsl");
        variantMesh.EnableShaderKeyword("VARIANT_PROBE_GREEN");
        variantMesh.EnableShaderKeyword("VARIANT_PROBE_BRIGHT");

        // Fallback probe cube
        auto& brokenMesh = addCube(
            "BrokenShader",
            2.25f,
            { 1.0f, 1.0f, 1.0f, 1.0f },
            {});
        brokenMesh.SetShaderPath(fixtures / "broken-shader.hlsl");

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            graphics.SceneCompositionTarget());
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        // Material shaderの撮影結果
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

        // keyword付きのvariantは緑、compileに失敗したShaderはマゼンタで描かれます。
        // fallback表示のマゼンタ画素数
        std::size_t magentaPixels{};
        // variant出力の緑画素数
        std::size_t greenPixels{};
        // 出力画像のshader表示色を集計
        for (std::size_t offset{};
            offset + 3u < capture.frame.pixels.size();
            offset += 4u)
        {
            // 現在画素の赤成分
            const int red = capture.frame.pixels[offset];
            // 現在画素の緑成分
            const int green = capture.frame.pixels[offset + 1u];
            // 現在画素の青成分
            const int blue = capture.frame.pixels[offset + 2u];
            // マゼンタfallbackを集計
            if (red > 150 && blue > 150 && green < 80)
            {
                ++magentaPixels;
            }
            // variantの緑出力を集計
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // Skinned modelのMaterial shaderと追加passを検証します。
    // animation・outline・occluded pass・compile失敗表示を比較します。
    // RenderSkinnedMaterialShaderCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] MaterialShaderCapture RenderSkinnedMaterialShaderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Skinned test device
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
        // 検証対象の雛形shader
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
        // D3D11のdepth/raster状態を比較
        if (api == LamaPon::RenderingApi::DirectX11)
        {
            // D3D11 pipeline state
            auto& states = LamaPon::Detail::GraphicsDeviceD3D11Access::States(
                graphics);
            // 輪郭pass用depth state
            D3D11_DEPTH_STENCIL_DESC depthDescription{};
            states.DepthDefault()->GetDesc(&depthDescription);
            // 輪郭pass用rasterizer state
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
            // skinned shader生成の世代番号
            std::uint64_t generation{};
            // shader取得時の診断
            std::string error;
            // Skinned material effect
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

        // animated modelを描くシーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 8.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.9f);

        // スキニングFBXのパス
        const auto modelPath = std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "AnimatedSausage.fbx";
        // FBXのCPU側モデル情報
        const auto modelAsset = graphics.Assets().LoadModel(modelPath);
        Require(
            modelAsset != nullptr
                && modelAsset->skeletalModel != nullptr
                && modelAsset->skeletalModel->hasLocalBounds,
            "The skinned material test model could not be loaded");
        // モデルのローカル境界
        const auto& bounds = modelAsset->skeletalModel->localBounds;
        // モデル境界の中心
        const DirectX::XMFLOAT3 modelCenter{
            (bounds.minimum.x + bounds.maximum.x) * 0.5f,
            (bounds.minimum.y + bounds.maximum.y) * 0.5f,
            (bounds.minimum.z + bounds.maximum.z) * 0.5f };
        // モデル境界の最大辺
        const float modelExtent = std::max({
            bounds.maximum.x - bounds.minimum.x,
            bounds.maximum.y - bounds.minimum.y,
            bounds.maximum.z - bounds.minimum.z,
            0.001f });
        // カメラ内へ収める一様倍率
        const float modelScale = 2.5f / modelExtent;
        // Shader fixtures
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        // addModel(name:名前,x:X)
        const auto addModel = [&](
            const char* const name,
            const float x) -> LamaPon::ModelRendererComponent&
        {
            // 生成するモデルobject
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = {
                x - modelCenter.x * modelScale,
                -modelCenter.y * modelScale,
                -modelCenter.z * modelScale };
            object.GetTransform().scale = {
                modelScale,
                modelScale,
                modelScale };
            // objectへ追加するmodel部品
            auto& model =
                object.AddComponent<LamaPon::ModelRendererComponent>(modelPath);
            model.SetAnimationPlayOnStart(false);
            // 構築したmodel fixtureを返します。
            return model;
        };
        // 遮蔽深度を作る手前のモデル
        // Occluded pass is checked with captured pixels.
        auto& blocker = scene.CreateGameObject("SkinnedOccluder");
        blocker.GetTransform().position = { -2.5f, 0.0f, 1.5f };
        blocker.GetTransform().scale = { 0.35f, 0.35f, 0.35f };
        blocker.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.18f, 0.2f, 0.24f, 1.0f });
        // 雛形shaderを割り当てるモデル
        auto& templateModel = addModel("TemplateModel", -2.5f);
        templateModel.SetMaterialOverrideEnabled(true);
        templateModel.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        templateModel.SetCustomParameter(0, { 1.0f, 0.5f, 0.2f, 0.6f });
        templateModel.SetCustomParameter(1, { 0.2f, 1.2f, 0.0f, 0.0f });
        templateModel.SetCustomParameter(2, { 1.0f, 1.0f, 0.0f, 0.0f });
        // まず追加passを無効にした画像を基準にし、その後だけ輪郭と遮蔽を有効にします。
        // 通常のskinned描画のAPI間一致と、D3D12の追加passが実画素を描くことを独立に検証できます。
        templateModel.SetCustomParameter(3, { 0.0f, 1.0f, 0.9f, 0.05f });
        templateModel.SetCustomParameter(4, { 0.05f, 0.85f, 1.0f, 0.0f });
        // Failed-shader model
        auto& brokenModel = addModel("BrokenModel", 2.5f);
        brokenModel.SetShaderPath(fixtures / "broken-shader.hlsl");

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        // 1フレームを描いて取得
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 最初のフレームでModelを読み込み、骨を動かした姿勢で描き直します。
        static_cast<void>(render());
        templateModel.SetAnimationTime(templateModel.AnimationDuration() * 0.5f);
        brokenModel.SetAnimationTime(brokenModel.AnimationDuration() * 0.5f);
        // baselineと追加passの比較画像
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

        // 背景色のRGB値
        const std::array<int, 3> background{
            capture.frame.pixels[0],
            capture.frame.pixels[1],
            capture.frame.pixels[2] };
        // Fallback pixel count
        std::size_t magentaPixels{};
        // Material shaderの画素数
        std::size_t shadedPixels{};
        // 輪郭passの画素数
        std::size_t outlinePixels{};
        // 遮蔽passの画素数
        std::size_t occludedPixels{};
        // 追加passの出力色を集計
        for (std::size_t offset{};
            offset + 3u < capture.frame.pixels.size();
            offset += 4u)
        {
            // 現在画素の赤成分
            const int red = capture.frame.pixels[offset];
            // 現在画素の緑成分
            const int green = capture.frame.pixels[offset + 1u];
            // 現在画素の青成分
            const int blue = capture.frame.pixels[offset + 2u];
            // マゼンタfallbackを集計
            if (red > 150 && blue > 150 && green < 80)
            {
                ++magentaPixels;
            }
            // 輪郭色の画素を集計
            if (red > blue + 40 && green > blue + 40)
            {
                ++outlinePixels;
            }
            // 遮蔽色の画素を集計
            if (green > red + 40 && blue > red + 40)
            {
                ++occludedPixels;
            }
            // 背景から変化した通常shader画素を集計
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // 同じMaterialのCubeがbatch描画と個別描画で一致するか検証します。
    // 未使用vectorの変更でbatchが分割される条件も確認します。
    // RenderMaterialShaderInstancingCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderMaterialShaderInstancingCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // instancingを描くデバイス
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
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        graphics.SetAsyncShaderCompilationEnabled(false);
        // D3D11雛形shaderのinstancing対応を確認
        if (api == LamaPon::RenderingApi::DirectX11)
        {
            // shader取得時の世代番号
            std::uint64_t generation{};
            // shader取得時の診断
            std::string error;
        // D3D11 material effect
            auto& effect = graphics.MaterialShader(
                "shaders/LamaPonCustomMaterial.hlsl",
                generation,
                error);
            Require(
                error.empty() && effect.SupportsInstancing(),
                "The DirectX 11 template material did not compile "
                "VSInstancedMain: " + error);
        }

        // instancing検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.2f, 6.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // addCube(name:名前,x:X)
        const auto addCube = [&](const char* const name, const float x)
            -> LamaPon::MeshRendererComponent&
        {
            // 生成するCube object
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.3f, 0.8f, 1.1f };
            object.GetTransform().SetEulerAngles(0.35f, 0.55f, 0.0f);
            // CubeのMeshRenderer
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.9f, 0.8f, 0.6f, 1.0f });
            mesh.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
            mesh.SetCustomParameter(0, { 0.3f, 0.8f, 1.0f, 0.55f });
            mesh.SetCustomParameter(1, { 0.15f, 0.0f, 0.0f, 0.0f });
            // 構築したmesh fixtureを返します。
            return mesh;
        };
        // 左側のinstance
        auto& left = addCube("InstancedLeft", -1.4f);
        // 右側のinstance
        auto& right = addCube("InstancedRight", 1.4f);

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        // Scene frame capture
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 初回描画でComponentとShader cacheを用意し、次のフレームでSceneのbatch収集へ入る状態にします。
        static_cast<void>(render());
        // instance batchの撮影画像
        const auto capture = render();

        // 現在のscene描画統計
        const auto& stats = scene.VisibilityStats();
        Require(
            stats.meshInstanceBatchCount == 1u
                && stats.meshInstancedRendererCount == 2u,
            "The material shader instance batch was not used on " + apiName
                + " (eligible " + std::to_string(left.CanBeInstanced())
                + "/" + std::to_string(right.CanBeInstanced())
                + ", keys " + std::to_string(left.InstanceBatchKey())
                + "/" + std::to_string(right.InstanceBatchKey()) + ")");
        // 両Cubeのshader診断
        const auto errors = left.ShaderError() + right.ShaderError();
        Require(
            errors.empty(),
            apiName + " reported a material shader instancing error: "
                + errors);

        // テンプレートが読まないcustom vectorだけを変えるとbatch keyが分かれ、見た目を変えずに1個ずつの描画になります。
        right.SetCustomVector(7, { 1.0f, 0.0f, 0.0f, 0.0f });
        // batch分割後の個別描画画像
        const auto individual = render();
        Require(
            scene.VisibilityStats().meshInstanceBatchCount == 0u,
            "The material shader instancing capture could not split the "
            "batch on " + apiName);
        // 画像差がある画素数
        std::size_t differentPixels{};
        // batch描画と個別描画の画像を比較
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size()
                 && offset + 3u < individual.pixels.size();
             offset += 4u)
        {
            // RGB各成分の差を確認
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                // 許容差を超えた成分を記録
                if (std::abs(
                        static_cast<int>(capture.pixels[offset + channel])
                        - static_cast<int>(
                            individual.pixels[offset + channel])) > 2)
                {
                    ++differentPixels;
                    // このpost-process分岐の検証を終えます。
                    break;
                }
            }
        }
        Require(
            differentPixels == 0u,
            apiName + " drew the material shader instance batch differently "
                "from individual draws in "
                + std::to_string(differentPixels) + " pixels");
        // 撮影したframe captureを返します。
        return capture;
    }

    // reversedに応じた面向きで筒の側面を生成します。
    // BuildProceduralTube(vertices: 頂点出力, indices: 索引出力, reversed: 面を反転するか)
    void BuildProceduralTube(
        std::vector<LamaPon::ProceduralMeshVertex>& vertices,
        std::vector<std::uint32_t>& indices,
        const bool reversed)
    {
        // 円筒の分割数
        constexpr std::uint32_t Segments = 16u;
        // 円筒の半径
        constexpr float Radius = 0.45f;
        // 円筒の半高
        constexpr float HalfHeight = 0.55f;
        // 円筒側面の上下頂点を生成
        for (std::uint32_t segment{}; segment <= Segments; ++segment)
        {
            // 円周上の補間率
            const float u = static_cast<float>(segment) / Segments;
            // 円周上の角度
            const float angle = u * DirectX::XM_2PI;
            // 現在位置の法線
            const DirectX::XMFLOAT3 normal{
                std::sin(angle), 0.0f, std::cos(angle) };
            vertices.push_back({
                { normal.x * Radius, -HalfHeight, normal.z * Radius },
                normal,
                { u, 1.0f } });
            vertices.push_back({
                { normal.x * Radius, HalfHeight, normal.z * Radius },
                normal,
                { u, 0.0f } });
        }
        // 隣り合う頂点組から側面を生成
        for (std::uint32_t segment{}; segment < Segments; ++segment)
        {
            // 現在区間の下頂点番号
            const std::uint32_t bottom = segment * 2u;
            // 現在区間の上頂点番号
            const std::uint32_t top = bottom + 1u;
            // 次区間の下頂点番号
            const std::uint32_t nextBottom = bottom + 2u;
            // 次区間の上頂点番号
            const std::uint32_t nextTop = bottom + 3u;
            // 指定された向きへ面を反転
            if (reversed)
            {
                indices.insert(
                    indices.end(),
                    { bottom, nextTop, top, bottom, nextBottom, nextTop });
            }
            // 通常の外向き面を追加
            else
            {
                indices.insert(
                    indices.end(),
                    { bottom, top, nextTop, bottom, nextTop, nextBottom });
            }
        }
    }

    // scene compositionを描画してback bufferを返します。
    // CaptureSceneFrame(graphics: 描画デバイス, scene: 描画Scene, clearColor: 背景RGBA)
    [[nodiscard]] Capture CaptureSceneFrame(
        LamaPon::GraphicsDevice& graphics,
        LamaPon::Scene& scene,
        const float clearColor[4])
    {
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        // scene composition描画先
        auto* const target = graphics.SceneCompositionTarget();
        Require(
            target != nullptr,
            "The capture has no scene composition target");
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            target);
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        // 描画後のscene画像
        Capture frame;
        frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
        graphics.EndFrame();
        // 構築した描画frameを返します。
        return frame;
    }

    // 面向きとCullModeごとの筒の描画を検証します。
    // 影パスにも同じカリング設定を適用します。
    // RenderMeshCullCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderMeshCullCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // cull検証用の描画デバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The mesh cull capture did not start the requested rendering API");
        // D3D11のLit / Environment shaderはasset rootから読み込みます。
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // cull検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.9f, 6.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.12f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.15f);

        // 光は自分の-Z軸の向きへ進むので、上から手前へ差し込むよう傾けます。
        // Sun light object
        auto& sunObject = scene.CreateGameObject("Sun");
        sunObject.GetTransform().SetEulerAngles(-0.9f, 0.5f, 0.0f);
        // 影を作る太陽光
        auto& sun = sunObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        sun.SetCastsShadows(true);

        // 開いた口から見える内側も照らすよう、カメラ側に置きます。
        // カメラ側から筒内を照らす点光源
        auto& pointObject = scene.CreateGameObject("PointLight");
        pointObject.GetTransform().position = { 0.0f, 1.5f, 4.0f };
        // 点光源コンポーネント
        auto& pointLight = pointObject.AddComponent<
            LamaPon::PointLightComponent>(
                DirectX::XMFLOAT3{ 1.0f, 0.95f, 0.85f },
                5.0f,
                14.0f);
        pointLight.SetCastsShadows(false);

        // 影を受ける床は両面で描きます。
        auto& floorObject = scene.CreateGameObject("Floor");
        // 床のメッシュ描画部品
        auto& floor = floorObject.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.7f, 0.7f, 0.72f, 1.0f });
        floor.SetProceduralMesh(
            {
                { { -4.5f, -0.9f, 2.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
                { { -4.5f, -0.9f, -2.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
                { { 4.5f, -0.9f, -2.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
                { { 4.5f, -0.9f, 2.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            },
            { 0u, 1u, 2u, 0u, 2u, 3u });
        floor.SetCullMode(LamaPon::ShaderCullMode::None);

        // 通常向き筒の頂点列
        std::vector<LamaPon::ProceduralMeshVertex> tubeVertices;
        // 通常向き筒の索引列
        std::vector<std::uint32_t> tubeIndices;
        BuildProceduralTube(tubeVertices, tubeIndices, false);
        // 反転向き筒の頂点列
        std::vector<LamaPon::ProceduralMeshVertex> reversedVertices;
        // 反転向き筒の索引列
        std::vector<std::uint32_t> reversedIndices;
        BuildProceduralTube(reversedVertices, reversedIndices, true);
        // addTube(name:名,x:X,color:色,reversed:反転)
        const auto addTube = [&](
            const char* const name,
            const float x,
            const DirectX::XMFLOAT4& color,
            const bool reversed) -> LamaPon::MeshRendererComponent&
        {
            // 生成する筒object
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().SetEulerAngles(0.7f, 0.3f, 0.0f);
            // 筒のメッシュ描画部品
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                color);
            // 面向きに応じた頂点列を設定
            if (reversed)
            {
                mesh.SetProceduralMesh(reversedVertices, reversedIndices);
            }
            // 通常向きの頂点列を設定
            else
            {
                mesh.SetProceduralMesh(tubeVertices, tubeIndices);
            }
            // 構築したmesh fixtureを返します。
            return mesh;
        };
        static_cast<void>(addTube(
            "OutsideTube", -2.7f, { 0.9f, 0.45f, 0.3f, 1.0f }, false));
        static_cast<void>(addTube(
            "InsideTube", -0.9f, { 0.3f, 0.8f, 0.45f, 1.0f }, true));
        addTube("FrontCulledTube", 0.9f, { 0.35f, 0.5f, 0.95f, 1.0f }, false)
            .SetCullMode(LamaPon::ShaderCullMode::Front);
        addTube("TwoSidedTube", 2.7f, { 0.9f, 0.85f, 0.35f, 1.0f }, false)
            .SetCullMode(LamaPon::ShaderCullMode::None);

        // 描画フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        static_cast<void>(CaptureSceneFrame(graphics, scene, clearColor));
        // 事前描画後の比較画像
        const auto capture = CaptureSceneFrame(graphics, scene, clearColor);
        // 光が当たった画素数
        std::size_t litPixels{};
        // 筒と床の可視画素を集計
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            // 背景より明るい画素を数える
            if (std::max({
                    capture.pixels[offset],
                    capture.pixels[offset + 1u],
                    capture.pixels[offset + 2u] }) > 24u)
            {
                ++litPixels;
            }
        }
        Require(
            litPixels > 2000u,
            apiName + " did not draw the culled tubes ("
                + std::to_string(litPixels) + " lit pixels)");
        // 撮影したframe captureを返します。
        return capture;
    }

    // 組み込み形状（Cube、Sphere、Cylinder、表と裏から見たPlane）の既定のカリングで、外側の面が残ることを比べます。
    // 両APIの形状を列の面積と輝度で比較します。
    // APIごとに異なるmeshを使うため画素単位では比較しません。
    // Shape cell count.
    constexpr std::uint32_t BuiltInShapeCellCount = 5u;

    // 組み込み形状の既定カリングで可視面を検証します。
    // RenderBuiltInShapeCullCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderBuiltInShapeCullCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 組み込み形状を描くデバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The built-in shape cull capture did not start the requested "
            "rendering API");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);

        // 形状検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.2f);
        // 形状を照らす太陽光
        auto& sunObject = scene.CreateGameObject("Sun");
        sunObject.GetTransform().SetEulerAngles(-0.5f, 0.3f, 0.0f);
        // 太陽光部品
        auto& sun = sunObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        sun.SetCastsShadows(false);
        // 形状を補助する点光源object
        auto& pointObject = scene.CreateGameObject("PointLight");
        pointObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        // 点光源部品
        auto& pointLight = pointObject.AddComponent<
            LamaPon::PointLightComponent>(
                DirectX::XMFLOAT3{ 1.0f, 1.0f, 1.0f },
                4.0f,
                12.0f);
        pointLight.SetCastsShadows(false);

        // addShape(name:名,shape:形,x:X,pitch:傾,color:色)
        const auto addShape = [&](
            const char* const name,
            const LamaPon::PrimitiveShape shape,
            const float x,
            const float pitch,
            const DirectX::XMFLOAT4& color)
        {
            // 生成する形状object
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 0.9f, 0.9f, 0.9f };
            object.GetTransform().SetEulerAngles(pitch, 0.4f, 0.0f);
            static_cast<void>(
                object.AddComponent<LamaPon::MeshRendererComponent>(
                    shape,
                    color));
        };
        addShape("Cube", LamaPon::PrimitiveShape::Cube,
            -2.8f, 0.5f, { 0.9f, 0.5f, 0.35f, 1.0f });
        addShape("Sphere", LamaPon::PrimitiveShape::Sphere,
            -1.4f, 0.0f, { 0.4f, 0.8f, 0.5f, 1.0f });
        addShape("Cylinder", LamaPon::PrimitiveShape::Cylinder,
            0.0f, 0.5f, { 0.45f, 0.55f, 0.95f, 1.0f });
        addShape("PlaneTop", LamaPon::PrimitiveShape::Plane,
            1.4f, 1.1f, { 0.95f, 0.85f, 0.4f, 1.0f });
        addShape("PlaneBottom", LamaPon::PrimitiveShape::Plane,
            2.8f, -1.1f, { 0.85f, 0.45f, 0.85f, 1.0f });

        // 描画フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        static_cast<void>(CaptureSceneFrame(graphics, scene, clearColor));
        // scene描画結果をcaptureして返します。
        return CaptureSceneFrame(graphics, scene, clearColor);
    }

    struct ShapeCell final
    {
        // 可視画素数
        std::size_t coverage{};
        // 平均輝度
        double brightness{};
    };

    // MeasureShapeCells(capture: 形状capture画像)
    // 画像を5列に分けて面積と明るさを測ります。
    [[nodiscard]] std::array<ShapeCell, BuiltInShapeCellCount>
        MeasureShapeCells(const Capture& capture)
    {
        // 列ごとの集計結果
        std::array<ShapeCell, BuiltInShapeCellCount> cells{};
        // 1列あたりの画素幅
        const std::uint32_t cellWidth = capture.width / BuiltInShapeCellCount;
        // 画像の行を順に走査
        for (std::uint32_t y{}; y < capture.height; ++y)
        {
            // 5列分の画素を走査
            for (std::uint32_t x{}; x < cellWidth * BuiltInShapeCellCount; ++x)
            {
                // 現在画素のRGBA位置
                const auto offset =
                    (static_cast<std::size_t>(y) * capture.width + x) * 4u;
                // 現在画素の最大輝度
                const int brightness = std::max({
                    capture.pixels[offset],
                    capture.pixels[offset + 1u],
                    capture.pixels[offset + 2u] });
                // 背景に近い画素を除外
                if (brightness <= 12)
                {
                    // 次の画素へ
                    continue;
                }
                // 現在画素が属する列
                auto& cell = cells[x / cellWidth];
                ++cell.coverage;
                cell.brightness += brightness;
            }
        }
        // 列ごとの輝度を平均化
        for (auto& cell : cells)
        {
            // 有効画素のある列だけを平均化
            if (cell.coverage != 0u)
            {
                cell.brightness /= static_cast<double>(cell.coverage);
            }
        }
        // scene内のcell一覧を返します。
        return cells;
    }

    // D3D11/12の形状ごとの面積と輝度を比較します。
    // RequireSimilarShapeCells(d3d11: 基準画像, d3d12: 比較画像)
    void RequireSimilarShapeCells(
        const Capture& d3d11,
        const Capture& d3d12)
    {
        // 各backendの5列測定結果
        static constexpr std::array<const char*, BuiltInShapeCellCount>
            names{ "Cube", "Sphere", "Cylinder", "Plane (top)",
                "Plane (bottom)" };
        // D3D11の列測定値
        const auto expected = MeasureShapeCells(d3d11);
        // D3D12の列測定値
        const auto actual = MeasureShapeCells(d3d12);
        // 各形状列の測定値を比較
        for (std::size_t cell{}; cell < BuiltInShapeCellCount; ++cell)
        {
            // D3D11の基準値
            const auto& reference = expected[cell];
            // D3D12の測定値
            const auto& measured = actual[cell];
            // D3D11に対する画素面積比
            const double coverageRatio = reference.coverage == 0u
                ? 0.0
                : static_cast<double>(measured.coverage) / reference.coverage;
            // 面積・明るさが許容範囲内か
            const bool similar = reference.coverage > 80u
                && coverageRatio > 0.8
                && coverageRatio < 1.25
                && std::abs(measured.brightness - reference.brightness)
                    <= std::max(14.0, reference.brightness * 0.2);
            Require(
                similar,
                std::string{ "DirectX 12 built-in " } + names[cell]
                    + " did not keep the same visible faces as DirectX 11 ("
                    + std::to_string(measured.coverage) + " px at "
                    + std::to_string(static_cast<int>(measured.brightness))
                    + " vs " + std::to_string(reference.coverage) + " px at "
                    + std::to_string(static_cast<int>(reference.brightness))
                    + ")");
        }
    }

    // 組み込みLitのinstancing結果をD3D11と比較します。
    // mesh/model双方のbatch数と画像出力を確認します。
    // RenderBuiltInInstancingCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderBuiltInInstancingCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // アニメーションもskinも無い、色付きの箱です（bufferはbase64で内蔵）。
        // Static model fixture
        const auto staticModelPath =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/static-box.gltf";
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 組み込みLitを描くデバイス
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
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // instancing検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.3f, 6.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.25f);
        // 法線の違いが明暗に出るよう、影の無い斜めの光を当てます。
        // Sun light object
        auto& sunObject = scene.CreateGameObject("Sun");
        sunObject.GetTransform().SetEulerAngles(-0.6f, 0.35f, 0.0f);
        // シーンを照らす太陽光
        auto& sun = sunObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        sun.SetCastsShadows(false);

        // addCube(name:名,position:位置,scale:倍率,color:色)
        const auto addCube = [&](
            const char* const name,
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& scale,
            const DirectX::XMFLOAT4& color)
        {
            // 生成するCube object
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
        // addModel(name:名,position:位置,scale:倍率)
        const auto addModel = [&](
            const char* const name,
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& scale)
        {
            // 生成するmodel object
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

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        // sceneを描画して画像を返す
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 最初のフレームでComponentとModelを用意し、次のフレームでSceneのbatch収集へ入れます。
        static_cast<void>(render());
        // batch描画の撮影画像
        const auto capture = render();

        // 現在の描画batch統計
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

        // 画面背景のRGB値
        const std::array<int, 3> background{
            capture.pixels[0],
            capture.pixels[1],
            capture.pixels[2] };
        // 左側mesh画素数
        std::size_t leftPixels{};
        // 右側model画素数
        std::size_t rightPixels{};
        // 描画結果を左右の領域に分類
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            // 背景色との差の最大値
            int difference{};
            // RGB各成分の背景差を確認
            for (std::size_t channel{}; channel < 3u; ++channel)
            {
                difference = std::max(
                    difference,
                    std::abs(
                        static_cast<int>(capture.pixels[offset + channel])
                        - background[channel]));
            }
            // 背景に近い画素を除外
            if (difference <= 12)
            {
                // 次の画素へ
                continue;
            }
            // 左右の描画領域を分ける
            if ((offset / 4u) % capture.width < capture.width / 2u)
            {
                ++leftPixels;
            }
            // 右側のmodel領域を集計
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // ParticleSystemのcustom shader結果をD3D11と比較します。
    // 既定・probe・compile失敗時の描画を確認します。
    // RenderCustomParticleShaderCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderCustomParticleShaderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Particle test device
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
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // particle検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        // Particle fixtures
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        // addParticles(name:名,x:X)
        const auto addParticles = [&](
            const char* const name,
            const float x) -> LamaPon::ParticleSystemComponent&
        {
            // 生成するparticle object
            auto& object = scene.CreateGameObject(name);
            // objectのparticle部品
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
            // 構築したparticle fixtureを返します。
            return particles;
        };
        static_cast<void>(addParticles("DefaultParticles", -1.4f));
        // Custom probe particle
        auto& probe = addParticles("ProbeParticles", 0.0f);
        probe.SetShaderPath(fixtures / "particle-probe.hlsl");
        probe.SetAuxiliaryTexturePath("textures/LamaPonLogo.png");
        probe.SetCustomParameter(0, { 0.35f, 1.0f, 0.6f, 0.0f });
        // Failed-shader particle
        auto& broken = addParticles("BrokenParticles", 1.4f);
        broken.SetShaderPath(fixtures / "broken-shader.hlsl");

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.05f, 0.05f, 0.08f, 1.0f };
        // sceneを描画して画像を返す
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 最初のフレームでtextureとShaderを読み込みます。
        static_cast<void>(render());
        // particle shaderの撮影画像
        const auto capture = render();
        // probe shaderの診断
        const std::string probeError(probe.ShaderError());
        // compile失敗shaderの診断
        const std::string brokenError(broken.ShaderError());

        // magenta fallbackの画素数
        std::size_t magentaPixels{};
        // 撮影画像のfallback色を集計
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            // magenta色のpixelを判定
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // DDS array/volume/cube arrayのsample結果を検証します。
    // probeはt7〜t9から指定sliceとfaceを読みます。
    // RenderDdsDimensionCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderDdsDimensionCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // DDS dimensionを描くデバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The DDS dimension capture did not start the requested rendering "
            "API");
        graphics.Assets().SetAssetRoot(LAMAPON_TEST_ASSET_DIR);
        // D3D11の非同期compileは最初のフレームを標準Litで描くため止めます。
        graphics.SetAsyncShaderCompilationEnabled(false);
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // DDS dimension検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 2.2f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        // DDS probe fixtures
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        // カメラへ向いた横長の四角形です。
        // 三角形の並びを両APIで揃えるためProcedural Meshにします。
        // probeを描く四角形の頂点列
        std::vector<LamaPon::ProceduralMeshVertex> vertices{
            { { -1.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
            { { -1.5f, 0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            { { 1.5f, 0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
            { { 1.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } } };
        // 四角形の三角形索引
        std::vector<std::uint32_t> indices{ 0u, 1u, 2u, 0u, 2u, 3u };
        // DDS probe用のobject
        auto& object = scene.CreateGameObject("DdsDimensions");
        // probe用の平面描画部品
        auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Plane,
            DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f });
        mesh.SetProceduralMesh(std::move(vertices), std::move(indices));
        mesh.SetShaderPath(fixtures / "dds-dimension-probe.hlsl");
        mesh.SetCustomTexturePath(0, fixtures / "array-probe.dds");
        mesh.SetCustomTexturePath(1, fixtures / "volume-probe.dds");
        mesh.SetCustomTexturePath(2, fixtures / "cube-array-probe.dds");

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.05f, 0.05f, 0.08f, 1.0f };
        // sceneを描画して画像を返す
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 最初のフレームでtextureとShaderを読み込みます。
        static_cast<void>(render());
        // DDS probeの撮影画像
        const auto capture = render();
        // probe shaderの診断
        const std::string shaderError(mesh.ShaderError());

        // pixel(x: 横位置)
        const auto pixel = [&](const std::uint32_t x)
        {
            // 現在位置のRGBA先頭
            const auto offset =
                (static_cast<std::size_t>(CanvasHeight / 2u) * CanvasWidth
                    + x) * 4u;
            // pixelのRGB channel値を返します。
            return std::array<int, 3>{
                capture.pixels[offset],
                capture.pixels[offset + 1u],
                capture.pixels[offset + 2u] };
        };
        // 2D arrayの2枚目の色
        const auto slice = pixel(CanvasWidth * 5u / 16u);
        // volume textureの奥側の色
        const auto volume = pixel(CanvasWidth / 2u);
        // cube arrayの2個目の色
        const auto cube = pixel(CanvasWidth * 11u / 16u);
        // describe(color:RGB)
        const auto describe = [](const std::array<int, 3>& color)
        {
            // pixel値を比較診断用文字列にします。
            return std::to_string(color[0]) + ","
                + std::to_string(color[1]) + ","
                + std::to_string(color[2]);
        };
        Require(
            shaderError.empty()
                && slice[1] > slice[0] + 60
                && slice[1] > slice[2] + 60
                && volume[2] > volume[0] + 60
                && volume[2] > volume[1] + 60
                && cube[0] > 150
                && cube[1] > 140
                && cube[2] < 100,
            apiName + " did not sample the DDS array, volume, and cube array "
                "textures (" + describe(slice) + " / " + describe(volume)
                + " / " + describe(cube) + "; error: [" + shaderError + "])");
        // 撮影したframe captureを返します。
        return capture;
    }

    // SSR履歴と反射の有効・無効を検証します。
    // 初回履歴は未確定、無効化後は基準画像へ戻ります。
    void RequireD3D12ScreenSpaceReflection()
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // SSR検証用の描画デバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // SSR検証シーン
        LamaPon::Scene scene(graphics);
        BuildScreenSpaceReflectionScene(scene);

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // scene compositionを撮影
        const auto captureComposition = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 撮影画像の幅
            std::uint32_t width{};
            // 撮影画像の高さ
            std::uint32_t height{};
            // 取得した合成画像
            auto capture = graphics.CaptureBackBuffer(width, height);
            graphics.EndFrame();
            Require(
                width == CanvasWidth && height == CanvasHeight,
                "The DirectX 12 SSR capture has unexpected dimensions");
            // 撮影したframe captureを返します。
            return capture;
        };
        // setReflectionEnabled(enabled:SSR有効)
        const auto setReflectionEnabled = [&](const bool enabled)
        {
            // 変更対象のSSR設定
            auto reflection = scene.ScreenSpaceReflection();
            reflection.enabled = enabled;
            scene.SetScreenSpaceReflectionSettings(reflection);
        };

        setReflectionEnabled(false);
        // SSRを無効にした基準画像
        const auto withoutReflection = captureComposition();
        setReflectionEnabled(true);
        Require(
            captureComposition() == withoutReflection,
            "The first DirectX 12 SSR frame read a missing color history");
        // SSRを有効にした反射画像
        const auto withReflection = captureComposition();
        // Lightingへ解決されたSSR状態
        const auto& reflection = graphics.Lighting().screenSpaceReflection;
        Require(
            reflection.enabled
                && graphics.IsGraphicsViewCurrent(reflection.texture)
                && graphics.IsGraphicsViewCurrent(reflection.depth)
                && reflection.depthPyramidMaximumMip == 8u,
            "The DirectX 12 scene did not apply SSR with a full Hi-Z "
            "pyramid");

        // 赤い反射が現れた画素数
        std::size_t reflectedPixels{};
        // 基準画像と反射画像を比較
        for (std::size_t offset{};
             offset + 3u < withReflection.size();
             offset += 4u)
        {
            // 床に赤色が増えた画素を数える
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

    // Sky gradientと太陽を指定APIで撮影します。
    // 手前のCubeでskyが深度を変えないことも確認します。
    // RenderSkyCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderSkyCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // skyを描く描画デバイス
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
        // sky検証シーン
        LamaPon::Scene scene(graphics);

        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 4.0f };
        // 正のピッチで少し上を向き、下端に地面、上端に天頂が入ります。
        cameraObject.GetTransform().SetEulerAngles(0.25f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);

        // Directional Lightは自分の-Z軸の向きへ進みます。
        // 180度振り返らせて少し下へ傾け、カメラの正面やや上に太陽が見えるようにします。
        // skyの太陽光
        auto& sunObject = scene.CreateGameObject("Sun");
        sunObject.GetTransform().SetEulerAngles(
            -0.3f,
            DirectX::XM_PI,
            0.0f);
        // 太陽光コンポーネント
        auto& sun = sunObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        sun.SetCastsShadows(false);
        // トーンマップ後も円盤がほぼ白く残り、数十画素を占めるように実際の太陽より明るく大きくします。
        sun.SetIntensity(4.0f);
        sun.SetAngularDiameterDegrees(8.0f);

        // 空の描画設定
        LamaPon::SkySettings sky;
        sky.enabled = true;
        sky.sunDriven = true;
        scene.SetSkySettings(sky);

        // 手前Cubeの頂点列
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 手前Cubeの索引列
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // sky前景のCube object
        auto& cube = scene.CreateGameObject("Cube");
        cube.GetTransform().position = { -1.2f, 0.3f, 0.0f };
        // 前景Cubeのmesh描画部品
        auto& cubeMesh = cube.AddComponent<LamaPon::MeshRendererComponent>(
            LamaPon::PrimitiveShape::Cube,
            DirectX::XMFLOAT4{ 0.9f, 0.15f, 0.1f, 1.0f });
        cubeMesh.SetProceduralMesh(cubeVertices, cubeIndices);

        // Composition clear
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        // composition描画先
        auto* const target = graphics.SceneCompositionTarget();
        Require(
            target != nullptr,
            "The sky capture has no scene composition target");
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            target);
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        // skyと前景を撮影した画像
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        // 空は画面全体を塗るためclearの黒は残らず、太陽円盤とにじみはほぼ白い画素になります。
        // 両APIとも空を描かずに一致する誤検出を防ぎます。
        // clear色の画素数
        std::size_t clearPixels{};
        // 太陽円盤の画素数
        std::size_t sunPixels{};
        // sky画像の黒と白の画素を集計
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            // 現在画素の赤成分
            const auto red = capture.pixels[offset];
            // 現在画素の緑成分
            const auto green = capture.pixels[offset + 1u];
            // 現在画素の青成分
            const auto blue = capture.pixels[offset + 2u];
            // clear色の黒画素を集計
            if (red == 0u && green == 0u && blue == 0u)
            {
                ++clearPixels;
            }
            // 白に近い太陽画素を集計
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // DDS cubeの面分割と不正入力の拒否を検証します。
    // RequireDdsCubeParser(cubeBytes: cube DDSデータ)
    void RequireDdsCubeParser(const std::vector<std::uint8_t>& cubeBytes)
    {
        // 6面へ分割したcube解析結果
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

        // rejects(prepare:解析)
        const auto rejects = [](const auto& prepare)
        {
            // 不正cubeが拒否されることを確認
            try
            {
                static_cast<void>(prepare());
            }
            // 想定した形式エラーを受け取る
            catch (const std::invalid_argument&)
            {
                // 指定pixel条件の判定結果を返します。
                return true;
            }
            // 指定pixel条件の判定結果を返します。
            return false;
        };
        // Incomplete cube
        auto partialCube = cubeBytes;
        WriteLittleEndian32(partialCube, 112u, 0x0600u);
        Require(
            rejects([&cubeBytes]
            {
                // DDS fixtureをtexture loader形式へ変換します。
                return LamaPon::TextureLoader::PrepareDdsTextureData(
                    cubeBytes);
            })
                && rejects([]
                {
                    // DDS fixtureをtexture loader形式へ変換します。
                    return LamaPon::TextureLoader::PrepareDdsCubeTextureData(
                        BuildRgbaDds());
                })
                && rejects([&partialCube]
                {
                    // DDS fixtureをtexture loader形式へ変換します。
                    return LamaPon::TextureLoader::PrepareDdsCubeTextureData(
                        partialCube);
                }),
            "The DDS parser accepted a cube as 2D, a 2D texture as a cube, "
            "or a cube with missing faces");
    }

    // DDS cubeをSkyとしてsampleできることを検証します。
    // gradient skyとの画像差とprefilter結果も確認します。
    // RenderCubemapSkyCapture(api: 描画API, profile: 起動設定, cubemapBytes: cube DDSデータ)
    [[nodiscard]] Capture RenderCubemapSkyCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile,
        const std::span<const std::uint8_t> cubemapBytes)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // cubemap skyを描くデバイス
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
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // DDS cube view
        const auto cubemapView =
            graphics.Assets().CreateTextureViewHandleFromMemory(
                cubemapBytes,
                true);
        Require(
            graphics.IsSampleableCubeView(cubemapView),
            apiName + " did not load the DDS cube as a sampleable "
                "TextureCube");

        // cubemap skyの描画設定
        LamaPon::SkySettings sky;
        sky.enabled = true;
        sky.intensity = 1.5f;
        sky.iblIntensity = 0.0f;

        // sky captureの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 複数面を見るカメラ方向
        const auto view = DirectX::XMMatrixLookAtLH(
            DirectX::XMVectorZero(),
            DirectX::XMVectorSet(0.65f, 0.34f, -0.68f, 0.0f),
            DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        // sky captureの投影行列
        const auto projection = DirectX::XMMatrixPerspectiveFovLH(
            DirectX::XMConvertToRadians(60.0f),
            static_cast<float>(CanvasWidth) / CanvasHeight,
            0.1f,
            100.0f);
        // captureFrame(texture:sky)
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
            // skyの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // DDS cubeを使ったsky画像
        const auto cubemapFrame = captureFrame(cubemapView);
        // gradient skyの比較画像
        const auto gradientFrame = captureFrame({});

        graphics.BeginFrame(clearColor);
        // Prefiltered views
        const auto prefiltered =
            graphics.TryGetPrefilteredEnvironmentViews(cubemapView, 1u);
        graphics.EndFrame();
        Require(
            prefiltered.IsValid()
                && graphics.IsSampleableCubeView(prefiltered.specular)
                && graphics.IsSampleableCubeView(prefiltered.irradiance)
                && prefiltered.specularMaximumMip == 7.0f,
            apiName + " did not prefilter the in-memory cubemap");

        // gradientと色が異なる画素数
        std::size_t changedPixels{};
        // cubemapとgradient画像を画素比較
        for (std::size_t offset{};
             offset + 3u < cubemapFrame.pixels.size()
                 && offset + 3u < gradientFrame.pixels.size();
             offset += 4u)
        {
            // RGB値が違う画素を数える
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
        // cubemap frame captureを返します。
        return cubemapFrame;
    }

    // CountChangedPixels(first: 基準画像, second: 比較画像)
    // 2枚の合成画像でRGB差がある画素数を返します。
    [[nodiscard]] std::size_t CountChangedPixels(
        const Capture& first,
        const Capture& second)
    {
        // RGB差がある画素数
        std::size_t changedPixels{};
        // 両画像に含まれる画素を比較
        for (std::size_t offset{};
             offset + 3u < first.pixels.size()
                 && offset + 3u < second.pixels.size();
             offset += 4u)
        {
            // RGB値が違う画素を数える
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
        // 変更pixel数を返します。
        return changedPixels;
    }

    // 距離別のLit/DirectXTK霧を指定APIで撮影します。
    // 強い霧は色への収束だけを確認します。
    // RenderFogCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderFogCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // fogを描くデバイス
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
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // fog検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.6f, 6.0f };
        cameraObject.GetTransform().SetEulerAngles(-0.08f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.8f);

        // 距離別Cubeの頂点列
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 距離別Cubeの索引列
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // 配置順のCube名
        constexpr std::array<const char*, 4> cubeNames{
            "NearCube", "MiddleCube", "FarCube", "FarthestCube" };
        // 近景から遠景へのCube配置
        constexpr std::array<DirectX::XMFLOAT3, 4> cubePositions{ {
            { -2.0f, 0.8f, 2.5f },
            { -0.8f, 0.8f, -1.0f },
            { 0.8f, 0.8f, -5.0f },
            { 2.8f, 0.8f, -10.0f } } };
        // 距離別Cubeを順に配置
        for (std::size_t index{}; index < cubeNames.size(); ++index)
        {
            // 生成するCube object
            auto& object = scene.CreateGameObject(cubeNames[index]);
            object.GetTransform().position = cubePositions[index];
            object.GetTransform().SetEulerAngles(0.4f, 0.6f, 0.0f);
            // Cubeのメッシュ描画部品
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.9f, 0.2f, 0.15f, 1.0f });
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
        }

        // 霧を比較するCMO object
        auto& modelObject = scene.CreateGameObject("CmoModel");
        modelObject.GetTransform().position = { 0.0f, -0.9f, 1.0f };
        modelObject.GetTransform().scale = { 1.2f, 1.2f, 1.2f };
        modelObject.GetTransform().SetEulerAngles(0.45f, 0.65f, 0.0f);
        // CMOのmodel描画部品
        auto& model = modelObject.AddComponent<
            LamaPon::ModelRendererComponent>(
                std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
                    / "models"
                    / "arrow.cmo");
        // Material上書き中はD3D11もDirectXTK EffectではなくLamaPon Litで描くため、D3D12と同じ霧の式で比べられます。
        model.SetMaterialOverrideEnabled(true);
        model.SetEmissiveColor({ 0.2f, 0.6f, 0.3f });

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // fog適用後のscene画像を撮影
        const auto captureFrame = [&graphics, &scene, &clearColor]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            // scene composition描画先
            auto* const target = graphics.SceneCompositionTarget();
            Require(
                target != nullptr,
                "The fog capture has no scene composition target");
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                target);
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };

        // 検証するfog設定
        LamaPon::FogSettings fog;
        fog.enabled = true;
        fog.color = { 0.55f, 0.65f, 0.8f };
        fog.startDistance = 4.0f;
        fog.endDistance = 18.0f;
        fog.density = 0.04f;
        scene.SetFogSettings(fog);
        // LitEffectへ適用した霧画像
        const auto litFogFrame = captureFrame();
        fog.enabled = false;
        scene.SetFogSettings(fog);
        // LitEffectの霧なし基準画像
        const auto litClearFrame = captureFrame();

        model.SetMaterialOverrideEnabled(false);
        fog.enabled = true;
        fog.startDistance = 0.0f;
        fog.endDistance = 0.01f;
        fog.density = 0.0f;
        scene.SetFogSettings(fog);
        // 強いfogを適用した画像
        const auto fullFogFrame = captureFrame();
        fog.enabled = false;
        scene.SetFogSettings(fog);
        // 強いfogを無効にした画像
        const auto unfoggedFrame = captureFrame();

        // LitEffectで変化した画素数
        const auto litFogPixels =
            CountChangedPixels(litFogFrame, litClearFrame);
        // 強いfogで変化した画素数
        const auto fullFogPixels =
            CountChangedPixels(fullFogFrame, unfoggedFrame);
        Require(
            litFogPixels > 400u && fullFogPixels > 800u,
            apiName + " did not apply the scene fog ("
                + std::to_string(litFogPixels) + " lit fog pixels, "
                + std::to_string(fullFogPixels) + " full fog pixels)");
        // fog適用後のlighting frameを返します。
        return litFogFrame;
    }

    // 3種の直射光によるLit画像を指定APIで撮影します。
    // 金属・法線map・非一様scaleのCubeを含めます。
    // RenderDirectLightingCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderDirectLightingCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 直射光を描くデバイス
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
        // 描画APIがDirectX 11か
        const bool directX11 = api == LamaPon::RenderingApi::DirectX11;
        // 診断表示用のAPI名
        const std::string apiName = directX11 ? "DirectX 11" : "DirectX 12";

        // 直射光検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.3f, 5.0f };
        cameraObject.GetTransform().SetEulerAngles(-0.05f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.05f);

        // 光は自分の-Z軸の向きへ進むので、上から手前へ差し込むよう傾けます。
        auto& sunObject = scene.CreateGameObject("Sun");
        sunObject.GetTransform().SetEulerAngles(-0.6f, 0.35f, 0.0f);
        // 影なしの太陽光
        auto& sun = sunObject.AddComponent<
            LamaPon::DirectionalLightComponent>();
        sun.SetCastsShadows(false);
        sun.SetAngularDiameterDegrees(3.0f);

        // 近距離の点光源object
        auto& pointObject = scene.CreateGameObject("PointLight");
        pointObject.GetTransform().position = { -1.2f, 0.9f, 1.6f };
        // 暖色の点光源
        auto& pointLight = pointObject.AddComponent<
            LamaPon::PointLightComponent>(
                DirectX::XMFLOAT3{ 1.0f, 0.7f, 0.4f },
                4.0f,
                4.0f);
        pointLight.SetCastsShadows(false);

        // 青色のspot light object
        auto& spotObject = scene.CreateGameObject("SpotLight");
        spotObject.GetTransform().position = { 1.4f, 0.4f, 2.2f };
        // 青色のspot light
        auto& spotLight = spotObject.AddComponent<
            LamaPon::SpotLightComponent>(
                DirectX::XMFLOAT3{ 0.4f, 0.7f, 1.0f },
                6.0f,
                6.0f,
                DirectX::XMConvertToRadians(18.0f),
                DirectX::XMConvertToRadians(28.0f));
        spotLight.SetCastsShadows(false);

        // 3材質で共有するCube頂点列
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 共有Cube索引列
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // addCube(name:名,x:X,color:色,metallic:金属,roughness:粗さ)
        const auto addCube = [&](
            const char* const name,
            const float x,
            const DirectX::XMFLOAT4& color,
            const float metallic,
            const float roughness) -> LamaPon::MeshRendererComponent&
        {
            // 生成するCube object
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 0.9f, 0.9f, 0.9f };
            object.GetTransform().SetEulerAngles(0.5f, 0.7f, 0.0f);
            // Cubeのメッシュ描画部品
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                color);
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
            mesh.SetMetallic(metallic);
            mesh.SetRoughness(roughness);
            // 構築したmesh fixtureを返します。
            return mesh;
        };
        addCube("GoldMetal", -1.6f, { 1.0f, 0.78f, 0.35f, 1.0f }, 0.9f, 0.15f);
        // 法線mapを使うCube
        auto& mapped = addCube(
            "NormalMapped",
            0.0f,
            { 0.6f, 0.7f, 0.9f, 1.0f },
            0.0f,
            0.55f);
        // 既存の画像を法線として読み、法線textureとstrengthの経路をD3D11 / D3D12で同じ入力にします。
        mapped.SetNormalTexturePath("textures/LamaPonLogo.png");
        mapped.SetNormalStrength(1.5f);
        // 非一様scaleの樹脂Cube
        auto& plastic = addCube(
            "RoughPlastic",
            1.6f,
            { 0.9f, 0.3f, 0.25f, 1.0f },
            0.25f,
            0.8f);
        // 非一様スケールでも、D3D11と同じ逆転置行列で法線を変換します。
        plastic.Owner().GetTransform().scale = { 1.2f, 0.6f, 0.9f };

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // scene compositionを撮影
        const auto captureFrame = [&graphics, &scene, &clearColor]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            // scene composition描画先
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
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 法線マップの読み込みを待たずに比べないよう、2フレーム目を使います。
        static_cast<void>(captureFrame());
        // 2フレーム目の比較画像
        const auto capture = captureFrame();
        // 明るい描画画素数
        std::size_t litPixels{};
        // Cubeの可視画素を走査
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            // 明るい画素を数える
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // 34灯のForward+ clustered lightingを検証します。
    // 画面の床へPoint/Spot光が届くことを確認します。
    // RenderClusteredLightingCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderClusteredLightingCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Clustered-light device
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
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // Clustered-light scene
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 4.0f, 7.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.5f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.02f);

        // 床で共有するCube頂点列
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 床のCube索引列
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // 光を受ける床object
        auto& floorObject = scene.CreateGameObject("Floor");
        floorObject.GetTransform().position = { 0.0f, -0.1f, 0.0f };
        floorObject.GetTransform().scale = { 12.0f, 0.2f, 10.0f };
        // 床のメッシュ描画部品
        auto& floor = floorObject.AddComponent<
            LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.8f, 0.8f, 0.8f, 1.0f });
        floor.SetProceduralMesh(cubeVertices, cubeIndices);
        floor.SetRoughness(0.7f);

        // Point Lightの色を巡回
        constexpr std::array<DirectX::XMFLOAT3, 4> pointColors{ {
            { 1.0f, 0.35f, 0.3f },
            { 0.35f, 1.0f, 0.4f },
            { 0.35f, 0.5f, 1.0f },
            { 1.0f, 0.9f, 0.35f } } };
        // 点光源の配置番号
        std::size_t pointIndex{};
        // 4行分の点光源を生成
        for (int row{}; row < 4; ++row)
        {
            // 各行に6灯を配置
            for (int column{}; column < 6; ++column)
            {
                // 生成する点光源object
                auto& object = scene.CreateGameObject("ClusterPoint");
                object.GetTransform().position = {
                    -5.0f + 2.0f * static_cast<float>(column),
                    0.6f,
                    -3.0f + 2.0f * static_cast<float>(row) };
        // Clustered point light
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
        // 床を照らすspot lightを10灯配置
        for (int index{}; index < 10; ++index)
        {
        // Spot-light object
            auto& object = scene.CreateGameObject("ClusterSpot");
            object.GetTransform().position = {
                -4.5f + static_cast<float>(index),
                2.5f,
                -4.4f };
            object.GetTransform().SetEulerAngles(
                -DirectX::XM_PIDIV2,
                0.0f,
                0.0f);
            // clustered spot light
            auto& light = object.AddComponent<LamaPon::SpotLightComponent>(
                DirectX::XMFLOAT3{ 0.9f, 0.8f, 1.0f },
                3.0f,
                4.0f,
                DirectX::XMConvertToRadians(20.0f),
                DirectX::XMConvertToRadians(30.0f));
            light.SetCastsShadows(false);
        }

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        graphics.BeginFrame(clearColor);
        graphics.BeginSceneComposition(clearColor);
        // scene composition描画先
        auto* const target = graphics.SceneCompositionTarget();
        Require(
            target != nullptr,
            "The clustered lighting capture has no scene composition target");
        scene.RenderMainCamera(
            static_cast<float>(CanvasWidth) / CanvasHeight,
            false,
            target);
        graphics.EndSceneComposition(scene.PostProcessFrameData());
        // Clustered-light capture
        Capture capture;
        capture.pixels = graphics.CaptureBackBuffer(
            capture.width,
            capture.height);
        graphics.EndFrame();

        // Resolved cluster info
        const auto& clustered = graphics.Lighting().clustered;
        // 有効な34灯clusterか
        const bool clusteredActive =
            clustered.enabled && clustered.lightCount == 34u;
        // 明るく描画された画素数
        std::size_t litPixels{};
        // 床の可視画素を集計
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            // 明るい画素を数える
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // 復元した2×2×2のベイクGI反映を検証します。
    // L1係数のpayloadとprobe shaderを使います。
    // RenderBakedGlobalIlluminationCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderBakedGlobalIlluminationCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // baked GIを描くデバイス
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
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // GI検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.4f, 4.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.06f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.3f);

        // Cubeで共有する頂点列
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 共有Cube索引列
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // addCube(name:名,x:X,metallic:金属,roughness:粗さ)
        const auto addCube = [&](
            const char* const name,
            const float x,
            const float metallic,
            const float roughness) -> LamaPon::MeshRendererComponent&
        {
            // 生成するCube object
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.1f, 1.1f, 1.1f };
            object.GetTransform().SetEulerAngles(0.45f, 0.6f, 0.0f);
            // Cubeのメッシュ描画部品
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.9f, 0.9f, 0.9f, 1.0f });
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
            mesh.SetMetallic(metallic);
            mesh.SetRoughness(roughness);
            // 構築したmesh fixtureを返します。
            return mesh;
        };
        addCube("GiDielectric", -1.7f, 0.0f, 0.8f);
        addCube("GiMetal", 0.0f, 0.6f, 0.4f);
        // Baked-GI probe cube
        auto& probe = addCube("BakedGiProbe", 1.7f, 0.0f, 0.5f);
        probe.SetShaderPath(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/baked-gi-probe.hlsl");

        // GI volumeの範囲と解像度
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
        // volume内のprobe数
        constexpr std::size_t ProbeCount = 8u;
        // R/G/Bの係数payload
        std::vector<std::uint16_t> payload(
            ProbeCount
            * LamaPon::BakedGlobalIlluminationCoefficientsPerProbe);
        // 各Z層のprobe係数を埋める
        for (std::size_t z{}; z < 2u; ++z)
        {
            // 各Y行を走査
            for (std::size_t y{}; y < 2u; ++y)
            {
                // 各X列を走査
                for (std::size_t x{}; x < 2u; ++x)
                {
                    // Z/Y/X順のprobe番号
                    const std::size_t probeIndex = (z * 2u + y) * 2u + x;
                    // 左右位置の補間値
                    const float side = static_cast<float>(x);
                    // 上下位置の補間値
                    const float height = static_cast<float>(y);
                    // R/G/BのSH係数
                    const std::array<std::array<float, 4>, 3> channels{ {
                        { 0.3f, 0.0f, 0.0f, 0.2f + 0.8f * side },
                        { 0.0f, 0.4f, 0.0f, 0.3f + 0.3f * height },
                        { 0.0f, 0.0f, 0.25f, 1.0f - 0.8f * side } } };
                    // RGB各chの係数をpack
                    for (std::size_t channel{}; channel < 3u; ++channel)
                    {
                        // L1基底の各係数をpack
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

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // scene compositionを撮影
        const auto captureFrame = [&graphics, &scene, &clearColor]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            // scene composition描画先
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
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // GI有効時の画像
        const auto giFrame = captureFrame();
        // Baked-GI shader error
        const auto shaderError = probe.ShaderError();
        // 3本のTexture3Dを作れずにフラットな環境光へ落ちていないことを確かめます。
        // Resolved baked GI
        const auto& bakedGi = graphics.Lighting().bakedGlobalIllumination;
        // RGB係数textureがすべて有効か
        const bool giActive = bakedGi.enabled
            && bakedGi.redCoefficients
            && bakedGi.greenCoefficients
            && bakedGi.blueCoefficients;
        settings.enabled = false;
        scene.SetBakedGlobalIlluminationSettings(settings);
        // GIを無効にした比較画像
        const auto flatFrame = captureFrame();
        // GI有効・無効で変化した画素数
        const auto changedPixels = CountChangedPixels(giFrame, flatFrame);
        Require(
            giActive && shaderError.empty() && changedPixels > 1500u,
            apiName + " did not light the cubes with baked global "
                "illumination (active: " + (giActive ? "yes" : "no") + ", "
                + std::to_string(changedPixels)
                + " changed pixels; error: [" + shaderError + "])");
        // global illumination frameを返します。
        return giFrame;
    }

    // 実行時に焼いた反射probeの寄与を検証します。
    // Box projection、probe blending、environment shaderを含めます。
    // RenderReflectionProbeCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderReflectionProbeCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // Reflection-probe device
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
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // Reflection-probe scene
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.4f, 4.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.06f, 0.0f, 0.0f);
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.1f);

        // 壁とCubeで共有する頂点列
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 共有形状の索引列
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // addWall(name:名,position:位置,scale:倍率,emissive:発光)
        const auto addWall = [&](
            const char* const name,
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& scale,
            const DirectX::XMFLOAT3& emissive)
        {
            // 生成する発光壁object
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = position;
            object.GetTransform().scale = scale;
            // 壁のメッシュ描画部品
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

        // addCube(name:名,x:X,metallic:金属,roughness:粗さ)
        const auto addCube = [&](
            const char* const name,
            const float x,
            const float metallic,
            const float roughness) -> LamaPon::MeshRendererComponent&
        {
            // 生成する反射Cube object
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.1f, 1.1f, 1.1f };
            object.GetTransform().SetEulerAngles(0.45f, 0.6f, 0.0f);
            // Cubeのメッシュ描画部品
            auto& mesh = object.AddComponent<LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.9f, 0.9f, 0.9f, 1.0f });
            mesh.SetProceduralMesh(cubeVertices, cubeIndices);
            mesh.SetMetallic(metallic);
            mesh.SetRoughness(roughness);
            // 構築したmesh fixtureを返します。
            return mesh;
        };
        addCube("ProbeMetal", -1.7f, 1.0f, 0.15f);
        addCube("ProbeRough", 0.0f, 0.0f, 0.9f);
        // Environment probe cube
        auto& shaderCube = addCube("ProbeShader", 1.7f, 0.0f, 0.5f);
        shaderCube.SetShaderPath(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/environment-probe.hlsl");

        // 左のプローブはボックス射影付きです。
        // 範囲4、混ぜ始め2なので、中央と右のCubeは2個目のプローブと比率で混ざります。
        // Box projection付きの主probe object
        auto& primaryObject = scene.CreateGameObject("PrimaryProbe");
        primaryObject.GetTransform().position = { -1.0f, 1.5f, 0.0f };
        // 主probe component
        auto& primaryProbe = primaryObject.AddComponent<
            LamaPon::ReflectionProbeComponent>(4.0f, 1.0f);
        primaryProbe.SetBoxExtents({ 5.0f, 2.75f, 6.5f });
        primaryProbe.SetBlendDistance(2.0f);
        primaryProbe.RequestBake();
        // Secondary probe object
        auto& secondaryObject = scene.CreateGameObject("SecondaryProbe");
        secondaryObject.GetTransform().position = { 2.5f, 1.5f, 0.0f };
        // 副probe component
        auto& secondaryProbe = secondaryObject.AddComponent<
            LamaPon::ReflectionProbeComponent>(4.0f, 1.2f);
        secondaryProbe.SetBlendDistance(2.0f);
        secondaryProbe.RequestBake();

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // probe反映後のscene画像を撮影
        const auto captureFrame = [&graphics, &scene, &clearColor]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            // scene composition描画先
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
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(
                frame.width,
                frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 最初のフレームの頭で2つのプローブを焼き、同じフレームから使います。
        // probeを焼いたscene画像
        const auto probeFrame = captureFrame();
        // Environment shader error
        const auto shaderError = shaderCube.ShaderError();
        // Both probes are baked.
        const bool baked = primaryProbe.IsBaked()
            && secondaryProbe.IsBaked()
            && graphics.IsSampleableCubeView(
                primaryProbe.BakedEnvironment().specular)
            && graphics.IsSampleableCubeView(
                secondaryProbe.BakedEnvironment().irradiance);
        primaryProbe.SetEnabled(false);
        secondaryProbe.SetEnabled(false);
        // probe無効時の比較画像
        const auto flatFrame = captureFrame();
        // probeで変化した画素数
        const auto changedPixels = CountChangedPixels(probeFrame, flatFrame);
        Require(
            baked && shaderError.empty() && changedPixels > 1500u,
            apiName + " did not light the cubes with baked reflection "
                "probes (baked: " + (baked ? "yes" : "no") + ", "
                + std::to_string(changedPixels)
                + " changed pixels; error: [" + shaderError + "])");
        // probe適用後のframeを返します。
        return probeFrame;
    }

    // Material override中のCMO shaderを指定APIで検証します。
    // 宣言済みtemplateと宣言なしのdefault stateを比較します。
    // RenderCmoMaterialShaderCapture(api: 描画API, profile: 起動設定)
    [[nodiscard]] Capture RenderCmoMaterialShaderCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 描画用ウィンドウ
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // CMO shaderを描くデバイス
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
        // 診断表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // CMO shader検証シーン
        LamaPon::Scene scene(graphics);
        // メインカメラ
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 6.0f };
        // 描画カメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // addModel(name:名,x:X)
        const auto addModel = [&](
            const char* const name,
            const float x) -> LamaPon::ModelRendererComponent&
        {
            // 生成するCMO object
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = { x, 0.0f, 0.0f };
            object.GetTransform().scale = { 1.4f, 1.4f, 1.4f };
            object.GetTransform().SetEulerAngles(0.45f, 0.65f, 0.0f);
            // CMOのmodel描画部品
            auto& renderer = object.AddComponent<
                LamaPon::ModelRendererComponent>(
                    std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
                        / "models"
                        / "arrow.cmo");
            renderer.SetMaterialOverrideEnabled(true);
            // 初期化したrendererを返します。
            return renderer;
        };
        // arrow.cmoの内蔵albedoは黒いため、テンプレートにはロゴを貼ります。
        // Declared-shader CMO
        auto& declared = addModel("DeclaredCmo", -1.3f);
        declared.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        declared.SetAlbedoTexturePath("textures/LamaPonLogo.png");
        declared.SetCustomParameter(0, { 1.0f, 0.4f, 0.2f, 0.7f });
        declared.SetCustomParameter(1, { 0.3f, 1.0f, 0.0f, 0.0f });
        // 宣言なしshaderを割り当てるCMO
        auto& defaultState = addModel("DefaultStateCmo", 1.3f);
        defaultState.SetShaderPath(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/facing-probe.hlsl");

        // 合成フレームの背景色
        constexpr float clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        // sceneを描画して画像を返す
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 現在フレームの撮影画像
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 最初のフレームでModelとShaderを読み込みます。
        static_cast<void>(render());
        // 2つのshaderを描いた画像
        const auto capture = render();
        // 宣言済みshaderの診断
        const auto declaredError = declared.ShaderError();
        // Default-state shader error
        const auto defaultStateError = defaultState.ShaderError();

        // 左側のtemplate描画画素数
        std::size_t leftPixels{};
        // Default-state pixel count
        std::size_t rightPixels{};
        // 背景外の画素を左右へ集計
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            // 背景に近い画素を除外
            if (std::max({
                    capture.pixels[offset],
                    capture.pixels[offset + 1u],
                    capture.pixels[offset + 2u] }) <= 12u)
            {
                // 背景画素を除外
                continue;
            }
            // 横位置を計算
            const auto x = (offset / 4u) % capture.width;
            // 左右のshader出力を分類
            if (x < capture.width / 2u)
            {
                ++leftPixels;
            }
            // 右側のdefault-state出力を集計
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // D3D11とテセレーション描画を比較し、Procedural Meshの非対応表示も確認します。
    // RenderTessellationCapture(api: 描画API、profile: 起動設定)
    [[nodiscard]] Capture RenderTessellationCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 非表示の描画窓
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 検証用描画器
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
        // 比較表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // 描画対象シーン
        LamaPon::Scene scene(graphics);
        // カメラ配置用オブジェクト
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 2.2f, 5.5f };
        cameraObject.GetTransform().SetEulerAngles(-0.35f, 0.0f, 0.0f);
        // メインカメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(0.6f);

        // 地形シェーダー付きメッシュを追加
        // addMesh(name: 名前、shape: 形状、position: 位置、scale: 拡大率、yaw: 水平回転)
        const auto addMesh = [&](
            const char* const name,
            const LamaPon::PrimitiveShape shape,
            const DirectX::XMFLOAT3& position,
            const DirectX::XMFLOAT3& scale,
            const float yaw) -> LamaPon::MeshRendererComponent&
        {
            // メッシュ用オブジェクト
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = position;
            object.GetTransform().scale = scale;
            object.GetTransform().SetEulerAngles(0.0f, yaw, 0.0f);
            // 地形シェーダー付き描画器
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
            // 構築したmesh fixtureを返します。
            return mesh;
        };
        // テセレーション平面
        auto& plane = addMesh(
            "TessellatedPlane",
            LamaPon::PrimitiveShape::Plane,
            { -1.4f, 0.0f, 0.0f },
            { 2.6f, 1.0f, 2.6f },
            0.3f);
        // テセレーション立方体
        auto& cube = addMesh(
            "TessellatedCube",
            LamaPon::PrimitiveShape::Cube,
            { 1.5f, 0.7f, 0.0f },
            { 1.3f, 1.3f, 1.3f },
            0.6f);
        // 手続き立方体の頂点
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 手続き立方体のインデックス
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // 非対応表示を確認する立方体
        auto& procedural = addMesh(
            "ProceduralCube",
            LamaPon::PrimitiveShape::Cube,
            { 0.3f, 1.9f, -1.2f },
            { 0.7f, 0.7f, 0.7f },
            0.4f);
        procedural.SetProceduralMesh(cubeVertices, cubeIndices);

        // フレーム消去色
        constexpr float clearColor[4]{ 0.05f, 0.05f, 0.08f, 1.0f };
        // シーンを描画して画像を取得
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 取得フレーム
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 最初のフレームでShaderを読み込みます。
        static_cast<void>(render());
        // 検査対象の描画画像
        const auto capture = render();
        // 平面Shaderのエラー
        const auto planeError = plane.ShaderError();
        // 立方体Shaderのエラー
        const auto cubeError = cube.ShaderError();
        // 手続きメッシュShaderのエラー
        const auto proceduralError = procedural.ShaderError();

        // 地形色の画素数
        std::size_t terrainPixels{};
        // 非対応表示の画素数
        std::size_t magentaPixels{};
        // 描画画像の画素を分類
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            // 赤チャンネル値
            const int red = capture.pixels[offset];
            // 緑チャンネル値
            const int green = capture.pixels[offset + 1u];
            // 青チャンネル値
            const int blue = capture.pixels[offset + 2u];
            // 地形色を集計
            if (green > red + 40 && green > blue + 40)
            {
                ++terrainPixels;
            }
            // 非対応表示色を集計
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // CMOのカスタム輪郭と遮蔽表示をD3D11と比較します。
    // RenderCmoOutlineCapture(api: 描画API、profile: 起動設定)
    [[nodiscard]] Capture RenderCmoOutlineCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 非表示の描画窓
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 検証用描画器
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
        // 比較表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // 描画対象シーン
        LamaPon::Scene scene(graphics);
        // カメラ配置用オブジェクト
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 5.0f };
        // メインカメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // モデルより先に描いて深度を書く遮蔽箱です。
        // Occluder cube vertices.
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 遮蔽用立方体のインデックス
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // 遮蔽用の手前オブジェクト
        auto& occluderObject = scene.CreateGameObject("Occluder");
        occluderObject.GetTransform().position = { 0.0f, -0.3f, 2.0f };
        occluderObject.GetTransform().scale = { 0.8f, 0.5f, 0.2f };
        // 遮蔽用立方体描画器
        auto& occluder = occluderObject.AddComponent<
            LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.35f, 0.35f, 0.35f, 1.0f });
        occluder.SetProceduralMesh(cubeVertices, cubeIndices);

        // 輪郭を描くCMOオブジェクト
        auto& object = scene.CreateGameObject("OutlinedCmo");
        object.GetTransform().position = { 0.0f, 0.2f, 0.0f };
        object.GetTransform().scale = { 1.6f, 1.6f, 1.6f };
        object.GetTransform().SetEulerAngles(0.45f, 0.65f, 0.0f);
        // CMOモデル描画器
        auto& renderer = object.AddComponent<
            LamaPon::ModelRendererComponent>(
                std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
                    / "models"
                    / "arrow.cmo");
        renderer.SetMaterialOverrideEnabled(true);
        renderer.SetShaderPath(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures/outline-probe.hlsl");
        // 3: x = 輪郭の太さ、yzw = 輪郭の色。
        // 4: rgb = 遮蔽表示の色、w > 0で遮蔽表示を有効にします。
        renderer.SetCustomParameter(3, { 0.08f, 1.0f, 0.85f, 0.1f });
        renderer.SetCustomParameter(4, { 0.2f, 0.6f, 1.0f, 1.0f });

        // フレーム消去色
        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        // シーンを描画して画像を取得
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 取得フレーム
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 最初のフレームでModelとShaderを読み込みます。
        static_cast<void>(render());
        // 検査対象の描画画像
        const auto capture = render();
        // カスタムShaderのエラー
        const auto shaderError = renderer.ShaderError();

        // 輪郭色の画素数
        std::size_t outlinePixels{};
        // 遮蔽色の画素数
        std::size_t occludedPixels{};
        // 描画画像の画素を分類
        for (std::size_t offset{};
             offset + 3u < capture.pixels.size();
             offset += 4u)
        {
            // 赤チャンネル値
            const int red = capture.pixels[offset];
            // 緑チャンネル値
            const int green = capture.pixels[offset + 1u];
            // 青チャンネル値
            const int blue = capture.pixels[offset + 2u];
            // 輪郭色を集計
            if (red > 170 && green > 140 && blue < 90)
            {
                ++outlinePixels;
            }
            // 遮蔽表示色を集計
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
        // 撮影したframe captureを返します。
        return capture;
    }

    // 各モデルShaderのワイヤーフレーム表示をD3D11と比較します。
    // RenderWireframeCapture(api: 描画API、profile: 起動設定)
    [[nodiscard]] Capture RenderWireframeCapture(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 非表示の描画窓
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 検証用描画器
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
        // 比較表示用のAPI名
        const std::string apiName = api == LamaPon::RenderingApi::DirectX11
            ? "DirectX 11"
            : "DirectX 12";

        // 描画対象シーン
        LamaPon::Scene scene(graphics);
        // カメラ配置用オブジェクト
        auto& cameraObject = scene.CreateGameObject("MainCamera");
        cameraObject.GetTransform().position = { 0.0f, 0.0f, 8.0f };
        // メインカメラ
        auto& camera = cameraObject.AddComponent<LamaPon::CameraComponent>();
        scene.SetMainCamera(camera);
        scene.SetAmbientLightColor({ 1.0f, 1.0f, 1.0f });
        scene.SetAmbientLightIntensity(1.0f);

        // 暗い線の視認用に奥へ明るい板を置きます。
        // Backdrop cube vertices.
        std::vector<LamaPon::ProceduralMeshVertex> cubeVertices;
        // 背景板のインデックス
        std::vector<std::uint32_t> cubeIndices;
        BuildProceduralCube(cubeVertices, cubeIndices);
        // ワイヤーフレーム用背景オブジェクト
        auto& backdropObject = scene.CreateGameObject("Backdrop");
        backdropObject.GetTransform().position = { 0.0f, 0.0f, -3.0f };
        backdropObject.GetTransform().scale = { 24.0f, 12.0f, 0.2f };
        // 明るい背景板の描画器
        auto& backdrop = backdropObject.AddComponent<
            LamaPon::MeshRendererComponent>(
                LamaPon::PrimitiveShape::Cube,
                DirectX::XMFLOAT4{ 0.55f, 0.55f, 0.6f, 1.0f });
        backdrop.SetProceduralMesh(cubeVertices, cubeIndices);

        // テストモデルの配置先
        const auto models = std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / "models";
        // Shader fixtureの配置先
        const auto fixtures =
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
            / "tests/fixtures";
        // ワイヤーフレーム設定対象
        std::vector<LamaPon::ModelRendererComponent*> renderers;
        // モデルと描画器をシーンへ追加
        // addModel(name: 名前、path: モデルパス、position: 位置、scale: 拡大率、roll: 傾き)
        const auto addModel = [&](
            const char* const name,
            const std::filesystem::path& path,
            const DirectX::XMFLOAT3& position,
            const float scale,
            const float roll = 0.0f) -> LamaPon::ModelRendererComponent&
        {
            // モデル配置用オブジェクト
            auto& object = scene.CreateGameObject(name);
            object.GetTransform().position = position;
            object.GetTransform().scale = { scale, scale, scale };
            object.GetTransform().SetEulerAngles(0.45f, 0.65f, roll);
            // 追加したモデル描画器
            auto& renderer =
                object.AddComponent<LamaPon::ModelRendererComponent>(path);
            renderer.SetAnimationPlayOnStart(false);
            renderer.SetWireframe(true);
            renderers.push_back(&renderer);
            // 初期化したrendererを返します。
            return renderer;
        };
        // Material上書き無しCMO
        static_cast<void>(addModel(
            "EffectCmo",
            models / "arrow.cmo",
            { -3.0f, 1.6f, 0.0f },
            1.6f));
        // Lit上書きのCMO
        auto& litCmo = addModel(
            "LitCmo",
            models / "arrow.cmo",
            { 0.0f, 1.6f, 0.0f },
            1.6f);
        litCmo.SetMaterialOverrideEnabled(true);
        litCmo.SetAlbedoTexturePath("textures/LamaPonLogo.png");
        // 宣言無しShaderのCMO
        auto& undeclaredCmo = addModel(
            "UndeclaredShaderCmo",
            models / "arrow.cmo",
            { 3.0f, 1.6f, 0.0f },
            1.6f);
        undeclaredCmo.SetMaterialOverrideEnabled(true);
        undeclaredCmo.SetShaderPath(fixtures / "facing-probe.hlsl");
        // 宣言付きShaderのCMO
        auto& declaredCmo = addModel(
            "DeclaredShaderCmo",
            models / "arrow.cmo",
            { -3.0f, -1.6f, 0.0f },
            1.6f);
        declaredCmo.SetMaterialOverrideEnabled(true);
        declaredCmo.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        declaredCmo.SetAlbedoTexturePath("textures/LamaPonLogo.png");
        // Lit既定表示のglTF
        static_cast<void>(addModel(
            "LitGltf",
            models / "TexturedRiggedSimple.gltf",
            { 0.0f, -1.6f, 0.0f },
            0.4f,
            1.1f));
        // カスタムShaderのglTF
        auto& shaderGltf = addModel(
            "ShaderGltf",
            models / "TexturedRiggedSimple.gltf",
            { 3.0f, -1.6f, 0.0f },
            0.4f,
            1.1f);
        shaderGltf.SetMaterialOverrideEnabled(true);
        shaderGltf.SetShaderPath("shaders/LamaPonCustomMaterial.hlsl");
        shaderGltf.SetCustomParameter(0, { 1.0f, 0.5f, 0.2f, 0.6f });

        // フレーム消去色
        constexpr float clearColor[4]{ 0.08f, 0.1f, 0.14f, 1.0f };
        // シーンを描画して画像を取得
        const auto render = [&]()
        {
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                static_cast<float>(CanvasWidth) / CanvasHeight,
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(scene.PostProcessFrameData());
            // 取得フレーム
            Capture frame;
            frame.pixels = graphics.CaptureBackBuffer(frame.width, frame.height);
            graphics.EndFrame();
            // 構築した描画frameを返します。
            return frame;
        };
        // 最初のフレームでModelとShaderを読み込みます。
        static_cast<void>(render());
        // ワイヤーフレーム画像
        const auto wireframe = render();
        // 読み込みエラー一覧
        std::string errors;
        // 全モデルのShader状態を確認
        for (auto* const renderer : renderers)
        {
            // Shaderエラーを記録
            if (!renderer->ShaderError().empty())
            {
                errors += "[" + renderer->ShaderError() + "] ";
            }
            // 塗りつぶし表示へ戻す
            renderer->SetWireframe(false);
        }
        // 塗りつぶし画像
        const auto solid = render();
        // 表示差分の画素数
        const auto changedPixels = CountChangedPixels(wireframe, solid);
        Require(
            errors.empty() && changedPixels > 1000u,
            apiName + " did not draw the Model Renderer wireframes ("
                + std::to_string(changedPixels)
                + " pixels changed from the solid frame; errors: " + errors
                + ")");
        // wireframe描画pixelを返します。
        return wireframe;
    }
}

namespace
{
    // 遷移表示と読み込み画面を両APIで比較します。
    struct SceneTransitionCase final
    {
        // テストケース名
        const char* name{};
        // パッケージの遷移設定
        LamaPonSceneShowcase::Look look;
        // 遷移の進行度
        float coverage{};
        // 表示対象へ切り替える向き
        bool revealing{};
        // 読み込み画面の不透明度
        float loadingScreenAlpha{};
    };

    // 遷移テストの背景色
    constexpr float TransitionClearColor[4]{ 0.08f, 0.12f, 0.18f, 1.0f };

    // 比較する遷移設定を列挙します。
    [[nodiscard]] std::vector<SceneTransitionCase> SceneTransitionCases()
    {
        using LamaPonSceneShowcase::Effect;
        // 遷移シェーダーの赤色
        const DirectX::XMFLOAT4 red{ 1.0f, 0.0f, 0.0f, 1.0f };
        // 強調表示の黄色
        const DirectX::XMFLOAT4 yellow{ 1.0f, 0.9f, 0.1f, 1.0f };
        // 基本色付き遷移設定を作成
        // make(effect: 遷移効果)
        const auto make = [&red](const Effect effect)
            {
                // 作成する遷移設定
                LamaPonSceneShowcase::Look look;
                look.effect = effect;
                look.color = red;
                // 指定条件に一致したcaseを返します。
                return look;
            };
        // 比較対象の遷移設定
        std::vector<SceneTransitionCase> cases;

        // 円形ワイプ設定
        auto iris = make(Effect::Iris);
        iris.accentColor = yellow;
        cases.push_back({ "iris", iris, 0.5f, false, 0.0f });

        // 斜めワイプ設定
        auto wipe = make(Effect::Wipe);
        wipe.direction =
            LamaPonSceneShowcase::Direction::TopLeftToBottomRight;
        wipe.softness = 0.1f;
        cases.push_back({ "diagonal wipe", wipe, 0.5f, false, 0.0f });

        // ブラインド設定
        auto blinds = make(Effect::Blinds);
        blinds.direction = LamaPonSceneShowcase::Direction::TopToBottom;
        blinds.divisions = 8;
        blinds.stagger = 0.0f;
        cases.push_back({ "blinds", blinds, 0.5f, false, 0.0f });

        // ダイヤ形タイル設定
        auto diamondTiles = make(Effect::DiamondTiles);
        diamondTiles.accentColor = yellow;
        diamondTiles.divisions = 8;
        cases.push_back(
            { "diamond tiles", diamondTiles, 0.5f, false, 0.0f });

        // ドット設定
        auto dots = make(Effect::Dots);
        dots.divisions = 8;
        cases.push_back({ "dots", dots, 0.7f, true, 0.0f });

        cases.push_back(
            { "diamond iris", make(Effect::Diamond), 0.5f, false, 0.0f });

        // ハートShader設定
        auto heart = make(Effect::Shader);
        heart.shaderPattern = LamaPonSceneShowcase::ShaderPattern::Heart;
        cases.push_back({ "shader heart", heart, 0.6f, false, 0.0f });

        // ディゾルブShader設定
        auto dissolve = make(Effect::Shader);
        dissolve.shaderPattern =
            LamaPonSceneShowcase::ShaderPattern::Dissolve;
        dissolve.accentColor = yellow;
        cases.push_back({ "shader dissolve", dissolve, 0.5f, false, 0.0f });

        // ルール画像Shader設定
        auto rule = make(Effect::Shader);
        rule.shaderPattern = LamaPonSceneShowcase::ShaderPattern::RuleImage;
        rule.ruleTexture = "rules/gradient.png";
        cases.push_back({ "rule image", rule, 0.5f, false, 0.0f });

        cases.push_back(
            { "fade", make(Effect::Fade), 0.5f, false, 0.0f });

        // 覆い終えた後に重ねる読み込み画面（フェード途中）です。
        cases.push_back(
            { "loading overlay", make(Effect::Fade), 1.0f, false, 0.5f });
        // 登録したpost-process case一覧を返します。
        return cases;
    }

    // 遷移設定を描画し、各フレームを取得します。
    // RenderSceneTransitionCaptures(api: 描画API、profile: 起動設定)
    [[nodiscard]] std::vector<Capture> RenderSceneTransitionCaptures(
        const LamaPon::RenderingApi api,
        const LamaPon::GraphicsStartupProfile profile)
    {
        // 非表示の描画窓
        HiddenWindow window{ CanvasWidth, CanvasHeight };
        // 検証用描画器
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(
            window.Get(),
            CanvasWidth,
            CanvasHeight,
            api,
            profile);
        Require(
            graphics.ActiveRenderingApi() == api,
            "The scene transition test did not start the requested API");
        // パッケージのシェーダーとルール画像はpackages/srcから読みます（.metaは作りません）。
        graphics.Assets().SetAssetRoot(
            std::filesystem::path{ LAMAPON_TEST_ASSET_DIR }.parent_path()
                / "packages/src/scene-transition-showcase",
            false);

        // Transition test scene
        LamaPon::Scene scene(graphics);
        // 全画面遷移オブジェクト
        auto& overlay = scene.CreateGameObject("SceneTransition.Overlay");
        overlay.AddComponent<LamaPon::UIRectTransformComponent>(
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            DirectX::XMFLOAT2{ 1.0f, 1.0f },
            DirectX::XMFLOAT2{ 0.5f, 0.5f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f },
            DirectX::XMFLOAT2{ 0.0f, 0.0f });
        // 遷移Shaderを使うスプライト
        auto& sprite =
            overlay.AddComponent<LamaPon::SpriteRendererComponent>();

        // 描画する読み込み画面設定
        LamaPon::SceneLoadingScreenSettings loading;
        loading.message = "Loading";
        loading.hint = "Hint";
        loading.showSpinner = true;
        // 遷移ごとの取得画像
        std::vector<Capture> captures;
        // 各遷移設定を描画
        for (const auto& transitionCase : SceneTransitionCases())
        {
            // 現在の遷移設定
            const auto& look = transitionCase.look;
            sprite.SetShaderPath("shaders/LamaPonSceneTransition.hlsl");
            sprite.SetTexturePath(
                LamaPon::PathFromUtf8(look.ruleTexture));
            sprite.SetColor(look.color);
            // パッケージShader用パラメーター
            const auto shaderFrame = LamaPonSceneShowcase::BuildShaderFrame(
                look,
                transitionCase.coverage,
                transitionCase.revealing,
                !look.ruleTexture.empty());
            // Shader引数をスプライトへ設定
            for (std::size_t index{};
                index < shaderFrame.parameters.size();
                ++index)
            {
                sprite.SetCustomParameter(
                    index,
                    shaderFrame.parameters[index]);
            }

            // 描画する遷移状態
            LamaPon::SceneTransitionFrame frame;
            frame.settings = LamaPon::MakeSceneTransition(0.4f);
            frame.phase = transitionCase.coverage >= 1.0f
                ? LamaPon::SceneTransitionPhase::Covered
                : LamaPon::SceneTransitionPhase::Covering;
            frame.coverage = transitionCase.coverage;
            frame.loadingScreenAlpha = transitionCase.loadingScreenAlpha;
            frame.loadingProgress = 0.4f;
            frame.loadingScreenTime = 0.3f;
            graphics.BeginFrame(TransitionClearColor);
            scene.Render2D();
            graphics.DrawLoadingScreen(frame, loading);
            // 取得する遷移画像
            Capture capture;
            capture.pixels = graphics.CaptureBackBuffer(
                capture.width,
                capture.height);
            graphics.EndFrame();
            Require(
                sprite.ShaderError().empty(),
                std::string("The package transition shader failed for '")
                    + transitionCase.name + "': " + sprite.ShaderError());
            captures.push_back(std::move(capture));
        }

        // シェーダーを使えないときはShaderErrorで分かります（パッケージの覆いはこれを見てフェードへ切り替えます）。
        sprite.SetShaderPath("shaders/does-not-exist-transition.hlsl");
        graphics.BeginFrame(TransitionClearColor);
        scene.Render2D();
        graphics.EndFrame();
        Require(
            !sprite.ShaderError().empty(),
            "A missing transition shader must report its error.");
        // 撮影したframe captureを返します。
        return captures;
    }

    // 取得画像から指定座標のRGBを返します。
    // TransitionPixel(capture: 画像、x: 横座標、y: 縦座標)
    [[nodiscard]] std::array<int, 3> TransitionPixel(
        const Capture& capture,
        const std::uint32_t x,
        const std::uint32_t y)
    {
        // RGBA画像内の画素位置
        const auto offset =
            (static_cast<std::size_t>(y) * capture.width + x) * 4u;
        Require(
            offset + 2u < capture.pixels.size(),
            "A scene transition pixel is outside the capture");
        // 取得pixelのRGB channel値を返します。
        return {
            static_cast<int>(capture.pixels[offset]),
            static_cast<int>(capture.pixels[offset + 1u]),
            static_cast<int>(capture.pixels[offset + 2u])
        };
    }

    // 背景色と一致する画素か判定します。
    // IsTransitionClear(pixel: RGB画素)
    [[nodiscard]] bool IsTransitionClear(
        const std::array<int, 3>& pixel) noexcept
    {
        // TransitionClearColorの(20, 31, 46)です。
        return std::abs(pixel[0] - 20) <= 3
            && std::abs(pixel[1] - 31) <= 3
            && std::abs(pixel[2] - 46) <= 3;
    }

    // 遷移の赤色と一致する画素か判定します。
    // IsTransitionRed(pixel: RGB画素)
    [[nodiscard]] bool IsTransitionRed(
        const std::array<int, 3>& pixel) noexcept
    {
        // pixel channelが許容範囲内か判定します。
        return pixel[0] >= 250 && pixel[1] <= 5 && pixel[2] <= 5;
    }

    // RGB画素を診断表示用文字列にします。
    // DescribeTransitionPixel(pixel: RGB画素)
    [[nodiscard]] std::string DescribeTransitionPixel(
        const std::array<int, 3>& pixel)
    {
        // RGB値を比較用の文字列に整形します。
        return "(" + std::to_string(pixel[0]) + ", "
            + std::to_string(pixel[1]) + ", "
            + std::to_string(pixel[2]) + ")";
    }

    // 遷移の形状と読み込み画面を検証します。
    // RequireSceneTransitionCaptures(api: 描画API、captures: 取得画像)
    void RequireSceneTransitionCaptures(
        const LamaPon::RenderingApi api,
        const std::vector<Capture>& captures)
    {
        // 遷移ケース一覧
        const auto cases = SceneTransitionCases();
        Require(
            captures.size() == cases.size(),
            "The scene transition captures are incomplete");
        // 診断表示用のAPI名
        const std::string apiName =
            api == LamaPon::RenderingApi::DirectX11
                ? "DirectX 11"
                : "DirectX 12";
        // 画面中央の横座標
        const std::uint32_t centerX = CanvasWidth / 2u;
        // 画面中央の縦座標
        const std::uint32_t centerY = CanvasHeight / 2u;
        // ケース名付きで検証
        // require(index: ケース番号、condition: 判定結果、message: 失敗理由)
        const auto require = [&](
                const std::size_t index,
                const bool condition,
                const std::string& message)
            {
                Require(
                    condition,
                    std::string("Scene transition '") + cases[index].name
                        + "' on " + apiName + ": " + message);
            };
        // 各遷移画像を検査
        for (std::size_t index{}; index < cases.size(); ++index)
        {
            // 現在の取得画像
            const auto& capture = captures[index];
            require(
                index,
                capture.width == CanvasWidth
                    && capture.height == CanvasHeight
                    && capture.pixels.size()
                        == static_cast<std::size_t>(CanvasWidth)
                            * CanvasHeight * 4u,
                "unexpected capture size");
            // 現在のケース名
            const std::string name = cases[index].name;
            // 画面中央のRGB値
            const auto center = TransitionPixel(capture, centerX, centerY);
            // 左上隅のRGB値
            const auto corner = TransitionPixel(capture, 2u, 2u);
            // 右下隅のRGB値
            const auto farCorner = TransitionPixel(
                capture,
                CanvasWidth - 3u,
                CanvasHeight - 3u);
            // 円形・ハート形の覆いを確認
            if (name == "iris"
                || name == "diamond iris"
                || name == "shader heart")
            {
                // 中心だけが残り、画面の角は覆われます。
                // シェーダーが使えずFadeで代用すると中心も半透明に覆われます。
                require(
                    index,
                    IsTransitionClear(center),
                    "the center must stay visible, got "
                        + DescribeTransitionPixel(center));
                require(
                    index,
                    IsTransitionRed(corner) && IsTransitionRed(farCorner),
                    "the corners must be covered, got "
                        + DescribeTransitionPixel(corner) + " and "
                        + DescribeTransitionPixel(farCorner));
            }
            // 斜めワイプの方向を確認
            else if (name == "diagonal wipe")
            {
                require(
                    index,
                    IsTransitionRed(corner)
                        && IsTransitionClear(farCorner),
                    "the wipe must start from the top-left, got "
                        + DescribeTransitionPixel(corner) + " and "
                        + DescribeTransitionPixel(farCorner));
            }
            // 模様状遷移の画素分布を確認
            else if (name == "shader dissolve"
                || name == "blinds"
                || name == "rule image")
            {
                // 覆われた画素数
                std::size_t covered{};
                // 背景が見える画素数
                std::size_t clear{};
                // 画面を間引いて模様を計測
                for (std::uint32_t y{ 1u }; y < CanvasHeight; y += 4u)
                {
                    // 行内の画素を計測
                    for (std::uint32_t x{ 1u }; x < CanvasWidth; x += 4u)
                    {
                        // 現在位置のRGB値
                        const auto pixel = TransitionPixel(capture, x, y);
                        covered += IsTransitionRed(pixel) ? 1u : 0u;
                        clear += IsTransitionClear(pixel) ? 1u : 0u;
                    }
                }
                // 一様なフェードになってしまうと、どちらも0です。
                require(
                    index,
                    covered > 64u && clear > 64u,
                    "the pattern must mix covered and visible pixels ("
                        + std::to_string(covered) + " covered, "
                        + std::to_string(clear) + " visible)");
            }
            // 一様なフェードを確認
            else if (name == "fade")
            {
                // 画面全体が一様に半分だけ覆われます。
                // 中央と隅が同じ色か判定
                const bool uniform =
                    std::abs(center[0] - corner[0]) <= 2
                    && std::abs(center[1] - corner[1]) <= 2
                    && std::abs(center[2] - corner[2]) <= 2;
                require(
                    index,
                    uniform
                        && center[0] > 110 && center[0] < 150
                        && center[1] < 30,
                    "a fade must cover the screen uniformly, got "
                        + DescribeTransitionPixel(center) + " and "
                        + DescribeTransitionPixel(corner));
            }
            // 読み込み画面の重なりを確認
            else if (name == "loading overlay")
            {
                // 覆い終えた赤の上へ、読み込み画面の背景が半分重なります。
                require(
                    index,
                    corner[0] < 250 && corner[0] > 60,
                    "the loading screen must fade over the cover, got "
                        + DescribeTransitionPixel(corner));
            }
        }
    }

    // 両APIの遷移画像を比較し、縁の丸め差だけ許容します。
    // RequireMatchingSceneTransitionCaptures(d3d11:DX11,d3d12:DX12)
    void RequireMatchingSceneTransitionCaptures(
        const std::vector<Capture>& d3d11,
        const std::vector<Capture>& d3d12)
    {
        // 遷移ケース一覧
        const auto cases = SceneTransitionCases();
        Require(
            d3d11.size() == cases.size() && d3d12.size() == cases.size(),
            "The scene transition captures are incomplete");
        // 各APIの対応画像を比較
        for (std::size_t index{}; index < cases.size(); ++index)
        {
            // D3D11側の画像
            const auto& left = d3d11[index];
            // D3D12側の画像
            const auto& right = d3d12[index];
            Require(
                left.pixels.size() == right.pixels.size(),
                std::string("Scene transition '") + cases[index].name
                    + "' captures have different sizes");
            // 差がある画素数
            std::size_t mismatched{};
            // 最大チャンネル差
            int largest{};
            // 各画像画素の色差を測定
            for (std::size_t pixel{};
                pixel * 4u + 3u < left.pixels.size();
                ++pixel)
            {
                // 現在画素の最大色差
                int difference{};
                // RGB各チャンネルを比較
                for (std::size_t channel{}; channel < 3u; ++channel)
                {
                    difference = std::max(
                        difference,
                        std::abs(
                            static_cast<int>(
                                left.pixels[pixel * 4u + channel])
                            - static_cast<int>(
                                right.pixels[pixel * 4u + channel])));
                }
                largest = std::max(largest, difference);
                mismatched += difference > 2 ? 1u : 0u;
            }
            // 許容する差分画素数
            const std::size_t allowed =
                static_cast<std::size_t>(CanvasWidth) * CanvasHeight / 200u;
            Require(
                mismatched <= allowed,
                std::string("Scene transition '") + cases[index].name
                    + "' differs between DirectX 11 and DirectX 12: "
                    + std::to_string(mismatched)
                    + " pixels, largest channel difference "
                    + std::to_string(largest));
        }
    }
}

// D3D11とD3D12の描画結果を比較します。
int main()
{
    // AssetManagerのWIC / DirectWrite factoryと文字textureはCOMを使います。
    // COM initialization result.
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // COM初期化を解除する必要があるか
    const bool uninitialize = SUCCEEDED(comResult);
    // テストの終了コード
    int result = 0;
    // 全描画検証を実行
    try
    {
        LamaPon::GraphicsDevice::SetPreferWarpAdapter(true);
        // D3D11/12で共用するcube DDSです。
        // Cube DDS bytes.
        const auto cubeSkyBytes = BuildCubeDds({ {
            { 230u, 60u, 40u },
            { 40u, 180u, 70u },
            { 60u, 90u, 230u },
            { 230u, 210u, 50u },
            { 200u, 60u, 200u },
            { 50u, 200u, 210u } } });
        RequireDdsCubeParser(cubeSkyBytes);
        // D3D11基準画像
        const auto d3d11 = RenderCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11遷移画像
        const auto d3d11SceneTransitions = RenderSceneTransitionCaptures(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        RequireSceneTransitionCaptures(
            LamaPon::RenderingApi::DirectX11,
            d3d11SceneTransitions);
        // D3D11後処理画像
        const auto d3d11PostProcess = RenderPostProcessCaptures(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 AO画像
        const auto d3d11AmbientOcclusion = RenderAmbientOcclusionCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 SSR画像
        const auto d3d11ScreenSpaceReflection =
            RenderScreenSpaceReflectionCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 CMO画像
        const auto d3d11CmoModel = RenderCmoModelCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11画面効果画像
        const auto d3d11CustomScreenEffect =
            RenderCustomScreenEffectCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11深度効果画像
        const auto d3d11ScreenEffectDepth = RenderScreenEffectDepthCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11補助入力画像
        const auto d3d11ScreenEffectTextures =
            RenderScreenEffectAuxiliaryCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Compute画像
        const auto d3d11ComputeEffect = RenderComputeEffectCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer,
            false);
        // D3D11 Compute入力画像
        const auto d3d11ComputeEffectInputs = RenderComputeEffectCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer,
            true);
        // D3D11 Material画像
        const auto d3d11MaterialShaders = RenderMaterialShaderCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 shader batch
        const auto d3d11MaterialShaderInstancing =
            RenderMaterialShaderInstancingCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Lit instancing
        const auto d3d11BuiltInInstancing = RenderBuiltInInstancingCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Cull画像
        const auto d3d11MeshCull = RenderMeshCullCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11形状Cull画像
        const auto d3d11BuiltInShapeCull = RenderBuiltInShapeCullCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Particle画像
        const auto d3d11CustomParticleShaders =
            RenderCustomParticleShaderCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 DDS画像
        const auto d3d11DdsDimensions = RenderDdsDimensionCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Skinning画像
        const auto d3d11SkinnedMaterialShaders =
            RenderSkinnedMaterialShaderCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Sky画像
        const auto d3d11Sky = RenderSkyCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Cubemap画像
        const auto d3d11CubemapSky = RenderCubemapSkyCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer,
            cubeSkyBytes);
        // D3D11 Fog画像
        const auto d3d11Fog = RenderFogCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11直接照明画像
        const auto d3d11DirectLighting = RenderDirectLightingCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Clustered照明
        const auto d3d11ClusteredLighting = RenderClusteredLightingCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 GI画像
        const auto d3d11BakedGlobalIllumination =
            RenderBakedGlobalIlluminationCapture(
                LamaPon::RenderingApi::DirectX11,
                LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Probe画像
        const auto d3d11ReflectionProbes = RenderReflectionProbeCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 CMO Material
        const auto d3d11CmoMaterialShaders = RenderCmoMaterialShaderCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Tessellation
        const auto d3d11Tessellation = RenderTessellationCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 CMO輪郭
        const auto d3d11CmoOutline = RenderCmoOutlineCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);
        // D3D11 Wireframe
        const auto d3d11Wireframe = RenderWireframeCapture(
            LamaPon::RenderingApi::DirectX11,
            LamaPon::GraphicsStartupProfile::FullRenderer);

        // D3D12側だけdebug layerを有効にし、描画中のvalidation errorを描画結果の一致とは別に検出します。
        LamaPon::Logger::Instance().Clear();
        LamaPon::GraphicsDevice::SetEnableDebugLayer(true);
        // D3D12基準画像
        const auto d3d12 = RenderCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        // D3D12遷移画像
        const auto d3d12SceneTransitions = RenderSceneTransitionCaptures(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        // D3D12後処理画像
        const auto d3d12PostProcess = RenderPostProcessCaptures(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        // D3D12 AO画像
        const auto d3d12AmbientOcclusion = RenderAmbientOcclusionCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        // D3D12 SSR画像
        const auto d3d12ScreenSpaceReflection =
            RenderScreenSpaceReflectionCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        // D3D12 CMO画像
        const auto d3d12CmoModel = RenderCmoModelCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        // D3D12画面効果画像
        const auto d3d12CustomScreenEffect =
            RenderCustomScreenEffectCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        // D3D12深度効果画像
        const auto d3d12ScreenEffectDepth = RenderScreenEffectDepthCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalBootstrap);
        // D3D12補助入力画像
        const auto d3d12ScreenEffectTextures =
            RenderScreenEffectAuxiliaryCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        // D3D12 Compute画像
        const auto d3d12ComputeEffect = RenderComputeEffectCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap,
            false);
        // D3D12 Compute入力画像
        const auto d3d12ComputeEffectInputs = RenderComputeEffectCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap,
            true);
        // D3D12 Material画像
        const auto d3d12MaterialShaders = RenderMaterialShaderCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 shader batch
        const auto d3d12MaterialShaderInstancing =
            RenderMaterialShaderInstancingCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        // D3D12 Lit instancing
        const auto d3d12BuiltInInstancing = RenderBuiltInInstancingCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 Cull画像
        const auto d3d12MeshCull = RenderMeshCullCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12形状Cull画像
        const auto d3d12BuiltInShapeCull = RenderBuiltInShapeCullCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 Particle画像
        const auto d3d12CustomParticleShaders =
            RenderCustomParticleShaderCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        // D3D12 DDS画像
        const auto d3d12DdsDimensions = RenderDdsDimensionCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 Skinning画像
        const auto d3d12SkinnedMaterialShaders =
            RenderSkinnedMaterialShaderCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        // D3D12 Sky画像
        const auto d3d12Sky = RenderSkyCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 Cubemap画像
        const auto d3d12CubemapSky = RenderCubemapSkyCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap,
            cubeSkyBytes);
        // D3D12 Fog画像
        const auto d3d12Fog = RenderFogCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12直接照明画像
        const auto d3d12DirectLighting = RenderDirectLightingCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 Clustered照明
        const auto d3d12ClusteredLighting = RenderClusteredLightingCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 GI画像
        const auto d3d12BakedGlobalIllumination =
            RenderBakedGlobalIlluminationCapture(
                LamaPon::RenderingApi::DirectX12Experimental,
                LamaPon::GraphicsStartupProfile::
                    AllowD3D12ExperimentalBootstrap);
        // D3D12 Probe画像
        const auto d3d12ReflectionProbes = RenderReflectionProbeCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 CMO Material
        const auto d3d12CmoMaterialShaders = RenderCmoMaterialShaderCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 Tessellation
        const auto d3d12Tessellation = RenderTessellationCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 CMO輪郭
        const auto d3d12CmoOutline = RenderCmoOutlineCapture(
            LamaPon::RenderingApi::DirectX12Experimental,
            LamaPon::GraphicsStartupProfile::AllowD3D12ExperimentalBootstrap);
        // D3D12 Wireframe
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
        RequireSceneTransitionCaptures(
            LamaPon::RenderingApi::DirectX12Experimental,
            d3d12SceneTransitions);
        RequireMatchingSceneTransitionCaptures(
            d3d11SceneTransitions,
            d3d12SceneTransitions);
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
            "mesh cull modes",
            d3d11MeshCull,
            d3d12MeshCull);
        RequireSimilarShapeCells(d3d11BuiltInShapeCull, d3d12BuiltInShapeCull);
        RequireMatchingFrameCaptures(
            "custom particle shaders",
            d3d11CustomParticleShaders,
            d3d12CustomParticleShaders);
        RequireMatchingFrameCaptures(
            "DDS array, volume, and cube array textures",
            d3d11DdsDimensions,
            d3d12DdsDimensions);
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
    // 失敗した検証内容を記録
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    LamaPon::GraphicsDevice::SetEnableDebugLayer(false);
    LamaPon::GraphicsDevice::SetPreferWarpAdapter(false);
    // 必要ならCOMを終了
    if (uninitialize)
    {
        CoUninitialize();
    }
    // processの成功終了codeを返します。
    return result;
}
