#include <array>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <chrono>

#include <boost/asio.hpp>

using boost::asio::ip::tcp;

namespace {

constexpr std::uint8_t kSocksVersion = 4;
constexpr std::uint8_t kConnect = 1;
constexpr std::uint8_t kBind = 2;
constexpr std::uint8_t kReplyGranted = 90;
constexpr std::uint8_t kReplyRejected = 91;

std::uint64_t get_or_create_ip_start_time(const std::string& ip) {
  std::string filename = "socks_session_" + ip;
  std::ifstream infile(filename);
  std::uint64_t start_time = 0;
  if (infile >> start_time) {
    return start_time;
  }
  std::uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  std::ofstream outfile(filename);
  outfile << now;
  return now;
}

bool is_time_limit_exceeded(std::uint64_t start_time) {
  std::uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  return (now < start_time) || (now - start_time > 20);
}

struct SocksRequest {
  std::uint8_t vn = 0;
  std::uint8_t cd = 0;
  std::uint16_t dst_port = 0;
  std::array<unsigned char, 4> raw_dst_ip{};
  std::string dst_ip;
  std::string user_id;
  std::string domain_name;
};

struct FirewallRule {
  std::uint8_t command = 0;
  std::array<std::string, 4> octets{};
};

struct Destination {
  tcp::endpoint endpoint;
  std::string ip;
};

std::string command_name(std::uint8_t command) {
  if (command == kConnect) {
    return "CONNECT";
  }
  if (command == kBind) {
    return "BIND";
  }
  return "UNKNOWN";
}

std::string ip_to_string(const std::array<unsigned char, 4>& ip) {
  std::ostringstream oss;
  oss << static_cast<int>(ip[0]) << "." << static_cast<int>(ip[1]) << "."
      << static_cast<int>(ip[2]) << "." << static_cast<int>(ip[3]);
  return oss.str();
}

bool is_socks4a_ip(const std::array<unsigned char, 4>& ip) {
  return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] != 0;
}

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string part;
  std::istringstream iss(value);
  while (std::getline(iss, part, delimiter)) {
    parts.push_back(part);
  }
  return parts;
}

std::string read_null_terminated(tcp::socket& socket) {
  std::string value;
  for (;;) {
    char ch = 0;
    boost::asio::read(socket, boost::asio::buffer(&ch, 1));
    if (ch == '\0') {
      break;
    }
    value.push_back(ch);
  }
  return value;
}

SocksRequest read_socks_request(tcp::socket& socket) {
  std::array<unsigned char, 8> header{};
  boost::asio::read(socket, boost::asio::buffer(header));

  SocksRequest request;
  request.vn = header[0];
  request.cd = header[1];
  request.dst_port =
      static_cast<std::uint16_t>(header[2] << 8) | header[3];
  request.raw_dst_ip = {header[4], header[5], header[6], header[7]};
  request.dst_ip = ip_to_string(request.raw_dst_ip);
  request.user_id = read_null_terminated(socket);
  if (is_socks4a_ip(request.raw_dst_ip)) {
    request.domain_name = read_null_terminated(socket);
  }
  return request;
}

std::array<unsigned char, 4> parse_ipv4(const std::string& ip) {
  std::array<unsigned char, 4> result{};
  auto parts = split(ip, '.');
  if (parts.size() != 4) {
    return result;
  }
  for (std::size_t i = 0; i < 4; ++i) {
    result[i] = static_cast<unsigned char>(std::stoi(parts[i]));
  }
  return result;
}

void write_socks_reply(tcp::socket& socket, std::uint8_t reply,
                       std::uint16_t port = 0,
                       const std::string& ip = "0.0.0.0") {
  auto ip_bytes = parse_ipv4(ip);
  std::array<unsigned char, 8> packet{
      0,
      reply,
      static_cast<unsigned char>((port >> 8) & 0xff),
      static_cast<unsigned char>(port & 0xff),
      ip_bytes[0],
      ip_bytes[1],
      ip_bytes[2],
      ip_bytes[3],
  };
  boost::asio::write(socket, boost::asio::buffer(packet));
}

