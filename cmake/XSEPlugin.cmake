add_compile_definitions(SKYRIM)
set(CommonLibPath "extern/CommonLibSSE-NG")
set(CommonLibName "CommonLibSSE")

add_library("${PROJECT_NAME}" SHARED)

target_compile_features(
	"${PROJECT_NAME}"
	PRIVATE
	cxx_std_23
)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(AddCXXFiles)
add_cxx_files("${PROJECT_NAME}")

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/Plugin.h.in
	${CMAKE_CURRENT_BINARY_DIR}/cmake/Plugin.h
	@ONLY
)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
	${CMAKE_CURRENT_BINARY_DIR}/cmake/version.rc
	@ONLY
)

target_sources(
	"${PROJECT_NAME}"
	PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}/cmake/Plugin.h
	${CMAKE_CURRENT_BINARY_DIR}/cmake/version.rc
)

target_precompile_headers(
	"${PROJECT_NAME}"
	PRIVATE
	include/PCH.h
)

# LTO defaults ON; presets can override via cache variable to skip the
# expensive LTCG link pass during development iteration.
if(NOT DEFINED CACHE{CMAKE_INTERPROCEDURAL_OPTIMIZATION})
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME ON)

set(BUILD_TESTS OFF)

# Define _WINDOWS for all Windows builds (required by FidelityFX API loader)
if(WIN32)
	add_compile_definitions(_WINDOWS)
endif()

# Build flavors (Release config), selected by presets:
#   shipping (ALL/Package): CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON (default)
#     -> max performance: /O2 /Zi /GL + /LTCG, full self-contained PDB.
#   PR / CI (PR preset):    IPO=OFF, SC_COMPILE_PDB=OFF, SC_DEVFAST_OPTS=OFF
#     -> mild optimization: /O2 without LTO, no compile-time debug info; the
#        linker still writes a small public-symbols PDB so packaging rules and
#        crash-log function names keep working. Measured ~2x faster than LTCG.
#   dev local (Dev-Fast):   IPO=OFF, SC_DEVFAST_OPTS=ON
#     -> fastest iteration: /Od /Z7 + incremental link, full PDB. Measured 2x
#        faster clean builds than /O2 and ~1.5x faster heavy-TU rebuilds; runs
#        slower in-game and must NEVER ship.
option(SC_DEVFAST_OPTS "Use minimal optimization (/Od) and incremental linking for fast dev iteration" OFF)
option(SC_COMPILE_PDB "Generate compile-time debug info (full PDB). OFF leaves only linker public symbols" ON)

