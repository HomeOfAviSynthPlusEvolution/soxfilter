cmake_minimum_required (VERSION 3.1)
project (soxfilter)
file(GLOB CODE "src/*.cpp")
add_library(soxfilter SHARED ${CODE})
add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE -D_CRT_SECURE_NO_WARNINGS)
set_property(TARGET soxfilter PROPERTY CXX_STANDARD 17)

include_directories(.)
include_directories(include)

if (CMAKE_GENERATOR_TOOLSET MATCHES "v[0-9]*_xp")
  target_compile_definitions(soxfilter PRIVATE WINVER=0x502 _WIN32_WINNT=0x502)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:threadSafeInit-")
endif()

target_link_libraries(soxfilter libsox lpc10 gsm)
