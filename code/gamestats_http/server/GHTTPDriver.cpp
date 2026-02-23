#include "GHTTPDriver.h"

namespace GHTTP {
	Driver::Driver(INetServer *server, const char *host, uint16_t port) : TCPDriver(server, host, port) {
	}
	INetPeer *Driver::CreatePeer(uv_tcp_t *sd) {
		return new Peer(this, sd);
	}
}
