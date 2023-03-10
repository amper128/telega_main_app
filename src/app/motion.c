/**
 * @file motion.с
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Функции управления движением
 */

#include <arpa/inet.h>
#include <byteswap.h>
#include <fcntl.h>
#include <math.h>
#include <termios.h>

#include <io/canbus.h>
#include <log/log.h>
#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/motion.h>

#define RC_PORT (5565)

#define DEADZONE (0.05f)

#define BTN_F1 (0x4U)
#define BTN_F2 (0x4U)
#define BTN_F3 (0x4U)

#define BTN_FIRE (0x1U)
#define BTN_CH1 (0x10U) /* btn[1] */
#define BTN_CH2 (0x20U) /* btn[1] */

#define BTN_A2 (0x4U)
#define BTN_B1 (0x8U)
#define BTN_D1 (0x4000U) /* btn[1] */

#define BTN_POV_CENTER (0x10U)

#define BTN_A3_UP (0x20U)
#define BTN_A3_RIGHT (0x40U)
#define BTN_A3_DOWN (0x80U)
#define BTN_A3_LEFT (0x100U)
#define BTN_A3_CENTER (0x100U)

#define BTN_A4_UP (0x400U)
#define BTN_A4_RIGHT (0x800U)
#define BTN_A4_DOWN (0x1000U)
#define BTN_A4_LEFT (0x2000U)
#define BTN_A4_CENTER (0x4000U)

#define BTN_C1_UP (0x8000U)
#define BTN_C1_RIGHT (0x1U)  /* btn[1] */
#define BTN_C1_DOWN (0x2U)   /* btn[1] */
#define BTN_C1_LEFT (0x4U)   /* btn[1] */
#define BTN_C1_CENTER (0x8U) /* btn[1] */

static shm_t motion_telemetry_shm;

static motion_telemetry_t mt;

/* текущий счетчик времени */
static uint64_t cur_mono;

static int servo_fd;

enum drive_mode_t {
	DRIVE_MODE_FREE, /* freewheel */
	DRIVE_MODE_DRIVE /* parking, drive, reverse */
};

static enum drive_mode_t dmode = DRIVE_MODE_FREE;
static enum drive_mode_t cached_dmode = DRIVE_MODE_FREE;
static uint64_t last_drv_can_tx;

/**
 * @brief ограничение значения в указанных пределах
 * @param val [in] исходное значение
 * @param max [in] верхний предел
 * @param min [in] нижний предел
 * @retval ограниченное значение
 */
static inline float
flimit(float val, float max, float min)
{
	float result = val;

	if (result > max) {
		result = max;
	}
	if (result < min) {
		result = min;
	}

	return result;
}

/**
 * @brief чтение двухбайтового целого
 * @param data [in] данные из сообщения
 * @retval сконвертированное значение
 */
static inline int16_t
vesc_read_i16(const uint16_t data)
{
	union {
		uint16_t u;
		int16_t i;
	} u;

	/* костыль */
	if (data == 0x0080) {
		return 0;
	}

	u.u = __bswap_16(data);

	return u.i;
}

/**
 * @brief чтение четырехбайтового целого
 * @param data [in] данные из сообщения
 * @retval сконвертированное значение
 */
static inline int32_t
vesc_read_i32(const uint32_t data)
{
	union {
		uint32_t u;
		int32_t i;
	} u;

	u.u = __bswap_32(data);

	return u.i;
}

/**
 * @brief чтение двухбайтового значения с делителем
 * @param data [in] данные из сообщения
 * @param div [in] делитель
 * @retval сконвертированное значение
 */
__attribute_used__ static inline double
vesc_read_float2(const uint16_t data, double div)
{
	union {
		uint16_t u;
		int16_t i;
	} u;

	u.u = __bswap_16(data);

	double f = (double)u.i;

	return f / div;
}

/**
 * @brief чтение четырехбайтового значения с делителем
 * @param data [in] данные из сообщения
 * @param div [in] делитель
 * @retval сконвертированное значение
 */
__attribute_used__ static inline double
vesc_read_float4(const uint32_t data, double div)
{
	union {
		uint32_t u;
		int32_t i;
	} u;

	u.u = __bswap_32(data);

	double f = (double)u.i;

	return f / div;
}

