cmake_minimum_required(VERSION 3.20)
project(MasteredEngine VERSION 2.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(CheckCXXCompilerFlag)
include(CheckPIESupported)
check_pie_supported(LANGUAGES CXX)

option(MASTERED_NATIVE "Optimise for the build machine" OFF)
option(MASTERED_SANITIZE "Build with address and undefined behaviour sanitizers" OFF)
option(MASTERED_BUILD_SHARED "Build the C API shared library" ON)

set(MASTERED_SOURCES
    src/biquad.cpp
    src/chain.cpp
    src/dynamics.cpp
    src/eq_fitter.cpp
    src/fft.cpp
    src/limiter.cpp
    src/linear_phase.cpp
    src/loudness.cpp
    src/mastering_engine.cpp
    src/presets.cpp
    src/spectrum.cpp
    src/wav_io.cpp
    src/waveform.cpp
)

add_library(mastered_engine STATIC ${MASTERED_SOURCES})
target_include_directories(mastered_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/mastered>
)
target_compile_definitions(mastered_engine PUBLIC MASTERED_VERSION="${PROJECT_VERSION}")
set_target_properties(mastered_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_library(MASTERED_MATH_LIBRARY m)
if(MASTERED_MATH_LIBRARY)
    target_link_libraries(mastered_engine PUBLIC ${MASTERED_MATH_LIBRARY})
endif()

add_executable(mastered_cli src/main.cpp)
set_target_properties(mastered_cli PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(mastered_cli PRIVATE mastered_engine)

set(MASTERED_TARGETS mastered_engine mastered_cli)

if(MASTERED_BUILD_SHARED)
    add_library(mastered SHARED src/c_api.cpp)
    target_link_libraries(mastered PRIVATE mastered_engine)
    target_compile_definitions(mastered PRIVATE MASTERED_BUILDING)
    list(APPEND MASTERED_TARGETS mastered)
endif()

foreach(target ${MASTERED_TARGETS})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(${target} PRIVATE -fstack-protector-strong -fno-strict-overflow)
        if(CMAKE_BUILD_TYPE STREQUAL "Release")
            target_compile_definitions(${target} PRIVATE _FORTIFY_SOURCE=2 _GLIBCXX_ASSERTIONS)
        endif()
        check_cxx_compiler_flag(-fcf-protection=full MASTERED_HAS_CF_PROTECTION)
        if(MASTERED_HAS_CF_PROTECTION)
            target_compile_options(${target} PRIVATE -fcf-protection=full)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_options(${target} PRIVATE -Wl,-z,relro,-z,now,-z,noexecstack)
        endif()
        if(MASTERED_NATIVE)
            target_compile_options(${target} PRIVATE -march=native)
        endif()
        if(MASTERED_SANITIZE)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endforeach()

install(TARGETS mastered_cli RUNTIME DESTINATION bin)
if(MASTERED_BUILD_SHARED)
    install(TARGETS mastered LIBRARY DESTINATION lib RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
    install(FILES include/mastered_c.h DESTINATION include/mastered)
endif()
