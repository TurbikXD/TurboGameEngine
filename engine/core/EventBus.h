#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::core {

class EventBus final {
public:
    EventBus() = default;
    ~EventBus() = default;

    template <class T, class Func>
    void subscribe(Func&& handler);

    template <class T>
    void publish(const T& event) const;

    void clear();

private:
    using UntypedHandler = std::function<void(const void*)>;

    void requireOwnerThread() const {
        if (std::this_thread::get_id() != m_ownerThread) {
            throw std::logic_error("EventBus access belongs to its owner thread");
        }
    }

    std::thread::id m_ownerThread{std::this_thread::get_id()};
    std::unordered_map<std::type_index, std::vector<std::shared_ptr<UntypedHandler>>> m_handlers;
};

template <class T, class Func>
void EventBus::subscribe(Func&& handler) {
    requireOwnerThread();
    auto typedHandler = std::function<void(const T&)>(std::forward<Func>(handler));
    m_handlers[typeid(T)].emplace_back(
        std::make_shared<UntypedHandler>([wrapped = std::move(typedHandler)](const void* rawEvent) {
            wrapped(*static_cast<const T*>(rawEvent));
        }));
}

template <class T>
void EventBus::publish(const T& event) const {
    requireOwnerThread();
    const auto it = m_handlers.find(typeid(T));
    if (it == m_handlers.end()) {
        return;
    }

    const auto handlers = it->second;
    for (const auto& handler : handlers) {
        (*handler)(&event);
    }
}

inline void EventBus::clear() {
    requireOwnerThread();
    m_handlers.clear();
}

}
