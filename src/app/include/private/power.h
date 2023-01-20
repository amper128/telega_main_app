/**
 * @file power.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Управление системой
 */

#pragma once

#include <netinet/in.h>
#include <stdbool.h>

typedef struct {
	struct sockaddr_in si_other;
	bool connected;
} connection_state_t;

int power_init(void);

int power_main(void);
