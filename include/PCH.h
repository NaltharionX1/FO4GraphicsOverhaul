#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <Windows.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef NDEBUG
#    include <spdlog/sinks/basic_file_sink.h>
#else
#    include <spdlog/sinks/msvc_sink.h>
#endif

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

namespace logger = F4SE::log;

#include "Core/FailOpen.h"
namespace Guard = Core::FailOpen::Guard;

#define DLLEXPORT __declspec(dllexport)

#include "Plugin.h"
