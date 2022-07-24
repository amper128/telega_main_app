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

#include <gst/gst.h>
#include <private/video.h>
#include <stdbool.h>

#include <svc/svc.h>

#define VIDEO_W (1280)
#define VIDEO_H (720)
#define VIDEO_FPS (30)
#define BITRATE (2000000)
#define FEC_PERCENT (50)

static GMainLoop *main_loop; /* GLib's Main Loop */

static gboolean
g_callback(gpointer data)
{
	if (!svc_cycle()) {
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

int
video_init(void)
{
	/* do nothing */
	return 0;
}

int
video_main(void)
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

	/* Modify the source's properties */
	g_object_set(source, "ispdigitalgainrange", "1 2", "wbmode", 1, "ee-mode", 0, NULL);
	g_object_set(conv, "flip-method", 0, NULL);
	g_object_set(encoder, "bitrate", BITRATE, "iframeinterval", 60, "preset-level", 3,
		     "control-rate", 0, "maxperf-enable", true, "profile", 2, NULL);
	g_object_set(rtp, "config-interval", 1, "mtu", 1420, "pt", 96, NULL);
	g_object_set(rtpfec, "percentage", FEC_PERCENT, "pt", 122, NULL);
	g_object_set(udpsink, "host", "192.168.50.100", "port", 5600, "sync", false, "async", false,
		     NULL);

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

	gsource = g_timeout_source_new_seconds(1);
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
