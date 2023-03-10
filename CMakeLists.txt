cmake_minimum_required(VERSION 3.10)

project(remote_control C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS YES)

set(OPTIMIZATION_FLAGS
	-O2
	-march=armv8-a+crypto
	-mcpu=cortex-a57+crypto
	)

set(GENERIC_DEFINES
	-D_DEFAULT_SOURCE
	-D_GNU_SOURCE
	-D_POSIX_SOURCE
	)

set(GENERIC_C_FLAGS
	-std=c${CMAKE_C_STANDARD}
	-Wall
	-Wextra
	-Wformat
	-Wformat-security
	-Wformat-y2k
	-Wignored-qualifiers
	-Wimport
	-Winit-self
	-Winvalid-pch
	-Wmissing-braces
	-Wmissing-declarations
	-Wmissing-field-initializers
	-Wmissing-noreturn
	-Wno-attributes
	-Wno-inline
	-Wno-long-long
	-Wno-missing-field-initializers
	-Wno-redundant-decls
	-Wno-variadic-macros
	-Wparentheses
	-Wpointer-arith
	-Wreturn-type
	-Wsequence-point
	-Wshadow
	-Wsign-compare
	-Wsign-conversion
	-Wswitch
	-Wswitch-default
	#-Wswitch-enum
	-Wtrigraphs
	-Wtype-limits
	-Wundef
	-Wuninitialized
	-Wunknown-pragmas
	-Wunreachable-code
	-Wunused
	-Wunused-function
	-Wunused-label
	-Wunused-parameter
	-Wunused-value
	-Wunused-variable
	-Wvolatile-register-var
	-Wwrite-strings
	-Werror
	)

set(COMPILER_FLAGS
	${GENERIC_DEFINES}
	${OPTIMIZATION_FLAGS}
	${GENERIC_C_FLAGS}
	)

string (REPLACE ";" " " COMPILER_FLAGS "${COMPILER_FLAGS}")
set(CMAKE_C_FLAGS ${COMPILER_FLAGS})

add_subdirectory(src)
