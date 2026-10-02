#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

namespace LamaPon
{
    // バックエンド共通のテクスチャ形式で、列挙値の互換性を保つため追加は末尾へ置く。
    enum class GraphicsTextureFormat : std::uint8_t
    {
        Rgba8Unorm,
        Bgra8Unorm,
        Bc1Unorm,
        Bc3Unorm,
        Bc5Unorm,
        Rgba16Float,
        R8Unorm,
        Rg8Unorm,
        Rgba8UnormSrgb,
        Bgra8UnormSrgb,
        Bgrx8Unorm,
        Bgrx8UnormSrgb,
        Bc1UnormSrgb,
        Bc2Unorm,
        Bc2UnormSrgb,
        Bc3UnormSrgb,
        Bc4Unorm,
        Bc4Snorm,
        Bc5Snorm,
        Bc6hUf16,
        Bc6hSf16,
        Bc7Unorm,
        Bc7UnormSrgb,
        R16Float,
        Rg16Float,
        R32Float,
        Rg32Float,
        Rgba32Float,
        R10g10b10a2Unorm,
        Rg16Unorm,
        B5g5r5a1Unorm,
        B5g6r5Unorm,
        B4g4r4a4Unorm,
        R16Unorm,
        A8Unorm,
        Rg8Snorm,
        Rgba8Snorm,
        Rg16Snorm,
        Rgba16Unorm,
        Rgba16Snorm,
        R8g8B8g8Unorm,
        G8r8G8b8Unorm,
        R8Snorm,
        R16Snorm,
        R11g11b10Float,
        R9g9b9e5SharedExp
    };

    // Immutableは全ミップの初期値を必須とし、PerMipUpdateはミップ単位の後続転送を許可する。
    enum class GraphicsTextureUpdateMode : std::uint8_t
    {
        Immutable,
        PerMipUpdate
    };

    struct GraphicsTexture2DDescription final
    {
        // テクスチャの幅
        std::uint32_t width{};
        // テクスチャの高さ
        std::uint32_t height{};
        // 作成するミップ数
        std::uint32_t mipLevels{ 1 };
        // テクセルの格納形式
        GraphicsTextureFormat format{
            GraphicsTextureFormat::Rgba8Unorm };
        // ミップ単位の更新許可
        GraphicsTextureUpdateMode updateMode{
            GraphicsTextureUpdateMode::Immutable };
    };

    // 全ミップの初期値を生成時に渡す不変の3Dテクスチャ設定。
    struct GraphicsTexture3DDescription final
    {
        // テクスチャの幅
        std::uint32_t width{};
        // テクスチャの高さ
        std::uint32_t height{};
        // テクスチャの奥行
        std::uint32_t depth{};
        // 作成するミップ数
        std::uint32_t mipLevels{ 1 };
        // テクセルの格納形式
        GraphicsTextureFormat format{
            GraphicsTextureFormat::Rgba8Unorm };
    };

    // 呼出中だけ借用する転送範囲で、2DのslicePitchは0なら内容サイズを使い、3Dは明示する。
    struct GraphicsTextureSubresourceData final
    {
        // 借用する転送バイト列
        std::span<const std::byte> bytes;
        // 一行のバイト数
        std::uint32_t rowPitch{};
        // 隣接する奥行面までのバイト数
        std::uint32_t slicePitch{};
    };

    // シェーダーへ公開するミップ範囲。
    struct GraphicsTextureViewDescription final
    {
        // 公開する最初のミップ
        std::uint32_t mostDetailedMip{};
        // 公開するミップ数
        std::uint32_t mipLevels{ 1 };
    };

    // ネイティブビュー解決時の用途検証に使う、描画API共通のビュー種別。
    enum class GraphicsViewKind : std::uint8_t
    {
        None,
        ShaderResource,
        RenderTarget,
        DepthStencil,
        UnorderedAccess
    };

    namespace Detail
    {
        class GraphicsResourceDomain;
        class GraphicsTexturePayload;
        class GraphicsBufferPayload;
        class GraphicsViewPayload;
        class GraphicsResourceHandleAccess;
    }

