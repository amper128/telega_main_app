/**
 * @file video.c
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2020-2023
 * @brief Передача видео с камер
 */

/*
 * gst-launch-1.0 \
 *	nvarguscamerasrc wbmode=5 tnr-mode=0 ee-mode=0 ! \
 *	'video/x-raw(memory:NVMM), width=800, height=480, format=NV12' ! \
 *	nvvidconv flip-method=2 ! \
 *	queue ! \
 *	videorate ! video/x-raw,framerate=30/1 ! \
 *	omxh264enc bitrate=600000 iframeinterval=60 preset-level=1 control-rate=2 ! \
 *	h264parse ! \
 *	rtph264pay config-interval=1 mtu=1420 pt=96 ! \
 *	rtpulpfecenc percentage=100 pt=122 ! \
 *	udpsink host=192.168.50.100 port=5600 sync=false async=false
 *
 */

/* PIP:
 * gst-launch-1.0 \
 *	nvcompositor name=comp \
 *	sink_0::xpos=0 sink_0::ypos=0 sink_0::width=480 sink_0::height=360  \
 *	sink_1::xpos=480 sink_1::ypos=0 sink_1::width=480 sink_1::height=360 ! \
 *	nvvidconv ! 'video/x-raw(memory:NVMM),format=I420' ! \
 *	nvv4l2h264enc bitrate=512000 iframeinterval=60 preset-level=1 ! \
 *	h264parse ! rtph264pay config-interval=1 mtu=1420 pt=96 ! \
 *	udpsink host=192.168.50.100 port=5601 sync=false async=false \
 *
 *	v4l2src device=/dev/video1 ! image/jpeg,width=1024,height=768,framerate=30/1 ! \
 *	jpegparse ! jpegdec ! \
 *	videocrop top=0 left=96 right=96 bottom=0 ! nvvidconv ! \
 *	'video/x-raw(memory:NVMM),format=RGBA' ! comp.sink_0 \
 *
 *	v4l2src device=/dev/video2 ! image/jpeg,width=1024,height=768,framerate=30/1 ! \
 *	jpegparse ! jpegdec ! \
 *	videocrop top=0 left=96 right=96 bottom=0 ! nvvidconv !
 *	'video/x-raw(memory:NVMM),format=RGBA' ! comp.sink_1
 */
/* использование nvjpegdec дает мегафризы c 0.01 фпс, поэтому обычный jpegdec */

#include <gst/gst.h>
#include <stdbool.h>

#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/power.h>
#include <private/video.h>

#define VIDEO_W (1280)
#define VIDEO_H (720)
#define VIDEO_FPS (30)
#define BITRATE (2000000)
#define FEC_PERCENT (10)

#define VIDEO_PIP_W (480)
#define VIDEO_PIP_H (360)
#define VIDEO_PIP_CAP_W (1024)
#define VIDEO_PIP_CAP_H (768)
#define BITRATE_PIP (512000)

#define UDP_PORT_VIDEO (5600)
#define UDP_PORT_VIDEO_PIP (5601)

static shm_t connect_status_shm;
/* локальная копия флага наличия подключения */
static bool m_connected = false;
static struct in_addr sin_addr; /* IP адрес */

static GMainLoop *main_loop; /* GLib's Main Loop */
static GMainLoop *pip_loop;  /* GLib's Main Loop */

static void
check_connect(void)
{
	/* читаем статус подключения */
	connection_state_t *cstate;
	void *p;
	shm_map_read(&connect_status_shm, &p);
	cstate = p;

	if (cstate->connected != m_connected) {
		/* изменилось состояние подключения */
		m_connected = cstate->connected;

		if (m_connected) {
			/* меняем адрес у UDP сокета */
			memcpy(&sin_addr, &cstate->sin_addr, sizeof(sin_addr));
		}
	}
}

static gboolean
g_callback(gpointer data)
{
	check_connect();

	if (!m_connected || !svc_cycle()) {
		g_main_loop_quit((GMainLoop *)data);
	}

	return TRUE;
}

static gboolean
gst_handle_message(GstBus *bus, GstMessage *msg, void *user_data)
{
	GError *err;
	gchar *debug_info;

	(void)bus;
	(void)user_data;

	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_ERROR:
		gst_message_parse_error(msg, &err, &debug_info);
		g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src),
			   err->message);
		g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
		g_clear_error(&err);
		g_free(debug_info);
		break;

	case GST_MESSAGE_EOS:
		g_print("End-Of-Stream reached.\n");
		break;

	default:
		/* We should not reach here because we only asked for ERRORs and EOS */
		g_printerr("Unexpected message received.\n");
		break;
	}

	gst_message_unref(msg);

	return true;
}

