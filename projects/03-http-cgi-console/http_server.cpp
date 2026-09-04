#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <boost/asio.hpp>

#include "console_components.hpp"

using boost::asio::ip::tcp;

namespace {

bool is_valid_cgi_path(const std::string& path) {
  if (path.size() < 2 || path.front() != '/') {
    return false;
  }
  if (path.find("..") != std::string::npos) {
    return false;
  }
  return path.size() >= 4 && path.substr(path.size() - 4) == ".cgi";
}

void reap_children(int) {
  while (waitpid(-1, nullptr, WNOHANG) > 0) {
  }
}

void set_env(const std::string& name, const std::string& value) {
  setenv(name.c_str(), value.c_str(), 1);
}

class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  explicit HttpSession(tcp::socket socket) : socket_(std::move(socket)) {}

  void start() { do_read(); }

 private:
  void do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(data_),
        [this, self](const boost::system::error_code& ec, std::size_t length) {
          if (ec) {
            return;
          }

          request_data_.append(data_.data(), length);
          if (request_data_.find("\r\n\r\n") == std::string::npos) {
            do_read();
            return;
          }

          HttpRequest request = parse_http_request(request_data_);
          if (request.path != "/panel.cgi") {
            send_forbidden();
            return;
          }
          execute_cgi(request);
        });
  }

  void send_forbidden() {
    response_ = "HTTP/1.1 403 Forbidden\r\n"
                "Content-type: text/plain\r\n\r\n"
                "Forbidden\n";

    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(response_),
        [this, self](const boost::system::error_code&, std::size_t) {
          boost::system::error_code ignored;
#if defined(BOOST_ASIO_NO_DEPRECATED)
          socket_.close(ignored);
#else
          boost::system::error_code close_result = socket_.close(ignored);
          (void)close_result;
#endif
        });
  }

  void execute_cgi(const HttpRequest& request) {
    tcp::endpoint local_endpoint = socket_.local_endpoint();
    tcp::endpoint remote_endpoint = socket_.remote_endpoint();
    int socket_fd = socket_.native_handle();

    pid_t pid = fork();
    if (pid < 0) {
      return;
    }

    if (pid == 0) {
      std::string executable = "." + request.path;

      dup2(socket_fd, STDOUT_FILENO);
      dup2(socket_fd, STDERR_FILENO);

      if (!is_valid_cgi_path(request.path) ||
          access(executable.c_str(), X_OK) != 0) {
        std::cout << "HTTP/1.1 404 Not Found\r\n"
                  << "Content-type: text/plain\r\n\r\n"
                  << "Not Found\n"
                  << std::flush;
        _exit(0);
      }

      set_env("REQUEST_METHOD", request.method);
      set_env("REQUEST_URI", request.uri);
      set_env("QUERY_STRING", request.query);
      set_env("SERVER_PROTOCOL", request.protocol);
      set_env("HTTP_HOST", request.host);
      set_env("SERVER_ADDR", local_endpoint.address().to_string());
      set_env("SERVER_PORT", std::to_string(local_endpoint.port()));
      set_env("REMOTE_ADDR", remote_endpoint.address().to_string());
      set_env("REMOTE_PORT", std::to_string(remote_endpoint.port()));

      std::cout << "HTTP/1.1 200 OK\r\n" << std::flush;

      char* argv[] = {const_cast<char*>(executable.c_str()), nullptr};
      execvp(argv[0], argv);
      std::cout << "Content-type: text/plain\r\n\r\n";
      std::cout << "exec failed: " << executable << "\n";
      std::cout.flush();
      _exit(1);
    }

    boost::system::error_code ignored;
#if defined(BOOST_ASIO_NO_DEPRECATED)
    socket_.close(ignored);
#else
    boost::system::error_code close_result = socket_.close(ignored);
    (void)close_result;
#endif
  }

  tcp::socket socket_;
  std::array<char, 8192> data_;
  std::string request_data_;
  std::string response_;
};

class HttpServer {
 public:
  HttpServer(boost::asio::io_context& io_context, unsigned short port)
      : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
    do_accept();
  }

 private:
  void do_accept() {
    acceptor_.async_accept(
        [this](const boost::system::error_code& ec, tcp::socket socket) {
          if (!ec) {
            std::make_shared<HttpSession>(std::move(socket))->start();
          }
          do_accept();
        });
  }

  tcp::acceptor acceptor_;
};

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: ./http_server [port]\n";
    return 1;
  }

  std::signal(SIGCHLD, reap_children);

  try {
    boost::asio::io_context io_context;
    HttpServer server(io_context,
                      static_cast<unsigned short>(std::atoi(argv[1])));
    io_context.run();
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }

  return 0;
}
