#include "engine/game/StateStack.h"

#include "engine/core/Log.h"
#include "engine/renderer/Renderer.h"

namespace engine::game {

void StateStack::push(std::unique_ptr<IGameState> state) {
    m_pending.push_back(PendingChange{Action::Push, std::move(state)});
}

void StateStack::pop() {
    m_pending.push_back(PendingChange{Action::Pop, nullptr});
}

void StateStack::replace(std::unique_ptr<IGameState> state) {
    m_pending.push_back(PendingChange{Action::Replace, std::move(state)});
}

void StateStack::clear() {
    m_pending.push_back(PendingChange{Action::Clear, nullptr});
}

void StateStack::handleEvent(const platform::Event& event) {
    if (!m_stack.empty()) {
        m_stack.back()->handleEvent(event);
    }
}

void StateStack::update(double dt) {
    if (!m_stack.empty()) {
        m_stack.back()->update(dt);
    }
}

void StateStack::render(renderer::Renderer& renderer) {
    for (const auto& state : m_stack) {
        state->render(renderer);
    }
}

void StateStack::applyPendingChanges() {
    for (auto& pending : m_pending) {
        switch (pending.action) {
            case Action::Push:
                if (pending.state) {
                    ENGINE_LOG_INFO("StateStack push");
                    pending.state->onEnter();
                    m_stack.push_back(std::move(pending.state));
                }
                break;
            case Action::Pop:
                if (!m_stack.empty()) {
                    ENGINE_LOG_INFO("StateStack pop");
                    m_stack.back()->onExit();
                    m_stack.pop_back();
                }
                break;
            case Action::Replace:
                if (!m_stack.empty()) {
                    ENGINE_LOG_INFO("StateStack replace old");
                    m_stack.back()->onExit();
                    m_stack.pop_back();
                }
                if (pending.state) {
                    ENGINE_LOG_INFO("StateStack replace new");
                    pending.state->onEnter();
                    m_stack.push_back(std::move(pending.state));
                }
                break;
            case Action::Clear:
                while (!m_stack.empty()) {
                    m_stack.back()->onExit();
                    m_stack.pop_back();
                }
                break;
        }
    }
    m_pending.clear();
}

bool StateStack::empty() const {
    return m_stack.empty();
}

} // namespace engine::game
