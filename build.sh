#!/bin/bash
# Build helper script for Mastered Engine
# Compiles C++17 mastering engine with CMake

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${BLUE}╔══════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║${NC}  ${GREEN}Mastered Engine - Professional Audio Mastering${NC}  ${BLUE}║${NC}"
    echo -e "${BLUE}║${NC}  Build System (CMake + C++17)                  ${BLUE}║${NC}"
    echo -e "${BLUE}╚══════════════════════════════════════════════════════════════╝${NC}\n"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
    exit 1
}

print_info() {
    echo -e "${YELLOW}→${NC} $1"
}

# Main build
main() {
    print_header
    
    # Step 1: Check prerequisites
    print_info "Checking prerequisites..."
    command -v cmake >/dev/null 2>&1 || print_error "CMake not found. Install with: apt install cmake"
    command -v make >/dev/null 2>&1 || print_error "Make not found. Install with: apt install build-essential"
    print_success "Prerequisites found (CMake, Make, C++17)"
    
    # Step 2: Create build directory
    echo ""
    print_info "Setting up build directory..."
    if [ ! -d "$BUILD_DIR" ]; then
        mkdir -p "$BUILD_DIR"
        print_success "Created: $BUILD_DIR"
    else
        print_success "Build directory exists"
    fi
    
    # Step 3: CMake configuration
    echo ""
    print_info "Configuring with CMake..."
    cd "$BUILD_DIR"
    cmake .. -DCMAKE_BUILD_TYPE=Release
    print_success "CMake configuration complete"
    
    # Step 4: Compile
    echo ""
    print_info "Compiling project (using $(nproc) cores)..."
    make -j$(nproc)
    print_success "Build complete"
    
    # Step 5: Verify artifacts
    echo ""
    print_info "Verifying build artifacts..."
    
    if [ -f "$BUILD_DIR/libmastered_engine.a" ]; then
        LIB_SIZE=$(du -h "$BUILD_DIR/libmastered_engine.a" | cut -f1)
        print_success "Static library: libmastered_engine.a ($LIB_SIZE)"
    else
        print_error "Static library not found!"
    fi
    
    if [ -f "$BUILD_DIR/mastered_cli" ]; then
        CLI_SIZE=$(du -h "$BUILD_DIR/mastered_cli" | cut -f1)
        print_success "CLI executable: mastered_cli ($CLI_SIZE)"
    else
        print_error "CLI executable not found!"
    fi
    
    # Step 6: Show usage
    echo ""
    print_info "Build directory: $BUILD_DIR"
    echo ""
    echo "Quick Start:"
    echo "  1. Generate test audio:"
    echo "     python3 generate_test_audio.py"
    echo ""
    echo "  2. Run analysis:"
    echo "     ./build/mastered_cli test_audio/reference.wav test_audio/unmastered.wav result.json"
    echo ""
    echo "  3. View results:"
    echo "     cat result.json"
    echo ""
    echo "Documentation:"
    echo "  • QUICKSTART.md    - 5-minute setup"
    echo "  • README.md        - Full documentation"
    echo "  • API_REFERENCE.md - Complete API"
    echo ""
    print_success "Build successful! Ready for integration."
}

# Run build
main
