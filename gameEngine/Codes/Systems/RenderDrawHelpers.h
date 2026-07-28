#pragma once
// =============================================================================
// RenderDrawHelpers
// Free helpers shared by RenderSystem*.cpp (PSO keys, materials, GPU fill, timing).
// No RenderSystem member state — safe to use from any path implementation file.
// =============================================================================

#include <DirectXMath.h>
#include <chrono>
#include <cstring>
#include <cmath>
#include <d3d12.h>

#include "RenderSystem.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "PipelineStateManager.h"
#include "ComponentStruct.h"
#include "constantStruct.h"
#include "d3dUtil.h"
#include "d3dx12.h"
#include "../Structs/IndirectDrawStructs.h"
#include "../Structs/RenderLimits.h"

using namespace DirectX;
using namespace ECS;

namespace RenderDrawHelpers
{
inline float ElapsedMs(std::chrono::high_resolution_clock::time_point t0)
{
    using namespace std::chrono;
    return duration<float, std::milli>(high_resolution_clock::now() - t0).count();
}

// Scene panel / play view render at 4x MSAA (must match SceneViewport::kMsaaCount).
inline constexpr UINT kSceneSampleCount = 4;

inline ID3D12PipelineState* GetScenePso(const PSOKey& key, ID3D12RootSignature* rs)
{
    return PipelineStateManager::Get().GetOrCreatePSO(
        key, rs, nullptr, false, kSceneSampleCount);
}

inline PSOKey MakeOpaquePsoKey(const char* shaderName)
{
    PSOKey key{};
    key.shaderName = shaderName;
    key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    // Stencil=1 under solid geometry so outline is silhouette-only.
    key.depthStencilDesc.StencilEnable = TRUE;
    key.depthStencilDesc.StencilReadMask = 0xFF;
    key.depthStencilDesc.StencilWriteMask = 0xFF;
    key.depthStencilDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    key.depthStencilDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    key.depthStencilDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    key.depthStencilDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    key.depthStencilDesc.BackFace = key.depthStencilDesc.FrontFace;
    return key;
}

// Inverted hull: expand along normals, draw back faces where stencil != solid.
inline PSOKey MakeOutlinePsoKey(const char* shaderName)
{
    PSOKey key{};
    key.shaderName = shaderName;
    key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    key.rasterizerDesc.CullMode = D3D12_CULL_MODE_FRONT;
    key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    key.depthStencilDesc.DepthEnable = TRUE;
    key.depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    key.depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    key.depthStencilDesc.StencilEnable = TRUE;
    key.depthStencilDesc.StencilReadMask = 0xFF;
    key.depthStencilDesc.StencilWriteMask = 0x00;
    key.depthStencilDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL;
    key.depthStencilDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    key.depthStencilDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    key.depthStencilDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    key.depthStencilDesc.BackFace = key.depthStencilDesc.FrontFace;
    return key;
}

inline void DrawMeshOutlineIndexed(
    ID3D12GraphicsCommandList* cmdList, Mesh* mesh, UINT instanceCount)
{
    if (!mesh || !mesh->indexBuffer || mesh->indexCount == 0 || instanceCount == 0)
        return;
    auto vbv = mesh->VertexBufferView();
    auto ibv = mesh->IndexBufferView();
    cmdList->IASetVertexBuffers(0, 1, &vbv);
    cmdList->IASetIndexBuffer(&ibv);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawIndexedInstanced(mesh->indexCount, instanceCount, 0, 0, 0);
}

inline void StoreWorldTransposed(const XMMATRIX& world, float out[16])
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixTranspose(world));
    std::memcpy(out, &m, sizeof(XMFLOAT4X4));
}

