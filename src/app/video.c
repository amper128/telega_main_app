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
 *	v4l2src device=/dev/video1 do-timestamp=true io-mode=2 ! \
 *	image/jpeg,width=1920,height=1080,framerate=30/1 ! \
 *	jpegparse ! nvjpegdec ! 'video/x-raw' ! \
 *	videocrop top=128 left=160 right=160 bottom=128 ! \
 *	nvvidconv flip-method=4 ! \
 *	'video/x-raw(memory:NVMM),format=I420,width=800,height=480' ! \
 *	nvv4l2h264enc bitrate=512000 iframeinterval=60 preset-level=1 ! \
 *	h264parse ! \
 *	rtph264pay config-interval=1 mtu=1420 pt=96 ! \
 *	udpsink host=192.168.50.100 port=5601 sync=false async=false
 */

#include <gst/gst.h>
#include <private/video.h>
#include <stdbool.h>

#include <svc/svc.h>

#define VIDEO_W (1280)
#define VIDEO_H (720)
#define VIDEO_FPS (30)
#define BITRATE (2000000)
#define FEC_PERCENT (10)

#define VIDEO_PIP_W (800)
#define VIDEO_PIP_H (480)
#define BITRATE_PIP (512000)

static GMainLoop *main_loop; /* GLib's Main Loop */
static GMainLoop *pip_loop;  /* GLib's Main Loop */

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

int
video_pip_main(void)
{
	GstElement *pipeline;
	GstElement *source, *srccaps;
	GstCaps *src_gst_c;
	GstStructure *srcstructure;
	GstElement *jpegparse, *dec, *deccaps;
	GstCaps *dec_gst_c;
	GstElement *crop;
	GstElement *conv, *convcaps;
	GstCaps *conv_gst_c;
	GstElement *encoder;
	GstElement *parser;
	GstElement *rtp, *rtpfec;
	GstElement *udpsink;

	GstBus *bus;

	GstStructure *convstructure;
	GstCapsFeatures *convfeat;
	GstElement *encoder_q;
	GstStateChangeReturn ret;
	GMainContext *context = NULL;
	GSource *gsource = NULL;

	/* Initialize GStreamer */
	gst_init(NULL, NULL);

	/* Create the elements */
	source = gst_element_factory_make("v4l2src", "source");
	jpegparse = gst_element_factory_make("jpegparse", "jpegparse");
	dec = gst_element_factory_make("nvjpegdec", "decoder");
	crop = gst_element_factory_make("videocrop", "videocrop");
	conv = gst_element_factory_make("nvvidconv", "vidconv");
	encoder_q = gst_element_factory_make("queue", "encoderq");
	encoder = gst_element_factory_make("nvv4l2h264enc", "h264encoder");
	parser = gst_element_factory_make("h264parse", "parser-h264");
	rtp = gst_element_factory_make("rtph264pay", "rtp");
	rtpfec = gst_element_factory_make("rtpulpfecenc", "rtpfec");
	udpsink = gst_element_factory_make("udpsink", "destination");

	/* Create the empty pipeline */
	pipeline = gst_pipeline_new("pip-pipeline");

	if (!pipeline || !source || !jpegparse || !dec || !crop || !udpsink || !conv ||
	    !encoder_q || !encoder || !parser || !rtp || !rtpfec) {
		g_printerr("Not all elements could be created.\n");
		return -1;
	}

	/* v4l2src caps */
	srccaps = gst_element_factory_make("capsfilter", "filter");
	g_assert(srccaps != NULL); /* should always exist */

	src_gst_c = gst_caps_new_empty();

	srcstructure =
	    gst_structure_new("image/jpeg", "framerate", GST_TYPE_FRACTION, VIDEO_FPS, 1, "width",
			      G_TYPE_INT, 1920, "height", G_TYPE_INT, 1080, NULL);
	if (!srcstructure) {
		g_printerr("Unable to create src caps.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	gst_caps_append_structure(src_gst_c, srcstructure);

	g_object_set(G_OBJECT(srccaps), "caps", src_gst_c, NULL);
	gst_caps_unref(src_gst_c);

	/* nvjpegdec caps */
	deccaps = gst_element_factory_make("capsfilter", "decodercaps");
	dec_gst_c = gst_caps_new_empty_simple("video/x-raw");
	g_object_set(G_OBJECT(deccaps), "caps", dec_gst_c, NULL);
	gst_caps_unref(dec_gst_c);

	/* nvvidconv caps */
	convcaps = gst_element_factory_make("capsfilter", "conv_filter");
	g_assert(convcaps != NULL); /* should always exist */

	conv_gst_c = gst_caps_new_empty();

	convstructure = gst_structure_new("video/x-raw", "framerate", GST_TYPE_FRACTION, VIDEO_FPS,
					  1, "format", G_TYPE_STRING, "I420", "width", G_TYPE_INT,
					  VIDEO_PIP_W, "height", G_TYPE_INT, VIDEO_PIP_H, NULL);
	if (!convstructure) {
		g_printerr("Unable to create conv caps.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	convfeat = gst_caps_features_new("memory:NVMM", NULL);
	if (!convfeat) {
		g_printerr("Unable to create conv feature.\n");
		gst_object_unref(pipeline);
		gst_object_unref(convstructure);
		return -1;
	}

	gst_caps_append_structure_full(conv_gst_c, convstructure, convfeat);

	g_object_set(G_OBJECT(convcaps), "caps", conv_gst_c, NULL);
	gst_caps_unref(conv_gst_c);

	/* Modify the source's properties */
	g_object_set(source, "device", "/dev/video1", "do-timestamp", true, "io-mode", 2, NULL);
	g_object_set(crop, "top", 128, "left", 160, "right", 160, "bottom", 128, NULL);
	g_object_set(conv, "flip-method", 4, NULL);
	g_object_set(encoder, "bitrate", BITRATE_PIP, "iframeinterval", 60, "preset-level", 3,
		     "control-rate", 0, "maxperf-enable", true, "profile", 2, NULL);
	g_object_set(rtp, "config-interval", 1, "mtu", 1420, "pt", 96, NULL);
	g_object_set(rtpfec, "percentage", FEC_PERCENT, "pt", 122, NULL);
	g_object_set(udpsink, "host", "192.168.50.100", "port", 5601, "sync", false, "async", false,
		     NULL);

	/* Build the pipeline */
	gst_bin_add_many(GST_BIN(pipeline), source, srccaps, jpegparse, dec, deccaps, crop, conv,
			 convcaps, encoder, parser, rtp, rtpfec, udpsink, NULL);
	if (gst_element_link_many(source, srccaps, jpegparse, dec, deccaps, crop, conv, convcaps,
				  encoder, parser, rtp, udpsink, NULL) != TRUE) {
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
	pip_loop = g_main_loop_new(context, FALSE);

	gsource = g_timeout_source_new_seconds(1);
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
