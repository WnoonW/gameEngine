#include <DirectXMath.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <vector>
#include <unordered_set>
#include <cstring>
#include "RenderSystem.h"
#include "TransformDirtyTracker.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "ShaderManager.h"
#include "constantStruct.h"
#include "d3dUtil.h"
#include "d3dx12.h"

using namespace DirectX;

namespace
{
    float ElapsedMs(std::chrono::high_resolution_clock::time_point t0)
    {
        using namespace std::chrono;
        return duration<float, std::milli>(high_resolution_clock::now() - t0).count();
    }
}

namespace
{
    void StoreWorldTransposed(const XMMATRIX& world, float out[16])
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixTranspose(world));
        std::memcpy(out, &m, sizeof(XMFLOAT4X4));
    }

    // Mirrors compose_world.hlsl (column_major store of W == StoreWorldTransposed)
    void GpuStyleComposeStore(float px, float py, float pz,
        float pitch, float yaw, float roll,
        float sx, float sy, float sz,
        float out[16])
    {
        const float cp = cosf(pitch), sp = sinf(pitch);
        const float cy = cosf(yaw),   syw = sinf(yaw);
        const float cr = cosf(roll),  sr = sinf(roll);

        // R elements (DirectXMath XMMatrixRotationRollPitchYaw)
        const float r11 = cr * cy + sr * sp * syw;
        const float r12 = sr * cp;
        const float r13 = sr * sp * cy - cr * syw;
        const float r21 = cr * sp * syw - sr * cy;
        const float r22 = cr * cp;
        const float r23 = sr * syw + cr * sp * cy;
        const float r31 = cp * syw;
        const float r32 = -sp;
        const float r33 = cp * cy;

        // W = S * R * T  (scale axes of R, translation in last row for row-vector)
        // S*R:
        const float sr11 = sx * r11, sr12 = sx * r12, sr13 = sx * r13;
        const float sr21 = sy * r21, sr22 = sy * r22, sr23 = sy * r23;
        const float sr31 = sz * r31, sr32 = sz * r32, sr33 = sz * r33;
        // (S*R)*T — translation only affects row3: t * (S*R) added... 
        // Row-vector: T has translation in row3; (S*R)*T keeps upper 3x3, row3 = t * upper + e4
        // DirectXMath multiply A*B: row i of result = row i of A dotted with columns of B.
        // For A=S*R (no translation) and B=T (identity + translation in row3):
        // result rows 0-2 = rows 0-2 of S*R
        // result row3 = (px,py,pz,1) transformed... actually row3 of T is (tx,ty,tz,1),
        // result.r3 = A.r0*tx + A.r1*ty + A.r2*tz + A.r3 = tx*row0 + ty*row1 + tz*row2 + row3
        // With A.r3 = (0,0,0,1): r3 = (tx*sr11 + ty*sr21 + tz*sr31, ... , 1)  NO that's wrong for DXMath
        //
        // DXMath matrices are row-major; XMMatrixTranslation puts tx,ty,tz in _41,_42,_43 (row3).
        // XMMatrixMultiply(A,B): for each row i: result.r[i] = A.r[i] * B (vector-matrix).
        // So (S*R)*T: rows 0-2 of S*R multiplied by T → same rows 0-2 (T only changes via w component)
        // Since rows 0-2 have w=0: unchanged 3x3.
        // row3 of S*R is (0,0,0,1); (0,0,0,1)*T = row3 of T = (tx,ty,tz,1).
        // So W upper 3x3 = S*R, translation = (px,py,pz). Correct.

        XMFLOAT4X4 W{};
        W._11 = sr11; W._12 = sr12; W._13 = sr13; W._14 = 0.f;
        W._21 = sr21; W._22 = sr22; W._23 = sr23; W._24 = 0.f;
        W._31 = sr31; W._32 = sr32; W._33 = sr33; W._34 = 0.f;
        W._41 = px;   W._42 = py;   W._43 = pz;   W._44 = 1.f;

        // column_major store of W == rows of W^T == StoreWorldTransposed
        XMFLOAT4X4 stored;
        XMStoreFloat4x4(&stored, XMMatrixTranspose(XMLoadFloat4x4(&W)));
        std::memcpy(out, &stored, sizeof(stored));
    }

    void ValidateComposeWorldMatchesCpu()
    {
        const float samples[][9] = {
            { 0,0,0, 0,0,0, 1,1,1 },
            { 1,2,3, 0.1f, 0.2f, 0.3f, 2, 0.5f, 1.5f },
            { -4, 0.5f, 8, XM_PIDIV2, -0.7f, 1.1f, 0.25f, 3.f, 0.8f },
        };
        for (const auto& s : samples)
        {
            TransformComponent tf{};
            tf.position = { s[0], s[1], s[2] };
            tf.rotation = { s[3], s[4], s[5] };
            tf.scale = { s[6], s[7], s[8] };

            float cpu[16]{}, gpuStyle[16]{};
            StoreWorldTransposed(tf.GetWorldMatrix(), cpu);
            GpuStyleComposeStore(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], gpuStyle);

            for (int i = 0; i < 16; ++i)
            {
                if (fabsf(cpu[i] - gpuStyle[i]) > 1e-5f)
                {
                    char buf[192];
                    sprintf_s(buf,
                        "[RenderSystem] ComposeWorld CPU/GPU mismatch at [%d]: cpu=%f gpuStyle=%f\n",
                        i, cpu[i], gpuStyle[i]);
                    OutputDebugStringA(buf);
                    return;
                }
            }
        }
        OutputDebugStringA("[RenderSystem] ComposeWorld matrix layout validated (CPU match).\n");
    }

    Material* ResolveSubmeshMaterial(const SubmeshGeometry& sub)
    {
        Material* material = sub.initMaterial;
        if (!material || !material->HasValidTexture())
        {
            if (!sub.initMaterialName.empty())
            {
                if (auto m = MaterialManager::Get().GetMaterial(sub.initMaterialName))
                    material = m->HasValidTexture() ? m.get() : nullptr;
            }
            if (!material)
                material = MaterialManager::Get().GetMissingTextureMaterial();
        }
        if (!material || !material->HasValidTexture())
            return nullptr;
        return material;
    }

    bool HasEntitySubMaterialOverride(Entity e)
    {
        const EntityMaterialData* data = MaterialManager::Get().GetEntityMaterialData(e);
        if (!data)
            return false;
        for (const auto& p : data->subMaterialNames)
        {
            if (!p.second.empty())
                return true;
        }
        return false;
    }

    Material* GetEntityMainMaterialOverride(Entity e)
    {
        const EntityMaterialData* data = MaterialManager::Get().GetEntityMaterialData(e);
        if (!data || data->mainMaterialName.empty())
            return nullptr;
        auto mat = MaterialManager::Get().GetMaterial(data->mainMaterialName);
        if (!mat || !mat->HasValidTexture())
            return nullptr;
        return mat.get();
    }

    Material* ResolveBatchMaterial(Material* mainOverride, const SubmeshGeometry& sub)
    {
        if (mainOverride && mainOverride->HasValidTexture())
            return mainOverride;
        return ResolveSubmeshMaterial(sub);
    }

    void DrawEntityWithResolvedMaterials(
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* frame,
        Entity e,
        TransformComponent& tf,
        RenderableComponent& rend)
    {
        if (!rend.visible || !rend.mesh)
            return;
        if (rend.objectCBIndex >= kMaxSceneObjects)
            return;

        if (tf.dirtyFrames > 0)
        {
            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf.GetWorldMatrix()));
            frame->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);
            --tf.dirtyFrames;
        }

        auto objectCB = frame->ObjectCB->Resource();
        const UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        const D3D12_GPU_VIRTUAL_ADDRESS objCBAddress =
            objectCB->GetGPUVirtualAddress() + (UINT64)rend.objectCBIndex * objCBByteSize;

        auto vbv = rend.mesh->VertexBufferView();
        auto ibv = rend.mesh->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (auto& pair : rend.mesh->DrawArgs)
        {
            const auto& sub = pair.second;
            Material* material = MaterialManager::Get().ResolveForDraw(e, pair.first, sub.initMaterial);
            if (!material || !material->HasValidTexture())
                continue;

            cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
            // Step E: heap Index → GPU handle (table of 1)
            if (material->mTextureHandle.Index != UINT_MAX)
            {
                cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);
            }
            cmdList->DrawIndexedInstanced(
                sub.IndexCount, 1,
                sub.StartIndexLocation,
                sub.BaseVertexLocation, 0);
        }
    }

    // Step E: bind SRV table slot by descriptor heap index (skip if same as last)
    bool BindMaterialByIndex(
        ID3D12GraphicsCommandList* cmdList,
        DescriptorAllocator* descriptorAllocator,
        UINT materialIndex,
        UINT& inoutLastIndex)
    {
        if (!descriptorAllocator || materialIndex == UINT_MAX)
            return false;
        if (materialIndex == inoutLastIndex)
            return true;

        D3D12_GPU_DESCRIPTOR_HANDLE h =
            descriptorAllocator->GetHeap()->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(materialIndex) * descriptorAllocator->GetDescriptorSize();
        cmdList->SetGraphicsRootDescriptorTable(2, h);
        inoutLastIndex = materialIndex;
        return true;
    }

    void FillMotionFromEntity(GpuMotion& out, GravityComponent* gravity)
    {
        out = {};
        if (!gravity || !gravity->enabled)
            return;
        out.linearVelocity[0] = gravity->velocity.x;
        out.linearVelocity[1] = gravity->velocity.y;
        out.linearVelocity[2] = gravity->velocity.z;
        out.angularVelocity[0] = out.angularVelocity[1] = out.angularVelocity[2] = 0.f;
        out.gravity = gravity->strength;
        out.flags = 1u;
        out.pad0 = out.pad1 = 0.f;
        out.pad2 = out.pad3 = 0;
    }

    // Step F1: TRS mirror. F2 motion integrates into GPU buffer after upload.
    void FillTransformFromEntity(
        GpuTransform& out,
        TransformComponent& tf,
        RenderableComponent& rend,
        BoundsComponent* bounds,
        UINT batchId)
    {
        out.position[0] = tf.position.x;
        out.position[1] = tf.position.y;
        out.position[2] = tf.position.z;
        out.rotation[0] = tf.rotation.x;
        out.rotation[1] = tf.rotation.y;
        out.rotation[2] = tf.rotation.z;
        out.scale[0] = tf.scale.x;
        out.scale[1] = tf.scale.y;
        out.scale[2] = tf.scale.z;
        out.pad0 = out.pad1 = out.pad2 = 0.f;
        out.batchId = batchId;
        out.flags = rend.visible ? 1u : 0u;
        out.pad5 = out.pad6 = 0;
        if (bounds)
        {
            out.boundsCenter[0] = bounds->worldBounds.Center.x;
            out.boundsCenter[1] = bounds->worldBounds.Center.y;
            out.boundsCenter[2] = bounds->worldBounds.Center.z;
            out.boundsExtents[0] = bounds->worldBounds.Extents.x;
            out.boundsExtents[1] = bounds->worldBounds.Extents.y;
            out.boundsExtents[2] = bounds->worldBounds.Extents.z;
        }
        else
        {
            out.boundsCenter[0] = out.boundsCenter[1] = out.boundsCenter[2] = 0.f;
            out.boundsExtents[0] = out.boundsExtents[1] = out.boundsExtents[2] = 0.f;
        }
        out.pad3 = out.pad4 = 0.f;
    }

    // Path3 draw source: world = GetWorldMatrix (Path2/Basic과 바이트 동일)
    void FillSourceFromEntity(
        GpuInstanceSource& out,
        TransformComponent& tf,
        RenderableComponent& rend,
        BoundsComponent* bounds,
        UINT batchId)
    {
        StoreWorldTransposed(tf.GetWorldMatrix(), out.worldMatrix);
        out.batchId = batchId;
        out.flags = rend.visible ? 1u : 0u;
        out.pad2 = 0;
        out.pad3 = 0;
        if (bounds)
        {
            out.boundsCenterX = bounds->worldBounds.Center.x;
            out.boundsCenterY = bounds->worldBounds.Center.y;
            out.boundsCenterZ = bounds->worldBounds.Center.z;
            out.boundsExtentsX = bounds->worldBounds.Extents.x;
            out.boundsExtentsY = bounds->worldBounds.Extents.y;
            out.boundsExtentsZ = bounds->worldBounds.Extents.z;
        }
        else
        {
            out.boundsCenterX = out.boundsCenterY = out.boundsCenterZ = 0.f;
            out.boundsExtentsX = out.boundsExtentsY = out.boundsExtentsZ = 0.f;
        }
        out.boundsPad0 = 0.f;
        out.boundsPad1 = 0.f;
    }
}

