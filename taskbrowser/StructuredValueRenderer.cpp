#include "internal/StructuredValueRenderer.hpp"

#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/types/TypeInfo.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace {

using DataSourcePtr = RTT::base::DataSourceBase::shared_ptr;

struct RenderNode {
  enum class Kind { scalar, structure, sequence, unavailable };
  Kind kind{Kind::scalar};
  std::string scalar;
  std::string type_name;
  std::vector<std::pair<std::string, RenderNode>> children;
  std::size_t omitted{0};
  bool collapsed{false};
};

enum class SnapshotStatus { ready, unavailable, evaluation_failed };

struct SnapshotResult {
  SnapshotStatus status;
  DataSourcePtr value;
};

SnapshotResult snapshot(const DataSourcePtr &source) {
  if (!source || source->getTypeInfo() == nullptr) {
    return {SnapshotStatus::evaluation_failed, {}};
  }

  source->reset();
  DataSourcePtr local = source->getTypeInfo()->buildValue();
  if (!local || !local->isAssignable()) {
    return {SnapshotStatus::unavailable, {}};
  }
  if (!local->update(source.get())) {
    return {SnapshotStatus::evaluation_failed, {}};
  }
  return {SnapshotStatus::ready, std::move(local)};
}

template <typename T>
std::optional<std::string> floatingText(const DataSourcePtr &source) {
  auto typed = boost::dynamic_pointer_cast<RTT::internal::DataSource<T>>(source);
  if (!typed) {
    return std::nullopt;
  }

  const T value = typed->value();
  std::ostringstream stream;
  stream << value;
  std::string text = stream.str();
  if (std::isfinite(value) && std::floor(value) == value &&
      text.find('.') == std::string::npos) {
    const std::size_t exponent = text.find_first_of("eE");
    text.insert(exponent == std::string::npos ? text.size() : exponent, ".0");
  }
  return text;
}

std::string scalarText(const DataSourcePtr &source, bool hexadecimal) {
  if (auto text = floatingText<float>(source)) {
    return *text;
  }
  if (auto text = floatingText<double>(source)) {
    return *text;
  }

  std::ostringstream stream;
  stream << (hexadecimal ? std::hex : std::dec) << source;
  return stream.str();
}

RenderNode unavailableNode() {
  RenderNode node;
  node.kind = RenderNode::Kind::unavailable;
  return node;
}

bool hasIndexedMemberSemantics(const DataSourcePtr &source) {
  // RTT's numeric-name lookup is the safe capability probe. Sequence member
  // factories return a lazy indexed data source even when the sequence is
  // empty; ordinary structure factories return null for an absent "0" field.
  return static_cast<bool>(source->getMember("0"));
}

