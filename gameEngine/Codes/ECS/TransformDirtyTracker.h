#pragma once
#include "Entity.h"
#include <vector>
#include <cstdint>

// Transform MarkDirty 알림 — Path2/3이 전체 ForEach 없이 dirty만 처리
namespace TransformDirtyTracker
{
    inline uint32_t& Generation()
    {
        static uint32_t g = 0;
        return g;
    }

    inline std::vector<ECS::Entity>& Entities()
    {
        static std::vector<ECS::Entity> v;
        return v;
    }

    inline void Notify(ECS::Entity entity)
    {
        ++Generation();
        if (entity != ECS::INVALID_ENTITY)
            Entities().push_back(entity);
    }

    // generation만 올림 (엔티티 모를 때 — 수신 측 full scan 폴백)
    inline void NotifyUnknown()
    {
        ++Generation();
    }

    inline void ClearEntities()
    {
        Entities().clear();
    }

    // 리스트를 가져가고 비움
    inline std::vector<ECS::Entity> TakeEntities()
    {
        std::vector<ECS::Entity> out;
        out.swap(Entities());
        return out;
    }
}