UINT RenderSystem::FrameCBAlignedSize()
{
    return d3dUtil::CalcConstantBufferByteSize(sizeof(GpuDrivenFrameConstants));
}

void RenderSystem::InvalidateDrawCache()
{
    mInstancedCacheValid = false;
    mGpuStructureDirty = true;
    mPendingTransformDirty.clear();
}

void RenderSystem::SetRenderPath(RenderPath path)
{
    if (path == RenderPath::ComputeIndirect && !mComputeIndirectReady)
    {
        OutputDebugStringA(
            "[RenderSystem] SetRenderPath(ComputeIndirect) failed: not ready. "
            "Path unchanged. Use Instanced/Basic instead.\n");
        return;
    }

    // 수동 지정 시 Auto 해제
    mAutoRenderPath = false;

    const char* name = "Unknown";
    switch (path)
    {
    case RenderPath::Basic: name = "Basic"; break;
    case RenderPath::Instanced: name = "Instanced"; break;
    case RenderPath::ComputeIndirect: name = "ComputeIndirect(GPU-driven)"; break;
    }
    char buf[160];
    sprintf_s(buf, "[RenderSystem] SetRenderPath -> %s (auto off)\n", name);
    OutputDebugStringA(buf);

    mRenderPath = path;
    if (path == RenderPath::ComputeIndirect)
        mGpuStructureDirty = true;
}

void RenderSystem::SetAutoRenderPathEnabled(bool enabled)
{
    mAutoRenderPath = enabled;
    char buf[96];
    sprintf_s(buf, "[RenderSystem] AutoRenderPath -> %s\n", enabled ? "ON" : "OFF");
    OutputDebugStringA(buf);
}

void RenderSystem::UpdateAutoRenderPath(World& world)
{
    if (!mAutoRenderPath)
        return;

    size_t n = 0;
    world.ForEach<RenderableComponent>(
        [&](Entity, RenderableComponent&)
        {
            ++n;
        });

    // Step G thresholds (기능 유지 + 대량 씬에서 Path3)
    //  < 32  : Instanced (오버헤드 적음, 안정)
    //  >= 32 : ComputeIndirect (가능하면)
    RenderPath chosen = RenderPath::Instanced;
    if (n >= 32 && mComputeIndirectReady)
        chosen = RenderPath::ComputeIndirect;
    else if (n > 0 && n < 4)
        chosen = RenderPath::Instanced; // Basic은 디버그용으로만 수동 사용

    if (chosen != mRenderPath)
    {
        if (chosen == RenderPath::ComputeIndirect)
            mGpuStructureDirty = true;
        mRenderPath = chosen;
        char buf[128];
        sprintf_s(buf, "[RenderSystem] Auto path -> %s (objects=%zu)\n",
            chosen == RenderPath::ComputeIndirect ? "ComputeIndirect" : "Instanced", n);
        OutputDebugStringA(buf);
    }
}

void RenderSystem::Initialize(ID3D12Device* device)
{
    mDevice = device;
    if (!mDevice)
        return;

    EnsureGpuResources(mDevice);

    ID3D12RootSignature* buildRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::IndirectBuild);
    ID3D12RootSignature* composeRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::ComposeWorld);

    auto cullBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\build_indirect_commands.hlsl", "CS_CullCompact", "cs_5_1");
    auto buildBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\build_indirect_commands.hlsl", "CS_BuildCommands", "cs_5_1");
    auto composeBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\compose_world.hlsl", "CS_ComposeWorld", "cs_5_1");
    auto motionBlob = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\update_motion.hlsl", "CS_UpdateMotion", "cs_5_1");

    if (cullBlob && buildBlob && buildRS)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = buildRS;
        psoDesc.CS = { cullBlob->GetBufferPointer(), cullBlob->GetBufferSize() };
        if (SUCCEEDED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mCullCompactPSO))))
        {
            psoDesc.CS = { buildBlob->GetBufferPointer(), buildBlob->GetBufferSize() };
            if (SUCCEEDED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mBuildCommandsPSO))))
                mComputeIndirectReady = true;
        }
    }

    if (composeBlob && composeRS)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = composeRS;
        psoDesc.CS = { composeBlob->GetBufferPointer(), composeBlob->GetBufferSize() };
        if (FAILED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mComposeWorldPSO))))
            mComposeWorldPSO.Reset();
        else
            ValidateComposeWorldMatchesCpu();
    }

    ID3D12RootSignature* motionRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::UpdateMotion);
    if (motionBlob && motionRS)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = motionRS;
        psoDesc.CS = { motionBlob->GetBufferPointer(), motionBlob->GetBufferSize() };
        if (FAILED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mUpdateMotionPSO))))
            mUpdateMotionPSO.Reset();
    }

    ID3D12RootSignature* hizRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::HiZBuild);
    auto hizCopy = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\hiz_build.hlsl", "CS_CopyDepth", "cs_5_1");
    auto hizDown = ShaderManager::Get().GetShader(
        L"Resources\\Shaders\\hiz_build.hlsl", "CS_Downsample", "cs_5_1");
    if (hizRS && hizCopy && hizDown)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = hizRS;
        psoDesc.CS = { hizCopy->GetBufferPointer(), hizCopy->GetBufferSize() };
        if (SUCCEEDED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mHiZCopyPSO))))
        {
            psoDesc.CS = { hizDown->GetBufferPointer(), hizDown->GetBufferSize() };
            mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mHiZDownsamplePSO));
        }
    }

    if (!mComputeIndirectReady)
        OutputDebugStringA("[RenderSystem] Path3 GPU-driven unavailable (CS/PSO). Path1/2 OK.\n");
    else if (mComposeWorldPSO && mUpdateMotionPSO)
        OutputDebugStringA("[RenderSystem] Path3 ready (Motion + ComposeWorld + Cull + EI + HiZ).\n");
    else if (mComposeWorldPSO)
        OutputDebugStringA("[RenderSystem] Path3 ready (ComposeWorld + Cull + EI + HiZ; no Motion PSO).\n");
    else
        OutputDebugStringA("[RenderSystem] Path3 ready (no ComposeWorld PSO; fallback needed).\n");

    {
        InstanceWorld dummy{};
        StoreWorldTransposed(XMMatrixIdentity(), dummy.worldMatrix);
        const UINT64 sz = sizeof(InstanceWorld);
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(sz),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&mDummyInstanceBuffer)));
        void* mapped = nullptr;
        ThrowIfFailed(mDummyInstanceBuffer->Map(0, nullptr, &mapped));
        std::memcpy(mapped, &dummy, sizeof(dummy));
        mDummyInstanceBuffer->Unmap(0, nullptr);
    }

    mRenderPath = mComputeIndirectReady ? RenderPath::ComputeIndirect : RenderPath::Instanced;
}

