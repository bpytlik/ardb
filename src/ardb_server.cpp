/*
 * ardb_server.cpp
 *
 *  Created on: 2013-4-8
 *      Author: wqy
 */
#include "ardb_server.hpp"
#include <stdarg.h>
#ifdef __USE_KYOTOCABINET__
#include "engine/kyotocabinet_engine.hpp"
#else
#include "engine/leveldb_engine.hpp"
#endif

#define REDIS_REPLY_STRING 1
#define REDIS_REPLY_ARRAY 2
#define REDIS_REPLY_INTEGER 3
#define REDIS_REPLY_NIL 4
#define REDIS_REPLY_STATUS 5
#define REDIS_REPLY_ERROR 6

#define ARDB_REPLY_DOUBLE 106

namespace ardb
{
static inline void fill_error_reply(ArdbReply& reply, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char buf[1024];
	vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	reply.type = REDIS_REPLY_ERROR;
	reply.str = buf;
}

static inline void fill_status_reply(ArdbReply& reply, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char buf[1024];
	vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	reply.type = REDIS_REPLY_STATUS;
	reply.str = buf;
}

static inline void fill_int_reply(ArdbReply& reply, int64 v)
{
	reply.type = REDIS_REPLY_INTEGER;
	reply.integer = v;
}
static inline void fill_double_reply(ArdbReply& reply, double v)
{
	reply.type = ARDB_REPLY_DOUBLE;
	reply.double_value = v;
}

static inline void fill_str_reply(ArdbReply& reply, const std::string& v)
{
	reply.type = REDIS_REPLY_STRING;
	reply.str = v;
}

template<typename T>
static inline void fill_array_reply(ArdbReply& reply, T& v)
{
	reply.type = REDIS_REPLY_ARRAY;
	typename T::iterator it = v.begin();
	while (it != v.end())
	{
		const ValueObject& vo = *it;
		ArdbReply r;
		if (vo.type == EMPTY)
		{
			r.type = REDIS_REPLY_NIL;
		}
		else
		{
			fill_str_reply(r, vo.ToString());
		}
		reply.elements.push_back(r);
		it++;
	}
}

static inline void fill_str_array_reply(ArdbReply& reply, StringArray& v)
{
	reply.type = REDIS_REPLY_ARRAY;
	StringArray::iterator it = v.begin();
	while (it != v.end())
	{
		ArdbReply r;
		fill_str_reply(r, *it);
		reply.elements.push_back(r);
		it++;
	}
}

static void encode_reply(Buffer& buf, ArdbReply& reply)
{
	switch (reply.type)
	{
	case REDIS_REPLY_NIL:
	{
		buf.Printf("$-1\r\n");
		break;
	}
	case REDIS_REPLY_STRING:
	{
		buf.Printf("$%d\r\n", reply.str.size());
		if (reply.str.size() > 0)
		{
			buf.Printf("%s\r\n", reply.str.c_str());
		}
		else
		{
			buf.Printf("\r\n");
		}
		break;
	}
	case REDIS_REPLY_ERROR:
	{
		buf.Printf("-%s\r\n", reply.str.c_str());
		break;
	}
	case REDIS_REPLY_INTEGER:
	{
		buf.Printf(":%lld\r\n", reply.integer);
		break;
	}
	case ARDB_REPLY_DOUBLE:
	{
		std::string doubleStrValue;
		fast_dtoa(reply.double_value, 9, doubleStrValue);
		buf.Printf("$%d\r\n", doubleStrValue.size());
		buf.Printf("%s\r\n", doubleStrValue.c_str());
		break;
	}
	case REDIS_REPLY_ARRAY:
	{
		buf.Printf("*%d\r\n", reply.elements.size());
		size_t i = 0;
		while (i < reply.elements.size())
		{
			encode_reply(buf, reply.elements[i]);
			i++;
		}
		break;
	}
	case REDIS_REPLY_STATUS:
	{
		buf.Printf("+%s\r\n", reply.str.c_str());
		break;
	}
	default:
	{
		ERROR_LOG("Recv unexpected redis reply type:%d", reply.type);
		break;
	}
	}
}

int ArdbServer::ParseConfig(const Properties& props, ArdbServerConfig& cfg)
{
	conf_get_int64(props, "port", cfg.listen_port);
	conf_get_string(props, "bind", cfg.listen_host);
	conf_get_string(props, "unixsocket", cfg.listen_unix_path);
	std::string daemonize;
	conf_get_string(props, "daemonize", daemonize);
	daemonize = string_tolower(daemonize);
	if (daemonize == "yes")
	{
		cfg.daemonize = true;
	}
	return 0;
}

ArdbServer::ArdbServer() :
		m_service(NULL), m_db(NULL), m_engine(NULL)
{
	struct RedisCommandHandlerSetting settingTable[] =
	{
	{ "ping", &ArdbServer::Ping, 0, 0 },
	{ "echo", &ArdbServer::Echo, 1, 1 },
	{ "quit", &ArdbServer::Quit, 0, 0 },
	{ "shutdown", &ArdbServer::Shutdown, 0, 1 },
	{ "slaveof", &ArdbServer::Slaveof, 2, 2 },
	{ "select", &ArdbServer::Select, 1, 1 },
	{ "append", &ArdbServer::Append, 2, 2 },
	{ "get", &ArdbServer::Get, 1, 1 },
	{ "set", &ArdbServer::Set, 2, 7 },
	{ "del", &ArdbServer::Del, 1, -1 },
	{ "exists", &ArdbServer::Exists, 1, 1 },
	{ "expire", &ArdbServer::Expire, 2, 2 },
	{ "expireat", &ArdbServer::Expireat, 2, 2 },
	{ "persist", &ArdbServer::Persist, 1, 1 },
	{ "type", &ArdbServer::Type, 1, 1 },
	{ "bitcount", &ArdbServer::Bitcount, 1, 3 },
	{ "bitop", &ArdbServer::Bitop, 3, -1 },
	{ "decr", &ArdbServer::Decr, 1, 1 },
	{ "decrby", &ArdbServer::Decrby, 2, 2 },
	{ "getbit", &ArdbServer::GetBit, 2, 2 },
	{ "getrange", &ArdbServer::GetRange, 3, 3 },
	{ "getset", &ArdbServer::GetSet, 2, 2 },
	{ "incr", &ArdbServer::Incr, 1, 1 },
	{ "incrby", &ArdbServer::Incrby, 2, 2 },
	{ "incrbyfloat", &ArdbServer::IncrbyFloat, 2, 2 },
	{ "mget", &ArdbServer::MGet, 1, -1 },
	{ "mset", &ArdbServer::MSet, 2, -1 },
	{ "msetnx", &ArdbServer::MSetNX, 2, -1 },
	{ "psetex", &ArdbServer::MSetNX, 3, 3 },
	{ "setbit", &ArdbServer::SetBit, 3, 3 },
	{ "setex", &ArdbServer::SetEX, 3, 3 },
	{ "setnx", &ArdbServer::SetNX, 2, 2 },
	{ "setrange", &ArdbServer::SetRange, 3, 3 },
	{ "strlen", &ArdbServer::Strlen, 1, 1 },
	{ "hdel", &ArdbServer::HDel, 2, -1 },
	{ "hexists", &ArdbServer::HExists, 2, 2 },
	{ "hget", &ArdbServer::HGet, 2, 2 },
	{ "hgetall", &ArdbServer::HGetAll, 1, 1 },
	{ "hincr", &ArdbServer::HIncrby, 3, 3 },
	{ "hincrbyfloat", &ArdbServer::HIncrbyFloat, 3, 3 },
	{ "hkeys", &ArdbServer::HKeys, 1, 1 },
	{ "hlen", &ArdbServer::HLen, 1, 1 },
	{ "hvals", &ArdbServer::HVals, 1, 1 },
	{ "hmget", &ArdbServer::HMGet, 2, -1 },
	{ "hset", &ArdbServer::HSet, 3, 3 },
	{ "hsetnx", &ArdbServer::HSetNX, 3, 3 },
	{ "hmset", &ArdbServer::HMSet, 3, -1 },
	{ "scard", &ArdbServer::SCard, 1, 1 },
	{ "sadd", &ArdbServer::SAdd, 2, -1 },
	{ "sdiff", &ArdbServer::SDiff, 2, -1 },
	{ "sdiffstore", &ArdbServer::SDiffStore, 3, -1 },
	{ "sinter", &ArdbServer::SInter, 2, -1 },
	{ "sinterstore", &ArdbServer::SInterStore, 3, -1 },
	{ "sismember", &ArdbServer::SIsMember, 2, 2 },
	{ "smembers", &ArdbServer::SMembers, 1, 1 },
	{ "smove", &ArdbServer::SMove, 3, 3 },
	{ "spop", &ArdbServer::SPop, 1, 1 },
	{ "sranmember", &ArdbServer::SRandMember, 1, 2 },
	{ "srem", &ArdbServer::SRem, 2, -1 },
	{ "sunion", &ArdbServer::SUnion, 2, -1 },
	{ "sunionstore", &ArdbServer::SUnionStore, 3, -1 },
	{ "zadd", &ArdbServer::ZAdd, 3, -1 },
	{ "zcard", &ArdbServer::ZCard, 1, 1 },
	{ "zcount", &ArdbServer::ZCount, 3, 3 },
	{ "zscore", &ArdbServer::ZScore, 2, 2 },};

	uint32 arraylen = arraysize(settingTable);
	for (uint32 i = 0; i < arraylen; i++)
	{
		m_handler_table[settingTable[i].name] = settingTable[i];
	}
}
ArdbServer::~ArdbServer()
{

}

int ArdbServer::Type(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->Type(ctx.currentDB, cmd[0]);
	switch (ret)
	{
	case SET_ELEMENT:
	{
		fill_status_reply(ctx.reply, "set");
		break;
	}
	case LIST_META:
	{
		fill_status_reply(ctx.reply, "list");
		break;
	}
	case ZSET_ELEMENT_SCORE:
	{
		fill_status_reply(ctx.reply, "zset");
		break;
	}
	case HASH_FIELD:
	{
		fill_status_reply(ctx.reply, "hash");
		break;
	}
	case KV:
	{
		fill_status_reply(ctx.reply, "string");
		break;
	}
	case TABLE_META:
	{
		fill_status_reply(ctx.reply, "table");
		break;
	}
	default:
	{
		fill_status_reply(ctx.reply, "none");
		break;
	}
	}
	return 0;
}

int ArdbServer::Persist(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->Persist(ctx.currentDB, cmd[0]);
	fill_int_reply(ctx.reply, ret == 0 ? 1 : 0);
	return 0;
}

int ArdbServer::Expire(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	fill_int_reply(ctx.reply, 1);
	return 0;
}

int ArdbServer::Expireat(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	fill_int_reply(ctx.reply, 1);
	return 0;
}

int ArdbServer::Exists(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	bool ret = m_db->Exists(ctx.currentDB, cmd[0]);
	fill_int_reply(ctx.reply, ret ? 1 : 0);
	return 0;
}

int ArdbServer::Del(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray array;
	ArgumentArray::iterator it = cmd.begin();
	while (it != cmd.end())
	{
		array.push_back(*it);
		it++;
	}
	m_db->Del(ctx.currentDB, array);
	fill_int_reply(ctx.reply, array.size());
	return 0;
}

int ArdbServer::Set(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	const std::string& key = cmd[0];
	const std::string& value = cmd[1];
	int ret = 0;
	if (cmd.size() == 2)
	{
		ret = m_db->Set(ctx.currentDB, key, value);
	}
	else
	{
		int i = 0;
		uint64 px = 0, ex = 0;
		for (i = 2; i < cmd.size(); i++)
		{
			std::string tmp = string_tolower(cmd[i]);
			if (tmp == "ex" || tmp == "px")
			{
				int64 iv;
				if (!raw_toint64(cmd[i + 1].c_str(), cmd[i + 1].size(), iv)
						|| iv < 0)
				{
					fill_error_reply(ctx.reply,
							"ERR value is not an integer or out of range");
					return 0;
				}
				if (tmp == "px")
				{
					px = iv;
				}
				else
				{
					ex = iv;
				}
				i++;
			}
			else
			{
				break;
			}
		}
		bool hasnx, hasxx;
		bool syntaxerror = false;
		if (i < cmd.size() - 1)
		{
			syntaxerror = true;
		}
		if (i == cmd.size() - 1)
		{
			std::string cmp = string_tolower(cmd[i]);
			if (cmp != "nx" && cmp != "xx")
			{
				syntaxerror = true;
			}
			else
			{
				hasnx = cmp == "nx";
				hasxx = cmp == "xx";
			}
		}
		if (syntaxerror)
		{
			fill_error_reply(ctx.reply, "ERR syntax error");
			return 0;
		}
		int nxx = 0;
		if (hasnx)
		{
			nxx = -1;
		}
		if (hasxx)
		{
			nxx = 1;
		}
		ret = m_db->Set(ctx.currentDB, key, value, ex, px, nxx);
	}
	if (0 == ret)
	{
		fill_status_reply(ctx.reply, "OK");
	}
	else
	{
		ctx.reply.type = REDIS_REPLY_NIL;
	}
	return 0;
}

int ArdbServer::Get(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	const std::string& key = cmd[0];
	std::string value;
	if (0 == m_db->Get(ctx.currentDB, key, &value))
	{
		fill_str_reply(ctx.reply, value);
	}
	else
	{
		ctx.reply.type = REDIS_REPLY_NIL;
	}
	return 0;
}

int ArdbServer::SetEX(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	uint32 secs;
	if (!string_touint32(cmd[1], secs))
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
		return 0;
	}
	m_db->SetEx(ctx.currentDB, cmd[0], cmd[2], secs);
	fill_status_reply(ctx.reply, "OK");
	return 0;
}
int ArdbServer::SetNX(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->SetNX(ctx.currentDB, cmd[0], cmd[1]);
	fill_int_reply(ctx.reply, ret);
	return 0;
}
int ArdbServer::SetRange(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int32 offset;
	if (!string_toint32(cmd[1], offset))
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
		return 0;
	}
	int ret = m_db->SetRange(ctx.currentDB, cmd[0], offset, cmd[2]);
	fill_int_reply(ctx.reply, ret);
	return 0;
}
int ArdbServer::Strlen(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->Strlen(ctx.currentDB, cmd[0]);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

int ArdbServer::SetBit(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int32 offset, val;
	if (!string_toint32(cmd[1], offset))
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
		return 0;
	}
	if (cmd[2] != "1" && cmd[2] != "0")
	{
		fill_error_reply(ctx.reply,
				"ERR bit is not an integer or out of range");
		return 0;
	}
	uint8 bit = cmd[2] != "1";
	int ret = m_db->SetBit(ctx.currentDB, cmd[0], offset, bit);
	fill_int_reply(ctx.reply, bit);
	return 0;
}

