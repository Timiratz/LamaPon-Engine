#include "LamaPon/Core/DebugOverlay.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/Scene.h"

#include <SpriteBatch.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>

namespace
{
    // 1行の最大文字数（テキストテクスチャの肥大化を防ぎます）。
    constexpr std::size_t MaximumLineBytes = 96;

    // UTF-8の文字境界を壊さないように切り詰めます。
    [[nodiscard]] std::string TruncateUtf8(
        std::string text,
        const std::size_t maximumBytes)
    {
        if (text.size() <= maximumBytes)
        {
            return text;
        }
        std::size_t end = maximumBytes;
        while (end > 0
            && (static_cast<unsigned char>(text[end])
                & 0xc0) == 0x80)
        {
            --end;
        }
        text.resize(end);
        text += "...";
        return text;
    }
}

namespace LamaPon
{
    void DebugOverlay::Update(
        GraphicsDevice& graphics,
        Scene& scene,
        const float deltaTime)
    {
        const bool pressed =
            graphics.Input().KeyboardState().F1;
        if (pressed && !m_toggleHeld)
        {
            m_visible = !m_visible;
            // 開いた直後に最新の内容を出します。
            m_refreshTimer = 1.0f;
        }
        m_toggleHeld = pressed;
        if (!m_visible)
        {
            return;
        }

        try
        {
            m_refreshTimer += deltaTime;
            if (m_refreshTimer >= 0.3f)
            {
                m_refreshTimer = 0.0f;
                RefreshLines(graphics, scene);
            }
            Draw(graphics);
        }
        catch (const std::exception&)
        {
            // オーバーレイの失敗でゲームを止めないようにします。
            m_visible = false;
        }
    }

    void DebugOverlay::RefreshLines(
        GraphicsDevice& graphics,
        Scene& scene)
    {
        m_lines.clear();
        char buffer[160];

        const auto& frame = graphics.FrameStats();
        std::snprintf(
            buffer,
            sizeof(buffer),
            "FPS %.0f   Frame %.2f ms   CPU %.2f ms",
            static_cast<double>(frame.framesPerSecond),
            static_cast<double>(
                frame.frameTimeMilliseconds),
            static_cast<double>(
                frame.cpuTimeMilliseconds));
        m_lines.push_back({ buffer, 0 });

        if (graphics.Gpu().IsSupported())
        {
            std::snprintf(
                buffer,
                sizeof(buffer),
                "GPU %.2f ms",
                static_cast<double>(
                    graphics.Gpu()
                        .LatestFrameMilliseconds()));
            m_lines.push_back({ buffer, 0 });
        }

        const auto& visibility = scene.VisibilityStats();
        const std::size_t culled =
            visibility.frustumCulledCount
            + visibility.occlusionCulledCount
            + visibility.lodCulledCount;
        std::snprintf(
            buffer,
            sizeof(buffer),
            "GameObject %zu   描画 %zu/%zu（カリング %zu）",
            scene.GameObjects().size(),
            visibility.visibleRendererCount,
            visibility.rendererCount,
            culled);
        m_lines.push_back({ buffer, 0 });

        const auto& physics = scene.PhysicsStats();
        std::snprintf(
            buffer,
            sizeof(buffer),
            "Collider 3D %zu / 2D %zu   接触候補 %zu",
            physics.colliderCount3D,
            physics.colliderCount2D,
            physics.candidatePairCount3D
                + physics.candidatePairCount2D);
        m_lines.push_back({ buffer, 0 });

        // 直近の警告・エラー（新しい順に最大4件）。
        const auto entries = Logger::Instance().Snapshot();
        int shown = 0;
        for (auto iterator = entries.rbegin();
            iterator != entries.rend() && shown < 4;
            ++iterator)
        {
            if (iterator->level < LogLevel::Warning)
            {
                continue;
            }
            const bool isError =
                iterator->level == LogLevel::Error;
            m_lines.push_back({
                TruncateUtf8(
                    (isError ? "[E] " : "[W] ")
                        + iterator->message,
                    MaximumLineBytes),
                isError ? 2 : 1 });
            ++shown;
        }

        m_lines.push_back({ "F1で閉じる", 0 });
    }

    void DebugOverlay::Draw(GraphicsDevice& graphics)
    {
        using namespace DirectX;
        if (m_lines.empty())
        {
            return;
        }

        // 先にテキストテクスチャを揃えてパネルサイズを決めます
        // （文字列単位でAssetManagerがキャッシュします）。
        constexpr float FontSize = 15.0f;
        constexpr float Padding = 10.0f;
        constexpr float LineGap = 4.0f;
        // 文字テクスチャは白で焼かれるので、行ごとの色は描くときに
        // 掛けます（色を焼くと、同じ文だけ色違いでもテクスチャが
        // 増えてしまいます）。
        struct Line final
        {
            std::shared_ptr<const TextTextureAsset> texture;
            XMFLOAT4 color{};
        };
        std::vector<Line> textures;
        textures.reserve(m_lines.size());
        float panelWidth = 0.0f;
        float panelHeight = Padding * 2.0f;
        for (const auto& line : m_lines)
        {
            const XMFLOAT4 color =
                line.severity == 2
                    ? XMFLOAT4{ 1.0f, 0.45f, 0.4f, 1.0f }
                    : (line.severity == 1
                        ? XMFLOAT4{
                            1.0f, 0.8f, 0.35f, 1.0f }
                        : XMFLOAT4{
                            0.92f, 0.95f, 1.0f, 1.0f });
            auto texture =
                graphics.Assets().LoadTextTexture(
                    line.text,
                    {},
                    FontSize);
            panelWidth = std::max(
                panelWidth,
                static_cast<float>(texture->width));
            panelHeight +=
                static_cast<float>(texture->height)
                + LineGap;
            textures.push_back(
                Line{ std::move(texture), color });
        }
        panelWidth += Padding * 2.0f;
        panelHeight -= LineGap;

        auto& sprites = graphics.BeginSprites();
        const XMFLOAT4 backgroundColor{
            0.0f, 0.0f, 0.0f, 0.68f };
        const XMFLOAT4 backgroundTint{
            backgroundColor.x * backgroundColor.w,
            backgroundColor.y * backgroundColor.w,
            backgroundColor.z * backgroundColor.w,
            backgroundColor.w };
        sprites.Draw(
            graphics.WhiteTexture(),
            XMFLOAT2{ 6.0f, 6.0f },
            nullptr,
            XMLoadFloat4(&backgroundTint),
            0.0f,
            XMFLOAT2{},
            XMFLOAT2{ panelWidth, panelHeight });

        float y = 6.0f + Padding;
        for (const auto& line : textures)
        {
            sprites.Draw(
                line.texture->view.Get(),
                XMFLOAT2{ 6.0f + Padding, y },
                nullptr,
                PremultipliedTextColor(line.color));
            y += static_cast<float>(line.texture->height)
                + LineGap;
        }
        graphics.EndSprites();
    }
}
