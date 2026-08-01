#pragma once
// In-game UI + simple effect billboards (NO ImGui).
// Screen modes: orthographic (CPU builds NDC quads).
// WorldBillboard / EffectBillboard: camera-facing quads in clip space.

#include <d3d12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <vector>
#include <string>

#include "World.h"
#include "ComponentStruct.h"
#include "DescriptorAllocator.h"
#include "AppStruct.h"

using Microsoft::WRL::ComPtr;

struct UiClickEvent
{
    ECS::Entity entity = 0;
    std::string actionId;
};

class UiSystem
{
public:
    void Initialize(ID3D12Device* device);
    void Shutdown();

    // After world 3D, before SceneViewport::End. Draws into current RT (Scene MSAA).
    void Render(
        ECS::World& world,
        ID3D12GraphicsCommandList* cmdList,
        DescriptorAllocator* descriptorAllocator,
        UINT screenWidth,
        UINT screenHeight,
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& proj,
        const DirectX::XMFLOAT3& eyePosW);

    // Call once per frame from play/game input path (pixel coords in scene RT).
    // Returns true if a button consumed the click.
    bool HandlePointer(
        ECS::World& world,
        float scenePixelX,
        float scenePixelY,
        UINT screenWidth,
        UINT screenHeight,
        bool leftDown,
        bool leftPressedThisFrame,
        std::vector<UiClickEvent>* outClicks);

    void SetSampleCount(UINT n)
    {
        const UINT next = (n > 0) ? n : 1;
        if (next == mSampleCount)
            return;
        mSampleCount = next;
        // Sample count baked into PSO — force rebuild on next draw
        mPsoAlpha.Reset();
        mPsoAdditive.Reset();
    }

    // Canvas scaler: design-space → current screen/RT pixels.
    void SetScaleMode(UiScaleMode mode) { mScaleMode = mode; }
    UiScaleMode GetScaleMode() const { return mScaleMode; }
    void SetGlobalDesignResolution(float w, float h)
    {
        mGlobalDesignW = (w > 1.f) ? w : 0.f;
        mGlobalDesignH = (h > 1.f) ? h : 0.f;
    }
    void GetGlobalDesignResolution(float& outW, float& outH) const
    {
        outW = mGlobalDesignW;
        outH = mGlobalDesignH;
    }

    // Resolve design-space layout to current screen pixels (shared by Render / pick).
    static void ResolveScreenLayout(
        const UiElementComponent& el,
        float screenW, float screenH,
        UiScaleMode mode,
        float globalDesignW, float globalDesignH,
        DirectX::XMFLOAT2& outPosPx,
        DirectX::XMFLOAT2& outSizePx,
        float* outScaleX = nullptr,
        float* outScaleY = nullptr);

private:
    struct UiVertex
    {
        float x, y;
        float u, v;
        float r, g, b, a;
    };

    struct DrawItem
    {
        UINT materialIndex = UINT_MAX;
        int zOrder = 0;
        bool additive = false;
        UiVertex v[6]{};
    };

    void EnsurePipeline(ID3D12Device* device);
    void EnsureUploadBuffer(ID3D12Device* device, UINT64 bytes);
    void EmitScreenQuad(
        std::vector<DrawItem>& out,
        const DirectX::XMFLOAT2& anchor,
        const DirectX::XMFLOAT2& pivot,
        const DirectX::XMFLOAT2& posPx,
        const DirectX::XMFLOAT2& sizePx,
        float rotRad,
        const DirectX::XMFLOAT4& color,
        const DirectX::XMFLOAT4& uv,
        UINT materialIndex,
        int zOrder,
        float screenW,
        float screenH);
    void EmitWorldBillboard(
        std::vector<DrawItem>& out,
        const DirectX::XMFLOAT3& worldPos,
        float sizeX,
        float sizeY,
        const DirectX::XMFLOAT4& color,
        const DirectX::XMFLOAT4& uv,
        UINT materialIndex,
        int zOrder,
        bool additive,
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& proj);

    ID3D12Device* mDevice = nullptr;
    ComPtr<ID3D12RootSignature> mRootSig;
    ComPtr<ID3D12PipelineState> mPsoAlpha;
    ComPtr<ID3D12PipelineState> mPsoAdditive;
    ComPtr<ID3D12Resource> mUpload;
    BYTE* mUploadMapped = nullptr;
    UINT64 mUploadBytes = 0;
    UINT mSampleCount = 4;
    UiScaleMode mScaleMode = UiScaleMode::Stretch;
    // 0 = elements without designW/H render 1:1 (legacy). New UI sets per-element design.
    float mGlobalDesignW = 0.0f;
    float mGlobalDesignH = 0.0f;
};
