cmake_minimum_required(VERSION 3.20)
project(ALEXZSilentFilmOFX VERSION 0.10.0 LANGUAGES CXX)

option(SILENTFILM_ENABLE_CUDA "Собрать CUDA backend и тест сравнения CPU/CUDA" OFF)
option(SILENTFILM_BUILD_CADENCE "Собрать нативный пересчёт каденса и Lua-команду Resolve (Linux)" ON)

set(OFX_SDK_ROOT "/opt/resolve/Developer/OpenFX" CACHE PATH "Корень установленного OpenFX SDK")
find_path(OFX_INCLUDE_DIR ofxImageEffect.h
    HINTS "${OFX_SDK_ROOT}/OpenFX-1.4/include" "${OFX_SDK_ROOT}/include"
    DOC "Каталог заголовков OpenFX")
if(NOT OFX_INCLUDE_DIR)
    message(FATAL_ERROR "Не найдены заголовки OpenFX. Задайте OFX_SDK_ROOT или OFX_INCLUDE_DIR.")
endif()

find_path(FFTW3_INCLUDE_DIR fftw3.h DOC "Каталог заголовков FFTW3")
find_library(FFTW3F_LIBRARY NAMES fftw3f DOC "Библиотека FFTW3 single precision")
if(NOT FFTW3_INCLUDE_DIR OR NOT FFTW3F_LIBRARY)
    message(FATAL_ERROR "Не найдена FFTW3 single precision (fftw3.h и libfftw3f). Установите пакет разработки FFTW3 или задайте FFTW3_INCLUDE_DIR и FFTW3F_LIBRARY.")
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

add_library(silentfilm_core STATIC src/core/probe.cpp src/core/optics.cpp src/core/film_optics.cpp src/core/camera_capture.cpp src/core/projector.cpp src/core/cadence.cpp)
target_include_directories(silentfilm_core PUBLIC include PRIVATE "${FFTW3_INCLUDE_DIR}")
target_link_libraries(silentfilm_core PUBLIC "${FFTW3F_LIBRARY}")
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
    find_package(CUDAToolkit 11.2 REQUIRED)
    find_package(Threads REQUIRED)
    add_library(silentfilm_cuda STATIC src/cuda/probe.cu src/cuda/camera.cu src/cuda/projector.cu)
    target_include_directories(silentfilm_cuda PUBLIC include)
    target_link_libraries(silentfilm_cuda PUBLIC CUDA::cufft_static)
    if(TARGET CUDA::nvJitLink_static)
        target_link_libraries(silentfilm_cuda PUBLIC CUDA::nvJitLink_static)
    endif()
    target_link_libraries(silentfilm_cuda PUBLIC CUDA::culibos CUDA::cudart_static CUDA::cuda_driver Threads::Threads ${CMAKE_DL_LIBS})
    set_target_properties(silentfilm_cuda PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RESOLVE_DEVICE_SYMBOLS OFF
        CUDA_RUNTIME_LIBRARY Static)
    if(NOT MSVC)
        target_compile_options(silentfilm_cuda PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fvisibility=hidden>)
    endif()
    target_compile_definitions(ALEXZSilentFilm PRIVATE SILENTFILM_ENABLE_CUDA=1)
    target_link_libraries(ALEXZSilentFilm PRIVATE silentfilm_cuda)

    # Device-link объект должен напрямую входить в конечный ELF, а не оставаться
    # невыбранным членом архива перед libcufft_static при host link.
    function(silentfilm_finalize_cuda target)
        set_target_properties(${target} PROPERTIES
            CUDA_RESOLVE_DEVICE_SYMBOLS ON
            CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
            CUDA_RUNTIME_LIBRARY Static)
    endfunction()
    silentfilm_finalize_cuda(ALEXZSilentFilm)
endif()

# Linux допускает unresolved symbols в MODULE; не выдаём такой OFX за готовый bundle.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(ALEXZSilentFilm PRIVATE "LINKER:-z,defs")
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

