#pragma once

#include <DirectXMath.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>

namespace LamaPon
{
    struct SkySettings final
    {
        bool enabled{};
        DirectX::XMFLOAT3 topColor{ 0.06f, 0.18f, 0.42f };
        DirectX::XMFLOAT3 horizonColor{ 0.48f, 0.68f, 0.9f };
        DirectX::XMFLOAT3 groundColor{ 0.04f, 0.05f, 0.08f };
        float intensity{ 1.0f };
        // キューブマップ（.dds）。空ならグラデーション空を使います。
        std::filesystem::path cubemapPath{};
        // キューブマップ環境光（IBL）の強度。0で無効。
        float iblIntensity{ 1.0f };
        // Directional Lightの向きで朝昼夜を切り替えます。入れると
        // 上の3色と環境光は太陽の高度から自動で決まり、空には
        // 太陽そのものも描かれます。ライトを回すだけで夜明けから
        // 日没まで動きます。末尾に足しています。
        bool sunDriven{};
    };

    // 太陽の高度から決まる空と環境光。sunDrivenのときに使います。
    struct SunDrivenSky final
    {
        DirectX::XMFLOAT3 topColor{};
        DirectX::XMFLOAT3 horizonColor{};
        DirectX::XMFLOAT3 groundColor{};
        DirectX::XMFLOAT3 ambientColor{};
        float ambientIntensity{};
        // 0=夜, 1=昼。霧の色などを合わせたいときに使えます。
        float dayAmount{};
    };

    // towardSunは「太陽へ向かう向き」（Directional Lightの進行方向の
    // 逆）です。yがそのまま高度のsinになります。
    //
    // 夜が真っ暗にならないのは意図的です。完全な黒にすると何も
    // 見えなくなってゲームにならないので、月明かり程度の青を
    // 残しています。
    [[nodiscard]] inline SunDrivenSky EvaluateSunDrivenSky(
        const DirectX::XMFLOAT3& towardSun)
    {
        const auto saturateValue = [](const float value)
        {
            return value < 0.0f
                ? 0.0f
                : (value > 1.0f ? 1.0f : value);
        };
        // 0で始まり1で終わる滑らかな補間（HLSLのsmoothstep）。
        const auto smooth = [&saturateValue](
            const float edge0,
            const float edge1,
            const float value)
        {
            const float t = saturateValue(
                (value - edge0) / (edge1 - edge0));
            return t * t * (3.0f - 2.0f * t);
        };
        const auto mix = [](
            const DirectX::XMFLOAT3& a,
            const DirectX::XMFLOAT3& b,
            const float t)
        {
            return DirectX::XMFLOAT3{
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t
            };
        };

        const float elevation = towardSun.y;
        // 昼の度合い。地平線ちょうど(0)ではまだ薄暗く、少し上がって
        // から一気に明るくなります。
        const float dayAmount = smooth(-0.10f, 0.30f, elevation);
        // 朝焼け・夕焼けの度合い。地平線付近だけで立ち上がります。
        const float duskAmount =
            smooth(-0.22f, -0.02f, elevation)
            * (1.0f - smooth(0.02f, 0.30f, elevation));

        const DirectX::XMFLOAT3 nightTop{ 0.010f, 0.020f, 0.060f };
        const DirectX::XMFLOAT3 nightHorizon{
            0.035f, 0.055f, 0.120f };
        const DirectX::XMFLOAT3 dayTop{ 0.130f, 0.330f, 0.720f };
        const DirectX::XMFLOAT3 dayHorizon{
            0.600f, 0.760f, 0.940f };
        const DirectX::XMFLOAT3 duskTop{ 0.170f, 0.160f, 0.360f };
        const DirectX::XMFLOAT3 duskHorizon{
            0.960f, 0.420f, 0.150f };

        SunDrivenSky result{};
        result.dayAmount = dayAmount;
        result.topColor = mix(
            mix(nightTop, dayTop, dayAmount),
            duskTop,
            duskAmount);
        result.horizonColor = mix(
            mix(nightHorizon, dayHorizon, dayAmount),
            duskHorizon,
            duskAmount);
        // 地面側は空の映り込みなので、地平の色を暗く落とした値。
        result.groundColor = {
            result.horizonColor.x * 0.18f + 0.010f,
            result.horizonColor.y * 0.18f + 0.011f,
            result.horizonColor.z * 0.18f + 0.014f
        };
        // 環境光は空全体の平均に近い色。夕方は暖色へ寄ります。
        result.ambientColor = mix(
            result.topColor,
            result.horizonColor,
            0.55f);
        const float maximumChannel = std::max(
            result.ambientColor.x,
            std::max(
                result.ambientColor.y,
                result.ambientColor.z));
        if (maximumChannel > 0.001f)
        {
            // 色は向きだけを使い、明るさはintensityへ寄せます
            // （SetAmbientLightColorが0〜1へ丸めるため）。
            result.ambientColor = {
                result.ambientColor.x / maximumChannel,
                result.ambientColor.y / maximumChannel,
                result.ambientColor.z / maximumChannel
            };
        }
        result.ambientIntensity =
            0.045f + 0.305f * dayAmount;
        return result;
    }