inline void GpuStyleComposeStore(
    float px, float py, float pz,
    float pitch, float yaw, float roll,
    float sx, float sy, float sz,
    float out[16])
{
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float cy = cosf(yaw), syw = sinf(yaw);
    const float cr = cosf(roll), sr = sinf(roll);

    const float r11 = cr * cy + sr * sp * syw;
    const float r12 = sr * cp;
    const float r13 = sr * sp * cy - cr * syw;
    const float r21 = cr * sp * syw - sr * cy;
    const float r22 = cr * cp;
    const float r23 = sr * syw + cr * sp * cy;
    const float r31 = cp * syw;
    const float r32 = -sp;
    const float r33 = cp * cy;

    const float sr11 = sx * r11, sr12 = sx * r12, sr13 = sx * r13;
    const float sr21 = sy * r21, sr22 = sy * r22, sr23 = sy * r23;
    const float sr31 = sz * r31, sr32 = sz * r32, sr33 = sz * r33;

    XMFLOAT4X4 W{};
    W._11 = sr11; W._12 = sr12; W._13 = sr13; W._14 = 0.f;
    W._21 = sr21; W._22 = sr22; W._23 = sr23; W._24 = 0.f;
    W._31 = sr31; W._32 = sr32; W._33 = sr33; W._34 = 0.f;
    W._41 = px;   W._42 = py;   W._43 = pz;   W._44 = 1.f;

    XMFLOAT4X4 stored;
    XMStoreFloat4x4(&stored, XMMatrixTranspose(XMLoadFloat4x4(&W)));
    std::memcpy(out, &stored, sizeof(stored));
}

inline void ValidateComposeWorldMatchesCpu()
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

inline Material* ResolveSubmeshMaterial(const SubmeshGeometry& sub)
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

inline bool HasEntitySubMaterialOverride(Entity e)
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

inline Material* GetEntityMainMaterialOverride(Entity e)
{
    const EntityMaterialData* data = MaterialManager::Get().GetEntityMaterialData(e);
    if (!data || data->mainMaterialName.empty())
        return nullptr;
    auto mat = MaterialManager::Get().GetMaterial(data->mainMaterialName);
    if (!mat || !mat->HasValidTexture())
        return nullptr;
    return mat.get();
}

inline Material* ResolveBatchMaterial(Material* mainOverride, const SubmeshGeometry& sub)
{
    if (mainOverride && mainOverride->HasValidTexture())
        return mainOverride;
    return ResolveSubmeshMaterial(sub);
}

inline void DrawEntityWithResolvedMaterials(
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
        if (material->mTextureHandle.Index != UINT_MAX)
            cmdList->SetGraphicsRootDescriptorTable(2, material->mTextureHandle.GPU);
        cmdList->DrawIndexedInstanced(
            sub.IndexCount, 1,
            sub.StartIndexLocation,
            sub.BaseVertexLocation, 0);
    }
}

inline bool BindMaterialByIndex(
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

inline void FillMotionFromEntity(GpuMotion& out, GravityComponent* gravity)
{
    out = {};
    if (!gravity || !gravity->enabled)
        return;
    out.linearVelocity[0] = gravity->velocity.x;
    out.linearVelocity[1] = gravity->velocity.y;
    out.linearVelocity[2] = gravity->velocity.z;
    out.angularVelocity[0] = gravity->angularVelocity.x;
    out.angularVelocity[1] = gravity->angularVelocity.y;
    out.angularVelocity[2] = gravity->angularVelocity.z;
    out.gravity = gravity->strength;
    out.flags = 1u;
    out.pad0 = out.pad1 = 0.f;
    out.pad2 = out.pad3 = 0;
}

inline void FillTransformFromEntity(
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

inline void FillSourceFromEntity(
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

inline void RestoreLodStats(
    GpuDrivenFrameStats& st,
    bool en, uint32_t culled, uint32_t switches, const uint32_t levels[4], float ms)
{
    st.lodEnabled = en;
    st.lodCulled = culled;
    st.lodSwitches = switches;
    st.lodMs = ms;
    for (int i = 0; i < 4; ++i)
        st.lodLevelCounts[i] = levels[i];
}

} // namespace RenderDrawHelpers
