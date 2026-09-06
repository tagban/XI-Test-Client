cmake_minimum_required(VERSION 3.28)
project(moghouse-renderer CXX)

# Objective-C++ only exists on Apple, and enabling it unconditionally makes the
# configure fail everywhere else.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Both static libraries here end up inside libmoghouse_interop.so, and Linux
# refuses to link non-PIC objects into a shared library - it fails at the very
# last step, on a relocation error naming a symbol rather than a cause. Windows
# has no such notion and macOS compiles PIC anyway, so this only ever mattered
# on Linux, which is why it went unnoticed until the first build there.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Dawn's exported target lists Threads::Threads in its link interface but its
# config never looks for it, so find_package(Dawn) fails unless we do this
# first.
find_package(Threads REQUIRED)

find_package(Dawn REQUIRED)
find_package(SDL3 REQUIRED)

if(WIN32)
    set(MH_SURFACE_SOURCE surface_win32.cpp)
elseif(APPLE)
    set(MH_SURFACE_SOURCE surface_metal.mm)
else()
    set(MH_SURFACE_SOURCE surface_linux.cpp)
endif()

# The renderer is a library first. MogHouse ships as one application, so the
# C# client has to be able to load this rather than talk to a second
# executable over a socket - see renderer/viewer.h.
add_library(moghouse-render STATIC
    music.cpp
    sounds.cpp
    character.cpp
    collision.cpp
    scene.cpp
    viewer.cpp
    zonemesh.cpp
    ${MH_SURFACE_SOURCE}

    textfont.cpp)
target_include_directories(moghouse-render PUBLIC .)
target_link_libraries(moghouse-render PUBLIC dawn::webgpu_dawn SDL3::SDL3 ffxi-dat)


# And a thin executable over it, so the renderer can still be exercised
# without the client.
add_executable(moghouse-renderer main.cpp)
target_link_libraries(moghouse-renderer PRIVATE moghouse-render)
# The glyph atlas is copied beside the executable so the renderer can find
# it without knowing where the source tree is.
add_custom_command(TARGET moghouse-renderer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:moghouse-renderer>/assets)

if(APPLE)
    target_link_libraries(moghouse-render PUBLIC "-framework QuartzCore" "-framework Cocoa")
    # surface_metal.mm uses __bridge casts, which only exist under ARC. Without
    # this the file does not compile.
    set_source_files_properties(surface_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

if(MSVC)
    add_custom_command(TARGET moghouse-renderer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:moghouse-renderer> $<TARGET_FILE_DIR:moghouse-renderer>
        COMMAND_EXPAND_LISTS
    )
endif()

# The FFXI asset readers, and a CLI that reports what they see. The CLI needs
# neither Dawn nor SDL, so it builds fast and can be checked against the python
# in tools/ that was validated across every zone in ROM/1.
add_library(ffxi-dat STATIC
    ffxi/bgw.cpp
    ffxi/spw.cpp
    ffxi/soundrefs.cpp
    ffxi/dat.cpp
    ffxi/d3m.cpp
    ffxi/filetable.cpp
    ffxi/generator.cpp
    ffxi/lighting.cpp
    ffxi/look.cpp
    ffxi/mmb.cpp
    ffxi/mo2.cpp
    ffxi/entitynames.cpp
    ffxi/mzb.cpp
    ffxi/os2.cpp
    ffxi/skeleton.cpp
    ffxi/sprite.cpp
    ffxi/texture.cpp

    ffxi/dialogue.cpp)
target_include_directories(ffxi-dat PUBLIC ffxi)

# The C ABI shim, so the C# client can load the renderer instead of talking to
# a second process. Lives outside renderer/ because it is the boundary, not the
# renderer.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../native/moghouse_interop moghouse_interop)

add_executable(ffxi-datdump ffxi/datdump_main.cpp)
target_link_libraries(ffxi-datdump PRIVATE ffxi-dat)

# Decoding a sound needs no GPU either, and "it parsed" is not evidence that
# the format was read correctly - see the autocorrelation check inside.
# What the people in a zone say. Needs no GPU, so it gets a CLI like the
# other readers and can be checked against a retail client by reading it.
add_executable(ffxi-dialogue ffxi/dialoguedump_main.cpp)
target_link_libraries(ffxi-dialogue PRIVATE ffxi-dat)

add_executable(ffxi-sounddump ffxi/sounddump_main.cpp ffxi/soundrefs.cpp)
target_link_libraries(ffxi-sounddump PRIVATE ffxi-dat)

# Skinning a character needs no GPU, so it gets its own CLI and can be checked
# the same way the zone readers were.
add_executable(ffxi-chardump chardump_main.cpp character.cpp)
target_include_directories(ffxi-chardump PRIVATE .)
target_link_libraries(ffxi-chardump PRIVATE ffxi-dat)

# Probing the ground query needs no GPU either, and finding out whether it
# answers from inside a render loop costs a rebuild per guess.
add_executable(ffxi-collisiondump collisiondump_main.cpp collision.cpp)
target_include_directories(ffxi-collisiondump PRIVATE .)
target_link_libraries(ffxi-collisiondump PRIVATE ffxi-dat)

# Comparing the drawn world against the walked one needs the scene built but
# never rasterised, so it runs without a GPU or a window. scene.cpp comes along
# because that is the code under test.
add_executable(ffxi-visiblecheck visiblecheck_main.cpp scene.cpp collision.cpp zonemesh.cpp)
target_include_directories(ffxi-visiblecheck PRIVATE .)
target_link_libraries(ffxi-visiblecheck PRIVATE ffxi-dat)
