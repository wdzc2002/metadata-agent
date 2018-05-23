#include "../src/reporter.h"

#include "../src/configuration.h"
#include "fake_clock.h"
#include "fake_http_server.h"
#include "gtest/gtest.h"
#include "gmock/gmock.h"

namespace google {

TEST(ReporterTest, MetadataReporter) {
  // Set up a fake server representing the Resource Metadata API.  It
  // will collect POST data from the MetadataReporter.
  testing::FakeServer server;
  constexpr const char kUpdatePath[] =
    "/v1beta2/projects/TestProjectId/resourceMetadata:batchUpdate";
  server.SetPostResponse(kUpdatePath, "ignored");

  // Configure the the MetadataReporter to point to the fake server
  // and start it with a 1 second polling interval.  We control time
  // using a fake clock.
  Configuration config(std::istringstream(
      "ProjectId: TestProjectId\n"
      "MetadataIngestionEndpointFormat: " + server.GetUrl() +
      "/v1beta2/projects/{{project_id}}/resourceMetadata:batchUpdate\n"
  ));
  MetadataStore store(config);
  MonitoredResource resource("type", {});
  MetadataStore::Metadata m(
      "default-version",
      false,
      time::rfc3339::FromString("2018-03-03T01:23:45.678901234Z"),
      time::rfc3339::FromString("2018-03-03T01:32:45.678901234Z"),
      json::object({{"f", json::string("hello")}}));
  store.UpdateMetadata(resource, std::move(m));
  double period_s = 1.0;
  MetadataReporter<testing::FakeClock> reporter(config, &store, period_s);

  // The headers & body we expect to see in the POST requests.
  std::map<std::string, std::string> expected_headers({
    {"Accept", "*/*"},
    {"Accept-Encoding", "identity;q=1.0, *;q=0"},
    {"Connection", "Close"},
    {"Content-Length", "229"},
    {"Content-Type", "application/json"},
    {"Host", server.HostPort()},
    {"User-Agent", "metadata-agent/0.0.19-1"},
  });
  std::string expected_body = json::object({
    {"entries", json::array({
      json::object({  // MonitoredResourceMetadata
        {"resource", json::object({
            {"type", json::string("type")},
            {"labels", json::object({})},
        })},
        {"rawContentVersion", json::string("default-version")},
        {"rawContent", json::object({{"f", json::string("hello")}})},
        {"state", json::string("ACTIVE")},
        {"createTime", json::string("2018-03-03T01:23:45.678901234Z")},
        {"collectTime", json::string("2018-03-03T01:32:45.678901234Z")},
      })
    })}
  })->ToString();

  // Wait for 1st post to server, then verify contents.
  server.WaitPosts(kUpdatePath, 1);
  EXPECT_EQ(1, server.GetPosts(kUpdatePath).size());
  for (const auto& post : server.GetPosts(kUpdatePath)) {
    EXPECT_EQ(expected_headers, post.headers);
    EXPECT_EQ(expected_body, post.body);
  }

  // Advance fake clock, wait for 2nd post, verify contents.
  testing::FakeClock::Advance(std::chrono::seconds(1));
  server.WaitPosts(kUpdatePath, 2);
  EXPECT_EQ(2, server.GetPosts(kUpdatePath).size());
  for (const auto& post : server.GetPosts(kUpdatePath)) {
    EXPECT_EQ(expected_headers, post.headers);
    EXPECT_EQ(expected_body, post.body);
  }

  // Advance fake clock, wait for 3rd post, verify contents.
  testing::FakeClock::Advance(std::chrono::seconds(1));
  server.WaitPosts(kUpdatePath, 3);
  EXPECT_EQ(3, server.GetPosts(kUpdatePath).size());
  for (const auto& post : server.GetPosts(kUpdatePath)) {
    EXPECT_EQ(expected_headers, post.headers);
    EXPECT_EQ(expected_body, post.body);
  }

  reporter.Stop();
}

}  // namespace google