int ArdbServer::PSetEX(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	uint32 mills;
	if (!string_touint32(cmd[1], mills))
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
		return 0;
	}
	m_db->PSetEx(ctx.currentDB, cmd[0], cmd[2], mills);
	fill_status_reply(ctx.reply, "OK");
	return 0;
}

int ArdbServer::MSetNX(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	if (cmd.size() % 2 != 0)
	{
		fill_error_reply(ctx.reply, "ERR wrong number of arguments for MSETNX");
		return 0;
	}
	SliceArray keys;
	SliceArray vals;
	for (int i = 0; i < cmd.size(); i += 2)
	{
		keys.push_back(cmd[i]);
		vals.push_back(cmd[i + 1]);
	}
	int count = m_db->MSetNX(ctx.currentDB, keys, vals);
	fill_int_reply(ctx.reply, count);
	return 0;
}

int ArdbServer::MSet(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	if (cmd.size() % 2 != 0)
	{
		fill_error_reply(ctx.reply, "ERR wrong number of arguments for MSET");
		return 0;
	}
	SliceArray keys;
	SliceArray vals;
	for (int i = 0; i < cmd.size(); i += 2)
	{
		keys.push_back(cmd[i]);
		vals.push_back(cmd[i + 1]);
	}
	m_db->MSet(ctx.currentDB, keys, vals);
	fill_status_reply(ctx.reply, "OK");
	return 0;
}

