/*
 * gst-launch-1.0 \
 *	alsasrc provide-clock=true buffer-time=8000 latency-time=8000 \
 *	actual-latency-time=1000 actual-buffer-time=8000 do-timestamp=true ! \
 *	audioconvert ! audioresample ! \
 *	queue ! \
 *	opusenc ! opusparse ! \
 *	rtpopuspay mtu=1420 pt=96 ! \
 *	"application/x-rtp,payload=(int)96" ! \
 *	udpsink host=192.168.50.100 port=5610 sync=false async=false
 *
 */

#include <gst/gst.h>
#include <private/audio.h>
#include <stdbool.h>

#include <svc/svc.h>

#define BITRATE (64000)
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
audio_init(void)
{
	/* do nothing */
	return 0;
}

int
audio_main(void)
{
	GstElement *pipeline, *source, *caps, *udpsink;
	GstBus *bus;
	GstCaps *filtercaps;
	GstStructure *srcstructure;
	GstCapsFeatures *feat;
	GstElement *conv, *resample, *encoder_q;
	GstElement *encoder;
	GstElement *parser;
	GstElement *rtp, *rtpfec;
	GstStateChangeReturn ret;
	GMainContext *context = NULL;
	GSource *gsource = NULL;

	/* Initialize GStreamer */
	gst_init(NULL, NULL);

	/* Create the elements */
	source = gst_element_factory_make("alsasrc", "source");
	conv = gst_element_factory_make("audioconvert", "audconv");
	resample = gst_element_factory_make("audioresample", "audresmaple");
	encoder_q = gst_element_factory_make("queue", "encoderq");
	encoder = gst_element_factory_make("opusenc", "opusencoder");
	parser = gst_element_factory_make("opusparse", "parser-opus");
	rtp = gst_element_factory_make("rtpopuspay", "rtp");
	rtpfec = gst_element_factory_make("rtpulpfecenc", "rtpfec");
	udpsink = gst_element_factory_make("udpsink", "destination");

	/* Create the empty pipeline */
	pipeline = gst_pipeline_new("test-pipeline");

	if (!pipeline || !source || !udpsink || !conv || !encoder_q || !resample || !encoder ||
	    !parser || !rtp || !rtpfec) {
		g_printerr("Not all elements could be created.\n");
		return -1;
	}

	caps = gst_element_factory_make("capsfilter", "filter");
	g_assert(caps != NULL); /* should always exist */

	filtercaps = gst_caps_new_empty();

	srcstructure = gst_structure_new("audio/x-raw", "channels", G_TYPE_INT, 1, NULL);
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

	/* Modify the source's properties */
	g_object_set(source, "provide-clock", true, "buffer-time", 1000, "latency-time", 1000,
		     "actual-latency-time", 1000, "actual-buffer-time", 1000, "do-timestamp", true,
		     NULL);

	g_object_set(encoder, "bitrate", BITRATE, NULL);
	g_object_set(rtp, "config-interval", 1, "mtu", 1420, "pt", 96, NULL);
	g_object_set(rtpfec, "percentage", FEC_PERCENT, "pt", 122, NULL);
	g_object_set(udpsink, "host", "192.168.50.100", "port", 5610, "sync", false, "async", false,
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