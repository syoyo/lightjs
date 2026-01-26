#include "http.h"
#include "tls.h"
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <iomanip>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  #define CLOSE_SOCKET closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #define INVALID_SOCKET -1
  #define SOCKET_ERROR -1
  #define CLOSE_SOCKET close
#endif

namespace lightjs {
namespace http {

URL URL::parse(const std::string& url) {
  URL result;

  size_t pos = 0;
  size_t protoEnd = url.find("://");
  if (protoEnd != std::string::npos) {
    result.protocol = url.substr(0, protoEnd);
    pos = protoEnd + 3;
  } else {
    result.protocol = "http";
  }

  size_t pathStart = url.find('/', pos);
  size_t queryStart = url.find('?', pos);

  std::string hostPort;
  if (pathStart != std::string::npos) {
    hostPort = url.substr(pos, pathStart - pos);
    size_t queryPos = (queryStart != std::string::npos && queryStart > pathStart)
                      ? queryStart : std::string::npos;
    if (queryPos != std::string::npos) {
      result.path = url.substr(pathStart, queryPos - pathStart);
      result.query = url.substr(queryPos + 1);
    } else {
      result.path = url.substr(pathStart);
    }
  } else {
    hostPort = url.substr(pos);
    result.path = "/";
  }

  size_t portPos = hostPort.find(':');
  if (portPos != std::string::npos) {
    result.host = hostPort.substr(0, portPos);
    result.port = static_cast<uint16_t>(std::stoi(hostPort.substr(portPos + 1)));
  } else {
    result.host = hostPort;
    result.port = (result.protocol == "https") ? 443 : 80;
  }

  return result;
}

std::string URL::toString() const {
  std::ostringstream oss;
  oss << protocol << "://" << host;
  if ((protocol == "http" && port != 80) || (protocol == "https" && port != 443)) {
    oss << ":" << port;
  }
  oss << path;
  if (!query.empty()) {
    oss << "?" << query;
  }
  return oss.str();
}

HTTPClient::HTTPClient() {
#ifdef _WIN32
  wsaInitialized_ = false;
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
    wsaInitialized_ = true;
  }
#endif
}

HTTPClient::~HTTPClient() {
#ifdef _WIN32
  if (wsaInitialized_) {
    WSACleanup();
  }
#endif
}

Response HTTPClient::get(const std::string& url) {
  return request("GET", url);
}

Response HTTPClient::request(const std::string& method, const std::string& urlStr,
                              const std::unordered_map<std::string, std::string>& headers,
                              const std::vector<uint8_t>& body) {
  URL url = URL::parse(urlStr);

  if (url.protocol == "file") {
    return fileRequest(url);
  } else if (url.protocol == "https") {
    return httpsRequest(url, method, headers, body);
  } else if (url.protocol == "http") {
    return httpRequest(url, method, headers, body);
  }

  Response resp;
  resp.statusCode = 400;
  resp.statusText = "Unsupported protocol";
  return resp;
}

Response HTTPClient::fileRequest(const URL& url) {
  Response resp;

  std::string path = url.host + url.path;
  std::ifstream file(path, std::ios::binary);

  if (!file) {
    resp.statusCode = 404;
    resp.statusText = "Not Found";
    return resp;
  }

  file.seekg(0, std::ios::end);
  size_t size = file.tellg();
  file.seekg(0, std::ios::beg);

  resp.body.resize(size);
  file.read(reinterpret_cast<char*>(resp.body.data()), size);

  resp.statusCode = 200;
  resp.statusText = "OK";
  resp.headers["Content-Length"] = std::to_string(size);

  return resp;
}

int HTTPClient::connectSocket(const std::string& host, uint16_t port) {
  struct addrinfo hints = {0}, *result = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  std::string portStr = std::to_string(port);
  if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) {
    return INVALID_SOCKET;
  }

  int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (sock == INVALID_SOCKET) {
    freeaddrinfo(result);
    return INVALID_SOCKET;
  }

  if (connect(sock, result->ai_addr, static_cast<int>(result->ai_addrlen)) == SOCKET_ERROR) {
    CLOSE_SOCKET(sock);
    freeaddrinfo(result);
    return INVALID_SOCKET;
  }

