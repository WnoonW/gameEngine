#pragma once
#include "../Common/d3dUtil.h"
#include "constantStruct.h"
#include "../Common/UploadBuffer.h"
#include <imgui.h>

namespace RenderLimits
{
    static constexpr UINT MaxObjectCount = 4096;
    static constexpr UINT MaxDrawCommandCount = 32768;
    static constexpr UINT MaxInstanceCount = 65536;   // Instancing용 InstanceBuffer 크기 (솔루션 2)
    static constexpr UINT DescriptorHeapCapacity = 32768;
}

#include <unordered_map>
#include <string>

struct MeshInstanceStats
{
    std::unordered_map<std::string, int> countByMesh;
    int totalInstances = 0;
    uint32_t estimatedDrawCommands = 0;
    uint32_t objectCBUsed = 0;
    uint32_t objectCBCapacity = RenderLimits::MaxObjectCount;
    uint32_t drawCommandCapacity = RenderLimits::MaxDrawCommandCount;
    uint32_t descriptorsUsed = 0;
    uint32_t descriptorCapacity = RenderLimits::DescriptorHeapCapacity;
    std::string lastCreateError;
};

// FrameResource.h / .cpp

struct FrameResource
{
    FrameResource(ID3D12Device* device, UINT objectCount, UINT argumentBufferSize, UINT instanceBufferSize);
    ~FrameResource();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    std::unique_ptr<UploadBuffer<PassConstants>> PassCB = nullptr;
    std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB = nullptr;

    // ArgumentBuffer (ExecuteIndirect용)
    Microsoft::WRL::ComPtr<ID3D12Resource> ArgumentBuffer;
    UINT8* MappedArgumentBuffer = nullptr;
    UINT ArgumentBufferSize = 0;

    // InstanceBuffer (ExecuteIndirect Instancing용)
    // StructuredBuffer<ObjectConstants>로 바인딩하여 SV_InstanceID로 접근
    Microsoft::WRL::ComPtr<ID3D12Resource> InstanceBuffer;
    UINT8* MappedInstanceBuffer = nullptr;
    UINT InstanceBufferSize = 0;

    UINT64 FenceValue = 0;
};


// =============================================
// FrameResource.cpp
// =============================================

inline FrameResource::FrameResource(ID3D12Device* device, UINT objectCount, UINT argumentBufferSize, UINT instanceBufferSize)
{
    ThrowIfFailed(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(CmdListAlloc.GetAddressOf())));

    PassCB = std::make_unique<UploadBuffer<PassConstants>>(device, 1, true);
    ObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(device, objectCount, true);

    // ==================== ArgumentBuffer 생성 ====================
    if (argumentBufferSize > 0)
    {
        ArgumentBufferSize = argumentBufferSize;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = argumentBufferSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&ArgumentBuffer)));

        // 영구 매핑 (Upload 버퍼는 보통 이렇게 사용)
        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(ArgumentBuffer->Map(0, &readRange,
            reinterpret_cast<void**>(&MappedArgumentBuffer)));
    }

    // ==================== InstanceBuffer 생성 (Instancing용) ====================
    // 솔루션 2: objectCount가 아닌 독립적으로 큰 크기 사용 (MaxInstanceCount 기준)
    if (instanceBufferSize > 0)
    {
        InstanceBufferSize = instanceBufferSize;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = instanceBufferSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&InstanceBuffer)));

        CD3DX12_RANGE readRange(0, 0);
        ThrowIfFailed(InstanceBuffer->Map(0, &readRange,
            reinterpret_cast<void**>(&MappedInstanceBuffer)));
    }
}

inline FrameResource::~FrameResource()
{
    if (MappedArgumentBuffer)
    {
        ArgumentBuffer->Unmap(0, nullptr);
        MappedArgumentBuffer = nullptr;
    }
    if (MappedInstanceBuffer)
    {
        InstanceBuffer->Unmap(0, nullptr);
        MappedInstanceBuffer = nullptr;
    }

    ArgumentBuffer.Reset();
    InstanceBuffer.Reset();
    PassCB.reset();
    ObjectCB.reset();
    CmdListAlloc.Reset();
}