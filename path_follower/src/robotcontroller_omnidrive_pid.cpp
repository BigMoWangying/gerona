#include "robotcontroller_omnidrive_pid.h"

/// PROJECT
#include <utils_general/Line2d.h>
#include <utils_general/MathHelper.h>
#include "BehaviouralPathDriver.h"
#include "pathfollower.h"
#include "behaviours.h"

using namespace Eigen;

RobotController_Omnidrive_Pid::RobotController_Omnidrive_Pid(ros::Publisher &cmd_publisher,
                                                             BehaviouralPathDriver *path_driver):
    RobotController(cmd_publisher, path_driver),
    pids_(2)
{
    visualizer_ = Visualizer::getInstance();

    configure();
}

void RobotController_Omnidrive_Pid::publishCommand()
{
    if (!cmd_.isValid()) {
        ROS_FATAL("Invalid Command in RobotController_Omnidrive_Pid.");
        return;
    }

    geometry_msgs::Twist msg = cmd_;
    cmd_pub_.publish(msg);

    setFilteredSpeed(cmd_.speed);
}

void RobotController_Omnidrive_Pid::stopMotion()
{
    cmd_.speed = 0;
    cmd_.direction_angle = 0;
    cmd_.rotation = 0;

    publishCommand();
}

void RobotController_Omnidrive_Pid::initOnLine()
{
    pids_.resetAll();
}

void RobotController_Omnidrive_Pid::behaveOnLine()
{
    // Calculate target line from current to next waypoint (if there is any)
    double e_direction = calculateDirectionError();
    double e_angle     = calculateAngleError();

    ROS_DEBUG("OnLine: e_dir = %g, e_angle = %g", e_direction, e_angle);

    if (visualizer_->hasSubscriber()) {
        visualizer_->drawSteeringArrow(1, path_driver_->getSlamPoseMsg(), e_angle, 0.2, 1.0, 0.2);
        visualizer_->drawSteeringArrow(2, path_driver_->getSlamPoseMsg(), e_direction, 0.2, 0.2, 1.0);
    }

    //TODO: speed control!

    if(setCommand(e_direction, e_angle, velocity_)) {
        // do not use this behaviour here, for the moment.
        //setStatus(path_msgs::FollowPathResult::MOTION_STATUS_MOVING);
        //throw new BehaviourAvoidObstacle(*path_driver_);
    }
}

void RobotController_Omnidrive_Pid::behaveAvoidObstacle()
{
    // not used
}

bool RobotController_Omnidrive_Pid::behaveApproachTurningPoint()
{
    // Check if done
    if (checkIfTurningPointApproached()) {
        return true;
    }

    // Calculate target line from current to next waypoint (if there is any)
    //double e_distance = calculateDistanceToWaypoint();
    double e_direction = calculateDirectionError();
    double e_angle     = calculateAngleError();

    ROS_DEBUG("Approach: e_dir = %g, e_angle = %g", e_direction, e_angle);

    if (visualizer_->hasSubscriber()) {
        visualizer_->drawCircle(2, ((geometry_msgs::Pose) path_.nextWaypoint()).position, 0.5, "/map", "turning point", 1, 1, 1);
        visualizer_->drawSteeringArrow(1, path_driver_->getSlamPoseMsg(), e_angle, 0.2, 1.0, 0.2);
        visualizer_->drawSteeringArrow(2, path_driver_->getSlamPoseMsg(), e_direction, 0.2, 0.2, 1.0);
    }

    float distance = std::sqrt(next_wp_local_.dot(next_wp_local_));
    float velocity = std::max(0.1f + distance / 2.0f, path_driver_->getOptions().min_velocity_);

    setCommand(e_direction, e_angle, velocity);

    return false;
}

void RobotController_Omnidrive_Pid::configure()
{
    ros::NodeHandle nh("~");

    nh.param( "dead_time", options_.dead_time_, 0.10 );

    double ta, kp, ki, i_max, delta_max, e_max;
    nh.param( "pid/ta", ta, 0.03 );
    nh.param( "pid/kp", kp, 1.5 );
    nh.param( "pid/ki", ki, 0.0 );
    nh.param( "pid/i_max", i_max, 0.0 );
    nh.param( "pid/delta_max", delta_max, 30.0 );
    nh.param( "pid/e_max", e_max, 0.10 );

    //FIXME: separate parameters for the controlers...
    pids_.setTa(ta);
    pids_.configure(DIRECTION, kp, M_PI*delta_max/180.0, e_max, 0.5, ki, i_max);
    pids_.configure(ORIENTATION, kp, M_PI*delta_max/180.0, e_max, 0.5, ki, i_max);
}

bool RobotController_Omnidrive_Pid::checkIfTurningPointApproached()
{
    Waypoint next_wp = path_.nextWaypoint();

    //! Difference of current robot pose to the next waypoint.
    Vector2d delta;
    delta << next_wp.x - path_driver_->getSlamPoseMsg().position.x,
             next_wp.y - path_driver_->getSlamPoseMsg().position.y;

//    if (controller_->getDirSign() < 0) {
//        delta *= -1;
//    }

    //! Unit vector pointing in the direction of the next waypoints orientation.
    Vector2d target_dir;
    target_dir << std::cos(next_wp.orientation), std::sin(next_wp.orientation);

    // atan2(y,x) = angle of the vector.
    //! Angle between the line from robot to waypoint and the waypoints orientation (only used for output?)
    double angle = MathHelper::AngleClamp(std::atan2(delta(1), delta(0)) - std::atan2(target_dir(1), target_dir(0)));

    ROS_DEBUG_STREAM_THROTTLE(1, "angle = " << angle);

    //        bool done = std::abs(angle) >= M_PI / 2;
    return delta.dot(target_dir) < 0;  // done, if angle is greater than 90°?!
}

