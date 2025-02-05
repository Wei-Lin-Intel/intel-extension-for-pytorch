#pragma once

namespace torch_ipex {
namespace cpu {
namespace kernel {

template <typename dst_type, typename src_type>
inline __attribute__((always_inline)) void move_ker(
    dst_type* inout,
    const src_type* in,
    int64_t len) {
#pragma omp simd
  for (int64_t i = 0; i < len; i++) {
    *(inout + i) = *(in + i);
  }
}

} // namespace kernel
} // namespace cpu
} // namespace torch_ipex
