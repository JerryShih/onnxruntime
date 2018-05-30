﻿#pragma once

#include "core/common/common.h"
#include "core/framework/op_kernel.h"
#include "core/util/math_cpuonly.h"

namespace Lotus {

template <typename T>
class ImageScaler final : public OpKernel {
 public:
  ImageScaler(const OpKernelInfo& info) : OpKernel(info) {
    info.GetAttr<float>("scale", &scale_);
    info.GetAttrs<float>("bias", bias_);
  }

  Status Compute(OpKernelContext* context) const override {
    const Tensor* X = context->Input<Tensor>(0);
    const auto dims = X->Shape().GetDims();

    if (dims.size() < 4) {
      return Status(LOTUS, INVALID_ARGUMENT,
                    "Input is expected to have four dimensions corresponding to [N,C,H,W]");
    }

    const int64_t N = dims[0];
    const int64_t C = dims[1];
    const int64_t H = dims[2];
    const int64_t W = dims[3];

    if (!bias_.empty() && bias_.size() != static_cast<size_t>(C)) {
      std::ostringstream err_msg;
      err_msg << "Bias size (" << bias_.size()
              << ") does not match the number of channels (" << C << ")";
      return Status(LOTUS, INVALID_ARGUMENT, err_msg.str());
    }

    Tensor* Y = context->Output(0, TensorShape({N, C, H, W}));
    ConstEigenArrayMap<T> X_arr(X->Data<T>(), H * W, N * C);
    EigenArrayMap<T> Y_arr(Y->MutableData<T>(), H * W, N * C);

    for (int64_t nc = 0; nc < N * C; ++nc) {
      Y_arr.col(nc) = scale_ * X_arr.col(nc) + bias_[nc % C];
    }
    return Status::OK();
  }

 protected:
  float scale_;
  std::vector<float> bias_;
};

}  //namespace Lotus