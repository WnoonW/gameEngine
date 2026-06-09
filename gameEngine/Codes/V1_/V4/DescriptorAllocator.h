#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <queue>
#include <cassert>

using Microsoft::WRL::ComPtr;

class DescriptorAllocator
{
public:
    struct DescriptorHandle
    {
        D3D12_CPU_DESCRIPTOR_HANDLE CPU = {};
        D3D12_GPU_DESCRIPTOR_HANDLE GPU = {};
        UINT Index = UINT_MAX;          // Heap 내 인덱스
    };

public:
    void Initialize(ID3D12Device* device, UINT numDescriptors = 8192);
    void Shutdown();

    // descriptor 할당
    DescriptorHandle Allocate();

    // descriptor 반환 (오브젝트 삭제 시 호출)
    void Free(const DescriptorHandle& handle);
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);   // ImGui용 간편 Free

    // 현재 사용 중인 descriptor 개수
    UINT GetUsedCount() const;

    ID3D12DescriptorHeap* GetHeap() const { return mHeap.Get(); }
    UINT GetDescriptorSize() const { return mDescriptorSize; }

private:
    ComPtr<ID3D12DescriptorHeap> mHeap;
    UINT mDescriptorSize = 0;
    UINT mNumDescriptors = 0;

    std::vector<bool>   mIsUsed;
    std::queue<UINT>    mFreeList;
};