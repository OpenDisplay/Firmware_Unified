# Define minimal required version of CMake.
cmake_minimum_required(VERSION "3.25")

# Project definition
project(
	opendisplay-bg22
	VERSION 1.0
	LANGUAGES C CXX ASM
)

option(OD_ENABLE_RTT "Enable SEGGER RTT logging backend" ON)
option(OD_GENERATE_GBL_OTA "After link, run Simplicity Commander to create OTA .gbl next to .s37" ON)
option(OD_TNB132M_BOOT_PROBE "At boot read TNB132M NDEF via Type-3 AI@0x48 + data@0x40 (log over RTT)" OFF)
option(OD_TNB132M_WRITE_NDEF "With BOOT_PROBE: before the read, write OD_TNB132M_NDEF_TEXT as a Text record" OFF)
set(OD_TNB132M_NDEF_TEXT "OpenDisplay" CACHE STRING "UTF-8 text (lang=en) written when OD_TNB132M_WRITE_NDEF=ON; max 25 chars")

set(OD_APP_VERSION "0x0300" CACHE STRING "16-bit BLE app version (hex, high byte=major, low=minor); ignored when OD_FW_VERSION is set")
set(OD_FW_VERSION "" CACHE STRING "App version string major.minor[.patch] (optional leading v); drives BLE 0x43 + boot screen")
set(OPENDISPLAY_BUILD_ID "bg22-dev" CACHE STRING "Firmware version string (GATT 0x0043 / build id)")
set(OD_SL_APPLICATION_VERSION "" CACHE STRING "If set, overrides the derived SL_APPLICATION_VERSION (decimal uint32)")

# Include the definition of the slc_opendisplay-bg22 target,
# which contains the content of the SLC project
include(opendisplay-bg22.cmake)

# main.c and other app sources live in target `slc`; they must see OD_ENABLE_RTT when compiled.
target_compile_definitions(slc PUBLIC
	OD_ENABLE_RTT=$<IF:$<BOOL:${OD_ENABLE_RTT}>,1,0>
	OD_TNB132M_BOOT_PROBE=$<IF:$<BOOL:${OD_TNB132M_BOOT_PROBE}>,1,0>
	OD_TNB132M_WRITE_NDEF=$<IF:$<BOOL:${OD_TNB132M_WRITE_NDEF}>,1,0>
	"OD_TNB132M_NDEF_TEXT=\"${OD_TNB132M_NDEF_TEXT}\""
	SL_APP_PROPERTIES=1
	"OD_APP_VERSION=${OD_APP_VERSION}"
	"OPENDISPLAY_BUILD_ID=\"${OPENDISPLAY_BUILD_ID}\""
)

if(NOT "${OD_FW_VERSION}" STREQUAL "")
  target_compile_definitions(slc PUBLIC "OD_FW_VERSION=\"${OD_FW_VERSION}\"")
  message(STATUS "OpenDisplay FW version: ${OD_FW_VERSION}")
endif()

# SL_APPLICATION_VERSION (app properties, read by the Gecko Bootloader/AppLoader) is derived
# from the firmware version unless OD_SL_APPLICATION_VERSION overrides it:
#
#     (major << 16) | (minor << 8) | patch        3.0.0 -> 196608, 3.1.2 -> 196866
#
# Firmware_Silabs 2.x stamped (major << 8) | minor (2.3.0 -> 515), which tops out at 65535, so
# every 3.x value is above every 2.x value and the sequence stays monotonic across the switch.
# The shipped bootloader is built with BOOTLOADER_ROLLBACK_PROTECTION 0, so nothing refuses a
# lower value today; this keeps that true if rollback protection is ever turned on. Source of
# the numbers: OD_FW_VERSION ("[v]M.m[.p]") when set, else OD_APP_VERSION (0xMMmm, patch 0).
if("${OD_SL_APPLICATION_VERSION}" STREQUAL "")
  if(NOT "${OD_FW_VERSION}" STREQUAL "")
    string(REGEX MATCH "^v?([0-9]+)\\.([0-9]+)(\\.([0-9]+))?" _od_v "${OD_FW_VERSION}")
    if(NOT _od_v)
      message(FATAL_ERROR "OD_FW_VERSION '${OD_FW_VERSION}' is not [v]major.minor[.patch]")
    endif()
    set(_od_maj "${CMAKE_MATCH_1}")
    set(_od_min "${CMAKE_MATCH_2}")
    set(_od_pat "${CMAKE_MATCH_4}")
    if("${_od_pat}" STREQUAL "")
      set(_od_pat 0)
    endif()
  else()
    math(EXPR _od_maj "(${OD_APP_VERSION} >> 8) & 255")
    math(EXPR _od_min "${OD_APP_VERSION} & 255")
    set(_od_pat 0)
  endif()
  if(_od_maj GREATER 255 OR _od_min GREATER 255 OR _od_pat GREATER 255)
    message(FATAL_ERROR "firmware version component above 255 cannot be encoded")
  endif()
  math(EXPR OD_SL_APPLICATION_VERSION "(${_od_maj} << 16) | (${_od_min} << 8) | ${_od_pat}")
endif()
message(STATUS "SL_APPLICATION_VERSION: ${OD_SL_APPLICATION_VERSION}")
target_compile_definitions(slc PUBLIC "SL_APPLICATION_VERSION=${OD_SL_APPLICATION_VERSION}u")

