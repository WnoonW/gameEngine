#pragma once
#include "Registry.h"
#include "ComponentStruct.h"
#include <d3d12.h>
#include <DirectXMath.h>

class RenderSystem {
public:
    void render(Registry& registry,
        ID3D12GraphicsCommandList* cmdList,
        class FrameResource* currentFrameResource,
        class DescriptorAllocator* descriptorAllocator, // 필요시 사용
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);
};