#include "GHTTPPeer.h"
#include "GHTTPDriver.h"

#include <OS/OpenSpy.h>
#include <OS/Buffer.h>

#include <OS/HTTP.h>

#include <OS/tasks.h>

#include <hiredis/hiredis.h>

#include <sstream>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

#include <jansson.h>

namespace {
	struct WarRecordState {
		std::chrono::steady_clock::time_point war_record_until;
		std::vector<std::pair<int, std::chrono::steady_clock::time_point>> recent_missions;
	};

	struct NickCacheEntry {
		std::string nick;
		std::chrono::steady_clock::time_point expires_at;
	};

	static std::mutex s_state_mutex;
	static std::unordered_map<std::string, WarRecordState> s_war_state;
	static std::mutex s_nick_mutex;
	static std::unordered_map<int, NickCacheEntry> s_nick_cache;

	static std::string make_state_key(const OS::Address &addr, int pid) {
		std::ostringstream ss;
		ss << addr.ToString(true) << ":" << pid;
		return ss.str();
	}

	static bool should_treat_as_war_record(const std::string &key, int mission) {
		using namespace std::chrono;
		auto now = steady_clock::now();
		std::lock_guard<std::mutex> lk(s_state_mutex);

		WarRecordState &st = s_war_state[key];
		if (st.war_record_until > now) {
			return true;
		}

		st.recent_missions.push_back({mission, now});
		auto window = seconds(2);
		st.recent_missions.erase(
			std::remove_if(st.recent_missions.begin(), st.recent_missions.end(), [&](const auto &p) {
				return (now - p.second) > window;
			}),
			st.recent_missions.end());

		std::unordered_set<int> uniq;
		for (auto &p : st.recent_missions) {
			uniq.insert(p.first);
		}

		if (uniq.size() >= 3) {
			st.war_record_until = now + seconds(10);
			return true;
		}

		return false;
	}

	static std::string get_query_value(const std::map<std::string, std::string> &q, const char *k) {
		auto it = q.find(k);
		if (it == q.end()) return "";
		return it->second;
	}

	static int to_int(const std::string &s, int def) {
		if (s.empty()) return def;
		return atoi(s.c_str());
	}

	static std::string build_http_status(int code) {
		switch (code) {
			case 200: return "200 OK";
			case 404: return "404 Not Found";
			case 400: return "400 Bad Request";
			default: return "500 Internal Server Error";
		}
	}

	static bool synthetic_fallback_enabled() {
		static int s_cached = -1;
		if (s_cached != -1) return s_cached == 1;
		char buf[16];
		size_t sz = sizeof(buf);
		if (uv_os_getenv("OPENSPY_GSTATS_HTTP_SYNTHETIC", buf, &sz) == 0) {
			s_cached = atoi(buf) ? 1 : 0;
		} else {
			s_cached = 0;
		}
		return s_cached == 1;
	}

	static std::string zset_key_total() {
		return "gstats:sniperelpc:total";
	}
	static std::string zset_key_mission(int mission) {
		std::ostringstream ss;
		ss << "gstats:sniperelpc:mission:" << mission;
		return ss.str();
	}

	static bool webservices_ready() {
		return OS::g_webServicesURL && OS::g_webServicesURL[0] && OS::g_webServicesAPIKey && OS::g_webServicesAPIKey[0];
	}

