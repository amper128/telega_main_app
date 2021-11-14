/**
 * @file canbus.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2020
 * @brief Функции для работы с can шиной
 */

#pragma once

#include <proto/vesc_proto.h>

struct can_packet_t {
	can_hdr_t hdr;
	uint8_t len;
	uint8_t data[8];
};

int can_init(void);

int read_can_msg(struct can_packet_t *msg);

int send_can_msg(struct can_packet_t *msg);
