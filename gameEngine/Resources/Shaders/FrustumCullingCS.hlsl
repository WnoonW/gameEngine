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
    uint MaterialIndex;
};

RWStructuredBuffer<DrawIndexedArgs> IndirectArgs : register(u0);
StructuredBuffer<IndirectDrawCommand> DrawCommands : register(t0);

cbuffer CullConstants : register(b0)
{
    uint DrawCommandCount;
};

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint index = dispatchThreadID.x;

    if (index >= DrawCommandCount)
        return;

    IndirectDrawCommand cmd = DrawCommands[index];

    DrawIndexedArgs arg;
    arg.IndexCountPerInstance = cmd.IndexCountPerInstance;
    arg.InstanceCount = cmd.InstanceCount; // ← 실제 인스턴스 수 사용
    arg.StartIndexLocation = cmd.StartIndexLocation;
    arg.BaseVertexLocation = cmd.BaseVertexLocation;
    arg.StartInstanceLocation = 0; // 나중에 개선 (현재는 0으로 시작)

    IndirectArgs[index] = arg;
}