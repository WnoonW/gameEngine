#include "UiSystem.h"
#include "ComponentStruct.h"
#include "MaterialManager.h"
#include "ShaderManager.h"
#include "d3dUtil.h"
#include "d3dx12.h"
#include <algorithm>
#include <cstring>

using namespace DirectX;
using namespace ECS;

void UiSystem::Initialize(ID3D12Device* device)
{
    mDevice = device;
    if (!mDevice)
        return;
    EnsurePipeline(mDevice);
}

void UiSystem::Shutdown()
{
    if (mUpload && mUploadMapped)
    {
        mUpload->Unmap(0, nullptr);
        mUploadMapped = nullptr;
    }
    mUpload.Reset();
    mPsoAlpha.Reset();
    mPsoAdditive.Reset();
    mRootSig.Reset();
    mDevice = nullptr;
}

void UiSystem::EnsurePipeline(ID3D12Device* device)
{
    // Must have PSO; root sig alone is not enough (shader fail used to stick forever).
    if (mRootSig && mPsoAlpha && mPsoAdditive)
        return;

    if (!mRootSig)
    {
        // b0 CB | table t0 texture | static sampler s0
        CD3DX12_DESCRIPTOR_RANGE srv;
        srv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER params[2];
        params[0].InitAsConstants(4, 0);
        params[1].InitAsDescriptorTable(1, &srv, D3D12_SHADER_VISIBILITY_PIXEL);

        CD3DX12_STATIC_SAMPLER_DESC samp(
            0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

        CD3DX12_ROOT_SIGNATURE_DESC rsDesc(
            2, params, 1, &samp,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized, err;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &err));
        ThrowIfFailed(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&mRootSig)));
    }

    auto vs = ShaderManager::Get().GetVertexShader(L"Resources\\Shaders\\ui_quad.hlsl");
    auto ps = ShaderManager::Get().GetPixelShader(L"Resources\\Shaders\\ui_quad.hlsl");
    if (!vs || !ps)
    {
        OutputDebugStringA("[UiSystem] ui_quad.hlsl compile failed (check CWD/Resources/Shaders)\n");
        return;
    }

    // Recreate PSOs if missing
    mPsoAlpha.Reset();
    mPsoAdditive.Reset();

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = mRootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    // Standard alpha blend
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.InputLayout = { layout, _countof(layout) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = mSampleCount;
    pso.SampleDesc.Quality = 0;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mPsoAlpha)));

    // Additive for VFX
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mPsoAdditive)));
}

void UiSystem::EnsureUploadBuffer(ID3D12Device* device, UINT64 bytes)
{
    if (mUpload && mUploadBytes >= bytes)
        return;
    if (mUpload && mUploadMapped)
    {
        mUpload->Unmap(0, nullptr);
        mUploadMapped = nullptr;
    }
    mUpload.Reset();
    mUploadBytes = (std::max)(bytes, (UINT64)65536);
    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(mUploadBytes),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mUpload)));
    ThrowIfFailed(mUpload->Map(0, nullptr, reinterpret_cast<void**>(&mUploadMapped)));
}

void UiSystem::ResolveScreenLayout(
    const UiElementComponent& el,
    float screenW, float screenH,
    UiScaleMode mode,
    float globalDesignW, float globalDesignH,
    XMFLOAT2& outPosPx,
    XMFLOAT2& outSizePx,
    float* outScaleX,
    float* outScaleY)
{
    // --- Percent layout: position = center 0..1, size = extent 0..1 of LIVE canvas ---
    // Edit: canvas = Scene dock image / RT. Play: canvas = window client.
    // Every frame remaps to current screenW/H so resize updates immediately.
    if (el.layoutPercent)
    {
        if (mode == UiScaleMode::Stretch || mode == UiScaleMode::UniformMin
            || mode == UiScaleMode::UniformMax)
        {
            // Stretch (default): pure percent of current canvas — no lag, no design lock.
            // Uniform*: still percent of current canvas (center+size scale together).
            // (Uniform letterboxing would need a virtual canvas; keep full-canvas % for sync.)
            outPosPx = { el.position.x * screenW, el.position.y * screenH };
            outSizePx = { el.size.x * screenW, el.size.y * screenH };
            if (outScaleX) *outScaleX = screenW;
            if (outScaleY) *outScaleY = screenH;
            return;
        }
    }

    // --- Legacy: design-space pixels ---
    float refW = (el.designW > 1.0f) ? el.designW
        : ((globalDesignW > 1.0f) ? globalDesignW : screenW);
    float refH = (el.designH > 1.0f) ? el.designH
        : ((globalDesignH > 1.0f) ? globalDesignH : screenH);
    if (refW < 1.0f) refW = screenW;
    if (refH < 1.0f) refH = screenH;

    float sx = screenW / refW;
    float sy = screenH / refH;
    switch (mode)
    {
    case UiScaleMode::UniformMin:
    {
        const float s = (std::min)(sx, sy);
        sx = s;
        sy = s;
        break;
    }
    case UiScaleMode::UniformMax:
    {
        const float s = (std::max)(sx, sy);
        sx = s;
        sy = s;
        break;
    }
    case UiScaleMode::Stretch:
    default:
        break;
    }

    outPosPx = { el.position.x * sx, el.position.y * sy };
    outSizePx = { el.size.x * sx, el.size.y * sy };
    if (outScaleX) *outScaleX = sx;
    if (outScaleY) *outScaleY = sy;
}

