#include <hxcpp.h>
#include <hx/TelemetryTracy.h>

#include <vector>
#include <mutex>
#include <string>
#include <unordered_set>

namespace
{
	// Manual zone for garbage collection, unaffected by HXCPP_TRACY_MAX_ZONE_DEPTH and __hxcpp_tracy_set_function_zones.
	// Only the thread performing garbage collection uses it, between __hxt_gc_start and __hxt_gc_end.
	TracyCZoneCtx gcZone;

	const ___tracy_source_location_data gcSourceLocation = { "GC::collect", "__hxt_gc_start", TracyFile, (uint32_t)TracyLine, 0 };

	// Plotted after each collection. memInfo values match MEM_INFO_* in Immix.cpp.
	struct GcPlot
	{
		const char* name;
		int memInfo;
	};

	const GcPlot gcPlots[] = {
		{ "GC Memory Used", 0 },
		{ "GC Memory Reserved", 1 },
		{ "GC Large Objects", 3 },
	};

	bool gcPlotsConfigured = false;

	const char* sohName = "Small Object Heap";
	const char* lohName = "Large Object Heap";

	std::vector<hx::Telemetry*> created;
	std::mutex createdLock;

	hx::QuickVec<void*> largeAllocs;
	std::mutex largeAllocsLock;

	// Hold a reference to small objects when their thread is destroyed, until Tracy can learn they've been freed.
	// Only used by tlmDestroy and GC sweep, which both run under the gThreadStateChangeLock mutex.
	hx::QuickVec<void*> orphanedSmallAllocs;

	// Tracy receives name pointers from HXCPP and reads them from its thread.
	// If the pointer is then freed, Tracy can't identify the frame set or plot.
	// (This is mainly an issue for non-ASCII strings!)
	// We use a set so each unique name is only stored once, and never free it so Tracy can read the names while the program is shutting down.
	std::unordered_set<std::string>* internedNames = new std::unordered_set<std::string>();
	std::mutex internedNamesLock;

	// Store the provided name as a UTF-8 pointer that Tracy can always access.
	const char* internName(const String& name)
	{
		if (!name.raw_ptr())
		{
			return nullptr;
		}

		hx::strbuf buffer;
		int byteLength = 0;
		const char* utf8 = name.utf8_str(&buffer, false, &byteLength);

		std::lock_guard<std::mutex> lock(internedNamesLock);

		return internedNames->emplace(utf8, byteLength).first->c_str();
	}

	bool isLargeObject(void* ptr)
	{
		auto ptrHeader = reinterpret_cast<size_t>(ptr) - sizeof(int);
		auto flags     = *reinterpret_cast<unsigned int*>(ptrHeader);

		return (flags & 0xffff) == 0;
	}

	// Reports every allocation in the list that was not marked this collection as freed, and removes it from the list.
	void sweepUnmarked(hx::QuickVec<void*>& allocs, const char* poolName, int byteMarkId, int endianByteMarkId)
	{
		hx::QuickVec<void*> retained;

		retained.safeReserveExtra(allocs.size());

		for (auto i = 0; i < allocs.size(); i++)
		{
			auto ptr      = allocs[i];
			auto markByte = reinterpret_cast<unsigned char*>(ptr)[endianByteMarkId];
			if (markByte != byteMarkId)
			{
				TracyFreeN(ptr, poolName);
			}
			else
			{
				retained.push(ptr);
			}
		}

		allocs.swap(retained);
	}
}

namespace hx
{
	class Telemetry
	{
	public:
		// The open zones (manual and Haxe function) on this thread, oldest to newest.
		std::vector<TracyCZoneCtx> tracyZones;
		// A count of the manual tracyZones.
		size_t manualZones;
		hx::QuickVec<void*> smallAllocs;

		// Whether Haxe function calls on this thread create zones. Set by __hxcpp_tracy_set_function_zones.
		bool functionZones;

		Telemetry() : tracyZones(0), manualZones(0), smallAllocs(), functionZones(true) {}

