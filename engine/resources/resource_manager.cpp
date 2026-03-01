#include "engine/resources/resource_manager.h"

namespace engine::resources {

void ResourceManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheByType.clear();
    m_loaderByType.clear();
    m_fallbackByType.clear();
}

} // namespace engine::resources
