cmake_minimum_required(VERSION 3.16)
project(MasteredEngine VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -Wall -Wextra -Wpedantic")

# Debug vs Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Find optional packages
find_package(FFTW3F QUIET)
if(FFTW3F_FOUND)
    message(STATUS "✓ FFTW3 found - using optimized FFT")
else()
    message(STATUS "⚠ FFTW3 not found - using fallback FFT (slower)")
endif()

# Source files
set(SOURCES
    src/audio_loader.cpp
    src/fft_analyzer.cpp
    src/eq_calculator.cpp
    src/mastering_engine.cpp
    src/spectrum_matcher.cpp
)

# Create static library
add_library(mastered_engine STATIC ${SOURCES})
set_target_properties(mastered_engine PROPERTIES VERSION 1.1.0)

# Link FFTW if available
if(FFTW3F_FOUND)
    target_link_libraries(mastered_engine PRIVATE FFTW3::FFTW3F)
    target_compile_definitions(mastered_engine PRIVATE USE_FFTW=1)
endif()

# Link math library
target_link_libraries(mastered_engine PRIVATE m)

# Enable strict warnings
if(MSVC)
    target_compile_options(mastered_engine PRIVATE /W4)
else()
    target_compile_options(mastered_engine PRIVATE -Wall -Wextra -Wpedantic -Werror=unused-result)
endif()

# Create CLI executable
add_executable(mastered_cli src/main.cpp)
target_link_libraries(mastered_cli PRIVATE mastered_engine)
set_target_properties(mastered_cli PROPERTIES VERSION 1.1.0)

# Installation targets
install(TARGETS mastered_engine
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
install(TARGETS mastered_cli
        RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include/mastered)

# Summary
message(STATUS "")
message(STATUS "=== Mastered Engine ${CMAKE_PROJECT_VERSION} ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: 17")
message(STATUS "FFTW3: ${FFTW3F_FOUND}")
message(STATUS "")
