/**
 * @file crc.h
 * @author Alexey Hohlov <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Функции подсчета CRC
 */

#pragma once

#include <stdint.h>
#include <sys/types.h>

uint16_t crc16(const uint8_t src[], size_t size, uint16_t seed);

