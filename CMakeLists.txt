cmake_minimum_required(VERSION 3.20)
project(RetracOGFN LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "MSVC requis (Visual Studio 2022).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Compile en x64 : cmake -S . -B build -A x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

add_compile_options(/permissive- /Zc:__cplusplus /MP /W3 /utf-8)

# --- modules optionnels : presents ou pas, on ne casse jamais la config ------
foreach(module common cheat injector)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/${module}/CMakeLists.txt")
        message(STATUS "module : src/${module}")
        add_subdirectory(src/${module})
    else()
        message(STATUS "module : src/${module} (absent, ignore)")
    endif()
endforeach()

# --- ImGui : uniquement utile aux modules internes (src/cheat) --------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/cheat/CMakeLists.txt")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.h")
        set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
        message(STATUS "ImGui : third_party/imgui local")
    else()
        include(FetchContent)
        set(FETCHCONTENT_QUIET OFF)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.90.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(imgui)
        set(IMGUI_DIR "${imgui_SOURCE_DIR}")
        message(STATUS "ImGui : recupere depuis GitHub")
    endif()
else()
    message(STATUS "ImGui : inutile (pas de src/cheat)")
endif()

# --- exe externe : seule cible obligatoire ----------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/external/CMakeLists.txt")
    message(FATAL_ERROR "src/external/CMakeLists.txt absent.")
endif()
add_subdirectory(src/external)
