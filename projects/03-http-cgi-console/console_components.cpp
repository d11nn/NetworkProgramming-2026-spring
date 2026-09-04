#include "console_components.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>
#include <sstream>
#include <utility>

namespace {

std::string url_decode(const std::string& value) {
  std::string decoded;
  decoded.reserve(value.size());

  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size() &&
        std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
      std::string hex = value.substr(i + 1, 2);
      char ch = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
      decoded.push_back(ch);
      i += 2;
    } else if (value[i] == '+') {
      decoded.push_back(' ');
    } else {
      decoded.push_back(value[i]);
    }
  }

  return decoded;
}

std::map<std::string, std::string> parse_query_map(const std::string& query) {
  std::map<std::string, std::string> params;
  std::size_t begin = 0;

  while (begin <= query.size()) {
    std::size_t end = query.find('&', begin);
    std::string item =
        query.substr(begin, end == std::string::npos ? std::string::npos
                                                     : end - begin);
    if (!item.empty()) {
      std::size_t equal = item.find('=');
      std::string key = url_decode(item.substr(0, equal));
      std::string value =
          equal == std::string::npos ? "" : url_decode(item.substr(equal + 1));
      params[key] = value;
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }

  return params;
}

std::string html_escape(const std::string& content) {
  std::string escaped;
  escaped.reserve(content.size());

  for (char ch : content) {
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
      case '\n':
        escaped += "&NewLine;";
        break;
      case '\r':
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }

  return escaped;
}

std::string js_string_escape(const std::string& content) {
  std::string escaped;
  escaped.reserve(content.size());

  for (char ch : content) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\'':
        escaped += "\\'";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }

  return escaped;
}

std::vector<std::string> load_commands(const std::string& file_name) {
  std::ifstream file("test_case/" + file_name);
  if (!file) {
    throw std::runtime_error("cannot open test_case/" + file_name);
  }

  std::vector<std::string> commands;
  std::string line;

  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    commands.push_back(line + "\n");
  }

  return commands;
}

}  // namespace

HttpRequest parse_http_request(const std::string& raw_request) {
  HttpRequest request;
  std::istringstream stream(raw_request);
  stream >> request.method >> request.target >> request.protocol;
  request.uri = request.target;

  std::size_t query_pos = request.target.find('?');
  request.path = request.target.substr(0, query_pos);
  if (query_pos != std::string::npos) {
    request.query = request.target.substr(query_pos + 1);
  }

  std::string line;
  std::getline(stream, line);
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      break;
    }
    std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') {
      value.erase(value.begin());
    }
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    if (name == "host") {
      request.host = value;
    }
  }

  return request;
}

std::vector<RemoteSessionConfig> parse_remote_sessions(
    const std::string& query) {
  std::map<std::string, std::string> params = parse_query_map(query);
  std::vector<RemoteSessionConfig> sessions;

  for (int i = 0; i < 5; ++i) {
    RemoteSessionConfig config;
    config.host = params["h" + std::to_string(i)];
    config.port = params["p" + std::to_string(i)];
    config.file = params["f" + std::to_string(i)];
    if (!config.host.empty() && !config.port.empty() && !config.file.empty()) {
      sessions.push_back(config);
    }
  }

  return sessions;
}

std::string render_console_page(
    const std::vector<RemoteSessionConfig>& sessions) {
  std::ostringstream html;
  html << R"(<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <title>NP Project 3 Console</title>
    <link
      rel="stylesheet"
      href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css"
      crossorigin="anonymous"
    />
    <link
      href="https://fonts.googleapis.com/css?family=Source+Code+Pro"
      rel="stylesheet"
    />
    <link
      rel="icon"
      type="image/png"
      href="https://cdn0.iconfinder.com/data/icons/small-n-flat/24/678068-terminal-512.png"
    />
    <style>
      * {
        font-family: 'Source Code Pro', monospace;
        font-size: 1rem !important;
      }
      body {
        background-color: #212529;
      }
      pre {
        color: #cccccc;
      }
      b {
        color: #01b468;
      }
    </style>
  </head>
  <body>
    <table class="table table-dark table-bordered">
      <thead>
        <tr>
)";

  for (const auto& session : sessions) {
    html << "          <th scope=\"col\">" << html_escape(session.host) << ":"
         << html_escape(session.port) << "</th>\n";
  }

  html << R"(        </tr>
      </thead>
      <tbody>
        <tr>
)";

  for (std::size_t i = 0; i < sessions.size(); ++i) {
    html << "          <td><pre id=\"s" << i
         << "\" class=\"mb-0\"></pre></td>\n";
  }

  html << R"(        </tr>
      </tbody>
    </table>
  </body>
</html>
)";

  return html.str();
}