    struct FogSettings final
    {
        bool enabled{};
        DirectX::XMFLOAT3 color{ 0.48f, 0.62f, 0.76f };
        float startDistance{ 8.0f };
        float endDistance{ 35.0f };
        float density{ 0.015f };
    };

    struct BloomSettings final
    {
        bool enabled{};
        float threshold{ 0.72f };
        float intensity{ 0.45f };
        float radius{ 2.0f };
    };

    // シーンの深度から形状の境界を検出して重ねるアウトライン。
    // 法線バッファは持たず、深度からビュー空間法線を再構成するため、
    // 追加のMRTを書かずに使えます。法線マップの細かな凹凸ではなく、
    // シルエットと形状の折れ目を対象にします。
    struct ScreenOutlineSettings final
    {
        bool enabled{};
        DirectX::XMFLOAT3 color{ 0.02f, 0.02f, 0.02f };
        float intensity{ 1.0f };
        // 近傍を何画素離して調べるか。1〜4画素。
        float thickness{ 1.0f };
        // 中心と近傍の相対的な距離差。小さいほど細かな段差も拾います。
        float depthThreshold{ 0.025f };
        // 中心と近傍の法線の差。小さいほど浅い折れ目も拾います。
        float normalThreshold{ 0.25f };
    };

    // 画面上の高輝度部分からゴースト、ハロー、放射状の筋を作る
    // スクリーンスペース・レンズフレア。Bloomとは別の光学表現です。
    struct ScreenSpaceLensFlareSettings final
    {
        bool enabled{};
        float threshold{ 1.6f };
        float intensity{ 0.28f };
        // 画面中心を基準にしたゴーストの離れ具合。
        float ghostDispersal{ 0.35f };
        float haloWidth{ 0.35f };
        float chromaticAberration{ 0.06f };
        float streakIntensity{ 0.18f };
        float streakLength{ 0.22f };
        // 筋を何方向へ伸ばすか（1〜4）。1で1本の横棒（アナモルフィック
        // 風）、2で十字、3以上で放射状になります。方向は半円を等分
        // します（筋は両側へ伸びるため、半円ぶんで一周を覆えます）。
        std::uint32_t streakDirections{ 1 };
        // 1本目の角度（度）。0で水平、45で斜めです。
        float streakAngleDegrees{ 0.0f };
    };

    // 被写界深度（DoF）。ピントの合った距離から離れたものをぼかして、
    // カメラで撮った絵のような奥行きを出します。
    //
    // 深度から画素ごとの「ぼけの大きさ」（CoC＝錯乱円）を求め、
    // 半解像度で円形にぼかしてから、CoCの大きさで元の絵と混ぜます。
    // 深度プリパスは要りません（メインパスが書いた深度をそのまま
    // 読みます）。SSAOやSSRを切っていても使えます。
    struct DepthOfFieldSettings final
    {
        bool enabled{};
        // ピントの合う距離（ワールド単位＝メートル）。
        float focusDistance{ 10.0f };
        // ピントが完全に合っている前後の幅（メートル）。この帯の中は
        // 1画素もぼけません。0にすると焦点面ちょうどだけが鋭くなり、
        // 手前も奥もすぐにぼけ始めます。
        float focusRange{ 2.0f };
        // ぼけの強さ。焦点面からの相対的なずれに掛ける倍率で、上げる
        // ほど絞りを開けたレンズのように大きくぼけます。
        float blurStrength{ 1.0f };
        // ぼけ半径の上限（画素）。見た目の上限と同時に処理コストの
        // 上限でもあります（サンプル点はこの円の中に散らすため、
        // 広げてもサンプル数は増えず、粒が粗くなります）。
        float maximumRadius{ 10.0f };
    };

