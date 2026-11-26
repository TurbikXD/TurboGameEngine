#include "engine/resources/resource_manager.h"

namespace engine::resources {

void ResourceManager::clearCache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheByType.clear();
}

void ResourceManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheByType.clear();
    m_loaderByType.clear();
    m_fallbackByType.clear();
}

}