void RenderSystem::DestroyGpuResources()
{
    for (UINT i = 0; i < kIndirectFrameCount; ++i)
    {
        auto& f = mFrames[i];
        if (f.requestUpload && f.requestMapped)
        {
            f.requestUpload->Unmap(0, nullptr);
            f.requestMapped = nullptr;
        }
        if (f.transformUpload && f.transformMapped)
        {
            f.transformUpload->Unmap(0, nullptr);
            f.transformMapped = nullptr;
        }
        if (f.motionUpload && f.motionMapped)
        {
            f.motionUpload->Unmap(0, nullptr);
            f.motionMapped = nullptr;
        }
        if (f.batchUpload && f.batchMapped)
        {
            f.batchUpload->Unmap(0, nullptr);
            f.batchMapped = nullptr;
        }
        if (f.submeshUpload && f.submeshMapped)
        {
            f.submeshUpload->Unmap(0, nullptr);
            f.submeshMapped = nullptr;
        }
        if (f.frameCBUpload && f.frameCBMapped)
        {
            f.frameCBUpload->Unmap(0, nullptr);
            f.frameCBMapped = nullptr;
        }
        f.requestUpload.Reset();
        f.transformUpload.Reset();
        f.motionUpload.Reset();
        f.batchUpload.Reset();
        f.submeshUpload.Reset();
        f.frameCBUpload.Reset();
        f.transformDefault.Reset();
        f.motionDefault.Reset();
        f.sourceDefault.Reset();
        f.batchDefault.Reset();
        f.submeshDefault.Reset();
        f.instanceBuffer.Reset();
        f.countBuffer.Reset();
        f.drawCmdBuffer.Reset();
        f.instanceState = D3D12_RESOURCE_STATE_COMMON;
        f.countState = D3D12_RESOURCE_STATE_COMMON;
        f.drawCmdState = D3D12_RESOURCE_STATE_COMMON;
        f.transformDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.motionDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.sourceDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.batchDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.submeshDefaultState = D3D12_RESOURCE_STATE_COMMON;
        f.uploadedTransformVersion = 0;
        f.uploadedMotionVersion = 0;
        f.composedTransformVersion = 0;
        f.uploadedMetaVersion = 0;
    }
    mCounterZeroUpload.Reset();
    mDummyInstanceBuffer.Reset();
}

void RenderSystem::Shutdown()
{
    DestroyHiZResources(mSrvAlloc);
    DestroyGpuResources();
    mCullCompactPSO.Reset();
    mBuildCommandsPSO.Reset();
    mComposeWorldPSO.Reset();
    mUpdateMotionPSO.Reset();
    mHiZCopyPSO.Reset();
    mHiZDownsamplePSO.Reset();
    mComputeIndirectReady = false;
    mInstancedCacheValid = false;
    mCachedInstancedBatches.clear();
    mCachedOverrideEntities.clear();
    mTransformCpu.clear();
    mMotionCpu.clear();
    mSourceCpu.clear();
    mBatchDescsCpu.clear();
    mSubmeshDescsCpu.clear();
    mGpuCpuBatches.clear();
    mGpuOverrideEntities.clear();
    mEntityToGpuSlot.clear();
    mPendingTransformDirty.clear();
    mDummyHiZTexture.Reset();
    mSrvAlloc = nullptr;
    mDevice = nullptr;
}

void RenderSystem::EnsureGpuResources(ID3D12Device* device)
{
    if (mFrames[0].instanceBuffer)
        return;

    // Path2 InstanceWorld upload OR Path3 source/transform sizes
    const UINT64 reqBufSize = (std::max)(
        sizeof(GpuInstanceSource) * kMaxInstancesPerDraw,
        sizeof(InstanceWorld) * kMaxInstancesPerDraw);
    const UINT64 xformBufSize = sizeof(GpuTransform) * kMaxInstancesPerDraw;
    const UINT64 motionBufSize = sizeof(GpuMotion) * kMaxInstancesPerDraw;
    const UINT64 sourceBufSize = sizeof(GpuInstanceSource) * kMaxInstancesPerDraw;
    const UINT64 instBufSize = sizeof(InstanceWorld) * kMaxInstancesPerDraw;
    const UINT64 batchBufSize = sizeof(GpuBatchDesc) * kMaxGpuBatches;
    const UINT64 submeshBufSize = sizeof(GpuSubmeshDesc) * kMaxGpuSubmeshDraws;
    const UINT64 counterBufSize = sizeof(UINT) * kMaxGpuBatches;
    const UINT64 drawCmdBufSize = sizeof(IndirectCommand) * kMaxGpuSubmeshDraws;
    const UINT64 frameCbSize = FrameCBAlignedSize();

    for (UINT fi = 0; fi < kIndirectFrameCount; ++fi)
    {
        auto& f = mFrames[fi];

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(instBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.instanceBuffer)));
        f.instanceState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(counterBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.countBuffer)));
        f.countState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(drawCmdBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.drawCmdBuffer)));
        f.drawCmdState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(reqBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.requestUpload)));
        ThrowIfFailed(f.requestUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.requestMapped)));

        // Step F1: TRS upload staging
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(xformBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.transformUpload)));
        ThrowIfFailed(f.transformUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.transformMapped)));

        // Step F2: motion seed upload
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(motionBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.motionUpload)));
        ThrowIfFailed(f.motionUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.motionMapped)));

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(batchBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.batchUpload)));
        ThrowIfFailed(f.batchUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.batchMapped)));

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(submeshBufSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.submeshUpload)));
        ThrowIfFailed(f.submeshUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.submeshMapped)));

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(frameCbSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&f.frameCBUpload)));
        ThrowIfFailed(f.frameCBUpload->Map(0, nullptr, reinterpret_cast<void**>(&f.frameCBMapped)));

        // Step F1/F2: TRS DEFAULT (UAV for motion, SRV for compose)
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(xformBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.transformDefault)));
        f.transformDefaultState = D3D12_RESOURCE_STATE_COMMON;

        // Step F2: motion DEFAULT (persistent velocity on GPU)
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(motionBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.motionDefault)));
        f.motionDefaultState = D3D12_RESOURCE_STATE_COMMON;

        // Compose output / Cull input (UAV+SRV)
        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(sourceBufSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.sourceDefault)));
        f.sourceDefaultState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(batchBufSize),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.batchDefault)));
        f.batchDefaultState = D3D12_RESOURCE_STATE_COMMON;

        ThrowIfFailed(device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(submeshBufSize),
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&f.submeshDefault)));
        f.submeshDefaultState = D3D12_RESOURCE_STATE_COMMON;
    }

    // 배치 카운터 일괄 제로
    ThrowIfFailed(device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(counterBufSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mCounterZeroUpload)));
    BYTE* mapped = nullptr;
    ThrowIfFailed(mCounterZeroUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    std::memset(mapped, 0, static_cast<size_t>(counterBufSize));
    mCounterZeroUpload->Unmap(0, nullptr);
}

void RenderSystem::ExtractFrustumPlanes(const XMMATRIX& viewProj, float outPlanes[6][4])
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, viewProj);

    const float raw[6][4] = {
        { m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41 },
        { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 },
        { m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42 },
        { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 },
        { m._13, m._23, m._33, m._43 },
        { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 },
    };

    for (int i = 0; i < 6; ++i)
    {
        XMFLOAT4 f{ raw[i][0], raw[i][1], raw[i][2], raw[i][3] };
        XMVECTOR p = XMLoadFloat4(&f);
        XMVECTOR n = XMVectorSetW(p, 0.0f);
        float len = XMVectorGetX(XMVector3Length(n));
        if (len > 1e-6f)
            p = XMVectorScale(p, 1.0f / len);
        XMStoreFloat4(&f, p);
        outPlanes[i][0] = f.x;
        outPlanes[i][1] = f.y;
        outPlanes[i][2] = f.z;
        outPlanes[i][3] = f.w;
    }
}

// ---------------------------------------------------------------------------
// Path3: rebuild contiguous (mesh, material) batches + source mirror
// ---------------------------------------------------------------------------
void RenderSystem::RebuildGpuDrivenScene(World& world)
{
    struct Entry
    {
        Entity entity = INVALID_ENTITY;
        Mesh* mesh = nullptr;
        Material* mainMat = nullptr;
        TransformComponent* tf = nullptr;
        RenderableComponent* rend = nullptr;
        BoundsComponent* bounds = nullptr;
    };

    std::vector<Entry> entries;
    entries.reserve(1024);
    mGpuOverrideEntities.clear();

    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.mesh) return;
            if (HasEntitySubMaterialOverride(e))
            {
                mGpuOverrideEntities.push_back(e);
                return;
            }
            Entry en;
            en.entity = e;
            en.mesh = rend.mesh;
            en.mainMat = GetEntityMainMaterialOverride(e);
            en.tf = &tf;
            en.rend = &rend;
            en.bounds = world.GetComponent<BoundsComponent>(e);
            entries.push_back(en);
        });

    std::stable_sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b)
        {
            if (a.mesh != b.mesh) return a.mesh < b.mesh;
            return a.mainMat < b.mainMat;
        });

    mTransformCpu.clear();
    mMotionCpu.clear();
    mSourceCpu.clear();
    mBatchDescsCpu.clear();
    mSubmeshDescsCpu.clear();
    mGpuCpuBatches.clear();
    mEntityToGpuSlot.clear();
    mMotionActiveCount = 0;

    const size_t cap = (std::min)(entries.size(), static_cast<size_t>(kMaxInstancesPerDraw));
    mTransformCpu.reserve(cap);
    mMotionCpu.reserve(cap);
    mSourceCpu.reserve(cap);

    UINT slot = 0;
    size_t i = 0;
    while (i < entries.size() && slot < kMaxInstancesPerDraw
        && mGpuCpuBatches.size() < kMaxGpuBatches)
    {
        Mesh* mesh = entries[i].mesh;
        Material* mainMat = entries[i].mainMat;
        const size_t groupBegin = i;

        // 같은 (mesh, material) 그룹 범위 확정
        while (i < entries.size()
            && entries[i].mesh == mesh
            && entries[i].mainMat == mainMat)
        {
            ++i;
        }
        const size_t groupEnd = i;
        const UINT groupCount = static_cast<UINT>((std::min)(
            groupEnd - groupBegin,
            static_cast<size_t>(kMaxInstancesPerDraw - slot)));
        if (groupCount == 0 || !mesh)
            continue;

        // 서브메시/머티리얼 먼저 해석 — 유효할 때만 소스 커밋
        std::vector<GpuSubmeshDesc> localSubs;
        std::vector<UINT> localMatIndices;
        for (auto& pair : mesh->DrawArgs)
        {
            if (mSubmeshDescsCpu.size() + localSubs.size() >= kMaxGpuSubmeshDraws)
                break;
            const auto& sub = pair.second;
            if (sub.IndexCount == 0)
                continue;
            Material* material = ResolveBatchMaterial(mainMat, sub);
            if (!material)
                continue;
            GpuSubmeshDesc sd{};
            sd.indexCount = sub.IndexCount;
            sd.startIndexLocation = sub.StartIndexLocation;
            sd.baseVertexLocation = sub.BaseVertexLocation;
            sd.batchId = 0; // 아래에서 채움
            localSubs.push_back(sd);
            localMatIndices.push_back(material->mTextureHandle.Index);
        }
        if (localSubs.empty())
            continue;

        const UINT batchId = static_cast<UINT>(mGpuCpuBatches.size());
        const UINT batchFirst = slot;

        for (UINT k = 0; k < groupCount; ++k)
        {
            const Entry& en = entries[groupBegin + k];
            GpuTransform xf{};
            GpuMotion mot{};
            GpuInstanceSource src{};
            FillTransformFromEntity(xf, *en.tf, *en.rend, en.bounds, batchId);
            FillMotionFromEntity(mot, world.GetComponent<GravityComponent>(en.entity));
            FillSourceFromEntity(src, *en.tf, *en.rend, en.bounds, batchId);
            mEntityToGpuSlot[en.entity] = slot;
            mTransformCpu.push_back(xf);
            mMotionCpu.push_back(mot);
            mSourceCpu.push_back(src);
            if (mot.flags & 1u)
                ++mMotionActiveCount;
            ++slot;
        }

        GpuCpuBatch cpuBatch{};
        cpuBatch.mesh = mesh;
        cpuBatch.mainMaterial = mainMat;
        cpuBatch.firstInstance = batchFirst;
        cpuBatch.instanceCount = groupCount;
        cpuBatch.firstSubmesh = static_cast<UINT>(mSubmeshDescsCpu.size());
        cpuBatch.submeshCount = static_cast<UINT>(localSubs.size());
        cpuBatch.materialIndices = std::move(localMatIndices);

        for (auto& sd : localSubs)
        {
            sd.batchId = batchId;
            mSubmeshDescsCpu.push_back(sd);
        }

        GpuBatchDesc bd{};
        bd.firstInstance = cpuBatch.firstInstance;
        bd.instanceCount = cpuBatch.instanceCount;
        bd.firstSubmesh = cpuBatch.firstSubmesh;
        bd.submeshCount = cpuBatch.submeshCount;
        mBatchDescsCpu.push_back(bd);
        mGpuCpuBatches.push_back(std::move(cpuBatch));
    }

    ++mTransformContentVersion;
    ++mMotionContentVersion;
    ++mMetaVersion;
    mGpuStructureDirty = false;
}

