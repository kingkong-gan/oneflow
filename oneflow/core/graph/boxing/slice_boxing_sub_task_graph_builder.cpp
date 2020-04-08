#include "oneflow/core/graph/boxing/slice_boxing_sub_task_graph_builder.h"
#include "oneflow/core/register/tensor_slice_view.h"
#include "oneflow/core/common/balanced_splitter.h"
#include "oneflow/core/graph/slice_boxing_task_node.h"
#include "oneflow/core/graph/boxing/sub_task_graph_builder_util.h"

namespace oneflow {

namespace {

void GroupParallelIdByMachine(const ParallelDesc& pd,
                              HashMap<int64_t, std::vector<int64_t>>* machine_id2parallel_ids) {
  FOR_RANGE(int64_t, parallel_id, 0, pd.parallel_num()) {
    (*machine_id2parallel_ids)[pd.MachineIdForParallelId(parallel_id)].push_back(parallel_id);
  }
}

bool ContainsEmptySlice(const std::vector<TensorSliceView>& slices) {
  return std::any_of(slices.cbegin(), slices.cend(),
                     [](const TensorSliceView& slice) { return slice.IsEmpty(); });
}

}  // namespace

Maybe<void> SliceBoxingSubTskGphBuilder::Build(
    SubTskGphBuilderCtx* ctx, const std::vector<CompTaskNode*>& sorted_src_comp_tasks,
    const std::vector<CompTaskNode*>& sorted_dst_comp_tasks, const ParallelDesc& src_parallel_desc,
    const ParallelDesc& dst_parallel_desc, const LogicalBlobId& lbi,
    const BlobDesc& logical_blob_desc, const SbpParallel& src_sbp_parallel,
    const SbpParallel& dst_sbp_parallel) const {
  if (SubTskGphBuilderUtil::BlobHasDynamicShape(logical_blob_desc)) {
    return Error::BoxingNotSupported();
  }
  if (!SubTskGphBuilderUtil::IsDeviceTypeCPUOrGPU(src_parallel_desc)) {
    return Error::BoxingNotSupported();
  }
  if (!SubTskGphBuilderUtil::IsDeviceTypeCPUOrGPU(dst_parallel_desc)) {
    return Error::BoxingNotSupported();
  }
  if (SubTskGphBuilderUtil::HasEmptySliceIfSplit(src_parallel_desc.parallel_num(), src_sbp_parallel,
                                                 logical_blob_desc)) {
    return Error::BoxingNotSupported();
  }
  if (SubTskGphBuilderUtil::HasEmptySliceIfSplit(dst_parallel_desc.parallel_num(), dst_sbp_parallel,
                                                 logical_blob_desc)) {
    return Error::BoxingNotSupported();
  }
  const auto GetBoxingGpuThrdId = [](const int64_t dev_id, CudaWorkType work_type) -> int64_t {
    if (work_type == CudaWorkType::kCopyH2D) {
      return Global<IDMgr>::Get()->GetGpuH2DThrdId(dev_id);
    } else if (work_type == CudaWorkType::kCopyD2H) {
      return Global<IDMgr>::Get()->GetGpuD2HThrdId(dev_id);
    } else {
      return Global<IDMgr>::Get()->GetGpuMixThrdId(dev_id);
    }
  };
  const auto NewEdge = [&ctx]() -> TaskEdge* { return ctx->task_graph()->NewEdge(); };
  const auto CreateBoxingNode121 = [&ctx, &lbi, &GetBoxingGpuThrdId](
                                       const ParallelDesc& pd, const int64_t parallel_id,
                                       const TensorSliceView& slice,
                                       SliceBoxingTaskMode mode) -> SliceBoxingTaskNode* {
    SliceBoxingTaskNode* node = ctx->task_graph()->NewNode<SliceBoxingTaskNode>();
    const int64_t machine_id = pd.MachineIdForParallelId(parallel_id);
    int64_t thrd_id = -1;
    if (pd.device_type() == DeviceType::kCPU) {
      thrd_id = Global<IDMgr>::Get()->PickCpuThrdIdEvenly(machine_id);
    } else if (pd.device_type() == DeviceType::kGPU) {
      thrd_id = GetBoxingGpuThrdId(pd.DeviceIdForParallelId(parallel_id), CudaWorkType::kCopyH2D);
    } else {
      UNIMPLEMENTED();
    }
    node->Init(lbi, slice, mode, machine_id, thrd_id);
    return node;
  };
  const auto CreateBoxingNodeToHost =
      [&ctx, &lbi, &GetBoxingGpuThrdId, &NewEdge](
          TaskNode* src_node, const TensorSliceView& src_slice,
          const TensorSliceView& dst_slice) -> SliceBoxingTaskNode* {
    SliceBoxingTaskNode* dst_node = ctx->task_graph()->NewNode<SliceBoxingTaskNode>();
    int64_t thrd_id = -1;
    if (src_node->device_type() == DeviceType::kCPU) {
      thrd_id = Global<IDMgr>::Get()->PickCpuThrdIdEvenly(src_node->machine_id());
    } else if (src_node->device_type() == DeviceType::kGPU) {
      thrd_id = GetBoxingGpuThrdId(src_node->GpuPhyId(), CudaWorkType::kCopyD2H);
    } else {
      UNIMPLEMENTED();
    }
    dst_node->Init(lbi, dst_slice, kSliceBoxingTaskModeCopy, src_node->machine_id(), thrd_id,
                   Global<IDMgr>::Get()->CpuMemZoneId());
    dst_node->ConnectToSrcNodeWithSlice(src_node, NewEdge(), src_slice);
    return dst_node;
  };
  const auto BuildSubTaskGphS2B = [&ctx, &CreateBoxingNode121, &NewEdge](
                                      const ParallelDesc& in_pd, const ParallelDesc& out_pd,
                                      const SbpParallel& in_sbp, const SbpParallel& out_sbp,
                                      const BlobDesc& blob_desc,
                                      const std::vector<TaskNode*>& in_nodes,
                                      std::vector<TaskNode*>* out_nodes) {
    CHECK(SubTskGphBuilderUtil::IsBoxingS2B(in_sbp, out_sbp));
    const std::vector<TensorSliceView> in_slices =
        SubTskGphBuilderUtil::GetTensorSliceView(in_pd.parallel_num(), in_sbp, blob_desc);
    CHECK(!ContainsEmptySlice(in_slices));
    const TensorSliceView out_slice = SubTskGphBuilderUtil::GetBroadcastTensorSliceView(blob_desc);
    FOR_RANGE(int64_t, out_id, 0, out_pd.parallel_num()) {
      SliceBoxingTaskNode* out_node =
          CreateBoxingNode121(out_pd, out_id, out_slice, kSliceBoxingTaskModeCopy);
      FOR_RANGE(int64_t, in_id, 0, in_pd.parallel_num()) {
        const TensorSliceView& in_slice = in_slices.at(in_id);
        TaskNode* in_node = in_nodes.at(in_id);
        if (SubTskGphBuilderUtil::IsOnSameGPU(in_node, out_node)) {
          out_node->ConnectToSrcNodeWithSlice(in_node, NewEdge(), in_slice);
        } else {
          TaskNode* proxy_node =
              ctx->GetProxyNode(in_node, in_node->MemZoneId121(), out_node->machine_id(),
                                Global<IDMgr>::Get()->CpuMemZoneId());
          out_node->ConnectToSrcNodeWithSlice(proxy_node, NewEdge(), in_slice);
        }
      }
      out_nodes->push_back(out_node);
    }
  };
  const auto BuildSubTaskGphS2S =
      [&ctx, &lbi, &CreateBoxingNode121, &CreateBoxingNodeToHost, &GetBoxingGpuThrdId, &NewEdge](
          const ParallelDesc& in_pd, const ParallelDesc& out_pd, const SbpParallel& in_sbp,
          const SbpParallel& out_sbp, const BlobDesc& blob_desc,
          const std::vector<TaskNode*>& in_nodes, std::vector<TaskNode*>* out_nodes) {
        CHECK(SubTskGphBuilderUtil::IsBoxingS2S(in_sbp, out_sbp));
        const std::vector<TensorSliceView> in_slices =
            SubTskGphBuilderUtil::GetTensorSliceView(in_pd.parallel_num(), in_sbp, blob_desc);
        CHECK(!ContainsEmptySlice(in_slices));
        const std::vector<TensorSliceView> out_slices =
            SubTskGphBuilderUtil::GetTensorSliceView(out_pd.parallel_num(), out_sbp, blob_desc);
        CHECK(!ContainsEmptySlice(out_slices));
        HashMap<int64_t, std::vector<int64_t>> machine_id2in_parallel_ids;
        GroupParallelIdByMachine(in_pd, &machine_id2in_parallel_ids);
        FOR_RANGE(int64_t, out_id, 0, out_pd.parallel_num()) {
          const TensorSliceView& out_slice = out_slices.at(out_id);
          SliceBoxingTaskNode* out_node =
              CreateBoxingNode121(out_pd, out_id, out_slice, kSliceBoxingTaskModeCopy);
          for (const auto& pair : machine_id2in_parallel_ids) {
            const int64_t in_machine_id = pair.first;
            const std::vector<int64_t>& in_parallel_ids = pair.second;
            if (out_node->machine_id() == in_machine_id) {
              for (const int64_t in_id : in_parallel_ids) {
                const TensorSliceView& in_slice = in_slices.at(in_id);
                const TensorSliceView& intersection = out_slice.Intersect(in_slice);
                if (intersection.IsEmpty()) { continue; }
                TaskNode* in_node = in_nodes.at(in_id);
                if (in_pd.device_type() == DeviceType::kGPU
                    && out_pd.device_type() == DeviceType::kCPU) {
                  SliceBoxingTaskNode* copy_to_host =
                      CreateBoxingNodeToHost(in_node, in_slice, intersection);
                  out_node->ConnectToSrcNodeWithSlice(copy_to_host, NewEdge(), intersection);
                } else {
                  out_node->ConnectToSrcNodeWithSlice(in_node, NewEdge(), in_slice);
                }
              }
            } else {
              std::vector<TensorSliceView> intersections;
              for (const int64_t in_id : in_parallel_ids) {
                intersections.push_back(out_slice.Intersect(in_slices.at(in_id)));
              }
              const TensorSliceView concat_slice =
                  TensorSliceView::Concatenate(intersections, in_sbp.split_parallel().axis());
              SliceBoxingTaskNode* local_concat_node =
                  ctx->task_graph()->NewNode<SliceBoxingTaskNode>();
              int64_t local_concat_thrd_id = -1;
              if (in_pd.device_type() == DeviceType::kCPU) {
                local_concat_thrd_id = Global<IDMgr>::Get()->PickCpuThrdIdEvenly(in_machine_id);
              } else if (in_pd.device_type() == DeviceType::kGPU) {
                local_concat_thrd_id = GetBoxingGpuThrdId(
                    in_nodes.at(in_parallel_ids.at(out_id % in_parallel_ids.size()))->GpuPhyId(),
                    CudaWorkType::kCopyD2H);
              }
              local_concat_node->Init(lbi, concat_slice, kSliceBoxingTaskModeCopy, in_machine_id,
                                      local_concat_thrd_id, Global<IDMgr>::Get()->CpuMemZoneId());
              for (const int64_t in_id : in_parallel_ids) {
                local_concat_node->ConnectToSrcNodeWithSlice(in_nodes.at(in_id), NewEdge(),
                                                             in_slices.at(in_id));
              }
              TaskNode* local_add_proxy_node =
                  ctx->GetProxyNode(local_concat_node, Global<IDMgr>::Get()->CpuMemZoneId(),
                                    out_node->machine_id(), Global<IDMgr>::Get()->CpuMemZoneId());
              out_node->ConnectToSrcNodeWithSlice(local_add_proxy_node, NewEdge(), concat_slice);
            }
          }
          out_nodes->push_back(out_node);
        }
      };
  const auto BuildSubTaskGphP2S =
      [&ctx, &lbi, &CreateBoxingNode121, &CreateBoxingNodeToHost, &GetBoxingGpuThrdId, &NewEdge](
          const ParallelDesc& in_pd, const ParallelDesc& out_pd, const SbpParallel& in_sbp,
          const SbpParallel& out_sbp, const BlobDesc& blob_desc,
          const std::vector<TaskNode*>& in_nodes, std::vector<TaskNode*>* out_nodes) {
        CHECK(SubTskGphBuilderUtil::IsBoxingP2S(in_sbp, out_sbp));
        const TensorSliceView in_slice =
            SubTskGphBuilderUtil::GetBroadcastTensorSliceView(blob_desc);
        const std::vector<TensorSliceView> out_slices =
            SubTskGphBuilderUtil::GetTensorSliceView(out_pd.parallel_num(), out_sbp, blob_desc);
        CHECK(!ContainsEmptySlice(out_slices));
        HashMap<int64_t, std::vector<int64_t>> machine_id2in_parallel_ids;
        GroupParallelIdByMachine(in_pd, &machine_id2in_parallel_ids);
        FOR_RANGE(int64_t, out_id, 0, out_pd.parallel_num()) {
          const TensorSliceView& out_slice = out_slices.at(out_id);
          SliceBoxingTaskNode* out_node =
              CreateBoxingNode121(out_pd, out_id, out_slice, kSliceBoxingTaskModeAdd);
          for (const auto& pair : machine_id2in_parallel_ids) {
            const int64_t in_machine_id = pair.first;
            const std::vector<int64_t>& in_parallel_ids = pair.second;
            if (out_node->machine_id() == in_machine_id) {
              for (const int64_t in_id : in_parallel_ids) {
                TaskNode* in_node = in_nodes.at(in_id);
                if (SubTskGphBuilderUtil::IsOnSameGPU(in_node, out_node)) {
                  out_node->ConnectToSrcNodeWithSlice(in_node, NewEdge(), in_slice);
                } else if (in_pd.device_type() == DeviceType::kGPU) {
                  SliceBoxingTaskNode* copy_to_host =
                      CreateBoxingNodeToHost(in_node, in_slice, out_slice);
                  out_node->ConnectToSrcNodeWithSlice(copy_to_host, NewEdge(), out_slice);
                } else {
                  out_node->ConnectToSrcNodeWithSlice(in_node, NewEdge(), in_slice);
                }
              }
            } else {
              auto* local_add_node = ctx->task_graph()->NewNode<SliceBoxingTaskNode>();
              int64_t local_add_thrd_id = -1;
              if (in_pd.device_type() == DeviceType::kCPU) {
                local_add_thrd_id = Global<IDMgr>::Get()->PickCpuThrdIdEvenly(in_machine_id);
              } else if (in_pd.device_type() == DeviceType::kGPU) {
                local_add_thrd_id = GetBoxingGpuThrdId(
                    in_nodes.at(in_parallel_ids.at(out_id % in_parallel_ids.size()))->GpuPhyId(),
                    CudaWorkType::kCopyD2H);
              }
              local_add_node->Init(lbi, out_slice, kSliceBoxingTaskModeAdd, in_machine_id,
                                   local_add_thrd_id, Global<IDMgr>::Get()->CpuMemZoneId());
              for (const int64_t in_id : in_parallel_ids) {
                local_add_node->ConnectToSrcNodeWithSlice(in_nodes.at(in_id), NewEdge(), in_slice);
              }
              TaskNode* local_add_proxy_node =
                  ctx->GetProxyNode(local_add_node, Global<IDMgr>::Get()->CpuMemZoneId(),
                                    out_node->machine_id(), Global<IDMgr>::Get()->CpuMemZoneId());
              out_node->ConnectToSrcNodeWithSlice(local_add_proxy_node, NewEdge(), out_slice);
            }
          }
          out_nodes->push_back(out_node);
        }
      };

  const auto BuildSubTaskGphP2B = [&ctx, &lbi, &GetBoxingGpuThrdId, &NewEdge](
                                      const ParallelDesc& in_pd, const ParallelDesc& out_pd,
                                      const SbpParallel& in_sbp, const SbpParallel& out_sbp,
                                      const BlobDesc& blob_desc,
                                      const std::vector<TaskNode*>& in_nodes,
                                      std::vector<TaskNode*>* out_nodes) {
    CHECK(SubTskGphBuilderUtil::IsBoxingP2B(in_sbp, out_sbp));
    const TensorSliceView slice = SubTskGphBuilderUtil::GetBroadcastTensorSliceView(blob_desc);
    HashMap<int64_t, std::vector<int64_t>> machine_id2in_parallel_ids;
    HashMap<int64_t, std::vector<int64_t>> machine_id2out_parallel_ids;
    GroupParallelIdByMachine(in_pd, &machine_id2in_parallel_ids);
    GroupParallelIdByMachine(out_pd, &machine_id2out_parallel_ids);
    std::vector<TaskNode*> out_box_nodes;
    for (const auto& machine_id7in_parallel_ids : machine_id2in_parallel_ids) {
      const int64_t in_machine_id = machine_id7in_parallel_ids.first;
      const std::vector<int64_t>& in_ids_on_machine = machine_id7in_parallel_ids.second;
      if (in_ids_on_machine.size() == 1) {
        out_box_nodes.push_back(in_nodes.at(in_ids_on_machine.front()));
      } else {
        auto* local_add_node = ctx->task_graph()->NewNode<SliceBoxingTaskNode>();
        int64_t local_add_thrd_id = -1;

        if (in_pd.device_type() == DeviceType::kCPU) {
          local_add_thrd_id = Global<IDMgr>::Get()->PickCpuThrdIdEvenly(in_machine_id);
        } else if (in_pd.device_type() == DeviceType::kGPU) {
          local_add_thrd_id = GetBoxingGpuThrdId(in_nodes.at(in_ids_on_machine.front())->GpuPhyId(),
                                                 CudaWorkType::kCopyH2D);
        }
        local_add_node->Init(lbi, slice, kSliceBoxingTaskModeAdd, in_machine_id, local_add_thrd_id);
        FOR_RANGE(int64_t, i, 0, in_ids_on_machine.size()) {
          local_add_node->ConnectToSrcNodeWithSlice(in_nodes.at(in_ids_on_machine.at(i)), NewEdge(),
                                                    slice);
        }
        out_box_nodes.push_back(local_add_node);
      }
    }
    out_nodes->resize(out_pd.parallel_num());
    for (const auto& machine_id7out_parallel_ids : machine_id2out_parallel_ids) {
      const int64_t out_machine_id = machine_id7out_parallel_ids.first;
      TaskNode* in_box_node = nullptr;
      if (out_box_nodes.size() == 1) {
        in_box_node = ctx->GetProxyNode(
            out_box_nodes.front(), out_box_nodes.front()->MemZoneId121(),
            machine_id7out_parallel_ids.first, Global<IDMgr>::Get()->CpuMemZoneId());
      } else {
        auto* add_node = ctx->task_graph()->NewNode<SliceBoxingTaskNode>();
        add_node->Init(lbi, slice, kSliceBoxingTaskModeAdd, machine_id7out_parallel_ids.first,
                       Global<IDMgr>::Get()->PickCpuThrdIdEvenly(machine_id7out_parallel_ids.first),
                       Global<IDMgr>::Get()->CpuMemZoneId());
        for (TaskNode* out_box_node : out_box_nodes) {
          TaskNode* out_boxing_node_proxy =
              ctx->GetProxyNode(out_box_node, out_box_node->MemZoneId121(), out_machine_id,
                                Global<IDMgr>::Get()->CpuMemZoneId());
          add_node->ConnectToSrcNodeWithSlice(out_boxing_node_proxy, NewEdge(), slice);
        }
        in_box_node = add_node;
      }
      for (const int64_t out_id : machine_id7out_parallel_ids.second) {
        int64_t mem_zone_id;
        if (out_pd.device_type() == DeviceType::kCPU) {
          mem_zone_id = Global<IDMgr>::Get()->CpuMemZoneId();
        } else if (out_pd.device_type() == DeviceType::kGPU) {
          mem_zone_id = Global<IDMgr>::Get()->GpuMemZoneId(out_pd.DeviceIdForParallelId(out_id));
        } else {
          UNIMPLEMENTED();
        }
        (*out_nodes)[out_id] = ctx->GetProxyNode(in_box_node, Global<IDMgr>::Get()->CpuMemZoneId(),
                                                 out_machine_id, mem_zone_id);
      }
    }
  };

  std::vector<TaskNode*> in_nodes;
  in_nodes.assign(sorted_src_comp_tasks.begin(), sorted_src_comp_tasks.end());
  std::vector<TaskNode*> out_nodes;
  if (SubTskGphBuilderUtil::IsBoxingS2B(src_sbp_parallel, dst_sbp_parallel)) {
    BuildSubTaskGphS2B(src_parallel_desc, dst_parallel_desc, src_sbp_parallel, dst_sbp_parallel,
                       logical_blob_desc, in_nodes, &out_nodes);
  } else if (SubTskGphBuilderUtil::IsBoxingS2S(src_sbp_parallel, dst_sbp_parallel)) {
    BuildSubTaskGphS2S(src_parallel_desc, dst_parallel_desc, src_sbp_parallel, dst_sbp_parallel,
                       logical_blob_desc, in_nodes, &out_nodes);
  } else if (SubTskGphBuilderUtil::IsBoxingP2S(src_sbp_parallel, dst_sbp_parallel)) {
    BuildSubTaskGphP2S(src_parallel_desc, dst_parallel_desc, src_sbp_parallel, dst_sbp_parallel,
                       logical_blob_desc, in_nodes, &out_nodes);
  } else if (SubTskGphBuilderUtil::IsBoxingP2B(src_sbp_parallel, dst_sbp_parallel)) {
    if (logical_blob_desc.shape().elem_cnt() < dst_parallel_desc.parallel_num()) {
      BuildSubTaskGphP2B(src_parallel_desc, dst_parallel_desc, src_sbp_parallel, dst_sbp_parallel,
                         logical_blob_desc, in_nodes, &out_nodes);
    } else {
      BlobDesc flat_blob_desc(logical_blob_desc.data_type());
      flat_blob_desc.mut_shape() = Shape({logical_blob_desc.shape().elem_cnt()});
      std::vector<TaskNode*> middle_nodes;
      SbpParallel middle_sbp;
      middle_sbp.mutable_split_parallel()->set_axis(0);
      BuildSubTaskGphP2S(src_parallel_desc, dst_parallel_desc, src_sbp_parallel, middle_sbp,
                         flat_blob_desc, in_nodes, &middle_nodes);
      BuildSubTaskGphS2B(dst_parallel_desc, dst_parallel_desc, middle_sbp, dst_sbp_parallel,
                         flat_blob_desc, middle_nodes, &out_nodes);
      for (TaskNode* out_node : out_nodes) {
        auto* slice_boxing_node = dynamic_cast<SliceBoxingTaskNode*>(out_node);
        CHECK_NOTNULL(slice_boxing_node);
        slice_boxing_node->SetOutShape(logical_blob_desc.shape());
      }
    }
  } else {
    UNIMPLEMENTED();
  }
  ctx->ConnectAll121(out_nodes, sorted_dst_comp_tasks);
  return Maybe<void>::Ok();
}

}  // namespace oneflow