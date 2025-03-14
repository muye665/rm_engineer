/*******************************************************************************
 * BSD 3-Clause License
 *
 * Copyright (c) 2021, Qiayuan Liao
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *******************************************************************************/

//
// Created by qiayuan on 5/29/21.
//

#pragma once

#include <rm_common/ros_utilities.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <geometry_msgs/Twist.h>
#include <std_msgs/Float64.h>
#include <moveit/move_group_interface/move_group_interface.h>
#include <rm_msgs/GimbalCmd.h>
#include <rm_msgs/GpioData.h>
#include <rm_msgs/MultiDofCmd.h>
#include <std_msgs/Int32.h>
#include <std_msgs/String.h>
#include <engineer_middleware/chassis_interface.h>
#include <engineer_middleware/points.h>

namespace engineer_middleware
{
template <class Interface>
class MotionBase
{
public:
  MotionBase(XmlRpc::XmlRpcValue& motion, Interface& interface) : interface_(interface)
  {
    time_out_ = xmlRpcGetDouble(motion["common"], "timeout", 3);
  };
  ~MotionBase() = default;
  virtual bool move() = 0;
  virtual bool isFinish() = 0;
  bool checkTimeout(ros::Duration period)
  {
    if (period.toSec() > time_out_)
    {
      ROS_ERROR("Step timeout,it should be finish in %f seconds", time_out_);
      return false;
    }
    return true;
  }
  virtual void stop() = 0;

protected:
  Interface& interface_;
  double time_out_{};
};

class MoveitMotionBase : public MotionBase<moveit::planning_interface::MoveGroupInterface>
{
public:
  MoveitMotionBase(XmlRpc::XmlRpcValue& motion, moveit::planning_interface::MoveGroupInterface& interface)
    : MotionBase<moveit::planning_interface::MoveGroupInterface>(motion, interface)
  {
    speed_ = xmlRpcGetDouble(motion["common"], "speed", 0.1);
    accel_ = xmlRpcGetDouble(motion["common"], "accel", 0.1);
  }
  bool move() override
  {
    interface_.setMaxVelocityScalingFactor(speed_);
    interface_.setMaxAccelerationScalingFactor(accel_);
    countdown_ = 5;
    return true;
  }
  bool isFinish() override
  {
    if (isReachGoal())
      countdown_--;
    else
      countdown_ = 5;
    return countdown_ < 0;
  }
  void stop() override
  {
    interface_.setMaxVelocityScalingFactor(0.);
    interface_.setMaxAccelerationScalingFactor(0.);
    interface_.stop();
  }
  std_msgs::Int32 getPlanningResult()
  {
    return msg_;
  }
  sensor_msgs::PointCloud2 getPointCloud2()
  {
    return points_.getPointCloud2();
  }

protected:
  virtual bool isReachGoal() = 0;
  double speed_, accel_;
  int countdown_{};
  std_msgs::Int32 msg_;
  Points points_;
};

class EndEffectorMotion : public MoveitMotionBase
{
public:
  EndEffectorMotion(XmlRpc::XmlRpcValue& motion, moveit::planning_interface::MoveGroupInterface& interface,
                    tf2_ros::Buffer& tf)
    : MoveitMotionBase(motion, interface), tf_(tf), has_pos_(false), has_ori_(false), is_cartesian_(false)
  {
    target_.pose.orientation.w = 1.;
    tolerance_position_ = xmlRpcGetDouble(motion, "tolerance_position", 0.01);
    tolerance_orientation_ = xmlRpcGetDouble(motion, "tolerance_orientation", 0.1);
    ROS_ASSERT(motion.hasMember("frame"));
    target_.header.frame_id = std::string(motion["frame"]);
    if (motion.hasMember("xyz"))
    {
      ROS_ASSERT(motion["xyz"].getType() == XmlRpc::XmlRpcValue::TypeArray);
      target_.pose.position.x = xmlRpcGetDouble(motion["xyz"], 0);
      target_.pose.position.y = xmlRpcGetDouble(motion["xyz"], 1);
      target_.pose.position.z = xmlRpcGetDouble(motion["xyz"], 2);
      has_pos_ = true;
    }
    if (motion.hasMember("rpy"))
    {
      ROS_ASSERT(motion["rpy"].getType() == XmlRpc::XmlRpcValue::TypeArray);
      tf2::Quaternion quat_tf;
      quat_tf.setRPY(motion["rpy"][0], motion["rpy"][1], motion["rpy"][2]);
      geometry_msgs::Quaternion quat_msg = tf2::toMsg(quat_tf);
      target_.pose.orientation = quat_msg;
      has_ori_ = true;
    }
    ROS_ASSERT(has_pos_ || has_ori_);
    if (motion.hasMember("cartesian"))
      is_cartesian_ = motion["cartesian"];
  }

