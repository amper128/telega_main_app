#
# libio - Input/Output library
#

file(GLOB_RECURSE libio_headers "include/*.h")

add_library(io
	canbus.c
	${libwfb_headers}
	)

target_include_directories(io
	PRIVATE
		include
	PUBLIC
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:usr/include>
	)

target_link_libraries(io
	PUBLIC
		svc
	)
