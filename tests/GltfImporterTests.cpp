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

    // RequireSameModel(device: GPUデバイス, context: GPU文脈, imported: 元モデル, cached: 復元モデル): CPUデータとGPU資源の一致を検証する。
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
        // 各ノードのキャッシュ内容を比較する。
        for (std::size_t index = 0;
            index < imported.nodes.size();
            ++index)
        {
            // 元モデルのノード
            const auto& left = imported.nodes[index];
            // キャッシュモデルの対応ノード
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
        // 各スキンのキャッシュ内容を比較する。
        for (std::size_t index = 0;
            index < imported.skins.size();
            ++index)
        {
            // 元モデルのスキン
            const auto& left = imported.skins[index];
            // キャッシュモデルの対応スキン
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
        // 各アニメーションをキャッシュと比較する。
        for (std::size_t index = 0;
            index < imported.animations.size();
            ++index)
        {
            // 元モデルのアニメーション
            const auto& left = imported.animations[index];
            // キャッシュモデルの対応アニメーション
            const auto& right = cached.animations[index];
            Require(
                left.name == right.name
                    && left.duration == right.duration
                    && left.tracks.size()
                        == right.tracks.size(),
                "cached animation must match");
            // 各トラックのチャンネルを比較する。
            for (std::size_t track = 0;
                track < left.tracks.size();
                ++track)
            {
                // 元アニメーションのトラック
                const auto& a = left.tracks[track];
                // キャッシュアニメーションの対応トラック
                const auto& b = right.tracks[track];
                // sameVector(x: 元ベクトル列, y: 復元ベクトル列): キー列の一致を判定する。
                const auto sameVector =
                    [](const LamaPon::SkeletalVectorChannel& x,
                       const LamaPon::SkeletalVectorChannel& y)
                {
                    // チャンネルキーの値を比較する。
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
                // sameQuaternion(x: 元回転列, y: 復元回転列): キー列の一致を判定する。
                const auto sameQuaternion =
                    [](const LamaPon::
                            SkeletalQuaternionChannel& x,
                       const LamaPon::
                            SkeletalQuaternionChannel& y)
                {
                    // チャンネルキーの値を比較する。
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
        // 各プリミティブのキャッシュ内容を比較する。
        for (std::size_t index = 0;
            index < imported.primitives.size();
            ++index)
        {
            // 元モデルのプリミティブ
            const auto& left = imported.primitives[index];
            // キャッシュモデルの対応プリミティブ
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
            // 各テクスチャ枠とcutout資源の有無を比較する。
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
                !left.cpuVertexData.empty()
                    && left.cpuVertexStride > 0
                    && left.cpuVertexData
                        == right.cpuVertexData
                    && left.cpuVertexStride
                        == right.cpuVertexStride
                    && left.cpuIndices == right.cpuIndices
                    && left.cpuLodIndices
                        == right.cpuLodIndices,
                "cached CPU geometry must match exactly");
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

    // RunTest(): glTFインポート、キャッシュ、非同期準備を検証する。
    int RunTest()
    {
        // 前回結果とユーザーキャッシュを避けるテスト用保存先
        const auto cacheRoot =
            std::filesystem::current_path()
            / "test-output"
            / "model-cache-gltf";
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
        // インポート対象glTF
        const auto modelPath =
            std::filesystem::path(LAMAPON_TEST_ASSET_DIR)
            / "models"
            / "RiggedSimple.glb";
        // 前方描画ロールの要否
        bool requiresForwardRole{};
        Require(
            LamaPon::GltfImporter::RequiresSkinning(
                assets,
                modelPath,
                &requiresForwardRole)
                && !requiresForwardRole,
            "glTF role probe did not classify the rigged fixture as "
                "Skinned-only.");
        // 読み込んだglTFモデル
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
        // 生成されたGPU資源を確認する。
        for (const auto& primitive : model->primitives)
        {
            Require(
                primitive.vertexBuffer
                    && primitive.indexBuffer
                    && primitive.inputLayout
                    && primitive.effect,
                "glTF GPU resources were not created.");
            Require(
                !primitive.cpuVertexData.empty()
                    && primitive.cpuVertexStride > 0
                    && !primitive.cpuIndices.empty(),
                "glTF CPU geometry was not retained.");
        }

        // アニメーション開始時刻のローカル姿勢
        std::vector<LamaPon::SkeletalPoseTransform> localA;
        // 中間時刻のローカル姿勢
        std::vector<LamaPon::SkeletalPoseTransform> localB;
        // 開始時刻のグローバル姿勢
        std::vector<DirectX::XMFLOAT4X4> globalA;
        // 中間時刻のグローバル姿勢
        std::vector<DirectX::XMFLOAT4X4> globalB;
        // モデルの先頭アニメーション
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

        // 初回保存したキャッシュから同じモデルを復元する。
        Require(
            std::filesystem::exists(cacheRoot)
                && !std::filesystem::is_empty(cacheRoot),
            "the import must write a model cache entry");
        // キャッシュから再読込したglTFモデル
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

        // 各キャッシュファイルを半分に切り、破損状態を作る。
        for (const auto& entry :
            std::filesystem::directory_iterator(cacheRoot))
        {
            // 破損させるキャッシュの半分のデータ
            std::vector<char> half(
                static_cast<std::size_t>(
                    std::filesystem::file_size(
                        entry.path()))
                / 2);
            {
                // キャッシュファイルを読み込む。
                std::ifstream input(
                    entry.path(),
                    std::ios::binary);
                input.read(
                    half.data(),
                    static_cast<std::streamsize>(
                        half.size()));
            }
            // 半分のデータで上書きする。
            std::ofstream output(
                entry.path(),
                std::ios::binary | std::ios::trunc);
            output.write(
                half.data(),
                static_cast<std::streamsize>(half.size()));
        }
        // 破損キャッシュから通常インポートへ戻ったモデル
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

        // キャッシュなしの非同期解析で完成モデルを準備する。
        std::filesystem::remove_all(cacheRoot);
        assets.Invalidate(modelPath);
        Require(
            assets.PrepareModelAsync(modelPath),
            "asynchronous glTF preparation must start");
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
                "asynchronous glTF preparation failed: "
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

// main(): COM寿命を管理してglTFインポート検査を実行する。
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
