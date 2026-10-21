# mh4n_services_fs: fs:USER and its file sessions, the archives of RomFS and system content (N1-12).
#
# The open-source replacements of the system archives are native/third_party/open_source_archives/data/*.dat.
# embed_archive.cmake turns each into a C++ source file at build time (only when the .dat changes), so no
# generated source is kept in the repository.
set(MH4N_ARCHIVE_DATA "${MH4N_ROOT}/third_party/open_source_archives/data")
set(embedded_sources "")
foreach (entry IN ITEMS bad_word_list:kBadWordList country_list:kCountryList mii:kMii shared_font:kSharedFont)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 file_name)
    list(GET parts 1 symbol)
    set(data_file "${MH4N_ARCHIVE_DATA}/${file_name}.dat")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_${file_name}.cpp")
    add_custom_command(
        OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND ${CMAKE_COMMAND} "-DINPUT=${data_file}" "-DOUTPUT=${generated}" "-DSYMBOL=${symbol}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_archive.cmake"
        DEPENDS "${data_file}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_archive.cmake"
        COMMENT "Embedding ${file_name}.dat"
        VERBATIM)
    list(APPEND embedded_sources "${generated}")
endforeach()

mh4n_module(
    SOURCES
        archive_extsavedata.cpp
        archive_ncch.cpp
        archive_savedata.cpp
        archive_selfncch.cpp
        disk_file.cpp
        fs_dir.cpp
        fs_file.cpp
        fs_state.cpp
        fs_user.cpp
        host_files.cpp
        io.cpp
        options.cpp
        positioned_file.cpp
        ${embedded_sources}
    DEPENDS
        mh4n_ipc
        mh4n_kernel
        mh4n_loader
        mh4n_runtime
)
# The generated sources are not our code: no -Werror for them.
set_source_files_properties(${embedded_sources} PROPERTIES COMPILE_OPTIONS "$<IF:$<BOOL:${MSVC}>,/w,-w>")
