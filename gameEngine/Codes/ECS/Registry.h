#pragma once
#include "Entity.h"
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <any>
#include <optional>

class Registry {
public:
    Entity createEntity() {
        Entity e = m_nextEntity++;
        m_entities.push_back(e);
        return e;
    }

    template<typename T>
    void addComponent(Entity entity, T component) {
        auto& storage = getStorage<T>();
        storage[entity] = std::move(component);
    }

    template<typename T>
    T& getComponent(Entity entity) {
        return getStorage<T>().at(entity);
    }

    template<typename T>
    bool hasComponent(Entity entity) const {
        auto type = std::type_index(typeid(T));
        auto it = m_componentStorages.find(type);
        if (it == m_componentStorages.end()) return false;
        const auto& storage = std::any_cast<const std::unordered_map<Entity, T>&>(it->second);
        return storage.find(entity) != storage.end();
    }

    template<typename... Ts>
    std::vector<Entity> view() {
        std::vector<Entity> result;
        for (auto e : m_entities) {
            if ((hasComponent<Ts>(e) && ...)) {
                result.push_back(e);
            }
        }
        return result;
    }

    void destroyEntity(Entity entity) {
        // 필요하면 구현 (지금은 간단 버전)
        m_entities.erase(std::remove(m_entities.begin(), m_entities.end(), entity), m_entities.end());
    }

private:
    std::vector<Entity> m_entities;
    std::unordered_map<std::type_index, std::any> m_componentStorages;
    Entity m_nextEntity = 1;

    template<typename T>
    std::unordered_map<Entity, T>& getStorage() {
        auto type = std::type_index(typeid(T));
        if (m_componentStorages.find(type) == m_componentStorages.end()) {
            m_componentStorages[type] = std::unordered_map<Entity, T>{};
        }
        return std::any_cast<std::unordered_map<Entity, T>&>(m_componentStorages[type]);
    }
};