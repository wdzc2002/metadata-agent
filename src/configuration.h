/*
 * Copyright 2017 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/
#ifndef AGENT_CONFIG_H_
#define AGENT_CONFIG_H_

#include <memory>
#include <mutex>
#include <string>

int main(int, char**);

namespace google {

class Configuration {
 public:
  Configuration();
  Configuration(std::istream& input);
  // Used to accept inline construction of streams.
  Configuration(std::istream&& input) : Configuration(input) {}
  ~Configuration();

  // Shared configuration.
  NOOPT const std::string& ProjectId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return project_id_;
  }
  NOOPT const std::string& CredentialsFile() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return credentials_file_;
  }
  NOOPT bool VerboseLogging() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return verbose_logging_;
  }
  // Metadata API server configuration options.
  NOOPT int MetadataApiNumThreads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_api_num_threads_;
  }
  NOOPT int MetadataApiPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_api_port_;
  }
  NOOPT const std::string& MetadataApiResourceTypeSeparator() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_api_resource_type_separator_;
  }
  // Metadata reporter options.
  NOOPT int MetadataReporterIntervalSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_reporter_interval_seconds_;
  }
  NOOPT bool MetadataReporterPurgeDeleted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_reporter_purge_deleted_;
  }
  NOOPT const std::string& MetadataReporterUserAgent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_reporter_user_agent_;
  }
  NOOPT const std::string& MetadataIngestionEndpointFormat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_ingestion_endpoint_format_;
  }
  NOOPT int MetadataIngestionRequestSizeLimitBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_ingestion_request_size_limit_bytes_;
  }
  NOOPT int MetadataIngestionRequestSizeLimitCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_ingestion_request_size_limit_count_;
  }
  NOOPT const std::string& MetadataIngestionRawContentVersion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metadata_ingestion_raw_content_version_;
  }
  // Instance metadata updater options.
  NOOPT int InstanceUpdaterIntervalSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instance_updater_interval_seconds_;
  }
  NOOPT const std::string& InstanceResourceType() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instance_resource_type_;
  }
  // Docker metadata updater options.
  NOOPT int DockerUpdaterIntervalSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return docker_updater_interval_seconds_;
  }
  NOOPT const std::string& DockerEndpointHost() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return docker_endpoint_host_;
  }
  NOOPT const std::string& DockerApiVersion() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return docker_api_version_;
  }
  NOOPT const std::string& DockerContainerFilter() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return docker_container_filter_;
  }
  // GKE metadata updater options.
  NOOPT int KubernetesUpdaterIntervalSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kubernetes_updater_interval_seconds_;
  }
  NOOPT const std::string& KubernetesEndpointHost() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kubernetes_endpoint_host_;
  }
  NOOPT const std::string& KubernetesPodLabelSelector() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kubernetes_pod_label_selector_;
  }
  NOOPT const std::string& KubernetesClusterName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kubernetes_cluster_name_;
  }
  NOOPT const std::string& KubernetesClusterLocation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kubernetes_cluster_location_;
  }
  NOOPT const std::string& KubernetesNodeName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kubernetes_node_name_;
  }
  NOOPT bool KubernetesUseWatch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kubernetes_use_watch_;
  }
  NOOPT bool KubernetesClusterLevelMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kubernetes_cluster_level_metadata_;
  }
  bool KubernetesServiceMetadata() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kubernetes_service_metadata_;
  }
  // Common metadata updater options.
  NOOPT const std::string& InstanceId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instance_id_;
  }
  NOOPT const std::string& InstanceZone() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return instance_zone_;
  }

  NOOPT const std::string& HealthCheckFile() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return health_check_file_;
  }

 private:
  friend class ConfigurationArgumentParserTest;
  friend int ::main(int, char**);  // Calls ParseArguments.

  class OptionMap;  // Internal helper class.

  void ParseConfigFile(const std::string& filename);
  void ParseConfiguration(std::istream& input);
  // Parse the command line.
  // A zero return value means that parsing succeeded and the program should
  // proceed.  A positive return value means that parsing failed.  A negative
  // value means that parsing succeeded, but all of the arguments were handled
  // within the function and the program should exit with a success exit code.
  int ParseArguments(int ac, char** av);

  mutable std::mutex mutex_;
  bool verbose_logging_;
  std::string project_id_;
  std::string credentials_file_;
  int metadata_api_num_threads_;
  int metadata_api_port_;
  std::string metadata_api_resource_type_separator_;
  int metadata_reporter_interval_seconds_;
  bool metadata_reporter_purge_deleted_;
  std::string metadata_reporter_user_agent_;
  std::string metadata_ingestion_endpoint_format_;
  int metadata_ingestion_request_size_limit_bytes_;
  int metadata_ingestion_request_size_limit_count_;
  std::string metadata_ingestion_raw_content_version_;
  int instance_updater_interval_seconds_;
  std::string instance_resource_type_;
  int docker_updater_interval_seconds_;
  std::string docker_endpoint_host_;
  std::string docker_api_version_;
  std::string docker_container_filter_;
  int kubernetes_updater_interval_seconds_;
  std::string kubernetes_endpoint_host_;
  std::string kubernetes_pod_label_selector_;
  std::string kubernetes_cluster_name_;
  std::string kubernetes_cluster_location_;
  std::string kubernetes_node_name_;
  bool kubernetes_use_watch_;
  bool kubernetes_cluster_level_metadata_;
  bool kubernetes_service_metadata_;
  std::string instance_id_;
  std::string instance_zone_;
  std::string health_check_file_;

  std::unique_ptr<const OptionMap> options_;
};

}

#endif  // AGENT_CONFIG_H_
