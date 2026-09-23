# =============================================================================
#  orderbook-alpha  —  build configuration
#
#  Three dependencies, handled three different ways on purpose:
#
#    GLFW          permissive licence, fetched at configure time.
#    Dear ImGui    permissive licence, fetched at configure time (it ships no
#                  CMakeLists of its own, so the sources are listed by hand).
#    IBKR TWS API  Interactive Brokers' own licence. NOT redistributable, so it
#                  is not vendored and not fetched — you download it yourself
#                  and point TWS_API_DIR at it. See README.md § Building.
#
#  If a local ./glfw or ./imgui-master checkout already exists (the layout this
#  project was developed against), that is used instead of fetching, so an
#  existing working tree keeps building offline.
# =============================================================================

cmake_minimum_required(VERSION 3.16)
project(orderbook_alpha CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

include(FetchContent)

# -----------------------------------------------------------------------------
#  1. IBKR TWS API  (required, user-supplied)
# -----------------------------------------------------------------------------
#  The official download unpacks as IBJts/source/cppclient/client. Older/
#  repackaged trees use TWS/cpp/client. Both are accepted.
# -----------------------------------------------------------------------------
set(TWS_API_DIR "" CACHE PATH
    "Path to the TWS API C++ client directory (the one containing EClientSocket.cpp)")

if(NOT TWS_API_DIR)
    foreach(candidate
        "${CMAKE_CURRENT_SOURCE_DIR}/TWS/cpp/client"
        "${CMAKE_CURRENT_SOURCE_DIR}/TWS/source/cppclient/client"
        "${CMAKE_CURRENT_SOURCE_DIR}/IBJts/source/cppclient/client")
        if(EXISTS "${candidate}/EClientSocket.cpp")
            set(TWS_API_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()

if(NOT TWS_API_DIR OR NOT EXISTS "${TWS_API_DIR}/EClientSocket.cpp")
    message(FATAL_ERROR
        "\n"
        "  The IBKR TWS API C++ client was not found.\n"
        "\n"
        "  It is licensed by Interactive Brokers and cannot be redistributed,\n"
        "  so it is not part of this repository. Download the API from\n"
        "  https://interactivebrokers.github.io/ and then either\n"
        "\n"
        "    * unpack it so that ./TWS/cpp/client/EClientSocket.cpp exists, or\n"
        "    * configure with -DTWS_API_DIR=<path to .../cppclient/client>\n"
        "\n")
endif()

message(STATUS "TWS API client: ${TWS_API_DIR}")
file(GLOB TWS_SOURCES CONFIGURE_DEPENDS "${TWS_API_DIR}/*.cpp")

# -----------------------------------------------------------------------------
#  2. GLFW  (window + OpenGL context + input)
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/glfw/CMakeLists.txt")
    message(STATUS "GLFW: using local checkout ./glfw")
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(glfw)
else()
    message(STATUS "GLFW: fetching 3.4")
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(glfw)
endif()

# -----------------------------------------------------------------------------
#  3. Dear ImGui  (immediate-mode UI — no retained widget tree to keep in sync
#     with a book that changes thousands of times a second)
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/imgui-master/imgui.cpp")
    message(STATUS "Dear ImGui: using local checkout ./imgui-master")
    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui-master")
else()
    message(STATUS "Dear ImGui: fetching v1.91.5")
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
endif()

set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp")

# -----------------------------------------------------------------------------
#  4. Target
# -----------------------------------------------------------------------------
add_executable(mm_alpha
    mm_alpha.cpp
    ib_wrapper.cpp
    ${IMGUI_SOURCES}
    ${TWS_SOURCES})

target_include_directories(mm_alpha PRIVATE
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${TWS_API_DIR}")

find_package(OpenGL REQUIRED)
target_link_libraries(mm_alpha PRIVATE glfw OpenGL::GL)

if(WIN32)
    # IB's socket layer is raw Winsock.
    target_link_libraries(mm_alpha PRIVATE ws2_32)
else()
    target_compile_definitions(mm_alpha PRIVATE IB_POSIX)
endif()
