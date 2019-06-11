// Copyright 2018 Intel Corporation All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef RMW_DPS_CPP__CUSTOM_NODE_INFO_HPP_
#define RMW_DPS_CPP__CUSTOM_NODE_INFO_HPP_

#include <dps/dps.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rmw/rmw.h"

#include "rmw_dps_cpp/names_common.hpp"
#include "rmw_dps_cpp/namespace_prefix.hpp"

class NodeListener;

typedef struct CustomNodeInfo
{
  DPS_Node * node_;
  std::string uuid_;
  rmw_guard_condition_t * graph_guard_condition_;
  size_t domain_id_;
  std::vector<std::string> advertisement_topics_;
  DPS_Publication * advertisement_;
  DPS_Subscription * discover_;
  NodeListener * listener_;
} CustomNodeInfo;

inline bool
operator<(const DPS_UUID & lhs, const DPS_UUID & rhs)
{
  return DPS_UUIDCompare(&lhs, &rhs) < 0;
}

class NodeListener
{
public:
  struct Topic
  {
    std::string topic;
    std::vector<std::string> types;
    bool operator==(const Topic & that) const
    {
      return this->topic == that.topic &&
             this->types == that.types;
    }
  };
  struct Node
  {
    Node()
    : namespace_("/") {}
    std::string name;
    std::string namespace_;
    std::vector<Topic> subscribers;
    std::vector<Topic> publishers;
    std::vector<Topic> services;
    bool operator==(const Node & that) const
    {
      return this->name == that.name &&
             this->namespace_ == that.namespace_ &&
             this->subscribers == that.subscribers &&
             this->publishers == that.publishers &&
             this->services == that.services;
    }
  };

  explicit NodeListener(const rmw_node_t * node)
  : node_(node)
  {}

  static void
  onPublication(DPS_Subscription * sub, const DPS_Publication * pub, uint8_t * payload, size_t len)
  {
    RCUTILS_LOG_DEBUG_NAMED(
      "rmw_dps_cpp",
      "%s(sub=%p,pub=%p,payload=%p,len=%zu)", __FUNCTION__, (void *)sub, (void *)pub, payload, len);

    NodeListener * listener = reinterpret_cast<NodeListener *>(DPS_GetSubscriptionData(sub));
    std::lock_guard<std::mutex> lock(listener->mutex_);
    std::string uuid;
    Node node;
    for (size_t i = 0; i < DPS_PublicationGetNumTopics(pub); ++i) {
      std::string topic = DPS_PublicationGetTopic(pub, i);
      size_t pos;
      pos = topic.find(dps_uuid_prefix);
      if (pos != std::string::npos) {
        uuid = topic.substr(pos + strlen(dps_uuid_prefix));
        continue;
      }
      pos = topic.find(dps_namespace_prefix);
      if (pos != std::string::npos) {
        // See _advertise() for explanation of "/" prefix below
        node.namespace_ = std::string("/") + topic.substr(pos + strlen(dps_namespace_prefix));
        continue;
      }
      pos = topic.find(dps_name_prefix);
      if (pos != std::string::npos) {
        node.name = topic.substr(pos + strlen(dps_name_prefix));
        continue;
      }
      Topic subscriber;
      if (process_topic_info(topic, dps_subscriber_prefix, subscriber)) {
        node.subscribers.push_back(subscriber);
        continue;
      }
      Topic publisher;
      if (process_topic_info(topic, dps_publisher_prefix, publisher)) {
        node.publishers.push_back(publisher);
        continue;
      }
      Topic service;
      if (process_topic_info(topic, dps_service_prefix, service)) {
        node.services.push_back(service);
        continue;
      }
    }
    if (uuid.empty()) {
      // ignore invalid advertisement
      return;
    }

    auto impl = static_cast<CustomNodeInfo *>(listener->node_->data);
    // trigger an advertisement for the new node
    if ((listener->discovered_nodes_.find(uuid) == listener->discovered_nodes_.end()) &&
      (DPS_UUIDCompare(DPS_PublicationGetUUID(impl->advertisement_),
      DPS_PublicationGetUUID(pub)) != 0))
    {
      _advertise(listener->node_);
    }
    Node old_node = listener->discovered_nodes_[uuid];
    listener->discovered_nodes_[uuid] = node;
    bool trigger = !(old_node == node);
    if (trigger) {
      rmw_ret_t ret = rmw_trigger_guard_condition(impl->graph_guard_condition_);
      if (ret != RMW_RET_OK) {
        RCUTILS_LOG_ERROR_NAMED(
          "rmw_dps_cpp",
          "failed to trigger guard condition");
      }
    }
  }

