#pragma once

#include "ocl/ocl-config.h"

#include <rtt/base/DataSourceBase.hpp>

#include <cstddef>
#include <string>

namespace OCL::detail {

struct StructuredValueRenderOptions {
  // Portable lower bound for the " = " prefix and the shortest explicit
  // omission marker with its closing delimiter: "{... output omitted}".
  static constexpr std::size_t minimum_max_result_bytes = 23U;

  std::size_t compact_width{100};
  std::size_t sequence_items{3};
  std::size_t max_structural_depth{3};
  std::size_t structure_members{20};
  std::size_t max_result_bytes{4096};
  std::size_t indentation{2};
  bool hexadecimal{false};
  bool sequence_indices{false};
};

enum class StructuredValueRenderStatus {
  rendered,
  evaluation_failed,
};

struct StructuredValueRenderResult {
  StructuredValueRenderStatus status;
  std::string text;
};

OCL_API StructuredValueRenderResult renderStructuredValue(
    RTT::base::DataSourceBase::shared_ptr source,
    const StructuredValueRenderOptions &options = {});

OCL_API std::string renderStructuredSnapshotForTest(
    RTT::base::DataSourceBase::shared_ptr snapshot,
    const StructuredValueRenderOptions &options = {});

} // namespace OCL::detail
