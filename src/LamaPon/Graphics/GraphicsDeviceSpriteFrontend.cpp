#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceState.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Scene/SceneManager.h"
#include "LamaPon/Scene/SceneTransition.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
    // RGBへアルファを事前乗算します(color: 元のRGBA色)。
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

    // アルファだけを乗算します(color: 元のRGBA色, alpha: 追加の透明度係数)。
    [[nodiscard]] DirectX::XMFLOAT4 WithAlpha(
        const DirectX::XMFLOAT4& color,
        const float alpha) noexcept
    {
        return { color.x, color.y, color.z, color.w * alpha };
    }

    // 白画像で矩形を描きます(pass: 送信先, x: 左端の画素位置, y: 上端の画素位置, width: 矩形幅, height: 矩形高, color: 事前乗算前のRGBA色)。
    void DrawRectangle(
        LamaPon::SpriteRenderPass& pass,
        const float x,
        const float y,
        const float width,
        const float height,
        const DirectX::XMFLOAT4& color)
    {
        // 送信する画像の描画指定
        LamaPon::SpriteDrawRequest request;
        request.position = { x, y };
        request.scale = {
            std::max(width, 0.0f),
            std::max(height, 0.0f) };
        request.tint = Premultiplied(color);
        static_cast<void>(pass.Draw(request));
    }


    // 画像のSRVを保持して返し、失敗ならnullptrです(assets: 画像の取得元, path: 画像パス, width: 成功時の画像幅出力, height: 成功時の画像高出力)。
    [[nodiscard]] std::shared_ptr<const LamaPon::TextureResourceSnapshot>
        TryLoadTextureResources(
            LamaPon::AssetManager& assets,
            const std::filesystem::path& path,
            std::uint32_t& width,
            std::uint32_t& height) noexcept
    {
        try
        {
            if (path.empty())
            {
                return nullptr;
            }
            // 取得した画像資産
            const auto texture = assets.LoadTexture(path);
            if (texture == nullptr
                || texture->width == 0
                || texture->height == 0)
            {
                return nullptr;
            }
            // 保持する画像資源の世代
            auto resources = texture->resources.Acquire();
            if (resources == nullptr
                || !resources->shaderResourceView)
            {
                return nullptr;
            }
            width = texture->width;
            height = texture->height;
            return resources;
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }

    // 中央寄せした文字画像を描きます(pass: 送信先, assets: 文字画像の取得元, text: 表示文, fontSize: 文字サイズ, canvasWidth: 画面幅, top: 画像の上端, color: 事前乗算前のRGBA色)。
    void DrawCenteredText(
        LamaPon::SpriteRenderPass& pass,
        LamaPon::AssetManager& assets,
        const std::string& text,
        const float fontSize,
        const float canvasWidth,
        const float top,
        const DirectX::XMFLOAT4& color)
    {
        if (text.empty())
        {
            return;
        }
        // 文字配置の領域幅（画素）
        const float textWidth =
            std::min(canvasWidth * 0.8f, 720.0f);
        // 中央寄せの文字配置設定
        const LamaPon::TextLayoutOptions layout{
            { textWidth, fontSize * 2.1f },
            LamaPon::TextHorizontalAlignment::Center,
            LamaPon::TextVerticalAlignment::Center,
            false
        };
        // 取得した画像資産
        const auto texture = assets.LoadTextTexture(
            text,
            "Yu Gothic UI",
            fontSize,
            layout);
        // 保持する画像資源の世代
        const auto resources = texture != nullptr
            ? texture->resources.Acquire()
            : nullptr;
        if (resources == nullptr
            || !resources->shaderResourceView)
        {
            return;
        }
        // 送信する画像の描画指定
        LamaPon::SpriteDrawRequest request;
        request.texture = resources->shaderResourceView;
        request.position = {
            (canvasWidth - static_cast<float>(texture->width)) * 0.5f,
            top
        };

        request.tint = Premultiplied(color);
        static_cast<void>(pass.Draw(request));
    }


    // 背景・進捗・回転表示を描きます(pass: 送信先, assets: 画像の取得元, progress: 0～1の進捗, settings: 読み込み画面の設定, alpha: 全体の透明度, elapsedSeconds: 回転表示の経過秒, canvasWidth: 画面幅, canvasHeight: 画面高)。
    void DrawLoadingOverlay(
        LamaPon::SpriteRenderPass& pass,
        LamaPon::AssetManager& assets,
        const float progress,
        const LamaPon::SceneLoadingScreenSettings& settings,
        const float alpha,
        const float elapsedSeconds,
        const float canvasWidth,
        const float canvasHeight)
    {
        DrawRectangle(
            pass,
            0.0f,
            0.0f,
            canvasWidth,
            canvasHeight,
            WithAlpha(settings.backgroundColor, alpha));

        // 背景画像の幅（画素）
        std::uint32_t imageWidth{};
        // 背景画像の高（画素）
        std::uint32_t imageHeight{};
        // 背景画像の保持資源
        if (const auto image = TryLoadTextureResources(
                assets,
                settings.backgroundTexture,
                imageWidth,
                imageHeight))
        {

            // 縦横比を保つ画像の拡大率
            const float scale = std::max(
                canvasWidth / static_cast<float>(imageWidth),
                canvasHeight / static_cast<float>(imageHeight));
            // 送信する画像の描画指定
            LamaPon::SpriteDrawRequest request;
            request.texture = image->shaderResourceView;
            request.position = {
                (canvasWidth - static_cast<float>(imageWidth) * scale)
                    * 0.5f,
                (canvasHeight - static_cast<float>(imageHeight) * scale)
                    * 0.5f
            };
            request.scale = { scale, scale };
            request.tint = Premultiplied({ 1.0f, 1.0f, 1.0f, alpha });
            static_cast<void>(pass.Draw(request));
        }

        // 進捗バーの幅（画素）
        const float barWidth =
            std::min(
                std::clamp(
                    canvasWidth * 0.58f,
                    240.0f,
                    760.0f),
                canvasWidth * 0.9f);
        // 進捗バーの高（画素）
        const float barHeight = 18.0f;
        // 進捗バーの左端（画素）
        const float barX = (canvasWidth - barWidth) * 0.5f;
        // 進捗バーの上端（画素）
        const float barY = canvasHeight * 0.62f;
        DrawRectangle(
            pass,
            barX,
            barY,
            barWidth,
            barHeight,
            WithAlpha(settings.barBackgroundColor, alpha));
        DrawRectangle(
            pass,
            barX,
            barY,
            barWidth * std::clamp(progress, 0.0f, 1.0f),
            barHeight,
            WithAlpha(settings.barFillColor, alpha));

        // 割合を付けた進捗の表示文
        std::string label = settings.message;
        if (settings.showPercentage)
        {
            label += " ";
            label += std::to_string(
                static_cast<int>(
                    std::lround(
                        std::clamp(progress, 0.0f, 1.0f)
                        * 100.0f)));
            label += "%";
        }
        // 透明度を反映した文字色
        const auto textColor = WithAlpha(settings.textColor, alpha);
        {
            // 文字配置の領域幅（画素）
            const float textWidth =
                std::min(canvasWidth * 0.8f, 720.0f);
            // 中央寄せの文字配置設定
            const LamaPon::TextLayoutOptions layout{
                { textWidth, 64.0f },
                LamaPon::TextHorizontalAlignment::Center,
                LamaPon::TextVerticalAlignment::Center,
                false
            };
            // 進捗表示文の画像資産
            const auto text = assets.LoadTextTexture(
                label,
                "Yu Gothic UI",
                30.0f,
                layout);
            // 進捗文字画像の保持資源
            const auto textResources = text != nullptr
                ? text->resources.Acquire()
                : nullptr;
            if (textResources != nullptr
                && textResources->shaderResourceView)
            {
                // 送信する画像の描画指定
                LamaPon::SpriteDrawRequest request;
                request.texture =
                    textResources->shaderResourceView;
                request.position = {
                    (canvasWidth - static_cast<float>(text->width))
                        * 0.5f,
                    barY - 86.0f
                };

                request.tint = Premultiplied(textColor);
                static_cast<void>(pass.Draw(request));
            }
        }

        DrawCenteredText(
            pass,
            assets,
            settings.hint,
            20.0f,
            canvasWidth,
            barY + barHeight + 18.0f,
            WithAlpha(settings.textColor, alpha * 0.8f));

        if (!settings.showSpinner)
        {
            return;
        }

        // 円画像の幅（画素）
        std::uint32_t dotWidth{};
        // 円画像の高（画素）
        std::uint32_t dotHeight{};
        // 円画像の保持資源
        const auto dot = TryLoadTextureResources(
            assets,
            L"builtin/circle",
            dotWidth,
            dotHeight);
        // 回転表示の点数
        constexpr int DotCount = 8;
        // 1周の角度（ラジアン）
        constexpr float TwoPi = 6.28318531f;
        // 点の回転半径（画素）
        const float radius = 16.0f;
        // 点の表示直径（画素）
        const float dotSize = 7.0f;
        // 回転中心の画面x座標
        const float centerX = canvasWidth - 48.0f;
        // 回転中心の画面y座標
        const float centerY = canvasHeight - 48.0f;
        // 明るい先頭位置の周回数
        const float head = elapsedSeconds * 1.25f;
        // 回転表示の点番号
        for (int index{}; index < DotCount; ++index)
        {
            // 点の円周上の割合
            const float slot =
                static_cast<float>(index) / static_cast<float>(DotCount);
            // 点の表示角（ラジアン）
            const float angle = slot * TwoPi - TwoPi * 0.25f;

            // 先頭からの周回差
            float trail = head - slot;
            trail -= std::floor(trail);
            // 当該点の表示透明度
            const float dotAlpha = alpha * (1.0f - trail * 0.8f);
            // 画像や点の画面x座標
            const float x = centerX + std::cos(angle) * radius;
            // 画像や点の画面y座標
            const float y = centerY + std::sin(angle) * radius;
            // 送信する画像の描画指定
            LamaPon::SpriteDrawRequest request;
            if (dot != nullptr)
            {
                // builtin/circleの円半径は画像の一辺の112/256なので、指定直径に合う矩形寸法へ補正します。
                // 円画像の半径と一辺の比
                constexpr float CircleRadiusRatio = 112.0f / 256.0f;
                // 指定直径に合う画像の一辺
                const float quad = dotSize / (2.0f * CircleRadiusRatio);
                request.texture = dot->shaderResourceView;
                request.position = { x - quad * 0.5f, y - quad * 0.5f };
                request.scale = {
                    quad / static_cast<float>(dotWidth),
                    quad / static_cast<float>(dotHeight) };
            }
            else
            {
                request.position = {
                    x - dotSize * 0.5f,
                    y - dotSize * 0.5f };
                request.scale = { dotSize, dotSize };
            }
            request.tint = Premultiplied(
                WithAlpha(settings.textColor, dotAlpha));
            static_cast<void>(pass.Draw(request));
        }
    }
}

