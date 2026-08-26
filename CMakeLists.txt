cmake_minimum_required(VERSION 3.21)

# Uruchamiaj z:
#   cmake -DCMAKE_TOOLCHAIN_FILE=/SDK/share/cmake/arm_conf.cmake ..

project(rsvp C)

if(NOT CMAKE_CROSSCOMPILING)
  message(WARNING "PocketBook toolchain missing. "
                  "Use: -DCMAKE_TOOLCHAIN_FILE=/SDK/share/cmake/arm_conf.cmake")
endif()

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})

# Ścieżki z arm_conf.cmake (SDK)
if(DEFINED PB_INCLUDE_DIRECTORIES)
  include_directories(${PB_INCLUDE_DIRECTORIES})
endif()
if(DEFINED PB_LINK_DIRECTORIES)
  link_directories(${PB_LINK_DIRECTORIES})
endif()

# FreeType — inkview.h robi #include <ft2build.h>
if(DEFINED FREETYPE_INCLUDE_DIRS)
  include_directories(${FREETYPE_INCLUDE_DIRS})
endif()

# Zapasowe ścieżki, gdy zmienne z toolchaina nie zadziałają
set(_PB_SYSROOT "/SDK/usr/arm-obreey-linux-gnueabi/sysroot")
foreach(_inc
    "${_PB_SYSROOT}/usr/include"
    "${_PB_SYSROOT}/usr/include/freetype2"
    "/SDK/usr/include"
    "/SDK/usr/include/freetype2")
  if(EXISTS "${_inc}")
    include_directories("${_inc}")
  endif()
endforeach()

# Lokalny inkview.h + libxml2/libzip z /pb-deps
include_directories(${CMAKE_SOURCE_DIR})

if(DEFINED ENV{PB_DEPS})
  set(PB_DEPS "$ENV{PB_DEPS}")
else()
  set(PB_DEPS "/pb-deps")
endif()

include_directories(${PB_DEPS}/include ${PB_DEPS}/include/libxml2)
link_directories(${PB_DEPS}/lib)

add_executable(rsvp.app main.c)

# Kolejność przy linkowaniu statycznym ma znaczenie
target_link_libraries(rsvp.app
  inkview
  freetype
  zip
  xml2
  z
  pthread
  m
  dl
)
