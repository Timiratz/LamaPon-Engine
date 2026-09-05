#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Assets/GltfImporter.h"
#include "LamaPon/Assets/ModelCache.h"
#include "LamaPon/Graphics/SkeletalModel.h"

#include <d3d11.h>
#include <objbase.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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


    // GPUバッファの中身を読み戻します（IMMUTABLEは直接Mapできない
    // ので、STAGINGへコピーしてから読みます）。
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

    // ディスクキャッシュから復元したモデルが、インポート直後の
    // モデルと完全に同じであることを確かめます。CPU側のデータは
    // 値の一致、GPUバッファは読み戻してバイト一致で見ます。
    void RequireSameModel(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const LamaPon::SkeletalModel& imported,
        const LamaPon::SkeletalModel& cached)
    {
        Require(
            imported.hasLocalBounds
                && cached.hasLocalBounds
                && std::memcmp(
                    &imported.localBounds,
                    &cached.localBounds,
                    sizeof(imported.localBounds)) == 0,
            "cached model bounds must match");
        Require(
            imported.nodes.size() == cached.nodes.size(),
            "cached node count must match");
        for (std::size_t index = 0;
            index < imported.nodes.size();
            ++index)
        {
            const auto& left = imported.nodes[index];
            const auto& right = cached.nodes[index];
            Require(
                left.name == right.name
                    && left.parent == right.parent
                    && std::memcmp(
                        &left.bindPose,
                        &right.bindPose,
                        sizeof(left.bindPose)) == 0,
                "cached node must match");
        }
        Require(
            imported.skins.size() == cached.skins.size(),
            "cached skin count must match");
        for (std::size_t index = 0;
            index < imported.skins.size();
            ++index)
        {
            const auto& left = imported.skins[index];
            const auto& right = cached.skins[index];
            Require(
                left.name == right.name
                    && left.joints == right.joints
                    && left.inverseBindMatrices.size()
                        == right.inverseBindMatrices.size()
                    && std::memcmp(
                        left.inverseBindMatrices.data(),
                        right.inverseBindMatrices.data(),
                        left.inverseBindMatrices.size()
                            * sizeof(DirectX::XMFLOAT4X4))
                        == 0,
                "cached skin must match");
        }
        Require(
            imported.animations.size()
                == cached.animations.size(),
            "cached animation count must match");
        for (std::size_t index = 0;
            index < imported.animations.size();
            ++index)
        {
            const auto& left = imported.animations[index];
            const auto& right = cached.animations[index];
            Require(
                left.name == right.name
                    && left.duration == right.duration
                    && left.tracks.size()
                        == right.tracks.size(),
                "cached animation must match");
            for (std::size_t track = 0;
                track < left.tracks.size();
                ++track)
            {
                const auto& a = left.tracks[track];
                const auto& b = right.tracks[track];
                const auto sameVector =
                    [](const LamaPon::SkeletalVectorChannel& x,
                       const LamaPon::SkeletalVectorChannel& y)
                {
                    return x.interpolation == y.interpolation
                        && x.keys.size() == y.keys.size()
                        && std::memcmp(
                            x.keys.data(),
                            y.keys.data(),
                            x.keys.size()
                                * sizeof(
                                    LamaPon::
                                        SkeletalVectorKey))
                            == 0;
                };
                const auto sameQuaternion =
                    [](const LamaPon::
                            SkeletalQuaternionChannel& x,
                       const LamaPon::
                            SkeletalQuaternionChannel& y)
                {
                    return x.interpolation == y.interpolation
                        && x.keys.size() == y.keys.size()
                        && std::memcmp(
                            x.keys.data(),
                            y.keys.data(),
                            x.keys.size()
                                * sizeof(
                                    LamaPon::
                                        SkeletalQuaternionKey))
                            == 0;
                };
                Require(
                    a.node == b.node
                        && sameVector(
                            a.translation,
                            b.translation)
                        && sameQuaternion(
                            a.rotation,
                            b.rotation)
                        && sameVector(a.scale, b.scale),
                    "cached animation track must match");
            }
        }
        Require(
            imported.primitives.size()
                == cached.primitives.size(),
            "cached primitive count must match");
        for (std::size_t index = 0;
            index < imported.primitives.size();
            ++index)
        {
            const auto& left = imported.primitives[index];
            const auto& right = cached.primitives[index];
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
                            sizeof(left.localBounds)) == 0)
                    && left.alpha == right.alpha
                    && left.doubleSided == right.doubleSided
                    && left.textureHasTransparency
                        == right.textureHasTransparency,
                "cached primitive metadata must match");
            Require(
                std::memcmp(
                    &left.baseColor,
                    &right.baseColor,
                    sizeof(left.baseColor)) == 0
                    && left.roughness == right.roughness
                    && left.metallic == right.metallic
                    && left.occlusionStrength
                        == right.occlusionStrength
                    && std::memcmp(
                        &left.emissiveFactor,
                        &right.emissiveFactor,
                        sizeof(left.emissiveFactor)) == 0,
                "cached primitive material must match");
            // テクスチャの有無（スロットごと）と、cutoutの有無。
            Require(
                (left.texture != nullptr)
                        == (right.texture != nullptr)
                    && (left.normalTexture != nullptr)
                        == (right.normalTexture != nullptr)
                    && (left.roughnessTexture != nullptr)
                        == (right.roughnessTexture != nullptr)
                    && (left.metallicTexture != nullptr)
                        == (right.metallicTexture != nullptr)
                    && (left.occlusionTexture != nullptr)
                        == (right.occlusionTexture != nullptr)
                    && (left.emissiveTexture != nullptr)
                        == (right.emissiveTexture != nullptr)
                    && (left.cutoutEffect != nullptr)
                        == (right.cutoutEffect != nullptr),
                "cached primitive resources must match");
            Require(
                right.vertexBuffer && right.indexBuffer
                    && right.inputLayout && right.effect,
                "cached primitive GPU resources must exist");
            Require(
                ReadBuffer(
                    device,
                    context,
                    left.vertexBuffer.Get())
                    == ReadBuffer(
                        device,
                        context,
                        right.vertexBuffer.Get()),
                "cached vertex bytes must match exactly");
            Require(
                ReadBuffer(
                    device,
                    context,
                    left.indexBuffer.Get())
                    == ReadBuffer(
                        device,
                        context,
                        right.indexBuffer.Get()),
                "cached index bytes must match exactly");
        }
    }

    int RunTest()
    {
        // モデルキャッシュを専用の置き場で走らせます（本物の
        // %LOCALAPPDATA%を汚さない・前回の実行結果を拾わない）。
        const auto cacheRoot =
            std::filesystem::current_path()
            / "test-output"
            / "model-cache-gltf";
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
            / "RiggedSimple.glb";
        const auto model = LamaPon::GltfImporter::Load(
            device.Get(),
            context.Get(),
            assets,
            modelPath);
        Require(model != nullptr, "glTF model was not loaded.");
        Require(!model->nodes.empty(), "glTF nodes are missing.");
        Require(!model->skins.empty(), "glTF skin is missing.");
        Require(
            !model->animations.empty(),
            "glTF animation is missing.");
        Require(
            !model->primitives.empty(),
            "glTF mesh primitive is missing.");
        Require(
            model->hasLocalBounds,
            "glTF model bounds are missing.");
        Require(
            model->animations.front().duration > 0.0f,
            "glTF animation duration is invalid.");
        Require(
            model->skins.front().joints.size()
                == model->skins.front()
                    .inverseBindMatrices.size(),
            "glTF inverse bind matrix count is invalid.");
        for (const auto& primitive : model->primitives)
        {
            Require(
                primitive.vertexBuffer
                    && primitive.indexBuffer
                    && primitive.inputLayout
                    && primitive.effect,
                "glTF GPU resources were not created.");
        }

        std::vector<LamaPon::SkeletalPoseTransform> localA;
        std::vector<LamaPon::SkeletalPoseTransform> localB;
        std::vector<DirectX::XMFLOAT4X4> globalA;
        std::vector<DirectX::XMFLOAT4X4> globalB;
        const auto& clip = model->animations.front();
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
        Require(
            PoseChanged(globalA, globalB),
            "glTF animation did not change the sampled pose.");

        // 1回目のインポートでキャッシュが書かれ、2回目は
        // そこから同じモデルが復元されること。
        Require(
            std::filesystem::exists(cacheRoot)
                && !std::filesystem::is_empty(cacheRoot),
            "the import must write a model cache entry");
        const auto cached = LamaPon::GltfImporter::Load(
            device.Get(),
            context.Get(),
            assets,
            modelPath);
        Require(
            cached != nullptr,
            "the cached model must load");
        RequireSameModel(
            device.Get(),
            context.Get(),
            *model,
            *cached);

        // 壊れたキャッシュは黙って捨てられ、普通のインポートへ
        // 落ちること（クラッシュも空モデルも出さない）。
        for (const auto& entry :
            std::filesystem::directory_iterator(cacheRoot))
        {
            std::vector<char> half(
                static_cast<std::size_t>(
                    std::filesystem::file_size(
                        entry.path()))
                / 2);
            {
                std::ifstream input(
                    entry.path(),
                    std::ios::binary);
                input.read(
                    half.data(),
                    static_cast<std::streamsize>(
                        half.size()));
            }
            std::ofstream output(
                entry.path(),
                std::ios::binary | std::ios::trunc);
            output.write(
                half.data(),
                static_cast<std::streamsize>(half.size()));
        }
        const auto fallback = LamaPon::GltfImporter::Load(
            device.Get(),
            context.Get(),
            assets,
            modelPath);
        Require(
            fallback != nullptr
                && fallback->primitives.size()
                    == model->primitives.size(),
            "a corrupted cache must fall back to a normal"
            " import");

        // ディスクキャッシュなしのglTF解析をワーカーで完走し、
        // 完成結果だけを呼び出し側のキャッシュへ反映できること。
        std::filesystem::remove_all(cacheRoot);
        assets.Invalidate(modelPath);
        Require(
            assets.PrepareModelAsync(modelPath),
            "asynchronous glTF preparation must start");
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
                "asynchronous glTF preparation failed: "
                + (preparationError.empty()
                    ? "timeout or stale result"
                    : preparationError));
        }
        const auto prepared = assets.LoadModel(modelPath);
        Require(
            prepared != nullptr
                && prepared->skeletalModel != nullptr
                && prepared->hasLocalBounds,
            "prepared glTF asset must be cached with bounds");

        std::cout
            << "glTF import: "
            << model->nodes.size() << " nodes, "
            << model->skins.size() << " skin, "
            << model->animations.size() << " animation, "
            << model->primitives.size() << " primitive\n";
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
