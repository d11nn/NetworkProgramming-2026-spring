#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../console_components.hpp"

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_parse_http_request() {
  const std::string request =
      "GET /console.cgi?h0=host&p0=1234&f0=t1.txt HTTP/1.1\r\n"
      "Host: Example.COM:8080\r\n"
      "User-Agent: test\r\n\r\n";

  HttpRequest parsed = parse_http_request(request);
  expect(parsed.method == "GET", "method should be GET");
  expect(parsed.uri == "/console.cgi?h0=host&p0=1234&f0=t1.txt",
         "uri should preserve the raw target");
  expect(parsed.path == "/console.cgi", "path should exclude query string");
  expect(parsed.query == "h0=host&p0=1234&f0=t1.txt",
         "query string should be parsed");
  expect(parsed.protocol == "HTTP/1.1", "protocol should be parsed");
  expect(parsed.host == "Example.COM:8080", "Host header should be captured");
}

void test_parse_remote_sessions() {
  const std::string query =
      "h0=local%68ost&p0=7000&f0=t%201.txt&"
      "h1=&p1=&f1=&"
      "h2=remote&p2=7002&f2=t2.txt";
  std::vector<RemoteSessionConfig> sessions = parse_remote_sessions(query);

  expect(sessions.size() == 2, "only non-empty sessions should be kept");
  expect(sessions[0].host == "localhost", "host should be URL-decoded");
  expect(sessions[0].port == "7000", "port should be parsed");
  expect(sessions[0].file == "t 1.txt", "file should be URL-decoded");
  expect(sessions[1].host == "remote", "later sessions should be preserved");
  expect(sessions[1].port == "7002", "later session port should be parsed");
  expect(sessions[1].file == "t2.txt", "later session file should be parsed");
}

void test_render_panel_page() {
  std::string html = render_panel_page();
  expect(html.find("Session 5") != std::string::npos,
         "panel should render five session rows");
  expect(html.find("option value=\"t5.txt\"") != std::string::npos,
         "panel should offer the hard-coded t1..t5 test cases");
  expect(html.find("option value=\"nplinux12.cs.nycu.edu.tw\"") !=
             std::string::npos,
         "panel should render all nplinux hosts");
}

void test_make_console_script() {
  std::string script =
      make_console_script(2, "% <tag> 'quoted' \\ slash\n", true);
  expect(script.find("document.getElementById('s2')") != std::string::npos,
         "script should target the right session");
  expect(script.find("<b>% &lt;tag&gt; &#39;quoted&#39; \\\\ slash&NewLine;</b>") !=
             std::string::npos,
         "script should escape HTML and JS-sensitive characters");
}

}  // namespace

int main() {
  try {
    test_parse_http_request();
    test_parse_remote_sessions();
    test_render_panel_page();
    test_make_console_script();
    std::cout << "test_components: PASS\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "test_components: FAIL: " << e.what() << "\n";
    return 1;
  }
}
