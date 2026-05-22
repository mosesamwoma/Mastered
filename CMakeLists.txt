cmake_minimum_required(VERSION 3.16)
project(MasteredEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native")

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Find required packages
find_package(FFTW3F QUIET)
if(NOT FFTW3F_FOUND)
    message(STATUS "FFTW3 not found - will build with fallback FFT")
endif()

# Source files
set(SOURCES
    src/audio_loader.cpp
    src/fft_analyzer.cpp
    src/eq_calculator.cpp
    src/mastering_engine.cpp
    src/spectrum_matcher.cpp
)

# Create library
add_library(mastered_engine STATIC ${SOURCES})

# Link FFTW if available
if(FFTW3F_FOUND)
    target_link_libraries(mastered_engine PRIVATE FFTW3::FFTW3F)
    target_compile_definitions(mastered_engine PRIVATE USE_FFTW=1)
endif()

# Example executable (optional)
add_executable(mastered_cli src/main.cpp)
target_link_libraries(mastered_cli PRIVATE mastered_engine)

# Installation
install(TARGETS mastered_engine
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
