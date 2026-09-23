# QCBridgeAE — see PLAN.md for what this is and where it's going.
#
# Nothing here needs the Adobe SDKs: src/common is the transport spine and is
# deliberately free of them, so the Windows machine can build and run the
# tests (lab/HANDOFF-windows.md, phase A5) before obtaining an SDK. The AEGP
# and Transmit targets land later and are the only ones that will need one.

cmake_minimum_required(VERSION 3.24)
project(QCBridgeAE VERSION 0.2.0 LANGUAGES CXX)

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
    src/common/convert/half_convert.cpp
)
target_include_directories(qcbae_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(qcbae_common PRIVATE /W4)
else()
    target_compile_options(qcbae_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# qcbae-aegp — the After Effects tap (phase A2, Route B).
#
# Needs the AE SDK, which is Adobe Confidential and never enters this repo
# (PLAN.md §Privacy 1). Point QCBAE_AE_SDK at your own copy; the default is
# the documented private/ location. Everything else in this project builds
# without it, so a machine with no SDK still gets the spine and its tests.
#
# Bundle mechanics follow the ordering minColor learned the hard way: Rez the
# PiPL, copy the .rsrc in, THEN codesign — copying after the linker seals the
# bundle ships an invalid signature. The .rsrc is a link dependency so a
# PiPL-only edit actually refreshes the bundle.
# ---------------------------------------------------------------------------
set(QCBAE_AE_SDK "${CMAKE_CURRENT_SOURCE_DIR}/private/sdk/AfterEffectsSDK"
    CACHE PATH "After Effects SDK root (not included; see PLAN.md)")
set(_ae_headers "${QCBAE_AE_SDK}/Examples/Headers")

if(APPLE)
    set(QCBAE_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity (- = ad-hoc)")
    if(QCBAE_CODESIGN_IDENTITY STREQUAL "-")
        set(_sign_flags --force --deep --sign -)
    else()
        set(_sign_flags --force --deep --options runtime --timestamp --sign "${QCBAE_CODESIGN_IDENTITY}")
    endif()
endif()

if(APPLE AND EXISTS "${_ae_headers}/AE_Effect.h")
    message(STATUS "AE SDK found: ${QCBAE_AE_SDK}")
    find_program(REZ_TOOL Rez)

    add_library(qcbae-aegp MODULE src/aegp/qcbae_aegp.cpp)
    target_link_libraries(qcbae-aegp PRIVATE qcbae_common)
    target_include_directories(qcbae-aegp PRIVATE
        "${_ae_headers}" "${_ae_headers}/SP" "${QCBAE_AE_SDK}/Examples/Util")
    set_target_properties(qcbae-aegp PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        OUTPUT_NAME "QCBridgeAE"
        # The plist templates take the bundle version from these, so
        # project() is the one version site.
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/aegp/Info.plist.in")
    # Without the explicit export, -dead_strip removes the entry point the
    # PiPL names and AE silently fails to bind the plugin.
    target_link_options(qcbae-aegp PRIVATE "-Wl,-exported_symbol,_EntryPointFunc" "-Wl,-dead_strip")

    set(_rsrc "${CMAKE_CURRENT_BINARY_DIR}/QCBridgeAE.rsrc")
    add_custom_command(OUTPUT "${_rsrc}"
        COMMAND ${REZ_TOOL} "${CMAKE_CURRENT_SOURCE_DIR}/src/aegp/QCBridgeAE_PiPL.r"
                -o "${_rsrc}" -useDF
                -i "${_ae_headers}" -i "${QCBAE_AE_SDK}/Examples/Resources"
                -i "${QCBAE_AE_SDK}/Examples/Util"
        DEPENDS src/aegp/QCBridgeAE_PiPL.r
        COMMENT "Rez PiPL (qcbae-aegp)")
    add_custom_target(qcbae-aegp_pipl DEPENDS "${_rsrc}")
    add_dependencies(qcbae-aegp qcbae-aegp_pipl)
    # Both the PiPL and the Info.plist must force a relink. POST_BUILD (rsrc
    # copy + codesign) only runs on relink, so editing either one otherwise
    # leaves a bundle whose signature no longer matches its contents —
    # "invalid Info.plist (plist or signature have been modified)". Hit that
    # once changing CFBundlePackageType; ninja reported no work to do.
    set_property(TARGET qcbae-aegp APPEND PROPERTY LINK_DEPENDS "${_rsrc}")
    set_property(TARGET qcbae-aegp APPEND PROPERTY LINK_DEPENDS
                 "${CMAKE_CURRENT_SOURCE_DIR}/src/aegp/Info.plist.in")
    add_custom_command(TARGET qcbae-aegp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:qcbae-aegp>/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy "${_rsrc}" "$<TARGET_BUNDLE_DIR:qcbae-aegp>/Contents/Resources/QCBridgeAE.rsrc"
        COMMAND codesign ${_sign_flags} "$<TARGET_BUNDLE_DIR:qcbae-aegp>"
        COMMAND codesign --verify --deep --strict "$<TARGET_BUNDLE_DIR:qcbae-aegp>"
        COMMENT "PiPL into bundle, then codesign" VERBATIM)
else()
    message(STATUS "AE SDK not found at ${QCBAE_AE_SDK} — skipping qcbae-aegp (everything else still builds)")
endif()

# ---------------------------------------------------------------------------
# qcbae-transmit — the Mercury Transmit device (A6), and its A4 probe (Route A).
#
# Needs the Premiere Pro SDK, also Adobe Confidential and also private/ only.
# A Transmit plugin on macOS is a plain BNDL bundle: no PiPL, and its whole
# ABI is one exported symbol, xTransmitEntry (PLAN.md §The tap). The same
# bundle loads in AE and Premiere from MediaCore (D6).
# ---------------------------------------------------------------------------
set(QCBAE_PR_SDK "${CMAKE_CURRENT_SOURCE_DIR}/private/sdk/PremiereProSDK"
    CACHE PATH "Premiere Pro SDK root (not included; see PLAN.md)")
set(_pr_headers "${QCBAE_PR_SDK}/Examples/Headers")

if(APPLE AND EXISTS "${_pr_headers}/PrSDKTransmit.h")
    message(STATUS "Premiere SDK found: ${QCBAE_PR_SDK}")
    function(qcbae_transmit_bundle target source output plist)
        add_library(${target} MODULE ${source})
        target_link_libraries(${target} PRIVATE qcbae_common)
        target_include_directories(${target} PRIVATE "${_pr_headers}")
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${output}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/${plist}")
        # The host binds by name; -dead_strip would otherwise take the entry point.
        target_link_options(${target} PRIVATE "-Wl,-exported_symbol,_xTransmitEntry" "-Wl,-dead_strip")
        # Same reason as the AEGP: POST_BUILD only runs on relink, so a
        # plist-only edit must force one or the signature stops matching.
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${plist}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND codesign ${_sign_flags} "$<TARGET_BUNDLE_DIR:${target}>"
            COMMAND codesign --verify --deep --strict "$<TARGET_BUNDLE_DIR:${target}>"
            COMMENT "codesign (${target})" VERBATIM)
    endfunction()
    # The product (A6): what users install.
    qcbae_transmit_bundle(qcbae-transmit src/transmit/qcbae_transmit.cpp
                          QCBridgeAE-Transmit src/transmit/Info.plist.in)
    # The A4 probe: publishes host frames unaltered into /qcbae-probe, with a
    # config file for control conditions. An instrument, kept for re-measuring.
    qcbae_transmit_bundle(qcbae-transmit-probe src/transmit/qcbae_transmit_probe.cpp
                          QCBridgeAE-Transmit-Probe src/transmit/Info-probe.plist.in)
elseif(WIN32 AND EXISTS "${_pr_headers}/PrSDKTransmit.h")
    message(STATUS "Premiere SDK found: ${QCBAE_PR_SDK}")
    # On Windows a Transmit plugin is a DLL named .prm, the way the SDK's
    # Transmitter sample builds it: PRWIN_ENV for the headers, /MD, the entry
    # point exported with __declspec(dllexport) (QCBAE_EXPORT in the source).
    # No PiPL, no manifest; MediaCore loads it by the extension.
    function(qcbae_transmit_prm target source output)
        add_library(${target} MODULE ${source})
        target_link_libraries(${target} PRIVATE qcbae_common)
        target_include_directories(${target} PRIVATE "${_pr_headers}" "${QCBAE_PR_SDK}/Examples/Utils")
        target_compile_definitions(${target} PRIVATE PRWIN_ENV)
        set_target_properties(${target} PROPERTIES
            OUTPUT_NAME "${output}"
            PREFIX ""
            SUFFIX ".prm")
    endfunction()
    qcbae_transmit_prm(qcbae-transmit src/transmit/qcbae_transmit.cpp QCBridgeAE-Transmit)
    qcbae_transmit_prm(qcbae-transmit-probe src/transmit/qcbae_transmit_probe.cpp QCBridgeAE-Transmit-Probe)
else()
    message(STATUS "Premiere SDK not found at ${QCBAE_PR_SDK} — skipping qcbae-transmit")
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

if(APPLE)
    add_executable(qcbae-gpubench tests/gpu_sample_bench.mm)
    target_link_libraries(qcbae-gpubench PRIVATE qcbae_common
        "-framework Metal" "-framework Foundation")
    target_compile_options(qcbae-gpubench PRIVATE -fobjc-arc)

    # QCView's copy-and-upload ingest against sampling the slot in place.
    add_executable(qcbae-ingestbench tests/ingest_bench.mm)
    target_link_libraries(qcbae-ingestbench PRIVATE
        "-framework Metal" "-framework Foundation")
    target_compile_options(qcbae-ingestbench PRIVATE -fobjc-arc)
endif()

include(CTest)
if(BUILD_TESTING)
    # Cross-process: fork() on POSIX, a re-exec of itself as the consumer on
    # Windows (A5). Either way two real processes share the mapping.
    add_executable(ring_tearing_test tests/ring_tearing_test.cpp)
    target_link_libraries(ring_tearing_test PRIVATE qcbae_common)
    add_test(NAME ring_tearing COMMAND ring_tearing_test)
    set_tests_properties(ring_tearing PROPERTIES TIMEOUT 120)

    # The Transmit conversion pass, read back the way QCView reads it, and the
    # portable half conversion held bit for bit to the hardware (PLAN.md D4).
    add_executable(convert_test tests/convert_test.cpp)
    target_link_libraries(convert_test PRIVATE qcbae_common)
    add_test(NAME convert COMMAND convert_test)

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
