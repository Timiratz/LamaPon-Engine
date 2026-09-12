#include "LamaPon/Graphics/D3D12SpriteRenderer.h"

#include "LamaPon/Graphics/D3D12Backend.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    // DirectXTK SpriteBatchと同じく、1回のdrawで送るquad数の上限です。
    // 16bit indexをBaseVertexLocationと組み合わせて全quadへ再利用します。
    constexpr std::size_t MaximumSpritesPerDraw = 2048u;
    constexpr std::size_t VerticesPerSprite = 4u;
    constexpr std::size_t IndicesPerSprite = 6u;

    // SpriteBatchのviewport変換と同じ式で、pixel座標をclip空間へ移します。
    constexpr char SpriteShaderSource[] = R"(
cbuffer SpriteViewport : register(b0)
{
    float2 ViewportScale;
};

Texture2D SpriteTexture : register(t0);
SamplerState SpriteSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float2 textureCoordinate : TEXCOORD;
};

struct PixelInput
{
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 textureCoordinate : TEXCOORD;
};

PixelInput SpriteVertexShader(VertexInput input)
{
    PixelInput output;
    output.position = float4(
        input.position.x * ViewportScale.x - 1.0f,
        1.0f - input.position.y * ViewportScale.y,
        input.position.z,
        1.0f);
    output.color = input.color;
    output.textureCoordinate = input.textureCoordinate;
    return output;
}

float4 SpritePixelShader(PixelInput input) : SV_Target
{
    return SpriteTexture.Sample(SpriteSampler, input.textureCoordinate)
        * input.color;
}

