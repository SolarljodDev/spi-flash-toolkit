cmake_minimum_required(VERSION 3.20)

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/cmake/mcu_config.cmake")
    message(FATAL_ERROR "cmake/mcu_config.cmake not found!\nRun setup.ps1 before first build.")
endif()
include(cmake/mcu_config.cmake)
# tool_paths.cmake: machine-specific paths (CMSIS_DIR, OPENOCD_EXE, etc.)
if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/tool_paths.cmake")
    include("${CMAKE_SOURCE_DIR}/cmake/tool_paths.cmake")
endif()
# build_config.cmake: user-editable compile flags (optimization, warnings, C std, etc.)
if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/build_config.cmake")
    include("${CMAKE_SOURCE_DIR}/cmake/build_config.cmake")
endif()

project(spi-flash-toolkit C ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CPU flags: override via BUILD_CPU_FLAGS_OVERRIDE in build_config.cmake, else use MCU defaults
if(BUILD_CPU_FLAGS_OVERRIDE)
    set(CPU_FLAGS "${BUILD_CPU_FLAGS_OVERRIDE}")
else()
    set(CPU_FLAGS "${MCU_CPU_FLAGS}")
    if(MCU_FPU_FLAGS)
        string(APPEND CPU_FLAGS " ${MCU_FPU_FLAGS}")
    endif()
endif()

# Float ABI: override via BUILD_FLOAT_ABI_OVERRIDE in build_config.cmake, else use MCU default
if(BUILD_FLOAT_ABI_OVERRIDE)
    set(FLOAT_FLAGS "-mfloat-abi=${BUILD_FLOAT_ABI_OVERRIDE}")
elseif(MCU_FLOAT_ABI)
    set(FLOAT_FLAGS "-mfloat-abi=${MCU_FLOAT_ABI}")
else()
    set(FLOAT_FLAGS "")
endif()

set(CMAKE_C_FLAGS
    "${CPU_FLAGS} ${FLOAT_FLAGS} ${BUILD_OPT} ${BUILD_C_STD} ${BUILD_WARNINGS} ${BUILD_DEBUG} -ffunction-sections -fdata-sections ${BUILD_EXTRA_DEFINES} ${BUILD_EXTRA_FLAGS}"
    CACHE STRING "" FORCE)
set(CMAKE_ASM_FLAGS
    "${CPU_FLAGS} ${FLOAT_FLAGS} -x assembler-with-cpp"
    CACHE STRING "" FORCE)
if(MCU_ARCH STREQUAL "RISCV")
    set(LINKER_WARN_FLAGS "")
    set(NOSTARTFILES "-nostartfiles")
else()
    set(LINKER_WARN_FLAGS ",--no-warn-rwx-segments")
    set(NOSTARTFILES "")
endif()
set(CMAKE_EXE_LINKER_FLAGS
    "${CPU_FLAGS} ${FLOAT_FLAGS} ${NOSTARTFILES} -T${CMAKE_SOURCE_DIR}/${MCU_LD_SCRIPT} -Wl,--gc-sections${LINKER_WARN_FLAGS} --specs=nano.specs -lm -lnosys"
    CACHE STRING "" FORCE)

add_definitions(-D${MCU_DEFINE})

# ================================================================
# USER FILES  вЂ”  add your .h and .c paths here
# ================================================================
# Include dirs: paths to folders that contain your .h files
include_directories(
    user/inc        # your headers  (auto-included)
    device/inc
    # path/to/extra/inc
)

# Sources: every .c in user/src/ is compiled automatically.
# Append extra files or whole directories below as needed:
file(GLOB_RECURSE USER_SOURCES "user/src/*.c")
# file(GLOB_RECURSE EXTRA_SRC  "extra/src/*.c")
# list(APPEND USER_SOURCES ${EXTRA_SRC})
# list(APPEND USER_SOURCES "lib/foo.c")
# ================================================================

if(MCU_NEEDS_CMSIS)
    if(DEFINED CMSIS_DIR AND EXISTS "${CMSIS_DIR}")
        include_directories("${CMSIS_DIR}")
    else()
        include_directories(cmsis)  # fallback to local copy
    endif()
endif()

set(SOURCES ${USER_SOURCES} ${MCU_STARTUP} ${MCU_SYSTEM})

add_executable(spi-flash-toolkit.elf ${SOURCES})

add_custom_command(TARGET spi-flash-toolkit.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:spi-flash-toolkit.elf>
            ${CMAKE_BINARY_DIR}/spi-flash-toolkit.hex
    COMMENT "HEX: spi-flash-toolkit.hex" VERBATIM)
add_custom_command(TARGET spi-flash-toolkit.elf POST_BUILD
    COMMAND ${CMAKE_SIZE_UTIL} $<TARGET_FILE:spi-flash-toolkit.elf>
    COMMENT "Firmware size:" VERBATIM)