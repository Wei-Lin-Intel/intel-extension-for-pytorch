#include <ATen/Context.h>
#include <ATen/WrapDimUtils.h>
#include <ATen/WrapDimUtilsMulti.h>
#include <ATen/core/DimVector.h>
#include <ATen/native/ReduceOps.h>
#include <ATen/native/ReduceOpsUtils.h>
#include <ATen/native/SharedReduceOps.h>
#include <ATen/native/TensorIterator.h>

#include <c10/core/ScalarType.h>
#include "comm/ATDispatch.h"
#include "comm/AccumulateType.h"
#include "comm/Numerics.h"
#include "comm/RegistrationDeclarations.h"

#include "Reduce.h"
#include "ReduceOpsUtils.h"

using namespace xpu::dpcpp;
using namespace at::native;

namespace at {
namespace AtenIpexTypeXPU {

template <typename scalar_t, typename acc_t>
struct ReduceAndOps {
  ReduceAndOps() {}
  acc_t operator()(scalar_t a, scalar_t b) const {
    return static_cast<acc_t>(a) && static_cast<acc_t>(b);
  }
};

template <typename scalar_t>
void and_kernel(TensorIterator& iter) {
  dpcpp_reduce_kernel<scalar_t, bool>(
      iter, func_wrapper<bool>(ReduceAndOps<scalar_t, bool>()), true);
}

template <typename scalar_t, typename acc_t>
struct ReduceOrOps {
  ReduceOrOps() {}
  acc_t operator()(scalar_t a, scalar_t b) const {
    return static_cast<acc_t>(a) || static_cast<acc_t>(b);
  }
};

template <typename scalar_t>
void or_kernel(TensorIterator& iter) {
  dpcpp_reduce_kernel<scalar_t, bool>(
      iter, func_wrapper<bool>(ReduceOrOps<scalar_t, bool>()), false);
}

inline Tensor& _all(Tensor& result, TensorIterator& iter) {
  if (iter.numel() == 0) {
    result.fill_(1);
  } else {
    IPEX_DISPATCH_ALL_TYPES_AND_COMPLEX_AND3(
        at::ScalarType::Half,
        at::ScalarType::BFloat16,
        at::ScalarType::Bool,
        iter.dtype(),
        "all",
        [&]() { and_kernel<scalar_t>(iter); });
  }

  return result;
}

Tensor all(const at::Tensor& self) {
  Tensor result;
  auto result_type = get_result_or_bytebool_dtype(self, result);
  result = at::empty({0}, self.options().dtype(result_type));
  auto iter = meta::make_reduction(
      "all", result, self, {}, false, self.scalar_type(), result.scalar_type());

  return at::AtenIpexTypeXPU::_all(result, iter);
}

Tensor& all_out(Tensor& result, const Tensor& self, int64_t dim, bool keepdim) {
  check_result_is_bytebool("all", self, result);
  dim = maybe_wrap_dim(dim, self.dim());
  if (_dimreduce_return_trivial(result, self, 1, dim, keepdim)) {
    return result;
  } else {
    auto iter = meta::make_reduction(
        "all",
        result,
        self,
        dim,
        keepdim,
        self.scalar_type(),
        result.scalar_type());
    return at::AtenIpexTypeXPU::_all(result, iter);
  }
}

Tensor all(const Tensor& self, int64_t dim, bool keepdim) {
  Tensor result;
  auto result_type = get_result_or_bytebool_dtype(self, result);
  result = at::empty({0}, self.options().dtype(result_type));
  return at::AtenIpexTypeXPU::all_out(result, self, dim, keepdim);
}

inline Tensor& _any(Tensor& result, TensorIterator& iter) {
  if (iter.numel() == 0) {
    result.fill_(0);
  } else {
    IPEX_DISPATCH_ALL_TYPES_AND_COMPLEX_AND3(
        at::ScalarType::Half,
        at::ScalarType::BFloat16,
        at::ScalarType::Bool,
        iter.dtype(),
        "any",
        [&]() { or_kernel<scalar_t>(iter); });
  }

  return result;
}

Tensor any(const at::Tensor& self) {
  Tensor result;
  auto result_type = get_result_or_bytebool_dtype(self, result);
  result = at::empty({0}, self.options().dtype(result_type));
  auto iter = meta::make_reduction(
      "any", result, self, {}, false, self.scalar_type(), result.scalar_type());

  return at::AtenIpexTypeXPU::_any(result, iter);
}

Tensor& any_out(Tensor& result, const Tensor& self, int64_t dim, bool keepdim) {
  check_result_is_bytebool("any", self, result);
  dim = maybe_wrap_dim(dim, self.dim());
  if (_dimreduce_return_trivial(result, self, 0, dim, keepdim)) {
    return result;
  } else {
    auto iter = meta::make_reduction(
        "any",
        result,
        self,
        dim,
        keepdim,
        self.scalar_type(),
        result.scalar_type());
    return at::AtenIpexTypeXPU::_any(result, iter);
  }
}

Tensor any(const Tensor& self, int64_t dim, bool keepdim) {
  Tensor result;
  auto result_type = get_result_or_bytebool_dtype(self, result);
  result = at::empty({0}, self.options().dtype(result_type));
  return at::AtenIpexTypeXPU::any_out(result, self, dim, keepdim);
}

} // namespace AtenIpexTypeXPU
} // namespace at