float4 ToneMappedPixelShader(PixelInput input) : SV_Target
{
    const float4 sampled = SpriteTexture.Sample(
        SpriteSampler,
        input.textureCoordinate) * input.color;
    const float3 mapped = saturate(
        sampled.rgb * (2.51f * sampled.rgb + 0.03f)
        / (sampled.rgb * (2.43f * sampled.rgb + 0.59f) + 0.14f));
    return float4(pow(mapped, 1.0f / 2.2f), sampled.a);
}
)";

    void ThrowIfFailed(
        const HRESULT result,
        const char* const operation)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(
                std::string(operation)
                + " failed with HRESULT "
                + std::to_string(static_cast<unsigned long>(result)));
        }
    }

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3DBlob> CompileSpriteShader(
        const char* const entryPoint,
        const char* const target)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            SpriteShaderSource,
            sizeof(SpriteShaderSource) - 1u,
            "LamaPonD3D12Sprite",
            nullptr,
            nullptr,
            entryPoint,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS
                | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            bytecode.GetAddressOf(),
            errors.GetAddressOf());
        if (FAILED(result))
        {
            std::string message =
                std::string("D3DCompile(") + entryPoint + ") failed";
            if (errors != nullptr && errors->GetBufferSize() > 0)
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

    [[nodiscard]] D3D12_BLEND_DESC MakeBlendDescription(
        const LamaPon::SpriteBlendMode blend)
    {
        // DirectXTK CommonStatesと同じ係数を、色とalphaの両方へ使います。
        D3D12_BLEND source = D3D12_BLEND_ONE;
        D3D12_BLEND destination = D3D12_BLEND_ZERO;
        switch (blend)
        {
        case LamaPon::SpriteBlendMode::NonPremultiplied:
            source = D3D12_BLEND_SRC_ALPHA;
            destination = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case LamaPon::SpriteBlendMode::AlphaBlend:
            destination = D3D12_BLEND_INV_SRC_ALPHA;
            break;
        case LamaPon::SpriteBlendMode::Additive:
            source = D3D12_BLEND_SRC_ALPHA;
            destination = D3D12_BLEND_ONE;
            break;
        case LamaPon::SpriteBlendMode::Opaque:
            break;
        default:
            throw std::invalid_argument(
                "The sprite blend mode is invalid.");
        }

        D3D12_RENDER_TARGET_BLEND_DESC target{};
        target.BlendEnable =
            source != D3D12_BLEND_ONE || destination != D3D12_BLEND_ZERO;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = source;
        target.DestBlend = destination;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = source;
        target.DestBlendAlpha = destination;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_BLEND_DESC description{};
        description.AlphaToCoverageEnable = FALSE;
        description.IndependentBlendEnable = FALSE;
        for (auto& renderTarget : description.RenderTarget)
        {
            renderTarget = target;
        }
        return description;
    }

    [[nodiscard]] D3D12_RASTERIZER_DESC MakeRasterizerDescription(
        const bool scissored) noexcept
    {
        // 通常passはSpriteBatch既定の反時計回りcull、scissor passはD3D11の
        // UI clipping rasterizerと同じくcullしません。
        D3D12_RASTERIZER_DESC description{};
        description.FillMode = D3D12_FILL_MODE_SOLID;
        description.CullMode = scissored
            ? D3D12_CULL_MODE_NONE
            : D3D12_CULL_MODE_BACK;
        description.FrontCounterClockwise = FALSE;
        description.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        description.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        description.SlopeScaledDepthBias =
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        description.DepthClipEnable = TRUE;
        description.MultisampleEnable = FALSE;
        description.AntialiasedLineEnable = FALSE;
        description.ForcedSampleCount = 0;
        description.ConservativeRaster =
            D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        return description;
    }

    [[nodiscard]] D3D12_DEPTH_STENCIL_DESC
        MakeDepthStencilDescription() noexcept
    {
        const D3D12_DEPTH_STENCILOP_DESC keep{
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP,
            D3D12_COMPARISON_FUNC_ALWAYS
        };
        D3D12_DEPTH_STENCIL_DESC description{};
        description.DepthEnable = FALSE;
        description.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        description.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        description.StencilEnable = FALSE;
        description.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        description.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        description.FrontFace = keep;
        description.BackFace = keep;
        return description;
    }

    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT2& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y);
    }

    [[nodiscard]] bool IsFinite(
        const DirectX::XMFLOAT4& value) noexcept
    {
        return std::isfinite(value.x)
            && std::isfinite(value.y)
            && std::isfinite(value.z)
            && std::isfinite(value.w);
    }

    [[nodiscard]] bool IsFinite(
        const LamaPon::SpriteClipRectangle& value) noexcept
    {
        return std::isfinite(value.minimumX)
            && std::isfinite(value.minimumY)
            && std::isfinite(value.maximumX)
            && std::isfinite(value.maximumY);
    }

    // D3D11のUI scissorと同じく、外側のclipとの交差へ畳み込みます。
    [[nodiscard]] D3D12_RECT MakeScissorRectangle(
        const LamaPon::SpriteClipRectangle& rectangle,
        const std::vector<D3D12_RECT>& stack)
    {
        const auto clampLong = [](const float value) noexcept
        {
            return static_cast<LONG>(
                std::clamp(
                    static_cast<double>(value),
                    0.0,
                    static_cast<double>(
                        (std::numeric_limits<LONG>::max)())));
        };
        D3D12_RECT result{
            clampLong(rectangle.minimumX),
            clampLong(rectangle.minimumY),
            clampLong(rectangle.maximumX),
            clampLong(rectangle.maximumY) };
        if (!stack.empty())
        {
            const auto& outer = stack.back();
            result.left = std::max(result.left, outer.left);
            result.top = std::max(result.top, outer.top);
            result.right = std::min(result.right, outer.right);
            result.bottom = std::min(result.bottom, outer.bottom);
        }
        result.right = std::max(result.right, result.left);
        result.bottom = std::max(result.bottom, result.top);
        return result;
    }
}

