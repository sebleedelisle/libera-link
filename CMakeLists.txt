cmake_minimum_required(VERSION 3.18)

execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_TAG_RESULT
)
if(GIT_TAG_RESULT EQUAL 0 AND GIT_TAG MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+)")
  set(PROJECT_VERSION_FROM_GIT "${CMAKE_MATCH_1}")
else()
  set(PROJECT_VERSION_FROM_GIT "0.0.0")
endif()

project(LiberaLink VERSION ${PROJECT_VERSION_FROM_GIT} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LIBERA_LINK_APP_VERSION "${PROJECT_VERSION}")
set(LIBERA_LINK_APP_BUNDLE_NAME "Libera Link")

if(APPLE OR WIN32)
  set(LIBERA_LINK_DEFAULT_BUNDLED_LIBUSB ON)
else()
  set(LIBERA_LINK_DEFAULT_BUNDLED_LIBUSB OFF)
endif()
option(LIBERA_LINK_USE_BUNDLED_LIBUSB
    "Use bundled libusb from libera-laser dependency tree"
    ${LIBERA_LINK_DEFAULT_BUNDLED_LIBUSB})
option(LIBERA_LINK_OPENIDN_LINK_MODE "Enable Link-specific IDN behavior changes" ON)
option(LIBERA_LINK_BUILD_GUI "Build the native GUI target when ImGui/GLFW sources are available" ON)

set(LIBERA_LINK_GUI_GLFW_SOURCE_DIR "" CACHE PATH "Path to a GLFW source tree for the GUI target")
set(LIBERA_LINK_GUI_IMGUI_SOURCE_DIR "" CACHE PATH "Path to an ImGui source tree for the GUI target")
set(LIBERA_LINK_GUI_GLFW_GIT_TAG "3.4" CACHE STRING "Pinned GLFW tag used when fetching GUI dependencies")
set(LIBERA_LINK_GUI_IMGUI_GIT_TAG "v1.92.7-docking" CACHE STRING "Pinned Dear ImGui tag used when fetching GUI dependencies")

set(LIBERA_USE_BUNDLED_LIBUSB ${LIBERA_LINK_USE_BUNDLED_LIBUSB} CACHE BOOL "" FORCE)
set(LIBERA_BUNDLED_LIBUSB_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/libera-laser/libs/helios_dac/sdk/cpp"
    CACHE PATH "Path to bundled libusb headers/binaries")
