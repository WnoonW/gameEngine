#pragma once
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include "Archetype.h"

namespace ECS {

    class ArchetypeManager {
    public:
        Archetype* GetOrCreateArchetype(const std::vector<ComponentTypeID>& componentTypes) {
            // 1. bitmask 계산
            uint64_t mask = 0;
            for (auto typeId : componentTypes) {
                mask |= (1ULL << typeId);
            }

            // 2. 이미 존재하는 Archetype이 있는지 확인
            auto it = mMaskToArchetypeIndex.find(mask);
            if (it != mMaskToArchetypeIndex.end()) {
                return mArchetypes[it->second].get();
            }

            // 3. 새 Archetype 생성
            auto newArchetype = std::make_unique<Archetype>();
            newArchetype->componentMask = mask;
            newArchetype->componentTypes = componentTypes;

            // 4. 컴포넌트 타입별로 데이터 배열 미리 준비
            for (auto typeId : componentTypes) {
                newArchetype->componentArrays[typeId] = std::vector<std::byte>{};
            }

            // 5. 저장 및 인덱스 기록
            size_t index = mArchetypes.size();
            mArchetypes.push_back(std::move(newArchetype));
            mMaskToArchetypeIndex[mask] = index;

            return mArchetypes.back().get();
        }

        std::vector<Archetype*> FindArchetypes(uint64_t requiredMask) {
            std::vector<Archetype*> result;
            for (auto& arch : mArchetypes) {
                if ((arch->componentMask & requiredMask) == requiredMask) {
                    result.push_back(arch.get());
                }
            }
            return result;
        }

        const std::vector<std::unique_ptr<Archetype>>& GetAllArchetypes() const {
            return mArchetypes;
        }

    private:
        std::vector<std::unique_ptr<Archetype>> mArchetypes;
        std::unordered_map<uint64_t, size_t> mMaskToArchetypeIndex;
    };

} // namespace ECS