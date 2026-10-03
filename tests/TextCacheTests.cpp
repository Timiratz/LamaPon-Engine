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
    // 条件不成立ならテストを失敗させます。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(
        const bool condition,
        const char* message)
    {
        // assertion失敗を例外で通知
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct Device final
    {
        // WARP描画デバイス
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        // WARP即時描画コンテキスト
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    };

    // テキスト描画テスト用のWARP deviceを作ります。
    [[nodiscard]] Device CreateWarpDevice()
    {
        // 作成したWARP deviceとcontext
        Device created{};
        // D3D11 device作成結果
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

    // 同じtextの再読込で同一assetを返すことを確認
    void TestCacheHitReturnsSameAsset()
    {
        // テスト用WARP device
        const auto gpu = CreateWarpDevice();
        // text textureを作成・保持する資産管理
        LamaPon::AssetManager assets(
            gpu.device.Get(),
            gpu.context.Get());

        // 初回に作るtext texture
        const auto first = assets.LoadTextTexture(
            "スコア",
            "Yu Gothic UI",
            30.0f);
        // 同じkeyでキャッシュから取得するtexture
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

    // 色がcache keyに含まれずtextを共有することを確認
    void TestColorIsNotPartOfTheCacheKey()
    {
        // テスト用WARP device
        const auto gpu = CreateWarpDevice();
        // text textureを作成・保持する資産管理
        LamaPon::AssetManager assets(
            gpu.device.Get(),
            gpu.context.Get());

        // 白色表示用に取得するtext texture
        const auto white = assets.LoadTextTexture(
            "ナイス！",
            "Yu Gothic UI",
            40.0f);
        // 金色表示用に取得する同じtext texture
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

    // 未参照entryを退避してcache byte上限を守ることを確認
    void TestBudgetEvictsUnusedEntries()
    {
        // テスト用WARP device
        const auto gpu = CreateWarpDevice();
        // text textureを作成・保持する資産管理
        LamaPon::AssetManager assets(
            gpu.device.Get(),
            gpu.context.Get());

        // 数枚ぶんだけの小さな予算にして、確実に溢れさせます。
        // 退避を発生させる小さいcache上限
        constexpr std::size_t budget = 64u * 1024u;
        assets.SetTextCacheBudgetBytes(budget);
        Require(
            assets.TextCacheBudgetBytes() == budget,
            "the budget must be readable back");

        // value: 上限を超える異なるスコア文字列番号
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

    // 外部参照中のtextureをcacheから退避しないことを確認
    void TestReferencedEntriesSurvive()
    {
        // テスト用WARP device
        const auto gpu = CreateWarpDevice();
        // text textureを作成・保持する資産管理
        LamaPon::AssetManager assets(
            gpu.device.Get(),
            gpu.context.Get());
        assets.SetTextCacheBudgetBytes(64u * 1024u);

        // live: 描画中に相当する保持参照
        std::vector<std::shared_ptr<const LamaPon::TextTextureAsset>>
            live;
        // index: 参照保持するHUD texture番号
        for (int index = 0; index < 5; ++index)
        {
            live.push_back(
                assets.LoadTextTexture(
                    "HUD " + std::to_string(index),
                    "Yu Gothic UI",
                    30.0f));
        }

        // value: cache上限を超えさせる使い捨て文字列番号
        for (int value = 0; value < 300; ++value)
        {
            static_cast<void>(
                assets.LoadTextTexture(
                    "捨てる " + std::to_string(value),
                    "Yu Gothic UI",
                    30.0f));
        }

        // 外部参照中の5件が同じassetのまま残ることを確認
        // index: asset identityを再確認するHUD番号
        for (int index = 0; index < 5; ++index)
        {
            // 既存HUD keyから再取得したtexture
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

    // Clear後にcache件数とbyte集計が0へ戻ることを確認
    void TestClearResetsAccounting()
    {
        // テスト用WARP device
        const auto gpu = CreateWarpDevice();
        // text textureを作成・保持する資産管理
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

// TextTexture cache hit・上限退避・clear処理を検証します。
int main()
{
    // COM初期化とcache検証を実行
    // テスト例外を失敗終了コードへ変換
    try
    {
        // COM初期化を試みた結果
        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        static_cast<void>(comResult);
        TestCacheHitReturnsSameAsset();
        TestColorIsNotPartOfTheCacheKey();
        TestBudgetEvictsUnusedEntries();
        TestReferencedEntriesSurvive();
        TestClearResetsAccounting();
    }
    // テスト例外を標準エラーと失敗終了コードへ変換
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
