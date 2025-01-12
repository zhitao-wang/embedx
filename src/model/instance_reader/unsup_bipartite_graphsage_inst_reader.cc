// Tencent is pleased to support the open source community by making embedx
// available.
//
// Copyright (C) 2021 THL A29 Limited, a Tencent company.  All rights reserved.
//
// Licensed under the BSD 3-Clause License and other third-party components,
// please refer to LICENSE for details.
//
// Author: Shuting Guo (shutingnjupt@gmail.com)
//         Chuan Cheng (chengchuancoder@gmail.com)
//

#include <deepx_core/common/str_util.h>
#include <deepx_core/dx_log.h>

#include <cinttypes>  // PRIu64
#include <vector>

#include "src/io/indexing.h"
#include "src/io/io_util.h"
#include "src/io/value.h"
#include "src/model/data_flow/neighbor_aggregation_flow.h"
#include "src/model/embed_instance_reader.h"
#include "src/model/instance_node_name.h"
#include "src/model/instance_reader_util.h"

namespace embedx {
namespace {

const std::string USER_ENCODER_NAME = "USER_ENCODER_NAME";
const std::string ITEM_ENCODER_NAME = "ITEM_ENCODER_NAME";

void ParseUserAndItemFrom(const vec_int_t& nodes, uint16_t user_group,
                          uint16_t item_group, vec_int_t* user_nodes,
                          vec_int_t* item_nodes) {
  for (auto& node : nodes) {
    auto group = io_util::GetNodeType(node);
    if (group == user_group) {
      user_nodes->emplace_back(node);
    } else if (group == item_group) {
      item_nodes->emplace_back(node);
    } else {
      DXERROR("Invalid node: %" PRIu64 " with ns_id: %d, expect %d or %d.",
              node, (int)group, (int)user_group, (int)item_group);
    }
  }
}

}  // namespace

/************************************************************************/
/* UnsupBipartiteInstReader */
/************************************************************************/
class UnsupBipartiteInstReader : public EmbedInstanceReader {
 private:
  bool is_train_ = true;
  int num_neg_ = 5;
  std::vector<int> num_neighbors_;
  bool use_neigh_feat_ = false;
  uint16_t user_ns_id_ = 0;
  uint16_t item_ns_id_ = 1;

 private:
  std::unique_ptr<NeighborAggregationFlow> flow_;

  vec_int_t src_nodes_;
  vec_int_t dst_nodes_;
  std::vector<vec_int_t> neg_nodes_list_;

  std::vector<Indexing> user_indexings_;
  std::vector<Indexing> item_indexings_;

 public:
  DEFINE_INSTANCE_READER_LIKE(UnsupBipartiteInstReader);

 public:
  bool InitGraphClient(const GraphClient* graph_client) override {
    if (!EmbedInstanceReader::InitGraphClient(graph_client)) {
      return false;
    }

    flow_ = NewNeighborAggregationFlow(graph_client);
    return true;
  }

 protected:
  bool InitConfigKV(const std::string& k, const std::string& v) override {
    if (InstanceReaderImpl::InitConfigKV(k, v)) {
    } else if (k == "is_train") {
      auto val = std::stoi(v);
      DXCHECK(val == 1 || val == 0);
      is_train_ = val;
    } else if (k == "num_neg") {
      num_neg_ = std::stoi(v);
      DXCHECK(num_neg_ > 0);
    } else if (k == "num_neighbors") {
      DXCHECK(deepx_core::Split<int>(v, ",", &num_neighbors_));
    } else if (k == "use_neigh_feat") {
      auto val = std::stoi(v);
      DXCHECK(val == 1 || val == 0);
      use_neigh_feat_ = val;
    } else if (k == "user_ns_id") {
      user_ns_id_ = std::stoi(v);
    } else if (k == "item_ns_id") {
      item_ns_id_ = std::stoi(v);
    } else {
      DXERROR("Unexpected config: %s = %s.", k.c_str(), v.c_str());
      return false;
    }

    DXINFO("Instance reader argument: %s = %s.", k.c_str(), v.c_str());
    return true;
  }

  bool GetBatch(Instance* inst) override {
    return is_train_ ? GetTrainBatch(inst) : GetPredictBatch(inst);
  }

  /************************************************************************/
  /* Read batch data from file for training */
  /************************************************************************/