  bool move() override
  {
    MoveitMotionBase::move();
    geometry_msgs::PoseStamped final_target;
    if (!target_.header.frame_id.empty())
    {
      try
      {
        tf2::doTransform(target_.pose, final_target.pose,
                         tf_.lookupTransform(interface_.getPlanningFrame(), target_.header.frame_id, ros::Time(0)));
        final_target.header.frame_id = interface_.getPlanningFrame();
      }
      catch (tf2::TransformException& ex)
      {
        ROS_WARN("%s", ex.what());
        return false;
      }
    }
    if (is_cartesian_)
    {
      moveit_msgs::RobotTrajectory trajectory;
      std::vector<geometry_msgs::Pose> waypoints;
      waypoints.push_back(target_.pose);
      if (interface_.computeCartesianPath(waypoints, 0.01, 0.0, trajectory) != 1)
      {
        ROS_INFO_STREAM("Collisions will occur in the"
                        << interface_.computeCartesianPath(waypoints, 0.01, 0.0, trajectory) << "of the trajectory");
        return false;
      }
      return interface_.asyncExecute(trajectory) == moveit::planning_interface::MoveItErrorCode::SUCCESS;
    }
    else
    {
      if (has_pos_ && has_ori_)
        interface_.setPoseTarget(final_target);
      else if (has_pos_ && !has_ori_)
        interface_.setPositionTarget(final_target.pose.position.x, final_target.pose.position.y,
                                     final_target.pose.position.z);
      else if (!has_pos_ && has_ori_)
        interface_.setOrientationTarget(final_target.pose.orientation.x, final_target.pose.orientation.y,
                                        final_target.pose.orientation.z, final_target.pose.orientation.w);
      moveit::planning_interface::MoveGroupInterface::Plan plan;
      msg_.data = interface_.plan(plan).val;
      return interface_.asyncExecute(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS;
    }
  }

protected:
  bool isReachGoal() override
  {
    geometry_msgs::Pose pose = interface_.getCurrentPose().pose;
    double roll_current, pitch_current, yaw_current, roll_goal, pitch_goal, yaw_goal;
    quatToRPY(pose.orientation, roll_current, pitch_current, yaw_current);
    quatToRPY(target_.pose.orientation, roll_goal, pitch_goal, yaw_goal);
    // TODO: Add orientation error check
    return (std::pow(pose.position.x - target_.pose.position.x, 2) +
                    std::pow(pose.position.y - target_.pose.position.y, 2) +
                    std::pow(pose.position.z - target_.pose.position.z, 2) <
                tolerance_position_ &&
            std::abs(angles::shortest_angular_distance(yaw_current, yaw_goal)) < tolerance_orientation_ &&
            std::abs(angles::shortest_angular_distance(pitch_current, pitch_goal)) < tolerance_orientation_ &&
            std::abs(angles::shortest_angular_distance(yaw_current, yaw_goal)) < tolerance_orientation_);
  }
  tf2_ros::Buffer& tf_;
  bool has_pos_, has_ori_, is_cartesian_;
  geometry_msgs::PoseStamped target_;
  double tolerance_position_, tolerance_orientation_;
};

class SpaceEeMotion : public EndEffectorMotion
{
public:
  SpaceEeMotion(XmlRpc::XmlRpcValue& motion, moveit::planning_interface::MoveGroupInterface& interface,
                tf2_ros::Buffer& tf)
    : EndEffectorMotion(motion, interface, tf)
  {
    point_resolution_ = xmlRpcGetDouble(motion, "point_resolution", 0.01);
    radius_ = xmlRpcGetDouble(motion, "radius", 0.1);
    max_planning_times_ = (int)xmlRpcGetDouble(motion, "max_planning_times", 3);
    if (motion.hasMember("is_refer_planning_frame"))
      is_refer_planning_frame_ = motion["is_refer_planning_frame"];
    else
      is_refer_planning_frame_ = false;
    if (motion.hasMember("spacial_shape"))
    {
      points_.cleanPoints();
      if (motion["spacial_shape"] == "SPHERE")
        points_.setValue(Points::SPHERE, target_.pose.position.x, target_.pose.position.y, target_.pose.position.z,
                         radius_, point_resolution_);
      else if (motion["spacial_shape"] == "BASICS")
        points_.setValue(Points::BASICS, target_.pose.position.x, target_.pose.position.y, target_.pose.position.z,
                         x_length_, y_length_, z_length_, point_resolution_);
      else
        ROS_ERROR("NO SUCH SHAPE");
      points_.generateGeometryPoints();
    }
  }
  bool move() override
  {
    points_.cleanPoints();
    points_.generateGeometryPoints();
    MoveitMotionBase::move();
    int move_times = (int)points_.getPoints().size();
    for (int i = 0; i < move_times && i < max_planning_times_; ++i)
    {
      if (!target_.header.frame_id.empty())
      {
        if (!is_refer_planning_frame_)
        {
          //            try
          //            {
          //              double roll, pitch, yaw, roll_temp, pitch_temp, yaw_temp;
          //              geometry_msgs::TransformStamped exchange2base;
          //              exchange2base = tf_.lookupTransform("base_link", target_.header.frame_id, ros::Time(0));
          //              quatToRPY(exchange2base.transform.rotation, roll_temp, pitch_temp, yaw_temp);
          //              quatToRPY(target_.pose.orientation, roll, pitch, yaw);
          //
          //              tf2::Quaternion tmp_tf_quaternion;
          //              tmp_tf_quaternion.setRPY(roll, pitch, yaw);
          //              geometry_msgs::Quaternion quat_tf = tf2::toMsg(tmp_tf_quaternion);
          //              quatToRPY(quat_tf, roll, pitch, yaw);
          //              points_.rectifyForRPY(pitch_temp, yaw_temp, k_x_, k_theta_, k_beta_);
          //              if (link7_length_)
          //                points_.rectifyForLink7(pitch_temp, link7_length_);
          //              target_.pose.position.x = points_.getPoints()[i].x;
          //              target_.pose.position.y = points_.getPoints()[i].y;
          //              target_.pose.position.z = points_.getPoints()[i].z;
          //              geometry_msgs::PoseStamped temp_target;
          //              temp_target.pose.position = target_.pose.position;
          //              temp_target.pose.orientation = quat_tf;
          //              tf2::doTransform(temp_target.pose, final_target_.pose,
          //                               tf_.lookupTransform(interface_.getPlanningFrame(), target_.header.frame_id,
          //                               ros::Time(0)));
          //              final_target_.pose.position.x = final_target_.pose.position.x;
          //              final_target_.pose.position.y = final_target_.pose.position.y;
          //              final_target_.pose.position.z = final_target_.pose.position.z;
          //              final_target_.header.frame_id = interface_.getPlanningFrame();
          //            }
          //            catch (tf2::TransformException& ex)
          //            {
          //              ROS_WARN("%s", ex.what());
          //              return false;
          //            }
        }
        else
        {
          try
          {
            double roll, pitch, yaw, roll_temp, pitch_temp, yaw_temp;
            geometry_msgs::TransformStamped base2exchange;
            base2exchange = tf_.lookupTransform("base_link", target_.header.frame_id, ros::Time(0));
            target_.pose.position.x = points_.getPoints()[i].x;
            target_.pose.position.y = points_.getPoints()[i].y;
            target_.pose.position.z = points_.getPoints()[i].z;
            quatToRPY(base2exchange.transform.rotation, roll, pitch, yaw);
            quatToRPY(target_.pose.orientation, roll_temp, pitch_temp, yaw_temp);
            quat_base2exchange_.setW(base2exchange.transform.rotation.w);
            quat_base2exchange_.setX(base2exchange.transform.rotation.x);
            quat_base2exchange_.setY(base2exchange.transform.rotation.y);
            quat_base2exchange_.setZ(base2exchange.transform.rotation.z);

            quat_target_.setW(target_.pose.orientation.w);
            quat_target_.setX(target_.pose.orientation.x);
            quat_target_.setY(target_.pose.orientation.y);
            quat_target_.setZ(target_.pose.orientation.z);

            tf2::Quaternion tf_quaternion;
            tf_quaternion = quat_base2exchange_ * quat_target_;

            final_target_.pose.position.x = base2exchange.transform.translation.x + target_.pose.position.x;
            final_target_.pose.position.y = base2exchange.transform.translation.y + target_.pose.position.y;
            final_target_.pose.position.z = base2exchange.transform.translation.z + target_.pose.position.z;
            final_target_.pose.orientation.w = tf_quaternion.w();
            final_target_.pose.orientation.x = tf_quaternion.x();
            final_target_.pose.orientation.y = tf_quaternion.y();
            final_target_.pose.orientation.z = tf_quaternion.z();

            final_target_.header.frame_id = interface_.getPlanningFrame();
          }
          catch (tf2::TransformException& ex)
          {
            ROS_WARN("%s", ex.what());
            return false;
          }
        }
      }
      interface_.setPoseTarget(final_target_);
      moveit::planning_interface::MoveGroupInterface::Plan plan;
      msg_.data = interface_.plan(plan).val;
      if (msg_.data == 1)
        return interface_.asyncExecute(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS;
    }
    return false;
  }

private:
  bool isReachGoal() override
  {
    geometry_msgs::Pose pose = interface_.getCurrentPose().pose;
    double roll_current, pitch_current, yaw_current, roll_goal, pitch_goal, yaw_goal;
    quatToRPY(pose.orientation, roll_current, pitch_current, yaw_current);
    quatToRPY(final_target_.pose.orientation, roll_goal, pitch_goal, yaw_goal);
    return (std::pow(pose.position.x - final_target_.pose.position.x, 2) +
                    std::pow(pose.position.y - final_target_.pose.position.y, 2) +
                    std::pow(pose.position.z - final_target_.pose.position.z, 2) <
                tolerance_position_ &&
            std::abs(angles::shortest_angular_distance(roll_current, roll_goal)) +
                    std::abs(angles::shortest_angular_distance(pitch_current, pitch_goal)) +
                    std::abs(angles::shortest_angular_distance(yaw_current, yaw_goal)) <
                tolerance_orientation_);
  }
  bool is_refer_planning_frame_;
  geometry_msgs::PoseStamped final_target_;
  tf2::Quaternion quat_base2exchange_, quat_target_;
  int max_planning_times_{};
  double radius_, point_resolution_, x_length_, y_length_, z_length_;
};

class JointMotion : public MoveitMotionBase
{
public:
  JointMotion(XmlRpc::XmlRpcValue& motion, moveit::planning_interface::MoveGroupInterface& interface,
              tf2_ros::Buffer& tf_buffer)
    : MoveitMotionBase(motion, interface), tf_buffer_(tf_buffer)
  {
    if (motion.hasMember("joints"))
    {
      ROS_ASSERT(motion["joints"].getType() == XmlRpc::XmlRpcValue::TypeArray);
      for (int i = 0; i < motion["joints"].size(); ++i)
      {
        if (motion["joints"][i].getType() == XmlRpc::XmlRpcValue::TypeDouble)
          target_.push_back(motion["joints"][i]);
        else if (motion["joints"][i] == "KEEP")
          target_.push_back(NAN);
        else if (motion["joints"][i] == "VARIABLE")
          target_.push_back(motion["variable"][i]);
        else
          ROS_ERROR("ERROR TYPE OR STRING!!!");
      }
    }
    if (motion.hasMember("tolerance"))
    {
      ROS_ASSERT(motion["tolerance"]["tolerance_joints"].getType() == XmlRpc::XmlRpcValue::TypeArray);
      for (int i = 0; i < motion["tolerance"]["tolerance_joints"].size(); ++i)
        tolerance_joints_.push_back(xmlRpcGetDouble(motion["tolerance"]["tolerance_joints"], i));
    }
    if (motion.hasMember("record_arm2base"))
      record_arm2base_ = bool(motion["record_arm2base"]);
  }
  static geometry_msgs::TransformStamped arm2base;
  bool move() override
  {
    if (record_arm2base_)
    {
      try
      {
        arm2base.header.frame_id = "base_link";
        arm2base.header.stamp = ros::Time::now();
        arm2base.child_frame_id = "chassis_target";

        arm2base = tf_buffer_.lookupTransform("base_link", "link4", ros::Time(0));
        ROS_INFO_STREAM("X is: " << arm2base.transform.translation.x << "Y is: " << arm2base.transform.translation.y);
      }
      catch (tf2::TransformException& ex)
      {
        ROS_WARN("%s", ex.what());
        return false;
      }
    }
    final_target_.clear();
    if (target_.empty())
      return false;
    MoveitMotionBase::move();
    for (int i = 0; i < (int)target_.size(); i++)
    {
      if (!std::isnormal(target_[i]))
      {
        final_target_.push_back(interface_.getCurrentJointValues()[i]);
      }
      else
      {
        final_target_.push_back(target_[i]);
      }
    }
    interface_.setJointValueTarget(final_target_);
    moveit::planning_interface::MoveGroupInterface::Plan plan;
    msg_.data = interface_.plan(plan).val;
    return (interface_.asyncExecute(plan) == moveit::planning_interface::MoveItErrorCode::SUCCESS);
  }

private:
  bool isReachGoal() override
  {
    std::vector<double> current = interface_.getCurrentJointValues();
    double error = 0.;
    bool flag = 1, joint_reached = 0;
    for (int i = 0; i < (int)final_target_.size(); ++i)
    {
      error = std::abs(final_target_[i] - current[i]);
      joint_reached = (error < tolerance_joints_[i]);
      //      if (!joint_reached)
      //        ROS_INFO_STREAM("Joint" << i + 1 << " didn't reach configured tolerance range,error: " << error);
      flag &= joint_reached;
    }
    return flag;
  }
  std::vector<double> target_, final_target_, tolerance_joints_;
  bool record_arm2base_{ false };
  tf2_ros::Buffer& tf_buffer_;
};

template <class MsgType>
class PublishMotion : public MotionBase<ros::Publisher>
{
public:
  PublishMotion(XmlRpc::XmlRpcValue& motion, ros::Publisher& interface) : MotionBase<ros::Publisher>(motion, interface)
  {
  }
  bool move() override
  {
    interface_.publish(msg_);
    return true;
  }
  bool isFinish() override
  {
    return true;
  }  // TODO: Add feedback
  void stop() override
  {
  }

protected:
  MsgType msg_;
};

class HandMotion : public PublishMotion<std_msgs::Float64>
{
public:
  HandMotion(XmlRpc::XmlRpcValue& motion, ros::Publisher& interface)
    : PublishMotion<std_msgs::Float64>(motion, interface)
  {
    ROS_ASSERT(motion.hasMember("position"));
    ROS_ASSERT(motion.hasMember("delay"));
    position_ = xmlRpcGetDouble(motion, "position", 0.0);
    delay_ = xmlRpcGetDouble(motion, "delay", 0.0);
  }
  bool move() override
  {
    start_time_ = ros::Time::now();
    msg_.data = position_;
    return PublishMotion::move();
  }
  bool isFinish() override
  {
    return ((ros::Time::now() - start_time_).toSec() > delay_);
  }

private:
  double position_, delay_;
  ros::Time start_time_;
};

class GpioMotion : public PublishMotion<rm_msgs::GpioData>
{
public:
  GpioMotion(XmlRpc::XmlRpcValue& motion, ros::Publisher& interface)
    : PublishMotion<rm_msgs::GpioData>(motion, interface)
  {
    delay_ = xmlRpcGetDouble(motion, "delay", 0.01);
    msg_.gpio_state.assign(6, false);
    msg_.gpio_name.assign(6, "no_registered");
    pin_ = motion["pin"];
    state_ = motion["state"];
    switch (pin_)
    {
      case 0:
        msg_.gpio_name[0] = "main_gripper";
        break;
      case 1:
        msg_.gpio_name[1] = "silver_gripper1";
        break;
      case 2:
        msg_.gpio_name[2] = "silver_gripper2";
        break;
      case 3:
        msg_.gpio_name[3] = "silver_gripper3";
        break;
      case 4:
        msg_.gpio_name[4] = "gold_gripper";
        break;
      case 5:
        msg_.gpio_name[5] = "silver_pump";
        break;
//      case 6:
//        msg_.gpio_name[6] = "unused";
//        ROS_WARN_STREAM("GPIO port 7 is unused now!");
//        break;
//      case 7:
//        msg_.gpio_name[7] = "unused";
//        ROS_WARN_STREAM("GPIO port 7 is unused now!");
//        break;
    }
  }
  bool move() override
  {
    start_time_ = ros::Time::now();
    msg_.gpio_state[pin_] = state_;
    return PublishMotion::move();
  }

