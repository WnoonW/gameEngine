#pragma once
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <string>
#include <cstdint>
#include "Entity.h"
#include "TransformDirtyTracker.h"
struct Mesh;
struct Material;

using namespace DirectX;


struct TransformComponent {
    XMFLOAT3 position{ 0.0f, 0.0f, 0.0f };
    XMFLOAT3 rotation{ 0.0f, 0.0f, 0.0f };
    XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };

    // ObjectCB / 인스턴스 버퍼에 다시 올려야 하는 프레임 수 (보통 gNumFrameResources)
    int dirtyFrames = 0;

    // F2: true면 Path3 TRS 업로드 생략 (CPU bounds/충돌용 미러 적분만)
    // 에디터 이동·충돌 보정 등 full MarkDirty()는 항상 false로 리셋
    bool suppressGpuUpload = false;

    // 엔티티를 알면 MarkDirty(entity) 권장 — dirty 리스트 기반 패치
    void MarkDirty(int frames = 3)
    {
        if (frames > dirtyFrames)
            dirtyFrames = frames;
        suppressGpuUpload = false;
        mWorldValid = false;
        TransformDirtyTracker::NotifyUnknown();
    }

    void MarkDirty(ECS::Entity entity, int frames = 3)
    {
        MarkDirty(entity, frames, false);
    }

    // suppressGpuUploadFlag: GPU motion 소유 슬롯의 gravity 미러 적분용
    void MarkDirty(ECS::Entity entity, int frames, bool suppressGpuUploadFlag)
    {
        if (!suppressGpuUploadFlag)
            suppressGpuUpload = false;
        else
            // full dirty가 이미 잡혀 있으면 업로드 우선 유지
            suppressGpuUpload = suppressGpuUpload || (dirtyFrames <= 0);

        if (frames > dirtyFrames)
            dirtyFrames = frames;
        mWorldValid = false;
        TransformDirtyTracker::Notify(entity);
    }

    // position/rotation/scale 변경 후 MarkDirty 호출 전제. 유효하면 재계산 없이 캐시 반환.
    XMMATRIX GetWorldMatrix() const
    {
        if (!mWorldValid)
        {
            const XMMATRIX T = XMMatrixTranslation(position.x, position.y, position.z);
            const XMMATRIX R = XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z);
            const XMMATRIX S = XMMatrixScaling(scale.x, scale.y, scale.z);
            const XMMATRIX W = S * R * T;
            XMStoreFloat4x4(&mWorldCache, W);
            mWorldValid = true;
            return W;
        }
        return XMLoadFloat4x4(&mWorldCache);
    }

    // designated initializer / aggregate 유지용 (외부에서 직접 쓰지 말 것)
    mutable bool mWorldValid = false;
    mutable XMFLOAT4X4 mWorldCache{
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
};


struct RenderableComponent {
    Mesh* mesh = nullptr;       // 현재 드로우 메시 (LOD 적용 후)
    uint32_t objectCBIndex = 0;
    bool visible = true;
};

// Step H: 거리 기반 LOD (levels[0] = 최고 해상도)
struct LodComponent {
    static constexpr int kMaxLevels = 4;
    Mesh* levels[kMaxLevels]{};
    int levelCount = 1;
    // dist > thresholds[i] * bias → level i+1
    float thresholds[kMaxLevels - 1]{ 20.f, 50.f, 100.f };
    float cullDistance = 250.f;
    int currentLevel = 0;
    bool culled = false;
    int forcedLevel = -1; // -1 = auto, 0..levelCount-1 = force
};


struct CameraComponent {
    float fov = DirectX::XM_PIDIV4;      // 45도 (기본값)
    float nearZ = 0.1f;
    float farZ = 1000.0f;
    bool useWindowAspect = true;         // true면 창 크기에 맞춰 aspect 자동 계산
    float aspectRatio = 16.0f / 9.0f;    // useWindowAspect가 false일 때 사용
    bool isMainCamera = true;            // 여러 카메라 중 메인 카메라 구분용
};

struct BoundsComponent {
    DirectX::BoundingBox localBounds;   // per-object union of all submeshes (local space)
    DirectX::BoundingBox worldBounds;   // transformed to world space
};

struct GravityComponent {
    bool enabled = true;
    float strength = 9.81f;
    XMFLOAT3 velocity{ 0.0f, 0.0f, 0.0f };
    // rad/s — pitch, yaw, roll (GetWorldMatrix / GPU motion과 동일)
    XMFLOAT3 angularVelocity{ 0.0f, 0.0f, 0.0f };
};

struct CollisionComponent {
    bool enabled = true;
    bool isStatic = false;
    float restitution = 0.0f;
};

struct SelectedComponent {
    // Editor tag: this entity is the active viewport selection.
};

