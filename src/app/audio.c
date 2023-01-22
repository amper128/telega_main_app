/*
 * gst-launch-1.0 \
 *	alsasrc provide-clock=true buffer-time=8000 latency-time=8000 do-timestamp=true ! \
 *	audioconvert ! audioresample ! \
 *	queue ! \
 *	opusenc ! opusparse ! \
 *	rtpopuspay mtu=1420 pt=96 ! \
 *	"application/x-rtp,payload=(int)96" ! \
 *	udpsink host=192.168.50.100 port=5610 sync=false async=false
 *
 */

#include <arpa/inet.h>
#include <gst/gst.h>
#include <stdbool.h>

#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/audio.h>
#include <private/power.h>

#define BITRATE (64000)
#define FEC_PERCENT (50)

#define UDP_PORT_AUDIO (5610)

static shm_t connect_status_shm;
/* локальная копия флага наличия подключения */
static bool m_connected = false;
static struct in_addr sin_addr; /* IP адрес */

static GMainLoop *main_loop; /* GLib's Main Loop */

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
audio_start(void)
{
	GstElement *pipeline, *source, *udpsink;
	GstBus *bus;
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
	/* alsasrc provide-clock=true buffer-time=8000 latency-time=8000 do-timestamp=true */
	source = gst_element_factory_make("alsasrc", "source");
	/* audioconvert */
	conv = gst_element_factory_make("audioconvert", "audconv");
	/* audioresample */
	resample = gst_element_factory_make("audioresample", "audresmaple");
	/* queue */
	encoder_q = gst_element_factory_make("queue", "encoderq");
	/* opusenc */
	encoder = gst_element_factory_make("opusenc", "opusencoder");
	/* opusparse */
	parser = gst_element_factory_make("opusparse", "parser-opus");
	/* rtpopuspay mtu=1420 pt=96 */
	rtp = gst_element_factory_make("rtpopuspay", "rtp");
	rtpfec = gst_element_factory_make("rtpulpfecenc", "rtpfec");
	/* udpsink host=192.168.50.100 port=5610 sync=false async=false*/
	udpsink = gst_element_factory_make("udpsink", "destination");

	/* Create the empty pipeline */
	pipeline = gst_pipeline_new("test-pipeline");

	if (!pipeline || !source || !udpsink || !conv || !encoder_q || !resample || !encoder ||
	    !parser || !rtp || !rtpfec) {
		g_printerr("Not all elements could be created.\n");
		return -1;
	}

	/* Modify the source's properties */
	g_object_set(source, "provide-clock", true, "buffer-time", 1000, "latency-time", 1000,
		     "do-timestamp", true, NULL);

	/* получаем значение IP адреса в виде строки */
	char *ip_string = inet_ntoa(sin_addr);
	g_object_set(encoder, "bitrate", BITRATE, NULL);
	g_object_set(rtp, "mtu", 1420, "pt", 96, NULL);
	g_object_set(rtpfec, "percentage", FEC_PERCENT, "pt", 122, NULL);
	g_object_set(udpsink, "host", ip_string, "port", UDP_PORT_AUDIO, "sync", false, "async",
		     false, NULL);

	/* Build the pipeline */
	gst_bin_add_many(GST_BIN(pipeline), source, conv, resample, encoder_q, encoder, parser, rtp,
			 rtpfec, udpsink, NULL);
	if (gst_element_link_many(source, conv, resample, encoder_q, encoder, parser, rtp, rtpfec,
				  udpsink, NULL) != TRUE) {
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

int
audio_init(void)
{
	/* do nothing */
	return 0;
}

int
audio_main(void)
{
	int result = 0;

	while (svc_cycle()) {
		check_connect();

		if (m_connected) {
			result = audio_start();
			if (result != 0) {
				break;
			}
		}
	}

	return result;
}