if(MSVC)
	# Define both: the Visual Studio generator implies UNICODE via the project
	# CharacterSet, but single-config generators (Ninja) do not — without it,
	# Win32 TCHAR APIs (GetModuleHandle etc.) resolve to their ANSI variants.
	add_compile_definitions(_UNICODE UNICODE)

	target_compile_definitions(${PROJECT_NAME} PRIVATE "$<$<CONFIG:DEBUG>:DEBUG>")

	set(SC_DEBUG_OPTS "/fp:strict;/ZI;/Od;/Gy")

	# Optimization level: full opt for shipping; minimal opt (/Od) for fast dev
	# iteration when SC_DEVFAST_OPTS is ON. On the no-LTO dev path the optimizer
	# time of the single recompiled TU dominates, so /Od is the per-file win.
	# /Gy (function-level linking) is enabled on the dev path to pair with the
	# incremental linker below; the shipping path keeps /Gy- as before.
	if(SC_DEVFAST_OPTS)
		set(SC_RELEASE_OPTS "/fp:fast;/Gy;/Gm-;/Gw;/sdl-;/GS-;/guard:cf-;/Od;/Ob1;/fp:except-")
	else()
		set(SC_RELEASE_OPTS "/fp:fast;/Gy-;/Gm-;/Gw;/sdl-;/GS-;/guard:cf-;/O2;/Ob2;/Oi;/Ot;/Oy;/fp:except-")
	endif()

	# Debug-info format and whole-program optimization depend on the build path.
	# Shipping (LTO on): separate PDB (/Zi) + whole-program optimization (/GL).
	# Dev (LTO off, SC_COMPILE_PDB on): embedded debug info (/Z7) avoids mspdbsrv
	# PDB-lock contention across parallel compiles and keeps debug data in the
	# .obj for caching. PR/CI (SC_COMPILE_PDB off): no compile-time debug info at
	# all — the linker's /DEBUG below still emits a public-symbols-only PDB.
	if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
		string(PREPEND SC_RELEASE_OPTS "/Zi;")
		string(APPEND SC_RELEASE_OPTS ";/GL")
	elseif(SC_COMPILE_PDB)
		string(PREPEND SC_RELEASE_OPTS "/Z7;")
	endif()

	target_compile_options(
		"${PROJECT_NAME}"
		PRIVATE
		/W4
		/WX
		/permissive-
		/Zc:alignedNew
		/Zc:auto
		/Zc:__cplusplus
		/Zc:externC
		/Zc:externConstexpr
		/Zc:forScope
		/Zc:hiddenFriend
		/Zc:implicitNoexcept
		/Zc:lambda
		/Zc:noexceptTypes
		/Zc:preprocessor
		/Zc:referenceBinding
		/Zc:rvalueCast
		/Zc:sizedDealloc
		/Zc:strictStrings
		/Zc:ternary
		/Zc:threadSafeInit
		/Zc:trigraphs
		/Zc:wchar_t
		/wd4200 # nonstandard extension used : zero-sized array in struct/union
	)

	# /MP (multi-process compilation) only for MSBuild; Ninja handles parallelism itself
	if(CMAKE_GENERATOR MATCHES "Visual Studio")
		target_compile_options("${PROJECT_NAME}" PRIVATE /MP)
	endif()

	target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:DEBUG>:${SC_DEBUG_OPTS}>")
	target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:RELEASE>:${SC_RELEASE_OPTS}>")

	if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
		target_link_options(
			${PROJECT_NAME}
			PRIVATE
			/WX
			"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
			"$<$<CONFIG:RELEASE>:/LTCG;/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
		)
	elseif(SC_DEVFAST_OPTS)
		# Dev (no-LTO) path: true incremental linking. /OPT:REF and /OPT:ICF are
		# mutually exclusive with /INCREMENTAL (the linker emits LNK4075 and
		# silently falls back to a full link), so the shipping flags above would
		# force a full ~18MB DLL link on every 1-file rebuild. /OPT:NOREF
		# /OPT:NOICF + /INCREMENTAL let the linker patch only changed functions.
		# /DEBUG:FULL keeps the PDB self-contained and deployable next to the
		# DLL — crash loggers and debuggers on other machines can resolve
		# symbols without the local .obj files. (/DEBUG:FASTLINK is no longer
		# supported by the VS2026 toolchain: LNK4315, fatal under /WX.)
		target_link_options(
			${PROJECT_NAME}
			PRIVATE
			/WX
			"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
			"$<$<CONFIG:RELEASE>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF;/DEBUG:FULL>"
		)
	else()
		# PR / CI path (no LTO, no incremental): compact one-shot link. /OPT:REF
		# /OPT:ICF keep the DLL shipping-sized; /DEBUG:FULL emits a PDB that is
		# small here because the objects carry no debug info (SC_COMPILE_PDB
		# OFF) — public symbols only, enough for function names in crash logs
		# and required by the $<TARGET_PDB_FILE> packaging rules.
		target_link_options(
			${PROJECT_NAME}
			PRIVATE
			/WX
			"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
			"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
		)
	endif()
endif()

add_subdirectory(${CommonLibPath} ${CommonLibName} EXCLUDE_FROM_ALL)

# CommonLibSSE-NG forces CMAKE_INTERPROCEDURAL_OPTIMIZATION ON for Release in its
# own CMakeLists, so its static lib ships /GL (whole-program) objects. Linking a
# /GL object drags LTCG into EVERY plugin link — even on the no-LTO dev path —
# and /GL is incompatible with the incremental linker (LNK4075, fatal under /WX).
# On the dev path (IPO off) force CommonLib's IPO off too so its objects carry no
# /GL: this is what actually lets the dev link skip LTCG and link incrementally.
# The shipping path leaves CommonLib's IPO untouched (full /GL + /LTCG).
if(MSVC AND NOT CMAKE_INTERPROCEDURAL_OPTIMIZATION)
	set_target_properties(
		${CommonLibName}
		PROPERTIES
		INTERPROCEDURAL_OPTIMIZATION OFF
		INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
	)
endif()

find_package(spdlog CONFIG REQUIRED)

target_include_directories(
	${PROJECT_NAME}
	PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}/cmake
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
	${PROJECT_NAME}
	PUBLIC
	CommonLibSSE::CommonLibSSE
)