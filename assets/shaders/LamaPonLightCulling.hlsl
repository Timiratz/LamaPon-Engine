// クラスタライトカリング（Forward+）のCompute Shaderです。
//
// ビューの視錐台を「横16×縦9×奥行き24」の格子（クラスタ）に切り、
// クラスタごとに「届くライトの番号表」を作ります。描画側
// （LamaPonLit.hlsl）は自分のピクセルが入っているクラスタの表だけを
// 見てライトを計算するので、シーン全体のライト数が増えても
// ピクセルあたりの計算量は「近くにあるライトの数」で決まります。
//
// 奥行きの分割は等間隔ではなく指数分割です（近くほど細かい）。
// 画面に映る物の多くはカメラの近くにあるので、近距離に格子を
// 集中させたほうが1クラスタあたりのライト数が減ります。

// ライト1灯ぶん。C++側のGpuLight（ClusteredLights.h）と
// 並びを一致させてください。
struct GpuLight
{
    // xyz=ワールド位置, w=届く距離。
    float4 PositionRange;
    // rgb=色, w=強度。
    float4 ColorIntensity;
    // xyz=向き（スポット）, w=内側コーンのcos。
    float4 DirectionInnerCosine;
    // x=外側コーンのcos, y=種別（0=ポイント, 1=スポット）,
    // z=影の参照（スポット=スロット+1, ポイント=ライト番号+1,
    //   0=影なし）, w=予約。
    float4 ExtraParameters;
};

cbuffer ClusterCullingBuffer : register(b0)
{
    // ワールド→ビュー変換。ライトの球をビュー空間へ移して
    // クラスタの箱と当てます。
    row_major float4x4 View;
    // x=横分割数, y=縦分割数, z=奥行き分割数, w=ライト総数。
    float4 GridParameters;
    // x=near, y=far, z=log(far/near), w=クラスタあたり上限。
    float4 DepthParameters;
    // 視錐台の広がり。x=tan(横半視野), y=tan(縦半視野)。
    // 深さdでの画面の届く範囲は x∈[-d*tanX, d*tanX] です。
    float4 FrustumParameters;
};

StructuredBuffer<GpuLight> Lights : register(t0);
// クラスタ番号×上限 + 何番目、の位置へライト番号を書きます。
RWStructuredBuffer<uint> LightIndexList : register(u0);
// クラスタごとの、表に入っているライト数。
RWStructuredBuffer<uint> ClusterLightCounts : register(u1);

// 球と軸平行の箱（AABB）の距離判定。
bool SphereIntersectsAabb(
    float3 center,
    float radius,
    float3 aabbMinimum,
    float3 aabbMaximum)
{
    const float3 closest = clamp(
        center,
        aabbMinimum,
        aabbMaximum);
    const float3 delta = center - closest;
    return dot(delta, delta) <= radius * radius;
}

// 1スレッド＝1クラスタ。スレッドが自分のクラスタの表を独占して
// 書くので、アトミック操作は要りません。
[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint gridX = (uint)GridParameters.x;
    const uint gridY = (uint)GridParameters.y;
    const uint gridZ = (uint)GridParameters.z;
    const uint clusterCount = gridX * gridY * gridZ;
    const uint clusterIndex = dispatchId.x;
    if (clusterIndex >= clusterCount)
    {
        return;
    }

    // 一次元番号を(x, y, z)へ戻します。
    const uint clusterZ = clusterIndex / (gridX * gridY);
    const uint remainder =
        clusterIndex - clusterZ * gridX * gridY;
    const uint clusterY = remainder / gridX;
    const uint clusterX = remainder - clusterY * gridX;

    // 奥行きの範囲（指数分割）。ビュー空間は右手系でカメラの前が
    // -Zなので、ここでは正の距離として扱い、最後に符号を付けます。
    const float nearPlane = DepthParameters.x;
    const float farPlane = DepthParameters.y;
    const float depthNear = nearPlane
        * pow(farPlane / nearPlane,
            (float)clusterZ / (float)gridZ);
    const float depthFar = nearPlane
        * pow(farPlane / nearPlane,
            ((float)clusterZ + 1.0f) / (float)gridZ);

    // クラスタの箱。横・縦の届く範囲は深さに比例して広がるので、
    // 手前と奥の両方で角を求めて、それらを含む箱にします
    // （保守的＝取りこぼしなし。多少余分に入るのは許容）。
    // クラスタ番号はピクセル座標と同じ「左上が(0,0)」です。
    // ビュー空間は右が+X・上が+Yなので、Yは向きを反転させます
    // （ここを揃えないと、描画側が引くクラスタと上下逆の箱を
    // 作ってしまい、ライトが1灯も見つからなくなります）。
    const float xRatioMinimum =
        (float)clusterX / (float)gridX * 2.0f - 1.0f;
    const float xRatioMaximum =
        ((float)clusterX + 1.0f) / (float)gridX * 2.0f - 1.0f;
    const float yRatioMinimum =
        1.0f - ((float)clusterY + 1.0f) / (float)gridY * 2.0f;
    const float yRatioMaximum =
        1.0f - (float)clusterY / (float)gridY * 2.0f;
    const float tanX = FrustumParameters.x;
    const float tanY = FrustumParameters.y;

    float3 aabbMinimum;
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
    // カメラの前は-Z。
    aabbMinimum.z = -depthFar;
    aabbMaximum.z = -depthNear;

    const uint maximumPerCluster =
        (uint)DepthParameters.w;
    const uint lightCount = (uint)GridParameters.w;
    uint written = 0;

    [loop]
    for (uint lightIndex = 0;
        lightIndex < lightCount
            && written < maximumPerCluster;
        ++lightIndex)
    {
        const GpuLight light = Lights[lightIndex];
        const float3 viewPosition = mul(
            float4(light.PositionRange.xyz, 1.0f),
            View).xyz;
        // スポットも球として当てます（コーンの正確な判定より
        // 余分に入りますが、取りこぼしはありません。余分な分は
        // ピクセル側のコーン減衰で0になります）。
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