if(SILENTFILM_BUILD_CADENCE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Offline Cadence пока подготовлен только для Linux. Для OFX без этой утилиты задайте SILENTFILM_BUILD_CADENCE=OFF.")
    endif()
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    pkg_check_modules(CADENCE_FFMPEG REQUIRED IMPORTED_TARGET
        libavformat>=58 libavcodec>=58 libavutil>=56 libswscale>=5)
    add_executable(silentfilm_cadence src/cli/cadence_main.cpp src/cli/media_io.cpp)
    target_link_libraries(silentfilm_cadence PRIVATE silentfilm_core PkgConfig::CADENCE_FFMPEG Threads::Threads)
    target_compile_features(silentfilm_cadence PRIVATE cxx_std_17)
    target_compile_options(silentfilm_cadence PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(silentfilm_cadence PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(silentfilm_cadence PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${configuration} "${CMAKE_BINARY_DIR}/bin")
    endforeach()
    if(SILENTFILM_ENABLE_CUDA)
        target_compile_definitions(silentfilm_cadence PRIVATE SILENTFILM_CLI_CUDA=1)
        target_link_libraries(silentfilm_cadence PRIVATE silentfilm_cuda)
        silentfilm_finalize_cuda(silentfilm_cadence)
        set(SILENTFILM_CADENCE_DEFAULT_BACKEND "cuda")
    else()
        set(SILENTFILM_CADENCE_DEFAULT_BACKEND "cpu")
    endif()
    configure_file(scripts/SilentFilm_Cadence.lua.in "${CMAKE_BINARY_DIR}/scripts/SilentFilm_Cadence.lua" @ONLY)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(silentfilm_core_tests tests/core_tests.cpp)
    target_link_libraries(silentfilm_core_tests PRIVATE silentfilm_core)
    add_test(NAME silentfilm_core COMMAND silentfilm_core_tests)
    add_executable(silentfilm_sensitometry_tests tests/sensitometry_tests.cpp)
    target_link_libraries(silentfilm_sensitometry_tests PRIVATE silentfilm_core)
    add_test(NAME silentfilm_sensitometry COMMAND silentfilm_sensitometry_tests)
    add_executable(silentfilm_camera_timing_tests tests/camera_timing_tests.cpp)
    target_link_libraries(silentfilm_camera_timing_tests PRIVATE silentfilm_core)
    add_test(NAME silentfilm_camera_timing COMMAND silentfilm_camera_timing_tests)
    add_executable(silentfilm_projector_tests tests/projector_tests.cpp)
    target_link_libraries(silentfilm_projector_tests PRIVATE silentfilm_core)
    add_test(NAME silentfilm_projector COMMAND silentfilm_projector_tests)
    add_executable(silentfilm_cadence_tests tests/cadence_tests.cpp)
    target_link_libraries(silentfilm_cadence_tests PRIVATE silentfilm_core)
    add_test(NAME silentfilm_cadence COMMAND silentfilm_cadence_tests)
    if(SILENTFILM_BUILD_CADENCE)
        find_program(SILENTFILM_TEST_FFMPEG ffmpeg PATHS /usr/bin NO_DEFAULT_PATH)
        find_program(SILENTFILM_TEST_FFPROBE ffprobe PATHS /usr/bin NO_DEFAULT_PATH)
        if(SILENTFILM_TEST_FFMPEG AND SILENTFILM_TEST_FFPROBE)
            add_test(NAME silentfilm_cadence_export COMMAND "${CMAKE_COMMAND}"
                "-DHELPER=$<TARGET_FILE:silentfilm_cadence>" "-DFFMPEG=${SILENTFILM_TEST_FFMPEG}"
                "-DFFPROBE=${SILENTFILM_TEST_FFPROBE}" "-DTEST_ROOT=${CMAKE_BINARY_DIR}/cadence-artifacts"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cadence_integration.cmake")
        endif()
    endif()
    if(SILENTFILM_ENABLE_CUDA)
        add_executable(silentfilm_cuda_tests tests/cuda_tests.cpp)
        target_link_libraries(silentfilm_cuda_tests PRIVATE silentfilm_core silentfilm_cuda)
        silentfilm_finalize_cuda(silentfilm_cuda_tests)
        add_test(NAME silentfilm_cuda_parity COMMAND silentfilm_cuda_tests)
        set_tests_properties(silentfilm_cuda_parity PROPERTIES SKIP_RETURN_CODE 77)
        add_executable(silentfilm_camera_cuda_tests tests/camera_cuda_tests.cpp)
        target_link_libraries(silentfilm_camera_cuda_tests PRIVATE silentfilm_core silentfilm_cuda)
        silentfilm_finalize_cuda(silentfilm_camera_cuda_tests)
        add_test(NAME silentfilm_camera_cuda_parity COMMAND silentfilm_camera_cuda_tests)
        set_tests_properties(silentfilm_camera_cuda_parity PROPERTIES SKIP_RETURN_CODE 77)
        add_executable(silentfilm_projector_cuda_tests tests/projector_cuda_tests.cpp)
        target_link_libraries(silentfilm_projector_cuda_tests PRIVATE silentfilm_core silentfilm_cuda)
        silentfilm_finalize_cuda(silentfilm_projector_cuda_tests)
        add_test(NAME silentfilm_projector_cuda_parity COMMAND silentfilm_projector_cuda_tests)
        set_tests_properties(silentfilm_projector_cuda_parity PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

# Установка выполняется только отдельной пользовательской командой.
install(TARGETS ALEXZSilentFilm
    LIBRARY DESTINATION "ALEXZSilentFilm.ofx.bundle/Contents/${OFX_BINARY_DIRECTORY}"
    RUNTIME DESTINATION "ALEXZSilentFilm.ofx.bundle/Contents/${OFX_BINARY_DIRECTORY}")
