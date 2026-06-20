// FrustumCullingCS.hlsl

struct DrawIndexedArgs
{
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    uint BaseVertexLocation;
    uint StartInstanceLocation;
};

struct IndirectDrawCommand
{
    uint IndexCountPerInstance;
    uint StartIndexLocation;
    uint BaseVertexLocation;
    uint InstanceCount;
    uint StartInstanceLocation;
    uint MaterialIndex;
};

struct InstanceData
{
    float4x4 WorldViewProj;
};

RWStructuredBuffer<DrawIndexedArgs> IndirectArgs : register(u0);
StructuredBuffer<IndirectDrawCommand> DrawCommands : register(t0);
StructuredBuffer<InstanceData> InstanceDatas : register(t1);

cbuffer CullConstants : register(b0)
{
    float4x4 ViewProj;
    uint DrawCommandCount;
};

bool IsInsideFrustum(float4 clipPos)
{
    return (clipPos.x >= -clipPos.w && clipPos.x <= clipPos.w) &&
           (clipPos.y >= -clipPos.w && clipPos.y <= clipPos.w) &&
           (clipPos.z >= 0.0 && clipPos.z <= clipPos.w);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint index = dispatchThreadID.x;

    if (index >= DrawCommandCount)
        return;

    IndirectDrawCommand cmd = DrawCommands[index];

    // 첫 번째 인스턴스의 위치로 간단한 컬링 테스트
    uint firstInstance = cmd.StartInstanceLocation;
    float4 clipPos = mul(float4(0, 0, 0, 1), InstanceDatas[firstInstance].WorldViewProj);

    bool visible = IsInsideFrustum(clipPos);

    DrawIndexedArgs arg;
    arg.IndexCountPerInstance = cmd.IndexCountPerInstance;
    arg.InstanceCount = visible ? cmd.InstanceCount : 0;
    arg.StartIndexLocation = cmd.StartIndexLocation;
    arg.BaseVertexLocation = cmd.BaseVertexLocation;
    arg.StartInstanceLocation = cmd.StartInstanceLocation;

    IndirectArgs[index] = arg;
}