int ArdbServer::MGet(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray keys;
	for (int i = 0; i < cmd.size(); i++)
	{
		keys.push_back(cmd[i]);
	}
	ValueArray res;
	m_db->MGet(ctx.currentDB, keys, res);
	fill_array_reply(ctx.reply, res);
	return 0;
}

int ArdbServer::IncrbyFloat(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	double increment, val;
	if (!string_todouble(cmd[1], increment))
	{
		fill_error_reply(ctx.reply, "ERR value is not a float or out of range");
		return 0;
	}
	int ret = m_db->IncrbyFloat(ctx.currentDB, cmd[0], increment, val);
	if (ret == 0)
	{
		fill_double_reply(ctx.reply, val);
	}
	else
	{
		fill_error_reply(ctx.reply, "ERR value is not a float or out of range");
	}
	return 0;
}

int ArdbServer::Incrby(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int64 increment, val;
	if (!string_toint64(cmd[1], increment))
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
		return 0;
	}
	int ret = m_db->Incrby(ctx.currentDB, cmd[0], increment, val);
	if (ret == 0)
	{
		fill_int_reply(ctx.reply, val);
	}
	else
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
	}
	return 0;
}

int ArdbServer::Incr(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int64_t val;
	int ret = m_db->Incr(ctx.currentDB, cmd[0], val);
	if (ret == 0)
	{
		fill_int_reply(ctx.reply, val);
	}
	else
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
	}
	return 0;
}

