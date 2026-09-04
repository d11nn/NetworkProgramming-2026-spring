#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "shell_core.h"

namespace {

constexpr int kMaxUsers = 30;
constexpr int kBacklog = 30;
constexpr int kMaxMessages = 64;
constexpr int kMessageSize = 2048;
constexpr int kNameSize = 64;
constexpr int kIpSize = 64;
constexpr char kDefaultName[] = "(no name)";
constexpr char kUserPipeDir[] = "user_pipe";
constexpr char kWelcome[] =
	"****************************************\n"
	"** Welcome to the information server. **\n"
	"****************************************\n";

using Command = shellcore::Command;
using NextType = shellcore::NextType;

struct SharedUser {
	int online = 0;
	int id = -1;
	int fd = -1;
	pid_t pid = -1;
	int port = 0;
	char name[kNameSize] = {};
	char ip[kIpSize] = {};
	int messageCount = 0;
	char messages[kMaxMessages][kMessageSize] = {};
};

struct SharedState {
	volatile int lock = 0;
	SharedUser users[kMaxUsers + 1];
	int userPipeExists[kMaxUsers + 1][kMaxUsers + 1] = {};
};

struct UserPipeRequest {
	int inputFrom = -1;
	int outputTo = -1;
};

struct UserPipeResolution {
	int inputFd = -1;
	int outputFd = -1;
	int holdFd = -1;
	int devNullInFd = -1;
	int devNullOutFd = -1;
};

int listenFd = -1;
int shmId = -1;
SharedState *sharedState = nullptr;
int currentUserId = -1;
int currentClientFd = -1;
pid_t parentPid = -1;

void lockShared() {
	while (__sync_lock_test_and_set(&sharedState->lock, 1)) {
		usleep(1000);
	}
}

void unlockShared() {
	__sync_lock_release(&sharedState->lock);
}

void copyString(char *dest, size_t size, const std::string &src) {
	std::snprintf(dest, size, "%s", src.c_str());
}

bool writeAll(int fd, const std::string &message) {
	const char *data = message.c_str();
	size_t remaining = message.size();
	while (remaining > 0) {
		ssize_t written = write(fd, data, remaining);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		data += written;
		remaining -= static_cast<size_t>(written);
	}
	return true;
}

void writeAllSignalSafe(int fd, const char *message) {
	size_t remaining = std::strlen(message);
	const char *data = message;
	while (remaining > 0) {
		ssize_t written = write(fd, data, remaining);
		if (written <= 0) {
			if (written < 0 && errno == EINTR) {
				continue;
			}
			return;
		}
		data += written;
		remaining -= static_cast<size_t>(written);
	}
}

std::string fifoPath(int senderId, int receiverId) {
	return std::string(kUserPipeDir) + "/" + std::to_string(senderId) + "_" +
		   std::to_string(receiverId);
}

std::string endpoint(const SharedUser &user) {
	return std::string(user.ip) + ":" + std::to_string(user.port);
}

std::string currentName() {
	lockShared();
	std::string name = sharedState->users[currentUserId].name;
	unlockShared();
	return name;
}

void flushPendingMessages(int) {
	if (sharedState == nullptr || currentUserId < 1 || currentClientFd < 0) {
		return;
	}

	lockShared();
	SharedUser &self = sharedState->users[currentUserId];
	for (int i = 0; i < self.messageCount; ++i) {
		writeAllSignalSafe(currentClientFd, self.messages[i]);
	}
	self.messageCount = 0;
	unlockShared();
}

void sendMessageToUser(int targetId, const std::string &message) {
	if (targetId == currentUserId) {
		writeAll(currentClientFd, message);
		return;
	}

	pid_t targetPid = -1;
	lockShared();
	SharedUser &target = sharedState->users[targetId];
	if (target.online) {
		if (target.messageCount < kMaxMessages) {
			copyString(target.messages[target.messageCount], kMessageSize, message);
			++target.messageCount;
		}
		targetPid = target.pid;
	}
	unlockShared();

	if (targetPid > 0) {
		kill(targetPid, SIGUSR1);
	}
}

void broadcastMessage(const std::string &message) {
	std::vector<int> targetIds;
	lockShared();
	for (int id = 1; id <= kMaxUsers; ++id) {
		if (sharedState->users[id].online) {
			targetIds.push_back(id);
		}
	}
	unlockShared();

	for (int id : targetIds) {
		sendMessageToUser(id, message);
	}
}

bool isDigitsOnly(const std::string &value, size_t start) {
	if (start >= value.size()) {
		return false;
	}
	for (size_t i = start; i < value.size(); ++i) {
		if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
			return false;
		}
	}
	return true;
}

