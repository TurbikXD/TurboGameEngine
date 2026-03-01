#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>

#include "engine/core/Log.h"

namespace engine::resources {

class ResourceManager final {
public:
    ResourceManager() = default;
    ~ResourceManager() = default;

    template <class T>
    using LoaderFn = std::function<std::shared_ptr<T>(const std::string&)>;

    template <class T>
    void registerLoader(LoaderFn<T> loader);

    template <class T>
    void registerFallback(std::shared_ptr<T> fallback);

    template <class T>
    std::shared_ptr<T> load(const std::string& path);

    void clear();

private:
    using UntypedLoaderFn = std::function<std::shared_ptr<void>(const std::string&)>;

    template <class T>
    std::shared_ptr<T> fallbackForType() const;

    mutable std::mutex m_mutex;
    std::unordered_map<std::type_index, std::unordered_map<std::string, std::weak_ptr<void>>> m_cacheByType;
    std::unordered_map<std::type_index, UntypedLoaderFn> m_loaderByType;
    std::unordered_map<std::type_index, std::shared_ptr<void>> m_fallbackByType;
};

template <class T>
void ResourceManager::registerLoader(LoaderFn<T> loader) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loaderByType[typeid(T)] = [wrappedLoader = std::move(loader)](const std::string& path) {
        return std::static_pointer_cast<void>(wrappedLoader(path));
    };
}

template <class T>
void ResourceManager::registerFallback(std::shared_ptr<T> fallback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fallbackByType[typeid(T)] = std::static_pointer_cast<void>(std::move(fallback));
}

template <class T>
std::shared_ptr<T> ResourceManager::load(const std::string& path) {
    UntypedLoaderFn loader;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& cacheForType = m_cacheByType[typeid(T)];
        const auto cacheIt = cacheForType.find(path);
        if (cacheIt != cacheForType.end()) {
            if (const auto cached = std::static_pointer_cast<T>(cacheIt->second.lock())) {
                ENGINE_LOG_INFO("ResourceManager cache hit [{}] '{}'", typeid(T).name(), path);
                return cached;
            }
            cacheForType.erase(cacheIt);
        }

        const auto loaderIt = m_loaderByType.find(typeid(T));
        if (loaderIt != m_loaderByType.end()) {
            loader = loaderIt->second;
        }
    }

    ENGINE_LOG_INFO("ResourceManager cache miss [{}] '{}'", typeid(T).name(), path);
    if (!loader) {
        ENGINE_LOG_ERROR("ResourceManager has no loader for type [{}]", typeid(T).name());
        return fallbackForType<T>();
    }

    std::shared_ptr<T> loaded;
    try {
        loaded = std::static_pointer_cast<T>(loader(path));
    } catch (const std::exception& ex) {
        ENGINE_LOG_ERROR("ResourceManager loader exception [{}] '{}': {}", typeid(T).name(), path, ex.what());
    } catch (...) {
        ENGINE_LOG_ERROR("ResourceManager loader unknown exception [{}] '{}'", typeid(T).name(), path);
    }

    if (!loaded) {
        ENGINE_LOG_WARN("ResourceManager load failed [{}] '{}', using fallback", typeid(T).name(), path);
        return fallbackForType<T>();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cacheByType[typeid(T)][path] = loaded;
    }
    return loaded;
}

template <class T>
std::shared_ptr<T> ResourceManager::fallbackForType() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_fallbackByType.find(typeid(T));
    if (it == m_fallbackByType.end()) {
        return nullptr;
    }
    return std::static_pointer_cast<T>(it->second);
}

} // namespace engine::resources
