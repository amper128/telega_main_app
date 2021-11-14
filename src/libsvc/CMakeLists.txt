#
# libsvc
#

file(GLOB_RECURSE libsvc_headers "include/*.h")

add_library(svc
	sharedmem.c
	svc.c
	timerfd.c
	${libsvc_headers}
	)

target_include_directories(svc
	PRIVATE
		include
	PUBLIC
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:usr/include>
	)

target_link_libraries(svc
	PUBLIC
		-lrt
	)