RenderNode captureNode(const DataSourcePtr &source, std::size_t structural_depth,
                       const OCL::detail::StructuredValueRenderOptions &options) {
  if (!source || source->getTypeInfo() == nullptr) {
    return unavailableNode();
  }

  try {
    const auto memberFactory = source->getTypeInfo()->getMemberFactory();
    if (!memberFactory || source->getTypeName() == "String") {
      if (!source->evaluate()) {
        return unavailableNode();
      }
      RenderNode node;
      node.scalar = scalarText(source, options.hexadecimal);
      return node;
    }

    const std::vector<std::string> names = source->getMemberNames();
    const bool has_size = std::find(names.begin(), names.end(), "size") != names.end();
    const bool has_capacity =
        std::find(names.begin(), names.end(), "capacity") != names.end();
    if (has_size && has_capacity && hasIndexedMemberSemantics(source)) {
      const DataSourcePtr size_member = source->getMember("size");
      auto size_source =
          boost::dynamic_pointer_cast<RTT::internal::DataSource<int>>(size_member);
      if (size_source) {
        RenderNode node;
        node.kind = RenderNode::Kind::sequence;
        if (structural_depth > options.max_structural_depth) {
          node.collapsed = true;
          return node;
        }

        const int reported_size = size_source->get();
        const std::size_t size = reported_size > 0 ? static_cast<std::size_t>(reported_size) : 0U;
        const std::size_t captured = std::min(size, options.sequence_items);
        node.children.reserve(captured);
        for (std::size_t index_value = 0; index_value < captured; ++index_value) {
          try {
            auto index = new RTT::internal::ConstantDataSource<int>(
                static_cast<int>(index_value));
            DataSourcePtr element = source->getMember(
                index, RTT::base::DataSourceBase::shared_ptr{});
            node.children.emplace_back(std::to_string(index_value),
                                       captureNode(element, structural_depth + 1U, options));
          } catch (...) {
            node.children.emplace_back(std::to_string(index_value), unavailableNode());
          }
        }
        node.omitted = size - captured;
        return node;
      }
    }

    RenderNode node;
    node.kind = RenderNode::Kind::structure;
    node.type_name = source->getTypeName();
    if (structural_depth > options.max_structural_depth) {
      node.collapsed = true;
      return node;
    }

    const std::size_t captured = std::min(names.size(), options.structure_members);
    node.children.reserve(captured);
    for (std::size_t index = 0; index < captured; ++index) {
      try {
        node.children.emplace_back(names[index], captureNode(source->getMember(names[index]),
                                                             structural_depth + 1U, options));
      } catch (...) {
        node.children.emplace_back(names[index], unavailableNode());
      }
    }
    node.omitted = names.size() - captured;
    return node;
  } catch (...) {
    return unavailableNode();
  }
}

std::string omissionText(const RenderNode &node) {
  return "... " + std::to_string(node.omitted) +
         (node.kind == RenderNode::Kind::sequence ? " items omitted" : " members omitted");
}

std::string childPrefix(const RenderNode &parent,
                        const std::pair<std::string, RenderNode> &child,
                        const OCL::detail::StructuredValueRenderOptions &options) {
  if (parent.kind != RenderNode::Kind::sequence) return child.first + ": ";
  const std::string index = options.sequence_indices ? "[" + child.first + "]: " : "";
  return index + child.second.type_name;
}

std::string childSeparator(bool multiline, bool sequence) {
  return multiline ? (sequence ? ",\n" : "\n") : ", ";
}

std::string renderCompact(const RenderNode &node,
                          const OCL::detail::StructuredValueRenderOptions &options) {
  if (node.kind == RenderNode::Kind::scalar) return node.scalar;
  if (node.kind == RenderNode::Kind::unavailable) return "<unavailable>";
  if (node.collapsed) return node.kind == RenderNode::Kind::sequence ? "[...]" : "{...}";

  const bool sequence = node.kind == RenderNode::Kind::sequence;
  std::string output(sequence ? "[" : "{");
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    if (index != 0U) output += ", ";
    output += childPrefix(node, node.children[index], options);
    output += renderCompact(node.children[index].second, options);
  }
  if (node.omitted != 0U) {
    if (!node.children.empty()) output += ", ";
    output += omissionText(node);
  }
  output += sequence ? "]" : "}";
  return output;
}

std::string indent(std::size_t depth, std::size_t indentation) {
  return std::string(depth * indentation, ' ');
}

std::string renderMultiline(const RenderNode &node, std::size_t depth,
                            const OCL::detail::StructuredValueRenderOptions &options);

std::string renderChild(const RenderNode &parent,
                        const std::pair<std::string, RenderNode> &child,
                        bool multiline, std::size_t depth,
                        const OCL::detail::StructuredValueRenderOptions &options) {
  const std::string compact = renderCompact(child.second, options);
  // Keep small custom elements and nested arrays on one line in a wrapped array.
  if (!multiline ||
      (parent.kind == RenderNode::Kind::sequence &&
       depth * options.indentation + childPrefix(parent, child, options).size() +
               compact.size() + 1U <= options.compact_width)) {
    return compact;
  }
  return renderMultiline(child.second, depth, options);
}