int ArdbServer::GetSet(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	std::string v;
	int ret = m_db->GetSet(ctx.currentDB, cmd[0], cmd[1], v);
	if (ret < 0)
	{
		ctx.reply.type = REDIS_REPLY_NIL;
	}
	else
	{
		fill_str_reply(ctx.reply, v);
	}
	return 0;
}

int ArdbServer::GetRange(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int32 start, end;
	if (!string_toint32(cmd[1], start) || !string_toint32(cmd[2], end))
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
		return 0;
	}
	std::string v;
	m_db->GetRange(ctx.currentDB, cmd[0], start, end, v);
	fill_str_reply(ctx.reply, v);
	return 0;
}

int ArdbServer::GetBit(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int32 offset;
	if (!string_toint32(cmd[1], offset))
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
		return 0;
	}
	int ret = m_db->GetBit(ctx.currentDB, cmd[0], offset);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

int ArdbServer::Decrby(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int64 decrement, val;
	if (!string_toint64(cmd[1], decrement))
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
		return 0;
	}
	int ret = m_db->Decrby(ctx.currentDB, cmd[0], decrement, val);
	if (ret == 0)
	{
		fill_int_reply(ctx.reply, val);
	}
	else
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
	}
	return 0;
}

int ArdbServer::Decr(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int64_t val;
	int ret = m_db->Decr(ctx.currentDB, cmd[0], val);
	if (ret == 0)
	{
		fill_int_reply(ctx.reply, val);
	}
	else
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
	}
	return 0;
}

