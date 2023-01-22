/**
 * @file power.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Управление системой
 */

#pragma once

#include <arpa/inet.h>
#include <stdbool.h>

typedef struct {
	struct in_addr sin_addr; /* IP адрес */
	bool connected;
} connection_state_t;

int power_init(void);

int power_main(void);
