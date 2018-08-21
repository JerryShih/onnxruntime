#pragma once

#include "core/framework/op_kernel.h"

namespace Lotus {
class KernelRegistry {
 public:
  // Register a kernel with kernel definition and function to create the kernel.
  Status Register(KernelDefBuilder& kernel_def_builder, KernelCreateFn kernel_creator);

  Status Register(KernelCreateInfo&);

  // Mainly for provide debug info
  std::vector<std::string> GetAllRegisteredOpNames() const;

  // factory functions should always return a unique_ptr for maximum flexibility
  // for its clients unless the factory is managing the lifecycle of the pointer
  // itself.
  // TODO(Task:132) Make usage of unique_ptr/shared_ptr as out param consistent
  Status CreateKernel(const LotusIR::Node& node,
                      const IExecutionProvider* execution_provider,
                      const SessionState& session_state,
                      std::unique_ptr<OpKernel>* op_kernel) const;

  Status SearchKernelRegistry(const LotusIR::Node& node,
                              /*out*/ const KernelCreateInfo** kernel_create_info) const;

  // check if a execution provider can create kernel for a node
  bool CanExecutionProviderCreateKernel(
      const LotusIR::Node& node,
      LotusIR::ProviderType exec_provider) const;

  KernelRegistry() : KernelRegistry([](std::function<void(KernelCreateInfo&&)>) {}) {}

  KernelRegistry(std::function<void(std::function<void(KernelCreateInfo&&)>)> kernel_reg_fn)
      : kernel_reg_fn_(kernel_reg_fn) {}

  void RegisterInternal(KernelCreateInfo& create_info) const;

 private:
  friend class InferenceSession;

  // Check if the node's input/outpuData/attributes are compatible with this
  // kernel_def, If so, the kernel defined by the kernel_def is used to
  // execute this node. exec_provider is used to match kernel when node has no provider
  static bool VerifyKernelDef(const LotusIR::Node& node,
                              const KernelDef& kernel_def,
                              std::string& error_str,
                              LotusIR::ProviderType exec_provider = "");

  // Kernel create function map from op name to kernel creation info.
  mutable std::unique_ptr<KernelCreateMap> kernel_creator_fn_map_ =
      std::make_unique<KernelCreateMap>();
  KernelCreateMap const& kernel_creator_fn_map() const;
  mutable std::once_flag kernelCreationFlag;

  std::function<void(std::function<void(KernelCreateInfo&&)>)> kernel_reg_fn_;
};
}  // namespace Lotus