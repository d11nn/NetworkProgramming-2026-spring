#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "shell_core.h"

namespace {

	int createListenSocket(int port) {
		// int socket(int domain, int type, int protocol);
		// domain == AF_INET means IPv4
		// socket type == SOCK_STREAM means TCP
		// protocol = 0 means via domain and socket type
		// socket return fd, everything is a file
		int listenFd = socket(AF_INET, SOCK_STREAM, 0); 
		
		// if listenFd < 0 means socket create fail
		if (listenFd < 0) {
			std::perror("socket");
			std::exit(1);
		}
		
		int enable = 1;
		/*
			setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
			sockfd: which fd for socket
			SOL_SOCKET: means socket layer but not TCP of IP layer
			SO_REUSEADDR: an option of SOL_SOCKET, means it can reuse same address, if not set this, may have an erroe: address already in use
			enable = enable SO_REUSEADDR
		*/
		if (setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
			std::perror("setsockopt");
			close(listenFd);
			std::exit(1);
		}

		sockaddr_in serverAddr; // declare an IPv4(sockaddr_in) address, can be used by bind()
		std::memset(&serverAddr, 0, sizeof(serverAddr));// initial serverAddr value to be all 0
		serverAddr.sin_family = AF_INET; // sin = socket internet, means this socket address is ipv4
		serverAddr.sin_addr.s_addr = htonl(INADDR_ANY); // host to network long(32-bit), INADDR_ANY means listen all NIC, not a specific IP.
		serverAddr.sin_port = htons(static_cast<uint16_t>(port)); // host to newwork short(16-bit), static_cast<uint16_t>(port) means transform port from int to uint16_t

		/*
			bind(socketfd, sockaddr, socklen_t)
			success: return 0, else -1
			because bind is general function, so just give it sockaddr * but not specific ipv4 addr_in
		*/
		if (bind(listenFd, reinterpret_cast<sockaddr *>(&serverAddr), sizeof(serverAddr)) < 0) {
			std::perror("bind");
			close(listenFd);
			std::exit(1);
		}
		/*
			listen(socketfd, backlog)
			success: 0, else -1
		*/
		if (listen(listenFd, 30) < 0) {
			std::perror("listen");
			close(listenFd);
			std::exit(1);
		}

		return listenFd;
	}
	// serveClient -> transform client socket to stdin/out/err, and start this client shell session
	// only child process will exec this function
	void serveClient(int connFd, int listenFd) {
		close(listenFd); // child don't need to accpet other connect
		/*
			dup2 means let newFd share same object with oldFd
		*/
		dup2(connFd, STDIN_FILENO);
		dup2(connFd, STDOUT_FILENO);
		dup2(connFd, STDERR_FILENO);
		// so 0/1/2 have connFd(socket) obj, so we just close it, use if just means prevent connFd is 0/1/2
		if (connFd > STDERR_FILENO) {
			close(connFd);
		}

		// Each connection gets an independent shell state and environment.
		shellcore::ShellSession shellSession;
		shellSession.run();
		_exit(0);
	}

}  // namespace

int main(int argc, char *argv[]) {
	/*
		./np_simple 12345
		so argc = 2, argv[0] = "./np_simple", argv[1] = "12345"
	*/
	if (argc != 2) {
		return 1;
	}

	int port = std::stoi(argv[1]); // 12345
	signal(SIGCHLD, shellcore::reapChildren); // same logic as npshell.cpp, just a handler for zombie process

	int listenFd = createListenSocket(port);
	while (true) {
		sockaddr_in clientAddr;
		socklen_t clientLen = sizeof(clientAddr);
		// try to use listenFd(lately create socket) to wait a client
		// if there is any client, return new fd (connFd)
		// connFd is a socket that chat with client
		// accpet success = 0, else -1
		// EINTR means interrupt by signal, can be retry
		int connFd = accept(listenFd, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);
		/*
			logic is same as
			if (connFd < 0) {
				continue;
			}
		*/
		if (connFd < 0) {
			if (errno == EINTR) {
				continue;
			}
			continue;
		}
		// fork a child process
		pid_t childPid = shellcore::forkOrReapUntilSuccess();
		// only child process will go to serve Client
		if (childPid == 0) {
			serveClient(connFd, listenFd);
		}
		// because serveClient already dup(connFd, 0/1/2), and close connFd, so parent don't need to maintain connFd.
		close(connFd);
	}

	return 0;
}
