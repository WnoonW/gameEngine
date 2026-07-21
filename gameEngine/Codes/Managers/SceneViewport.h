#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <cstdint>

#include "DescriptorAllocator.h"

// Offscreen color + depth target for the editor Scene panel.
// Color is exposed as an SRV for ImGui::Image after End().
// Depth is exposed as R24_UNORM SRV for Hi-Z / occlusion (after End).
class SceneViewport
{
public:
    void Initialize(
        ID3D12Device* device,
        DescriptorAllocator& srvAllocator,
        DXGI_FORMAT colorFormat,
        DXGI_FORMAT depthFormat);

    void Shutdown();

    bool Resize(UINT width, UINT height);

    void Begin(ID3D12GraphicsCommandList* cmdList, const float clearColor[4]);
    void End(ID3D12GraphicsCommandList* cmdList);

    bool IsValid() const { return mColor != nullptr && mWidth > 0 && mHeight > 0; }
    UINT GetWidth() const { return mWidth; }
    UINT GetHeight() const { return mHeight; }
    float GetAspectRatio() const
    {
        return (mHeight > 0) ? static_cast<float>(mWidth) / static_cast<float>(mHeight) : 1.0f;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpu() const { return mSrv.GPU; }

    // Depth (for Path3 Hi-Z). Valid after End() → NON_PIXEL_SHADER_RESOURCE.
    ID3D12Resource* GetDepthResource() const { return mDepth.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthSrvCpu() const { return mDepthSrv.CPU; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetDepthSrvGpu() const { return mDepthSrv.GPU; }
    bool HasDepthSrv() const { return mDepthSrv.Index != UINT_MAX; }

private:
    void DestroySizeDependentResources();
    void CreateSizeDependentResources(UINT width, UINT height);

    ID3D12Device* mDevice = nullptr;
    DescriptorAllocator* mSrvAllocator = nullptr;
    DXGI_FORMAT mColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    Microsoft::WRL::ComPtr<ID3D12Resource> mColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDepth;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    DescriptorAllocator::DescriptorHandle mSrv{};
    DescriptorAllocator::DescriptorHandle mDepthSrv{};

    UINT mWidth = 0;
    UINT mHeight = 0;
    D3D12_RESOURCE_STATES mColorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mDepthState = D3D12_RESOURCE_STATE_COMMON;
};
