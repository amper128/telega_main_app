/**
 * @file telemetry.c
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Телеметрия
 */

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

#include <log/log.h>
#include <svc/crc.h>
#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/gps.h>
#include <private/sensors.h>
#include <private/system_telemetry.h>
#include <private/telemetry.h>

static shm_t gps_shm;
static shm_t sensors_shm;
static shm_t sys_status_shm;

#define SERVER "192.168.50.100"
#define PORT 5011

#define X1E7 (10000000)

static void
read_gps_status(RC_td_t *td)
{
	gps_status_t *gps_status;
	void *p;

	shm_map_read(&gps_shm, &p);
	gps_status = p;

	float conv;
	conv = gps_status->latitude;
	td->gps.LatitudeX1E7 = (int32_t)(conv * X1E7);

	conv = gps_status->longitude;
	td->gps.LongitudeX1E7 = (int32_t)(conv * X1E7);

	conv = gps_status->speed;
	td->gps.SpeedKPHX10 = (uint16_t)(conv * 10.0f);

	conv = gps_status->course;
	td->gps.CourseDegrees = (uint16_t)conv;

	conv = gps_status->altitude;
	td->gps.GPSAltitudecm = (int32_t)(conv * 100.0f);

	conv = gps_status->hdop;
	td->gps.HDOPx10 = (uint8_t)(conv * 10.0f);

	td->gps.SatsInUse = gps_status->sats_use;
	td->gps.SatsInView = gps_status->sats_view;
}

static void
read_sensors_status(RC_td_t *td)
{
	union {
		sensors_status_t *s;
		void *p;
	} p;

	shm_map_read(&sensors_shm, &p.p);

	double conv;
	conv = p.s->angle_x;
	td->orientation.PitchDegrees = (int16_t)(conv * 10.0);
	conv = p.s->angle_y;
	td->orientation.RollDegrees = (int16_t)(conv * 10.0);
	conv = p.s->angle_z;
	td->orientation.YawDegrees = (int16_t)(conv * 10.0);

	conv = p.s->vbat;
	td->power.PackVoltageX100 = (uint16_t)(conv * 100.0);

	conv = p.s->curr;
	td->power.PackCurrentX10 = (uint16_t)(conv * 10.0);

	conv = p.s->pwr;
	td->power.mAHConsumed = (uint16_t)conv;
}

static void
read_system_status(RC_td_t *td)
{
	union {
		sys_telemetry_data_t *s;
		void *p;
	} p;

	shm_map_read(&sys_status_shm, &p.p);

	td->system.CPUload = p.s->cpuload;
	/* 0 - AO temp
	 * 1 - CPU temp
	 * 2 - GPU temp
	 * 3 - PLL temp
	 * 4 - PMIC temp
	 * 5 - FAN
	 */
	td->system.CPUtemp = p.s->temp[1];
}

int
telemetry_init(void)
{
	shm_map_init("shm_gps", sizeof(gps_status_t));
	shm_map_init("shm_sensors", sizeof(sensors_status_t));

	return 0;
}

int
telemetry_main(void)
{
	int result = 1;

	do {
		if (!shm_map_open("shm_gps", &gps_shm)) {
			break;
		}

		if (!shm_map_open("shm_sensors", &sensors_shm)) {
			break;
		}

		if (!shm_map_open("sys_status", &sys_status_shm)) {
			break;
		}

		/* UDP init */
		struct sockaddr_in si_other;
		int s, slen = sizeof(si_other);

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

		RC_td_t rc_td;
		memset((uint8_t *)&rc_td, 0, sizeof(rc_td));
		rc_td.magic = RC_TELEMETRY_MAGIC;

		result = 0;

		while (svc_cycle()) {
			read_gps_status(&rc_td);
			read_sensors_status(&rc_td);
			read_system_status(&rc_td);

			rc_td.CRC = crc16((uint8_t *)&rc_td, offsetof(RC_td_t, CRC), 0U);

			/* UDP send */
			if (sendto(s, (uint8_t *)&rc_td, sizeof(rc_td), 0,
				   (struct sockaddr *)&si_other, (socklen_t)slen) == -1) {
				log_err("cannot send to socket");
				result = 1;
				break;
			}
		}
	} while (0);

	return result;
}
