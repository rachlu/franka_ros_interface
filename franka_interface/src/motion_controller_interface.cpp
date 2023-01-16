/***************************************************************************
* Adapted from arm_controller_interface.cpp (sawyer_simulator package)

*
* @package: franka_interface
* @metapackage: franka_ros_interface
* @author: Saif Sidhik <sxs1412@bham.ac.uk>
*

**************************************************************************/

/***************************************************************************
* Copyright (c) 2019-2020, Saif Sidhik
* Copyright (c) 2013-2018, Rethink Robotics Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
**************************************************************************/

#include <franka_interface/motion_controller_interface.h>
#include <controller_manager_msgs/SwitchController.h>

namespace franka_interface {

void MotionControllerInterface::init(ros::NodeHandle& nh,
        boost::shared_ptr<controller_manager::ControllerManager> controller_manager) {
  current_mode_ = -1;

  if (!nh.getParam("/controllers_config/joint_position_controller", joint_position_controller_name_)) {
        joint_position_controller_name_ = "joint_position_controller";
    }
  if (!nh.getParam("/controllers_config/joint_velocity_controller", joint_velocity_controller_name_)) {
        joint_velocity_controller_name_ = "joint_velocity_controller";
    }
  if (!nh.getParam("/controllers_config/joint_torque_controller", joint_torque_controller_name_)) {
        joint_torque_controller_name_ = "joint_torque_controller";
    }
  if (!nh.getParam("/controllers_config/joint_impedance_controller", joint_impedance_controller_name_)) {
        joint_impedance_controller_name_ = "joint_impedance_controller";
    }
  if (!nh.getParam("/controllers_config/cartesian_pose_controller", cartesian_pose_controller_name_)) {
        cartesian_pose_controller_name_ = "cartesian_pose_controller";
   }
  if (!nh.getParam("/controllers_config/cartesian_impedance_controller", cartesian_impedance_controller_name_)) {
        cartesian_impedance_controller_name_ = "cartesian_impedance_controller";
   }
  if (!nh.getParam("/controllers_config/cartesian_force_controller", cartesian_force_controller_name_)) {
        cartesian_force_controller_name_ = "cartesian_force_controller";
    }
  if (!nh.getParam("/controllers_config/trajectory_controller", trajectory_controller_name_)) {
        trajectory_controller_name_ = "position_joint_trajectory_controller";
    }
  if (!nh.getParam("/controllers_config/default_controller", default_controller_name_)) {
        default_controller_name_ = "position_joint_trajectory_controller";
    }

  current_controller_name_ = default_controller_name_;

  all_controllers_.clear();
  all_controllers_.push_back(joint_position_controller_name_);
  all_controllers_.push_back(joint_velocity_controller_name_);
  all_controllers_.push_back(joint_torque_controller_name_);
  all_controllers_.push_back(joint_impedance_controller_name_);
  all_controllers_.push_back(cartesian_pose_controller_name_);
  all_controllers_.push_back(cartesian_impedance_controller_name_);
  all_controllers_.push_back(cartesian_force_controller_name_);
  all_controllers_.push_back(trajectory_controller_name_);

  bool default_defined = false;

  for (size_t i = 0; i < all_controllers_.size(); ++i){
    if (all_controllers_[i] == default_controller_name_){
      default_defined = true;
      break;
    }
  }

  controller_name_to_mode_map_[joint_position_controller_name_] = franka_core_msgs::JointCommand::POSITION_MODE;
  controller_name_to_mode_map_[joint_torque_controller_name_] = franka_core_msgs::JointCommand::TORQUE_MODE;
  controller_name_to_mode_map_[joint_impedance_controller_name_] = franka_core_msgs::JointCommand::IMPEDANCE_MODE;
  controller_name_to_mode_map_[joint_velocity_controller_name_] = franka_core_msgs::JointCommand::VELOCITY_MODE;
  controller_name_to_mode_map_[cartesian_pose_controller_name_] = -1; // Unsure
  controller_name_to_mode_map_[cartesian_force_controller_name_] = -1; // Unsure
  controller_name_to_mode_map_[cartesian_impedance_controller_name_] = -1; // Unsure about this?
  controller_name_to_mode_map_[trajectory_controller_name_] = -1;

  if (! default_defined){
    ROS_ERROR_STREAM_NAMED("MotionControllerInterface", "Default controller not present in the provided controllers!");
  }

  controller_manager_ = controller_manager;
  joint_command_sub_ = nh.subscribe("/franka_ros_interface/motion_controller/arm/joint_commands", 1,
                       &MotionControllerInterface::jointCommandCallback, this);

  // Command Timeout
  joint_command_timeout_sub_ = nh.subscribe("/franka_ros_interface/motion_controller/arm/joint_command_timeout", 1,
                       &MotionControllerInterface::jointCommandTimeoutCallback, this);
  double command_timeout_default;
  nh.param<double>("/controllers_config/command_timeout", command_timeout_default, 0.2);
  auto p_cmd_timeout_length = std::make_shared<ros::Duration>(std::min(1.0,
                                std::max(0.0, command_timeout_default)));
  box_timeout_length_.set(p_cmd_timeout_length);

  ROS_INFO_STREAM("MotionControllerInterface Initialised");

  // Update at 100Hz
  cmd_timeout_timer_ = nh.createTimer(100, &MotionControllerInterface::commandTimeoutCheck, this);
}

void MotionControllerInterface::commandTimeoutCheck(const ros::TimerEvent& e) {
  // lock out other thread(s) which are getting called back via ros.
  std::lock_guard<std::mutex> guard(mtx_);
  // Check Command Timeout
  std::shared_ptr<const ros::Duration>  p_timeout_length;
  box_timeout_length_.get(p_timeout_length);
  std::shared_ptr<const ros::Time>  p_cmd_msg_time;
  box_cmd_timeout_.get(p_cmd_msg_time);
  bool command_timeout = (p_cmd_msg_time && p_timeout_length &&
      ((ros::Time::now() - *p_cmd_msg_time.get()) > (*p_timeout_length.get())));
  if(command_timeout && (current_controller_name_ != default_controller_name_)) {
    // Timeout violated, force robot back to Default Controller Mode

    ROS_WARN_STREAM("MotionControllerInterface: Command timeout violated: Switching to Default control mode." << default_controller_name_);
    switchToDefaultController();
  }
}

bool MotionControllerInterface::switchToDefaultController() {

  std::vector<std::string> start_controllers; 
  std::vector<std::string> stop_controllers;

  for (size_t i = 0; i < all_controllers_.size(); ++i) {

    if (all_controllers_[i] == default_controller_name_)
      start_controllers.push_back(all_controllers_[i]);
    else stop_controllers.push_back(all_controllers_[i]);
  }
  if (!controller_manager_->switchController(start_controllers, stop_controllers,
                              controller_manager_msgs::SwitchController::Request::BEST_EFFORT))
    {
      ROS_ERROR_STREAM_NAMED("MotionControllerInterface", "Failed to switch controllers");
      return false;
    }
    current_controller_name_ = start_controllers[0];
    current_mode_ = controller_name_to_mode_map_[current_controller_name_];
    ROS_INFO_STREAM("MotionControllerInterface: Controller " << start_controllers[0]
                            << " started; Controllers " << stop_controllers[0] <<
                            ", " << stop_controllers[1] <<
                            ", " << stop_controllers[2] <<
                            ", " << stop_controllers[3] <<
                            ", " << stop_controllers[4] <<
			    ", " << stop_controllers[5] <<
                            ", " << stop_controllers[6] << " stopped.");
  return true;
}


void MotionControllerInterface::jointCommandTimeoutCallback(const std_msgs::Float64 msg) {
  ROS_INFO_STREAM("MotionControllerInterface: Joint command timeout: " << msg.data);
  auto p_cmd_timeout_length = std::make_shared<ros::Duration>(
                                std::min(1.0, std::max(0.0, double(msg.data))));
  box_timeout_length_.set(p_cmd_timeout_length);
}

bool MotionControllerInterface::switchControllers(int control_mode) {
  std::vector<std::string> start_controllers;
  std::vector<std::string> stop_controllers;

  if(current_mode_ != control_mode)
  {
    switch (control_mode)
    {
      case franka_core_msgs::JointCommand::POSITION_MODE:
        start_controllers.push_back(joint_position_controller_name_);
        stop_controllers.push_back(joint_impedance_controller_name_);
        stop_controllers.push_back(joint_torque_controller_name_);
        stop_controllers.push_back(joint_velocity_controller_name_);
	stop_controllers.push_back(cartesian_pose_controller_name_);
        stop_controllers.push_back(cartesian_impedance_controller_name_);
        stop_controllers.push_back(cartesian_force_controller_name_);
        stop_controllers.push_back(trajectory_controller_name_);
        break;
      case franka_core_msgs::JointCommand::IMPEDANCE_MODE:
        start_controllers.push_back(joint_impedance_controller_name_);
        stop_controllers.push_back(joint_position_controller_name_);
        stop_controllers.push_back(joint_torque_controller_name_);
        stop_controllers.push_back(joint_velocity_controller_name_);
	stop_controllers.push_back(cartesian_pose_controller_name_);
        stop_controllers.push_back(cartesian_impedance_controller_name_);
        stop_controllers.push_back(cartesian_force_controller_name_);
        stop_controllers.push_back(trajectory_controller_name_);
        break;
      case franka_core_msgs::JointCommand::TORQUE_MODE:
        start_controllers.push_back(joint_torque_controller_name_);
        stop_controllers.push_back(joint_position_controller_name_);
        stop_controllers.push_back(joint_impedance_controller_name_);
        stop_controllers.push_back(joint_velocity_controller_name_);
	stop_controllers.push_back(cartesian_pose_controller_name_);
        stop_controllers.push_back(cartesian_impedance_controller_name_);
        stop_controllers.push_back(cartesian_force_controller_name_);
        stop_controllers.push_back(trajectory_controller_name_);
        break;
      case franka_core_msgs::JointCommand::VELOCITY_MODE:
        start_controllers.push_back(joint_velocity_controller_name_);
        stop_controllers.push_back(joint_position_controller_name_);
        stop_controllers.push_back(joint_impedance_controller_name_);
        stop_controllers.push_back(joint_torque_controller_name_);
	stop_controllers.push_back(cartesian_pose_controller_name_);
        stop_controllers.push_back(cartesian_impedance_controller_name_);
        stop_controllers.push_back(cartesian_force_controller_name_);
        stop_controllers.push_back(trajectory_controller_name_);
        break;        
      default:
        ROS_ERROR_STREAM_NAMED("MotionControllerInterface", "Unknown JointCommand mode "
                                << control_mode << ". Ignoring command.");
        return false;
    }
    if (!controller_manager_->switchController(start_controllers, stop_controllers,
                              controller_manager_msgs::SwitchController::Request::BEST_EFFORT))
    {
      ROS_ERROR_STREAM_NAMED("MotionControllerInterface", "Failed to switch controllers");
      return false;
    }
    current_mode_ = control_mode;
    current_controller_name_ = start_controllers[0];
    ROS_INFO_STREAM("MotionControllerInterface: Controller " << start_controllers[0]
                            << " started; Controllers " << stop_controllers[0] <<
                            ", " << stop_controllers[1] <<
                            ", " << stop_controllers[2] <<
                            ", " << stop_controllers[3] <<
                            ", " << stop_controllers[4] <<
			    ", " << stop_controllers[5] <<
                            ", " << stop_controllers[6] << " stopped.");
  }
  return true;
}

void MotionControllerInterface::jointCommandCallback(const franka_core_msgs::JointCommandConstPtr& msg) {
  // lock out other thread(s) which are getting called back via ros.
  std::lock_guard<std::mutex> guard(mtx_);
  if(switchControllers(msg->mode)) {
    auto p_cmd_msg_time = std::make_shared<ros::Time>(ros::Time::now());
    box_cmd_timeout_.set(p_cmd_msg_time);
  }
}

}
