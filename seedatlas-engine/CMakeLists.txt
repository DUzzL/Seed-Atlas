cmake_minimum_required(VERSION 3.10)
project(seedatlas_engine C)

if (NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# -fwrapv is required: the generators rely on wrapping signed arithmetic.
# -ffp-contract=off keeps the noise rounding identical to Java (no FMA fusing).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -fwrapv -ffp-contract=off")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g3 -DDEBUG")

set(HEADERS
	finders.h
	generator.h
	layers.h
	biomenoise.h
	biomes.h
	noise.h
	rng.h
	util.h
	quadbase.h
)
set(SOURCES
	finders.c
	generator.c
	layers.c
	biomenoise.c
	biomes.c
	noise.c
	util.c
	quadbase.c
)

add_library(objects OBJECT ${SOURCES})
set_property(TARGET objects PROPERTY POSITION_INDEPENDENT_CODE 1)

add_library(seedatlas_engine SHARED $<TARGET_OBJECTS:objects>)
add_library(seedatlas_engine_static STATIC $<TARGET_OBJECTS:objects>)

install(TARGETS seedatlas_engine seedatlas_engine_static DESTINATION lib)
install(FILES ${HEADERS} DESTINATION include)

