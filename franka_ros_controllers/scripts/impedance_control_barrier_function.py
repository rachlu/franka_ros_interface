#!/usr/bin/env python

# this is to find out the transform between the webcam frame and robot frame
import numpy as np
import tf.transformations as tfm
import tf2_ros
import rospy
import copy
import pdb
import matplotlib.pyplot as plt
from matplotlib import cm

import ros_helper, franka_helper
from franka_interface import ArmInterface 
from geometry_msgs.msg import TransformStamped, PoseStamped, WrenchStamped
from std_msgs.msg import Float32MultiArray, Float32, Bool
from franka_tools import CollisionBehaviourInterface

from pbal_barrier_controller import PbalBarrierController

def initialize_frame():
    frame_message = TransformStamped()
    frame_message.header.frame_id = "base"
    frame_message.header.stamp = rospy.Time.now()
    frame_message.child_frame_id = "hand_estimate"
    frame_message.transform.translation.x = 0.0
    frame_message.transform.translation.y = 0.0
    frame_message.transform.translation.z = 0.0

    frame_message.transform.rotation.x = 0.0
    frame_message.transform.rotation.y = 0.0
    frame_message.transform.rotation.z = 0.0
    frame_message.transform.rotation.w = 1.0
    return frame_message

def update_frame(frame_pose_stamped, frame_message):
    frame_message.header.stamp = rospy.Time.now()
    frame_message.transform.translation.x = frame_pose_stamped.pose.position.x
    frame_message.transform.translation.y = frame_pose_stamped.pose.position.y
    frame_message.transform.translation.z = frame_pose_stamped.pose.position.z
    frame_message.transform.rotation.x = frame_pose_stamped.pose.orientation.x
    frame_message.transform.rotation.y = frame_pose_stamped.pose.orientation.y
    frame_message.transform.rotation.z = frame_pose_stamped.pose.orientation.z
    frame_message.transform.rotation.w = frame_pose_stamped.pose.orientation.w

def contact2_pose_list(d, s, theta, pivot):

    # sin/cos of line contact orientation
    sint, cost = np.sin(theta), np.cos(theta)
    
    # line contact orientation in world frame
    endpoint_orien_mat = np.array([[-sint, -cost, 0], 
        [0, 0, 1], [-cost, sint, 0]])

    # line contact position in world frame
    endpoint_position = pivot + np.dot(endpoint_orien_mat, 
        np.array([d, s, 0.]))

    # homogenous transform w.r.t world frame
    endpoint_homog = np.vstack([np.vstack([endpoint_orien_mat.T,  
        endpoint_position]).T, np.array([0., 0., 0., 1.])])

    # pose stamped for line contact in world frame
    endpoint_pose = ros_helper.pose_from_matrix(endpoint_homog)

    return ros_helper.pose_stamped2list(endpoint_pose)

def robot2_pose_list(x, z, theta, pivot):

    # sin/cos of line contact orientation
    sint, cost = np.sin(theta), np.cos(theta)
    
    # line contact orientation in world frame
    endpoint_orien_mat = np.array([[-sint, -cost, 0], 
        [0, 0, 1], [-cost, sint, 0]])

    # line contact position in world frame
    endpoint_position = np.array([x, pivot[1], z])

    # homogenous transform w.r.t world frame
    endpoint_homog = np.vstack([np.vstack([endpoint_orien_mat.T,  
        endpoint_position]).T, np.array([0., 0., 0., 1.])])

    # pose stamped for line contact in world frame
    endpoint_pose = ros_helper.pose_from_matrix(endpoint_homog)

    return ros_helper.pose_stamped2list(endpoint_pose)

def world_contact2_pose_list(x, y, theta, pivot):

    # sin/cos of line contact orientation
    sint, cost = np.sin(theta), np.cos(theta)
    
    # line contact orientation in world frame
    endpoint_orien_mat = np.array([[-sint, -cost, 0], 
        [0, 0, 1], [-cost, sint, 0]])

    # line contact position in world frame
    endpoint_position = pivot + np.dot(np.identity(3), 
        np.array([x, y, 0.]))

    # homogenous transform w.r.t world frame
    endpoint_homog = np.vstack([np.vstack([endpoint_orien_mat.T,  
        endpoint_position]).T, np.array([0., 0., 0., 1.])])

    # pose stamped for line contact in world frame
    endpoint_pose = ros_helper.pose_from_matrix(endpoint_homog)


    return ros_helper.pose_stamped2list(endpoint_pose)


def pivot_xyz_callback(data):
    global pivot_xyz
    pivot_xyz =  [data.transform.translation.x,
        data.transform.translation.y,
        data.transform.translation.z]

