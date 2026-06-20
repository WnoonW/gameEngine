// FrustumCullingCS.hlsl

struct DrawIndexedArgs
{
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    uint BaseVertexLocation;
    uint StartInstanceLocation;
};

RWStructuredBuffer<DrawIndexedArgs> IndirectArgs : register(u0);

// 나중에 상수 버퍼로 받을 데이터
cbuffer CullConstants : register(b0)
{
    float4x4 ViewProj;
    uint MaxInstanceCount;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint index = dispatchThreadID.x;
    
    if (index >= MaxInstanceCount)
        return;

    // TODO: 나중에 인스턴스 데이터 StructuredBuffer에서 읽어올 예정
    // 지금은 일단 모든 인스턴스를 살아있다고 가정하고 Argument 작성

    DrawIndexedArgs arg;
    arg.IndexCountPerInstance = 36; // 나중에 Submesh에서 가져올 값
    arg.InstanceCount = 1;
    arg.StartIndexLocation = 0;
    arg.BaseVertexLocation = 0;
    arg.StartInstanceLocation = index;

    IndirectArgs[index] = arg;
}