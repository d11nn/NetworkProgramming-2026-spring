#ifndef CONSOLE_COMPONENTS_HPP
#define CONSOLE_COMPONENTS_HPP

#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

using boost::asio::ip::tcp;

struct RemoteSessionConfig {
  std::string host;
  std::string port;
  std::string file;
};

struct SocksConfig {
  std::string host;
  std::string port;
};

inline int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  return -1;
}

inline std::string url_decode(const std::string& value) {
  std::string decoded;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '+') {
      decoded.push_back(' ');
    } else if (value[i] == '%' && i + 2 < value.size()) {
      int high = hex_value(value[i + 1]);
      int low = hex_value(value[i + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
      } else {
        decoded.push_back(value[i]);
      }
    } else {
      decoded.push_back(value[i]);
    }
  }
  return decoded;
}

inline std::map<std::string, std::string> parse_query_params(
    const std::string& query) {
  std::map<std::string, std::string> params;
  std::size_t begin = 0;
  while (begin <= query.size()) {
    std::size_t end = query.find('&', begin);
    std::string pair =
        query.substr(begin, end == std::string::npos ? std::string::npos
                                                     : end - begin);
    if (!pair.empty()) {
      std::size_t eq = pair.find('=');
      std::string key = url_decode(pair.substr(0, eq));
      std::string value =
          eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1));
      params[key] = value;
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return params;
}

inline std::vector<RemoteSessionConfig> parse_remote_sessions(
    const std::string& query) {
  auto params = parse_query_params(query);
  std::vector<RemoteSessionConfig> sessions;
  for (int i = 0; i < 5; ++i) {
    RemoteSessionConfig session;
    session.host = params["h" + std::to_string(i)];
    session.port = params["p" + std::to_string(i)];
    session.file = params["f" + std::to_string(i)];
    if (!session.host.empty() && !session.port.empty() &&
        !session.file.empty()) {
      sessions.push_back(session);
    }
  }
  return sessions;
}

inline SocksConfig parse_socks_config(const std::string& query) {
  auto params = parse_query_params(query);
  return SocksConfig{params["sh"], params["sp"]};
}

inline std::string html_escape(const std::string& value) {
  std::string escaped;
  for (char ch : value) {
    switch (ch) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&#39;";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

inline std::string js_escape(const std::string& value) {
  std::ostringstream escaped;
  for (unsigned char ch : value) {
    switch (ch) {
      case '\\':
        escaped << "\\\\";
        break;
      case '\'':
        escaped << "\\'";
        break;
      case '\n':
        escaped << "\\n";
        break;
      case '\r':
        break;
      case '<':
        escaped << "\\x3C";
        break;
      case '>':
        escaped << "\\x3E";
        break;
      case '&':
        escaped << "\\x26";
        break;
      default:
        if (std::isprint(ch)) {
          escaped << ch;
        } else {
          escaped << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(ch) << std::dec;
        }
        break;
    }
  }
  return escaped.str();
}

inline std::string render_console_page(
    const std::vector<RemoteSessionConfig>& sessions) {
  std::ostringstream html;
  html << "<!DOCTYPE html>\n"
       << "<html lang=\"en\">\n"
       << "<head>\n"
       << "  <meta charset=\"UTF-8\" />\n"
       << "  <title>NP Project 4 Console</title>\n"
       << "  <link rel=\"stylesheet\" "
       << "href=\"https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/"
          "bootstrap.min.css\" />\n"
       << "  <style>* { font-family: 'Source Code Pro', monospace; } "
       << "body { background-color: #212529; } "
       << "pre { color: #f8f9fa; white-space: pre-wrap; }</style>\n"
       << "</head>\n"
       << "<body>\n"
       << "  <table class=\"table table-dark table-bordered\">\n"
       << "    <thead><tr>";
  for (const auto& session : sessions) {
    html << "<th>" << html_escape(session.host) << ":"
         << html_escape(session.port) << "</th>";
  }
  html << "</tr></thead>\n"
       << "    <tbody><tr>";
  for (std::size_t i = 0; i < sessions.size(); ++i) {
    html << "<td><pre id=\"s" << i << "\" class=\"mb-0\"></pre></td>";
  }
  html << "</tr></tbody>\n"
       << "  </table>\n"
       << "</body>\n"
       << "</html>\n";
  return html.str();
}

inline std::vector<std::string> load_commands(const std::string& file) {
  std::ifstream input("test_case/" + file);
  std::vector<std::string> commands;
  std::string line;
  while (std::getline(input, line)) {
    commands.push_back(line + "\n");
  }
  return commands;
}

class RemoteBatchSession
    : public std::enable_shared_from_this<RemoteBatchSession> {
 public:
  using OutputCallback = std::function<void(const std::string&)>;

  RemoteBatchSession(boost::asio::io_context& io_context,
                     RemoteSessionConfig config, int session_id,
                     OutputCallback output)
      : io_context_(io_context),
        resolver_(io_context),
        socket_(io_context),
        config_(std::move(config)),
        session_id_(session_id),
        output_(std::move(output)),
        commands_(load_commands(config_.file)) {}

  virtual void start() { resolve_remote(config_.host, config_.port); }

 protected:
  void resolve_remote(const std::string& host, const std::string& port) {
    auto self = shared_from_this();
    resolver_.async_resolve(
        host, port,
        [this, self](const boost::system::error_code& ec,
                     const tcp::resolver::results_type& endpoints) {
          if (!ec) {
            connect_remote(endpoints);
          }
        });
  }

  virtual void connect_remote(const tcp::resolver::results_type& endpoints) {
    auto self = shared_from_this();
    boost::asio::async_connect(
        socket_, endpoints,
        [this, self](const boost::system::error_code& ec,
                     const tcp::endpoint&) {
          if (!ec) {
            read_remote();
          }
        });
  }

  void read_remote() {
    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(buffer_),
        [this, self](const boost::system::error_code& ec,
                     std::size_t length) {
          if (ec) {
            return;
          }
          std::string data(buffer_.data(), length);
          output_text(data);
          if (received_prompt(data)) {
            write_next_command();
          } else {
            read_remote();
          }
        });
  }

  bool received_prompt(const std::string& data) {
    std::string combined = prompt_tail_ + data;
    if (combined.find("% ") != std::string::npos) {
      prompt_tail_.clear();
      return true;
    }
    prompt_tail_ = combined.empty() ? "" : combined.substr(combined.size() - 1);
    return false;
  }

  void write_next_command() {
    if (next_command_ >= commands_.size()) {
      read_remote();
      return;
    }

    pending_command_ = commands_[next_command_++];
    output_text("<b>" + html_escape(pending_command_) + "</b>", false);
    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(pending_command_),
        [this, self](const boost::system::error_code& ec, std::size_t) {
          if (!ec) {
            read_remote();
          }
        });
  }

  void output_text(const std::string& text, bool escape_html = true) {
    std::string html = escape_html ? html_escape(text) : text;
    std::ostringstream script;
    script << "<script>document.getElementById('s" << session_id_
           << "').innerHTML += '" << js_escape(html) << "';</script>";
    output_(script.str());
  }

  boost::asio::io_context& io_context_;
  tcp::resolver resolver_;
  tcp::socket socket_;
  RemoteSessionConfig config_;
  int session_id_ = 0;
  OutputCallback output_;
  std::array<char, 4096> buffer_{};
  std::vector<std::string> commands_;
  std::size_t next_command_ = 0;
  std::string pending_command_;
  std::string prompt_tail_;
};

