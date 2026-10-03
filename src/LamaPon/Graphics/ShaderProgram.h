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

    // 宣言の一つのパスをD3D11のプログラムへ変換する。
    class ShaderProgram final
    {
    public:
        // 各ステージを生成し、成功時だけプログラムを置き換える(device: 借用するD3D11機器, assets: 借用する資産管理器, hlslPath: HLSLパス, pass: コンパイルするパス記述, error: 出力診断, defines: 全ステージのマクロ一覧)。
        // 必須ステージの失敗時は旧資源を保持し、任意ステージの失敗時はその段階だけ省く。
        // ジオメトリシェーダーには三角形入力を要求する。
        [[nodiscard]] bool Compile(
            ID3D11Device* device,
            AssetManager& assets,
            const std::filesystem::path& hlslPath,
            const ShaderPassDesc& pass,
            std::string& error,
            const std::vector<std::string>& defines = {});

        // 保持中の頂点シェーダーを借用する。
        [[nodiscard]] ID3D11VertexShader*
            VertexShader() const noexcept
        {
            return m_vertexShader.Get();
        }
        // 保持中のピクセルシェーダーを借用する。
        [[nodiscard]] ID3D11PixelShader*
            PixelShader() const noexcept
        {
            return m_pixelShader.Get();
        }
        // 保持中のジオメトリシェーダーを借用する。
        [[nodiscard]] ID3D11GeometryShader*
            GeometryShader() const noexcept
        {
            return m_geometryShader.Get();
        }
        // 保持中のハルシェーダーを借用する。
        [[nodiscard]] ID3D11HullShader*
            HullShader() const noexcept
        {
            return m_hullShader.Get();
        }
        // 保持中のドメインシェーダーを借用する。
        [[nodiscard]] ID3D11DomainShader*
            DomainShader() const noexcept
        {
            return m_domainShader.Get();
        }
        // 保持中のcomputeシェーダーを借用する。
        [[nodiscard]] ID3D11ComputeShader*
            ComputeShader() const noexcept
        {
            return m_computeShader.Get();
        }
        // 頂点レイアウト作成用のバイトコードを借用する。
        [[nodiscard]] ID3DBlob*
            VertexShaderByteCode() const noexcept
        {
            return m_vertexShaderByteCode.Get();
        }

    private:
        // 所有する頂点シェーダー
        Microsoft::WRL::ComPtr<ID3D11VertexShader>
            m_vertexShader;
        // 所有するピクセルシェーダー
        Microsoft::WRL::ComPtr<ID3D11PixelShader>
            m_pixelShader;
        // 所有するジオメトリシェーダー
        Microsoft::WRL::ComPtr<ID3D11GeometryShader>
            m_geometryShader;
        // 所有するハルシェーダー
        Microsoft::WRL::ComPtr<ID3D11HullShader>
            m_hullShader;
        // 所有するドメインシェーダー
        Microsoft::WRL::ComPtr<ID3D11DomainShader>
            m_domainShader;
        // 所有するcomputeシェーダー
        Microsoft::WRL::ComPtr<ID3D11ComputeShader>
            m_computeShader;
        // 所有する頂点シェーダーのバイト列
        Microsoft::WRL::ComPtr<ID3DBlob>
            m_vertexShaderByteCode;
    };
}
