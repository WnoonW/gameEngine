#pragma once
#include <cstdint>
#include <string>

namespace ECS {

    using ComponentTypeID = uint64_t;

    struct ComponentType {
        ComponentTypeID id = 0;
        size_t size = 0;           // sizeof(컴포넌트)
        std::string name;          // 디버깅 및 로깅용
    };

} // namespace ECS

// Component.h에 추가
namespace ECS {

    template<typename T>
    ComponentTypeID GetComponentTypeID() {
        static ComponentTypeID id = sNextComponentID++;
        return id;
    }

    inline ComponentTypeID sNextComponentID = 0;

} // namespace ECS