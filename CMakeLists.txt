# QCBridgeAE — see PLAN.md for what this is and where it's going.
#
# Nothing here needs the Adobe SDKs: src/common is the transport spine and is
# deliberately free of them, so the Windows machine can build and run the
# tests (lab/HANDOFF-windows.md, phase A5) before obtaining an SDK. The AEGP
# and Transmit targets land later and are the only ones that will need one.

cmake_minimum_required(VERSION 3.24)
project(QCBridgeAE VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# qcbae_common — the shared-surface spine (phase A1)
# ---------------------------------------------------------------------------
add_library(qcbae_common STATIC
    src/common/surface/shared_ring.cpp
)
target_include_directories(qcbae_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(qcbae_common PRIVATE /W4)
else()
    target_compile_options(qcbae_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# qcbae-probe — synthetic producer + Metal viewer (phase A1b, macOS only).
# Throwaway: `produce` is replaced by the AE plugin in A2, `view` by QCView in
# A3. It exists so the transport can be watched before anything depends on it.
# ---------------------------------------------------------------------------
if(APPLE)
    add_executable(qcbae-probe src/viewer-probe/probe_main.mm)
    target_link_libraries(qcbae-probe PRIVATE qcbae_common
        "-framework Cocoa" "-framework Metal" "-framework QuartzCore")
    target_compile_options(qcbae-probe PRIVATE -fobjc-arc)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# Measurement, not assertion: the numbers behind D1. Run by hand; the Windows
# machine should re-run it, since the ratios may differ off unified memory.
add_executable(qcbae-convbench tests/convert_bench.cpp)
target_link_libraries(qcbae-convbench PRIVATE qcbae_common)

include(CTest)
if(BUILD_TESTING)
    # Cross-process, so it needs fork(); the Windows port arrives with A5.
    if(NOT WIN32)
        add_executable(ring_tearing_test tests/ring_tearing_test.cpp)
        target_link_libraries(ring_tearing_test PRIVATE qcbae_common)
        add_test(NAME ring_tearing COMMAND ring_tearing_test)
        set_tests_properties(ring_tearing PROPERTIES TIMEOUT 120)
    endif()

    add_executable(ring_unit_test tests/ring_unit_test.cpp)
    target_link_libraries(ring_unit_test PRIVATE qcbae_common)
    add_test(NAME ring_unit COMMAND ring_unit_test)

    # The zero-copy claim that justified shared memory over IOSurface. It is
    # a test, not a note, because "the window looks right" cannot tell a
    # shared mapping from a silent upload.
    if(APPLE)
        add_executable(metal_zerocopy_test tests/metal_zerocopy_test.mm)
        target_link_libraries(metal_zerocopy_test PRIVATE qcbae_common
            "-framework Metal" "-framework Foundation")
        target_compile_options(metal_zerocopy_test PRIVATE -fobjc-arc)
        add_test(NAME metal_zerocopy COMMAND metal_zerocopy_test)

        # D1's premise: one shader samples every per-tier wire format.
        add_executable(texture_format_test tests/texture_format_test.mm)
        target_link_libraries(texture_format_test PRIVATE qcbae_common
            "-framework Metal" "-framework Foundation")
        target_compile_options(texture_format_test PRIVATE -fobjc-arc)
        add_test(NAME texture_format COMMAND texture_format_test)
    endif()
endif()
