// SSAOとScreenEffectで共有し、右手系射影の_33・_43は負値のまま渡す。

// depthParameters.zが0なら呼出側で深度を読まず、depthUnprojection.xyには射影_11・_22の逆数を渡す。
#ifndef LAMAPON_SCREEN_DEPTH_INCLUDED
#define LAMAPON_SCREEN_DEPTH_INCLUDED


// 深度から正の奥行きを求め、遠平面では1e6を返す(deviceDepth: 0～1のデバイス深度, depthParameters: X射影_33・Y射影_43)。
float LamaPonSceneDistance(
    float deviceDepth,
    float4 depthParameters)
{
    // 深度と射影_33の和
    const float denominator = deviceDepth + depthParameters.x;
    if (denominator > -1e-6f)
    {
        return 1e6f;
    }
    return depthParameters.y / denominator;
}


// 右X・上Y・奥Zの正の奥行き座標を復元する(uv: 左上原点の画像UV, deviceDepth: デバイス深度, depthParameters: X射影_33・Y射影_43, depthUnprojection: XY射影対角の逆数)。
float3 LamaPonViewPositionFromDepth(
    float2 uv,
    float deviceDepth,
    float4 depthParameters,
    float4 depthUnprojection)
{
    // 正のビュー奥行き
    const float viewZ =
        LamaPonSceneDistance(deviceDepth, depthParameters);
    // 右X・上Yの正規化画面座標
    const float2 ndc = float2(
        uv.x * 2.0f - 1.0f,
        1.0f - uv.y * 2.0f);
    return float3(
        ndc.x * depthUnprojection.x * viewZ,
        ndc.y * depthUnprojection.y * viewZ,
        viewZ);
}


// 段差の小さい隣接点で法線を求め、退化時は-Zを返す(origin: 中心位置, left: 左隣位置, right: 右隣位置, up: 上隣位置, down: 下隣位置)。
float3 LamaPonNormalFromNeighbours(
    float3 origin,
    float3 left,
    float3 right,
    float3 up,
    float3 down)
{

    // 段差の小さい側の右向き差分
    const float3 horizontal =
        abs(left.z - origin.z) < abs(right.z - origin.z)
            ? (origin - left)
            : (right - origin);
    // 段差の小さい側の上向き差分
    const float3 vertical =
        abs(up.z - origin.z) < abs(down.z - origin.z)
            ? (up - origin)
            : (origin - down);

    // 近傍から求めた非正規化法線
    const float3 normal = cross(vertical, horizontal);
    // 法線の長さの二乗
    const float lengthSquared = dot(normal, normal);
    if (lengthSquared < 1e-12f)
    {

        return float3(0.0f, 0.0f, -1.0f);
    }
    return normal * rsqrt(lengthSquared);
}


// 深度と4近傍からビュー空間の法線を復元する(depthTexture: 利用可能な深度画像, pixel: 画面画素位置, inverseScreenSize: 画像幅・高さの逆数, depthParameters: X射影_33・Y射影_43, depthUnprojection: XY射影対角の逆数)。
float3 LamaPonReconstructViewNormal(
    Texture2D depthTexture,
    int2 pixel,
    float2 inverseScreenSize,
    float4 depthParameters,
    float4 depthUnprojection)
{
    // 中心画素の画像UV
    const float2 uv =
        (float2(pixel) + 0.5f) * inverseScreenSize;
    // 左右1画素のUV差
    const float2 offsetX = float2(inverseScreenSize.x, 0.0f);
    // 上下1画素のUV差
    const float2 offsetY = float2(0.0f, inverseScreenSize.y);

    // 深度から復元した中心位置
    const float3 origin = LamaPonViewPositionFromDepth(
        uv,
        depthTexture.Load(int3(pixel, 0)).r,
        depthParameters,
        depthUnprojection);
    // 深度から復元した左隣位置
    const float3 left = LamaPonViewPositionFromDepth(
        uv - offsetX,
        depthTexture.Load(
            int3(pixel + int2(-1, 0), 0)).r,
        depthParameters,
        depthUnprojection);
    // 深度から復元した右隣位置
    const float3 right = LamaPonViewPositionFromDepth(
        uv + offsetX,
        depthTexture.Load(
            int3(pixel + int2(1, 0), 0)).r,
        depthParameters,
        depthUnprojection);

    // 深度から復元した上隣位置
    const float3 up = LamaPonViewPositionFromDepth(
        uv - offsetY,
        depthTexture.Load(
            int3(pixel + int2(0, -1), 0)).r,
        depthParameters,
        depthUnprojection);
    // 深度から復元した下隣位置
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