bool RenderSystem::PatchOneGpuEntity(
    World& world, FrameResource* frameResource, Entity e,
    bool& anyPatched, bool& motionPatched)
{
    auto* tf = world.GetComponent<TransformComponent>(e);
    auto* rend = world.GetComponent<RenderableComponent>(e);
    if (!tf || !rend || !rend->mesh)
        return false;

    auto it = mEntityToGpuSlot.find(e);
    if (it == mEntityToGpuSlot.end())
        return false;

    const UINT slot = it->second;
    if (slot >= mTransformCpu.size() || slot >= mSourceCpu.size() || slot >= mMotionCpu.size())
        return false;

    if (tf->dirtyFrames <= 0)
    {
        const uint32_t want = rend->visible ? 1u : 0u;
        if ((mTransformCpu[slot].flags & 1u) != want)
        {
            mTransformCpu[slot].flags = want;
            mSourceCpu[slot].flags = want;
            anyPatched = true;
        }
        return false; // no longer pending
    }

    BoundsComponent* bounds = world.GetComponent<BoundsComponent>(e);
    const UINT batchId = mTransformCpu[slot].batchId;
    FillTransformFromEntity(mTransformCpu[slot], *tf, *rend, bounds, batchId);
    FillSourceFromEntity(mSourceCpu[slot], *tf, *rend, bounds, batchId);

    // F2: GPU owns velocity after seed. Re-seed motion only when:
    // - GPU motion off (CPU gravity path), or
    // - enable flag toggled (add/remove gravity)
    GravityComponent* gravity = world.GetComponent<GravityComponent>(e);
    const bool gpuOwnsMotion = mGpuMotionEnabled && mUpdateMotionPSO;
    if (!gpuOwnsMotion)
    {
        FillMotionFromEntity(mMotionCpu[slot], gravity);
        motionPatched = true;
    }
    else
    {
        const uint32_t want = (gravity && gravity->enabled) ? 1u : 0u;
        const uint32_t prev = mMotionCpu[slot].flags & 1u;
        if (want != prev)
        {
            FillMotionFromEntity(mMotionCpu[slot], gravity);
            if (want)
                ++mMotionActiveCount;
            else if (mMotionActiveCount > 0)
                --mMotionActiveCount;
            motionPatched = true;
        }
    }

    if (frameResource && frameResource->ObjectCB
        && rend->objectCBIndex < kMaxSceneObjects)
    {
        ObjectConstants objConst{};
        XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf->GetWorldMatrix()));
        frameResource->ObjectCB->CopyData(static_cast<int>(rend->objectCBIndex), objConst);
    }
    --tf->dirtyFrames;
    anyPatched = true;
    return tf->dirtyFrames > 0;
}

void RenderSystem::PatchGpuDrivenTransforms(World& world, FrameResource* frameResource)
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    mLastStats.skippedPatch = false;
    mLastStats.usedDirtyList = false;
    mLastStats.dirtyPatched = 0;
    mLastStats.dirtyListIn = 0;

    const uint32_t gen = TransformDirtyTracker::Generation();
    auto incoming = TransformDirtyTracker::TakeEntities();
    mLastStats.dirtyListIn = static_cast<uint32_t>(incoming.size());

    if (!incoming.empty())
    {
        mPendingTransformDirty.insert(
            mPendingTransformDirty.end(), incoming.begin(), incoming.end());
        mLastStats.usedDirtyList = true;
    }

    const bool genChanged = (gen != mLastProcessedDirtyGen);
    const bool hasPending = !mPendingTransformDirty.empty();

    if (!genChanged && !hasPending)
    {
        mLastStats.skippedPatch = true;
        mLastStats.patchMs = ElapsedMs(t0);
        mLastStats.pendingDirty = 0;
        return;
    }

    // MarkDirty() without entity → generation only: full scan once
    if (genChanged && mPendingTransformDirty.empty())
    {
        world.ForEach<TransformComponent, RenderableComponent>(
            [&](Entity e, TransformComponent& tf, RenderableComponent&)
            {
                if (tf.dirtyFrames > 0)
                    mPendingTransformDirty.push_back(e);
            });
        mLastStats.usedDirtyList = false;
    }

    if (mPendingTransformDirty.size() > 1)
    {
        std::sort(mPendingTransformDirty.begin(), mPendingTransformDirty.end());
        mPendingTransformDirty.erase(
            std::unique(mPendingTransformDirty.begin(), mPendingTransformDirty.end()),
            mPendingTransformDirty.end());
    }

    bool anyPatched = false;
    bool motionPatched = false;
    std::vector<Entity> stillPending;
    stillPending.reserve(mPendingTransformDirty.size());
    uint32_t patched = 0;

    for (Entity e : mPendingTransformDirty)
    {
        bool touched = false;
        bool motTouched = false;
        const bool keep = PatchOneGpuEntity(world, frameResource, e, touched, motTouched);
        if (touched)
        {
            anyPatched = true;
            ++patched;
        }
        if (motTouched)
            motionPatched = true;
        if (keep)
            stillPending.push_back(e);
    }

    mPendingTransformDirty = std::move(stillPending);
    mLastProcessedDirtyGen = gen;
    mLastStats.dirtyPatched = patched;
    mLastStats.pendingDirty = static_cast<uint32_t>(mPendingTransformDirty.size());

    if (anyPatched)
        ++mTransformContentVersion;
    if (motionPatched)
        ++mMotionContentVersion;

    mLastStats.patchMs = ElapsedMs(t0);
}

