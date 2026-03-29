cmake_minimum_required(VERSION 3.24)
project(vlc-subtitle-gen LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── whisper.cpp via FetchContent ──────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp
    GIT_TAG        v1.7.5
    GIT_SHALLOW    TRUE
)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_METAL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(whisper)

# ── FFmpeg via pkg-config ─────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL   REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

# ── Folly (structured concurrency) ───────────────────────────────────────────
find_package(folly CONFIG REQUIRED)

# Folly's cmake config adds the macOS SDK include path as -isystem, which
# breaks libc++ wrapper headers (cerrno, cmath, etc.) by causing them to
# find the C header before the C++ wrapper.  Mark it as implicit so CMake
# won't emit the -isystem flag.
if(APPLE)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _sdk_path
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(APPEND CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES
        "${_sdk_path}/usr/include"
        "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include"
        "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk/usr/include")
endif()


# ── Main executable ──────────────────────────────────────────────────────────
add_executable(vlc-subtitle-gen
    src/main.cpp
    src/audio_decoder.cpp
    src/transcriber.cpp
    src/srt_writer.cpp
    src/pipeline.cpp
)

target_include_directories(vlc-subtitle-gen PRIVATE src)

target_link_libraries(vlc-subtitle-gen PRIVATE
    whisper
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWRESAMPLE
    Folly::folly_coro_task
    Folly::folly_coro_blocking_wait
    Folly::folly_coro_collect
    Folly::folly_executors_cpu_thread_pool_executor
)

# ── Benchmark executable ─────────────────────────────────────────────────────
add_executable(bench_main
    benchmark/bench_main.cpp
    src/audio_decoder.cpp
    src/transcriber.cpp
    src/srt_writer.cpp
    src/pipeline.cpp
)

target_include_directories(bench_main PRIVATE src)

target_link_libraries(bench_main PRIVATE
    whisper
    PkgConfig::AVFORMAT
    PkgConfig::AVCODEC
    PkgConfig::AVUTIL
    PkgConfig::SWRESAMPLE
    Folly::folly_coro_task
    Folly::folly_coro_blocking_wait
    Folly::folly_coro_collect
    Folly::folly_executors_cpu_thread_pool_executor
)
