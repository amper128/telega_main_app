#include <ctype.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus.h>
#include <gio/gio.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <private/network_status.h>

#include <svc/sharedmem.h>
#include <svc/svc.h>

#define MM_DBUS_SERVICE "org.freedesktop.ModemManager1"
#define MM_DBUS_PATH "/org/freedesktop/ModemManager1"
#define MM_DBUS_GET_PROPERTIES "org.freedesktop.DBus.Properties.Get"

static shm_t modem_status_shm;

typedef enum {
	MM_MODEM_MODE_NONE = 0,
	MM_MODEM_MODE_CS = 1 << 0,
	MM_MODEM_MODE_2G = 1 << 1,
	MM_MODEM_MODE_3G = 1 << 2,
	MM_MODEM_MODE_4G = 1 << 3
} MMModemMode;

#define MMODE_CS (1U)
#define MMODE_2G (2U)
#define MMODE_3G (3U)
#define MMODE_4G (4U)

static int32_t
get_modem_state(GDBusProxy *ifproxy)
{
	GError *error = NULL;
	GVariant *ret, *ret1;
	gint32 state;

	ret = g_dbus_proxy_call_sync(
	    ifproxy, MM_DBUS_GET_PROPERTIES,
	    g_variant_new("(ss)", "org.freedesktop.ModemManager1.Modem", "CurrentModes"),
	    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (!ret) {
		g_dbus_error_strip_remote_error(error);
		log_err("Failed to get property: %s", error->message);
		g_error_free(error);
		return -1;
	}

	g_variant_get(ret, "(v)", &ret1);
	g_variant_unref(ret);

	g_variant_get(ret1, "(uu)", &state);
	g_variant_unref(ret1);

	return state;
}

static int32_t
get_modem_signal(GDBusProxy *ifproxy)
{
	GError *error = NULL;
	GVariant *ret, *ret1;
	guint32 signal;

	ret = g_dbus_proxy_call_sync(
	    ifproxy, MM_DBUS_GET_PROPERTIES,
	    g_variant_new("(ss)", "org.freedesktop.ModemManager1.Modem", "SignalQuality"),
	    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (!ret) {
		g_dbus_error_strip_remote_error(error);
		log_err("Failed to get property: %s", error->message);
		g_error_free(error);
		return -1;
	}

	g_variant_get(ret, "(v)", &ret1);
	g_variant_unref(ret);

	g_variant_get(ret1, "(ub)", &signal);
	g_variant_unref(ret1);

	return (int32_t)signal;
}

static int32_t
get_modem_mode(GDBusProxy *ifproxy)
{
	GError *error = NULL;
	GVariant *ret, *ret1;
	guint32 mode;

	ret = g_dbus_proxy_call_sync(
	    ifproxy, MM_DBUS_GET_PROPERTIES,
	    g_variant_new("(ss)", "org.freedesktop.ModemManager1.Modem", "CurrentModes"),
	    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (!ret) {
		g_dbus_error_strip_remote_error(error);
		log_err("Failed to get property: %s", error->message);
		g_error_free(error);
		return -1;
	}

	g_variant_get(ret, "(v)", &ret1);
	g_variant_unref(ret);

	g_variant_get(ret1, "(uu)", &mode);
	g_variant_unref(ret1);

	return (int32_t)mode;
}

static const char *
get_modem_operator_name(GDBusProxy *ifproxy)
{
	GError *error = NULL;
	GVariant *prop;
	GVariant *result;

	result = g_dbus_proxy_call_sync(
	    ifproxy, MM_DBUS_GET_PROPERTIES,
	    g_variant_new("(ss)", "org.freedesktop.ModemManager1.Modem.Modem3gpp", "OperatorName"),
	    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	if (!result) {
		g_dbus_error_strip_remote_error(error);
		log_err("Failed to get property: %s", error->message);
		g_error_free(error);
		return NULL;
	}

	g_variant_get(result, "(v)", &prop);
	g_variant_unref(result);
	if (prop == NULL) {
		return NULL;
	}

	return g_variant_get_string(prop, NULL);
}

static void
modem_status(const char *path)
{
	modem_status_t ms = {
	    0,
	};

	GDBusProxy *ifproxy;

	ifproxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
						MM_DBUS_SERVICE, path,
						"org.freedesktop.DBus.Properties", NULL, NULL);
	g_assert(ifproxy);

	int32_t state = get_modem_state(ifproxy);
	if (state >= 0) {
		ms.Status = (uint8_t)state;
	}

	int32_t signal = get_modem_signal(ifproxy);
	if (signal >= 0) {
		ms.Signal = (uint8_t)signal;
	}

	int32_t mode = get_modem_mode(ifproxy);
	if (mode >= 0) {
		if (mode & MM_MODEM_MODE_4G) {
			ms.Mode = MMODE_4G;
		} else if (mode & MM_MODEM_MODE_3G) {
			ms.Mode = MMODE_3G;
		} else if (mode & MM_MODEM_MODE_2G) {
			ms.Mode = MMODE_2G;
		} else if (mode & MM_MODEM_MODE_CS) {
			ms.Mode = MMODE_CS;
		} else {
			ms.Mode = 0;
		}
	}

	const char *name = get_modem_operator_name(ifproxy);
	if (name != NULL) {
		strncpy(ms.OpName, name, OPNAMELEN - 1U);
	}

	shm_map_write(&modem_status_shm, &ms, sizeof(modem_status_t));
}

static void
list_modems(void)
{
	GDBusProxy *proxy;
	GError *error = NULL;
	GVariant *ret;

	proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
					      MM_DBUS_SERVICE, MM_DBUS_PATH,
					      "org.freedesktop.DBus.ObjectManager", NULL, &error);
	g_assert(proxy != NULL);

	ret = g_dbus_proxy_call_sync(proxy, "GetManagedObjects", NULL, G_DBUS_CALL_FLAGS_NONE, -1,
				     NULL, &error);
	if (!ret) {
		g_dbus_error_strip_remote_error(error);
		log_err("GetManagedObjects failed: %s", error->message);
		g_error_free(error);
		return;
	}

	GVariant *result;
	const gchar *object_path;
	GVariantIter i;
	GVariant *ifaces_and_properties;

	result = g_variant_get_child_value(ret, 0);
	g_variant_iter_init(&i, result);
	/* (a{oa{sa{sv}}}) */
	while (g_variant_iter_next(&i, "{&o@a{sa{sv}}}", &object_path, &ifaces_and_properties)) {
		modem_status(object_path);
		/* supports only one modem */
		break;
	}
	g_variant_unref(result);
	g_variant_unref(ret);
}

int
network_status_init(void)
{
	int result = 1;

	do {
		shm_map_init("modem_status", sizeof(modem_status_t));

		if (!shm_map_open("modem_status", &modem_status_shm)) {
			break;
		}

#if !GLIB_CHECK_VERSION(2, 35, 0)
		/* Initialize GType system */
		g_type_init();
#endif

		result = 0;
	} while (false);

	return result;
}

int
network_status_main(void)
{
	while (svc_cycle()) {
		list_modems();
	}

	return 0;
}
