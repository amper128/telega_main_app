/**
 * @file gps.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Работа с GPS
 */

#pragma once

#include <svc/platform.h>

typedef struct {
	float latitude;
	float longitude;

	float speed;
	float course;
	float altitude;

	float hdop;

	uint8_t sats_use;
	uint8_t sats_view;

	uint8_t fix_type;
} gps_status_t;

int gps_init(void);

int gps_main(void);
