#
# sources
#

find_package(PkgConfig)
pkg_search_module(GLIB REQUIRED glib-2.0)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0)

pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(DBUS REQUIRED dbus-glib-1)

pkg_check_modules(GIO REQUIRED gio-2.0)

pkg_search_module(OPUS REQUIRED opus)
pkg_check_modules(PULSE REQUIRED libpulse)
pkg_check_modules(PULSE_SIMPLE REQUIRED libpulse-simple)

file(GLOB_RECURSE app_headers "include/*.h")
file(GLOB_RECURSE global_headers "${PROJECT_SOURCE_DIR}/include/*.h")

add_executable(${PROJECT_NAME}
	audio_stream.c
	gps.c
	main.c
	minmea.c
	motion.c
	network_status.c
	power.c
	system_telemetry.c
	telemetry.c
	video.c
	voicestream.c
	${app_headers}
	${global_headers}
	)

target_include_directories(${PROJECT_NAME}
	PRIVATE
		include
		${GLIB2_INCLUDE_DIRS}
		${GSTREAMER_INCLUDE_DIRS}
		${DBUS_INCLUDE_DIRS}
		${OPUS_INCLUDE_DIRS}
		${PULSE_INCLUDE_DIRS}
		${PULSE_SIMPLE_INCLUDE_DIRS}
	)

target_link_libraries(${PROJECT_NAME}
		svc
		log
		io
		netlink
		${GST_APP_LIBRARIES}
		${GST_VIDEO_LIBRARIES}
		${DBUS_LIBRARIES}
		${GIO_LIBRARIES}
		mp3lame
		${OPUS_LIBRARIES}
		${PULSE_LIBRARIES}
		${PULSE_SIMPLE_LIBRARIES}
	)
