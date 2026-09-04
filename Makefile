CXX      := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -MMD -MP -I/opt/homebrew/include
LDFLAGS  := -L/opt/homebrew/lib -lraylib \
            -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)
DEP := $(OBJ:.o=.d)
BIN := build/smash

# Headless harness: simulation + search only, so it links without raylib.
# Built as one command, so headers are listed explicitly rather than relying on
# the generated .d file (which a multi-source compile would overwrite per TU).
BENCH_SRC := tools/bench.cpp src/game.cpp src/eval.cpp src/search.cpp
BENCH_HDR := $(wildcard src/*.h)

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

bench: build/bench

build/bench: $(BENCH_SRC) $(BENCH_HDR) | build
	$(CXX) $(CXXFLAGS) -Isrc $(BENCH_SRC) -o $@

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build:
	@mkdir -p build

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build

-include $(DEP)

.PHONY: all run clean bench