static int
video_start(void)
{
	GstElement *pipeline, *source, *caps, *ratec, *udpsink;
	GstBus *bus;
	GstCaps *filtercaps, *ratecaps;
	GstStructure *srcstructure;
	GstCapsFeatures *feat;
	GstElement *conv, *encoder_q, *rate;
	GstElement *encoder;
	GstElement *parser;
	GstElement *rtp, *rtpfec;
	GstStateChangeReturn ret;
	GMainContext *context = NULL;
	GSource *gsource = NULL;

	/* Initialize GStreamer */
	gst_init(NULL, NULL);

	/* Create the elements */
	source = gst_element_factory_make("nvarguscamerasrc", "source");
	conv = gst_element_factory_make("nvvidconv", "vidconv");
	encoder_q = gst_element_factory_make("queue", "encoderq");
	rate = gst_element_factory_make("videorate", "videorate");
	// encoder		= gst_element_factory_make ("omxh264enc" , "h264encoder");
	encoder = gst_element_factory_make("nvv4l2h264enc", "h264encoder");
	parser = gst_element_factory_make("h264parse", "parser-h264");
	rtp = gst_element_factory_make("rtph264pay", "rtp");
	rtpfec = gst_element_factory_make("rtpulpfecenc", "rtpfec");
	udpsink = gst_element_factory_make("udpsink", "destination");

	/* Create the empty pipeline */
	pipeline = gst_pipeline_new("test-pipeline");

	if (!pipeline || !source || !udpsink || !conv || !encoder_q || !rate || !encoder ||
	    !parser || !rtp || !rtpfec) {
		g_printerr("Not all elements could be created.\n");
		return -1;
	}

	caps = gst_element_factory_make("capsfilter", "filter");
	g_assert(caps != NULL); /* should always exist */

	filtercaps = gst_caps_new_empty();

	srcstructure = gst_structure_new("video/x-raw", "format", G_TYPE_STRING, "NV12",
					 "framerate", GST_TYPE_FRACTION, VIDEO_FPS, 1, "width",
					 G_TYPE_INT, VIDEO_W, "height", G_TYPE_INT, VIDEO_H, NULL);
	if (!srcstructure) {
		g_printerr("Unable to create caps.\n");
		gst_object_unref(pipeline);
		gst_caps_unref(filtercaps);
		return -1;
	}

	feat = gst_caps_features_new("memory:NVMM", NULL);
	if (!feat) {
		g_printerr("Unable to create feature.\n");
		gst_object_unref(pipeline);
		gst_caps_unref(filtercaps);
		gst_object_unref(srcstructure);
		return -1;
	}

	gst_caps_append_structure_full(filtercaps, srcstructure, feat);

	g_object_set(G_OBJECT(caps), "caps", filtercaps, NULL);
	gst_caps_unref(filtercaps);

	ratec = gst_element_factory_make("capsfilter", "framerate");
	ratecaps =
	    gst_caps_new_simple("video/x-raw", "framerate", GST_TYPE_FRACTION, VIDEO_FPS, 1, NULL);
	g_object_set(G_OBJECT(ratec), "caps", ratecaps, NULL);
	gst_caps_unref(ratecaps);

	/* получаем значение IP адреса в виде строки */
	char *ip_string = inet_ntoa(sin_addr);
	/* Modify the source's properties */
	g_object_set(source, "ispdigitalgainrange", "1 2", "wbmode", 1, "ee-mode", 0, NULL);
	g_object_set(conv, "flip-method", 0, NULL);
	g_object_set(encoder, "bitrate", BITRATE, "iframeinterval", 60, "preset-level", 3,
		     "control-rate", 0, "maxperf-enable", true, "profile", 2, NULL);
	g_object_set(rtp, "config-interval", 1, "mtu", 1420, "pt", 96, NULL);
	g_object_set(rtpfec, "percentage", FEC_PERCENT, "pt", 122, NULL);
	g_object_set(udpsink, "host", ip_string, "port", UDP_PORT_VIDEO, "sync", false, "async",
		     false, NULL);

	/* Build the pipeline */
	gst_bin_add_many(GST_BIN(pipeline), source, caps, conv,
			 /*encoder_q, */ /*rate, ratec,*/ encoder, parser, rtp, rtpfec, udpsink,
			 NULL);
	if (gst_element_link_many(source, caps, conv, /*encoder_q,*/ /* rate, ratec,*/ encoder,
				  parser, rtp, rtpfec, udpsink, NULL) != TRUE) {
		g_printerr("Elements could not be linked.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	/* Start playing */
	ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
	if (ret == GST_STATE_CHANGE_FAILURE) {
		g_printerr("Unable to set the pipeline to the playing state.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	/* start pipeline */
	bus = gst_element_get_bus(pipeline);
	gst_bus_add_watch(bus, (GstBusFunc)gst_handle_message, NULL);

	/* create g_main_loop */
	context = g_main_context_new();
	main_loop = g_main_loop_new(context, FALSE);

	gsource = g_timeout_source_new(50);
	g_source_set_callback(gsource, g_callback, main_loop, NULL);
	g_source_attach(gsource, context);

	/* run g_main_loop */
	g_main_loop_run(main_loop);

	/* Free resources */
	gst_object_unref(bus);
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(pipeline);

	return 0;
}

static int
video_pip_start(void)
{
	GstElement *pipeline;
	GstElement *source1, *srccaps1;
	GstElement *source2, *srccaps2;
	GstCaps *src_gst_c;
	GstStructure *srcstructure;
	GstElement *jpegparse1, *jpegparse2, *dec1, *dec2;
	GstElement *nvcompositor;
	GstElement *crop1, *crop2;
	GstElement *conv1, *conv2, *convcaps1, *convcaps2;
	GstElement *conv_comp, *convcaps_comp;
	GstCaps *conv_gst_c1, *conv_gst_c2;
	GstElement *encoder;
	GstElement *parser;
	GstElement *rtp, *rtpfec;
	GstElement *udpsink;

	GstBus *bus;

	GstStructure *convstructure1, *convstructure2;
	GstCapsFeatures *convfeat1, *convfeat2;
	GstElement *encoder_q;
	GstStateChangeReturn ret;
	GMainContext *context = NULL;
	GSource *gsource = NULL;

	/* Initialize GStreamer */
	gst_init(NULL, NULL);

	/* Create the elements */
	source1 = gst_element_factory_make("v4l2src", "source1");
	source2 = gst_element_factory_make("v4l2src", "source2");
	jpegparse1 = gst_element_factory_make("jpegparse", "jpegparse1");
	jpegparse2 = gst_element_factory_make("jpegparse", "jpegparse2");
	dec1 = gst_element_factory_make("jpegdec", "decoder1");
	dec2 = gst_element_factory_make("jpegdec", "decoder2");
	crop1 = gst_element_factory_make("videocrop", "videocrop1");
	crop2 = gst_element_factory_make("videocrop", "videocrop2");
	conv1 = gst_element_factory_make("nvvidconv", "vidconv1");
	conv2 = gst_element_factory_make("nvvidconv", "vidconv2");
	conv_comp = gst_element_factory_make("nvvidconv", "vidconv_comp");

	nvcompositor = gst_element_factory_make("nvcompositor", "compositor");

	encoder_q = gst_element_factory_make("queue", "encoderq");
	encoder = gst_element_factory_make("nvv4l2h264enc", "h264encoder");
	parser = gst_element_factory_make("h264parse", "parser-h264");
	rtp = gst_element_factory_make("rtph264pay", "rtp");
	rtpfec = gst_element_factory_make("rtpulpfecenc", "rtpfec");
	udpsink = gst_element_factory_make("udpsink", "destination");

	/* Create the empty pipeline */
	pipeline = gst_pipeline_new("pip-pipeline");

	if (!pipeline || !source1 || !source2 || !jpegparse1 || !jpegparse2 || !dec1 || !dec2 ||
	    !crop1 || !crop2 || !udpsink || !conv1 || !conv2 || !nvcompositor || !conv_comp ||
	    !encoder_q || !encoder || !parser || !rtp || !rtpfec) {
		g_printerr("Not all elements could be created.\n");
		return -1;
	}

	/* v4l2src caps */
	srccaps1 = gst_element_factory_make("capsfilter", "srcfilter1");
	srccaps2 = gst_element_factory_make("capsfilter", "srcfilter2");
	g_assert(srccaps1 != NULL); /* should always exist */
	g_assert(srccaps2 != NULL); /* should always exist */

	src_gst_c = gst_caps_new_empty();

	srcstructure = gst_structure_new("image/jpeg", "framerate", GST_TYPE_FRACTION, VIDEO_FPS, 1,
					 "width", G_TYPE_INT, VIDEO_PIP_CAP_W, "height", G_TYPE_INT,
					 VIDEO_PIP_CAP_H, NULL);
	if (!srcstructure) {
		g_printerr("Unable to create src caps.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	gst_caps_append_structure(src_gst_c, srcstructure);

	g_object_set(G_OBJECT(srccaps1), "caps", src_gst_c, NULL);
	g_object_set(G_OBJECT(srccaps2), "caps", src_gst_c, NULL);
	gst_caps_unref(src_gst_c);

	/* nvvidconv caps */
	convcaps1 = gst_element_factory_make("capsfilter", "conv_filter1");
	convcaps2 = gst_element_factory_make("capsfilter", "conv_filter2");
	g_assert(convcaps1 != NULL); /* should always exist */
	g_assert(convcaps2 != NULL); /* should always exist */

	conv_gst_c1 = gst_caps_new_empty();
	conv_gst_c2 = gst_caps_new_empty();

	convstructure1 = gst_structure_new("video/x-raw", "framerate", GST_TYPE_FRACTION, VIDEO_FPS,
					   1, "format", G_TYPE_STRING, "RGBA", "width", G_TYPE_INT,
					   VIDEO_PIP_W, "height", G_TYPE_INT, VIDEO_PIP_H, NULL);
	convstructure2 = gst_structure_copy(convstructure1);

	if (!convstructure1 || !convstructure2) {
		g_printerr("Unable to create conv caps.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	convfeat1 = gst_caps_features_new("memory:NVMM", NULL);
	convfeat2 = gst_caps_features_copy(convfeat1);
	if (!convfeat1 || !convfeat2) {
		g_printerr("Unable to create conv feature.\n");
		gst_object_unref(pipeline);
		gst_object_unref(convstructure1);
		gst_object_unref(convstructure2);
		return -1;
	}

	gst_caps_append_structure_full(conv_gst_c1, convstructure1, convfeat1);
	gst_caps_append_structure_full(conv_gst_c2, convstructure2, convfeat2);

	g_object_set(G_OBJECT(convcaps1), "caps", conv_gst_c1, NULL);
	g_object_set(G_OBJECT(convcaps2), "caps", conv_gst_c2, NULL);
	gst_caps_unref(conv_gst_c1);
	gst_caps_unref(conv_gst_c2);

	/* videoconvert after compositor */
	convcaps_comp = gst_element_factory_make("capsfilter", "comp_filter");
	g_assert(convcaps_comp != NULL); /* should always exist */
	GstStructure *convstructure_comp;
	GstCaps *filtercaps_comp = gst_caps_new_empty();
	convstructure_comp =
	    gst_structure_new("video/x-raw", "format", G_TYPE_STRING, "NV12", NULL);
	if (!convstructure_comp) {
		g_printerr("Unable to create caps.\n");
		gst_object_unref(pipeline);
		gst_caps_unref(filtercaps_comp);
		return -1;
	}

	GstCapsFeatures *feat_comp = gst_caps_features_new("memory:NVMM", NULL);
	if (!feat_comp) {
		g_printerr("Unable to create feature.\n");
		gst_object_unref(pipeline);
		gst_caps_unref(filtercaps_comp);
		gst_object_unref(convstructure_comp);
		return -1;
	}
	gst_caps_append_structure_full(filtercaps_comp, convstructure_comp, feat_comp);

	g_object_set(G_OBJECT(convcaps_comp), "caps", filtercaps_comp, NULL);
	gst_caps_unref(filtercaps_comp);

	/* получаем значение IP адреса в виде строки */
	char *ip_string = inet_ntoa(sin_addr);
	/* Modify the source's properties */
	g_object_set(source1, "device", "/dev/video1", NULL);
	g_object_set(source2, "device", "/dev/video2", NULL);
	g_object_set(crop1, "top", 0, "left", 96, "right", 96, "bottom", 0, NULL);
	g_object_set(crop2, "top", 0, "left", 96, "right", 96, "bottom", 0, NULL);
	g_object_set(conv1, "flip-method", 4, NULL);
	g_object_set(conv2, "flip-method", 0, NULL);
	g_object_set(encoder, "bitrate", BITRATE_PIP, "iframeinterval", 60, "preset-level", 3,
		     "control-rate", 0, "maxperf-enable", true, "profile", 2, NULL);
	g_object_set(rtp, "config-interval", 1, "mtu", 1420, "pt", 96, NULL);
	g_object_set(rtpfec, "percentage", FEC_PERCENT, "pt", 122, NULL);
	g_object_set(udpsink, "host", ip_string, "port", UDP_PORT_VIDEO_PIP, "sync", false, "async",
		     false, NULL);

	/* Build the pipeline */
	gst_bin_add_many(GST_BIN(pipeline), source1, srccaps1, jpegparse1, dec1, crop1, conv1,
			 convcaps1, NULL);
	gst_bin_add_many(GST_BIN(pipeline), source2, srccaps2, jpegparse2, dec2, crop2, conv2,
			 convcaps2, NULL);
	gst_bin_add_many(GST_BIN(pipeline), nvcompositor, conv_comp, convcaps_comp, encoder, parser,
			 rtp, rtpfec, udpsink, NULL);

	/* linking source1 */
	if (gst_element_link_many(source1, srccaps1, jpegparse1, dec1, crop1, conv1, convcaps1,
				  NULL) != TRUE) {
		g_printerr("source1 could not be linked to conv1.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	/* linking source2 */
	if (gst_element_link_many(source2, srccaps2, jpegparse2, dec2, crop2, conv2, convcaps2,
				  NULL) != TRUE) {
		g_printerr("source2 could not be linked to conv2.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	/* linking compositor with sink */
	if (gst_element_link_many(nvcompositor, conv_comp, convcaps_comp, encoder, parser, rtp,
				  rtpfec, udpsink, NULL) != TRUE) {
		g_printerr("Compositor could not be linked to sink.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	/* connect source1 to compositor */
	GstPad *srcpad1 = gst_element_get_static_pad(convcaps1, "src");
	GstPadTemplate *sink_pad_template1 =
	    gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(nvcompositor), "sink_%u");
	GstPad *sinkpad1 = gst_element_request_pad(nvcompositor, sink_pad_template1, NULL, NULL);

	if (gst_pad_link(srcpad1, sinkpad1) != GST_PAD_LINK_OK) {
		g_printerr("source1 and sink pads could not be linked.\n");
		return -1;
	}
	g_object_set(sinkpad1, "xpos", 0, NULL);
	g_object_set(sinkpad1, "ypos", 0, NULL);
	g_object_set(sinkpad1, "width", VIDEO_PIP_W, NULL);
	g_object_set(sinkpad1, "height", VIDEO_PIP_H, NULL);

	/* connect source2 to compositor */
	GstPad *srcpad2 = gst_element_get_static_pad(convcaps2, "src");
	GstPadTemplate *sink_pad_template2 =
	    gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(nvcompositor), "sink_%u");
	GstPad *sinkpad2 = gst_element_request_pad(nvcompositor, sink_pad_template2, NULL, NULL);

	if (gst_pad_link(srcpad2, sinkpad2) != GST_PAD_LINK_OK) {
		g_printerr("source2 and sink pads could not be linked.\n");
		return -1;
	}
	g_object_set(sinkpad2, "xpos", VIDEO_PIP_W, NULL);
	g_object_set(sinkpad2, "ypos", 0, NULL);
	g_object_set(sinkpad2, "width", VIDEO_PIP_W, NULL);
	g_object_set(sinkpad2, "height", VIDEO_PIP_H, NULL);

	/* Start playing */
	ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
	if (ret == GST_STATE_CHANGE_FAILURE) {
		g_printerr("Unable to set the pipeline to the playing state.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	/* start pipeline */
	bus = gst_element_get_bus(pipeline);
	gst_bus_add_watch(bus, (GstBusFunc)gst_handle_message, NULL);

	/* create g_main_loop */
	context = g_main_context_new();
	pip_loop = g_main_loop_new(context, FALSE);

	gsource = g_timeout_source_new(50);
	g_source_set_callback(gsource, g_callback, pip_loop, NULL);
	g_source_attach(gsource, context);

	/* run g_main_loop */
	g_main_loop_run(pip_loop);

	/* Free resources */
	gst_object_unref(bus);
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(pipeline);

	return 0;
}

int
video_init(void)
{
	/* do nothing */
	return 0;
}

int
video_main(void)
{
	int result = 0;

	do {
		if (!shm_map_open("connect_status", &connect_status_shm)) {
			break;
		}

		while (svc_cycle()) {
			check_connect();

			if (m_connected) {
				result = video_start();
				if (result != 0) {
					break;
				}
			}
		}
	} while (0);

	return result;
}

int
video_pip_main(void)
{
	int result = 0;

	do {
		if (!shm_map_open("connect_status", &connect_status_shm)) {
			break;
		}

		while (svc_cycle()) {
			check_connect();

			if (m_connected) {
				result = video_pip_start();
				if (result != 0) {
					break;
				}
			}
		}
	} while (0);

	return result;
}
