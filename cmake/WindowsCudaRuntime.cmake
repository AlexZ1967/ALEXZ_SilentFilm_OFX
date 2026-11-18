# Windows cuFFT доступна только как DLL. Не меняем PATH процесса Resolve.
if(NOT MSVC)
    message(FATAL_ERROR "Windows CUDA pipeline подготовлен для MSVC x64; используйте Visual Studio 2022.")
endif()
set(SILENTFILM_CUDA_RUNTIME_DIR "${CUDAToolkit_BIN_DIR}" CACHE PATH "Каталог DLL выбранного CUDA Toolkit (bin или bin/x64)")
file(GLOB cufft_runtime_candidates "${SILENTFILM_CUDA_RUNTIME_DIR}/cufft64_*.dll"
    "${SILENTFILM_CUDA_RUNTIME_DIR}/x64/cufft64_*.dll")
list(LENGTH cufft_runtime_candidates cufft_runtime_count)
if(NOT cufft_runtime_count EQUAL 1)
    message(FATAL_ERROR "Нужна ровно одна cufft64_*.dll от выбранного Toolkit. Укажите SILENTFILM_CUDA_RUNTIME_DIR.")
endif()
list(GET cufft_runtime_candidates 0 cufft_runtime)
get_filename_component(cufft_runtime_name "${cufft_runtime}" NAME)
get_filename_component(cuda_runtime_folder "${cufft_runtime}" DIRECTORY)
file(GLOB cuda_jit_runtime "${cuda_runtime_folder}/nvJitLink*.dll")
set(silentfilm_cuda_runtime_files "${cufft_runtime}" ${cuda_jit_runtime})

# Delay-load hook загружает cuFFT по абсолютному пути из папки нашего модуля.
# Копии у тестов позволяют запустить CTest без добавления Toolkit в PATH.
get_property(silentfilm_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS silentfilm_targets)
    get_target_property(target_type ${target} TYPE)
    if(target STREQUAL "ALEXZSilentFilm" OR target STREQUAL "silentfilm_cadence" OR (target_type STREQUAL "EXECUTABLE" AND target MATCHES "_cuda_tests$"))
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows_cuda_loader.cpp")
        target_compile_definitions(${target} PRIVATE SILENTFILM_CUFFT_DLL="${cufft_runtime_name}")
        target_link_libraries(${target} PRIVATE delayimp)
        target_link_options(${target} PRIVATE "/DELAYLOAD:${cufft_runtime_name}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${silentfilm_cuda_runtime_files} "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endforeach()
install(FILES ${silentfilm_cuda_runtime_files}
    DESTINATION "ALEXZSilentFilm.ofx.bundle/Contents/Win64")
if(SILENTFILM_BUILD_CADENCE)
    install(FILES ${silentfilm_cuda_runtime_files} DESTINATION "SilentFilmCadence/bin")
endif()