namespace LamaPon::Detail
{
    D3D12SpriteRenderer::D3D12SpriteRenderer(D3D12Backend& backend)
        : m_backend(&backend)
    {
        if (!backend.IsInitialized())
        {
            throw std::invalid_argument(
                "The DirectX 12 sprite renderer requires an initialized "
                "backend.");
        }
        auto* const device = backend.Device();

        m_vertexShader = CompileSpriteShader(
            "SpriteVertexShader",
            "vs_5_0");
        m_pixelShader = CompileSpriteShader(
            "SpritePixelShader",
            "ps_5_0");
        m_toneMapPixelShader = CompileSpriteShader(
            "ToneMappedPixelShader",
            "ps_5_0");

        D3D12_DESCRIPTOR_RANGE textureRange{};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = 1;
        textureRange.BaseShaderRegister = 0;
        textureRange.RegisterSpace = 0;
        textureRange.OffsetInDescriptorsFromTableStart = 0;

        std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
        parameters[0].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.RegisterSpace = 0;
        parameters[0].Constants.Num32BitValues = 2;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters[1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &textureRange;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // SpriteBatch既定のLinearClampと同じsamplerです。
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MipLODBias = 0.0f;
        sampler.MaxAnisotropy = D3D12_MAX_MAXANISOTROPY;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters =
            static_cast<UINT>(parameters.size());
        rootDescription.pParameters = parameters.data();
        rootDescription.NumStaticSamplers = 1;
        rootDescription.pStaticSamplers = &sampler;
        rootDescription.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Microsoft::WRL::ComPtr<ID3DBlob> serializedRoot;
        Microsoft::WRL::ComPtr<ID3DBlob> rootErrors;
        const HRESULT serialized = D3D12SerializeRootSignature(
            &rootDescription,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRoot.GetAddressOf(),
            rootErrors.GetAddressOf());
        if (FAILED(serialized))
        {
            std::string message =
                "D3D12SerializeRootSignature(sprite) failed";
            if (rootErrors != nullptr && rootErrors->GetBufferSize() > 0)
            {
                message += ": ";
                message.append(
                    static_cast<const char*>(
                        rootErrors->GetBufferPointer()),
                    rootErrors->GetBufferSize());
            }
            throw std::runtime_error(message);
        }
        ThrowIfFailed(
            device->CreateRootSignature(
                0,
                serializedRoot->GetBufferPointer(),
                serializedRoot->GetBufferSize(),
                IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateRootSignature(sprite)");

        const auto indexBytes =
            MaximumSpritesPerDraw * IndicesPerSprite
            * sizeof(std::uint16_t);
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        uploadHeap.CreationNodeMask = 1;
        uploadHeap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC indexDescription{};
        indexDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        indexDescription.Width = indexBytes;
        indexDescription.Height = 1;
        indexDescription.DepthOrArraySize = 1;
        indexDescription.MipLevels = 1;
        indexDescription.Format = DXGI_FORMAT_UNKNOWN;
        indexDescription.SampleDesc.Count = 1;
        indexDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(
            device->CreateCommittedResource(
                &uploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &indexDescription,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(m_indexBuffer.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateCommittedResource(sprite indices)");
        void* mapped{};
        const D3D12_RANGE noRead{};
        ThrowIfFailed(
            m_indexBuffer->Map(0, &noRead, &mapped),
            "ID3D12Resource::Map(sprite indices)");
        auto* const indices = static_cast<std::uint16_t*>(mapped);
        for (std::size_t sprite{}; sprite < MaximumSpritesPerDraw; ++sprite)
        {
            // SpriteBatchと同じ三角形の並びです。
            const auto first =
                static_cast<std::uint16_t>(sprite * VerticesPerSprite);
            auto* const quad = indices + sprite * IndicesPerSprite;
            quad[0] = first;
            quad[1] = static_cast<std::uint16_t>(first + 1u);
            quad[2] = static_cast<std::uint16_t>(first + 2u);
            quad[3] = static_cast<std::uint16_t>(first + 1u);
            quad[4] = static_cast<std::uint16_t>(first + 3u);
            quad[5] = static_cast<std::uint16_t>(first + 2u);
        }
        m_indexBuffer->Unmap(0, nullptr);
    }

    D3D12SpriteRenderer::~D3D12SpriteRenderer() noexcept = default;

    std::uint64_t D3D12SpriteRenderer::Begin(
        const SpritePassDescription& description,
        const GraphicsViewHandle& fallbackTexture,
        SpriteShaderStatus& status)
    {
        if (m_failed)
        {
            throw std::logic_error(
                "The DirectX 12 sprite renderer failed and must be "
                "recovered by reinitializing the graphics device.");
        }
        if (m_activeToken != 0)
        {
            throw std::logic_error(
                "A sprite render pass is already active.");
        }
        if (description.blend > SpriteBlendMode::Opaque)
        {
            throw std::invalid_argument(
                "The sprite blend mode is invalid.");
        }

        SpriteShaderStatus preparedStatus;
        if (!description.pixelShader.empty())
        {
            // D3D11でcompileに失敗した場合と同じく、既定pipelineで描いて
            // 呼び出し側へ理由を返します。
            preparedStatus.fallback =
                SpriteShaderFallback::DefaultPipeline;
            preparedStatus.error =
                "Custom sprite pixel shaders are not implemented for "
                "DirectX 12 Experimental.";
        }
        auto token = m_nextToken++;
        if (token == 0)
        {
            token = m_nextToken++;
        }

        status = std::move(preparedStatus);
        m_blend = description.blend;
        m_toneMapped = false;
        m_fallbackTexture = fallbackTexture;
        m_sprites.clear();
        m_scissorStack.clear();
        m_activeToken = token;
        return token;
    }

    bool D3D12SpriteRenderer::Draw(
        const std::uint64_t token,
        const SpriteDrawRequest& request)
    {
        RequireOwner(token);
        if (!IsFinite(request.position)
            || !IsFinite(request.tint)
            || !std::isfinite(request.rotation)
            || !IsFinite(request.origin)
            || !IsFinite(request.scale)
            || !std::isfinite(request.layerDepth))
        {
            return false;
        }
        if (request.hasSourceRectangle
            && (request.sourceRectangle.right
                    <= request.sourceRectangle.left
                || request.sourceRectangle.bottom
                    <= request.sourceRectangle.top))
        {
            return false;
        }
        if (request.flip > SpriteFlip::Both)
        {
            return false;
        }

        const auto& view = request.texture
            ? request.texture
            : m_fallbackTexture;
        const auto binding = m_backend->TryResolveShaderResource(view);
        if (!binding)
        {
            return false;
        }

        // DirectXTK SpriteBatchと同じ演算順で、source rectangleの有無ごとに
        // 正規化UV、原点、表示寸法を求めます。
        const float textureWidth = static_cast<float>(binding->width);
        const float textureHeight = static_cast<float>(binding->height);
        const float inverseTextureWidth = 1.0f / textureWidth;
        const float inverseTextureHeight = 1.0f / textureHeight;
        float sourceX = 0.0f;
        float sourceY = 0.0f;
        float sourceWidth = 1.0f;
        float sourceHeight = 1.0f;
        float originX{};
        float originY{};
        float destinationWidth{};
        float destinationHeight{};
        if (request.hasSourceRectangle)
        {
            const auto left =
                static_cast<float>(request.sourceRectangle.left);
            const auto top =
                static_cast<float>(request.sourceRectangle.top);
            const float texelWidth =
                static_cast<float>(request.sourceRectangle.right) - left;
            const float texelHeight =
                static_cast<float>(request.sourceRectangle.bottom) - top;
            destinationWidth = request.scale.x * texelWidth;
            destinationHeight = request.scale.y * texelHeight;
            originX = request.origin.x / texelWidth;
            originY = request.origin.y / texelHeight;
            sourceX = left * inverseTextureWidth;
            sourceY = top * inverseTextureHeight;
            sourceWidth = texelWidth * inverseTextureWidth;
            sourceHeight = texelHeight * inverseTextureHeight;
        }
        else
        {
            destinationWidth = request.scale.x * textureWidth;
            destinationHeight = request.scale.y * textureHeight;
            originX = request.origin.x * inverseTextureWidth;
            originY = request.origin.y * inverseTextureHeight;
        }

        float rotationSin = 0.0f;
        float rotationCos = 1.0f;
        const bool rotated = request.rotation != 0.0f;
        if (rotated)
        {
            DirectX::XMScalarSinCos(
                &rotationSin,
                &rotationCos,
                request.rotation);
        }

        // flipはcorner表のindexをbitで入れ替え、頂点位置は変えずにUVだけを
        // 反転します（Horizontal=1、Vertical=2）。
        static constexpr std::array<DirectX::XMFLOAT2, VerticesPerSprite>
            CornerOffsets{ {
                { 0.0f, 0.0f },
                { 1.0f, 0.0f },
                { 0.0f, 1.0f },
                { 1.0f, 1.0f }
            } };
        const auto mirrorBits =
            static_cast<std::size_t>(request.flip) & 3u;

        QueuedSprite sprite;
        sprite.texture = binding->descriptor;
        sprite.view = view;
        for (std::size_t corner{}; corner < VerticesPerSprite; ++corner)
        {
            const float cornerX =
                (CornerOffsets[corner].x - originX) * destinationWidth;
            const float cornerY =
                (CornerOffsets[corner].y - originY) * destinationHeight;
            auto& vertex = sprite.vertices[corner];
            if (rotated)
            {
                vertex.position.x =
                    (cornerX * rotationCos + cornerY * -rotationSin)
                    + request.position.x;
                vertex.position.y =
                    (cornerX * rotationSin + cornerY * rotationCos)
                    + request.position.y;
            }
            else
            {
                vertex.position.x = cornerX + request.position.x;
                vertex.position.y = cornerY + request.position.y;
            }
            vertex.position.z = request.layerDepth;
            vertex.color = request.tint;
            const auto& textureCorner = CornerOffsets[corner ^ mirrorBits];
            vertex.textureCoordinate = {
                textureCorner.x * sourceWidth + sourceX,
                textureCorner.y * sourceHeight + sourceY
            };
        }
        m_sprites.push_back(std::move(sprite));
        return true;
    }

    bool D3D12SpriteRenderer::PushScissor(
        const std::uint64_t token,
        const SpriteClipRectangle& rectangle)
    {
        RequireOwner(token);
        if (!IsFinite(rectangle))
        {
            return false;
        }
        auto nextScissors = m_scissorStack;
        nextScissors.push_back(
            MakeScissorRectangle(rectangle, nextScissors));
        // D3D11と同じく、それまでのSpriteを現在のclipで確定してから
        // 次のclipへ切り替えます。
        FlushOrFail();
        m_scissorStack.swap(nextScissors);
        return true;
    }

    bool D3D12SpriteRenderer::PopScissor(const std::uint64_t token)
    {
        RequireOwner(token);
        if (m_scissorStack.empty())
        {
            return false;
        }
        FlushOrFail();
        m_scissorStack.pop_back();
        return true;
    }

    void D3D12SpriteRenderer::End(const std::uint64_t token)
    {
        RequireOwner(token);
        FlushOrFail();
        ClearPass();
    }

    void D3D12SpriteRenderer::Abort(const std::uint64_t token) noexcept
    {
        if (token == 0 || token != m_activeToken)
        {
            return;
        }
        try
        {
            Flush();
        }
        catch (...)
        {
            m_failed = true;
        }
        ClearPass();
    }

    void D3D12SpriteRenderer::CompositeToneMapped(
        const GraphicsViewHandle& texture,
        const GraphicsViewHandle& fallbackTexture)
    {
        const auto binding = m_backend->TryResolveShaderResource(texture);
        const auto& viewport = m_backend->ActiveViewport();
        if (!binding
            || binding->width == 0u
            || binding->height == 0u
            || viewport.Width <= 0.0f
            || viewport.Height <= 0.0f)
        {
            throw std::invalid_argument(
                "Tone-mapped composition requires a current texture and "
                "output viewport.");
        }
        SpritePassDescription description;
        description.blend = SpriteBlendMode::Opaque;
        SpriteShaderStatus status;
        const auto token = Begin(
            description,
            fallbackTexture,
            status);
        m_toneMapped = true;
        try
        {
            SpriteDrawRequest request;
            request.texture = texture;
            request.scale = {
                viewport.Width / static_cast<float>(binding->width),
                viewport.Height / static_cast<float>(binding->height) };
            if (!Draw(token, request))
            {
                throw std::runtime_error(
                    "The HDR scene texture was rejected by the DirectX "
                    "12 compositor.");
            }
            End(token);
        }
        catch (...)
        {
            Abort(token);
            throw;
        }
    }

    void D3D12SpriteRenderer::RequireOwner(
        const std::uint64_t token) const
    {
        if (token == 0 || token != m_activeToken)
        {
            throw std::logic_error(
                "The sprite render pass does not own the active DirectX 12 "
                "batch.");
        }
    }

    void D3D12SpriteRenderer::FlushOrFail()
    {
        try
        {
            Flush();
        }
        catch (...)
        {
            m_failed = true;
            ClearPass();
            throw;
        }
    }

    void D3D12SpriteRenderer::ClearPass() noexcept
    {
        m_sprites.clear();
        m_scissorStack.clear();
        m_fallbackTexture.Reset();
        m_toneMapped = false;
        m_activeToken = 0;
    }

    void D3D12SpriteRenderer::Flush()
    {
        if (m_sprites.empty())
        {
            return;
        }

        auto* const commandList = m_backend->BeginFrameCommands();
        auto* const descriptorHeap =
            m_backend->ShaderResourceDescriptorHeap();
        if (descriptorHeap == nullptr)
        {
            throw std::logic_error(
                "The DirectX 12 sprite renderer requires a shader resource "
                "descriptor heap.");
        }
        const std::uint64_t vertexBytes =
            static_cast<std::uint64_t>(m_sprites.size())
            * sizeof(QueuedSprite::vertices);
        if (vertexBytes > std::numeric_limits<UINT>::max())
        {
            throw std::length_error(
                "The DirectX 12 sprite batch is too large.");
        }
        auto* const pipelineState = PipelineState(
            m_blend,
            !m_scissorStack.empty(),
            m_backend->ActiveColorFormat(),
            m_toneMapped);

        const auto upload = m_backend->AllocateFrameUpload(
            vertexBytes,
            alignof(Vertex));
        auto* vertexData = upload.data;
        for (const auto& sprite : m_sprites)
        {
            std::memcpy(
                vertexData,
                sprite.vertices.data(),
                sizeof(sprite.vertices));
            vertexData += sizeof(sprite.vertices);
        }

        const auto& viewport = m_backend->ActiveViewport();
        const std::array<float, 2> viewportScale{
            viewport.Width > 0.0f ? 2.0f / viewport.Width : 0.0f,
            viewport.Height > 0.0f ? 2.0f / viewport.Height : 0.0f
        };
        const D3D12_RECT scissor = m_scissorStack.empty()
            ? m_backend->ActiveScissorRectangle()
            : m_scissorStack.back();
        const D3D12_VERTEX_BUFFER_VIEW vertexBufferView{
            upload.gpuAddress,
            static_cast<UINT>(vertexBytes),
            static_cast<UINT>(sizeof(Vertex))
        };
        const D3D12_INDEX_BUFFER_VIEW indexBufferView{
            m_indexBuffer->GetGPUVirtualAddress(),
            static_cast<UINT>(
                MaximumSpritesPerDraw * IndicesPerSprite
                * sizeof(std::uint16_t)),
            DXGI_FORMAT_R16_UINT
        };
        ID3D12DescriptorHeap* descriptorHeaps[]{ descriptorHeap };

        commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        commandList->SetPipelineState(pipelineState);
        commandList->SetDescriptorHeaps(
            static_cast<UINT>(std::size(descriptorHeaps)),
            descriptorHeaps);
        commandList->SetGraphicsRoot32BitConstants(
            0,
            static_cast<UINT>(viewportScale.size()),
            viewportScale.data(),
            0);
        commandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        commandList->IASetIndexBuffer(&indexBufferView);
        commandList->RSSetScissorRects(1, &scissor);

        // Deferred順を保ったまま、同じtextureが連続する範囲だけをまとめます。
        std::size_t first{};
        while (first < m_sprites.size())
        {
            std::size_t end = first + 1u;
            while (end < m_sprites.size()
                && end - first < MaximumSpritesPerDraw
                && m_sprites[end].texture.ptr
                    == m_sprites[first].texture.ptr)
            {
                ++end;
            }
            commandList->SetGraphicsRootDescriptorTable(
                1,
                m_sprites[first].texture);
            commandList->DrawIndexedInstanced(
                static_cast<UINT>((end - first) * IndicesPerSprite),
                1,
                0,
                static_cast<INT>(first * VerticesPerSprite),
                0);
            first = end;
        }
        // 後続の描画へSprite用のclipを残しません。
        const auto& fullScissor = m_backend->ActiveScissorRectangle();
        commandList->RSSetScissorRects(1, &fullScissor);
        m_sprites.clear();
    }

    ID3D12PipelineState* D3D12SpriteRenderer::PipelineState(
        const SpriteBlendMode blend,
        const bool scissored,
        const DXGI_FORMAT colorFormat,
        const bool toneMapped)
    {
        const auto blendIndex = static_cast<std::size_t>(blend);
        if (blend > SpriteBlendMode::Opaque)
        {
            throw std::invalid_argument(
                "The sprite blend mode is invalid.");
        }
        const std::size_t formatIndex = colorFormat
                == D3D12Backend::PrimaryColorFormat
            ? 0u
            : colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                ? 1u
                : throw std::invalid_argument(
                    "The active DirectX 12 sprite target format is "
                    "unsupported.");
        auto& pipeline = m_pipelineStates[
            (toneMapped ? 16u : 0u)
            + formatIndex * 8u
            + blendIndex * 2u
            + (scissored ? 1u : 0u)];
        if (pipeline != nullptr)
        {
            return pipeline.Get();
        }

        static const std::array<D3D12_INPUT_ELEMENT_DESC, 3>
            InputElements{ {
                {
                    "POSITION",
                    0,
                    DXGI_FORMAT_R32G32B32_FLOAT,
                    0,
                    offsetof(Vertex, position),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    0
                },
                {
                    "COLOR",
                    0,
                    DXGI_FORMAT_R32G32B32A32_FLOAT,
                    0,
                    offsetof(Vertex, color),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    0
                },
                {
                    "TEXCOORD",
                    0,
                    DXGI_FORMAT_R32G32_FLOAT,
                    0,
                    offsetof(Vertex, textureCoordinate),
                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                    0
                }
            } };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = m_rootSignature.Get();
        description.VS = {
            m_vertexShader->GetBufferPointer(),
            m_vertexShader->GetBufferSize()
        };
        description.PS = {
            toneMapped
                ? m_toneMapPixelShader->GetBufferPointer()
                : m_pixelShader->GetBufferPointer(),
            toneMapped
                ? m_toneMapPixelShader->GetBufferSize()
                : m_pixelShader->GetBufferSize()
        };
        description.BlendState = MakeBlendDescription(blend);
        description.SampleMask = std::numeric_limits<UINT>::max();
        description.RasterizerState = MakeRasterizerDescription(scissored);
        description.DepthStencilState = MakeDepthStencilDescription();
        description.InputLayout = {
            InputElements.data(),
            static_cast<UINT>(InputElements.size())
        };
        description.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.NumRenderTargets = 1;
        // Primary outputには深度bufferもbindされるため、深度testを使わない
        // Spriteでもformatだけは一致させます。
        description.RTVFormats[0] = colorFormat;
        description.DSVFormat = D3D12Backend::PrimaryDepthFormat;
        description.SampleDesc.Count = 1;
        ThrowIfFailed(
            m_backend->Device()->CreateGraphicsPipelineState(
                &description,
                IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf())),
            "ID3D12Device::CreateGraphicsPipelineState(sprite)");
        return pipeline.Get();
    }
}
