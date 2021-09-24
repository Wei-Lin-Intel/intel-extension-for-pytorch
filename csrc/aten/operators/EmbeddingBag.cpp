#include <ATen/ATen.h>
#include <torch/torch.h>

#include <core/Memory.h>
#include <core/TensorImplUtils.h>
#include <runtime/Utils.h>
#include <utils/DPCPP.h>

#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/Algorithm.h"
#include "comm/Atomics.h"
#include "comm/Numerics.h"
#include "comm/PSTLFunctions.h"

#ifdef USE_ONEDPL
#include <oneapi/dpl/algorithm>
#include <oneapi/dpl/execution>
#include <oneapi/dpl/iterator>
#include <oneapi/dpl/numeric>
#endif

using namespace xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

constexpr int MODE_SUM = 0;
constexpr int MODE_MEAN = 1;
constexpr int MODE_MAX = 2;

constexpr int64_t NROWS_PER_THREAD = 10;
constexpr int64_t WARP_SIZE = 64;

void krn_partials_per_segment(
    int64_t* ret,
    const int64_t* segment_offsets,
    int64_t num_of_segments,
    int64_t numel) {
  auto& queue = dpcppGetCurrentQueue();
  int64_t group_size = 32;
  auto num_groups = CeilDiv(num_of_segments, group_size);
  auto total_items = num_groups * group_size;

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto ret_data = ret;
    auto offsets_data = segment_offsets;
    auto kfn = DPCPP_Q_KFN(DPCPP::nd_item<1> item) {
      auto ret_ptr = ret_data;
      auto offsets_ptr = offsets_data;
      int64_t id = item.get_global_id(0);
      if (id < num_of_segments) {
        const int64_t idx_start = offsets_ptr[id];
        const int64_t idx_end =
            (id == num_of_segments - 1) ? numel : offsets_ptr[id + 1];
        const int64_t size = idx_end - idx_start;
        ret_ptr[id] = CeilDiv(size, NROWS_PER_THREAD);
      }
    };

    // kick off kernel
    cgh.parallel_for(
        DPCPP::nd_range<1>(
            DPCPP::range<1>(total_items), DPCPP::range<1>(group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

void krn_partial_segment_offset(
    int64_t* ret,
    const int64_t* partials_per_segment,
    const int64_t* partials_per_segment_offset,
    const int64_t* segment_offsets,
    int64_t num_of_segments) {
  auto& queue = dpcppGetCurrentQueue();
  int64_t group_size = 32;
  auto num_groups = CeilDiv(num_of_segments, group_size);
  auto total_items = num_groups * group_size;

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto ret_data = ret;
    auto partials_per_segment_data = partials_per_segment;
    auto partials_per_segment_offset_data = partials_per_segment_offset;
    auto segment_offsets_data = segment_offsets;
    auto kfn = DPCPP_Q_KFN(DPCPP::nd_item<1> item) {
      auto ret_ptr = ret_data;
      auto partials_per_segment_ptr = partials_per_segment_data;
      auto partials_per_segment_offset_ptr = partials_per_segment_offset_data;
      auto segment_offsets_ptr = segment_offsets_data;

      int64_t id = item.get_global_id(0);
      if (id < num_of_segments) {
        int64_t idx = partials_per_segment_offset_ptr[id];
        const int64_t num_partials = partials_per_segment_ptr[id];
        const int64_t segment_offset = segment_offsets_ptr[id];
        for (int64_t i = 0; i < num_partials; ++i) {
          ret_ptr[idx++] = segment_offset + i * NROWS_PER_THREAD;
        }
      }
    };

    // kick off kernel
    cgh.parallel_for(
        DPCPP::nd_range<1>(
            DPCPP::range<1>(total_items), DPCPP::range<1>(group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

template <typename scalar_t>
void compute_grad_weight_bags(
    const Tensor& indices,
    const Tensor& gradOutput,
    const Tensor& offset2bag,
    const Tensor& count,
    int64_t numel,
    int64_t stride,
    int mode_mean,
    const Tensor& bag_size,
    const Tensor& per_sample_weights,
    const Tensor& segment_offsets,
    int64_t num_of_segments,
    const Tensor& grad_weight_per_segment) {
  auto& queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();

  int64_t work_group_size = dpcppMaxWorkGroupSize(dev_id);
  int64_t stride_warped = CeilDiv(stride, work_group_size) * work_group_size;
  int64_t group_size = std::min(stride_warped, dpcppMaxWorkGroupSize(dev_id));
  auto num_groups = CeilDiv(num_of_segments * stride_warped, group_size);
  auto total_items = num_groups * group_size;

  bool per_sample_weight_defined = per_sample_weights.defined();
  bool count_defined = count.defined();
  int64_t per_sample_weights_stride =
      per_sample_weights.defined() ? per_sample_weights.stride(0) : 0;

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto grad_weight_per_segment_data =
        grad_weight_per_segment.data_ptr<acc_type<scalar_t>>();
    auto indices_data = indices.data_ptr<int64_t>();
    auto gradOutput_data = gradOutput.data_ptr<scalar_t>();
    auto offset2bag_data = offset2bag.data_ptr<int64_t>();
    auto count_data = count_defined
        ? count.data_ptr<int64_t>()
        : offset2bag_data; // use the offset2bag_data handler as the dummy
                           // buffer.
    auto bag_size_data = bag_size.data_ptr<int64_t>();
    auto per_sample_weights_data = per_sample_weight_defined
        ? per_sample_weights.data_ptr<scalar_t>()
        : gradOutput_data; // ise the gradOutput_data handler as the dummy
                           // buffer.
    auto segment_offsets_data = segment_offsets.data_ptr<int64_t>();

    auto kfn = DPCPP_Q_KFN(DPCPP::nd_item<1> item) {
      auto grad_weight_per_segment_ptr = grad_weight_per_segment_data;
      auto indices_ptr = indices_data;
      auto gradOutput_ptr = gradOutput_data;
      auto offset2bag_ptr = offset2bag_data;
      auto count_ptr = count_defined ? count_data : NULL;
      auto bag_size_ptr = bag_size_data;
      auto per_sample_weights_ptr =
          per_sample_weight_defined ? per_sample_weights_data : NULL;
      auto segment_offsets_ptr = segment_offsets_data;

      const int gid = item.get_global_linear_id();
      const int id = gid / stride_warped;
      const int startFeature = gid % stride_warped;
      if (startFeature >= stride) {
        return;
      }
      if (id >= num_of_segments) {
        return;
      }

      const int idx_begin = segment_offsets_ptr[id];
      const int idx_end =
          (id == num_of_segments - 1) ? numel : segment_offsets_ptr[id + 1];

      acc_type<scalar_t> weight = 0;
      for (int idx = idx_begin; idx < idx_end; ++idx) {
        const int orig_row = indices_ptr[idx];
        const int seq_number = offset2bag_ptr[orig_row];
        const int grad_output_row = seq_number * stride;

        acc_type<scalar_t> scale = count_ptr ? 1.0 / count_ptr[idx] : 1.0;
        if (per_sample_weight_defined) {
          scale *= per_sample_weights_ptr[idx * per_sample_weights_stride];
        }

        acc_type<scalar_t> gradient =
            gradOutput_ptr[grad_output_row + startFeature];
        if (mode_mean) {
          gradient /= bag_size_ptr[seq_number];
        }
        weight += gradient * scale;
      }
      grad_weight_per_segment_ptr[id * stride + startFeature] = weight;
    };

    // kick off kernel
    cgh.parallel_for(
        DPCPP::nd_range<1>(
            DPCPP::range<1>(total_items), DPCPP::range<1>(group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

template <typename scalar_t>
void compute_grad_weight(
    const Tensor& indices,
    const Tensor& grad_output,
    const Tensor& count,
    ptrdiff_t numel,
    int64_t stride,
    const Tensor& segment_offsets,
    int64_t num_of_segments,
    const Tensor& grad_weight_per_segment) {
  auto& queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();

  int64_t work_group_size = dpcppMaxWorkGroupSize(dev_id);
  int64_t stride_warped = CeilDiv(stride, work_group_size) * work_group_size;
  int64_t group_size = std::min(stride_warped, dpcppMaxWorkGroupSize(dev_id));
  auto num_groups = CeilDiv(num_of_segments * stride_warped, group_size);
  auto total_items = num_groups * group_size;

  bool count_defined = count.defined();

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto grad_weight_per_segment_data =
        grad_weight_per_segment.data_ptr<acc_type<scalar_t>>();
    auto indices_data = indices.data_ptr<int64_t>();
    auto grad_output_data = grad_output.data_ptr<scalar_t>();
    auto count_data = count_defined
        ? count.data_ptr<int64_t>()
        : indices_data; // use the indices_data handler as the dummy buffer.
    auto segment_offsets_data = segment_offsets.data_ptr<int64_t>();

    auto kfn = DPCPP_Q_KFN(DPCPP::nd_item<1> item) {
      auto grad_weight_per_segment_ptr = grad_weight_per_segment_data;
      auto indices_ptr = indices_data;
      auto grad_output_ptr = grad_output_data;
      auto count_ptr = count_defined ? count_data : NULL;
      auto segment_offsets_ptr = segment_offsets_data;

      const int gid = item.get_global_linear_id();
      const int id = gid / stride_warped;
      const int startFeature = gid % stride_warped;
      if (startFeature >= stride) {
        return;
      }
      if (id >= num_of_segments) {
        return;
      }
      const int idx_begin = segment_offsets_ptr[id];
      const int idx_end =
          (id == num_of_segments - 1) ? numel : segment_offsets_ptr[id + 1];

      acc_type<scalar_t> weight = 0;
      for (int idx = idx_begin; idx < idx_end; idx++) {
        const int64_t target_row = indices_ptr[idx];
        const acc_type<scalar_t> scale = count_ptr ? 1.0 / count_ptr[idx] : 1.0;
        weight += grad_output_ptr[target_row * stride + startFeature] * scale;
      }
      grad_weight_per_segment_ptr[id * stride + startFeature] = weight;
    };

    // kick off kernel
    cgh.parallel_for(
        DPCPP::nd_range<1>(
            DPCPP::range<1>(total_items), DPCPP::range<1>(group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

template <typename scalar_t>
void sum_and_scatter(
    const Tensor& input,
    const Tensor& grad_weight,
    int64_t stride,
    const Tensor& segment_offsets,
    int64_t num_of_segments,
    const Tensor& grad_weight_per_segment,
    const Tensor& segment_sizes_offsets,
    int64_t num_of_partial_segments,
    const int64_t padding_idx) {
  auto& queue = dpcppGetCurrentQueue();
  auto dev_id = dpcppGetDeviceIdOfCurrentQueue();

  int64_t work_group_size = dpcppMaxWorkGroupSize(dev_id);
  int64_t stride_warped = CeilDiv(stride, work_group_size) * work_group_size;
  int64_t group_size = std::min(stride_warped, dpcppMaxWorkGroupSize(dev_id));
  auto num_groups = CeilDiv(num_of_segments * stride_warped, group_size);
  auto total_items = num_groups * group_size;

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto grad_weight_data = grad_weight.data_ptr<scalar_t>();
    auto input_data = input.data_ptr<int64_t>();
    auto segment_offsets_data = segment_offsets.data_ptr<int64_t>();
    auto grad_weight_per_segment_data =
        grad_weight_per_segment.data_ptr<acc_type<scalar_t>>();
    auto segment_sizes_offsets_data = segment_sizes_offsets.data_ptr<int64_t>();

    auto kfn = DPCPP_Q_KFN(DPCPP::nd_item<1> item) {
      auto grad_weight_ptr = grad_weight_data;
      auto input_ptr = input_data;
      auto segment_offsets_ptr = segment_offsets_data;
      auto grad_weight_per_segment_ptr = grad_weight_per_segment_data;
      auto segment_sizes_offsets_ptr = segment_sizes_offsets_data;

      const int gid = item.get_global_linear_id();
      const int id = gid / stride_warped;
      const int startFeature = gid % stride_warped;
      if (startFeature >= stride) {
        return;
      }
      if (id >= num_of_segments) {
        return;
      }

      const int idx_begin = segment_sizes_offsets_ptr[id];
      const int idx_end = (id == num_of_segments - 1)
          ? num_of_partial_segments
          : segment_sizes_offsets_ptr[id + 1];
      acc_type<scalar_t> weight = 0;
      for (int idx = idx_begin; idx < idx_end; idx++) {
        weight += grad_weight_per_segment_ptr[idx * stride + startFeature];
      }

      int64_t target_row = input_ptr[segment_offsets_ptr[id]];
      if (target_row != padding_idx) {
        grad_weight_ptr[target_row * stride + startFeature] = weight;
      }
    };

    // kick off kernel
    cgh.parallel_for(
        DPCPP::nd_range<1>(
            DPCPP::range<1>(total_items), DPCPP::range<1>(group_size)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

Tensor embedding_bag_backward_dpcpp_kernel(
    const Tensor& grad,
    const Tensor& orig_indices,
    const Tensor& sorted_indices,
    const Tensor& count,
    int64_t num_weights,
    int padding_idx,
    bool scale_grad_by_freq,
    bool mode_mean,
    const Tensor& offset2bag,
    const Tensor& bag_size,
    const Tensor& per_sample_weights) {
#ifndef USE_ONEDPL
  throw std::runtime_error(
      "no oneDPL found when compile. USM embedding not supported");
#else
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  auto policy = oneapi::dpl::execution::make_device_policy(dpcpp_queue);
  const int64_t numel = sorted_indices.numel();
  auto grad_weight = at::zeros({num_weights, grad.size(-1)}, grad.options());
  const int64_t stride = grad_weight.stride(0);

  auto segment_offsets = at::empty({numel}, orig_indices.options());
  int64_t num_of_segments;
  {
    // sorted:          2 5 5 5 7 7 8 9 9
    // dummy:           1 1 0 0 1 0 1 1 0
    // segment_offsets: 0 1 - - 4 - 6 7 -
    auto sorted_indices_begin = sorted_indices.data_ptr<int64_t>();
    auto dummy = at::empty_like(sorted_indices);
    auto dummy_begin = dummy.data_ptr<int64_t>();
    auto idx_tensor = at::empty_like(sorted_indices);
    auto idx_begin = idx_tensor.data_ptr<int64_t>();
    std::adjacent_difference(
        policy,
        sorted_indices_begin,
        sorted_indices_begin + numel,
        dummy_begin,
        [](auto lhs, auto rhs) -> bool {
          if (lhs != rhs) {
            return true;
          }
          return false;
        });

    // For algorithm adjacent difference, for output, its first element is
    // always equal to source first element. We need to set it as 1 manually.
    dummy[0] = 1;
    Tensor count_tensor =
        at::empty({numel}, at::TensorOptions().device(kXPU).dtype(kLong));
    auto count_begin = count_tensor.data_ptr<int64_t>();
    at::AtenIpexTypeXPU::iota(count_begin, count_begin + numel, (int64_t)0);
    auto segment_offsets_begin = segment_offsets.data_ptr<int64_t>();
    at::AtenIpexTypeXPU::transform(
        dummy_begin,
        dummy_begin + numel,
        count_begin,
        idx_begin,
        [](auto d, auto idx) { return d ? idx : -1; });
    auto ends = at::AtenIpexTypeXPU::copy_if(
        idx_begin, idx_begin + numel, segment_offsets_begin, [](auto x) {
          return x != -1;
        });
    num_of_segments = std::distance(segment_offsets_begin, ends);
  }

  auto partials_per_segment =
      at::empty({num_of_segments}, orig_indices.options());

  krn_partials_per_segment(
      partials_per_segment.data_ptr<int64_t>(),
      segment_offsets.data_ptr<int64_t>(),
      num_of_segments,
      numel);

  // In order to compute `partial_segment_offset`, which is the start index
  // of each partial-segment in `sorted_indices`, we need to compute the
  // start position of each _segment_ in `partial_segment_offset`.
  // Unit: index in `partial_segment_offset`
  auto partials_per_segment_offset =
      at::empty({num_of_segments}, orig_indices.options());
  at::AtenIpexTypeXPU::exclusive_scan(
      partials_per_segment.data_ptr<int64_t>(),
      partials_per_segment.data_ptr<int64_t>() + num_of_segments,
      partials_per_segment_offset.data_ptr<int64_t>(),
      0);

  // The total number of partial-segments is the sum of
  // `partials_per_segment_offset`
  const int num_of_partial_segments =
      partials_per_segment[num_of_segments - 1].item<int64_t>() +
      partials_per_segment_offset[num_of_segments - 1].item<int64_t>();

  auto partial_segment_offset =
      at::empty({num_of_partial_segments}, orig_indices.options());
  krn_partial_segment_offset(
      partial_segment_offset.data_ptr<int64_t>(),
      partials_per_segment.data_ptr<int64_t>(),
      partials_per_segment_offset.data_ptr<int64_t>(),
      segment_offsets.data_ptr<int64_t>(),
      num_of_segments);

  IPEX_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      grad.scalar_type(),
      "embedding_bag_backward_dpcpp_compute_grad_weight",
      [&] {
        TensorOptions op;
        if (grad.dtype() == at::kBFloat16) {
          op = grad.options().dtype(at::kFloat);
        } else {
          op = grad.options();
        }
        auto grad_weight_per_segment =
            at::empty({num_of_partial_segments, stride}, op);
        // Compute the sum of each partial-segment and handle bags
        if (offset2bag.defined()) {
          compute_grad_weight_bags<scalar_t>(
              orig_indices,
              grad,
              offset2bag,
              count,
              numel,
              stride,
              mode_mean,
              bag_size,
              per_sample_weights,
              partial_segment_offset,
              num_of_partial_segments,
              grad_weight_per_segment);
        } else {
          compute_grad_weight<scalar_t>(
              orig_indices,
              grad,
              count,
              numel,
              stride,
              partial_segment_offset,
              num_of_partial_segments,
              grad_weight_per_segment);
        }

        sum_and_scatter<scalar_t>(
            sorted_indices,
            grad_weight,
            stride,
            segment_offsets,
            num_of_segments,
            grad_weight_per_segment,
            partials_per_segment_offset,
            num_of_partial_segments,
            padding_idx);
      });

  return grad_weight;
#endif
}

// This kernel assumes that all input tensors except `weight` and
// per_sample_weights are contiguous.
template <typename scalar_t>
void EmbeddingBag_updateOutputKernel(
    int64_t* input,
    int64_t* offsets,
    scalar_t* weight,
    scalar_t* output,
    int64_t* offset2bag,
    int64_t numIndices,
    int64_t numBags,
    int64_t featureSize,
    int64_t weight_stide0,
    int64_t weight_stride1,
    int mode,
    int64_t* bag_size,
    int64_t* max_indices,
    scalar_t* per_sample_weights,
    int64_t per_sample_weights_stride) {
  // the strategy here is that each bag x feature is handled by a single thread

  using accscalar_t = acc_type<scalar_t>;
  auto& queue = dpcppGetCurrentQueue();
  auto workersPerChunk = [featureSize]() -> int64_t {
    int64_t _workersPerChunk = 64;
    if (featureSize < 64 && featureSize >= 32) {
      _workersPerChunk = 32;
    } else if (featureSize < 32) {
      _workersPerChunk = 16;
    }
    return _workersPerChunk;
  }();
  int64_t chunksPerBag = CeilDiv(featureSize, (int64_t)workersPerChunk);
  int64_t numChunks = numBags * chunksPerBag;
  int64_t kernel_range = 1024 * workersPerChunk;
  int64_t chunksPerWorkGroup = 256 / workersPerChunk;
  bool per_sample_weights_defined = per_sample_weights ? true : false;

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto input_data = input;
    auto offsets_data = offsets;
    auto weight_data = weight;
    auto output_data = output;
    auto offset2bag_data = offset2bag;
    auto bag_size_data = bag_size;
    // use the weight handler as the dummy handler.
    // The kernel would not access the data thru the per_sample_weights_ptr in
    // false case
    auto per_sample_weights_data =
        per_sample_weights_defined ? per_sample_weights : weight_data;
    // use the offset2bag handler as the dummy handler.
    // The kernel would not access the data thru the max_indices_ptr in false
    // case
    auto max_indices_data = mode == MODE_MAX ? max_indices : offset2bag_data;

    auto kfn = DPCPP_Q_KFN(DPCPP::nd_item<2> item) {
      auto input_ptr = input_data;
      auto offsets_ptr = offsets_data;
      auto weight_ptr = weight_data;
      auto output_ptr = output_data;
      auto offset2bag_ptr = offset2bag_data;
      auto bag_size_ptr = bag_size_data;
      auto per_sample_weights_ptr = per_sample_weights_data;
      auto max_indices_ptr = max_indices_data;

      int64_t chunkOffset = item.get_group()[0] * item.get_local_range()[1] +
          item.get_local_id()[1];

      for (int64_t chunk = chunkOffset; chunk < numChunks;
           chunk += item.get_group_range()[0] * item.get_global_range()[1]) {
        int64_t featureDim = (chunk % chunksPerBag) * item.get_local_range(0) +
            item.get_local_id(0);
        if (featureDim < featureSize) {
          int64_t bag = chunk / chunksPerBag;
          auto weightFeat = weight_ptr + featureDim * weight_stride1;
          int64_t begin = offsets_ptr[bag];
          int64_t end =
              (bag < numBags - 1) ? (offsets_ptr[bag + 1]) : numIndices;

          accscalar_t weightFeatSum = 0;
          scalar_t weightFeatMax;

          int64_t bag_size_ = 0;
          int64_t maxWord = -1;
          for (int64_t emb = begin; emb < end; emb++) {
            const int64_t weightRow = input_ptr[emb] * weight_stide0;
            scalar_t weightValue = weightFeat[weightRow];

            if (mode == MODE_MAX) {
              if (emb == begin || weightValue > weightFeatMax) {
                weightFeatMax = weightValue;
                maxWord = input_ptr[emb];
              }
            } else {
              if (per_sample_weights_defined) {
                accscalar_t scaleWeightBy = static_cast<accscalar_t>(
                    per_sample_weights_ptr[emb * per_sample_weights_stride]);
                weightFeatSum +=
                    scaleWeightBy * static_cast<accscalar_t>(weightValue);
              } else {
                weightFeatSum += static_cast<accscalar_t>(weightValue);
              }
            }

            bag_size_++;
            if (featureDim == 0) {
              offset2bag_ptr[emb] = bag;
            }
          }
          if (mode == MODE_MEAN) {
            if (end == begin) {
              bag_size_ptr[bag] = 0;
            } else {
              weightFeatSum =
                  weightFeatSum / static_cast<accscalar_t>(bag_size_);
              bag_size_ptr[bag] = bag_size_;
            }
          }

          if (mode == MODE_MEAN || mode == MODE_SUM) {
            output_ptr[bag * featureSize + featureDim] =
                static_cast<scalar_t>(weightFeatSum);
          } else if (mode == MODE_MAX) {
            if (end == begin) {
              // If bag is empty, set output to 0.
              weightFeatMax = 0;
            }
            max_indices_ptr[bag * featureSize + featureDim] = maxWord;
            output_ptr[bag * featureSize + featureDim] = weightFeatMax;
          }
        }
      }
    };

    // kick off kernel
    cgh.parallel_for(
        DPCPP::nd_range<2>(
            DPCPP::range<2>(kernel_range, chunksPerWorkGroup),
            DPCPP::range<2>(workersPerChunk, chunksPerWorkGroup)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

Tensor embedding_bag_backward_dpcpp_sum_avg(
    const Tensor& grad,
    const Tensor& indices,
    const Tensor& offsets,
    const Tensor& offset2bag,
    const Tensor& bag_size,
    int64_t num_weights,
    bool scale_grad_by_freq,
    int64_t mode,
    const Tensor& per_sample_weights) {
#ifndef USE_ONEDPL
  throw std::runtime_error(
      "no oneDPL found when compile. USM embedding not supported");
#else
  auto grad_weight = at::zeros({num_weights, grad.size(1)}, grad.options());

  ptrdiff_t numel = indices.numel();

  if (numel == 0) {
    // all empty bags
    return at::zeros({num_weights, grad.size(1)}, grad.options());
  }

  int64_t stride = grad_weight.stride(0);

  auto sorted_indices = at::empty_like(indices);
  auto orig_indices = at::empty_like(indices);

  auto& dpcpp_queue = dpcppGetCurrentQueue();
  auto policy = oneapi::dpl::execution::make_device_policy(dpcpp_queue);
  // directly
  {
    sorted_indices.copy_(indices);

    auto count_begin = oneapi::dpl::counting_iterator<int64_t>(0);
    auto orig_begin = orig_indices.data_ptr<int64_t>();
    std::copy(policy, count_begin, count_begin + numel, orig_begin);

    auto sorted_begin = sorted_indices.data_ptr<int64_t>();
    auto zipped_begin =
        oneapi::dpl::make_zip_iterator(sorted_begin, orig_begin);
    std::sort(
        policy, zipped_begin, zipped_begin + numel, [](auto lhs, auto rhs) {
          using std::get;
          return get<0>(lhs) < get<0>(rhs);
        });
  }

  Tensor count;
  if (scale_grad_by_freq) {
    count = at::empty_like(indices);
    count.fill_(1);

    // Compute an increasing sequence per unique item in sortedIndices:
    // sorted: 2 5 5 5 7 7 8 9 9
    //  count: 1 1 2 3 1 2 1 1 2
    auto sorted_begin = sorted_indices.data_ptr<int64_t>();
    auto count_begin = count.data_ptr<int64_t>();
    oneapi::dpl::inclusive_scan_by_segment(
        policy, sorted_begin, sorted_begin + numel, count_begin, count_begin);

    // Take the maximum of each count per unique key in reverse:
    // sorted: 2 5 5 5 7 7 8 9 9
    //  count: 1 3 3 3 2 2 1 2 2
    auto revers_sorted_begin = std::make_reverse_iterator(sorted_begin + numel);
    auto revers_count_begin = std::make_reverse_iterator(count_begin + numel);
    oneapi::dpl::inclusive_scan_by_segment(
        policy,
        revers_sorted_begin,
        revers_sorted_begin + numel,
        revers_count_begin,
        revers_count_begin,
        std::equal_to<int64_t>(),
        oneapi::dpl::maximum<int64_t>());
  }

  return embedding_bag_backward_dpcpp_kernel(
      grad,
      orig_indices,
      sorted_indices,
      count,
      num_weights,
      /* padding_idx= */ -1,
      scale_grad_by_freq,
      mode == MODE_MEAN,
      offset2bag,
      bag_size,
      per_sample_weights);
#endif
}

template <typename scalar_t>
void EmbeddingBag_accGradParametersKernel_max(
    int64_t* max_indices,
    scalar_t* gradOutput,
    scalar_t* gradWeight,
    int64_t stride,
    int64_t numBags) {
  auto& queue = dpcppGetCurrentQueue();
  int64_t chunksPerBag = CeilDiv(stride, (int64_t)64);
  int64_t numChunks = numBags * chunksPerBag;
  int64_t kernel_range = 1024 * 64;

  auto cgf = DPCPP_Q_CGF(cgh) {
    auto max_indices_data = max_indices;
    auto gradOutput_data = gradOutput;
    auto gradWeight_data = gradWeight;

    auto kfn = DPCPP_Q_KFN(DPCPP::nd_item<2> item) {
      auto max_indices_ptr = max_indices_data;
      auto gradOutput_ptr = gradOutput_data;
      auto gradWeight_ptr = gradWeight_data;

      int64_t chunkOffset = item.get_group()[0] * item.get_local_range()[1] +
          item.get_local_id()[1];

      for (int64_t chunk = chunkOffset; chunk < numChunks;
           chunk += item.get_group_range()[0] * item.get_global_range()[1]) {
        int64_t featureDim = (chunk % chunksPerBag) * item.get_local_range(0) +
            item.get_local_id(0);
        if (featureDim < stride) {
          int64_t bag = chunk / chunksPerBag;

          int64_t word_idx = max_indices_ptr[bag * stride + featureDim];
          if (word_idx >= 0) {
            // If bag is empty, we have max_indices[idx] set to -1 in forward.
            atomicAdd(
                (dpcpp_global_ptr_pt<scalar_t>)&(
                    gradWeight_ptr[word_idx * stride + featureDim]),
                gradOutput_ptr[bag * stride + featureDim]);
          }
        }
      }
    };

    // kick off kernel
    cgh.parallel_for(
        DPCPP::nd_range<2>(
            DPCPP::range<2>(kernel_range, 4), DPCPP::range<2>(64, 4)),
        kfn);
  };
  DPCPP_Q_SUBMIT(queue, cgf);
}

Tensor embedding_bag_backward_dpcpp_max(
    const Tensor& grad,
    const Tensor& max_indices,
    int64_t num_weights) {
  auto grad_weight = at::zeros({num_weights, grad.size(1)}, grad.options());

  int64_t stride = grad_weight.stride(0);

  int64_t numBags = grad.size(0);

  // for atomicAdd, only support float datatype.
  EmbeddingBag_accGradParametersKernel_max<float>(
      max_indices.data_ptr<int64_t>(),
      grad.data_ptr<float>(),
      grad_weight.data_ptr<float>(),
      stride,
      numBags);

  return grad_weight;
}

std::tuple<Tensor, Tensor, Tensor, Tensor> _embedding_bag_dpcpp(
    const Tensor& weight,
    const Tensor& indices,
    const Tensor& offsets,
    const bool scale_grad_by_freq,
    const int64_t mode,
    bool sparse,
    const Tensor& per_sample_weights) {
  auto indices_arg = TensorArg(indices, "indices", 1);
  checkScalarType("embedding_bag_dpcpp", indices_arg, kLong);
  auto offsets_arg = TensorArg(offsets, "offsets", 1);
  checkScalarType("embedding_bag_dpcpp", offsets_arg, kLong);
  auto weight_arg = TensorArg(weight, "weight", 1);
  IsOnSameDevice("embedding_bag_dpcpp", weight_arg, indices_arg);
  IsOnSameDevice("embedding_bag_dpcpp", weight_arg, offsets_arg);

  int64_t numIndices = indices.size(0);
  int64_t numBags = offsets.size(0);
  int64_t featureSize = weight.size(1);

  auto bag_size = at::empty(offsets.sizes(), indices.options());
  auto offset2bag = at::empty({indices.size(0)}, indices.options());
  auto output = at::empty({offsets.size(0), weight.size(1)}, weight.options());
  Tensor max_indices;
  if (MODE_MAX)
    max_indices =
        at::empty({offsets.size(0), weight.size(1)}, indices.options());

  IPEX_DISPATCH_FLOATING_TYPES_AND2(
      at::ScalarType::Half,
      at::ScalarType::BFloat16,
      weight.scalar_type(),
      "embedding_bag_dpcpp",
      [&] {
        EmbeddingBag_updateOutputKernel<scalar_t>(
            indices.data_ptr<int64_t>(),
            offsets.data_ptr<int64_t>(),
            weight.data_ptr<scalar_t>(),
            output.data_ptr<scalar_t>(),
            offset2bag.data_ptr<int64_t>(),
            numIndices,
            numBags,
            featureSize,
            weight.stride(0),
            weight.stride(1),
            mode,
            bag_size.data_ptr<int64_t>(),
            mode == MODE_MAX ? max_indices.data_ptr<int64_t>() : NULL,
            per_sample_weights.defined()
                ? per_sample_weights.data_ptr<scalar_t>()
                : NULL,
            per_sample_weights.defined() ? per_sample_weights.stride(0) : 0);
      });

  return std::tuple<Tensor, Tensor, Tensor, Tensor>(
      output, offset2bag, bag_size, max_indices);
}

Tensor _embedding_bag_dense_backward_dpcpp(
    const Tensor& grad_,
    const Tensor& indices,
    const Tensor& offsets,
    const Tensor& offset2bag,
    const Tensor& bag_size_,
    const Tensor& max_indices,
    int64_t num_weights,
    bool scale_grad_by_freq,
    int64_t mode,
    const Tensor& per_sample_weights) {
  Tensor grad = grad_.contiguous();

  switch (mode) {
    case MODE_SUM:
    case MODE_MEAN:
      if (mode == MODE_MEAN)
        TORCH_INTERNAL_ASSERT(!per_sample_weights.defined());
      return embedding_bag_backward_dpcpp_sum_avg(
          grad,
          indices,
          offsets,
          offset2bag,
          bag_size_,
          num_weights,
          scale_grad_by_freq,
          mode,
          per_sample_weights);

    case MODE_MAX:
      TORCH_INTERNAL_ASSERT(!per_sample_weights.defined());
      return embedding_bag_backward_dpcpp_max(grad, max_indices, num_weights);

    default:
      TORCH_CHECK(0, "Unknown mode for embedding_bag_backward_dpcpp ", mode);
  }
}

} // namespace impl

std::tuple<Tensor, Tensor, Tensor, Tensor> _embedding_bag(
    const Tensor& weight,
    const Tensor& indices,
    const Tensor& offsets,
    bool scale_grad_by_freq,
    int64_t mode,
    bool sparse,
    const Tensor& per_sample_weights,
    bool include_last_offset) {
  // TODO: include_last_offset
  return impl::_embedding_bag_dpcpp(
      weight,
      indices,
      offsets,
      scale_grad_by_freq,
      mode,
      sparse,
      per_sample_weights);
}

Tensor _embedding_bag_dense_backward(
    const Tensor& grad,
    const Tensor& indices,
    const Tensor& offsets,
    const Tensor& offset2bag,
    const Tensor& bag_size,
    const Tensor& maximum_indices,
    int64_t num_weights,
    bool scale_grad_by_freq,
    int64_t mode,
    const Tensor& per_sample_weights) {
  return impl::_embedding_bag_dense_backward_dpcpp(
      grad,
      indices,
      offsets,
      offset2bag,
      bag_size,
      maximum_indices,
      num_weights,
      scale_grad_by_freq,
      mode,
      per_sample_weights);
}

} // namespace AtenIpexTypeXPU
} // namespace at