bool RobotController_Omnidrive_Pid::setCommand(double e_direction, double e_rotation, float speed)
{
    BehaviouralPathDriver::Options path_driver_opt = path_driver_->getOptions();
    BehaviourDriveBase* behaviour = ((BehaviourDriveBase*) path_driver_->getActiveBehaviour());

    setStatus(path_msgs::FollowPathResult::MOTION_STATUS_MOVING);

    // FIXME: Synchronize timers of the controllers
    double errors[] = {e_direction, e_rotation};
    vector<double> deltas;
    if (!pids_.execute(errors, deltas)) {
        // Nothing to do
        return false;
    }
    double delta_dir = deltas[DIRECTION];
    double delta_rot = deltas[ORIENTATION];
    ROS_DEBUG("PID-Direction: error = %g,\t delta = %g", e_direction, delta_dir);
    ROS_DEBUG("PID-Rotation:  error = %g,\t delta = %g", e_rotation, delta_rot);

    visualizer_->drawSteeringArrow(14, path_driver_->getSlamPoseMsg(), delta_dir, 0.0, 1.0, 1.0);


    bool collision = false;
    //TODO: VFH here


    double steer = std::abs(delta_dir);
    //ROS_DEBUG_STREAM("dir=" << dir_sign_ << ", steer=" << steer);
    if(steer > path_driver_opt.steer_slow_threshold_) {
        ROS_WARN_STREAM_THROTTLE(2, "slowing down");
        speed *= 0.5;
    }

    // make sure, the speed is in the allowed range
    if (speed < path_driver_opt.min_velocity_) {
        speed = path_driver_opt.min_velocity_;
        ROS_WARN_THROTTLE(5, "Velocity is below minimum. It is set to minimum velocity.");
    } else if (speed > path_driver_opt.max_velocity_) {
        speed = path_driver_opt.max_velocity_;
        ROS_WARN_THROTTLE(5, "Velocity is above maximum. Reduce to maximum velocity.");
    }
    //TODO: bounds for rotational speed?


    cmd_.direction_angle = delta_dir;
    cmd_.rotation = delta_rot;


    collision |= behaviour->isCollision(cmd_.direction_angle);

    if(collision) {
        ROS_WARN_THROTTLE(1, "Collision!");
        setStatus(path_msgs::FollowPathResult::MOTION_STATUS_COLLISION); //FIXME: not so good to use result-constant if it is not finishing the action...

        stopMotion();
    } else {
        cmd_.speed = speed;
    }

    ROS_DEBUG("Set speed to %g", cmd_.speed);
    return false; // return true here, if VFH is at work?! (compare to ackermann_pid)
}

Eigen::Vector2d RobotController_Omnidrive_Pid::predictPosition()
{
//    double dt = options_.dead_time_;
//    double v  = getFilteredSpeed();
//    double ds = dt*v;

    //FIXME: do real prediction here

    return path_driver_->getSlamPose().head<2>();
}

double RobotController_Omnidrive_Pid::calculateLineError()
{
    // control to the path segment after the current one -> determine next but one waypoint
    geometry_msgs::PoseStamped followup_next_wp_map;
    followup_next_wp_map.header.stamp = ros::Time::now();
    // use the current segment, if it is the last one
    if(path_.wp_idx + 1 == path_.current_path->size()) {
        followup_next_wp_map.pose = path_.getWaypoint(path_.wp_idx - 1);
    } else {
        followup_next_wp_map.pose = path_.getWaypoint(path_.wp_idx + 1);
    }

    // transform this waypoint to local frame
    Vector3d followup_next_wp_local;
    if (!path_driver_->getNode()->transformToLocal( followup_next_wp_map, followup_next_wp_local)) {
        setStatus(path_msgs::FollowPathResult::MOTION_STATUS_INTERNAL_ERROR);
        throw new BehaviourEmergencyBreak(*path_driver_);
    }

    // segment line
    Line2d target_line;
    target_line = Line2d( next_wp_local_.head<2>(), followup_next_wp_local.head<2>());
    visualizer_->visualizeLine(target_line);

    Vector2d pred_position = predictPosition();
    //visualizer_->drawMark(0, );

    return -target_line.GetSignedDistance(pred_position);
}

double RobotController_Omnidrive_Pid::calculateDirectionError()
{
    Vector2d vec_to_wp = next_wp_local_.head<2>();
    double angle = atan2(vec_to_wp(1), vec_to_wp(0));

    return angle;
}

double RobotController_Omnidrive_Pid::calculateDistanceToWaypoint()
{
    Vector2d pred_pos = predictPosition();
    Vector2d distance = next_wp_local_.head<2>() - pred_pos;

    //FIXME: this is wrong?!
    if(std::abs(distance(1)) < 0.1) {
        return 0;
    }

    return distance(1);
}