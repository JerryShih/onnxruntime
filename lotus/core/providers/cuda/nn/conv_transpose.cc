#include "conv_transpose.h"

namespace Lotus {
namespace Cuda {

#define REGISTER_KERNEL_TYPED(T)                                                \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                                \
      ConvTranspose,                                                            \
      kOnnxDomain,                                                              \
      1,                                                                        \
      T,                                                                        \
      kCudaExecutionProvider,                                                   \
      KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      ConvTranspose<T>);

REGISTER_KERNEL_TYPED(float)
REGISTER_KERNEL_TYPED(double)
REGISTER_KERNEL_TYPED(MLFloat16)

template <typename T>
Status ConvTranspose<T>::ComputeInternal(OpKernelContext* context) const {
  typedef typename ToCudaType<T>::MappedType CudaT;

  const Tensor* X = context->Input<Tensor>(0);
  const TensorShape& x_shape = X->Shape();
  const auto& x_dims = x_shape.GetDims();
  auto x_data = reinterpret_cast<const CudaT*>(X->Data<T>());

  const Tensor* W = context->Input<Tensor>(1);
  const TensorShape& w_shape = W->Shape();
  std::vector<int64_t> w_dims = w_shape.GetDims();
  auto w_data = reinterpret_cast<const CudaT*>(W->Data<T>());

  size_t num_inputs = OpKernel::Node().InputDefs().size();
  bool has_bias = (num_inputs == 3);

  CudaT* y_data = nullptr;

  {
    std::lock_guard<std::mutex> lock(s_.mutex);
    // TODO: add a global cache if need to handle cases for multiple frames running simultaneuously with different batch_size
    bool input_dims_changed = (s_.last_x_dims != x_dims);
    bool w_dims_changed = (s_.last_w_dims != w_dims);
    if (input_dims_changed || w_dims_changed) {
      if (input_dims_changed)
        s_.last_x_dims = x_dims;

      if (w_dims_changed)
        s_.last_w_dims = w_dims;

      Prepare p;
      LOTUS_RETURN_IF_ERROR(PrepareForCompute(context, has_bias, p));

      const auto& y_dims = p.Y->Shape().GetDims();
      s_.y_dims = y_dims;

      LOTUS_RETURN_IF_ERROR(s_.x_tensor.Set(x_dims, CudnnTensor::GetDataType<CudaT>()));
      LOTUS_RETURN_IF_ERROR(s_.y_tensor.Set(y_dims, CudnnTensor::GetDataType<CudaT>()));

      if (w_dims_changed)
        LOTUS_RETURN_IF_ERROR(s_.filter_desc.Set(w_dims, CudnnTensor::GetDataType<CudaT>()));

      cudnnConvolutionMode_t mode = CUDNN_CROSS_CORRELATION;
      LOTUS_RETURN_IF_ERROR(s_.conv_desc.Set(p.kernel_shape.size(), p.pads, p.strides, p.dilations, mode, CudnnTensor::GetDataType<CudaT>()));
      CUDNN_RETURN_IF_ERROR(cudnnSetConvolutionGroupCount(s_.conv_desc, gsl::narrow_cast<int>(group_)));

      IAllocatorUniquePtr<void> algo_search_workspace = GetScratchBuffer<void>(AlgoSearchWorkspaceSize);

      if (has_bias) {
        const auto& b_shape = p.B->Shape();
        LOTUS_RETURN_IF_NOT(b_shape.NumDimensions() == 1, "bias should be 1D");
        std::vector<int64_t> b_dims(2 + p.kernel_shape.size());
        b_dims[0] = 1;           // N
        b_dims[1] = b_shape[0];  // C
        for (int i = 0; i < p.kernel_shape.size(); i++)
          b_dims[2 + i] = 1;

        LOTUS_RETURN_IF_ERROR(s_.b_tensor.Set(b_dims, CudnnTensor::GetDataType<CudaT>()));
      }

      y_data = reinterpret_cast<CudaT*>(p.Y->MutableData<T>());

      cudnnConvolutionBwdDataAlgoPerf_t perf;
      int algo_count = 1;
      CUDNN_RETURN_IF_ERROR(cudnnFindConvolutionBackwardDataAlgorithmEx(
          CudnnHandle(),
          s_.filter_desc,
          w_data,
          s_.x_tensor,
          x_data,
          s_.conv_desc,
          s_.y_tensor,
          y_data,
          1,
          &algo_count,
          &perf,
          algo_search_workspace.get(),
          AlgoSearchWorkspaceSize));
      CUDNN_RETURN_IF_ERROR(cudnnSetConvolutionMathType(s_.conv_desc, perf.mathType));
      s_.algo = perf.algo;
      s_.workspace_bytes = perf.memory;
    }
  }

  if (!y_data) {
    Tensor* Y = context->Output(0, TensorShape(s_.y_dims));
    y_data = reinterpret_cast<CudaT*>(Y->MutableData<T>());
  }

  const auto alpha = Consts<CudaT>::One;
  const auto beta = Consts<CudaT>::Zero;

  IAllocatorUniquePtr<void> workspace = GetScratchBuffer<void>(s_.workspace_bytes);

  CUDNN_RETURN_IF_ERROR(
      cudnnConvolutionBackwardData(
          CudnnHandle(),
          &alpha,
          s_.filter_desc,
          w_data,
          s_.x_tensor,
          x_data,
          s_.conv_desc,
          s_.algo,
          workspace.get(),
          s_.workspace_bytes,
          &beta,
          s_.y_tensor,
          y_data));

  if (has_bias) {
    const Tensor* B = context->Input<Tensor>(2);
    auto b_data = reinterpret_cast<const CudaT*>(B->Data<T>());
    CUDNN_RETURN_IF_ERROR(cudnnAddTensor(CudnnHandle(), &alpha, s_.b_tensor, b_data, &alpha, s_.y_tensor, y_data));
  }

  return Status::OK();
}

}  // namespace Cuda
}  // namespace Lotus
