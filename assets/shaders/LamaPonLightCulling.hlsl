// ビュー視錐台を奥行き方向に指数分割し、各クラスタに届くライト番号を上限まで登録する。

// GpuLightの並びはC++のClusteredLights.hと一致させる。
struct GpuLight
{

    // XYZ位置・W到達半径
    float4 PositionRange;

    // RGBライト色・W強度
    float4 ColorIntensity;

    // XYZ方向・W内角cos
    float4 DirectionInnerCosine;
    // Y種別は0point・1spot、Z影参照はspot枠+1・point番号+1・0影なし、Wは予約。
    // X外角cos・Y種別・Z影参照
    float4 ExtraParameters;
};

// 光源分類用のクラスタ定数
cbuffer ClusterCullingBuffer : register(b0)
{

    // 行優先のWorld→ビュー行列
    row_major float4x4 View;

    // XYZ分割数・Wライト総数
    float4 GridParameters;

    // 近遠距離・対数比・灯数上限
    float4 DepthParameters;

    // XYの半画角tan
    float4 FrustumParameters;
};

// 全ライトのGPU情報配列
StructuredBuffer<GpuLight> Lights : register(t0);

// クラスタ番号順のライト参照列
RWStructuredBuffer<uint> LightIndexList : register(u0);

// クラスタごとの登録灯数
RWStructuredBuffer<uint> ClusterLightCounts : register(u1);


// 球とAABBが交差するか判定する(center: 球の中心, radius: 球の半径, aabbMinimum: AABB最小座標, aabbMaximum: AABB最大座標)。
bool SphereIntersectsAabb(
    float3 center,
    float radius,
    float3 aabbMinimum,
    float3 aabbMaximum)
{
    // 球中心に最も近いAABB位置
    const float3 closest = clamp(
        center,
        aabbMinimum,
        aabbMaximum);
    // AABB最近位置から球中心への差
    const float3 delta = center - closest;
    return dot(delta, delta) <= radius * radius;
}


// 1Threadが1クラスタの番号表を独占して作る(dispatchId: クラスタを指定するThread ID)。
[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    // クラスタの横分割数
    const uint gridX = (uint)GridParameters.x;
    // クラスタの縦分割数
    const uint gridY = (uint)GridParameters.y;
    // クラスタの奥行き分割数
    const uint gridZ = (uint)GridParameters.z;
    // 全クラスタ数
    const uint clusterCount = gridX * gridY * gridZ;
    // 処理するクラスタの一次元番号
    const uint clusterIndex = dispatchId.x;
    if (clusterIndex >= clusterCount)
    {
        return;
    }


    // クラスタの奥行き番号
    const uint clusterZ = clusterIndex / (gridX * gridY);
    // 奥行きを除いた平面番号
    const uint remainder =
        clusterIndex - clusterZ * gridX * gridY;
    // クラスタの縦番号
    const uint clusterY = remainder / gridX;
    // クラスタの横番号
    const uint clusterX = remainder - clusterY * gridX;

    // ビュー前方は-Zで、指数分割の距離には正の値を使う。
    // ビューの近クリップ距離
    const float nearPlane = DepthParameters.x;
    // ビューの遠クリップ距離
    const float farPlane = DepthParameters.y;
    // クラスタ近端の正の距離
    const float depthNear = nearPlane
        * pow(farPlane / nearPlane,
            (float)clusterZ / (float)gridZ);
    // クラスタ遠端の正の距離
    const float depthFar = nearPlane
        * pow(farPlane / nearPlane,
            ((float)clusterZ + 1.0f) / (float)gridZ);

    // 左上原点のクラスタ番号を右X・上Yへ変換し、近端と遠端の両方を含むAABBを作る。
    // クラスタ左端の画面比
    const float xRatioMinimum =
        (float)clusterX / (float)gridX * 2.0f - 1.0f;
    // クラスタ右端の画面比
    const float xRatioMaximum =
        ((float)clusterX + 1.0f) / (float)gridX * 2.0f - 1.0f;
    // クラスタ下端の上向き画面比
    const float yRatioMinimum =
        1.0f - ((float)clusterY + 1.0f) / (float)gridY * 2.0f;
    // クラスタ上端の上向き画面比
    const float yRatioMaximum =
        1.0f - (float)clusterY / (float)gridY * 2.0f;
    // 横半画角のtan
    const float tanX = FrustumParameters.x;
    // 縦半画角のtan
    const float tanY = FrustumParameters.y;

    // クラスタAABBの最小座標
    float3 aabbMinimum;
    // クラスタAABBの最大座標
    float3 aabbMaximum;
    aabbMinimum.x = min(
        xRatioMinimum * tanX * depthNear,
        xRatioMinimum * tanX * depthFar);
    aabbMaximum.x = max(
        xRatioMaximum * tanX * depthNear,
        xRatioMaximum * tanX * depthFar);
    aabbMinimum.y = min(
        yRatioMinimum * tanY * depthNear,
        yRatioMinimum * tanY * depthFar);
    aabbMaximum.y = max(
        yRatioMaximum * tanY * depthNear,
        yRatioMaximum * tanY * depthFar);

    aabbMinimum.z = -depthFar;
    aabbMaximum.z = -depthNear;

    // クラスタに登録する灯数上限
    const uint maximumPerCluster =
        (uint)DepthParameters.w;
    // 全ライト数
    const uint lightCount = (uint)GridParameters.w;
    // このクラスタの登録済み灯数
    uint written = 0;

    // 判定するライト番号
    [loop]
    for (uint lightIndex = 0;
        lightIndex < lightCount
            && written < maximumPerCluster;
        ++lightIndex)
    {
        // 判定するライトのGPU情報
        const GpuLight light = Lights[lightIndex];
        // ライト中心のビュー位置
        const float3 viewPosition = mul(
            float4(light.PositionRange.xyz, 1.0f),
            View).xyz;
        // スポットも到達半径の球で判定し、余分な寄与はPixel側のコーン減衰で除く。
        if (SphereIntersectsAabb(
                viewPosition,
                light.PositionRange.w,
                aabbMinimum,
                aabbMaximum))
        {
            LightIndexList[
                clusterIndex * maximumPerCluster
                    + written] = lightIndex;
            ++written;
        }
    }

    ClusterLightCounts[clusterIndex] = written;
}
