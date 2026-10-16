# Writes ${OUT} (a header defining MH4N_GIT_REVISION) when its content changes.
# Run by the mh4n_version target: cmake -DSOURCE_DIR=<native/> -DOUT=<header> -P GitRevision.cmake
set(revision "unknown")
find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" describe --always --dirty --abbrev=10
        OUTPUT_VARIABLE described
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE status
        ERROR_QUIET)
    if (status EQUAL 0 AND NOT described STREQUAL "")
        set(revision "${described}")
    endif()
endif()
set(content "#pragma once\n#define MH4N_GIT_REVISION \"${revision}\"\n")
set(old "")
if (EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if (NOT old STREQUAL content)
    get_filename_component(dir "${OUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${OUT}" "${content}")
endif()
