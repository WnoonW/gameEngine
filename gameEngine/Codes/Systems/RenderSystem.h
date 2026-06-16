#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>

#include "World.h"
#include "ComponentStruct.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"

using namespace ECS;

struct RenderStats
{
    int totalRenderableEntities = 0;
    int drawBatches = 0;
    int peakInstancesPerBatch = 0;
    int instanceBufferCapacity = 0;
};

class RenderSystem
{
public:
    void Initialize(ID3D12Device* device,
        std::vector<std::unique_ptr<FrameResource>>& frameResources,
        int gNumFrameResources,
        DescriptorAllocator& descriptorAllocator);

    void render(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    const RenderStats& GetLastRenderStats() const { return mLastRenderStats; }

private:
    std::vector<DescriptorAllocator::DescriptorHandle> mPassCBVHandles;
    std::vector<DescriptorAllocator::DescriptorHandle> mInstanceSRVHandles;
    UINT mMaxInstancesPerFrame = 0;
    RenderStats mLastRenderStats{};
};