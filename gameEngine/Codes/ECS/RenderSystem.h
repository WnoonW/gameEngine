#pragma once
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>
#include <memory>

#include "Registry.h"
#include "ComponentStruct.h"
#include "../V1_/V4/DescriptorAllocator.h"
#include "../Structs/AppStruct.h"

class RenderSystem {
public:
    void createCBV(ID3D12Device* device, std::vector<std::unique_ptr<FrameResource>>& frameResources, 
                                    int gNumFrameResources, DescriptorAllocator& descriptorAllocator, Entity entity, Registry& registry);
    void render(Registry& registry,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    std::vector<std::vector<DescriptorAllocator::DescriptorHandle>> mEntityCBVHandles;
};