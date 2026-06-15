#include <DirectXMath.h>
#include "RenderSystem.h"
#include "MaterialManager.h"
#include "MeshManager.h"
#include "RootSignatureManager.h"
#include "PipelineStateManager.h"
#include "constantStruct.h"

using namespace DirectX;

void RenderSystem::createCBV(ID3D12Device* device,
    std::vector<std::unique_ptr<FrameResource>>& frameResources,
    int gNumFrameResources,
    DescriptorAllocator& descriptorAllocator,
    Entity entity,                    
    ECS::World& world)                
{
    // 새 ECS 방식으로 컴포넌트 가져오기
    RenderableComponent* rend = world.GetComponent<RenderableComponent>(entity);
    if (!rend) return;   // 안전장치

    // 엔티티 인덱스에 맞춰 벡터 크기 확장
    if (mEntityCBVHandles.size() <= entity)
        mEntityCBVHandles.resize(entity + 1);

    mEntityCBVHandles[entity].resize(gNumFrameResources);

    for (int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
    {
        auto objectCB = frameResources[frameIndex]->ObjectCB->Resource();

        auto handle = descriptorAllocator.Allocate();
        mEntityCBVHandles[entity][frameIndex] = handle;

        UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = objectCB->GetGPUVirtualAddress() + (UINT64)rend->objectCBIndex * objCBByteSize;
        cbvDesc.SizeInBytes = objCBByteSize;

        device->CreateConstantBufferView(&cbvDesc, handle.CPU);
    }
}

/*void RenderSystem::render(Registry& registry,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    auto entities = registry.view<TransformComponent, RenderableComponent>();

    // Descriptor Heap 설정
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    for (Entity e : entities)
    {
        auto& tf = registry.getComponent<TransformComponent>(e);
        auto& rend = registry.getComponent<RenderableComponent>(e);

        if (!rend.visible) continue;

        // 1. 행렬 계산 및 CB 데이터 업데이트
        XMMATRIX world = tf.GetWorldMatrix();
        XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
        XMMATRIX wvp = XMMatrixMultiply(world, viewProj);

        ObjectConstants objConst{};
        XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));
        currentFrameResource->ObjectCB->CopyData(rend.objectCBIndex, objConst);

        // 2. Mesh
        Mesh* mesh = MeshManager::Get().GetMesh(rend.meshName);
        if (!mesh || mesh->DrawArgs.empty()) continue;

        auto vbv = mesh->VertexBufferView();
        auto ibv = mesh->IndexBufferView();

        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
        cmdList->SetGraphicsRootSignature(sceneRS);

        PSOKey key;
        key.shaderName = "object";
        key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);
        cmdList->SetPipelineState(pso);

        // =====================================================
        // 서브메시별 Material 바인딩 + 그리기
        // =====================================================
        for (auto& pair : mesh->DrawArgs)
        {
            const auto& sub = pair.second;
            Material* material = sub.material;
            


            // CBV 바인딩 (Root Parameter 0)
            if (!mEntityCBVHandles.empty() &&
                e < mEntityCBVHandles.size() &&
                currentFrameIndex < mEntityCBVHandles[e].size())
            {
                cmdList->SetGraphicsRootDescriptorTable(0, mEntityCBVHandles[e][currentFrameIndex].GPU);
            }

            // Texture 바인딩 (Root Parameter 1)
            cmdList->SetGraphicsRootDescriptorTable(1, material->mTextureHandle.GPU);

            // 서브메시 그리기
            cmdList->DrawIndexedInstanced(
                sub.IndexCount,
                1,
                sub.StartIndexLocation,
                sub.BaseVertexLocation,
                0
            );
        }
    }
}*/

/*void RenderSystem::render(Registry& registry,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    auto entities = registry.view<TransformComponent, RenderableComponent>();

    // Descriptor Heap 설정 (한 번만)
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    std::vector<DrawCall> drawCalls;
    drawCalls.reserve(2048);

    // ============================================
    // 1단계: DrawCall 수집
    // ============================================
    for (Entity e : entities)
    {
        auto& tf = registry.getComponent<TransformComponent>(e);
        auto& rend = registry.getComponent<RenderableComponent>(e);

        if (!rend.visible) continue;

        Mesh* mesh = MeshManager::Get().GetMesh(rend.meshName);
        if (!mesh || mesh->DrawArgs.empty()) continue;

        // 미리 PSOKey 생성 (같은 key면 같은 PSO)
        PSOKey psoKey;
        psoKey.shaderName = "object";
        psoKey.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoKey.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoKey.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

        ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);
        ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(psoKey, sceneRS);

        // Transform & CB 업데이트는 여기서 해도 되고, 나중에 모아서 해도 됨
        XMMATRIX world = tf.GetWorldMatrix();
        XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
        XMMATRIX wvp = XMMatrixMultiply(world, viewProj);

        ObjectConstants objConst{};
        XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));
        currentFrameResource->ObjectCB->CopyData(rend.objectCBIndex, objConst);

        for (auto& pair : mesh->DrawArgs)
        {
            DrawCall dc{};
            dc.entity = e;
            dc.mesh = mesh;
            dc.submesh = &pair.second;
            dc.material = pair.second.material;
            dc.rootSignature = sceneRS;
            dc.pso = pso;

            drawCalls.push_back(dc);
        }
    }

    // ============================================
    // 2단계: 정렬 (RootSig → PSO 순)
    // ============================================
    std::sort(drawCalls.begin(), drawCalls.end(), [](const DrawCall& a, const DrawCall& b)
        {
            if (a.rootSignature != b.rootSignature)
                return a.rootSignature < b.rootSignature;
            return a.pso < b.pso;
        });

    // ============================================
    // 3단계: 정렬된 순서대로 그리기 (state change 최소화)
    // ============================================
    ID3D12RootSignature* lastRS = nullptr;
    ID3D12PipelineState* lastPSO = nullptr;

    for (const auto& dc : drawCalls)
    {
        // Root Signature 변경 최소화
        if (dc.rootSignature != lastRS)
        {
            cmdList->SetGraphicsRootSignature(dc.rootSignature);
            lastRS = dc.rootSignature;
        }

        // PSO 변경 최소화
        if (dc.pso != lastPSO)
        {
            cmdList->SetPipelineState(dc.pso);
            lastPSO = dc.pso;
        }

        // Vertex / Index Buffer (같은 Mesh가 연속이면 최적화 가능하지만 일단 매번)
        auto vbv = dc.mesh->VertexBufferView();
        auto ibv = dc.mesh->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // CBV 바인딩
        if (currentFrameIndex < mEntityCBVHandles[dc.entity].size())
        {
            cmdList->SetGraphicsRootDescriptorTable(0, mEntityCBVHandles[dc.entity][currentFrameIndex].GPU);
        }

        // Texture 바인딩
        cmdList->SetGraphicsRootDescriptorTable(1, dc.material->mTextureHandle.GPU);

        // Draw
        cmdList->DrawIndexedInstanced(
            dc.submesh->IndexCount,
            1,
            dc.submesh->StartIndexLocation,
            dc.submesh->BaseVertexLocation,
            0
        );
    }
}*/

/*void RenderSystem::render1(Registry& registry,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    auto entities = registry.view<TransformComponent, RenderableComponent>();

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    ID3D12RootSignature* lastRS = nullptr;
    ID3D12PipelineState* lastPSO = nullptr;

    for (Entity e : entities)
    {
        auto& tf = registry.getComponent<TransformComponent>(e);
        auto& rend = registry.getComponent<RenderableComponent>(e);

        if (!rend.visible || !rend.mesh) continue;   // mesh가 null이면 스킵

        // 1. Transform → CB 업데이트
        XMMATRIX world = tf.GetWorldMatrix();
        XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
        XMMATRIX wvp = XMMatrixMultiply(world, viewProj);

        ObjectConstants objConst{};
        XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));
        currentFrameResource->ObjectCB->CopyData(rend.objectCBIndex, objConst);

        // 2. Mesh (string lookup 제거!)
        auto vbv = rend.mesh->VertexBufferView();
        auto ibv = rend.mesh->IndexBufferView();

        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // RootSignature + PSO (Last State 캐싱)
        ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);

        PSOKey key{};
        key.shaderName = "object";
        key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

        ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);

        if (sceneRS != lastRS)
        {
            cmdList->SetGraphicsRootSignature(sceneRS);
            lastRS = sceneRS;
        }
        if (pso != lastPSO)
        {
            cmdList->SetPipelineState(pso);
            lastPSO = pso;
        }

        // 서브메시 그리기
        for (auto& pair : rend.mesh->DrawArgs)
        {
            const auto& sub = pair.second;

            // sub.material (raw) 우선, 없으면 rend.material 사용
            Material* material = sub.material ? sub.material : rend.material.get();

            if (!material) continue;

            // CBV 바인딩
            if (!mEntityCBVHandles.empty() &&
                e < mEntityCBVHandles.size() &&
                currentFrameIndex < mEntityCBVHandles[e].size())
            {
                cmdList->SetGraphicsRootDescriptorTable(0, mEntityCBVHandles[e][currentFrameIndex].GPU);
            }

            // Texture 바인딩
            cmdList->SetGraphicsRootDescriptorTable(1, material->mTextureHandle.GPU);

            // Draw
            cmdList->DrawIndexedInstanced(
                sub.IndexCount,
                1,
                sub.StartIndexLocation,
                sub.BaseVertexLocation,
                0
            );
        }
    }
}*/

void RenderSystem::render(ECS::World& world,
    ID3D12GraphicsCommandList* cmdList,
    FrameResource* currentFrameResource,
    DescriptorAllocator* descriptorAllocator,
    int currentFrameIndex,
    const XMMATRIX& viewMatrix,
    const XMMATRIX& projMatrix)
{
    // Descriptor Heap 설정
    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorAllocator->GetHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    ID3D12RootSignature* lastRS = nullptr;
    ID3D12PipelineState* lastPSO = nullptr;

    // === 새 ECS 방식으로 변경 ===
    world.ForEach<TransformComponent, RenderableComponent>(
        [&](Entity e, TransformComponent& tf, RenderableComponent& rend)
        {
            if (!rend.visible || !rend.mesh) return;

            // 1. Transform → CB 업데이트
            XMMATRIX worldMat = tf.GetWorldMatrix();
            XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projMatrix);
            XMMATRIX wvp = XMMatrixMultiply(worldMat, viewProj);

            ObjectConstants objConst{};
            XMStoreFloat4x4(&objConst.WorldViewProj, XMMatrixTranspose(wvp));
            currentFrameResource->ObjectCB->CopyData(rend.objectCBIndex, objConst);

            // 2. Mesh 바인딩
            auto vbv = rend.mesh->VertexBufferView();
            auto ibv = rend.mesh->IndexBufferView();

            cmdList->IASetVertexBuffers(0, 1, &vbv);
            cmdList->IASetIndexBuffer(&ibv);
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // 3. RootSignature + PSO (Last State 캐싱)
            ID3D12RootSignature* sceneRS = RootSignatureManager::Get().GetRootSignature(RootSignatureType::Scene);

            PSOKey key{};
            key.shaderName = "object";
            key.blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            key.rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            key.depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

            ID3D12PipelineState* pso = PipelineStateManager::Get().GetOrCreatePSO(key, sceneRS);

            if (sceneRS != lastRS) { cmdList->SetGraphicsRootSignature(sceneRS); lastRS = sceneRS; }
            if (pso != lastPSO) { cmdList->SetPipelineState(pso); lastPSO = pso; }

            // 4. Material 바인딩 + 그리기
            for (auto& pair : rend.mesh->DrawArgs)
            {
                const auto& sub = pair.second;
                Material* material = sub.material ? sub.material : rend.material.get();
                if (!material) continue;

                // CBV 바인딩
                if (e < mEntityCBVHandles.size() &&
                    currentFrameIndex < mEntityCBVHandles[e].size())
                {
                    cmdList->SetGraphicsRootDescriptorTable(0, mEntityCBVHandles[e][currentFrameIndex].GPU);
                }

                // Texture 바인딩
                cmdList->SetGraphicsRootDescriptorTable(1, material->mTextureHandle.GPU);

                cmdList->DrawIndexedInstanced(
                    sub.IndexCount, 1,
                    sub.StartIndexLocation,
                    sub.BaseVertexLocation, 0);
            }
        });
}