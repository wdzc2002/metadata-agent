#include "fake_http_server.h"

#include "../src/logging.h"

namespace google {
namespace testing {

FakeServer::FakeServer()
    // Note: An empty port selects a random available port (this behavior
    // is not documented).
    : server_(Server::options(handler_).address("127.0.0.1").port("")) {
  server_.listen();
  server_thread_ = std::thread([this] { server_.run(); });
}

FakeServer::~FakeServer() {
  server_.stop();
  server_thread_.join();
}

std::string FakeServer::HostPort() {
  return server_.address() + ":" + server_.port();
}

std::string FakeServer::GetUrl() {
  network::uri_builder builder;
  builder.scheme("http").host(server_.address()).port(server_.port());
  return builder.uri().string();
}

void FakeServer::SetResponse(const std::string& path,
                             const std::string& response) {
  handler_.path_responses[path] = response;
}

void FakeServer::SetPostResponse(const std::string& path,
                                 const std::string& response) {
  handler_.path_posts[path].response = response;
}

void FakeServer::WaitPosts(const std::string& path, int count) {
  const auto it = handler_.path_posts.find(path);
  if (it == handler_.path_posts.end()) {
    LOG(ERROR) << "No post response enabled for path " << path;
    return;
  }
  std::unique_lock<std::mutex> lk(it->second.mutex);
  it->second.cv.wait(lk, [&it, count]{return it->second.posts.size() >= count;});
  lk.unlock();
}

const std::vector<FakeServer::Post>& FakeServer::GetPosts(
    const std::string& path) {
  const auto it = handler_.path_posts.find(path);
  if (it == handler_.path_posts.end()) {
    LOG(ERROR) << "No post response enabled for path " << path;
    static std::vector<Post> empty;
    return empty;
  }
  return it->second.posts;
}

void FakeServer::Handler::operator()(Server::request const &request,
                                     Server::connection_ptr connection) {
  auto it = path_responses.find(request.destination);
  if (it != path_responses.end()) {
    connection->set_status(Server::connection::ok);
    connection->set_headers(std::map<std::string, std::string>({
        {"Content-Type", "text/plain"},
    }));
    connection->write(it->second);
    return;
  }

  auto p = path_posts.find(request.destination);
  if (p != path_posts.end()) {
    auto& state = p->second;
    connection->read([&state, &request](Server::connection::input_range range,
                                        boost::system::error_code error,
                                        size_t size,
                                        Server::connection_ptr conn) {
      Post post;
      for (const auto& h : request.headers) {
        post.headers[h.name] = h.value;
      }
      post.body.append(range.begin(), range.end());
      {
        std::lock_guard<std::mutex> lk(state.mutex);
        state.posts.push_back(post);
      }
      state.cv.notify_all();
    });
    connection->set_status(Server::connection::ok);
    connection->set_headers(std::map<std::string, std::string>({
        {"Content-Type", "text/plain"},
    }));
    connection->write(p->second.response);
    return;
  }

  // Note: We have to set headers; otherwise, an exception is thrown.
  connection->set_status(Server::connection::not_found);
  connection->set_headers(std::map<std::string, std::string>());
}

}  // testing
}  // google