int ArdbServer::Bitop(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray keys;
	for (int i = 2; i < cmd.size(); i++)
	{
		keys.push_back(cmd[i]);
	}
	int ret = m_db->BitOP(ctx.currentDB, cmd[0], cmd[1], keys);
	if (ret < 0)
	{
		fill_error_reply(ctx.reply, "ERR syntax error");
	}
	else
	{
		fill_int_reply(ctx.reply, ret);
	}
	return 0;
}

int ArdbServer::Bitcount(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	if (cmd.size() == 2)
	{
		fill_error_reply(ctx.reply, "ERR syntax error");
		return 0;
	}
	int count = 0;
	if (cmd.size() == 1)
	{
		count = m_db->BitCount(ctx.currentDB, cmd[0], 0, -1);
	}
	else
	{
		int32 start, end;
		if (!string_toint32(cmd[1], start) || !string_toint32(cmd[2], end))
		{
			fill_error_reply(ctx.reply,
					"ERR value is not an integer or out of range");
			return 0;
		}
		count = m_db->BitCount(ctx.currentDB, cmd[0], start, end);
	}
	fill_int_reply(ctx.reply, count);
	return 0;
}

int ArdbServer::Append(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	const std::string& key = cmd[0];
	const std::string& value = cmd[1];
	int ret = m_db->Append(ctx.currentDB, key, value);
	if (ret > 0)
	{
		fill_int_reply(ctx.reply, ret);
	}
	else
	{
		fill_error_reply(ctx.reply, "ERR failed to append key:%s", key.c_str());
	}
	return 0;
}

int ArdbServer::Ping(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	fill_status_reply(ctx.reply, "PONG");
	return 0;
}
int ArdbServer::Echo(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	ctx.reply.str = cmd[0];
	ctx.reply.type = REDIS_REPLY_STRING;
	return 0;
}
int ArdbServer::Select(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	ctx.currentDB = cmd[0];
	fill_status_reply(ctx.reply, "OK");
	DEBUG_LOG("Select db is %s", cmd[0].c_str());
	return 0;
}

int ArdbServer::Quit(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	fill_status_reply(ctx.reply, "OK");
	return -1;
}

int ArdbServer::Shutdown(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	m_service->Stop();
	return -1;
}

int ArdbServer::Slaveof(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	return 0;
}

int ArdbServer::HMSet(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	if ((cmd.size() - 1) % 2 != 0)
	{
		fill_error_reply(ctx.reply, "ERR wrong number of arguments for HMSet");
		return 0;
	}
	SliceArray fs;
	SliceArray vals;
	for (int i = 1; i < cmd.size(); i += 2)
	{
		fs.push_back(cmd[i]);
		vals.push_back(cmd[i + 1]);
	}
	m_db->HMSet(ctx.currentDB, cmd[0], fs, vals);
	fill_status_reply(ctx.reply, "OK");
	return 0;
}
int ArdbServer::HSet(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->HSet(ctx.currentDB, cmd[0], cmd[1], cmd[2]);
	fill_int_reply(ctx.reply, 1);
	return 0;
}
int ArdbServer::HSetNX(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->HSetNX(ctx.currentDB, cmd[0], cmd[1], cmd[2]);
	fill_int_reply(ctx.reply, ret);
	return 0;
}
int ArdbServer::HVals(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	StringArray keys;
	m_db->HVals(ctx.currentDB, cmd[0], keys);
	fill_str_array_reply(ctx.reply, keys);
	return 0;
}

