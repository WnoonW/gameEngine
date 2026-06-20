#pragma once
#include "../Common/d3dUtil.h"
#include "constantStruct.h"
#include "../Common/UploadBuffer.h"
#include <imgui.h>

// FrameResource.h / .cpp

struct FrameResource
{
    FrameResource(ID3D12Device* device, UINT objectCount, UINT maxIndirectArgs = 16384);   // 생성자 변경
    ~FrameResource();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    std::unique_ptr<UploadBuffer<PassConstants>>   PassCB = nullptr;
    std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB = nullptr;
    std::unique_ptr<UploadBuffer<InstanceData>>    InstanceDataBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> IndirectArgsUAVBuffer;
    UINT64 FenceValue = 0;
};


inline FrameResource::FrameResource(ID3D12Device* device, UINT objectCount, UINT maxIndirectArgs)
{
    ThrowIfFailed(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(CmdListAlloc.GetAddressOf())));

    PassCB = std::make_unique<UploadBuffer<PassConstants>>(device, 1, true);
    ObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(device, objectCount, true);
    InstanceDataBuffer = std::make_unique<UploadBuffer<InstanceData>>(device, objectCount, false);

    // === Phase 2: Indirect Args UAV 버퍼 생성 ===
    D3D12_HEAP_PROPERTIES heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC resDesc = CD3DX12_RESOURCE_DESC::Buffer(
        sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) * maxIndirectArgs,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS   // ← 중요!
    );

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProp,
        D3D12_HEAP_FLAG_NONE,
        &resDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,   // Compute Shader가 쓸 상태
        nullptr,
        IID_PPV_ARGS(&IndirectArgsUAVBuffer)));
}

inline FrameResource::~FrameResource()
{
    if (IndirectArgsUAVBuffer) IndirectArgsUAVBuffer.Reset();

    if (InstanceDataBuffer) InstanceDataBuffer.reset();
    if (ObjectCB) ObjectCB.reset();
    if (PassCB) PassCB.reset();
    if (CmdListAlloc) CmdListAlloc.Reset();
}