def generalized_positions_callback(data):
    global generalized_positions
    generalized_positions = data.data

def end_effector_wrench_callback(data):
    global end_effector_wrench
    end_effector_wrench = data 

def object_apriltag_pose_callback(data):
    global object_apriltag_pose
    object_apriltag_pose = data

def robot_friction_coeff_callback(data):
    global robot_friction_coeff
    robot_friction_coeff = data.data

def com_ray_callback(data):
    global theta0
    theta0 = data.data

def gravity_torque_callback(data):
    global mgl
    mgl = data.data


if __name__ == '__main__':

    RATE = 30.
    rospy.init_node("impedance_control_test")
    rate = rospy.Rate(RATE)

    # constants
    LCONTACT = 0.1
    MU_GROUND = 1.0
    MU_CONTACT = 0.1
    IMPEDANCE_STIFFNESS_LIST = [1000, 1000, 1000, 100, 30, 100]
    NMAX = 40. # was 40

    # arm interface
    arm = ArmInterface()
    rospy.sleep(0.5)

    print("Setting collision behaviour")
    collision = CollisionBehaviourInterface()
    rospy.sleep(0.5)
    torque_upper = [40.0, 40.0, 36.0, 36.0, 32.0, 28.0, 24.0] # default [20.0, 20.0, 18.0, 18.0, 16.0, 14.0, 12.0]
    force_upper = [100.0, 100.0, 100.0, 25.0, 25.0, 25.0] # [20.0, 20.0, 20.0, 25.0, 25.0, 25.0]
    collision.set_ft_contact_collision_behaviour(torque_upper=torque_upper, 
        force_upper=force_upper)
    rospy.sleep(1.0)

    pivot_xyz, generalized_positions, end_effector_wrench, object_apriltag_pose \
        , robot_friction_coeff, theta0, mgl = None, None, None, None, None, None, None

    # subscribers
    pivot_xyz_sub = rospy.Subscriber("/pivot_frame", 
        TransformStamped, pivot_xyz_callback)
    generalized_positions_sub = rospy.Subscriber("/generalized_positions", 
        Float32MultiArray,  generalized_positions_callback)
    end_effector_wrench_sub = rospy.Subscriber("/end_effector_sensor_in_end_effector_frame", 
        WrenchStamped,  end_effector_wrench_callback)
    object_apriltag_pose_sub = rospy.Subscriber("/obj_apriltag_pose_in_world_from_camera_publisher", 
        PoseStamped, object_apriltag_pose_callback)
    robot_friction_coeff_sub = rospy.Subscriber("/robot_friction_estimate", 
        Float32, robot_friction_coeff_callback)
    gravity_torque_sub = rospy.Subscriber("/gravity_torque", Float32, gravity_torque_callback)
    com_ray_sub = rospy.Subscriber("/com_ray", Float32, com_ray_callback)


    # setting up publisher
    pivot_sliding_flag_pub = rospy.Publisher('/pivot_sliding_flag', Bool, 
        queue_size=10)
    pivot_sliding_flag_msg = Bool()


    # make sure subscribers are receiving commands
    print("Waiting for pivot estimate to stabilize")
    while pivot_xyz is None:
        rospy.sleep(0.1)

    # make sure subscribers are receiving commands
    print("Waiting for generalized position estimate to stabilize")
    while generalized_positions is None:
        rospy.sleep(0.1)

    # make sure subscribers are receiving commands
    print("Waiting for end effector wrench")
    while end_effector_wrench is None:
        rospy.sleep(0.1)

    print("Waiting for robot_friction_coeff")
    while robot_friction_coeff is None:
        rospy.sleep(0.1)

    print("Waiting for theta0")
    while theta0 is None:
        rospy.sleep(0.1)

    print("Waiting for mgl")
    while mgl is None:
        rospy.sleep(0.1)

    # intialize frame
    frame_message = initialize_frame()
    target_frame_pub = rospy.Publisher('/target_frame', 
        TransformStamped, queue_size=10) 

    # set up transform broadcaster
    target_frame_broadcaster = tf2_ros.TransformBroadcaster()

    # target pose 
    # print(pivot_xyz[0])
    load_initial_config = True
    dt, st, theta_t, delta_t = generalized_positions[0], -0.01, np.pi/5, 10.
    x_piv, z_piv = 0.55, pivot_xyz[2]
    integral_multiplier = 5
    
    tagret_pose_contact_frame = np.array([dt, st, theta_t])
    target_pivot_xz = np.array([x_piv, z_piv])

    mode = -1

    if mode == 2 or mode == 3:
        pivot_sliding_flag = True
    else:
        pivot_sliding_flag = False

    # build barrier function model
    obj_params = dict()
    obj_params['pivot'] = np.array([pivot_xyz[0], pivot_xyz[2]])
    obj_params['mgl'] = mgl
    obj_params['theta0'] = theta0
    obj_params['mu_contact'] = MU_CONTACT
    obj_params['mu_ground'] = MU_GROUND
    obj_params['l_contact'] = LCONTACT

    theta_mult = 10.
    x_pivot_mult = 200.

    # impedance parameters
    param_dict = dict()
    param_dict['obj_params'] = obj_params
    param_dict['K_theta'] = 30.0*theta_mult
    param_dict['K_s'] = 1.
    param_dict['K_x_pivot'] = 3. * (3.) * (0.03/5.) * 10. * x_pivot_mult
    param_dict['trust_region'] = np.array([1., 1., 3., 3., 1., 1., 1.,1.])
    param_dict['concavity_rotating'] = 6.*theta_mult
    param_dict['concavity_sliding'] = 0.3
    param_dict['concavity_x_sliding'] = .3*x_pivot_mult
    param_dict['regularization_constant'] = 0.00001
    param_dict['torque_margin'] = 0.002 * NMAX
    param_dict['s_scale'] = 1/2000.
    param_dict['x_piv_scale'] = 0.03/5.

    # create inverse model
    pbc = PbalBarrierController(param_dict)

    # initial impedance target
    if load_initial_config:
        impedance_target = []

        # open file and read the content in a list
        with open('final_impedance_pose.txt', 'r') as filehandle:
            for line in filehandle:            
                currentPlace = float(line[:-1]) 
                impedance_target.append(currentPlace)

        impedance_target = np.array(impedance_target)
    else:

        impedance_target_contact = generalized_positions
        impedance_target = pbc.pbal_helper.forward_kin(
            impedance_target_contact)
    
    # make pose to send to franka
    impedance_target_6D_list = robot2_pose_list(impedance_target[0],
        impedance_target[1],
        impedance_target[2], 
        pivot_xyz)

    impedance_target_pose = franka_helper.list2franka_pose(
        impedance_target_6D_list)


    arm.set_cart_impedance_pose(impedance_target_pose,
                stiffness=IMPEDANCE_STIFFNESS_LIST)

    rospy.sleep(1.0)

    # initialize lists for plotting
    t_list = []
    target_pose_list = []
    contact_pose_list = []
    delta_contact_pose_list = []
    wrench_increment_contact_list = []
    measured_wrench_contact_list = []
    impedance_target_list = []
    pivot_xz_list = []
    target_pivot_xz_list = []

    start_time = rospy.Time.now().to_sec()
    print('starting control loop')
    while not rospy.is_shutdown():

            # current time
            t = rospy.Time.now().to_sec() - start_time

            # exit condition
            if t > 1.5 * delta_t:
                break

            # publish if we intend to slide at pivot
            pivot_sliding_flag_msg.data = pivot_sliding_flag
            pivot_sliding_flag_pub.publish(pivot_sliding_flag_msg)

            # update estimated values in controller
            pbc.pivot = np.array([pivot_xyz[0], pivot_xyz[2]])
            pbc.mgl = mgl
            pbc.theta0 = theta0
            pbc.mu_contact = MU_CONTACT #robot_friction_coeff #max(0.1, robot_friction_coeff)

            print(pbc.mu_contact)
            
            # snapshot of current generalized position estimate
            contact_pose = copy.deepcopy(generalized_positions)

            # compute error
            delta_contact_pose = tagret_pose_contact_frame - contact_pose
            delta_pivot_pose = target_pivot_xz - pbc.pivot

            # measured contact wrench
            measured_contact_wench_6D = ros_helper.wrench_stamped2list(end_effector_wrench)
            measured_contact_wrench = -np.array([
                measured_contact_wench_6D[0], 
                measured_contact_wench_6D[1],
                measured_contact_wench_6D[-1]])

            # compute wrench increment          
            try:
                wrench_increment_contact = pbc.solve_qp(measured_contact_wrench, \
                    contact_pose, delta_contact_pose, delta_pivot_pose, mode, NMAX)           
            except Exception as e:
                print("couldn't find solution")
                break

            # convert wrench to robot frame
            contact2robot = pbc.pbal_helper.contact2robot(contact_pose)
            wrench_increment_robot = np.dot(contact2robot, 
                wrench_increment_contact)

            # compute impedance increment
            impedance_increment_robot = wrench_increment_robot / np.array(
                IMPEDANCE_STIFFNESS_LIST)[[0, 2, 4]]
            impedance_target += integral_multiplier * impedance_increment_robot / RATE            

            # make pose to send to franka
            waypoint_pose_list = robot2_pose_list(impedance_target[0],
                impedance_target[1],
                impedance_target[2], 
                pivot_xyz)

            waypoint_franka_pose = franka_helper.list2franka_pose(
                waypoint_pose_list)

            # send command to franka
            arm.set_cart_impedance_pose(waypoint_franka_pose,
                stiffness=IMPEDANCE_STIFFNESS_LIST)

            # pubish target frame
            update_frame(ros_helper.list2pose_stamped(waypoint_pose_list), 
                frame_message)
            target_frame_pub.publish(frame_message)
            target_frame_broadcaster.sendTransform(frame_message)

            # store time
            t_list.append(t)

            # store end-effector pose
            target_pose_list.append(tagret_pose_contact_frame)
            contact_pose_list.append(contact_pose)
            delta_contact_pose_list.append(delta_contact_pose)

            target_pivot_xz_list.append(target_pivot_xz)
            pivot_xz_list.append(pbc.pivot)

            # store wrenches
            wrench_increment_contact_list.append(wrench_increment_contact)
            measured_wrench_contact_list.append(measured_contact_wrench)

            # store impedances
            impedance_target_copy = copy.deepcopy(impedance_target)
            impedance_target_list.append(impedance_target_copy)

            # print(impedance_target)
            print(np.concatenate([contact_pose, pbc.pivot]))

            rate.sleep()

    print('control loop completed')

    # terminate rosbags
    with open('final_impedance_pose.txt', 'w') as filehandle:
        for listitem in impedance_target:
                filehandle.write('%s\n' % listitem)
    # ros_helper.terminate_rosbag()

    # unsubscribe from topics
    pivot_xyz_sub.unregister()
    generalized_positions_sub.unregister()
    end_effector_wrench_sub.unregister()
    object_apriltag_pose_sub.unregister()
    robot_friction_coeff_sub.unregister()

    # convert to array
    t_array = np.array(t_list)
    target_pose_array = np.array(target_pose_list)
    contact_pose_array = np.array(contact_pose_list)
    delta_contact_pose_array = np.array(delta_contact_pose_list)
    wrench_increment_contact_array = np.array(
        wrench_increment_contact_list)
    measured_wrench_contact_array = np.array(measured_wrench_contact_list)
    impedance_target_array = np.array(impedance_target_list)
    pivot_xz_array = np.array(pivot_xz_list)
    target_pivot_xz_array = np.array(target_pivot_xz_list)


    labels = ['d', 's', 'theta', 'px', 'pz']
    fig, ax = plt.subplots(5,1)
    for i in range(5):
        if i < 3:
            ax[i].plot(t_array, target_pose_array[:, i], 'k', label='target')
            ax[i].plot(t_array, contact_pose_array[:, i], 'b', label='measured')
        else:
            ax[i].plot(t_array, target_pivot_xz_array[:, i-3], 'k', label='target')
            ax[i].plot(t_array, pivot_xz_array[:, i-3], 'b', label='measured')
        ax[i].set_ylabel(labels[i])
        # ax[i].legend()

    ax[3].plot(t_array, impedance_target_array[:, 0], 'r')
    # print("hello")
    # print(impedance_target_array)


    print("plotting")
    start = 0.0
    stop = 1.0
    number_of_lines = len(t_list)
    cm_subsection = np.linspace(start, stop, number_of_lines) 
    colors = [cm.jet(x) for x in cm_subsection]

    fig2, axs2 = plt.subplots(1, 3)
    axs2 = pbc.plot_boundaries(axs2, contact_pose_list[0], 
        measured_wrench_contact_list[0], NMAX)

    for contact_pose, delta_contact_pose, measured_wrench, delta_wrench, color in zip(
        contact_pose_list, delta_contact_pose_list, 
        measured_wrench_contact_list, wrench_increment_contact_list, colors):

        axs2 = pbc.plot_projections(axs2, contact_pose, 5 * delta_wrench / RATE, 
            measured_wrench, NMAX, color, False)

        # axs2 = pbc.plot_cost_function_projection(axs2, contact_pose, 
        #     delta_contact_pose, measured_wrench, mode, color)


    fig3, axs3 = plt.subplots(1, 3)
    axs3 = pbc.plot_boundaries(axs3, contact_pose_list[0], 
        measured_wrench_contact_list[0], NMAX)

    axs3[0].plot(measured_wrench_contact_array[:, 1], measured_wrench_contact_array[:, 0])
    axs3[1].plot(measured_wrench_contact_array[:, 2], measured_wrench_contact_array[:, 0])
    # ax3[0].plot(measured_wrench_contact_array[:, 2], measured_wrench_contact_array[:, 0])


    plt.show()
