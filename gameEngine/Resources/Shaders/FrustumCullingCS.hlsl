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

cbuffer CullConstants : register(b0)
{
    uint MaxInstanceCount;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint index = dispatchThreadID.x;

    if (index >= MaxInstanceCount)
        return;

    DrawIndexedArgs arg;
    arg.IndexCountPerInstance = 36;
    arg.InstanceCount = 1;
    arg.StartIndexLocation = 0;
    arg.BaseVertexLocation = 0;
    arg.StartInstanceLocation = index;

    IndirectArgs[index] = arg;
}