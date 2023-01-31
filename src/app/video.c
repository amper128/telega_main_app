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
#define BITRATE (3000000)
#define FEC_PERCENT (25)

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

enum usb_cam_type_t { CAMERA_FRONT, CAMERA_BACK };

struct usb_cam_src_data_t {
	GstElement *source;
	GstElement *source_capsfilter;
	GstElement *jpegparse;
	GstElement *jpegdec;
	GstElement *crop;
	GstElement *vidconv;
	GstElement *vidconv_capsfilter;
};

/* v4l2src device=%s ! image/jpeg,width=%u,height=%u,framerate=30/1 ! \
 * jpegparse ! jpegdec ! \
 * videocrop top=0 left=96 right=96 bottom=0 ! nvvidconv flip=%u ! \
 * 'video/x-raw(memory:NVMM),format=RGBA'
 */

static int
make_usb_cam_source_stream(struct usb_cam_src_data_t *cdata, enum usb_cam_type_t ctype,
			   const char devname[])
{
	static const struct {
		const gchar *srcname;
		const gchar *capsname;
		const gchar *jpegparsename;
		const gchar *jpegdecname;
		const gchar *cropname;
		const gchar *vidconvname;
		const gchar *vidconvcapsname;
	} names[] = {[CAMERA_FRONT] = {.srcname = "source_front",
				       .capsname = "capsfilter_front",
				       .jpegparsename = "jpegparse_front",
				       .jpegdecname = "jpegdec_front",
				       .cropname = "videocrop_front",
				       .vidconvname = "vidconv_front",
				       .vidconvcapsname = "vidconvcapsfilter_front"},
		     [CAMERA_BACK] = {.srcname = "source_back",
				      .capsname = "capsfilter_back",
				      .jpegparsename = "jpegparse_back",
				      .jpegdecname = "jpegdec_back",
				      .cropname = "videocrop_back",
				      .vidconvname = "vidconv_back",
				      .vidconvcapsname = "vidconvcapsfilter_back"}};

	cdata->source = gst_element_factory_make("v4l2src", names[ctype].srcname);
	if (!cdata->source) {
		g_printerr("Cannot create v4l2src.\n");
		return -1;
	}

	cdata->source_capsfilter = gst_element_factory_make("capsfilter", names[ctype].capsname);
	if (!cdata->source) {
		g_printerr("Cannot create capsfilter.\n");
		gst_object_unref(cdata->source);
		return -1;
	}

	GstCaps *gstcaps = gst_caps_new_empty();
	if (!gstcaps) {
		g_printerr("Cannot create gstcaps.\n");
		gst_object_unref(cdata->source);
		gst_object_unref(cdata->source_capsfilter);
		return -1;
	}

	GstStructure *srcstructure;
	srcstructure = gst_structure_new("image/jpeg", "framerate", GST_TYPE_FRACTION, VIDEO_FPS, 1,
					 "width", G_TYPE_INT, VIDEO_PIP_CAP_W, "height", G_TYPE_INT,
					 VIDEO_PIP_CAP_H, NULL);
	if (!srcstructure) {
		g_printerr("Cannot create srcstructure.\n");
		gst_object_unref(cdata->source);
		gst_object_unref(cdata->source_capsfilter);
		gst_caps_unref(gstcaps);
		return -1;
	}

	cdata->jpegparse = gst_element_factory_make("jpegparse", names[ctype].jpegparsename);
	if (!cdata->jpegparse) {
		g_printerr("Cannot create jpegparse.\n");
		gst_object_unref(cdata->source);
		gst_object_unref(cdata->source_capsfilter);
		gst_caps_unref(gstcaps);
		gst_structure_free(srcstructure);
		return -1;
	}

	cdata->jpegdec = gst_element_factory_make("jpegdec", names[ctype].jpegdecname);
	if (!cdata->jpegdec) {
		g_printerr("Cannot create jpegdec.\n");
		gst_object_unref(cdata->source);
		gst_object_unref(cdata->source_capsfilter);
		gst_caps_unref(gstcaps);
		gst_structure_free(srcstructure);
		gst_object_unref(cdata->jpegparse);
		return -1;
	}

	cdata->crop = gst_element_factory_make("videocrop", names[ctype].cropname);
	if (!cdata->crop) {
		g_printerr("Cannot create videocrop.\n");
		gst_object_unref(cdata->source);
		gst_object_unref(cdata->source_capsfilter);
		gst_caps_unref(gstcaps);
		gst_structure_free(srcstructure);
		gst_object_unref(cdata->jpegparse);
		gst_object_unref(cdata->jpegdec);
		return -1;
	}

	cdata->vidconv = gst_element_factory_make("nvvidconv", names[ctype].vidconvname);
	if (!cdata->vidconv) {
		g_printerr("Cannot create nvvidconv.\n");
		gst_object_unref(cdata->source);
		gst_object_unref(cdata->source_capsfilter);
		gst_caps_unref(gstcaps);
		gst_structure_free(srcstructure);
		gst_object_unref(cdata->jpegparse);
		gst_object_unref(cdata->jpegdec);
		gst_object_unref(cdata->crop);
		return -1;
	}

	GstStructure *vidconv_structure;
	/*vidconv_structure = gst_structure_new(
	    "video/x-raw", "framerate", GST_TYPE_FRACTION, VIDEO_FPS, 1, "format", G_TYPE_STRING,
	    "RGBA", "width", G_TYPE_INT, VIDEO_PIP_W, "height", G_TYPE_INT, VIDEO_PIP_H, NULL);*/
	vidconv_structure = gst_structure_new("video/x-raw", "format", G_TYPE_STRING, "RGBA", NULL);
	if (!vidconv_structure) {
		g_printerr("Cannot create vidconv_structure.\n");
		gst_object_unref(cdata->source);
		gst_object_unref(cdata->source_capsfilter);
		gst_caps_unref(gstcaps);
		gst_structure_free(srcstructure);
		gst_object_unref(cdata->jpegparse);
		gst_object_unref(cdata->jpegdec);
		gst_object_unref(cdata->crop);
		gst_object_unref(cdata->vidconv);
	}

	GstCapsFeatures *vidconv_features;
	vidconv_features = gst_caps_features_new("memory:NVMM", NULL);
	if (!vidconv_features) {
		g_printerr("Cannot create vidconv_features.\n");
		gst_object_unref(cdata->source);
		gst_object_unref(cdata->source_capsfilter);
		gst_caps_unref(gstcaps);
		gst_structure_free(srcstructure);
		gst_object_unref(cdata->jpegparse);
		gst_object_unref(cdata->jpegdec);
		gst_object_unref(cdata->crop);
		gst_object_unref(cdata->vidconv);
		gst_structure_free(vidconv_structure);
	}

	cdata->vidconv_capsfilter =
	    gst_element_factory_make("capsfilter", names[ctype].vidconvcapsname);
	if (!cdata->vidconv_capsfilter) {
		g_printerr("Cannot create vidconv_capsfilter.\n");
		gst_object_unref(cdata->source);
		gst_object_unref(cdata->source_capsfilter);
		gst_caps_unref(gstcaps);
		gst_structure_free(srcstructure);
		gst_object_unref(cdata->jpegparse);
		gst_object_unref(cdata->jpegdec);
		gst_object_unref(cdata->crop);
		gst_object_unref(cdata->vidconv);
		gst_structure_free(vidconv_structure);
		gst_caps_features_free(vidconv_features);
		return -1;
	}

	/* set 'device=%s' to v4l2src */
	g_object_set(cdata->source, "device", devname, NULL);

	/* add 'image/jpeg,width=%u,height=%u,framerate=30/1' to v4l2src */
	gst_caps_append_structure(gstcaps, srcstructure);
	g_object_set(G_OBJECT(cdata->source_capsfilter), "caps", gstcaps, NULL);
	gst_caps_unref(gstcaps);

	/* set videocrop params */
	g_object_set(cdata->crop, "top", 0, NULL);
	g_object_set(cdata->crop, "left", 96, NULL);
	g_object_set(cdata->crop, "right", 96, NULL);
	g_object_set(cdata->crop, "bottom", 0, NULL);

	/* set flipmode in vidconv */
	int32_t flipmode = 0;
	if (ctype == CAMERA_BACK) {
		/* horizontal mirror */
		flipmode = 4;
	}
	g_object_set(cdata->vidconv, "flip-method", flipmode, NULL);

	/* add 'memory:NVMM' to 'video/x-raw' */
	gst_caps_append_structure_full(gstcaps, vidconv_structure, vidconv_features);

	/* apply 'video/x-raw' caps */
	g_object_set(cdata->vidconv_capsfilter, "caps", gstcaps, NULL);

	/* cleanup */
	gst_caps_unref(gstcaps);

	return 0;
}

