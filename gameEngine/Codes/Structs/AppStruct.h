#pragma once
#include "../Common/d3dUtil.h"
#include "constantStruct.h"
#include "../Common/UploadBuffer.h"
#include <imgui.h>

// FrameResource.h / .cpp

struct FrameResource
{
    FrameResource(ID3D12Device* device, UINT objectCount);   // 생성자 변경
    ~FrameResource();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    std::unique_ptr<UploadBuffer<PassConstants>>   PassCB = nullptr;
    std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB = nullptr;
    std::unique_ptr<UploadBuffer<InstanceData>>    InstanceDataBuffer = nullptr;

    UINT64 FenceValue = 0;
};


inline FrameResource::FrameResource(ID3D12Device* device, UINT objectCount)
{
    ThrowIfFailed(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(CmdListAlloc.GetAddressOf())));

    PassCB = std::make_unique<UploadBuffer<PassConstants>>(device, 1, true);
    ObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(device, objectCount, true);
    InstanceDataBuffer = std::make_unique<UploadBuffer<InstanceData>>(device, objectCount, false);
}

inline FrameResource::~FrameResource()
{
    if (ObjectCB)
        ObjectCB.reset();

    if (PassCB)
        PassCB.reset();

    if (InstanceDataBuffer)
        InstanceDataBuffer.reset();

    if (CmdListAlloc)
        CmdListAlloc.Reset();
}