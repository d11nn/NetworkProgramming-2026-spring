#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

#include <boost/asio.hpp>

#include "console_components.hpp"

int main() {
  try {
    std::string query = std::getenv("QUERY_STRING") == nullptr
                            ? ""
                            : std::getenv("QUERY_STRING");
    std::vector<RemoteSessionConfig> sessions = parse_remote_sessions(query);
    SocksConfig socks_config = parse_socks_config(query);

    std::cout << "Content-type: text/html\r\n\r\n";
    std::cout << render_console_page(sessions) << std::flush;

    if (socks_config.host.empty() || socks_config.port.empty()) {
      return 0;
    }

    boost::asio::io_context io_context;
    for (std::size_t i = 0; i < sessions.size(); ++i) {
      std::make_shared<SocksBatchSession>(
          io_context, sessions[i], socks_config, static_cast<int>(i),
          [](const std::string& output) {
            std::cout << output << std::flush;
          })
          ->start();
    }
    io_context.run();
  } catch (const std::exception& e) {
    std::cout << "Content-type: text/plain\r\n\r\n";
    std::cout << e.what() << "\n";
  }

  return 0;
}
