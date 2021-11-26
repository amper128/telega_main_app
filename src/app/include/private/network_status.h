/**
 * @file network_status.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Информация о сетевых подключениях
 */

#pragma once

#include <svc/platform.h>

#define OPNAMELEN (32U)

typedef struct {
	char OpName[OPNAMELEN];
	uint8_t Status;
	uint8_t Signal;
	uint8_t Mode;
	uint8_t __pad;
} modem_status_t;

int network_status_init(void);

int network_status_main(void);
