/*
 * gst-launch-1.0 \
 *	nvarguscamerasrc wbmode=5 tnr-mode=0 ee-mode=0 ! \
 *	'video/x-raw(memory:NVMM), width=800, height=480, format=NV12' ! \
 *	nvvidconv flip-method=2 ! \
 *	queue ! \
 *	videorate ! video/x-raw,framerate=30/1 ! \
 *	omxh265enc bitrate=600000 iframeinterval=60 preset-level=1 control-rate=2 ! \
 *	h265parse ! \
 *	rtph265pay config-interval=1 mtu=1420 pt=96 ! \
 *	rtpulpfecenc percentage=100 pt=122 ! \
 *	udpsink host=192.168.50.100 port=5600 sync=false async=false
 *
 */

#include <gst/gst.h>
#include <private/video.h>
#include <stdbool.h>

#define VIDEO_W (1920)
#define VIDEO_H (1080)
#define VIDEO_FPS (30)
#define BITRATE (8000000)
#define FEC_PERCENT (25)

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
	GstMessage *msg;
	GstStateChangeReturn ret;

	/* Initialize GStreamer */
	gst_init(NULL, NULL);

	/* Create the elements */
	source = gst_element_factory_make("nvarguscamerasrc", "source");
	conv = gst_element_factory_make("nvvidconv", "vidconv");
	encoder_q = gst_element_factory_make("queue", "encoderq");
	rate = gst_element_factory_make("videorate", "videorate");
	// encoder		= gst_element_factory_make ("omxh265enc" , "h265encoder");
	encoder = gst_element_factory_make("nvv4l2h265enc", "h265encoder");
	parser = gst_element_factory_make("h265parse", "parser-h265");
	rtp = gst_element_factory_make("rtph265pay", "rtp");
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
	g_object_set(conv, "flip-method", 2, NULL);
	g_object_set(encoder, "bitrate", BITRATE,
		     /*"iframeinterval", 60, "preset-level", 0,*/ "control-rate", 1,
		     "maxperf-enable", true, "MeasureEncoderLatency", true, NULL);
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

	/* Wait until error or EOS */
	bus = gst_element_get_bus(pipeline);
	msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
					 GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

	/* Parse message */
	if (msg != NULL) {
		GError *err;
		gchar *debug_info;

		switch (GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_ERROR:
			gst_message_parse_error(msg, &err, &debug_info);
			g_printerr("Error received from element %s: %s\n",
				   GST_OBJECT_NAME(msg->src), err->message);
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
	}

	/* Free resources */
	gst_object_unref(bus);
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(pipeline);

	return 0;
}
