// 深度バッファから「距離」「ビュー空間の位置」「法線」を求める共有実装。
//
// エンジン内部（SSAO）と自作のScreenEffectの両方がここを使います。
// 同じ式を2箇所へ書くと、片方だけ変更したときに描画結果が食い違う
// ため、式はここで一元管理します。
//
// 使う側が用意する2つのfloat4:
//
//   depthParameters   x = 射影の_33
//                     y = 射影の_43
//                     z = 深度が使えるなら1（0なら深度を読まないこと）
//   depthUnprojection x = 1 / 射影の_11
//                     y = 1 / 射影の_22
//
// 射影は右手系なので_33も_43も負です（例: near 0.1 / far 1000 で
// _33 = -1.0001、_43 = -0.10001）。左手系のつもりで符号を変えると、
// 下の遠平面判定が全画素で成立し、画面全体が同じ距離になります。

#ifndef LAMAPON_SCREEN_DEPTH_INCLUDED
#define LAMAPON_SCREEN_DEPTH_INCLUDED

// 深度（0〜1）からカメラまでの距離（メートル）。
// 何も描かれていない遠平面では分母が0へ近づくので、十分遠い値を
// 返します。
float LamaPonSceneDistance(
    float deviceDepth,
    float4 depthParameters)
{
    const float denominator = deviceDepth + depthParameters.x;
    if (denominator > -1e-6f)
    {
        return 1e6f;
    }
    return depthParameters.y / denominator;
}

// UVと深度からビュー空間の位置。zは上の距離そのものです
// （x=右、y=上、z=奥。距離を正で持つので、この3本はSSAOと同じ
// 左手系の並びになります）。
float3 LamaPonViewPositionFromDepth(
    float2 uv,
    float deviceDepth,
    float4 depthParameters,
    float4 depthUnprojection)
{
    const float viewZ =
        LamaPonSceneDistance(deviceDepth, depthParameters);
    const float2 ndc = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f);
    return float3(
        ndc.x * depthUnprojection.x * viewZ,
        ndc.y * depthUnprojection.y * viewZ,
        viewZ);
}

// 上下左右のビュー空間位置から法線を組み立てます。
//
// 単純にcross(ddx, ddy)で面法線を取ると、輪郭のところで手前と奥を
// またいだ差分になって法線が寝てしまいます。左右・上下それぞれで
// 「奥行きの段差が小さい方」を選ぶと、輪郭では必ず同じ面の側が
// 選ばれます。隣が空（距離1e6）の場合も段差が巨大になるので、
// 自動的に反対側が選ばれます。
float3 LamaPonNormalFromNeighbours(
    float3 origin,
    float3 left,
    float3 right,
    float3 up,
    float3 down)
{
    // どちら向きに引いても+x／+y方向のベクトルになるよう符号を
    // 揃えます。
    const float3 horizontal =
        abs(left.z - origin.z) < abs(right.z - origin.z)
            ? (origin - left)
            : (right - origin);
    const float3 vertical =
        abs(up.z - origin.z) < abs(down.z - origin.z)
            ? (up - origin)
            : (origin - down);

    const float3 normal = cross(vertical, horizontal);
    const float lengthSquared = dot(normal, normal);
    if (lengthSquared < 1e-12f)
    {
        // 退化した場合（1px幅の物体など）は真正面を向かせます。
        return float3(0.0f, 0.0f, -1.0f);
    }
    return normal * rsqrt(lengthSquared);
}

// ScreenEffect用のまとめ。深度テクスチャと画素座標を渡すだけで
// ビュー空間の法線が返ります。カメラを向いている面は-zです。
//
// inverseScreenSizeは1画素ぶんのUV（ScreenParameters.ScreenSize.zw）。
float3 LamaPonReconstructViewNormal(
    Texture2D depthTexture,
    int2 pixel,
    float2 inverseScreenSize,
    float4 depthParameters,
    float4 depthUnprojection)
{
    const float2 uv =
        (float2(pixel) + 0.5f) * inverseScreenSize;
    const float2 offsetX = float2(inverseScreenSize.x, 0.0f);
    const float2 offsetY = float2(0.0f, inverseScreenSize.y);

    const float3 origin = LamaPonViewPositionFromDepth(
        uv,
        depthTexture.Load(int3(pixel, 0)).r,
        depthParameters,
        depthUnprojection);
    const float3 left = LamaPonViewPositionFromDepth(
        uv - offsetX,
        depthTexture.Load(
            int3(pixel + int2(-1, 0), 0)).r,
        depthParameters,
        depthUnprojection);
    const float3 right = LamaPonViewPositionFromDepth(
        uv + offsetX,
        depthTexture.Load(
            int3(pixel + int2(1, 0), 0)).r,
        depthParameters,
        depthUnprojection);
    // 画面のyは下向きなので、引いた側が画面の上（ビュー空間の+y）。
    const float3 up = LamaPonViewPositionFromDepth(
        uv - offsetY,
        depthTexture.Load(
            int3(pixel + int2(0, -1), 0)).r,
        depthParameters,
        depthUnprojection);
    const float3 down = LamaPonViewPositionFromDepth(
        uv + offsetY,
        depthTexture.Load(
            int3(pixel + int2(0, 1), 0)).r,
        depthParameters,
        depthUnprojection);

    return LamaPonNormalFromNeighbours(
        origin, left, right, up, down);
}

#endif
