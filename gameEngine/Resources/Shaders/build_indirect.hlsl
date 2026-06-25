// build_indirect.hlsl
// Compute Shader to record IndirectDrawCommand on GPU

struct IndirectDrawCommand
{
    uint baseInstance;
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    int  BaseVertexLocation;
    uint StartInstanceLocation;
};

struct GroupDrawData
{
    uint baseInstance;
    uint instanceCount;
    uint indexCountPerInstance;
    uint startIndexLocation;
    int  baseVertexLocation;
};

// Input: list of group data prepared on CPU
StructuredBuffer<GroupDrawData> gGroupData : register(t0);

// Output: the argument buffer for ExecuteIndirect
RWStructuredBuffer<IndirectDrawCommand> gOutCommands : register(u0);

// Optional: output count (for ExecuteIndirect with count buffer)
RWStructuredBuffer<uint> gOutCount : register(u1);

// Root constant (b0) - matches root signature InitAsConstants(1, 0)
uint gNumGroups : register(b0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint i = dispatchThreadID.x;
    if (i >= gNumGroups)
        return;

    GroupDrawData data = gGroupData[i];

    IndirectDrawCommand cmd;
    cmd.baseInstance = data.baseInstance;  // 이 값이 CommandSignature에 의해 root constant(b13)로 전달됨
    cmd.IndexCountPerInstance = data.indexCountPerInstance;
    cmd.InstanceCount = data.instanceCount;
    cmd.StartIndexLocation = data.startIndexLocation;
    cmd.BaseVertexLocation = data.baseVertexLocation;
    cmd.StartInstanceLocation = 0;  // CONSTANT의 baseInstance를 사용하므로 0

    gOutCommands[i] = cmd;

    // For count buffer support (we write the total at the end or use atomic if needed)
    // Here we just set at [0] if last thread, but better use separate dispatch or CPU count for simplicity.
    // For demo, we will use CPU known count.
}