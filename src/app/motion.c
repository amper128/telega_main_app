/**
 * @file motion.с
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Функции управления движением
 */

#include <io/canbus.h>
#include <log/log.h>
#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/motion.h>

static void
parse_msg(const struct can_packet_t *msg)
{
	log_inf("recv: from=%X, cmd=%x, data_len=%u", msg->msg.id, msg->msg.cmd, msg->len);
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
