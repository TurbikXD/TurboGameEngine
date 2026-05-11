#pragma once

#include "imgui.h"

#if !defined(CaptureMouseFromApp)
#    define CaptureMouseFromApp() SetNextFrameWantCaptureMouse(true)
#endif