void UiSystem::EmitScreenQuad(
    std::vector<DrawItem>& out,
    const XMFLOAT2& anchor,
    const XMFLOAT2& pivot,
    const XMFLOAT2& posPx,
    const XMFLOAT2& sizePx,
    float rotRad,
    const XMFLOAT4& color,
    const XMFLOAT4& uv,
    UINT materialIndex,
    int zOrder,
    float screenW,
    float screenH)
{
    if (screenW < 1.f || screenH < 1.f || sizePx.x <= 0.f || sizePx.y <= 0.f)
        return;

    // Anchor point in pixels (y down for layout; flip for NDC later)
    const float ax = anchor.x * screenW + posPx.x;
    const float ay = anchor.y * screenH + posPx.y;

    // Local corners relative to pivot (pixel space, y down)
    const float l = -pivot.x * sizePx.x;
    const float r = (1.f - pivot.x) * sizePx.x;
    const float t = -pivot.y * sizePx.y;
    const float b = (1.f - pivot.y) * sizePx.y;

    const float c = cosf(rotRad);
    const float s = sinf(rotRad);
    auto rot = [&](float x, float y, float& ox, float& oy)
    {
        ox = ax + x * c - y * s;
        oy = ay + x * s + y * c;
    };

    float x0, y0, x1, y1, x2, y2, x3, y3;
    rot(l, t, x0, y0);
    rot(r, t, x1, y1);
    rot(r, b, x2, y2);
    rot(l, b, x3, y3);

    auto toNdc = [&](float px, float py, float& nx, float& ny)
    {
        nx = (px / screenW) * 2.f - 1.f;
        ny = 1.f - (py / screenH) * 2.f; // y-down pixels → y-up NDC
    };

    float n0x, n0y, n1x, n1y, n2x, n2y, n3x, n3y;
    toNdc(x0, y0, n0x, n0y);
    toNdc(x1, y1, n1x, n1y);
    toNdc(x2, y2, n2x, n2y);
    toNdc(x3, y3, n3x, n3y);

    const float u0 = uv.x, v0 = uv.y, u1 = uv.z, v1 = uv.w;
    DrawItem item{};
    item.materialIndex = materialIndex;
    item.zOrder = zOrder;
    item.additive = false;
    auto setV = [&](int i, float x, float y, float u, float v)
    {
        item.v[i] = { x, y, u, v, color.x, color.y, color.z, color.w };
    };
    // two triangles: 0-1-2, 0-2-3
    setV(0, n0x, n0y, u0, v0);
    setV(1, n1x, n1y, u1, v0);
    setV(2, n2x, n2y, u1, v1);
    setV(3, n0x, n0y, u0, v0);
    setV(4, n2x, n2y, u1, v1);
    setV(5, n3x, n3y, u0, v1);
    out.push_back(item);
}

void UiSystem::EmitWorldBillboard(
    std::vector<DrawItem>& out,
    const XMFLOAT3& worldPos,
    float sizeX,
    float sizeY,
    const XMFLOAT4& color,
    const XMFLOAT4& uv,
    UINT materialIndex,
    int zOrder,
    bool additive,
    const XMMATRIX& view,
    const XMMATRIX& proj)
{
    // Camera-facing: use view-space axes (right/up from inverse view rows)
    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    XMVECTOR right = invView.r[0];
    XMVECTOR up = invView.r[1];
    right = XMVector3Normalize(right);
    up = XMVector3Normalize(up);

    XMVECTOR center = XMLoadFloat3(&worldPos);
    XMVECTOR hx = XMVectorScale(right, sizeX * 0.5f);
    XMVECTOR hy = XMVectorScale(up, sizeY * 0.5f);

    XMVECTOR p0 = center - hx + hy; // TL
    XMVECTOR p1 = center + hx + hy; // TR
    XMVECTOR p2 = center + hx - hy; // BR
    XMVECTOR p3 = center - hx - hy; // BL

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    auto toClip = [&](FXMVECTOR p, float& x, float& y)
    {
        XMVECTOR c = XMVector3TransformCoord(p, viewProj);
        XMFLOAT3 f;
        XMStoreFloat3(&f, c);
        x = f.x;
        y = f.y;
    };

    float x0, y0, x1, y1, x2, y2, x3, y3;
    toClip(p0, x0, y0);
    toClip(p1, x1, y1);
    toClip(p2, x2, y2);
    toClip(p3, x3, y3);

    const float u0 = uv.x, v0 = uv.y, u1 = uv.z, v1 = uv.w;
    DrawItem item{};
    item.materialIndex = materialIndex;
    item.zOrder = zOrder;
    item.additive = additive;
    auto setV = [&](int i, float x, float y, float u, float v)
    {
        item.v[i] = { x, y, u, v, color.x, color.y, color.z, color.w };
    };
    setV(0, x0, y0, u0, v0);
    setV(1, x1, y1, u1, v0);
    setV(2, x2, y2, u1, v1);
    setV(3, x0, y0, u0, v0);
    setV(4, x2, y2, u1, v1);
    setV(5, x3, y3, u0, v1);
    out.push_back(item);
}

