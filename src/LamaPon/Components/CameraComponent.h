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
        // 右手系の透視カメラを設定します(verticalFieldOfView: 0〜πの間の縦視野角ラジアン, nearPlane: 正の近平面距離, farPlane: 近平面より遠い遠平面距離)。
        // 値は検証しないため呼び出し側で有限な値を渡します。
        explicit CameraComponent(
            float verticalFieldOfView = DirectX::XM_PIDIV4,
            float nearPlane = 0.1f,
            float farPlane = 1000.0f) noexcept;

        // 所有物体の-Zを前向き、+Yを上向きにした右手系ビュー行列を返します。
        // ワールド変換のY・Z軸は非零かつ平行でない必要があります。
        [[nodiscard]] DirectX::XMMATRIX ViewMatrix() const noexcept;
        // 保存した視野角と深度範囲から右手系透視行列を返します(aspectRatio: 有限で正の幅高さ比)。
        [[nodiscard]] DirectX::XMMATRIX ProjectionMatrix(float aspectRatio) const noexcept;
        // 保存と表示に使うコンポーネント型名Cameraを返します。
        [[nodiscard]] std::string_view TypeName() const noexcept override { return "Camera"; }

        // 縦視野角をラジアンで返します。
        [[nodiscard]] float VerticalFieldOfView() const noexcept { return m_verticalFieldOfView; }
        // 検証せず縦視野角を保存します(radians: 有限で0〜πの間のラジアン角)。
        void SetVerticalFieldOfView(float radians) noexcept { m_verticalFieldOfView = radians; }
        // 近平面の距離を返します。
        [[nodiscard]] float NearPlane() const noexcept { return m_nearPlane; }
        // 近平面の距離を検証せず設定します(nearPlane: 近平面距離)。
        void SetNearPlane(float nearPlane) noexcept { m_nearPlane = nearPlane; }
        // 遠平面の距離を返します。
        [[nodiscard]] float FarPlane() const noexcept { return m_farPlane; }
        // 遠平面の距離を検証せず設定します(farPlane: 遠平面距離)。
        void SetFarPlane(float farPlane) noexcept { m_farPlane = farPlane; }

        // 追加の描画先テクスチャ名を設定し、空名ならテクスチャ描画を解除します(name: 描画先の共有名)。
        // スプライトやUI画像に同じRender Texture名を指定すると結果を表示できます。
        void SetTargetTexture(std::string name)
        {
            m_targetTexture = std::move(name);
        }
        // 描画先テクスチャの共有名への読み取り参照を返します。
        [[nodiscard]] const std::string&
            TargetTexture() const noexcept
        {
            return m_targetTexture;
        }
        // 描画先テクスチャ名が設定されているか返します。
        [[nodiscard]] bool RendersToTexture() const noexcept
        {
            return !m_targetTexture.empty();
        }
        // 描画先の解像度を保存し、0は1に直します(width: 横ピクセル数, height: 縦ピクセル数)。
        void SetTargetTextureSize(
            std::uint32_t width,
            std::uint32_t height) noexcept
        {
            m_targetTextureWidth = width == 0 ? 1 : width;
            m_targetTextureHeight = height == 0 ? 1 : height;
        }
        // 描画先の横ピクセル数を返します。
        [[nodiscard]] std::uint32_t
            TargetTextureWidth() const noexcept
        {
            return m_targetTextureWidth;
        }
        // 描画先の縦ピクセル数を返します。
        [[nodiscard]] std::uint32_t
            TargetTextureHeight() const noexcept
        {
            return m_targetTextureHeight;
        }
        // 描画先テクスチャの背景RGBAを保存します(color: 背景色)。
        void SetTargetClearColor(
            const DirectX::XMFLOAT4& color) noexcept
        {
            m_targetClearColor = color;
        }
        // 描画先テクスチャの背景RGBAへの参照を返します。
        [[nodiscard]] const DirectX::XMFLOAT4&
            TargetClearColor() const noexcept
        {
            return m_targetClearColor;
        }

    private:
        // 縦視野角のラジアン値
        float m_verticalFieldOfView;
        // カメラの近平面距離
        float m_nearPlane;
        // カメラの遠平面距離
        float m_farPlane;
        // 描画先テクスチャの共有名
        std::string m_targetTexture;
        // 描画先の横ピクセル数
        std::uint32_t m_targetTextureWidth{ 512 };
        // 描画先の縦ピクセル数
        std::uint32_t m_targetTextureHeight{ 512 };
        // 描画先の背景RGBA
        DirectX::XMFLOAT4 m_targetClearColor{
            0.05f,
            0.06f,
            0.08f,
            1.0f
        };
    };
}