int ArdbServer::HMGet(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	ValueArray vals;
	SliceArray fs;
	for (int i = 1; i < cmd.size(); i++)
	{
		fs.push_back(cmd[i]);
	}
	m_db->HMGet(ctx.currentDB, cmd[0], fs, vals);
	fill_array_reply(ctx.reply, vals);
	return 0;
}

int ArdbServer::HLen(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int len = m_db->HLen(ctx.currentDB, cmd[0]);
	fill_int_reply(ctx.reply, len);
	return 0;
}

int ArdbServer::HKeys(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	StringArray keys;
	m_db->HKeys(ctx.currentDB, cmd[0], keys);
	fill_str_array_reply(ctx.reply, keys);
	return 0;
}

int ArdbServer::HIncrbyFloat(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	double increment, val = 0;
	if (!string_todouble(cmd[2], increment))
	{
		fill_error_reply(ctx.reply, "ERR value is not a float or out of range");
		return 0;
	}
	m_db->HIncrbyFloat(ctx.currentDB, cmd[0], cmd[1], increment, val);
	fill_double_reply(ctx.reply, val);
	return 0;
}

int ArdbServer::HIncrby(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int64 increment, val = 0;
	if (!string_toint64(cmd[2], increment))
	{
		fill_error_reply(ctx.reply,
				"ERR value is not an integer or out of range");
		return 0;
	}
	m_db->HIncrby(ctx.currentDB, cmd[0], cmd[1], increment, val);
	fill_int_reply(ctx.reply, val);
	return 0;
}

int ArdbServer::HGetAll(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	StringArray fields;
	ValueArray results;
	int ret = m_db->HGetAll(ctx.currentDB, cmd[0], fields, results);
	ctx.reply.type = REDIS_REPLY_ARRAY;
	for (int i = 0; i < fields.size(); i++)
	{
		ArdbReply reply1, reply2;
		fill_str_reply(reply1, fields[i]);
		fill_str_reply(reply2, results[i].ToString());
		ctx.reply.elements.push_back(reply1);
		ctx.reply.elements.push_back(reply2);
	}
	return 0;
}

int ArdbServer::HGet(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	std::string v;
	int ret = m_db->HGet(ctx.currentDB, cmd[0], cmd[1], &v);
	if (ret < 0)
	{
		ctx.reply.type = REDIS_REPLY_NIL;
	}
	else
	{
		fill_str_reply(ctx.reply, v);
	}
	return 0;
}

