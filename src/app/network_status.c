#include <ctype.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus.h>
#include <gio/gio.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <private/network_status.h>

#define MM_DBUS_SERVICE "org.freedesktop.ModemManager1"
#define MM_DBUS_PATH "/org/freedesktop/ModemManager1/Modem/0"
#define MM_DBUS_GET_PROPERTIES "org.freedesktop.DBus.Properties"

static void
list_connections(GDBusProxy *proxy)
{
	int i;
	GError *error = NULL;
	GVariant *ret, *ret1;
	char **paths;
	gchar *name;
	GDBusProxy *ifproxy;
	// GDBusProxyFlags flags;

	/* Call ListConnections D-Bus method */
	ret = g_dbus_proxy_call_sync(proxy, "GetDevices", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
				     &error);
	if (!ret) {
		g_dbus_error_strip_remote_error(error);
		g_print("ListConnections failed: %s\n", error->message);
		g_error_free(error);
		return;
	}

	g_variant_get(ret, "(^ao)", &paths);
	g_variant_unref(ret);

	/*flags = (GDBusProxyFlags)(G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
				  G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START);*/

	for (i = 0; paths[i]; i++) {
		g_print("%s\n", paths[i]);
		/*ifproxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
					   flags,
					   NULL,
					   "org.freedesktop.NetworkManager",
					   paths[i],//"/org/freedesktop/NetworkManager/Devices/0"
					   "org.freedesktop.NetworkManager.Device",
					   NULL,
					   &error);

		//name = g_dbus_proxy_get_interface_name(ifproxy);
		ret = g_dbus_proxy_get_cached_property(ifproxy, "FirmwareVersion");*/

		ifproxy = g_dbus_proxy_new_for_bus_sync(
		    G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL,
		    "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager/Devices/0",
		    "org.freedesktop.DBus.Properties", NULL, NULL);
		g_assert(ifproxy);

		ret = g_dbus_proxy_call_sync(
		    ifproxy, "Get",
		    g_variant_new("(ss)", "org.freedesktop.NetworkManager.Device", "Interface"),
		    G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
		if (!ret) {
			g_dbus_error_strip_remote_error(error);
			g_warning("Failed to get property: %s\n", error->message);
			g_error_free(error);
			return;
		}

		g_print("\nType String of Variant:- %s\n", g_variant_get_type_string(ret));

		g_variant_get(ret, "(v)", &ret1);
		g_variant_unref(ret);

		g_print("\nType String of Variant:- %s\n", g_variant_get_type_string(ret1));
		g_variant_get(ret1, "s", &name);
		g_variant_unref(ret1);

		g_print("Interface name:- %s\n", name);
	}
	g_strfreev(paths);
}

int
network_status_init(void)
{
	return 0;
}

int
network_status_main(void)
{
	GDBusProxy *proxy;
	GDBusProxyFlags flags;
	GError *error = NULL;
	flags = (GDBusProxyFlags)(G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
				  G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START);

#if !GLIB_CHECK_VERSION(2, 35, 0)
	/* Initialize GType system */
	g_type_init();
#endif

	proxy = g_dbus_proxy_new_for_bus_sync(
	    G_BUS_TYPE_SYSTEM, flags, NULL, /* GDBusInterfaceInfo */
	    "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
	    "org.freedesktop.NetworkManager", NULL, /* GCancellable */
	    &error);
	g_assert(proxy != NULL);

	/* List connections of system settings service */
	list_connections(proxy);

	g_object_unref(proxy);

	return 0;
}
