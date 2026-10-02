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
    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を検査失敗にする。
    void Require(const bool condition, const char* message)
    {
        // 検査条件の不成立を検出する。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // PoseChanged(left: 比較元の姿勢, right: 比較先の姿勢): 行列の差から姿勢変化を判定する。
    bool PoseChanged(
        const std::vector<DirectX::XMFLOAT4X4>& left,
        const std::vector<DirectX::XMFLOAT4X4>& right)
    {
        // 配列長の差は姿勢変化として扱う。
        if (left.size() != right.size())
        {
            return true;
        }
        // 各姿勢行列を比較する。
        for (std::size_t index = 0;
            index < left.size();
            ++index)
        {
            // 比較元行列の要素列
            const float* a =
                reinterpret_cast<const float*>(&left[index]);
            // 比較先行列の要素列
            const float* b =
                reinterpret_cast<const float*>(&right[index]);
            // 行列の全要素を比較する。
            for (std::size_t element = 0;
                element < 16;
                ++element)
            {
                // 許容誤差を超える差を検出する。
                if (std::abs(a[element] - b[element])
                    > 0.0001f)
                {
                    return true;
                }
            }
        }
        return false;
    }


    // ReadBuffer(device: GPUデバイス, context: GPU文脈, buffer: 読み取り元): ステージング経由で内容をCPUへ読み戻す。
    [[nodiscard]] std::vector<std::uint8_t> ReadBuffer(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Buffer* buffer)
    {
        // 読み取り元のバッファー設定
        D3D11_BUFFER_DESC description{};
        buffer->GetDesc(&description);
        // CPU読み取り用の一時バッファー設定
        D3D11_BUFFER_DESC staging = description;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        // 読み取り用ステージングバッファー
        Microsoft::WRL::ComPtr<ID3D11Buffer> copy;
        Require(
            SUCCEEDED(device->CreateBuffer(
                &staging,
                nullptr,
                copy.ReleaseAndGetAddressOf())),
            "staging buffer creation must succeed");
        context->CopyResource(copy.Get(), buffer);
        // ステージングバッファーのCPUマップ結果
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(
            SUCCEEDED(context->Map(
                copy.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped)),
            "staging buffer map must succeed");
        // GPUバッファーの読み取り結果
        std::vector<std::uint8_t> bytes(description.ByteWidth);
        std::memcpy(
            bytes.data(),
            mapped.pData,
            description.ByteWidth);
        context->Unmap(copy.Get(), 0);
        return bytes;
    }

    // RunTest(): FBXインポート、キャッシュ、アニメーションを検証する。
    int RunTest()
    {
        // 前回結果とユーザーキャッシュを避けるテスト用保存先
        const auto cacheRoot =
            std::filesystem::current_path()
            / "test-output"
            / "model-cache-fbx";
        std::filesystem::remove_all(cacheRoot);
        LamaPon::ModelCache::SetCacheDirectoryOverride(
            cacheRoot);

        // Direct3Dデバイス
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        // Direct3D即時コンテキスト
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        // 選択された機能レベル
        D3D_FEATURE_LEVEL featureLevel{};
        // 試行する機能レベル
        const D3D_FEATURE_LEVEL requested[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        // デバイス作成結果
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

        // テスト対象モデルを管理する資産マネージャー
        LamaPon::AssetManager assets(device.Get(), context.Get());
        // インポート対象FBX
        const auto modelPath =
            std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "AnimatedSausage.fbx";
        // 前方描画ロールの要否
        bool requiresForwardRole{};
        Require(
            LamaPon::FbxImporter::RequiresSkinning(
                assets,
                modelPath,
                &requiresForwardRole)
                && !requiresForwardRole,
            "FBX role probe did not classify the rigged fixture as "
                "Skinned-only.");
        // 読み込んだFBXモデル
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
        // モデルプリミティブの共有頂点を調べる。
        bool usesSharedVertices{};
        // 全プリミティブのインデックスを確認する。
        for (const auto& primitive : model->primitives)
        {
            // GPUインデックスバッファーの内容
            const auto indexBytes = ReadBuffer(
                device.Get(),
                context.Get(),
                primitive.indexBuffer.Get());
            // 32ビットインデックスを順に読む。
            for (std::size_t offset = 0;
                offset + sizeof(std::uint32_t)
                    <= indexBytes.size();
                offset += sizeof(std::uint32_t))
            {
                // 現在のインデックス値
                std::uint32_t index{};
                std::memcpy(
                    &index,
                    indexBytes.data() + offset,
                    sizeof(index));
                // 共有頂点を指すインデックスを検出する。
                if (index
                    != offset / sizeof(std::uint32_t))
                {
                    usesSharedVertices = true;
                    // 共有頂点が確認できたため走査を終える。
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
        // 生成されたGPU資源を確認する。
        for (const auto& primitive : model->primitives)
        {
            Require(
                primitive.vertexBuffer
                    && primitive.indexBuffer
                    && primitive.inputLayout
                    && primitive.effect,
                "FBX GPU resources were not created.");
            Require(
                !primitive.cpuVertexData.empty()
                    && primitive.cpuVertexStride > 0
                    && !primitive.cpuIndices.empty(),
                "FBX CPU geometry was not retained.");
        }

        // キャッシュ後も形状と資源内容が一致すること。
        // FBXの添字は再生成されるため、GPUバイト列まで照合する。
        Require(
            std::filesystem::exists(cacheRoot)
                && !std::filesystem::is_empty(cacheRoot),
            "the import must write a model cache entry");
        // キャッシュから再読込したFBXモデル
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
        // 各プリミティブのメタデータとバッファーを比較する。
        for (std::size_t index = 0;
            index < model->primitives.size();
            ++index)
        {
            // 元モデルのプリミティブ
            const auto& left = model->primitives[index];
            // キャッシュモデルの対応プリミティブ
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
            Require(
                left.cpuVertexData == right.cpuVertexData
                    && left.cpuVertexStride
                        == right.cpuVertexStride
                    && left.cpuIndices == right.cpuIndices
                    && left.cpuLodIndices
                        == right.cpuLodIndices,
                "cached FBX CPU geometry must match");
        }
        // キャッシュ後も同じ時刻の姿勢を再現する。
        {
            // 元モデルのローカル姿勢
            std::vector<LamaPon::SkeletalPoseTransform> localA;
            // キャッシュモデルのローカル姿勢
            std::vector<LamaPon::SkeletalPoseTransform> localB;
            // 元モデルのグローバル姿勢
            std::vector<DirectX::XMFLOAT4X4> globalA;
            // キャッシュモデルのグローバル姿勢
            std::vector<DirectX::XMFLOAT4X4> globalB;
            // 元モデルの先頭アニメーション
            const auto& clip = model->animations.front();
            // キャッシュモデルの先頭アニメーション
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

        // 時刻差で姿勢が変わるアニメーションの有無
        bool animatedPoseChanged{};
        // 全アニメーションの姿勢変化を確認する。
        for (const auto& clip : model->animations)
        {
            // 開始時刻のローカル姿勢
            std::vector<LamaPon::SkeletalPoseTransform> localA;
            // 中間時刻のローカル姿勢
            std::vector<LamaPon::SkeletalPoseTransform> localB;
            // 開始時刻のグローバル姿勢
            std::vector<DirectX::XMFLOAT4X4> globalA;
            // 中間時刻のグローバル姿勢
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

        // ブレンド元アニメーション
        LamaPon::SkeletalAnimationClip blendFrom;
        blendFrom.duration = 1.0f;
        blendFrom.tracks.push_back({});
        blendFrom.tracks.back().node = 0;
        blendFrom.tracks.back().translation.keys = {
            { 0.0f, { 0.0f, 0.0f, 0.0f } }
        };
        // ブレンド先アニメーション
        LamaPon::SkeletalAnimationClip blendTo;
        blendTo.duration = 1.0f;
        blendTo.tracks.push_back({});
        blendTo.tracks.back().node = 0;
        blendTo.tracks.back().translation.keys = {
            { 0.0f, { 10.0f, 4.0f, -2.0f } }
        };
        // ブレンド後のローカル姿勢
        std::vector<LamaPon::SkeletalPoseTransform>
            blendedLocal;
        // ブレンド後のグローバル姿勢
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
        // 重み付き姿勢の入力サンプル
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

        // Immediate Contextなしの非同期解析でも資源と境界を完成する。
        std::filesystem::remove_all(cacheRoot);
        assets.Invalidate(modelPath);
        Require(
            assets.PrepareModelAsync(modelPath),
            "asynchronous FBX preparation must start");
        // 非同期モデル準備の現在状態
        LamaPon::ModelPreparationState preparationState =
            LamaPon::ModelPreparationState::Pending;
        // 非同期準備の失敗理由
        std::string preparationError;
        // 非同期準備の待機期限
        const auto preparationDeadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(30);
        // 準備完了まで状態を確認する。
        while (std::chrono::steady_clock::now()
            < preparationDeadline)
        {
            preparationState =
                assets.PollModelPreparation(
                    modelPath,
                    &preparationError);
            // 準備が保留中なら待機を続ける。
            if (preparationState
                != LamaPon::ModelPreparationState::Pending)
            {
                break;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
        // 期限内の準備失敗を検出する。
        if (preparationState
            != LamaPon::ModelPreparationState::Ready)
        {
            throw std::runtime_error(
                "asynchronous FBX preparation failed: "
                + (preparationError.empty()
                    ? "timeout or stale result"
                    : preparationError));
        }
        // 非同期準備から取得したモデル
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
        // 全アニメーション名と再生時間を表示する。
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

// main(): COM寿命を管理してFBXインポート検査を実行する。
int main()
{
    // COM初期化結果
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // COM終了処理が必要か
    const bool uninitializeCom = SUCCEEDED(comResult);

    // RunTest内のCOM資源破棄後に返す終了コード
    int exitCode = 1;
    // 検査失敗を終了コードへ変換する。
    try
    {
        exitCode = RunTest();
    }
    // 例外(exception: 検査失敗情報)を診断へ変換する。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        exitCode = 1;
    }

    // COM初期化に成功した場合だけ終了処理する。
    if (uninitializeCom)
    {
        CoUninitialize();
    }
    return exitCode;
}
