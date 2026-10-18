cmake_minimum_required(VERSION 3.15)
project(FemtoMegaAcceptanceTools VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ORBBEC_SDK_ROOT "" CACHE PATH "Root of an unpacked Orbbec SDK v2 package")
if(ORBBEC_SDK_ROOT AND NOT OrbbecSDK_DIR)
    set(OrbbecSDK_DIR "${ORBBEC_SDK_ROOT}/lib" CACHE PATH "OrbbecSDK package directory" FORCE)
endif()

find_package(OrbbecSDK REQUIRED CONFIG)

add_executable(femto_mega_acceptance src/femto_mega_acceptance.cpp)

if(TARGET ob::OrbbecSDK)
    target_link_libraries(femto_mega_acceptance PRIVATE ob::OrbbecSDK)
elseif(TARGET OrbbecSDK::OrbbecSDK)
    target_link_libraries(femto_mega_acceptance PRIVATE OrbbecSDK::OrbbecSDK)
else()
    message(FATAL_ERROR "OrbbecSDK was found, but neither ob::OrbbecSDK nor OrbbecSDK::OrbbecSDK is exported")
endif()

if(MSVC)
    target_compile_options(femto_mega_acceptance PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(femto_mega_acceptance PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(BUILD_K4A_WRAPPER_SMOKE "Build the optional Orbbec K4A Wrapper smoke-test executable" OFF)
set(K4A_WRAPPER_ROOT "" CACHE PATH "Root of an installed/unpacked OrbbecSDK-K4A-Wrapper")

if(BUILD_K4A_WRAPPER_SMOKE)
    find_path(K4A_INCLUDE_DIR k4a/k4a.h
        HINTS "${K4A_WRAPPER_ROOT}" "${K4A_WRAPPER_ROOT}/include"
        PATH_SUFFIXES include)
    find_library(K4A_LIBRARY NAMES k4a
        HINTS "${K4A_WRAPPER_ROOT}" "${K4A_WRAPPER_ROOT}/lib"
        PATH_SUFFIXES lib lib64)
    if(NOT K4A_INCLUDE_DIR OR NOT K4A_LIBRARY)
        message(FATAL_ERROR "K4A wrapper headers/library not found. Set K4A_WRAPPER_ROOT.")
    endif()
    add_executable(k4a_wrapper_smoke src/k4a_wrapper_smoke.cpp)
    target_include_directories(k4a_wrapper_smoke PRIVATE "${K4A_INCLUDE_DIR}")
    target_link_libraries(k4a_wrapper_smoke PRIVATE "${K4A_LIBRARY}")
    if(MSVC)
        target_compile_options(k4a_wrapper_smoke PRIVATE /W4 /utf-8)
    else()
        target_compile_options(k4a_wrapper_smoke PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

install(TARGETS femto_mega_acceptance RUNTIME DESTINATION bin)
if(TARGET k4a_wrapper_smoke)
    install(TARGETS k4a_wrapper_smoke RUNTIME DESTINATION bin)
endif()
