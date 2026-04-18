cmake_minimum_required(VERSION 3.18)

project(IDNBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(IDN_BRIDGE_APP_VERSION "0.0.0")

option(IDN_BRIDGE_USE_BUNDLED_LIBUSB "Use bundled libusb from libera-laser dependency tree" ON)
option(IDN_BRIDGE_OPENIDN_BRIDGE_MODE "Enable bridge-specific OpenIDN behavior changes" ON)
option(IDN_BRIDGE_BUILD_GUI "Build the native GUI target when ImGui/GLFW sources are available" ON)

set(IDN_BRIDGE_GUI_GLFW_SOURCE_DIR "" CACHE PATH "Path to a GLFW source tree for the GUI target")
set(IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR "" CACHE PATH "Path to an ImGui source tree for the GUI target")

set(LIBERA_USE_BUNDLED_LIBUSB ${IDN_BRIDGE_USE_BUNDLED_LIBUSB} CACHE BOOL "" FORCE)
set(LIBERA_BUNDLED_LIBUSB_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/libera-laser/libs/helios_dac/sdk/cpp"
    CACHE PATH "" FORCE)
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

if(NOT IDN_BRIDGE_OPENIDN_BRIDGE_MODE)
  message(FATAL_ERROR
      "IDN_BRIDGE_OPENIDN_BRIDGE_MODE=OFF is no longer supported by the vendored "
      "third_party/openidn subset."
  )
endif()

if(NOT IDN_BRIDGE_GUI_GLFW_SOURCE_DIR)
  set(_fallback_glfw "${CMAKE_CURRENT_SOURCE_DIR}/../libera-lab/build/_deps/glfw-src")
  if(EXISTS "${_fallback_glfw}/CMakeLists.txt")
    set(IDN_BRIDGE_GUI_GLFW_SOURCE_DIR "${_fallback_glfw}")
  endif()
endif()

if(NOT IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR)
  set(_fallback_imgui "${CMAKE_CURRENT_SOURCE_DIR}/../libera-lab/build/_deps/imgui-src")
  if(EXISTS "${_fallback_imgui}/imgui.cpp")
    set(IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR "${_fallback_imgui}")
  endif()
endif()

add_library(openidn-core STATIC ${OPENIDN_SOURCES})
target_compile_definitions(openidn-core PUBLIC ODF_USE_TAXI_SOCK)
if(IDN_BRIDGE_OPENIDN_BRIDGE_MODE)
  target_compile_definitions(openidn-core PUBLIC OPENIDN_BRIDGE_MODE=1)
endif()
target_include_directories(openidn-core PUBLIC "${OPENIDN_ROOT}")
if(MSVC)
  target_compile_options(openidn-core PRIVATE /W0)
else()
  target_compile_options(openidn-core PRIVATE -w)
endif()

find_package(Threads REQUIRED)
target_link_libraries(openidn-core PUBLIC Threads::Threads)

add_library(idn_bridge_core STATIC
    src/BridgeRuntime.cpp
    src/LiberaPaths.cpp
)
target_include_directories(idn_bridge_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE
        "${OPENIDN_ROOT}"
)
target_link_libraries(idn_bridge_core
    PUBLIC
        libera-core
        openidn-core
        Threads::Threads
)

add_executable(idn_bridge src/main.cpp)
target_link_libraries(idn_bridge PRIVATE idn_bridge_core)

if(MSVC)
  target_compile_options(idn_bridge_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(idn_bridge_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IDN_BRIDGE_BUILD_GUI)
  set(_gui_deps_ready TRUE)

  if(NOT EXISTS "${IDN_BRIDGE_GUI_GLFW_SOURCE_DIR}/CMakeLists.txt")
    message(WARNING "GLFW source tree not found; idn_bridge_gui target will be skipped.")
    set(_gui_deps_ready FALSE)
  endif()

  if(NOT EXISTS "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}/imgui.cpp")
    message(WARNING "ImGui source tree not found; idn_bridge_gui target will be skipped.")
    set(_gui_deps_ready FALSE)
  endif()

  if(_gui_deps_ready)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${IDN_BRIDGE_GUI_GLFW_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/gui-deps/glfw")

    add_library(idn_bridge_imgui STATIC
        "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}/imgui.cpp"
        "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
        "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
        "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
        "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
        "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
        "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
    )
    target_include_directories(idn_bridge_imgui PUBLIC
        "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}"
        "${IDN_BRIDGE_GUI_IMGUI_SOURCE_DIR}/backends"
    )
    target_link_libraries(idn_bridge_imgui PUBLIC glfw)

    if(APPLE)
      find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
      find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
      target_link_libraries(idn_bridge_imgui PUBLIC ${OPENGL_FRAMEWORK})
    elseif(WIN32)
      target_link_libraries(idn_bridge_imgui PUBLIC opengl32)
    else()
      find_package(OpenGL REQUIRED)
      target_link_libraries(idn_bridge_imgui PUBLIC OpenGL::GL)
    endif()

    add_executable(idn_bridge_gui
        src/gui_main.cpp
        src/LiberaApp.cpp
        src/LiberaPluginsWindow.cpp
        $<$<PLATFORM_ID:Darwin>:${CMAKE_CURRENT_SOURCE_DIR}/src/LiberaFileDialog_macos.mm>
        $<$<NOT:$<PLATFORM_ID:Darwin>>:${CMAKE_CURRENT_SOURCE_DIR}/src/LiberaFileDialog_other.cpp>
        src/fonts/RobotoMedium.cpp
        src/fonts/RobotoBold.cpp
        src/fonts/ForkAwesome.cpp
    )
    target_include_directories(idn_bridge_gui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(idn_bridge_gui PRIVATE APP_VERSION="${IDN_BRIDGE_APP_VERSION}")
    target_link_libraries(idn_bridge_gui PRIVATE idn_bridge_core idn_bridge_imgui)

    if(APPLE)
      target_link_libraries(idn_bridge_gui PRIVATE ${APPKIT_FRAMEWORK})
    endif()

    if(MSVC)
      target_compile_options(idn_bridge_gui PRIVATE /W4 /permissive-)
    else()
      target_compile_options(idn_bridge_gui PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(APPLE)
      set_target_properties(idn_bridge_gui PROPERTIES
          MACOSX_BUNDLE TRUE
          MACOSX_BUNDLE_BUNDLE_NAME "Libera Portal"
          MACOSX_BUNDLE_GUI_IDENTIFIER "com.sebleedelisle.idn-bridge"
          MACOSX_BUNDLE_BUNDLE_VERSION "${IDN_BRIDGE_APP_VERSION}"
          MACOSX_BUNDLE_SHORT_VERSION_STRING "${IDN_BRIDGE_APP_VERSION}"
          MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
      )
    endif()
  endif()
endif()