namespace LamaPon
{
    // 設定された読み込み画面を描きます(progress: 0～1の進捗, settings: 表示設定, width: 画面幅で0は現在値, height: 画面高で0は現在値)。
    void GraphicsDevice::DrawLoadingScreen(
        const float progress,
        const SceneLoadingScreenSettings& settings,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (!settings.enabled)
        {
            return;
        }

        // 画像の取得準備から描画終了までリースを保持し、資源世代の混在を防ぎます。
        // 再初期化を防ぐ操作リース
        [[maybe_unused]] auto operationLease =
            AcquireResourceLease();
        // 使用する画面幅（画素）
        const std::uint32_t canvasWidth =
            width == 0 ? m_state->m_width : width;
        // 使用する画面高（画素）
        const std::uint32_t canvasHeight =
            height == 0 ? m_state->m_height : height;
        // 送信先のスプライトパス
        auto pass = BeginSpritePass();
        DrawLoadingOverlay(
            pass,
            Assets(),
            progress,
            settings,
            1.0f,
            0.0f,
            static_cast<float>(canvasWidth),
            static_cast<float>(canvasHeight));
        pass.End();
    }

    // 遷移状態に応じて読み込み画面を描きます(frame: 進捗・透明度・経過時間, settings: 表示設定, width: 画面幅で0は現在値, height: 画面高で0は現在値)。
    void GraphicsDevice::DrawLoadingScreen(
        const SceneTransitionFrame& frame,
        const SceneLoadingScreenSettings& settings,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        // 遷移状態から求めた透明度
        const float alpha = frame.legacyLoadingScreen
            ? (frame.loadingScreenAlpha > 0.0f ? 1.0f : 0.0f)
            : std::clamp(frame.loadingScreenAlpha, 0.0f, 1.0f);
        if (!settings.enabled || alpha <= 0.0f)
        {
            return;
        }

        // 再初期化を防ぐ操作リース
        [[maybe_unused]] auto operationLease =
            AcquireResourceLease();
        // 使用する画面幅（画素）
        const std::uint32_t canvasWidth =
            width == 0 ? m_state->m_width : width;
        // 使用する画面高（画素）
        const std::uint32_t canvasHeight =
            height == 0 ? m_state->m_height : height;
        // 送信先のスプライトパス
        auto pass = BeginSpritePass();
        DrawLoadingOverlay(
            pass,
            Assets(),
            frame.loadingProgress,
            settings,
            alpha,
            frame.loadingScreenTime,
            static_cast<float>(canvasWidth),
            static_cast<float>(canvasHeight));
        pass.End();
    }