static void
cleanup_usb_cam_source_stream(struct usb_cam_src_data_t *cdata)
{
	log_dbg("cleanup cdata->source");
	gst_object_unref(cdata->source);
	log_dbg("cleanup cdata->source_capsfilter");
	gst_object_unref(cdata->source_capsfilter);
	log_dbg("cleanup cdata->jpegparse");
	gst_object_unref(cdata->jpegparse);
	log_dbg("cleanup cdata->jpegdec");
	gst_object_unref(cdata->jpegdec);
	log_dbg("cleanup cdata->crop");
	gst_object_unref(cdata->crop);
	log_dbg("cleanup cdata->vidconv");
	gst_object_unref(cdata->vidconv);
	log_dbg("cleanup cdata->vidconv_capsfilter");
	gst_object_unref(cdata->vidconv_capsfilter);
}

struct compositor_data_t {
	GstElement *nvcompositor;
	GstElement *nvvidconv;
	GstElement *nvvidconv_caps;
	GstStructure *convstructure;
};

/*
 * nvcompositor name=comp \
 *	sink_0::xpos=0 sink_0::ypos=0 sink_0::width=480 sink_0::height=360  \
 *	sink_1::xpos=480 sink_1::ypos=0 sink_1::width=480 sink_1::height=360 ! \
 *	nvvidconv ! 'video/x-raw(memory:NVMM),format=NV12'
 */
