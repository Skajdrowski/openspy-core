#ifndef _GHTTP_DRIVER_H
#define _GHTTP_DRIVER_H

#include <stdint.h>
#include <OS/Net/drivers/TCPDriver.h>

#include "GHTTPPeer.h"

namespace GHTTP {
	class Driver : public OS::TCPDriver {
	public:
		Driver(INetServer *server, const char *host, uint16_t port);
	protected:
		virtual INetPeer *CreatePeer(uv_tcp_t *sd);
	};
}

#endif