bool isUserPipeToken(const std::string &token, char marker) {
	return token.size() > 1 && token[0] == marker && isDigitsOnly(token, 1);
}

std::vector<Command> parseCommands(const std::string &line, UserPipeRequest &userPipe) {
	std::vector<std::string> tokens = shellcore::splitBySpace(line);
	std::vector<Command> commands;
	Command current;

	for (size_t i = 0; i < tokens.size(); ++i) {
		const std::string &token = tokens[i];
		if (token == "|") {
			current.nextType = NextType::ORDINARY_PIPE;
			commands.push_back(current);
			current = Command();
		} else if (shellcore::isNumberPipeToken(token, '|')) {
			current.nextType = NextType::NUMBER_PIPE_OUT;
			current.nextCount = shellcore::parseNumberPipeCount(token);
			commands.push_back(current);
			current = Command();
		} else if (shellcore::isNumberPipeToken(token, '!')) {
			current.nextType = NextType::NUMBER_PIPE_BOTH;
			current.nextCount = shellcore::parseNumberPipeCount(token);
			commands.push_back(current);
			current = Command();
		} else if (token == ">") {
			if (i + 1 < tokens.size()) {
				current.redirectStdout = true;
				current.redirectFile = tokens[++i];
			}
		} else if (isUserPipeToken(token, '<')) {
			userPipe.inputFrom = std::stoi(token.substr(1));
		} else if (isUserPipeToken(token, '>')) {
			userPipe.outputTo = std::stoi(token.substr(1));
		} else {
			current.argv.push_back(token);
		}
	}

	if (!current.argv.empty()) {
		commands.push_back(current);
	}
	return commands;
}

std::string joinMessage(const std::vector<std::string> &tokens, size_t start) {
	if (start >= tokens.size()) {
		return "";
	}
	std::string message = tokens[start];
	for (size_t i = start + 1; i < tokens.size(); ++i) {
		message += " " + tokens[i];
	}
	return message;
}

void advanceBuiltinSegment(shellcore::ShellState &shell) {
	shellcore::discardIncomingPipe(shell, shell.executedSegments + 1);
	++shell.executedSegments;
}

bool handleRwgBuiltin(shellcore::ShellState &shell, const std::string &line) {
	std::vector<std::string> tokens = shellcore::splitBySpace(line);
	if (tokens.empty()) {
		return true;
	}

	const std::string &command = tokens[0];
	if (command == "who") {
		std::string output = "<ID>\t<fd>\t<nickname>\t<IP:port>\t<indicate me>\n";
		lockShared();
		for (int id = 1; id <= kMaxUsers; ++id) {
			SharedUser &user = sharedState->users[id];
			if (!user.online) {
				continue;
			}
			output += std::to_string(user.id) + "\t" + std::to_string(user.fd) +
					  "\t" + user.name + "\t" + endpoint(user);
			if (id == currentUserId) {
				output += "\t<-me";
			}
			output += "\n";
		}
		unlockShared();
		writeAll(currentClientFd, output);
		advanceBuiltinSegment(shell);
		return true;
	}

	if (command == "name") {
		if (tokens.size() >= 2) {
			std::string newName = tokens[1];
			std::string ipPort;
			bool duplicate = false;
			lockShared();
			for (int id = 1; id <= kMaxUsers; ++id) {
				if (sharedState->users[id].online &&
					std::string(sharedState->users[id].name) == newName) {
					duplicate = true;
					break;
				}
			}
			if (!duplicate) {
				copyString(sharedState->users[currentUserId].name, kNameSize, newName);
				ipPort = endpoint(sharedState->users[currentUserId]);
			}
			unlockShared();

			if (duplicate) {
				writeAll(currentClientFd, "*** User '" + newName + "' already exists. ***\n");
			} else {
				broadcastMessage("*** User from " + ipPort + " is named '" + newName + "'. ***\n");
			}
		}
		advanceBuiltinSegment(shell);
		return true;
	}

	if (command == "tell") {
		if (tokens.size() >= 3) {
			int targetId = std::stoi(tokens[1]);
			bool exists = false;
			std::string senderName;
			lockShared();
			exists = targetId >= 1 && targetId <= kMaxUsers && sharedState->users[targetId].online;
			senderName = sharedState->users[currentUserId].name;
			unlockShared();

			if (!exists) {
				writeAll(currentClientFd, "*** Error: user #" + std::to_string(targetId) +
										 " does not exist yet. ***\n");
			} else {
				sendMessageToUser(targetId, "*** " + senderName + " told you ***: " +
											 joinMessage(tokens, 2) + "\n");
			}
		}
		advanceBuiltinSegment(shell);
		return true;
	}

	if (command == "yell") {
		broadcastMessage("*** " + currentName() + " yelled ***: " + joinMessage(tokens, 1) + "\n");
		advanceBuiltinSegment(shell);
		return true;
	}

	return false;
}