static int
make_compositor_stream(struct compositor_data_t *cdata)
{
	cdata->nvcompositor = gst_element_factory_make("nvcompositor", "compositor");
	if (!cdata->nvcompositor) {
		g_printerr("Cannot create nvcompositor.\n");
		return -1;
	}

	cdata->nvvidconv = gst_element_factory_make("nvvidconv", "compositor_vidconv");
	if (!cdata->nvvidconv) {
		g_printerr("Cannot create compositor_vidconv.\n");
		gst_object_unref(cdata->nvcompositor);
		return -1;
	}

	cdata->nvvidconv_caps = gst_element_factory_make("capsfilter", "compositor_capsfilter");
	if (!cdata->nvvidconv) {
		g_printerr("Cannot create compositor_capsfilter.\n");
		gst_object_unref(cdata->nvcompositor);
		gst_object_unref(cdata->nvvidconv);
		return -1;
	}

	GstCaps *filtercaps = gst_caps_new_empty();
	if (!filtercaps) {
		g_printerr("Cannot create compositor_capsfilter.\n");
		gst_object_unref(cdata->nvcompositor);
		gst_object_unref(cdata->nvvidconv);
		gst_object_unref(cdata->nvvidconv_caps);
		return -1;
	}

	cdata->convstructure =
	    gst_structure_new("video/x-raw", "format", G_TYPE_STRING, "NV12", NULL);
	if (!cdata->convstructure) {
		g_printerr("Unable to create caps.\n");
		gst_object_unref(cdata->nvcompositor);
		gst_object_unref(cdata->nvvidconv);
		gst_object_unref(cdata->nvvidconv_caps);
		gst_caps_unref(filtercaps);
		return -1;
	}

	GstCapsFeatures *feat_comp = gst_caps_features_new("memory:NVMM", NULL);
	if (!feat_comp) {
		g_printerr("Unable to create feature.\n");
		gst_object_unref(cdata->nvcompositor);
		gst_object_unref(cdata->nvvidconv);
		gst_object_unref(cdata->nvvidconv_caps);
		gst_caps_unref(filtercaps);
		gst_structure_free(cdata->convstructure);
		return -1;
	}
	gst_caps_append_structure_full(filtercaps, cdata->convstructure, feat_comp);

	g_object_set(G_OBJECT(cdata->nvvidconv_caps), "caps", filtercaps, NULL);

	/* cleanup */
	gst_caps_unref(filtercaps);
	// gst_structure_free(cdata->convstructure);

	return 0;
}

enum encoder_type_t { ENCODER_H264, ENCODER_H265 };

struct encoder_data_t {
	GstElement *encoder;
	GstElement *parser;
	GstElement *rtppay;
	GstElement *rtpfec;
};

