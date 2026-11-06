cmake_minimum_required(VERSION 3.20)
project(ALEXZSilentFilmOFX VERSION 0.4.0 LANGUAGES CXX)

option(SILENTFILM_ENABLE_CUDA "Собрать CUDA backend и тест сравнения CPU/CUDA" OFF)

set(OFX_SDK_ROOT "/opt/resolve/Developer/OpenFX" CACHE PATH "Корень установленного OpenFX SDK")
find_path(OFX_INCLUDE_DIR ofxImageEffect.h
    HINTS "${OFX_SDK_ROOT}/OpenFX-1.4/include" "${OFX_SDK_ROOT}/include"
    DOC "Каталог заголовков OpenFX")
if(NOT OFX_INCLUDE_DIR)
    message(FATAL_ERROR "Не найдены заголовки OpenFX. Задайте OFX_SDK_ROOT или OFX_INCLUDE_DIR.")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(OFX_BINARY_DIRECTORY "Linux-x86-64")
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(OFX_BINARY_DIRECTORY "Win64")
elseif(APPLE)
    set(OFX_BINARY_DIRECTORY "MacOS")
else()
    message(FATAL_ERROR "Для этой платформы ещё не определён каталог OpenFX bundle.")
endif()

add_library(silentfilm_core STATIC src/core/probe.cpp src/core/optics.cpp)
target_include_directories(silentfilm_core PUBLIC include)
target_compile_features(silentfilm_core PUBLIC cxx_std_17)
set_target_properties(silentfilm_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

add_library(ALEXZSilentFilm MODULE src/ofx/plugin.cpp)
target_include_directories(ALEXZSilentFilm PRIVATE "${OFX_INCLUDE_DIR}")
target_link_libraries(ALEXZSilentFilm PRIVATE silentfilm_core)
target_compile_features(ALEXZSilentFilm PRIVATE cxx_std_17)

if(SILENTFILM_ENABLE_CUDA)
    if(APPLE)
        message(FATAL_ERROR "На macOS планируется Metal; CUDA backend здесь не поддерживается.")
    endif()
    # Можно переопределить для текущей GPU, например -DCMAKE_CUDA_ARCHITECTURES=86.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES "75;86;89;90" CACHE STRING "Целевые CUDA architectures")
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_library(silentfilm_cuda STATIC src/cuda/probe.cu)
    target_include_directories(silentfilm_cuda PUBLIC include)
    target_link_libraries(silentfilm_cuda PUBLIC CUDA::cudart_static)
    set_target_properties(silentfilm_cuda PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
        CUDA_RUNTIME_LIBRARY Static)
    if(NOT MSVC)
        target_compile_options(silentfilm_cuda PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fvisibility=hidden>)
    endif()
    target_compile_definitions(ALEXZSilentFilm PRIVATE SILENTFILM_ENABLE_CUDA=1)
    target_link_libraries(ALEXZSilentFilm PRIVATE silentfilm_cuda)
endif()

set(OFX_BUNDLE "${CMAKE_BINARY_DIR}/ofx/ALEXZSilentFilm.ofx.bundle")
set_target_properties(ALEXZSilentFilm PROPERTIES
    PREFIX "" SUFFIX ".ofx"
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${OFX_BUNDLE}/Contents/${OFX_BINARY_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${OFX_BUNDLE}/Contents/${OFX_BINARY_DIRECTORY}")
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(ALEXZSilentFilm PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${configuration} "${OFX_BUNDLE}/Contents/${OFX_BINARY_DIRECTORY}"
        RUNTIME_OUTPUT_DIRECTORY_${configuration} "${OFX_BUNDLE}/Contents/${OFX_BINARY_DIRECTORY}")
endforeach()

foreach(target silentfilm_core ALEXZSilentFilm)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(silentfilm_core_tests tests/core_tests.cpp)
    target_link_libraries(silentfilm_core_tests PRIVATE silentfilm_core)
    add_test(NAME silentfilm_core COMMAND silentfilm_core_tests)
    add_executable(silentfilm_sensitometry_tests tests/sensitometry_tests.cpp)
    target_link_libraries(silentfilm_sensitometry_tests PRIVATE silentfilm_core)
    add_test(NAME silentfilm_sensitometry COMMAND silentfilm_sensitometry_tests)
    if(SILENTFILM_ENABLE_CUDA)
        add_executable(silentfilm_cuda_tests tests/cuda_tests.cpp)
        target_link_libraries(silentfilm_cuda_tests PRIVATE silentfilm_core silentfilm_cuda)
        add_test(NAME silentfilm_cuda_parity COMMAND silentfilm_cuda_tests)
        set_tests_properties(silentfilm_cuda_parity PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# Установка выполняется только отдельной пользовательской командой.
install(TARGETS ALEXZSilentFilm
    LIBRARY DESTINATION "ALEXZSilentFilm.ofx.bundle/Contents/${OFX_BINARY_DIRECTORY}"
    RUNTIME DESTINATION "ALEXZSilentFilm.ofx.bundle/Contents/${OFX_BINARY_DIRECTORY}")
