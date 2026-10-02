#include "LamaPon/Core/DebugOverlay.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/Log.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Input/InputSystem.h"
#include "LamaPon/Scene/Scene.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>

namespace
{
    // 省略前の本文上限バイト数
    constexpr std::size_t MaximumLineBytes = 96;

    // UTF-8の文字境界で本文を短縮します(text: 表示本文の複製, maximumBytes: 省略前の上限バイト数)。
    // 短縮時は本文の末尾へ3バイトの省略記号を追加します。
    [[nodiscard]] std::string TruncateUtf8(
        std::string text,
        const std::size_t maximumBytes)
    {
        if (text.size() <= maximumBytes)
        {
            return text;
        }
        // UTF-8文字境界に合わせる末尾
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
        // 現在のF1キー押下状態
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
        // 統計行の整形先
        char buffer[160];

        // 描画フレームの計測統計
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

        // シーンの可視判定統計
        const auto& visibility = scene.VisibilityStats();
        // カリングした描画対象の総数
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

        // シーンの物理演算統計
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

        // 直近の診断履歴の複製
        const auto entries = Logger::Instance().Snapshot();
        // 追加済みの警告・エラー行数
        int shown = 0;
        // 新しい順に照合する診断の位置
        for (auto iterator = entries.rbegin();
            iterator != entries.rend() && shown < 4;
            ++iterator)
        {
            if (iterator->level < LogLevel::Warning)
            {
                continue;
            }
            // 表示する診断がエラーか
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

        // 描画資源の世代を固定する借用
        // GPUアップロードをパスの外で完了するため、本文資源を先に準備します。
        [[maybe_unused]] auto operationLease =
            graphics.AcquireResourceLease();

        // 本文のフォントサイズ
        constexpr float FontSize = 15.0f;
        // パネル内の余白ピクセル
        constexpr float Padding = 10.0f;
        // 本文の行間ピクセル
        constexpr float LineGap = 4.0f;
        // 色別のキャッシュを増やさないよう本文を白で生成し、描画時に着色します。
        struct Line final
        {
            // 本文画像の寿命を保持する参照
            std::shared_ptr<const TextTextureAsset> texture;
            // 本文画像の描画ビュー
            GraphicsViewHandle view;
            // 本文へ乗算する表示色
            XMFLOAT4 color{};
        };
        // 本文の描画資源と表示色
        std::vector<Line> textures;
        textures.reserve(m_lines.size());
        // 背景パネルの幅ピクセル
        float panelWidth = 0.0f;
        // 背景パネルの高さピクセル
        float panelHeight = Padding * 2.0f;
        // 資源を準備する表示行
        for (const auto& line : m_lines)
        {
            // 診断の重要度に応じた表示色
            const XMFLOAT4 color =
                line.severity == 2
                    ? XMFLOAT4{ 1.0f, 0.45f, 0.4f, 1.0f }
                    : (line.severity == 1
                        ? XMFLOAT4{
                            1.0f, 0.8f, 0.35f, 1.0f }
                        : XMFLOAT4{
                            0.92f, 0.95f, 1.0f, 1.0f });
            // キャッシュから取得する本文画像
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
            // 本文画像の現在の描画資源
            const auto resources = texture->resources.Acquire();
            textures.push_back(
                Line{
                    std::move(texture),
                    resources
                        ? resources->shaderResourceView
                        : GraphicsViewHandle{},
                    color });
        }
        panelWidth += Padding * 2.0f;
        panelHeight -= LineGap;

        // オーバーレイ用描画パスの設定
        SpritePassDescription description;
        description.blend = SpriteBlendMode::NonPremultiplied;
        // オーバーレイを描くスプライトパス
        auto pass = graphics.BeginSpritePass(description);
        // パス内の描画要求の送信先
        const auto context = pass.Context();
        // 背景の黒と透過率
        const XMFLOAT4 backgroundColor{
            0.0f, 0.0f, 0.0f, 0.68f };
        // 乗算済みアルファの背景色
        const XMFLOAT4 backgroundTint{
            backgroundColor.x * backgroundColor.w,
            backgroundColor.y * backgroundColor.w,
            backgroundColor.z * backgroundColor.w,
            backgroundColor.w };
        // 背景パネルの描画要求
        SpriteDrawRequest backgroundRequest;
        // テクスチャ未指定でバックエンドの白画像を使い、背景色を乗算します。
        backgroundRequest.position = { 6.0f, 6.0f };
        backgroundRequest.scale = { panelWidth, panelHeight };
        backgroundRequest.tint = backgroundTint;
        static_cast<void>(context.Draw(backgroundRequest));

        // 次の本文行のY座標ピクセル
        float y = 6.0f + Padding;
        // 描画する本文の資源と色
        for (const auto& line : textures)
        {
            // 本文資源の欠落や描画失敗では行送りを省略します。
            if (!line.view)
            {
                continue;
            }
            // 本文1行の描画要求
            SpriteDrawRequest request;
            request.texture = line.view;
            request.position = { 6.0f + Padding, y };
            XMStoreFloat4(
                &request.tint,
                PremultipliedTextColor(line.color));
            if (context.Draw(request))
            {
                y += static_cast<float>(line.texture->height)
                    + LineGap;
            }
        }
        pass.End();
    }
}
