#pragma once

#include <d3d12.h>
#include "DescriptorAllocator.h"

// Startup materials / meshes / primitives / LOD variants used by both Editor and Game.
namespace DefaultAssets
{
    // Records into an open command list (caller submits & flushes).
    void Load(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12CommandQueue* cmdQueue,
        DescriptorAllocator& descriptorAllocator);
}
