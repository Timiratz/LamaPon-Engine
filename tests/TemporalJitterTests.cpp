// TAAのサブピクセルずらし（Halton列）を検査します。
//
// ずらし方が偏ると、そのフレームだけアンチエイリアスが効かずに
// ちらついて見えます。ここでは「1周のあいだにピクセル内へ均等に
// 散ること」と「射影行列への織り込みが1ピクセル分ちょうどになる
// こと」を数値で確かめます。GPUは要りません。

#include "LamaPon/Graphics/TemporalJitter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void Require(
        const bool condition,
        const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right,
        const float tolerance = 1.0e-5f) noexcept
    {
        return std::abs(left - right) <= tolerance;
    }
}

int main()
{
    // ① Halton列の既知の値。基数2は 1/2, 1/4, 3/4, 1/8 ... と
    // 続きます（桁を逆順に読んだ小数）。ここがずれていると列全体が
    // 別物になります。
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

    // ② ずらし量は必ず -0.5〜+0.5 の範囲へ収まること。範囲を
    // 超えると隣のピクセルを描いてしまい、輪郭が太ります。
    constexpr std::uint32_t period = 8u;
    std::vector<DirectX::XMFLOAT2> offsets;
    for (std::uint32_t index = 0u; index < period; ++index)
    {
        const auto offset =
            LamaPon::TemporalJitterOffset(index);
        Require(
            offset.x >= -0.5f && offset.x <= 0.5f
                && offset.y >= -0.5f
                && offset.y <= 0.5f,
            "Jitter must stay inside one pixel.");
        offsets.push_back(offset);
    }

    // ③ 1周のあいだに同じ点が出ないこと。同じ点が続くと、その
    // フレームは前と同じ絵になるのでアンチエイリアスが進みません。
    for (std::size_t left = 0; left < offsets.size(); ++left)
    {
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

    // ④ 1周すると同じ列へ戻ること（周期8）。
    Require(
        NearlyEqual(
            LamaPon::TemporalJitterOffset(0u).x,
            LamaPon::TemporalJitterOffset(period).x)
        && NearlyEqual(
            LamaPon::TemporalJitterOffset(0u).y,
            LamaPon::TemporalJitterOffset(period).y),
        "The jitter sequence must repeat with its period.");

    // ⑤ 平均が中央（0）に近いこと。偏っていると絵全体が
    // 半ピクセルずれて見えます。
    float sumX = 0.0f;
    float sumY = 0.0f;
    for (const auto& offset : offsets)
    {
        sumX += offset.x;
        sumY += offset.y;
    }
    const float averageX =
        sumX / static_cast<float>(offsets.size());
    const float averageY =
        sumY / static_cast<float>(offsets.size());
    Require(
        std::abs(averageX) < 0.1f
            && std::abs(averageY) < 0.1f,
        "The jitter sequence must be centred on the pixel.");

    // ⑥ 射影への織り込み。1ピクセルずらすと、クリップ空間では
    // 2/幅（yは符号が逆）だけ動くはずです。ここを間違えると
    // ずらし量が画面の解像度に応じて狂います。
    {
        using namespace DirectX;
        const XMMATRIX projection =
            XMMatrixPerspectiveFovLH(
                XM_PIDIV4,
                16.0f / 9.0f,
                0.1f,
                100.0f);
        constexpr std::uint32_t width = 320u;
        constexpr std::uint32_t height = 180u;
        const XMMATRIX jittered =
            LamaPon::ApplyTemporalJitter(
                projection,
                XMFLOAT2{ 1.0f, 1.0f },
                width,
                height);
        XMFLOAT4X4 before{};
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
        // ずらし0なら行列は変わらないこと（無効時にバイト単位で
        // 同じ絵になることの土台です）。
        const XMMATRIX unchanged =
            LamaPon::ApplyTemporalJitter(
                projection,
                XMFLOAT2{ 0.0f, 0.0f },
                width,
                height);
        XMFLOAT4X4 same{};
        XMStoreFloat4x4(&same, unchanged);
        Require(
            same._31 == before._31
                && same._32 == before._32,
            "Zero jitter must leave the projection untouched.");
    }

    if (g_failures == 0)
    {
        std::cout << "Temporal jitter tests passed." << '\n';
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
