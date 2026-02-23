#ifndef _GHTTP_SERVER_H
#define _GHTTP_SERVER_H

#include <OS/Net/NetServer.h>

namespace GHTTP {
	class Server : public INetServer {
	public:
		Server();
		virtual ~Server();
		void tick();
	};
}

#endif