  freeaddrinfo(result);
  return sock;
}

void HTTPClient::closeSocket(int sock) {
  if (sock != INVALID_SOCKET) {
    CLOSE_SOCKET(sock);
  }
}

bool HTTPClient::sendData(int sock, const std::vector<uint8_t>& data) {
  size_t totalSent = 0;
  while (totalSent < data.size()) {
    int sent = send(sock, reinterpret_cast<const char*>(data.data() + totalSent),
                    static_cast<int>(data.size() - totalSent), 0);
    if (sent == SOCKET_ERROR) {
      return false;
    }
    totalSent += sent;
  }
  return true;
}

std::vector<uint8_t> HTTPClient::receiveData(int sock) {
  std::vector<uint8_t> result;
  char buffer[4096];

  while (true) {
    int received = recv(sock, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      break;
    }
    result.insert(result.end(), buffer, buffer + received);

    if (received < sizeof(buffer)) {
      break;
    }
  }

  return result;
}

Response HTTPClient::httpsRequest(const URL& url, const std::string& method,
                                   const std::unordered_map<std::string, std::string>& headers,
                                   const std::vector<uint8_t>& body) {
  Response resp;

  int sock = connectSocket(url.host, url.port);
  if (sock == INVALID_SOCKET) {
    resp.statusCode = 0;
    resp.statusText = "Connection failed";
    return resp;
  }

  // Create TLS connection with socket callbacks
  auto tlsConn = std::make_unique<tls::TLSConnection>(
    [sock](const uint8_t* data, size_t len) -> bool {
      size_t totalSent = 0;
      while (totalSent < len) {
        int sent = send(sock, reinterpret_cast<const char*>(data + totalSent),
                        static_cast<int>(len - totalSent), 0);
        if (sent <= 0) return false;
        totalSent += sent;
      }
      return true;
    },
    [sock](uint8_t* data, size_t maxLen) -> int {
      return recv(sock, reinterpret_cast<char*>(data), static_cast<int>(maxLen), 0);
    }
  );

  // Perform TLS handshake
  if (!tlsConn->handshake(url.host)) {
    closeSocket(sock);
    resp.statusCode = 0;
    resp.statusText = "TLS handshake failed: " + tlsConn->getLastError();
    return resp;
  }

  // Build HTTP request
  std::ostringstream request;
  request << method << " " << url.path;
  if (!url.query.empty()) {
    request << "?" << url.query;
  }
  request << " HTTP/1.1\r\n";
  request << "Host: " << url.host << "\r\n";
  request << "Connection: close\r\n";
  request << "User-Agent: LightJS/1.0\r\n";

  for (const auto& [key, value] : headers) {
    request << key << ": " << value << "\r\n";
  }

  if (!body.empty()) {
    request << "Content-Length: " << body.size() << "\r\n";
  }

  request << "\r\n";

  std::string requestStr = request.str();
  std::vector<uint8_t> requestData(requestStr.begin(), requestStr.end());
  requestData.insert(requestData.end(), body.begin(), body.end());

  // Send request over TLS
  if (!tlsConn->send(requestData.data(), requestData.size())) {
    tlsConn->close();
    closeSocket(sock);
    resp.statusCode = 0;
    resp.statusText = "TLS send failed";
    return resp;
  }

  // Receive response over TLS
  std::vector<uint8_t> responseData;
  uint8_t buffer[4096];
  int received;

  while ((received = tlsConn->recv(buffer, sizeof(buffer))) > 0) {
    responseData.insert(responseData.end(), buffer, buffer + received);
  }

  tlsConn->close();
  closeSocket(sock);

  if (responseData.empty()) {
    resp.statusCode = 0;
    resp.statusText = "No response";
    return resp;
  }

  return parseResponse(responseData);
}