void RenderSystem::UploadGpuDrivenFrameData(
    ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame)
{
    const auto t0 = std::chrono::high_resolution_clock::now();
    mLastStats.didSourceUpload = false;
    mLastStats.didMetaUpload = false;
    mLastStats.usedDefaultHeapCopy = false;

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& st, D3D12_RESOURCE_STATES target)
    {
        if (st != target)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(res, st, target));
            st = target;
        }
    };

    // Step F1: TRS staging → DEFAULT (compose / motion input)
    if (frame.uploadedTransformVersion != mTransformContentVersion && frame.transformMapped)
    {
        const UINT64 bytes = sizeof(GpuTransform) * mTransformCpu.size();
        if (!mTransformCpu.empty() && bytes > 0)
        {
            std::memcpy(frame.transformMapped, mTransformCpu.data(), static_cast<size_t>(bytes));
            transition(frame.transformDefault.Get(), frame.transformDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                frame.transformDefault.Get(), 0,
                frame.transformUpload.Get(), 0,
                bytes);
            transition(frame.transformDefault.Get(), frame.transformDefaultState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            mLastStats.usedDefaultHeapCopy = true;
        }
        frame.uploadedTransformVersion = mTransformContentVersion;
        mLastStats.didSourceUpload = true; // TRS upload (legacy stat name)
    }
    else
    {
        transition(frame.transformDefault.Get(), frame.transformDefaultState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Step F2: motion seed → DEFAULT (only on version change; GPU owns velocity after)
    if (frame.uploadedMotionVersion != mMotionContentVersion && frame.motionMapped && frame.motionDefault)
    {
        const UINT64 bytes = sizeof(GpuMotion) * mMotionCpu.size();
        if (!mMotionCpu.empty() && bytes > 0)
        {
            std::memcpy(frame.motionMapped, mMotionCpu.data(), static_cast<size_t>(bytes));
            transition(frame.motionDefault.Get(), frame.motionDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                frame.motionDefault.Get(), 0,
                frame.motionUpload.Get(), 0,
                bytes);
            transition(frame.motionDefault.Get(), frame.motionDefaultState,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            mLastStats.usedDefaultHeapCopy = true;
        }
        frame.uploadedMotionVersion = mMotionContentVersion;
    }
    else if (frame.motionDefault)
    {
        transition(frame.motionDefault.Get(), frame.motionDefaultState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (frame.uploadedMetaVersion != mMetaVersion)
    {
        if (frame.batchMapped && !mBatchDescsCpu.empty())
        {
            const UINT64 bytes = sizeof(GpuBatchDesc) * mBatchDescsCpu.size();
            std::memcpy(frame.batchMapped, mBatchDescsCpu.data(), static_cast<size_t>(bytes));
            transition(frame.batchDefault.Get(), frame.batchDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                frame.batchDefault.Get(), 0,
                frame.batchUpload.Get(), 0,
                bytes);
            transition(frame.batchDefault.Get(), frame.batchDefaultState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            mLastStats.usedDefaultHeapCopy = true;
        }
        if (frame.submeshMapped && !mSubmeshDescsCpu.empty())
        {
            const UINT64 bytes = sizeof(GpuSubmeshDesc) * mSubmeshDescsCpu.size();
            std::memcpy(frame.submeshMapped, mSubmeshDescsCpu.data(), static_cast<size_t>(bytes));
            transition(frame.submeshDefault.Get(), frame.submeshDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyBufferRegion(
                frame.submeshDefault.Get(), 0,
                frame.submeshUpload.Get(), 0,
                bytes);
            transition(frame.submeshDefault.Get(), frame.submeshDefaultState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            mLastStats.usedDefaultHeapCopy = true;
        }
        frame.uploadedMetaVersion = mMetaVersion;
        mLastStats.didMetaUpload = true;
    }
    else
    {
        transition(frame.batchDefault.Get(), frame.batchDefaultState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(frame.submeshDefault.Get(), frame.submeshDefaultState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    mLastStats.uploadMs = ElapsedMs(t0);
}

bool RenderSystem::ShouldSkipCpuGravity() const
{
    return mGpuMotionEnabled
        && mUpdateMotionPSO
        && mRenderPath == RenderPath::ComputeIndirect
        && mComputeIndirectReady;
}

void RenderSystem::SetGpuMotionEnabled(bool enabled)
{
    if (mGpuMotionEnabled == enabled)
        return;
    mGpuMotionEnabled = enabled;
    // Force motion buffer re-upload on next Path3 frame (seed from current ECS)
    ++mMotionContentVersion;
}

void RenderSystem::NotifyEntityMotionChanged(World& world, Entity e)
{
    auto it = mEntityToGpuSlot.find(e);
    if (it == mEntityToGpuSlot.end())
        return;
    const UINT slot = it->second;
    if (slot >= mMotionCpu.size())
        return;

    const uint32_t prev = mMotionCpu[slot].flags & 1u;
    FillMotionFromEntity(mMotionCpu[slot], world.GetComponent<GravityComponent>(e));
    const uint32_t now = mMotionCpu[slot].flags & 1u;
    if (prev != now)
    {
        if (now)
            ++mMotionActiveCount;
        else if (mMotionActiveCount > 0)
            --mMotionActiveCount;
    }
    ++mMotionContentVersion;
}

void RenderSystem::SyncGpuMotionFromWorld(World& world)
{
    if (mMotionCpu.empty() || mEntityToGpuSlot.empty())
    {
        mMotionActiveCount = 0;
        return;
    }

    bool anyChange = false;
    uint32_t active = 0;

    for (const auto& kv : mEntityToGpuSlot)
    {
        const UINT slot = kv.second;
        if (slot >= mMotionCpu.size())
            continue;

        GpuMotion want{};
        FillMotionFromEntity(want, world.GetComponent<GravityComponent>(kv.first));
        GpuMotion& cur = mMotionCpu[slot];

        const bool wantOn = (want.flags & 1u) != 0;
        const bool curOn = (cur.flags & 1u) != 0;

        // Enable / disable / strength change → reseed from ECS (resets v to component)
        // Stay-on: do NOT overwrite velocity (GPU owns it after seed)
        if (wantOn != curOn || (wantOn && want.gravity != cur.gravity))
        {
            cur = want;
            anyChange = true;
        }

        if (cur.flags & 1u)
            ++active;
    }

    mMotionActiveCount = active;
    if (anyChange)
        ++mMotionContentVersion;
}

bool RenderSystem::DispatchUpdateMotion(
    ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame, UINT numInstances)
{
    mLastStats.didGpuMotion = false;
    mLastStats.motionMs = 0.f;
    mLastStats.motionActive = mMotionActiveCount;
    mLastStats.gpuMotionEnabled = mGpuMotionEnabled;

    if (!mGpuMotionEnabled || !mUpdateMotionPSO || !frame.transformDefault || !frame.motionDefault)
        return false;
    if (numInstances == 0 || mMotionActiveCount == 0)
        return false;
    if (numInstances > mTransformCpu.size())
        numInstances = static_cast<UINT>(mTransformCpu.size());

    ID3D12RootSignature* motionRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::UpdateMotion);
    if (!motionRS)
        return false;

    const auto t0 = std::chrono::high_resolution_clock::now();

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& st, D3D12_RESOURCE_STATES target)
    {
        if (st != target)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(res, st, target));
            st = target;
        }
    };

    transition(frame.transformDefault.Get(), frame.transformDefaultState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(frame.motionDefault.Get(), frame.motionDefaultState,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetComputeRootSignature(motionRS);
    cmdList->SetPipelineState(mUpdateMotionPSO.Get());

    const float dt = (mFrameDeltaTime > 0.f && mFrameDeltaTime < 0.25f)
        ? mFrameDeltaTime
        : (1.f / 60.f);
    // Root constants: uint, uint, float, uint — pack float as bits
    UINT constants[4];
    constants[0] = numInstances;
    constants[1] = kMaxInstancesPerDraw;
    std::memcpy(&constants[2], &dt, sizeof(float));
    constants[3] = 0;
    cmdList->SetComputeRoot32BitConstants(0, 4, constants, 0);
    cmdList->SetComputeRootUnorderedAccessView(1, frame.transformDefault->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(2, frame.motionDefault->GetGPUVirtualAddress());

    cmdList->Dispatch((numInstances + 63u) / 64u, 1, 1);

    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::UAV(frame.transformDefault.Get());
        b[1] = CD3DX12_RESOURCE_BARRIER::UAV(frame.motionDefault.Get());
        cmdList->ResourceBarrier(2, b);
    }

    // Compose reads transforms as SRV
    transition(frame.transformDefault.Get(), frame.transformDefaultState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    mLastStats.didGpuMotion = true;
    mLastStats.motionMs = ElapsedMs(t0);
    return true;
}

void RenderSystem::DispatchComposeWorld(
    ID3D12GraphicsCommandList* cmdList, FrameGpuResources& frame, UINT numInstances)
{
    mLastStats.didComposeWorld = false;
    mLastStats.composeMs = 0.f;

    if (!frame.sourceDefault || numInstances == 0)
        return;
    if (numInstances > mTransformCpu.size())
        numInstances = static_cast<UINT>(mTransformCpu.size());
    if (numInstances == 0)
        return;

    // Already composed for this transform version on this frame slot
    if (frame.composedTransformVersion == mTransformContentVersion
        && frame.composedTransformVersion != 0)
    {
        // Cull expects source as SRV
        if (frame.sourceDefaultState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                frame.sourceDefault.Get(),
                frame.sourceDefaultState,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
            frame.sourceDefaultState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        return;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& st, D3D12_RESOURCE_STATES target)
    {
        if (st != target)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(res, st, target));
            st = target;
        }
    };

    ID3D12RootSignature* composeRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::ComposeWorld);

    // GPU path: TRS (transformDefault) → CS_ComposeWorld → sourceDefault
    if (mComposeWorldPSO && composeRS && frame.transformDefault)
    {
        transition(frame.transformDefault.Get(), frame.transformDefaultState,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(frame.sourceDefault.Get(), frame.sourceDefaultState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cmdList->SetComputeRootSignature(composeRS);
        cmdList->SetPipelineState(mComposeWorldPSO.Get());

        const UINT constants[4] = {
            numInstances,
            kMaxInstancesPerDraw,
            0u,
            0u
        };
        cmdList->SetComputeRoot32BitConstants(0, 4, constants, 0);
        cmdList->SetComputeRootShaderResourceView(
            1, frame.transformDefault->GetGPUVirtualAddress());
        cmdList->SetComputeRootUnorderedAccessView(
            2, frame.sourceDefault->GetGPUVirtualAddress());

        cmdList->Dispatch((numInstances + 63u) / 64u, 1, 1);

        // UAV write → SRV read (cull)
        {
            D3D12_RESOURCE_BARRIER b[2];
            b[0] = CD3DX12_RESOURCE_BARRIER::UAV(frame.sourceDefault.Get());
            b[1] = CD3DX12_RESOURCE_BARRIER::Transition(
                frame.sourceDefault.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(2, b);
            frame.sourceDefaultState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }

        frame.composedTransformVersion = mTransformContentVersion;
        mLastStats.didComposeWorld = true;
        mLastStats.composeMs = ElapsedMs(t0);
        return;
    }

    // Fallback: CPU GetWorldMatrix path (PSO missing)
    if (!frame.requestMapped || mSourceCpu.empty())
        return;
    if (numInstances > mSourceCpu.size())
        numInstances = static_cast<UINT>(mSourceCpu.size());

    const UINT64 bytes = sizeof(GpuInstanceSource) * numInstances;
    std::memcpy(frame.requestMapped, mSourceCpu.data(), static_cast<size_t>(bytes));

    transition(frame.sourceDefault.Get(), frame.sourceDefaultState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(
        frame.sourceDefault.Get(), 0,
        frame.requestUpload.Get(), 0,
        bytes);
    transition(frame.sourceDefault.Get(), frame.sourceDefaultState,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    frame.composedTransformVersion = mTransformContentVersion;
    mLastStats.didComposeWorld = true;
    mLastStats.composeMs = ElapsedMs(t0);
}

void RenderSystem::render(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    // Step G: 매 프레임 자동 경로 (수동 SetRenderPath 시 auto off)
    UpdateAutoRenderPath(world);

    switch (mRenderPath)
    {
    case RenderPath::ComputeIndirect:
        if (mComputeIndirectReady
            && mCullCompactPSO
            && mBuildCommandsPSO
            && RootSignatureManager::Get().GetSceneCommandSignature())
        {
            renderComputeIndirect(world, cmdList, currentFrameResource, descriptorAllocator,
                currentFrameIndex, viewMatrix, projMatrix);
            return;
        }
        {
            static bool sLogged = false;
            if (!sLogged)
            {
                OutputDebugStringA(
                    "[RenderSystem] ComputeIndirect unavailable at render time; "
                    "fallback -> Instanced (path flag set to Instanced).\n");
                sLogged = true;
            }
            mRenderPath = RenderPath::Instanced;
        }
        renderInstanced(world, cmdList, currentFrameResource, descriptorAllocator,
            currentFrameIndex, viewMatrix, projMatrix);
        return;
    case RenderPath::Instanced:
        renderInstanced(world, cmdList, currentFrameResource, descriptorAllocator,
            currentFrameIndex, viewMatrix, projMatrix);
        return;
    case RenderPath::Basic:
    default:
        renderBasic(world, cmdList, currentFrameResource, descriptorAllocator, viewMatrix, projMatrix);
        return;
    }
}

// ---------------------------------------------------------------------------
// Path 1: Basic
// ---------------------------------------------------------------------------
void RenderSystem::renderBasic(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    (void)viewMatrix;
    (void)projMatrix;

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    PSOKey key{};
    key.shaderName = "object_cb";
    key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);

    if (sceneRS)
        cmdList->SetGraphicsRootSignature(sceneRS);
    if (pso)
        cmdList->SetPipelineState(pso);

    if (mDummyInstanceBuffer)
        cmdList->SetGraphicsRootShaderResourceView(3, mDummyInstanceBuffer->GetGPUVirtualAddress());

    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    UINT lastMat = UINT_MAX;
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (rend.objectCBIndex >= kMaxSceneObjects) return;

            if (tf.dirtyFrames > 0)
            {
                ObjectConstants objConst{};
                XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf.GetWorldMatrix()));
                currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);
                --tf.dirtyFrames;
            }

            auto objectCB = currentFrameResource->ObjectCB->Resource();
            UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
            D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() +
                (UINT64)rend.objectCBIndex * objCBByteSize;

            auto vbv = rend.mesh->VertexBufferView();
            auto ibv = rend.mesh->IndexBufferView();
            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            for (auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = MaterialManager::Get().ResolveForDraw(e, pair.first, sub.initMaterial);
                if (!material || !material->HasValidTexture())
                    continue;

                cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
                BindMaterialByIndex(cmdList, descriptorAllocator,
                    material->mTextureHandle.Index, lastMat);
                cmdList->DrawIndexedInstanced(
                    sub.IndexCount, 1,
                    sub.StartIndexLocation,
                    sub.BaseVertexLocation, 0);
            }
        });
}

// ---------------------------------------------------------------------------
// Path 2 helpers
// ---------------------------------------------------------------------------
void RenderSystem::DrawInstancedBatches(
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    FrameGpuResources& frame,
    DescriptorAllocator* descriptorAllocator,
    const std::vector<Entity>& overrideEntities,
    World& world)
{
    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    if (!sceneRS)
        return;

    cmdList->SetGraphicsRootSignature(sceneRS);
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    UINT lastMat = UINT_MAX;

    if (!overrideEntities.empty())
    {
        PSOKey basicKey{};
        basicKey.shaderName = "object_cb";
        basicKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        basicKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        basicKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        if (ID3D12PipelineState* basicPso = PipelineStateManager::Get().GetOrCreatePSO(basicKey, sceneRS))
            cmdList->SetPipelineState(basicPso);
        if (mDummyInstanceBuffer)
            cmdList->SetGraphicsRootShaderResourceView(3, mDummyInstanceBuffer->GetGPUVirtualAddress());

        for (Entity e : overrideEntities)
        {
            auto* tf = world.GetComponent<TransformComponent>(e);
            auto* rend = world.GetComponent<RenderableComponent>(e);
            if (tf && rend)
                DrawEntityWithResolvedMaterials(cmdList, currentFrameResource, e, *tf, *rend);
        }
        lastMat = UINT_MAX; // PSO/path switch — force rebind
    }

    if (mCachedInstancedBatches.empty())
        return;

    PSOKey gfxKey{};
    gfxKey.shaderName = "object_instanced";
    gfxKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    gfxKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    gfxKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    ID3D12PipelineState* gfxPSO = PipelineStateManager::Get().GetOrCreatePSO(gfxKey, sceneRS);
    if (!gfxPSO)
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            OutputDebugStringA(
                "[RenderSystem] Instanced PSO failed; drawing overrides only.\n");
            sLogged = true;
        }
        return;
    }

    cmdList->SetPipelineState(gfxPSO);
    lastMat = UINT_MAX;

    BYTE* upload = frame.requestMapped;
    UINT uploadCursor = 0;

    for (const CachedInstancedBatch& batch : mCachedInstancedBatches)
    {
        Mesh* mesh = batch.mesh;
        const auto& instances = batch.instances;
        if (!mesh || instances.empty())
            continue;

        const UINT n = static_cast<UINT>((std::min)(
            instances.size(),
            static_cast<size_t>(kMaxInstancesPerDraw - uploadCursor)));
        if (n == 0)
            break;

        const UINT64 byteOffset = (UINT64)uploadCursor * sizeof(InstanceWorld);
        std::memcpy(upload + byteOffset, instances.data(), sizeof(InstanceWorld) * n);

        const D3D12_GPU_VIRTUAL_ADDRESS instVA =
            frame.requestUpload->GetGPUVirtualAddress() + byteOffset;

        cmdList->SetGraphicsRootShaderResourceView(3, instVA);

        auto vbv = mesh->VertexBufferView();
        auto ibv = mesh->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (auto& pair : mesh->DrawArgs)
        {
            const auto& sub = pair.second;
            if (sub.IndexCount == 0)
                continue;

            Material* material = ResolveBatchMaterial(batch.mainMaterial, sub);
            if (!material || !material->HasValidTexture())
                continue;

            BindMaterialByIndex(cmdList, descriptorAllocator,
                material->mTextureHandle.Index, lastMat);
            cmdList->DrawIndexedInstanced(
                sub.IndexCount, n,
                sub.StartIndexLocation,
                sub.BaseVertexLocation, 0);
        }

        uploadCursor += n;
    }
}

void RenderSystem::renderInstanced(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    (void)viewMatrix;
    (void)projMatrix;

    EnsureGpuResources(mDevice);

    const int frameIdx = ((currentFrameIndex % (int)kIndirectFrameCount) + (int)kIndirectFrameCount)
        % (int)kIndirectFrameCount;
    FrameGpuResources& frame = mFrames[frameIdx];

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    if (!sceneRS)
    {
        renderBasic(world, cmdList, currentFrameResource, descriptorAllocator, viewMatrix, projMatrix);
        return;
    }

    // Step B: dirty generation + pending 없으면 캐시 재사용 (ForEach 스킵)
    {
        auto incoming = TransformDirtyTracker::TakeEntities();
        if (!incoming.empty())
            mPendingTransformDirty.insert(
                mPendingTransformDirty.end(), incoming.begin(), incoming.end());

        const uint32_t gen = TransformDirtyTracker::Generation();
        const bool staticScene =
            mInstancedCacheValid
            && mPendingTransformDirty.empty()
            && gen == mPath2CachedDirtyGen;

        if (staticScene)
        {
            mLastStats = {};
            mLastStats.path = RenderPath::Instanced;
            mLastStats.autoPath = mAutoRenderPath;
            mLastStats.skippedPatch = true;
            mLastStats.sourceCount = 0;
            for (const auto& b : mCachedInstancedBatches)
                mLastStats.sourceCount += static_cast<uint32_t>(b.instances.size());
            mLastStats.batchCount = static_cast<uint32_t>(mCachedInstancedBatches.size());
            DrawInstancedBatches(cmdList, currentFrameResource, frame,
                descriptorAllocator, mCachedOverrideEntities, world);
            return;
        }

        if (mInstancedCacheValid)
            mInstancedCacheValid = false;
    }

    std::map<std::pair<Mesh*, Material*>, std::vector<InstanceWorld>> batchMap;
    std::vector<Entity> overrideEntities;
    UINT totalObjects = 0;

    mPendingTransformDirty.clear();
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;
            if (totalObjects >= kMaxInstancesPerDraw) return;

            if (HasEntitySubMaterialOverride(e))
            {
                overrideEntities.push_back(e);
                return;
            }

            if (tf.dirtyFrames > 0 && rend.objectCBIndex < kMaxSceneObjects)
            {
                ObjectConstants objConst{};
                XMStoreFloat4x4(&objConst.World, XMMatrixTranspose(tf.GetWorldMatrix()));
                currentFrameResource->ObjectCB->CopyData(static_cast<int>(rend.objectCBIndex), objConst);
                --tf.dirtyFrames;
                // multi-frame ObjectCB: 남은 dirty는 다음 프레임에도 캐시 무효
                if (tf.dirtyFrames > 0)
                    mPendingTransformDirty.push_back(e);
            }

            Material* mainMat = GetEntityMainMaterialOverride(e);
            InstanceWorld inst{};
            StoreWorldTransposed(tf.GetWorldMatrix(), inst.worldMatrix);
            batchMap[{rend.mesh, mainMat}].push_back(inst);
            ++totalObjects;
        });

    mCachedInstancedBatches.clear();
    mCachedInstancedBatches.reserve(batchMap.size());
    for (auto& kv : batchMap)
    {
        CachedInstancedBatch b;
        b.mesh = kv.first.first;
        b.mainMaterial = kv.first.second;
        b.instances = std::move(kv.second);
        mCachedInstancedBatches.push_back(std::move(b));
    }
    mCachedOverrideEntities = std::move(overrideEntities);
    mInstancedCacheValid = true;
    mPath2CachedDirtyGen = TransformDirtyTracker::Generation();
    TransformDirtyTracker::ClearEntities();

    mLastStats = {};
    mLastStats.path = RenderPath::Instanced;
    mLastStats.autoPath = mAutoRenderPath;
    mLastStats.pendingDirty = static_cast<uint32_t>(mPendingTransformDirty.size());
    mLastStats.batchCount = static_cast<uint32_t>(mCachedInstancedBatches.size());
    for (const auto& b : mCachedInstancedBatches)
        mLastStats.sourceCount += static_cast<uint32_t>(b.instances.size());

    DrawInstancedBatches(cmdList, currentFrameResource, frame,
        descriptorAllocator, mCachedOverrideEntities, world);
}

// ---------------------------------------------------------------------------
// Path 3: GPU-driven — persistent source + global cull/compact + EI
// ---------------------------------------------------------------------------
void RenderSystem::renderComputeIndirect(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    EnsureGpuResources(mDevice);

    const int frameIdx = ((currentFrameIndex % (int)kIndirectFrameCount) + (int)kIndirectFrameCount)
        % (int)kIndirectFrameCount;
    FrameGpuResources& frame = mFrames[frameIdx];

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    mSrvAlloc = descriptorAllocator;
    mLastStats = {};
    mLastStats.path = RenderPath::ComputeIndirect;
    mLastStats.autoPath = mAutoRenderPath;
    mLastStats.cullEnabled = mGpuFrustumCull;
    mLastStats.occlusionEnabled = mGpuOcclusion;
    mLastStats.hizValid = IsHiZSampleReady();
    mLastStats.hizMips = mHiZMipCount;
    mLastStats.eiCalls = 0;
    mLastStats.multiEiRuns = 0;

    // 1) CPU: 구조 변경 시 배치 재빌드 + dirty 슬롯 패치
    if (mGpuStructureDirty)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        RebuildGpuDrivenScene(world);
        mLastStats.didRebuild = true;
        mLastStats.rebuildMs = ElapsedMs(t0);
        // 소스 미러는 최신. multi-frame ObjectCB용 dirtyFrames만 pending 유지.
        TransformDirtyTracker::ClearEntities();
        mPendingTransformDirty.clear();
        for (const auto& kv : mEntityToGpuSlot)
        {
            if (auto* tf = world.GetComponent<TransformComponent>(kv.first))
            {
                if (tf->dirtyFrames > 0)
                    mPendingTransformDirty.push_back(kv.first);
            }
        }
        mLastProcessedDirtyGen = TransformDirtyTracker::Generation();
    }
    PatchGpuDrivenTransforms(world, currentFrameResource);
    // Gravity toggle / Enabled checkbox — reseed motion without scene rebuild
    SyncGpuMotionFromWorld(world);

    mLastStats.sourceCount = static_cast<uint32_t>(mSourceCpu.size());
    mLastStats.batchCount = static_cast<uint32_t>(mBatchDescsCpu.size());
    mLastStats.submeshDraws = static_cast<uint32_t>(mSubmeshDescsCpu.size());

    // Sub-override는 Basic으로
    ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
    if (sceneRS && !mGpuOverrideEntities.empty())
    {
        cmdList->SetGraphicsRootSignature(sceneRS);
        if (currentFrameResource && currentFrameResource->PassCB)
        {
            cmdList->SetGraphicsRootConstantBufferView(
                1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
        }
        PSOKey basicKey{};
        basicKey.shaderName = "object_cb";
        basicKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        basicKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        basicKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        if (ID3D12PipelineState* basicPso = PipelineStateManager::Get().GetOrCreatePSO(basicKey, sceneRS))
            cmdList->SetPipelineState(basicPso);
        if (mDummyInstanceBuffer)
            cmdList->SetGraphicsRootShaderResourceView(3, mDummyInstanceBuffer->GetGPUVirtualAddress());

        for (Entity e : mGpuOverrideEntities)
        {
            auto* tf = world.GetComponent<TransformComponent>(e);
            auto* rend = world.GetComponent<RenderableComponent>(e);
            if (tf && rend)
                DrawEntityWithResolvedMaterials(cmdList, currentFrameResource, e, *tf, *rend);
        }
    }

    if (mSourceCpu.empty() || mGpuCpuBatches.empty() || mSubmeshDescsCpu.empty())
        return;

    UploadGpuDrivenFrameData(cmdList, frame);

    // Step F2 motion → F1 compose → cull
    const UINT numXforms = static_cast<UINT>((std::min)(
        (std::min)(mTransformCpu.size(), mSourceCpu.size()),
        static_cast<size_t>(kMaxInstancesPerDraw)));
    if (DispatchUpdateMotion(cmdList, frame, numXforms))
    {
        // GPU changed TRS — force compose this frame slot
        frame.composedTransformVersion = 0;
    }
    DispatchComposeWorld(cmdList, frame, numXforms);

    // 2) Frame CB (카메라/컬링/오클루전 — 매 프레임)
    const XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
    mLastViewProj = viewProj;
    mLastRtWidth = (mHiZWidth > 0) ? mHiZWidth : 1;
    mLastRtHeight = (mHiZHeight > 0) ? mHiZHeight : 1;

    GpuDrivenFrameConstants cb{};
    ExtractFrustumPlanes(viewProj, cb.frustumPlanes);
    cb.numInstances = numXforms;
    cb.enableFrustumCull = mGpuFrustumCull ? 1u : 0u;
    cb.numBatches = static_cast<uint32_t>(mBatchDescsCpu.size());
    cb.numSubmeshDraws = static_cast<uint32_t>(mSubmeshDescsCpu.size());
    cb.maxInstances = kMaxInstancesPerDraw;
    const bool hizReady = IsHiZSampleReady();
    cb.enableOcclusion = (mGpuOcclusion && hizReady) ? 1u : 0u;
    cb.hizMipCount = mHiZMipCount;
    cb.hizValid = hizReady ? 1u : 0u;
    {
        XMFLOAT4X4 vpT;
        XMStoreFloat4x4(&vpT, XMMatrixTranspose(viewProj));
        std::memcpy(cb.viewProj, &vpT, sizeof(vpT));
    }
    cb.rtWidth = static_cast<float>((std::max)(1u, mLastRtWidth));
    cb.rtHeight = static_cast<float>((std::max)(1u, mLastRtHeight));
    cb.zNear = mLastNear;
    cb.zFar = mLastFar;
    std::memcpy(frame.frameCBMapped, &cb, sizeof(cb));

    ID3D12RootSignature* buildRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::IndirectBuild);
    ID3D12CommandSignature* cmdSig = RootSignatureManager::Get().GetSceneCommandSignature();

    PSOKey gfxKey{};
    gfxKey.shaderName = "object_instanced";
    gfxKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    gfxKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    gfxKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    ID3D12PipelineState* gfxPSO = PipelineStateManager::Get().GetOrCreatePSO(gfxKey, sceneRS);

    if (!buildRS || !sceneRS || !gfxPSO || !cmdSig || !mCullCompactPSO || !mBuildCommandsPSO)
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            OutputDebugStringA(
                "[RenderSystem] GPU-driven setup failed; fallback -> Instanced.\n");
            sLogged = true;
        }
        mRenderPath = RenderPath::Instanced;
        renderInstanced(world, cmdList, currentFrameResource, descriptorAllocator,
            currentFrameIndex, viewMatrix, projMatrix);
        return;
    }

    // 3) 카운터 제로 (배치 전체 1회)
    {
        if (frame.countState != D3D12_RESOURCE_STATE_COPY_DEST)
        {
            cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                frame.countBuffer.Get(), frame.countState, D3D12_RESOURCE_STATE_COPY_DEST));
            frame.countState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        const UINT64 counterBytes = sizeof(UINT) * kMaxGpuBatches;
        cmdList->CopyBufferRegion(
            frame.countBuffer.Get(), 0,
            mCounterZeroUpload.Get(), 0,
            counterBytes);
    }

    // UAV 전환
    {
        D3D12_RESOURCE_BARRIER b[3];
        UINT nb = 0;
        auto toUav = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES& st)
        {
            if (st != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                b[nb++] = CD3DX12_RESOURCE_BARRIER::Transition(
                    res, st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                st = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
        };
        toUav(frame.countBuffer.Get(), frame.countState);
        toUav(frame.instanceBuffer.Get(), frame.instanceState);
        toUav(frame.drawCmdBuffer.Get(), frame.drawCmdState);
        if (nb) cmdList->ResourceBarrier(nb, b);
    }

    // 4) 전역 CullCompact 1회 (DEFAULT 힙 source/batch/submesh + HiZ t3)
    cmdList->SetComputeRootSignature(buildRS);
    cmdList->SetComputeRootConstantBufferView(0, frame.frameCBUpload->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(1, frame.sourceDefault->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(2, frame.batchDefault->GetGPUVirtualAddress());
    cmdList->SetComputeRootShaderResourceView(3, frame.submeshDefault->GetGPUVirtualAddress());

    // Hi-Z: 링에서 "충분히 오래된" 슬롯만 샘플 (in-flight 쓰기와 분리)
    EnsureDummyHiZSrv(descriptorAllocator);
    D3D12_GPU_DESCRIPTOR_HANDLE hizGpu = GetHiZSampleSrvGpu();
    if (hizGpu.ptr == 0)
        hizGpu = mDummyHiZSrv.GPU;
    if (hizGpu.ptr == 0)
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            OutputDebugStringA("[RenderSystem] No valid HiZ SRV; skip GPU-driven frame.\n");
            sLogged = true;
        }
        return;
    }
    cmdList->SetComputeRootDescriptorTable(4, hizGpu);

    cmdList->SetComputeRootUnorderedAccessView(5, frame.instanceBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(6, frame.countBuffer->GetGPUVirtualAddress());
    cmdList->SetComputeRootUnorderedAccessView(7, frame.drawCmdBuffer->GetGPUVirtualAddress());

    cmdList->SetPipelineState(mCullCompactPSO.Get());
    if (numXforms == 0)
        return;
    cmdList->Dispatch((numXforms + 63u) / 64u, 1, 1);

    {
        D3D12_RESOURCE_BARRIER uav[2];
        uav[0] = CD3DX12_RESOURCE_BARRIER::UAV(frame.instanceBuffer.Get());
        uav[1] = CD3DX12_RESOURCE_BARRIER::UAV(frame.countBuffer.Get());
        cmdList->ResourceBarrier(2, uav);
    }

    // 5) 전역 BuildCommands 1회
    cmdList->SetPipelineState(mBuildCommandsPSO.Get());
    const UINT numSubs = static_cast<UINT>((std::min)(
        mSubmeshDescsCpu.size(), static_cast<size_t>(kMaxGpuSubmeshDraws)));
    if (numSubs == 0)
        return;
    cmdList->Dispatch((numSubs + 63u) / 64u, 1, 1);

    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::Transition(
            frame.drawCmdBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            frame.instanceBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(2, b);
        frame.drawCmdState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        frame.instanceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    // 6) Step E: material heap Index로 테이블 바인딩 (동일 인덱스면 스킵)
    cmdList->SetGraphicsRootSignature(sceneRS);
    cmdList->SetPipelineState(gfxPSO);
    if (currentFrameResource && currentFrameResource->PassCB)
    {
        cmdList->SetGraphicsRootConstantBufferView(
            1, currentFrameResource->PassCB->Resource()->GetGPUVirtualAddress());
    }

    UINT lastMat = UINT_MAX;
    for (const GpuCpuBatch& batch : mGpuCpuBatches)
    {
        if (!batch.mesh || batch.instanceCount == 0 || batch.submeshCount == 0)
            continue;

        const D3D12_GPU_VIRTUAL_ADDRESS instVA =
            frame.instanceBuffer->GetGPUVirtualAddress()
            + (UINT64)batch.firstInstance * sizeof(InstanceWorld);
        cmdList->SetGraphicsRootShaderResourceView(3, instVA);

        auto vbv = batch.mesh->VertexBufferView();
        auto ibv = batch.mesh->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // 같은 머티리얼 연속 구간 → ExecuteIndirect MaxCommandCount > 1
        UINT si = 0;
        while (si < batch.submeshCount)
        {
            if (batch.firstSubmesh + si >= mSubmeshDescsCpu.size()
                || si >= batch.materialIndices.size())
                break;

            const UINT mat = batch.materialIndices[si];
            const UINT runStart = si;
            while (si < batch.submeshCount
                && si < batch.materialIndices.size()
                && batch.materialIndices[si] == mat)
            {
                ++si;
            }
            const UINT runLen = si - runStart;
            if (runLen == 0)
                break;

            BindMaterialByIndex(cmdList, descriptorAllocator, mat, lastMat);

            const UINT cmdIndex = batch.firstSubmesh + runStart;
            const UINT64 argOffset = (UINT64)cmdIndex * sizeof(IndirectCommand);
            cmdList->ExecuteIndirect(
                cmdSig, runLen,
                frame.drawCmdBuffer.Get(), argOffset,
                nullptr, 0);

            ++mLastStats.eiCalls;
            if (runLen > 1)
                ++mLastStats.multiEiRuns;
        }
    }
}

// ---------------------------------------------------------------------------
// Step D: Hierarchical-Z (triple-buffered ring)
// ---------------------------------------------------------------------------
bool RenderSystem::IsHiZSampleReady() const
{
    // GPU가 최대 (kHiZRingSize-1) 프레임 뒤처질 수 있으므로
    // 링을 한 바퀴 채운 뒤에만 샘플 허용
    return mHiZBuildCount >= kHiZRingSize
        && mHiZRing[0].texture != nullptr;
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderSystem::GetHiZSampleSrvGpu() const
{
    if (!IsHiZSampleReady())
    {
        if (mDummyHiZSrv.Index != UINT_MAX)
            return mDummyHiZSrv.GPU;
        return {};
    }
    // mHiZWriteSlot = 다음에 쓸 칸
    // 마지막 기록 = writeSlot-1, 안전한 샘플 = writeSlot+1 (= 2프레임 전, ring=3)
    const UINT readSlot = (mHiZWriteSlot + 1u) % kHiZRingSize;
    return mHiZRing[readSlot].srv.GPU;
}

void RenderSystem::DestroyHiZResources(DescriptorAllocator* alloc)
{
    if (alloc)
    {
        for (UINT i = 0; i < kHiZRingSize; ++i)
        {
            if (mHiZRing[i].srv.Index != UINT_MAX)
            {
                alloc->Free(mHiZRing[i].srv);
                mHiZRing[i].srv = {};
            }
            if (mHiZRing[i].uav.Index != UINT_MAX)
            {
                alloc->Free(mHiZRing[i].uav);
                mHiZRing[i].uav = {};
            }
            mHiZRing[i].texture.Reset();
            mHiZRing[i].state = D3D12_RESOURCE_STATE_COMMON;
        }
    }
    else
    {
        for (UINT i = 0; i < kHiZRingSize; ++i)
        {
            mHiZRing[i].texture.Reset();
            mHiZRing[i].srv = {};
            mHiZRing[i].uav = {};
            mHiZRing[i].state = D3D12_RESOURCE_STATE_COMMON;
        }
    }
    mHiZWidth = mHiZHeight = 0;
    mHiZMipCount = 1;
    mHiZWriteSlot = 0;
    mHiZBuildCount = 0;
}

void RenderSystem::EnsureDummyHiZSrv(DescriptorAllocator* alloc)
{
    if (!mDevice || !alloc)
        return;
    if (mDummyHiZSrv.Index != UINT_MAX && mDummyHiZTexture)
        return;

    if (!mDummyHiZTexture)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = 1;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &d,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&mDummyHiZTexture)));
    }

    if (mDummyHiZSrv.Index == UINT_MAX)
    {
        mDummyHiZSrv = alloc->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = 1;
        mDevice->CreateShaderResourceView(mDummyHiZTexture.Get(), &srv, mDummyHiZSrv.CPU);
    }
}

