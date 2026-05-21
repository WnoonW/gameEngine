#pragma once
#include "../Common/d3dUtil.h"
#include "constantStruct.h"
#include "../Common/UploadBuffer.h"

// FrameResource.h / .cpp

struct FrameResource
{
    FrameResource(ID3D12Device* device, UINT objectCount);   // 생성자 변경
    //~FrameResource();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    //std::unique_ptr<UploadBuffer<PassConstants>>   PassCB = nullptr;
    std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB = nullptr;

    UINT64 FenceValue = 0;
};


inline FrameResource::FrameResource(ID3D12Device* device, UINT objectCount)
{
    ThrowIfFailed(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(CmdListAlloc.GetAddressOf())));

    // 필요에 따라 PassCB도 여기서 생성
    // PassCB = std::make_unique<UploadBuffer<PassConstants>>(device, 1, true);

    ObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(device, objectCount, true);
}