		// The current nesting depth of Haxe functions, ignoring manual zones.
		size_t functionZoneDepth() const
		{
			return tracyZones.size() - manualZones;
		}
	};
}

int __hxcpp_hxt_start_telemetry(bool profiler, bool allocations)
{
	hx::Throw(HX_CSTRING("Not implemented"));

	return 0;
}

TelemetryFrame* __hxcpp_hxt_dump_telemetry(int)
{
	hx::Throw(HX_CSTRING("Not implemented"));

	return 0;
}

void __hxcpp_hxt_stash_telemetry() { }

void __hxcpp_hxt_ignore_allocs(int) {}

void __hxt_new_string(void* obj, int inSize) { }

void __hxt_new_array(void* obj, int inSize) { }

void __hxt_new_hash(void* obj, int inSize) { }

void __hxt_gc_new(hx::StackContext* stack, void* obj, int inSize, const char* name) { }

void __hxt_gc_alloc(void* obj, int inSize)
{
	#ifdef HXCPP_TRACY_MEMORY
		if (isLargeObject(obj))
		{
			std::lock_guard<std::mutex> lock(largeAllocsLock);

			largeAllocs.push(obj);

			TracyAllocN(obj, inSize, lohName);
		}
		else
		{
			hx::StackContext::getCurrent()->mTelemetry->smallAllocs.push(obj);

			TracyAllocN(obj, inSize, sohName);
		}
	#endif
}

void __hxt_gc_free_large(void* obj)
{
	std::lock_guard<std::mutex> lock(largeAllocsLock);

	for (auto i = 0; i < largeAllocs.size(); i++)
	{
		if (largeAllocs[i] == obj)
		{
			largeAllocs.erase(i);

			TracyFreeN(obj, lohName);

			return;
		}
	}
}

void __hxt_gc_realloc(void* oldObj, void* newObj, int newSize) { }

void __hxt_gc_after_mark(int byteMarkId, int endianByteMarkId)
{
	#ifdef HXCPP_TRACY_MEMORY
		std::lock_guard<std::mutex> lock(createdLock);

		for (auto&& telemetry : created)
		{
			sweepUnmarked(telemetry->smallAllocs, sohName, byteMarkId, endianByteMarkId);
		}

		sweepUnmarked(orphanedSmallAllocs, sohName, byteMarkId, endianByteMarkId);

		sweepUnmarked(largeAllocs, lohName, byteMarkId, endianByteMarkId);
	#endif
}

void __hxt_gc_start()
{
	gcZone = ___tracy_emit_zone_begin(&gcSourceLocation, true);
}

void __hxt_gc_end()
{
	___tracy_emit_zone_end(gcZone);

	// Heap totals after each collection. Unlike HXCPP_TRACY_MEMORY, these work with HXCPP_GC_GENERATIONAL.
	// The values are read while the collector holds gThreadStateChangeLock and no thread can allocate,
	// which also serializes the one-time configuration below. On-demand mode resends it to each new connection.
	if (!gcPlotsConfigured)
	{
		for (auto&& plot : gcPlots)
		{
			::tracy::Profiler::ConfigurePlot(plot.name, ::tracy::PlotFormatType::Memory, true, true, 0);
		}
		gcPlotsConfigured = true;
	}

	for (auto&& plot : gcPlots)
	{
		::tracy::Profiler::PlotData(plot.name, static_cast<int64_t>(__hxcpp_gc_mem_info(plot.memInfo)));
	}
}

hx::Telemetry* hx::tlmCreate(StackContext* stack)
{
	auto obj = new hx::Telemetry();

	std::lock_guard<std::mutex> lock(createdLock);

	created.push_back(obj);

	return obj;
}

