/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2009, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Eitan Marder-Eppstein
*********************************************************************/

#include <hanp_local_planner/hanp_local_planner.h>

#include <cmath>
#include <queue>

#include <base_local_planner/goal_functions.h>
#include <base_local_planner/map_grid_cost_point.h>

#include <pcl_conversions/pcl_conversions.h>
#include <ros/console.h>
#include <pluginlib/class_list_macros.h>
#include <base_local_planner/goal_functions.h>
#include <nav_msgs/Path.h>

PLUGINLIB_EXPORT_CLASS(hanp_local_planner::HANPLocalPlanner, nav_core::BaseLocalPlanner)

namespace hanp_local_planner
{
    void HANPLocalPlanner::reconfigureCB(HANPLocalPlannerConfig &config, uint32_t level)
    {
        boost::mutex::scoped_lock l(configuration_mutex_);

        if (setup_ && config.restore_defaults)
        {
            config = default_config_;
            config.restore_defaults = false;
        }
        if ( ! setup_)
        {
            default_config_ = config;
            setup_ = true;
        }

        base_local_planner::LocalPlannerLimits limits;
        limits.max_trans_vel = config.max_trans_vel;
        limits.min_trans_vel = config.min_trans_vel;
        limits.max_vel_x = config.max_vel_x;
        limits.min_vel_x = config.min_vel_x;
        limits.max_vel_y = config.max_vel_y;
        limits.min_vel_y = config.min_vel_y;
        limits.max_rot_vel = config.max_rot_vel;
        limits.min_rot_vel = config.min_rot_vel;
        limits.acc_lim_x = config.acc_lim_x;
        limits.acc_lim_y = config.acc_lim_y;
        limits.acc_lim_theta = config.acc_lim_theta;
        limits.acc_limit_trans = config.acc_limit_trans;
        limits.xy_goal_tolerance = config.xy_goal_tolerance;
        limits.yaw_goal_tolerance = config.yaw_goal_tolerance;
        limits.prune_plan = config.prune_plan;
        limits.trans_stopped_vel = config.trans_stopped_vel;
        limits.rot_stopped_vel = config.rot_stopped_vel;
        planner_util_.reconfigureCB(limits, config.restore_defaults);

        generator_.setParameters(config.sim_time, config.sim_granularity,
            config.angular_sim_granularity, config.use_dwa, sim_period_);

        sim_time_ = config.sim_time;

        double resolution = planner_util_.getCostmap()->getResolution();
        pdist_scale_ = config.path_distance_bias;
        path_costs_->setScale(resolution * pdist_scale_ * 0.5);
        alignment_costs_->setScale(resolution * pdist_scale_ * 0.5);

        gdist_scale_ = config.goal_distance_bias;
        goal_costs_->setScale(resolution * gdist_scale_ * 0.5);
        goal_front_costs_->setScale(resolution * gdist_scale_ * 0.5);

        occdist_scale_ = config.occdist_scale;
        obstacle_costs_->setScale(resolution * occdist_scale_);

        stop_time_buffer_ = config.stop_time_buffer;
        oscillation_costs_.setOscillationResetDist(config.oscillation_reset_dist, config.oscillation_reset_angle);
        forward_point_distance_ = config.forward_point_distance;
        goal_front_costs_->setXShift(forward_point_distance_);
        alignment_costs_->setXShift(forward_point_distance_);
        obstacle_costs_->setParams(config.max_trans_vel, config.max_scaling_factor, config.scaling_speed);

        int vx_samp, vy_samp, vth_samp;
        vx_samp = config.vx_samples;
        vy_samp = config.vy_samples;
        vth_samp = config.vth_samples;

        if (vx_samp <= 0)
        {
            ROS_WARN("You've specified that you don't want any samples in the x dimension. We'll at least assume that you want to sample one value... so we're going to set vx_samples to 1 instead");
            vx_samp = 1;
            config.vx_samples = vx_samp;
        }

        if (vy_samp <= 0)
        {
            ROS_WARN("You've specified that you don't want any samples in the y dimension. We'll at least assume that you want to sample one value... so we're going to set vy_samples to 1 instead");
            vy_samp = 1;
            config.vy_samples = vy_samp;
        }

        if (vth_samp <= 0)
        {
            ROS_WARN("You've specified that you don't want any samples in the th dimension. We'll at least assume that you want to sample one value... so we're going to set vth_samples to 1 instead");
            vth_samp = 1;
            config.vth_samples = vth_samp;
        }

        vsamples_[0] = vx_samp;
        vsamples_[1] = vy_samp;
        vsamples_[2] = vth_samp;
    }

