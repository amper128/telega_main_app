#
# sources
#

find_package(PkgConfig)
pkg_search_module(GLIB REQUIRED glib-2.0)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0)

file(GLOB_RECURSE app_headers "include/*.h")

add_executable(${PROJECT_NAME}
	main.c
	motion.c
	video.c
	${app_headers}
	)

target_include_directories(${PROJECT_NAME}
	PRIVATE
		include
		${GLIB2_INCLUDE_DIRS}
		${GSTREAMER_INCLUDE_DIRS}
	)

target_link_libraries(${PROJECT_NAME}
		svc
		log
		io
		netlink
		${GST_APP_LIBRARIES}
		${GST_VIDEO_LIBRARIES}
	)
