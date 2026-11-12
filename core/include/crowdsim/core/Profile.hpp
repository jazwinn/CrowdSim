#pragma once

/// @file Profile.hpp
/// Profiling macros. Compile with -DCROWDSIM_ENABLE_TRACY=ON to route them to
/// Tracy; otherwise they compile to nothing.

#if defined(CROWDSIM_TRACY)
#include <tracy/Tracy.hpp>
#define CS_PROFILE_ZONE() ZoneScoped
#define CS_PROFILE_ZONE_NAMED(name) ZoneScopedN(name)
#define CS_PROFILE_FRAME() FrameMark
#else
#define CS_PROFILE_ZONE() ((void)0)
#define CS_PROFILE_ZONE_NAMED(name) ((void)0)
#define CS_PROFILE_FRAME() ((void)0)
#endif