std::vector<FirewallRule> load_firewall_rules() {
  std::ifstream file("socks.conf");
  if (!file) {
    return {
        FirewallRule{kConnect, {"*", "*", "*", "*"}},
        FirewallRule{kBind, {"*", "*", "*", "*"}},
    };
  }

  std::vector<FirewallRule> rules;
  std::string line;
  while (std::getline(file, line)) {
    auto comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }

    std::istringstream iss(line);
    std::string action;
    char command = '\0';
    std::string ip_pattern;
    if (!(iss >> action >> command >> ip_pattern)) {
      continue;
    }
    if (action != "permit") {
      continue;
    }

    auto octets = split(ip_pattern, '.');
    if (octets.size() != 4) {
      continue;
    }
    if (command == 'c') {
      rules.push_back(FirewallRule{
          kConnect, {octets[0], octets[1], octets[2], octets[3]}});
    } else if (command == 'b') {
      rules.push_back(FirewallRule{
          kBind, {octets[0], octets[1], octets[2], octets[3]}});
    }
  }
  return rules;
}

bool firewall_allows(std::uint8_t command, const std::string& dst_ip) {
  auto dst_octets = split(dst_ip, '.');
  if (dst_octets.size() != 4) {
    return false;
  }

  for (const auto& rule : load_firewall_rules()) {
    if (rule.command != command) {
      continue;
    }

    bool matched = true;
    for (std::size_t i = 0; i < 4; ++i) {
      if (rule.octets[i] != "*" && rule.octets[i] != dst_octets[i]) {
        matched = false;
        break;
      }
    }
    if (matched) {
      return true;
    }
  }
  return false;
}

Destination resolve_destination(boost::asio::io_context& io_context,
                                const SocksRequest& request) {
  if (!request.domain_name.empty()) {
    tcp::resolver resolver(io_context);
    auto endpoints =
        resolver.resolve(request.domain_name, std::to_string(request.dst_port));
    for (const auto& entry : endpoints) {
      if (entry.endpoint().address().is_v4()) {
        return Destination{entry.endpoint(), entry.endpoint().address().to_string()};
      }
    }
    throw std::runtime_error("no ipv4 endpoint");
  }

  auto address = boost::asio::ip::make_address_v4(request.dst_ip);
  return Destination{tcp::endpoint(address, request.dst_port), request.dst_ip};
}

void print_server_message(const tcp::socket& socket,
                          const SocksRequest& request,
                          const std::string& dst_ip,
                          const std::string& reply) {
  auto endpoint = socket.remote_endpoint();
  std::cout << "<S_IP>: " << endpoint.address().to_string() << "\n"
            << "<S_PORT>: " << endpoint.port() << "\n"
            << "<D_IP>: " << dst_ip << "\n"
            << "<D_PORT>: " << request.dst_port << "\n"
            << "<Command>: " << command_name(request.cd) << "\n"
            << "<Reply>: " << reply << "\n"
            << std::flush;
}

void close_socket(tcp::socket& socket) {
  boost::system::error_code ignored;
  socket.shutdown(tcp::socket::shutdown_both, ignored);
  socket.close(ignored);
}

void relay_stream(tcp::socket& input, tcp::socket& output, std::uint64_t start_time) {
  std::array<char, 8192> data{};
  boost::system::error_code ec;
  for (;;) {
    if (is_time_limit_exceeded(start_time)) {
      break;
    }
    std::size_t length = input.read_some(boost::asio::buffer(data), ec);
    if (ec) {
      break;
    }
    if (is_time_limit_exceeded(start_time)) {
      break;
    }
    boost::asio::write(output, boost::asio::buffer(data.data(), length), ec);
    if (ec) {
      break;
    }
  }
  close_socket(input);
  close_socket(output);
}

void relay_bidirectional(tcp::socket& first, tcp::socket& second, std::uint64_t start_time) {
  std::thread first_to_second(relay_stream, std::ref(first), std::ref(second), start_time);
  std::thread second_to_first(relay_stream, std::ref(second), std::ref(first), start_time);
  first_to_second.join();
  second_to_first.join();
}

