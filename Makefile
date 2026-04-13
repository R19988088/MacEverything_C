CXX = clang++
CXXFLAGS = -std=c++20 -O2
FRAMEWORKS = -framework CoreServices
CORE_SRCS = $(wildcard MacEverything/Core/*.cpp)

# === Build targets ===
.PHONY: test test-fast test-slow test-all build clean app dmg help

test_all: test_all.cpp $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(FRAMEWORKS) $^ -o $@

benchmark: benchmark.cpp $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(FRAMEWORKS) $^ -o $@

# === Test targets ===
test: test-fast

test-fast: test_all
	./test_all --fast

test-slow: test_all
	./test_all --slow

test-all: test_all
	./test_all

# === Xcode build ===
app:
	DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild \
		-project MacEverything.xcodeproj -scheme MacEverything \
		-configuration Release build SYMROOT=build

# === Package ===
dmg: app
	-hdiutil detach /Volumes/MacEverything 2>/dev/null
	hdiutil create -volname MacEverything \
		-srcfolder build/Release/MacEverything.app \
		-ov -format UDZO MacEverything.dmg

# === Cleanup ===
clean:
	rm -f test_all benchmark
	rm -rf build/

# === Help ===
help:
	@echo "Available targets:"
	@echo "  make test       - Run fast unit tests (alias for test-fast)"
	@echo "  make test-fast  - Run fast unit tests (Part 3, 3b, 3c, 3d, 3e, 5)"
	@echo "  make test-slow  - Run slow integration tests (Part 1, 4, 6)"
	@echo "  make test-all   - Run all tests"
	@echo "  make app        - Build MacEverything.app via Xcode"
	@echo "  make dmg        - Build + package into DMG"
	@echo "  make benchmark  - Build benchmark binary"
	@echo "  make clean      - Remove build artifacts"
