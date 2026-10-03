#include "LamaPon/Graphics/SpriteRendering.h"

#include "LamaPon/Graphics/GraphicsResource.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace
{
    struct DestructionCounts final
    {
        // ドメイン破棄数
        int domains{};
        // テクスチャ破棄数
        int textures{};
        // バッファー破棄数
        int buffers{};
        // ビュー破棄数
        int views{};
    };

    class TestDomain final
        : public LamaPon::Detail::GraphicsResourceDomain
    {
    public:
        // TestDomain(counts: 破棄数): ドメインの破棄数を追跡する。
        explicit TestDomain(DestructionCounts& counts) noexcept
            : m_counts(counts)
        {
        }

        // ドメイン破棄数を記録する。
        ~TestDomain() noexcept override
        {
            ++m_counts.domains;
        }

    private:
        // 破棄数の記録先
        DestructionCounts& m_counts;
    };

    class TestTexturePayload final
        : public LamaPon::Detail::GraphicsTexturePayload
    {
    public:
        // TestTexturePayload(domain: 所属ドメイン, counts: 破棄数): テクスチャ破棄数を追跡する。
        TestTexturePayload(
            std::shared_ptr<
                LamaPon::Detail::GraphicsResourceDomain> domain,
            DestructionCounts& counts)
            : GraphicsTexturePayload(std::move(domain))
            , m_counts(counts)
        {
        }

        // テクスチャ破棄数を記録する。
        ~TestTexturePayload() noexcept override
        {
            ++m_counts.textures;
        }

    private:
        // 破棄数の記録先
        DestructionCounts& m_counts;
    };

    class TestBufferPayload final
        : public LamaPon::Detail::GraphicsBufferPayload
    {
    public:
        // TestBufferPayload(domain: 所属ドメイン, counts: 破棄数): バッファー破棄数を追跡する。
        TestBufferPayload(
            std::shared_ptr<
                LamaPon::Detail::GraphicsResourceDomain> domain,
            DestructionCounts& counts)
            : GraphicsBufferPayload(std::move(domain))
            , m_counts(counts)
        {
        }

        // バッファー破棄数を記録する。
        ~TestBufferPayload() noexcept override
        {
            ++m_counts.buffers;
        }

    private:
        // 破棄数の記録先
        DestructionCounts& m_counts;
    };

    class TestViewPayload final
        : public LamaPon::Detail::GraphicsViewPayload
    {
    public:
        // TestViewPayload(domain: 所属ドメイン, kind: ビュー種別, resource: テクスチャ, counts: 破棄数): テクスチャビューを構築する。
        TestViewPayload(
            std::shared_ptr<
                LamaPon::Detail::GraphicsResourceDomain> domain,
            const LamaPon::GraphicsViewKind kind,
            LamaPon::GraphicsTextureHandle resource,
            DestructionCounts& counts)
            : GraphicsViewPayload(
                std::move(domain),
                kind,
                std::move(resource))
            , m_counts(counts)
        {
        }

        // TestViewPayload(domain: 所属ドメイン, kind: ビュー種別, resource: バッファー, counts: 破棄数): バッファービューを構築する。
        TestViewPayload(
            std::shared_ptr<
                LamaPon::Detail::GraphicsResourceDomain> domain,
            const LamaPon::GraphicsViewKind kind,
            LamaPon::GraphicsBufferHandle resource,
            DestructionCounts& counts)
            : GraphicsViewPayload(
                std::move(domain),
                kind,
                std::move(resource))
            , m_counts(counts)
        {
        }

        // ビュー破棄数を記録する。
        ~TestViewPayload() noexcept override
        {
            ++m_counts.views;
        }

    private:
        // 破棄数の記録先
        DestructionCounts& m_counts;
    };

    // Require(condition: 成立条件, message: 失敗理由): 条件不成立なら検査を失敗させる。
    void Require(const bool condition, const char* const message)
    {
        // 条件不成立を検出する。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // RequireInvalidArgument(function: 検査処理, message: 失敗理由): invalid_argument の送出を検証する。
    template <typename Function>
    void RequireInvalidArgument(
        Function&& function,
        const char* const message)
    {
        // 検査処理を実行する。
        try
        {
            std::forward<Function>(function)();
        }
        // 期待した例外を成功として扱う。
        catch (const std::invalid_argument&)
        {
            return;
        }
        // 期待外の例外を失敗として扱う。
        catch (...)
        {
        }
        throw std::runtime_error(message);
    }
}

static_assert(std::is_nothrow_default_constructible_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_copy_constructible_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_move_constructible_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_copy_assignable_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_move_assignable_v<
    LamaPon::GraphicsTextureHandle>);
static_assert(std::is_nothrow_destructible_v<
    LamaPon::GraphicsTextureHandle>);

static_assert(std::is_nothrow_default_constructible_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_copy_constructible_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_move_constructible_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_copy_assignable_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_move_assignable_v<
    LamaPon::GraphicsBufferHandle>);
static_assert(std::is_nothrow_destructible_v<
    LamaPon::GraphicsBufferHandle>);

static_assert(std::is_nothrow_default_constructible_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_copy_constructible_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_move_constructible_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_copy_assignable_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_move_assignable_v<
    LamaPon::GraphicsViewHandle>);
static_assert(std::is_nothrow_destructible_v<
    LamaPon::GraphicsViewHandle>);

static_assert(noexcept(
    std::declval<const LamaPon::GraphicsTextureHandle&>()
        == std::declval<const LamaPon::GraphicsTextureHandle&>()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsTextureHandle&>()
        == nullptr));
static_assert(noexcept(
    std::declval<LamaPon::GraphicsTextureHandle&>().Reset()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsBufferHandle&>()
        == std::declval<const LamaPon::GraphicsBufferHandle&>()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsBufferHandle&>()
        == nullptr));
static_assert(noexcept(
    std::declval<LamaPon::GraphicsBufferHandle&>().Reset()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsViewHandle&>()
        == std::declval<const LamaPon::GraphicsViewHandle&>()));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsViewHandle&>()
        == nullptr));