  std::vector<Node>
  get_discovered_nodes() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Node> nodes(discovered_nodes_.size());
    size_t i = 0;
    for (auto it : discovered_nodes_) {
      nodes[i++] = it.second;
    }
    return nodes;
  }

  size_t
  count_subscribers(const char * topic_name) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto it : discovered_nodes_) {
      count += std::count_if(it.second.subscribers.begin(), it.second.subscribers.end(),
          [topic_name](const Topic & subscriber) {return subscriber.topic == topic_name;});
    }
    return count;
  }

  size_t
  count_publishers(const char * topic_name) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto it : discovered_nodes_) {
      count += std::count_if(it.second.publishers.begin(), it.second.publishers.end(),
          [topic_name](const Topic & publisher) {return publisher.topic == topic_name;});
    }
    return count;
  }

  size_t
  count_services(const char * topic_name) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto it : discovered_nodes_) {
      count += std::count_if(it.second.services.begin(), it.second.services.end(),
          [topic_name](const Topic & service) {return service.topic == topic_name;});
    }
    return count;
  }

  std::map<std::string, std::set<std::string>>
  get_subscriber_names_and_types_by_node(const char * name, const char * namespace_)
  {
    std::map<std::string, std::set<std::string>> names_and_types;
    std::lock_guard<std::mutex> lock(mutex_);
    auto uuid_node_pair = std::find_if(discovered_nodes_.begin(), discovered_nodes_.end(),
        [name, namespace_](const std::pair<std::string, Node> & pair) {
          return pair.second.name == name && pair.second.namespace_ == namespace_;
        });
    if (uuid_node_pair != discovered_nodes_.end()) {
      for (auto it : uuid_node_pair->second.subscribers) {
        names_and_types[it.topic].insert(it.types.begin(), it.types.end());
      }
    }
    return names_and_types;
  }

  std::map<std::string, std::set<std::string>>
  get_publisher_names_and_types_by_node(const char * name, const char * namespace_)
  {
    std::map<std::string, std::set<std::string>> names_and_types;
    std::lock_guard<std::mutex> lock(mutex_);
    auto uuid_node_pair = std::find_if(discovered_nodes_.begin(), discovered_nodes_.end(),
        [name, namespace_](const std::pair<std::string, Node> & pair) {
          return pair.second.name == name && pair.second.namespace_ == namespace_;
        });
    if (uuid_node_pair != discovered_nodes_.end()) {
      for (auto it : uuid_node_pair->second.publishers) {
        names_and_types[it.topic].insert(it.types.begin(), it.types.end());
      }
    }
    return names_and_types;
  }

  std::map<std::string, std::set<std::string>>
  get_service_names_and_types_by_node(const char * name, const char * namespace_)
  {
    std::map<std::string, std::set<std::string>> names_and_types;
    std::lock_guard<std::mutex> lock(mutex_);
    auto uuid_node_pair = std::find_if(discovered_nodes_.begin(), discovered_nodes_.end(),
        [name, namespace_](const std::pair<std::string, Node> & pair) {
          return pair.second.name == name && pair.second.namespace_ == namespace_;
        });
    if (uuid_node_pair != discovered_nodes_.end()) {
      for (auto it : uuid_node_pair->second.services) {
        names_and_types[it.topic].insert(it.types.begin(), it.types.end());
      }
    }
    return names_and_types;
  }

  std::map<std::string, std::set<std::string>>
  get_topic_names_and_types()
  {
    std::map<std::string, std::set<std::string>> names_and_types;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto uuid_node_pair : discovered_nodes_) {
      for (auto it : uuid_node_pair.second.subscribers) {
        names_and_types[it.topic].insert(it.types.begin(), it.types.end());
      }
      for (auto it : uuid_node_pair.second.publishers) {
        names_and_types[it.topic].insert(it.types.begin(), it.types.end());
      }
      for (auto it : uuid_node_pair.second.services) {
        names_and_types[it.topic].insert(it.types.begin(), it.types.end());
      }
    }
    return names_and_types;
  }

private:
  static bool
  process_topic_info(const std::string & topic_str, const char * prefix, Topic & topic)
  {
    size_t pos = topic_str.find(prefix);
    if (pos != std::string::npos) {
      pos = pos + strlen(prefix);
      size_t end_pos = topic_str.find("&types=");
      // See _advertise() for explanation of "/" prefix below
      if (end_pos != std::string::npos) {
        topic.topic = std::string("/") + topic_str.substr(pos, end_pos - pos);
        pos = end_pos + strlen("&types=");
      } else {
        topic.topic = std::string("/") + topic_str.substr(pos);
      }
      while (pos != std::string::npos) {
        end_pos = topic_str.find(",", pos);
        if (end_pos != std::string::npos) {
          topic.types.push_back(topic_str.substr(pos, end_pos - pos));
          pos = end_pos + 1;
        } else {
          topic.types.push_back(topic_str.substr(pos));
          pos = end_pos;
        }
      }
      return true;
    }
    return false;
  }

  mutable std::mutex mutex_;
  std::map<std::string, Node> discovered_nodes_;
  const rmw_node_t * node_;
};

#endif  // RMW_DPS_CPP__CUSTOM_NODE_INFO_HPP_