    // モーションブラー（カメラの動きによるブレ）。
    //
    // 深度からワールド位置を復元し、前フレームの行列で射影し直して
    // 「この画素が前フレームどこに写っていたか」を求めます。その差が
    // 画面上の移動量なので、その線に沿ってサンプルして平均します。
    //
    // カメラの動きだけが対象です。物体ごとの速度は持っていないので
    // （速度バッファを足すと自作Shaderが書けなくなるため、TAAと同じ
    // 理由で避けています）、止まっているカメラの前を走るキャラクターは
    // ブレません。カメラを振る・走る・乗り物に乗る演出に効きます。
    struct MotionBlurSettings final
    {
        bool enabled{};
        // ブレの強さ。1.0で「前フレームから今フレームまでに動いた
        // ぶん」をそのまま伸ばします（シャッターが開いている割合に
        // 相当します）。
        float intensity{ 0.5f };
        // 1画素が伸びる最大の長さ（画素）。上限を切らないと、カメラを
        // 素早く振ったときに画面全体が溶けます。
        float maximumRadius{ 16.0f };
    };

    // 自動露出（明順応・暗順応）。
    //
    // 画面の平均的な明るさを測り、暗いシーンでは露出を開け、明るい
    // シーンでは絞ります。暗所と明所の移動時に、視覚の明順応・暗順応に
    // 相当する変化を表現します。
    //
    // 測るのはトーンマップ前のHDRで、結果はトーンマップの露出（段数）
    // への補正として効きます。手動の露出はこの上に乗る「補正値」に
    // なるので、両方同時に使えます。
    struct AutoExposureSettings final
    {
        bool enabled{};
        // 目標の明るさ（中間グレー）。上げると全体が明るく写ります。
        // 写真の18%グレーが由来です。
        float keyValue{ 0.18f };
        // 測った明るさをこの範囲へ丸めます。真っ暗なシーンで露出を
        // 開けきると暗部のノイズだけが持ち上がり、真っ白なシーンで
        // 絞りきると何も見えなくなるため、上下を切ります。
        float minimumLuminance{ 0.02f };
        float maximumLuminance{ 8.0f };
        // 明るい場所へ出たときに慣れる速さ（1秒あたり）。
        float speedToBright{ 3.0f };
        // 暗い場所へ入ったときに慣れる速さ（1秒あたり）。本物の目も
        // 暗順応のほうが遅いので、既定を分けています。
        float speedToDark{ 1.0f };
    };

    // SSAO（遮蔽による陰り）。物が接している隙間や角を暗くして、
    // 接地感を出します。深度バッファだけを使うため、法線バッファは
    // 必要ありません。品質設定でも一括でオフにできます。
    struct AmbientOcclusionSettings final
    {
        bool enabled{};
        // 遮蔽を探す距離（ワールド単位）。大きいほど広い範囲の
        // 陰りが出ますが、ぼんやりします。
        float radius{ 0.5f };
        // 陰りの濃さ（0＝無効、1＝最大）。
        float strength{ 0.6f };
    };

    // TAA（時間的アンチエイリアス）。
    //
    // 毎フレーム射影をサブピクセルだけずらして描き、前フレームの
    // 結果と混ぜます。1枚のなかで平均を取るMSAAと違い、時間をかけて
    // 平均するのでコストが安く、輪郭だけでなくテクスチャの
    // ちらつきにも効きます。
    //
    // 前フレームの絵は「カメラの動きから逆算した位置」で読みます
    // （再投影）。物体ごとの速度は持っていないので、速く動くものは
    // 近傍クランプで履歴を捨てることでにじみを抑えます。
    struct TemporalAntiAliasingSettings final
    {
        bool enabled{};
        // 前フレームをどれだけ残すか（0で無効、0.95で強め）。
        // 上げるほど滑らかになりますが、動きに対して眠くなります。
        float historyWeight{ 0.9f };
        // ずらす量（ピクセル単位）。1.0で1ピクセル幅に散らします。
        float jitterScale{ 1.0f };
        // 近傍クランプの緩さ。小さいほど履歴を捨てやすく、
        // にじみが減る代わりにアンチエイリアスも弱くなります。
        float clampTolerance{ 1.0f };
    };

