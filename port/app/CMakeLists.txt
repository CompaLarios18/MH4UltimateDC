# mh4u: the port's own app (main.cpp). A window of its own (SDL2) around Azahar's core with the
# game's recompiled code as the CPU. Built with port/ when Azahar is configured with SDL2.

# Dear ImGui (MIT, third_party/imgui/LICENSE.txt), v1.92.9b, for the app's menus: the core and
# the SDL2 platform backend here; each renderer's backend is added where it is used.
set(MH4U_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_library(mh4u_imgui STATIC
    ${MH4U_IMGUI_DIR}/imgui.cpp
    ${MH4U_IMGUI_DIR}/imgui_draw.cpp
    ${MH4U_IMGUI_DIR}/imgui_tables.cpp
    ${MH4U_IMGUI_DIR}/imgui_widgets.cpp
    ${MH4U_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
)
target_include_directories(mh4u_imgui PUBLIC ${MH4U_IMGUI_DIR} ${MH4U_IMGUI_DIR}/backends)
target_link_libraries(mh4u_imgui PUBLIC SDL2::SDL2)
target_compile_definitions(mh4u_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS SDL_MAIN_HANDLED)

add_executable(mh4u
    config.cpp
    config.h
    dialog.cpp
    dialog.h
    input.cpp
    input.h
    main.cpp
    window.cpp
    window.h
    # the in-game menu (Dear ImGui over the game) and the 3DS software keyboard
    ui/keyboard.cpp
    ui/keyboard.h
    ui/layer.h
    ui/menu.cpp
    ui/menu.h
    ui/options.cpp
    ui/options.h
    ui/ui.cpp
    ui/ui.h
)
target_include_directories(mh4u PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# each renderer's window, with Dear ImGui's backend for it (compiled in ui/render_*.cpp)
if (ENABLE_OPENGL)
    target_sources(mh4u PRIVATE window_gl.cpp ui/render_gl.cpp)
    target_link_libraries(mh4u PRIVATE glad)
endif()
if (ENABLE_VULKAN)
    target_sources(mh4u PRIVATE window_vk.cpp ui/render_vk.cpp)
    target_link_libraries(mh4u PRIVATE vulkan-headers)
endif()
if (ENABLE_SOFTWARE_RENDERER)
    target_sources(mh4u PRIVATE window_sw.cpp ui/render_sw.cpp)
endif()

target_compile_definitions(mh4u PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(mh4u PRIVATE mh4u_backend mh4u_imgui audio_core citra_common citra_core
                      input_common network video_core inih SDL2::SDL2 ${PLATFORM_LIBRARIES}
                      Threads::Threads)
if (ENABLE_WEB_SERVICE)
    target_link_libraries(mh4u PRIVATE web_service)
endif()

if (WIN32)
    # a game's window, no console window behind it
    set_target_properties(mh4u PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(mh4u PRIVATE comdlg32 shell32)
endif()
if (APPLE)
    # Vulkan runs on MoltenVK, which Azahar's build fetches (MOLTENVK_LIBRARY); the package puts
    # it next to the executable
    set_target_properties(mh4u PROPERTIES BUILD_RPATH "@executable_path"
                          INSTALL_RPATH "@executable_path;@executable_path/../Frameworks")
endif()