Response HTTPClient::httpRequest(const URL& url, const std::string& method,
                                  const std::unordered_map<std::string, std::string>& headers,
                                  const std::vector<uint8_t>& body) {
  Response resp;

  int sock = connectSocket(url.host, url.port);
  if (sock == INVALID_SOCKET) {
    resp.statusCode = 0;
    resp.statusText = "Connection failed";
    return resp;
  }

  std::ostringstream request;
  request << method << " " << url.path;
  if (!url.query.empty()) {
    request << "?" << url.query;
  }
  request << " HTTP/1.1\r\n";
  request << "Host: " << url.host << "\r\n";
  request << "Connection: close\r\n";
  request << "User-Agent: LightJS/1.0\r\n";

  for (const auto& [key, value] : headers) {
    request << key << ": " << value << "\r\n";
  }

  if (!body.empty()) {
    request << "Content-Length: " << body.size() << "\r\n";
  }

  request << "\r\n";

  std::string requestStr = request.str();
  std::vector<uint8_t> requestData(requestStr.begin(), requestStr.end());
  requestData.insert(requestData.end(), body.begin(), body.end());

  if (!sendData(sock, requestData)) {
    closeSocket(sock);
    resp.statusCode = 0;
    resp.statusText = "Send failed";
    return resp;
  }

  std::vector<uint8_t> responseData = receiveData(sock);
  closeSocket(sock);

  if (responseData.empty()) {
    resp.statusCode = 0;
    resp.statusText = "No response";
    return resp;
  }

  return parseResponse(responseData);
}

Response HTTPClient::parseResponse(const std::vector<uint8_t>& data) {
  Response resp;

  std::string dataStr(data.begin(), data.end());
  size_t headerEnd = dataStr.find("\r\n\r\n");

  if (headerEnd == std::string::npos) {
    resp.statusCode = 0;
    resp.statusText = "Invalid response";
    return resp;
  }

  std::string headerSection = dataStr.substr(0, headerEnd);
  std::vector<uint8_t> bodyData(data.begin() + headerEnd + 4, data.end());

  auto lines = split(headerSection, '\n');
  if (lines.empty()) {
    resp.statusCode = 0;
    resp.statusText = "Invalid response";
    return resp;
  }

  std::string statusLine = trim(lines[0]);
  auto statusParts = split(statusLine, ' ');
  if (statusParts.size() >= 2) {
    resp.statusCode = std::stoi(statusParts[1]);
  }
  if (statusParts.size() >= 3) {
    resp.statusText = statusParts[2];
    for (size_t i = 3; i < statusParts.size(); i++) {
      resp.statusText += " " + statusParts[i];
    }
  }

  for (size_t i = 1; i < lines.size(); i++) {
    std::string line = trim(lines[i]);
    size_t colonPos = line.find(':');
    if (colonPos != std::string::npos) {
      std::string key = trim(line.substr(0, colonPos));
      std::string value = trim(line.substr(colonPos + 1));
      resp.headers[key] = value;
    }
  }

  resp.body = bodyData;
  return resp;
}

std::string urlEncode(const std::string& str) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase;

  for (unsigned char c : str) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      oss << c;
    } else {
      oss << '%' << std::setw(2) << static_cast<int>(c);
    }
  }

  return oss.str();
}

std::string urlDecode(const std::string& str) {
  std::ostringstream oss;

  for (size_t i = 0; i < str.length(); i++) {
    if (str[i] == '%' && i + 2 < str.length()) {
      std::string hex = str.substr(i + 1, 2);
      int value = std::stoi(hex, nullptr, 16);
      oss << static_cast<char>(value);
      i += 2;
    } else if (str[i] == '+') {
      oss << ' ';
    } else {
      oss << str[i];
    }
  }

  return oss.str();
}

std::string toLower(const std::string& str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

std::vector<std::string> split(const std::string& str, char delim) {
  std::vector<std::string> result;
  std::stringstream ss(str);
  std::string item;

  while (std::getline(ss, item, delim)) {
    result.push_back(item);
  }

  return result;
}

std::string trim(const std::string& str) {
  size_t start = 0;
  while (start < str.length() && std::isspace(str[start])) {
    start++;
  }

  size_t end = str.length();
  while (end > start && std::isspace(str[end - 1])) {
    end--;
  }

  return str.substr(start, end - start);
}

}
}