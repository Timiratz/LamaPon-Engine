#include "LamaPon/Graphics/TemporalJitter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// Halton列の分布と射影行列への適用量を検証します。
namespace
{
    // 失敗したassertionの件数
    int g_failures = 0;

    // 条件不成立を失敗一覧へ記録します。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(
        const bool condition,
        const std::string& message)
    {
        // assertion失敗を集計する
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    // leftとrightの差がtolerance内か返します。
    // NearlyEqual(left: 左値, right: 右値, tolerance: 許容誤差)
    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right,
        const float tolerance = 1.0e-5f) noexcept
    {
        return std::abs(left - right) <= tolerance;
    }
}

// Halton列と射影行列のジッター量を検証します。
int main()
{
    // Halton列の既知値を確認
    Require(
        NearlyEqual(
            LamaPon::HaltonSequence(1u, 2u), 0.5f),
        "Halton(1,2) must be 1/2.");
    Require(
        NearlyEqual(
            LamaPon::HaltonSequence(2u, 2u), 0.25f),
        "Halton(2,2) must be 1/4.");
    Require(
        NearlyEqual(
            LamaPon::HaltonSequence(3u, 2u), 0.75f),
        "Halton(3,2) must be 3/4.");
    Require(
        NearlyEqual(
            LamaPon::HaltonSequence(4u, 2u), 0.125f),
        "Halton(4,2) must be 1/8.");
    Require(
        NearlyEqual(
            LamaPon::HaltonSequence(1u, 3u),
            1.0f / 3.0f),
        "Halton(1,3) must be 1/3.");
    Require(
        NearlyEqual(
            LamaPon::HaltonSequence(2u, 3u),
            2.0f / 3.0f),
        "Halton(2,3) must be 2/3.");

    // jitter offsetが1ピクセルの範囲に収まることを確認
    // 反復するjitter列の周期
    constexpr std::uint32_t period = 8u;
    // 周期内の画面ピクセルoffset
    std::vector<DirectX::XMFLOAT2> offsets;
    // index: 周期内のjitter位置
    for (std::uint32_t index = 0u; index < period; ++index)
    {
        // indexに対応するサンプルoffset
        const auto offset =
            LamaPon::TemporalJitterOffset(index);
        Require(
            offset.x >= -0.5f && offset.x <= 0.5f
                && offset.y >= -0.5f
                && offset.y <= 0.5f,
            "Jitter must stay inside one pixel.");
        offsets.push_back(offset);
    }

    // 1周期中に重複offsetがないことを確認
    // left: 比較元offsetの位置
    for (std::size_t left = 0; left < offsets.size(); ++left)
    {
        // right: leftより後ろの比較先
        for (std::size_t right = left + 1;
            right < offsets.size();
            ++right)
        {
            Require(
                !NearlyEqual(
                    offsets[left].x,
                    offsets[right].x,
                    1.0e-4f)
                || !NearlyEqual(
                    offsets[left].y,
                    offsets[right].y,
                    1.0e-4f),
                "Jitter offsets must all differ within one period.");
        }
    }

    // 周期末に列の先頭offsetへ戻ることを確認
    Require(
        NearlyEqual(
            LamaPon::TemporalJitterOffset(0u).x,
            LamaPon::TemporalJitterOffset(period).x)
        && NearlyEqual(
            LamaPon::TemporalJitterOffset(0u).y,
            LamaPon::TemporalJitterOffset(period).y),
        "The jitter sequence must repeat with its period.");

    // 周期内offsetの平均が画素中心に近いことを確認
    // X方向offset合計
    float sumX = 0.0f;
    // Y方向offset合計
    float sumY = 0.0f;
    // offset: 周期内の各画面ピクセル変位
    for (const auto& offset : offsets)
    {
        sumX += offset.x;
        sumY += offset.y;
    }
    // X方向offsetの周期平均
    const float averageX =
        sumX / static_cast<float>(offsets.size());
    // Y方向offsetの周期平均
    const float averageY =
        sumY / static_cast<float>(offsets.size());
    Require(
        std::abs(averageX) < 0.1f
            && std::abs(averageY) < 0.1f,
        "The jitter sequence must be centered on the pixel.");

    // 1ピクセル変位が解像度に応じたclip空間量へ変換されることを確認
    {
        using namespace DirectX;
        // 比較元の透視射影行列
        const XMMATRIX projection =
            XMMatrixPerspectiveFovLH(
                XM_PIDIV4,
                16.0f / 9.0f,
                0.1f,
                100.0f);
        // テスト画面幅
        constexpr std::uint32_t width = 320u;
        // テスト画面高さ
        constexpr std::uint32_t height = 180u;
        // 1ピクセルずらしを適用した射影行列
        const XMMATRIX jittered =
            LamaPon::ApplyTemporalJitter(
                projection,
                XMFLOAT2{ 1.0f, 1.0f },
                width,
                height);
        // 元行列の係数を比較可能な形式で保持
        XMFLOAT4X4 before{};
        // jitter適用後の係数
        XMFLOAT4X4 after{};
        XMStoreFloat4x4(&before, projection);
        XMStoreFloat4x4(&after, jittered);
        Require(
            NearlyEqual(
                after._31 - before._31,
                2.0f / static_cast<float>(width)),
            "One pixel of jitter must move clip x by 2/width.");
        Require(
            NearlyEqual(
                after._32 - before._32,
                -2.0f / static_cast<float>(height)),
            "One pixel of jitter must move clip y by -2/height.");
        // zero jitterが射影行列を変更しないことを確認
        const XMMATRIX unchanged =
            LamaPon::ApplyTemporalJitter(
                projection,
                XMFLOAT2{ 0.0f, 0.0f },
                width,
                height);
        // zero jitter適用後の係数
        XMFLOAT4X4 same{};
        XMStoreFloat4x4(&same, unchanged);
        Require(
            same._31 == before._31
                && same._32 == before._32,
            "Zero jitter must leave the projection untouched.");
    }

    // すべて成功した場合だけ成功メッセージを表示
    if (g_failures == 0)
    {
        std::cout << "Temporal jitter tests passed." << '\n';
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