// ---------------------------------------------------------------------------
// In-game UI (NOT ImGui) — image quads.
//
// Component roles (do not merge):
//   UiElementComponent  — layout / space / visibility
//   UiImageComponent    — what to draw (material, tint, UV)
//   UiButtonComponent   — optional input (actionId + hover/press)
//   UiPresetInstanceTag — marks entities spawned from a preset instance
//
// World-space camera-facing quads also exist as EffectBillboardComponent (VFX).
// That path is separate on purpose:
//   Ui WorldBillboard  = gameplay UI (HP bar, nameplate, prompt) — layout/zOrder/preset
//   EffectBillboard    = one-shot VFX (flash, spark) — lifetime, additive, auto-destroy
// Both may share EmitWorldBillboard in UiSystem; components stay distinct.
//
// Field semantics (UiElement):
//   visible — render on/off for every mode
//   active  — logic on/off:
//               ScreenConditional: must be true to draw
//               all modes: must be true to accept pointer (ScreenAlways can stay
//               drawn with active=false to disable input only)
//   layoutPercent=true (canonical): position/size are 0..1 of live canvas
//   layoutPercent=false (legacy): design-space pixels + designW/H scale
// ---------------------------------------------------------------------------
enum class UiSpaceMode : int
{
    ScreenAlways = 0,      // Ortho HUD: draw when visible (active ignored for draw)
    ScreenConditional = 1, // Ortho: draw only when visible && active
    WorldBillboard = 2,    // World Transform + camera-facing UI (not VFX)
};

// Per-widget canvas scaler (also used as "default for new UI" on UiSystem).
// Stretch: live canvas % — width/height scale independently (may squash widgets).
// UniformMin / UniformMax: position same as Stretch (% of live canvas);
//   size uses one scale from designW/H so authored pixel aspect is preserved.
enum class UiScaleMode : int
{
    Stretch = 0,     // X/Y independent % of live canvas (may squash)
    UniformMin = 1,  // Keep aspect fit:  pos = live %; size s = min(sx,sy)
    UniformMax = 2,  // Keep aspect fill: pos = live %; size s = max(sx,sy)
};

struct UiElementComponent
{
    UiSpaceMode mode = UiSpaceMode::ScreenAlways;
    bool active = true;   // logic / Conditional draw / pointer (see header notes)
    bool visible = true;  // render gate (all modes)
    int zOrder = 0;
    // How this widget maps design → live canvas (overridable per UI in Inspector).
    UiScaleMode scaleMode = UiScaleMode::Stretch;
    // Screen origin for offsets (usually 0,0 = top-left of canvas).
    DirectX::XMFLOAT2 anchor{ 0.0f, 0.0f };
    // Local pivot. Percent screen UI default (0.5,0.5) → position is the CENTER.
    DirectX::XMFLOAT2 pivot{ 0.5f, 0.5f };
    // layoutPercent=true (canonical):
    //   position = center 0..1 of canvas; size = extent 0..1 of authoring design.
    //   Stretch: pos & size both % of live screen (widget aspect may change).
    //   Uniform*: pos % of live screen (like Stretch); size keeps design aspect.
    // layoutPercent=false (legacy only): design-space pixels + designW/H.
    DirectX::XMFLOAT2 position{ 0.5f, 0.5f };
    DirectX::XMFLOAT2 size{ 0.1f, 0.1f };
    float rotationRad = 0.0f;
    bool layoutPercent = true;
    // Authoring canvas size when this widget was created/saved (required for Uniform*).
    float designW = 0.0f;
    float designH = 0.0f;
    // Keep Aspect Fit (UniformMin): stick to canvas edges across resize (set by editor snap).
    bool snapLeft = false;
    bool snapRight = false;
    bool snapTop = false;
    bool snapBottom = false;
};

// Shared draw/input gates — keep UiSystem / Engine pick in sync.
inline bool UiElementShouldDraw(const UiElementComponent& el)
{
    if (!el.visible)
        return false;
    if (el.mode == UiSpaceMode::ScreenConditional && !el.active)
        return false;
    return true;
}

// Screen-space pointer only (WorldBillboard pick is not supported in MVP).
inline bool UiElementShouldAcceptPointer(const UiElementComponent& el)
{
    if (!el.visible || !el.active)
        return false;
    if (el.mode == UiSpaceMode::WorldBillboard)
        return false;
    return true;
}

struct UiImageComponent
{
    std::string materialName;
    DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 uvRect{ 0.0f, 0.0f, 1.0f, 1.0f };
};

struct UiButtonComponent
{
    std::string actionId;
    bool interactable = true;
    bool hovered = false;  // runtime
    bool pressed = false;  // runtime
};

inline bool UiButtonShouldAcceptPointer(const UiElementComponent& el, const UiButtonComponent& btn)
{
    return btn.interactable && UiElementShouldAcceptPointer(el);
}

// Marks UI entities spawned from a registered preset instance (POD only — ECS memcpy).
struct UiPresetInstanceTag
{
    uint32_t instanceId = 0;
};

// World-space VFX billboard (camera-facing). NOT gameplay UI — use Ui WorldBillboard for that.
// Shared renderer helper with UI world quads; lifecycle (age/lifetime) is Effect-only.
struct EffectBillboardComponent
{
    std::string materialName;
    DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    float size = 1.0f;       // world units, square extent
    bool additive = true;
    float lifetime = -1.0f;  // seconds; <0 = infinite
    float age = 0.0f;
    bool visible = true;
};