std::string renderMultiline(const RenderNode &node, std::size_t depth,
                            const OCL::detail::StructuredValueRenderOptions &options) {
  if (node.kind == RenderNode::Kind::scalar) return node.scalar;
  if (node.kind == RenderNode::Kind::unavailable) return "<unavailable>";
  if (node.collapsed) return node.kind == RenderNode::Kind::sequence ? "[...]" : "{...}";
  if (node.children.empty() && node.omitted == 0U) return renderCompact(node, options);

  const bool sequence = node.kind == RenderNode::Kind::sequence;
  std::string output(sequence ? "[\n" : "{\n");
  bool first = true;
  for (std::size_t child_index = 0; child_index < node.children.size(); ++child_index) {
    const auto &child = node.children[child_index];
    if (!first) output += childSeparator(true, sequence);
    output += indent(depth + 1U, options.indentation);
    output += childPrefix(node, child, options);
    output += renderChild(node, child, true, depth + 1U, options);
    first = false;
  }
  if (node.omitted != 0U) {
    if (!first) output += childSeparator(true, sequence);
    output += indent(depth + 1U, options.indentation) + omissionText(node);
  }
  output += "\n" + indent(depth, options.indentation) + (sequence ? "]" : "}");
  return output;
}

std::string truncateScalar(const std::string &text, std::size_t budget) {
  if (text.size() <= budget) return text;
  const std::string complete_marker =
      "... " + std::to_string(text.size()) + " bytes omitted";
  if (complete_marker.size() > budget) return "... output omitted";
  std::size_t prefix = budget;
  for (;;) {
    const std::size_t omitted = text.size() - std::min(prefix, text.size());
    const std::string marker = "... " + std::to_string(omitted) + " bytes omitted";
    const std::size_t next = budget > marker.size() ? budget - marker.size() : 0U;
    if (next == prefix) return text.substr(0, prefix) + marker;
    prefix = next;
  }
}

std::string renderBounded(const RenderNode &node, bool multiline, std::size_t depth,
                          const OCL::detail::StructuredValueRenderOptions &options,
                          std::size_t budget) {
  if (node.kind == RenderNode::Kind::scalar) return truncateScalar(node.scalar, budget);
  if (node.kind == RenderNode::Kind::unavailable) return "<unavailable>";
  if (node.collapsed) return node.kind == RenderNode::Kind::sequence ? "[...]" : "{...}";
  if (node.children.empty() && node.omitted == 0U) return renderCompact(node, options);

  const bool sequence = node.kind == RenderNode::Kind::sequence;
  const std::string closing = multiline
      ? "\n" + indent(depth, options.indentation) + (sequence ? "]" : "}")
      : (sequence ? "]" : "}");
  std::string output = multiline ? (sequence ? "[\n" : "{\n") : (sequence ? "[" : "{");
  if (multiline && (!node.children.empty() || node.omitted != 0U) &&
      renderMultiline(node, depth, options).size() > budget) {
    const std::string multiline_marker =
        output + indent(depth + 1U, options.indentation) +
        "... output omitted" + closing;
    const std::string compact_marker = sequence
        ? "[... output omitted]"
        : "{... output omitted}";
    if (multiline_marker.size() > budget && compact_marker.size() <= budget) {
      return compact_marker;
    }
  }
  bool first = true;

  for (std::size_t child_index = 0; child_index < node.children.size(); ++child_index) {
    const auto &child = node.children[child_index];
    const std::string separator = first ? "" : childSeparator(multiline, sequence);
    const std::string prefix =
        (multiline ? indent(depth + 1U, options.indentation) : "") +
        childPrefix(node, child, options);
    const std::string complete = renderChild(node, child, multiline, depth + 1U, options);
    const bool needs_future_omission =
        child_index + 1U < node.children.size() || node.omitted != 0U;
    const std::string future_separator = childSeparator(multiline, sequence);
    const std::string future_prefix = multiline ? indent(depth + 1U, options.indentation) : "";
    const std::size_t future_reserve = needs_future_omission
        ? future_separator.size() + future_prefix.size() +
              std::string("... output omitted").size()
        : 0U;
    if (output.size() + separator.size() + prefix.size() + complete.size() +
            future_reserve + closing.size() <= budget) {
      output += separator + prefix + complete;
      first = false;
      continue;
    }
    if (child.second.kind == RenderNode::Kind::scalar) {
      const std::size_t used = output.size() + separator.size() + prefix.size() + closing.size();
      const std::size_t available = budget > used + future_reserve
          ? budget - used - future_reserve
          : 0U;
      if (used + child.second.scalar.size() <= budget) {
        const std::string marker = "... output omitted";
        const std::string omission_prefix = multiline ? indent(depth + 1U, options.indentation) : "";
        if (output.size() + separator.size() + omission_prefix.size() + marker.size() + closing.size() <= budget) {
          output += separator + omission_prefix + marker;
        }
        return output + closing;
      }
      const std::string abbreviated = truncateScalar(child.second.scalar, available);
      if (used + abbreviated.size() <= budget) {
        output += separator + prefix + abbreviated;
        first = false;
        continue;
      }
    }
    const std::string marker = "... output omitted";
    const std::string omission_prefix = multiline ? indent(depth + 1U, options.indentation) : "";
    if (output.size() + separator.size() + omission_prefix.size() + marker.size() + closing.size() <= budget) {
      output += separator + omission_prefix + marker;
    }
    return output + closing;
  }

  if (node.omitted != 0U) {
    const std::string separator = first ? "" : childSeparator(multiline, sequence);
    const std::string marker = omissionText(node);
    const std::string marker_prefix = multiline ? indent(depth + 1U, options.indentation) : "";
    if (output.size() + separator.size() + marker_prefix.size() + marker.size() + closing.size() <= budget) {
      output += separator + marker_prefix + marker;
    } else {
      const std::string output_marker = "... output omitted";
      if (output.size() + separator.size() + marker_prefix.size() + output_marker.size() + closing.size() <= budget) {
        output += separator + marker_prefix + output_marker;
      }
    }
  }
  return output + closing;
}

