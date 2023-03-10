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
#define PORT 5100

#define RC_POWER_MAGIC (0x52435f504f574552ULL)

#define RC_CONNECT_CMD (0x44434f4e4e454354ULL)
#define RC_REBOOT_CMD (0x44525245424f4f54ULL)
#define RC_SHUTDOWN_CMD (0x445253485554444eULL)
#define RC_KEEPALIVE_CMD (0x4b505f414c495645ULL)

#define CONNECT_TMO (1000000000ULL)
#define DISCONNECT_TMO (2000000000ULL)

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
static uint64_t last_keepalive = 0ULL;

static shm_t connect_status_shm;

static void
power_cmd_read(int sock)
{
	socklen_t slen = sizeof(si_other);
	struct sockaddr_in pwr_sockaddr;
	socklen_t pwr_slen = sizeof(pwr_sockaddr);
	pwr_sockaddr.sin_family = AF_INET;
	pwr_sockaddr.sin_port = htons(PORT);
	pwr_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	uint64_t mono = svc_get_monotime();

	connection_state_t cstate;

	if (!connected) {
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
	} else {
		if ((mono - last_keepalive) >= DISCONNECT_TMO) {
			log_warn("disconnected");
			connected = false;
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
				case RC_REBOOT_CMD: {
					int result;
					/* do reboot */
					log_err("REBOOT");
					result = system("reboot");
					(void)result;
					break;
				}

				case RC_SHUTDOWN_CMD: {
					int result;
					/* do shutdown */
					log_err("SHUTDOWN");
					result = system("halt -p");
					(void)result;
					break;
				}

				case RC_KEEPALIVE_CMD:
					/* reply keepalive */
					last_keepalive = mono;
					connected = true;
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

	cstate.connected = connected;
	memcpy(&cstate.sin_addr, &si_other.sin_addr, sizeof(si_other.sin_addr));
	shm_map_write(&connect_status_shm, &cstate, sizeof(connection_state_t));
}

int
power_init(void)
{
	int result = 1;

	do {
		shm_map_init("connect_status", sizeof(connection_state_t));

		if (!shm_map_open("connect_status", &connect_status_shm)) {
			break;
		}

		result = 0;
	} while (false);

	return result;
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

		struct sockaddr_in pwr_sockaddr;
		pwr_sockaddr.sin_family = AF_INET;
		pwr_sockaddr.sin_port = htons(PORT);
		pwr_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
		memset(pwr_sockaddr.sin_zero, '\0', sizeof(pwr_sockaddr.sin_zero));

		if (bind(s, (struct sockaddr *)&pwr_sockaddr, sizeof(struct sockaddr)) == -1) {
			log_err("bind()");
			break;
		}

		int flags = fcntl(s, F_GETFL, 0);
		fcntl(s, F_SETFL, flags | O_NONBLOCK);

		while (svc_cycle()) {
			power_cmd_read(s);
		}
	} while (0);

	return result;
}
