# DLL-зависимости внешнего приложения не попадают в OFX-модуль.
set(SILENTFILM_WINDOWS_MEDIA_ROOT "" CACHE PATH "vcpkg installed/x64-silentfilm-media")
if(NOT IS_DIRECTORY "${SILENTFILM_WINDOWS_MEDIA_ROOT}/bin")
    message(FATAL_ERROR "Укажите SILENTFILM_WINDOWS_MEDIA_ROOT; сначала запустите Prepare-Cadence-Dependencies.ps1.")
endif()
add_library(silentfilm_media_libraries INTERFACE)
target_include_directories(silentfilm_media_libraries INTERFACE "${SILENTFILM_WINDOWS_MEDIA_ROOT}/include")
foreach(component avformat avcodec avutil swscale)
    find_library(SILENTFILM_${component}_LIBRARY NAMES ${component}
        PATHS "${SILENTFILM_WINDOWS_MEDIA_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(silentfilm_media_libraries INTERFACE "${SILENTFILM_${component}_LIBRARY}")
endforeach()
target_link_libraries(silentfilm_media_libraries INTERFACE ws2_32)
set(CADENCE_MEDIA_TARGET silentfilm_media_libraries)
# Для Windows flow обязателен: не выдаём урезанный simple-only helper за полный.
find_package(OpenCV REQUIRED COMPONENTS core imgproc video
    PATHS "${SILENTFILM_WINDOWS_MEDIA_ROOT}/share/opencv4" NO_DEFAULT_PATH)

find_program(SILENTFILM_TEST_FFMPEG ffmpeg PATHS "${SILENTFILM_WINDOWS_MEDIA_ROOT}/tools/ffmpeg" NO_DEFAULT_PATH REQUIRED)
find_program(SILENTFILM_TEST_FFPROBE ffprobe PATHS "${SILENTFILM_WINDOWS_MEDIA_ROOT}/tools/ffmpeg" NO_DEFAULT_PATH REQUIRED)
