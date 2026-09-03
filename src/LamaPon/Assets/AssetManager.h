#pragma once

#include "LamaPon/Assets/AssetDatabase.h"
#include "LamaPon/Assets/TextureLoader.h"
#include "LamaPon/Graphics/TextLayout.h"
#include "LamaPon/Physics/CollisionTypes.h"

#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace DirectX
{
    inline namespace DX11
    {
        class IEffect;
        class Model;
    }
}

struct ID2D1Factory;
struct IDWriteFactory;
struct IWICImagingFactory;

namespace LamaPon
{
    class AnimationClip;
    class AnimatorController;
    class AssetArchive;
    class DataAsset;
    class SkeletalModel;

    struct TextureAsset final
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        std::uint32_t width{};
        std::uint32_t height{};
        std::filesystem::path sourcePath;
        // DDSキューブマップとして読み込まれた場合true
        // （SRVはTextureCube）。
        bool isCube{};
    };

    struct ModelAsset final
    {
        std::shared_ptr<DirectX::Model> model;
        std::shared_ptr<SkeletalModel> skeletalModel;
        // CMO/SDKMESHが持つパーツ別のDiffuseColor。
        // カスタムShaderでも原モデルの色を再利用できるよう保持します。
        std::unordered_map<
            const DirectX::IEffect*,
            DirectX::XMFLOAT4> embeddedDiffuseColors;
        Bounds3D localBounds{};
        bool hasLocalBounds{};
        std::filesystem::path sourcePath;
    };

    struct TextTextureAsset final
    {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        std::uint32_t width{};
        std::uint32_t height{};
    };

    struct AssetPrefetchReport final
    {
        std::size_t requestedFiles{};
        std::size_t loadedFiles{};
        std::size_t cachedFiles{};
        std::size_t failedFiles{};
        std::size_t loadedBytes{};
        bool cancelled{};
        // ワーカースレッドでGPUテクスチャまで作成できた数。
        std::size_t preparedTextures{};
    };

    enum class ModelPreparationState
    {
        NotQueued,
        Pending,
        Ready,
        Failed
    };

    class AssetManager final
    {
    public:
        AssetManager(ID3D11Device* device, ID3D11DeviceContext* context);
        ~AssetManager();

        AssetManager(const AssetManager&) = delete;
        AssetManager& operator=(const AssetManager&) = delete;

        void SetAssetRoot(std::filesystem::path assetRoot);
        [[nodiscard]] const std::filesystem::path& AssetRoot() const noexcept { return m_assetRoot; }
        [[nodiscard]] std::filesystem::path ResolvePath(const std::filesystem::path& path) const;

        // True once a shipped game's encrypted assets.tpak has been
        // loaded in place of a loose assets/ folder.
        [[nodiscard]] bool IsArchived() const noexcept
        {
            return m_archive != nullptr;
        }
        // Every asset load should go through this instead of opening
        // std::ifstream directly, so it transparently works whether the
        // assets are loose files (editor) or packed into an encrypted
        // archive (exported game).
        [[nodiscard]] std::vector<std::uint8_t> ReadFileBytes(
            const std::filesystem::path& path) const;
        [[nodiscard]] bool FileExists(
            const std::filesystem::path& path) const;
        [[nodiscard]] AssetPrefetchReport PrefetchFiles(
            const std::vector<std::filesystem::path>& paths,
            const std::function<bool(
                std::size_t completed,
                std::size_t total)>& progress = {});
        void ClearPrefetchedFiles() noexcept;
        [[nodiscard]] std::size_t
            PrefetchedFileCount() const noexcept;
        [[nodiscard]] std::size_t
            PrefetchedByteCount() const noexcept;
        [[nodiscard]] AssetDatabase& Database() noexcept
        {
            return m_database;
        }
        [[nodiscard]] const AssetDatabase& Database() const noexcept
        {
            return m_database;
        }

        // エンコード済みの画像バイト列からSRVを作ります。DDSは
        // そのまま、それ以外はWICデコード→ミップ生成→（設定に
        // より）BC圧縮で、結果はTextureCacheへ残ります。
        //
        // モデル（glTF/FBX）に埋まった画像と、モデルが参照する
        // 外部画像のための入口です。以前はインポーターが
        // CreateWICTextureFromMemoryを直接呼んでいて、単体
        // テクスチャだけが圧縮とキャッシュの恩恵を受けていました。
        // usageは圧縮フォーマットの選択に使います（法線はBC5）。
        [[nodiscard]]
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            CreateTextureViewFromMemory(
                std::span<const std::uint8_t> bytes,
                bool isDds,
                TextureLoader::TextureUsage usage
                    = TextureLoader::TextureUsage::Color);

        // WIC系テクスチャ生成時にBC圧縮を使うかどうか。
        // GraphicsSettingsから反映され、ワーカースレッドからも
        // 読まれるためatomicです。
        void SetRuntimeTextureCompressionEnabled(
            const bool enabled) noexcept
        {
            m_runtimeTextureCompression.store(
                enabled,
                std::memory_order_relaxed);
        }
        [[nodiscard]] bool
            RuntimeTextureCompressionEnabled()
                const noexcept
        {
            return m_runtimeTextureCompression.load(
                std::memory_order_relaxed);
        }

        // 段階的GPUアップロード: 転送データがこのバイト数以上の
        // WICテクスチャは、粗いミップから複数フレームへ分割して
        // アップロードします（読み込み時のフレームスパイク対策）。
        static constexpr std::size_t
            DefaultProgressiveUploadThreshold =
                8u * 1024u * 1024u;
        // PumpTextureUploadsが1回で転送する上限バイト数の既定値。
        static constexpr std::size_t
            DefaultUploadBudgetPerFrame =
                16u * 1024u * 1024u;
        static constexpr std::size_t
            DefaultModelUploadBudgetPerFrame =
                16u * 1024u * 1024u;

        void SetProgressiveUploadThreshold(
            const std::size_t bytes) noexcept
        {
            m_progressiveUploadThreshold.store(
                bytes,
                std::memory_order_relaxed);
        }
        [[nodiscard]] std::size_t
            ProgressiveUploadThreshold() const noexcept
        {
            return m_progressiveUploadThreshold.load(
                std::memory_order_relaxed);
        }
        // 保留中の段階アップロードを予算内で進めます。
        // immediate contextを使うためメインスレッド専用です
        // （毎フレームGraphicsDevice::BeginFrameから呼ばれます）。
        void PumpTextureUploads(
            std::size_t byteBudget =
                DefaultUploadBudgetPerFrame);
        [[nodiscard]] std::size_t
            PendingTextureUploadCount() const noexcept;

        // 非同期モデル準備中の頂点・インデックスバッファ生成を、
        // フレーム単位の予算で進めます。インポーターから呼ばれる
        // WaitForModelUploadBudgetは準備ワーカー以外では待ちません。
        void PumpModelUploads(
            std::size_t byteBudget =
                DefaultModelUploadBudgetPerFrame) noexcept;
        void WaitForModelUploadBudget(
            std::size_t byteCount);
        [[nodiscard]] std::size_t
            PendingModelUploadCount() const noexcept;
        [[nodiscard]] std::size_t
            ModelUploadBytesLastFrame() const noexcept
        {
            return m_modelUploadBytesLastFrame.load(
                std::memory_order_relaxed);
        }

        // usageは圧縮フォーマットの選択に使います。法線マップを
        // 既定のColorで読むとBC1（RGB565）に落ちて陰影に帯が出るので、
        // マテリアルのスロットに合わせて渡してください。
        [[nodiscard]] std::shared_ptr<const TextureAsset> LoadTexture(
            const std::filesystem::path& path,
            TextureLoader::TextureUsage usage
                = TextureLoader::TextureUsage::Color);
        [[nodiscard]] std::shared_ptr<const ModelAsset> LoadModel(
            const std::filesystem::path& path);
        // 重いFBX/glTF解析とDeviceリソース生成をワーカーで行います。
        // 完成結果のキャッシュ反映はPollModelPreparationを呼ぶ
        // メインスレッド側でだけ行われます。
        bool PrepareModelAsync(
            const std::filesystem::path& path);
        [[nodiscard]] ModelPreparationState
            PollModelPreparation(
                const std::filesystem::path& path,
                std::string* error = nullptr);
        [[nodiscard]] std::shared_ptr<const ModelAsset> CreateModelInstance(
            const std::filesystem::path& path);
        [[nodiscard]] std::shared_ptr<const AnimationClip>
            LoadAnimationClip(
                const std::filesystem::path& path);
        [[nodiscard]] std::shared_ptr<const AnimationClip>
            ReloadAnimationClip(
                const std::filesystem::path& path);
        [[nodiscard]] std::shared_ptr<const AnimatorController>
            LoadAnimatorController(
                const std::filesystem::path& path);
        [[nodiscard]] std::shared_ptr<const AnimatorController>
            ReloadAnimatorController(
                const std::filesystem::path& path);
        // 文字テクスチャは**白**で焼きます。色は描くとき（SpriteBatchの
        // 色）で掛けてください。色を焼き込むと、色を変えるたびに別の
        // テクスチャができてしまい、フェードのような演出ができません
        // （キャッシュのキーからも色が消えるので、同じ文字なら色違いでも
        // 1枚で足ります）。
        // データアセット（`*.asset.json`）を読み込みます。読み込みは
        // アーカイブ越しにも通るため、書き出したゲームでも同じ
        // コードで動きます。同じパスは2回目からキャッシュを返します。
        [[nodiscard]] std::shared_ptr<const DataAsset>
            LoadDataAsset(
                const std::filesystem::path& path);
        // 保存し直した直後など、キャッシュを捨てて読み直します。
        [[nodiscard]] std::shared_ptr<const DataAsset>
            ReloadDataAsset(
                const std::filesystem::path& path);

        [[nodiscard]] std::shared_ptr<const TextTextureAsset> LoadTextTexture(
            std::string_view text,
            std::string_view fontFamily,
            float fontSize,
            const TextLayoutOptions& layout = {});

        void Clear() noexcept;
        // 指定したアセットのキャッシュだけを破棄します。
        // インポートや外部更新で内容が変わったファイルを次回
        // 読み込み時に読み直させる用途で、Clear()と違って
        // シーンが使用中の他アセットを二重に読み込ませません。
        void Invalidate(
            const std::filesystem::path& path) noexcept;
        [[nodiscard]] std::size_t CachedTextureCount() const noexcept
        {
            std::scoped_lock lock(m_textureMutex);
            return m_textureCache.size();
        }
        [[nodiscard]] std::size_t CachedModelCount() const noexcept
        {
            try
            {
                std::scoped_lock lock(m_modelMutex);
                return m_modelCache.size();
            }
            catch (...)
            {
                return 0;
            }
        }
        [[nodiscard]] std::size_t CachedTextCount() const noexcept
        {
            return m_textCache.size();
        }
        [[nodiscard]] std::size_t
            CachedTextBytes() const noexcept
        {
            return m_textCacheBytes;
        }
        // 文字テクスチャキャッシュの上限（バイト）。文字列ごとに
        // 1枚できるため、スコアや残り時間のように中身が変わり続ける
        // 表示では上限が無いと増え続けます。超えた分は「古くて誰も
        // 表示に使っていないもの」から捨てます。
        void SetTextCacheBudgetBytes(
            const std::size_t bytes) noexcept
        {
            m_textCacheBudgetBytes = bytes;
            TrimTextCache();
        }
        [[nodiscard]] std::size_t
            TextCacheBudgetBytes() const noexcept
        {
            return m_textCacheBudgetBytes;
        }
        [[nodiscard]] std::size_t
            CachedAnimationCount() const noexcept
        {
            return m_animationCache.size();
        }
        [[nodiscard]] std::size_t
            CachedDataAssetCount() const noexcept
        {
            return m_dataAssetCache.size();
        }

    private:
        // 文字テクスチャ1枚ぶんのキャッシュ項目。lastUsedはLRUの
        // 判定用、bytesは予算計算用（幅×高さ×4）です。
        struct TextCacheEntry final
        {
            std::shared_ptr<TextTextureAsset> asset;
            std::uint64_t lastUsed{};
            std::size_t bytes{};
        };

        void TrimTextCache() noexcept;
        [[nodiscard]] static std::wstring MakeCacheKey(const std::filesystem::path& path);
        [[nodiscard]] std::vector<std::uint8_t>
            ReadFileBytesUncached(
                const std::filesystem::path& resolvedPath) const;
        // キャッシュを触らないテクスチャ生成本体。immediate context
        // を使わないため、ワーカースレッドから安全に呼べます。
        [[nodiscard]] std::shared_ptr<TextureAsset>
            LoadTextureUncached(
                const std::filesystem::path& resolvedPath,
                TextureLoader::TextureUsage usage
                    = TextureLoader::TextureUsage::Color);
        [[nodiscard]] std::shared_ptr<ModelAsset> LoadModelUncached(
            const std::filesystem::path& resolvedPath,
            ID3D11DeviceContext* context);
        void WaitForModelPreparation() noexcept;
        void EndModelUploadPreparation() noexcept;
        void DisableModelUploadThrottle() noexcept;

        ID3D11Device* m_device{};
        ID3D11DeviceContext* m_context{};
        std::filesystem::path m_assetRoot;
        std::unique_ptr<AssetArchive> m_archive;
        AssetDatabase m_database;
        std::unordered_map<std::wstring, std::shared_ptr<TextureAsset>> m_textureCache;
        std::unordered_map<std::wstring, std::shared_ptr<ModelAsset>> m_modelCache;
        struct PendingModelPreparation final
        {
            std::filesystem::path path;
            std::wstring cacheKey;
            std::future<std::shared_ptr<ModelAsset>> future;
            std::uint64_t generation{};
        };
        std::optional<PendingModelPreparation>
            m_pendingModelPreparation;
        mutable std::mutex m_modelMutex;
        std::uint64_t m_modelGeneration{};
        mutable std::mutex m_modelUploadMutex;
        std::condition_variable m_modelUploadCondition;
        std::thread::id m_modelPreparationThread;
        std::size_t m_modelUploadFrameBudget{
            DefaultModelUploadBudgetPerFrame
        };
        std::size_t m_modelUploadBudgetRemaining{};
        std::size_t m_modelUploadBytesCurrentFrame{};
        std::atomic<std::size_t>
            m_modelUploadBytesLastFrame{};
        bool m_modelUploadThrottled{};
        std::unordered_map<std::wstring, TextCacheEntry> m_textCache;
        // LRUの順番付け用の単調増加カウンターと、現在の合計バイト数。
        std::uint64_t m_textCacheClock{};
        std::size_t m_textCacheBytes{};
        bool m_textCacheBudgetWarningIssued{};
        // 既定32MiB。1280x720のHUD文字なら数百枚は入る余裕があり、
        // それでも「増え続ける」ことは無くなります。
        std::size_t m_textCacheBudgetBytes{
            32ull * 1024ull * 1024ull
        };
        std::unordered_map<
            std::wstring,
            std::shared_ptr<AnimationClip>>
                m_animationCache;
        std::unordered_map<
            std::wstring,
            std::shared_ptr<AnimatorController>>
                m_animatorControllerCache;
        std::unordered_map<
            std::wstring,
            std::shared_ptr<const DataAsset>>
                m_dataAssetCache;
        // m_textureCacheはシーンロードワーカーとメインスレッドの
        // 両方から使われるため専用mutexで保護します。
        mutable std::mutex m_textureMutex;
        std::atomic<bool> m_runtimeTextureCompression{};
        // 段階アップロード待ちの1テクスチャ分。ミップは末尾
        // （最小）から先に転送し、揃った範囲だけを見るSRVへ
        // 差し替えていきます。
        struct PendingTextureUpload final
        {
            std::shared_ptr<TextureAsset> asset;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            TextureLoader::PreparedTextureData data;
            // 次に転送するミップ（負になったら完了）。
            std::ptrdiff_t nextLevel{};
        };
        std::deque<PendingTextureUpload> m_pendingUploads;
        mutable std::mutex m_uploadMutex;
        std::atomic<std::size_t> m_progressiveUploadThreshold{
            DefaultProgressiveUploadThreshold
        };
        mutable std::mutex m_prefetchMutex;
        mutable std::unordered_map<
            std::wstring,
            std::shared_ptr<
                const std::vector<std::uint8_t>>>
            m_prefetchedBytes;
        mutable std::size_t
            m_prefetchedByteCount{};
        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
        Microsoft::WRL::ComPtr<IWICImagingFactory> m_wicFactory;
    };
}