    // テクスチャを共有所有し、既定構築・移動後・Reset後は空となるハンドル。
    class GraphicsTextureHandle final
    {
    public:
        // 空のテクスチャハンドルを作成する。
        GraphicsTextureHandle() noexcept = default;
        // テクスチャの共有参照を複製する。
        GraphicsTextureHandle(
            const GraphicsTextureHandle&) noexcept = default;
        // テクスチャの共有参照を移し、元を空にする。
        GraphicsTextureHandle(
            GraphicsTextureHandle&&) noexcept = default;
        // テクスチャの共有参照をコピー代入する。
        GraphicsTextureHandle& operator=(
            const GraphicsTextureHandle&) noexcept = default;
        // テクスチャの共有参照を移動代入する。
        GraphicsTextureHandle& operator=(
            GraphicsTextureHandle&&) noexcept = default;
        // このハンドルの共有参照を解放する。
        ~GraphicsTextureHandle() noexcept = default;

        // 実体を所有しているか判定する。
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_payload != nullptr;
        }

        // このハンドルの共有参照を解除する。
        void Reset() noexcept
        {
            m_payload.reset();
        }

        // 共有実体のアドレスが同じか判定する(left: 左のハンドル, right: 右のハンドル)。
        friend bool operator==(
            const GraphicsTextureHandle& left,
            const GraphicsTextureHandle& right) noexcept
        {
            return left.m_payload == right.m_payload;
        }

        // 共有実体が空か判定する(handle: 確認するハンドル)。
        friend bool operator==(
            const GraphicsTextureHandle& handle,
            std::nullptr_t) noexcept
        {
            return handle.m_payload == nullptr;
        }

    private:
        // 共有実体をハンドルへ包み、ヌルなら空を作る(payload: 所有するテクスチャ実体)。
        explicit GraphicsTextureHandle(
            std::shared_ptr<Detail::GraphicsTexturePayload>
                payload) noexcept
            : m_payload(std::move(payload))
        {
        }

        // 共有所有するテクスチャ実体
        std::shared_ptr<Detail::GraphicsTexturePayload>
            m_payload;