void UiSystem::Render(
    World& world,
    ID3D12GraphicsCommandList* cmdList,
    DescriptorAllocator* descriptorAllocator,
    UINT screenWidth,
    UINT screenHeight,
    const XMMATRIX& view,
    const XMMATRIX& proj,
    const XMFLOAT3& /*eyePosW*/)
{
    if (!cmdList || !descriptorAllocator || !mDevice)
        return;
    if (!mRootSig || !mPsoAlpha)
        EnsurePipeline(mDevice);
    if (!mRootSig || !mPsoAlpha)
        return;

    const float sw = static_cast<float>((std::max)(1u, screenWidth));
    const float sh = static_cast<float>((std::max)(1u, screenHeight));

    std::vector<DrawItem> items;
    items.reserve(64);

    world.ForEach<UiElementComponent, UiImageComponent>(
        [&](Entity, UiElementComponent& el, UiImageComponent& img)
        {
            if (!el.visible)
                return;
            if (el.mode == UiSpaceMode::ScreenConditional && !el.active)
                return;

            // Prefer named material; fall back to Default / Missing so play export always draws.
            Material* mat = MaterialManager::Get().GetMaterial(img.materialName).get();
            if (!mat || !mat->HasValidTexture())
                mat = MaterialManager::Get().GetDefaultMaterial().get();
            if (!mat || !mat->HasValidTexture())
                mat = MaterialManager::Get().GetMissingTextureMaterial();
            if (!mat || !mat->HasValidTexture())
                return;
            const UINT mi = mat->mTextureHandle.Index;

            if (el.mode == UiSpaceMode::WorldBillboard)
            {
                // Need transform — look up via second pass is awkward in ForEach without Transform
                // Skip here; handled below with Transform
                return;
            }

            XMFLOAT2 posPx{}, sizePx{};
            ResolveScreenLayout(el, sw, sh, mScaleMode, mGlobalDesignW, mGlobalDesignH,
                posPx, sizePx);
            EmitScreenQuad(items, el.anchor, el.pivot, posPx, sizePx,
                el.rotationRad, img.color, img.uvRect, mi, el.zOrder, sw, sh);
        });

    world.ForEach<TransformComponent, UiElementComponent, UiImageComponent>(
        [&](Entity, TransformComponent& tf, UiElementComponent& el, UiImageComponent& img)
        {
            if (!el.visible || el.mode != UiSpaceMode::WorldBillboard)
                return;
            if (!el.active && el.mode == UiSpaceMode::ScreenConditional)
                return;

            Material* mat = MaterialManager::Get().GetMaterial(img.materialName).get();
            if (!mat || !mat->HasValidTexture())
                mat = MaterialManager::Get().GetDefaultMaterial().get();
            if (!mat || !mat->HasValidTexture())
                mat = MaterialManager::Get().GetMissingTextureMaterial();
            if (!mat || !mat->HasValidTexture())
                return;

            EmitWorldBillboard(items, tf.position, el.size.x, el.size.y,
                img.color, img.uvRect, mat->mTextureHandle.Index, el.zOrder, false, view, proj);
        });

    // VFX billboards
    world.ForEach<TransformComponent, EffectBillboardComponent>(
        [&](Entity, TransformComponent& tf, EffectBillboardComponent& fx)
        {
            if (!fx.visible)
                return;
            auto mat = MaterialManager::Get().GetMaterial(fx.materialName);
            if (!mat || !mat->HasValidTexture())
                return;
            XMFLOAT4 uv{ 0, 0, 1, 1 };
            EmitWorldBillboard(items, tf.position, fx.size, fx.size,
                fx.color, uv, mat->mTextureHandle.Index, 1000, fx.additive, view, proj);
        });

    if (items.empty())
    {
        static bool sLoggedEmpty = false;
        if (!sLoggedEmpty)
        {
            OutputDebugStringA("[UiSystem] Render: no draw items (no visible UI / materials / PSO)\n");
            sLoggedEmpty = true;
        }
        return;
    }

    std::stable_sort(items.begin(), items.end(),
        [](const DrawItem& a, const DrawItem& b) { return a.zOrder < b.zOrder; });

    const UINT64 bytes = items.size() * sizeof(UiVertex) * 6;
    EnsureUploadBuffer(mDevice, bytes);
    UiVertex* dst = reinterpret_cast<UiVertex*>(mUploadMapped);
    for (size_t i = 0; i < items.size(); ++i)
        std::memcpy(dst + i * 6, items[i].v, sizeof(UiVertex) * 6);

    ID3D12DescriptorHeap* heaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetGraphicsRootSignature(mRootSig.Get());

    float constants[4] = { sw, sh, 0.f, 0.f };
    cmdList->SetGraphicsRoot32BitConstants(0, 4, constants, 0);

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = mUpload->GetGPUVirtualAddress();
    vbv.StrideInBytes = sizeof(UiVertex);
    vbv.SizeInBytes = static_cast<UINT>(bytes);
    cmdList->IASetVertexBuffers(0, 1, &vbv);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Draw groups by blend + material to cut state changes a bit
    for (size_t i = 0; i < items.size(); ++i)
    {
        const DrawItem& it = items[i];
        cmdList->SetPipelineState(it.additive ? mPsoAdditive.Get() : mPsoAlpha.Get());

        D3D12_GPU_DESCRIPTOR_HANDLE h =
            descriptorAllocator->GetHeap()->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(it.materialIndex) * descriptorAllocator->GetDescriptorSize();
        cmdList->SetGraphicsRootDescriptorTable(1, h);

        cmdList->DrawInstanced(6, 1, static_cast<UINT>(i * 6), 0);
    }
}

