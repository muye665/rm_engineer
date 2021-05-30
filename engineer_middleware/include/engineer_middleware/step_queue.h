//
// Created by qiayuan on 4/3/21.
//

#ifndef ENGINEER_MIDDLEWARE_STEP_QUEUE_H_
#define ENGINEER_MIDDLEWARE_STEP_QUEUE_H_
#pragma once

// STL
#include <deque>
#include <string>

#include "engineer_middleware/step.h"

namespace engineer_middleware {
class StepQueue {
 public:
  StepQueue(const XmlRpc::XmlRpcValue &steps, tf2_ros::Buffer &tf,
            moveit::planning_interface::MoveGroupInterface &arm_group,
            moveit::planning_interface::MoveGroupInterface &hand_group,
            ros::Publisher &card_pub, ros::Publisher &gimbal_pub) {
    ROS_ASSERT(steps.getType() == XmlRpc::XmlRpcValue::TypeArray);
    for (int i = 0; i < steps.size(); ++i)
      queue_.emplace_back(steps[i], tf, arm_group, hand_group, card_pub, gimbal_pub);
  }
  bool forward() {
  }
  void reset() { itr_ = queue_.begin(); }
  const std::deque<Step> &getQueue() const { return queue_; }
  std::deque<Step>::size_type size() const { return queue_.size(); }
 private:
  std::deque<Step> queue_;
  std::deque<Step>::iterator itr_;
};
}

#endif //ENGINEER_MIDDLEWARE_STEP_QUEUE_H_
