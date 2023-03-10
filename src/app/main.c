/**
 * @file main.c
 * @author Алексей Хохлов <root@amper.me>
 * @copyright WTFPL License
 * @date 2020
 * @brief Точка входа сервиса, основные функции
 */

#include <sys/prctl.h>
#include <sys/wait.h>

#include <log/log.h>
#include <log/read.h>
#include <svc/sharedmem.h>
#include <svc/svc.h>
#include <svc/timerfd.h>

#include <private/audio.h>
#include <private/gps.h>
#include <private/motion.h>
#include <private/network_status.h>
#include <private/power.h>
#include <private/system_telemetry.h>
#include <private/telemetry.h>
#include <private/video.h>
#include <private/voicestream.h>

#define SERVICES_MAX (32U)

typedef struct {
	pid_t pid;
	const char *name;
	svc_context_t *ctx;
} svc_t;

typedef struct {
	const char *name;
	int (*init)(void);
	int (*main)(void);
	uint64_t period;
} svc_desc_t;

static svc_t svc_list[SERVICES_MAX];
static size_t svc_count = 0U;
static svc_context_t *svc_main;

static int
start_svc(const svc_desc_t *svc_desc)
{
	if (svc_count == SERVICES_MAX) {
		log_err("Service list overflow!");
		return -1;
	}

	log_inf("Starting svc \"%s\"...", svc_desc->name);

	svc_t *svc = &svc_list[svc_count];

	svc->ctx = svc_create_context(svc_desc->name);
	svc->ctx->log_buffer = log_create(svc_desc->name);

	pid_t pid;

	pid = fork();
	if (pid == -1) {
		log_err("cannot fork");
		return -1;
	}

	if (pid == 0) {
		/* we are new service */
		svc_init_context(svc->ctx);

		prctl(PR_SET_NAME, (unsigned long)svc_desc->name, 0, 0, 0);
		log_init();

		/* setup timer */
		svc->ctx->period = svc_desc->period;
		if (svc->ctx->period > 0ULL) {
			svc->ctx->timerfd = timerfd_init(svc_desc->period, svc_desc->period);
			if (svc->ctx->timerfd < 0) {
				log_err("cannot setup timer");
				exit(1);
			}
		}

		exit(svc_desc->main());
	}

	svc->pid = pid;
	svc->name = svc_desc->name;

	svc_count++;

	return 0;
}

static int
start_microservices(void)
{
	static const struct {
		svc_desc_t svc[SERVICES_MAX];
		size_t count;
	} svc_start_list = {
	    {
		{"power", power_init, power_main, 10ULL * TIME_MS},
		{"gps", gps_init, gps_main, 0ULL},
		{"motion", motion_init, motion_main, 50ULL * TIME_MS},
		{"sys_stat", system_telemetry_init, system_telemetry_main, 1ULL * TIME_S},
		{"telemetry", telemetry_init, telemetry_main, 100ULL * TIME_MS},
		{"video", video_init, video_main, 10ULL * TIME_MS},
		{"video_pip", video_init, video_pip_main, 10ULL * TIME_MS},
		{"audio", audio_init, audio_main, 10ULL * TIME_MS},
		{"voice", voice_init, voice_main, 0ULL},
		{"netinfo", network_status_init, network_status_main, 1ULL * TIME_S},
	    },
	    10U};

	size_t i;

	for (i = 0U; i < svc_start_list.count; i++) {
		svc_start_list.svc[i].init();
	}

	for (i = 0U; i < svc_start_list.count; i++) {
		start_svc(&svc_start_list.svc[i]);
	}

	return 0;
}

static void
main_cycle(void)
{
	log_print("main", svc_main->log_buffer);
	size_t i;
	for (i = 0U; i < svc_count; i++) {
		svc_list[i].ctx->watchdog = svc_get_monotime();
		log_print(svc_list[i].name, svc_list[i].ctx->log_buffer);

		int wstatus;
		pid_t result = waitpid(svc_list[i].pid, &wstatus, WNOHANG);
		if (result > 0) {
			log_err("SVC '%s' PID %i EXITED", svc_list[i].name, svc_list[i].pid);
			log_print("main", svc_main->log_buffer);
			exit(1);
		}
	}
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	svc_main = svc_create_context("main");
	svc_init_context(svc_main);
	svc_main->log_buffer = log_create("main");
	log_init();

	int timerfd;

	timerfd = timerfd_init(50ULL * TIME_MS, 50ULL * TIME_MS);
	if (timerfd < 0) {
		return 1;
	}

	if (start_microservices()) {
		return 1;
	}

	while (timerfd_wait(timerfd)) {
		main_cycle();
	}
}
