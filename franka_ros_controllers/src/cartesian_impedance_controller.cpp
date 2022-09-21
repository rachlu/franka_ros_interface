// Copyright (c) 2017 Franka Emika GmbH
// Use of this source code is governed by the Apache-2.0 license, see LICENSE
#include <franka_ros_controllers/cartesian_impedance_controller.h>

#include <iostream>
#include <cmath>
#include <memory>

#include <controller_interface/controller_base.h>
#include <franka/robot_state.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>

#include "pseudo_inversion.h"

namespace franka_ros_controllers {

bool CartesianImpedanceController::init(hardware_interface::RobotHW* robot_hw,
                                        ros::NodeHandle& node_handle) {
  std::vector<double> cartesian_stiffness_vector;
  std::vector<double> cartesian_damping_vector;

  sub_equilibrium_pose_ = node_handle.subscribe(
      "/equilibrium_pose", 20, &CartesianImpedanceController::equilibriumPoseCallback, this,
      ros::TransportHints().reliable().tcpNoDelay());
  stiffness_params_ = node_handle.subscribe(
      "/impedance_stiffness", 20, &CartesianImpedanceController::stiffnessParamCallback, this,
      ros::TransportHints().reliable().tcpNoDelay());
 
  std::string arm_id;
  if (!node_handle.getParam("arm_id", arm_id)) {
    ROS_ERROR_STREAM("CartesianImpedanceController: Could not read parameter arm_id");
    return false;
  }
  std::vector<std::string> joint_names;
  if (!node_handle.getParam("joint_names", joint_names) || joint_names.size() != 7) {
    ROS_ERROR(
        "CartesianImpedanceController: Invalid or no joint_names parameters provided, "
        "aborting controller init!");
    return false;
  }
  if (!node_handle.getParam("stiffness_gains", stiffness_gains_) || stiffness_gains_.size() != 6) {
    ROS_ERROR(
        "CartesianImpedanceController:  Invalid or no cartesian_stiffness_ parameters provided, aborting "
        "controller init!");
    return false;
  }

  auto* model_interface = robot_hw->get<franka_hw::FrankaModelInterface>();
  if (model_interface == nullptr) {
    ROS_ERROR_STREAM(
        "CartesianImpedanceController: Error getting model interface from hardware");
    return false;
  }
  try {
    model_handle_ = std::make_unique<franka_hw::FrankaModelHandle>(
        model_interface->getHandle(arm_id + "_model"));
  } catch (hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM(
        "CartesianImpedanceController: Exception getting model handle from interface: "
        << ex.what());
    return false;
  }

  auto* state_interface = robot_hw->get<franka_hw::FrankaStateInterface>();
  if (state_interface == nullptr) {
    ROS_ERROR_STREAM(
        "CartesianImpedanceController: Error getting state interface from hardware");
    return false;
  }
  try {
    state_handle_ = std::make_unique<franka_hw::FrankaStateHandle>(
        state_interface->getHandle(arm_id + "_robot"));
  } catch (hardware_interface::HardwareInterfaceException& ex) {
    ROS_ERROR_STREAM(
        "CartesianImpedanceController: Exception getting state handle from interface: "
        << ex.what());
    return false;
  }

  auto* effort_joint_interface = robot_hw->get<hardware_interface::EffortJointInterface>();
  if (effort_joint_interface == nullptr) {
    ROS_ERROR_STREAM(
        "CartesianImpedanceController: Error getting effort joint interface from hardware");
    return false;
  }
  for (size_t i = 0; i < 7; ++i) {
    try {
      joint_handles_.push_back(effort_joint_interface->getHandle(joint_names[i]));
    } catch (const hardware_interface::HardwareInterfaceException& ex) {
      ROS_ERROR_STREAM(
          "CartesianImpedanceController: Exception getting joint handles: " << ex.what());
      return false;
    }
  }

  cartesian_stiffness_target_.setIdentity();
  cartesian_damping_target_.setIdentity();
  nullspace_stiffness_target_ = 0;
  for (size_t i = 0; i < 6; ++i) { 
    cartesian_stiffness_target_(i,i) = stiffness_gains_[i];
    cartesian_damping_target_(i,i) = 2.0 * sqrt(stiffness_gains_[i]);
  }
  

  position_d_.setZero();
  orientation_d_.coeffs() << 0.0, 0.0, 0.0, 1.0;
  position_d_target_.setZero();
  orientation_d_target_.coeffs() << 0.0, 0.0, 0.0, 1.0;

  cartesian_stiffness_.setZero();
  cartesian_damping_.setZero();

  return true;
}

void CartesianImpedanceController::starting(const ros::Time& /*time*/) {
  // compute initial velocity with jacobian and set x_attractor and q_d_nullspace
  // to initial configuration
  franka::RobotState initial_state = state_handle_->getRobotState();
  // get jacobian
  std::array<double, 42> jacobian_array =
      model_handle_->getZeroJacobian(franka::Frame::kEndEffector);
  // convert to eigen
  Eigen::Map<Eigen::Matrix<double, 6, 7>> jacobian(jacobian_array.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1>> dq_initial(initial_state.dq.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1>> q_initial(initial_state.q.data());
  Eigen::Affine3d initial_transform(Eigen::Matrix4d::Map(initial_state.O_T_EE.data()));

  // set equilibrium point to current state
  position_d_ = initial_transform.translation();
  orientation_d_ = Eigen::Quaterniond(initial_transform.linear());
  position_d_target_ = initial_transform.translation();
  orientation_d_target_ = Eigen::Quaterniond(initial_transform.linear());

  // set nullspace equilibrium configuration to initial q
  q_d_nullspace_ = q_initial;
}

void CartesianImpedanceController::update(const ros::Time& /*time*/,
                                                 const ros::Duration& /*period*/) {
  // get state variables
  franka::RobotState robot_state = state_handle_->getRobotState();
  std::array<double, 7> coriolis_array = model_handle_->getCoriolis();
  std::array<double, 42> jacobian_array =
      model_handle_->getZeroJacobian(franka::Frame::kEndEffector);

  // convert to Eigen
  Eigen::Map<Eigen::Matrix<double, 7, 1>> coriolis(coriolis_array.data());
  Eigen::Map<Eigen::Matrix<double, 6, 7>> jacobian(jacobian_array.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1>> q(robot_state.q.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1>> dq(robot_state.dq.data());
  Eigen::Map<Eigen::Matrix<double, 7, 1>> tau_J_d(  // NOLINT (readability-identifier-naming)
      robot_state.tau_J_d.data());
  Eigen::Affine3d transform(Eigen::Matrix4d::Map(robot_state.O_T_EE.data()));
  Eigen::Vector3d position(transform.translation());
  Eigen::Quaterniond orientation(transform.linear());

  // compute error to desired pose
  // position error
  Eigen::Matrix<double, 6, 1> error;
  error.head(3) << position - position_d_;

  // orientation error
  if (orientation_d_.coeffs().dot(orientation.coeffs()) < 0.0) {
    orientation.coeffs() << -orientation.coeffs();
  }
  // "difference" quaternion
  Eigen::Quaterniond error_quaternion(orientation * orientation_d_.inverse());
  // convert to axis angle
  Eigen::AngleAxisd error_quaternion_angle_axis(error_quaternion);
  // compute "orientation error"
  error.tail(3) << error_quaternion_angle_axis.axis() * error_quaternion_angle_axis.angle();

  // compute control
  // allocate variables
  Eigen::VectorXd tau_task(7), tau_nullspace(7), tau_d(7);

  // pseudoinverse for nullspace handling
  // kinematic pseuoinverse
  Eigen::MatrixXd jacobian_transpose_pinv;
  pseudoInverse(jacobian.transpose(), jacobian_transpose_pinv);

  // Cartesian PD control with damping ratio = 1
  tau_task << jacobian.transpose() *
                  (-cartesian_stiffness_ * error - cartesian_damping_ * (jacobian * dq));
  // nullspace PD control with damping ratio = 1
  tau_nullspace << (Eigen::MatrixXd::Identity(7, 7) -
                    jacobian.transpose() * jacobian_transpose_pinv) *
                       (nullspace_stiffness_ * (q_d_nullspace_ - q) -
                        (2.0 * sqrt(nullspace_stiffness_)) * dq);
  // Desired torque
  tau_d << tau_task + tau_nullspace + coriolis;
  // Saturate torque rate to avoid discontinuities
  tau_d << saturateTorqueRate(tau_d, tau_J_d);
  for (size_t i = 0; i < 7; ++i) {
    joint_handles_[i].setCommand(tau_d(i));
  }

  // update parameters changed online either through dynamic reconfigure or through the interactive
  // target by filtering
  cartesian_stiffness_ = filter_params_ * cartesian_stiffness_target_ + (1.0 - filter_params_) * cartesian_stiffness_;
  cartesian_damping_ = filter_params_ * cartesian_damping_target_ + (1.0 - filter_params_) * cartesian_damping_;
  nullspace_stiffness_ = filter_params_ * nullspace_stiffness_target_ + (1.0 - filter_params_) * nullspace_stiffness_;
  position_d_ = filter_params_ * position_d_target_ + (1.0 - filter_params_) * position_d_;
  Eigen::AngleAxisd aa_orientation_d(orientation_d_);
  Eigen::AngleAxisd aa_orientation_d_target(orientation_d_target_);
  aa_orientation_d.axis() = filter_params_ * aa_orientation_d_target.axis() + (1.0 - filter_params_) * aa_orientation_d.axis();
  aa_orientation_d.angle() = filter_params_ * aa_orientation_d_target.angle() + (1.0 - filter_params_) * aa_orientation_d.angle();
  orientation_d_ = Eigen::Quaterniond(aa_orientation_d);
}

Eigen::Matrix<double, 7, 1> CartesianImpedanceController::saturateTorqueRate(
    const Eigen::Matrix<double, 7, 1>& tau_d_calculated,
    const Eigen::Matrix<double, 7, 1>& tau_J_d) {  // NOLINT (readability-identifier-naming)
  Eigen::Matrix<double, 7, 1> tau_d_saturated{};
  for (size_t i = 0; i < 7; i++) {
    double difference = tau_d_calculated[i] - tau_J_d[i];
    tau_d_saturated[i] = tau_J_d[i] + std::max(std::min(difference, delta_tau_max_), -delta_tau_max_);
  }
  return tau_d_saturated;
}

void CartesianImpedanceController::stiffnessParamCallback(
     const franka_core_msgs::CartImpedanceStiffness& msg) {

  cartesian_stiffness_target_.setIdentity();
  cartesian_damping_target_.setIdentity(); // Damping ratio = 1
  //nullspace_stiffness_target_ = config.nullspace_stiffness; TODO


  if(msg.use_flag == 0)
  {

    cartesian_stiffness_target_(0,0) = msg.x;
    cartesian_stiffness_target_(1,1) = msg.y;
    cartesian_stiffness_target_(2,2) = msg.z;
    cartesian_stiffness_target_(3,3) = msg.xrot;
    cartesian_stiffness_target_(4,4) = msg.yrot;
    cartesian_stiffness_target_(5,5) = msg.zrot;
    // Updated by Orion and Neel to reduce damping (12/13/21)
    if(msg.bx == -1.)
    {
   
      cartesian_damping_target_(0,0) = .5 * sqrt(msg.x);
      cartesian_damping_target_(1,1) = .5 * sqrt(msg.y);
      cartesian_damping_target_(2,2) = .5 * sqrt(msg.z);
      cartesian_damping_target_(3,3) = .5 * sqrt(msg.xrot);
      cartesian_damping_target_(4,4) = .5 * sqrt(msg.yrot);
      cartesian_damping_target_(5,5) = .5 * sqrt(msg.zrot);  
      // cartesian_damping_target_(0,0) = 2.0 * sqrt(msg.x);
      // cartesian_damping_target_(1,1) = 2.0 * sqrt(msg.y);
      // cartesian_damping_target_(2,2) = 2.0 * sqrt(msg.z);
      // cartesian_damping_target_(3,3) = 2.0 * sqrt(msg.xrot);
      // cartesian_damping_target_(4,4) = 2.0 * sqrt(msg.yrot);
      // cartesian_damping_target_(5,5) = 2.0 * sqrt(msg.zrot);
    }
    else
    {
      cartesian_damping_target_(0,0) = msg.bx;
      cartesian_damping_target_(1,1) = msg.by;
      cartesian_damping_target_(2,2) = msg.bz;
      cartesian_damping_target_(3,3) = msg.bxrot;
      cartesian_damping_target_(4,4) = msg.byrot;
      cartesian_damping_target_(5,5) = msg.bzrot;
    }

  }
  else
  {

    cartesian_stiffness_target_(0,0) = msg.xx;
    cartesian_stiffness_target_(0,1) = msg.xy;
    cartesian_stiffness_target_(0,2) = msg.xz;
    cartesian_stiffness_target_(1,0) = msg.yx;
    cartesian_stiffness_target_(1,1) = msg.yy;
    cartesian_stiffness_target_(1,2) = msg.yz;
    cartesian_stiffness_target_(2,0) = msg.zx;
    cartesian_stiffness_target_(2,1) = msg.zy;
    cartesian_stiffness_target_(2,2) = msg.zz;
    cartesian_stiffness_target_(3,3) = msg.xxrot;
    cartesian_stiffness_target_(3,4) = msg.xyrot;
    cartesian_stiffness_target_(3,5) = msg.xzrot;
    cartesian_stiffness_target_(4,3) = msg.yxrot;
    cartesian_stiffness_target_(4,4) = msg.yyrot;
    cartesian_stiffness_target_(4,5) = msg.yzrot;
    cartesian_stiffness_target_(5,3) = msg.zxrot;
    cartesian_stiffness_target_(5,4) = msg.zyrot;
    cartesian_stiffness_target_(5,5) = msg.zzrot;

    cartesian_damping_target_(0,0) = msg.bxx;
    cartesian_damping_target_(0,1) = msg.bxy;
    cartesian_damping_target_(0,2) = msg.bxz;
    cartesian_damping_target_(1,0) = msg.byx;
    cartesian_damping_target_(1,1) = msg.byy;
    cartesian_damping_target_(1,2) = msg.byz;
    cartesian_damping_target_(2,0) = msg.bzx;
    cartesian_damping_target_(2,1) = msg.bzy;
    cartesian_damping_target_(2,2) = msg.bzz;
    cartesian_damping_target_(3,3) = msg.bxxrot;
    cartesian_damping_target_(3,4) = msg.bxyrot;
    cartesian_damping_target_(3,5) = msg.bxzrot;
    cartesian_damping_target_(4,3) = msg.byxrot;
    cartesian_damping_target_(4,4) = msg.byyrot;
    cartesian_damping_target_(4,5) = msg.byzrot;
    cartesian_damping_target_(5,3) = msg.bzxrot;
    cartesian_damping_target_(5,4) = msg.bzyrot;
    cartesian_damping_target_(5,5) = msg.bzzrot;


  }





}

void CartesianImpedanceController::equilibriumPoseCallback(
    const geometry_msgs::PoseStampedConstPtr& msg) {
  position_d_target_ << msg->pose.position.x, msg->pose.position.y, msg->pose.position.z;
  Eigen::Quaterniond last_orientation_d_target(orientation_d_target_);
  orientation_d_target_.coeffs() << msg->pose.orientation.x, msg->pose.orientation.y,
      msg->pose.orientation.z, msg->pose.orientation.w;
  if (last_orientation_d_target.coeffs().dot(orientation_d_target_.coeffs()) < 0.0) {
    orientation_d_target_.coeffs() << -orientation_d_target_.coeffs();
  }
}

}  // namespace franka_ros_controllers

PLUGINLIB_EXPORT_CLASS(franka_ros_controllers::CartesianImpedanceController,
                       controller_interface::ControllerBase)
