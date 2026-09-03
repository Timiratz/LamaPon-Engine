#pragma once

#include "LamaPon/Scene/Component.h"

#include <cstdint>
#include <string>
#include <utility>

namespace LamaPon
{
    class CameraComponent final : public Component
    {
    public:
        explicit CameraComponent(
            float verticalFieldOfView = DirectX::XM_PIDIV4,
            float nearPlane = 0.1f,
            float farPlane = 1000.0f) noexcept;

        [[nodiscard]] DirectX::XMMATRIX ViewMatrix() const noexcept;
        [[nodiscard]] DirectX::XMMATRIX ProjectionMatrix(float aspectRatio) const noexcept;
        [[nodiscard]] std::string_view TypeName() const noexcept override { return "Camera"; }

        [[nodiscard]] float VerticalFieldOfView() const noexcept { return m_verticalFieldOfView; }
        void SetVerticalFieldOfView(float radians) noexcept { m_verticalFieldOfView = radians; }
        [[nodiscard]] float NearPlane() const noexcept { return m_nearPlane; }
        [[nodiscard]] float FarPlane() const noexcept { return m_farPlane; }

        // 描画先のレンダーテクスチャ名。空なら通常どおり画面へ
        // 描きます。名前を付けると毎フレームその名前のテクスチャへ
        // 描画され、SpriteRendererやUI Imageの「Render Texture」に
        // 同じ名前を指定して画面へ出せます（ミニマップ、防犯カメラ、
        // キャラクターアイコンなど）。
        void SetTargetTexture(std::string name)
        {
            m_targetTexture = std::move(name);
        }
        [[nodiscard]] const std::string&
            TargetTexture() const noexcept
        {
            return m_targetTexture;
        }
        [[nodiscard]] bool RendersToTexture() const noexcept
        {
            return !m_targetTexture.empty();
        }
        // レンダーテクスチャの解像度（ピクセル）。小さいほど軽い
        // です。既定は512x512です。
        void SetTargetTextureSize(
            std::uint32_t width,
            std::uint32_t height) noexcept
        {
            m_targetTextureWidth = width == 0 ? 1 : width;
            m_targetTextureHeight = height == 0 ? 1 : height;
        }
        [[nodiscard]] std::uint32_t
            TargetTextureWidth() const noexcept
        {
            return m_targetTextureWidth;
        }
        [[nodiscard]] std::uint32_t
            TargetTextureHeight() const noexcept
        {
            return m_targetTextureHeight;
        }
        // レンダーテクスチャを毎フレーム塗りつぶす色です。
        // アルファを0にすると背景が透明なテクスチャになります。
        void SetTargetClearColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_targetClearColor = color;
        }
        [[nodiscard]] const DirectX::XMFLOAT4&
            TargetClearColor() const noexcept
        {
            return m_targetClearColor;
        }

    private:
        float m_verticalFieldOfView;
        float m_nearPlane;
        float m_farPlane;
        std::string m_targetTexture;
        std::uint32_t m_targetTextureWidth{ 512 };
        std::uint32_t m_targetTextureHeight{ 512 };
        DirectX::XMFLOAT4 m_targetClearColor{
            0.05f,
            0.06f,
            0.08f,
            1.0f
        };
    };
}
