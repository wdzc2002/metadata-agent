#include "../src/configuration.h"
#include "../src/resource.h"
#include "../src/kubernetes.h"
#include "../src/updater.h"
#include "fake_http_server.h"
#include "gtest/gtest.h"

#include <boost/network/protocol/http/server.hpp>

namespace google {

class KubernetesTest : public ::testing::Test {
 protected:
  static MetadataUpdater::ResourceMetadata GetNodeMetadata(
      const KubernetesReader& reader, const json::Object *node,
      Timestamp collected_at, bool is_deleted)
      throw(json::Exception) {
    return reader.GetNodeMetadata(node, collected_at, is_deleted);
  }

  static MetadataUpdater::ResourceMetadata GetPodMetadata(
      const KubernetesReader& reader, const json::Object* pod,
      json::value associations, Timestamp collected_at, bool is_deleted)
      throw(json::Exception) {
    return reader.GetPodMetadata(
        pod, std::move(associations), collected_at, is_deleted);
  }

  static MetadataUpdater::ResourceMetadata GetContainerMetadata(
      const KubernetesReader& reader, const json::Object* pod,
      const json::Object* container_spec, const json::Object* container_status,
      json::value associations, Timestamp collected_at, bool is_deleted)
      throw(json::Exception) {
    return reader.GetContainerMetadata(pod, container_spec, container_status,
                                       std::move(associations), collected_at,
                                       is_deleted);
  }

  static std::vector<MetadataUpdater::ResourceMetadata> GetPodAndContainerMetadata(
      const KubernetesReader& reader, const json::Object* pod,
      Timestamp collected_at, bool is_deleted) throw(json::Exception) {
    return reader.GetPodAndContainerMetadata(pod, collected_at, is_deleted);
  }

  static MetadataUpdater::ResourceMetadata GetLegacyResource(
      const KubernetesReader& reader, const json::Object* pod,
      const std::string& container_name) throw(json::Exception) {
    return reader.GetLegacyResource(pod, container_name);
  }

  static json::value ComputePodAssociations(const KubernetesReader& reader,
                                     const json::Object* pod) {
    return reader.ComputePodAssociations(pod);
  }

  static void UpdateOwnersCache(KubernetesReader* reader, const std::string& key,
                         const json::value& value) {
    reader->owners_[key] = value->Clone();
  }

  static MetadataUpdater::ResourceMetadata GetClusterMetadata(
      const KubernetesReader& reader, Timestamp collected_at)
      throw(json::Exception) {
    return reader.GetClusterMetadata(collected_at);
  }

  static void UpdateServiceToMetadataCache(
      KubernetesReader* reader, const json::Object* service, bool is_deleted)
      throw(json::Exception) {
    return reader->UpdateServiceToMetadataCache(service, is_deleted);
  }

