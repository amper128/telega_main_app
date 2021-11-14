/**
 * @file vesc_proto.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Описание протокола обмена по CAN-шине с контроллером VESC
 */

#pragma once

#include <stdint.h>

typedef struct {
	uint8_t id;
	uint8_t cmd;
	uint8_t __reserved[2U];
} __attribute__((packed)) can_msg_t;