    // 縦横比を保って起動ロゴを描きます(logoPath: 画像パス, width: 画面幅で0は現在値, height: 画面高で0は現在値)。
    void GraphicsDevice::DrawStartupLogo(
        const std::filesystem::path& logoPath,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        if (logoPath.empty())
        {
            return;
        }

        // 画像の存在確認から描画終了までリースを保持し、資源世代の混在を防ぎます。
        // 再初期化を防ぐ操作リース
        [[maybe_unused]] auto operationLease =
            AcquireResourceLease();
        if (!Assets().FileExists(logoPath))
        {
            return;
        }

        // 起動ロゴの画像資産
        std::shared_ptr<const TextureAsset> logo;
        try
        {

            logo = Assets().LoadTexture(logoPath);
        }
        catch (const std::exception&)
        {
            return;
        }
        // 起動ロゴの保持資源
        const auto logoResources = logo != nullptr
            ? logo->resources.Acquire()
            : nullptr;
        if (logoResources == nullptr
            || !logoResources->shaderResourceView
            || logo->width == 0
            || logo->height == 0)
        {
            return;
        }

        // 使用する画面幅（画素）
        const std::uint32_t canvasWidth =
            width == 0 ? m_state->m_width : width;
        // 使用する画面高（画素）
        const std::uint32_t canvasHeight =
            height == 0 ? m_state->m_height : height;
        // ロゴの表示上限幅（画素）
        const float maximumWidth = std::min(
            static_cast<float>(canvasWidth) * 0.32f,
            360.0f);
        // ロゴの表示上限高（画素）
        const float maximumHeight = std::min(
            static_cast<float>(canvasHeight) * 0.42f,
            360.0f);
        // 縦横比を保つ画像の拡大率
        const float scale = std::min(
            maximumWidth / static_cast<float>(logo->width),
            maximumHeight / static_cast<float>(logo->height));
        // ロゴの表示幅（画素）
        const float drawWidth =
            static_cast<float>(logo->width) * scale;
        // ロゴの表示高（画素）
        const float drawHeight =
            static_cast<float>(logo->height) * scale;
        // 画像や点の画面x座標
        const float x =
            (static_cast<float>(canvasWidth) - drawWidth) * 0.5f;
        // 画像や点の画面y座標
        const float y =
            static_cast<float>(canvasHeight) * 0.30f
            - drawHeight * 0.5f;

        // 送信する画像の描画指定
        SpriteDrawRequest request;
        request.texture =
            logoResources->shaderResourceView;
        request.position = { x, y };
        request.scale = { scale, scale };
        // 送信先のスプライトパス
        auto pass = BeginSpritePass();
        static_cast<void>(pass.Draw(request));
        pass.End();
    }

    // 2D描画の共通オフセットを設定します(offset: 画面上の移動量)。
    void GraphicsDevice::SetSprite2DOffset(
        const DirectX::XMFLOAT2& offset) noexcept
    {
        m_state->m_sprite2DOffset = offset;
    }

    // 2D描画の共通オフセットを参照します。
    const DirectX::XMFLOAT2& GraphicsDevice::Sprite2DOffset() const noexcept
    {
        return m_state->m_sprite2DOffset;
    }
}
