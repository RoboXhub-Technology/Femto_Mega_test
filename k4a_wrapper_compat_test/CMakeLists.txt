cmake_minimum_required(VERSION 3.20)

project(femto_mega_k4a_wrapper_test LANGUAGES CXX)

set(K4A_WRAPPER_ROOT "" CACHE PATH
    "Root directory of the prebuilt OrbbecSDK-K4A-Wrapper package")

# Prefer an explicit cache value, then an environment variable, then a few
# convenient local layouts. The final candidate preserves the layout used for
# the reference run on 2026-10-18; it is not required on another computer.
if(NOT K4A_WRAPPER_ROOT)
    set(_K4A_WRAPPER_CANDIDATES
        "$ENV{K4A_WRAPPER_ROOT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../OrbbecSDK-K4A-Wrapper"
        "${CMAKE_CURRENT_SOURCE_DIR}/../OrbbecSDK-K4A-Wrapperp"
        "D:/Femto_mega/OrbbecSDK-K4A-Wrapperp")
    foreach(_candidate IN LISTS _K4A_WRAPPER_CANDIDATES)
        if(_candidate AND EXISTS "${_candidate}/include/k4a/k4a.h")
            set(K4A_WRAPPER_ROOT "${_candidate}" CACHE PATH
                "Root directory of the prebuilt OrbbecSDK-K4A-Wrapper package" FORCE)
            break()
        endif()
    endforeach()
endif()

if(NOT K4A_WRAPPER_ROOT)
    message(FATAL_ERROR
        "K4A_WRAPPER_ROOT is not set. Pass -DK4A_WRAPPER_ROOT=<prebuilt-wrapper-root> "
        "or set the K4A_WRAPPER_ROOT environment variable.")
endif()
get_filename_component(K4A_WRAPPER_ROOT "${K4A_WRAPPER_ROOT}" ABSOLUTE
                       BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(K4A_HEADER "${K4A_WRAPPER_ROOT}/include/k4a/k4a.h")
set(K4A_IMPORT_LIB "${K4A_WRAPPER_ROOT}/lib/k4a.lib")

if(NOT EXISTS "${K4A_HEADER}")
    message(FATAL_ERROR "k4a.h was not found: ${K4A_HEADER}")
endif()
if(NOT EXISTS "${K4A_IMPORT_LIB}")
    message(FATAL_ERROR "k4a.lib was not found: ${K4A_IMPORT_LIB}")
endif()

add_executable(k4a_wrapper_compat_test src/main.cpp)
target_compile_features(k4a_wrapper_compat_test PRIVATE cxx_std_17)
target_include_directories(k4a_wrapper_compat_test PRIVATE
                           "${K4A_WRAPPER_ROOT}/include")
target_link_libraries(k4a_wrapper_compat_test PRIVATE "${K4A_IMPORT_LIB}")

if(MSVC)
    target_compile_options(k4a_wrapper_compat_test PRIVATE /W4 /EHsc /utf-8)
endif()

# The Wrapper release is a self-contained runtime. Copy its matching DLLs and
# resources next to the test executable. Do not substitute the separately
# installed OrbbecSDK DLL because that would mix SDK versions.
set(K4A_RUNTIME_FILES
    k4a.dll
    OrbbecSDK.dll
    depthengine_2_0.dll
    OrbbecSDKConfig.xml
    OrbbecSDKConfig.md)

foreach(runtime_file IN LISTS K4A_RUNTIME_FILES)
    if(EXISTS "${K4A_WRAPPER_ROOT}/bin/${runtime_file}")
        add_custom_command(TARGET k4a_wrapper_compat_test POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${K4A_WRAPPER_ROOT}/bin/${runtime_file}"
                    "$<TARGET_FILE_DIR:k4a_wrapper_compat_test>/${runtime_file}"
            VERBATIM)
    endif()
endforeach()

if(EXISTS "${K4A_WRAPPER_ROOT}/bin/extensions")
    add_custom_command(TARGET k4a_wrapper_compat_test POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${K4A_WRAPPER_ROOT}/bin/extensions"
                "$<TARGET_FILE_DIR:k4a_wrapper_compat_test>/extensions"
        VERBATIM)
endif()