/*
 * nvv4l2h264enc bitrate=%u iframeinterval=60 preset-level=1 ! \
 * h264parse ! rtph264pay config-interval=1 mtu=1420 pt=96 ! \
 * rtpulpfecenc percentage=%u pt=122
 */
static int
make_encoder(struct encoder_data_t *edata, enum encoder_type_t etype, uint32_t bitrate,
	     uint32_t fecpercentage)
{
	switch (etype) {
	case ENCODER_H265:
		edata->encoder = gst_element_factory_make("nvv4l2h265enc", "h265encoder");
		break;
	case ENCODER_H264:
	default:
		edata->encoder = gst_element_factory_make("nvv4l2h264enc", "h264encoder");
		break;
	}

	if (!edata->encoder) {
		g_printerr("Cannot create encoder.\n");
		return -1;
	}

	switch (etype) {
	case ENCODER_H265:
		edata->parser = gst_element_factory_make("h265parse", "h265parser");
		break;
	case ENCODER_H264:
	default:
		edata->parser = gst_element_factory_make("h264parse", "h264parser");
		break;
	}

	if (!edata->parser) {
		g_printerr("Cannot create parser.\n");
		gst_object_unref(edata->encoder);
		return -1;
	}

	switch (etype) {
	case ENCODER_H265:
		edata->rtppay = gst_element_factory_make("rtph265pay", "rtppay");
		break;
	case ENCODER_H264:
	default:
		edata->rtppay = gst_element_factory_make("rtph264pay", "rtppay");
		break;
	}

	if (!edata->rtppay) {
		g_printerr("Cannot create rtppay.\n");
		gst_object_unref(edata->encoder);
		gst_object_unref(edata->parser);
		return -1;
	}

	edata->rtpfec = gst_element_factory_make("rtpulpfecenc", "rtpfec");
	if (!edata->rtpfec) {
		g_printerr("Cannot create rtppay.\n");
		gst_object_unref(edata->encoder);
		gst_object_unref(edata->parser);
		gst_object_unref(edata->rtppay);
		return -1;
	}

	g_object_set(edata->encoder, "bitrate", bitrate, "iframeinterval", 60, "preset-level", 3,
		     "control-rate", 0, "maxperf-enable", true, "profile", 2, NULL);

	g_object_set(edata->rtppay, "config-interval", 1, "mtu", 1420, "pt", 96, NULL);

	g_object_set(edata->rtpfec, "percentage", fecpercentage, "pt", 122, NULL);

	return 0;
}

static void
cleanup_encoder(struct encoder_data_t *edata)
{
	gst_object_unref(edata->encoder);
	gst_object_unref(edata->parser);
	gst_object_unref(edata->rtppay);
	gst_object_unref(edata->rtpfec);
}

struct udpsink_data_t {
	GstElement *udpsink;
};

/*
 * udpsink host=%s port=%u sync=false async=false
 */
static int
make_udp_sink(struct udpsink_data_t *udata, uint32_t udp_port)
{
	udata->udpsink = gst_element_factory_make("udpsink", "destination");
	if (!udata->udpsink) {
		g_printerr("Cannot create udpsink.\n");
		return -1;
	}

	/* получаем значение IP адреса в виде строки */
	/* указатель на буфер переиспользуется потом, удалять не надо */
	char *ip_string = inet_ntoa(sin_addr);

	g_object_set(udata->udpsink, "host", ip_string, NULL);
	g_object_set(udata->udpsink, "port", udp_port, NULL);
	g_object_set(udata->udpsink, "sync", false, NULL);
	g_object_set(udata->udpsink, "async", false, NULL);

	return 0;
}

static void
cleanup_udp_sink(struct udpsink_data_t *udata)
{
	gst_object_unref(udata->udpsink);
}

static int
connect_to_compositor(GstElement *element, struct compositor_data_t *compositor, uint32_t xpos)
{
	GstPad *srcpad = gst_element_get_static_pad(element, "src");
	GstPadTemplate *sink_pad_template = gst_element_class_get_pad_template(
	    GST_ELEMENT_GET_CLASS(compositor->nvcompositor), "sink_%u");
	GstPad *sinkpad =
	    gst_element_request_pad(compositor->nvcompositor, sink_pad_template, NULL, NULL);

	if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
		g_printerr("source and sink pads could not be linked.\n");
		return -1;
	}
	g_object_set(sinkpad, "xpos", xpos, NULL);
	g_object_set(sinkpad, "ypos", 0, NULL);
	g_object_set(sinkpad, "width", VIDEO_PIP_W, NULL);
	g_object_set(sinkpad, "height", VIDEO_PIP_H, NULL);

	return 0;
}

