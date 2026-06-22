BUILD ?= build

.PHONY: all build bench clean

all: build

build:
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j

bench:
	scripts/run_bench.sh

clean:
	rm -rf $(BUILD) results
