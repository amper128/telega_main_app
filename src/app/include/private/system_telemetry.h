/**
 * @file system_telemetry.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Телеметрия оборудования
 */

#pragma once

#define MAXTEMP (6U)

typedef struct {
	uint8_t cpuload;
	int8_t temp[MAXTEMP];
} sys_telemetry_data_t;

int system_telemetry_init(void);

int system_telemetry_main(void);
