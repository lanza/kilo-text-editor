cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_COMPILER $ENV{PROJECTS}/llvm-project/build/Release/bin/clang++)
set(CMAKE_C_COMPILER $ENV{PROJECTS}/llvm-project/build/Release/bin/clang)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)

project(main CXX C Swift OBJC)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_compile_definitions(DBG_MACRO_NO_WARNING)
add_compile_options(-fcolor-diagnostics)

set(LLVM_DIR ~/.llvm/lib/cmake/llvm)
find_package(LLVM REQUIRED)
find_package(ZLIB REQUIRED)
include_directories(${LLVM_INCLUDE_DIRS})

find_package(glog REQUIRED)
set(GFLAGS_USE_TARGET_NAMESPACE ON)
find_package(gflags REQUIRED)

include_directories(include)
add_subdirectory(include)

add_subdirectory(utils)

add_subdirectory(lib)
enable_testing()
add_subdirectory(test)
add_subdirectory(unittests)
add_subdirectory(benchmarks)

function(exec name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(
    ${name}
    PUBLIC
    LLVMSupport
    LLVMCore
    dbg_macro
    glog::glog
    gflags::gflags_shared
    Person
    Utility
  )
  target_compile_options(${name} PUBLIC -fno-rtti)
endfunction()

exec(main)

add_executable(objc objc.m)

find_library(FOUNDATION Foundation)
target_link_libraries(
  objc
  PUBLIC ${FOUNDATION}
)
