#ifndef __COMMON_NET_H__
#define __COMMON_NET_H__
#include <netinet/in.h>
#include <string>
namespace net
{
	using _TyConnectionId = evutil_socket_t;
	enum class event
	{
		unknown = 0,
		connected,
		request,
		disconnected,
		error,
		timeout
	};


	bool IsThreadEnable();
	void EnvInitialize();
	bool FmtAddress(struct ::sockaddr_in& addr, int nPort, const std::string& strAddr = "");

	std::string ParseSockAddr(std::string& strAddr, int& nPort, const struct sockaddr& addr);
	std::string ParseSockAddr(std::string& strAddr, int& nPort, const struct sockaddr_storage& addr);

	bool CheckSockAddress(struct sockaddr* pAddr, int nLength);
	bool SockAddrSafeCopy(struct sockaddr& dst, const struct sockaddr& src);
	bool SockAddrSafeCopy(struct sockaddr_storage& dst, const struct sockaddr& src);
} // namespace net

#endif
