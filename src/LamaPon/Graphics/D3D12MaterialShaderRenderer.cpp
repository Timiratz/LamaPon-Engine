#include "LamaPon/Graphics/D3D12MaterialShaderRenderer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Graphics/ClusteredLights.h"
#include "LamaPon/Graphics/D3D12Backend.h"
#include "LamaPon/Graphics/D3D12EnvironmentPrefilter.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/LitMaterial.h"
#include "LamaPon/Graphics/ShaderCompiler.h"

#include <DirectXMath.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    using LamaPon::LitMaterial;
    using LamaPon::PrimitiveRenderVertex;
    using LamaPon::ShaderBlendMode;
    using LamaPon::ShaderCullMode;

    // マテリアルの定数バッファ数
    constexpr std::size_t ConstantBufferCount = 4u;
    // マテリアルの読み取り枠数
    constexpr std::size_t TextureSlotCount = 26u;

    // b0の432バイトの配置をLamaPonLit.hlslと一致させます。
    struct ObjectConstants final
    {
        // ワールド変換行列
        DirectX::XMFLOAT4X4 world{};
        // ビューと射影の合成行列
        DirectX::XMFLOAT4X4 viewProjection{};
        // 法線用の逆転置行列
        DirectX::XMFLOAT4X4 worldInverseTranspose{};
        // マテリアルのRGBA色
        DirectX::XMFLOAT4 materialColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        // カメラのワールド位置
        DirectX::XMFLOAT4 cameraPosition{};
        // カメラのワールド前方
        DirectX::XMFLOAT4 cameraForward{};
        // 粗さ・法線強度有無・金属度
        DirectX::XMFLOAT4 materialParameters{ 0.5f, 1.0f, 0.0f, 0.0f };
        // 自作シェーダーのパラメーター
        std::array<DirectX::XMFLOAT4, LitMaterial::CustomParameterCount>
            customParameters{};
        // 粗さ・金属・AO有無と強度
        DirectX::XMFLOAT4 materialTextureParameters{ 0.0f, 0.0f, 0.0f, 1.0f };
        // 発光RGBと画像の有無
        DirectX::XMFLOAT4 emissiveParameters{};
        // 循環秒・デルタ秒・フレーム数
        DirectX::XMFLOAT4 timeParameters{};
    };
    static_assert(sizeof(ObjectConstants) == 432u);

    struct DirectionalConstants final
    {
        // 方向XYZと光の強度
        DirectX::XMFLOAT4 directionIntensity{};
        // 光のRGBと太陽の角半径
        DirectX::XMFLOAT4 color{};
    };

    struct PointConstants final
    {
        // 位置XYZと光の範囲
        DirectX::XMFLOAT4 positionRange{};
        // 光のRGBと強度
        DirectX::XMFLOAT4 colorIntensity{};
    };

    struct SpotConstants final
    {
        // 位置XYZと光の範囲
        DirectX::XMFLOAT4 positionRange{};
        // 方向XYZと内側角のcos
        DirectX::XMFLOAT4 directionInnerCosine{};
        // 光のRGBと強度
        DirectX::XMFLOAT4 colorIntensity{};
        // 外側角のcosと影枠番号
        DirectX::XMFLOAT4 outerCosinePadding{};
    };

    // b1の2176バイトの配置をLamaPonLit.hlslと一致させます。
    struct LightingConstants final
    {
        // 強度適用済みの環境光RGB
        DirectX::XMFLOAT4 ambient{};
        // 平行・点・スポット・影の数
        std::array<std::uint32_t, 4> lightCounts{};
        // 平行光の定数列
        std::array<DirectionalConstants, LamaPon::MaximumDirectionalLights>
            directionalLights{};
        // 点光源の定数列
        std::array<PointConstants, LamaPon::MaximumPointLights>
            pointLights{};
        // スポット光の定数列
        std::array<SpotConstants, LamaPon::MaximumSpotLights> spotLights{};
        // カスケード別の影描画行列
        std::array<DirectX::XMFLOAT4X4, LamaPon::MaximumShadowCascades>
            shadowViewProjections{};
        // カスケード別の終端距離
        DirectX::XMFLOAT4 shadowCascadeSplits{};
        // 平行光番号・バイアスと強度
        DirectX::XMFLOAT4 shadowParameters{};
        // 霧のRGB
        DirectX::XMFLOAT4 fogColor{};
        // 霧の開始・終端・密度・有無
        DirectX::XMFLOAT4 fogParameters{};
        // IBL強度・有無・最大ミップ
        DirectX::XMFLOAT4 environmentParameters{};
        // スポット影の描画行列
        std::array<DirectX::XMFLOAT4X4, LamaPon::MaximumSpotShadows>
            spotShadowViewProjections{};
        // スポット影のバイアスと強度
        std::array<DirectX::XMFLOAT4, LamaPon::MaximumSpotShadows>
            spotShadowParameters{};
        // 点光源の番号・バイアス・強度
        DirectX::XMFLOAT4 pointShadowParameters{};
        // 平行・スポット・点影の画素幅
        DirectX::XMFLOAT4 shadowTexelSizes{};
        // 画面寸法の逆数とSSAO有無
        DirectX::XMFLOAT4 screenAmbientOcclusionParameters{};
        // クラスタXYZの分割数と有無
        DirectX::XMFLOAT4 clusteredParameters{};
        // 近遠距離・対数比・ライト上限
        DirectX::XMFLOAT4 clusteredDepthParameters{};
        // 画面寸法の逆数とライト数
        DirectX::XMFLOAT4 clusteredScreenParameters{};
        // 反射補正箱の中心
        DirectX::XMFLOAT4 reflectionBoxCenter{};
        // 反射補正箱の大きさと有無
        DirectX::XMFLOAT4 reflectionBoxParameters{};
        // 第2反射補正箱の中心
        DirectX::XMFLOAT4 reflectionSecondaryBoxCenter{};
        // 第2反射補正箱の大きさと有無
        DirectX::XMFLOAT4 reflectionSecondaryBoxParameters{};
        // 第2反射プローブの混合率
        DirectX::XMFLOAT4 reflectionBlendParameters{};
        // SSR強度・有無・距離・歩数
        DirectX::XMFLOAT4 screenReflectionParameters{};
        // 画面寸法の逆数と射影Z成分
        DirectX::XMFLOAT4 screenReflectionScreen{};
        // SSR厚み・粗さ上限・最大段
        DirectX::XMFLOAT4 screenReflectionQuality{};
        // SSR履歴を描いた合成行列
        DirectX::XMFLOAT4X4 screenReflectionPreviousViewProjection{};
        // GI領域の最小座標と有無
        DirectX::XMFLOAT4 bakedGiVolumeMinimum{};
        // GI領域寸法の逆数と強度
        DirectX::XMFLOAT4 bakedGiInverseSize{};
        // GI格子のXYZ解像度
        DirectX::XMFLOAT4 bakedGiResolution{};
    };
    static_assert(sizeof(LightingConstants) == 2176u);

    // b2はHLSLのfloat4x3と一致する72本の骨行列です。
    struct BoneConstants final
    {
        // 素材b2用の骨変形行列列
        std::array<DirectX::XMFLOAT3X4, 72> transforms{};
    };
    static_assert(sizeof(BoneConstants) == 3456u);

    // b3の自作シェーダー用ベクトルをHLSLと同じ順序で格納します。
    struct CustomVectorConstants final
    {
        // 素材b3用の任意ベクトル列
        std::array<DirectX::XMFLOAT4, LitMaterial::CustomVectorCount>
            vectors{};
    };
    static_assert(sizeof(CustomVectorConstants) == 1024u);

    // b4の配置を内蔵スキニングシェーダーと一致させます。
    struct SkinningConstants final
    {
        // ワールド変換行列
        DirectX::XMFLOAT4X4 world{};
        // 法線用の逆転置行列
        DirectX::XMFLOAT4X4 worldInverseTranspose{};
        // ワールド・ビュー・射影の合成
        DirectX::XMFLOAT4X4 worldViewProjection{};
        // 頂点出力の白色と素材アルファ
        DirectX::XMFLOAT4 diffuseColor{};
        // 互換頂点出力用の霧係数
        DirectX::XMFLOAT4 fogVector{};
        // 互換頂点用の骨変形行列列
        std::array<DirectX::XMFLOAT3X4, 72> bones{};
    };
    static_assert(sizeof(SkinningConstants) == 3680u);

    // b4に対応するルート引数番号
    constexpr UINT SkinningParameter =
        static_cast<UINT>(ConstantBufferCount + TextureSlotCount);

    // 骨変形頂点のバイト間隔
    constexpr std::uint32_t SkinnedVertexStride = 60u;

    // SkinnedEffect互換の頂点変形とPSSkinnedMainの入出力を持ち、素材のb0～b3を避けてb4を使うHLSL
    constexpr char SkinnedVertexShaderSource[] = R"(
// 互換骨変形用のb4定数
cbuffer SkinnedVertexParameters : register(b4)
{
    // ワールド変換行列
    row_major float4x4 World;
    // 法線用の逆転置行列
    row_major float4x4 WorldInverseTranspose;
    // ワールド・ビュー・射影の合成
    row_major float4x4 WorldViewProj;
    // 頂点出力の白色と素材アルファ
    float4 DiffuseColor;
    // 頂点出力用の霧係数
    float4 FogVector;
    // 最大72本の骨変形行列
    float4x3 Bones[72];
};

struct VertexInput
{
    // 変形前の頂点位置
    float4 Position : SV_Position;
    // 変形前の頂点法線
    float3 Normal : NORMAL;
    // 画像を参照するUV座標
    float2 TexCoord : TEXCOORD0;
    // 影響する4本の骨番号
    uint4 Indices : BLENDINDICES0;
    // 4本の骨の変形重み
    float4 Weights : BLENDWEIGHT0;
};

struct VertexOutput
{
    // 画像を参照するUV座標
    float2 TexCoord : TEXCOORD0;
    // ワールド位置XYZと霧の量
    float4 PositionWS : TEXCOORD1;
    // ワールド法線
    float3 NormalWS : TEXCOORD2;
    // 白色RGBと素材アルファ
    float4 Diffuse : COLOR0;
    // クリップ座標の頂点位置
    float4 PositionPS : SV_Position;
};

// 4本の骨で変形し、互換の頂点出力を作ります(input: 頂点と骨番号・重み)。
VertexOutput SkinnedVertexShader(VertexInput input)
{
    // 重みを合成した骨変形行列
    float4x3 skinning = 0;
    // 合成する骨の番号
    for (int index = 0; index < 4; index++)
    {
        skinning += Bones[input.Indices[index]] * input.Weights[index];
    }
    // 骨変形後の同次頂点位置
    float4 position = input.Position;
    position.xyz = mul(input.Position, skinning);
    // 骨変形後の頂点法線
    float3 normal = mul(input.Normal, (float3x3)skinning);

    // 画素段へ渡す頂点出力
    VertexOutput output;
    output.PositionPS = mul(position, WorldViewProj);
    output.PositionWS = float4(
        mul(position, World).xyz,
        saturate(dot(position, FogVector)));
    output.NormalWS = normalize(
        mul(normal, (float3x3)WorldInverseTranspose));
    output.Diffuse = float4(1.0f, 1.0f, 1.0f, DiffuseColor.a);
    output.TexCoord = input.TexCoord;
    return output;
}

