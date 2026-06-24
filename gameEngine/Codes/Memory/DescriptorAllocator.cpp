#include "DescriptorAllocator.h"
#include "d3dUtil.h"
#include <stdexcept>

void DescriptorAllocator::Initialize(ID3D12Device* device, UINT numDescriptors)
{
    assert(device != nullptr);
    mNumDescriptors = numDescriptors;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = numDescriptors;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mHeap)));

    mDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    mIsUsed.resize(numDescriptors, false);

    // 처음에는 전부 사용 가능
    for (UINT i = 0; i < numDescriptors; ++i)
        mFreeList.push(i);
}

void DescriptorAllocator::Shutdown()
{
    mHeap.Reset();
    mIsUsed.clear();
    while (!mFreeList.empty()) mFreeList.pop();
}

DescriptorAllocator::DescriptorHandle DescriptorAllocator::Allocate()
{
    if (mFreeList.empty())
    {
        throw std::runtime_error("Descriptor Heap is full! Increase size or implement better allocator.");
    }

    UINT index = mFreeList.front();
    mFreeList.pop();
    mIsUsed[index] = true;

    DescriptorHandle handle;
    handle.Index = index;

    auto cpuStart = mHeap->GetCPUDescriptorHandleForHeapStart();
    auto gpuStart = mHeap->GetGPUDescriptorHandleForHeapStart();

    handle.CPU.ptr = cpuStart.ptr + (UINT64)index * mDescriptorSize;
    handle.GPU.ptr = gpuStart.ptr + (UINT64)index * mDescriptorSize;

    return handle;
}

DescriptorAllocator::DescriptorHandle DescriptorAllocator::Allocate(UINT count)
{
    if (count == 0) count = 1;
    if (mFreeList.size() < count)
    {
        throw std::runtime_error("Not enough descriptors in heap for range allocation.");
    }

    // 연속된 count개의 인덱스를 찾음 (간단한 구현)
    std::vector<UINT> allocatedIndices;
    allocatedIndices.reserve(count);

    for (UINT i = 0; i < count; ++i)
    {
        if (mFreeList.empty()) break;

        UINT index = mFreeList.front();
        mFreeList.pop();
        mIsUsed[index] = true;
        allocatedIndices.push_back(index);
    }

    if (allocatedIndices.size() < count)
    {
        // 실패 시 롤백 (간단하게 예외 처리)
        for (auto idx : allocatedIndices)
        {
            mIsUsed[idx] = false;
            mFreeList.push(idx);
        }
        throw std::runtime_error("Failed to allocate contiguous descriptor range.");
    }

    DescriptorHandle handle;
    handle.Index = allocatedIndices[0];   // 시작 인덱스

    auto cpuStart = mHeap->GetCPUDescriptorHandleForHeapStart();
    auto gpuStart = mHeap->GetGPUDescriptorHandleForHeapStart();

    handle.CPU.ptr = cpuStart.ptr + (UINT64)allocatedIndices[0] * mDescriptorSize;
    handle.GPU.ptr = gpuStart.ptr + (UINT64)allocatedIndices[0] * mDescriptorSize;

    return handle;
}

void DescriptorAllocator::Free(const DescriptorHandle& handle)
{
    UINT index = handle.Index;

    if (index == UINT_MAX && mHeap)
    {
        auto cpuStart = mHeap->GetCPUDescriptorHandleForHeapStart();
        index = (UINT)((handle.CPU.ptr - cpuStart.ptr) / mDescriptorSize);
    }

    if (index < mNumDescriptors && mIsUsed[index])
    {
        mIsUsed[index] = false;
        mFreeList.push(index);
    }
}

UINT DescriptorAllocator::GetUsedCount() const
{
    return mNumDescriptors - (UINT)mFreeList.size();
}