  static void UpdateServiceToPodsCache(
      KubernetesReader* reader, const json::Object* endpoints, bool is_deleted)
      throw(json::Exception) {
    return reader->UpdateServiceToPodsCache(endpoints, is_deleted);
  }
};

TEST_F(KubernetesTest, GetNodeMetadata) {
  Configuration config(std::istringstream(
    "KubernetesClusterName: TestClusterName\n"
    "KubernetesClusterLocation: TestClusterLocation\n"
    "MetadataIngestionRawContentVersion: TestVersion\n"
    "InstanceResourceType: gce_instance\n"
    "InstanceZone: TestZone\n"
    "InstanceId: TestID\n"
  ));
  Environment environment(config);
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.
  json::value node = json::object({
    {"metadata", json::object({
      {"name", json::string("testname")},
      {"creationTimestamp", json::string("2018-03-03T01:23:45.678901234Z")},
    })}
  });
  const auto m =
      GetNodeMetadata(reader, node->As<json::Object>(), Timestamp(), false);
  EXPECT_EQ(1, m.ids().size());
  EXPECT_EQ("k8s_node.testname", m.ids()[0]);
  EXPECT_EQ(MonitoredResource("k8s_node", {
    {"cluster_name", "TestClusterName"},
    {"node_name", "testname"},
    {"location", "TestClusterLocation"},
  }), m.resource());
  EXPECT_EQ("TestVersion", m.metadata().version);
  EXPECT_FALSE(m.metadata().is_deleted);
  EXPECT_EQ(time::rfc3339::FromString("2018-03-03T01:23:45.678901234Z"),
            m.metadata().created_at);
  EXPECT_EQ(Timestamp(), m.metadata().collected_at);
  json::value big = json::object({
    {"blobs", json::object({
      {"association", json::object({
        {"version", json::string("TestVersion")},
        {"raw", json::object({
          {"infrastructureResource", json::object({
            {"type", json::string("gce_instance")},
            {"labels", json::object({
              {"instance_id", json::string("TestID")},
              {"zone", json::string("TestZone")},
            })},
          })},
        })},
      })},
      {"api", json::object({
        {"version", json::string("1.6")},  // Hard-coded in kubernetes.cc.
        {"raw", std::move(node)},
      })},
    })},
  });
  EXPECT_EQ(big->ToString(), m.metadata().metadata->ToString());
}

TEST_F(KubernetesTest, ComputePodAssociations) {
  Configuration config(std::stringstream(
    "KubernetesClusterName: TestClusterName\n"
    "KubernetesClusterLocation: TestClusterLocation\n"
    "MetadataIngestionRawContentVersion: TestVersion\n"
    "InstanceZone: TestZone\n"
    "InstanceId: TestID\n"
  ));
  Environment environment(config);
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.
  json::value controller = json::object({
    {"controller", json::boolean(true)},
    {"apiVersion", json::string("1.2.3")},
    {"kind", json::string("TestKind")},
    {"name", json::string("TestName")},
    {"uid", json::string("TestUID1")},
    {"metadata", json::object({
      {"name", json::string("InnerTestName")},
      {"kind", json::string("InnerTestKind")},
      {"uid", json::string("InnerTestUID1")},
    })},
  });
  UpdateOwnersCache(&reader, "1.2.3/TestKind/TestUID1", controller);
  json::value pod = json::object({
    {"metadata", json::object({
      {"namespace", json::string("TestNamespace")},
      {"uid", json::string("TestUID0")},
      {"ownerReferences", json::array({
        json::object({{"no_controller", json::boolean(true)}}),
        std::move(controller),
      })},
    })},
    {"spec", json::object({
      {"nodeName", json::string("TestSpecNodeName")},
    })},
  });

  json::value expected_associations = json::object({
    {"raw", json::object({
      {"controllers", json::object({
        {"topLevelControllerName", json::string("InnerTestName")},
        {"topLevelControllerType", json::string("TestKind")},
      })},
      {"infrastructureResource", json::object({
        {"labels", json::object({
          {"instance_id", json::string("TestID")},
          {"zone", json::string("TestZone")},
        })},
        {"type", json::string("gce_instance")},
      })},
      {"nodeName", json::string("TestSpecNodeName")},
    })},
    {"version", json::string("TestVersion")},
  });
  const auto associations =
      ComputePodAssociations(reader, pod->As<json::Object>());
  EXPECT_EQ(expected_associations->ToString(), associations->ToString());
}

TEST_F(KubernetesTest, GetPodMetadata) {
  Configuration config(std::stringstream(
    "KubernetesClusterName: TestClusterName\n"
    "KubernetesClusterLocation: TestClusterLocation\n"
    "MetadataApiResourceTypeSeparator: \".\"\n"
    "MetadataIngestionRawContentVersion: TestVersion\n"
  ));
  Environment environment(config);
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.

  json::value pod = json::object({
    {"metadata", json::object({
      {"namespace", json::string("TestNamespace")},
      {"name", json::string("TestName")},
      {"uid", json::string("TestUid")},
      {"creationTimestamp", json::string("2018-03-03T01:23:45.678901234Z")},
    })},
  });
  const auto m = GetPodMetadata(reader, pod->As<json::Object>(),
                                json::string("TestAssociations"), Timestamp(),
                                false);

  EXPECT_EQ(std::vector<std::string>(
      {"k8s_pod.TestUid", "k8s_pod.TestNamespace.TestName"}), m.ids());
  EXPECT_EQ(MonitoredResource("k8s_pod", {
    {"cluster_name", "TestClusterName"},
    {"pod_name", "TestName"},
    {"location", "TestClusterLocation"},
    {"namespace_name", "TestNamespace"},
  }), m.resource());
  EXPECT_EQ("TestVersion", m.metadata().version);
  EXPECT_FALSE(m.metadata().is_deleted);
  EXPECT_EQ(time::rfc3339::FromString("2018-03-03T01:23:45.678901234Z"),
            m.metadata().created_at);
  EXPECT_EQ(Timestamp(), m.metadata().collected_at);
  EXPECT_FALSE(m.metadata().ignore);
  json::value expected_metadata = json::object({
    {"blobs", json::object({
      {"api", json::object({
        {"raw", json::object({
          {"metadata", json::object({
            {"creationTimestamp",
              json::string("2018-03-03T01:23:45.678901234Z")},
            {"name", json::string("TestName")},
            {"namespace", json::string("TestNamespace")},
            {"uid", json::string("TestUid")},
          })},
        })},
        {"version", json::string("1.6")},
      })},
      {"association", json::string("TestAssociations")},
    })},
  });
  EXPECT_EQ(expected_metadata->ToString(), m.metadata().metadata->ToString());
}

TEST_F(KubernetesTest, GetLegacyResource) {
  Configuration config(std::stringstream(
    "KubernetesClusterName: TestClusterName\n"
    "MetadataApiResourceTypeSeparator: \".\"\n"
    "InstanceZone: TestZone\n"
    "InstanceId: TestID\n"
  ));
  Environment environment(config);
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.
  json::value pod = json::object({
    {"metadata", json::object({
      {"namespace", json::string("TestNamespace")},
      {"name", json::string("TestName")},
      {"uid", json::string("TestUid")},
    })},
  });
  const auto m = GetLegacyResource(reader, pod->As<json::Object>(),
                                   "TestContainerName");
  EXPECT_EQ(std::vector<std::string>({
    "gke_container.TestNamespace.TestUid.TestContainerName",
    "gke_container.TestNamespace.TestName.TestContainerName",
  }), m.ids());
  EXPECT_EQ(MonitoredResource("gke_container", {
    {"cluster_name", "TestClusterName"},
    {"container_name", "TestContainerName"},
    {"instance_id", "TestID"},
    {"namespace_id", "TestNamespace"},
    {"pod_id", "TestUid"},
    {"zone", "TestZone"},
  }), m.resource());
  EXPECT_TRUE(m.metadata().ignore);
}

TEST_F(KubernetesTest, GetClusterMetadataEmpty) {
  Configuration config(std::istringstream(
    "KubernetesClusterName: TestClusterName\n"
    "KubernetesClusterLocation: TestClusterLocation\n"
    "MetadataIngestionRawContentVersion: TestVersion\n"
  ));
  Environment environment(config);
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.
  const auto m = GetClusterMetadata(reader, Timestamp());
  EXPECT_TRUE(m.ids().empty());
  EXPECT_EQ(MonitoredResource("k8s_cluster", {
    {"cluster_name", "TestClusterName"},
    {"location", "TestClusterLocation"},
  }), m.resource());
  EXPECT_EQ("TestVersion", m.metadata().version);
  EXPECT_FALSE(m.metadata().is_deleted);
  EXPECT_EQ(Timestamp(), m.metadata().created_at);
  EXPECT_EQ(Timestamp(), m.metadata().collected_at);
  json::value empty_cluster = json::object({
    {"blobs", json::object({
      {"services", json::array({})},
    })},
  });
  EXPECT_EQ(empty_cluster->ToString(), m.metadata().metadata->ToString());
}

TEST_F(KubernetesTest, GetClusterMetadataEmptyService) {
  Configuration config(std::istringstream(
    "KubernetesClusterName: TestClusterName\n"
    "KubernetesClusterLocation: TestClusterLocation\n"
    "MetadataIngestionRawContentVersion: TestVersion\n"
  ));
  Environment environment(config);
  json::value service = json::object({
    {"metadata", json::object({
      {"name", json::string("testname")},
      {"namespace", json::string("testnamespace")},
    })},
  });
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.
  UpdateServiceToMetadataCache(
      &reader, service->As<json::Object>(), /*is_deleted=*/false);
  const auto m = GetClusterMetadata(reader, Timestamp());
  EXPECT_TRUE(m.ids().empty());
  EXPECT_EQ(MonitoredResource("k8s_cluster", {
    {"cluster_name", "TestClusterName"},
    {"location", "TestClusterLocation"},
  }), m.resource());
  EXPECT_EQ("TestVersion", m.metadata().version);
  EXPECT_FALSE(m.metadata().is_deleted);
  EXPECT_EQ(Timestamp(), m.metadata().created_at);
  EXPECT_EQ(Timestamp(), m.metadata().collected_at);
  json::value expected_cluster = json::object({
    {"blobs", json::object({
      {"services", json::array({
        json::object({
          {"api", json::object({
            {"pods", json::array({})},
            {"raw", std::move(service)},
            {"version", json::string("1.6")},  // Hard-coded in kubernetes.cc.
          })},
        }),
      })},
    })},
  });
  EXPECT_EQ(expected_cluster->ToString(), m.metadata().metadata->ToString());
}

TEST_F(KubernetesTest, GetClusterMetadataServiceWithPods) {
  Configuration config(std::istringstream(
    "KubernetesClusterName: TestClusterName\n"
    "KubernetesClusterLocation: TestClusterLocation\n"
    "MetadataIngestionRawContentVersion: TestVersion\n"
  ));
  Environment environment(config);
  json::value service = json::object({
    {"metadata", json::object({
      {"name", json::string("testname")},
      {"namespace", json::string("testnamespace")},
    })},
  });
  json::value endpoints = json::object({
    {"metadata", json::object({
      {"name", json::string("testname")},
      {"namespace", json::string("testnamespace")},
    })},
    {"subsets", json::array({
      json::object({
        {"addresses", json::array({
          json::object({
            {"targetRef", json::object({
              {"kind", json::string("Pod")},
              {"name", json::string("my-pod")},
            })},
          }),
        })},
      }),
    })},
  });
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.
  UpdateServiceToMetadataCache(
      &reader, service->As<json::Object>(), /*is_deleted=*/false);
  UpdateServiceToPodsCache(
      &reader, endpoints->As<json::Object>(), /*is_deleted=*/false);
  const auto m = GetClusterMetadata(reader, Timestamp());
  EXPECT_TRUE(m.ids().empty());
  EXPECT_EQ(MonitoredResource("k8s_cluster", {
    {"cluster_name", "TestClusterName"},
    {"location", "TestClusterLocation"},
  }), m.resource());
  EXPECT_EQ("TestVersion", m.metadata().version);
  EXPECT_FALSE(m.metadata().is_deleted);
  EXPECT_EQ(Timestamp(), m.metadata().created_at);
  EXPECT_EQ(Timestamp(), m.metadata().collected_at);
  MonitoredResource pod_mr = MonitoredResource("k8s_pod", {
    {"cluster_name", "TestClusterName"},
    {"namespace_name", "testnamespace"},
    {"pod_name", "my-pod"},
    {"location", "TestClusterLocation"},
  });
  json::value expected_cluster = json::object({
    {"blobs", json::object({
      {"services", json::array({
        json::object({
          {"api", json::object({
            {"pods", json::array({
              pod_mr.ToJSON(),
            })},
            {"raw", std::move(service)},
            {"version", json::string("1.6")},  // Hard-coded in kubernetes.cc.
          })},
        }),
      })},
    })},
  });
  EXPECT_EQ(expected_cluster->ToString(), m.metadata().metadata->ToString());
}

TEST_F(KubernetesTest, GetClusterMetadataDeletedService) {
  Configuration config(std::istringstream(
    "KubernetesClusterName: TestClusterName\n"
    "KubernetesClusterLocation: TestClusterLocation\n"
    "MetadataIngestionRawContentVersion: TestVersion\n"
  ));
  Environment environment(config);
  json::value service = json::object({
    {"metadata", json::object({
      {"name", json::string("testname")},
      {"namespace", json::string("testnamespace")},
    })},
  });
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.
  UpdateServiceToMetadataCache(
      &reader, service->As<json::Object>(), /*is_deleted=*/false);
  UpdateServiceToMetadataCache(
      &reader, service->As<json::Object>(), /*is_deleted=*/true);
  const auto m = GetClusterMetadata(reader, Timestamp());
  EXPECT_TRUE(m.ids().empty());
  json::value empty_cluster = json::object({
    {"blobs", json::object({
      {"services", json::array({})},
    })},
  });
  EXPECT_EQ(empty_cluster->ToString(), m.metadata().metadata->ToString());
}

TEST_F(KubernetesTest, GetContainerMetadata) {
  Configuration config(std::stringstream(
    "KubernetesClusterName: TestClusterName\n"
    "KubernetesClusterLocation: TestClusterLocation\n"
    "MetadataApiResourceTypeSeparator: \".\"\n"
  ));
  Environment environment(config);
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.
  json::value pod = json::object({
    {"metadata", json::object({
      {"namespace", json::string("TestNamespace")},
      {"name", json::string("TestName")},
      {"uid", json::string("TestUid")},
      {"creationTimestamp", json::string("2018-03-03T01:23:45.678901234Z")},
      {"labels", json::object({{"label", json::string("TestLabel")}})},
    })},
  });
  json::value spec = json::object({{"name", json::string("TestSpecName")}});
  json::value status = json::object({
    {"containerID", json::string("docker://TestContainerID")},
  });
  const auto m = GetContainerMetadata(
      reader,
      pod->As<json::Object>(),
      spec->As<json::Object>(),
      status->As<json::Object>(),
      json::string("TestAssociations"),
      Timestamp(),
      /*is_deleted=*/false);

  EXPECT_EQ(std::vector<std::string>({
    "k8s_container.TestUid.TestSpecName",
    "k8s_container.TestNamespace.TestName.TestSpecName",
    "k8s_container.TestContainerID",
  }), m.ids());
  EXPECT_EQ(MonitoredResource("k8s_container", {
    {"cluster_name", "TestClusterName"},
    {"container_name", "TestSpecName"},
    {"location", "TestClusterLocation"},
    {"namespace_name", "TestNamespace"},
    {"pod_name", "TestName"},
  }), m.resource());
  EXPECT_EQ("1.6", m.metadata().version);
  EXPECT_FALSE(m.metadata().is_deleted);
  EXPECT_EQ(time::rfc3339::FromString("2018-03-03T01:23:45.678901234Z"),
            m.metadata().created_at);
  EXPECT_EQ(Timestamp(), m.metadata().collected_at);
  EXPECT_FALSE(m.metadata().ignore);
  json::value expected_metadata = json::object({
    {"blobs", json::object({
      {"association", json::string("TestAssociations")},
      {"labels", json::object({
        {"raw", json::object({
          {"label", json::string("TestLabel")},
        })},
        {"version", json::string("1.6")},
      })},
      {"spec", json::object({
        {"raw", json::object({
          {"name", json::string("TestSpecName")},
        })},
        {"version", json::string("1.6")},
      })},
      {"status", json::object({
        {"raw", json::object({
          {"containerID", json::string("docker://TestContainerID")},
        })},
        {"version", json::string("1.6")},
      })},
    })},
  });
  EXPECT_EQ(expected_metadata->ToString(), m.metadata().metadata->ToString());
}
TEST_F(KubernetesTest, GetPodAndContainerMetadata) {
  Configuration config(std::stringstream(
    "KubernetesClusterName: TestClusterName\n"
    "KubernetesClusterLocation: TestClusterLocation\n"
    "MetadataApiResourceTypeSeparator: \".\"\n"
    "MetadataIngestionRawContentVersion: TestVersion\n"
    "InstanceZone: TestZone\n"
    "InstanceId: TestID\n"
  ));
  Environment environment(config);
  KubernetesReader reader(config, nullptr);  // Don't need HealthChecker.

  json::value controller = json::object({
    {"controller", json::boolean(true)},
    {"apiVersion", json::string("1.2.3")},
    {"kind", json::string("TestKind")},
    {"name", json::string("TestName")},
    {"uid", json::string("TestUID1")},
    {"metadata", json::object({
      {"name", json::string("InnerTestName")},
      {"kind", json::string("InnerTestKind")},
      {"uid", json::string("InnerTestUID1")},
    })},
  });
  UpdateOwnersCache(&reader, "1.2.3/TestKind/TestUID1", controller);
  json::value pod = json::object({
    {"metadata", json::object({
      {"name", json::string("TestPodName")},
      {"namespace", json::string("TestNamespace")},
      {"uid", json::string("TestPodUid")},
      {"creationTimestamp", json::string("2018-03-03T01:23:45.678901234Z")},
    })},
    {"spec", json::object({
      {"nodeName", json::string("TestSpecNodeName")},
      {"containers", json::array({
        json::object({{"name", json::string("TestContainerName0")}}),
      })},
    })},
    {"status", json::object({
      {"containerID", json::string("docker://TestContainerID")},
      {"containerStatuses", json::array({
        json::object({
          {"name", json::string("TestContainerName0")},
        }),
      })},
    })},
  });

  auto m = GetPodAndContainerMetadata(
      reader, pod->As<json::Object>(), Timestamp(), false);
  EXPECT_EQ(3, m.size());
  EXPECT_EQ(std::vector<std::string>({
    "gke_container.TestNamespace.TestPodUid.TestContainerName0",
    "gke_container.TestNamespace.TestPodName.TestContainerName0",
  }), m[0].ids());
  EXPECT_EQ(MonitoredResource("gke_container", {
    {"cluster_name", "TestClusterName"},
    {"container_name", "TestContainerName0"},
    {"instance_id", "TestID"},
    {"namespace_id", "TestNamespace"},
    {"pod_id", "TestPodUid"},
    {"zone", "TestZone"}
  }), m[0].resource());
  EXPECT_TRUE(m[0].metadata().ignore);

  EXPECT_EQ(std::vector<std::string>({
    "k8s_container.TestPodUid.TestContainerName0",
    "k8s_container.TestNamespace.TestPodName.TestContainerName0"
  }), m[1].ids());
  EXPECT_EQ(MonitoredResource("k8s_container", {
    {"cluster_name", "TestClusterName"},
    {"container_name", "TestContainerName0"},
    {"location", "TestClusterLocation"},
    {"namespace_name", "TestNamespace"},
    {"pod_name", "TestPodName"},
  }), m[1].resource());
  EXPECT_FALSE(m[1].metadata().ignore);
  EXPECT_EQ("1.6", m[1].metadata().version);
  EXPECT_FALSE(m[1].metadata().is_deleted);
  EXPECT_EQ(time::rfc3339::FromString("2018-03-03T01:23:45.678901234Z"),
            m[1].metadata().created_at);
  EXPECT_EQ(Timestamp(), m[1].metadata().collected_at);
  json::value container_metadata = json::object({
    {"blobs", json::object({
      {"association", json::object({
        {"raw", json::object({
          {"controllers", json::object({
            {"topLevelControllerName", json::string("TestPodName")},
            {"topLevelControllerType", json::string("Pod")},
          })},
          {"infrastructureResource", json::object({
            {"labels", json::object({
              {"instance_id", json::string("TestID")},
              {"zone", json::string("TestZone")},
            })},
            {"type", json::string("gce_instance")},
          })},
          {"nodeName", json::string("TestSpecNodeName")},
        })},
        {"version", json::string("TestVersion")},
      })},
      {"spec", json::object({
        {"raw", json::object({
          {"name", json::string("TestContainerName0")},
        })},
        {"version", json::string("1.6")},
      })},
      {"status", json::object({
        {"raw", json::object({
          {"name", json::string("TestContainerName0")},
        })},
        {"version", json::string("1.6")},
      })},
    })},
  });
  EXPECT_EQ(container_metadata->ToString(), m[1].metadata().metadata->ToString());

  EXPECT_EQ(std::vector<std::string>({
    "k8s_pod.TestPodUid",
    "k8s_pod.TestNamespace.TestPodName"
  }), m[2].ids());
  EXPECT_EQ(MonitoredResource("k8s_pod", {
      {"cluster_name", "TestClusterName"},
      {"location", "TestClusterLocation"},
      {"namespace_name", "TestNamespace"},
      {"pod_name", "TestPodName"},
  }), m[2].resource());
  EXPECT_FALSE(m[2].metadata().ignore);
  EXPECT_EQ("TestVersion", m[2].metadata().version);
  EXPECT_FALSE(m[2].metadata().is_deleted);
  EXPECT_EQ(time::rfc3339::FromString("2018-03-03T01:23:45.678901234Z"),
            m[2].metadata().created_at);
  EXPECT_EQ(Timestamp(), m[2].metadata().collected_at);
  json::value pod_metadata = json::object({
    {"blobs", json::object({
      {"api", json::object({
        {"raw", json::object({
          {"metadata", json::object({
            {"creationTimestamp",
              json::string("2018-03-03T01:23:45.678901234Z")},
            {"name", json::string("TestPodName")},
            {"namespace", json::string("TestNamespace")},
            {"uid", json::string("TestPodUid")},
          })},
          {"spec", json::object({
            {"containers", json::array({
              json::object({{"name", json::string("TestContainerName0")}})
            })},
            {"nodeName", json::string("TestSpecNodeName")},
          })},
          {"status", json::object({
            {"containerID", json::string("docker://TestContainerID")},
            {"containerStatuses", json::array({
              json::object({{"name", json::string("TestContainerName0")}})
            })},
          })},
        })},
        {"version", json::string("1.6")},
      })},
      {"association", json::object({
        {"raw", json::object({
          {"controllers", json::object({
            {"topLevelControllerName", json::string("TestPodName")},
            {"topLevelControllerType", json::string("Pod")},
          })},
          {"infrastructureResource", json::object({
            {"labels", json::object({
              {"instance_id", json::string("TestID")},
              {"zone", json::string("TestZone")},
            })},
            {"type", json::string("gce_instance")},
          })},
          {"nodeName", json::string("TestSpecNodeName")},
        })},
        {"version", json::string("TestVersion")},
      })},
    })},
  });
  EXPECT_EQ(pod_metadata->ToString(),
            m[2].metadata().metadata->ToString());
}

// Polls store until collected_at for resource is newer than
// last_timestamp.  Returns false if newer timestamp not found after 3
// seconds (polling every 100 millis).
bool WaitForNewerCollectionTimestamp(const MetadataStore& store,
                                     const MonitoredResource& resource,
                                     Timestamp last_timestamp) {
  for (int i = 0; i < 30; i++){
    const auto metadata_map = store.GetMetadataMap();
    const auto m = metadata_map.find(resource);
    if (m != metadata_map.end() && m->second.collected_at > last_timestamp) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

TEST_F(KubernetesTest, KubernetesUpdater) {
  // Create a fake server representing the Kubernetes master.
  testing::FakeServer server;
  server.SetResponse("/api/v1/nodes?limit=1", "{}");
  server.SetResponse("/api/v1/pods?limit=1", "{}");
  server.AllowStream(
      "/api/v1/pods?fieldSelector=spec.nodeName%3DTestNodeName&watch=true");
  server.AllowStream(
      "/api/v1/watch/nodes/TestNodeName?watch=true");

  // Start the updater with a config that points to the fake server.
  Configuration config(std::istringstream(
      "InstanceId: TestID\n"
      "InstanceResourceType: gce_instance\n"
      "InstanceZone: TestZone\n"
      "KubernetesClusterLocation: TestClusterLocation\n"
      "KubernetesClusterName: TestClusterName\n"
      "KubernetesEndpointHost: " + server.GetUrl() + "\n"
      "KubernetesNodeName: TestNodeName\n"
      "KubernetesUseWatch: true\n"
  ));
  MetadataStore store(config);
  KubernetesUpdater updater(config, /*health_checker=*/nullptr, &store);
  std::thread updater_thread([&updater] { updater.start(); });

  // Wait for updater's watchers to connect to the server (hanging GETs).
  server.WaitForOneStreamWatcher(
      "/api/v1/pods?fieldSelector=spec.nodeName%3DTestNodeName&watch=true");
  server.WaitForOneStreamWatcher(
      "/api/v1/watch/nodes/TestNodeName?watch=true");

  // For nodes, send stream responses from the fake Kubernetes
  // master and verify that the updater propagates them to the store.
  Timestamp last_nodes_timestamp = time_point();
  for (int i = 0; i < 3; i++) {
    json::value resp = json::object({
      {"type", json::string("ADDED")},
      {"object", json::object({
        {"metadata", json::object({
          {"name", json::string("TestNodeName")},
          {"creationTimestamp", json::string("2018-03-03T01:23:45.678901234Z")},
        })}
      })}
    });
    // Send a response to the watcher.
    server.SendStreamResponse(
        "/api/v1/watch/nodes/TestNodeName?watch=true",
        resp->ToString());
    // Wait until watcher has processed response (by polling the store).
    MonitoredResource resource("k8s_node",
                               {{"cluster_name", "TestClusterName"},
                                {"location", "TestClusterLocation"},
                                {"node_name", "TestNodeName"}});
    EXPECT_TRUE(
        WaitForNewerCollectionTimestamp(store, resource, last_nodes_timestamp));

    const auto metadata_map = store.GetMetadataMap();
    const auto& metadata = metadata_map.at(resource);
    // TODO: Insert tests of metadata values.
    last_nodes_timestamp = metadata.collected_at;
  }

  // For pods, do the same thing.
  Timestamp last_pods_timestamp = time_point();
  for (int i = 0; i < 3; i++) {
    json::value resp = json::object({
      {"type", json::string("ADDED")},
      {"object", json::object({
        {"metadata", json::object({
          {"name", json::string("TestPodName")},
          {"namespace", json::string("TestNamespace")},
          {"uid", json::string("TestPodUid")},
          {"creationTimestamp", json::string("2018-03-03T01:23:45.678901234Z")},
        })},
        {"spec", json::object({
          {"nodeName", json::string("TestSpecNodeName")},
          {"containers", json::array({
            json::object({{"name", json::string("TestContainerName0")}}),
          })},
        })},
        {"status", json::object({
          {"containerID", json::string("docker://TestContainerID")},
          {"containerStatuses", json::array({
            json::object({
              {"name", json::string("TestContainerName0")},
            }),
          })},
        })},
      })}
    });
    // Send a response to the watcher.
    server.SendStreamResponse(
        "/api/v1/pods?fieldSelector=spec.nodeName%3DTestNodeName&watch=true",
        resp->ToString());
    // Wait until watcher has processed response (by polling the store).
    MonitoredResource resource("k8s_pod",
                               {{"cluster_name", "TestClusterName"},
                                {"location", "TestClusterLocation"},
                                {"namespace_name", "TestNamespace"},
                                {"pod_name", "TestPodName"}});
    EXPECT_TRUE(
        WaitForNewerCollectionTimestamp(store, resource, last_nodes_timestamp));

    const auto metadata_map = store.GetMetadataMap();
    const auto& metadata = metadata_map.at(resource);
    // TODO: Insert tests of metadata values.
    last_pods_timestamp = metadata.collected_at;
  }

  // Terminate the hanging GETs on the server so that the updater will finish.
  server.TerminateAllStreams();
  updater.stop();
  updater_thread.join();
}

}  // namespace google