        friend class Detail::GraphicsViewPayload;
        friend class Detail::GraphicsResourceHandleAccess;
    };

    // バッファーを共有所有し、既定構築・移動後・Reset後は空となるハンドル。
    class GraphicsBufferHandle final
    {
    public:
        // 空のバッファーハンドルを作成する。
        GraphicsBufferHandle() noexcept = default;
        // バッファーの共有参照を複製する。
        GraphicsBufferHandle(
            const GraphicsBufferHandle&) noexcept = default;
        // バッファーの共有参照を移し、元を空にする。
        GraphicsBufferHandle(
            GraphicsBufferHandle&&) noexcept = default;
        // バッファーの共有参照をコピー代入する。
        GraphicsBufferHandle& operator=(
            const GraphicsBufferHandle&) noexcept = default;
        // バッファーの共有参照を移動代入する。
        GraphicsBufferHandle& operator=(
            GraphicsBufferHandle&&) noexcept = default;
        // このハンドルの共有参照を解放する。
        ~GraphicsBufferHandle() noexcept = default;

        // 実体を所有しているか判定する。
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_payload != nullptr;
        }

        // このハンドルの共有参照を解除する。
        void Reset() noexcept
        {
            m_payload.reset();
        }

        // 共有実体のアドレスが同じか判定する(left: 左のハンドル, right: 右のハンドル)。
        friend bool operator==(
            const GraphicsBufferHandle& left,
            const GraphicsBufferHandle& right) noexcept
        {
            return left.m_payload == right.m_payload;
        }

        // 共有実体が空か判定する(handle: 確認するハンドル)。
        friend bool operator==(
            const GraphicsBufferHandle& handle,
            std::nullptr_t) noexcept
        {
            return handle.m_payload == nullptr;
        }

    private:
        // 共有実体をハンドルへ包み、ヌルなら空を作る(payload: 所有するバッファー実体)。
        explicit GraphicsBufferHandle(
            std::shared_ptr<Detail::GraphicsBufferPayload>
                payload) noexcept
            : m_payload(std::move(payload))
        {
        }

        // 共有所有するバッファー実体
        std::shared_ptr<Detail::GraphicsBufferPayload>
            m_payload;

        friend class Detail::GraphicsViewPayload;
        friend class Detail::GraphicsResourceHandleAccess;
    };

    namespace Detail
    {
        // 資源を解放する状態を保持するバックエンドの初期化世代。
        // 初期化のたびに新規作成し、ネイティブ資源の解決では同じ世代の実体だけを受け付ける。
        // 最後の参照は任意のスレッドで解放され得るため、D3D12の資源と記述子はフェンス完了まで遅延破棄する。
        class GraphicsResourceDomain
        {
        public:
            // 派生側を含む初期化世代の所有状態を解放する。
            virtual ~GraphicsResourceDomain() noexcept = default;

            // 初期化世代のコピーを禁止する。
            GraphicsResourceDomain(
                const GraphicsResourceDomain&) = delete;
            // 初期化世代のコピー代入を禁止する。
            GraphicsResourceDomain& operator=(
                const GraphicsResourceDomain&) = delete;

        protected:
            // 資源の所有世代を作成する。
            GraphicsResourceDomain() noexcept = default;
        };

        // 初期化世代を共有所有し、バックエンド終了後も資源解放に必要な状態を保持する基底。
        class GraphicsResourcePayload
        {
        public:
            // 共有する初期化世代への参照を解放する。
            virtual ~GraphicsResourcePayload() noexcept = default;

            // 資源実体のコピーを禁止する。
            GraphicsResourcePayload(
                const GraphicsResourcePayload&) = delete;
            // 資源実体のコピー代入を禁止する。
            GraphicsResourcePayload& operator=(
                const GraphicsResourcePayload&) = delete;

            // 共有所有する初期化世代への参照を借用する。
            [[nodiscard]] const std::shared_ptr<
                GraphicsResourceDomain>& Domain() const noexcept
            {
                return m_domain;
            }

        protected:
            // 初期化世代を共有所有し、ヌルなら例外を送出する(domain: 資源が属する共有世代)。
            explicit GraphicsResourcePayload(
                std::shared_ptr<GraphicsResourceDomain> domain)
                : m_domain(std::move(domain))
            {
                if (m_domain == nullptr)
                {
                    throw std::invalid_argument(
                        "A graphics resource payload requires a domain.");
                }
            }

        private:
            // 資源の解放状態を保持する共有世代
            std::shared_ptr<GraphicsResourceDomain> m_domain;
        };

        class GraphicsTexturePayload
            : public GraphicsResourcePayload
        {
        public:
            // 派生側を含むテクスチャ実体を解放する。
            ~GraphicsTexturePayload() noexcept override = default;

        protected:
            using GraphicsResourcePayload::GraphicsResourcePayload;
        };

        class GraphicsBufferPayload
            : public GraphicsResourcePayload
        {
        public:
            // 派生側を含むバッファー実体を解放する。
            ~GraphicsBufferPayload() noexcept override = default;

        protected:
            using GraphicsResourcePayload::GraphicsResourcePayload;
        };

        // ビュー実体と参照先資源を共有所有して、描画API間で同じ寿命を保証する。
        // 種別None・空の資源・世代の不一致はinvalid_argumentで拒否する。
        class GraphicsViewPayload
            : public GraphicsResourcePayload
        {
        public:
            // 派生側を含むビューと参照先資源を解放する。
            ~GraphicsViewPayload() noexcept override = default;

            // ビューの参照用途を返す。
            [[nodiscard]] GraphicsViewKind Kind() const noexcept
            {
                return m_kind;
            }

        protected:
            // 同じ世代のテクスチャを参照するビューを作る(domain: 所有する世代, kind: ビューの用途, resource: 所有する参照先)。
            GraphicsViewPayload(
                std::shared_ptr<GraphicsResourceDomain> domain,
                const GraphicsViewKind kind,
                GraphicsTextureHandle resource)
                : GraphicsResourcePayload(std::move(domain))
                , m_kind(kind)
                , m_resource(std::move(resource))
            {
                ValidateTextureResource();
            }

            // 同じ世代のバッファーを参照するビューを作る(domain: 所有する世代, kind: ビューの用途, resource: 所有する参照先)。
            GraphicsViewPayload(
                std::shared_ptr<GraphicsResourceDomain> domain,
                const GraphicsViewKind kind,
                GraphicsBufferHandle resource)
                : GraphicsResourcePayload(std::move(domain))
                , m_kind(kind)
                , m_resource(std::move(resource))
            {
                ValidateBufferResource();
            }

        private:
            // ビュー種別がNoneならinvalid_argumentを送出する。
            void ValidateKind() const
            {
                if (m_kind == GraphicsViewKind::None)
                {
                    throw std::invalid_argument(
                        "A graphics view payload requires a view kind.");
                }
            }

            // テクスチャの不在または世代の不一致ならinvalid_argumentを送出する。
            void ValidateTextureResource() const
            {
                ValidateKind();
                // 検証するビューの参照先資源
                const auto& resource =
                    std::get<GraphicsTextureHandle>(m_resource);
                if (resource.m_payload == nullptr)
                {
                    throw std::invalid_argument(
                        "A graphics view payload requires a resource.");
                }
                if (resource.m_payload->Domain().get()
                    != Domain().get())
                {
                    throw std::invalid_argument(
                        "A graphics view and its resource must share a domain.");
                }
            }

            // バッファーの不在または世代の不一致ならinvalid_argumentを送出する。
            void ValidateBufferResource() const
            {
                ValidateKind();
                // 検証するビューの参照先資源
                const auto& resource =
                    std::get<GraphicsBufferHandle>(m_resource);
                if (resource.m_payload == nullptr)
                {
                    throw std::invalid_argument(
                        "A graphics view payload requires a resource.");
                }
                if (resource.m_payload->Domain().get()
                    != Domain().get())
                {
                    throw std::invalid_argument(
                        "A graphics view and its resource must share a domain.");
                }
            }

            // ビューの参照用途
            GraphicsViewKind m_kind{ GraphicsViewKind::None };
            // ビューから共有所有する参照先資源
            std::variant<
                GraphicsTextureHandle,
                GraphicsBufferHandle> m_resource;

            friend class GraphicsResourceHandleAccess;
        };
    }

    // ビューと参照先資源を共有所有し、既定構築・移動後・Reset後は空となるハンドル。
    class GraphicsViewHandle final
    {
    public:
        // 空のビューハンドルを作成する。
        GraphicsViewHandle() noexcept = default;
        // ビューと参照先の共有参照を複製する。
        GraphicsViewHandle(
            const GraphicsViewHandle&) noexcept = default;
        // ビューの共有参照を移し、元を空にする。
        GraphicsViewHandle(
            GraphicsViewHandle&&) noexcept = default;
        // ビューの共有参照をコピー代入する。
        GraphicsViewHandle& operator=(
            const GraphicsViewHandle&) noexcept = default;
        // ビューの共有参照を移動代入する。
        GraphicsViewHandle& operator=(
            GraphicsViewHandle&&) noexcept = default;
        // このハンドルの共有参照を解放する。
        ~GraphicsViewHandle() noexcept = default;

        // 実体を所有しているか判定する。
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return m_payload != nullptr;
        }

        // ビューの参照用途を返し、空ならNoneを返す。
        [[nodiscard]] GraphicsViewKind Kind() const noexcept
        {
            return m_payload != nullptr
                ? m_payload->Kind()
                : GraphicsViewKind::None;
        }

        // このハンドルの共有参照を解除する。
        void Reset() noexcept
        {
            m_payload.reset();
        }

        // 共有実体のアドレスが同じか判定する(left: 左のハンドル, right: 右のハンドル)。
        friend bool operator==(
            const GraphicsViewHandle& left,
            const GraphicsViewHandle& right) noexcept
        {
            return left.m_payload == right.m_payload;
        }

        // 共有実体が空か判定する(handle: 確認するハンドル)。
        friend bool operator==(
            const GraphicsViewHandle& handle,
            std::nullptr_t) noexcept
        {
            return handle.m_payload == nullptr;
        }

    private:
        // 共有実体をハンドルへ包み、ヌルなら空を作る(payload: 所有するビュー実体)。
        explicit GraphicsViewHandle(
            std::shared_ptr<Detail::GraphicsViewPayload>
                payload) noexcept
            : m_payload(std::move(payload))
        {
        }

        // 共有所有するビュー実体
        std::shared_ptr<Detail::GraphicsViewPayload> m_payload;

        friend class Detail::GraphicsResourceHandleAccess;
    };

    namespace Detail
    {
        // バックエンドだけがハンドルの共有実体へアクセスする窓口。
        // 借用した実体・世代・資源への参照は、所有元ハンドルの保持中だけ使う。
        class GraphicsResourceHandleAccess final
        {
        public:
            // 共有実体をテクスチャハンドルで包み、ヌルなら空を返す(payload: 所有するテクスチャ実体)。
            [[nodiscard]] static GraphicsTextureHandle MakeTexture(
                std::shared_ptr<GraphicsTexturePayload>
                    payload) noexcept
            {
                return GraphicsTextureHandle{ std::move(payload) };
            }

            // 共有実体をバッファーハンドルで包み、ヌルなら空を返す(payload: 所有するバッファー実体)。
            [[nodiscard]] static GraphicsBufferHandle MakeBuffer(
                std::shared_ptr<GraphicsBufferPayload>
                    payload) noexcept
            {
                return GraphicsBufferHandle{ std::move(payload) };
            }

            // 共有実体をビューハンドルで包み、ヌルなら空を返す(payload: 所有するビュー実体)。
            [[nodiscard]] static GraphicsViewHandle MakeView(
                std::shared_ptr<GraphicsViewPayload>
                    payload) noexcept
            {
                return GraphicsViewHandle{ std::move(payload) };
            }

            // テクスチャ実体を借用し、空ならヌルを返す(handle: 参照するハンドル)。
            [[nodiscard]] static const GraphicsTexturePayload* Payload(
                const GraphicsTextureHandle& handle) noexcept
            {
                return handle.m_payload.get();
            }

            // バッファー実体を借用し、空ならヌルを返す(handle: 参照するハンドル)。
            [[nodiscard]] static const GraphicsBufferPayload* Payload(
                const GraphicsBufferHandle& handle) noexcept
            {
                return handle.m_payload.get();
            }

            // ビュー実体を借用し、空ならヌルを返す(handle: 参照するハンドル)。
            [[nodiscard]] static const GraphicsViewPayload* Payload(
                const GraphicsViewHandle& handle) noexcept
            {
                return handle.m_payload.get();
            }

            // 所有する初期化世代を借用し、空ならヌルを返す(handle: テクスチャハンドル)。
            [[nodiscard]] static const GraphicsResourceDomain* Domain(
                const GraphicsTextureHandle& handle) noexcept
            {
                return handle.m_payload != nullptr
                    ? handle.m_payload->Domain().get()
                    : nullptr;
            }

            // 所有する初期化世代を借用し、空ならヌルを返す(handle: バッファーハンドル)。
            [[nodiscard]] static const GraphicsResourceDomain* Domain(
                const GraphicsBufferHandle& handle) noexcept
            {
                return handle.m_payload != nullptr
                    ? handle.m_payload->Domain().get()
                    : nullptr;
            }

            // 所有する初期化世代を借用し、空ならヌルを返す(handle: ビューハンドル)。
            [[nodiscard]] static const GraphicsResourceDomain* Domain(
                const GraphicsViewHandle& handle) noexcept
            {
                return handle.m_payload != nullptr
                    ? handle.m_payload->Domain().get()
                    : nullptr;
            }

            // ビューのテクスチャ参照を借用し、空または別種ならヌルを返す(handle: 参照するビュー)。
            [[nodiscard]] static const GraphicsTextureHandle*
                TextureResource(
                    const GraphicsViewHandle& handle) noexcept
            {
                if (handle.m_payload == nullptr)
                {
                    return nullptr;
                }
                return std::get_if<GraphicsTextureHandle>(
                    &handle.m_payload->m_resource);
            }

            // ビューのバッファー参照を借用し、空または別種ならヌルを返す(handle: 参照するビュー)。
            [[nodiscard]] static const GraphicsBufferHandle*
                BufferResource(
                    const GraphicsViewHandle& handle) noexcept
            {
                if (handle.m_payload == nullptr)
                {
                    return nullptr;
                }
                return std::get_if<GraphicsBufferHandle>(
                    &handle.m_payload->m_resource);
            }
        };
    }
}
