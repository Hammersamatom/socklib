#include "socks.hpp"
#include "errors.hpp"
#include "initialization.hpp"
#include "macros.hpp"
extern "C" {
	#ifdef __SKS_AS_POSIX__
		#include <sys/socket.h> //general socket
		#include <unistd.h> //close(...) and unlink(...)
		#include <poll.h> //poll(...)
		#include <unistd.h> //unlink(...)
		#include <sys/time.h> //timeval
		#include <sys/ioctl.h>
	#elif defined __SKS_AS_WINDOWS__
		#include <ws2tcpip.h> //WinSock 2
		//#include <io.h> //_mktemp
		//#include <fileapi.h> //to get temp dir
		//#include <shlwapi.h> //PathCombineA
		#pragma comment(lib, "Ws2_32.lib")

		#define MSG_NOSIGNAL 0 //Windows does not have this flag, and instead has SO_NOSIGPIPE

		#define poll WSAPoll
		#define POLLIN POLLRDNORM
		#define POLLOUT POLLWRNORM
		#define ssize_t int
		#define errno WSAGetLastError() //Acceptable, but only if reading socket errors, per https://docs.microsoft.com/en-us/windows/win32/winsock/error-codes-errno-h-errno-and-wsagetlasterror-2
	#endif
}
#include <vector>
#include <chrono>
#include <csignal>

namespace sks {
	const versionInfo version = { 0, 10, 0 };

