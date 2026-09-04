#include <array>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include "console_components.hpp"

using boost::asio::ip::tcp;

namespace {

class CgiHttpSession : public std::enable_shared_from_this<CgiHttpSession> {
 public:
  CgiHttpSession(boost::asio::io_context& io_context, tcp::socket socket)
      : io_context_(io_context),
        socket_(std::move(socket)),
        active_remotes_(0),
        closing_(false) {}

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
          dispatch(parse_http_request(request_data_));
        });
  }

  void dispatch(const HttpRequest& request) {
    if (request.path == "/panel.cgi") {
      enqueue("HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n" +
              render_panel_page());
      close_when_drained();
      return;
    }

    if (request.path == "/console.cgi") {
      std::vector<RemoteSessionConfig> sessions =
          parse_remote_sessions(request.query);
      enqueue("HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n" +
              render_console_page(sessions));
      start_remote_sessions(sessions);
      return;
    }

    enqueue("HTTP/1.1 404 Not Found\r\nContent-type: text/plain\r\n\r\n"
            "Not Found\n");
    close_when_drained();
  }

  void start_remote_sessions(const std::vector<RemoteSessionConfig>& sessions) {
    active_remotes_ = sessions.size();
    if (active_remotes_ == 0) {
      close_when_drained();
      return;
    }

    for (std::size_t i = 0; i < sessions.size(); ++i) {
      auto self = shared_from_this();
      auto remote = std::make_shared<RemoteBatchSession>(
          io_context_, sessions[i], static_cast<int>(i),
          [self](const std::string& output) { self->enqueue(output); },
          [self]() { self->remote_done(); });
      remotes_.push_back(remote);
      remote->start();
    }
  }

  void remote_done() {
    if (active_remotes_ > 0) {
      --active_remotes_;
    }
    if (active_remotes_ == 0) {
      close_when_drained();
    }
  }

  void enqueue(std::string output) {
    bool writing = !pending_.empty();
    pending_.push_back(std::move(output));
    if (!writing) {
      do_write();
    }
  }

  void do_write() {
    if (pending_.empty()) {
      if (closing_) {
        boost::system::error_code ignored;
#if defined(BOOST_ASIO_NO_DEPRECATED)
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
#else
        boost::system::error_code shutdown_result =
            socket_.shutdown(tcp::socket::shutdown_both, ignored);
        (void)shutdown_result;
        boost::system::error_code close_result = socket_.close(ignored);
        (void)close_result;
#endif
      }
      return;
    }

    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(pending_.front()),
        [this, self](const boost::system::error_code& ec, std::size_t) {
          if (ec) {
            boost::system::error_code ignored;
#if defined(BOOST_ASIO_NO_DEPRECATED)
            socket_.close(ignored);
#else
            boost::system::error_code close_result = socket_.close(ignored);
            (void)close_result;
#endif
            return;
          }
          pending_.pop_front();
          do_write();
        });
  }

  void close_when_drained() {
    closing_ = true;
    if (pending_.empty()) {
      do_write();
    }
  }

  boost::asio::io_context& io_context_;
  tcp::socket socket_;
  std::array<char, 8192> data_;
  std::string request_data_;
  std::deque<std::string> pending_;
  std::vector<std::shared_ptr<RemoteBatchSession>> remotes_;
  std::size_t active_remotes_;
  bool closing_;
};

class CgiServer {
 public:
  CgiServer(boost::asio::io_context& io_context, unsigned short port)
      : io_context_(io_context),
        acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
    do_accept();
  }

 private:
  void do_accept() {
    acceptor_.async_accept(
        [this](const boost::system::error_code& ec, tcp::socket socket) {
          if (!ec) {
            std::make_shared<CgiHttpSession>(io_context_, std::move(socket))
                ->start();
          }
          do_accept();
        });
  }

  boost::asio::io_context& io_context_;
  tcp::acceptor acceptor_;
};

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: cgi_server.exe [port]\n";
    return 1;
  }

  try {
    boost::asio::io_context io_context;
    CgiServer server(io_context, static_cast<unsigned short>(std::atoi(argv[1])));
    io_context.run();
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }

  return 0;
}
