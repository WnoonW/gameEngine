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
        ComPtr<ID3D12Resource> commandBuffer;
        ComPtr<ID3D12Resource> countBuffer;   // UINT * kMaxIndirectGroups
        ComPtr<ID3D12Resource> buildCBUpload; // aligned slot * kMaxIndirectGroups
        BYTE* requestMapped = nullptr;
        BYTE* buildCBMapped = nullptr;
        D3D12_RESOURCE_STATES commandState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES countState = D3D12_RESOURCE_STATE_COMMON;
    };

    struct GroupJob
    {
        Mesh* mesh = nullptr;
        UINT64 materialGpu = 0;
        UINT requestOffset = 0;  // element index into request buffer
        UINT requestCount = 0;
        UINT commandOffset = 0;  // element index into command buffer
        UINT groupIndex = 0;     // count buffer / build CB slot
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
    ComPtr<ID3D12Resource> mCountZeroUpload;

    FrameIndirectResources mFrames[kIndirectFrameCount]{};
    bool mIndirectReady = false;
};
