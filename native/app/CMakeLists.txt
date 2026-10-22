# The app tree of the native port (docs/native/app.md, N2b-03): mh4u-native's window, UI and glue,
# against SDL3, Dear ImGui and the app's shared sources (port/app). Configured only with
# -DMH4N_APP=ON (native/CMakeLists.txt); the modules under native/src never see any of it.
#
#   native/app/<path>/CMakeLists.txt   an app module (mh4n_app_module), found at any depth, except
#                                      under program/, tests/ and third_party/
#   native/app/program/                mh4u-native
#   native/app/tests/<name>_test.cpp   a doctest binary linked against mh4n::app
#
# SDL2 and SDL3 export the same function names: nothing here links SDL2.

set(MH4N_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
# the app's shared sources (config.h, app_window.h, ui/, util/, ...), included as "name.h"
set(MH4N_APP_SHARED_DIR "${MH4N_ROOT}/../port/app")
get_filename_component(MH4N_APP_SHARED_DIR "${MH4N_APP_SHARED_DIR}" ABSOLUTE)

# ---- third party -------------------------------------------------------------------------------
# Dear ImGui v1.92.9b: the core files the Azahar app also builds, and the SDL3 platform backend.
set(MH4N_IMGUI_DIR "${MH4N_APP_SHARED_DIR}/third_party/imgui")
add_library(mh4n_imgui STATIC
    ${MH4N_IMGUI_DIR}/imgui.cpp
    ${MH4N_IMGUI_DIR}/imgui_draw.cpp
    ${MH4N_IMGUI_DIR}/imgui_tables.cpp
    ${MH4N_IMGUI_DIR}/imgui_widgets.cpp
    ${MH4N_APP_DIR}/third_party/imgui/backends/imgui_impl_sdl3.cpp)
target_include_directories(mh4n_imgui SYSTEM PUBLIC ${MH4N_IMGUI_DIR}
                           ${MH4N_APP_DIR}/third_party/imgui/backends)
target_link_libraries(mh4n_imgui PUBLIC SDL3::SDL3)
target_compile_definitions(mh4n_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(mh4n_imgui PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)

# inih (BSD-3-Clause), laid out as Azahar's externals/inih so the include flags are the same.
set(MH4N_INIH_DIR "${MH4N_ROOT}/third_party/inih/inih")
add_library(mh4n_inih STATIC ${MH4N_INIH_DIR}/ini.c ${MH4N_INIH_DIR}/cpp/INIReader.cpp)
target_include_directories(mh4n_inih SYSTEM PUBLIC ${MH4N_INIH_DIR} ${MH4N_INIH_DIR}/cpp)
target_compile_options(mh4n_inih PRIVATE $<IF:$<BOOL:${MSVC}>,/w,-w>)

# ---- helpers -----------------------------------------------------------------------------------
# mh4n_app_module(SOURCES a.cpp ... [DEPENDS mh4n_app_x ...] [THIRD_PARTY target ...])
#
# Called from native/app/<path>/CMakeLists.txt; defines the static library mh4n_app_<path with '_'
# for '/'>. Like mh4n_module: app modules in DEPENDS are linked whole (their registrations must
# survive the linker), third-party targets normally. Every app module sees the app's shared
# headers (port/app), SDL3, Dear ImGui, inih and {fmt}.
set_property(GLOBAL PROPERTY MH4N_APP_MODULES "")

function(mh4n_app_module)
    cmake_parse_arguments(PARSE_ARGV 0 ARG "" "" "SOURCES;DEPENDS;THIRD_PARTY")
    if (ARG_UNPARSED_ARGUMENTS OR ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "mh4n_app_module: bad arguments '${ARG_UNPARSED_ARGUMENTS}' "
                            "'${ARG_KEYWORDS_MISSING_VALUES}' in ${CMAKE_CURRENT_LIST_DIR}")
    endif()
    file(RELATIVE_PATH rel "${MH4N_APP_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    if (rel STREQUAL "" OR rel MATCHES "^\\.\\.")
        message(FATAL_ERROR "mh4n_app_module is for directories under native/app/ "
                            "(${CMAKE_CURRENT_SOURCE_DIR})")
    endif()
    if (NOT ARG_SOURCES)
        message(FATAL_ERROR "mh4n_app_module: ${rel} has no SOURCES")
    endif()
    string(REPLACE "/" "_" suffix "${rel}")
    set(name "mh4n_app_${suffix}")

    add_library(${name} STATIC ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE mh4n_warnings)
    target_link_libraries(${name} PUBLIC mh4n_headers SDL3::SDL3 mh4n_imgui mh4n_inih fmt::fmt)
    target_include_directories(${name} PUBLIC "${MH4N_APP_SHARED_DIR}")

    set(deps ${ARG_DEPENDS})
    list(REMOVE_DUPLICATES deps)
    foreach (dep IN LISTS deps)
        if (dep STREQUAL name)
            message(FATAL_ERROR "mh4n_app_module: ${name} depends on itself")
        endif()
        target_link_libraries(${name} PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,${dep}>")
    endforeach()
    if (ARG_THIRD_PARTY)
        target_link_libraries(${name} PUBLIC ${ARG_THIRD_PARTY})
    endif()

    set_property(GLOBAL APPEND PROPERTY MH4N_APP_MODULES ${name})
    set_property(GLOBAL APPEND PROPERTY MH4N_APP_MODULE_DEPS ${deps})
endfunction()

# ---- modules -----------------------------------------------------------------------------------
file(GLOB_RECURSE app_module_lists CONFIGURE_DEPENDS LIST_DIRECTORIES false
     "${MH4N_APP_DIR}/*/CMakeLists.txt")
list(SORT app_module_lists)
foreach (list_file IN LISTS app_module_lists)
    get_filename_component(module_dir "${list_file}" DIRECTORY)
    file(RELATIVE_PATH module_rel "${MH4N_APP_DIR}" "${module_dir}")
    if (module_rel MATCHES "^(program|tests|third_party)(/|$)")
        continue()
    endif()
    add_subdirectory("${module_dir}" "${CMAKE_BINARY_DIR}/app/${module_rel}")
endforeach()

get_property(MH4N_ALL_APP_MODULES GLOBAL PROPERTY MH4N_APP_MODULES)
get_property(MH4N_ALL_APP_DEPS GLOBAL PROPERTY MH4N_APP_MODULE_DEPS)
list(REMOVE_DUPLICATES MH4N_ALL_APP_DEPS)
foreach (dep IN LISTS MH4N_ALL_APP_DEPS)
    if (NOT dep IN_LIST MH4N_ALL_APP_MODULES)
        message(FATAL_ERROR "An app module depends on '${dep}', which is not a module under "
                            "native/app/ (modules: ${MH4N_ALL_APP_MODULES})")
    endif()
endforeach()
# (and nothing under native/src can depend on an app module: mh4n_module accepts only modules of
# native/src, so a DEPENDS naming one is the configure error "not a module under native/src/")

# mh4n::app: every app module (linked whole), every native module, SDL3, Dear ImGui and inih.
add_library(mh4n_app INTERFACE)
add_library(mh4n::app ALIAS mh4n_app)
target_link_libraries(mh4n_app INTERFACE mh4n::all SDL3::SDL3 mh4n_imgui mh4n_inih)
foreach (module IN LISTS MH4N_ALL_APP_MODULES)
    target_link_libraries(mh4n_app INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${module}>")
endforeach()

# ---- the program and the tests -----------------------------------------------------------------
add_subdirectory(program)
if (MH4N_TESTS)
    add_subdirectory(tests)
endif()
