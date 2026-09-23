cmake_minimum_required(VERSION 3.10)
project(test_performance)

add_executable(
        test_performance
        ../../source/main.cpp
        ../../source/scriptstring.cpp
        ../../source/test_array.cpp
        ../../source/test_assign.cpp
        ../../source/test_basic.cpp
        ../../source/test_basic2.cpp
        ../../source/test_call.cpp
        ../../source/test_call2.cpp
        ../../source/test_classprop.cpp
        ../../source/test_fib.cpp
        ../../source/test_globalvar.cpp
        ../../source/test_int.cpp
        ../../source/test_intf.cpp
        ../../source/test_mthd.cpp
        ../../source/test_retobj.cpp
        ../../source/test_string.cpp
        ../../source/test_string2.cpp
        ../../source/test_string_pooled.cpp
        ../../source/test_thisprop.cpp
        ../../source/test_vector3.cpp
        ../../source/utils.cpp
        ../../../../add_on/scriptarray/scriptarray.cpp
        ../../../../add_on/scriptstdstring/scriptstdstring.cpp
        ../../../../add_on/scriptstdstring/scriptstdstring_utils.cpp
        )
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../angelscript/projects/cmake angelscript)
target_link_libraries(test_performance ${ANGELSCRIPT_LIBRARY_NAME})
target_include_directories(test_performance PRIVATE ../../../../angelscript/include)
set_target_properties(test_performance PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/../../bin)
set_target_properties(test_performance PROPERTIES CXX_STANDARD 11)
if(WIN32)
    target_link_libraries(test_performance winmm)
endif()