  bool GetTrainBatch(Instance* inst) {
    std::vector<EdgeValue> values;
    if (!line_parser_.NextBatch<EdgeValue>(batch_, &values)) {
      line_parser_.Close();
      inst->clear_batch();
      return false;
    }

    src_nodes_ = Collect<EdgeValue, int_t>(values, &EdgeValue::src_node);
    dst_nodes_ = Collect<EdgeValue, int_t>(values, &EdgeValue::dst_node);
    DXCHECK(graph_client_->SharedSampleNegative(num_neg_, dst_nodes_,
                                                dst_nodes_, &neg_nodes_list_));

    // Parse user and item node from src、dst and neg nodes
    vec_int_t user_nodes, item_nodes;
    ParseUserAndItemFrom(src_nodes_, user_ns_id_, item_ns_id_, &user_nodes,
                         &item_nodes);
    ParseUserAndItemFrom(dst_nodes_, user_ns_id_, item_ns_id_, &user_nodes,
                         &item_nodes);
    for (auto& neg_nodes : neg_nodes_list_) {
      ParseUserAndItemFrom(neg_nodes, user_ns_id_, item_ns_id_, &user_nodes,
                           &item_nodes);
    }

    // Fill instance
    FillInstance(inst, USER_ENCODER_NAME, user_nodes, num_neighbors_,
                 &user_indexings_);
    FillInstance(inst, ITEM_ENCODER_NAME, item_nodes, num_neighbors_,
                 &item_indexings_);

    // Fill edge and label
    auto index_func = [this](int_t node) {
      auto group = io_util::GetNodeType(node);
      if (group == user_ns_id_) {
        int index = user_indexings_[0].Get(node);
        DXCHECK(index >= 0);
        return (int_t)index;
      }
      auto index = item_indexings_[0].Get(node);
      DXCHECK(index >= 0);
      return (int_t)index + user_indexings_[0].Size();
    };
    flow_->FillEdgeAndLabel(inst, instance_name::X_SRC_ID_NAME,
                            instance_name::X_DST_ID_NAME, deepx_core::Y_NAME,
                            src_nodes_, dst_nodes_, neg_nodes_list_, index_func,
                            index_func);

    inst->set_batch((int)src_nodes_.size());

    return true;
  }

  /************************************************************************/
  /* Read batch data from file for predicting */
  /************************************************************************/
  bool GetPredictBatch(Instance* inst) {
    std::vector<NodeValue> values;
    if (!line_parser_.NextBatch<NodeValue>(batch_, &values)) {
      line_parser_.Close();
      inst->clear_batch();
      return false;
    }

    src_nodes_ = Collect<NodeValue, int_t>(values, &NodeValue::node);

    // Parse user and item nodes from src nodes;
    vec_int_t user_nodes, item_nodes;
    ParseUserAndItemFrom(src_nodes_, user_ns_id_, item_ns_id_, &user_nodes,
                         &item_nodes);

    // Fill Instance
    FillInstance(inst, USER_ENCODER_NAME, user_nodes, num_neighbors_,
                 &user_indexings_);
    FillInstance(inst, ITEM_ENCODER_NAME, item_nodes, num_neighbors_,
                 &item_indexings_);

    // Fill index
    FillIndex(inst, instance_name::X_SRC_ID_NAME, src_nodes_);

    // set predict nodes
    auto* predict_nodes_ptr =
        &inst->get_or_insert<vec_int_t>(instance_name::X_PREDICT_NODE_NAME);
    *predict_nodes_ptr = src_nodes_;

    inst->set_batch((int)src_nodes_.size());
    return true;
  }

 private:
  int Index(int_t node) const {
    auto group = io_util::GetNodeType(node);
    if (group == user_ns_id_) {
      int index = user_indexings_[0].Get(node);
      DXCHECK(index >= 0);
      return (int_t)index;
    }
    auto index = item_indexings_[0].Get(node);
    DXCHECK(index >= 0);
    return (int_t)index + user_indexings_[0].Size();
  }

  void FillIndex(Instance* inst, const std::string& name,
                 const vec_int_t& nodes) const {
    auto* id_ptr = &inst->get_or_insert<csr_t>(name);
    id_ptr->clear();
    for (auto node : nodes) {
      id_ptr->emplace(Index(node), 1);
      id_ptr->add_row();
    }
  }

  void FillInstance(Instance* inst, const std::string& encoder_name,
                    const vec_int_t& nodes,
                    const std::vector<int>& num_neighbors,
                    std::vector<Indexing>* indexings) {
    // Sample subgraph
    vec_set_t level_nodes;
    vec_map_neigh_t level_neighs;
    flow_->SampleSubGraph(nodes, num_neighbors, &level_nodes, &level_neighs);

    // Fill node feature
    flow_->FillLevelNodeFeature(
        inst, instance_name::X_NODE_FEATURE_NAME + encoder_name, level_nodes);

    // Fill neighbor feature
    if (use_neigh_feat_) {
      flow_->FillLevelNeighFeature(
          inst, instance_name::X_NEIGH_FEATURE_NAME + encoder_name,
          level_nodes);
    }

    // Fill self And neigbor block
    inst_util::CreateIndexings(level_nodes, indexings);
    flow_->FillSelfAndNeighGraphBlock(
        inst, instance_name::X_SELF_BLOCK_NAME + encoder_name,
        instance_name::X_NEIGH_BLOCK_NAME + encoder_name, level_nodes,
        level_neighs, *indexings, false);
  }
};

INSTANCE_READER_REGISTER(UnsupBipartiteInstReader, "UnsupBipartiteInstReader");
INSTANCE_READER_REGISTER(UnsupBipartiteInstReader, "unsup_bipartite_graphsage");

}  // namespace embedx
