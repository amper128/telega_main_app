/**
 * @file netlink.h
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Функции работы с сетью
 */

#pragma once

#include <svc/platform.h>

#define IFNAM_SIZE (16U)

#define NL_MAX_IFACES (4U)

typedef struct {
	char ifname[IFNAM_SIZE];
	int ifi_index;
} if_desc_t;

int nl_get_eth_list(if_desc_t if_list[]);

int nl_get_can_list(if_desc_t if_list[]);

int nl_link_up(const if_desc_t *iface);

int nl_link_down(const if_desc_t *iface);