	socket::socket(domain d, type t, int protocol) {
		if (autoInitialize) {
			initialize();
		}

		m_sockFD = ::socket(d, t, protocol);
		//On error, -1 is returned, and errno is set appropriately.
		#ifdef __SKS_AS_POSIX__
			if (m_sockFD == -1) {
		#elif defined __SKS_AS_WINDOWS__
			if (m_sockFD == INVALID_SOCKET) {
		#endif
			//Error creating socket; check errno for:
			/*-----------------------------------------------------------------------------------------------------------------------\
			|     EACCES      | Permission to create a socket of the specified type and/or protocol is denied.                       |
			|  EAFNOSUPPORT   | The implementation does not support the specified address family.                                    |
			|     EINVAL      | Unknown protocol, or protocol family not available.                                                  |
			|     EINVAL      | Invalid flags in type.                                                                               |
			|     EMFILE      | Process file table overflow.                                                                         |
			|     ENFILE      | The system limit on the total number of open files has been reached.                                 |
			| ENOBUFS/ENOMEM  | Insufficient memory is available. The socket cannot be created until sufficient resources are freed. |
			| EPROTONOSUPPORT | The protocol type or the specified protocol is not supported within this domain.                     |
			\------------------------------------------------------------------------------------------------------------------------/
			Other errors may be generated by the underlying protocol modules. A catch-all should also be present for this.
			*/
			throw sysErr(errno);
		}
		//Socket created successfully by this line
		m_validFD = true;
		m_domain = d;
		m_type = t;
		m_protocol = protocol;

		#ifdef __SKS_AS_WINDOWS__
		//Windows doesn't support MSG_NOSIGNAL, but does(?) support SO_NOSIGPIPE option, which achives the same results
		//Windows also apparently doesn't use the pipe signal???
		#endif
	}
	
	socket::socket(int sockFD, domain d, type t, int protocol) {
		//We already have a socket fd, we have to figure out some info now
		//sd_is_socket(sockFD, d, t, -1); //Can be used to check if the fd is for a socket of given domain and type; I don't know if its POSIX so it stays commented-out
		
		m_sockFD = sockFD;
		m_validFD = true;
		m_domain = d;
		m_type = t;
		m_protocol = protocol;
		if (autoInitialize) {
			initialize();
		}
	}
	
	socket::socket(socket&& s) {
		//this socket should be identical to other socket s
		//other socket should be left invalid
		std::swap(m_validFD, s.m_validFD);
		std::swap(m_sockFD, s.m_sockFD);
		std::swap(m_domain, s.m_domain);
		std::swap(m_type, s.m_type);
		std::swap(m_protocol, s.m_protocol);
	}

	socket::~socket() {
		//If we have just done a move operation on this socket, file descriptor should not be touched/read
		if (m_validFD) {
			//(Potentially) used later, but cannot be got after shutdown and closing
			#ifdef __SKS_AS_POSIX__
				address local = localAddress();
			#endif
			//Shutdown socket
			//This makes sure all remaining bytes are sent to network before closing it up
			//Long story short, no data is lost unless it is lost while traversing the network
			//Also may send protocol info, such as a FIN for TCP
			#ifdef __SKS_AS_POSIX__
				shutdown(m_sockFD, SHUT_RDWR);
			#else
				shutdown(m_sockFD, SD_BOTH);
			#endif
			//On error, -1 shall be returned and errno set to indicate the error.
			//For the reasons below, no checking is done on shutdown(...)
			/*-----------------------------------------------------------------------------------------\
			|  EBADF   | The socket argument is not a valid file descriptor.                           | EBADF isn't checked since we personally handle the fd
			|  EINVAL  | The how argument is invalid.                                                  | EINVAL is a set value and will not happen in normal operation
			| ENOTCONN | The socket is not connected.                                                  | ENOTCONN may occur here under normal operation, ignore if it does
			| ENOTSOCK | The socket argument does not refer to a socket.                               | ENOTSOCK isn't checked since we personally handle the fd
			| ENOBUFS  | Insufficient resources were available in the system to perform the operation. |
			\-----------------------------------------------------------------------------------------*/
			
			//Close socket fully
			//Any bytes not transmitted are gone
			//I'm not certain, but I think this isn't a formal/clean close either
			#ifdef __SKS_AS_POSIX__
				int e = close(m_sockFD);
			#else
				int e = closesocket(m_sockFD);
			#endif
			//On error, -1 is returned, and errno is set appropriately.
			if (e == -1) {
				//Error closing socket
				/*---------------------------------------------------------------------\
				| EBADF | fd isn't a valid open file descriptor.                       | EBADF isn't checked since we personally handle the fd
				| EINTR | The close() call was interrupted by a signal; see signal(7). |
				|  EIO  | An I/O error occurred.                                       |
				\---------------------------------------------------------------------*/
				
				//throw sysErr(errno);
				//Throwing in a deconstructor is bad for reasons, so don't throw exceptions
			}
			
			#ifdef __SKS_AS_POSIX__
				if (m_domain == unix) {
					unixAddress localUnix = (unixAddress)local;
					if (localUnix.named()) {
						//We are currently bound to a named unix address, unlink it
						unlink(localUnix.name().c_str());
					}
				}
			#endif

			if (autoInitialize) {
				deinitialize();
			}
		}
	}

	socket& socket::operator=(socket&& s) {
		std::swap(m_validFD, s.m_validFD);
		std::swap(m_sockFD, s.m_sockFD);
		std::swap(m_domain, s.m_domain);
		std::swap(m_type, s.m_type);
		std::swap(m_protocol, s.m_protocol);
		return *this;
	}

	bool socket::operator==(const socket& r) const {
		return m_sockFD == r.m_sockFD;
	}
	bool socket::operator!=(const socket& r) const {
		return m_sockFD != r.m_sockFD;
	}
	
	void socket::bind(const address& address) {
		sockaddr_storage addr = address;
		return bind((sockaddr*)&addr, address.size());
	}
	void socket::bind(const sockaddr* address, socklen_t len) {
		//Make sure domain of address matches that of this socket
		if (address->sa_family != m_domain) {
			throw sysErr(EFAULT); //Bad Address, since domain mis-matched between address and this socket
		}
		//Do binding
		int e = ::bind(m_sockFD, address, len);
		//On error, -1 is returned, and errno is set appropriately.
		if (e == -1) {
			//NOTICE: This table (from man bind(2) isn't fully correct, EFAULT can be given if address has mismatching AF with socket)
			/*--------------------------------------------------------------------------\
			|   EACCES   | The address is protected, and the user is not the superuser. |
			| EADDRINUSE | The given address is already in use.                         |
			|   EBADF    | sockfd is not a valid descriptor.                            |
			|   EINVAL   | The socket is already bound to an address.                   |
			|  ENOTSOCK  | sockfd is a descriptor for a file, not a socket.             |
			\---------------------------------------------------------------------------/
			EBADF, ENOTSOCK aren't checked since we personally handle the fd
			*/
			//The following errors are specific to UNIX domain (AF_UNIX) sockets:
			/*--------------------------------------------------------------------------------------------------------------\
			|    EACCES     | Search permission is denied on a component of the path prefix. (See also path_resolution(7).) |
			| EADDRNOTAVAIL | A nonexistent interface was requested or the requested address was not local.                 |
			|    EFAULT     | addr points outside the user's accessible address space.                                      |
			|    EINVAL     | The addrlen is wrong, or the socket was not in the AF_UNIX family.                            |
			|     ELOOP     | Too many symbolic links were encountered in resolving addr.                                   |
			| ENAMETOOLONG  | addr is too long.                                                                             |
			|    ENOENT     | The file does not exist.                                                                      |
			|    ENOMEM     | Insufficient kernel memory was available.                                                     |
			|    ENOTDIR    | A component of the path prefix is not a directory.                                            |
			|     EROFS     | The socket inode would reside on a read-only file system.                                     |
			\---------------------------------------------------------------------------------------------------------------/
			EINVAL IS checked since the addressUnix constructor asks for length, but only for the first half since domain check is done seperately
			*/
			throw sysErr(errno);
		}
	}
	
	void socket::listen(int backlog) {
		int e = ::listen(m_sockFD, backlog);
		//On error, -1 is returned, and errno is set appropriately.
		if (e == -1) {
			/*-------------------------------------------------------------------------------\
			| EADDRINUSE | Another socket is already listening on the same port.             |
			|   EBADF    | The argument sockfd is not a valid descriptor.                    |
			|  ENOTSOCK  | The argument sockfd is not a socket.                              |
			| EOPNOTSUPP | The socket is not of a type that supports the listen() operation. |
			\--------------------------------------------------------------------------------/
			*/
			throw sysErr(errno);
		}
	}
	
	socket socket::accept() {
		int peerFD = ::accept(m_sockFD, nullptr, nullptr);
		//On error, -1 is returned, and errno is set appropriately.
		if (peerFD == -1) {
			throw sysErr(errno);
		}
		//We have the file descriptor, construct a socket (class) around it
		socket peer(peerFD, m_domain, m_type, m_protocol);
		return peer;
	}
	
	void socket::connect(const address& address) {
		sockaddr_storage addr = address;
		return connect((sockaddr*)&addr, address.size());
	}
	void socket::connect(const sockaddr* address, socklen_t len) {
		int e = ::connect(m_sockFD, address, len);
		//On error, -1 is returned, and errno is set appropriately.
		if (e == -1) {
			throw sysErr(errno);
		}
		//Nothing went wrong! We are now connected and theoretically ready to send/receive data
	}
	
	void socket::send(const std::vector<uint8_t>& data, int flags) {
		return send(data.data(), data.size(), flags);
	}
	void socket::send(const uint8_t* data, size_t len, int flags) {
		size_t sent = 0;
		//send may not send all data at once, so we have a loop here
		while (sent < len) {
			//NOTE: SIGPIPE is suppressed by MSG_NOSIGNAL
			ssize_t r = ::send(m_sockFD, (const char*)data + sent, len - sent, flags | MSG_NOSIGNAL);
			//On success, these calls return the number of characters sent. On error, -1 is returned, and errno is set appropriately.
			if (r == -1) {
				throw sysErr(errno);
			}
			sent += r; //We sent r bytes with this send
		}
	}
	void socket::send(const std::vector<uint8_t>& data, const address& to, int flags) {
		return send(data.data(), data.size(), to, flags);
	}
	void socket::send(const uint8_t* data, size_t len, const address& to, int flags) {
		sockaddr_storage addr = to;
		return send(data, len, (sockaddr*)&addr, to.size(), flags);
	}
	void socket::send(const std::vector<uint8_t>& data, const sockaddr* toAddr, socklen_t addrLen, int flags) {
		return send(data.data(), data.size(), toAddr, addrLen, flags);
	}
	void socket::send(const uint8_t* data, size_t len, const sockaddr* toAddr, socklen_t addrLen, int flags) {
		size_t sent = 0;
		//send may not send all data at once, so we have a loop here
		while (sent < len) {
			ssize_t r = ::sendto(m_sockFD, (const char*)data + sent, len - sent, flags | MSG_NOSIGNAL, toAddr, addrLen);
			if (r == -1) {
				throw sysErr(errno);
			}
			sent += r; //We sent r bytes with this send
		}
	}
	
	std::vector<uint8_t> socket::receive(size_t bufSize, int flags) {
		std::vector<uint8_t> buffer(bufSize);
		size_t recvSize = receive(buffer.data(), buffer.size(), flags);
		buffer.resize(recvSize);
		return buffer;
	}
	size_t socket::receive(uint8_t* buf, size_t bufSize, int flags) {
		ssize_t r = recv(m_sockFD, (char*)buf, bufSize, flags | MSG_NOSIGNAL);
		if (r == -1) {
			throw sysErr(errno);
		}
		return r;
	}
	std::vector<uint8_t> socket::receive(address& from, size_t bufSize, int flags) {
		std::vector<uint8_t> buffer(bufSize);
		size_t recvSize = receive(from, buffer.data(), buffer.size(), flags);
		buffer.resize(recvSize);
		return buffer;
	}
	size_t socket::receive(address& from, uint8_t* buf, size_t bufSize, int flags) {
		sockaddr_storage addr;
		socklen_t addrLen = sizeof(addr);
		size_t recvSize = receive((sockaddr*)&addr, &addrLen, buf, bufSize, flags);
		from = address(addr, addrLen);
		return recvSize;
	}
	std::vector<uint8_t> socket::receive(sockaddr* fromAddr, socklen_t* addrLen, size_t bufSize, int flags) {
		std::vector<uint8_t> buffer(bufSize);
		size_t recvSize = receive(fromAddr, addrLen, buffer.data(), buffer.size(), flags);
		buffer.resize(recvSize);
		return buffer;
	}
	size_t socket::receive(sockaddr* fromAddr, socklen_t* addrLen, uint8_t* buf, size_t bufSize, int flags) {
		ssize_t r = recvfrom(m_sockFD, (char*)buf, bufSize, flags | MSG_NOSIGNAL, fromAddr, addrLen);
		if (r == -1) {
			throw sysErr(errno);
		}
		return r;
	}
	
	#ifdef __SKS_AS_POSIX__
		typedef timeval timeoutT;
		timeval microsecondsToTimeoutT(std::chrono::microseconds us) {
			timeval tv;
			tv.tv_sec = us.count() / 1000000;
			tv.tv_usec = us.count() % 1000000;
			return tv;
		}
		std::chrono::microseconds timeoutTToMicroseconds(timeval tv) {
			return std::chrono::microseconds(tv.tv_usec) + std::chrono::seconds(tv.tv_sec);
		}
	#else
		typedef DWORD timeoutT;
		DWORD microsecondsToTimeoutT(std::chrono::microseconds us) {
			return us.count() / 1000;
		}
		std::chrono::microseconds timeoutTToMicroseconds(DWORD ms) {
			return std::chrono::microseconds(ms * 1000);
		}
	#endif

	void socket::sendTimeout(std::chrono::microseconds timeout) {
		//Set the tx timeout option
		timeoutT tv = microsecondsToTimeoutT(timeout);
		int e = setsockopt(m_sockFD, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
		if (e == -1) {
			throw sysErr(errno);
		}
	}
	std::chrono::microseconds socket::sendTimeout() const {
		timeoutT tv;
		socklen_t tvl = sizeof(tv);
		int e = getsockopt(m_sockFD, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, &tvl);
		if (e == -1) {
			throw sysErr(errno);
		}
		
		return timeoutTToMicroseconds(tv);
	}
	
	void socket::receiveTimeout(std::chrono::microseconds timeout) {
		//Set the rx timeout option
		timeoutT tv = microsecondsToTimeoutT(timeout);
		int e = setsockopt(m_sockFD, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
		if (e == -1) {
			throw sysErr(errno);
		}
	}
	std::chrono::microseconds socket::receiveTimeout() const {
		timeoutT tv;
		socklen_t tvl = sizeof(tv);
		int e = getsockopt(m_sockFD, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, &tvl);
		if (e == -1) {
			throw sysErr(errno);
		}

		return timeoutTToMicroseconds(tv);
	}

	bool socket::writeReady(std::chrono::milliseconds timeout) const {
		//Check if the socket can be written to, waiting for up to <timeout> milliseconds
		pollfd pfd;
		pfd.fd = m_sockFD;
		pfd.events = POLLOUT;
		pfd.revents = 0; //Zero it out since we read later and don't want any issues
		int r = poll(&pfd, 1, timeout.count());
		#ifdef __SKS_AS_POSIX__
			if (r == -1) {
		#else
			if (r == SOCKET_ERROR) {
		#endif
			throw sysErr(errno);
		}
		//POLLHUP and POLLOUT are mutually exclusive; this function will return false if socket disconnects

		return (pfd.revents & POLLOUT) == POLLOUT;
	}
	bool socket::readReady(std::chrono::milliseconds timeout) const {
		pollfd pfd;
		pfd.fd = m_sockFD;
		pfd.events = POLLIN;
		pfd.revents = 0; //Zero it out since we read later and don't want any issues
		int r = poll(&pfd, 1, timeout.count());
		#ifdef __SKS_AS_POSIX__
			if (r == -1) {
		#else
			if (r == SOCKET_ERROR) {
		#endif
			throw sysErr(errno);
		}

		return (pfd.revents & POLLIN) == POLLIN;
	}
	size_t socket::bytesReady() const {
		#ifdef __SKS_AS_POSIX__
			int bytes;
			int r = ioctl(m_sockFD, FIONREAD, &bytes);
			if (r == -1) {
				throw sysErr(errno);
			}
		#else
			unsigned long bytes;
			int r = ioctlsocket(m_sockFD, FIONREAD, &bytes);
			if (r != 0) {
				throw sysErr(errno);
			}
		#endif

		return bytes;
	}
	
	void socket::socketOption(boolOption option, bool value, optionLevel level) {
		int boolConv = value;
		int e = setsockopt(m_sockFD, level, option, (const char*)&boolConv, sizeof(boolConv));
		if (e == -1) {
			throw sysErr(errno);
		}
	}
	bool socket::socketOption(boolOption option, optionLevel level) const {
		bool value;
		socklen_t len;
		int e = getsockopt(m_sockFD, level, option, (char*)&value, &len);
		if (e == -1) {
			throw sysErr(errno);
		}
		return value;
	}
	void socket::socketOption(intOption option, int value, optionLevel level) {
		int e = setsockopt(m_sockFD, level, option, (const char*)&value, sizeof(value));
		if (e == -1) {
			throw sysErr(errno);
		}
	}
	int socket::socketOption(intOption option, optionLevel level) const {
		int value;
		socklen_t len;
		int e = getsockopt(m_sockFD, level, option, (char*)&value, &len);
		if (e == -1) {
			throw sysErr(errno);
		}
		return value;
	}
	
	address socket::connectedAddress() const {
		sockaddr_storage sa;
		socklen_t salen = sizeof(sa);
		connectedAddress((sockaddr*)&sa, &salen);
		return address(sa, salen);
	}
	void socket::connectedAddress(sockaddr* addr, socklen_t* len) const {
		int e = getpeername(m_sockFD, addr, len);
		if (e == -1) {
			throw sysErr(errno);
		}
	}
	address socket::localAddress() const {
		sockaddr_storage sa;
		socklen_t salen = sizeof(sa);
		localAddress((sockaddr*)&sa, &salen);
		return address(sa, salen);
	}
	void socket::localAddress(sockaddr* addr, socklen_t* len) const {
		int e = getsockname(m_sockFD, addr, len);
		if (e == -1) {
			throw sysErr(errno);
		}
	}

	int socket::socketFD(bool takeOwnership) {
		if (takeOwnership) {
			m_validFD = false; //We have lost ownership. Do not do anything with socket when deconstructing
		}
		return m_sockFD;
	}
	int socket::socketFD() const {
		return m_sockFD;
	}



	std::pair<socket, socket> createUnixPair(type t, int protocol) {
		const domain d = unix;
		#ifdef __SKS_AS_POSIX__
			//Create two sockets with given params
			int FDs[2];
			int e = socketpair(d, t, protocol, FDs); //Technically, there could be some system that allows more than just unix sockets, but its unlikely and not particularly hard to DIY
			if (e == -1) {
				throw sysErr(errno);
			}
			//We have two socket file descriptors, wrap into classes then put into return pair
			return std::pair<socket, socket>{ socket(FDs[0], d, t, protocol), socket(FDs[1], d, t, protocol) };
		#else
			//Windows special, windows no fork, windows require workaround for same function
			//windows may not even need, but windows gets
			/*char* tempname = _mktemp("unix-XXXXXXXXXXXX"); //12 chars random + ".unix" + '\0'
			char tmpDir[MAX_PATH];
			int r = GetTempPathA(MAX_PATH, tmpDir);
			if (r != 0) {
				//Issues getting temp path
			}
			char tmpFile[MAX_PATH];
			LPSTR r2 = PathCombineA(tmpFile, tmpDir, tempname);
			if (r2 != tmpFile) {
				//Issues combining paths
			}

			std::string listenerFile = tmpFile;
			sks::unixAddress listenerAddress(listenerFile);

			//We now have a temp file we can bind a unix socket to
			socket listener(unix, t, protocol);
			listener.bind(listenerAddress);
			u_long nonblock = 1;
			r = ioctlsocket(listener.m_sockFD, FIONBIO, &nonblock);
			if (r != 0) {
				//Couln't set non-blocking
			}
			listener.listen(1);*/
			throw std::runtime_error("createUnixPair is not implemented for windows systems.");
		#endif
	}

	std::vector<std::reference_wrapper<socket>> writeReadySockets(std::vector<std::reference_wrapper<socket>>& sockets, std::chrono::milliseconds timeout) {
		std::vector<pollfd> pollstructs;
		for (size_t i = 0; i < sockets.size(); i++) {
			pollfd pfd;
			pfd.fd = sockets[i].get().m_sockFD;
			pfd.events = POLLOUT;
			pfd.revents = 0; //Zero it out since we read later and don't want any issues
			pollstructs.push_back(pfd);
		}

		//Do poll
		int r = poll(pollstructs.data(), pollstructs.size(), timeout.count());
		if (r == -1) {
			throw sysErr(errno);
		}

		std::vector<std::reference_wrapper<socket>> readySockets;
		for (size_t i = 0; i < pollstructs.size(); i++) {
			if ((pollstructs[i].revents & POLLOUT) == POLLOUT) {
				//Write ready
				readySockets.push_back(sockets[i]);
			}
		}
		return readySockets;
	}
	std::vector<std::reference_wrapper<socket>> readReadySockets(std::vector<std::reference_wrapper<socket>>& sockets, std::chrono::milliseconds timeout) {
		std::vector<pollfd> pollstructs;
		for (size_t i = 0; i < sockets.size(); i++) {
			pollfd pfd;
			pfd.fd = sockets[i].get().m_sockFD;
			pfd.events = POLLIN;
			pfd.revents = 0; //Zero it out since we read later and don't want any issues
			pollstructs.push_back(pfd);
		}

		//Do poll
		int r = poll(pollstructs.data(), pollstructs.size(), timeout.count());
		if (r == -1) {
			throw sysErr(errno);
		}

		std::vector<std::reference_wrapper<socket>> readySockets;
		for (size_t i = 0; i < pollstructs.size(); i++) {
			if ((pollstructs[i].revents & POLLIN) == POLLIN) {
				//Read ready
				readySockets.push_back(sockets[i]);
			}
		}
		return readySockets;
	}
};
