# LLSCT
## Building
LLSCT is only supported for Linux (but may run on Intel-based Macs with some tweaks).

Requires gcc-12 (for C++20 features).

### Dependencies
LLSCT currently requires the following dependencies:
- gperftools
- libunwind
- CMake (version >= 3.25)
- Ninja
- Python3
- Python packages: pandas, seaborn
You can install all of these using [Homebrew ](https://brew.sh).

### Installing Dependencies
```sh
brew install gperftools libunwind cmake ninja python3
pip3 install pandas seaborn
```

### Building llsct-llvm
To build LLSCT, you will need to clone two repositories: [llsct-llvm](https://github.com/nmosier/clouxx-llvm) and llsct-passes (this repository).
First, clone and build __llsct-llvm__:
```sh
git clone https://github.com/nmosier/clouxx-llvm --depth=1 llsct-llvm
mkdir llsct-llvm/build && cd llsct-llvm/build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../install -DLLVM_ENABLE_ASSERTIONS=On -DLLVM_ENABLE_PROJECTS='clang;lld' -DLLVM_TARGETS_TO_BUILD='X86' ../llvm
ninja
ninja install
cd ../..
```

### Building llsct-passes
Now, clone and configure __llsct-passes__:
```sh
git clone https://github.com/nmosier/clouxx-passes llsct-passes
mkdir llsct-passes/build && cd llsct-passes/build
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLLSCT_LLVM_DIR=../llsct-llvm/install ..
ninja src/all
```
The last command builds all of LLSCT's IR passes.

To build all the benchmark programs so that you can run them as standaloen programs, build the `raw_compile` target:
```sh
ninja raw_compile
```
All the test binaries will have filenames matching `bench-new/raw_<project>_<name>_<size>_<mitigation>`.

To run the benchmark to obtain a graph of runtime overhead:
```sh
ninja time_compile clean_bench && ninja -j1 time_pdf
```
and a PDF of the overhead plot will be written to `bench-new/time.pdf`.
