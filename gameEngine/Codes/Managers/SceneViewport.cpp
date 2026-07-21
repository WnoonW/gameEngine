#include "SceneViewport.h"
#include "d3dUtil.h"

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
    mWidth = 0;
    mHeight = 0;
}

bool SceneViewport::Resize(UINT width, UINT height)
{
    width = (width == 0) ? 1 : width;
    height = (height == 0) ? 1 : height;

    if (mColor && mWidth == width && mHeight == height)
        return false;

    DestroySizeDependentResources();
    CreateSizeDependentResources(width, height);
    return true;
}

void SceneViewport::DestroySizeDependentResources()
{
    mColor.Reset();
    mDepth.Reset();
    mWidth = 0;
    mHeight = 0;
    mColorState = D3D12_RESOURCE_STATE_COMMON;
    mDepthState = D3D12_RESOURCE_STATE_COMMON;
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

    // ---- Color (RT + SRV) ----
    D3D12_RESOURCE_DESC colorDesc{};
    colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colorDesc.Alignment = 0;
    colorDesc.Width = width;
    colorDesc.Height = height;
    colorDesc.DepthOrArraySize = 1;
    colorDesc.MipLevels = 1;
    colorDesc.Format = mColorFormat;
    colorDesc.SampleDesc.Count = 1;
    colorDesc.SampleDesc.Quality = 0;
    colorDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE colorClear{};
    colorClear.Format = mColorFormat;
    colorClear.Color[0] = 0.690196097f;
    colorClear.Color[1] = 0.768627524f;
    colorClear.Color[2] = 0.870588303f;
    colorClear.Color[3] = 1.0f;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &colorDesc,
        D3D12_RESOURCE_STATE_COMMON,
        &colorClear,
        IID_PPV_ARGS(&mColor)));
    mColorState = D3D12_RESOURCE_STATE_COMMON;

    mDevice->CreateRenderTargetView(
        mColor.Get(),
        nullptr,
        mRtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = mColorFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    mDevice->CreateShaderResourceView(mColor.Get(), &srvDesc, mSrv.CPU);

    // ---- Depth (typeless: DSV + depth SRV for Hi-Z) ----
    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Alignment = 0;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = mDepthFormat; // D24_UNORM_S8_UINT
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_COMMON,
        &depthClear,
        IID_PPV_ARGS(&mDepth)));
    mDepthState = D3D12_RESOURCE_STATE_COMMON;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Format = mDepthFormat;
    dsvDesc.Texture2D.MipSlice = 0;
    mDevice->CreateDepthStencilView(
        mDepth.Get(),
        &dsvDesc,
        mDsvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
    depthSrv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrv.Texture2D.MostDetailedMip = 0;
    depthSrv.Texture2D.MipLevels = 1;
    depthSrv.Texture2D.PlaneSlice = 0;
    depthSrv.Texture2D.ResourceMinLODClamp = 0.0f;
    mDevice->CreateShaderResourceView(mDepth.Get(), &depthSrv, mDepthSrv.CPU);
}

void SceneViewport::Begin(ID3D12GraphicsCommandList* cmdList, const float clearColor[4])
{
    assert(cmdList);
    assert(IsValid());

    if (mColorState != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mColor.Get(),
            mColorState,
            D3D12_RESOURCE_STATE_RENDER_TARGET));
        mColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    if (mDepthState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mDepth.Get(),
            mDepthState,
            D3D12_RESOURCE_STATE_DEPTH_WRITE));
        mDepthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = mDsvHeap->GetCPUDescriptorHandleForHeapStart();

    cmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);
    cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
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

    if (mColorState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mColor.Get(),
            mColorState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        mColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // Depth → CS/PS readable for Hi-Z build (next cull uses previous Hi-Z)
    if (mDepthState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            mDepth.Get(),
            mDepthState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        mDepthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
}
