#
# libnetlink
#

file(GLOB_RECURSE libnetlink_headers "include/*.h")

add_library(netlink
	netlink_base.c
	addattr_l.c
	addattr_u32.c
	check_error.c
	link_updown.c
	link_list.c
	${libnetlink_headers}
	)

target_include_directories(netlink
	PRIVATE
		include
	PUBLIC
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:usr/include>
	)
