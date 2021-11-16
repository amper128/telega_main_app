/**
 * @file system_telemetry.c
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2021
 * @brief Телеметрия оборудования
 */

#include <stdio.h>
#include <string.h>

#include <svc/sharedmem.h>
#include <svc/svc.h>

#include <private/system_telemetry.h>

static shm_t sys_status_shm;

static int8_t
get_tzone_temp(size_t tzone)
{
	int8_t result = 0U;
	char path[128U];
	snprintf(path, sizeof(path) - 1U, "/sys/class/thermal/thermal_zone%zu/temp", tzone);
	FILE *fp = fopen(path, "r");
	int temp = 0;
	int r = fscanf(fp, "%d", &temp);
	fclose(fp);
	if (r > 0) {
		result = (int8_t)(temp / 1000);
	}

	return result;
}

static uint8_t
get_cpuload(void)
{
	uint8_t cpu_load = 0U;
	static long double a[4], b[4];

	FILE *fp;
	int r;

	fp = fopen("/proc/stat", "r");
	r = fscanf(fp, "%*s %Lf %Lf %Lf %Lf", &b[0], &b[1], &b[2], &b[3]);
	fclose(fp);

	if (r > 0) {
		cpu_load = (uint8_t)(((b[0] + b[1] + b[2]) - (a[0] + a[1] + a[2])) /
				     ((b[0] + b[1] + b[2] + b[3]) - (a[0] + a[1] + a[2] + a[3]))) *
			   100U;
	}

	a[0] = b[0];
	a[1] = b[1];
	a[2] = b[2];
	a[3] = b[3];

	return cpu_load;
}

static void
read_status(void)
{
	sys_telemetry_data_t td;

	td.cpuload = get_cpuload();
	size_t i;

	for (i = 0U; i < MAXTEMP; i++) {
		td.temp[i] = get_tzone_temp(i);
	}

	shm_map_write(&sys_status_shm, &td, sizeof(sys_telemetry_data_t));
}

int
system_telemetry_init(void)
{
	int result = 1;

	do {
		shm_map_init("sys_status", sizeof(sys_telemetry_data_t));

		if (!shm_map_open("sys_status", &sys_status_shm)) {
			break;
		}

		result = 0;
	} while (false);

	return result;
}

int
system_telemetry_main(void)
{
	int result = 0;

	while (svc_cycle()) {
		read_status();
	}

	return result;
}
