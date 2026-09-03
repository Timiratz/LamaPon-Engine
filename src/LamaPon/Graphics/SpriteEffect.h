#pragma once

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <filesystem>

namespace LamaPon
{
    class AssetManager;

    // 画面へ同時に効かせられるLight2Dの数。b1の定数バッファに載せる
    // ので、CustomParameters（b0・8本しかなく、自作Shaderの持ち物）を
    // 圧迫しません。
    inline constexpr std::size_t MaximumSprite2DLights = 16;

    struct Sprite2DLight final
    {
        // xy=画面ピクセル座標, z=届く半径, w=強さ。
        DirectX::XMFLOAT4 positionRadiusIntensity{};
        // rgb=色, w=予約。
        DirectX::XMFLOAT4 color{};
    };

    // 組み込みの2D照明Shader（LamaPonSpriteLit.hlsl）が読む灯り一覧。
    // 自作Shaderは宣言しなければ何の影響も受けません。
    struct Sprite2DLighting final
    {
        // x=灯数, yzw=予約。
        DirectX::XMUINT4 counts{};
        std::array<Sprite2DLight, MaximumSprite2DLights>
            lights{};
    };

    // SpriteBatch が生成した頂点はそのまま使用し、ピクセル処理だけを
    // プロジェクト側の HLSL に差し替えるためのエフェクトです。
    class SpriteEffect final
    {
    public:
        static constexpr std::size_t CustomParameterCount = 8;
        // Parameter 6 はSprite tint、Parameter 7 は描画矩形
        // (left, top, width, height)、Parameter 8 はUI viewport
        // (width, height, texture width, texture height)として
        // エンジンが描画時に設定します。
        using CustomParameters = std::array<
            DirectX::XMFLOAT4,
            CustomParameterCount>;

        SpriteEffect(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            AssetManager& assets,
            const std::filesystem::path& shaderPath);

        void SetParameters(
            const CustomParameters& parameters) noexcept;
        // b1へ載せるLight2Dの一覧。組み込みの2D照明Shaderだけが
        // 読みますが、バインド自体は常に行います（前の描画の内容が
        // 残ったまま読まれるのを防ぐため）。
        void SetLights(
            const Sprite2DLighting& lighting) noexcept;
        void Apply();

    private:
        struct Constants final
        {
            CustomParameters parameters{};
        };

        ID3D11DeviceContext* m_context{};
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_pixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_constantBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer>
            m_lightBuffer;
        Constants m_constants;
        Sprite2DLighting m_lighting{};
    };
}