int ArdbServer::HExists(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->HExists(ctx.currentDB, cmd[0], cmd[1]);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

int ArdbServer::HDel(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray fields;
	for (int i = 1; i < cmd.size(); i++)
	{
		fields.push_back(cmd[i]);
	}
	int ret = m_db->HDel(ctx.currentDB, cmd[0], fields);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

//========================Set CMDs==============================
int ArdbServer::SAdd(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray values;
	for (int i = 1; i < cmd.size(); i++)
	{
		values.push_back(cmd[i]);
	}
	int count = m_db->SAdd(ctx.currentDB, cmd[0], values);
	fill_int_reply(ctx.reply, count);
	return 0;
}

int ArdbServer::SCard(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->SCard(ctx.currentDB, cmd[0]);
	fill_int_reply(ctx.reply, ret > 0 ? ret : 0);
	return 0;
}

int ArdbServer::SDiff(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray keys;
	for (int i = 0; i < cmd.size(); i++)
	{
		keys.push_back(cmd[i]);
	}
	ValueSet vs;
	m_db->SDiff(ctx.currentDB, keys, vs);
	fill_array_reply(ctx.reply, vs);
	return 0;
}

int ArdbServer::SDiffStore(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray keys;
	for (int i = 1; i < cmd.size(); i++)
	{
		keys.push_back(cmd[i]);
	}
	ValueSet vs;
	int ret = m_db->SDiffStore(ctx.currentDB, cmd[0], keys);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

int ArdbServer::SInter(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray keys;
	for (int i = 0; i < cmd.size(); i++)
	{
		keys.push_back(cmd[i]);
	}
	ValueSet vs;
	m_db->SInter(ctx.currentDB, keys, vs);
	fill_array_reply(ctx.reply, vs);
	return 0;
}

int ArdbServer::SInterStore(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray keys;
	for (int i = 1; i < cmd.size(); i++)
	{
		keys.push_back(cmd[i]);
	}
	ValueSet vs;
	int ret = m_db->SInterStore(ctx.currentDB, cmd[0], keys);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

int ArdbServer::SIsMember(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->SIsMember(ctx.currentDB, cmd[0], cmd[1]);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

int ArdbServer::SMembers(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	ValueArray vs;
	m_db->SMembers(ctx.currentDB, cmd[0], vs);
	fill_array_reply(ctx.reply, vs);
	return 0;
}

int ArdbServer::SMove(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->SMove(ctx.currentDB, cmd[0], cmd[1], cmd[2]);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

int ArdbServer::SPop(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	std::string res;
	m_db->SPop(ctx.currentDB, cmd[0], res);
	fill_str_reply(ctx.reply, res);
	return 0;
}

int ArdbServer::SRandMember(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	ValueArray vs;
	int32 count = 1;
	if (cmd.size() > 1)
	{
		if (!string_toint32(cmd[1], count))
		{
			fill_error_reply(ctx.reply,
							"ERR value is not an integer or out of range");
			return 0;
		}
	}
	m_db->SRandMember(ctx.currentDB, cmd[0], vs, count);
	fill_array_reply(ctx.reply, vs);
	return 0;
}

int ArdbServer::SRem(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray keys;
	for (int i = 1; i < cmd.size(); i++)
	{
		keys.push_back(cmd[i]);
	}
	ValueSet vs;
	int ret = m_db->SRem(ctx.currentDB, cmd[0], keys);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

int ArdbServer::SUnion(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray keys;
	for (int i = 0; i < cmd.size(); i++)
	{
		keys.push_back(cmd[i]);
	}
	ValueSet vs;
	m_db->SUnion(ctx.currentDB, keys, vs);
	fill_array_reply(ctx.reply, vs);
	return 0;
}

int ArdbServer::SUnionStore(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	SliceArray keys;
	for (int i = 1; i < cmd.size(); i++)
	{
		keys.push_back(cmd[i]);
	}
	ValueSet vs;
	int ret = m_db->SUnionStore(ctx.currentDB, cmd[0], keys);
	fill_int_reply(ctx.reply, ret);
	return 0;
}


//===========================Sorted Sets cmds==============================
int ArdbServer::ZAdd(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	if ((cmd.size() - 1) % 2 != 0)
	{
		fill_error_reply(ctx.reply, "ERR wrong number of arguments for ZAdd");
		return 0;
	}
	m_db->Multi(ctx.currentDB);
	for(int i = 1 ; i < cmd.size(); i+=2){
	    double score;
	    if (!string_todouble(cmd[i], score))
	    {
			fill_error_reply(ctx.reply,
					"ERR value is not a float or out of range");
			m_db->Discard(ctx.currentDB);
			return 0;
		}
	    m_db->ZAdd(ctx.currentDB, cmd[0], score, cmd[i+1]);
	}
	m_db->Exec(ctx.currentDB);
	return 0;
}

int ArdbServer::ZCard(ArdbConnContext& ctx, ArgumentArray& cmd)
{
	int ret = m_db->ZCard(ctx.currentDB, cmd[0]);
	fill_int_reply(ctx.reply, ret);
	return 0;
}

int ArdbServer::ZCount(ArdbConnContext& ctx, ArgumentArray& cmd){
    int ret = m_db->ZCount(ctx.currentDB, cmd[0], cmd[1], cmd[2]);
    fill_int_reply(ctx.reply, ret);
    	return 0;
}

int ArdbServer::ZIncrby(ArdbConnContext& ctx, ArgumentArray& cmd){
	double increment, value;
	m_db->ZIncrby(ctx.currentDB, cmd[0], increment, cmd[2], value);
    return 0;
}

int ArdbServer::ZRange(ArdbConnContext& ctx, ArgumentArray& cmd){
	bool withscores = false;
	if(cmd.size() == 4){
		if(string_tolower(cmd[3]) !=  "WITHSCORES"){
			fill_error_reply(ctx.reply,
								"ERR syntax error");
			return 0;
		}
		withscores = true;
	}
	int start, stop;
	if (!string_toint32(cmd[1], start) || !string_toint32(cmd[2], stop))
	{
		fill_error_reply(ctx.reply, "ERR value is not an integer or out of range");
		return 0;
	}
	QueryOptions options;
	options.withscores = withscores;
    ValueArray vs;
	m_db->ZRange(ctx.currentDB, cmd[0], start, stop, vs, options);
	return 0;
}

int ArdbServer::ZScore(ArdbConnContext& ctx, ArgumentArray& cmd){
	double score ;
	int ret = m_db->ZScore(ctx.currentDB, cmd[0], cmd[1], score);
	if(ret < 0){
		ctx.reply.type = REDIS_REPLY_NIL;
	}else{
		fill_double_reply(ctx.reply, score);
	}
	return 0;
}

void ArdbServer::ProcessRedisCommand(ArdbConnContext& ctx,
		RedisCommandFrame& args)
{
	ctx.reply.Clear();
	std::string* cmd = args.GetArgument(0);
	if (NULL != cmd)
	{
		int ret = 0;
		lower_string(*cmd);
		RedisCommandHandlerSettingTable::iterator found = m_handler_table.find(
				*cmd);
		if (found != m_handler_table.end())
		{
			RedisCommandHandler handler = found->second.handler;
			args.GetArguments().pop_front();

			bool valid_cmd = true;
			if (found->second.min_arity >= 0)
			{
				valid_cmd = args.GetArguments().size()
						>= found->second.min_arity;
			}
			if (found->second.max_arity >= 0 && valid_cmd)
			{
				valid_cmd = args.GetArguments().size()
						<= found->second.max_arity;
			}

			if (!valid_cmd)
			{
				fill_error_reply(ctx.reply,
						"ERR wrong number of arguments for '%s' command",
						cmd->c_str());
			}
			else
			{
				ret = (this->*handler)(ctx, args.GetArguments());
			}

		}
		else
		{
			ERROR_LOG("No handler found for:%s", cmd->c_str());
			fill_error_reply(ctx.reply, "ERR unknown command '%s'",
					cmd->c_str());
		}

		Buffer buf;
		if (ctx.reply.type != 0)
		{
			Buffer buf;
			encode_reply(buf, ctx.reply);
			ctx.conn->Write(buf);
		}
		if (ret < 0)
		{
			ctx.conn->Close();
		}
	}
}

static void ardb_pipeline_init(ChannelPipeline* pipeline, void* data)
{
	ChannelUpstreamHandler<RedisCommandFrame>* handler =
			(ChannelUpstreamHandler<RedisCommandFrame>*) data;
	pipeline->AddLast("decoder", new RedisFrameDecoder);
	pipeline->AddLast("handler", handler);
}

static void ardb_pipeline_finallize(ChannelPipeline* pipeline, void* data)
{
	ChannelHandler* handler = pipeline->Get("decoder");
	DELETE(handler);
}

int ArdbServer::Start(const Properties& props)
{
	ParseConfig(props, m_cfg);
	struct RedisRequestHandler: public ChannelUpstreamHandler<RedisCommandFrame>
	{
		ArdbServer* server;
		ArdbConnContext ardbctx;
		void MessageReceived(ChannelHandlerContext& ctx,
				MessageEvent<RedisCommandFrame>& e)
		{
			ardbctx.conn = ctx.GetChannel();
			server->ProcessRedisCommand(ardbctx, *(e.GetMessage()));
		}
		RedisRequestHandler(ArdbServer* s) :
				server(s)
		{
		}
	};
#ifdef __USE_KYOTOCABINET__
	m_engine = new KCDBEngineFactory(props);
#else
	m_engine = new LevelDBEngineFactory(props);
#endif
	m_db = new Ardb(m_engine);
	m_service = new ChannelService(m_cfg.max_clients + 32);
	RedisRequestHandler handler(this);

	ChannelOptions ops;
	ops.tcp_nodelay = true;
	if (m_cfg.listen_host.empty() && m_cfg.listen_unix_path.empty())
	{
		m_cfg.listen_host = "0.0.0.0";
		if (m_cfg.listen_port == 0)
		{
			m_cfg.listen_port = 6379;
		}
	}
	if (!m_cfg.listen_host.empty())
	{
		SocketHostAddress address(m_cfg.listen_host.c_str(), m_cfg.listen_port);
		ServerSocketChannel* server = m_service->NewServerSocketChannel();
		if (!server->Bind(&address))
		{
			ERROR_LOG(
					"Failed to bind on %s:%d", m_cfg.listen_host.c_str(), m_cfg.listen_port);
			return -1;
		}
		server->Configure(ops);
		server->SetChannelPipelineInitializor(ardb_pipeline_init, &handler);
		server->SetChannelPipelineFinalizer(ardb_pipeline_finallize, NULL);
	}
	if (!m_cfg.listen_unix_path.empty())
	{
		SocketUnixAddress address(m_cfg.listen_unix_path);
		ServerSocketChannel* server = m_service->NewServerSocketChannel();
		if (!server->Bind(&address))
		{
			ERROR_LOG( "Failed to bind on %s", m_cfg.listen_unix_path.c_str());
			return -1;
		}
		server->Configure(ops);
		server->SetChannelPipelineInitializor(ardb_pipeline_init, &handler);
		server->SetChannelPipelineFinalizer(ardb_pipeline_finallize, NULL);
	}
	m_service->Start();
	DELETE(m_engine);
	DELETE(m_db);
	DELETE(m_service);
	return 0;
}
}