# Silicon Labs application properties.
#
# app_properties.c is what the Gecko Bootloader/AppLoader reads to validate the application, so
# it gates the .gbl OTA path -- the only field-update mechanism any target in this repo has. It
# is REQUIRED, not optional, and it comes from the pinned SDK rather than a vendored copy.
#
# SDK_PATH must be a FULL Simplicity SDK. A tree pruned to just the components this target
# compiles against carries no bootloader_interface/ and fails here -- see docs/TOOLCHAINS.md for
# the pinned install location.
#
# The target keeps its own config/app_properties_config.h and config/sl_application_type.h:
# those carry the OpenDisplay signature, version, capabilities and product id, which ARE ours
# and must not come from the SDK.
set(OPD_APP_PROPERTIES_SRC
    "${SDK_PATH}/bootloader_interface/platform/bootloader/app_properties/app_properties.c")
if(NOT EXISTS "${OPD_APP_PROPERTIES_SRC}")
  message(FATAL_ERROR
    "The pinned Simplicity SDK has no app_properties.c at\n"
    "  ${OPD_APP_PROPERTIES_SRC}\n"
    "This is REQUIRED, not optional: without it the .gbl OTA image is not valid and the\n"
    "only field-update path this fleet has stops working. Check SDK_PATH.")
endif()
target_sources(slc PRIVATE "${OPD_APP_PROPERTIES_SRC}")
target_include_directories(slc PUBLIC
    "${SDK_PATH}/bootloader_interface/platform/bootloader")
target_include_directories(slc PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/../config"
)

if(NOT OD_ENABLE_RTT)
    set_source_files_properties(
        "../third_party/segger_rtt/SEGGER_RTT.c"
        "../od_rtt.c"
        PROPERTIES HEADER_FILE_ONLY TRUE
    )
endif()

add_executable(opendisplay-bg22
    # Add additional sources here
)

target_include_directories(opendisplay-bg22 PUBLIC
    # Add additional include paths here
)

target_compile_definitions(opendisplay-bg22 PUBLIC
    OD_ENABLE_RTT=$<IF:$<BOOL:${OD_ENABLE_RTT}>,1,0>
)

target_compile_options(opendisplay-bg22 PUBLIC
    # Set additional compiler flags here
)

target_link_options(opendisplay-bg22 PUBLIC
    # Set additional linker flags here
)

# Link with the content defined in the SLC project
target_link_libraries(opendisplay-bg22 PRIVATE
    slc
)

set(OPD_BOOTLOADER_S37 "${CMAKE_CURRENT_LIST_DIR}/../bootloader-artifact/opendisplay-bootloader.s37")
if(EXISTS "${OPD_BOOTLOADER_S37}")
  add_custom_command(
    TARGET opendisplay-bg22
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OPD_BOOTLOADER_S37}"
      "$<TARGET_FILE_DIR:opendisplay-bg22>/bootloader-apploader.s37"
    COMMENT "Stage bootloader-apploader.s37 (from bootloader-artifact/)"
    VERBATIM
  )
else()
  message(WARNING "Missing ${OPD_BOOTLOADER_S37} — full-flash bootloader not staged (see bootloader-artifact/README.md)")
endif()

# Include managed project content if available
include(opendisplay-bg22_project.cmake OPTIONAL RESULT_VARIABLE managed_project)
if(managed_project)
    message(STATUS "Using managed project content from ${managed_project}")
endif()

# Force the gcc linker command
set_target_properties(opendisplay-bg22 PROPERTIES LINKER_LANGUAGE C)

# Create .bin, .hex and .s37 artifacts after building the project
add_custom_command(TARGET opendisplay-bg22
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_SREC_CMD} "$<TARGET_FILE:opendisplay-bg22>" "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.s37"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_IHEX_CMD} "$<TARGET_FILE:opendisplay-bg22>" "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.hex"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_BIN_CMD}  "$<TARGET_FILE:opendisplay-bg22>" "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.bin"
)

set(OPD_COMMANDER "")
if(DEFINED ENV{COMMANDER} AND EXISTS "$ENV{COMMANDER}")
  cmake_path(SET OPD_COMMANDER "$ENV{COMMANDER}" NORMALIZE)
elseif(DEFINED POST_BUILD_EXE AND POST_BUILD_EXE AND EXISTS "${POST_BUILD_EXE}")
  set(OPD_COMMANDER "${POST_BUILD_EXE}")
else()
  cmake_path(SET _opd_cmd_candidate "${CMAKE_CURRENT_LIST_DIR}/../../SimplicityCommander-Linux/commander/commander")
  if(EXISTS "${_opd_cmd_candidate}")
    set(OPD_COMMANDER "${_opd_cmd_candidate}")
  endif()
endif()
if(NOT OPD_COMMANDER)
  find_program(OPD_COMMANDER NAMES commander)
endif()

if(OD_GENERATE_GBL_OTA AND OPD_COMMANDER)
  add_custom_command(
    TARGET opendisplay-bg22
    POST_BUILD
    COMMAND "${OPD_COMMANDER}" gbl create
      "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.gbl"
      --app "$<TARGET_FILE_DIR:opendisplay-bg22>/$<TARGET_FILE_BASE_NAME:opendisplay-bg22>.s37"
    COMMENT "OTA: commander gbl create -> $<TARGET_FILE_BASE_NAME:opendisplay-bg22>.gbl"
    VERBATIM
  )
elseif(OD_GENERATE_GBL_OTA)
  message(STATUS "Simplicity Commander not found (slt/COMMANDER/ PATH); CMake will not emit .gbl — use build-and-flash.sh or set COMMANDER / POST_BUILD_EXE")
endif()

# Run post-build pipeline to perform additional post-processing
if(post_build_command)
add_custom_command(TARGET opendisplay-bg22
    POST_BUILD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    COMMAND ${post_build_command}
)
endif()