set(LIBERA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBERA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LIBERA_BUILD_INTEGRATION_TESTS OFF CACHE BOOL "" FORCE)
set(LIBERA_BUILD_HARDWARE_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(extern/libera-laser)

set(OPENIDN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/openidn")

set(OPENIDN_SOURCES
    "${OPENIDN_ROOT}/shared/AdapterBase.cpp"
    "${OPENIDN_ROOT}/shared/DACHWInterface.cpp"
    "${OPENIDN_ROOT}/shared/DecoderBase.cpp"
    "${OPENIDN_ROOT}/shared/LaproAdapter.cpp"
    "${OPENIDN_ROOT}/shared/ODFTools.cpp"
    "${OPENIDN_ROOT}/output/RTOutput.cpp"
    "${OPENIDN_ROOT}/output/RTLaproGraphOut.cpp"
    "${OPENIDN_ROOT}/output/IDNLaproDecoder.cpp"
    "${OPENIDN_ROOT}/output/IdtfDecoder.cpp"
    "${OPENIDN_ROOT}/output/STDLaproGraphOut.cpp"
    "${OPENIDN_ROOT}/output/V1LaproGraphOut.cpp"
    "${OPENIDN_ROOT}/server/IDNService.cpp"
    "${OPENIDN_ROOT}/server/IDNSession.cpp"
    "${OPENIDN_ROOT}/server/IDNLaproService.cpp"
    "${OPENIDN_ROOT}/server/IDNLaproGraConInlet.cpp"
    "${OPENIDN_ROOT}/server/IDNLaproGraDisInlet.cpp"
    "${OPENIDN_ROOT}/server/IDNServer.cpp"
    "${OPENIDN_ROOT}/stage/SockIDNServer.cpp"
)

if(NOT LIBERA_LINK_OPENIDN_LINK_MODE)
  message(FATAL_ERROR
      "LIBERA_LINK_OPENIDN_LINK_MODE=OFF is no longer supported by the vendored "
      "third_party/openidn subset."
  )
endif()

add_library(openidn-core STATIC ${OPENIDN_SOURCES})
target_compile_definitions(openidn-core PUBLIC ODF_USE_TAXI_SOCK)
if(LIBERA_LINK_OPENIDN_LINK_MODE)
  target_compile_definitions(openidn-core PUBLIC OPENIDN_LINK_MODE=1)
endif()
target_include_directories(openidn-core PUBLIC "${OPENIDN_ROOT}")
if(MSVC)
  target_compile_options(openidn-core PRIVATE /W0)
else()
  target_compile_options(openidn-core PRIVATE -w)
endif()

find_package(Threads REQUIRED)
target_link_libraries(openidn-core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(openidn-core PUBLIC ws2_32)
endif()

add_library(libera_link_core STATIC
    src/LinkRuntime.cpp
    src/virtual_controller/EtherDreamVirtualControllerHost.cpp
    src/virtual_controller/IdnVirtualControllerHost.cpp
    src/virtual_controller/VirtualControllerHostRegistry.cpp
    src/LiberaPaths.cpp
)
target_include_directories(libera_link_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE
        "${OPENIDN_ROOT}"
)
target_link_libraries(libera_link_core
    PUBLIC
        libera-core
        openidn-core
        Threads::Threads
)

add_executable(libera_link src/main.cpp)
target_link_libraries(libera_link PRIVATE libera_link_core)

option(LIBERA_LINK_BUILD_HARDWARE_TESTS "Build hardware tests (requires Helios USB)" OFF)
add_executable(test_point_stream_framer
    extern/libera-laser/tests/test_point_stream_framer.cpp)
target_link_libraries(test_point_stream_framer PRIVATE libera-core)

add_executable(test_etherdream_virtual_controller_host
    tests/test_etherdream_virtual_controller_host.cpp)
target_link_libraries(test_etherdream_virtual_controller_host PRIVATE libera_link_core)

if(LIBERA_LINK_BUILD_HARDWARE_TESTS)
  add_executable(test_helios_usb_stream
      extern/libera-laser/tests/test_helios_usb_stream.cpp)
  target_link_libraries(test_helios_usb_stream PRIVATE libera-core)
endif()

if(MSVC)
  target_compile_options(libera_link_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(libera_link_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LIBERA_LINK_BUILD_GUI)
  include(FetchContent)

  set(_gui_deps_ready TRUE)
  set(_libera_link_gui_glfw_source_dir "${LIBERA_LINK_GUI_GLFW_SOURCE_DIR}")
  set(_libera_link_gui_imgui_source_dir "${LIBERA_LINK_GUI_IMGUI_SOURCE_DIR}")

  if(NOT EXISTS "${_libera_link_gui_glfw_source_dir}/CMakeLists.txt")
    if(LIBERA_LINK_GUI_GLFW_SOURCE_DIR)
      message(WARNING
          "Configured LIBERA_LINK_GUI_GLFW_SOURCE_DIR='${LIBERA_LINK_GUI_GLFW_SOURCE_DIR}' is invalid; "
          "fetching GLFW ${LIBERA_LINK_GUI_GLFW_GIT_TAG} instead."
      )
    else()
      message(STATUS "Fetching GLFW ${LIBERA_LINK_GUI_GLFW_GIT_TAG} for libera_link_gui")
    endif()
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        ${LIBERA_LINK_GUI_GLFW_GIT_TAG}
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(glfw)
    set(_libera_link_gui_glfw_source_dir "${glfw_SOURCE_DIR}")
  endif()

  if(NOT EXISTS "${_libera_link_gui_imgui_source_dir}/imgui.cpp")
    if(LIBERA_LINK_GUI_IMGUI_SOURCE_DIR)
      message(WARNING
          "Configured LIBERA_LINK_GUI_IMGUI_SOURCE_DIR='${LIBERA_LINK_GUI_IMGUI_SOURCE_DIR}' is invalid; "
          "fetching Dear ImGui ${LIBERA_LINK_GUI_IMGUI_GIT_TAG} instead."
      )
    else()
      message(STATUS "Fetching Dear ImGui ${LIBERA_LINK_GUI_IMGUI_GIT_TAG} for libera_link_gui")
    endif()
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        ${LIBERA_LINK_GUI_IMGUI_GIT_TAG}
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)
    set(_libera_link_gui_imgui_source_dir "${imgui_SOURCE_DIR}")
  endif()

  if(NOT EXISTS "${_libera_link_gui_glfw_source_dir}/CMakeLists.txt")
    message(WARNING "GLFW source tree not found; libera_link_gui target will be skipped.")
    set(_gui_deps_ready FALSE)
  endif()

  if(NOT EXISTS "${_libera_link_gui_imgui_source_dir}/imgui.cpp")
    message(WARNING "ImGui source tree not found; libera_link_gui target will be skipped.")
    set(_gui_deps_ready FALSE)
  endif()

  if(_gui_deps_ready)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    if(NOT TARGET glfw)
      add_subdirectory("${_libera_link_gui_glfw_source_dir}" "${CMAKE_CURRENT_BINARY_DIR}/gui-deps/glfw")
    endif()

    add_library(libera_link_imgui STATIC
        "${_libera_link_gui_imgui_source_dir}/imgui.cpp"
        "${_libera_link_gui_imgui_source_dir}/imgui_draw.cpp"
        "${_libera_link_gui_imgui_source_dir}/imgui_tables.cpp"
        "${_libera_link_gui_imgui_source_dir}/imgui_widgets.cpp"
        "${_libera_link_gui_imgui_source_dir}/imgui_demo.cpp"
        "${_libera_link_gui_imgui_source_dir}/backends/imgui_impl_glfw.cpp"
        "${_libera_link_gui_imgui_source_dir}/backends/imgui_impl_opengl3.cpp"
    )
    target_include_directories(libera_link_imgui PUBLIC
        "${_libera_link_gui_imgui_source_dir}"
        "${_libera_link_gui_imgui_source_dir}/backends"
    )
    target_link_libraries(libera_link_imgui PUBLIC glfw)

    if(APPLE)
      find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
      find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
      target_link_libraries(libera_link_imgui PUBLIC ${OPENGL_FRAMEWORK})
    elseif(WIN32)
      target_link_libraries(libera_link_imgui PUBLIC opengl32)
    else()
      find_package(OpenGL REQUIRED)
      target_link_libraries(libera_link_imgui PUBLIC OpenGL::GL)
    endif()

    add_executable(libera_link_gui
        src/gui_main.cpp
        src/LiberaApp.cpp
        src/LiberaPluginsWindow.cpp
        $<$<PLATFORM_ID:Darwin>:${CMAKE_CURRENT_SOURCE_DIR}/src/LiberaFileDialog_macos.mm>
        $<$<NOT:$<PLATFORM_ID:Darwin>>:${CMAKE_CURRENT_SOURCE_DIR}/src/LiberaFileDialog_other.cpp>
        src/fonts/RobotoMedium.cpp
        src/fonts/RobotoBold.cpp
        src/fonts/ForkAwesome.cpp
    )
    if(APPLE)
      set(LIBERA_LINK_MACOS_BUNDLE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icons/libera-link.icns")
      set_source_files_properties("${LIBERA_LINK_MACOS_BUNDLE_ICON}" PROPERTIES
          MACOSX_PACKAGE_LOCATION "Resources"
      )
      target_sources(libera_link_gui PRIVATE "${LIBERA_LINK_MACOS_BUNDLE_ICON}")
    elseif(WIN32)
      target_sources(libera_link_gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/icons/libera-link.rc")
    endif()
    target_include_directories(libera_link_gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(libera_link_gui PRIVATE APP_VERSION="${LIBERA_LINK_APP_VERSION}")
    target_link_libraries(libera_link_gui PRIVATE libera_link_core libera_link_imgui)

    if(APPLE)
      target_link_libraries(libera_link_gui PRIVATE ${APPKIT_FRAMEWORK})
    endif()

    if(MSVC)
      target_compile_options(libera_link_gui PRIVATE /W4 /permissive-)
    else()
      target_compile_options(libera_link_gui PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(APPLE)
      set_target_properties(libera_link_gui PROPERTIES
          MACOSX_BUNDLE TRUE
          OUTPUT_NAME "${LIBERA_LINK_APP_BUNDLE_NAME}"
          MACOSX_BUNDLE_BUNDLE_NAME "${LIBERA_LINK_APP_BUNDLE_NAME}"
          MACOSX_BUNDLE_GUI_IDENTIFIER "com.sebleedelisle.libera-link"
          MACOSX_BUNDLE_BUNDLE_VERSION "${LIBERA_LINK_APP_VERSION}"
          MACOSX_BUNDLE_SHORT_VERSION_STRING "${LIBERA_LINK_APP_VERSION}"
          MACOSX_BUNDLE_ICON_FILE "libera-link.icns"
          MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
      )
    endif()
  endif()
endif()
