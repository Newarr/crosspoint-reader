#pragma once

namespace turnnet {

struct TriggerCounts {
  int next = 0;
  int previous = 0;
};

inline int net(const TriggerCounts counts) { return counts.next - counts.previous; }

}  // namespace turnnet