// 失敗時のマゼンタを返します(input: 互換性のため受ける未使用頂点)。
float4 SkinnedErrorPixelShader(VertexOutput input) : SV_Target
{
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
)";

    // 埋め込みHLSLをコンパイルし、失敗時は診断付きで送出します(source: HLSL文字列, sourceSize: 文字列のバイト数, sourceName: 診断に使う名前, entryPoint: 実行入口名, target: シェーダーモデル)。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileEmbeddedShader(
        const char* const source,
        const std::size_t sourceSize,
        const char* const sourceName,
        const char* const entryPoint,
        const char* const target)
    {
        // 生成されたシェーダーコード
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        // コンパイル時の診断
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // コンパイルの結果
        const HRESULT result = D3DCompile(
            source,
            sourceSize,
            sourceName,
            nullptr,
            nullptr,
            entryPoint,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            bytecode.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result))
        {
            // 診断を含める例外文面
            std::string message =
                std::string("D3DCompile(") + entryPoint + ") failed";
            if (errors != nullptr && errors->GetBufferSize() != 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        return bytecode;
    }

    enum class BlendKind : std::uint8_t
    {
        Opaque,
        NonPremultiplied,
        AdditivePreservingAlpha,
        Premultiplied,
        // DirectXTKのCommonStates::Additiveです。
        Additive
    };

    enum class DepthKind : std::uint8_t
    {
        Default,
        Read,
        None,
        // 深度を書かず、奥にある画素だけを通します。
        Occluded
    };

    // 同世代の読み取りビューを解決できた入力です。
    struct LightingViews final
    {
        // 平行光の影ビューが有効
        bool directionalShadow{};
        // スポット影ビューが有効
        bool spotShadow{};
        // 点光源の影ビューが有効
        bool pointShadow{};
        // SSAOビューが有効
        bool screenAmbientOcclusion{};
        // SSRの色と深度ビューが有効
        bool screenReflection{};
        // 環境キューブが有効
        bool environment{};
        // 反射と放射照度キューブが有効
        bool prefilteredEnvironment{};
        // Forward+の3本が有効
        bool clustered{};
        // GI係数のRGB体積画像が有効
        bool bakedGlobalIllumination{};
    };

    // 失敗したHRESULTを例外へ変換します(result: APIの戻り値, operation: 例外へ記載する操作名)。
    void ThrowIfFailed(
        const HRESULT result,
        const char* const operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string{ operation }
                + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }

    // 任意の入口をコンパイルし、失敗時は空を返します(assets: シェーダー取得元, path: HLSLのパス, entryPoint: 実行入口名, target: シェーダーモデル, keywords: 有効なキーワード)。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> TryCompileShader(
        LamaPon::AssetManager& assets,
        const std::filesystem::path& path,
        const char* const entryPoint,
        const char* const target,
        const std::vector<std::string>& keywords) noexcept
    {
        try
        {
            return LamaPon::CompileShaderCached(
                assets,
                path,
                entryPoint,
                target,
                keywords);
        }
        catch (...)
        {
            return {};
        }
    }

    // GSの入力が三角形か返し、反映情報を取得できなければ許可します(byteCode: 有効なGSのコード)。
    [[nodiscard]] bool TakesTriangles(ID3DBlob* const byteCode) noexcept
    {
        // コードの反映情報
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
        // シェーダーの入力と資源情報
        D3D11_SHADER_DESC description{};
        if (FAILED(D3DReflect(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                IID_ID3D11ShaderReflection,
                &reflection))
            || FAILED(reflection->GetDesc(&description)))
        {
            return true;
        }
        return description.InputPrimitive == D3D_PRIMITIVE_TRIANGLE;
    }

    // 各段が読むb0～b3を累積し、反映情報の取得失敗時は全枠を有効化します(byteCode: 空を許す段のコード, buffers: 使用枠の累積出力)。
    void MarkConstantBuffers(
        ID3DBlob* const byteCode,
        std::array<bool, ConstantBufferCount>& buffers) noexcept
    {
        if (byteCode == nullptr)
        {
            return;
        }
        // コードの反映情報
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
        // シェーダーの入力と資源情報
        D3D11_SHADER_DESC description{};
        if (FAILED(D3DReflect(
                byteCode->GetBufferPointer(),
                byteCode->GetBufferSize(),
                IID_ID3D11ShaderReflection,
                &reflection))
            || FAILED(reflection->GetDesc(&description)))
        {
            buffers.fill(true);
            return;
        }
        // 反映した資源の番号
        for (UINT index{}; index < description.BoundResources; ++index)
        {
            // 定数バッファの結合情報
            D3D11_SHADER_INPUT_BIND_DESC binding{};
            if (SUCCEEDED(reflection->GetResourceBindingDesc(index, &binding))
                && binding.Type == D3D_SIT_CBUFFER
                && binding.BindPoint < buffers.size())
            {
                buffers[binding.BindPoint] = true;
            }
        }
    }

    // 宣言された合成方式を内部の状態番号へ変換します(blend: シェーダーの合成方式)。
    [[nodiscard]] BlendKind ToBlendKind(const ShaderBlendMode blend) noexcept
    {
        switch (blend)
        {
        case ShaderBlendMode::Alpha:
            return BlendKind::NonPremultiplied;
        case ShaderBlendMode::Additive:
            return BlendKind::AdditivePreservingAlpha;
        case ShaderBlendMode::Premultiplied:
            return BlendKind::Premultiplied;
        case ShaderBlendMode::Opaque:
        default:
            return BlendKind::Opaque;
        }
    }

    // 合成係数を作り、AdditivePreservingAlphaだけは出力アルファを保持します(blend: 合成方式)。
    [[nodiscard]] D3D12_BLEND_DESC MakeBlendDescription(
        const BlendKind blend) noexcept
    {
        // 出力スロットの合成設定
        D3D12_RENDER_TARGET_BLEND_DESC target{};
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        switch (blend)
        {
        case BlendKind::NonPremultiplied:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            target.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
            target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BlendKind::AdditivePreservingAlpha:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_ONE;
            target.DestBlend = D3D12_BLEND_ONE;
            target.SrcBlendAlpha = D3D12_BLEND_ZERO;
            target.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        case BlendKind::Additive:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            target.DestBlend = D3D12_BLEND_ONE;
            target.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
            target.DestBlendAlpha = D3D12_BLEND_ONE;
            break;
        case BlendKind::Premultiplied:
            target.BlendEnable = TRUE;
            target.SrcBlend = D3D12_BLEND_ONE;
            target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case BlendKind::Opaque:
        default:
            target.BlendEnable = FALSE;
            target.SrcBlend = D3D12_BLEND_ONE;
            target.DestBlend = D3D12_BLEND_ZERO;
            target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlendAlpha = D3D12_BLEND_ZERO;
            break;
        }
        // 生成するGPUの描画状態
        D3D12_BLEND_DESC result{};
        // 各出力スロットの合成設定
        for (auto& renderTarget : result.RenderTarget)
        {
            renderTarget = target;
        }
        return result;
    }

    // 時計回りを表面としてカリングと辺の描画を設定します(cull: 除外する面, wireframe: 辺だけを描く指定)。
    [[nodiscard]] D3D12_RASTERIZER_DESC MakeRasterizerDescription(
        const ShaderCullMode cull,
        const bool wireframe) noexcept
    {
        // 生成するGPUの描画状態
        D3D12_RASTERIZER_DESC result{};
        result.FillMode = wireframe
            ? D3D12_FILL_MODE_WIREFRAME
            : D3D12_FILL_MODE_SOLID;
        result.CullMode = cull == ShaderCullMode::Front
            ? D3D12_CULL_MODE_FRONT
            : cull == ShaderCullMode::None
                ? D3D12_CULL_MODE_NONE
                : D3D12_CULL_MODE_BACK;
        result.FrontCounterClockwise = FALSE;
        result.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        result.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        result.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        result.DepthClipEnable = TRUE;
        // ワイヤーフレームの線幅をDirectXTKと同じ設定にします。
        result.MultisampleEnable = wireframe ? TRUE : FALSE;
        return result;
    }

    // 通常・読取・無効・奥だけの深度状態を作ります(depth: 深度の用途)。
    [[nodiscard]] D3D12_DEPTH_STENCIL_DESC MakeDepthDescription(
        const DepthKind depth) noexcept
    {
        // 生成するGPUの描画状態
        D3D12_DEPTH_STENCIL_DESC result{};
        result.DepthEnable = depth != DepthKind::None;
        result.DepthWriteMask = depth == DepthKind::Default
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
        result.DepthFunc = depth == DepthKind::Occluded
            ? D3D12_COMPARISON_FUNC_GREATER
            : D3D12_COMPARISON_FUNC_LESS_EQUAL;
        result.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        result.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        return result;
    }

    // b0～b4・t0～t25と素材・影サンプラーの署名を作ります(device: 生成元のデバイス, pointSampler: 素材の最近傍補間指定)。
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12RootSignature>
        CreateMaterialRootSignature(
            ID3D12Device* const device,
            const bool pointSampler)
    {
        // t0～t25の個別SRV範囲
        std::array<D3D12_DESCRIPTOR_RANGE, TextureSlotCount> ranges{};
        // ルート引数またはSRVの番号
        for (UINT index{}; index < ranges.size(); ++index)
        {
            ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[index].NumDescriptors = 1;
            ranges[index].BaseShaderRegister = index;
        }
        // b0～b4と各t枠のルート引数列
        std::array<
            D3D12_ROOT_PARAMETER,
            ConstantBufferCount + TextureSlotCount + 1u> parameters{};
        // ルート引数またはSRVの番号
        for (UINT index{}; index < ConstantBufferCount; ++index)
        {
            parameters[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            parameters[index].Descriptor.ShaderRegister = index;
            parameters[index].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        // ルート引数またはSRVの番号
        for (std::size_t index{}; index < ranges.size(); ++index)
        {
            // 対象SRVのルート引数
            auto& parameter = parameters[ConstantBufferCount + index];
            parameter.ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameter.DescriptorTable.NumDescriptorRanges = 1;
            parameter.DescriptorTable.pDescriptorRanges = &ranges[index];
            parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        // b4の骨変形用ルート引数
        auto& skinning = parameters[SkinningParameter];
        skinning.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        skinning.Descriptor.ShaderRegister = 4;
        skinning.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        // 素材と影の固定サンプラー
        std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
        // s0の素材サンプラー
        auto& material = samplers[0];
        material.Filter = pointSampler
            ? D3D12_FILTER_MIN_MAG_MIP_POINT
            : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        material.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        material.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        material.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        material.MaxAnisotropy = 1;
        material.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        material.MaxLOD = D3D12_FLOAT32_MAX;
        material.ShaderRegister = 0;
        material.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // s1の影比較サンプラー
        auto& shadow = samplers[1];
        shadow.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        shadow.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadow.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadow.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadow.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        shadow.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        shadow.MinLOD = 0.0f;
        shadow.MaxLOD = 0.0f;
        shadow.ShaderRegister = 1;
        shadow.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // ルート署名の生成設定
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = static_cast<UINT>(parameters.size());
        description.pParameters = parameters.data();
        description.NumStaticSamplers = static_cast<UINT>(samplers.size());
        description.pStaticSamplers = samplers.data();
        description.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        // 直列化したルート署名
        Microsoft::WRL::ComPtr<ID3DBlob> serialized;
        // 署名の直列化時の診断
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        // ルート署名の直列化結果
        const HRESULT serializedResult = D3D12SerializeRootSignature(
            &description,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(serializedResult))
        {
            // 診断を含める例外文面
            std::string message =
                "D3D12SerializeRootSignature(material shader) failed";
            if (errors != nullptr && errors->GetBufferSize() > 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        // 生成したルート署名
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serialized->GetBufferPointer(),
                serialized->GetBufferSize(),
                IID_PPV_ARGS(rootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature(material shader)");
        return rootSignature;
    }

    // 解決済みのビューで照明機能を有効化し、定数を作ります(lighting: ライトと効果の設定, views: ビューの解決結果, active: 使用する機能の出力)。
    [[nodiscard]] LightingConstants BuildLightingConstants(
        const LamaPon::LightingState& lighting,
        const LightingViews& views,
        LightingViews& active) noexcept
    {
        // 有効機能を反映する照明定数
        LightingConstants constants;
        // 負値を除いた環境光の強度
        const float ambientIntensity =
            std::max(lighting.ambientIntensity, 0.0f);
        constants.ambient = {
            lighting.ambientColor.x * ambientIntensity,
            lighting.ambientColor.y * ambientIntensity,
            lighting.ambientColor.z * ambientIntensity,
            1.0f
        };
        constants.fogColor = {
            lighting.fog.color.x,
            lighting.fog.color.y,
            lighting.fog.color.z,
            1.0f
        };
        constants.fogParameters = {
            lighting.fog.startDistance,
            lighting.fog.endDistance,
            lighting.fog.density,
            lighting.fog.enabled ? 1.0f : 0.0f
        };
        constants.lightCounts = {
            static_cast<std::uint32_t>(std::min(
                lighting.directionalLightCount,
                LamaPon::MaximumDirectionalLights)),
            static_cast<std::uint32_t>(std::min(
                lighting.pointLightCount,
                LamaPon::MaximumPointLights)),
            static_cast<std::uint32_t>(std::min(
                lighting.spotLightCount,
                LamaPon::MaximumSpotLights)),
            static_cast<std::uint32_t>(std::min(
                lighting.directionalShadow.cascadeCount,
                LamaPon::MaximumShadowCascades))
        };
        // 種類別のライト番号
        for (std::size_t index{}; index < constants.lightCounts[0]; ++index)
        {
            // 対象ライトの設定
            const auto& source = lighting.directionalLights[index];

            constants.directionalLights[index] = {
                { source.direction.x,
                    source.direction.y,
                    source.direction.z,
                    source.intensity },
                { source.color.x,
                    source.color.y,
                    source.color.z,
                    source.angularRadius }
            };
        }
        // 種類別のライト番号
        for (std::size_t index{}; index < constants.lightCounts[1]; ++index)
        {
            // 対象ライトの設定
            const auto& source = lighting.pointLights[index];
            constants.pointLights[index] = {
                { source.position.x,
                    source.position.y,
                    source.position.z,
                    source.range },
                { source.color.x,
                    source.color.y,
                    source.color.z,
                    source.intensity }
            };
        }
        // 種類別のライト番号
        for (std::size_t index{}; index < constants.lightCounts[2]; ++index)
        {
            // 対象ライトの設定
            const auto& source = lighting.spotLights[index];
            constants.spotLights[index] = {
                { source.position.x,
                    source.position.y,
                    source.position.z,
                    source.range },
                { source.direction.x,
                    source.direction.y,
                    source.direction.z,
                    source.innerConeCosine },
                { source.color.x,
                    source.color.y,
                    source.color.z,
                    source.intensity },
                { source.outerConeCosine, 0.0f, 0.0f, 0.0f }
            };
        }

        // 平行光の影設定
        const auto& shadow = lighting.directionalShadow;
        active.directionalShadow = shadow.enabled
            && views.directionalShadow
            && shadow.cascadeCount != 0
            && shadow.lightIndex < constants.lightCounts[0];
        if (!active.directionalShadow)
        {
            constants.lightCounts[3] = 0u;
        }
        constants.shadowViewProjections = shadow.lightViewProjections;
        constants.shadowCascadeSplits = {
            shadow.cascadeSplits[0],
            shadow.cascadeSplits[1],
            shadow.cascadeSplits[2],
            shadow.cascadeSplits[3]
        };
        constants.shadowParameters = {
            active.directionalShadow
                ? static_cast<float>(shadow.lightIndex + 1)
                : 0.0f,
            shadow.bias,
            shadow.normalBias,
            shadow.strength
        };

        active.spotShadow = false;
        // スポット影のスロット番号
        for (std::size_t slot{}; slot < LamaPon::MaximumSpotShadows; ++slot)
        {
            // 対象スポット影の設定
            const auto& spotShadow = lighting.spotShadows[slot];
            if (!views.spotShadow
                || !spotShadow.enabled
                || spotShadow.lightIndex < 0
                || static_cast<std::size_t>(spotShadow.lightIndex)
                    >= constants.lightCounts[2])
            {
                continue;
            }
            constants.spotShadowViewProjections[slot] =
                spotShadow.lightViewProjection;
            constants.spotShadowParameters[slot] = {
                spotShadow.bias,
                spotShadow.normalBias,
                spotShadow.strength,
                1.0f
            };
            constants.spotLights[
                static_cast<std::size_t>(spotShadow.lightIndex)]
                .outerCosinePadding.y = static_cast<float>(slot + 1);
            active.spotShadow = true;
        }

        // 点光源の影設定
        const auto& pointShadow = lighting.pointShadow;
        active.pointShadow = pointShadow.enabled
            && views.pointShadow
            && pointShadow.lightIndex >= 0
            && static_cast<std::size_t>(pointShadow.lightIndex)
                < constants.lightCounts[1];
        constants.pointShadowParameters = {
            active.pointShadow
                ? static_cast<float>(pointShadow.lightIndex + 1)
                : 0.0f,
            pointShadow.bias,
            pointShadow.strength,
            0.0f
        };
        constants.shadowTexelSizes = {
            1.0f / std::max(lighting.directionalShadowResolution, 1.0f),
            1.0f / std::max(lighting.localShadowResolution, 1.0f),
            1.0f / std::max(lighting.localShadowResolution, 1.0f),
            0.0f
        };

        // 画面空間AOの設定
        const auto& occlusion = lighting.screenAmbientOcclusion;
        active.screenAmbientOcclusion =
            occlusion.enabled && views.screenAmbientOcclusion;
        constants.screenAmbientOcclusionParameters = {
            occlusion.inverseWidth,
            occlusion.inverseHeight,
            active.screenAmbientOcclusion ? 1.0f : 0.0f,
            0.0f
        };

        // 画面空間反射の設定
        const auto& reflection = lighting.screenSpaceReflection;
        active.screenReflection =
            reflection.enabled && views.screenReflection;
        constants.screenReflectionParameters = {
            std::clamp(reflection.intensity, 0.0f, 1.0f),
            active.screenReflection ? 1.0f : 0.0f,
            std::max(reflection.maximumDistance, 0.01f),
            static_cast<float>(std::clamp<std::uint32_t>(
                reflection.stepCount,
                1u,
                128u))
        };
        constants.screenReflectionScreen = {
            reflection.inverseWidth,
            reflection.inverseHeight,
            reflection.projectionZ,
            reflection.projectionW
        };
        constants.screenReflectionQuality = {
            std::max(reflection.thickness, 0.001f),
            std::clamp(reflection.roughnessCutoff, 0.0f, 1.0f),
            static_cast<float>(reflection.depthPyramidMaximumMip),
            0.0f
        };
        constants.screenReflectionPreviousViewProjection =
            reflection.previousViewProjection;

        // 反射・放射照度の両キューブが揃う場合だけ事前畳み込みを使い、最大ミップ0は元キューブを読みます。
        // 環境画像による照明の設定
        const auto& environment = lighting.environment;
        active.environment = environment.enabled && views.environment;
        active.prefilteredEnvironment =
            active.environment && views.prefilteredEnvironment;
        constants.environmentParameters = {
            active.environment ? std::max(environment.intensity, 0.0f) : 0.0f,
            active.environment ? 1.0f : 0.0f,
            active.prefilteredEnvironment
                ? environment.specularMaximumMip
                : 0.0f,
            0.0f
        };


        // Forward+の格子設定
        const auto& clustered = lighting.clustered;
        active.clustered = clustered.enabled && views.clustered;
        constants.clusteredParameters = {
            static_cast<float>(LamaPon::ClusteredLights::GridWidth),
            static_cast<float>(LamaPon::ClusteredLights::GridHeight),
            static_cast<float>(LamaPon::ClusteredLights::GridDepth),
            active.clustered ? 1.0f : 0.0f
        };
        constants.clusteredDepthParameters = {
            clustered.nearPlane,
            clustered.farPlane,
            std::log(std::max(
                clustered.farPlane
                    / std::max(clustered.nearPlane, 0.0001f),
                1.0001f)),
            static_cast<float>(
                LamaPon::ClusteredLights::MaximumLightsPerCluster)
        };
        constants.clusteredScreenParameters = {
            clustered.inverseWidth,
            clustered.inverseHeight,
            static_cast<float>(clustered.lightCount),
            0.0f
        };


        // ベイクした間接光の設定
        const auto& bakedGi = lighting.bakedGlobalIllumination;
        active.bakedGlobalIllumination =
            bakedGi.enabled && views.bakedGlobalIllumination;
        constants.bakedGiVolumeMinimum = {
            bakedGi.volumeMinimum.x,
            bakedGi.volumeMinimum.y,
            bakedGi.volumeMinimum.z,
            active.bakedGlobalIllumination ? 1.0f : 0.0f
        };
        constants.bakedGiInverseSize = {
            1.0f / std::max(bakedGi.volumeSize.x, 0.0001f),
            1.0f / std::max(bakedGi.volumeSize.y, 0.0001f),
            1.0f / std::max(bakedGi.volumeSize.z, 0.0001f),
            std::max(bakedGi.intensity, 0.0f)
        };
        constants.bakedGiResolution = {
            std::max(bakedGi.resolution.x, 1.0f),
            std::max(bakedGi.resolution.y, 1.0f),
            std::max(bakedGi.resolution.z, 1.0f),
            0.0f
        };
        return constants;
    }
}

namespace LamaPon::Detail
{
    D3D12MaterialShaderRenderer::D3D12MaterialShaderRenderer(
        D3D12Backend& backend)
        : m_backend(&backend)
    {
        // 借用する初期化済みデバイス
        auto* const device = backend.Device();
        if (device == nullptr)
        {
            throw std::invalid_argument(
                "The DirectX 12 material shader renderer requires an "
                "initialized backend.");
        }
        m_linearRootSignature = CreateMaterialRootSignature(device, false);
        m_pointRootSignature = CreateMaterialRootSignature(device, true);
        m_skinnedVertexShader = CompileEmbeddedShader(
            SkinnedVertexShaderSource,
            sizeof(SkinnedVertexShaderSource) - 1u,
            "LamaPonD3D12SkinnedMaterial",
            "SkinnedVertexShader",
            "vs_5_0");
        m_skinnedErrorShader.vertexShader = m_skinnedVertexShader;
        m_skinnedErrorShader.pixelShader = CompileEmbeddedShader(
            SkinnedVertexShaderSource,
            sizeof(SkinnedVertexShaderSource) - 1u,
            "LamaPonD3D12SkinnedMaterial",
            "SkinnedErrorPixelShader",
            "ps_5_0");
        // LamaPonShaderError.hlslの宣言（不透明・両面・深度書き込み）です。
        m_skinnedErrorShader.renderState.declared = true;
        m_skinnedErrorShader.renderState.blend = ShaderBlendMode::Opaque;
        m_skinnedErrorShader.renderState.cull = ShaderCullMode::None;
        m_skinnedErrorShader.renderState.depthWrite = true;
        m_skinnedErrorShader.renderState.depthTest = true;
        // 既定法線の単色画像
        const auto flatNormal =
            backend.CreateSolidRgba8Texture({ 128u, 128u, 255u, 255u });
        m_flatNormalView = backend.CreateShaderResourceView(flatNormal);
    }

    D3D12MaterialShaderRenderer::~D3D12MaterialShaderRenderer() noexcept =
        default;

    D3D12MaterialShaderRenderer::ShaderEntry&
        D3D12MaterialShaderRenderer::Prepare(
            AssetManager& assets,
            const MaterialShaderSource& source,
            const bool skinned)
    {
        // 通常または骨変形のキャッシュ
        auto& entry =
            (skinned ? m_skinnedShaders : m_shaders)[source.cacheKey];
        // 再確認間隔を判定する現在時刻
        const auto now = std::chrono::steady_clock::now();
        if (entry.observed
            && !entry.forceReload
            && now < entry.nextCheck)
        {
            return entry;
        }
        entry.nextCheck = now + std::chrono::milliseconds(250);

        // アーカイブでは保存時刻を調べず、存在状態の変化または無効化要求で再読み込みします。
        // 資源アーカイブを使用中か
        const bool archived = assets.IsArchived();
        // 保存時刻の取得エラー
        std::error_code fileError;
        // シェーダー元ファイルの有無
        const bool sourceExists = assets.FileExists(source.path);
        // 元ファイルの保存時刻
        const auto writeTime = (sourceExists && !archived)
            ? std::filesystem::last_write_time(source.path, fileError)
            : std::filesystem::file_time_type{};
        // キャッシュ更新が必要か
        const bool changed = !entry.observed
            || entry.forceReload
            || entry.sourceExists != sourceExists
            || (sourceExists
                && !archived
                && entry.writeTime != writeTime);
        if (!changed)
        {
            return entry;
        }
        entry.observed = true;
        entry.forceReload = false;
        entry.sourceExists = sourceExists;
        entry.writeTime = writeTime;
        entry.pipelines.clear();
        if (!sourceExists)
        {
            entry.error =
                "Shader file was not found: " + PathToUtf8(source.path);
            entry.vertexShader.Reset();
            entry.pixelShader.Reset();
            entry.instancedVertexShader.Reset();
            entry.geometryShader.Reset();
            entry.hullShader.Reset();
            entry.domainShader.Reset();
            entry.outlineVertexShader.Reset();
            entry.outlinePixelShader.Reset();
            entry.occludedPixelShader.Reset();
            entry.occludedUsesMaterialVertexShader = false;
            entry.passError.clear();
            return entry;
        }

        try
        {
            // 描画状態を読むHLSLのバイト列
            const auto sourceBytes = assets.ReadFileBytes(source.path);
            // HLSLで宣言された描画状態
            const auto renderState = ParseShaderRenderState(
                std::string_view{
                    reinterpret_cast<const char*>(sourceBytes.data()),
                    sourceBytes.size() });
            // 骨変形の主パスは素材VSも検証した上で、互換内蔵VSと素材PSを組み合わせます。
            // 素材の通常または骨変形VS
            auto vertexShader = CompileShaderCached(
                assets,
                source.path,
                skinned ? "VSSkinnedMain" : "VSMain",
                "vs_5_0",
                source.keywords);
            // 素材の通常または骨変形PS
            auto pixelShader = CompileShaderCached(
                assets,
                source.path,
                skinned ? "PSSkinnedMain" : "PSMain",
                "ps_5_0",
                source.keywords);
            // まとめ描き用の任意VS
            auto instancedVertexShader = skinned
                ? Microsoft::WRL::ComPtr<ID3DBlob>{}
                : TryCompileShader(
                    assets,
                    source.path,
                    "VSInstancedMain",
                    "vs_5_0",
                    source.keywords);
            // 三角形入力の任意GS
            auto geometryShader = TryCompileShader(
                assets,
                source.path,
                "GSMain",
                "gs_5_0",
                source.keywords);
            if (geometryShader != nullptr
                && !TakesTriangles(geometryShader.Get()))
            {
                throw std::runtime_error(
                    "GSMain must take 'triangle' input."
                    " LamaPon only ever draws triangles, so a"
                    " point/line geometry shader cannot be used.");
            }
            // HSとDSの両方を生成できた場合だけテセレーションを使います。
            // 4制御点の任意HS
            auto hullShader = TryCompileShader(
                assets,
                source.path,
                "HSMain",
                "hs_5_0",
                source.keywords);
            // 分割後の頂点を作る任意DS
            auto domainShader = TryCompileShader(
                assets,
                source.path,
                "DSMain",
                "ds_5_0",
                source.keywords);
            if (hullShader == nullptr || domainShader == nullptr)
            {
                hullShader.Reset();
                domainShader.Reset();
            }
            // HSとDSの両方が有効か
            const bool hasTessellation = hullShader != nullptr;
            // 輪郭はVSとPSの両方を生成できた場合だけ使います。
            // 輪郭用の任意VS
            Microsoft::WRL::ComPtr<ID3DBlob> outlineVertexShader;
            // 輪郭用の任意PS
            Microsoft::WRL::ComPtr<ID3DBlob> outlinePixelShader;
            // 遮蔽表示用の任意PS
            Microsoft::WRL::ComPtr<ID3DBlob> occludedPixelShader;
            // 互換の素材VSを使う遮蔽表示
            bool occludedUsesMaterialVertexShader{};
            outlineVertexShader = TryCompileShader(
                assets,
                source.path,
                skinned ? "VSSkinnedOutline" : "VSOutline",
                "vs_5_0",
                source.keywords);
            outlinePixelShader = TryCompileShader(
                assets,
                source.path,
                "PSOutline",
                "ps_5_0",
                source.keywords);
            if (outlineVertexShader == nullptr
                || outlinePixelShader == nullptr)
            {
                outlineVertexShader.Reset();
                outlinePixelShader.Reset();
            }
            occludedPixelShader = TryCompileShader(
                assets,
                source.path,
                skinned ? "PSSkinnedOccluded" : "PSOccluded",
                "ps_5_0",
                source.keywords);
            // 遮蔽表示用の任意PS
            if (skinned && occludedPixelShader == nullptr)
            {
                // 専用の骨変形PSがない場合はPSOccludedと素材の骨変形VSを組み合わせます。
                occludedPixelShader = TryCompileShader(
                    assets,
                    source.path,
                    "PSOccluded",
                    "ps_5_0",
                    source.keywords);
                occludedUsesMaterialVertexShader =
                    occludedPixelShader != nullptr;
            }
            // 各シェーダーが使うb0～b3
            std::array<bool, ConstantBufferCount> constantBuffers{};
            MarkConstantBuffers(vertexShader.Get(), constantBuffers);
            MarkConstantBuffers(pixelShader.Get(), constantBuffers);
            MarkConstantBuffers(instancedVertexShader.Get(), constantBuffers);
            MarkConstantBuffers(geometryShader.Get(), constantBuffers);
            MarkConstantBuffers(hullShader.Get(), constantBuffers);
            MarkConstantBuffers(domainShader.Get(), constantBuffers);
            MarkConstantBuffers(outlineVertexShader.Get(), constantBuffers);
            MarkConstantBuffers(outlinePixelShader.Get(), constantBuffers);
            MarkConstantBuffers(occludedPixelShader.Get(), constantBuffers);

            entry.vertexShader = std::move(vertexShader);
            entry.pixelShader = std::move(pixelShader);
            entry.instancedVertexShader = std::move(instancedVertexShader);
            entry.geometryShader = std::move(geometryShader);
            entry.hullShader = std::move(hullShader);
            entry.domainShader = std::move(domainShader);
            entry.outlineVertexShader = std::move(outlineVertexShader);
            entry.outlinePixelShader = std::move(outlinePixelShader);
            entry.occludedPixelShader = std::move(occludedPixelShader);
            entry.occludedUsesMaterialVertexShader =
                occludedUsesMaterialVertexShader;
            entry.renderState = renderState;
            entry.constantBuffers = constantBuffers;
            entry.hasTessellation = hasTessellation;
            entry.generation = m_nextGeneration++;
            entry.error.clear();
            entry.passError.clear();
        }
        // コンパイル失敗の診断
        catch (const std::exception& exception)
        {
            entry.error = source.describeFailure
                ? source.describeFailure(exception.what())
                : std::string(exception.what());
            // 再コンパイル失敗時は旧コードを外し、代替表示へ切り替えます。
            entry.vertexShader.Reset();
            entry.pixelShader.Reset();
            entry.instancedVertexShader.Reset();
            entry.geometryShader.Reset();
            entry.hullShader.Reset();
            entry.domainShader.Reset();
            entry.outlineVertexShader.Reset();
            entry.outlinePixelShader.Reset();
            entry.occludedPixelShader.Reset();
            entry.occludedUsesMaterialVertexShader = false;
            entry.passError.clear();
        }
        return entry;
    }

    ID3D12PipelineState* D3D12MaterialShaderRenderer::PipelineState(
        ShaderEntry& entry,
        const PipelineKey& key)
    {
        // 描画条件ごとのPSOキャッシュ
        auto& pipeline = entry.pipelines[key];
        if (pipeline != nullptr)
        {
            return pipeline.Get();
        }

        // 60バイトのDirectXTK骨変形頂点の配置
        static const std::array<D3D12_INPUT_ELEMENT_DESC, 7> skinnedInputs{ {
            { "SV_Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 40u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 44u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 52u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 56u,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        } };
        // 位置・法線・UVの基本頂点の配置
        static const std::array<D3D12_INPUT_ELEMENT_DESC, 3> inputs{ {
            { "SV_Position", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                offsetof(PrimitiveRenderVertex, position),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
                offsetof(PrimitiveRenderVertex, normal),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
                offsetof(PrimitiveRenderVertex, textureCoordinate),
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        } };
        // 基本頂点とスロット1の80バイトの行列・色
        static const std::array<D3D12_INPUT_ELEMENT_DESC, 8>
            instancedInputs{ {
            inputs[0],
            inputs[1],
            inputs[2],
            { "INSTANCE_TRANSFORM", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 0u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCE_TRANSFORM", 1, DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 16u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCE_TRANSFORM", 2, DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 32u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCE_TRANSFORM", 3, DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 48u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCE_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
                1, 64u, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 }
        } };
        // 各段と描画状態のPSO設定
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = key.pointSampler
            ? m_pointRootSignature.Get()
            : m_linearRootSignature.Get();
        // 輪郭は専用VSを使い、遮蔽表示は必要に応じて互換VSから素材VSへ切り替えます。
        // 通常・輪郭・遮蔽のパス番号
        const auto pass = static_cast<MaterialShaderPass>(key.pass);
        // このパスで使用するVS
        auto* const vertexShader = pass == MaterialShaderPass::Outline
            ? entry.outlineVertexShader.Get()
            : pass == MaterialShaderPass::Occluded
                    && key.skinned
                    && entry.occludedUsesMaterialVertexShader
                ? entry.vertexShader.Get()
            : key.instanced
                ? entry.instancedVertexShader.Get()
            : key.skinned
                ? m_skinnedVertexShader.Get()
                : entry.vertexShader.Get();
        // このパスで使用するPS
        auto* const pixelShader = pass == MaterialShaderPass::Outline
            ? entry.outlinePixelShader.Get()
            : pass == MaterialShaderPass::Occluded
                ? entry.occludedPixelShader.Get()
                : entry.pixelShader.Get();
        if (vertexShader == nullptr
            || (!key.depthOnly && pixelShader == nullptr))
        {
            throw std::logic_error(
                "The material shader has no entry point for the requested "
                "pass.");
        }
        description.VS = {
            vertexShader->GetBufferPointer(),
            vertexShader->GetBufferSize()
        };
        if (!key.depthOnly)
        {
            description.PS = {
                pixelShader->GetBufferPointer(),
                pixelShader->GetBufferSize()
            };
        }
        // 骨変形と輪郭ではGSの入力互換性を保証できないためGSを外します。
        if (!key.skinned
            && pass != MaterialShaderPass::Outline
            && entry.geometryShader != nullptr)
        {
            // 深度のみの描画でもGSによる形状を反映します。
            description.GS = {
                entry.geometryShader->GetBufferPointer(),
                entry.geometryShader->GetBufferSize()
            };
        }
        // パッチではHS・DSを結合し、深度のみの描画にも分割後の形状を反映します。
        if (key.tessellated)
        {
            if (entry.hullShader == nullptr || entry.domainShader == nullptr)
            {
                throw std::logic_error(
                    "The tessellated material shader has no hull or domain "
                    "shader.");
            }
            description.HS = {
                entry.hullShader->GetBufferPointer(),
                entry.hullShader->GetBufferSize()
            };
            description.DS = {
                entry.domainShader->GetBufferPointer(),
                entry.domainShader->GetBufferSize()
            };
        }
        description.BlendState =
            MakeBlendDescription(static_cast<BlendKind>(key.blend));
        description.SampleMask = std::numeric_limits<UINT>::max();
        description.RasterizerState = MakeRasterizerDescription(
            static_cast<ShaderCullMode>(key.cull),
            key.wireframe);
        description.DepthStencilState =
            MakeDepthDescription(static_cast<DepthKind>(key.depth));
        description.InputLayout = key.skinned
            ? D3D12_INPUT_LAYOUT_DESC{
                skinnedInputs.data(),
                static_cast<UINT>(skinnedInputs.size()) }
            : key.instanced
                ? D3D12_INPUT_LAYOUT_DESC{
                    instancedInputs.data(),
                    static_cast<UINT>(instancedInputs.size()) }
            : D3D12_INPUT_LAYOUT_DESC{
                inputs.data(),
                static_cast<UINT>(inputs.size()) };
        description.PrimitiveTopologyType = key.tessellated
            ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH
            : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = key.depthOnly ? 0u : 1u;
        if (!key.depthOnly)
        {
            description.RTVFormats[0] = key.colorFormat;
        }
        description.DSVFormat = key.depthFormat;
        description.SampleDesc.Count = 1;
        // 生成したPSOの所有参照
        Microsoft::WRL::ComPtr<ID3D12PipelineState> created;
        ThrowIfFailed(
            m_backend->Device()->CreateGraphicsPipelineState(
                &description,
                IID_PPV_ARGS(created.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateGraphicsPipelineState(material shader)");
        pipeline = std::move(created);
        return pipeline.Get();
    }

    MaterialShaderDrawResult D3D12MaterialShaderRenderer::Draw(
        AssetManager& assets,
        const MaterialShaderSource& shader,
        const MaterialShaderSource& placeholder,
        const bool prepass,
        const PrimitiveDrawRequest& request,
        const std::span<const PrimitiveRenderVertex> vertices,
        const std::span<const std::uint32_t> indices,
        const MaterialShaderDrawRequest& material,
        const LightingState& lighting)
    {
        // 描画成否とシェーダー診断
        MaterialShaderDrawResult result;
        // 骨変形用の頂点と骨の指定
        const auto* const skinned = material.skinned;
        // 複数インスタンスをまとめるか
        const bool instanced = skinned == nullptr
            && !material.instances.empty();
        // 基本頂点と骨変形頂点では入力のバイト配置が異なります。
        // 実際に描く頂点のバイト列
        const auto drawVertices = skinned != nullptr
            ? skinned->vertices
            : std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(vertices.data()),
                vertices.size_bytes());
        // 実際に描く頂点番号列
        const auto drawIndices = skinned != nullptr
            ? skinned->indices
            : indices;
        // 頂点間隔のバイト数
        const std::uint32_t vertexStride = skinned != nullptr
            ? skinned->vertexStride
            : static_cast<std::uint32_t>(sizeof(PrimitiveRenderVertex));
        if (material.material == nullptr
            || drawVertices.empty()
            || drawIndices.empty()
            || material.instances.size()
                > std::numeric_limits<UINT>::max()
            || material.instances.size_bytes()
                > std::numeric_limits<UINT>::max()
            || (instanced && material.pass != MaterialShaderPass::Main))
        {
            return result;
        }
        if (skinned != nullptr)
        {
            // 入力骨変形頂点の数
            const auto vertexCount = drawVertices.size() / SkinnedVertexStride;
            // 頂点範囲外の索引か判定します(index: 入力の頂点番号)。
            if (vertexStride != SkinnedVertexStride
                || drawVertices.size() % SkinnedVertexStride != 0u
                || drawIndices.size() % 3u != 0u
                || std::ranges::any_of(
                    drawIndices,
                    [vertexCount](const std::uint32_t index)
                    {
                        return index >= vertexCount;
                    }))
            {
                return result;
            }
        }
        // 描画するマテリアル
        const auto& litMaterial = *material.material;
        // 色を出力せず深度だけ描くか
        const bool depthOnly = request.depthOnly;
        if (depthOnly && !m_backend->IsDepthOnlyPassActive())
        {
            return result;
        }

        // 主パスのVSとPSが有効か返します(entry: 準備したシェーダー)。
        const auto usable = [](const ShaderEntry& entry) noexcept
        {
            return entry.vertexShader != nullptr
                && entry.pixelShader != nullptr;
        };
        // 使用する通常または代替コード
        auto* active = &Prepare(assets, shader, skinned != nullptr);
        result.generation = active->generation;
        result.error = active->error;
        result.passError = active->passError;
        // 4制御点パッチへ分けられない入力のテセレーションは代替表示へ切り替えます。
        if (usable(*active)
            && active->hasTessellation
            && (skinned != nullptr
                || material.directXTKPart != nullptr
                || material.tessellationPatches.empty()
                || material.tessellationPatches.size() % 4u != 0u))
        {
            result.error = skinned != nullptr
                    || material.directXTKPart != nullptr
                ? "This shader uses tessellation (HSMain/DSMain),"
                    " which only works on a Mesh Renderer whose"
                    " shape can be split into quad patches"
                    " (Plane and Cube)."
                : "This shader uses tessellation (HSMain/DSMain),"
                    " which only works on shapes that can be split"
                    " into quad patches (Plane and Cube).";
        }
        // 骨変形互換または通常の代替表示へ切り替え、利用成否を返します。
        const auto usePlaceholder = [&]()
        {
            if (skinned != nullptr)
            {
                active = &m_skinnedErrorShader;
                result.placeholder = true;
                return true;
            }
            // 準備した代替シェーダー
            auto& fallback = Prepare(assets, placeholder, false);
            if (!usable(fallback))
            {
                return false;
            }
            active = &fallback;
            result.placeholder = true;
            return true;
        };
        if ((!usable(*active) || !result.error.empty())
            && (result.error.empty() || !usePlaceholder()))
        {
            return result;
        }
        if (instanced && active->instancedVertexShader == nullptr)
        {
            return result;
        }

        // 輪郭・遮蔽表示は専用入口がある場合だけ描き、代替表示と深度描画では行いません。
        if (material.pass != MaterialShaderPass::Main
            && (result.placeholder
                || depthOnly
                || (material.pass == MaterialShaderPass::Outline
                    ? active->outlineVertexShader == nullptr
                    : active->occludedPixelShader == nullptr)))
        {
            return result;
        }

        // 骨変形を除き、宣言付きの半透明・深度書き込みなしは深度プリパスから除外します。
        if (depthOnly
            && prepass
            && skinned == nullptr
            && active->renderState.declared
            && (active->renderState.blend != ShaderBlendMode::Opaque
                || !active->renderState.depthWrite))
        {

            result.renderState = active->renderState;
            return result;
        }

        // 入力形式と素材・追加パスに合わせてPSOキーを作ります(entry: 使用するシェーダー)。
        const auto makeKey = [&](const ShaderEntry& entry)
        {
            // 出力形式と描画状態のPSOキー
            PipelineKey key;
            key.depthOnly = depthOnly;

            key.pointSampler = litMaterial.CustomParameters()[7].w >= 0.5f;
            key.colorFormat = depthOnly
                ? DXGI_FORMAT_UNKNOWN
                : m_backend->ActiveColorFormat();
            key.depthFormat = m_backend->ActiveDepthFormat();
            // 基本頂点は既定状態から素材宣言とWorld Overlayを適用し、深度描画では既定を保ちます。
            key.skinned = skinned != nullptr;
            key.instanced = instanced;
            // 代替表示はテセレーションを外して三角形を描きます。
            key.tessellated = entry.hasTessellation
                && !instanced
                && entry.hullShader != nullptr
                && skinned == nullptr
                && !material.tessellationPatches.empty();
            // 適用する合成方式
            auto blend = BlendKind::Opaque;
            // 適用する深度の判定と書き込み
            auto depth = DepthKind::Default;
            // 除外する面の向き
            auto cull = ShaderCullMode::Back;
            // 塗りつぶさず辺だけ描くか
            bool wireframe{};
            if (skinned != nullptr)
            {
                // 骨変形は深度描画にも宣言を適用し、ワイヤーフレーム指定は宣言より優先します。
                // シェーダーが宣言した描画状態
                const auto& state = entry.renderState;
                if (state.declared && !request.wireframe)
                {
                    blend = ToBlendKind(state.blend);
                    depth = state.depthTest
                        ? (state.depthWrite
                            ? DepthKind::Default
                            : DepthKind::Read)
                        : DepthKind::None;
                    cull = state.cull == ShaderCullMode::Front
                        ? ShaderCullMode::Back
                        : state.cull == ShaderCullMode::None
                                || skinned->doubleSided
                            ? ShaderCullMode::None
                            : ShaderCullMode::Front;
                }
                else
                {
                    blend = skinned->alphaPass
                        ? BlendKind::NonPremultiplied
                        : BlendKind::Opaque;
                    depth = skinned->alphaPass
                        ? DepthKind::Read
                        : DepthKind::Default;
                    cull = request.wireframe || skinned->doubleSided
                        ? ShaderCullMode::None
                        : ShaderCullMode::Front;
                }
                wireframe = request.wireframe;
            }
            else if (key.tessellated)
            {
                // パッチの深度描画ではカリングせず、World Overlayは色描画の深度判定を外します。
                // シェーダーが宣言した描画状態
                const auto& state = entry.renderState;
                cull = ShaderCullMode::None;
                if (!depthOnly && state.declared)
                {
                    blend = ToBlendKind(state.blend);
                    depth = state.depthTest
                        ? (state.depthWrite
                            ? DepthKind::Default
                            : DepthKind::Read)
                        : DepthKind::None;
                    cull = state.cull;
                }
                else if (!depthOnly && litMaterial.BaseColor().w < 1.0f)
                {
                    blend = BlendKind::NonPremultiplied;
                    depth = DepthKind::Read;
                }
                if (!depthOnly && material.worldOverlay)
                {
                    depth = DepthKind::None;
                }
            }
            else if (material.directXTKPart != nullptr)
            {
                // DirectXTK部品は色・深度とも部品の既定状態を素材宣言で上書きします。
                // DirectXTKモデルの部品条件
                const auto& part = *material.directXTKPart;
                blend = part.alphaPass
                    ? (part.premultipliedAlpha
                        ? BlendKind::Premultiplied
                        : BlendKind::NonPremultiplied)
                    : BlendKind::Opaque;
                depth = part.alphaPass
                    ? DepthKind::Read
                    : DepthKind::Default;
                // 素材の宣言がある場合はワイヤーフレームより塗りつぶしの宣言を優先します。
                cull = request.wireframe
                    ? ShaderCullMode::None
                    : part.counterClockwise
                        ? ShaderCullMode::Back
                        : ShaderCullMode::Front;
                // シェーダーが宣言した描画状態
                const auto& state = entry.renderState;
                wireframe = request.wireframe && !state.declared;
                if (state.declared)
                {
                    // DirectXTK部品の加算はアルファを保持しないAdditiveを使います。
                    blend = state.blend == ShaderBlendMode::Additive
                        ? BlendKind::Additive
                        : ToBlendKind(state.blend);
                    depth = state.depthTest
                        ? (state.depthWrite
                            ? DepthKind::Default
                            : DepthKind::Read)
                        : DepthKind::None;
                    cull = state.cull;
                }
            }
            else if (!depthOnly)
            {
                // シェーダーが宣言した描画状態
                const auto& state = entry.renderState;
                if (material.worldOverlay)
                {
                    blend = BlendKind::NonPremultiplied;
                    depth = DepthKind::None;
                }
                else if (state.declared)
                {
                    blend = ToBlendKind(state.blend);
                    depth = state.depthTest
                        ? (state.depthWrite
                            ? DepthKind::Default
                            : DepthKind::Read)
                        : DepthKind::None;
                    cull = state.cull;
                }
                else
                {
                    // 素材のアルファが1未満か
                    const bool translucent =
                        litMaterial.BaseColor().w < 1.0f;
                    blend = translucent
                        ? BlendKind::Premultiplied
                        : BlendKind::Opaque;
                    depth = translucent
                        ? DepthKind::Read
                        : DepthKind::Default;
                }
            }
            key.pass = static_cast<std::uint8_t>(material.pass);
            if (material.pass == MaterialShaderPass::Outline)
            {
                // 輪郭は素材宣言を上書きし、骨変形と基本頂点それぞれの輪郭状態で描きます。
                blend = key.skinned
                    ? BlendKind::Opaque
                    : BlendKind::NonPremultiplied;
                depth = key.skinned
                    ? DepthKind::Default
                    : DepthKind::Read;
                cull = key.skinned
                    ? ShaderCullMode::Back
                    : ShaderCullMode::Front;
                wireframe = false;
            }
            else if (material.pass == MaterialShaderPass::Occluded)
            {
                // 遮蔽表示は深度を書かず奥だけを非プリマルチプライド合成で描きます。
                blend = BlendKind::NonPremultiplied;
                depth = DepthKind::Occluded;
                cull = ShaderCullMode::Back;
                wireframe = false;
            }
            else if (material.cullOverride.has_value())
            {
                cull = *material.cullOverride;
            }
            key.wireframe = wireframe;
            if (key.depthFormat == DXGI_FORMAT_UNKNOWN)
            {
                depth = DepthKind::None;
            }
            key.blend = static_cast<std::uint8_t>(blend);
            key.depth = static_cast<std::uint8_t>(depth);
            key.cull = static_cast<std::uint8_t>(cull);
            return key;
        };

        // 適用する出力形式と描画状態
        auto key = makeKey(*active);
        // 借用するこの描画のPSO
        ID3D12PipelineState* pipeline{};
        try
        {
            pipeline = PipelineState(*active, key);
        }
        // PSO生成失敗の診断
        catch (const std::exception& exception)
        {
            if (result.placeholder)
            {
                throw;
            }
            // 整形したPSO生成の診断
            const auto failure = shader.describeFailure
                ? shader.describeFailure(exception.what())
                : std::string(exception.what());
            if (instanced)
            {
                // まとめ描きのPSO失敗時はその入口を外し、呼び出し元の個別描画へ戻します。
                active->instancedVertexShader.Reset();
                active->passError = failure;
                result.passError = failure;
                return result;
            }
            if (material.pass != MaterialShaderPass::Main)
            {
                // 追加パスのPSO失敗時は通常パスを保ち、失敗した入口だけを外します。
                if (material.pass == MaterialShaderPass::Outline)
                {
                    active->outlineVertexShader.Reset();
                    active->outlinePixelShader.Reset();
                }
                else
                {
                    active->occludedPixelShader.Reset();
                    active->occludedUsesMaterialVertexShader = false;
                }
                active->passError = failure;
                result.passError = failure;
                return result;
            }
            // 通常パスのPSO失敗時は全入口を外し、診断を保持して代替表示を描きます。
            active->error = failure;
            active->vertexShader.Reset();
            active->pixelShader.Reset();
            active->instancedVertexShader.Reset();
            active->geometryShader.Reset();
            active->hullShader.Reset();
            active->domainShader.Reset();
            active->outlineVertexShader.Reset();
            active->outlinePixelShader.Reset();
            active->occludedPixelShader.Reset();
            active->occludedUsesMaterialVertexShader = false;
            active->pipelines.clear();
            result.error = active->error;
            if (!usePlaceholder())
            {
                return result;
            }
            key = makeKey(*active);
            pipeline = PipelineState(*active, key);
        }
        result.renderState = active->renderState;

        // t0～t25のGPU読み取り記述子
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, TextureSlotCount> textures{};
        // 借用する読み取り記述子ヒープ
        ID3D12DescriptorHeap* descriptorHeap{};
        // 解決した平行光の影SRV
        std::optional<D3D12Backend::ShaderResourceBinding> directionalShadow;
        // 解決したスポット影SRV
        std::optional<D3D12Backend::ShaderResourceBinding> spotShadow;
        // 解決した点光源の影SRV
        std::optional<D3D12Backend::ShaderResourceBinding> pointShadow;
        // 解決したSSAOのSRV
        std::optional<D3D12Backend::ShaderResourceBinding> occlusion;
        // 解決したSSR履歴色のSRV
        std::optional<D3D12Backend::ShaderResourceBinding> reflectionColor;
        // 解決したSSR深度のSRV
        std::optional<D3D12Backend::ShaderResourceBinding> reflectionDepth;
        // 解決した環境キューブSRV
        std::optional<D3D12Backend::ShaderResourceBinding> environment;
        // 解決した反射キューブSRV
        std::optional<D3D12Backend::ShaderResourceBinding> environmentSpecular;
        // 解決した放射照度キューブSRV
        std::optional<D3D12Backend::ShaderResourceBinding> environmentIrradiance;
        // 解決したForward+ライトSRV
        std::optional<D3D12Backend::ShaderResourceBinding> clusterLights;
        // 解決したクラスタ番号表SRV
        std::optional<D3D12Backend::ShaderResourceBinding> clusterIndices;
        // 解決したクラスタ個数表SRV
        std::optional<D3D12Backend::ShaderResourceBinding> clusterCounts;
        // 解決した赤成分GI体積SRV
        std::optional<D3D12Backend::ShaderResourceBinding> bakedGiRed;
        // 解決した緑成分GI体積SRV
        std::optional<D3D12Backend::ShaderResourceBinding> bakedGiGreen;
        // 解決した青成分GI体積SRV
        std::optional<D3D12Backend::ShaderResourceBinding> bakedGiBlue;
        // 既定の白画像SRV
        std::optional<D3D12Backend::ShaderResourceBinding> white;
        // ビュー種別を確認した有効入力
        LightingViews views;
        if (!depthOnly)
        {
            descriptorHeap = m_backend->ShaderResourceDescriptorHeap();
            white = m_backend->TryResolveShaderResource(
                request.fallbackTexture);
            if (descriptorHeap == nullptr || !white)
            {
                return result;
            }
            // 指定または既定SRVを解決します(view: 指定の読み取りビュー, fallback: 未指定時のビュー, descriptor: 成功時のGPU記述子)。
            const auto resolve = [this](
                const GraphicsViewHandle& view,
                const GraphicsViewHandle& fallback,
                D3D12_GPU_DESCRIPTOR_HANDLE& descriptor)
            {
                // 同世代で解決したSRV情報
                const auto binding = m_backend->TryResolveShaderResource(
                    view ? view : fallback);
                if (!binding)
                {
                    return false;
                }
                descriptor = binding->descriptor;
                return true;
            };
            // 指定画像を解決できなければ描画せず、未指定だけを白画像・平坦法線へ置換します。
            // 全ての素材画像を解決できたか
            bool valid = resolve(request.albedo, request.fallbackTexture, textures[0])
                && resolve(request.normalTexture, m_flatNormalView, textures[1])
                && resolve(request.roughnessTexture, request.fallbackTexture, textures[11])
                && resolve(request.metallicTexture, request.fallbackTexture, textures[12])
                && resolve(request.occlusionTexture, request.fallbackTexture, textures[13])
                && resolve(request.emissiveTexture, request.fallbackTexture, textures[14]);
            // 自作素材画像の枠番号
            for (std::size_t index{};
                valid && index < material.customTextures.size();
                ++index)
            {
                valid = resolve(
                    material.customTextures[index],
                    request.fallbackTexture,
                    textures[LitMaterial::CustomTextureFirstSlot + index]);
            }
            if (!valid)
            {
                return result;
            }
            directionalShadow = m_backend->TryResolveShaderResource(
                request.directionalShadow.texture);
            spotShadow = m_backend->TryResolveShaderResource(
                request.spotShadowTexture);
            pointShadow = m_backend->TryResolveShaderResource(
                request.pointShadow.texture);
            occlusion = m_backend->TryResolveShaderResource(
                request.screenAmbientOcclusion.texture);
            reflectionColor = m_backend->TryResolveShaderResource(
                request.screenSpaceReflection.texture);
            reflectionDepth = m_backend->TryResolveShaderResource(
                request.screenSpaceReflection.depth);
            views.directionalShadow = directionalShadow.has_value();
            views.spotShadow = spotShadow.has_value();
            views.pointShadow = pointShadow.has_value();
            views.screenAmbientOcclusion = occlusion.has_value();
            views.screenReflection =
                reflectionColor.has_value() && reflectionDepth.has_value();
            environment = m_backend->TryResolveShaderResource(
                lighting.environment.texture);
            environmentSpecular = m_backend->TryResolveShaderResource(
                lighting.environment.specular);
            environmentIrradiance = m_backend->TryResolveShaderResource(
                lighting.environment.irradiance);
            // 解決したSRVがキューブか返します(binding: SRVの解決結果)。
            const auto isCube = [](
                const std::optional<D3D12Backend::ShaderResourceBinding>&
                    binding)
            {
                return binding.has_value()
                    && binding->dimension == D3D12_SRV_DIMENSION_TEXTURECUBE;
            };
            views.environment = isCube(environment);
            views.prefilteredEnvironment =
                isCube(environmentSpecular) && isCube(environmentIrradiance);
            clusterLights = m_backend->TryResolveShaderResource(
                lighting.clustered.lights);
            clusterIndices = m_backend->TryResolveShaderResource(
                lighting.clustered.lightIndices);
            clusterCounts = m_backend->TryResolveShaderResource(
                lighting.clustered.clusterCounts);
            // 解決したSRVがバッファか返します(binding: SRVの解決結果)。
            const auto isBuffer = [](
                const std::optional<D3D12Backend::ShaderResourceBinding>&
                    binding)
            {
                return binding.has_value()
                    && binding->dimension == D3D12_SRV_DIMENSION_BUFFER;
            };
            views.clustered = isBuffer(clusterLights)
                && isBuffer(clusterIndices)
                && isBuffer(clusterCounts);
            bakedGiRed = m_backend->TryResolveShaderResource(
                lighting.bakedGlobalIllumination.redCoefficients);
            bakedGiGreen = m_backend->TryResolveShaderResource(
                lighting.bakedGlobalIllumination.greenCoefficients);
            bakedGiBlue = m_backend->TryResolveShaderResource(
                lighting.bakedGlobalIllumination.blueCoefficients);
            // 解決したSRVが3D画像か返します(binding: SRVの解決結果)。
            const auto isVolume = [](
                const std::optional<D3D12Backend::ShaderResourceBinding>&
                    binding)
            {
                return binding.has_value()
                    && binding->dimension == D3D12_SRV_DIMENSION_TEXTURE3D;
            };
            views.bakedGlobalIllumination = isVolume(bakedGiRed)
                && isVolume(bakedGiGreen)
                && isVolume(bakedGiBlue);
        }
        // 定数で有効化する照明機能
        LightingViews activeLighting;
        // 有効な照明を反映した定数
        auto lightingConstants =
            BuildLightingConstants(lighting, views, activeLighting);
        if (!depthOnly)
        {
            // 未設定の入力には対応する次元のnull SRVを設定します。
            // 未設定の2D画像を表すSRV
            const auto null2D = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_TEXTURE2D);
            // 未設定の2D配列を表すSRV
            const auto nullArray = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
            // 未設定のキューブを表すSRV
            const auto nullCube = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_TEXTURECUBE);
            // 未設定の3D画像を表すSRV
            const auto null3D = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_TEXTURE3D);
            // 未設定のバッファを表すSRV
            const auto nullBuffer = m_backend->NullShaderResourceDescriptor(
                D3D12_SRV_DIMENSION_BUFFER);
            textures[2] = activeLighting.directionalShadow
                ? directionalShadow->descriptor
                : nullArray;
            // t3は事前畳み込み反射または元キューブ、t6は放射照度です。
            textures[3] = activeLighting.environment
                ? (activeLighting.prefilteredEnvironment
                    ? environmentSpecular->descriptor
                    : environment->descriptor)
                : nullCube;
            textures[4] = activeLighting.spotShadow
                ? spotShadow->descriptor
                : nullArray;
            textures[5] = activeLighting.pointShadow
                ? pointShadow->descriptor
                : nullCube;
            textures[6] = activeLighting.prefilteredEnvironment
                ? environmentIrradiance->descriptor
                : nullCube;
            // t15のSSAOは未設定時に白画像を使い、遮蔽を適用しません。
            textures[15] = activeLighting.screenAmbientOcclusion
                ? occlusion->descriptor
                : white->descriptor;
            textures[16] = activeLighting.clustered
                ? clusterLights->descriptor
                : nullBuffer;
            textures[17] = activeLighting.clustered
                ? clusterIndices->descriptor
                : nullBuffer;
            textures[18] = activeLighting.clustered
                ? clusterCounts->descriptor
                : nullBuffer;
            // 有効な反射プローブは環境IBLを上書きし、第2プローブをt19・t20へ設定します。
            // 反射プローブの解決結果
            const auto probe = ResolveD3D12ReflectionProbe(
                *m_backend,
                request.reflectionProbe);
            if (probe.active)
            {
                textures[3] = probe.specular;
                textures[6] = probe.irradiance;
                lightingConstants.environmentParameters =
                    probe.environmentParameters;
                lightingConstants.reflectionBoxCenter = probe.boxCenter;
                lightingConstants.reflectionBoxParameters =
                    probe.boxParameters;
                lightingConstants.reflectionSecondaryBoxCenter =
                    probe.secondaryBoxCenter;
                lightingConstants.reflectionSecondaryBoxParameters =
                    probe.secondaryBoxParameters;
                lightingConstants.reflectionBlendParameters =
                    probe.blendParameters;
            }
            textures[19] = probe.blended
                ? probe.secondarySpecular
                : nullCube;
            textures[20] = probe.blended
                ? probe.secondaryIrradiance
                : nullCube;
            textures[21] = activeLighting.screenReflection
                ? reflectionColor->descriptor
                : null2D;
            textures[22] = activeLighting.screenReflection
                ? reflectionDepth->descriptor
                : null2D;
            // t23～t25は間接光のRGB別L1係数の体積画像です。
            textures[23] = activeLighting.bakedGlobalIllumination
                ? bakedGiRed->descriptor
                : null3D;
            textures[24] = activeLighting.bakedGlobalIllumination
                ? bakedGiGreen->descriptor
                : null3D;
            textures[25] = activeLighting.bakedGlobalIllumination
                ? bakedGiBlue->descriptor
                : null3D;
        }


        // 変換行列と素材のb0定数
        ObjectConstants object;
        object.world = request.world;
        // ワールド変換行列
        const auto world = DirectX::XMLoadFloat4x4(&request.world);
        // カメラのビュー行列
        const auto view = DirectX::XMLoadFloat4x4(&request.view);
        // カメラの射影行列
        const auto projection = DirectX::XMLoadFloat4x4(&request.projection);
        DirectX::XMStoreFloat4x4(&object.viewProjection, view * projection);
        // 逆行列計算で得る行列式
        DirectX::XMVECTOR determinant{};
        DirectX::XMStoreFloat4x4(
            &object.worldInverseTranspose,
            DirectX::XMMatrixTranspose(
                DirectX::XMMatrixInverse(&determinant, world)));
        // カメラのワールド変換
        const auto inverseView = DirectX::XMMatrixInverse(&determinant, view);
        DirectX::XMStoreFloat4(&object.cameraPosition, inverseView.r[3]);
        DirectX::XMStoreFloat4(
            &object.cameraForward,
            DirectX::XMVector3Normalize(
                DirectX::XMVectorNegate(inverseView.r[2])));
        // 浮動小数点秒の循環周期
        constexpr double TimeWrapSeconds = 3600.0;
        object.timeParameters = {
            static_cast<float>(
                std::fmod(Time::TimeSinceStartup(), TimeWrapSeconds)),
            Time::DeltaTime(),
            static_cast<float>(Time::FrameCount() & 0xFFFFFFull),
            0.0f
        };
        object.materialColor = litMaterial.BaseColor();
        object.materialParameters = {
            litMaterial.Roughness(),
            litMaterial.NormalStrength(),
            request.normalTexture ? 1.0f : 0.0f,
            litMaterial.Metallic()
        };
        object.customParameters = litMaterial.CustomParameters();
        object.materialTextureParameters = {
            request.roughnessTexture ? 1.0f : 0.0f,
            request.metallicTexture ? 1.0f : 0.0f,
            request.occlusionTexture ? 1.0f : 0.0f,
            request.occlusionStrength
        };
        object.emissiveParameters = {
            request.emissiveFactor.x,
            request.emissiveFactor.y,
            request.emissiveFactor.z,
            request.emissiveTexture ? 1.0f : 0.0f
        };

        // パッチは4制御点を索引なしで描き、通常形状は索引列を使います。
        // 4制御点パッチの頂点バイト列
        const std::span<const std::uint8_t> patchVertices(
            reinterpret_cast<const std::uint8_t*>(
                material.tessellationPatches.data()),
            material.tessellationPatches.size_bytes());
        // GPUへコピーする頂点列
        const auto uploadVertices = key.tessellated
            ? patchVertices
            : drawVertices;
        // 頂点列のバイト数
        const auto vertexBytes =
            static_cast<std::uint64_t>(uploadVertices.size());
        // 索引列のバイト数
        const auto indexBytes = key.tessellated
            ? std::uint64_t{}
            : static_cast<std::uint64_t>(drawIndices.size_bytes());
        if (vertexBytes > std::numeric_limits<UINT>::max()
            || indexBytes > std::numeric_limits<UINT>::max())
        {
            throw std::length_error(
                "The DirectX 12 material shader primitive is too large.");
        }
        // 描画命令を記録するリスト
        auto* const commandList = depthOnly
            ? m_backend->CurrentFrameCommands()
            : m_backend->BeginFrameCommands();
        // フレーム所有の頂点書込領域
        const auto vertexUpload = m_backend->AllocateFrameUpload(
            vertexBytes,
            alignof(float));
        std::memcpy(
            vertexUpload.data,
            uploadVertices.data(),
            uploadVertices.size());
        // フレーム所有の行列・色の領域
        D3D12Backend::FrameUploadAllocation instanceUpload{};
        if (instanced)
        {
            instanceUpload = m_backend->AllocateFrameUpload(
                static_cast<std::uint64_t>(material.instances.size_bytes()),
                alignof(float));
            std::memcpy(
                instanceUpload.data,
                material.instances.data(),
                material.instances.size_bytes());
        }
        // フレーム所有の索引書込領域
        D3D12Backend::FrameUploadAllocation indexUpload{};
        if (!key.tessellated)
        {
            indexUpload = m_backend->AllocateFrameUpload(
                indexBytes,
                alignof(std::uint32_t));
            std::memcpy(
                indexUpload.data,
                drawIndices.data(),
                drawIndices.size_bytes());
        }
        // 256バイト整列の定数をコピーしGPUアドレスを返します(value: コピーする定数構造体)。
        const auto uploadConstants = [this](const auto& value)
        {
            // 定数用の整列済み書込領域
            const auto allocation = m_backend->AllocateFrameUpload(
                sizeof(value),
                D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            std::memcpy(allocation.data, &value, sizeof(value));
            return allocation.gpuAddress;
        };
        // b0～b3のGPUアドレス
        std::array<D3D12_GPU_VIRTUAL_ADDRESS, ConstantBufferCount>
            constantBuffers{};
        constantBuffers[0] = uploadConstants(object);
        if (active->constantBuffers[1])
        {
            constantBuffers[1] = uploadConstants(lightingConstants);
        }
        if (active->constantBuffers[2])
        {
            // 素材VSが読むb2と互換内蔵VSが読むb4へ骨行列を送り、未指定分は単位行列で埋めます。
            // 素材b2用の骨定数
            BoneConstants bones;
            // 未指定の骨に使う単位行列
            DirectX::XMFLOAT3X4 identity{};
            DirectX::XMStoreFloat3x4(&identity, DirectX::XMMatrixIdentity());
            bones.transforms.fill(identity);
            if (skinned != nullptr)
            {
                // 上限72本で切る入力骨数
                const auto boneCount =
                    std::min(skinned->bones.size(), bones.transforms.size());
                // コピーする骨行列の番号
                for (std::size_t bone{}; bone < boneCount; ++bone)
                {
                    DirectX::XMStoreFloat3x4(
                        &bones.transforms[bone],
                        DirectX::XMLoadFloat4x4(&skinned->bones[bone]));
                }
            }
            constantBuffers[2] = uploadConstants(bones);
        }
        if (active->constantBuffers[3])
        {
            // 素材b3用の任意ベクトル
            CustomVectorConstants vectors;
            vectors.vectors = litMaterial.CustomVectors();
            constantBuffers[3] = uploadConstants(vectors);
        }
        // 互換骨変形b4のGPUアドレス
        D3D12_GPU_VIRTUAL_ADDRESS skinningBuffer{};
        if (skinned != nullptr)
        {
            // 互換内蔵VSには白色RGBと素材アルファを渡し、霧の係数は0のままにします。
            // 互換頂点段b4の変換と骨定数
            SkinningConstants skinning;
            skinning.world = request.world;
            DirectX::XMStoreFloat4x4(
                &skinning.worldInverseTranspose,
                DirectX::XMMatrixTranspose(
                    DirectX::XMMatrixInverse(&determinant, world)));
            DirectX::XMStoreFloat4x4(
                &skinning.worldViewProjection,
                DirectX::XMMatrixMultiply(
                    DirectX::XMMatrixMultiply(world, view),
                    projection));
            skinning.diffuseColor = {
                1.0f,
                1.0f,
                1.0f,
                litMaterial.BaseColor().w
            };
            // 未指定の骨に使う単位行列
            DirectX::XMFLOAT3X4 identity{};
            DirectX::XMStoreFloat3x4(&identity, DirectX::XMMatrixIdentity());
            skinning.bones.fill(identity);
            // 上限72本で切る入力骨数
            const auto boneCount =
                std::min(skinned->bones.size(), skinning.bones.size());
            // コピーする骨行列の番号
            for (std::size_t bone{}; bone < boneCount; ++bone)
            {
                DirectX::XMStoreFloat3x4(
                    &skinning.bones[bone],
                    DirectX::XMLoadFloat4x4(&skinned->bones[bone]));
            }
            skinningBuffer = uploadConstants(skinning);
        }

        commandList->SetGraphicsRootSignature(
            key.pointSampler
                ? m_pointRootSignature.Get()
                : m_linearRootSignature.Get());
        commandList->SetPipelineState(pipeline);
        // 定数または画像の枠番号
        for (UINT index{}; index < ConstantBufferCount; ++index)
        {
            if (constantBuffers[index] != 0u)
            {
                commandList->SetGraphicsRootConstantBufferView(
                    index,
                    constantBuffers[index]);
            }
        }
        if (skinningBuffer != 0u)
        {
            commandList->SetGraphicsRootConstantBufferView(
                SkinningParameter,
                skinningBuffer);
        }
        if (!depthOnly)
        {
            // 設定する読み取りヒープ配列
            ID3D12DescriptorHeap* heaps[]{ descriptorHeap };
            commandList->SetDescriptorHeaps(1, heaps);
            // 定数または画像の枠番号
            for (UINT index{}; index < TextureSlotCount; ++index)
            {
                commandList->SetGraphicsRootDescriptorTable(
                    static_cast<UINT>(ConstantBufferCount) + index,
                    textures[index]);
            }
        }
        // 主頂点列のGPU入力設定
        const D3D12_VERTEX_BUFFER_VIEW vertexView{
            vertexUpload.gpuAddress,
            static_cast<UINT>(vertexBytes),
            vertexStride
        };
        if (instanced)
        {
            // 主頂点とインスタンスの入力列
            const std::array<D3D12_VERTEX_BUFFER_VIEW, 2> vertexViews{ {
                vertexView,
                {
                    instanceUpload.gpuAddress,
                    static_cast<UINT>(material.instances.size_bytes()),
                    static_cast<UINT>(sizeof(MaterialShaderInstanceData))
                }
            } };
            commandList->IASetVertexBuffers(
                0,
                static_cast<UINT>(vertexViews.size()),
                vertexViews.data());
        }
        else
        {
            commandList->IASetVertexBuffers(0, 1, &vertexView);
        }
        if (key.tessellated)
        {
            commandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
        }
        else
        {
            // 32ビット索引列のGPU入力設定
            const D3D12_INDEX_BUFFER_VIEW indexView{
                indexUpload.gpuAddress,
                static_cast<UINT>(indexBytes),
                DXGI_FORMAT_R32_UINT
            };
            commandList->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->IASetIndexBuffer(&indexView);
        }
        if (!depthOnly)
        {
            // 現在の描画先のビューポート
            const auto& viewport = m_backend->ActiveViewport();
            // 現在の描画先の切り取り範囲
            const auto& scissor = m_backend->ActiveScissorRectangle();
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
        }
        if (key.tessellated)
        {
            commandList->DrawInstanced(
                static_cast<UINT>(material.tessellationPatches.size()),
                1,
                0,
                0);
        }
        else
        {
            commandList->DrawIndexedInstanced(
                static_cast<UINT>(drawIndices.size()),
                instanced
                    ? static_cast<UINT>(material.instances.size())
                    : 1u,
                0,
                0,
                0);
        }
        result.drawn = true;
        return result;
    }

    void D3D12MaterialShaderRenderer::Invalidate(
        const std::filesystem::path& shaderPath) noexcept
    {
        try
        {
            // 完全一致または「パス?」で始まる全キーワード版の再読み込みを要求します。
            // 無効化する元HLSLのパス
            const auto prefix = shaderPath.wstring();
            // 通常または骨変形キャッシュ
            for (auto* const shaders : { &m_shaders, &m_skinnedShaders })
            {
                // key: キーワード別のキー、entry: 無効化するコードと状態
                for (auto& [key, entry] : *shaders)
                {
                    // キーワードを含むキャッシュキー
                    const auto text = key.wstring();
                    if (text == prefix
                        || (text.size() > prefix.size()
                            && text.rfind(prefix, 0) == 0
                            && text[prefix.size()] == L'?'))
                    {
                        entry.forceReload = true;
                    }
                }
            }
        }
        catch (...)
        {
        }
    }

    MaterialShaderPasses D3D12MaterialShaderRenderer::PreparePasses(
        AssetManager& assets,
        const MaterialShaderSource& shader)
    {
        // 準備した通常シェーダー
        const auto& entry = Prepare(assets, shader, false);
        // 使用可能な追加パスの有無
        MaterialShaderPasses passes;
        // 主VS・PSが無効またはテセレーションありなら、追加パスなしとして返します。
        if (entry.vertexShader == nullptr
            || entry.pixelShader == nullptr
            || entry.hasTessellation)
        {
            return passes;
        }
        passes.outline = entry.outlineVertexShader != nullptr;
        passes.occluded = entry.occludedPixelShader != nullptr;
        passes.instanced = entry.instancedVertexShader != nullptr;
        return passes;
    }

    bool D3D12MaterialShaderRenderer::TryGetRenderState(
        const std::filesystem::path& cacheKey,
        ShaderRenderState& state) const noexcept
    {
        try
        {
            // 通常キャッシュの検索結果
            const auto found = m_shaders.find(cacheKey);
            if (found == m_shaders.end()
                || found->second.vertexShader == nullptr
                || found->second.pixelShader == nullptr)
            {
                return false;
            }
            state = found->second.renderState;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
}