    HANPLocalPlanner::HANPLocalPlanner() : initialized_(false), odom_helper_("odom"), setup_(false) { }

    void HANPLocalPlanner::initialize(std::string name, tf::TransformListener* tf, costmap_2d::Costmap2DROS* costmap_ros)
    {
        if (! isInitialized())
        {
            ros::NodeHandle private_nh("~/" + name);
            g_plan_pub_ = private_nh.advertise<nav_msgs::Path>("global_plan", 1);
            l_plan_pub_ = private_nh.advertise<nav_msgs::Path>("local_plan", 1);
            point_head_pub_ = private_nh.advertise<geometry_msgs::PointStamped>("point_head", 1);
            tf_ = tf;
            costmap_ros_ = costmap_ros;
            costmap_ros_->getRobotPose(current_pose_);

            costmap_2d::Costmap2D* costmap = costmap_ros_->getCostmap();

            planner_util_.initialize(tf, costmap, costmap_ros_->getGlobalFrameID());


            obstacle_costs_ = new base_local_planner::ObstacleCostFunction(planner_util_.getCostmap());
            path_costs_ = new base_local_planner::MapGridCostFunction(planner_util_.getCostmap());
            goal_costs_ = new base_local_planner::MapGridCostFunction(planner_util_.getCostmap(), 0.0, 0.0, true);
            goal_front_costs_ = new base_local_planner::MapGridCostFunction(planner_util_.getCostmap(), 0.0, 0.0, true);
            alignment_costs_ = new base_local_planner::MapGridCostFunction(planner_util_.getCostmap());

            goal_front_costs_->setStopOnFailure( false );
            alignment_costs_->setStopOnFailure( false );

            std::string controller_frequency_param_name;
            if(!private_nh.searchParam("controller_frequency", controller_frequency_param_name))
            {
                sim_period_ = 0.05;
            }
            else
            {
                double controller_frequency = 0;
                private_nh.param(controller_frequency_param_name, controller_frequency, 20.0);
                if(controller_frequency > 0)
                {
                    sim_period_ = 1.0 / controller_frequency;
                }
                else
                {
                    ROS_WARN("A controller_frequency less than 0 has been set. Ignoring the parameter, assuming a rate of 20Hz");
                    sim_period_ = 0.05;
                }
            }
            ROS_INFO("Sim period is set to %.2f", sim_period_);

            oscillation_costs_.resetOscillationFlags();

            bool sum_scores;
            private_nh.param("sum_scores", sum_scores, false);
            obstacle_costs_->setSumScores(sum_scores);

            private_nh.param("publish_cost_grid_pc", publish_cost_grid_pc_, false);
            map_viz_.initialize(name, planner_util_.getGlobalFrame(), boost::bind(&HANPLocalPlanner::getCellCosts, this, _1, _2, _3, _4, _5, _6));

            std::string frame_id;
            private_nh.param("global_frame_id", frame_id, std::string("odom"));

            traj_cloud_ = new pcl::PointCloud<base_local_planner::MapGridCostPoint>;
            traj_cloud_->header.frame_id = frame_id;
            traj_cloud_pub_.advertise(private_nh, "trajectory_cloud", 1);
            private_nh.param("publish_traj_pc", publish_traj_pc_, false);

            std::vector<base_local_planner::TrajectoryCostFunction*> critics;
            critics.push_back(&oscillation_costs_);
            critics.push_back(obstacle_costs_);
            critics.push_back(goal_front_costs_);
            critics.push_back(alignment_costs_);
            critics.push_back(path_costs_);
            critics.push_back(goal_costs_);

            std::vector<base_local_planner::TrajectorySampleGenerator*> generator_list;
            generator_list.push_back(&generator_);

            scored_sampling_planner_ = base_local_planner::SimpleScoredSamplingPlanner(generator_list, critics);

            private_nh.param("cheat_factor", cheat_factor_, 1.0);


            if( private_nh.getParam( "odom_topic", odom_topic_ ))
            {
                odom_helper_.setOdomTopic( odom_topic_ );
            }

            initialized_ = true;

            dsrv_ = new dynamic_reconfigure::Server<HANPLocalPlannerConfig>(private_nh);
            dynamic_reconfigure::Server<HANPLocalPlannerConfig>::CallbackType cb = boost::bind(&HANPLocalPlanner::reconfigureCB, this, _1, _2);
            dsrv_->setCallback(cb);
        }
        else
        {
            ROS_WARN("This planner has already been initialized, doing nothing.");
        }
    }

