#ifndef _GHTTP_PEER_H
#define _GHTTP_PEER_H

#include <OS/Net/NetPeer.h>
#include <string>
#include <map>

#define MAX_UNPROCESSED_DATA 32768

namespace GHTTP {
	class Driver;

	class Peer : public INetPeer {
	public:
		Peer(Driver *driver, uv_tcp_t *sd);
		virtual ~Peer();

		void OnConnectionReady();
		void think();
		void Delete(bool timeout = false);

	private:
		void on_stream_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);

		void handle_http_request(const std::string &request);
		void send_http_response(int status_code, const std::string &body);

		static bool parse_request_line(const std::string &request, std::string &method, std::string &target);
		static void parse_query_string(const std::string &query, std::map<std::string, std::string> &out);

		std::string handle_score(const std::map<std::string, std::string> &query);
		std::string handle_mission(const std::map<std::string, std::string> &query);

		std::string m_recv_accumulator;
	};
}

#endif
