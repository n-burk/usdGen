# PW-1 / E-4 probes. Reproduce with: make run
# Release-equivalent flags (build/CMakeCache.txt: CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG).
NANOFLANN := <usdgen-src>/thirdparty/nanoflann
CXX       ?= g++
CXXFLAGS  := -std=c++17 -O3 -DNDEBUG -w
LDLIBS    := -lpthread

all: compile_probe bench_knn

compile_probe: compile_probe.cpp
	$(CXX) $(CXXFLAGS) -I $(NANOFLANN) $< -o $@ $(LDLIBS)

bench_knn: bench_knn.cpp
	$(CXX) $(CXXFLAGS) -I $(NANOFLANN) $< -o $@ $(LDLIBS)

run: all
	./compile_probe
	./bench_knn 11

clean:
	rm -f compile_probe bench_knn
