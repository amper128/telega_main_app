/**
 * @file audio_stream.c
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2023
 * @brief Захват аудио, кодирование, передача
 */

#include <arpa/inet.h>
#include <lame/lame.h>
#include <opus.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <stdbool.h>

#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/audio.h>
#include <private/power.h>

#include <proto/audio_stream.h>

#define FRAMES_COUNT (120U)
#define NSTREAMS (64U)
#define NCHANNELS (2U)

#define MAX_PACKET_SIZE (1400U)
#define MAX_DATA_SIZE (MAX_PACKET_SIZE - sizeof(packet_header_t))

#define UDP_PORT_AUDIO (5610)

static shm_t connect_status_shm;
/* локальная копия флага наличия подключения */
static bool m_connected = false;
static struct sockaddr_in si_other;

typedef struct {
	void *encoder_p;
	codec_type_t codec_type;
	uint8_t __reserved[4];
} encoder_desc_t;

static uint32_t packet_id = 0U;

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
			memcpy(&si_other.sin_addr, &cstate->sin_addr, sizeof(si_other.sin_addr));
		}
	}
}

static pa_simple *
init_capture(int rate, pa_sample_format_t format)
{
	static pa_buffer_attr buffer_attr;
	pa_simple *rec;

	pa_sample_spec ss;

	ss.format = format;
	ss.channels = NCHANNELS;
	ss.rate = (uint32_t)rate;

	/* exactly space for the entire play time */
	buffer_attr.maxlength = (uint32_t)((size_t)rate * sizeof(float) * NSTREAMS);
	buffer_attr.tlength = (uint32_t)-1;
	/* Setting prebuf to 0 guarantees us the streams will run synchronously,
	 * no matter what */
	buffer_attr.prebuf = 0;
	buffer_attr.minreq = (uint32_t)-1;
	buffer_attr.fragsize = 8192U;
	int err = 0;

	rec = pa_simple_new(NULL,	    // Use the default server.
			    "Test capture", // Our application's name.
			    PA_STREAM_RECORD,
			    NULL,	  // Use the default device.
			    "Music",	  // Description of our stream.
			    &ss,	  // Our sample format.
			    NULL,	  // Use default channel map
			    &buffer_attr, // Use default buffering attributes.
			    &err);

	if (rec == NULL) {
		fprintf(stderr, "cannot create pulseaudio capture: %s\n", pa_strerror(err));
		exit(1);
	}

	return rec;
}

static encoder_desc_t *
init_encoder(int rate, int kbitrate, codec_type_t codec)
{
	switch (codec) {
	case CODEC_MP3: {
		lame_t lame = lame_init();

		lame_set_in_samplerate(lame, rate);
		lame_set_VBR(lame, vbr_off);
		lame_set_brate(lame, kbitrate);
		lame_set_force_short_blocks(lame, 1);
		lame_init_params(lame);

		encoder_desc_t *result = malloc(sizeof(encoder_desc_t));
		result->codec_type = codec;
		result->encoder_p = lame;

		return result;
	}
	case CODEC_OPUS: {
		OpusEncoder *opus_encoder;
		int err = 0;
		opus_encoder = opus_encoder_create(rate, NCHANNELS, OPUS_APPLICATION_AUDIO, &err);
		if (err < 0) {
			fprintf(stderr, "failed to create an encoder: %s\n", opus_strerror(err));
			exit(1);
		}

		err = opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(kbitrate * 1000));
		if (err < 0) {
			fprintf(stderr, "failed to set bitrate: %s\n", opus_strerror(err));
			exit(1);
		}

		encoder_desc_t *result = malloc(sizeof(encoder_desc_t));
		result->codec_type = codec;
		result->encoder_p = opus_encoder;

		return result;
	}

	default:
		fprintf(stderr, "Cannot create unknown codec\n");
		exit(1);
	}
}

static void
free_encoder(encoder_desc_t *encoder)
{
	switch (encoder->codec_type) {
	default:
	case CODEC_MP3: {
		lame_t lame = (lame_t)encoder->encoder_p;
		lame_close(lame);
	} break;

	case CODEC_OPUS: {
		OpusEncoder *opus = (OpusEncoder *)encoder->encoder_p;
		opus_encoder_destroy(opus);
	} break;
	}
	free(encoder);
}