void hx::tlmDestroy(hx::Telemetry* telemetry)
{
	std::lock_guard<std::mutex> lock(createdLock);

	created.erase(std::find(created.begin(), created.end(), telemetry));

	#ifdef HXCPP_TRACY_MEMORY
	// If we drop this thread's small allocations here, Tracy doesn't learn they've been freed.
	// If the GC then reuses those addresses, Tracy will throw an error.
	// Instead, we hold onto them to free later.

	orphanedSmallAllocs.safeReserveExtra(telemetry->smallAllocs.size());

	for (auto i = 0; i < telemetry->smallAllocs.size(); i++)
	{
		orphanedSmallAllocs.push(telemetry->smallAllocs[i]);
	}
	#endif

	delete telemetry;
}

void hx::tlmAttach(hx::Telemetry* telemetry, hx::StackContext* stack)
{
	// Telemetry is pooled with its StackContext, so a newly attached thread must not inherit the last thread's setting.
	telemetry->functionZones = true;
}

void hx::tlmDetach(hx::Telemetry* telemetry)
{
	//
}

void hx::tlmSampleEnter(hx::Telemetry* telemetry, hx::StackFrame* frame)
{
	// Skip function zones if we call __hxcpp_tracy_set_function_zones,
	// or reach a maximum configured depth.
	bool skipZone = !telemetry->functionZones;

	#ifdef HXCPP_TRACY_MAX_ZONE_DEPTH
	skipZone = skipZone || telemetry->functionZoneDepth() >= HXCPP_TRACY_MAX_ZONE_DEPTH;
	#endif

	if (skipZone)
	{
		telemetry->tracyZones.push_back(TracyCZoneCtx{});

		return;
	}

	auto srcloc =
		___tracy_alloc_srcloc(
			frame->lineNumber,
			frame->position->fileName,
			strlen(frame->position->fileName),
			frame->position->fullName,
			strlen(frame->position->fullName),
			0);

	#if HXCPP_TRACY_INCLUDE_CALLSTACKS
	// Determine depth from function zone depth: +1 since we are about to add one.
	// Clamped when required by Tracy (see __hxcpp_tracy_callstack_depth).
	auto depth = __hxcpp_tracy_callstack_depth(telemetry->functionZoneDepth() + 1);

	telemetry->tracyZones.push_back(___tracy_emit_zone_begin_alloc_callstack(srcloc, depth, true));
	#else
	telemetry->tracyZones.push_back(___tracy_emit_zone_begin_alloc(srcloc, true));
	#endif
}

void hx::tlmSampleExit(hx::Telemetry* telemetry)
{
	if (telemetry->tracyZones.empty())
	{
		return;
	}

	___tracy_emit_zone_end(telemetry->tracyZones.back());

	telemetry->tracyZones.pop_back();
}

bool __hxcpp_tracy_is_connected()
{
	return ::tracy::GetProfiler().IsConnected();
}

void __hxcpp_tracy_framemark()
{
	::tracy::Profiler::SendFrameMark(0);
}

void __hxcpp_tracy_framemark_named(String name)
{
	::tracy::Profiler::SendFrameMark(internName(name));
}

void __hxcpp_tracy_framemark_start(String name)
{
	___tracy_emit_frame_mark_start(internName(name));
}

void __hxcpp_tracy_framemark_end(String name)
{
	___tracy_emit_frame_mark_end(internName(name));
}

void __hxcpp_tracy_plot(String name, ::Float val)
{
	::tracy::Profiler::PlotData(internName(name), val);
}

void __hxcpp_tracy_plot_config(String name, uint8_t format, bool step, bool fill, int color)
{
	::tracy::Profiler::ConfigurePlot(internName(name), ::tracy::PlotFormatType(format), step, fill, color);
}

void __hxcpp_tracy_message(String msg, int color)
{
	hx::strbuf buffer;
	int byteLength = 0;
	const char *utf8 = msg.utf8_str(&buffer, false, &byteLength);
	::tracy::Profiler::LogString(::tracy::MessageSourceType::User, ::tracy::MessageSeverity::Info, color, 0, byteLength, utf8);
}

void __hxcpp_tracy_message_app_info(String info)
{
	hx::strbuf buffer;
	int byteLength = 0;
	const char *utf8 = info.utf8_str(&buffer, false, &byteLength);
	::tracy::Profiler::MessageAppInfo(utf8, byteLength);
}

