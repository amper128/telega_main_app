/**
 * @file motion.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Функции управления движением
 */

#pragma once

#include <svc/platform.h>

#define DRIVES_COUNT (6U)

#define DRIVE_ENABLED (1U)

typedef struct {
	uint32_t flags;
	int32_t rpm;
	int16_t current_X10;
	int16_t duty_X10;
	int32_t ah_X10000;
	int32_t ahch_X10000;
	int32_t wh_X10000;
	int32_t whch_X10000;
	int16_t temp_fet_X10;
	int16_t temp_motor_X10;
	int16_t current_in_X10;
	int16_t pid_pos_now_X50;
	int32_t tacho_value;
	int16_t v_in_X10;
	int16_t __pad[3U];
} drive_telemetry_t;

typedef struct {
	drive_telemetry_t dt[DRIVES_COUNT];
	uint32_t mode;
} motion_telemetry_t;

int motion_init(void);

int motion_main(void);