  bool isFinish() override
  {
    return ((ros::Time::now() - start_time_).toSec() > delay_);
  }

private:
  ros::Time start_time_;
  double delay_;
  bool state_;
  int pin_;
};

class StoneNumMotion : public PublishMotion<std_msgs::String>
{
public:
  StoneNumMotion(XmlRpc::XmlRpcValue& motion, ros::Publisher& interface)
    : PublishMotion<std_msgs::String>(motion, interface)
  {
    msg_.data = static_cast<std::string>(motion["change"]);
  }
};

class JointPositionMotion : public PublishMotion<std_msgs::Float64>
{
public:
  JointPositionMotion(XmlRpc::XmlRpcValue& motion, ros::Publisher& interface, tf2_ros::Buffer& tf)
    : PublishMotion<std_msgs::Float64>(motion, interface), tf_(tf)
  {
    original_tf_ = std::string(motion["original_tf"]);
    reference_tf_ = std::string(motion["reference_tf"]);
    direction_ = std::string(motion["direction"]);
    target_ = xmlRpcGetDouble(motion, "target", 0.0);
    delay_ = xmlRpcGetDouble(motion, "delay", 0.0);
  }
  bool move() override
  {
    double roll, pitch, yaw;
    geometry_msgs::TransformStamped tf;
    tf = tf_.lookupTransform(original_tf_, reference_tf_, ros::Time(0));
    quatToRPY(tf.transform.rotation, roll, pitch, yaw);
    start_time_ = ros::Time::now();
    if (direction_ == "roll")
      msg_.data = roll;
    else if (direction_ == "pitch")
      msg_.data = pitch;
    else if (direction_ == "yaw")
      msg_.data = yaw;
    else
      msg_.data = target_;
    return PublishMotion::move();
  }
  bool isFinish() override
  {
    return ((ros::Time::now() - start_time_).toSec() > delay_);
  }

private:
  double target_, delay_;
  ros::Time start_time_;
  tf2_ros::Buffer& tf_;
  std::string original_tf_, reference_tf_, direction_;
};

class GimbalMotion : public PublishMotion<rm_msgs::GimbalCmd>
{
public:
  GimbalMotion(XmlRpc::XmlRpcValue& motion, ros::Publisher& interface)
    : PublishMotion<rm_msgs::GimbalCmd>(motion, interface)
  {
    if (motion.hasMember("frame"))
      msg_.target_pos.header.frame_id = std::string(motion["frame"]);
    if (motion.hasMember("position"))
    {
      ROS_ASSERT(motion["position"].getType() == XmlRpc::XmlRpcValue::TypeArray);
      msg_.target_pos.point.x = xmlRpcGetDouble(motion["position"], 0);
      msg_.target_pos.point.y = xmlRpcGetDouble(motion["position"], 1);
      msg_.target_pos.point.z = xmlRpcGetDouble(motion["position"], 2);
    }
    msg_.mode = msg_.DIRECT;
  }
};

class ReversalMotion : public PublishMotion<rm_msgs::MultiDofCmd>
{
public:
  ReversalMotion(XmlRpc::XmlRpcValue& motion, ros::Publisher& interface)
    : PublishMotion<rm_msgs::MultiDofCmd>(motion, interface)
  {
    delay_ = xmlRpcGetDouble(motion, "delay", 0.0);
    if (std::string(motion["mode"]) == "POSITION")
      msg_.mode = msg_.POSITION;
    else
      msg_.mode = msg_.VELOCITY;
    if (motion.hasMember("values"))
    {
      ROS_ASSERT(motion["values"].getType() == XmlRpc::XmlRpcValue::TypeArray);
      msg_.linear.x = xmlRpcGetDouble(motion["values"], 0);
      msg_.linear.y = xmlRpcGetDouble(motion["values"], 1);
      msg_.linear.z = xmlRpcGetDouble(motion["values"], 2);
      msg_.angular.x = xmlRpcGetDouble(motion["values"], 3);
      msg_.angular.y = xmlRpcGetDouble(motion["values"], 4);
      msg_.angular.z = xmlRpcGetDouble(motion["values"], 5);
    }
  }
  void setZero()
  {
    zero_msg_.mode = msg_.mode;
    zero_msg_.linear.x = 0.;
    zero_msg_.linear.y = 0.;
    zero_msg_.linear.z = 0.;
    zero_msg_.angular.x = 0.;
    zero_msg_.angular.y = 0.;
    zero_msg_.angular.z = 0.;
  }
  bool move() override
  {
    start_time_ = ros::Time::now();
    interface_.publish(msg_);
    if (msg_.mode == msg_.POSITION)
    {
      ros::Duration(0.2).sleep();
      ReversalMotion::setZero();
      interface_.publish(zero_msg_);
    }
    return true;
  }
  bool isFinish() override
  {
    return ((ros::Time::now() - start_time_).toSec() > delay_);
  }

private:
  double delay_;
  ros::Time start_time_;
  rm_msgs::MultiDofCmd zero_msg_;
};

class JointPointMotion : public PublishMotion<std_msgs::Float64>
{
public:
  JointPointMotion(XmlRpc::XmlRpcValue& motion, ros::Publisher& interface)
    : PublishMotion<std_msgs::Float64>(motion, interface)
  {
    ROS_ASSERT(motion.hasMember("target"));
    target_ = xmlRpcGetDouble(motion, "target", 0.0);
    delay_ = xmlRpcGetDouble(motion, "delay", 0.0);
  }
  bool move() override
  {
    start_time_ = ros::Time::now();
    msg_.data = target_;
    return PublishMotion::move();
  }
  bool isFinish() override
  {
    return ((ros::Time::now() - start_time_).toSec() > delay_);
  }

private:
  double target_, delay_;
  ros::Time start_time_;
};

class ExtendMotion : public PublishMotion<std_msgs::Float64>
{
public:
  ExtendMotion(XmlRpc::XmlRpcValue& motion, ros::Publisher& interface, bool is_front)
    : PublishMotion<std_msgs::Float64>(motion, interface)
  {
    ROS_ASSERT(motion.hasMember("front") || motion.hasMember("back"));
    if (is_front)
      target_ = xmlRpcGetDouble(motion, "front", 0.0);
    else
      target_ = xmlRpcGetDouble(motion, "back", 0.0);
  }
  bool move() override
  {
    msg_.data = target_;
    return PublishMotion::move();
  }

private:
  double target_;
};

class ChassisMotion : public MotionBase<ChassisInterface>
{
public:
  ChassisMotion(XmlRpc::XmlRpcValue& motion, ChassisInterface& interface)
    : MotionBase<ChassisInterface>(motion, interface)
  {
    chassis_tolerance_position_ = xmlRpcGetDouble(motion, "chassis_tolerance_position", 0.01);
    chassis_tolerance_angular_ = xmlRpcGetDouble(motion, "chassis_tolerance_angular", 0.01);
    if (motion.hasMember("frame"))
      target_.header.frame_id = std::string(motion["frame"]);
    if (motion.hasMember("position"))
    {
      target_.pose.position.x = xmlRpcGetDouble(motion["position"], 0);
      target_.pose.position.y = xmlRpcGetDouble(motion["position"], 1);
    }
    if (motion.hasMember("yaw"))
    {
      tf2::Quaternion quat_tf;
      quat_tf.setRPY(0, 0, motion["yaw"]);
      geometry_msgs::Quaternion quat_msg = tf2::toMsg(quat_tf);
      target_.pose.orientation = quat_msg;
    }
  }
  bool move() override
  {
    interface_.setGoal(target_);
    return true;
  }
  bool isFinish() override
  {
    return interface_.getErrorPos() < chassis_tolerance_position_ &&
           interface_.getErrorYaw() < chassis_tolerance_angular_;
  }
  void stop() override
  {
    interface_.stop();
  }

protected:
  geometry_msgs::PoseStamped target_;
  double chassis_tolerance_position_, chassis_tolerance_angular_;
};

class ChassisTargetMotion : public ChassisMotion
{
public:
  ChassisTargetMotion(XmlRpc::XmlRpcValue& motion, ChassisInterface& interface, tf2_ros::Buffer& tf_buffer)
    : ChassisMotion(motion, interface), tf_buffer_(tf_buffer)
  {
    chassis_tolerance_position_ = xmlRpcGetDouble(motion, "chassis_tolerance_position", 0.01);
    chassis_tolerance_angular_ = xmlRpcGetDouble(motion, "chassis_tolerance_angular", 0.01);
    if (motion.hasMember("frame"))
      target_.header.frame_id = std::string(motion["frame"]);
    x_offset_ = xmlRpcGetDouble(motion["offset"], 0);
    y_offset_ = xmlRpcGetDouble(motion["offset"], 1);
    yaw_scale_ = xmlRpcGetDouble(motion, "yaw_scale", 1);
    move_target_ = std::string(motion["target_frame"]);
  }
  bool move() override
  {
    if (move_target_ == "arm")
    {
      ROS_INFO_STREAM("TARGET IS ARM");
      try
      {
        geometry_msgs::TransformStamped arm2base_now = tf_buffer_.lookupTransform("base_link", "link4", ros::Time(0));

        target_.pose.position.x =
            JointMotion::arm2base.transform.translation.x - arm2base_now.transform.translation.x + x_offset_;
        target_.pose.position.y =
            JointMotion::arm2base.transform.translation.y - arm2base_now.transform.translation.y + y_offset_;
        tf2::Quaternion quat_tf;
        quat_tf.setRPY(0, 0, 0);
        geometry_msgs::Quaternion quat_msg = tf2::toMsg(quat_tf);
        target_.pose.orientation = quat_msg;
        interface_.setGoal(target_);
        return true;
      }
      catch (tf2::TransformException& ex)
      {
        ROS_WARN("%s", ex.what());
        return false;
      }
    }
    else
    {
      ROS_INFO_STREAM("TARGET IS " << move_target_);
      try
      {
        double roll, pitch, yaw;
        geometry_msgs::TransformStamped base2target;
        base2target = tf_buffer_.lookupTransform("base_link", move_target_, ros::Time(0));
        target_.pose.position.x = base2target.transform.translation.x + x_offset_;
        target_.pose.position.y = base2target.transform.translation.y + y_offset_;
        ROS_INFO_STREAM("base2target x: " << base2target.transform.translation.x);
        ROS_INFO_STREAM("base2target y: " << base2target.transform.translation.y);
        ROS_INFO_STREAM("target x: " << target_.pose.position.x);
        ROS_INFO_STREAM("target y: " << target_.pose.position.y);
        quatToRPY(base2target.transform.rotation, roll, pitch, yaw);
        tf2::Quaternion quat_tf;
        quat_tf.setRPY(0, 0, yaw * yaw_scale_);
        geometry_msgs::Quaternion quat_msg = tf2::toMsg(quat_tf);
        target_.pose.orientation = quat_msg;
        interface_.setGoal(target_);
        return true;
      }
      catch (tf2::TransformException& ex)
      {
        ROS_WARN("%s", ex.what());
        return false;
      }
    }
  }
  bool isFinish() override
  {
    return interface_.getErrorPos() < chassis_tolerance_position_ &&
           interface_.getErrorYaw() < chassis_tolerance_angular_;
  }
  void stop() override
  {
    interface_.stop();
  }

private:
  double x_offset_{}, y_offset_{}, yaw_scale_{};
  std::string move_target_{};
  tf2_ros::Buffer& tf_buffer_;
};

class AutoExchangeMotion : public MoveitMotionBase
{
public:
  AutoExchangeMotion( XmlRpc::XmlRpcValue& motion, moveit::planning_interface::MoveGroupInterface& interface, tf2_ros::Buffer& tf )
    : MoveitMotionBase( motion, interface ), tf_buffer_( tf ), has_p1_( false ), has_p2_( false )
  {
    target_mid_.pose.orientation.w = 1.;
    target_final_.pose.orientation.w = 1.;
    tolerance_position_ = xmlRpcGetDouble( motion, "tolerance_position", 0.01 );
    tolerance_orientation_ = xmlRpcGetDouble( motion, "tolerance_orientation", 0.03 );
    ROS_ASSERT( motion.hasMember("points") || motion.hasMember("auto") );
    if ( motion.hasMember("points") )
    {
      XmlRpc::XmlRpcValue& points = motion["points"];
      ROS_ASSERT(points.getType() == XmlRpc::XmlRpcValue::TypeStruct);
      if ( points.hasMember("point_mid") )
      {
        ROS_ASSERT( points["point_mid"].hasMember("frame") );
        target_mid_.header.frame_id = std::string( points["point_mid"]["frame"] );
        if ( points["point_mid"].hasMember("xyz") )
        {
          ROS_ASSERT( points["point_mid"]["xyz"].getType() == XmlRpc::XmlRpcValue::TypeArray );
          target_mid_.pose.position.x = xmlRpcGetDouble( points["point_mid"]["xyz"], 0 );
          target_mid_.pose.position.y = xmlRpcGetDouble( points["point_mid"]["xyz"], 1 );
          target_mid_.pose.position.z = xmlRpcGetDouble( points["point_mid"]["xyz"], 2 );
        }
        if ( points["point_mid"].hasMember("rpy"))
        {
          ROS_ASSERT( points["point_mid"]["rpy"].getType() == XmlRpc::XmlRpcValue::TypeArray );
          tf2::Quaternion quat_tf;
          quat_tf.setRPY( points["point_mid"]["rpy"][0], points["point_mid"]["rpy"][1], points["point_mid"]["rpy"][2] );
          geometry_msgs::Quaternion quat_msg = tf2::toMsg(quat_tf);
          target_mid_.pose.orientation = quat_msg;
        }
        has_p1_ = true;
      }
      if ( points.hasMember("point_final") )
      {
        ROS_ASSERT( points["point_final"].hasMember("frame") );
        target_final_.header.frame_id = std::string( points["point_final"]["frame"] );
        if ( points["point_final"].hasMember("xyz") )
        {
          ROS_ASSERT( points["point_final"]["xyz"].getType() == XmlRpc::XmlRpcValue::TypeArray );
          target_final_.pose.position.x = xmlRpcGetDouble( points["point_final"]["xyz"], 0 );
          target_final_.pose.position.y = xmlRpcGetDouble( points["point_final"]["xyz"], 1 );
          target_final_.pose.position.z = xmlRpcGetDouble( points["point_final"]["xyz"], 2 );
        }
        if ( points["point_final"].hasMember("rpy"))
        {
          ROS_ASSERT( points["point_final"]["rpy"].getType() == XmlRpc::XmlRpcValue::TypeArray );
          tf2::Quaternion quat_tf;
          quat_tf.setRPY( points["point_final"]["rpy"][0], points["point_final"]["rpy"][1], points["point_final"]["rpy"][2] );
          geometry_msgs::Quaternion quat_msg = tf2::toMsg(quat_tf);
          target_final_.pose.orientation = quat_msg;
        }
        has_p2_ = true;
      }
      ROS_ASSERT( has_p1_ && has_p2_ );
    }
    if ( motion.hasMember("auto") )
    {
      double straight_distance = xmlRpcGetDouble( motion["auto"],"straight_distance",0.2 );
      ROS_ASSERT(motion["auto"].hasMember("frame") );
      std::string target_frame_id = std::string( motion["auto"]["frame"] );
      target_mid_.header.frame_id = target_frame_id;
      target_final_.header.frame_id = target_frame_id;

      tf2::Quaternion tool_tf;
      tool_tf.setRPY(0.0, 3.14, 0.0);
      target_mid_.pose.position.x = straight_distance;
      target_mid_.pose.position.y = 0.0;
      target_mid_.pose.position.z = 0.0;
      target_mid_.pose.orientation = tf2::toMsg(tool_tf);

      target_final_.pose.position.x = 0.0;
      target_final_.pose.position.y = 0.0;
      target_final_.pose.position.z = 0.0;
      target_final_.pose.orientation = tf2::toMsg(tool_tf);
    }
  }

