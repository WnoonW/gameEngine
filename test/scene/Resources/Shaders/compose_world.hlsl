// Step F1: TRS → GpuInstanceSource.world (byte-compatible with CPU path)
//
// CPU:
//   W = S * R * T   (DirectXMath, row-vector: v' = v * W)
//   StoreWorldTransposed: XMStoreFloat4x4(transpose(W))
//   → memory = columns of W as consecutive float4s
//
// HLSL default pack_matrix(column_major):
//   writing float4x4 stores columns as consecutive float4s
//   → o.world = W  matches CPU StoreWorldTransposed (do NOT also transpose)
//
// VS: mul(float4(pos,1), world)  — same as object_instanced / Path2

#pragma pack_matrix(column_major)

struct GpuTransform
{
    float3 position; float pad0;
    float3 rotation; float pad1; // pitch, yaw, roll (radians)
    float3 scale;    float pad2;
    float3 boundsCenter;  float pad3;
    float3 boundsExtents; float pad4;
    uint batchId;
    uint flags;
    uint pad5;
    uint pad6;
};

struct GpuInstanceSource
{
    float4x4 world;
    float boundsCenterX, boundsCenterY, boundsCenterZ, boundsPad0;
    float boundsExtentsX, boundsExtentsY, boundsExtentsZ, boundsPad1;
    uint batchId;
    uint flags;
    uint pad2;
    uint pad3;
};

cbuffer cbCompose : register(b0)
{
    uint gNumInstances;
    uint gMaxInstances;
    uint gPad0;
    uint gPad1;
};

StructuredBuffer<GpuTransform>        gXforms : register(t0);
RWStructuredBuffer<GpuInstanceSource> gOut    : register(u0);

float4x4 MatrixScale(float3 s)
{
    // Constructor args are always row-major fill of matrix elements
    return float4x4(
        s.x, 0,   0,   0,
        0,   s.y, 0,   0,
        0,   0,   s.z, 0,
        0,   0,   0,   1);
}

float4x4 MatrixTranslation(float3 t)
{
    return float4x4(
        1,   0,   0,   0,
        0,   1,   0,   0,
        0,   0,   1,   0,
        t.x, t.y, t.z, 1);
}

// DirectXMath XMMatrixRotationRollPitchYaw (no-intrinsics path in DirectXMathMatrix.inl)
float4x4 MatrixRotationRollPitchYaw(float pitch, float yaw, float roll)
{
    float cp = cos(pitch), sp = sin(pitch);
    float cy = cos(yaw),   sy = sin(yaw);
    float cr = cos(roll),  sr = sin(roll);

    float4x4 m;
    // row0
    m._11 = cr * cy + sr * sp * sy;
    m._12 = sr * cp;
    m._13 = sr * sp * cy - cr * sy;
    m._14 = 0;
    // row1
    m._21 = cr * sp * sy - sr * cy;
    m._22 = cr * cp;
    m._23 = sr * sy + cr * sp * cy;
    m._24 = 0;
    // row2
    m._31 = cp * sy;
    m._32 = -sp;
    m._33 = cp * cy;
    m._34 = 0;
    // row3
    m._41 = 0;
    m._42 = 0;
    m._43 = 0;
    m._44 = 1;
    return m;
}

[numthreads(64, 1, 1)]
void CS_ComposeWorld(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= gNumInstances || i >= gMaxInstances)
        return;

    GpuTransform x = gXforms[i];
    float4x4 S = MatrixScale(x.scale);
    float4x4 R = MatrixRotationRollPitchYaw(x.rotation.x, x.rotation.y, x.rotation.z);
    float4x4 T = MatrixTranslation(x.position);

    // Same multiply order as TransformComponent::GetWorldMatrix: S * R * T
    float4x4 W = mul(mul(S, R), T);

    GpuInstanceSource o;
    // column_major store of W == CPU XMStoreFloat4x4(XMMatrixTranspose(W))
    o.world = W;
    o.boundsCenterX = x.boundsCenter.x;
    o.boundsCenterY = x.boundsCenter.y;
    o.boundsCenterZ = x.boundsCenter.z;
    o.boundsPad0 = 0;
    o.boundsExtentsX = x.boundsExtents.x;
    o.boundsExtentsY = x.boundsExtents.y;
    o.boundsExtentsZ = x.boundsExtents.z;
    o.boundsPad1 = 0;
    o.batchId = x.batchId;
    o.flags = x.flags;
    o.pad2 = 0;
    o.pad3 = 0;
    gOut[i] = o;
}