void RenderSystem::PrepareHiZForSceneSize(DescriptorAllocator* alloc, UINT width, UINT height)
{
    // Caller must guarantee GPU idle (e.g. Flush after SceneViewport::Resize).
    width = (std::max)(1u, width);
    height = (std::max)(1u, height);
    if (mHiZRing[0].texture && mHiZWidth == width && mHiZHeight == height)
        return;

    DestroyHiZResources(alloc);
    // Actual create deferred to BuildHiZ / EnsureHiZResources
    mHiZWidth = 0;
    mHiZHeight = 0;
}

void RenderSystem::EnsureHiZResources(DescriptorAllocator* alloc, UINT width, UINT height)
{
    if (!mDevice || !alloc)
        return;

    width = (std::max)(1u, width);
    height = (std::max)(1u, height);

    if (mHiZRing[0].texture && mHiZWidth == width && mHiZHeight == height)
        return;

    // Size mismatch with live resources: do NOT destroy in-flight (TDR).
    // Wait for PrepareHiZForSceneSize after GPU flush.
    if (mHiZRing[0].texture)
    {
        static bool sLogged = false;
        if (!sLogged)
        {
            OutputDebugStringA(
                "[RenderSystem] HiZ size mismatch mid-flight; skip recreate until GPU idle prepare.\n");
            sLogged = true;
        }
        return;
    }

    mSrvAlloc = alloc;
    mHiZMipCount = 1;
    mHiZWidth = width;
    mHiZHeight = height;
    mHiZWriteSlot = 0;
    mHiZBuildCount = 0;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MostDetailedMip = 0;
    srv.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Texture2D.MipSlice = 0;

    for (UINT i = 0; i < kHiZRingSize; ++i)
    {
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&mHiZRing[i].texture)));
        mHiZRing[i].state = D3D12_RESOURCE_STATE_COMMON;

        mHiZRing[i].srv = alloc->Allocate();
        mDevice->CreateShaderResourceView(mHiZRing[i].texture.Get(), &srv, mHiZRing[i].srv.CPU);

        mHiZRing[i].uav = alloc->Allocate();
        mDevice->CreateUnorderedAccessView(
            mHiZRing[i].texture.Get(), nullptr, &uav, mHiZRing[i].uav.CPU);
    }

    EnsureDummyHiZSrv(alloc);
}

