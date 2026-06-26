#pragma once
#include <string>
#include <DirectXMath.h>
#include <optional>
#include <DirectXCollision.h>

#include "World.h"
#include "RenderSystem.h"
#include "BoundsSystem.h"
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

    // 매 프레임 업데이트 (나중에 시스템들 추가 예정)
    void Update();

    // AABB 업데이트 시스템
    void UpdateBounds();

    // 렌더링
    void Render(ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        int currentFrameIndex,
        const XMMATRIX& viewMatrix,
        const XMMATRIX& projMatrix);

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

    Entity PickObject(int mouseX, int mouseY, float clientWidth, float clientHeight,
                      const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

    Entity GetSelectedEntity() const { return mSelectedEntity; }
    void SetSelectedEntity(Entity e) { mSelectedEntity = e; }

    RenderableComponent* GetRenderable(Entity entity);
    void SetEntityMainMaterial(Entity entity, const std::string& materialName);
    void SetEntitySubMaterial(Entity entity, const std::string& submeshKey, const std::string& materialName);
    std::string GetEntityMainMaterial(Entity entity) const;
    std::string GetEntitySubMaterial(Entity entity, const std::string& submeshKey) const;

    void RotateSelected(float dYaw, float dPitch);
    void MoveSelectedViewRelative(float forward, float right, float up, float speed, const XMMATRIX& view);

    void Shutdown();
private:
    ECS::World mWorld;
    RenderSystem mRenderSystem;
    BoundsSystem mBoundsSystem;
    ResourceManager* mResourceManager = nullptr;

    ID3D12Device* mDevice = nullptr;
    std::vector<std::unique_ptr<FrameResource>>* mFrameResources = nullptr;
    int mGNumFrameResources = 0;
    DescriptorAllocator* mDescriptorAllocator = nullptr;

    uint32_t mNextObjectCBIndex = 0;

    Entity mSelectedEntity = INVALID_ENTITY;
};