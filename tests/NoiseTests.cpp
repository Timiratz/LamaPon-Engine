// ノイズ関数（C++側）の性質を検査します。
//
// GPU側との一致は、GPUデバイスが必要なため
// tests/EngineRenderRegressionTests.cpp の
// 「ノイズのCPU/GPU一致」の段で検査しています。こちらでは
// CPU実装が満たすべき性質（決定性・範囲・連続性・格子非依存）を
// 見ます。

#include "LamaPon/Core/Noise.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    int g_failures = 0;

    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    [[nodiscard]] bool InRange01(const float value) noexcept
    {
        return value >= 0.0f && value <= 1.0f;
    }
}

int main()
{
    using namespace LamaPon;

    // (1) 決定性: 同じ入力なら常に同じ値。
    Require(
        Noise::Value2D(3.25f, -7.5f)
            == Noise::Value2D(3.25f, -7.5f),
        "Value2D must be deterministic.");
    Require(
        Noise::Perlin2D(0.1f, 0.2f)
            == Noise::Perlin2D(0.1f, 0.2f),
        "Perlin2D must be deterministic.");

    // (2) 範囲: すべて0〜1に収まる（負の座標や大きな座標でも）。
    for (float y = -40.0f; y <= 40.0f; y += 3.7f)
    {
        for (float x = -40.0f; x <= 40.0f; x += 3.7f)
        {
            Require(
                InRange01(Noise::Value2D(x, y)),
                "Value2D out of range at "
                    + std::to_string(x) + ","
                    + std::to_string(y));
            Require(
                InRange01(Noise::Perlin2D(x, y)),
                "Perlin2D out of range at "
                    + std::to_string(x) + ","
                    + std::to_string(y));
            Require(
                InRange01(Noise::Worley2D(x, y)),
                "Worley2D out of range at "
                    + std::to_string(x) + ","
                    + std::to_string(y));
            Require(
                InRange01(
                    Noise::FractalValue2D(x, y, 5)),
                "FractalValue2D out of range at "
                    + std::to_string(x) + ","
                    + std::to_string(y));
            Require(
                InRange01(Noise::Value3D(x, y, 1.5f)),
                "Value3D out of range at "
                    + std::to_string(x) + ","
                    + std::to_string(y));
            Require(
                InRange01(Noise::Value1D(x)),
                "Value1D out of range at "
                    + std::to_string(x));
        }
    }

    // (3) 連続性: これが乱数との決定的な違い。座標を少し動かしたとき、
    //    値も少しだけ変わること（乱数なら大きく飛ぶ）。
    {
        float maximumStep = 0.0f;
        float previous = Noise::Value2D(10.0f, 10.0f);
        for (int step = 1; step <= 2000; ++step)
        {
            const float x =
                10.0f + static_cast<float>(step) * 0.001f;
            const float current = Noise::Value2D(x, 10.0f);
            maximumStep = std::max(
                maximumStep,
                std::abs(current - previous));
            previous = current;
        }
        // 0.001刻みで動かして0.05を超える段差が無いこと。
        // 乱数ならここは1.0近くになります。
        Require(
            maximumStep < 0.05f,
            "Value2D must be continuous; largest step was "
                + std::to_string(maximumStep));
    }

    // (4) 格子の縞が出ないこと: 整数座標ちょうどの値だけが偏ると、
    //    タイル状の模様として見えます。整数座標と半端な座標で
    //    平均が近いことを確認します。
    {
        double integerSum = 0.0;
        double fractionalSum = 0.0;
        int samples = 0;
        for (int y = 0; y < 40; ++y)
        {
            for (int x = 0; x < 40; ++x)
            {
                integerSum += Noise::Value2D(
                    static_cast<float>(x),
                    static_cast<float>(y));
                fractionalSum += Noise::Value2D(
                    static_cast<float>(x) + 0.5f,
                    static_cast<float>(y) + 0.5f);
                ++samples;
            }
        }
        const double integerMean =
            integerSum / samples;
        const double fractionalMean =
            fractionalSum / samples;
        Require(
            std::abs(integerMean - fractionalMean) < 0.06,
            "Integer and fractional means differ too much"
            " (grid artefacts): "
                + std::to_string(integerMean) + " vs "
                + std::to_string(fractionalMean));
        // だいたい中央に分布していること。
        Require(
            integerMean > 0.35 && integerMean < 0.65,
            "Value2D mean should sit near 0.5, got "
                + std::to_string(integerMean));
    }

    // (5) fBmのオクターブが効いていること（回数を増やすと細部が
    //    増え、隣接値の差が大きくなる）。
    {
        const auto roughness =
            [](const int octaves)
            {
                float total = 0.0f;
                for (int step = 0; step < 400; ++step)
                {
                    const float x =
                        static_cast<float>(step) * 0.05f;
                    total += std::abs(
                        Noise::FractalValue2D(
                            x + 0.05f, 3.0f, octaves)
                        - Noise::FractalValue2D(
                            x, 3.0f, octaves));
                }
                return total;
            };
        Require(
            roughness(6) > roughness(1),
            "More fBm octaves must add detail.");
    }

    // (6) Curlは発散が小さいこと（湧き出し・吸い込みが無い流れ）。
    {
        float maximumDivergence = 0.0f;
        constexpr float e = 0.01f;
        for (float y = 1.0f; y < 5.0f; y += 0.31f)
        {
            for (float x = 1.0f; x < 5.0f; x += 0.31f)
            {
                const auto right = Noise::Curl2D(x + e, y);
                const auto left = Noise::Curl2D(x - e, y);
                const auto up = Noise::Curl2D(x, y + e);
                const auto down = Noise::Curl2D(x, y - e);
                const float divergence =
                    (right.x - left.x) / (2.0f * e)
                    + (up.y - down.y) / (2.0f * e);
                maximumDivergence = std::max(
                    maximumDivergence,
                    std::abs(divergence));
            }
        }
        // 数値微分の誤差があるので緩めに見ます。
        Require(
            maximumDivergence < 5.0f,
            "Curl2D should be nearly divergence free, got "
                + std::to_string(maximumDivergence));
    }

    if (g_failures != 0)
    {
        std::cerr << g_failures
            << " noise assertion(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Noise tests passed.\n";
    return EXIT_SUCCESS;
}
