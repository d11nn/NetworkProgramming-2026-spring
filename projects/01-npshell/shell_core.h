#ifndef SHELL_CORE_H
#define SHELL_CORE_H

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace shellcore {

enum class NextType {
	NONE,
	ORDINARY_PIPE,
	NUMBER_PIPE_OUT,
	NUMBER_PIPE_BOTH,
};

struct Command {
	std::vector<std::string> argv;
	NextType nextType = NextType::NONE; // NONE, ORDINARY_PIPE, NUMBER_PIPE_OUT, NUMBER_PIPE_BOTH
	int nextCount = 0; // |N's N 
	bool redirectStdout = false; // if argv has >, true
	std::string redirectFile; // if true, keep file name
};

struct PipeFD {
	int readFd = -1;
	int writeFd = -1;
};

// A line may contain multiple ordinary-pipe segments.
struct LinePlan {
	std::vector<long long> commandSegments;
	std::vector<bool> segmentStarts;
	long long segmentCount = 0;
};

// waitpid(pid_t pid, int *status, int option)
// when pid == -1, meaning wait for any child process.
// status == nullptr just means don't store status information
// options == WNOHANG means return immediately if no child has exited.
// waitpid > 0: success reap a child, return that child PID
inline void reapChildren(int) {
	while (waitpid(-1, nullptr, WNOHANG) > 0) {
	}
}


inline bool isNumberPipeToken(const std::string &token, char marker) {
	if (token.size() < 2 || token[0] != marker) {
		return false;
	}

	for (size_t i = 1; i < token.size(); ++i) {
		if (!std::isdigit(static_cast<unsigned char>(token[i]))) {
			return false;
		}
	}
	return true;
}

inline bool isNumberPipeType(NextType nextType) {
	return nextType == NextType::NUMBER_PIPE_OUT || nextType == NextType::NUMBER_PIPE_BOTH;
}

inline int parseNumberPipeCount(const std::string &token) {
	return std::stoi(token.substr(1));
}

inline void closeFd(int &fd) {
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
}

inline void closePipe(PipeFD &pipeFd) {
	closeFd(pipeFd.readFd);
	closeFd(pipeFd.writeFd);
}

inline PipeFD createPipeOrDie() {
	int pipefd[2];
	if (pipe(pipefd) < 0) {
		std::perror("pipe");
		std::exit(1);
	}
	return PipeFD{pipefd[0], pipefd[1]};
}

inline std::vector<std::string> splitBySpace(const std::string &line) {
	std::vector<std::string> tokens; // ls -al
	std::istringstream input(line); // can be sequential read ls -> -al
	std::string token;
	while (input >> token) {
		tokens.push_back(token);
	}
	// tokens will be ["ls", "-al"]
	return tokens;
}

inline std::vector<Command> parseLine(const std::string &line) {
	std::vector<std::string> tokens = splitBySpace(line);
	std::vector<Command> commands;
	if (tokens.empty()) {
		return commands;
	}

	Command current;
	for (size_t i = 0; i < tokens.size(); ++i) {
		const std::string &token = tokens[i];
		// tokens = ["ls", "-al"]
		if (token == "|") {
			current.nextType = NextType::ORDINARY_PIPE;
			commands.push_back(current);
			current = Command();
		} else if (isNumberPipeToken(token, '|')) {
			current.nextType = NextType::NUMBER_PIPE_OUT;
			// parseNumberPipeCount |2 -> to 2
			current.nextCount = parseNumberPipeCount(token);
			commands.push_back(current);
			current = Command();
		} else if (isNumberPipeToken(token, '!')) {
			current.nextType = NextType::NUMBER_PIPE_BOTH;
			current.nextCount = parseNumberPipeCount(token);
			commands.push_back(current);
			current = Command();
		} else if (token == ">") {
			// cat > out.txt
			// i + 1 < tokens.size to make sure there are file name
			if (i + 1 < tokens.size()) {
				current.redirectStdout = true;
				current.redirectFile = tokens[++i];
			}
		} else {
			// don't have any special marker
			current.argv.push_back(token);
		}
	}
	// line = ls -al, don't have any pipe
	// or cat > out.txt
	if (!current.argv.empty()) {
		commands.push_back(current);
	}
	return commands;
}

inline bool isBuiltin(const Command &command) {
	const std::string &name = command.argv.empty() ? std::string() : command.argv[0];
	return name == "setenv" || name == "printenv" || name == "exit";
}

inline void executeBuiltin(const Command &command) {
	const std::string &name = command.argv[0];
	if (name == "setenv") {
		if (command.argv.size() >= 3) {
			// use c_str() because const chat* but not sting
			setenv(command.argv[1].c_str(), command.argv[2].c_str(), 1);
		}
		return;
	}

	if (name == "printenv" && command.argv.size() >= 2) {
		// printenv PATH
		const char *value = getenv(command.argv[1].c_str());
		if (value != nullptr) {
			std::cout << value << '\n';
		}
	}
}

inline std::vector<bool> buildWaitability(const std::vector<Command> &commands) {
	std::vector<bool> waitable(commands.size(), true);
	size_t segmentStart = 0;
	while (segmentStart < commands.size()) {
		size_t segmentEnd = segmentStart;
		while (segmentEnd + 1 < commands.size() &&
				 commands[segmentEnd].nextType == NextType::ORDINARY_PIPE) {
			++segmentEnd;
		}

		if (isNumberPipeType(commands[segmentEnd].nextType)) {
			for (size_t i = segmentStart; i <= segmentEnd; ++i) {
				waitable[i] = false;
			}
		}
		segmentStart = segmentEnd + 1;
	}
	return waitable;
}

inline void graceReap(const std::vector<pid_t> &pids) {
	if (pids.empty()) {
		return;
	}

	std::vector<bool> done(pids.size(), false);
	while (true) {
		bool madeProgress = false;
		for (size_t i = 0; i < pids.size(); ++i) {
			if (done[i]) {
				continue;
			}

			int status = 0;
			pid_t result = waitpid(pids[i], &status, WNOHANG);
			if (result == pids[i] || (result < 0 && errno == ECHILD)) {
				done[i] = true;
				madeProgress = true;
			} else if (result < 0 && errno != EINTR) {
				std::perror("waitpid");
				std::exit(1);
			}
		}

		if (!madeProgress) {
			break;
		}
	}
}

inline pid_t forkOrReapUntilSuccess() {
	// Retry fork after reaping one finished child when process slots are tight.
	while (true) {
		pid_t childPid = fork(); // try to fork current process;
		if (childPid >= 0) { 
			return childPid; // if parent sucess get a childPid, return childPid
		}

		if (waitpid(-1, nullptr, 0) > 0) { // if fork fail, reap a child
			continue;
		}

		if (errno == EINTR) {
			continue;
		}

		std::perror("fork");
		std::exit(1);
	}
}

class ShellSession {
public:
	void run() {
		setenv("PATH", "bin:.", 1); // define PATH is bin:., 1 means overwrite

		std::string line;
		while (true) {
			printPrompt(); // cout << "% " and cout.flush()
			// if getline fail, end while loop
			// only when ctrl + D or socket break will trigger it
			// getline will make shell wait for input
			if (!std::getline(std::cin, line)) {
				break;
			}
			// if last char is '\r', delete it
			trimTrailingCarriageReturn(line);

			// if user only press enter
			if (line.empty()) {
				continue;
			}

			// transform user input to data structure can be executed by shell
			std::vector<Command> commands = parseLine(line);
			// line = "  ", command will be empty
			if (commands.empty()) {
				continue;
			}
			// return false if not Builtin
			if (commands.size() == 1 && tryRunBuiltin(commands[0])) {
				continue;
			}
			
			LinePlan linePlan = buildLinePlan(commands);
			executeExternalCommands(commands, linePlan);
			executedSegments += linePlan.segmentCount;
		}
	}

private:
	struct IoPlan {
		int outFd = STDOUT_FILENO;
		int errFd = STDERR_FILENO;
		int nextInFd = -1;
		int redirectFd = -1;
		PipeFD ordinaryPipe;
	};

	std::unordered_map<long long, PipeFD> numberedPipes;
	long long executedSegments = 0;

	void printPrompt() const {
		std::cout << "% ";
		std::cout.flush();
	}

	void trimTrailingCarriageReturn(std::string &line) const {
		if (!line.empty() && line.back() == '\r') line.pop_back();
	}

	LinePlan buildLinePlan(const std::vector<Command> &commands) const {
		// Numbered pipes target future segments, not just future lines.
		LinePlan linePlan;
		linePlan.commandSegments.resize(commands.size(), 0);
		linePlan.segmentStarts.resize(commands.size(), false);

		long long segmentCursor = executedSegments;
		for (size_t i = 0; i < commands.size(); ++i) {
			if (i == 0 || commands[i - 1].nextType != NextType::ORDINARY_PIPE) {
				++segmentCursor;
				linePlan.segmentStarts[i] = true;
			}
			linePlan.commandSegments[i] = segmentCursor;
		}

		linePlan.segmentCount = segmentCursor - executedSegments;
		return linePlan;
	}

	bool tryRunBuiltin(const Command &command) {
		if (!isBuiltin(command)) {
			return false;
		}
		// executedSegments means already run segment
		// so now segment is executedSegments + 1
		// and you should discardIncoming pipe
		// or this delay pipe will mistakenly be preserved in numberedPipes
		discardIncomingPipe(executedSegments + 1);
		if (command.argv[0] == "exit") {
			std::exit(0);
		}

		executeBuiltin(command);
		++executedSegments;
		return true;
	}

	void discardIncomingPipe(long long targetSegment) {
		/*
			Assume:
			numberedPipes = {
				5 -> PipeFD{readFd=10, writeFd=11},
				8 -> PipeFD{readFd=12, writeFd=13}
			}
			so 
		*/
		auto incomingIt = numberedPipes.find(targetSegment);
		if (incomingIt == numberedPipes.end()) {
			return;
		}
		// so you can close corresponding Pipe
		closePipe(incomingIt->second);
		numberedPipes.erase(incomingIt);
	}

	void executeExternalCommands(const std::vector<Command> &commands, const LinePlan &linePlan) {
		std::vector<bool> waitable = buildWaitability(commands);
		std::vector<pid_t> waitList, backgroundList;
		int inFd = -1;

		for (size_t i = 0; i < commands.size(); ++i) {
			const Command &command = commands[i];
			if (command.argv.empty()) {
				continue;
			}

			long long segmentIndex = linePlan.commandSegments[i];
			if (linePlan.segmentStarts[i]) {
				attachIncomingNumberedPipe(segmentIndex, inFd);
			}

			IoPlan ioPlan;
			if (!prepareIoPlan(command, segmentIndex, ioPlan, inFd)) {
				continue;
			}

			pid_t childPid = forkOrReapUntilSuccess();
			if (childPid == 0) {
				runChildProcess(command, ioPlan, inFd);
			}

			if (waitable[i]) {
				waitList.push_back(childPid);
			} else {
				backgroundList.push_back(childPid);
			}

			closeFd(inFd);
			inFd = ioPlan.nextInFd;
			closeParentIo(ioPlan);
		}

		closeFd(inFd);
		for (pid_t childPid : waitList) {
			waitpid(childPid, nullptr, 0);
		}

		graceReap(backgroundList);
	}

	void attachIncomingNumberedPipe(long long segmentIndex, int &inFd) {
		auto incomingIt = numberedPipes.find(segmentIndex);
		if (incomingIt == numberedPipes.end()) {
			return;
		}

		if (inFd < 0) {
			inFd = incomingIt->second.readFd;
		} else if (inFd != incomingIt->second.readFd) {
			closeFd(incomingIt->second.readFd);
		}

		closeFd(incomingIt->second.writeFd);
		numberedPipes.erase(incomingIt);
	}

	bool prepareIoPlan(const Command &command, long long segmentIndex, IoPlan &ioPlan, int &inFd) {
		// Only one output route is active: redirection, ordinary pipe, numbered pipe, or stdout.
		if (command.redirectStdout) {
			ioPlan.redirectFd = open(command.redirectFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (ioPlan.redirectFd < 0) {
				std::perror("open");
				closeFd(inFd);
				return false;
			}
			ioPlan.outFd = ioPlan.redirectFd;
			return true;
		}

		if (command.nextType == NextType::ORDINARY_PIPE) {
			ioPlan.ordinaryPipe = createPipeOrDie();
			ioPlan.outFd = ioPlan.ordinaryPipe.writeFd;
			ioPlan.nextInFd = ioPlan.ordinaryPipe.readFd;
			return true;
		}

		if (!isNumberPipeType(command.nextType)) {
			return true;
		}

		long long targetSegment = segmentIndex + command.nextCount;
		PipeFD &targetPipe = numberedPipes[targetSegment];
		if (targetPipe.readFd < 0 && targetPipe.writeFd < 0) {
			targetPipe = createPipeOrDie();
		}

		ioPlan.outFd = targetPipe.writeFd;
		if (command.nextType == NextType::NUMBER_PIPE_BOTH) {
			ioPlan.errFd = ioPlan.outFd;
		}
		return true;
	}

	[[noreturn]] void runChildProcess(const Command &command, const IoPlan &ioPlan, int inFd) {
		// The child only keeps the FDs that its command really needs.
		if (inFd >= 0) {
			dup2(inFd, STDIN_FILENO);
		}
		if (ioPlan.outFd >= 0 && ioPlan.outFd != STDOUT_FILENO) {
			dup2(ioPlan.outFd, STDOUT_FILENO);
		}
		if (ioPlan.errFd != STDERR_FILENO) {
			dup2(ioPlan.errFd, STDERR_FILENO);
		}

		closeChildIo(ioPlan, inFd);
		closeAllNumberedPipesInChild();

		std::vector<char *> argv;
		argv.reserve(command.argv.size() + 1);
		for (const std::string &arg : command.argv) {
			argv.push_back(const_cast<char *>(arg.c_str()));
		}
		argv.push_back(nullptr);

		execvp(argv[0], argv.data());
		std::cerr << "Unknown command: [" << command.argv[0] << "]." << std::endl;
		_exit(0);
	}

	void closeChildIo(const IoPlan &ioPlan, int inFd) const {
		int ordinaryReadFd = ioPlan.ordinaryPipe.readFd;
		int ordinaryWriteFd = ioPlan.ordinaryPipe.writeFd;
		int redirectFd = ioPlan.redirectFd;
		int childInFd = inFd;
		int childOutFd = ioPlan.outFd;

		closeFd(ordinaryReadFd);
		closeFd(ordinaryWriteFd);
		closeFd(redirectFd);
		closeFd(childInFd);
		if (childOutFd >= 0 && childOutFd != STDOUT_FILENO) {
			closeFd(childOutFd);
		}
	}

	void closeAllNumberedPipesInChild() const {
		// Close inherited delayed pipes so only the intended writers/readers stay alive.
		for (const auto &entry : numberedPipes) {
			int readFd = entry.second.readFd;
			int writeFd = entry.second.writeFd;
			closeFd(readFd);
			closeFd(writeFd);
		}
	}

	void closeParentIo(const IoPlan &ioPlan) const {
		int ordinaryWriteFd = ioPlan.ordinaryPipe.writeFd;
		int redirectFd = ioPlan.redirectFd;
		closeFd(ordinaryWriteFd);
		closeFd(redirectFd);
	}
};

}  // namespace shellcore

#endif
