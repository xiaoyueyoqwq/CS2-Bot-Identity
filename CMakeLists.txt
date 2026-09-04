cmake_minimum_required(VERSION 3.20)
project(BotIdentity CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Required env vars:
#   HL2SDKCS2    : hl2sdk-cs2 checkout
#   MMSOURCE_DEV : metamod-source checkout
#

set(HL2SDK  $ENV{HL2SDKCS2})
set(MMS     $ENV{MMSOURCE_DEV})

if(NOT HL2SDK OR NOT EXISTS "${HL2SDK}")
    message(FATAL_ERROR "HL2SDKCS2 env var not set or path missing: '${HL2SDK}'")
endif()
if(NOT MMS OR NOT EXISTS "${MMS}")
    message(FATAL_ERROR "MMSOURCE_DEV env var not set or path missing: '${MMS}'")
endif()

# SDK source files that Metamod plugins must compile themselves
set(SDK_SOURCES
    ${HL2SDK}/public/tier0/memoverride.cpp
    ${HL2SDK}/tier1/convar.cpp
    ${MMS}/core/sourcehook/sourcehook.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_chookidman.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_chookmaninfo.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_cvfnptr.cpp
    ${MMS}/core/sourcehook/sourcehook_impl_cproto.cpp
)

add_library(BotIdentity SHARED
    src/plugin.cpp
    src/bot_info.cpp
    src/entity_access.cpp
    src/shm_pub.cpp
    ${SDK_SOURCES}
)

target_include_directories(BotIdentity PRIVATE
    src
    ${CMAKE_BINARY_DIR}/gen
    ${HL2SDK}/thirdparty/protobuf-3.21.8/src
    ${MMS}/core
    ${MMS}/core/sourcehook
    ${MMS}/core/sourcemm
    ${HL2SDK}/public
    ${HL2SDK}/public/engine
    ${HL2SDK}/public/game/server
    ${HL2SDK}/public/game/shared
    ${HL2SDK}/public/mathlib
    ${HL2SDK}/public/tier0
    ${HL2SDK}/public/entity2
    ${HL2SDK}/game/shared
    ${HL2SDK}/common
)

target_compile_definitions(BotIdentity PRIVATE
    META_IS_SOURCE2
    PLATFORM_64BITS
    X64BITS
)

if(WIN32)
    set(BOTIDENT_PLATFORM_DIR win64)
    set(BOTIDENT_SUFFIX ".dll")
    set(HL2SDK_LIBDIR ${HL2SDK}/lib/public/win64)
    target_include_directories(BotIdentity PRIVATE ${HL2SDK}/public/tier1)
    target_link_libraries(BotIdentity PRIVATE
        psapi
        ${HL2SDK_LIBDIR}/tier0.lib
        ${HL2SDK_LIBDIR}/mathlib.lib
        ${HL2SDK_LIBDIR}/interfaces.lib
    )
    if(MSVC)
        target_link_options(BotIdentity PRIVATE /NODEFAULTLIB:LIBCMTD)
        target_compile_options(BotIdentity PRIVATE /W3 /permissive- /utf-8
            /wd4267 /wd4244 /wd4146 /wd4996 /wd4661)
    endif()
    target_compile_definitions(BotIdentity PRIVATE
        _CRT_SECURE_NO_WARNINGS WIN32 _WIN32 _WINDOWS
        COMPILER_MSVC COMPILER_MSVC64 NOMINMAX _ALLOW_KEYWORD_MACROS
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BOTIDENT_PLATFORM_DIR linuxsteamrt64)
    set(BOTIDENT_SUFFIX ".so")
    set(HL2SDK_LIBDIR ${HL2SDK}/lib/linux64)
    target_link_libraries(BotIdentity PRIVATE
        ${HL2SDK_LIBDIR}/libtier0.so
        ${HL2SDK_LIBDIR}/mathlib.a
        ${HL2SDK_LIBDIR}/interfaces.a
        pthread dl
    )
    target_compile_definitions(BotIdentity PRIVATE
        POSIX _LINUX LINUX COMPILER_GCC
        stricmp=strcasecmp _stricmp=strcasecmp
        _strnicmp=strnicmp strnicmp=strnicmp
        _snprintf=snprintf _vsnprintf=vsnprintf
        _alloca=alloca strcmpi=strcasecmp
    )
    target_compile_options(BotIdentity PRIVATE
        -fPIC -fno-rtti -fvisibility=hidden
        -idirafter ${HL2SDK}/public/tier1
        -Wno-non-virtual-dtor -Wno-overloaded-virtual
        -Wno-register -Wno-narrowing -Wno-deprecated-copy
    )
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

set_target_properties(BotIdentity PROPERTIES
    OUTPUT_NAME "BotIdentity"
    PREFIX ""
    SUFFIX "${BOTIDENT_SUFFIX}"
)

# Install
install(TARGETS BotIdentity
    LIBRARY DESTINATION addons/BotIdentity/bin/${BOTIDENT_PLATFORM_DIR}
    RUNTIME DESTINATION addons/BotIdentity/bin/windows
)

install(FILES
    ${CMAKE_SOURCE_DIR}/config.json
    ${CMAKE_SOURCE_DIR}/gamedata.json
    DESTINATION addons/BotIdentity
)
