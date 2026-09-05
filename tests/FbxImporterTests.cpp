#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/FbxImporter.h"
#include "LamaPon/Assets/ModelCache.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <d3d11.h>
#include <objbase.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool PoseChanged(
        const std::vector<DirectX::XMFLOAT4X4>& left,
        const std::vector<DirectX::XMFLOAT4X4>& right)
    {
        if (left.size() != right.size())
        {
            return true;
        }
        for (std::size_t index = 0;
            index < left.size();
            ++index)
        {
            const float* a =
                reinterpret_cast<const float*>(&left[index]);
            const float* b =
                reinterpret_cast<const float*>(&right[index]);
            for (std::size_t element = 0;
                element < 16;
                ++element)
            {
                if (std::abs(a[element] - b[element])
                    > 0.0001f)
                {
                    return true;
                }
            }
        }
        return false;
    }


    // GPUバッファの中身を読み戻します（IMMUTABLEはSTAGINGへ
    // コピーしてから読みます）。
    [[nodiscard]] std::vector<std::uint8_t> ReadBuffer(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Buffer* buffer)
    {
        D3D11_BUFFER_DESC description{};
        buffer->GetDesc(&description);
        D3D11_BUFFER_DESC staging = description;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        Microsoft::WRL::ComPtr<ID3D11Buffer> copy;
        Require(
            SUCCEEDED(device->CreateBuffer(
                &staging,
                nullptr,
                copy.ReleaseAndGetAddressOf())),
            "staging buffer creation must succeed");
        context->CopyResource(copy.Get(), buffer);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(
            SUCCEEDED(context->Map(
                copy.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped)),
            "staging buffer map must succeed");
        std::vector<std::uint8_t> bytes(description.ByteWidth);
        std::memcpy(
            bytes.data(),
            mapped.pData,
            description.ByteWidth);
        context->Unmap(copy.Get(), 0);
        return bytes;
    }

    int RunTest()
    {
        // モデルキャッシュを専用の置き場で走らせます（本物の
        // %LOCALAPPDATA%を汚さない・前回の実行結果を拾わない）。
        const auto cacheRoot =
            std::filesystem::current_path()
            / "test-output"
            / "model-cache-fbx";
        std::filesystem::remove_all(cacheRoot);
        LamaPon::ModelCache::SetCacheDirectoryOverride(
            cacheRoot);

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL featureLevel{};
        const D3D_FEATURE_LEVEL requested[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        const HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            requested,
            static_cast<UINT>(std::size(requested)),
            D3D11_SDK_VERSION,
            device.ReleaseAndGetAddressOf(),
            &featureLevel,
            context.ReleaseAndGetAddressOf());
        Require(
            SUCCEEDED(result),
            "Unable to create the Direct3D WARP test device.");
        static_cast<void>(featureLevel);

        LamaPon::AssetManager assets(device.Get(), context.Get());
        const auto modelPath =
            std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "AnimatedSausage.fbx";
        const auto model = LamaPon::FbxImporter::Load(
            device.Get(),
            context.Get(),
            assets,
            modelPath);
        Require(model != nullptr, "FBX model was not loaded.");
        Require(!model->nodes.empty(), "FBX nodes are missing.");
        Require(!model->skins.empty(), "FBX skin is missing.");
        Require(
            !model->animations.empty(),
            "FBX animation is missing.");
        Require(
            !model->primitives.empty(),
            "FBX mesh primitive is missing.");
        Require(
            model->hasLocalBounds,
            "FBX model bounds are missing.");
        bool usesSharedVertices{};
        for (const auto& primitive : model->primitives)
        {
            const auto indexBytes = ReadBuffer(
                device.Get(),
                context.Get(),
                primitive.indexBuffer.Get());
            for (std::size_t offset = 0;
                offset + sizeof(std::uint32_t)
                    <= indexBytes.size();
                offset += sizeof(std::uint32_t))
            {
                std::uint32_t index{};
                std::memcpy(
                    &index,
                    indexBytes.data() + offset,
                    sizeof(index));
                if (index
                    != offset / sizeof(std::uint32_t))
                {
                    usesSharedVertices = true;
                    break;
                }
            }
        }
        Require(
            usesSharedVertices,
            "FBX vertices should be shared by the index buffer.");
        Require(
            model->animations.front().duration > 0.0f,
            "FBX animation duration is invalid.");
        Require(
            model->skins.front().joints.size()
                == model->skins.front()
                    .inverseBindMatrices.size(),
            "FBX inverse bind matrix count is invalid.");
        for (const auto& primitive : model->primitives)
        {
            Require(
                primitive.vertexBuffer
                    && primitive.indexBuffer
                    && primitive.inputLayout
                    && primitive.effect,
                "FBX GPU resources were not created.");
        }

        // 2回目はディスクキャッシュから復元されること。FBXは添字を
        // ファイルへ持たず0..N-1を作り直すので、頂点・添字バッファの
        // バイト一致まで確かめます（CPU側の深い比較はglTFのテストが
        // 持っています。ここはFBX特有の経路＝連番の復元と静的結合の
        // 再現を押さえます）。
        Require(
            std::filesystem::exists(cacheRoot)
                && !std::filesystem::is_empty(cacheRoot),
            "the import must write a model cache entry");
        const auto cached = LamaPon::FbxImporter::Load(
            device.Get(),
            context.Get(),
            assets,
            modelPath);
        Require(
            cached != nullptr
                && cached->nodes.size()
                    == model->nodes.size()
                && cached->skins.size()
                    == model->skins.size()
                && cached->animations.size()
                    == model->animations.size()
                && cached->primitives.size()
                    == model->primitives.size(),
            "the cached model must have the same shape");
        Require(
            cached->hasLocalBounds
                && std::memcmp(
                    &cached->localBounds,
                    &model->localBounds,
                    sizeof(model->localBounds)) == 0,
            "cached FBX model bounds must match");
        for (std::size_t index = 0;
            index < model->primitives.size();
            ++index)
        {
            const auto& left = model->primitives[index];
            const auto& right = cached->primitives[index];
            Require(
                left.indexCount == right.indexCount
                    && left.meshNode == right.meshNode
                    && left.skin == right.skin
                    && left.hasLocalBounds
                        == right.hasLocalBounds
                    && (!left.hasLocalBounds
                        || std::memcmp(
                            &left.localBounds,
                            &right.localBounds,
                            sizeof(left.localBounds)) == 0),
                "cached FBX primitive metadata must match");
            Require(
                ReadBuffer(
                    device.Get(),
                    context.Get(),
                    left.vertexBuffer.Get())
                    == ReadBuffer(
                        device.Get(),
                        context.Get(),
                        right.vertexBuffer.Get()),
                "cached FBX vertex bytes must match");
            Require(
                ReadBuffer(
                    device.Get(),
                    context.Get(),
                    left.indexBuffer.Get())
                    == ReadBuffer(
                        device.Get(),
                        context.Get(),
                        right.indexBuffer.Get()),
                "cached FBX index bytes must match");
        }
        // アニメーションも同じ姿勢を出すこと（キーの取り違えを
        // 検出するには、数の一致より姿勢の一致が強い検査です）。
        {
            std::vector<LamaPon::SkeletalPoseTransform> localA;
            std::vector<LamaPon::SkeletalPoseTransform> localB;
            std::vector<DirectX::XMFLOAT4X4> globalA;
            std::vector<DirectX::XMFLOAT4X4> globalB;
            const auto& clip = model->animations.front();
            const auto& cachedClip =
                cached->animations.front();
            LamaPon::SkeletalModel::SamplePose(
                model->nodes,
                &clip,
                clip.duration * 0.5f,
                localA,
                globalA);
            LamaPon::SkeletalModel::SamplePose(
                cached->nodes,
                &cachedClip,
                cachedClip.duration * 0.5f,
                localB,
                globalB);
            Require(
                globalA.size() == globalB.size()
                    && std::memcmp(
                        globalA.data(),
                        globalB.data(),
                        globalA.size()
                            * sizeof(DirectX::XMFLOAT4X4))
                        == 0,
                "the cached animation must sample the same"
                " pose");
        }

        bool animatedPoseChanged{};
        for (const auto& clip : model->animations)
        {
            std::vector<LamaPon::SkeletalPoseTransform> localA;
            std::vector<LamaPon::SkeletalPoseTransform> localB;
            std::vector<DirectX::XMFLOAT4X4> globalA;
            std::vector<DirectX::XMFLOAT4X4> globalB;
            LamaPon::SkeletalModel::SamplePose(
                model->nodes,
                &clip,
                0.0f,
                localA,
                globalA);
            LamaPon::SkeletalModel::SamplePose(
                model->nodes,
                &clip,
                clip.duration * 0.5f,
                localB,
                globalB);
            animatedPoseChanged =
                animatedPoseChanged
                || PoseChanged(globalA, globalB);
        }
        Require(
            animatedPoseChanged,
            "FBX animation did not change the sampled pose.");

        LamaPon::SkeletalAnimationClip blendFrom;
        blendFrom.duration = 1.0f;
        blendFrom.tracks.push_back({});
        blendFrom.tracks.back().node = 0;
        blendFrom.tracks.back().translation.keys = {
            { 0.0f, { 0.0f, 0.0f, 0.0f } }
        };
        LamaPon::SkeletalAnimationClip blendTo;
        blendTo.duration = 1.0f;
        blendTo.tracks.push_back({});
        blendTo.tracks.back().node = 0;
        blendTo.tracks.back().translation.keys = {
            { 0.0f, { 10.0f, 4.0f, -2.0f } }
        };
        std::vector<LamaPon::SkeletalPoseTransform>
            blendedLocal;
        std::vector<DirectX::XMFLOAT4X4>
            blendedGlobal;
        LamaPon::SkeletalModel::SampleBlendedPose(
            model->nodes,
            &blendFrom,
            0.0f,
            &blendTo,
            0.0f,
            0.25f,
            blendedLocal,
            blendedGlobal);
        Require(
            !blendedLocal.empty()
                && std::abs(
                    blendedLocal[0].translation.x
                    - 2.5f) < 0.0001f
                && std::abs(
                    blendedLocal[0].translation.y
                    - 1.0f) < 0.0001f
                && std::abs(
                    blendedLocal[0].translation.z
                    + 0.5f) < 0.0001f,
            "Skeletal pose blending did not interpolate translation.");
        std::vector<LamaPon::SkeletalPoseSample>
            weightedSamples{
                { &blendFrom, 0.0f, 0.75f },
                { &blendTo, 0.0f, 0.25f }
            };
        LamaPon::SkeletalModel::SampleWeightedPose(
            model->nodes,
            weightedSamples,
            blendedLocal,
            blendedGlobal);
        Require(
            !blendedLocal.empty()
                && std::abs(
                    blendedLocal[0].translation.x
                    - 2.5f) < 0.0001f,
            "Weighted skeletal pose sampling produced an incorrect result.");
        LamaPon::SkeletalModel::SampleWeightedPose(
            model->nodes,
            weightedSamples,
            blendedLocal,
            blendedGlobal,
            0);
        Require(
            !blendedLocal.empty()
                && std::abs(
                    blendedLocal[0].translation.x
                    - model->nodes[0]
                        .bindPose.translation.x)
                    < 0.0001f,
            "Root Motion was not removed from the rendered skeleton pose.");

        // キャッシュを消して実際のFBX解析からやり直し、Immediate
        // Contextを使わないワーカー経路でもGPUリソースと境界を
        // 完成できることを確認します。
        std::filesystem::remove_all(cacheRoot);
        assets.Invalidate(modelPath);
        Require(
            assets.PrepareModelAsync(modelPath),
            "asynchronous FBX preparation must start");
        LamaPon::ModelPreparationState preparationState =
            LamaPon::ModelPreparationState::Pending;
        std::string preparationError;
        const auto preparationDeadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now()
            < preparationDeadline)
        {
            preparationState =
                assets.PollModelPreparation(
                    modelPath,
                    &preparationError);
            if (preparationState
                != LamaPon::ModelPreparationState::Pending)
            {
                break;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        if (preparationState
            != LamaPon::ModelPreparationState::Ready)
        {
            throw std::runtime_error(
                "asynchronous FBX preparation failed: "
                + (preparationError.empty()
                    ? "timeout or stale result"
                    : preparationError));
        }
        const auto prepared = assets.LoadModel(modelPath);
        Require(
            prepared != nullptr
                && prepared->skeletalModel != nullptr
                && prepared->hasLocalBounds,
            "prepared FBX asset must be cached with bounds");

        std::cout
            << "FBX import: "
            << model->nodes.size() << " nodes, "
            << model->skins.size() << " skin, "
            << model->animations.size() << " animation, "
            << model->primitives.size() << " primitive\n";
        for (std::size_t index = 0;
            index < model->animations.size();
            ++index)
        {
            std::cout
                << "  clip " << index << ": "
                << model->animations[index].name
                << " ("
                << model->animations[index].duration
                << " s)\n";
        }
        return 0;
    }
}

int main()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);

    // デバイス、コンテキスト、AssetManager、モデルのGPUリソースはCOMを
    // 保持するため、RunTest内で破棄してからCoUninitializeを呼びます。
    int exitCode = 1;
    try
    {
        exitCode = RunTest();
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        exitCode = 1;
    }

    if (uninitializeCom)
    {
        CoUninitialize();
    }
    return exitCode;
}
