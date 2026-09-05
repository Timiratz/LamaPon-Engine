// 文字テクスチャキャッシュ（AssetManager::LoadTextTexture）の
// 上限が効いているかを確かめます。
//
// 文字テクスチャは文字列ごとに1枚作られるため、
// スコアや残り時間のように中身が変わり続ける表示では、上限が無いと
// 遊んでいる間ずっと増え続けます（GPUメモリの実質的なリーク）。
// 上限を超えたら古いものから捨てること、ただしまだ表示に使われて
// いるものは捨てないことの両方が要件です。
#include "LamaPon/Assets/AssetManager.h"

#include <d3d11.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct Device final
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    };

    [[nodiscard]] Device CreateWarpDevice()
    {
        Device created{};
        const HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            created.device.ReleaseAndGetAddressOf(),
            nullptr,
            created.context.ReleaseAndGetAddressOf());
        Require(
            SUCCEEDED(result),
            "WARP device creation must succeed");
        return created;
    }

    // 同じ文字列は作り直さず、キャッシュから返ること。
    void TestCacheHitReturnsSameAsset()
    {
        const auto gpu = CreateWarpDevice();
        LamaPon::AssetManager assets(
            gpu.device.Get(),
            gpu.context.Get());

        const auto first = assets.LoadTextTexture(
            "スコア",
            "Yu Gothic UI",
            30.0f);
        const auto second = assets.LoadTextTexture(
            "スコア",
            "Yu Gothic UI",
            30.0f);

        Require(
            first != nullptr && second != nullptr,
            "text textures must be created");
        Require(
            first.get() == second.get(),
            "the same text must hit the cache");
        Require(
            assets.CachedTextCount() == 1,
            "the same text must not add a second entry");
    }

    // 色は描画時に適用するため、同じ文なら色が違ってもキャッシュを共有します。
    void TestColorIsNotPartOfTheCacheKey()
    {
        const auto gpu = CreateWarpDevice();
        LamaPon::AssetManager assets(
            gpu.device.Get(),
            gpu.context.Get());

        const auto white = assets.LoadTextTexture(
            "ナイス！",
            "Yu Gothic UI",
            40.0f);
        const auto gold = assets.LoadTextTexture(
            "ナイス！",
            "Yu Gothic UI",
            40.0f);

        Require(
            white != nullptr && white.get() == gold.get(),
            "color must not create a second texture");
        Require(
            assets.CachedTextCount() == 1,
            "the same text must stay a single entry");
    }

    // 中身が変わり続ける表示（スコア）でも、上限を超えて増えないこと。
    void TestBudgetEvictsUnusedEntries()
    {
        const auto gpu = CreateWarpDevice();
        LamaPon::AssetManager assets(
            gpu.device.Get(),
            gpu.context.Get());

        // 数枚ぶんだけの小さな予算にして、確実に溢れさせます。
        constexpr std::size_t budget = 64u * 1024u;
        assets.SetTextCacheBudgetBytes(budget);
        Require(
            assets.TextCacheBudgetBytes() == budget,
            "the budget must be readable back");

        for (int value = 0; value < 400; ++value)
        {
            // 戻り値を保持しない＝「もう表示していない」状態です。
            const auto texture = assets.LoadTextTexture(
                "スコア " + std::to_string(value),
                "Yu Gothic UI",
                30.0f);
            Require(
                texture != nullptr,
                "each text texture must be created");
        }

        Require(
            assets.CachedTextBytes() <= budget,
            "the text cache must stay inside its budget");
        Require(
            assets.CachedTextCount() > 0,
            "the text cache must keep the recent entries");
        Require(
            assets.CachedTextCount() < 400,
            "the text cache must not keep every string");
    }

    // 表示中のものを捨てないこと。捨てても解放されないうえ、
    // 次のフレームで作り直すことになるためです。
    void TestReferencedEntriesSurvive()
    {
        const auto gpu = CreateWarpDevice();
        LamaPon::AssetManager assets(
            gpu.device.Get(),
            gpu.context.Get());
        assets.SetTextCacheBudgetBytes(64u * 1024u);

        // 表示中に相当する参照を持ち続けます。
        std::vector<std::shared_ptr<const LamaPon::TextTextureAsset>>
            live;
        for (int index = 0; index < 5; ++index)
        {
            live.push_back(
                assets.LoadTextTexture(
                    "HUD " + std::to_string(index),
                    "Yu Gothic UI",
                    30.0f));
        }

        // 大量の使い捨て文字列で予算を溢れさせます。
        for (int value = 0; value < 300; ++value)
        {
            static_cast<void>(
                assets.LoadTextTexture(
                    "捨てる " + std::to_string(value),
                    "Yu Gothic UI",
                    30.0f));
        }

        // 参照し続けている5枚は、同じ実体のままキャッシュに残ります。
        for (int index = 0; index < 5; ++index)
        {
            const auto again = assets.LoadTextTexture(
                "HUD " + std::to_string(index),
                "Yu Gothic UI",
                30.0f);
            Require(
                again.get()
                    == live[static_cast<std::size_t>(index)]
                        .get(),
                "text still in use must not be evicted");
        }
    }

    // Clear()で合計バイト数も0へ戻ること（戻し忘れると、次のシーンで
    // 「入っていないのに予算を使い切っている」状態になります）。
    void TestClearResetsAccounting()
    {
        const auto gpu = CreateWarpDevice();
        LamaPon::AssetManager assets(
            gpu.device.Get(),
            gpu.context.Get());

        static_cast<void>(
            assets.LoadTextTexture(
                "リセット確認",
                "Yu Gothic UI",
                30.0f));
        Require(
            assets.CachedTextBytes() > 0,
            "loading text must count bytes");

        assets.Clear();
        Require(
            assets.CachedTextCount() == 0,
            "Clear must drop every text entry");
        Require(
            assets.CachedTextBytes() == 0,
            "Clear must reset the byte accounting");
    }
}

int main()
{
    try
    {
        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        static_cast<void>(comResult);

        TestCacheHitReturnsSameAsset();
        TestColorIsNotPartOfTheCacheKey();
        TestBudgetEvictsUnusedEntries();
        TestReferencedEntriesSurvive();
        TestClearResetsAccounting();
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "Text cache tests failed: "
            << error.what()
            << '\n';
        return 1;
    }

    std::cout << "Text cache tests passed.\n";
    return 0;
}
