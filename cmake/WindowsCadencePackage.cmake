file(GLOB silentfilm_media_runtime "${SILENTFILM_WINDOWS_MEDIA_ROOT}/bin/*.dll")
if(NOT EXISTS "${SILENTFILM_WINDOWS_MEDIA_ROOT}/bin/zstd.dll")
    message(FATAL_ERROR "Нет zstd.dll для Color DRX; установите zstd:x64-silentfilm-media.")
endif()
get_property(package_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS package_targets)
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${silentfilm_media_runtime} "$<TARGET_FILE_DIR:${target}>" VERBATIM)
    endif()
endforeach()
# Python CTest запускается без GTK; Zstd загружается по явному пути.
if(SILENTFILM_TEST_PYTHON)
    get_property(package_tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS package_tests)
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
            "PYTHONUTF8=1" "SILENTFILM_ZSTD_LIBRARY=${SILENTFILM_WINDOWS_MEDIA_ROOT}/bin/zstd.dll"
            "SILENTFILM_FFMPEG=${SILENTFILM_TEST_FFMPEG}")
    endforeach()
endif()
install(TARGETS silentfilm_cadence RUNTIME DESTINATION "SilentFilmCadence/bin")
install(FILES ${silentfilm_media_runtime} "${SILENTFILM_TEST_FFMPEG}" "${SILENTFILM_TEST_FFPROBE}"
    DESTINATION "SilentFilmCadence/bin")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/scripts/" DESTINATION "SilentFilmCadence/scripts"
    FILES_MATCHING PATTERN "cadence_*.py")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/presets/" DESTINATION "SilentFilmCadence/presets"
    FILES_MATCHING PATTERN "crank_exposure_template*.drx")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/scripts/windows/Start-Cadence.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/windows/Install-Cadence-Menu.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/windows/Start-Cadence.cmd" DESTINATION "SilentFilmCadence")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/WINDOWS_CADENCE_APP.md" DESTINATION "SilentFilmCadence")
configure_file(scripts/windows/cadence_runtime.json.in "${CMAKE_BINARY_DIR}/cadence_runtime.json" @ONLY)
install(FILES "${CMAKE_BINARY_DIR}/cadence_runtime.json" DESTINATION "SilentFilmCadence")
