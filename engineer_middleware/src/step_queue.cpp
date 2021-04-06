//
// Created by qiayuan on 4/3/21.
//


#include "engineer_middleware/step_queue.h"

namespace engineer_middleware {

StepQueue::StepQueue(const XmlRpc::XmlRpcValue &steps,
                     moveit::planning_interface::MoveGroupInterface &arm_group,
                     moveit::planning_interface::MoveGroupInterface &hand_group) :
    arm_group_(arm_group), hand_group_(hand_group) {
  ROS_ASSERT(steps.getType() == XmlRpc::XmlRpcValue::TypeArray);
  arm_group.getCurrentState();
  for (int i = 0; i < steps.size(); ++i) {
    queue_.emplace_back(steps[i], arm_group, hand_group);
  }
}

bool StepQueue::move() {
  for (auto &step:queue_) {
    step.compute(*arm_group_.getCurrentState());
    step.move();
    ros::WallDuration(.5).sleep();
  }
  return true;
}

const std::deque<Step> &StepQueue::getQueue() const {
  return queue_;
}

}