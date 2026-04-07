#pragma once

#include <functional>
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

    std::unordered_map<std::type_index, std::vector<UntypedHandler>> m_handlers;
};

template <class T, class Func>
void EventBus::subscribe(Func&& handler) {
    auto typedHandler = std::function<void(const T&)>(std::forward<Func>(handler));
    m_handlers[typeid(T)].emplace_back(
        [wrapped = std::move(typedHandler)](const void* rawEvent) { wrapped(*static_cast<const T*>(rawEvent)); });
}

template <class T>
void EventBus::publish(const T& event) const {
    const auto it = m_handlers.find(typeid(T));
    if (it == m_handlers.end()) {
        return;
    }

    for (const auto& handler : it->second) {
        handler(&event);
    }
}

inline void EventBus::clear() {
    m_handlers.clear();
}

} // namespace engine::core
