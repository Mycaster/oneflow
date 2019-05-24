#include "oneflow/core/graph/op_graph.h"

namespace oneflow {

std::string OpEdge::VisualStr() const {
  std::string str;
  int32_t idx = 0;
  for (const LogicalBlobId& lbi : lbis_) {
    if (idx++ > 0) { str += "\\n"; }
    str += lbi.blob_name() + ":";
    str += src_node()->LogicalBlobDesc4Lbi(lbi).shape().ToString();
  }
  return str;
}

bool* OpNode::MutHasBatchDim4Lbi(const LogicalBlobId& lbi) {
  CHECK_EQ(MutProducerOpNode4Lbi(lbi), this);
  return &lbi2has_batch_dim_[lbi];
}
bool OpNode::HasBatchDim4Lbi(const LogicalBlobId& lbi) const {
  return ProducerOpNode4Lbi(lbi)->lbi2has_batch_dim_.at(lbi);
}

const SbpParallel& OpNode::SbpParallel4BnInOp(const std::string& bn_in_op) const {
  return sbp_signature_.bn_in_op2sbp_parallel().at(bn_in_op);
}

const SbpParallel& OpNode::SbpParallel4Lbi(const LogicalBlobId& lbi) const {
  const SbpParallel* ret = nullptr;
  for (const auto& ibn : op().input_bns()) {
    if (op().BnInOp2Lbi(ibn) == lbi) {
      const auto* sbp_parallel = &SbpParallel4BnInOp(ibn);
      if (ret != nullptr) { CHECK(*ret == *sbp_parallel); }
      ret = sbp_parallel;
    }
  }
  if (ret != nullptr) { return *ret; }
  for (const auto& obn : op().output_bns()) {
    if (op().BnInOp2Lbi(obn) == lbi) { return SbpParallel4BnInOp(obn); }
  }
  UNIMPLEMENTED();
}

std::string OpNode::VisualStr() const {
  std::string str = op().op_name();
  {
    for (int64_t machine_id : parallel_desc().sorted_machine_ids()) {
      std::string dev_type;
      if (parallel_desc().device_type() == DeviceType::kCPU) {
        dev_type = "cpu";
      } else if (parallel_desc().device_type() == DeviceType::kGPU) {
        dev_type = "gpu";
      } else {
        UNIMPLEMENTED();
      }
      std::string parallel_desc_str = std::to_string(machine_id) + ":" + dev_type + ":";
      const auto& dev_phy_ids = parallel_desc().sorted_dev_phy_ids(machine_id);
      parallel_desc_str += std::to_string(dev_phy_ids.front());
      if (dev_phy_ids.back() > dev_phy_ids.front()) {
        parallel_desc_str += "-" + std::to_string(dev_phy_ids.back());
      }
      str += "\\n" + parallel_desc_str;
    }
  }
  auto GetTimeShapeStr = [&](const Shape& shape, const std::string& prefix) {
    std::string time_shape_str = prefix + ":";
    time_shape_str += shape.ToString();
    return time_shape_str;
  };
  if (in_edges().empty() == false) {
    str += "\\n" + GetTimeShapeStr(*GetInputBlobFastestTimeShape(), "in_blob_time_shape");
  }
  str += "\\n" + GetTimeShapeStr(*out_blob_time_shape(), "out_blob_time_shape");
  return str;
}

const BlobDesc& OpNode::LogicalBlobDesc4Lbi(const LogicalBlobId& lbi) const {
  return ProducerOpNode4Lbi(lbi)->lbi2logical_blob_desc_.at(lbi);
}

BlobDesc* OpNode::MutLogicalBlobDesc4Lbi(const LogicalBlobId& lbi) {
  CHECK_EQ(lbi.op_name(), op().op_name());
  return &lbi2logical_blob_desc_[lbi];
}

const Shape* OpNode::out_blob_time_shape() const {
  const Shape* ret = out_blob_time_shape_.get();
  if (ret != nullptr && ret->elem_cnt() == 0) { return nullptr; }
  return ret;
}

Shape* OpNode::mut_out_blob_time_shape() {
  if (!out_blob_time_shape_) { out_blob_time_shape_.reset(new Shape()); }
  return out_blob_time_shape_.get();
}

const Shape* OpNode::GetInputBlobTimeShape(const std::string& bn_in_op) const {
  return SrcNode4InputBnInOp(bn_in_op)->out_blob_time_shape();
}

OpNode* OpNode::ProducerOpNode4BnInOp(const std::string& bn_in_op) {
  if (ibns_.find(bn_in_op) != ibns_.end()) { return SrcNode4InputBnInOp(bn_in_op); }
  return this;
}

OpNode* OpNode::SrcNode4InputBnInOp(const std::string& bn_in_op) const {
  const LogicalBlobId& lbi = op().BnInOp2Lbi(bn_in_op);
  CHECK(ibns_.find(bn_in_op) != ibns_.end());
  return SrcNode4InputLbi(lbi);
}

OpNode* OpNode::MutProducerOpNode4Lbi(const LogicalBlobId& lbi) {
  OpNode* producer = SrcNode4InputLbi(lbi);
  if (producer == nullptr) { producer = this; }
  return producer;
}

const OpNode* OpNode::ProducerOpNode4Lbi(const LogicalBlobId& lbi) const {
  const OpNode* producer = SrcNode4InputLbi(lbi);
  if (producer == nullptr) { producer = this; }
  return producer;
}

OpNode* OpNode::SrcNode4InputLbi(const LogicalBlobId& lbi) const {
  for (OpEdge* edge : in_edges()) {
    for (const LogicalBlobId& edge_lbi : edge->lbis()) {
      if (lbi == edge_lbi) { return edge->src_node(); }
    }
  }
  return nullptr;
}

const Shape* OpNode::GetInputBlobFastestTimeShape() const {
  const Shape* ret = nullptr;
  for (OpEdge* edge : in_edges()) {
    const Shape* shape = edge->src_node()->out_blob_time_shape();
    if (ret == nullptr || shape->elem_cnt() > ret->elem_cnt()) { ret = shape; }
  }
  for (OpEdge* edge : in_edges()) {
    CHECK_EQ(ret->elem_cnt() % edge->src_node()->out_blob_time_shape()->elem_cnt(), 0);
  }
  return ret;
}

void OpNode::ForEachParallelBlobDesc(const BlobDesc& blob_desc, const SbpParallel& sbp_parallel,
                                     const std::function<void(const BlobDesc&)>& Handler) const {
  if (sbp_parallel.has_split_parallel()) {
    // split BlobDesc
    int32_t axis = sbp_parallel.split_parallel().axis();
    CHECK_GE(axis, 0);
    CHECK_LT(axis, blob_desc.shape().NumAxes());
    CHECK_GE(blob_desc.shape().At(axis), parallel_desc().parallel_num());
    BalancedSplitter bs(blob_desc.shape().At(axis), parallel_desc().parallel_num());
    FOR_RANGE(int64_t, axis_parallel_id, 0, parallel_desc().parallel_num()) {
      BlobDesc sub_blob_desc(blob_desc);
      sub_blob_desc.mut_shape().Set(axis, bs.At(axis_parallel_id).size());
      Handler(sub_blob_desc);
    }
  } else {
    CHECK(sbp_parallel.has_broadcast_parallel() || sbp_parallel.has_partial_sum_parallel());
    // broadcast BlobDesc
    FOR_RANGE(int64_t, axis_parallel_id, 0, parallel_desc().parallel_num()) { Handler(blob_desc); }
  }
}

void OpNode::ConcatBlobDesc(const std::vector<BlobDesc>& blob_descs,
                            const SbpParallel& sbp_parallel,
                            BlobDesc* concatenated_blob_desc) const {
  CHECK_EQ(blob_descs.size(), parallel_desc().parallel_num());
  if (sbp_parallel.has_split_parallel()) {
    int32_t axis = sbp_parallel.split_parallel().axis();
    // concat BlobDesc
    CHECK_GE(axis, 0);
    CHECK_LT(axis, blob_descs.at(0).shape().NumAxes());
    int64_t logical_blob_axis_dim = 0;
    for (const BlobDesc& blob_desc : blob_descs) {
      logical_blob_axis_dim += blob_desc.shape().At(axis);
    }
    CHECK_GE(logical_blob_axis_dim, parallel_desc().parallel_num());
    BalancedSplitter bs(logical_blob_axis_dim, parallel_desc().parallel_num());
    std::vector<BlobDesc> same_blob_descs(blob_descs);
    FOR_RANGE(int64_t, axis_parallel_id, 0, parallel_desc().parallel_num()) {
      CHECK_EQ(bs.At(axis_parallel_id).size(), blob_descs.at(axis_parallel_id).shape().At(axis));
      same_blob_descs.at(axis_parallel_id).mut_shape().Set(axis, logical_blob_axis_dim);
    }
    for (const BlobDesc& blob_desc : same_blob_descs) { CHECK(blob_desc == same_blob_descs.at(0)); }
    concatenated_blob_desc->CopyAllFrom(same_blob_descs.at(0));
  } else {
    // select first BlobDesc
    for (const BlobDesc& blob_desc : blob_descs) { CHECK(blob_desc == blob_descs.at(0)); }
    concatenated_blob_desc->CopyAllFrom(blob_descs.at(0));
  }
}

int64_t OpNode::GetAxisParallelNum(
    const std::function<void(bool*, int32_t*, int64_t*)>& GetAxisParallelInfo) const {
  bool is_split = false;
  int32_t axis = -1;
  int64_t axis_parallel_num = 0;
  GetAxisParallelInfo(&is_split, &axis, &axis_parallel_num);
  return axis_parallel_num;
}

void OpNode::SplitLogicalInputBlobDesc() {
  for (const std::string& bn : op().input_bns()) {
    const LogicalBlobId& lbi = op().BnInOp2Lbi(bn);
    const BlobDesc& logical_blob_desc = ProducerOpNode4BnInOp(bn)->LogicalBlobDesc4Lbi(lbi);
    const SbpParallel& sbp_parallel = SbpParallel4BnInOp(bn);
    ForEachParallelBlobDesc(logical_blob_desc, sbp_parallel, [&](const BlobDesc& blob_desc) {
      bn2parallel_id2blob_desc_[bn].push_back(blob_desc);
    });
    CHECK_EQ(bn2parallel_id2blob_desc_.at(bn).size(), parallel_desc().parallel_num());
  }
}

void OpNode::ConcatLogicalOutputBlobDesc() {
  for (const std::string& bn : op().output_bns()) {
    const LogicalBlobId& lbi = op().BnInOp2Lbi(bn);
    const SbpParallel& sbp_parallel = SbpParallel4BnInOp(bn);
    ConcatBlobDesc(bn2parallel_id2blob_desc_.at(bn), sbp_parallel, MutLogicalBlobDesc4Lbi(lbi));
  }
}

void OpNode::CheckBlobDescs(const std::function<BlobDesc*(const std::string&)>& GetBlobDesc4BnInOp,
                            const ParallelContext* parallel_ctx) const {
  int64_t parallel_id = parallel_ctx->parallel_id();
  auto Check = [&](const std::string& bn) {
    if (bn2parallel_id2blob_desc_.find(bn) == bn2parallel_id2blob_desc_.end()) { return; }
    CHECK_EQ(parallel_ctx->parallel_num(), bn2parallel_id2blob_desc_.at(bn).size());
    const BlobDesc& blob_desc_from_exec_graph = *GetBlobDesc4BnInOp(bn);
    const BlobDesc& blob_desc_from_op_graph = bn2parallel_id2blob_desc_.at(bn).at(parallel_id);
    CHECK_EQ(blob_desc_from_exec_graph.shape(), blob_desc_from_op_graph.shape());
    CHECK_EQ(blob_desc_from_exec_graph.data_type(), blob_desc_from_op_graph.data_type());
  };
  for (const std::string& bn : op().input_bns()) { Check(bn); }
  for (const std::string& bn : op().output_bns()) { Check(bn); }
  for (const std::string& bn : op().data_tmp_bns()) { Check(bn); }
  for (const std::string& bn : op().fw_buf_bns()) { Check(bn); }
  for (const std::string& bn : op().model_bns()) { Check(bn); }
  for (const std::string& bn : op().const_model_bns()) { Check(bn); }
  for (const std::string& bn : op().const_buf_bns()) { Check(bn); }
  for (const std::string& bn : op().forward_model_bns()) { Check(bn); }
}

void OpGraph::InferOpModelSize(HashMap<std::string, size_t>* op_name2model_size) {
  auto BlobDesc4ModelLbi = MakeGetterBlobDesc4ModelLbi();
  ForEachNode([&](OpNode* op_node) {
    size_t model_size = 0;
    for (const std::string& model_bn : op_node->op().model_bns()) {
      const auto& lbi = op_node->op().BnInOp2Lbi(model_bn);
      int64_t elem_cnt = BlobDesc4ModelLbi(lbi).shape().elem_cnt();
      model_size += elem_cnt * GetSizeOfDataType(Global<JobDesc>::Get()->DefaultDataType());
      model_size = RoundUp(model_size, kCudaAlignSize);
    }
    size_t parallel_num = op_node->parallel_desc().parallel_num();
    if (op_node->parallel_desc().policy() == ParallelPolicy::kModelParallel) {
      model_size = (model_size + parallel_num - 1) / parallel_num;
    }
    CHECK(op_name2model_size->emplace(op_node->op().op_name(), model_size).second);
  });
}

void OpGraph::Init(const Job& job) {
  InitNodes(job);
  ForEachNode(
      [&](OpNode* node) { CHECK(op_name2op_node_.emplace(node->op().op_name(), node).second); });
  InitEdges();
  // CHECK(!FindFirstNontrivialSCC());
  FixOpParallelDesc();
  UpdateOpNodeHasInDiff();
  InferTimeShape();
  InferLogicalBlobDesc(job);
}

void OpGraph::InitNodes(const Job& job) {
  auto ParallelConf4OpName = MakeGetterParallelConf4OpName(job.placement());
  for (const auto& op_conf : job.net().op()) {
    OpNode* node = new OpNode(ParallelDesc(*ParallelConf4OpName(op_conf.name())), op_conf);
    AddAllocatedNode(node);
  }
}

void OpGraph::InitEdges() {
  HashMap<LogicalBlobId, OpNode*> lbi2producer;
  HashMap<std::string, HashMap<LogicalBlobId, std::string>> producer_op_name2lbi2obn;
  ForEachNode([&](OpNode* op_node) {
    for (const auto& obn : op_node->op().output_bns()) {
      const auto& lbi = op_node->op().BnInOp2Lbi(obn);
      CHECK(lbi2producer.emplace(lbi, op_node).second);
      CHECK(producer_op_name2lbi2obn[op_node->op().op_name()].emplace(lbi, obn).second);
    }
  });
  ForEachNode([&](OpNode* op_node) {
    HashMap<std::string, HashSet<LogicalBlobId>> producer_op_name2lbis;
    HashMap<std::string, HashMap<LogicalBlobId, std::vector<std::string>>>
        consumer_op_name2lbi2ibns;
    for (const auto& ibn : op_node->op().input_bns()) {
      const LogicalBlobId& lbi = op_node->op().BnInOp2Lbi(ibn);
      producer_op_name2lbis[lbi.op_name()].insert(lbi);
      consumer_op_name2lbi2ibns[op_node->op().op_name()][lbi].push_back(ibn);
    }
    for (const auto& pair : producer_op_name2lbis) {
      std::vector<LogicalBlobId> lbis{pair.second.begin(), pair.second.end()};
      const auto& lbi2obn = producer_op_name2lbi2obn.at(pair.first);
      const auto& lbi2ibns = consumer_op_name2lbi2ibns.at(op_node->op().op_name());
      OpNode* producer = lbi2producer.at(lbis.at(0));
      Connect(producer, NewEdge(lbis, lbi2obn, lbi2ibns), op_node);
    }
  });
}

void OpGraph::FixOpParallelDesc() const {
  ForEachNode([&](OpNode* node) { node->op().FixParallelDesc(node->mut_parallel_desc()); });
}

void OpGraph::UpdateOpNodeHasInDiff() const {
  TopoForEachNode([&](OpNode* op_node) {
    bool has_diff = false;
    for (OpEdge* edge : op_node->in_edges()) {
      if (edge->src_node()->has_in_diff() || edge->src_node()->has_model_diff()) {
        edge->set_has_diff(true);
        has_diff = true;
        break;
      }
    }
    op_node->set_has_in_diff(has_diff);
  });
}

void OpGraph::InferTimeShape() const {
  TopoForEachNode([&](OpNode* op_node) {
    ParallelContext parallel_ctx;
    parallel_ctx.set_parallel_id(0);
    parallel_ctx.set_parallel_num(op_node->parallel_desc().parallel_num());
    parallel_ctx.set_policy(op_node->parallel_desc().policy());
    auto GetInputBlobTimeShape = [&](const std::string& bn_in_op) {
      return op_node->GetInputBlobTimeShape(bn_in_op);
    };
    op_node->op().InferOutputBlobTimeShapeIf(GetInputBlobTimeShape, &parallel_ctx,
                                             op_node->mut_out_blob_time_shape());
  });
}

void OpGraph::InferOpNodeSbpSignature(OpNode* op_node, const SbpSignature& sbp_sig_conf) const {
  HashMap<std::string, SbpInferHint> ibn2sbp_infer_hint;
  for (const std::string& ibn : op_node->op().input_bns()) {
    const LogicalBlobId& lbi = op_node->op().BnInOp2Lbi(ibn);
    OpNode* producer = op_node->SrcNode4InputBnInOp(ibn);
    const ParallelDesc* parallel_desc = &op_node->parallel_desc();
    const BlobDesc* logical_blob_desc = &producer->LogicalBlobDesc4Lbi(lbi);
    const auto& sbp = producer->SbpParallel4Lbi(lbi);
    ibn2sbp_infer_hint.emplace(ibn, SbpInferHint(parallel_desc, logical_blob_desc, sbp));
  }
  SbpSignature* sbp_signature = op_node->mut_sbp_signature();
  auto SbpInferHint4Ibn = [&](const std::string& ibn) -> const SbpInferHint& {
    return ibn2sbp_infer_hint.at(ibn);
  };
  std::function<int32_t(const SbpSignature&)> CalcOrderValue4SbpSig;
  if (sbp_sig_conf.bn_in_op2sbp_parallel().empty()) {
    auto OrderValue4HasBatchDim = [&](const std::string& bn,
                                      const SbpParallel& sbp_parallel) -> int32_t {
      return -1
             * (op_node->HasBatchDim4Lbi(op_node->op().BnInOp2Lbi(bn))
                && sbp_parallel.has_split_parallel() && sbp_parallel.split_parallel().axis() == 0);
    };
    auto OrderValue4HasNoBatchDim = [&](const std::string& ibn,
                                        const SbpParallel& sbp_parallel) -> int32_t {
      return -2
             * (op_node->HasBatchDim4Lbi(op_node->op().BnInOp2Lbi(ibn)) == false
                && SbpInferHint4Ibn(ibn).sbp_parallel().has_split_parallel() == false
                && sbp_parallel.has_split_parallel() == false);
    };
    CalcOrderValue4SbpSig = [&](const SbpSignature& sbp_signature) -> int32_t {
      int32_t order_value = 0;
      for (const auto& ibn : op_node->op().input_bns()) {
        const auto& sbp_parallel_it = sbp_signature.bn_in_op2sbp_parallel().find(ibn);
        CHECK(sbp_parallel_it != sbp_signature.bn_in_op2sbp_parallel().end());
        order_value += OrderValue4HasBatchDim(ibn, sbp_parallel_it->second);
        order_value += OrderValue4HasNoBatchDim(ibn, sbp_parallel_it->second);
      }
      for (const auto& obn : op_node->op().output_bns()) {
        const auto& sbp_parallel_it = sbp_signature.bn_in_op2sbp_parallel().find(obn);
        CHECK(sbp_parallel_it != sbp_signature.bn_in_op2sbp_parallel().end());
        order_value += OrderValue4HasBatchDim(obn, sbp_parallel_it->second);
      }
      return order_value;
    };
  } else {
    CalcOrderValue4SbpSig = [](const SbpSignature&) -> int32_t { return 0; };
  }
  op_node->op().InferSbpSignatureIf(sbp_signature, sbp_sig_conf, CalcOrderValue4SbpSig,
                                    SbpInferHint4Ibn, op_node->parallel_desc());
  op_node->op().FixSbpSignature(SbpInferHint4Ibn, sbp_signature);
}

void OpGraph::InferOpNodeLogicalBlobDesc(OpNode* op_node) const {
  auto* bn2parallel_id2blob_desc = op_node->mut_bn2parallel_id2blob_desc();
  op_node->SplitLogicalInputBlobDesc();
  int64_t parallel_num = op_node->parallel_desc().parallel_num();
  const auto& input_bns = op_node->op().input_bns();
  FOR_RANGE(int64_t, parallel_id, 0, parallel_num) {
    auto BlobDesc4BnInOp = [&](const std::string& bn) -> BlobDesc* {
      if (std::find(input_bns.begin(), input_bns.end(), bn) != input_bns.end()) {
        CHECK(bn2parallel_id2blob_desc->find(bn) != bn2parallel_id2blob_desc->end());
        CHECK_EQ(bn2parallel_id2blob_desc->at(bn).size(), parallel_num);
      } else if (bn2parallel_id2blob_desc->find(bn) == bn2parallel_id2blob_desc->end()) {
        (*bn2parallel_id2blob_desc)[bn].resize(parallel_num);
      } else {
        CHECK_EQ(bn2parallel_id2blob_desc->at(bn).size(), parallel_num);
      }
      return &(*bn2parallel_id2blob_desc)[bn][parallel_id];
    };
    ParallelContext parallel_ctx;
    parallel_ctx.set_parallel_id(parallel_id);
    parallel_ctx.set_parallel_num(parallel_num);
    parallel_ctx.set_policy(op_node->parallel_desc().policy());
    op_node->op().InferBlobDescsIf(BlobDesc4BnInOp, &parallel_ctx,
                                   Global<JobDesc>::Get()->RecordPieceSize(), [](OpContext*) {});
  }
  op_node->ConcatLogicalOutputBlobDesc();
}

void OpGraph::InferLogicalBlobDesc(const Job& job) const {
  TopoForEachNode([&](OpNode* op_node) {
    // infer has_batch_dim
    auto HasBatchDim4BnInOp = [&](const std::string& bn) -> bool* {
      return op_node->ProducerOpNode4BnInOp(bn)->MutHasBatchDim4Lbi(op_node->op().BnInOp2Lbi(bn));
    };
    auto LogicalBlobDesc4Ibn = [&](const std::string& ibn) -> const BlobDesc& {
      const auto& ibns = op_node->op().input_bns();
      CHECK(std::find(ibns.begin(), ibns.end(), ibn) != ibns.end());
      return op_node->LogicalBlobDesc4Lbi(op_node->op().BnInOp2Lbi(ibn));
    };
    op_node->op().InferHasBatchDimIf(LogicalBlobDesc4Ibn, HasBatchDim4BnInOp);
    // infer sbp_signature
    SbpSignature sbp_sig_conf;
    {
      const auto& op_name2sbp_sig_conf = job.sbp_conf().op_name2sbp_signature_conf();
      const auto& it = op_name2sbp_sig_conf.find(op_node->op().op_name());
      if (it != op_name2sbp_sig_conf.end()) { sbp_sig_conf = it->second; }
    }
    InferOpNodeSbpSignature(op_node, sbp_sig_conf);
    // infer logical_blob_desc
    InferOpNodeLogicalBlobDesc(op_node);
  });
}

BalancedSplitter OpGraph::GetBalancedSplitter(const std::string& op_name,
                                              const LogicalBlobId& lbi) const {
  OpNode* op_node = op_name2op_node_.at(GetOpNameKey(op_name, lbi));
  const SbpParallel& sbp_parallel = GetSbpParallel(op_name, lbi);
  CHECK(sbp_parallel.has_split_parallel());
  int64_t split_num = GetSplitNum(op_name, lbi);
  if (IsBatchDimBlob(op_name, lbi)) {
    CHECK_EQ(sbp_parallel.split_parallel().axis(), 0);
    CHECK_EQ(split_num % op_node->parallel_desc().parallel_num(), 0);
  } else {
    CHECK_GE(split_num, op_node->parallel_desc().parallel_num());
  }
  return BalancedSplitter(split_num, op_node->parallel_desc().parallel_num());
}

int32_t OpGraph::GetModelSplitAxis(const std::string& op_name, const LogicalBlobId& lbi) const {
  const SbpParallel& sbp_parallel = GetSbpParallel(op_name, lbi);
  CHECK(sbp_parallel.has_split_parallel());
  return sbp_parallel.split_parallel().axis();
}

int64_t OpGraph::GetSplitNum(const std::string& op_name, const LogicalBlobId& lbi) const {
  OpNode* op_node = op_name2op_node_.at(GetOpNameKey(op_name, lbi));
  const LogicalBlobId& lbi_key = GetLogicalBlobIdKey(op_name, lbi);
  const SbpParallel& sbp_parallel = op_node->SbpParallel4Lbi(lbi_key);
  CHECK(sbp_parallel.has_split_parallel());
  return op_node->LogicalBlobDesc4Lbi(lbi_key).shape().At(sbp_parallel.split_parallel().axis());
}

const SbpParallel& OpGraph::GetSbpParallel(const std::string& op_name,
                                           const LogicalBlobId& lbi) const {
  return op_name2op_node_.at(GetOpNameKey(op_name, lbi))
      ->SbpParallel4Lbi(GetLogicalBlobIdKey(op_name, lbi));
}

DataType OpGraph::GetBlobDataType(const LogicalBlobId& lbi) const {
  return op_name2op_node_.at(lbi.op_name())
      ->LogicalBlobDesc4Lbi(GetLogicalBlobIdKey(lbi.op_name(), lbi))
      .data_type();
}

const BlobDesc& OpGraph::GetLogicalBlobDesc(const LogicalBlobId& lbi) const {
  return op_name2op_node_.at(lbi.op_name())
      ->LogicalBlobDesc4Lbi(GetLogicalBlobIdKey(lbi.op_name(), lbi));
}

bool OpGraph::IsBatchDimBlob(const std::string& op_name, const LogicalBlobId& lbi) const {
  return op_name2op_node_.at(GetOpNameKey(op_name, lbi))
      ->HasBatchDim4Lbi(GetLogicalBlobIdKey(op_name, lbi));
}

void OpGraph::CheckBlobDescs(const std::string& op_name,
                             const std::function<BlobDesc*(const std::string&)>& GetBlobDesc4BnInOp,
                             const ParallelContext* parallel_ctx) const {
  if (op_name2op_node_.find(op_name) == op_name2op_node_.end()) { return; }
  op_name2op_node_.at(op_name)->CheckBlobDescs(GetBlobDesc4BnInOp, parallel_ctx);
}

void OpGraph::ForEachPseudoChain(
    const std::function<void(const HashSet<OpNode*>&)>& Handler) const {
  auto IsReachable = MakePredicatorIsReachable();
  ForEachChainFamily(
      [&](const HashSet<OpNode*>& nodes) { ForEachPseudoChain(nodes, IsReachable, Handler); });
}

void OpGraph::ForEachChainFamily(
    const std::function<void(const HashSet<OpNode*>&)>& Handler) const {
  auto IsSameSbpEdge = [](OpEdge* edge) -> bool {
    for (const LogicalBlobId& lbi : edge->lbis()) {
      if (edge->src_node()->SbpParallel4Lbi(lbi) != edge->dst_node()->SbpParallel4Lbi(lbi)) {
        return false;
      }
    }
    return true;
  };
  auto WithSameParallelDescAndTimeShape = [](OpEdge* edge) -> bool {
    OpNode* src = edge->src_node();
    OpNode* dst = edge->dst_node();
    if (!src->parallel_desc().EqualsIgnoringPolicy(dst->parallel_desc())) { return false; }
    if (src->in_edges().empty()) { return false; }
    if (*src->GetInputBlobFastestTimeShape() != *src->out_blob_time_shape()) { return false; }
    if (*dst->GetInputBlobFastestTimeShape() != *dst->out_blob_time_shape()) { return false; }
    if (*src->out_blob_time_shape() != *dst->out_blob_time_shape()) { return false; }
    return true;
  };
  auto Is121Edge = [&](OpEdge* edge) -> bool {
    return IsSameSbpEdge(edge) && WithSameParallelDescAndTimeShape(edge);
  };
  auto ForEachConnectedWithSameSbp7ParallelDesc7TimeShape =
      [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
        for (OpEdge* edge : node->in_edges()) {
          if (Is121Edge(edge)) { Handler(edge->src_node()); }
        }
        for (OpEdge* edge : node->out_edges()) {
          if (Is121Edge(edge)) { Handler(edge->dst_node()); }
        }
      };
  ForEachConnectedComponent(source_nodes(), ForEachConnectedWithSameSbp7ParallelDesc7TimeShape,
                            Handler);
}

void OpGraph::ForEachPseudoChain(
    const HashSet<OpNode*>& nodes, const std::function<bool(OpNode* src, OpNode* dst)>& IsReachable,
    const std::function<void(const HashSet<OpNode*>&)>& Handler) const {
  if (nodes.size() <= 1) { return; }
  if ((*nodes.begin())->parallel_desc().device_type() == DeviceType::kCPU) { return; }
  if ((*nodes.begin())->parallel_desc().policy() != ParallelPolicy::kDataParallel) { return; }
  HashSet<OpNode*> all_nodes(nodes);
  while (all_nodes.size() > 1) {
    HashSet<OpNode*> chain;
    ReverseTopoGetPseudoChain(all_nodes, &chain, IsReachable);
    Handler(chain);
    for (OpNode* node_in_chain : chain) { all_nodes.erase(node_in_chain); }
  }
}

void OpGraph::ReverseTopoGetPseudoChain(
    const HashSet<OpNode*>& op_nodes, HashSet<OpNode*>* pseudo_chain_nodes,
    const std::function<bool(OpNode* src, OpNode* dst)>& IsReachable) const {
  // get sink nodes
  std::list<OpNode*> sinks;
  auto IsSink = [&](OpNode* node) {
    for (OpNode* inner_node : op_nodes) {
      if (IsReachable(node, inner_node)) { return false; }
    }
    return true;
  };
  for (OpNode* op_node : op_nodes) {
    if (IsSink(op_node)) { sinks.push_back(op_node); }
  }
  // generate connections of subgraph
  HashMap<OpNode*, std::vector<OpNode*>> node2in_nodes;
  HashMap<OpNode*, std::vector<OpNode*>> node2out_nodes;
  auto IsInSubset = [&](OpNode* node) { return op_nodes.find(node) != op_nodes.end(); };
  auto ReachableToAnySink = [&](OpNode* node) {
    for (OpNode* sink : sinks) {
      if (node == sink) { return true; }
      if (IsReachable(node, sink)) { return true; }
    }
    return false;
  };
  auto AnyOutputNodesNotInSubsetAndReachableToSink = [&](OpNode* node) {
    for (OpEdge* edge : node->out_edges()) {
      if (!IsInSubset(edge->dst_node()) && ReachableToAnySink(edge->dst_node())) { return true; }
    }
    return false;
  };
  for (OpNode* node : op_nodes) {
    if (AnyOutputNodesNotInSubsetAndReachableToSink(node)) { continue; }
    node->ForEachNodeOnOutEdge([&](OpNode* out_node) {
      if (IsInSubset(out_node)) {
        node2in_nodes[out_node].push_back(node);
        node2out_nodes[node].push_back(out_node);
      }
    });
  }
  // get chain nodes
  auto ForEachInNode = [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
    for (OpNode* in_node : node2in_nodes[node]) { Handler(in_node); }
  };
  auto ForEachOutNode = [&](OpNode* node, const std::function<void(OpNode*)>& Handler) {
    for (OpNode* out_node : node2out_nodes[node]) { Handler(out_node); }
  };
  TopoForEachNode(sinks, ForEachOutNode, ForEachInNode,
                  [&](OpNode* node) { CHECK(pseudo_chain_nodes->emplace(node).second); });
}

std::string OpGraph::GetOpNameKey(const std::string& op_name, const LogicalBlobId& lbi) const {
  CHECK(!lbi.has_is_packed_id());
  std::string op_name_key;
  if (op_name2op_node_.find(op_name) == op_name2op_node_.end()) {
    CHECK(lbi.has_clone_id());
    return lbi.op_name();
  } else {
    CHECK(!lbi.has_clone_id());
    return op_name;
  }
}

LogicalBlobId OpGraph::GetLogicalBlobIdKey(const std::string& op_name,
                                           const LogicalBlobId& lbi) const {
  CHECK(!lbi.has_is_packed_id());
  if (op_name2op_node_.find(op_name) == op_name2op_node_.end()) {
    CHECK(lbi.has_clone_id());
    LogicalBlobId lbi_key;
    lbi_key.set_op_name(lbi.op_name());
    lbi_key.set_blob_name(lbi.blob_name());
    return lbi_key;
  } else {
    CHECK(!lbi.has_clone_id());
    return lbi;
  }
}

std::function<const BlobDesc&(const LogicalBlobId&)> OpGraph::MakeGetterBlobDesc4ModelLbi() const {
  HashMap<LogicalBlobId, BlobDesc> lbi2unparalleled_blob_desc;
  TopoForEachNode([&](OpNode* op_node) {
    ParallelContext parallel_ctx;
    parallel_ctx.set_parallel_id(0);
    parallel_ctx.set_parallel_num(1);
    parallel_ctx.set_policy(op_node->parallel_desc().policy());
    auto MutUnparalleledBlobDesc4BnInOp = [&](const std::string& bn) -> BlobDesc* {
      return &lbi2unparalleled_blob_desc[op_node->op().BnInOp2Lbi(bn)];
    };
    // the real important data we want to get is:
    // a) model blobs' byte size;
    // b) number of axes of blobs' body shape;
    // Hence the argument record_piece_size can be any positive number, here it's 1
    op_node->op().InferBlobDescsIf(MutUnparalleledBlobDesc4BnInOp, &parallel_ctx, 1,
                                   [](OpContext*) {});
  });
  auto model_lbi2blob_desc = std::make_shared<HashMap<LogicalBlobId, BlobDesc>>();
  ForEachNode([&](OpNode* op_node) {
    auto ForEachModelBn = [&](const std::function<void(const std::string&)>& Handler) {
      for (const std::string& bn : op_node->op().model_bns()) { Handler(bn); }
      for (const std::string& bn : op_node->op().const_model_bns()) { Handler(bn); }
      for (const std::string& bn : op_node->op().forward_model_bns()) { Handler(bn); }
    };
    ForEachModelBn([&](const std::string& model_bn) {
      const auto& lbi = op_node->op().BnInOp2Lbi(model_bn);
      CHECK(model_lbi2blob_desc->emplace(lbi, lbi2unparalleled_blob_desc.at(lbi)).second);
    });
  });
  return [model_lbi2blob_desc](const LogicalBlobId& model_lbi) -> const BlobDesc& {
    return model_lbi2blob_desc->at(model_lbi);
  };
}

}  // namespace oneflow
