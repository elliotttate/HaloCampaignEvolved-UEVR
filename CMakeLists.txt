cmake_minimum_required(VERSION 3.24)

project(HaloCEMotionControls LANGUAGES CXX)

set(
    UEVR_SDK_INCLUDE
    ""
    CACHE PATH
    "Directory containing uevr/Plugin.hpp (defaults to official UEVR 1.05)"
)

# Build against the oldest official praydog plugin ABI we target. A plugin
# compiled against API 2.34 also loads on newer UEVR builds; compiling against
# a newer header would unnecessarily raise the loader's minimum version. This
# is an ABI choice, not a claim that UEVR 1.05's scanner supports modern Halo.
if(NOT UEVR_SDK_INCLUDE)
    set(_local_uevr_105 "${CMAKE_CURRENT_SOURCE_DIR}/../UEVR-1.05-sdk/include")
    if(EXISTS "${_local_uevr_105}/uevr/Plugin.hpp")
        set(UEVR_SDK_INCLUDE "${_local_uevr_105}")
    else()
        include(FetchContent)
        FetchContent_Declare(
            uevr_105_sdk
            GIT_REPOSITORY "https://github.com/praydog/UEVR.git"
            GIT_TAG "1.05"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(uevr_105_sdk)
        if(NOT uevr_105_sdk_POPULATED)
            FetchContent_Populate(uevr_105_sdk)
        endif()
        set(UEVR_SDK_INCLUDE "${uevr_105_sdk_SOURCE_DIR}/include")
    endif()
endif()

if(NOT EXISTS "${UEVR_SDK_INCLUDE}/uevr/Plugin.hpp")
    message(FATAL_ERROR "UEVR_SDK_INCLUDE does not contain uevr/Plugin.hpp")
endif()

add_library(HaloCEMotionControls SHARED
    src/HaloCEVRConfig.cpp
    src/HaloCEVRCore.cpp
    src/main.cpp
)

target_compile_features(HaloCEMotionControls PRIVATE cxx_std_20)
target_compile_definitions(HaloCEMotionControls PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_include_directories(HaloCEMotionControls PRIVATE
    "${UEVR_SDK_INCLUDE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

set_target_properties(HaloCEMotionControls PROPERTIES
    OUTPUT_NAME "HaloCEMotionControls"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(HaloCEVRUEVRCompatibilityTest
        tests/test-uevr-compat.cpp
    )
    target_compile_features(HaloCEVRUEVRCompatibilityTest PRIVATE cxx_std_20)
    add_test(
        NAME HaloCEMotionControls.OfficialUEVRCompatibility
        COMMAND HaloCEVRUEVRCompatibilityTest
            $<TARGET_FILE:HaloCEMotionControls>
    )

    add_executable(HaloCEVRCoreTests
        tests/test-core.cpp
        src/HaloCEVRConfig.cpp
        src/HaloCEVRCore.cpp
    )
    target_compile_features(HaloCEVRCoreTests PRIVATE cxx_std_20)
    target_compile_definitions(HaloCEVRCoreTests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_include_directories(HaloCEVRCoreTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME HaloCEMotionControls.CoreUnitTests
        COMMAND HaloCEVRCoreTests
    )
    add_test(
        NAME HaloCEMotionControls.SourceValidation
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-source.ps1"
            -SourcePath "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    )
    add_test(
        NAME HaloCEMotionControls.ReticleValidation
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-reticle.ps1"
            -SourcePath "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
            -LuaPath "${CMAKE_CURRENT_SOURCE_DIR}/scripts/halo_motion_reticle.lua"
    )
    add_test(
        NAME HaloCEMotionControls.ValidationPlan
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/Invoke-HaloModValidation.ps1"
            -Suite Plan
    )
    add_test(
        NAME HaloCEMotionControls.PoseMatrixHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-pose-matrix.ps1"
    )
    add_test(
        NAME HaloCEMotionControls.ExtendedValidationHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-extended-validation.ps1"
    )
    add_test(
        NAME HaloCEMotionControls.ReticleSweepHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-reticle-sweep.ps1"
    )
    add_test(
        NAME HaloCEMotionControls.TopLevelValidationHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-mod-validation.ps1"
    )
    add_test(
        NAME HaloCEMotionControls.ReticleColorSequenceHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-reticle-color-sequence.ps1"
    )
    add_test(
        NAME HaloCEMotionControls.OfficialUEVRReleaseIntegration
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-release.ps1"
            -ReleaseRoot "${CMAKE_CURRENT_SOURCE_DIR}/release"
            -PackagerPath "${CMAKE_CURRENT_SOURCE_DIR}/tools/New-HaloStandaloneRelease.ps1"
    )
endif()
