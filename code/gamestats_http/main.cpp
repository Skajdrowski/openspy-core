#include <stdio.h>
#include <string>
#include <OS/Net/NetServer.h>
#include <OS/OpenSpy.h>

#include "server/GHTTPServer.h"
#include "server/GHTTPDriver.h"

INetServer *g_httpserver = NULL;

void tick_handler(uv_timer_t *handle) {
	g_httpserver->tick();
}

int main() {
	uv_loop_t *loop = uv_default_loop();
	uv_timer_t tick_timer;

	uv_timer_init(uv_default_loop(), &tick_timer);

	OS::Init("gstats_http");
	g_httpserver = new GHTTP::Server();

	char address_buff[256];
	char port_buff[16];
	size_t temp_env_sz = sizeof(address_buff);

	if (uv_os_getenv("OPENSPY_GSTATS_HTTP_BIND_ADDR", (char *)&address_buff, &temp_env_sz) != UV_ENOENT) {
		temp_env_sz = sizeof(port_buff);

		uint16_t port = 80;
		if (uv_os_getenv("OPENSPY_GSTATS_HTTP_BIND_PORT", (char *)&port_buff, &temp_env_sz) != UV_ENOENT) {
			port = atoi(port_buff);
		}

		GHTTP::Driver *driver = new GHTTP::Driver(g_httpserver, address_buff, port);

		OS::LogText(OS::ELogLevel_Info, "Adding gstats_http Driver: %s:%d\n", address_buff, port);
		g_httpserver->addNetworkDriver(driver);
	} else {
		OS::LogText(OS::ELogLevel_Warning, "Missing gstats_http bind address environment variable");
	}

	uv_timer_start(&tick_timer, tick_handler, 0, 250);
	uv_run(loop, UV_RUN_DEFAULT);
	uv_loop_close(loop);

	delete g_httpserver;

	OS::Shutdown();
	return 0;
}