    bool HANPLocalPlanner::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan)
    {
        if (! isInitialized())
        {
            ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
            return false;
        }
        //when we get a new plan, we also want to clear any latch we may have on goal tolerances
        latchedStopRotateController_.resetLatching();

        ROS_INFO("Got new plan");

        oscillation_costs_.resetOscillationFlags();
        return planner_util_.setPlan(orig_global_plan);
    }

    bool HANPLocalPlanner::isGoalReached()
    {
        if (! isInitialized())
        {
            ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
            return false;
        }
        if ( ! costmap_ros_->getRobotPose(current_pose_))
        {
            ROS_ERROR("Could not get robot pose");
            return false;
        }

        if(latchedStopRotateController_.isGoalReached(&planner_util_, odom_helper_, current_pose_))
        {
            ROS_INFO("Goal reached");
            return true;
        }
        else
        {
            return false;
        }
    }

    void HANPLocalPlanner::publishLocalPlan(std::vector<geometry_msgs::PoseStamped>& path)
    {
        base_local_planner::publishPlan(path, l_plan_pub_);
    }

    void HANPLocalPlanner::publishGlobalPlan(std::vector<geometry_msgs::PoseStamped>& path)
    {
        base_local_planner::publishPlan(path, g_plan_pub_);
    }

    void HANPLocalPlanner::publishPointHead(geometry_msgs::PointStamped& point_head)
    {
        point_head_pub_.publish(point_head);
    }

    HANPLocalPlanner::~HANPLocalPlanner()
    {
        delete dsrv_;
    }

   bool HANPLocalPlanner::hanpComputeVelocityCommands(tf::Stamped<tf::Pose> &global_pose, geometry_msgs::Twist& cmd_vel)
   {
        if(! isInitialized())
        {
            ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
            return false;
        }

        tf::Stamped<tf::Pose> robot_vel;
        odom_helper_.getRobotVel(robot_vel);

        /* For timing uncomment
        struct timeval start, end;
        double start_t, end_t, t_diff;
        gettimeofday(&start, NULL);
        */

        tf::Stamped<tf::Pose> drive_cmds;
        drive_cmds.frame_id_ = costmap_ros_->getBaseFrameID();

        base_local_planner::Trajectory path = findBestPath(global_pose, robot_vel, drive_cmds, costmap_ros_->getRobotFootprint());
        //ROS_ERROR("Best: %.2f, %.2f, %.2f, %.2f", path.xv_, path.yv_, path.thetav_, path.cost_);

        /* For timing uncomment
        gettimeofday(&end, NULL);
        start_t = start.tv_sec + double(start.tv_usec) / 1e6;
        end_t = end.tv_sec + double(end.tv_usec) / 1e6;
        t_diff = end_t - start_t;
        ROS_INFO("Cycle time: %.9f", t_diff);
        */

        cmd_vel.linear.x = drive_cmds.getOrigin().getX();
        cmd_vel.linear.y = drive_cmds.getOrigin().getY();
        cmd_vel.angular.z = tf::getYaw(drive_cmds.getRotation());

        std::vector<geometry_msgs::PoseStamped> local_plan;
        if(path.cost_ < 0)
        {
            ROS_DEBUG_NAMED("hanp_local_planner", "The hanp local planner failed to find a valid plan, cost functions discarded all candidates. This can mean there is an obstacle too close to the robot.");
            local_plan.clear();
            publishLocalPlan(local_plan);

            // publish to point head in the front of the robot
            geometry_msgs::PointStamped point_head;
            point_head.header.stamp = ros::Time::now();
            point_head.header.frame_id = costmap_ros_->getBaseFrameID();
            point_head.point.x = 0.1;
            point_head.point.y = 0.0;
            point_head.point.z = 1.5; // TODO: make robot height as parameter
            publishPointHead(point_head);
            return false;
        }

        ROS_DEBUG_NAMED("hanp_local_planner", "A valid velocity command of (%.2f, %.2f, %.2f) was found for this cycle.",
            cmd_vel.linear.x, cmd_vel.linear.y, cmd_vel.angular.z);

        for(unsigned int i = 0; i < path.getPointsSize(); ++i)
        {
            double p_x, p_y, p_th;
            path.getPoint(i, p_x, p_y, p_th);

            tf::Stamped<tf::Pose> p = tf::Stamped<tf::Pose>(tf::Pose(
                tf::createQuaternionFromYaw(p_th), tf::Point(p_x, p_y, 0.0)),
                ros::Time::now(), costmap_ros_->getGlobalFrameID());
            geometry_msgs::PoseStamped pose;
            tf::poseStampedTFToMsg(p, pose);
            local_plan.push_back(pose);
        }

        publishLocalPlan(local_plan);

        // publish head pose
        geometry_msgs::PointStamped point_head;
        point_head.header.stamp = ros::Time::now();
        point_head.header.frame_id = local_plan.back().header.frame_id;
        point_head.point = local_plan.back().pose.position;
        point_head.point.z = 1.5; // TODO: make robot height as parameter
        publishPointHead(point_head);

        return true;
    }

    bool HANPLocalPlanner::computeVelocityCommands(geometry_msgs::Twist& cmd_vel)
    {
        if ( ! costmap_ros_->getRobotPose(current_pose_))
        {
            ROS_ERROR("Could not get robot pose");
            return false;
        }
        std::vector<geometry_msgs::PoseStamped> transformed_plan;
        if ( ! planner_util_.getLocalPlan(current_pose_, transformed_plan))
        {
            ROS_ERROR("Could not get local plan");
            return false;
        }

        if(transformed_plan.empty())
        {
            ROS_WARN_NAMED("hanp_local_planner", "Received an empty transformed plan.");
            return false;
        }
        ROS_DEBUG_NAMED("hanp_local_planner", "Received a transformed plan with %zu points.", transformed_plan.size());

        updatePlanAndLocalCosts(current_pose_, transformed_plan);

        if (latchedStopRotateController_.isPositionReached(&planner_util_, current_pose_))
        {
            std::vector<geometry_msgs::PoseStamped> local_plan;
            std::vector<geometry_msgs::PoseStamped> transformed_plan;
            publishGlobalPlan(transformed_plan);
            publishLocalPlan(local_plan);

            // publish to point head in the front of the robot
            geometry_msgs::PointStamped point_head;
            point_head.header.stamp = ros::Time::now();
            point_head.header.frame_id = costmap_ros_->getBaseFrameID();
            point_head.point.x = 0.1;
            point_head.point.y = 0.0;
            point_head.point.z = 1.5; // TODO: make robot height as parameter
            publishPointHead(point_head);

            base_local_planner::LocalPlannerLimits limits = planner_util_.getCurrentLimits();
            return latchedStopRotateController_.computeVelocityCommandsStopRotate(cmd_vel,
                limits.getAccLimits(), sim_period_, &planner_util_, odom_helper_,
                current_pose_, boost::bind(&HANPLocalPlanner::checkTrajectory, this, _1, _2, _3));
        }
        else
        {
            bool isOk = hanpComputeVelocityCommands(current_pose_, cmd_vel);
            if (isOk)
            {
                publishGlobalPlan(transformed_plan);
            }
            else
            {
                ROS_WARN_NAMED("hanp_local_planner", "HANP local planner failed to produce path.");
                std::vector<geometry_msgs::PoseStamped> empty_plan;
                publishGlobalPlan(empty_plan);
            }
            return isOk;
        }
    }

    bool HANPLocalPlanner::getCellCosts(int cx, int cy, float &path_cost, float &goal_cost, float &occ_cost, float &total_cost)
    {
        path_cost = path_costs_->getCellCosts(cx, cy);
        goal_cost = goal_costs_->getCellCosts(cx, cy);
        occ_cost = planner_util_.getCostmap()->getCost(cx, cy);
        if (path_cost == path_costs_->obstacleCosts() ||
            path_cost == path_costs_->unreachableCellCosts() ||
            occ_cost >= costmap_2d::INSCRIBED_INFLATED_OBSTACLE)
        {
            return false;
        }

        double resolution = planner_util_.getCostmap()->getResolution();
        total_cost = pdist_scale_ * resolution * path_cost +
            gdist_scale_ * resolution * goal_cost + occdist_scale_ * occ_cost;
        return true;
    }

    bool HANPLocalPlanner::checkTrajectory(Eigen::Vector3f pos, Eigen::Vector3f vel, Eigen::Vector3f vel_samples)
    {
        oscillation_costs_.resetOscillationFlags();
        base_local_planner::Trajectory traj;
        geometry_msgs::PoseStamped goal_pose = global_plan_.back();
        Eigen::Vector3f goal(goal_pose.pose.position.x, goal_pose.pose.position.y, tf::getYaw(goal_pose.pose.orientation));
        base_local_planner::LocalPlannerLimits limits = planner_util_.getCurrentLimits();
        generator_.initialise(pos, vel, goal, &limits, vsamples_);
        generator_.generateTrajectory(pos, vel, vel_samples, traj);
        double cost = scored_sampling_planner_.scoreTrajectory(traj, -1);
        if(cost >= 0)
        {
            return true;
        }
        ROS_WARN("Invalid Trajectory %f, %f, %f, cost: %f", vel_samples[0], vel_samples[1], vel_samples[2], cost);
        return false;
    }

    void HANPLocalPlanner::updatePlanAndLocalCosts(tf::Stamped<tf::Pose> global_pose,
        const std::vector<geometry_msgs::PoseStamped>& new_plan)
    {
        global_plan_.resize(new_plan.size());
        for (unsigned int i = 0; i < new_plan.size(); ++i)
        {
            global_plan_[i] = new_plan[i];
        }

        path_costs_->setTargetPoses(global_plan_);

        goal_costs_->setTargetPoses(global_plan_);

        geometry_msgs::PoseStamped goal_pose = global_plan_.back();

        Eigen::Vector3f pos(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), tf::getYaw(global_pose.getRotation()));
        double sq_dist = (pos[0] - goal_pose.pose.position.x) * (pos[0] - goal_pose.pose.position.x) +
            (pos[1] - goal_pose.pose.position.y) * (pos[1] - goal_pose.pose.position.y);

        std::vector<geometry_msgs::PoseStamped> front_global_plan = global_plan_;
        double angle_to_goal = atan2(goal_pose.pose.position.y - pos[1], goal_pose.pose.position.x - pos[0]);
        front_global_plan.back().pose.position.x = front_global_plan.back().pose.position.x +
            forward_point_distance_ * cos(angle_to_goal);
        front_global_plan.back().pose.position.y = front_global_plan.back().pose.position.y +
            forward_point_distance_ * sin(angle_to_goal);

        goal_front_costs_->setTargetPoses(front_global_plan);

        if (sq_dist > forward_point_distance_ * forward_point_distance_ * cheat_factor_)
        {
            double resolution = planner_util_.getCostmap()->getResolution();
            alignment_costs_->setScale(resolution * pdist_scale_ * 0.5);

            alignment_costs_->setTargetPoses(global_plan_);
        }
        else
        {
            alignment_costs_->setScale(0.0);
        }
    }

    base_local_planner::Trajectory HANPLocalPlanner::findBestPath(tf::Stamped<tf::Pose> global_pose,
        tf::Stamped<tf::Pose> global_vel, tf::Stamped<tf::Pose>& drive_velocities,
        std::vector<geometry_msgs::Point> footprint_spec)
    {
        obstacle_costs_->setFootprint(footprint_spec);

        boost::mutex::scoped_lock l(configuration_mutex_);

        Eigen::Vector3f pos(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), tf::getYaw(global_pose.getRotation()));
        Eigen::Vector3f vel(global_vel.getOrigin().getX(), global_vel.getOrigin().getY(), tf::getYaw(global_vel.getRotation()));
        geometry_msgs::PoseStamped goal_pose = global_plan_.back();
        Eigen::Vector3f goal(goal_pose.pose.position.x, goal_pose.pose.position.y, tf::getYaw(goal_pose.pose.orientation));
        base_local_planner::LocalPlannerLimits limits = planner_util_.getCurrentLimits();

        generator_.initialise(pos, vel, goal, &limits, vsamples_);

        result_traj_.cost_ = -7;

        std::vector<base_local_planner::Trajectory> all_explored;
        scored_sampling_planner_.findBestTrajectory(result_traj_, &all_explored);

        if(publish_traj_pc_)
        {
            base_local_planner::MapGridCostPoint pt;
            traj_cloud_->points.clear();
            traj_cloud_->width = 0;
            traj_cloud_->height = 0;
            std_msgs::Header header;
            pcl_conversions::fromPCL(traj_cloud_->header, header);
            header.stamp = ros::Time::now();
            traj_cloud_->header = pcl_conversions::toPCL(header);
            for(std::vector<base_local_planner::Trajectory>::iterator t=all_explored.begin(); t != all_explored.end(); ++t)
            {
                if(t->cost_<0)
                    continue;

                for(unsigned int i = 0; i < t->getPointsSize(); ++i)
                {
                    double p_x, p_y, p_th;
                    t->getPoint(i, p_x, p_y, p_th);
                    pt.x=p_x;
                    pt.y=p_y;
                    pt.z=0;
                    pt.path_cost=p_th;
                    pt.total_cost=t->cost_;
                    traj_cloud_->push_back(pt);
                }
            }
            traj_cloud_pub_.publish(*traj_cloud_);
        }

        if (publish_cost_grid_pc_)
        {
            map_viz_.publishCostCloud(planner_util_.getCostmap());
        }

        oscillation_costs_.updateOscillationFlags(pos, &result_traj_, planner_util_.getCurrentLimits().min_trans_vel);

        if (result_traj_.cost_ < 0)
        {
            drive_velocities.setIdentity();
        }
        else
        {
            tf::Vector3 start(result_traj_.xv_, result_traj_.yv_, 0);
            drive_velocities.setOrigin(start);
            tf::Matrix3x3 matrix;
            matrix.setRotation(tf::createQuaternionFromYaw(result_traj_.thetav_));
            drive_velocities.setBasis(matrix);
        }

        return result_traj_;
    }
};
