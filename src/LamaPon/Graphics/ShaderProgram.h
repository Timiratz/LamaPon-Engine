#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>
#include <vector>

namespace LamaPon
{
    class AssetManager;
    struct ShaderPassDesc;

    // Manifestの1 passをD3D11のShader Objectへ変換する共通部品です。
    // Shader定義自体からD3D11型を切り離し、Backend固有の生成処理を
    // このクラスへ閉じ込めます。
    class ShaderProgram final
    {
    public:
        // 必須stageのコンパイルまたはShader Object生成に失敗した場合は
        // falseを返し、errorへ原因を格納します。optional stageの失敗は
        // そのstageを未設定のままスキップします。definesは全stageへ
        // 同じShader variant keywordとして渡されます。
        [[nodiscard]] bool Compile(
            ID3D11Device* device,
            AssetManager& assets,
            const std::filesystem::path& hlslPath,
            const ShaderPassDesc& pass,
            std::string& error,
            const std::vector<std::string>& defines = {});

        [[nodiscard]] ID3D11VertexShader*
            VertexShader() const noexcept
        {
            return m_vertexShader.Get();
        }
        [[nodiscard]] ID3D11PixelShader*
            PixelShader() const noexcept
        {
            return m_pixelShader.Get();
        }
        [[nodiscard]] ID3D11GeometryShader*
            GeometryShader() const noexcept
        {
            return m_geometryShader.Get();
        }
        [[nodiscard]] ID3D11HullShader*
            HullShader() const noexcept
        {
            return m_hullShader.Get();
        }
        [[nodiscard]] ID3D11DomainShader*
            DomainShader() const noexcept
        {
            return m_domainShader.Get();
        }
        [[nodiscard]] ID3D11ComputeShader*
            ComputeShader() const noexcept
        {
            return m_computeShader.Get();
        }
        // Input Layoutを作る描画経路向けに、main vertex stageの
        // コンパイル済みバイトコードを保持します。
        [[nodiscard]] ID3DBlob*
            VertexShaderByteCode() const noexcept
        {
            return m_vertexShaderByteCode.Get();
        }

    private:
        Microsoft::WRL::ComPtr<ID3D11VertexShader>
            m_vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_pixelShader;
        Microsoft::WRL::ComPtr<ID3D11GeometryShader>
            m_geometryShader;
        Microsoft::WRL::ComPtr<ID3D11HullShader>
            m_hullShader;
        Microsoft::WRL::ComPtr<ID3D11DomainShader>
            m_domainShader;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader>
            m_computeShader;
        Microsoft::WRL::ComPtr<ID3DBlob>
            m_vertexShaderByteCode;
    };
}