UserPipeResolution resolveUserPipes(const UserPipeRequest &request, const std::string &line) {
	UserPipeResolution resolution;
	if (request.inputFrom >= 0) {
		bool senderExists = false;
		bool pipeExists = false;
		std::string senderName;
		std::string receiverName;
		lockShared();
		if (request.inputFrom >= 1 && request.inputFrom <= kMaxUsers) {
			senderExists = sharedState->users[request.inputFrom].online;
			pipeExists = sharedState->userPipeExists[request.inputFrom][currentUserId] != 0;
			senderName = sharedState->users[request.inputFrom].name;
			receiverName = sharedState->users[currentUserId].name;
			if (senderExists && pipeExists) {
				sharedState->userPipeExists[request.inputFrom][currentUserId] = 0;
			}
		}
		unlockShared();

		if (!senderExists) {
			writeAll(currentClientFd, "*** Error: user #" + std::to_string(request.inputFrom) +
									 " does not exist yet. ***\n");
			resolution.devNullInFd = open("/dev/null", O_RDONLY);
			resolution.inputFd = resolution.devNullInFd;
		} else if (!pipeExists) {
			writeAll(currentClientFd, "*** Error: the pipe #" + std::to_string(request.inputFrom) +
									 "->#" + std::to_string(currentUserId) +
									 " does not exist yet. ***\n");
			resolution.devNullInFd = open("/dev/null", O_RDONLY);
			resolution.inputFd = resolution.devNullInFd;
		} else {
			std::string path = fifoPath(request.inputFrom, currentUserId);
			resolution.inputFd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
			broadcastMessage("*** " + receiverName + " (#" + std::to_string(currentUserId) +
							 ") just received from " + senderName + " (#" +
							 std::to_string(request.inputFrom) + ") by '" + line + "' ***\n");
			unlink(path.c_str());
		}
	}

	if (request.outputTo >= 0) {
		bool receiverExists = false;
		bool pipeExists = false;
		std::string senderName;
		std::string receiverName;
		lockShared();
		if (request.outputTo >= 1 && request.outputTo <= kMaxUsers) {
			receiverExists = sharedState->users[request.outputTo].online;
			pipeExists = sharedState->userPipeExists[currentUserId][request.outputTo] != 0;
			senderName = sharedState->users[currentUserId].name;
			receiverName = sharedState->users[request.outputTo].name;
			if (receiverExists && !pipeExists) {
				sharedState->userPipeExists[currentUserId][request.outputTo] = 1;
			}
		}
		unlockShared();

		if (!receiverExists) {
			writeAll(currentClientFd, "*** Error: user #" + std::to_string(request.outputTo) +
									 " does not exist yet. ***\n");
			resolution.devNullOutFd = open("/dev/null", O_WRONLY);
			resolution.outputFd = resolution.devNullOutFd;
		} else if (pipeExists) {
			writeAll(currentClientFd, "*** Error: the pipe #" + std::to_string(currentUserId) +
									 "->#" + std::to_string(request.outputTo) +
									 " already exists. ***\n");
			resolution.devNullOutFd = open("/dev/null", O_WRONLY);
			resolution.outputFd = resolution.devNullOutFd;
		} else {
			std::string path = fifoPath(currentUserId, request.outputTo);
			unlink(path.c_str());
			if (mkfifo(path.c_str(), 0666) < 0 && errno != EEXIST) {
				resolution.devNullOutFd = open("/dev/null", O_WRONLY);
				resolution.outputFd = resolution.devNullOutFd;
			} else {
				resolution.holdFd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
				resolution.outputFd = open(path.c_str(), O_WRONLY);
				if (resolution.outputFd < 0) {
					shellcore::closeFd(resolution.holdFd);
					resolution.devNullOutFd = open("/dev/null", O_WRONLY);
					resolution.outputFd = resolution.devNullOutFd;
				}
			}
			broadcastMessage("*** " + senderName + " (#" + std::to_string(currentUserId) +
							 ") just piped '" + line + "' to " + receiverName + " (#" +
							 std::to_string(request.outputTo) + ") ***\n");
		}
	}
	return resolution;
}

