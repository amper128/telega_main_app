/**
 * @file power.с
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Управление системой
 */

#include <arpa/inet.h>
#include <fcntl.h>

#include <log/log.h>
#include <svc/crc.h>
#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/power.h>

#define SERVER "192.168.50.100"
#define PORT 5011

#define RC_POWER_MAGIC (0x52435f504f574552ULL)

#define RC_CONNECT_CMD (0x44434f4e4e454354ULL)
#define RC_REBOOT_CMD (0x44525245424f4f54ULL)
#define RC_SHUTDOWN_CMD (0x445253485554444eULL)
#define RC_KEEPALIVE_CMD (0x4b505f414c495645ULL)

#define CONNECT_TMO (1000000000ULL)

typedef struct {
	uint64_t magic;
	uint64_t cmd;
	uint64_t payload[8];
	uint16_t __pad[3U];
	uint16_t CRC;
} pwr_ctl_t;

static struct sockaddr_in si_other;
static bool connected = false;
static uint64_t connect_tm = 0ULL;

static void
power_cmd_read(int sock)
{
	socklen_t slen = sizeof(si_other);
	struct sockaddr_in pwr_sockaddr;
	socklen_t pwr_slen = sizeof(pwr_sockaddr);
	pwr_sockaddr.sin_family = AF_INET;
	pwr_sockaddr.sin_port = htons(PORT);
	pwr_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (!connected) {
		uint64_t mono = svc_get_monotime();

		if ((mono - connect_tm) >= CONNECT_TMO) {
			/* send "connect" command */

			union {
				pwr_ctl_t pc;
				uint8_t u8[sizeof(pwr_ctl_t)];
			} r;

			r.pc.magic = RC_POWER_MAGIC;
			r.pc.cmd = RC_CONNECT_CMD;
			r.pc.CRC = crc16(r.u8, offsetof(pwr_ctl_t, CRC), 0U);
			connect_tm = mono;

			if (sendto(sock, r.u8, sizeof(pwr_ctl_t), 0, (struct sockaddr *)&si_other,
				   slen) == -1) {
				log_err("cannot send to socket");
			}
		}
	}

	do {
		uint8_t data[sizeof(pwr_ctl_t)];
		ssize_t data_len = recvfrom(sock, data, sizeof(pwr_ctl_t), 0,
					    (struct sockaddr *)&pwr_sockaddr, &pwr_slen);

		if (data_len > 0) {
			union {
				pwr_ctl_t *pc;
				uint8_t *u8;
			} r;

			r.u8 = data;

			uint16_t crc = crc16(data, offsetof(pwr_ctl_t, CRC), 0U);
			if (crc == r.pc->CRC) {
				if (r.pc->magic != RC_POWER_MAGIC) {
					/* wrong packet magic */
					continue;
				}

				switch (r.pc->cmd) {
				case RC_REBOOT_CMD:
					/* do reboot */
					log_err("REBOOT");
					break;

				case RC_SHUTDOWN_CMD:
					/* do shutdown */
					log_err("SHUTDOWN");
					break;

				case RC_KEEPALIVE_CMD:
					/* reply keepalive */
					if (sendto(sock, r.u8, sizeof(pwr_ctl_t), 0,
						   (struct sockaddr *)&si_other, slen) == -1) {
						log_err("cannot send to socket");
					}
					break;

				default:
					break;
				}
			}
		} else {
			break;
		}
	} while (true);
}

int
power_init(void)
{
	return 0;
}

int
power_main(void)
{
	int result = 1;

	do {
		/* UDP init */
		int s;

		if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
			log_err("cannot create socket");
			break;
		}

		memset((char *)&si_other, 0, sizeof(si_other));
		si_other.sin_family = AF_INET;
		si_other.sin_port = htons(PORT);

		if (inet_aton(SERVER, &si_other.sin_addr) == 0) {
			log_err("inet_aton() failed");
			break;
		}

		while (svc_cycle()) {
			power_cmd_read(s);
		}
	} while (0);

	return result;
}
