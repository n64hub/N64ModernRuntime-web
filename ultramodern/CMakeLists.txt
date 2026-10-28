cmake_minimum_required(VERSION 3.20)

project(ultramodern)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(ultramodern STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/audio.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/error_handling.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/events.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/extensions.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/input.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mesgqueue.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/misc_ultra.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer_context.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rsp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/scheduling.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/task_win32.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/threadqueue.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/threads.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/timer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ultrainit.cpp"
)

target_include_directories(ultramodern PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    "${PROJECT_SOURCE_DIR}/../thirdparty"
    "${PROJECT_SOURCE_DIR}/../thirdparty/concurrentqueue"
    "${PROJECT_SOURCE_DIR}/../thirdparty/sse2neon"
)

target_compile_options(ultramodern PRIVATE
#    -Wall
#    -Wextra
    -Wno-unused-parameter
)

if (WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# osStopThread on another thread, and osStartThread on a thread stopped while it waited on a message queue, as libultra
# does them (threads.cpp); without it, stopping another thread asserts.
option(ULTRAMODERN_LIBULTRA_THREAD_STOP "osStopThread/osStartThread on another thread as libultra does them (always on with Emscripten)" OFF)
if (ULTRAMODERN_LIBULTRA_THREAD_STOP OR EMSCRIPTEN)
    target_compile_definitions(ultramodern PRIVATE ULTRAMODERN_LIBULTRA_THREAD_STOP=1)
endif()

# An external message whose queue is full and that is to be sent again waits in a list of its own, tried again first at
# the next OS call or idle wait, instead of going back into the queue the idle thread spins on (mesgqueue.cpp).
option(ULTRAMODERN_HOLD_BLOCKED_MESSAGES "A blocked external message held in a list, not requeued (always on with Emscripten)" OFF)
if (ULTRAMODERN_HOLD_BLOCKED_MESSAGES OR EMSCRIPTEN)
    target_compile_definitions(ultramodern PRIVATE ULTRAMODERN_HOLD_BLOCKED_MESSAGES=1)
endif()

# An RSP task other than graphics runs from a copy of its OSTask and its data, taken as the game submits it
# (src/rsp.cpp, task_submitted).
option(ULTRAMODERN_RSP_TASKS_COPIED "An RSP task other than graphics runs from a copy of what the game submitted (always on with Emscripten)" OFF)
if (ULTRAMODERN_RSP_TASKS_COPIED OR EMSCRIPTEN)
    target_compile_definitions(ultramodern PRIVATE ULTRAMODERN_RSP_TASKS_COPIED=1)
endif()

# A load from ROM onto display list commands the renderer may not have read yet waits until it has (src/displaylists.cpp).
target_sources(ultramodern PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/displaylists.cpp")
option(ULTRAMODERN_LOADS_WAIT_FOR_DISPLAY_LISTS "A load from ROM onto display list commands the renderer may not have read yet waits for it (always on with Emscripten)" OFF)
if (ULTRAMODERN_LOADS_WAIT_FOR_DISPLAY_LISTS OR EMSCRIPTEN)
    target_compile_definitions(ultramodern PRIVATE ULTRAMODERN_LOADS_WAIT_FOR_DISPLAY_LISTS=1)
endif()

# The VI thread queues a VI's screen update after it swaps in the framebuffer the game gave it, not before
# (src/events.cpp, vi_thread_func).
option(ULTRAMODERN_SCREEN_UPDATE_AFTER_VI "A VI's screen update queued after its framebuffer swap (always on with Emscripten)" OFF)
if (ULTRAMODERN_SCREEN_UPDATE_AFTER_VI OR EMSCRIPTEN)
    target_compile_definitions(ultramodern PRIVATE ULTRAMODERN_SCREEN_UPDATE_AFTER_VI=1)
endif()