	static std::unordered_map<int, std::string> fetch_nicks_webservices(const std::vector<int> &pids) {
		std::unordered_map<int, std::string> out;
		if (pids.empty() || !webservices_ready()) return out;

		json_t *root = json_object();
		json_t *arr = json_array();
		for (int pid : pids) {
			json_array_append_new(arr, json_integer(pid));
		}
		json_object_set_new(root, "target_profileids", arr);

		char *payload = json_dumps(root, 0);
		json_decref(root);
		if (!payload) return out;

		std::string url = std::string(OS::g_webServicesURL) + "/v1/Profile/lookup";
		OS::HTTPClient client(url);
		OS::HTTPResponse resp = client.Post(payload, nullptr);
		free(payload);

		if (resp.status_code != 200 || resp.buffer.empty()) return out;

		json_error_t jerr;
		json_t *resp_json = json_loads(resp.buffer.c_str(), 0, &jerr);
		if (!resp_json) return out;

		auto try_add_profile = [&](json_t *profile_obj) {
			if (!profile_obj || !json_is_object(profile_obj)) return;
			json_t *id_obj = json_object_get(profile_obj, "id");
			if (!id_obj || !json_is_integer(id_obj)) return;
			int pid = (int)json_integer_value(id_obj);

			const char *name = nullptr;
			json_t *un = json_object_get(profile_obj, "uniquenick");
			if (un && json_is_string(un)) name = json_string_value(un);
			if (!name || !name[0]) {
				json_t *n = json_object_get(profile_obj, "nick");
				if (n && json_is_string(n)) name = json_string_value(n);
			}
			if (name && name[0]) {
				out[pid] = name;
			}
		};

		if (json_is_array(resp_json)) {
			size_t n = json_array_size(resp_json);
			for (size_t i = 0; i < n; i++) {
				try_add_profile(json_array_get(resp_json, i));
			}
		} else {
			try_add_profile(resp_json);
		}

		json_decref(resp_json);
		return out;
	}

	static std::unordered_map<int, std::string> resolve_nicks(const std::vector<int> &pids) {
		using namespace std::chrono;
		std::unordered_map<int, std::string> result;
		if (pids.empty()) return result;

		std::vector<int> missing;
		missing.reserve(pids.size());
		auto now = steady_clock::now();

		{
			std::lock_guard<std::mutex> lk(s_nick_mutex);
			for (int pid : pids) {
				auto it = s_nick_cache.find(pid);
				if (it != s_nick_cache.end() && it->second.expires_at > now && !it->second.nick.empty()) {
					result[pid] = it->second.nick;
				} else {
					missing.push_back(pid);
				}
			}
		}

		if (!missing.empty()) {
			auto fetched = fetch_nicks_webservices(missing);
			std::lock_guard<std::mutex> lk(s_nick_mutex);
			for (int pid : missing) {
				NickCacheEntry ent;
				auto fit = fetched.find(pid);
				if (fit != fetched.end()) {
					ent.nick = fit->second;
					result[pid] = ent.nick;
				}
				ent.expires_at = now + minutes(5);
				s_nick_cache[pid] = ent;
			}
		}

		return result;
	}
}

namespace GHTTP {
	Peer::Peer(Driver *driver, uv_tcp_t *sd) : INetPeer(driver, sd) {
		m_delete_flag = false;
		m_timeout_flag = false;
		uv_clock_gettime(UV_CLOCK_MONOTONIC, &m_last_recv);
		uv_clock_gettime(UV_CLOCK_MONOTONIC, &m_last_ping);
		OnConnectionReady();
	}

	Peer::~Peer() {
		OS::LogText(OS::ELogLevel_Info, "[%s] Connection closed", getAddress().ToString().c_str());
	}

	void Peer::OnConnectionReady() {
		OS::LogText(OS::ELogLevel_Info, "[%s] New HTTP connection", getAddress().ToString().c_str());
	}

	void Peer::think() {
		if (m_delete_flag) return;
	}

	void Peer::Delete(bool timeout) {
		m_timeout_flag = timeout;
		m_delete_flag = true;
	}

	void Peer::on_stream_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
		m_recv_accumulator.append((const char *)buf->base, nread);
		if (m_recv_accumulator.size() > MAX_UNPROCESSED_DATA) {
			Delete();
			return;
		}

		size_t header_end = m_recv_accumulator.find("\r\n\r\n");
		if (header_end == std::string::npos) {
			return;
		}
		std::string req = m_recv_accumulator.substr(0, header_end + 4);
		m_recv_accumulator.erase(0, header_end + 4);

