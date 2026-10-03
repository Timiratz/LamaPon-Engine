#include "LamaPon/Core/Noise.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

// CPUノイズの決定性・範囲・連続性・格子非依存を検証します。
namespace
{
    // 失敗したnoise assertion数
    int g_failures = 0;

    // 条件不成立を失敗一覧へ追加します。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(const bool condition, const std::string& message)
    {
        // assertion失敗を集計する
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    // valueが0から1の範囲内か返します。
    // InRange01(value: 範囲を確認するnoise値)
    [[nodiscard]] bool InRange01(const float value) noexcept
    {
        return value >= 0.0f && value <= 1.0f;
    }
}

// Noise関数の決定性・範囲・連続性を検証します。
int main()
{
    using namespace LamaPon;

    // 同じ入力が同じ値を返すことを確認
    Require(
        Noise::Value2D(3.25f, -7.5f)
            == Noise::Value2D(3.25f, -7.5f),
        "Value2D must be deterministic.");
    Require(
        Noise::Perlin2D(0.1f, 0.2f)
            == Noise::Perlin2D(0.1f, 0.2f),
        "Perlin2D must be deterministic.");

    // 座標を振ってnoise値が0から1に収まることを確認
    // y: 走査するY座標
    for (float y = -40.0f; y <= 40.0f; y += 3.7f)
    {
        // x: 走査するX座標
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

    // 小さな座標差に対し値の変化も小さいことを確認
    {
        // 隣接サンプル間の最大差
        float maximumStep = 0.0f;
        // 現在位置より前のnoise値
        float previous = Noise::Value2D(10.0f, 10.0f);
        // step: 連続性を測るサンプル番号
        for (int step = 1; step <= 2000; ++step)
        {
            // サンプル位置のX座標
            const float x =
                10.0f + static_cast<float>(step) * 0.001f;
            // 現在位置のnoise値
            const float current = Noise::Value2D(x, 10.0f);
            maximumStep = std::max(
                maximumStep,
                std::abs(current - previous));
            previous = current;
        }
        // 0.001刻みの隣接値差が0.05未満であることを確認
        Require(
            maximumStep < 0.05f,
            "Value2D must be continuous; largest step was "
                + std::to_string(maximumStep));
    }

    // 整数格子と半端格子のnoise平均が近いことを確認
    {
        // 整数座標でのnoise合計
        double integerSum = 0.0;
        // 半端座標でのnoise合計
        double fractionalSum = 0.0;
        // 格子上の比較サンプル数
        int samples = 0;
        // y: 平均比較に使う格子行
        for (int y = 0; y < 40; ++y)
        {
            // x: 平均比較に使う格子列
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
        // 整数格子のnoise平均
        const double integerMean =
            integerSum / samples;
        // 半端格子のnoise平均
        const double fractionalMean =
            fractionalSum / samples;
        Require(
            std::abs(integerMean - fractionalMean) < 0.06,
            "Integer and fractional means differ too much"
            " (grid artefacts): "
                + std::to_string(integerMean) + " vs "
                + std::to_string(fractionalMean));
        // 整数格子平均が中央付近にあることを確認
        Require(
            integerMean > 0.35 && integerMean < 0.65,
            "Value2D mean should sit near 0.5, got "
                + std::to_string(integerMean));
    }

    // fBmのoctaves増加で細部が増えることを確認
    {
        // roughness(octaves: fBmの重ね合わせ回数)
        const auto roughness =
            [](const int octaves)
            {
                // サンプル差の合計
                float total = 0.0f;
                // step: 局所差を測るサンプル番号
                for (int step = 0; step < 400; ++step)
                {
                    // サンプル位置のX座標
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

    // Curlの発散が小さいことを数値微分で確認
    {
        // 格子上の最大発散量
        float maximumDivergence = 0.0f;
        // 中央差分で使う座標幅
        constexpr float e = 0.01f;
        // y: Curlを評価する格子行
        for (float y = 1.0f; y < 5.0f; y += 0.31f)
        {
            // x: Curlを評価する格子列
            for (float x = 1.0f; x < 5.0f; x += 0.31f)
            {
                // X方向の右側ベクトル
                const auto right = Noise::Curl2D(x + e, y);
                // X方向の左側ベクトル
                const auto left = Noise::Curl2D(x - e, y);
                // Y方向の上側ベクトル
                const auto up = Noise::Curl2D(x, y + e);
                // Y方向の下側ベクトル
                const auto down = Noise::Curl2D(x, y - e);
                // ベクトル場の発散近似
                const float divergence =
                    (right.x - left.x) / (2.0f * e)
                    + (up.y - down.y) / (2.0f * e);
                maximumDivergence = std::max(
                    maximumDivergence,
                    std::abs(divergence));
            }
        }
        // 数値微分誤差を許容して発散上限を確認
        Require(
            maximumDivergence < 5.0f,
            "Curl2D should be nearly divergence free, got "
                + std::to_string(maximumDivergence));
    }

    // 失敗があれば集計して異常終了
    if (g_failures != 0)
    {
        std::cerr << g_failures
            << " noise assertion(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Noise tests passed.\n";
    return EXIT_SUCCESS;
}
