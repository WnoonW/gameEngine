#pragma once
#include "constantStruct.h"
#include "../Common/UploadBuffer.h"

// FrameResource.h 또는 d3dUtil.h에 추가
struct FrameResource
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    // 이 프레임이 끝났는지 확인할 Fence 값
    UINT64 FenceValue = 0;

    // 나중에 추가할 per-frame 리소스들 (강력 추천)
    // std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB = nullptr;
    // std::unique_ptr<UploadBuffer<PassConstants>> PassCB = nullptr;
    // ...
};