		handle_http_request(req);
	}

	bool Peer::parse_request_line(const std::string &request, std::string &method, std::string &target) {
		size_t line_end = request.find("\r\n");
		if (line_end == std::string::npos) return false;
		std::string line = request.substr(0, line_end);
		size_t sp1 = line.find(' ');
		if (sp1 == std::string::npos) return false;
		size_t sp2 = line.find(' ', sp1 + 1);
		if (sp2 == std::string::npos) return false;
		method = line.substr(0, sp1);
		target = line.substr(sp1 + 1, sp2 - (sp1 + 1));
		return true;
	}

	void Peer::parse_query_string(const std::string &query, std::map<std::string, std::string> &out) {
		size_t start = 0;
		while (start < query.size()) {
			size_t amp = query.find('&', start);
			std::string part = (amp == std::string::npos) ? query.substr(start) : query.substr(start, amp - start);
			size_t eq = part.find('=');
			if (eq != std::string::npos) {
				out[part.substr(0, eq)] = part.substr(eq + 1);
			} else if (!part.empty()) {
				out[part] = "";
			}
			if (amp == std::string::npos) break;
			start = amp + 1;
		}
	}

	void Peer::handle_http_request(const std::string &request) {
		std::string method, target;
		if (!parse_request_line(request, method, target) || method != "GET") {
			send_http_response(400, "");
			return;
		}

		OS::LogText(OS::ELogLevel_Debug, "[%s] HTTP %s %s", getAddress().ToString().c_str(), method.c_str(), target.c_str());

		std::string path = target;
		std::string query_str;
		size_t qpos = target.find('?');
		if (qpos != std::string::npos) {
			path = target.substr(0, qpos);
			query_str = target.substr(qpos + 1);
		}

		std::map<std::string, std::string> query;
		parse_query_string(query_str, query);

		std::string body;
		if (path == "/sniperelpc/score.asp") {
			body = handle_score(query);
			send_http_response(200, body);
			return;
		}
		if (path == "/sniperelpc/mission.asp") {
			body = handle_mission(query);
			send_http_response(200, body);
			return;
		}

		send_http_response(404, "");
	}

	void Peer::send_http_response(int status_code, const std::string &body) {
		std::ostringstream ss;
		ss << "HTTP/1.1 " << build_http_status(status_code) << "\r\n";
		ss << "Content-Type: text/plain\r\n";
		ss << "Content-Length: " << body.size() << "\r\n";
		ss << "Connection: close\r\n";
		ss << "\r\n";
		ss << body;

		OS::Buffer buffer;
		std::string out = ss.str();
		buffer.WriteBuffer(out.c_str(), out.size());
		append_send_buffer(buffer, true);
		Delete();
	}

	std::string Peer::handle_score(const std::map<std::string, std::string> &query) {
		redisContext *ctx = TaskShared::getThreadLocalRedisContext();
		std::ostringstream out;
		out << "SnipeScore|";

		if (!ctx) return out.str();

		int rows = to_int(get_query_value(query, "rows"), 20);
		int pid = to_int(get_query_value(query, "pid"), -1);
		int anchor = to_int(get_query_value(query, "score"), -1);

		OS::LogText(OS::ELogLevel_Debug, "[%s] score.asp rows=%d pid=%d score=%d", getAddress().ToString().c_str(), rows, pid, anchor);

		std::string key = zset_key_total();

		std::unordered_map<int, std::string> nick_map;
		auto emit_row = [&](int rank1, int row_pid, long long score) {
			out << rank1 << "|";
			auto it = nick_map.find(row_pid);
			out << (it != nick_map.end() ? it->second : "player") << "|";
			out << row_pid << "|";
			out << score << "|";
		};

		if (pid != -1) {
			redisReply *sreply = (redisReply *)redisCommand(ctx, "ZSCORE %s %d", key.c_str(), pid);
			if (!sreply || sreply->type != REDIS_REPLY_STRING) {
				if (sreply) freeReplyObject(sreply);
				if (synthetic_fallback_enabled()) {
					emit_row(1, pid, 0);
				}
				return out.str();
			}
			long long score = atoll(sreply->str);
			freeReplyObject(sreply);

			redisReply *rreply = (redisReply *)redisCommand(ctx, "ZREVRANK %s %d", key.c_str(), pid);
			if (!rreply || rreply->type != REDIS_REPLY_INTEGER) {
				if (rreply) freeReplyObject(rreply);
				if (synthetic_fallback_enabled()) {
					emit_row(1, pid, score);
				}
				return out.str();
			}
			int rank1 = (int)rreply->integer + 1;
			freeReplyObject(rreply);
			nick_map = resolve_nicks({pid});
			emit_row(rank1, pid, score);
			return out.str();
		}

		redisReply *reply = NULL;
		if (anchor >= 0) {
			reply = (redisReply *)redisCommand(ctx, "ZREVRANGEBYSCORE %s %d -inf LIMIT 0 %d WITHSCORES", key.c_str(), anchor - 1, rows);
		} else {
			reply = (redisReply *)redisCommand(ctx, "ZREVRANGE %s 0 %d WITHSCORES", key.c_str(), rows - 1);
		}

		if (!reply || reply->type != REDIS_REPLY_ARRAY) {
			if (reply) freeReplyObject(reply);
			return out.str();
		}
		if (reply->elements < 2) {
			freeReplyObject(reply);
			if (synthetic_fallback_enabled()) {
				emit_row(1, 0, 0);
			}
			return out.str();
		}

		{
			std::vector<int> pids;
			pids.reserve(reply->elements / 2);
			for (size_t i = 0; i + 1 < reply->elements; i += 2) {
				pids.push_back(atoi(reply->element[i]->str));
			}
			nick_map = resolve_nicks(pids);
		}

		for (size_t i = 0; i + 1 < reply->elements; i += 2) {
			int row_pid = atoi(reply->element[i]->str);
			long long score = atoll(reply->element[i + 1]->str);

			redisReply *rreply = (redisReply *)redisCommand(ctx, "ZREVRANK %s %d", key.c_str(), row_pid);
			if (!rreply || rreply->type != REDIS_REPLY_INTEGER) {
				if (rreply) freeReplyObject(rreply);
				continue;
			}
			int rank1 = (int)rreply->integer + 1;
			freeReplyObject(rreply);

			emit_row(rank1, row_pid, score);
		}

		freeReplyObject(reply);
		return out.str();
	}

	std::string Peer::handle_mission(const std::map<std::string, std::string> &query) {
		redisContext *ctx = TaskShared::getThreadLocalRedisContext();
		std::ostringstream out;
		out << "SnipeMission|";

		if (!ctx) return out.str();

		int rows = to_int(get_query_value(query, "rows"), 20);
		int pid = to_int(get_query_value(query, "pid"), -1);
		int anchor = to_int(get_query_value(query, "score"), -1);
		int mission = to_int(get_query_value(query, "mission"), -1);

		OS::LogText(OS::ELogLevel_Debug, "[%s] mission.asp rows=%d pid=%d score=%d mission=%d", getAddress().ToString().c_str(), rows, pid, anchor, mission);

		if (mission < 0) {
			return out.str();
		}

		std::string key = zset_key_mission(mission);

		std::unordered_map<int, std::string> nick_map;
		auto emit_row = [&](int rank1, int row_pid, long long total_score) {
			out << rank1 << "|";
			auto it = nick_map.find(row_pid);
			out << (it != nick_map.end() ? it->second : "player") << "|";
			out << row_pid << "|";
			for (int i = 0; i < 14; i++) {
				out << 0 << "|";
			}
			out << total_score << "|";
		};

		// Personal row lookup (used by Own Score and sometimes by War Record).
		if (pid != -1 && anchor < 0) {
			if (rows == 1) {
				std::string state_key = make_state_key(getAddress(), pid);
				bool war_record = should_treat_as_war_record(state_key, mission);
				OS::LogText(OS::ELogLevel_Debug, "[%s] mission.asp pid+rows=1 classified_as=%s", getAddress().ToString().c_str(), war_record ? "war_record" : "own_score");

				if (war_record) {
					redisReply *top_reply = (redisReply *)redisCommand(ctx, "ZREVRANGE %s 0 0 WITHSCORES", key.c_str());
					if (!top_reply || top_reply->type != REDIS_REPLY_ARRAY || top_reply->elements < 2) {
						if (top_reply) freeReplyObject(top_reply);
						if (synthetic_fallback_enabled()) {
							emit_row(1, 0, 0);
						}
						return out.str();
					}
					int top_pid = atoi(top_reply->element[0]->str);
					long long top_score = atoll(top_reply->element[1]->str);
					freeReplyObject(top_reply);

					redisReply *rreply = (redisReply *)redisCommand(ctx, "ZREVRANK %s %d", key.c_str(), top_pid);
					if (!rreply || rreply->type != REDIS_REPLY_INTEGER) {
						if (rreply) freeReplyObject(rreply);
						if (synthetic_fallback_enabled()) {
							emit_row(1, top_pid, top_score);
						}
						return out.str();
					}
					int rank1 = (int)rreply->integer + 1;
					freeReplyObject(rreply);
					nick_map = resolve_nicks({top_pid});
					emit_row(rank1, top_pid, top_score);
					return out.str();
				}
			}

			redisReply *sreply = (redisReply *)redisCommand(ctx, "ZSCORE %s %d", key.c_str(), pid);
			if (!sreply || sreply->type != REDIS_REPLY_STRING) {
				if (sreply) freeReplyObject(sreply);
				if (synthetic_fallback_enabled()) {
					emit_row(1, pid, 0);
				}
				return out.str();
			}
			long long score = atoll(sreply->str);
			freeReplyObject(sreply);

			redisReply *rreply = (redisReply *)redisCommand(ctx, "ZREVRANK %s %d", key.c_str(), pid);
			if (!rreply || rreply->type != REDIS_REPLY_INTEGER) {
				if (rreply) freeReplyObject(rreply);
				if (synthetic_fallback_enabled()) {
					emit_row(1, pid, score);
				}
				return out.str();
			}
			int rank1 = (int)rreply->integer + 1;
			freeReplyObject(rreply);
			nick_map = resolve_nicks({pid});
			emit_row(rank1, pid, score);
			return out.str();
		}

		// Leaderboard paging/list request.
		redisReply *reply = NULL;
		if (anchor >= 0) {
			reply = (redisReply *)redisCommand(ctx, "ZREVRANGEBYSCORE %s %d -inf LIMIT 0 %d WITHSCORES", key.c_str(), anchor - 1, rows);
		} else {
			reply = (redisReply *)redisCommand(ctx, "ZREVRANGE %s 0 %d WITHSCORES", key.c_str(), rows - 1);
		}

		if (!reply || reply->type != REDIS_REPLY_ARRAY) {
			if (reply) freeReplyObject(reply);
			return out.str();
		}
		if (reply->elements < 2) {
			freeReplyObject(reply);
			if (synthetic_fallback_enabled()) {
				emit_row(1, 0, 0);
			}
			return out.str();
		}

		{
			std::vector<int> pids;
			pids.reserve(reply->elements / 2);
			for (size_t i = 0; i + 1 < reply->elements; i += 2) {
				pids.push_back(atoi(reply->element[i]->str));
			}
			nick_map = resolve_nicks(pids);
		}

		for (size_t i = 0; i + 1 < reply->elements; i += 2) {
			int row_pid = atoi(reply->element[i]->str);
			long long score = atoll(reply->element[i + 1]->str);

			redisReply *rreply = (redisReply *)redisCommand(ctx, "ZREVRANK %s %d", key.c_str(), row_pid);
			if (!rreply || rreply->type != REDIS_REPLY_INTEGER) {
				if (rreply) freeReplyObject(rreply);
				continue;
			}
			int rank1 = (int)rreply->integer + 1;
			freeReplyObject(rreply);

			emit_row(rank1, row_pid, score);
		}

		freeReplyObject(reply);
		return out.str();
	}
}
