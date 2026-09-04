#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shell_core.h"

namespace {

constexpr int kMaxUsers = 30;
constexpr int kBacklog = 30;
constexpr char kDefaultName[] = "(no name)";
constexpr char kWelcome[] =
	"****************************************\n"
	"** Welcome to the information server. **\n"
	"****************************************\n";

using Command = shellcore::Command;
using LinePlan = shellcore::LinePlan;
using NextType = shellcore::NextType;
using PipeFD = shellcore::PipeFD;
using ShellState = shellcore::ShellState;

struct Client {
	bool online = false;
	int id = -1;
	int fd = -1;
	std::string name = kDefaultName;
	std::string ip;
	int port = 0;
	std::string inputBuffer;
	ShellState shell;
};

struct UserPipeRequest {
	int inputFrom = -1;
	int outputTo = -1;
};

struct UserPipeResolution {
	int inputFd = -1;
	int outputFd = -1;
	int devNullInFd = -1;
	int devNullOutFd = -1;
};

std::map<int, Client> clients;
std::map<std::pair<int, int>, PipeFD> userPipes;
int listenFd = -1;

void closeFd(int &fd) {
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
}

void closePipe(PipeFD &pipeFd) {
	closeFd(pipeFd.readFd);
	closeFd(pipeFd.writeFd);
}

void reapChildren(int) {
	while (waitpid(-1, nullptr, WNOHANG) > 0) {
	}
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

void sendTo(Client &client, const std::string &message) {
	if (client.online) {
		writeAll(client.fd, message);
	}
}

void sendPrompt(Client &client) {
	sendTo(client, "% ");
}

void broadcast(const std::string &message) {
	for (auto &entry : clients) {
		if (entry.second.online) {
			sendTo(entry.second, message);
		}
	}
}

std::string endpoint(const Client &client) {
	return client.ip + ":" + std::to_string(client.port);
}

std::vector<std::string> splitBySpace(const std::string &line) {
	std::vector<std::string> tokens;
	std::istringstream input(line);
	std::string token;
	while (input >> token) {
		tokens.push_back(token);
	}
	return tokens;
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
	std::vector<std::string> tokens = splitBySpace(line);
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

Client *findClientById(int id) {
	auto it = clients.find(id);
	if (it == clients.end() || !it->second.online) {
		return nullptr;
	}
	return &it->second;
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

bool handleRwgBuiltin(Client &client, const std::string &line) {
	std::vector<std::string> tokens = splitBySpace(line);
	if (tokens.empty()) {
		return true;
	}

	const std::string &name = tokens[0];
	if (name == "who") {
		sendTo(client, "<ID>\t<fd>\t<nickname>\t<IP:port>\t<indicate me>\n");
		for (int id = 1; id <= kMaxUsers; ++id) {
			Client *target = findClientById(id);
			if (target == nullptr) {
				continue;
			}
			std::string row = std::to_string(target->id) + "\t" +
							  std::to_string(target->fd) + "\t" +
							  target->name + "\t" + endpoint(*target);
			if (target->id == client.id) {
				row += "\t<-me";
			}
			sendTo(client, row + "\n");
		}
		shellcore::discardIncomingPipe(client.shell, client.shell.executedSegments + 1);
		++client.shell.executedSegments;
		return true;
	}

	if (name == "name") {
		if (tokens.size() >= 2) {
			const std::string &newName = tokens[1];
			for (const auto &entry : clients) {
				if (entry.second.online && entry.second.name == newName) {
					sendTo(client, "*** User '" + newName + "' already exists. ***\n");
					shellcore::discardIncomingPipe(client.shell, client.shell.executedSegments + 1);
					++client.shell.executedSegments;
					return true;
				}
			}
			client.name = newName;
			broadcast("*** User from " + endpoint(client) + " is named '" + newName + "'. ***\n");
		}
		shellcore::discardIncomingPipe(client.shell, client.shell.executedSegments + 1);
		++client.shell.executedSegments;
		return true;
	}

	if (name == "tell") {
		if (tokens.size() >= 3) {
			int targetId = std::stoi(tokens[1]);
			Client *target = findClientById(targetId);
			if (target == nullptr) {
				sendTo(client, "*** Error: user #" + std::to_string(targetId) + " does not exist yet. ***\n");
			} else {
				sendTo(*target, "*** " + client.name + " told you ***: " + joinMessage(tokens, 2) + "\n");
			}
		}
		shellcore::discardIncomingPipe(client.shell, client.shell.executedSegments + 1);
		++client.shell.executedSegments;
		return true;
	}

	if (name == "yell") {
		broadcast("*** " + client.name + " yelled ***: " + joinMessage(tokens, 1) + "\n");
		shellcore::discardIncomingPipe(client.shell, client.shell.executedSegments + 1);
		++client.shell.executedSegments;
		return true;
	}

	return false;
}

UserPipeResolution resolveUserPipes(Client &client, const UserPipeRequest &request, const std::string &line) {
	UserPipeResolution resolution;
	if (request.inputFrom >= 0) {
		Client *sender = findClientById(request.inputFrom);
		auto key = std::make_pair(request.inputFrom, client.id);
		auto pipeIt = userPipes.find(key);
		if (sender == nullptr) {
			sendTo(client, "*** Error: user #" + std::to_string(request.inputFrom) + " does not exist yet. ***\n");
			resolution.devNullInFd = open("/dev/null", O_RDONLY);
			resolution.inputFd = resolution.devNullInFd;
		} else if (pipeIt == userPipes.end()) {
			sendTo(client, "*** Error: the pipe #" + std::to_string(request.inputFrom) + "->#" +
						   std::to_string(client.id) + " does not exist yet. ***\n");
			resolution.devNullInFd = open("/dev/null", O_RDONLY);
			resolution.inputFd = resolution.devNullInFd;
		} else {
			resolution.inputFd = pipeIt->second.readFd;
			closeFd(pipeIt->second.writeFd);
			broadcast("*** " + client.name + " (#" + std::to_string(client.id) +
					  ") just received from " + sender->name + " (#" +
					  std::to_string(sender->id) + ") by '" + line + "' ***\n");
			userPipes.erase(pipeIt);
		}
	}

	if (request.outputTo >= 0) {
		Client *receiver = findClientById(request.outputTo);
		auto key = std::make_pair(client.id, request.outputTo);
		if (receiver == nullptr) {
			sendTo(client, "*** Error: user #" + std::to_string(request.outputTo) + " does not exist yet. ***\n");
			resolution.devNullOutFd = open("/dev/null", O_WRONLY);
			resolution.outputFd = resolution.devNullOutFd;
		} else if (userPipes.find(key) != userPipes.end()) {
			sendTo(client, "*** Error: the pipe #" + std::to_string(client.id) + "->#" +
						   std::to_string(request.outputTo) + " already exists. ***\n");
			resolution.devNullOutFd = open("/dev/null", O_WRONLY);
			resolution.outputFd = resolution.devNullOutFd;
		} else {
			PipeFD pipeFd = shellcore::createPipeOrDie();
			resolution.outputFd = pipeFd.writeFd;
			userPipes[key] = pipeFd;
			broadcast("*** " + client.name + " (#" + std::to_string(client.id) +
					  ") just piped '" + line + "' to " + receiver->name + " (#" +
					  std::to_string(receiver->id) + ") ***\n");
		}
	}
	return resolution;
}

void closeUserPipeResolution(UserPipeResolution &resolution) {
	closeFd(resolution.devNullInFd);
	closeFd(resolution.devNullOutFd);
}

std::vector<int> collectUserPipeFds() {
	std::vector<int> fds;
	for (const auto &entry : userPipes) {
		fds.push_back(entry.second.readFd);
		fds.push_back(entry.second.writeFd);
	}
	return fds;
}

bool executeLine(Client &client, std::string line) {
	if (!line.empty() && line.back() == '\r') {
		line.pop_back();
	}
	if (line.empty()) {
		return true;
	}

	std::vector<std::string> tokens = splitBySpace(line);
	if (!tokens.empty() && tokens[0] == "exit") {
		return false;
	}

	if (handleRwgBuiltin(client, line)) {
		return true;
	}

	UserPipeRequest userPipeRequest;
	std::vector<Command> commands = parseCommands(line, userPipeRequest);
	if (commands.empty()) {
		return true;
	}

	if (commands.size() == 1 &&
		shellcore::executeShellBuiltin(commands[0], client.shell, client.fd)) {
		return true;
	}

	UserPipeResolution userPipe = resolveUserPipes(client, userPipeRequest, line);
	shellcore::ExecutionConfig config;
	config.inputFd = userPipe.inputFd;
	config.outputFd = userPipe.outputFd >= 0 ? userPipe.outputFd : client.fd;
	config.errorFd = client.fd;
	config.forceLastOutput = userPipe.outputFd >= 0;
	config.closeInChild = collectUserPipeFds();
	shellcore::executeExternalCommands(commands, client.shell, config);
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

int smallestUnusedId() {
	for (int id = 1; id <= kMaxUsers; ++id) {
		if (findClientById(id) == nullptr) {
			return id;
		}
	}
	return -1;
}

void cleanupClientUserPipes(int id) {
	for (auto it = userPipes.begin(); it != userPipes.end();) {
		if (it->first.first == id || it->first.second == id) {
			shellcore::closePipe(it->second);
			it = userPipes.erase(it);
		} else {
			++it;
		}
	}
}

void cleanupClient(int id, bool announce) {
	Client *client = findClientById(id);
	if (client == nullptr) {
		return;
	}

	std::string name = client->name;
	closeFd(client->fd);
	shellcore::closeShellState(client->shell);
	cleanupClientUserPipes(id);
	client->online = false;
	if (announce) {
		broadcast("*** User '" + name + "' left. ***\n");
	}
	clients.erase(id);
}

void acceptClient() {
	sockaddr_in addr;
	socklen_t len = sizeof(addr);
	int connFd = accept(listenFd, reinterpret_cast<sockaddr *>(&addr), &len);
	if (connFd < 0) {
		return;
	}

	int id = smallestUnusedId();
	if (id < 0) {
		close(connFd);
		return;
	}

	Client client;
	client.online = true;
	client.id = id;
	client.fd = connFd;
	client.name = kDefaultName;
	client.ip = inet_ntoa(addr.sin_addr);
	client.port = ntohs(addr.sin_port);
	clients[id] = client;

	sendTo(clients[id], kWelcome);
	broadcast("*** User '" + clients[id].name + "' entered from " + endpoint(clients[id]) + ". ***\n");
	sendPrompt(clients[id]);
}

void handleClientInput(Client &client) {
	char buffer[4096];
	ssize_t count = read(client.fd, buffer, sizeof(buffer));
	if (count <= 0) {
		if (count < 0 && errno == EINTR) {
			return;
		}
		cleanupClient(client.id, true);
		return;
	}

	client.inputBuffer.append(buffer, static_cast<size_t>(count));
	size_t newlinePos = std::string::npos;
	while ((newlinePos = client.inputBuffer.find('\n')) != std::string::npos) {
		std::string line = client.inputBuffer.substr(0, newlinePos);
		client.inputBuffer.erase(0, newlinePos + 1);
		bool keepRunning = executeLine(client, line);
		if (!keepRunning) {
			cleanupClient(client.id, true);
			return;
		}
		sendPrompt(client);
	}
}

void runServer(int port) {
	listenFd = createListenSocket(port);
	while (true) {
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(listenFd, &readSet);
		int maxFd = listenFd;

		for (const auto &entry : clients) {
			if (entry.second.online) {
				FD_SET(entry.second.fd, &readSet);
				maxFd = std::max(maxFd, entry.second.fd);
			}
		}

		int ready = select(maxFd + 1, &readSet, nullptr, nullptr, nullptr);
		/*
			int select(
				int nfds,
				fd_set *readfds,
				fd_set *writefds,
				fd_set *exceptfds,
				struct timeval *timeout
			);
		*/
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			std::perror("select");
			continue;
		}

		if (FD_ISSET(listenFd, &readSet)) {
			acceptClient();
		}

		std::vector<int> readyClients;
		for (const auto &entry : clients) {
			if (entry.second.online && FD_ISSET(entry.second.fd, &readSet)) {
				readyClients.push_back(entry.first);
			}
		}
		for (int id : readyClients) {
			Client *client = findClientById(id);
			if (client != nullptr) {
				handleClientInput(*client);
			}
		}
	}
}

}  // namespace

int main(int argc, char *argv[]) {
	if (argc != 2) {
		return 1;
	}

	signal(SIGCHLD, shellcore::reapChildren);
	runServer(std::stoi(argv[1]));
	return 0;
}
