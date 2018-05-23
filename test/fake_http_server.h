#ifndef FAKE_HTTP_SERVER_H_
#define FAKE_HTTP_SERVER_H_

#include <boost/network/protocol/http/server.hpp>

namespace google {
namespace testing {

// Starts a server in a separate thread, allowing it to choose an
// available port.
class FakeServer {
 public:
  FakeServer();
  ~FakeServer();

  // Returns the <host>:<port> string for this server.
  std::string HostPort();

  // Returns the URL for this server without a trailing slash:
  // http://<host>:<port>
  std::string GetUrl();

  // Sets the response for GET requests to a path.
  void SetResponse(const std::string& path, const std::string& response);

  // Sets the response for POST requests to a path.  Also initializes
  // state for collecting posted data.
  void SetPostResponse(const std::string& path, const std::string& response);

  // Blocks until the number of POST requests to path reaches count.
  void WaitPosts(const std::string& path, int count);

  // Represents the headers and body sent in a single POST request.
  struct Post {
    std::map<std::string, std::string> headers;
    std::string body;
  };

  // Returns all POST data sent on requests to path.
  const std::vector<Post>& GetPosts(const std::string& path);

 private:
  struct Handler;
  typedef boost::network::http::server<Handler> Server;

  // Handler that maps paths to response strings.
  struct Handler {
    struct PostState {
      std::string response;
      std::mutex mutex;
      std::condition_variable cv;
      std::vector<Post> posts;
    };

    void operator()(Server::request const &request,
                    Server::connection_ptr connection);
    std::map<std::string, std::string> path_responses;
    std::map<std::string, PostState> path_posts;
  };

  Handler handler_;
  Server server_;
  std::thread server_thread_;
};

}  // testing
}  // google

#endif  // FAKE_HTTP_SERVER_H_
