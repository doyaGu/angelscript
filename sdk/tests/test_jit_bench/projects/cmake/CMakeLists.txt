cmake_minimum_required(VERSION 3.10)
project(test_jit_bench)

add_executable(
        test_jit_bench
        ../../source/main.cpp
        ../../../../add_on/scriptarray/scriptarray.cpp
        ../../../../add_on/scriptdictionary/scriptdictionary.cpp
        ../../../../add_on/scriptmath/scriptmath.cpp
        ../../../../add_on/scriptstdstring/scriptstdstring.cpp
        ../../../../add_on/scriptstdstring/scriptstdstring_utils.cpp
        )
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../angelscript/projects/cmake angelscript)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../add_on/jit angelscript_jit)
target_link_libraries(test_jit_bench ${ANGELSCRIPT_LIBRARY_NAME} angelscript_jit)
target_include_directories(test_jit_bench PRIVATE ../../../../angelscript/include)
set_target_properties(test_jit_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/../../bin)
set_target_properties(test_jit_bench PROPERTIES CXX_STANDARD 11)
