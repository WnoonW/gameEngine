#pragma once
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include "Entity.h"
#include "ArchetypeManager.h"

namespace ECS {

    class World {
    public:
        Entity CreateEntity() {
            Entity e = mNextEntityID++;
            mEntityLocation[e] = { nullptr, 0 };
            return e;
        }

        void DestroyEntity(Entity entity) {
            auto it = mEntityLocation.find(entity);
            if (it == mEntityLocation.end()) return;

            auto [arch, index] = it->second;
            if (arch) {
                arch->RemoveEntity(entity);
            }
            mEntityLocation.erase(entity);
        }

        template<typename T>
        void AddComponent(Entity entity, T&& component) {
            auto it = mEntityLocation.find(entity);
            if (it == mEntityLocation.end()) return;

            auto [oldArch, oldIndex] = it->second;
            ComponentTypeID typeId = GetComponentTypeID<T>();

            std::vector<ComponentTypeID> newTypes;
            if (oldArch) newTypes = oldArch->componentTypes;
            newTypes.push_back(typeId);

            std::sort(newTypes.begin(), newTypes.end());
            auto last = std::unique(newTypes.begin(), newTypes.end());
            newTypes.erase(last, newTypes.end());

            Archetype* newArch = mArchetypeManager.GetOrCreateArchetype(newTypes);
            size_t newIndex = newArch->entities.size();

            // 기존 데이터 완전 복사
            if (oldArch && oldArch != newArch) {
                for (auto type : oldArch->componentTypes) {
                    size_t size = oldArch->componentSizes[type];
                    void* src = oldArch->GetComponentDataPtr(type, oldIndex);
                    if (src) {
                        auto& dest = newArch->componentArrays[type];
                        size_t oldSize = dest.size();
                        dest.resize(oldSize + size);
                        std::memcpy(dest.data() + oldSize, src, size);
                        newArch->componentSizes[type] = size;
                    }
                }
                oldArch->RemoveEntity(entity);
            }

            // 새 엔티티 등록
            newArch->entities.push_back(entity);
            newArch->componentSizes[typeId] = sizeof(T);

            // 새 컴포넌트 데이터 추가
            auto& arr = newArch->componentArrays[typeId];
            size_t oldSize = arr.size();
            arr.resize(oldSize + sizeof(T));
            std::memcpy(arr.data() + oldSize, &component, sizeof(T));

            mEntityLocation[entity] = { newArch, newIndex };
        }

        template<typename T>
        void RemoveComponent(Entity entity) {
            auto it = mEntityLocation.find(entity);
            if (it == mEntityLocation.end()) return;

            auto [oldArch, oldIndex] = it->second;
            if (!oldArch) return;

            ComponentTypeID removeType = GetComponentTypeID<T>();
            if (!oldArch->HasComponent(removeType)) return;

            // 제거 후 남은 컴포넌트 조합 생성
            std::vector<ComponentTypeID> newTypes;
            for (auto type : oldArch->componentTypes) {
                if (type != removeType) newTypes.push_back(type);
            }

            if (newTypes.empty()) {
                // 모든 컴포넌트가 제거된 경우
                oldArch->RemoveEntity(entity);
                mEntityLocation[entity] = { nullptr, 0 };
                return;
            }

            Archetype* newArch = mArchetypeManager.GetOrCreateArchetype(newTypes);
            size_t newIndex = newArch->entities.size();

            // 기존 데이터 복사 (제거할 컴포넌트 제외)
            for (auto type : newTypes) {
                size_t size = oldArch->componentSizes[type];
                void* src = oldArch->GetComponentDataPtr(type, oldIndex);
                if (src) {
                    auto& dest = newArch->componentArrays[type];
                    size_t oldSize = dest.size();
                    dest.resize(oldSize + size);
                    std::memcpy(dest.data() + oldSize, src, size);
                    newArch->componentSizes[type] = size;
                }
            }

            oldArch->RemoveEntity(entity);
            newArch->entities.push_back(entity);
            mEntityLocation[entity] = { newArch, newIndex };
        }

        template<typename T>
        T* GetComponent(Entity entity) {
            auto it = mEntityLocation.find(entity);
            if (it == mEntityLocation.end()) return nullptr;

            auto [arch, index] = it->second;
            if (!arch) return nullptr;

            return arch->GetComponent<T>(index);
        }

        template<typename... Components>
        void ForEach(auto&& func) {
            uint64_t requiredMask = 0;
            ((requiredMask |= (1ULL << GetComponentTypeID<Components>())), ...);

            auto archetypes = mArchetypeManager.FindArchetypes(requiredMask);

            for (Archetype* arch : archetypes) {
                for (size_t i = 0; i < arch->entities.size(); ++i) {
                    Entity e = arch->entities[i];
                    func(e, *arch->GetComponent<Components>(i)...);
                }
            }
        }

    private:
        ArchetypeManager mArchetypeManager;
        std::unordered_map<Entity, std::pair<Archetype*, size_t>> mEntityLocation;
        Entity mNextEntityID = 1;
    };

} // namespace ECS