#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>

#include "World.h"
#include "ComponentStruct.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"

using namespace ECS;

class RenderSystem
{
public:
    void createCBV(ID3D12Device* device,
        std::vector<std::unique_ptr<FrameResource>>& frameResources,
        int gNumFrameResources,
        DescriptorAllocator& descriptorAllocator,
        Entity entity,
        World& world);

    void renderIndexedInstanced(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    void renderExecuteIndirect(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    // 이전 구현 (draw마다 ExecuteIndirect(1), 비교/디버그용)
    void renderExecuteIndirect1(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

private:
    std::vector<std::vector<DescriptorAllocator::DescriptorHandle>> mEntityCBVHandles;
};