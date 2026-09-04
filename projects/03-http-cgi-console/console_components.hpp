#ifndef CONSOLE_COMPONENTS_HPP
#define CONSOLE_COMPONENTS_HPP

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>

struct HttpRequest {
  std::string method;
  std::string uri;
  std::string target;
  std::string path;
  std::string query;
  std::string protocol;
  std::string host;
};

struct RemoteSessionConfig {
  std::string host;
  std::string port;
  std::string file;
};

HttpRequest parse_http_request(const std::string& raw_request);
std::vector<RemoteSessionConfig> parse_remote_sessions(const std::string& query);
std::string render_console_page(const std::vector<RemoteSessionConfig>& sessions);
std::string render_panel_page();
std::string make_console_script(int session_id, const std::string& content,
                                bool command);

class RemoteBatchSession
    : public std::enable_shared_from_this<RemoteBatchSession> {
 public:
  using OutputCallback = std::function<void(const std::string&)>;
  using DoneCallback = std::function<void()>;

  RemoteBatchSession(boost::asio::io_context& io_context,
                     RemoteSessionConfig config, int session_id,
                     OutputCallback output, DoneCallback done = DoneCallback());

  void start();

 private:
  void do_resolve();
  void do_connect(
      const boost::asio::ip::tcp::resolver::results_type& endpoints);
  void do_read();
  void send_next_command();
  void finish();

  boost::asio::io_context& io_context_;
  boost::asio::ip::tcp::resolver resolver_;
  boost::asio::ip::tcp::socket socket_;
  RemoteSessionConfig config_;
  int session_id_;
  OutputCallback output_;
  DoneCallback done_;
  std::vector<std::string> commands_;
  std::size_t next_command_;
  std::array<char, 4096> data_;
  bool last_char_was_percent_;
  bool finished_;
};

#endif