std::string renderSnapshot(const DataSourcePtr &snapshot,
                           const OCL::detail::StructuredValueRenderOptions &options) {
  const RenderNode node = captureNode(snapshot, 1U, options);
  const std::string compact = renderCompact(node, options);
  const bool multiline = compact.size() + 3U > options.compact_width;
  const std::size_t budget = options.max_result_bytes > 3U ? options.max_result_bytes - 3U : 0U;
  return renderBounded(node, multiline, 0U, options, budget);
}

} // namespace

namespace OCL::detail {

StructuredValueRenderResult renderStructuredValue(
    DataSourcePtr source, const StructuredValueRenderOptions &options) {
  if (options.max_result_bytes <
      StructuredValueRenderOptions::minimum_max_result_bytes) {
    return {StructuredValueRenderStatus::evaluation_failed, {}};
  }
  try {
    const SnapshotResult local = snapshot(source);
    if (local.status == SnapshotStatus::evaluation_failed) {
      return {StructuredValueRenderStatus::evaluation_failed, {}};
    }
    if (local.status == SnapshotStatus::unavailable) {
      if (!source->evaluate()) {
        return {StructuredValueRenderStatus::evaluation_failed, {}};
      }
      return {StructuredValueRenderStatus::rendered,
              truncateScalar(scalarText(source, options.hexadecimal),
                             options.max_result_bytes > 3U ? options.max_result_bytes - 3U : 0U)};
    }
    return {StructuredValueRenderStatus::rendered, renderSnapshot(local.value, options)};
  } catch (const std::exception &) {
    return {StructuredValueRenderStatus::evaluation_failed, {}};
  } catch (...) {
    return {StructuredValueRenderStatus::evaluation_failed, {}};
  }
}

std::string renderStructuredSnapshotForTest(
    DataSourcePtr snapshot, const StructuredValueRenderOptions &options) {
  return renderSnapshot(snapshot, options);
}

} // namespace OCL::detail
