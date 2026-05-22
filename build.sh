#!/bin/bash
# Build helper script for Mastered Engine

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${GREEN}=== Mastered Engine Build ===${NC}\n"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_info() {
    echo -e "${YELLOW}→ $1${NC}"
}

# Check for build directory
if [ ! -d "$BUILD_DIR" ]; then
    print_info "Creating build directory..."
    mkdir -p "$BUILD_DIR"
fi

cd "$BUILD_DIR"

# Run CMake
print_info "Configuring with CMake..."
cmake ..
print_success "CMake configuration complete"

# Build
print_info "Building project..."
make -j$(nproc)
print_success "Build complete"

# Show build artifacts
echo ""
print_header "Build Artifacts:"
ls -lh "$BUILD_DIR/libmastered_engine.a" 2>/dev/null && print_success "Static library created"
ls -lh "$BUILD_DIR/mastered_cli" 2>/dev/null && print_success "CLI executable created"

echo ""
print_info "Build directory: $BUILD_DIR"
print_info "To run tests:"
echo "  cd $PROJECT_ROOT"
echo "  python3 generate_test_audio.py"
echo "  ./build/mastered_cli test_audio/reference.wav test_audio/unmastered.wav result.json"
