#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <algorithm>
#include "Entity.h"
#include "Component.h"

namespace ECS {

    class Archetype {
    public:
        uint64_t componentMask = 0;
        std::vector<ComponentTypeID> componentTypes;
        std::unordered_map<ComponentTypeID, std::vector<std::byte>> componentArrays;
        std::unordered_map<ComponentTypeID, size_t> componentSizes;
        std::vector<Entity> entities;

        size_t GetEntityCount() const { return entities.size(); }

        bool HasComponent(ComponentTypeID typeId) const {
            return (componentMask & (1ULL << typeId)) != 0;
        }

        void* GetComponentDataPtr(ComponentTypeID typeId, size_t index) {
            if (!HasComponent(typeId) || index >= entities.size()) return nullptr;
            auto& arr = componentArrays[typeId];
            size_t size = componentSizes[typeId];
            return arr.data() + (index * size);
        }

        template<typename T>
        T* GetComponent(size_t index) {
            return reinterpret_cast<T*>(GetComponentDataPtr(GetComponentTypeID<T>(), index));
        }

        // 엔티티 제거 + 데이터 정리
        size_t RemoveEntity(Entity entity) {
            auto it = std::find(entities.begin(), entities.end(), entity);
            if (it == entities.end()) return SIZE_MAX;

            size_t index = std::distance(entities.begin(), it);

            // 데이터 뒤에서 앞으로 당기기
            for (auto& [typeId, arr] : componentArrays) {
                size_t size = componentSizes[typeId];
                if ((index + 1) * size <= arr.size()) {
                    std::memmove(arr.data() + index * size,
                        arr.data() + (index + 1) * size,
                        arr.size() - (index + 1) * size);
                    arr.resize(arr.size() - size);
                }
            }

            entities.erase(it);
            return index;
        }
    };

} // namespace ECS