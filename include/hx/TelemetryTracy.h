#ifndef HX_TELEMETRY_TRACY_H
#define HX_TELEMETRY_TRACY_H

#ifndef HXCPP_TRACY
	#error "Error: HXCPP_TRACY must be defined."
#endif

#define TRACY_ENABLE
// NOTE: This has to match what TracyClient.cpp uses to build, or Tracy config will fail to link.
#ifdef HXCPP_TRACY_ON_DEMAND
#define TRACY_ON_DEMAND
#endif
#include <hxcpp.h>
#include "../../project/thirdparty/tracy-0.14.1/tracy/TracyC.h"
#include "../../project/thirdparty/tracy-0.14.1/tracy/Tracy.hpp"

#if TRACY_HAS_CALLSTACK == 1 || TRACY_HAS_CALLSTACK == 2 || TRACY_HAS_CALLSTACK == 5
// Clamp Haxe zone nesting callstack depth based on what Tracy accepts on this platform.
// Windows and Android throw an error at 63 or above, other platforms have no limit.
#define HXCPP_TRACY_MAX_CALLSTACK_DEPTH 62
#endif

inline int __hxcpp_tracy_callstack_depth(size_t zoneDepth)
{
	#ifdef HXCPP_TRACY_MAX_CALLSTACK_DEPTH
	if (zoneDepth > HXCPP_TRACY_MAX_CALLSTACK_DEPTH)
	{
		return HXCPP_TRACY_MAX_CALLSTACK_DEPTH;
	}
	#endif
	return static_cast<int>(zoneDepth);
}

#ifdef HXCPP_TRACY_MEMORY
	#ifdef HXCPP_GC_MOVING
		#error "Error: HXCPP_TRACY_MEMORY is not supported when HXCPP_GC_MOVING is active."
	#endif
	#ifdef HXCPP_GC_GENERATIONAL
		#error "Error: HXCPP_TRACY_MEMORY is not supported when HXCPP_GC_GENERATIONAL is active."
	#endif
#endif

namespace hx
{
	// Tracks a manually created Tracy zone.
	// Pushed onto the zone stack with any automatic function zones, so __hxcpp_tracy_zone_* targets the right zone.
	// Manual zones are always recorded, and don't count toward HXCPP_TRACY_MAX_ZONE_DEPTH.
	class TracyZoneScope
	{
	public:
		TracyZoneScope(const ::hx::StackFrame& frame, const ::String& name, const ::String& text);
		~TracyZoneScope();

		TracyZoneScope(const TracyZoneScope&) = delete;
		TracyZoneScope& operator=(const TracyZoneScope&) = delete;

	private:
		::hx::Telemetry* telemetry;
	};
}

// Create a named zone for the rest of the enclosing scope.
#define HXCPP_TRACY_ZONE(name) ::hx::TracyZoneScope TracyConcat(_hx_tracy_zone, TracyLine)(_hx_stackframe, name, ::String());

// Create a zone with a fixed name along with additional dynamic text, such as a call parameter.
// Use this instead of providing unique names for zones, since Tracy has a hard cap on source locations.
#define HXCPP_TRACY_ZONE_TEXT(name, text) ::hx::TracyZoneScope TracyConcat(_hx_tracy_zone, TracyLine)(_hx_stackframe, name, text);

// Whether Tracy is connected. Nothing is recorded if this is false and HXCPP_TRACY_ON_DEMAND is on.
bool __hxcpp_tracy_is_connected();
void __hxcpp_tracy_framemark();
void __hxcpp_tracy_framemark_named(::String name);
void __hxcpp_tracy_framemark_start(::String name);
void __hxcpp_tracy_framemark_end(::String name);
void __hxcpp_tracy_plot(::String name, ::Float val);
void __hxcpp_tracy_plot_config(::String name, uint8_t format, bool step, bool fill, int color);
void __hxcpp_tracy_message(::String msg, int color);
void __hxcpp_tracy_message_app_info(::String info);
void __hxcpp_tracy_set_thread_name_and_group(String name, int groupHint);
// Enable or disable the zones created automatically for Haxe function calls on the calling thread. Manual zones are unaffected.
// Returns the previous setting so callers can restore it.
bool __hxcpp_tracy_set_function_zones(bool enabled);
int __hxcpp_tracy_get_zone_count();

// Sets the properties of the current zone on the calling thread (either a manual zone or function zone).
// No-op if the function zone was skipped.
void __hxcpp_tracy_zone_text(::String text);
void __hxcpp_tracy_zone_name(::String name);
void __hxcpp_tracy_zone_color(int color);
void __hxcpp_tracy_zone_value(::cpp::Int64 value);

#endif
