BUILD ?= build
BUILD_ASAN ?= build-asan
BUILD_TSAN ?= build-tsan

.PHONY: all configure build test bench examples asan tsan clean

all: build

configure:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release

build: configure
	cmake --build $(BUILD) -j

test: build
	./$(BUILD)/ferry_tests

bench: build
	./$(BUILD)/ferry_bench 5 256 4 4

examples: build
	@echo "run: ./$(BUILD)/ferry_hello [port] [threads]"
	@echo "run: ./$(BUILD)/ferry_https [cert] [key]"

asan:
	cmake -S . -B $(BUILD_ASAN) -G Ninja -DCMAKE_BUILD_TYPE=Debug \
		-DFERRY_BUILD_EXAMPLES=OFF -DFERRY_BUILD_BENCH=OFF \
		-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1"
	cmake --build $(BUILD_ASAN) -j
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		./$(BUILD_ASAN)/ferry_tests

tsan:
	cmake -S . -B $(BUILD_TSAN) -G Ninja -DCMAKE_BUILD_TYPE=Debug \
		-DFERRY_BUILD_EXAMPLES=OFF -DFERRY_BUILD_BENCH=OFF \
		-DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1 -g"
	cmake --build $(BUILD_TSAN) -j
	TSAN_OPTIONS=halt_on_error=0:exitcode=0 ./$(BUILD_TSAN)/ferry_tests

clean:
	rm -rf $(BUILD) $(BUILD_ASAN) $(BUILD_TSAN)