static_assert(noexcept(
    std::declval<const LamaPon::GraphicsViewHandle&>().Kind()));
static_assert(noexcept(
    std::declval<LamaPon::GraphicsViewHandle&>().Reset()));

// main(): グラフィックス資源ハンドルの所有権と検証動作を確認する。
int main()
{
    // 失敗を終了コードへ変換する。
    try
    {
        using LamaPon::Detail::GraphicsResourceHandleAccess;

        // 空のテクスチャハンドル
        LamaPon::GraphicsTextureHandle emptyTexture;
        // 空のバッファーハンドル
        LamaPon::GraphicsBufferHandle emptyBuffer;
        // 空のビュー ハンドル
        LamaPon::GraphicsViewHandle emptyView;
        Require(!emptyTexture && emptyTexture == nullptr,
            "A default texture handle must be empty");
        Require(!emptyBuffer && emptyBuffer == nullptr,
            "A default buffer handle must be empty");
        Require(!emptyView && emptyView == nullptr
                && emptyView.Kind()
                    == LamaPon::GraphicsViewKind::None,
            "A default view handle must be empty and have no kind");
        Require(GraphicsResourceHandleAccess::Domain(emptyTexture)
                    == nullptr
                && GraphicsResourceHandleAccess::Domain(emptyBuffer)
                    == nullptr
                && GraphicsResourceHandleAccess::Domain(emptyView)
                    == nullptr,
            "Empty handles must not expose a resource domain");

        // 資源破棄数の記録先
        DestructionCounts counts;
        // テスト用資源ドメイン
        auto domain = std::make_shared<TestDomain>(counts);
        // 共有されるテクスチャ本体
        auto texturePayload =
            std::make_shared<TestTexturePayload>(domain, counts);
        // テクスチャ資源ハンドル
        auto texture = GraphicsResourceHandleAccess::MakeTexture(
            texturePayload);
        // 同一本体を指す別ハンドル
        auto textureAlias = GraphicsResourceHandleAccess::MakeTexture(
            texturePayload);
        texturePayload.reset();
        Require(texture && texture == textureAlias,
            "Handles made from the same payload must share identity");

        // 複製テクスチャハンドル
        auto textureCopy = texture;
        Require(textureCopy == texture,
            "Copying a texture handle must preserve identity");
        // 移動したテクスチャハンドル
        auto movedTexture = std::move(textureCopy);
        Require(movedTexture == texture && !textureCopy,
            "Moving a texture handle must transfer identity");
        textureAlias.Reset();

        // テクスチャ参照を保持するビュー本体
        auto viewPayload = std::make_shared<TestViewPayload>(
            domain,
            LamaPon::GraphicsViewKind::ShaderResource,
            texture,
            counts);
        // テクスチャビュー ハンドル
        auto view = GraphicsResourceHandleAccess::MakeView(
            std::move(viewPayload));
        // 複製ビュー ハンドル
        auto viewCopy = view;
        Require(viewCopy == view
                && view.Kind()
                    == LamaPon::GraphicsViewKind::ShaderResource,
            "A copied view must preserve identity and kind");
        Require(
            GraphicsResourceHandleAccess::TextureResource(view)
                != nullptr
                && GraphicsResourceHandleAccess::BufferResource(view)
                    == nullptr,
            "A texture view must retain its texture resource type");

        texture.Reset();
        movedTexture.Reset();
        Require(counts.textures == 0,
            "A view must strongly own its texture resource");
        view.Reset();
        Require(counts.textures == 0 && counts.views == 0,
            "A copied view must share ownership of its payload");
        // 移動したビュー ハンドル
        auto movedView = std::move(viewCopy);
        Require(!viewCopy
                && movedView.Kind()
                    == LamaPon::GraphicsViewKind::ShaderResource,
            "Moving a view must empty the source and preserve its kind");
        movedView.Reset();
        Require(counts.textures == 1 && counts.views == 1,
            "The last view must release its view and texture payloads");

        // 共有されるバッファー本体
        auto bufferPayload =
            std::make_shared<TestBufferPayload>(domain, counts);
        // バッファー資源ハンドル
        auto buffer = GraphicsResourceHandleAccess::MakeBuffer(
            std::move(bufferPayload));
        // 複製バッファーハンドル
        auto bufferCopy = buffer;
        Require(bufferCopy == buffer,
            "Copying a buffer handle must preserve identity");
        // 移動したバッファーハンドル
        auto movedBuffer = std::move(bufferCopy);
        Require(movedBuffer == buffer && !bufferCopy,
            "Moving a buffer handle must transfer identity");
        // バッファー参照を保持するビュー本体
        auto bufferViewPayload = std::make_shared<TestViewPayload>(
            domain,
            LamaPon::GraphicsViewKind::UnorderedAccess,
            buffer,
            counts);
        // バッファービュー ハンドル
        auto bufferView = GraphicsResourceHandleAccess::MakeView(
            std::move(bufferViewPayload));
        Require(
            GraphicsResourceHandleAccess::TextureResource(bufferView)
                == nullptr
                && GraphicsResourceHandleAccess::BufferResource(bufferView)
                    != nullptr,
            "A buffer view must retain its buffer resource type");
        buffer.Reset();
        movedBuffer.Reset();
        Require(counts.buffers == 0,
            "A view must strongly own its buffer resource");
        bufferView.Reset();
        Require(counts.buffers == 1 && counts.views == 2,
            "The last buffer view must release its buffer payload");

        // 別所有元の資源ドメイン
        auto foreignDomain =
            std::make_shared<TestDomain>(counts);
        // 別ドメインに属するテクスチャ
        auto foreignTexture =
            GraphicsResourceHandleAccess::MakeTexture(
                std::make_shared<TestTexturePayload>(
                    foreignDomain,
                    counts));
        RequireInvalidArgument(
            // 異なるドメインの資源を拒否する。
            [&]
            {
                static_cast<void>(
                    std::make_shared<TestViewPayload>(
                        domain,
                        LamaPon::GraphicsViewKind::ShaderResource,
                        foreignTexture,
                        counts));
            },
            "A view must reject a resource from another domain");
        RequireInvalidArgument(
            // None 種別を拒否する。
            [&]
            {
                static_cast<void>(
                    std::make_shared<TestViewPayload>(
                        foreignDomain,
                        LamaPon::GraphicsViewKind::None,
                        foreignTexture,
                        counts));
            },
            "A non-empty view payload must reject the None kind");
        RequireInvalidArgument(
            // 空の資源を拒否する。
            [&]
            {
                static_cast<void>(
                    std::make_shared<TestViewPayload>(
                        domain,
                        LamaPon::GraphicsViewKind::ShaderResource,
                        LamaPon::GraphicsTextureHandle{},
                        counts));
            },
            "A view payload must reject an empty resource");

        foreignTexture.Reset();
        foreignDomain.reset();
        Require(counts.textures == 2 && counts.domains == 1,
            "A resource payload must strongly own its domain");
        domain.reset();
        Require(counts.domains == 2,
            "The last resource owner must release its domain");
    }
    // 例外(error: 失敗情報)を終了コードへ変換する。
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
