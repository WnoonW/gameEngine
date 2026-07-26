#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <cstdint>

#include "DescriptorAllocator.h"

// Offscreen color + depth for the editor Scene panel / play view.
// Renders at 4x MSAA, resolves color for ImGui::Image (and CopyColorTo).
// Depth stays MSAA; Hi-Z samples sample-0 via Texture2DMS SRV.
class SceneViewport
{
public:
    static constexpr UINT kMsaaCount = 4;

    void Initialize(
        ID3D12Device* device,
        DescriptorAllocator& srvAllocator,
        DXGI_FORMAT colorFormat,
        DXGI_FORMAT depthFormat);

    void Shutdown();

    bool Resize(UINT width, UINT height);

    void Begin(ID3D12GraphicsCommandList* cmdList, const float clearColor[4]);
    void End(ID3D12GraphicsCommandList* cmdList);

    // Play mode: resolve already done in End(); copy resolved color into backbuffer.
    void CopyColorTo(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* dest,
        D3D12_RESOURCE_STATES destStateBefore);

    bool IsValid() const { return mColorResolved != nullptr && mWidth > 0 && mHeight > 0; }
    UINT GetWidth() const { return mWidth; }
    UINT GetHeight() const { return mHeight; }
    UINT GetSampleCount() const { return kMsaaCount; }
    float GetAspectRatio() const
    {
        return (mHeight > 0) ? static_cast<float>(mWidth) / static_cast<float>(mHeight) : 1.0f;
    }

    // Resolved (non-MSAA) color for ImGui
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpu() const { return mSrv.GPU; }

    // MSAA depth for Hi-Z (Texture2DMS). Valid after End() → NON_PIXEL_SHADER_RESOURCE.
    ID3D12Resource* GetDepthResource() const { return mDepthMsaa.Get(); }
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

    // MSAA targets (render)
    Microsoft::WRL::ComPtr<ID3D12Resource> mColorMsaa;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDepthMsaa;
    // Resolved color (ImGui / copy)
    Microsoft::WRL::ComPtr<ID3D12Resource> mColorResolved;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    DescriptorAllocator::DescriptorHandle mSrv{};
    DescriptorAllocator::DescriptorHandle mDepthSrv{};

    UINT mWidth = 0;
    UINT mHeight = 0;
    D3D12_RESOURCE_STATES mColorMsaaState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mDepthMsaaState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES mColorResolvedState = D3D12_RESOURCE_STATE_COMMON;
};
