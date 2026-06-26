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
    void render(World& world,
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        DescriptorAllocator* descriptorAllocator,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);
};