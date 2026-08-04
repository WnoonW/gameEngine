#pragma once
#include <string>
#include <vector>
#include <DirectXMath.h>
#include <optional>
#include <DirectXCollision.h>

#include "World.h"
#include "RenderSystem.h"
#include "UiSystem.h"
#include "BoundsSystem.h"
#include "GravitySystem.h"
#include "CollisionSystem.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"
#include "ResourceManager.h"
#include "ComponentStruct.h"
#include "UiPresetSerializer.h"
#include "SceneSerializer.h"
#include <unordered_map>
#include <cstdint>

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

    // 렌더링 (3D world)
    void Render(ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        int currentFrameIndex,
        const XMMATRIX& viewMatrix,
        const XMMATRIX& projMatrix);

    // In-game UI + VFX billboards (after 3D, before SceneViewport::End). No ImGui.
    void RenderUi(
        ID3D12GraphicsCommandList* cmdList,
        UINT screenWidth, UINT screenHeight,
        const XMMATRIX& viewMatrix,
        const XMMATRIX& projMatrix);

    Entity CreateUiImage(
        UiSpaceMode mode,
        const std::string& materialName,
        DirectX::XMFLOAT2 size,
        DirectX::XMFLOAT3 worldOrScreenPos = { 0, 0, 0 },
        DirectX::XMFLOAT2 anchor = { 0.5f, 0.5f });

    // ScreenAlways: top-left (minX,minY) + size in design-space pixels.
    // designW/H = authoring resolution (0 = use engine global design res).
    Entity CreateUiImageScreenRect(
        const std::string& materialName,
        float minX, float minY, float maxX, float maxY,
        float designW = 0.0f, float designH = 0.0f);

    // UI canvas scaler (screen-space elements).
    // Default scale mode applied when creating new UI (per-element can override).
    void SetUiScaleMode(UiScaleMode mode);
    UiScaleMode GetUiScaleMode() const;
    void SetUiDesignResolution(float width, float height);
    void GetUiDesignResolution(float& outW, float& outH) const;

    void DestroyUiEntity(Entity e);
    // active: logic / Conditional draw / pointer. visible: render (all modes). See ComponentStruct.h.
    void SetUiActive(Entity e, bool active);
    void SetUiVisible(Entity e, bool visible);
    const std::vector<Entity>& GetUiEntities();
    void DestroyAllUiEntities();
    UiElementComponent* GetUiElement(Entity e);
    UiImageComponent* GetUiImage(Entity e);
    // Screen-space UI pick (panel/window pixels). Returns INVALID_ENTITY if none.
    Entity PickUiScreen(float pixelX, float pixelY, UINT screenW, UINT screenH);

    // --- UI presets: save layout → register on scene load → Spawn when needed ---
    // Capture every live UiElement in the world into a preset blob.
    UiPresetData CaptureCurrentUiAsPreset(const std::string& presetName);
    // Write current UI to UiPresets/<name>.uipreset (and register under that name).
    bool SaveCurrentUiAsPreset(const std::string& presetName, std::string* outError = nullptr);
    // Delete .uipreset file, unregister, remove from scene preload (+ destroy instance).
    bool DeleteUiPreset(const std::string& name, std::string* outError = nullptr);
    bool RegisterUiPreset(const std::string& name, const UiPresetData& data);
    bool RegisterUiPresetFromFile(const std::string& nameOrPath, std::string* outError = nullptr);
    void UnregisterUiPreset(const std::string& name);
    void ClearUiPresetRegistry();
    bool HasUiPreset(const std::string& name) const;
    std::vector<std::string> GetRegisteredUiPresetNames() const;

    // Scene preload list (saved as ui_preset=name,visible). Visibility toggled in editor.
    const std::vector<SceneUiPresetEntry>& GetSceneUiPresetList() const { return mSceneUiPresets; }
    void SetSceneUiPresetList(std::vector<SceneUiPresetEntry> entries);
    void AddSceneUiPreset(const std::string& name, bool visible = true);
    void RemoveSceneUiPreset(const std::string& name);
    // Toggle visibility for a scene-listed preset (spawns instance if needed). Saved with scene.
    bool SetSceneUiPresetVisible(const std::string& name, bool visible);
    bool GetSceneUiPresetVisible(const std::string& name) const;
    // Load every name in the scene list into the registry (files under UiPresets/).
    bool PreloadSceneUiPresets(std::string* outError = nullptr);
    // After preload: spawn each listed preset and apply saved visible flags.
    void ApplySceneUiPresetVisibility();

    // Instantiate a registered preset. Returns instance id (>0) or 0 on failure.
    uint32_t SpawnUiPreset(const std::string& presetName, bool startActive = true);
    void SetUiPresetInstanceActive(uint32_t instanceId, bool active);
    void SetUiPresetInstanceVisible(uint32_t instanceId, bool visible);
    void DestroyUiPresetInstance(uint32_t instanceId);
    void DestroyAllUiPresetInstances();
    size_t CountUiPresetInstances(const std::string& presetName = {}) const;

    Entity CreateEffectBillboard(
        const std::string& materialName,
        DirectX::XMFLOAT3 worldPos,
        float size = 1.0f,
        DirectX::XMFLOAT4 color = { 1, 1, 1, 1 },
        bool additive = true,
        float lifetime = -1.0f);

    bool HandleUiPointer(
        float scenePixelX, float scenePixelY,
        UINT screenW, UINT screenH,
        bool leftDown, bool leftPressedThisFrame,
        std::vector<UiClickEvent>* outClicks);

    void SetRenderPath(RenderPath path);
    RenderPath GetRenderPath() const;
    bool IsComputeIndirectReady() const;
    void SetAutoRenderPathEnabled(bool enabled);
    bool IsAutoRenderPathEnabled() const;
    void SetGpuFrustumCullEnabled(bool enabled);
    bool IsGpuFrustumCullEnabled() const;
    void SetGpuOcclusionEnabled(bool enabled);
    bool IsGpuOcclusionEnabled() const;
    void SetGpuMotionEnabled(bool enabled);
    bool IsGpuMotionEnabled() const;
    void SetLodEnabled(bool enabled);
    bool IsLodEnabled() const;
    void SetLodDistanceCullEnabled(bool enabled);
    bool IsLodDistanceCullEnabled() const;
    void SetLodBias(float bias);
    float GetLodBias() const;
    void SetLodCullDistance(float d);
    float GetLodCullDistance() const;
    const GpuDrivenFrameStats& GetLastFrameStats() const;

    // --- Graphics style / lighting (PassCB) ---
    void SetGraphicsStyle(GraphicsStyle style);
    GraphicsStyle GetGraphicsStyle() const { return mGraphicsStyle; }
    void SetToonBands(float bands);
    float GetToonBands() const { return mToonBands; }
    void SetToonOutlineEnabled(bool enabled);
    bool IsToonOutlineEnabled() const { return mToonOutlineEnabled; }
    void SetOutlineWidth(float w);
    float GetOutlineWidth() const { return mOutlineWidth; }
    void SetSunDirection(DirectX::XMFLOAT3 dir);
    DirectX::XMFLOAT3 GetSunDirection() const { return mSunDirection; }
    void SetSunStrength(DirectX::XMFLOAT3 rgb);
    DirectX::XMFLOAT3 GetSunStrength() const { return mSunStrength; }
    void SetAmbientLight(DirectX::XMFLOAT3 rgb);
    DirectX::XMFLOAT3 GetAmbientLight() const { return mAmbientRgb; }
    void SetSpecularPower(float p);
    float GetSpecularPower() const { return mSpecularPower; }

    void SetShadowsEnabled(bool enabled);
    bool IsShadowsEnabled() const { return mShadowsEnabled; }
    void SetShadowBias(float bias);
    float GetShadowBias() const { return mShadowBias; }

    // Depth-only pass into the directional shadow map (call before Scene RT Begin).
    void RenderShadowMap(
        ID3D12GraphicsCommandList* cmdList,
        FrameResource* currentFrameResource,
        int currentFrameIndex,
        const DirectX::XMMATRIX& viewMatrix,
        const DirectX::XMMATRIX& projMatrix);

    // Scene RT 리사이즈 후 GPU idle 상태에서 Hi-Z 재할당
    void PrepareHiZForSceneSize(UINT width, UINT height);

    // Scene End 이후: previous-frame Hi-Z 갱신
    void BuildHiZ(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* sceneDepth,
        D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSrvCpu,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvGpu,
        UINT width, UINT height);

    // 엔티티 생성 (이름 기반).
    // materialName empty = Main Material None (no override).
    Entity CreateRenderableEntity(const std::string& meshName,
        const std::string& materialName = {},
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

    // Screen UI: move layout position (percent or legacy px units). Multi-select applies same delta.
    void MoveSelectedUi(float dPosX, float dPosY);
    // Snap selected mesh AABB faces/centers to other mesh world AABBs (threshold in world units).
    void SnapSelectedMesh(float threshold);
    // Snap selected screen UI to editor canvas L/R/T/B edges only.
    // thresholdPercent: 0..100 of canvas axis (X uses W, Y uses H).
    // Returns true if any selected UI moved due to snap.
    bool SnapSelectedUi(float thresholdPercent, float canvasW, float canvasH);

    TransformComponent* GetTransform(Entity entity);
    bool HasGravityComponent(Entity entity);
    GravityComponent* GetGravityComponent(Entity entity);
    void SetEntityGravityEnabled(Entity entity, bool enabled);
    // F2: gravity param edit → reseed Path3 motion slot
    void NotifyEntityMotionChanged(Entity entity);

    bool HasCollisionComponent(Entity entity);
    CollisionComponent* GetCollisionComponent(Entity entity);
    void SetEntityCollisionEnabled(Entity entity, bool enabled);

    // 씬에 렌더 가능한 오브젝트 수 (Renderable 컴포넌트 보유 엔티티)
    size_t GetRenderableObjectCount();

    // Hierarchy UI용: 렌더 가능 엔티티 ID 목록 (오름차순, 캐시됨)
    const std::vector<Entity>& GetRenderableEntities();

    // 생성/삭제 등으로 목록·인스턴스 캐시 무효화
    void NotifyRenderableListChanged();

    // 렌더 엔티티 삭제 (카메라 등 비-렌더 엔티티는 유지)
    void DestroyRenderableEntity(Entity entity);
    void ClearRenderableEntities();

    // 씬 파일 (.scene) 저장/불러오기
    // packLiveUiIntoFile: embed current live UI into THIS file only (no UiPresets/*.uipreset, no editor list mutation)
    bool SaveSceneToFile(
        const std::string& path,
        const std::string& sceneName = {},
        bool packLiveUiIntoFile = false);
    bool LoadSceneFromFile(const std::string& path, std::string* outError = nullptr);

    void Shutdown();
private:
    void RebuildRenderableListCacheIfNeeded();

    ECS::World mWorld;
    RenderSystem mRenderSystem;
    UiSystem mUiSystem;
    BoundsSystem mBoundsSystem;
    GravitySystem mGravitySystem;
    CollisionSystem mCollisionSystem;
    ResourceManager* mResourceManager = nullptr;

    std::vector<Entity> mUiListCache;
    bool mUiListDirty = true;
    void RebuildUiListCacheIfNeeded();

    // UI preset registry + live instances
    std::unordered_map<std::string, UiPresetData> mUiPresetRegistry;
    std::vector<SceneUiPresetEntry> mSceneUiPresets;
    SceneUiPresetEntry* FindSceneUiPreset(const std::string& name);
    const SceneUiPresetEntry* FindSceneUiPreset(const std::string& name) const;
    struct UiPresetInstance
    {
        uint32_t id = 0;
        std::string presetName;
        std::vector<Entity> entities;
    };
    std::unordered_map<uint32_t, UiPresetInstance> mUiPresetInstances;
    uint32_t mNextUiPresetInstanceId = 1;
    Entity SpawnUiPresetElement(const UiPresetElementData& el, uint32_t instanceId, bool startActive);

    ID3D12Device* mDevice = nullptr;
    std::vector<std::unique_ptr<FrameResource>>* mFrameResources = nullptr;
    int mGNumFrameResources = 0;
    DescriptorAllocator* mDescriptorAllocator = nullptr;

    uint32_t mNextObjectCBIndex = 0;
    uint32_t mMaxObjectCBSlots = 8192;

    std::vector<Entity> mRenderableListCache;
    bool mRenderableListDirty = true;

    void SyncOutlinePassFlag();

    GraphicsStyle mGraphicsStyle = GraphicsStyle::Realistic;
    float mToonBands = 3.0f;
    bool mToonOutlineEnabled = true;
    float mOutlineWidth = 0.025f;
    float mSpecularPower = 32.0f;
    DirectX::XMFLOAT3 mSunDirection = { 0.35f, -1.0f, 0.25f };
    DirectX::XMFLOAT3 mSunStrength = { 1.0f, 0.96f, 0.90f };
    DirectX::XMFLOAT3 mAmbientRgb = { 0.22f, 0.24f, 0.30f };
    bool mShadowsEnabled = true;
    float mShadowBias = 0.003f;
};