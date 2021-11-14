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

#include <io/canbus.h>
#include <log/log.h>
#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/motion.h>

#define RC_PORT (5565)

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
__attribute__((used)) static inline int16_t
vesc_read_i16(const uint16_t data)
{
	union {
		uint16_t u;
		int16_t i;
	} u;

	u.u = __bswap_constant_16(data);

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

	u.u = __bswap_constant_32(data);

	return u.i;
}

/**
 * @brief чтение двухбайтового значения с делителем
 * @param data [in] данные из сообщения
 * @param div [in] делитель
 * @retval сконвертированное значение
 */
static inline double
vesc_read_float2(const uint16_t data, double div)
{
	union {
		uint16_t u;
		int16_t i;
	} u;

	u.u = __bswap_constant_16(data);

	double f = (double)u.i;

	return f / div;
}

/**
 * @brief чтение четырехбайтового значения с делителем
 * @param data [in] данные из сообщения
 * @param div [in] делитель
 * @retval сконвертированное значение
 */
static inline double
vesc_read_float4(const uint32_t data, double div)
{
	union {
		uint16_t u;
		int16_t i;
	} u;

	u.u = __bswap_constant_16(data);

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

	v.u = __bswap_constant_32(*u.u);
	memcpy(dest, v.u8, 4U);
}

/**
 * @brief разбор сообщений протокола
 * @param msg [in] данные сообщения
 */
static void
parse_msg(const struct can_packet_t *msg)
{
	switch (msg->hdr.cmd) {
	case (uint8_t)VESC_CAN_PACKET_STATUS: {
		union {
			const struct {
				uint32_t rpm;
				uint16_t current_X10;
				uint16_t duty_X1000;
			} * status;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;
		log_inf("rpm: %i, current: %.1f, duty: %.3f", vesc_read_i32(u.status->rpm),
			vesc_read_float2(u.status->current_X10, 10.0),
			vesc_read_float2(u.status->duty_X1000, 1000.0));
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
		log_inf("consumed: %.4f ah, charged: %.4f ah",
			vesc_read_float4(u.status2->ah_X10000, 10000.0),
			vesc_read_float4(u.status2->ahch_X10000, 10000.0));
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
		log_inf("consumed: %.4f wh, charged: %.4f wh",
			vesc_read_float4(u.status3->wh_X10000, 10000.0),
			vesc_read_float4(u.status3->whch_X10000, 10000.0));
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
		log_inf("temp_fet: %.1f, temp_motor: %.1f, current_in: %.1f, pid_pos: %.2f",
			vesc_read_float2(u.status4->temp_fet_X10, 10.0),
			vesc_read_float2(u.status4->temp_motor_X10, 10.0),
			vesc_read_float2(u.status4->current_in_X10, 10.0),
			vesc_read_float2(u.status4->pid_pos_now_X50, 50.0));
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
		log_inf("tacho: %i, v_in: %.1f", vesc_read_i32(u.status5->tacho_value),
			vesc_read_float2(u.status5->v_in_X10, 10.0));
		break;
	}

	default:
		log_inf("recv: from=%X, cmd=%x, data_len=%u", msg->hdr.id, msg->hdr.cmd, msg->len);
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
}

static void
do_motion(float speed, float steering)
{
	struct can_packet_t msg;

	while (read_can_msg(&msg)) {
		parse_msg(&msg);
	}

	(void)speed;
	(void)steering;

	set_drv_duty(0U, speed);
}

int
motion_init(void)
{
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

		while (svc_cycle()) {
			uint8_t rc_data[512];
			int data_len = recvfrom(rc_sock, rc_data, 512U, 0,
						(struct sockaddr *)&rc_sockaddr, &slen_rc);
			if (data_len > 0) {
				struct _r {
					uint32_t seqno;
					int16_t res;
					int16_t axis[4];
					int16_t data[4];
					int8_t sq;
				};

				union {
					struct _r *r;
					uint8_t *u8;
				} r;

				r.u8 = rc_data;

				speed = (float)r.r->axis[1] / 500.0f;
				steering = (float)r.r->axis[0] / 500.0f;
			}

			do_motion(speed, steering);
		}
	} while (0);

	return result;
}