void closeUserPipeResolution(UserPipeResolution &resolution) {
	shellcore::closeFd(resolution.outputFd);
}

bool executeLine(shellcore::ShellState &shell, std::string line) {
	if (!line.empty() && line.back() == '\r') {
		line.pop_back();
	}
	if (line.empty()) {
		return true;
	}

	std::vector<std::string> tokens = shellcore::splitBySpace(line);
	if (!tokens.empty() && tokens[0] == "exit") {
		return false;
	}
	if (handleRwgBuiltin(shell, line)) {
		return true;
	}

	UserPipeRequest request;
	std::vector<Command> commands = parseCommands(line, request);
	if (commands.empty()) {
		return true;
	}
	if (commands.size() == 1 && shellcore::executeShellBuiltin(commands[0], shell, currentClientFd)) {
		return true;
	}

	UserPipeResolution userPipe = resolveUserPipes(request, line);
	shellcore::ExecutionConfig config;
	config.inputFd = userPipe.inputFd;
	config.outputFd = userPipe.outputFd >= 0 ? userPipe.outputFd : currentClientFd;
	config.errorFd = currentClientFd;
	config.forceLastOutput = userPipe.outputFd >= 0;
	shellcore::executeExternalCommands(commands, shell, config);
	closeUserPipeResolution(userPipe);
	return true;
}

int createListenSocket(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		std::perror("socket");
		std::exit(1);
	}

	int enable = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
		std::perror("setsockopt");
		std::exit(1);
	}

	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(static_cast<uint16_t>(port));
	if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		std::perror("bind");
		std::exit(1);
	}
	if (listen(fd, kBacklog) < 0) {
		std::perror("listen");
		std::exit(1);
	}
	return fd;
}

int assignUser(int fd, const sockaddr_in &addr) {
	lockShared();
	int assignedId = -1;
	for (int id = 1; id <= kMaxUsers; ++id) {
		if (!sharedState->users[id].online) {
			assignedId = id;
			SharedUser &user = sharedState->users[id];
			user.online = 1;
			user.id = id;
			user.fd = fd;
			user.pid = getpid();
			user.port = ntohs(addr.sin_port);
			user.messageCount = 0;
			copyString(user.name, kNameSize, kDefaultName);
			copyString(user.ip, kIpSize, inet_ntoa(addr.sin_addr));
			break;
		}
	}
	unlockShared();
	return assignedId;
}

void cleanupUser(int id, bool announce) {
	std::string name;
	std::vector<std::string> paths;
	lockShared();
	if (id >= 1 && id <= kMaxUsers && sharedState->users[id].online) {
		name = sharedState->users[id].name;
		sharedState->users[id].online = 0;
		sharedState->users[id].messageCount = 0;
		for (int peer = 1; peer <= kMaxUsers; ++peer) {
			if (sharedState->userPipeExists[id][peer]) {
				sharedState->userPipeExists[id][peer] = 0;
				paths.push_back(fifoPath(id, peer));
			}
			if (sharedState->userPipeExists[peer][id]) {
				sharedState->userPipeExists[peer][id] = 0;
				paths.push_back(fifoPath(peer, id));
			}
		}
	}
	unlockShared();

	for (const std::string &path : paths) {
		unlink(path.c_str());
	}
	if (announce && !name.empty()) {
		broadcastMessage("*** User '" + name + "' left. ***\n");
	}
}

