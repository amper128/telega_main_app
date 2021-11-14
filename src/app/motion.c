/**
 * @file motion.с
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Функции управления движением
 */

#include <byteswap.h>
#include <io/canbus.h>
#include <log/log.h>
#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/motion.h>

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

/**
 * @brief разбор сообщений протокола
 * @param msg [in] данные сообщения
 */
static void
parse_msg(const struct can_packet_t *msg)
{
	switch (msg->msg.cmd) {
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
		log_inf("recv: from=%X, cmd=%x, data_len=%u", msg->msg.id, msg->msg.cmd, msg->len);
		break;
	}
}

static void
do_motion()
{
	struct can_packet_t msg;

	while (read_can_msg(&msg)) {
		parse_msg(&msg);
	}
}

int
motion_init(void)
{
	return 0;
}

int
motion_main(void)
{
	if (can_init() < 0) {
		return 1;
	}

	while (svc_cycle()) {
		do_motion();
	}

	return 0;
}
