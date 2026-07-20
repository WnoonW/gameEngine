#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <vector>

#include "World.h"
#include "ComponentStruct.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"
#include "../Structs/IndirectDrawStructs.h"
#include "../Structs/RenderLimits.h"

using namespace ECS;
using Microsoft::WRL::ComPtr;

enum class RenderPath
{
    Direct = 0,
    Indirect = 1
};

class RenderSystem
{
public:
    void Initialize(ID3D12Device* device);
    void Shutdown();

    void SetRenderPath(RenderPath path) { mRenderPath = path; }
    RenderPath GetRenderPath() const { return mRenderPath; }

    void render(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

private:
    struct FrameIndirectResources
    {
        ComPtr<ID3D12Resource> requestUpload;
        ComPtr<ID3D12Resource> instanceBuffer;
        ComPtr<ID3D12Resource> countBuffer;
        ComPtr<ID3D12Resource> drawCmdBuffer;
        ComPtr<ID3D12Resource> buildCBUpload;
        BYTE* requestMapped = nullptr;
        BYTE* buildCBMapped = nullptr;
        D3D12_RESOURCE_STATES instanceState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES countState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES drawCmdState = D3D12_RESOURCE_STATE_COMMON;
    };

    // 한 메시 타입 = 인스턴스 압축 1회, 서브메시마다 드로우
    struct MeshBatch
    {
        Mesh* mesh = nullptr;
        UINT requestOffset = 0;
        UINT requestCount = 0;
        UINT batchIndex = 0; // build CB slot
    };

    struct SubmeshDraw
    {
        UINT64 materialGpu = 0;
        UINT indexCount = 0;
        UINT startIndex = 0;
        INT baseVertex = 0;
    };

    void renderDirect(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    void renderIndirect(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    void EnsureIndirectResources(ID3D12Device* device);
    void DestroyIndirectResources();
    void ExtractFrustumPlanes(const DirectX::XMMATRIX& viewProj, float outPlanes[6][4]);
    static UINT BuildCBAlignedSize();

    RenderPath mRenderPath = RenderPath::Indirect;
    ID3D12Device* mDevice = nullptr;
    ComPtr<ID3D12PipelineState> mIndirectBuildPSO;
    ComPtr<ID3D12PipelineState> mIndirectFinalizePSO;
    ComPtr<ID3D12Resource> mCountZeroUpload;
    ComPtr<ID3D12Resource> mDummyInstanceBuffer;

    FrameIndirectResources mFrames[kIndirectFrameCount]{};
    bool mIndirectReady = false;
};
