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

    void render(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    // 추후 PassCB를 root에 바인딩할 때 사용 예정 (지금은 Fill만)
    // void BindPassCB(ID3D12GraphicsCommandList* cmdList, FrameResource* fr);

private:
    std::vector<std::vector<DescriptorAllocator::DescriptorHandle>> mEntityCBVHandles;
};