void RenderSystem::BuildHiZ(
    ID3D12GraphicsCommandList* cmdList,
    DescriptorAllocator* descriptorAllocator,
    ID3D12Resource* sceneDepth,
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSrvCpu,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvGpu,
    UINT width, UINT height)
{
    (void)sceneDepth;
    (void)sceneDepthSrvCpu;

    mLastStats.didBuildHiZ = false;
    mLastStats.hizMs = 0.f;

    if (!cmdList || !descriptorAllocator || !mDevice)
        return;
    if (!mHiZCopyPSO)
        return;
    if (width == 0 || height == 0)
        return;
    if (sceneDepthSrvGpu.ptr == 0)
        return;

    const auto t0 = std::chrono::high_resolution_clock::now();
    mSrvAlloc = descriptorAllocator;
    EnsureHiZResources(descriptorAllocator, width, height);

    // Still wrong size / not created (wait for PrepareHiZ after flush)
    if (!mHiZRing[0].texture || mHiZWidth != width || mHiZHeight != height)
        return;

    ID3D12RootSignature* hizRS =
        RootSignatureManager::Get().GetRootSignature(RootSignatureType::HiZBuild);
    HiZSlot& slot = mHiZRing[mHiZWriteSlot];
    if (!hizRS || !slot.texture || slot.uav.Index == UINT_MAX)
        return;

    ID3D12DescriptorHeap* heaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(hizRS);

    // 이 슬롯만 쓰기 — 샘플 중인 다른 슬롯과 분리
    if (slot.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            slot.texture.Get(), slot.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        slot.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    uint32_t consts[8] = { width, height, mHiZWidth, mHiZHeight, 0, 0, 0, 0 };
    cmdList->SetComputeRoot32BitConstants(0, 8, consts, 0);
    cmdList->SetComputeRootDescriptorTable(1, sceneDepthSrvGpu);
    cmdList->SetComputeRootDescriptorTable(2, slot.uav.GPU);
    cmdList->SetPipelineState(mHiZCopyPSO.Get());
    cmdList->Dispatch((mHiZWidth + 7) / 8, (mHiZHeight + 7) / 8, 1);

    {
        D3D12_RESOURCE_BARRIER b[2];
        b[0] = CD3DX12_RESOURCE_BARRIER::UAV(slot.texture.Get());
        b[1] = CD3DX12_RESOURCE_BARRIER::Transition(
            slot.texture.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(2, b);
        slot.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    mHiZWriteSlot = (mHiZWriteSlot + 1u) % kHiZRingSize;
    ++mHiZBuildCount;

    mLastStats.didBuildHiZ = true;
    mLastStats.hizMs = ElapsedMs(t0);
    mLastStats.hizMips = mHiZMipCount;
    mLastStats.hizValid = IsHiZSampleReady();
}