  bool move() override
  {
    MoveitMotionBase::move();
    if ( !target_mid_.header.frame_id.empty() )
    {
      try
      {
        tf2::doTransform( target_mid_.pose, plan_target_mid_.pose,
                         tf_buffer_.lookupTransform(interface_.getPlanningFrame(), target_mid_.header.frame_id, ros::Time(0)) );
        plan_target_mid_.header.frame_id = interface_.getPlanningFrame();
      }
      catch ( tf2::TransformException& ex )
      {
        ROS_WARN( "%s", ex.what() );
        return false;
      }
    }
    if ( !target_final_.header.frame_id.empty() )
    {
      try
      {
        tf2::doTransform( target_final_.pose, plan_target_final_.pose,
                         tf_buffer_.lookupTransform(interface_.getPlanningFrame(), target_final_.header.frame_id, ros::Time(0)) );
        plan_target_final_.header.frame_id = interface_.getPlanningFrame();
      }
      catch ( tf2::TransformException& ex )
      {
        ROS_WARN( "%s", ex.what() );
        return false;
      }
    }
    std::vector<geometry_msgs::PoseStamped> targets;
    targets.push_back( plan_target_final_ );
    targets.push_back( plan_target_mid_ );
    interface_.setPoseTargets( targets );
    moveit::planning_interface::MoveGroupInterface::Plan plan;
    msg_.data = interface_.plan( plan ).val;
    return interface_.asyncExecute( plan ) == moveit::planning_interface::MoveItErrorCode::SUCCESS;
  }
protected:
  bool isReachGoal() override
  {
    geometry_msgs::Pose pose = interface_.getCurrentPose().pose;
    double roll_current, pitch_current, yaw_current, roll_goal, pitch_goal, yaw_goal;
    quatToRPY(pose.orientation, roll_current, pitch_current, yaw_current);
    quatToRPY(plan_target_final_.pose.orientation, roll_goal, pitch_goal, yaw_goal);

    return ( ( std::pow( pose.position.x - plan_target_final_.pose.position.x, 2 ) +
               std::pow( pose.position.y - plan_target_final_.pose.position.y, 2 ) +
               std::pow( pose.position.z - plan_target_final_.pose.position.z, 2 ) <
               std::pow( tolerance_position_, 2 ) ) &&
            std::abs(angles::shortest_angular_distance(yaw_current, yaw_goal)) < tolerance_orientation_ &&
            std::abs(angles::shortest_angular_distance(pitch_current, pitch_goal)) < tolerance_orientation_ &&
            std::abs(angles::shortest_angular_distance(yaw_current, yaw_goal)) < tolerance_orientation_);
  }
  tf2_ros::Buffer& tf_buffer_;
  geometry_msgs::PoseStamped target_mid_, target_final_;
  geometry_msgs::PoseStamped plan_target_mid_, plan_target_final_;
  double tolerance_position_, tolerance_orientation_;
  bool has_p1_, has_p2_;
};
};  // namespace engineer_middleware
