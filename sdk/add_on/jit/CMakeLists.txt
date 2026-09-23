cmake_minimum_required(VERSION 3.24)
project(angelscript_jit CXX)

# The JIT compiler generates machine code with AsmJit (zlib license), which is
# fetched at a pinned revision unless a local copy is pointed to with AS_JIT_ASMJIT_DIR.
set(AS_JIT_ASMJIT_DIR "" CACHE PATH "Path to a local AsmJit source tree (leave empty to download it)")
set(AS_JIT_ASMJIT_REPOSITORY "https://github.com/asmjit/asmjit.git" CACHE STRING "AsmJit git repository")
set(AS_JIT_ASMJIT_TAG "dffd8b164f228abbc47246c9881f107d656952b2" CACHE STRING "AsmJit git revision")
option(AS_JIT_LOGGING "Include support for logging the generated code (increases binary size)" ON)

if(NOT TARGET asmjit::asmjit)
    set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
    set(ASMJIT_TEST OFF CACHE BOOL "" FORCE)
    set(ASMJIT_NO_FOREIGN ON CACHE BOOL "" FORCE)
    set(ASMJIT_NO_NATVIS ON CACHE BOOL "" FORCE)
    if(NOT AS_JIT_LOGGING)
        set(ASMJIT_NO_TEXT ON CACHE BOOL "" FORCE)
        set(ASMJIT_NO_LOGGING ON CACHE BOOL "" FORCE)
    endif()
    if(AS_JIT_ASMJIT_DIR)
        add_subdirectory(${AS_JIT_ASMJIT_DIR} asmjit EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(asmjit
            GIT_REPOSITORY ${AS_JIT_ASMJIT_REPOSITORY}
            GIT_TAG        ${AS_JIT_ASMJIT_TAG}
            GIT_SHALLOW    OFF
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(asmjit)
    endif()
endif()

# The engine library must already be part of the build (see the CMake project
# in sdk/angelscript/projects/cmake). The JIT compiler needs the internal
# headers of the engine, so it must be compiled with the same options.
if(NOT ANGELSCRIPT_LIBRARY_NAME)
    set(ANGELSCRIPT_LIBRARY_NAME angelscript)
endif()

add_library(angelscript_jit STATIC
    jit.h
    jit.cpp
    jit_bytecode.h
    jit_bytecode.cpp
    jit_codegen.h
    jit_codegen.cpp
    jit_codegen_math.cpp
    jit_codegen_call.cpp
    jit_codegen_arch.cpp
    jit_runtime.h
    jit_runtime.cpp
    jit_unwind.h
    jit_unwind.cpp
)
target_compile_features(angelscript_jit PUBLIC cxx_std_20)
target_include_directories(angelscript_jit
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/../../angelscript/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../angelscript/source)
target_link_libraries(angelscript_jit PUBLIC ${ANGELSCRIPT_LIBRARY_NAME} PRIVATE asmjit::asmjit)
if(AS_NO_EXCEPTIONS)
    target_compile_definitions(angelscript_jit PRIVATE AS_NO_EXCEPTIONS)
endif()
if(NOT AS_JIT_LOGGING)
    target_compile_definitions(angelscript_jit PRIVATE AS_JIT_NO_LOGGING)
endif()
if(MSVC)
    target_compile_definitions(angelscript_jit PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