std::string render_panel_page() {
  std::ostringstream html;
  html << R"(<!DOCTYPE html>
<html lang="en">
  <head>
    <title>NP Project 3 Panel</title>
    <link
      rel="stylesheet"
      href="https://cdn.jsdelivr.net/npm/bootstrap@4.5.3/dist/css/bootstrap.min.css"
      crossorigin="anonymous"
    />
    <link
      href="https://fonts.googleapis.com/css?family=Source+Code+Pro"
      rel="stylesheet"
    />
    <link
      rel="icon"
      type="image/png"
      href="https://cdn4.iconfinder.com/data/icons/iconsimple-setting-time/512/dashboard-512.png"
    />
    <style>
      * {
        font-family: 'Source Code Pro', monospace;
      }
    </style>
  </head>
  <body class="bg-secondary pt-5">
    <form action="console.cgi" method="GET">
      <table class="table mx-auto bg-light" style="width: inherit">
        <thead class="thead-dark">
          <tr>
            <th scope="col">#</th>
            <th scope="col">Host</th>
            <th scope="col">Port</th>
            <th scope="col">Input File</th>
          </tr>
        </thead>
        <tbody>
)";

  for (int i = 0; i < 5; ++i) {
    html << "          <tr>\n"
         << "            <th scope=\"row\" class=\"align-middle\">Session "
         << (i + 1) << "</th>\n"
         << "            <td>\n"
         << "              <div class=\"input-group\">\n"
         << "                <select name=\"h" << i
         << "\" class=\"custom-select\">\n"
         << "                  <option></option>\n";
    for (int host = 1; host <= 12; ++host) {
      html << "                  <option value=\"nplinux" << host
           << ".cs.nycu.edu.tw\">nplinux" << host << "</option>\n";
    }
    html << "                </select>\n"
         << "                <div class=\"input-group-append\">\n"
         << "                  <span class=\"input-group-text\">.cs.nycu.edu.tw</span>\n"
         << "                </div>\n"
         << "              </div>\n"
         << "            </td>\n"
         << "            <td>\n"
         << "              <input name=\"p" << i
         << "\" type=\"text\" class=\"form-control\" size=\"5\" />\n"
         << "            </td>\n"
         << "            <td>\n"
         << "              <select name=\"f" << i
         << "\" class=\"custom-select\">\n"
         << "                <option></option>\n";
    for (int file = 1; file <= 5; ++file) {
      html << "                <option value=\"t" << file << ".txt\">t" << file
           << ".txt</option>\n";
    }
    html << "              </select>\n"
         << "            </td>\n"
         << "          </tr>\n";
  }

  html << R"(          <tr>
            <td colspan="3"></td>
            <td>
              <button type="submit" class="btn btn-info btn-block">Run</button>
            </td>
          </tr>
        </tbody>
      </table>
    </form>
  </body>
</html>
)";

  return html.str();
}

std::string make_console_script(int session_id, const std::string& content,
                                bool command) {
  std::string escaped = js_string_escape(html_escape(content));
  std::ostringstream script;
  script << "<script>document.getElementById('s" << session_id
         << "').innerHTML += '";
  if (command) {
    script << "<b>";
  }
  script << escaped;
  if (command) {
    script << "</b>";
  }
  script << "';</script>\n";
  return script.str();
}

RemoteBatchSession::RemoteBatchSession(boost::asio::io_context& io_context,
                                       RemoteSessionConfig config,
                                       int session_id, OutputCallback output,
                                       DoneCallback done)
    : io_context_(io_context),
      resolver_(io_context),
      socket_(io_context),
      config_(std::move(config)),
      session_id_(session_id),
      output_(std::move(output)),
      done_(std::move(done)),
      next_command_(0),
      last_char_was_percent_(false),
      finished_(false) {}

void RemoteBatchSession::start() {
  try {
    commands_ = load_commands(config_.file);
  } catch (const std::exception& e) {
    output_(make_console_script(session_id_, std::string(e.what()) + "\n",
                                false));
    finish();
    return;
  }
  do_resolve();
}

void RemoteBatchSession::do_resolve() {
  auto self = shared_from_this();
  resolver_.async_resolve(
      config_.host, config_.port,
      [this, self](const boost::system::error_code& ec,
                   boost::asio::ip::tcp::resolver::results_type endpoints) {
        if (ec) {
          output_(make_console_script(session_id_, ec.message() + "\n", false));
          finish();
          return;
        }
        do_connect(endpoints);
      });
}

void RemoteBatchSession::do_connect(
    const boost::asio::ip::tcp::resolver::results_type& endpoints) {
  auto self = shared_from_this();
  boost::asio::async_connect(
      socket_, endpoints,
      [this, self](const boost::system::error_code& ec,
                   const boost::asio::ip::tcp::endpoint&) {
        if (ec) {
          output_(make_console_script(session_id_, ec.message() + "\n", false));
          finish();
          return;
        }
        do_read();
      });
}

void RemoteBatchSession::do_read() {
  auto self = shared_from_this();
  socket_.async_read_some(
      boost::asio::buffer(data_),
      [this, self](const boost::system::error_code& ec, std::size_t length) {
        if (ec) {
          finish();
          return;
        }

        std::string content(data_.data(), length);
        output_(make_console_script(session_id_, content, false));
        bool has_prompt = content.find("% ") != std::string::npos ||
                          (last_char_was_percent_ && !content.empty() &&
                           content.front() == ' ');
        last_char_was_percent_ = !content.empty() && content.back() == '%';
        if (has_prompt) {
          send_next_command();
        } else {
          do_read();
        }
      });
}

void RemoteBatchSession::send_next_command() {
  if (next_command_ >= commands_.size()) {
    finish();
    return;
  }

  std::string command = commands_[next_command_++];
  output_(make_console_script(session_id_, command, true));

  auto self = shared_from_this();
  boost::asio::async_write(
      socket_, boost::asio::buffer(command),
      [this, self](const boost::system::error_code& ec, std::size_t) {
        if (ec) {
          finish();
          return;
        }
        do_read();
      });
}

void RemoteBatchSession::finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  boost::system::error_code ignored;
#if defined(BOOST_ASIO_NO_DEPRECATED)
  socket_.close(ignored);
#else
  boost::system::error_code close_result = socket_.close(ignored);
  (void)close_result;
#endif
  if (done_) {
    done_();
  }
}