void __hxcpp_tracy_set_thread_name_and_group(String name, int groupHint)
{
	hx::strbuf buffer;
	::tracy::SetThreadNameWithHint(name.utf8_str(&buffer), groupHint);
}

bool __hxcpp_tracy_set_function_zones(bool enabled)
{
	auto telemetry = hx::StackContext::getCurrent()->mTelemetry;
	bool previous = telemetry->functionZones;

	telemetry->functionZones = enabled;

	return previous;
}

int __hxcpp_tracy_get_zone_count()
{
	return static_cast<int>(hx::StackContext::getCurrent()->mTelemetry->functionZoneDepth());
}

hx::TracyZoneScope::TracyZoneScope(const hx::StackFrame& frame, const String& name, const String& text)
	: telemetry(frame.ctx->mTelemetry)
{
	hx::strbuf nameBuffer;
	int nameLength = 0;
	const char* utf8Name = name.utf8_str(&nameBuffer, false, &nameLength);

	// Provide source location data for the zone.
	auto srcloc =
		___tracy_alloc_srcloc_name(
			frame.lineNumber,
			frame.position->fileName,
			strlen(frame.position->fileName),
			frame.position->fullName,
			strlen(frame.position->fullName),
			utf8Name,
			nameLength,
			0);

	// Create the zone.
	#ifdef HXCPP_TRACY_INCLUDE_CALLSTACKS
	auto zone = ___tracy_emit_zone_begin_alloc_callstack(srcloc, __hxcpp_tracy_callstack_depth(telemetry->functionZoneDepth()), true);
	#else
	auto zone = ___tracy_emit_zone_begin_alloc(srcloc, true);
	#endif

	// Apply zone text if relevant.
	if (text.raw_ptr())
	{
		hx::strbuf textBuffer;
		int textLength = 0;
		const char* utf8Text = text.utf8_str(&textBuffer, false, &textLength);

		___tracy_emit_zone_text(zone, utf8Text, textLength);
	}

	telemetry->tracyZones.push_back(zone);
	telemetry->manualZones++;
}

hx::TracyZoneScope::~TracyZoneScope()
{
	// Clean up the zone.
	___tracy_emit_zone_end(telemetry->tracyZones.back());

	telemetry->tracyZones.pop_back();
	telemetry->manualZones--;
}

namespace
{
	// The thread's innermost profiling zone.
	// Used when setting zone properties (Tracy only allows the current innermost zone to be assigned).
	// Property calls on skipped zones do nothing (no errors!).
	TracyCZoneCtx innermostZone()
	{
		auto telemetry = hx::StackContext::getCurrent()->mTelemetry;

		if (telemetry->tracyZones.empty())
		{
			return TracyCZoneCtx{};
		}

		return telemetry->tracyZones.back();
	}
}

void __hxcpp_tracy_zone_text(String text)
{
	// Set the zone's text, accounting for UTF-8 string length.
	hx::strbuf buffer;
	int byteLength = 0;
	const char* utf8 = text.utf8_str(&buffer, false, &byteLength);

	___tracy_emit_zone_text(innermostZone(), utf8, byteLength);
}

void __hxcpp_tracy_zone_name(String name)
{
	// Set the zone's name, accounting for UTF-8 string length.
	hx::strbuf buffer;
	int byteLength = 0;
	const char* utf8 = name.utf8_str(&buffer, false, &byteLength);

	___tracy_emit_zone_name(innermostZone(), utf8, byteLength);
}

void __hxcpp_tracy_zone_color(int color)
{
	// Set the zone's color.
	___tracy_emit_zone_color(innermostZone(), static_cast<uint32_t>(color));
}

void __hxcpp_tracy_zone_value(cpp::Int64 value)
{
	// Set the zone's value.
	___tracy_emit_zone_value(innermostZone(), static_cast<uint64_t>(value));
}
