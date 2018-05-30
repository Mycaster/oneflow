#ifndef ONEFLOW_CORE_KERNEL_REDUCE_GATHER_KERNEL_H_
#define ONEFLOW_CORE_KERNEL_REDUCE_GATHER_KERNEL_H_

#include "oneflow/core/kernel/kernel.h"

namespace oneflow {

class ReduceGatherKernel final : public Kernel {
 public:
  OF_DISALLOW_COPY_AND_MOVE(ReduceGatherKernel);
  ReduceGatherKernel() = default;
  ~ReduceGatherKernel() = default;

 private:
  void ForwardDataContent(const KernelCtx&,
                          std::function<Blob*(const std::string&)>) const override;
};

}  // namespace oneflow

#endif  // ONEFLOW_CORE_KERNEL_REDUCE_GATHER_KERNEL_H_
