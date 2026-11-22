# True Crime: Streets of LA (original Xbox, PAL), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(game LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
include(${SDK}/cmake/XbeRecomp.cmake)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# which European disc is in assets: the five-language disc (config/game.toml) or the English-only disc, a build
# of the same game laid out a little differently (config/game_en.toml, and TC_EN_DISC for src/game/hooks.cpp)
set(GAME_CONFIG game.toml)
set(GAME_EN_DISC 0)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe)
    file(SHA1 ${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xbe GAME_XBE_SHA1)
    # English-only disc: as dumped, or from an xiso copy (one start-up byte differs, the code is the same)
    if(GAME_XBE_SHA1 STREQUAL "ca906ba5a9459da4309df1d185683f4293c9d2ed" OR
       GAME_XBE_SHA1 STREQUAL "19aa3175ed8b31454a1d40e3cae34f5fd92b5505")
        set(GAME_CONFIG game_en.toml)
        set(GAME_EN_DISC 1)
    endif()
endif()
message(STATUS "True Crime: building for the ${GAME_CONFIG} disc")

# partitions as xbe_symbols wrote them into the config
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/${GAME_CONFIG})
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/${GAME_CONFIG})
    file(STRINGS config/${GAME_CONFIG} GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(TrueCrimeLA default.xbe ${GAME_CONFIG} ${GAME_PARTITIONS} game)
target_compile_definitions(TrueCrimeLA PRIVATE TC_EN_DISC=${GAME_EN_DISC})

# [TCSOLA] the setup window (src/common/tc_setup.cpp): Windows' WMA encoder for the PC songs, the folder picker
target_link_libraries(TrueCrimeLA PRIVATE mfplat mfreadwrite mfuuid mf shell32 ole32 uuid)