    // SSR（画面空間反射）。濡れた床、磨いた金属、水面に周りの景色を
    // 映します。
    //
    // 画面に写っているものしか映せません（画面の外や物の裏側は
    // 情報が無いため）。足りない分はリフレクションプローブ／Skyの
    // 環境反射へ滑らかに戻します。
    //
    // 計算はLitシェーダーの中で行います。シェーダーが持っている
    // 正確な法線（ノーマルマップ適用済み）をそのまま使えるので、
    // 法線バッファを別に持つ必要がありません。深度は深度プリパスの
    // ものを使い、色は前フレームのHDRを再投影して読みます。
    struct ScreenSpaceReflectionSettings final
    {
        bool enabled{};
        // 反射の強さ（0で無効、1で等倍）。
        float intensity{ 1.0f };
        // レイを進める最大距離（ワールド単位）。
        float maximumDistance{ 12.0f };
        // レイの反復の上限。Hi-Zトラバーサルは何も無い空間を大股で
        // 飛ぶので、48で画面の端から端まで届きます（検証シーンでは
        // 48以上に増やしても結果が1変わりません）。床すれすれを長く
        // 這う映り込みが途中で切れるときだけ増やしてください。
        std::uint32_t stepCount{ 48 };
        // 物の厚みの見積もり（ワールド単位）。レイが物の側面から
        // 入った歩は、画面空間では「その物の裏かもしれない」と
        // 区別が付きません。どこまでを裏と見なすかがこの値です。
        // 小さすぎると反射が途中で水平に切れ、大きすぎると物の裏の
        // 床にも映り込みます。
        //
        // 既定値1.2はメートル単位の人間サイズの物体を想定しています。
        // 手すりや板のような薄い物体が中心の場面では下げてください。
        float thickness{ 1.2f };
        // この粗さを超えた面には掛けません。ざらざらした面の反射は
        // ぼやけていて、1本のレイでは表現できないためです。
        float roughnessCutoff{ 0.45f };
    };

    // ボリュメトリックライト（光の筋、god ray）。
    //
    // カメラからのレイに沿ってシャドウマップを引き、「光が届いて
    // いる区間」を積んで空気中の散乱を描きます。窓から差す光、霧の
    // 中のスポットライト、木漏れ日がこれです。
    //
    // 既存のカスケードシャドウをそのまま使うので、影を有効にした
    // 平行光源が必要です（影が無いと遮るものが分からないため）。
    struct VolumetricLightSettings final
    {
        bool enabled{};
        // 光の筋の濃さ。上げるほど空気が濃く見えます。
        float intensity{ 0.35f };
        // レイに沿って何回サンプルするか。多いほど滑らかですが
        // 重くなります。少ないと縞（バンディング）が出ます。
        std::uint32_t sampleCount{ 32 };
        // 光の筋を届かせる最大距離（ワールド単位）。既定は
        // 平行光源の影の距離の既定値（24m）に合わせています。
        // これより遠くは影の情報が無いので、実際には影の距離まで
        // しか進みません（Scene側で抑えます）。
        float maximumDistance{ 24.0f };
        // 前方散乱の強さ（0〜0.95）。太陽の方を向いたときだけ
        // 明るくなる度合いで、上げるほど指向性が強くなります。
        float scattering{ 0.6f };
    };

    // ベイクした間接光（照度ボリューム）。
    //
    // シーンへ3D格子のライトプローブを敷き、格子の各点で周囲の光を
    // 焼いて「場所ごとに違う環境光」にします。赤い壁のそばの床が
    // ほんのり赤くなるのは、直接光が1回反射したバウンス光による
    // ものです。UV展開が要らず、動くオブジェクトにも効く一方、
    // そのかわり間接光の影はぼやけます（精細さは格子の密度次第）。
    //
    // ベイクは手動です（Inspectorのボタン。シーンを変えたら押し
    // 直してください）。結果はシーンファイルへ一緒に保存されます。
    struct BakedGlobalIlluminationSettings final
    {
        bool enabled{};
        // ボリュームの中心と大きさ（ワールド単位）。この箱の外では
        // 従来のフラットな環境光へ滑らかに戻ります。
        DirectX::XMFLOAT3 center{};
        DirectX::XMFLOAT3 size{ 20.0f, 10.0f, 20.0f };
        // 各軸のプローブの個数。多いほど間接光の変化が細かく
        // なりますが、ベイク時間とデータが増えます（合計の上限は
        // 32768点）。
        std::uint32_t resolutionX{ 8 };
        std::uint32_t resolutionY{ 4 };
        std::uint32_t resolutionZ{ 8 };
        // 間接光の強さ（1で焼いたまま）。
        float intensity{ 1.0f };
    };

    struct ColorGradingSettings final
    {
        bool toneMappingEnabled{ true };
        bool enabled{ true };
        float exposure{ 0.15f };
        float contrast{ 1.05f };
        float saturation{ 1.08f };
        float temperature{ 0.02f };
        float tint{};
        float vignette{ 0.12f };
        // 自動露出が決めた露出の補正（段数）。設定として保存する値では
        // なく、ポスト処理が毎フレーム埋める作業用の欄です。上の
        // exposureへ加算されて効きます。カラー調整のオン/オフには
        // 左右されません（露出はカメラの挙動なので、色の作り込みを
        // 切ったときに露出まで戻ると驚くためです）。
        float autoExposureStops{};
    };
}