bool UiSystem::HandlePointer(
    World& world,
    float scenePixelX,
    float scenePixelY,
    UINT screenWidth,
    UINT screenHeight,
    bool leftDown,
    bool leftPressedThisFrame,
    std::vector<UiClickEvent>* outClicks)
{
    if (screenWidth == 0 || screenHeight == 0)
        return false;

    struct Hit
    {
        Entity e;
        int z;
        UiButtonComponent* btn;
        UiElementComponent* el;
        float l, t, r, b;
    };
    std::vector<Hit> hits;

    const float sw = static_cast<float>(screenWidth);
    const float sh = static_cast<float>(screenHeight);

    world.ForEach<UiElementComponent, UiImageComponent, UiButtonComponent>(
        [&](Entity e, UiElementComponent& el, UiImageComponent&, UiButtonComponent& btn)
        {
            if (!el.visible || !btn.interactable)
                return;
            if (el.mode == UiSpaceMode::ScreenConditional && !el.active)
                return;
            if (el.mode == UiSpaceMode::WorldBillboard)
                return; // 2D pick only for screen UI in MVP

            XMFLOAT2 posPx{}, sizePx{};
            ResolveScreenLayout(el, sw, sh, mScaleMode, mGlobalDesignW, mGlobalDesignH,
                posPx, sizePx);

            const float ax = el.anchor.x * sw + posPx.x;
            const float ay = el.anchor.y * sh + posPx.y;
            const float l = ax - el.pivot.x * sizePx.x;
            const float t = ay - el.pivot.y * sizePx.y;
            const float r = l + sizePx.x;
            const float b = t + sizePx.y;
            hits.push_back({ e, el.zOrder, &btn, &el, l, t, r, b });
        });

    std::sort(hits.begin(), hits.end(),
        [](const Hit& a, const Hit& b) { return a.z > b.z; });

    bool consumed = false;
    for (Hit& h : hits)
    {
        const bool inside =
            scenePixelX >= h.l && scenePixelX <= h.r &&
            scenePixelY >= h.t && scenePixelY <= h.b;
        h.btn->hovered = inside;
        if (!inside)
        {
            h.btn->pressed = false;
            continue;
        }
        consumed = true;
        if (leftPressedThisFrame)
            h.btn->pressed = true;
        if (!leftDown && h.btn->pressed)
        {
            h.btn->pressed = false;
            if (outClicks)
            {
                UiClickEvent ev;
                ev.entity = h.e;
                ev.actionId = h.btn->actionId;
                outClicks->push_back(std::move(ev));
            }
        }
        break; // top-most only
    }
    return consumed;
}