static inline void
vesc_write_i32(const int32_t data, uint8_t *dest)
{
	union {
		const uint32_t *u;
		const int32_t *i;
	} u;

	u.i = &data;

	union {
		uint32_t u;
		uint8_t u8[4];
	} v;

	v.u = __bswap_32(*u.u);
	memcpy(dest, v.u8, 4U);
}

/**
 * @brief разбор сообщений протокола
 * @param msg [in] данные сообщения
 */
static void
parse_msg(const struct can_packet_t *msg)
{
	uint8_t drive_id = msg->hdr.id;
	if (drive_id >= DRIVES_COUNT) {
		if (msg->hdr.cmd == (uint8_t)VESC_CAN_PACKET_PONG) {
			/* do nothing */
			return;
		}
		log_warn("unknown msg from drive ID %u", drive_id);
		log_warn("cmd 0x%02x, len %u", msg->hdr.cmd, msg->len);
		return;
	}

	switch (msg->hdr.cmd) {
	case (uint8_t)VESC_CAN_PACKET_STATUS: {
		union {
			const struct {
				uint32_t rpm;
				uint16_t current_X10;
				uint16_t duty_X10;
			} * status;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;

		mt.dt[drive_id].rpm = vesc_read_i32(u.status->rpm);
		mt.dt[drive_id].current_X10 = vesc_read_i16(u.status->current_X10);
		mt.dt[drive_id].duty_X10 = vesc_read_i16(u.status->duty_X10);

		/*log_inf("rpm: %i, current: %.1f, duty: %.3f", mt.dt[drive_id].rpm,
			vesc_read_float2(u.status->current_X10, 10.0),
			vesc_read_float2(u.status->duty_X10, 10.0));*/
		break;
	}

	case (uint8_t)VESC_CAN_PACKET_STATUS_2: {
		union {
			const struct {
				uint32_t ah_X10000;
				uint32_t ahch_X10000;
			} * status2;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;

		mt.dt[drive_id].ah_X10000 = vesc_read_i32(u.status2->ah_X10000);
		mt.dt[drive_id].ahch_X10000 = vesc_read_i32(u.status2->ahch_X10000);

		/*log_inf("consumed: %.4f ah, charged: %.4f ah",
			vesc_read_float4(u.status2->ah_X10000, 10000.0),
			vesc_read_float4(u.status2->ahch_X10000, 10000.0));*/
		break;
	}

	case (uint8_t)VESC_CAN_PACKET_STATUS_3: {
		union {
			const struct {
				uint32_t wh_X10000;
				uint32_t whch_X10000;
			} * status3;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;

		mt.dt[drive_id].wh_X10000 = vesc_read_i32(u.status3->wh_X10000);
		mt.dt[drive_id].whch_X10000 = vesc_read_i32(u.status3->whch_X10000);

		/*log_inf("consumed: %.4f wh, charged: %.4f wh",
			vesc_read_float4(u.status3->wh_X10000, 10000.0),
			vesc_read_float4(u.status3->whch_X10000, 10000.0));*/
		break;
	}

	case (uint8_t)VESC_CAN_PACKET_STATUS_4: {
		union {
			const struct {
				uint16_t temp_fet_X10;
				uint16_t temp_motor_X10;
				uint16_t current_in_X10;
				uint16_t pid_pos_now_X50;
			} * status4;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;

		mt.dt[drive_id].temp_fet_X10 = vesc_read_i16(u.status4->temp_fet_X10);
		mt.dt[drive_id].temp_motor_X10 = vesc_read_i16(u.status4->temp_motor_X10);
		mt.dt[drive_id].current_in_X10 = vesc_read_i16(u.status4->current_in_X10);
		mt.dt[drive_id].pid_pos_now_X50 = vesc_read_i16(u.status4->pid_pos_now_X50);

		/*log_inf("temp_fet: %.1f, temp_motor: %.1f, current_in: %.1f, pid_pos: %.2f",
			vesc_read_float2(u.status4->temp_fet_X10, 10.0),
			vesc_read_float2(u.status4->temp_motor_X10, 10.0),
			vesc_read_float2(u.status4->current_in_X10, 10.0),
			vesc_read_float2(u.status4->pid_pos_now_X50, 50.0));*/
		break;
	}

	case (uint8_t)VESC_CAN_PACKET_STATUS_5: {
		union {
			const struct {
				uint32_t tacho_value;
				uint16_t v_in_X10;
				uint16_t reserved;
			} * status5;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;

		uint16_t V =
		    u.status5->v_in_X10 & 0xFF7FU; /* накладываем маску, а то лишний бит бывает */
		mt.dt[drive_id].tacho_value = vesc_read_i32(u.status5->tacho_value);
		mt.dt[drive_id].v_in_X10 = vesc_read_i16(V);

		/*log_inf("tacho: %i, v_in: %.1f", mt.dt[drive_id].tacho_value,
			vesc_read_float2(V, 10.0));*/
		break;
	}

	case (uint8_t)VESC_CAN_PACKET_PONG: {
		/* do nothing */
		break;
	}

	default:
		log_inf("recv: from=0x%02X, cmd=0x%02x, data_len=%u", msg->hdr.id, msg->hdr.cmd,
			msg->len);
		break;
	}
}

static void
set_drv_duty(uint8_t drv_id, float duty)
{
	float d = flimit(duty, 1.0f, -1.0f);

	struct can_packet_t msg = {
	    0,
	};

	msg.hdr.cmd = (uint8_t)VESC_CAN_PACKET_SET_DUTY;
	msg.hdr.id = drv_id;

	int32_t conv = (int32_t)(d * 100000.0f);
	vesc_write_i32(conv, msg.data);
	msg.len = sizeof(conv);
	send_can_msg(&msg);

	last_drv_can_tx = cur_mono;
}

static void
drv_free(uint8_t drv_id)
{
	struct can_packet_t msg = {
	    0,
	};

	msg.hdr.cmd = (uint8_t)VESC_CAN_PACKET_SET_CURRENT;
	msg.hdr.id = drv_id;

	int32_t conv = 0;
	vesc_write_i32(conv, msg.data);
	msg.len = sizeof(conv);
	send_can_msg(&msg);

	last_drv_can_tx = cur_mono;
}

static void
drv_keepalive(uint8_t drv_id)
{
	struct can_packet_t msg = {
	    0,
	};

	msg.hdr.cmd = (uint8_t)VESC_CAN_PACKET_PING;
	msg.hdr.id = drv_id;
	msg.len = 0U;
	send_can_msg(&msg);

	last_drv_can_tx = cur_mono;
}

static void
do_freedrive(void)
{
	if (cached_dmode != dmode) {
		uint8_t i;
		for (i = 0U; i < DRIVES_COUNT; i++) {
			drv_free(i);
		}
		cached_dmode = dmode;
	} else {
		if ((cur_mono - last_drv_can_tx) >= (50ULL * TIME_MS)) {
			/* send keepalive */
			uint8_t i;
			for (i = 0U; i < DRIVES_COUNT; i++) {
				drv_keepalive(i);
			}
		}
	}
}

static void
do_drive(float speed, float steering)
{
	if (fabsf(speed) < DEADZONE) {
		speed = 0.0f;
	} else {
		if (speed > 0.0f) {
			speed -= DEADZONE;
		} else {
			speed += DEADZONE;
		}
	}

	if (fabsf(steering) < DEADZONE) {
		steering = 0.0f;
	} else {
		if (steering > 0.0f) {
			steering -= DEADZONE;
		} else {
			steering += DEADZONE;
		}
	}

	speed = flimit(speed, 1.0f, -1.0f);
	steering = flimit(steering, 1.0f, -1.0f);

	static const float plimit = 0.25f;

	float left;
	float right;
	float lsp;
	float rsp;

	float pspeed;
	float pscale;

	if (speed > 0.0f) {
		/* forward */
		if (steering > 0.0f) {
			left = 1.0f;
			right = 1.0f - steering;
		} else {
			/* 1 - (-steering) */
			left = 1.0f + steering;
			right = 1.0f;
		}
	} else {
		/* reverse */
		if (steering > 0.0f) {
			left = 1.0f - steering;
			right = 1.0f;
		} else {
			left = 1.0f;
			/* 1 - (-steering) */
			right = 1.0f + steering;
		}
	}

	/* scale to throttle */
	left *= speed;
	right *= speed;

	/* calculate pivot amount
	 * - strength of pivot (pspeed) based on steering input
	 * - blending of pivot vs drive (pscale) based on throttle input
	 */
	pspeed = steering;
	if (fabsf(speed) > plimit) {
		pscale = 0.0f;
	} else {
		pscale = 1.0f - (fabsf(speed) / plimit);
	}

	/* Calculate final mix of Drive and Pivot */
	lsp = (1.0f - pscale) * left + pscale * (pspeed);
	rsp = (1.0f - pscale) * right - pscale * (pspeed);

	/* like ESP */
	static float sd[DRIVES_COUNT] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
	/* cast from function ... */
	int rpm;
	rpm = abs(mt.dt[0].rpm);
	float lmin = (float)rpm;
	rpm = abs(mt.dt[1].rpm);
	float rmin = (float)rpm;

	size_t i;
	size_t idx;
	for (i = 1U; i < 3U; i++) {
		/* left */
		idx = i * 2U;
		rpm = abs(mt.dt[idx].rpm);
		if ((float)rpm < lmin) {
			if (sd[idx] > 0.99f) {
				lmin = (float)rpm;
			}
		}

		/* right */
		idx = (i * 2U) + 1U;
		rpm = abs(mt.dt[idx].rpm);
		if ((float)rpm < rmin) {
			if (sd[idx] > 0.99f) {
				rmin = (float)rpm;
			}
		}
	}

	for (i = 0U; i < 3U; i++) {
		/* left */
		idx = i * 2U;
		if (abs(mt.dt[idx].rpm) >= 5) {
			rpm = abs(mt.dt[idx].rpm);
			if (lmin / (float)rpm < 0.9f) {
				sd[idx] -= 0.05f;
			} else {
				sd[idx] += 0.05f;
			}
		} else {
			sd[idx] += 0.05f;
		}
		sd[idx] = flimit(sd[idx], 1.0f, 0.0f);

		/* right */
		idx = (i * 2U) + 1U;
		if (abs(mt.dt[idx].rpm) >= 5) {
			rpm = abs(mt.dt[idx].rpm);
			if (rmin / (float)rpm < 0.9f) {
				sd[idx] -= 0.05f;
			} else {
				sd[idx] += 0.05f;
			}
		} else {
			sd[idx] += 0.05f;
		}
		sd[idx] = flimit(sd[idx], 1.0f, 0.0f);
	}

	/* do drive */
	set_drv_duty(0U, lsp * sd[0]);
	set_drv_duty(1U, rsp * sd[1]);
	set_drv_duty(2U, lsp * sd[2]);
	set_drv_duty(3U, rsp * sd[3]);
	set_drv_duty(4U, lsp * sd[4]);
	set_drv_duty(5U, rsp * sd[5]);
}

/**
 * @brief режимы работы задних фонарей
 */
typedef enum {
	TAIL_LIGHT_MODE_NORMAL,	       /**< @brief габариты */
	TAIL_LIGHT_MODE_BRAKING,       /**< @brief торможение */
	TAIL_LIGHT_MODE_EXTRA_BRAKING, /**< @brief резкое торможение */
	TAIL_LIGHT_MODE_BACK	       /**< @brief задний ход */
} tail_light_mode_t;

static void
set_tail_light_mode(tail_light_mode_t mode)
{
	static tail_light_mode_t cur_mode = TAIL_LIGHT_MODE_NORMAL;
	if (cur_mode != mode) {
		struct can_packet_t msg = {
		    0,
		};

		cur_mode = mode;

		msg.hdr.id = 100U;

		switch (mode) {
		default:
		case TAIL_LIGHT_MODE_NORMAL:
			/* static color */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_MODE;
			msg.len = 2U;
			msg.data[0U] = 1U;
			msg.data[1U] = (uint8_t)LEDS_MODE_STATIC_COLOR;
			send_can_msg(&msg);

			/* red */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_COLOR;
			msg.len = 4U;
			msg.data[0U] = 1U;
			msg.data[1U] = 255U;
			msg.data[2U] = 0U;
			msg.data[3U] = 0U;
			send_can_msg(&msg);

			/* brightness */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_BRIGHTNESS;
			msg.len = 2U;
			msg.data[0U] = 1U;
			msg.data[1U] = 32U;
			send_can_msg(&msg);
			break;

		case TAIL_LIGHT_MODE_BRAKING:
			/* static color */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_MODE;
			msg.len = 2U;
			msg.data[0U] = 1U;
			msg.data[1U] = (uint8_t)LEDS_MODE_STATIC_COLOR;
			send_can_msg(&msg);

			/* red */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_COLOR;
			msg.len = 4U;
			msg.data[0U] = 1U;
			msg.data[1U] = 255U;
			msg.data[2U] = 0U;
			msg.data[3U] = 0U;
			send_can_msg(&msg);

			/* brightness */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_BRIGHTNESS;
			msg.len = 2U;
			msg.data[0U] = 1U;
			msg.data[1U] = 255U;
			send_can_msg(&msg);
			break;

		case TAIL_LIGHT_MODE_EXTRA_BRAKING:
			/* static color */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_MODE;
			msg.len = 2U;
			msg.data[0U] = 1U;
			msg.data[1U] = (uint8_t)LEDS_MODE_BLINKING;
			send_can_msg(&msg);

			/* red */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_COLOR;
			msg.len = 4U;
			msg.data[0U] = 1U;
			msg.data[1U] = 255U;
			msg.data[2U] = 0U;
			msg.data[3U] = 0U;
			send_can_msg(&msg);

			/* brightness */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_BRIGHTNESS;
			msg.len = 2U;
			msg.data[0U] = 1U;
			msg.data[1U] = 255U;
			send_can_msg(&msg);

			/* period */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_PERIOD;
			msg.len = 2U;
			msg.data[0U] = 1U;
			msg.data[1U] = 5U;
			send_can_msg(&msg);
			break;

		case TAIL_LIGHT_MODE_BACK:
			/* static color */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_MODE;
			msg.len = 2U;
			msg.data[0U] = 1U;
			msg.data[1U] = (uint8_t)LEDS_MODE_STATIC_COLOR;
			send_can_msg(&msg);

			/* red */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_COLOR;
			msg.len = 4U;
			msg.data[0U] = 1U;
			msg.data[1U] = 255U;
			msg.data[2U] = 255U;
			msg.data[3U] = 255U;
			send_can_msg(&msg);

			/* brightness */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_BRIGHTNESS;
			msg.len = 2U;
			msg.data[0U] = 1U;
			msg.data[1U] = 255U;
			send_can_msg(&msg);
			break;
		}
	}
}

static void
control_tail_lights(float speed)
{
	static float sp_acc[16U];
	static size_t acc_pos = 0U;
	static float acc_value;
	static const float delta = 0.05f;

	sp_acc[acc_pos % 16U] = speed;
	acc_value += speed;
	acc_value -= sp_acc[(acc_pos + 1U) % 16U];
	acc_pos++;

	float acc_speed = acc_value / 16.0f;

	if (speed > 0.0f) {
		/* forward */
		if (speed < (acc_speed - (3.0f * delta))) {
			/* extreme braking */
			set_tail_light_mode(TAIL_LIGHT_MODE_EXTRA_BRAKING);
		} else if (speed < (acc_speed - delta)) {
			/* braking */
			set_tail_light_mode(TAIL_LIGHT_MODE_BRAKING);
		} else {
			/* normal */
			set_tail_light_mode(TAIL_LIGHT_MODE_NORMAL);
		}
	} else {
		if (speed < -delta) {
			/* backward */
			set_tail_light_mode(TAIL_LIGHT_MODE_BACK);
		} else {
			/* normal */
			set_tail_light_mode(TAIL_LIGHT_MODE_NORMAL);
		}
	}
}

static void
control_side_lights(bool connected)
{
	static bool state = false;

	if (state != connected) {
		state = connected;

		struct can_packet_t msg = {
		    0,
		};
		msg.hdr.id = 100U;

		if (connected) {
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_MODE;
			msg.len = 2U;
			msg.data[0U] = 0U;
			msg.data[1U] = (uint8_t)LEDS_MODE_RUNNING_SHAPE;
			send_can_msg(&msg);

			/* green */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_COLOR;
			msg.len = 4U;
			msg.data[0U] = 0U;
			msg.data[1U] = 0U;
			msg.data[2U] = 255U;
			msg.data[3U] = 0U;
			send_can_msg(&msg);
		} else {
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_MODE;
			msg.len = 2U;
			msg.data[0U] = 0U;
			msg.data[1U] = (uint8_t)LEDS_MODE_FADING;
			send_can_msg(&msg);

			/* dark orange */
			msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_COLOR;
			msg.len = 4U;
			msg.data[0U] = 0U;
			msg.data[1U] = 64U;
			msg.data[2U] = 32U;
			msg.data[3U] = 0U;
			send_can_msg(&msg);
		}
	}
}

static void
control_headlights(float brightness)
{
	static uint8_t br = 255U;

	uint8_t val = (uint8_t)(brightness * 255.0f);

	if (br != val) {
		struct can_packet_t msg = {
		    0,
		};

		msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_MODE;
		msg.hdr.id = 101U;
		msg.len = 2U;
		msg.data[0U] = 0U;
		msg.data[1U] = (uint8_t)LEDS_MODE_STATIC_COLOR;
		send_can_msg(&msg);

		msg.data[0U] = 1U;
		send_can_msg(&msg);

		msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SET_BRIGHTNESS;
		msg.data[0U] = 0U;
		msg.data[1U] = val;
		send_can_msg(&msg);

		msg.data[0U] = 1U;
		send_can_msg(&msg);

		br = val;
	}
}

static void
send_lights_sync(uint32_t counter)
{
	struct can_packet_t msg = {
	    0,
	};

	union {
		uint32_t u32;
		uint8_t u8[4U];
	} u;

	u.u32 = counter;

	msg.hdr.cmd = (uint8_t)LIGHT_CAN_PACKET_SYNC;
	msg.hdr.id = 100U;
	msg.len = 4U;
	msg.data[0U] = u.u8[0];
	msg.data[1U] = u.u8[1];
	msg.data[2U] = u.u8[2];
	msg.data[3U] = u.u8[3];
	send_can_msg(&msg);

	msg.hdr.id = 101U;
	send_can_msg(&msg);
}

static int
serial_open(const char *name, const speed_t baud)
{
	int fd = -1;

	do {
		fd = open(name, O_RDWR);
	} while ((fd < 0) && (errno == EINTR));

	if (fd < 0) {
		log_err("could not open serial device %s: %s", name, strerror(errno));
		return fd;
	}

	// disable echo on serial lines
	if (isatty(fd)) {
		struct termios ios;

		tcgetattr(fd, &ios);
		ios.c_lflag = 0;		   /* disable ECHO, ICANON, etc... */
		ios.c_oflag &= (tcflag_t)(~ONLCR); /* Stop \n -> \r\n translation on output */
		ios.c_iflag &= (tcflag_t)(~(
		    ICRNL | INLCR));		/* Stop \r -> \n & \n -> \r translation on input */
		ios.c_iflag |= (IGNCR | IXOFF); /* Ignore \r & XON/XOFF on input */

		if (baud != B0) {
			cfsetispeed(&ios, baud);
			cfsetospeed(&ios, baud);
		}

		tcsetattr(fd, TCSANOW, &ios);
	}

	return fd;
}

struct rc_data_t {
	/*uint32_t seqno;
	int16_t res;*/
	int16_t axis[6];
	uint16_t buttons[4];
	int8_t sq;
	int8_t _pad;
};

static void
camera_control(struct rc_data_t *rc)
{
	static float servo_pan = 90.0f;
	static float servo_tilt = 90.0f;
	float pan = (float)(rc->axis[2] - 1500) / 500.0f * 4.0f;
	if (fabsf(pan) > 0.1f) {
		servo_pan += pan;
	}

	if (servo_pan > 180.0f) {
		servo_pan = 180.0f;
	}
	if (servo_pan < 0.0f) {
		servo_pan = 0.0f;
	}

	float tilt = (float)(rc->axis[3] - 1500) / 500.0f * 4.0f;
	if (fabsf(tilt) > 0.1f) {
		servo_tilt += tilt;
	}

	if (servo_tilt > 160.0f) {
		servo_tilt = 160.0f;
	}
	if (servo_tilt < 60.0f) {
		servo_tilt = 60.0f;
	}

	if (rc->buttons[0] & BTN_POV_CENTER) {
		servo_pan = 90.0f;
		servo_tilt = 90.0f;
	}

	if (rc->buttons[0] & BTN_A3_UP) {
		servo_pan = 90.0f;
		servo_tilt = 160.0f;
	}

	if (rc->buttons[0] & BTN_A3_RIGHT) {
		servo_pan = 180.0f;
		servo_tilt = 100.0f;
	}

	if (rc->buttons[0] & BTN_A3_DOWN) {
		servo_pan = 90.0f;
		servo_tilt = 70.0f;
	}

	if (rc->buttons[0] & BTN_A3_LEFT) {
		servo_pan = 0.0f;
		servo_tilt = 100.0f;
	}

	uint8_t data[5] = {0xA5, (uint8_t)servo_pan, (uint8_t)servo_tilt, 0U, 0U};
	data[4] = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
	ssize_t w = write(servo_fd, data, sizeof(data));
	(void)w;
}

int
motion_init(void)
{
	shm_map_init("motion_status", sizeof(motion_telemetry_shm));

	return 0;
}

int
motion_main(void)
{
	int result = 0;

	do {
		if (can_init() < 0) {
			result = -1;
			break;
		}

		servo_fd = serial_open("/dev/ttyUSB0", B115200);
		if (servo_fd < 0) {
			return 1;
		}

		shm_map_open("motion_status", &motion_telemetry_shm);

		struct sockaddr_in rc_sockaddr;
		int rc_sock;
		socklen_t slen_rc = sizeof(rc_sockaddr);
		rc_sockaddr.sin_family = AF_INET;
		rc_sockaddr.sin_port = htons(RC_PORT);
		rc_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
		memset(rc_sockaddr.sin_zero, '\0', sizeof(rc_sockaddr.sin_zero));

		if ((rc_sock = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
			log_err("Could not create UDP socket!");
			break;
		}

		if (bind(rc_sock, (struct sockaddr *)&rc_sockaddr, sizeof(struct sockaddr)) == -1) {
			log_err("bind()");
			break;
		}

		int flags = fcntl(rc_sock, F_GETFL, 0);
		fcntl(rc_sock, F_SETFL, flags | O_NONBLOCK);

		float speed = 0.0f;
		float steering = 0.0f;
		float head_brightness = 0.0f;

		uint64_t last_rc_rx = svc_get_monotime();
		bool rc_connected = false;
		uint32_t l_counter = 0U;

		while (svc_cycle()) {
			cur_mono = svc_get_monotime();
			uint8_t rc_data[512];

			do {
				ssize_t data_len =
				    recvfrom(rc_sock, rc_data, 512U, 0,
					     (struct sockaddr *)&rc_sockaddr, &slen_rc);

				if (data_len > 0) {
					union {
						struct rc_data_t *r;
						uint8_t *u8;
					} r;

					r.u8 = rc_data;

					camera_control(r.r);

					speed = (float)(r.r->axis[1] - 1500) / 500.0f;
					steering = (float)(r.r->axis[0] - 1500) / 500.0f;

					if ((r.r->axis[0] != 1500) || (r.r->axis[1] != 1500)) {
						dmode = DRIVE_MODE_DRIVE;
					}

					if (r.r->buttons[1] & BTN_D1) {
						dmode = DRIVE_MODE_FREE;
					}

					head_brightness = (float)(r.r->axis[4] - 1500) / 500.0f;
					if (head_brightness < 0.0f) {
						head_brightness = 0.0f;
					}

					last_rc_rx = cur_mono;
					rc_connected = true;
				} else {
					break;
				}
			} while (true);

			if (rc_connected) {
				/* проверка что связь с центром не потеряна */
				if ((cur_mono - last_rc_rx) > (500ULL * TIME_MS)) {
					log_warn("RC connection lost! Stop drone!");

					speed = 0;
					steering = 0;
					rc_connected = false;
				}
			}

			/* парсим входящие сообщения */
			struct can_packet_t msg;
			while (read_can_msg(&msg)) {
				parse_msg(&msg);
			}
			mt.mode = (uint32_t)dmode;
			shm_map_write(&motion_telemetry_shm, &mt, sizeof(mt));

			switch (dmode) {
			case DRIVE_MODE_DRIVE:
				do_drive(speed, steering);
				break;
			case DRIVE_MODE_FREE:
			default:
				do_freedrive();
			}

			control_side_lights(rc_connected);
			control_tail_lights(speed);
			control_headlights(head_brightness);
			send_lights_sync(l_counter++);
		}
	} while (0);

	return result;
}
