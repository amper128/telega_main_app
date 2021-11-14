#
# liblog
#

file(GLOB_RECURSE liblog_headers "include/*.h")

add_library(log
	buffer.c
	create.c
	log.c
	print.c
	put_record.c
	${liblog_headers}
	)

target_include_directories(log
	PRIVATE
		include
	PUBLIC
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:usr/include>
	)
