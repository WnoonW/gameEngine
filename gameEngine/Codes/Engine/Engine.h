#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>
#include <optional>
#include <DirectXCollision.h>

#include "World.h"
#include "RenderSystem.h"
#include "BoundsSystem.h"
#include "GravitySystem.h"
#include "CollisionSystem.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"
#include "ResourceManager.h"
#include "ComponentStruct.h"

using namespace DirectX;

class Engine
{
public:
    Engine();
    ~Engine();

    // 초기화
    bool Initialize(ID3D12Device* device,
        std::vector<std::unique_ptr<FrameResource>>& frameResources,
        int gNumFrameResources,
        DescriptorAllocator& descriptorAllocator);

    void Update(float deltaTime);

    // AABB 업데이트 시스템
    void UpdateBounds();

    // 렌더링
    void Render(ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        int currentFrameIndex,
        const XMMATRIX& viewMatrix,
        const XMMATRIX& projMatrix);

    void SetRenderPath(RenderPath path);
    RenderPath GetRenderPath() const;
    bool IsComputeIndirectReady() const;
    void SetAutoRenderPathEnabled(bool enabled);
    bool IsAutoRenderPathEnabled() const;
    void SetGpuFrustumCullEnabled(bool enabled);
    bool IsGpuFrustumCullEnabled() const;
    void SetGpuOcclusionEnabled(bool enabled);
    bool IsGpuOcclusionEnabled() const;
    const GpuDrivenFrameStats& GetLastFrameStats() const;

    // Scene RT 리사이즈 후 GPU idle 상태에서 Hi-Z 재할당
    void PrepareHiZForSceneSize(UINT width, UINT height);

    // Scene End 이후: previous-frame Hi-Z 갱신
    void BuildHiZ(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* sceneDepth,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSrvCpu,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvGpu,
        UINT width, UINT height);

    // 엔티티 생성 (이름 기반)
    Entity CreateRenderableEntity(const std::string& meshName,
        const std::string& materialName,
        XMFLOAT3 position = { 0.0f, 0.0f, 0.0f });

    // ECS CameraComponent + TransformComponent 로부터 View/Proj 계산
    // (View는 rotation 기반, Proj는 CameraComponent에서)
    bool GetMainCameraViewProj(DirectX::XMMATRIX& outView, DirectX::XMMATRIX& outProj, float aspectRatio);

    // PassCB 채우기 (ECS camera component 활용)
    // view / proj 는 호출측에서 넘겨주거나 내부 계산
    void FillPassCB(FrameResource* currentFrame,
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& proj,
        float clientWidth, float clientHeight,
        float aspectRatio,
        float totalTime, float deltaTime);

    // 카메라 엔티티 생성 편의 함수
    Entity CreateMainCamera(XMFLOAT3 position = { 0.0f, 5.0f, -10.0f },
        float fov = DirectX::XM_PIDIV4,
        float nearZ = 0.1f, float farZ = 1000.0f);

    // ECS 카메라의 Transform 업데이트 (orbit 컨트롤 연동용)
    void SetCameraTransform(Entity camEntity, const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& rotation);

    // 클릭 피킹. setSelection=true면 선택 갱신 (additive면 기존 유지+추가)
    Entity PickObject(int mouseX, int mouseY, float clientWidth, float clientHeight,
                      const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj,
                      bool setSelection = true, bool additive = false);

    // Scene 픽셀 좌표 직사각형으로 AABB 투영 교차 다중 선택
    // rect: min/max in scene image space (same as pick coords)
    size_t SelectObjectsInRect(
        float rectMinX, float rectMinY, float rectMaxX, float rectMaxY,
        float sceneWidth, float sceneHeight,
        const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj,
        bool additive = false);

    Entity GetSelectedEntity();
    std::vector<Entity> GetSelectedEntities();
    size_t GetSelectedCount();
    void SetSelectedEntity(Entity e);
    void AddSelectedEntity(Entity e);
    void ToggleSelectedEntity(Entity e);
    void ClearSelection();
    bool IsEntitySelected(Entity entity);

    RenderableComponent* GetRenderable(Entity entity);
    void SetEntityMainMaterial(Entity entity, const std::string& materialName);
    void SetEntitySubMaterial(Entity entity, const std::string& submeshKey, const std::string& materialName);
    std::string GetEntityMainMaterial(Entity entity) const;
    std::string GetEntitySubMaterial(Entity entity, const std::string& submeshKey) const;

    void RotateSelected(float dYaw, float dPitch);
    void MoveSelectedViewRelative(float forward, float right, float up, float speed, const XMMATRIX& view);
    void MoveSelectedPlanar(float forward, float right, float up, float speed,
        const XMFLOAT3& horizForward, const XMFLOAT3& horizRight);

    TransformComponent* GetTransform(Entity entity);
    bool HasGravityComponent(Entity entity);
    GravityComponent* GetGravityComponent(Entity entity);
    void SetEntityGravityEnabled(Entity entity, bool enabled);

    bool HasCollisionComponent(Entity entity);
    CollisionComponent* GetCollisionComponent(Entity entity);
    void SetEntityCollisionEnabled(Entity entity, bool enabled);

    // 씬에 렌더 가능한 오브젝트 수 (Renderable 컴포넌트 보유 엔티티)
    size_t GetRenderableObjectCount();

    // Hierarchy UI용: 렌더 가능 엔티티 ID 목록 (오름차순, 캐시됨)
    const std::vector<Entity>& GetRenderableEntities();

    // 생성/삭제 등으로 목록·인스턴스 캐시 무효화
    void NotifyRenderableListChanged();

    void Shutdown();
private:
    void RebuildRenderableListCacheIfNeeded();

    ECS::World mWorld;
    RenderSystem mRenderSystem;
    BoundsSystem mBoundsSystem;
    GravitySystem mGravitySystem;
    CollisionSystem mCollisionSystem;
    ResourceManager* mResourceManager = nullptr;

    ID3D12Device* mDevice = nullptr;
    std::vector<std::unique_ptr<FrameResource>>* mFrameResources = nullptr;
    int mGNumFrameResources = 0;
    DescriptorAllocator* mDescriptorAllocator = nullptr;

    uint32_t mNextObjectCBIndex = 0;
    uint32_t mMaxObjectCBSlots = 8192;

    std::vector<Entity> mRenderableListCache;
    bool mRenderableListDirty = true;
};