class SocksBatchSession : public RemoteBatchSession {
 public:
  SocksBatchSession(boost::asio::io_context& io_context,
                    RemoteSessionConfig config, SocksConfig socks_config,
                    int session_id, OutputCallback output)
      : RemoteBatchSession(io_context, std::move(config), session_id,
                           std::move(output)),
        socks_config_(std::move(socks_config)) {}

  void start() override { resolve_remote(socks_config_.host, socks_config_.port); }

 private:
  void connect_remote(const tcp::resolver::results_type& endpoints) override {
    auto self = shared_from_this();
    boost::asio::async_connect(
        socket_, endpoints,
        [this, self](const boost::system::error_code& ec,
                     const tcp::endpoint&) {
          if (!ec) {
            send_socks_request();
          }
        });
  }

  void send_socks_request() {
    socks_request_ = build_socks4a_request();
    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(socks_request_),
        [this, self](const boost::system::error_code& ec, std::size_t) {
          if (!ec) {
            read_socks_reply();
          }
        });
  }

  std::vector<unsigned char> build_socks4a_request() const {
    std::uint16_t port = static_cast<std::uint16_t>(std::stoi(config_.port));
    std::vector<unsigned char> request;
    request.push_back(4);
    request.push_back(1);
    request.push_back(static_cast<unsigned char>((port >> 8) & 0xff));
    request.push_back(static_cast<unsigned char>(port & 0xff));
    request.push_back(0);
    request.push_back(0);
    request.push_back(0);
    request.push_back(1);
    request.push_back(0);
    request.insert(request.end(), config_.host.begin(), config_.host.end());
    request.push_back(0);
    return request;
  }

  void read_socks_reply() {
    auto self = shared_from_this();
    boost::asio::async_read(
        socket_, boost::asio::buffer(socks_reply_),
        [this, self](const boost::system::error_code& ec, std::size_t) {
          if (!ec && socks_reply_[1] == 90) {
            read_remote();
          }
        });
  }

  SocksConfig socks_config_;
  std::vector<unsigned char> socks_request_;
  std::array<unsigned char, 8> socks_reply_{};
};

#endif