void handle_connect(boost::asio::io_context& io_context, tcp::socket& socket,
                    const SocksRequest& request,
                    const Destination& destination,
                    std::uint64_t start_time) {
  if (is_time_limit_exceeded(start_time)) {
    write_socks_reply(socket, kReplyRejected);
    print_server_message(socket, request, destination.ip, "Reject");
    return;
  }
  if (!firewall_allows(request.cd, destination.ip)) {
    write_socks_reply(socket, kReplyRejected);
    print_server_message(socket, request, destination.ip, "Reject");
    return;
  }

  tcp::socket destination_socket(io_context);
  boost::system::error_code ec;
  destination_socket.connect(destination.endpoint, ec);
  if (ec) {
    write_socks_reply(socket, kReplyRejected);
    print_server_message(socket, request, destination.ip, "Reject");
    return;
  }

  write_socks_reply(socket, kReplyGranted);
  print_server_message(socket, request, destination.ip, "Accept");
  relay_bidirectional(socket, destination_socket, start_time);
}

void handle_bind(boost::asio::io_context& io_context, tcp::socket& socket,
                 const SocksRequest& request, const Destination& destination,
                 std::uint64_t start_time) {
  if (is_time_limit_exceeded(start_time)) {
    write_socks_reply(socket, kReplyRejected);
    print_server_message(socket, request, destination.ip, "Reject");
    return;
  }
  if (!firewall_allows(request.cd, destination.ip)) {
    write_socks_reply(socket, kReplyRejected);
    print_server_message(socket, request, destination.ip, "Reject");
    return;
  }

  try {
    tcp::acceptor bind_acceptor(io_context, tcp::endpoint(tcp::v4(), 0));
    std::string bind_ip = socket.local_endpoint().address().to_string();
    std::uint16_t bind_port = bind_acceptor.local_endpoint().port();
    write_socks_reply(socket, kReplyGranted, bind_port, bind_ip);

    tcp::socket destination_socket(io_context);
    bind_acceptor.accept(destination_socket);
    auto peer_endpoint = destination_socket.remote_endpoint();
    std::string peer_ip = peer_endpoint.address().to_string();
    if (is_time_limit_exceeded(start_time)) {
      write_socks_reply(socket, kReplyRejected, peer_endpoint.port(), peer_ip);
      print_server_message(socket, request, destination.ip, "Reject");
      return;
    }
    if (peer_ip != destination.ip) {
      write_socks_reply(socket, kReplyRejected, peer_endpoint.port(), peer_ip);
      print_server_message(socket, request, destination.ip, "Reject");
      return;
    }

    write_socks_reply(socket, kReplyGranted, peer_endpoint.port(), peer_ip);
    print_server_message(socket, request, destination.ip, "Accept");
    relay_bidirectional(destination_socket, socket, start_time);
  } catch (const std::exception&) {
    write_socks_reply(socket, kReplyRejected);
    print_server_message(socket, request, destination.ip, "Reject");
  }
}

void handle_client(tcp::socket socket) {
  try {
    boost::asio::io_context io_context;
    std::string client_ip = socket.remote_endpoint().address().to_string();
    std::uint64_t start_time = get_or_create_ip_start_time(client_ip);

    auto request = read_socks_request(socket);
    if (request.vn != kSocksVersion ||
        (request.cd != kConnect && request.cd != kBind)) {
      write_socks_reply(socket, kReplyRejected);
      return;
    }

    auto destination = resolve_destination(io_context, request);
    if (request.cd == kConnect) {
      handle_connect(io_context, socket, request, destination, start_time);
      return;
    }

    handle_bind(io_context, socket, request, destination, start_time);
  } catch (const std::exception&) {
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc != 2) {
      return 1;
    }

    std::system("rm -f socks_session_*");

    std::signal(SIGCHLD, SIG_IGN);

    boost::asio::io_context io_context;
    tcp::acceptor acceptor(io_context,
                           tcp::endpoint(tcp::v4(), std::atoi(argv[1])));

    for (;;) {
      tcp::socket socket(io_context);
      acceptor.accept(socket);
      pid_t pid = fork();
      if (pid == 0) {
        boost::system::error_code ignored;
        acceptor.close(ignored);
        handle_client(std::move(socket));
        _exit(0);
      }
      socket.close();
    }
  } catch (const std::exception&) {
    return 1;
  }
}
