#pragma once

#include "LamaPon/Assets/AssetDatabase.h"
#include "LamaPon/Assets/TextureLoader.h"
#include "LamaPon/Graphics/GraphicsResource.h"
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
    class GraphicsBackend;
    class SkeletalModel;
    struct MemorySnapshotEntry;

    namespace Detail
    {
        class TextureResourceSlot;
    }

    // 取得した同世代の資源組は描画完了まで保持する。
    struct TextureResourceSnapshot final
    {
        // 同世代の画像資源
        GraphicsTextureHandle texture;
        // 同世代の描画ビュー
        GraphicsViewHandle shaderResourceView;
        // 共通ビューが有効なら優先し、互換ビューは従来経路で使う。
        // D3D11互換ビュー
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            d3d11ShaderResourceView;
    };

    // コピーとムーブ後も同じ公開窓口を共有する。
    class TextureResourceBinding final
    {
    public:
        // 空の資源組を共有する公開窓口を作る。
        TextureResourceBinding();
        // 公開窓口の所有を解放する。
        ~TextureResourceBinding();
        // 同じ公開窓口を共有する。
        TextureResourceBinding(
            const TextureResourceBinding&) noexcept;
        // 元の公開窓口を保持したまま共有する。
        TextureResourceBinding(
            TextureResourceBinding&&) noexcept;
        // 同じ公開窓口を共有する。
        TextureResourceBinding& operator=(
            const TextureResourceBinding&) noexcept;
        // 元の公開窓口を保持したまま共有する。
        TextureResourceBinding& operator=(
            TextureResourceBinding&&) noexcept;

        // 描画完了まで保持する同世代の資源組を取得する。
        [[nodiscard]] std::shared_ptr<
            const TextureResourceSnapshot> Acquire() const noexcept;
        // 完成した資源組をまとめて差し替える(snapshot: 公開する資源組)。
        void Publish(TextureResourceSnapshot snapshot);

    private:
        // 共有する公開窓口
        std::shared_ptr<Detail::TextureResourceSlot> m_slot;
    };

    struct TextureAsset final
    {
        // 差し替え可能な資源組
        TextureResourceBinding resources;
        // 元画像の横幅
        std::uint32_t width{};
        // 元画像の高さ
        std::uint32_t height{};
        // 解決済みの画像パス
        std::filesystem::path sourcePath;
        // キューブ画像の識別
        bool isCube{};
        // 0は容量不明として扱う。
        // 全ミップの推定GPU容量
        std::uint64_t gpuBytes{};
    };

    struct ModelAsset final
    {
        // DirectXTK描画モデル
        std::shared_ptr<DirectX::Model> model;
        // 共通形式の描画モデル
        std::shared_ptr<SkeletalModel> skeletalModel;
        // パーツ別の元材質色
        std::unordered_map<
            const DirectX::IEffect*,
            DirectX::XMFLOAT4> embeddedDiffuseColors;
        // モデル座標の外接範囲
        Bounds3D localBounds{};
        // 外接範囲の有効性
        bool hasLocalBounds{};
        // 解決済みのモデルパス
        std::filesystem::path sourcePath;
    };

    struct TextTextureAsset final
    {
        // 文字画像の資源組
        TextureResourceBinding resources;
        // 文字画像の横幅
        std::uint32_t width{};
        // 文字画像の高さ
        std::uint32_t height{};
    };

    struct AssetPrefetchReport final
    {
        // 要求されたファイル数
        std::size_t requestedFiles{};
        // 新規読み込み件数
        std::size_t loadedFiles{};
        // 既存先読みの再利用数
        std::size_t cachedFiles{};
        // 読み込み失敗件数
        std::size_t failedFiles{};
        // 新規読み込みの総容量
        std::size_t loadedBytes{};
        // 通知による中断の有無
        bool cancelled{};
        // 仮表示を含む生成画像数
        std::size_t preparedTextures{};
    };

    enum class ModelPreparationState
    {
        // 対象の準備要求がない状態
        NotQueued,
        // 準備が完了していない状態
        Pending,
        // 共有モデルを利用可能な状態
        Ready,
        // 回収時の失敗を通知する状態
        Failed
    };

    // 文字・解析済みキャッシュ操作とルート切替はメインスレッドで直列化する。
    // 借用Backendは管理中に停止・再初期化せず、先に本管理器を破棄する。
    class AssetManager final
    {
    public:
        // D3D11資源を借用して読み込み管理を作る(device: 描画デバイス, context: 即時コンテキスト)。
        AssetManager(
            ID3D11Device* device,
            ID3D11DeviceContext* context);
        // 初期化済みBackendを借用して管理を作る(device: D3D11互換デバイス, context: D3D11互換コンテキスト, backend: 管理中の存続が必要なBackend)。
        AssetManager(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            GraphicsBackend& backend);
        // モデル処理を終了してキャッシュを解放する。
        ~AssetManager();

        // 共有キャッシュの複製を禁止する。
        AssetManager(const AssetManager&) = delete;
        // 共有キャッシュの代入を禁止する。
        AssetManager& operator=(const AssetManager&) = delete;

        // 不足メタ情報の作成を許可してルートを切り替える(assetRoot: アセット配置先)。
        void SetAssetRoot(std::filesystem::path assetRoot);
        // 準備モデルの完了後にルートと台帳を更新する(assetRoot: アセット配置先, createMissingMeta: 不足メタ情報の作成許可)。
        void SetAssetRoot(
            std::filesystem::path assetRoot,
            bool createMissingMeta);
        // 管理中のアセットルートを参照する。
        [[nodiscard]] const std::filesystem::path& AssetRoot() const noexcept { return m_assetRoot; }
        // ルート基準でパスを字句正規化する(path: 相対または絶対パス)。
        [[nodiscard]] std::filesystem::path ResolvePath(const std::filesystem::path& path) const;

        // 暗号化アーカイブから読み込む状態を返す。
        [[nodiscard]] bool IsArchived() const noexcept
        {
            return m_archive != nullptr;
        }
        // 先読みを優先して画像以外も含むバイト列を複製する(path: 読み込み対象)。
        [[nodiscard]] std::vector<std::uint8_t> ReadFileBytes(
            const std::filesystem::path& path) const;
        // 先読みを使わずファイルまたはアーカイブを読む(path: 読み込み対象)。
        [[nodiscard]] std::vector<std::uint8_t> ReadFileBytesFresh(
            const std::filesystem::path& path) const;
        // 組み込み画像・アーカイブ・通常ファイルの存在を調べる(path: 確認対象)。
        [[nodiscard]] bool FileExists(
            const std::filesystem::path& path) const;
        // 処理済み分を保持して先読みする(paths: 対象一覧, progress: falseで中断する通知, completed: 処理済み件数, total: 要求件数)。
        [[nodiscard]] AssetPrefetchReport PrefetchFiles(
            const std::vector<std::filesystem::path>& paths,
            const std::function<bool(
                std::size_t completed,
                std::size_t total)>& progress = {});
        // 先読み世代を更新して保持バイト列を破棄する。
        void ClearPrefetchedFiles() noexcept;
        // 先読みしたファイル数を返し取得失敗時は0とする。
        [[nodiscard]] std::size_t
            PrefetchedFileCount() const noexcept;
        // 先読みした合計バイト数を返し取得失敗時は0とする。
        [[nodiscard]] std::size_t
            PrefetchedByteCount() const noexcept;
        // 管理中の台帳を参照する。
        [[nodiscard]] AssetDatabase& Database() noexcept
        {
            return m_database;
        }
        // 管理中の台帳を読み取り参照する。
        [[nodiscard]] const AssetDatabase& Database() const noexcept
        {
            return m_database;
        }

        // DDS以外は復号・ミップ生成・圧縮結果をディスクキャッシュに保存する。
        // 画像の全ミップをD3D11へ転送する(bytes: 符号化画像, isDds: DDS形式指定, usage: 圧縮用途)。
        [[nodiscard]]
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            CreateTextureViewFromMemory(
                std::span<const std::uint8_t> bytes,
                bool isDds,
                TextureLoader::TextureUsage usage
                    = TextureLoader::TextureUsage::Color);

        // 画像を現在のBackendへ取り込む(bytes: 符号化画像, isDds: DDS形式指定, usage: 圧縮用途)。
        [[nodiscard]] GraphicsViewHandle
            CreateTextureViewHandleFromMemory(
                std::span<const std::uint8_t> bytes,
                bool isDds,
                TextureLoader::TextureUsage usage
                    = TextureLoader::TextureUsage::Color);

        // WIC画像の実行時圧縮を切り替える(enabled: BC圧縮の許可)。
        void SetRuntimeTextureCompressionEnabled(
            const bool enabled) noexcept
        {
            m_runtimeTextureCompression.store(
                enabled,
                std::memory_order_relaxed);
        }
        // WIC画像の実行時圧縮設定を返す。
        [[nodiscard]] bool
            RuntimeTextureCompressionEnabled()
                const noexcept
        {
            return m_runtimeTextureCompression.load(
                std::memory_order_relaxed);
        }


        // 段階転送の既定閾値
        static constexpr std::size_t
            DefaultProgressiveUploadThreshold =
                8u * 1024u * 1024u;

        // 画像転送の既定予算
        static constexpr std::size_t
            DefaultUploadBudgetPerFrame =
                16u * 1024u * 1024u;
        // モデル転送の既定予算
        static constexpr std::size_t
            DefaultModelUploadBudgetPerFrame =
                16u * 1024u * 1024u;

        // 段階転送を始める画像容量を設定する(bytes: 閾値バイト数)。
        void SetProgressiveUploadThreshold(
            const std::size_t bytes) noexcept
        {
            m_progressiveUploadThreshold.store(
                bytes,
                std::memory_order_relaxed);
        }
        // 段階転送を始める画像容量を返す。
        [[nodiscard]] std::size_t
            ProgressiveUploadThreshold() const noexcept
        {
            return m_progressiveUploadThreshold.load(
                std::memory_order_relaxed);
        }
        // メインスレッドで粗いミップから転送する(byteBudget: 最低1ミップの前進を許す目安容量)。
        void PumpTextureUploads(
            std::size_t byteBudget =
                DefaultUploadBudgetPerFrame);
        // 段階転送の待機画像数を返す。
        [[nodiscard]] std::size_t
            PendingTextureUploadCount() const noexcept;

        // モデル準備ワーカーの転送予算を補充する(byteBudget: 最低1バイトの目安容量)。
        void PumpModelUploads(
            std::size_t byteBudget =
                DefaultModelUploadBudgetPerFrame) noexcept;
        // バッファを分割しないため実際の転送量はフレーム予算を超え得る。
        // 専用準備ワーカーのみ転送予算を待つ(byteCount: 分割しないバッファの全容量)。
        void WaitForModelUploadBudget(
            std::size_t byteCount);
        // モデル転送の予算制御中なら1を返す。
        [[nodiscard]] std::size_t
            PendingModelUploadCount() const noexcept;
        // 前フレームにモデル転送へ計上した容量を返す。
        [[nodiscard]] std::size_t
            ModelUploadBytesLastFrame() const noexcept
        {
            return m_modelUploadBytesLastFrame.load(
                std::memory_order_relaxed);
        }

        // Backend停止前にモデル処理の受付を永久に閉じて完了を待つ。
        void QuiesceGraphicsWork() noexcept;

        // パスと圧縮用途をキーに画像を共有する(path: 対象画像, usage: マテリアル用途)。
        [[nodiscard]] std::shared_ptr<const TextureAsset> LoadTexture(
            const std::filesystem::path& path,
            TextureLoader::TextureUsage usage
                = TextureLoader::TextureUsage::Color);
        // モデルを共有し同じパスの準備があれば待つ(path: 対象モデル)。
        [[nodiscard]] std::shared_ptr<const ModelAsset> LoadModel(
            const std::filesystem::path& path);
        // 同時に1件だけモデル準備を予約する(path: 対象モデル)。
        bool PrepareModelAsync(
            const std::filesystem::path& path);
        // 失敗した準備結果は回収され、次の照会では未予約状態になる。
        // 待機せず準備結果を回収する(path: 対象モデル, error: 任意の失敗理由出力)。
        [[nodiscard]] ModelPreparationState
            PollModelPreparation(
                const std::filesystem::path& path,
                std::string* error = nullptr);
        // 準備完了を待って独立したモデル資源を作る(path: 対象モデル)。
        [[nodiscard]] std::shared_ptr<const ModelAsset> CreateModelInstance(
            const std::filesystem::path& path);
        // 先読み経路からアニメーションを共有する(path: 対象クリップ)。
        [[nodiscard]] std::shared_ptr<const AnimationClip>
            LoadAnimationClip(
                const std::filesystem::path& path);
        // 先読みバイト列は残るため、ファイル更新の反映前にInvalidateする。
        // 解析済みクリップを捨て再読み込みする(path: 対象クリップ)。
        [[nodiscard]] std::shared_ptr<const AnimationClip>
            ReloadAnimationClip(
                const std::filesystem::path& path);
        // 先読み経路から制御設定と参照先を読み込む(path: 対象コントローラー)。
        [[nodiscard]] std::shared_ptr<const AnimatorController>
            LoadAnimatorController(
                const std::filesystem::path& path);
        // 先読みバイト列は残るため、ファイル更新の反映前にInvalidateする。
        // 解析済み制御設定を捨て再読み込みする(path: 対象コントローラー)。
        [[nodiscard]] std::shared_ptr<const AnimatorController>
            ReloadAnimatorController(
                const std::filesystem::path& path);
        // 先読み経路からデータアセットを共有する(path: 対象データ)。
        [[nodiscard]] std::shared_ptr<const DataAsset>
            LoadDataAsset(
                const std::filesystem::path& path);
        // 先読みバイト列は残るため、ファイル更新の反映前にInvalidateする。
        // 解析済みデータを捨て再読み込みする(path: 対象データ)。
        [[nodiscard]] std::shared_ptr<const DataAsset>
            ReloadDataAsset(
                const std::filesystem::path& path);

        // メインスレッドで白い文字画像を共有する(text: 表示文字列, fontFamily: 書体名, fontSize: 文字サイズ, layout: 配置と折り返し)。
        [[nodiscard]] std::shared_ptr<const TextTextureAsset> LoadTextTexture(
            std::string_view text,
            std::string_view fontFamily,
            float fontSize,
            const TextLayoutOptions& layout = {});

        // モデル準備完了後に全キャッシュと段階転送を破棄する。
        void Clear() noexcept;
        // 指定パスの全用途キャッシュと段階転送を無効化する(path: 更新対象)。
        void Invalidate(
            const std::filesystem::path& path) noexcept;
        // 用途別の画像キャッシュ件数を返す。
        [[nodiscard]] std::size_t CachedTextureCount() const noexcept
        {
            // 画像キャッシュ参照の排他
            std::scoped_lock lock(m_textureMutex);
            return m_textureCache.size();
        }
        // モデルキャッシュ件数を返し取得失敗時は0とする。
        [[nodiscard]] std::size_t CachedModelCount() const noexcept
        {
            try
            {
                // モデルキャッシュ参照の排他
                std::scoped_lock lock(m_modelMutex);
                return m_modelCache.size();
            }
            catch (...)
            {
                return 0;
            }
        }
        // 文字画像キャッシュ件数を返す。
        [[nodiscard]] std::size_t CachedTextCount() const noexcept
        {
            return m_textCache.size();
        }
        // 文字画像キャッシュの推定容量を返す。
        [[nodiscard]] std::size_t
            CachedTextBytes() const noexcept
        {
            return m_textCacheBytes;
        }
        // 使用中の項目のみなら予算超過を許容する。
        // 未使用の古い文字画像を予算まで追い出す(bytes: 目安容量)。
        void SetTextCacheBudgetBytes(
            const std::size_t bytes) noexcept
        {
            m_textCacheBudgetBytes = bytes;
            TrimTextCache();
        }
        // 文字画像キャッシュの目安容量を返す。
        [[nodiscard]] std::size_t
            TextCacheBudgetBytes() const noexcept
        {
            return m_textCacheBudgetBytes;
        }
        // 解析済みアニメーションの件数を返す。
        [[nodiscard]] std::size_t
            CachedAnimationCount() const noexcept
        {
            return m_animationCache.size();
        }
        // 解析済みデータアセットの件数を返す。
        [[nodiscard]] std::size_t
            CachedDataAssetCount() const noexcept
        {
            return m_dataAssetCache.size();
        }
        // メインスレッドでキャッシュ資源の内訳を追記する(entries: 既存内容を保つ追加先)。
        void AppendMemoryEntries(
            std::vector<MemorySnapshotEntry>& entries) const;

    private:
        // 互換資源とBackendを借用して管理を初期化する(device: 互換デバイス, context: 即時コンテキスト, backend: 任意の初期化済みBackend)。
        AssetManager(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            GraphicsBackend* backend);


        struct TextCacheEntry final
        {
            // 共有する文字画像
            std::shared_ptr<TextTextureAsset> asset;
            // 最後に使った順序
            std::uint64_t lastUsed{};
            // 文字画像の推定容量
            std::size_t bytes{};
        };

        // 粗いミップから転送し、完了範囲のみを公開する。
        struct PendingTextureUpload final
        {
            // 公開先の画像アセット
            std::shared_ptr<TextureAsset> asset;
            // 転送先D3D11画像
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            // 転送先の共通画像資源
            GraphicsTextureHandle textureHandle;
            // 未転送のミップ列
            TextureLoader::PreparedTextureData data;
            // 次の転送ミップ番号
            std::ptrdiff_t nextLevel{};
        };

        // 使用中の文字画像を残して古い項目を予算まで追い出す。
        void TrimTextCache() noexcept;
        // パス文字列を小文字化してキーを作る(path: 正規化済み対象)。
        [[nodiscard]] static std::wstring MakeCacheKey(const std::filesystem::path& path);
        // 先読みを使わず全バイトを読む(resolvedPath: 解決済み対象パス)。
        [[nodiscard]] std::vector<std::uint8_t>
            ReadFileBytesUncached(
                const std::filesystem::path& resolvedPath) const;
        // キャッシュを触らず画像を生成する(resolvedPath: 解決済みパス, usage: 圧縮用途, pendingUpload: 必要な段階転送の出力)。
        [[nodiscard]] std::shared_ptr<TextureAsset>
            LoadTextureUncached(
                const std::filesystem::path& resolvedPath,
                TextureLoader::TextureUsage usage,
                std::optional<PendingTextureUpload>& pendingUpload);
        // 形式別に共有キャッシュ外でモデルを作る(resolvedPath: 解決済みパス, context: ワーカーでは空の即時コンテキスト)。
        [[nodiscard]] std::shared_ptr<ModelAsset> LoadModelUncached(
            const std::filesystem::path& resolvedPath,
            ID3D11DeviceContext* context);
        // 共有キャッシュまたは準備結果からモデルを取得する(path: 対象モデル)。
        [[nodiscard]] std::shared_ptr<const ModelAsset> LoadModelImpl(
            const std::filesystem::path& path);
        // 終了受付を確認して実行中のモデル処理数を増やす。
        [[nodiscard]] bool TryBeginGraphicsWork() noexcept;
        // 実行中のモデル処理数を減らし終了待ちへ通知する。
        void EndGraphicsWork() noexcept;
        // 予算待ちを解除して準備結果を破棄し失敗を抑止する。
        void WaitForModelPreparation() noexcept;
        // 専用準備スレッドの登録と予算待ちを解除する。
        void EndModelUploadPreparation() noexcept;
        // 準備スレッドの登録を残して予算待ちを解除する。
        void DisableModelUploadThrottle() noexcept;

        // 借用するD3D11デバイス
        ID3D11Device* m_device{};
        // 借用する即時コンテキスト
        ID3D11DeviceContext* m_context{};
        // 借用する描画Backend
        GraphicsBackend* m_backend{};
        // 絶対アセットルート
        std::filesystem::path m_assetRoot;
        // 暗号化アーカイブ
        std::unique_ptr<AssetArchive> m_archive;
        // アセット参照台帳
        AssetDatabase m_database;
        // 用途別の共有画像
        std::unordered_map<std::wstring, std::shared_ptr<TextureAsset>> m_textureCache;
        // パス別の共有モデル
        std::unordered_map<std::wstring, std::shared_ptr<ModelAsset>> m_modelCache;
        struct PendingModelPreparation final
        {
            // 準備対象のモデルパス
            std::filesystem::path path;
            // モデルの再利用キー
            std::wstring cacheKey;
            // 非同期準備の結果
            std::future<std::shared_ptr<ModelAsset>> future;
            // 開始時のモデル世代
            std::uint64_t generation{};
        };
        // 単一モデルの準備状態
        std::optional<PendingModelPreparation>
            m_pendingModelPreparation;
        // モデル処理数の排他
        mutable std::mutex m_graphicsWorkMutex;
        // モデル処理の終了通知
        std::condition_variable m_graphicsWorkCondition;
        // 実行中のモデル処理数
        std::size_t m_activeGraphicsWork{};
        // 新規モデル処理の受付
        bool m_acceptingGraphicsWork{ true };
        // モデル準備と共有の排他
        mutable std::mutex m_modelMutex;
        // モデル無効化の世代
        std::uint64_t m_modelGeneration{};
        // モデル転送予算の排他
        mutable std::mutex m_modelUploadMutex;
        // 転送予算の補充通知
        std::condition_variable m_modelUploadCondition;
        // 予算を待つ専用スレッド
        std::thread::id m_modelPreparationThread;
        // モデルのフレーム予算
        std::size_t m_modelUploadFrameBudget{
            DefaultModelUploadBudgetPerFrame
        };
        // モデル転送の残り予算
        std::size_t m_modelUploadBudgetRemaining{};
        // 現在フレームの転送容量
        std::size_t m_modelUploadBytesCurrentFrame{};
        // 前フレームの転送容量
        std::atomic<std::size_t>
            m_modelUploadBytesLastFrame{};
        // モデル転送の予算制御
        bool m_modelUploadThrottled{};
        // 共有する白い文字画像
        std::unordered_map<std::wstring, TextCacheEntry> m_textCache;

        // 文字画像の使用順序
        std::uint64_t m_textCacheClock{};
        // 文字画像の推定総容量
        std::size_t m_textCacheBytes{};
        // 使用中画像の予算超過警告
        bool m_textCacheBudgetWarningIssued{};

        // 文字画像の目安容量
        std::size_t m_textCacheBudgetBytes{
            32ull * 1024ull * 1024ull
        };
        // 共有アニメーション
        std::unordered_map<
            std::wstring,
            std::shared_ptr<AnimationClip>>
                m_animationCache;
        // 共有制御設定
        std::unordered_map<
            std::wstring,
            std::shared_ptr<AnimatorController>>
                m_animatorControllerCache;
        // 共有データアセット
        std::unordered_map<
            std::wstring,
            std::shared_ptr<const DataAsset>>
                m_dataAssetCache;

        // 画像キャッシュの排他
        mutable std::mutex m_textureMutex;
        // 全画像の無効化世代
        std::uint64_t m_textureEpoch{};
        // 画像パス別の更新世代
        std::unordered_map<std::wstring, std::uint64_t>
            m_texturePathGenerations;
        // WIC画像のBC圧縮設定
        std::atomic<bool> m_runtimeTextureCompression{};
        // 段階転送の待機画像列
        std::deque<PendingTextureUpload> m_pendingUploads;
        // 段階転送キューの排他
        mutable std::mutex m_uploadMutex;
        // 画像段階転送の閾値
        std::atomic<std::size_t> m_progressiveUploadThreshold{
            DefaultProgressiveUploadThreshold
        };
        // 先読みキャッシュの排他
        mutable std::mutex m_prefetchMutex;
        // 先読みの共有バイト列
        mutable std::unordered_map<
            std::wstring,
            std::shared_ptr<
                const std::vector<std::uint8_t>>>
            m_prefetchedBytes;
        // 先読みの合計容量
        mutable std::size_t
            m_prefetchedByteCount{};
        // 先読みの無効化世代
        std::uint64_t m_prefetchEpoch{};
        // 単一スレッドの文字描画
        Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
        // 文字レイアウトの生成
        Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
        // 文字画像の作成
        Microsoft::WRL::ComPtr<IWICImagingFactory> m_wicFactory;
    };
}