void sendPrompt() {
	writeAll(currentClientFd, "% ");
}

void serveClient(int connFd, int listenSocket, const sockaddr_in &addr) {
	close(listenSocket);
	currentClientFd = connFd;
	currentUserId = assignUser(connFd, addr);
	if (currentUserId < 0) {
		close(connFd);
		_exit(0);
	}

	signal(SIGUSR1, flushPendingMessages);
	writeAll(currentClientFd, kWelcome);
	broadcastMessage("*** User '" + std::string(kDefaultName) + "' entered from " +
					 endpoint(sharedState->users[currentUserId]) + ". ***\n");
	sendPrompt();

	shellcore::ShellState shell;
	std::string inputBuffer;
	char buffer[4096];
	while (true) {
		ssize_t count = read(connFd, buffer, sizeof(buffer));
		if (count <= 0) {
			if (count < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		inputBuffer.append(buffer, static_cast<size_t>(count));
		size_t newlinePos = std::string::npos;
		while ((newlinePos = inputBuffer.find('\n')) != std::string::npos) {
			std::string line = inputBuffer.substr(0, newlinePos);
			inputBuffer.erase(0, newlinePos + 1);
			if (!executeLine(shell, line)) {
				shellcore::closeShellState(shell);
				cleanupUser(currentUserId, true);
				close(connFd);
				_exit(0);
			}
			sendPrompt();
		}
	}

	shellcore::closeShellState(shell);
	cleanupUser(currentUserId, true);
	close(connFd);
	_exit(0);
}

void cleanupSharedMemory() {
	if (sharedState != nullptr) {
		for (int sender = 1; sender <= kMaxUsers; ++sender) {
			for (int receiver = 1; receiver <= kMaxUsers; ++receiver) {
				if (sharedState->userPipeExists[sender][receiver]) {
					unlink(fifoPath(sender, receiver).c_str());
				}
			}
		}
		shmdt(sharedState);
		sharedState = nullptr;
	}
	if (shmId >= 0) {
		shmctl(shmId, IPC_RMID, nullptr);
		shmId = -1;
	}
}

void handleSigint(int) {
	if (parentPid > 0 && getpid() != parentPid) {
		_exit(0);
	}
	cleanupSharedMemory();
	_exit(0);
}

void initSharedMemory() {
	shmId = shmget(IPC_PRIVATE, sizeof(SharedState), IPC_CREAT | 0600);
	if (shmId < 0) {
		std::perror("shmget");
		std::exit(1);
	}
	void *addr = shmat(shmId, nullptr, 0);
	if (addr == reinterpret_cast<void *>(-1)) {
		std::perror("shmat");
		std::exit(1);
	}
	sharedState = reinterpret_cast<SharedState *>(addr);
	std::memset(sharedState, 0, sizeof(SharedState));
	for (int id = 1; id <= kMaxUsers; ++id) {
		sharedState->users[id].id = id;
	}
}

void runServer(int port) {
	parentPid = getpid();
	mkdir(kUserPipeDir, 0777);
	initSharedMemory();
	signal(SIGINT, handleSigint);
	signal(SIGHUP, handleSigint);
	signal(SIGQUIT, handleSigint);
	signal(SIGTERM, handleSigint);
	signal(SIGCHLD, shellcore::reapChildren);
	listenFd = createListenSocket(port);

	while (true) {
		sockaddr_in addr;
		socklen_t len = sizeof(addr);
		int connFd = accept(listenFd, reinterpret_cast<sockaddr *>(&addr), &len);
		if (connFd < 0) {
			if (errno == EINTR) {
				continue;
			}
			continue;
		}

		pid_t pid = shellcore::forkOrReapUntilSuccess();
		if (pid == 0) {
			serveClient(connFd, listenFd, addr);
		}
		close(connFd);
	}
}

}  // namespace

int main(int argc, char *argv[]) {
	if (argc != 2) {
		return 1;
	}
	runServer(std::stoi(argv[1]));
	return 0;
}