static int
encode_frames(encoder_desc_t *encoder, int16_t *buffer, size_t frames, uint8_t *out, size_t out_max)
{
	int encoded;

	switch (encoder->codec_type) {
	default:
	case CODEC_MP3: {
		lame_t lame = (lame_t)encoder->encoder_p;

		encoded =
		    lame_encode_buffer_interleaved(lame, buffer, (int)frames, out, (int)out_max);

	} break;

	case CODEC_OPUS: {
		OpusEncoder *opus = encoder->encoder_p;

		encoded = opus_encode(opus, buffer, (int)frames, out, (opus_int32)out_max);
	} break;
	}

	return encoded;
}

static int
audio_start(void)
{
	int rate = 48000;
	int kbitrate = 160;
	codec_type_t codec = CODEC_OPUS;

	union {
		uint8_t u8[MAX_PACKET_SIZE];
		struct {
			packet_header_t hdr;
			uint8_t data[];
		} data;
	} packet;

	pa_simple *rec = init_capture(rate, PA_SAMPLE_S16LE);
	int err = 0;
	int16_t *input_buffer;
	size_t frame_size = pa_sample_size_of_format(PA_SAMPLE_S16LE);
	size_t data_size = frame_size * FRAMES_COUNT * NCHANNELS;

	const size_t enc_size = 8192U;
	unsigned char *enc_buffer;

	input_buffer = malloc(data_size * 8);
	enc_buffer = malloc(enc_size * 8);

	encoder_desc_t *encoder = init_encoder(rate, kbitrate, codec);

	/* инициализируем UDP сокет */
	int s, slen = sizeof(si_other);

	if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		fprintf(stderr, "cannot create socket\n");
		exit(1);
	}

	// memset((char *)&si_other, 0, sizeof(si_other));
	si_other.sin_family = AF_INET;
	si_other.sin_port = htons(UDP_PORT_AUDIO);

	/*if (inet_aton(addr, &si_other.sin_addr) == 0) {
		fprintf(stderr, "inet_aton() failed\n");
		exit(1);
	}*/

	log_dbg("start streaming");

	while (m_connected) {
		check_connect();

		int encoded;
		size_t offset = 0U;

		pa_simple_read(rec, input_buffer, data_size, &err);

		encoded = encode_frames(encoder, input_buffer, FRAMES_COUNT, enc_buffer, enc_size);

		while (encoded > 0) {
			uint16_t len = (uint16_t)encoded;
			if (len > MAX_DATA_SIZE) {
				len = MAX_DATA_SIZE;
			}
			packet.data.hdr.magic = PACKET_MAGIC;
			packet.data.hdr.uid = packet_id++;
			packet.data.hdr.packet_len = len + sizeof(packet_header_t);
			packet.data.hdr.codec_type = (uint8_t)codec;
			packet.data.hdr.channels = NCHANNELS;
			packet.data.hdr.format = (uint8_t)PA_SAMPLE_S16LE;
			packet.data.hdr.rate = (uint32_t)rate;

			memcpy(packet.data.data, &enc_buffer[offset], len);
			offset += len;
			encoded -= len;

			/* UDP send */
			if (sendto(s, packet.u8, packet.data.hdr.packet_len, 0,
				   (struct sockaddr *)&si_other, (socklen_t)slen) == -1) {
				fprintf(stderr, "cannot send to socket\n");
				break;
			}
		}
	}

	log_dbg("stop streaming");

	pa_simple_free(rec);

	free(input_buffer);
	free(enc_buffer);

	free_encoder(encoder);

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

	do {
		if (!shm_map_open("connect_status", &connect_status_shm)) {
			break;
		}

		while (svc_cycle()) {
			check_connect();

			if (m_connected) {
				log_dbg("audio start");
				result = audio_start();
				if (result != 0) {
					break;
				}
			}
		}
	} while (0);

	return result;
}
