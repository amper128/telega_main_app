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

#define bswap16(x) ((x) >> 8 | ((x)&255) << 8)

static inline int32_t
bswap32(int32_t x)
{
	union {
		const int32_t *i;
		const uint32_t *u;
	} u1;

	union {
		int32_t i;
		uint32_t u;
	} u2;

	u1.i = &x;

	u2.u = __bswap_constant_32(*u1.u);

	return u2.i;
}

static void
parse_msg(const struct can_packet_t *msg)
{
	switch (msg->msg.cmd) {
	case VESC_CAN_PACKET_STATUS: {
		union {
			const struct {
				int32_t rpm;
				int16_t current_X10;
				int16_t duty_X1000;
			} * status;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;
		log_inf("rpm: %i, current: %.1f, duty: %.3f", bswap32(u.status->rpm),
			(double)bswap16(u.status->current_X10) / 10.0,
			(double)bswap16(u.status->duty_X1000) / 1000.0);
		break;
	}

	case VESC_CAN_PACKET_STATUS_2: {
		union {
			const struct {
				int32_t ah_X10000;
				int32_t ahch_X10000;
			} * status2;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;
		log_inf("consumed: %.4f ah, charged: %.4f ah",
			(double)bswap32(u.status2->ah_X10000) / 10000.0,
			(double)bswap32(u.status2->ahch_X10000) / 10000.0);
		break;
	}

	case VESC_CAN_PACKET_STATUS_3: {
		union {
			const struct {
				int32_t wh_X10000;
				int32_t whch_X10000;
			} * status3;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;
		log_inf("consumed: %.4f wh, charged: %.4f wh",
			(double)bswap32(u.status3->wh_X10000) / 10000.0,
			(double)bswap32(u.status3->whch_X10000) / 10000.0);
		break;
	}

	case VESC_CAN_PACKET_STATUS_4: {
		union {
			const struct {
				int16_t temp_fet_X10;
				int16_t temp_motor_X10;
				int16_t current_in_X10;
				int16_t pid_pos_now_X50;
			} * status4;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;
		log_inf("temp_fet: %.1f, temp_motor: %.1f, current_in: %.1f, pid_pos: %.2f",
			(double)bswap16(u.status4->temp_fet_X10) / 10.0,
			(double)bswap16(u.status4->temp_motor_X10) / 10.0,
			(double)bswap16(u.status4->current_in_X10) / 10.0,
			(double)bswap16(u.status4->pid_pos_now_X50) / 50.0);
		break;
	}

	case VESC_CAN_PACKET_STATUS_5: {
		union {
			const struct {
				int32_t tacho_value;
				int16_t v_in_X10;
				int16_t reserved;
			} * status5;
			const uint8_t *p8;
		} u;

		u.p8 = msg->data;
		log_inf("tacho: %i, v_in: %.1f", bswap32(u.status5->tacho_value),
			(double)bswap16(u.status5->v_in_X10) / 10.0);
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

	if (read_can_msg(&msg)) {
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
