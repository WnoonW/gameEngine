#include "SceneViewport.h"
#include "d3dUtil.h"
#include "d3dx12.h"
#include <cstring>

void SceneViewport::Initialize(
    ID3D12Device* device,
    DescriptorAllocator& srvAllocator,
    DXGI_FORMAT colorFormat,
    DXGI_FORMAT depthFormat)
{
    mDevice = device;
    mSrvAllocator = &srvAllocator;
    mColorFormat = colorFormat;
    mDepthFormat = depthFormat;

    if (mSrv.Index == UINT_MAX)
        mSrv = mSrvAllocator->Allocate();
    if (mDepthSrv.Index == UINT_MAX)
        mDepthSrv = mSrvAllocator->Allocate();

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mRtvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&mDsvHeap)));
}

void SceneViewport::Shutdown()
{
    DestroySizeDependentResources();

    if (mSrvAllocator)
    {
        if (mSrv.Index != UINT_MAX)
        {
            mSrvAllocator->Free(mSrv);
            mSrv = {};
        }
        if (mDepthSrv.Index != UINT_MAX)
        {
            mSrvAllocator->Free(mDepthSrv);
            mDepthSrv = {};
        }
    }

    mRtvHeap.Reset();
    mDsvHeap.Reset();
    mDevice = nullptr;
    mSrvAllocator = nullptr;
}

bool SceneViewport::Resize(UINT width, UINT height)
{
    width = (width == 0) ? 1 : width;
    height = (height == 0) ? 1 : height;

    if (mColorResolved && mWidth == width && mHeight == height)
        return false;

    DestroySizeDependentResources();
    CreateSizeDependentResources(width, height);
    return true;
}

void SceneViewport::DestroySizeDependentResources()
{
    mColorMsaa.Reset();
    mDepthMsaa.Reset();
    mColorResolved.Reset();
    mWidth = 0;
    mHeight = 0;
    mColorMsaaState = D3D12_RESOURCE_STATE_COMMON;
    mDepthMsaaState = D3D12_RESOURCE_STATE_COMMON;
    mColorResolvedState = D3D12_RESOURCE_STATE_COMMON;
}

void SceneViewport::CreateSizeDependentResources(UINT width, UINT height)
{
    assert(mDevice);
    assert(mRtvHeap);
    assert(mDsvHeap);
    assert(mSrv.Index != UINT_MAX);
    assert(mDepthSrv.Index != UINT_MAX);

    mWidth = width;
    mHeight = height;

    const float clearRgb[4] = {
        0.690196097f, 0.768627524f, 0.870588303f, 1.0f
    };

    // ---- MSAA color (render target) ----
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = mColorFormat;
        desc.SampleDesc.Count = kMsaaCount;
        desc.SampleDesc.Quality = 0;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = mColorFormat;
        memcpy(clear.Color, clearRgb, sizeof(clearRgb));

        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            &clear,
            IID_PPV_ARGS(&mColorMsaa)));
        mColorMsaaState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = mColorFormat;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        mDevice->CreateRenderTargetView(
            mColorMsaa.Get(), &rtv, mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // ---- Resolved color (ImGui SRV / copy source) ----
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = mColorFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; // not required for resolve dest but OK

        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&mColorResolved)));
        mColorResolvedState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = mColorFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        mDevice->CreateShaderResourceView(mColorResolved.Get(), &srvDesc, mSrv.CPU);
    }

    // ---- MSAA depth (DSV + Texture2DMS SRV for Hi-Z sample 0) ----
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        desc.SampleDesc.Count = kMsaaCount;
        desc.SampleDesc.Quality = 0;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = mDepthFormat;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;

        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            &clear,
            IID_PPV_ARGS(&mDepthMsaa)));
        mDepthMsaaState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
        dsvDesc.Format = mDepthFormat;
        mDevice->CreateDepthStencilView(
            mDepthMsaa.Get(), &dsvDesc, mDsvHeap->GetCPUDescriptorHandleForHeapStart());

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
        depthSrv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        mDevice->CreateShaderResourceView(mDepthMsaa.Get(), &depthSrv, mDepthSrv.CPU);
    }
}

void SceneViewport::Begin(ID3D12GraphicsCommandList* cmdList, const float clearColor[4])
{
    assert(cmdList);
    assert(IsValid());

    if (mColorMsaaState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mColorMsaa.Get(), mColorMsaaState, D3D12_RESOURCE_STATE_RENDER_TARGET));
        mColorMsaaState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (mDepthMsaaState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mDepthMsaa.Get(), mDepthMsaaState, D3D12_RESOURCE_STATE_DEPTH_WRITE));
        mDepthMsaaState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = mDsvHeap->GetCPUDescriptorHandleForHeapStart();

    cmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);
    cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(mWidth);
    vp.Height = static_cast<float>(mHeight);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    cmdList->RSSetViewports(1, &vp);

    D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    cmdList->RSSetScissorRects(1, &scissor);
}

void SceneViewport::End(ID3D12GraphicsCommandList* cmdList)
{
    assert(cmdList);
    assert(IsValid());

    // Resolve MSAA color → non-MSAA for ImGui / CopyColorTo
    if (mColorMsaaState != D3D12_RESOURCE_STATE_RESOLVE_SOURCE)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mColorMsaa.Get(), mColorMsaaState, D3D12_RESOURCE_STATE_RESOLVE_SOURCE));
        mColorMsaaState = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    }
    if (mColorResolvedState != D3D12_RESOURCE_STATE_RESOLVE_DEST)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mColorResolved.Get(), mColorResolvedState, D3D12_RESOURCE_STATE_RESOLVE_DEST));
        mColorResolvedState = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    }

    cmdList->ResolveSubresource(
        mColorResolved.Get(), 0,
        mColorMsaa.Get(), 0,
        mColorFormat);

    // Resolved color → PS for ImGui
    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mColorResolved.Get(),
        D3D12_RESOURCE_STATE_RESOLVE_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    mColorResolvedState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // Depth MSAA → CS readable for Hi-Z (Texture2DMS)
    if (mDepthMsaaState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mDepthMsaa.Get(),
            mDepthMsaaState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        mDepthMsaaState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
}

void SceneViewport::CopyColorTo(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* dest,
    D3D12_RESOURCE_STATES destStateBefore)
{
    assert(cmdList);
    assert(dest);
    assert(IsValid());

    // End() leaves resolved color in PIXEL_SHADER_RESOURCE
    if (mColorResolvedState != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mColorResolved.Get(),
            mColorResolvedState,
            D3D12_RESOURCE_STATE_COPY_SOURCE));
        mColorResolvedState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }

    if (destStateBefore != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            dest, destStateBefore, D3D12_RESOURCE_STATE_COPY_DEST));
    }

    cmdList->CopyResource(dest, mColorResolved.Get());
}
