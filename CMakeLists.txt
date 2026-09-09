cmake_minimum_required(VERSION 3.20)
project(InitiumV2)

set(CMAKE_CXX_STANDARD 23)

if (CMAKE_BUILD_TYPE MATCHES Debug)
    add_compile_options(
            -g
            -O1
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
    )
    add_link_options(
            -fsanitize=address,undefined
    )
elseif (CMAKE_BUILD_TYPE MATCHES RelWithDebInfo)
    add_compile_options(
            -O2
            -g
            -UNDEBUG
    )
elseif (CMAKE_BUILD_TYPE MATCHES Release)
    add_compile_options(-O3 -march=native -ffast-math)
    add_link_options(-flto)
endif ()

include(FetchContent)

FetchContent_Declare(
        ctre
        GIT_REPOSITORY https://github.com/hanickadot/compile-time-regular-expressions.git
        GIT_TAG v3.9.0
)
FetchContent_MakeAvailable(ctre)

add_subdirectory(libs)
add_subdirectory(apps)
add_subdirectory(web/server)
