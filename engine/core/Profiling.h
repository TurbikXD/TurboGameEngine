#pragma once

#if defined(ENGINE_TRACY_PROFILE)
#    include <tracy/Tracy.hpp>
#    define ENGINE_PROFILE_ZONE(name) ZoneScopedN(name)
#    define ENGINE_PROFILE_FRAME_MARK() FrameMark
#    define ENGINE_PROFILE_THREAD_NAME(name) tracy::SetThreadName(name)
#    define ENGINE_PROFILE_PLOT(name, value) TracyPlot(name, static_cast<double>(value))
#    define ENGINE_PROFILE_MESSAGE(literal) TracyMessageL(literal)
#    define ENGINE_PROFILE_CONNECTED() TracyIsConnected
#else
#    define ENGINE_PROFILE_ZONE(name) ((void)0)
#    define ENGINE_PROFILE_FRAME_MARK() ((void)0)
#    define ENGINE_PROFILE_THREAD_NAME(name) ((void)0)
#    define ENGINE_PROFILE_PLOT(name, value) ((void)(value))
#    define ENGINE_PROFILE_MESSAGE(literal) ((void)0)
#    define ENGINE_PROFILE_CONNECTED() false
#endif
