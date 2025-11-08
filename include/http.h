#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>

namespace lightjs {
namespace http {

struct URL {
  std::string protocol;
  std::string host;
  uint16_t port;
  std::string path;
  std::string query;

  URL() : port(0) {}

  static URL parse(const std::string& url);
  std::string toString() const;
};

struct Response {
  int statusCode;
  std::string statusText;
  std::unordered_map<std::string, std::string> headers;
  std::vector<uint8_t> body;

  Response() : statusCode(0) {}

  std::string bodyAsString() const {
    return std::string(body.begin(), body.end());
  }
};

class HTTPClient {
public:
  HTTPClient();
  ~HTTPClient();

  Response get(const std::string& url);
  Response request(const std::string& method, const std::string& url,
                   const std::unordered_map<std::string, std::string>& headers = {},
                   const std::vector<uint8_t>& body = {});

private:
  Response httpRequest(const URL& url, const std::string& method,
                       const std::unordered_map<std::string, std::string>& headers,
                       const std::vector<uint8_t>& body);
  Response fileRequest(const URL& url);

  int connectSocket(const std::string& host, uint16_t port);
  void closeSocket(int sock);
  bool sendData(int sock, const std::vector<uint8_t>& data);
  std::vector<uint8_t> receiveData(int sock);

  Response parseResponse(const std::vector<uint8_t>& data);

#ifdef _WIN32
  bool wsaInitialized_;
#endif
};

std::string urlEncode(const std::string& str);
std::string urlDecode(const std::string& str);
std::string toLower(const std::string& str);
std::vector<std::string> split(const std::string& str, char delim);
std::string trim(const std::string& str);

}
}