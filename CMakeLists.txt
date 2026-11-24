# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(PROJECT_VER "1.0.3")   # TamaNest version (shown in SETTINGS > UPDATE; updates compare it)
# Test builds without editing this file: idf.py -B build-dev -DTAMANEST_VER=0.9.9 ... (see RELEASE.md)
if(DEFINED TAMANEST_VER)
    set(PROJECT_VER "${TAMANEST_VER}")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-Wno-format)
project(tamanest)