static int
video_start(void)
{
	GstElement *pipeline, *source, *caps, *ratec;
	GstBus *bus;
	GstCaps *filtercaps, *ratecaps;
	GstStructure *srcstructure;
	GstCapsFeatures *feat;
	GstElement *conv, *rate;
	GstStateChangeReturn ret;
	GMainContext *context = NULL;
	GSource *gsource = NULL;

	struct encoder_data_t encoder;
	struct udpsink_data_t udpsink;

	/* Create the elements */
	source = gst_element_factory_make("nvarguscamerasrc", "source");
	conv = gst_element_factory_make("nvvidconv", "vidconv");
	rate = gst_element_factory_make("videorate", "videorate");

	if (make_encoder(&encoder, ENCODER_H264, BITRATE, FEC_PERCENT) != 0) {
		return -1;
	}

	if (make_udp_sink(&udpsink, UDP_PORT_VIDEO) != 0) {
		cleanup_encoder(&encoder);
		return -1;
	}

	/* Create the empty pipeline */
	pipeline = gst_pipeline_new("test-pipeline");

	if (!pipeline || !source || !conv || !rate) {
		g_printerr("Not all elements could be created.\n");
		cleanup_encoder(&encoder);
		cleanup_udp_sink(&udpsink);
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
		cleanup_encoder(&encoder);
		cleanup_udp_sink(&udpsink);
		return -1;
	}

	feat = gst_caps_features_new("memory:NVMM", NULL);
	if (!feat) {
		g_printerr("Unable to create feature.\n");
		gst_object_unref(pipeline);
		gst_caps_unref(filtercaps);
		gst_object_unref(srcstructure);
		cleanup_encoder(&encoder);
		cleanup_udp_sink(&udpsink);
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

	/* Build the pipeline */
	gst_bin_add_many(GST_BIN(pipeline), source, caps, conv,
			 /*rate, ratec,*/ encoder.encoder, encoder.parser, encoder.rtppay,
			 encoder.rtpfec, udpsink.udpsink, NULL);
	if (gst_element_link_many(source, caps, conv,
				  /* rate, ratec,*/ encoder.encoder, encoder.parser, encoder.rtppay,
				  encoder.rtpfec, udpsink.udpsink, NULL) != TRUE) {
		g_printerr("Elements could not be linked.\n");
		gst_object_unref(pipeline);
		cleanup_encoder(&encoder);
		cleanup_udp_sink(&udpsink);
		return -1;
	}

	/* Start playing */
	ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
	if (ret == GST_STATE_CHANGE_FAILURE) {
		g_printerr("Unable to set the pipeline to the playing state.\n");
		gst_object_unref(pipeline);
		cleanup_encoder(&encoder);
		cleanup_udp_sink(&udpsink);
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

	struct usb_cam_src_data_t cam_front;
	struct usb_cam_src_data_t cam_back;
	struct compositor_data_t compositor;
	struct encoder_data_t pip_encoder;
	struct udpsink_data_t udpsink;

	GstBus *bus;
	GstStateChangeReturn ret;
	GMainContext *context = NULL;
	GSource *gsource = NULL;

	if (make_usb_cam_source_stream(&cam_front, CAMERA_FRONT, "/dev/video2") != 0) {
		return -1;
	}

	if (make_usb_cam_source_stream(&cam_back, CAMERA_BACK, "/dev/video1") != 0) {
		cleanup_usb_cam_source_stream(&cam_front);
		return -1;
	}

	if (make_compositor_stream(&compositor) != 0) {
		cleanup_usb_cam_source_stream(&cam_front);
		cleanup_usb_cam_source_stream(&cam_back);
		return -1;
	}

	if (make_encoder(&pip_encoder, ENCODER_H264, BITRATE_PIP, FEC_PERCENT) != 0) {
		cleanup_usb_cam_source_stream(&cam_front);
		cleanup_usb_cam_source_stream(&cam_back);
		return -1;
	}

	if (make_udp_sink(&udpsink, UDP_PORT_VIDEO_PIP) != 0) {
		cleanup_usb_cam_source_stream(&cam_front);
		cleanup_usb_cam_source_stream(&cam_back);
		cleanup_encoder(&pip_encoder);
		return -1;
	}

	/* Create the empty pipeline */
	pipeline = gst_pipeline_new("pip-pipeline");
	if (!pipeline) {
		g_printerr("Cannot create pip pipeline.\n");
		cleanup_usb_cam_source_stream(&cam_front);
		cleanup_usb_cam_source_stream(&cam_back);
		cleanup_encoder(&pip_encoder);
		cleanup_udp_sink(&udpsink);
		return -1;
	}

	/* Build the pipeline */
	gst_bin_add_many(GST_BIN(pipeline), cam_front.source, cam_front.source_capsfilter,
			 cam_front.jpegparse, cam_front.jpegdec, cam_front.crop, cam_front.vidconv,
			 cam_front.vidconv_capsfilter, NULL);
	gst_bin_add_many(GST_BIN(pipeline), cam_back.source, cam_back.source_capsfilter,
			 cam_back.jpegparse, cam_back.jpegdec, cam_back.crop, cam_back.vidconv,
			 cam_back.vidconv_capsfilter, NULL);
	gst_bin_add_many(GST_BIN(pipeline), compositor.nvcompositor, compositor.nvvidconv,
			 compositor.nvvidconv_caps, pip_encoder.encoder, pip_encoder.parser,
			 pip_encoder.rtppay, pip_encoder.rtpfec, udpsink.udpsink, NULL);

	/* linking source1 */
	if (gst_element_link_many(cam_front.source, cam_front.source_capsfilter,
				  cam_front.jpegparse, cam_front.jpegdec, cam_front.crop,
				  cam_front.vidconv, cam_front.vidconv_capsfilter, NULL) != TRUE) {
		g_printerr("source1 could not be linked to conv1.\n");
		gst_object_unref(pipeline);
		cleanup_usb_cam_source_stream(&cam_front);
		cleanup_usb_cam_source_stream(&cam_back);
		cleanup_encoder(&pip_encoder);
		cleanup_udp_sink(&udpsink);
		return -1;
	}

	/* linking source2 */
	if (gst_element_link_many(cam_back.source, cam_back.source_capsfilter, cam_back.jpegparse,
				  cam_back.jpegdec, cam_back.crop, cam_back.vidconv,
				  cam_back.vidconv_capsfilter, NULL) != TRUE) {
		g_printerr("source2 could not be linked to conv2.\n");
		gst_object_unref(pipeline);
		cleanup_usb_cam_source_stream(&cam_front);
		cleanup_usb_cam_source_stream(&cam_back);
		cleanup_encoder(&pip_encoder);
		cleanup_udp_sink(&udpsink);
		return -1;
	}

	/* linking compositor with sink */
	if (gst_element_link_many(compositor.nvcompositor, compositor.nvvidconv,
				  compositor.nvvidconv_caps, pip_encoder.encoder,
				  pip_encoder.parser, pip_encoder.rtppay, pip_encoder.rtpfec,
				  udpsink.udpsink, NULL) != TRUE) {
		g_printerr("Compositor could not be linked to sink.\n");
		gst_object_unref(pipeline);
		cleanup_usb_cam_source_stream(&cam_front);
		cleanup_usb_cam_source_stream(&cam_back);
		cleanup_encoder(&pip_encoder);
		cleanup_udp_sink(&udpsink);
		return -1;
	}

	/* connect source1 to compositor */
	connect_to_compositor(cam_front.vidconv_capsfilter, &compositor, 0);

	/* connect source2 to compositor */
	connect_to_compositor(cam_back.vidconv_capsfilter, &compositor, VIDEO_PIP_W);

	/* Start playing */
	ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
	if (ret == GST_STATE_CHANGE_FAILURE) {
		g_printerr("Unable to set the pipeline to the playing state.\n");
		gst_object_unref(pipeline);
		cleanup_usb_cam_source_stream(&cam_front);
		cleanup_usb_cam_source_stream(&cam_back);
		cleanup_encoder(&pip_encoder);
		cleanup_udp_sink(&udpsink);
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
	/* Initialize GStreamer */
	gst_init(NULL, NULL);

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
