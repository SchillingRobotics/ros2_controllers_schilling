// Copyright 2024 ros2_control Development Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "multi_time_trajectory_controller/trajectory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "angles/angles.h"
#include "control_msgs/msg/multi_axis_trajectory.hpp"
#include "hardware_interface/macros.hpp"
#include "rclcpp/duration.hpp"
#include "rclcpp/time.hpp"

namespace multi_time_trajectory_controller
{
namespace
{
// Safe default joint kinematic limits for Ruckig, in case none is defined in the URDF or
// joint_limits.yaml
constexpr double DEFAULT_MAX_VELOCITY = 1.5;      // rad/s
constexpr double DEFAULT_MAX_ACCELERATION = 5.0;  // rad/s^2
constexpr double DEFAULT_MAX_JERK = 200.0;        // rad/s^3
}  // namespace

Trajectory::Trajectory()
: trajectory_start_time_(0),
  time_before_traj_msg_(0),
  sampled_already_(false),
  have_previous_ruckig_output_(false)
{
}

Trajectory::Trajectory(std::shared_ptr<control_msgs::msg::MultiAxisTrajectory> joint_trajectory)
: trajectory_msg_(joint_trajectory),
  trajectory_start_time_(static_cast<rclcpp::Time>(joint_trajectory->header.stamp)),
  sampled_already_(false),
  have_previous_ruckig_output_(false)
{
}

Trajectory::Trajectory(
  const rclcpp::Time & current_time,
  const std::vector<control_msgs::msg::AxisTrajectoryPoint> & current_point,
  std::shared_ptr<control_msgs::msg::MultiAxisTrajectory> joint_trajectory)
: trajectory_msg_(joint_trajectory),
  trajectory_start_time_(static_cast<rclcpp::Time>(joint_trajectory->header.stamp))
{
  set_point_before_trajectory_msg(current_time, current_point);
  //   update(joint_trajectory);
}

void Trajectory::set_point_before_trajectory_msg(
  const rclcpp::Time & current_time,
  const std::vector<control_msgs::msg::AxisTrajectoryPoint> & current_point,
  const std::vector<bool> & joints_angle_wraparound)
{
  time_before_traj_msg_ = current_time;
  state_before_traj_msg_ = current_point;

  // Compute offsets due to wrapping joints
  wraparound_joint(
    state_before_traj_msg_, trajectory_msg_->axis_trajectories[0].axis_points,
    joints_angle_wraparound);
}

void wraparound_joint(
  std::vector<control_msgs::msg::AxisTrajectoryPoint> & current_position,
  const std::vector<control_msgs::msg::AxisTrajectoryPoint> next_position,
  const std::vector<bool> & joints_angle_wraparound)
{
  double dist;
  // joints_angle_wraparound is even empty, or has the same size as the number of joints
  for (size_t i = 0; i < joints_angle_wraparound.size(); i++)
  {
    if (joints_angle_wraparound[i])
    {
      dist =
        angles::shortest_angular_distance(current_position[i].position, next_position[i].position);

      // Deal with singularity at M_PI shortest distance
      if (std::abs(std::abs(dist) - M_PI) < 1e-9)
      {
        dist = next_position[i].position > current_position[i].position ? std::abs(dist)
                                                                        : -std::abs(dist);
      }

      current_position[i].position = next_position[i].position - dist;
    }
  }
}

void Trajectory::update(
  std::shared_ptr<control_msgs::msg::MultiAxisTrajectory> joint_trajectory,
  const std::vector<joint_limits::JointLimits> & joint_limits, const rclcpp::Duration & period)
{
  trajectory_msg_ = joint_trajectory;
  trajectory_start_time_ = static_cast<rclcpp::Time>(joint_trajectory->header.stamp);
  sampled_already_ = false;

  // Initialize Ruckig-smoothing-related stuff
  size_t dim = joint_trajectory->axis_names.size();
  // TODO(andyz): update only the Ruckig::delta_time member of the smoother.
  // dim should not update since it doesn't change with every new trajectory
  // See https://github.com/pantor/ruckig/issues/118
  smoother_ = std::make_unique<ruckig::Ruckig<ruckig::DynamicDOFs>>(dim, period.seconds());
  ruckig_input_ = ruckig::InputParameter<ruckig::DynamicDOFs>(dim);
  ruckig_output_ = ruckig::OutputParameter<ruckig::DynamicDOFs>(dim);
  have_previous_ruckig_output_ = false;

  ruckig_input_.max_velocity.clear();
  ruckig_input_.max_velocity.resize(dim, DEFAULT_MAX_VELOCITY);
  ruckig_input_.max_acceleration.clear();
  ruckig_input_.max_acceleration.resize(dim, DEFAULT_MAX_ACCELERATION);
  ruckig_input_.max_jerk.clear();
  ruckig_input_.max_jerk.resize(dim, DEFAULT_MAX_JERK);

  for (size_t i = 0; i < dim; ++i)
  {
    RCLCPP_DEBUG(
      rclcpp::get_logger("trajectory"), "max vel for joint %zu is %f", i,
      ruckig_input_.max_velocity[i]);
    RCLCPP_DEBUG(
      rclcpp::get_logger("trajectory"), "max acc for joint %zu is %f", i,
      ruckig_input_.max_acceleration[i]);
    RCLCPP_DEBUG(
      rclcpp::get_logger("trajectory"), "max jerk for joint %zu is %f", i,
      ruckig_input_.max_jerk[i]);
    if (joint_limits[i].has_velocity_limits)
    {
      ruckig_input_.max_velocity[i] = joint_limits[i].max_velocity;
      RCLCPP_DEBUG(
        rclcpp::get_logger("trajectory"), "Setting max vel for joint %zu to %f", i,
        joint_limits[i].max_velocity);
    }
    if (joint_limits[i].has_acceleration_limits)
    {
      ruckig_input_.max_acceleration[i] = joint_limits[i].max_acceleration;
      RCLCPP_DEBUG(
        rclcpp::get_logger("trajectory"), "Setting max acc for joint %zu to %f", i,
        joint_limits[i].max_acceleration);
    }
    if (joint_limits[i].has_jerk_limits)
    {
      ruckig_input_.max_jerk[i] = joint_limits[i].max_jerk;
      RCLCPP_DEBUG(
        rclcpp::get_logger("trajectory"), "Setting max jerk for joint %zu to %f", i,
        joint_limits[i].max_jerk);
    }
  }
  previous_state_.resize(dim);
  for (std::size_t i = 0; i < dim; ++i)
  {
    previous_state_[i].position = std::numeric_limits<double>::quiet_NaN();
    previous_state_[i].velocity = std::numeric_limits<double>::quiet_NaN();
    previous_state_[i].acceleration = std::numeric_limits<double>::quiet_NaN();
  }
}

bool Trajectory::sample(
  const rclcpp::Time & sample_time,
  const joint_trajectory_controller::interpolation_methods::InterpolationMethod
    interpolation_method,
  std::vector<control_msgs::msg::AxisTrajectoryPoint> & output_state,
  TrajectoryPointConstIter & start_segment_itr, TrajectoryPointConstIter & end_segment_itr,
  const rclcpp::Duration & period,
  std::unique_ptr<joint_limits::JointLimiterInterface<joint_limits::JointLimits>> & joint_limiter,
  std::vector<control_msgs::msg::AxisTrajectoryPoint> & splines_state,
  std::vector<control_msgs::msg::AxisTrajectoryPoint> & ruckig_state,
  std::vector<control_msgs::msg::AxisTrajectoryPoint> & ruckig_input_state)
{
  THROW_ON_NULLPTR(trajectory_msg_)

  if (
    trajectory_msg_->axis_trajectories.empty() ||
    std::any_of(
      trajectory_msg_->axis_trajectories.begin(), trajectory_msg_->axis_trajectories.end(),
      [](control_msgs::msg::AxisTrajectory traj) { return traj.axis_points.empty(); }))
  {
    // TODO(henrygerardmoore): how should we handle this case?
    // start_segment_itr = end();
    // end_segment_itr = end();
    return false;
  }

  // first sampling of this trajectory
  if (!sampled_already_)
  {
    if (trajectory_start_time_.seconds() == 0.0)
    {
      trajectory_start_time_ = sample_time;
    }

    sampled_already_ = true;
  }

  // sampling before the current point
  if (sample_time < time_before_traj_msg_)
  {
    return false;
  }

  // TODO(anyone): this shouldn't be initialized at runtime
  output_state = {};

  auto do_ruckig_smoothing =
    interpolation_method ==
    joint_trajectory_controller::interpolation_methods::InterpolationMethod::RUCKIG;

  std::size_t num_axes = trajectory_msg_->axis_trajectories.size();
  // output_state splines_state ruckig_state ruckig_input_state
  if (output_state.size() < num_axes)
  {
    output_state.resize(num_axes);
  }
  if (splines_state.size() < num_axes)
  {
    splines_state.resize(num_axes);
  }
  if (ruckig_state.size() < num_axes)
  {
    ruckig_state.resize(num_axes);
  }
  if (ruckig_input_state.size() < num_axes)
  {
    ruckig_input_state.resize(num_axes);
  }

  for (std::size_t axis_index = 0; axis_index < num_axes; ++axis_index)
  {
    auto & trajectory = trajectory_msg_->axis_trajectories[axis_index];
    auto & first_point_in_msg = trajectory.axis_points[0];
    const rclcpp::Time first_point_timestamp =
      trajectory_start_time_ + first_point_in_msg.time_from_start;

    // current time hasn't reached traj time of the first point in the msg yet
    if (sample_time < first_point_timestamp)
    {
      // If interpolation is disabled, just forward the next waypoint
      if (
        interpolation_method ==
        joint_trajectory_controller::interpolation_methods::InterpolationMethod::NONE)
      {
        output_state = state_before_traj_msg_;
        previous_state_ = state_before_traj_msg_;
      }
      else
      {
        // const size_t dim = first_point_in_msg.velocities.size();
        //
        // double max_vel_ratio = 1.0;
        // for (size_t dim_i = 0; dim_i < dim; ++dim_i)
        // {
        //   if (std::fabs(first_point_in_msg.velocities[dim_i]) > joint_limits[dim_i].max_velocity)
        //   {
        //     const double ratio =
        //     std::fabs(first_point_in_msg.velocities[dim_i] / joint_limits[dim_i].max_velocity);
        //     if (ratio > max_vel_ratio)
        //     {
        //       max_vel_ratio = ratio;
        //     }
        //   }
        // }
        //
        // for (size_t dim_i = 0; dim_i < dim; ++dim_i)
        // {
        //   // Set the target velocities to follow the joint limits
        // first_point_in_msg.velocities[dim_i] = first_point_in_msg.velocities[dim_i] /
        // max_vel_ratio
        // }

        // it changes points only if position and velocity do not exist, but their derivatives
        deduce_from_derivatives(
          state_before_traj_msg_[axis_index], first_point_in_msg,
          (first_point_timestamp - time_before_traj_msg_).seconds());

        interpolate_between_points(
          time_before_traj_msg_, state_before_traj_msg_[axis_index], first_point_timestamp,
          first_point_in_msg, sample_time, do_ruckig_smoothing, false, output_state[axis_index],
          period, splines_state[axis_index], ruckig_state[axis_index],
          ruckig_input_state[axis_index], axis_index);
      }
      start_segment_itr = begin(axis_index);  // no segments before the first
      end_segment_itr = begin(axis_index);

      previous_state_ = output_state;
      return true;
    }

    // time_from_start + trajectory time is the expected arrival time of trajectory
    const auto point_before_last_idx = trajectory.axis_points.size() - 1;
    for (size_t i = 0; i < point_before_last_idx; ++i)
    {
      auto & point = trajectory.axis_points[i];
      auto & next_point = trajectory.axis_points[i + 1];

      const rclcpp::Time t0 = trajectory_start_time_ + point.time_from_start;
      const rclcpp::Time t1 = trajectory_start_time_ + next_point.time_from_start;

      if (sample_time >= t0 && sample_time < t1)
      {
        // TODO(anyone): this shouldn't be initialized at runtime
        output_state = {};

        // If interpolation is disabled, just forward the next waypoint
        if (
          interpolation_method ==
          joint_trajectory_controller::interpolation_methods::InterpolationMethod::NONE)
        {
          output_state[axis_index] = next_point;
        }
        // Do interpolation
        else
        {
          // const size_t dim = next_point.velocities.size();
          //
          // double max_vel_ratio = 1.0;
          // for (size_t dim_i = 0; dim_i < dim; ++dim_i)
          // {
          //   if (std::fabs(next_point.velocities[dim_i]) > joint_limits[dim_i].max_velocity)
          //   {
          //     const double ratio =
          //       std::fabs(next_point.velocities[dim_i] / joint_limits[dim_i].max_velocity);
          //     if (ratio > max_vel_ratio)
          //     {
          //       max_vel_ratio = ratio;
          //     }
          //   }
          // }
          //
          // for (size_t dim_i = 0; dim_i < dim; ++dim_i)
          // {
          //   // Set the target velocities to follow the joint limits
          //   next_point.velocities[dim_i] = next_point.velocities[dim_i] / max_vel_ratio;
          // }

          // it changes points only if position and velocity do not exist, but their derivatives
          deduce_from_derivatives(point, next_point, (t1 - t0).seconds());

          if (!interpolate_between_points(
                t0, point, t1, next_point, sample_time, do_ruckig_smoothing, false,
                output_state[axis_index], period, splines_state[axis_index],
                ruckig_state[axis_index], ruckig_input_state[axis_index], axis_index))
          {
            return false;
          }
        }
        start_segment_itr = begin(axis_index) + static_cast<int64_t>(i);
        end_segment_itr = begin(axis_index) + static_cast<int64_t>(i + 1);

        if (joint_limiter)
        {
          // TODO(henrygerardmoore): what should we do with this?
        }
        previous_state_ = output_state;
        return true;
      }
    }

    // whole animation has played out - but still continue s interpolating and smoothing
    auto & last_point = trajectory.axis_points[trajectory.axis_points.size() - 1];
    const rclcpp::Time t0 = trajectory_start_time_ + last_point.time_from_start;

    if (last_point.acceleration != 0)
    {
      last_point.velocity += last_point.acceleration * period.seconds();
      // remember velocity over multiple calls
    }
    if (last_point.velocity != 0)
    {
      last_point.position += last_point.velocity * period.seconds();
      // remember velocity over multiple calls
    }

    // do not do splines when trajectory has finished because the time is achieved
    if (!interpolate_between_points(
          t0, last_point, t0, last_point, sample_time, do_ruckig_smoothing, true,
          output_state[axis_index], period, splines_state[axis_index], ruckig_state[axis_index],
          ruckig_input_state[axis_index], axis_index))
    {
      return false;
    }

    if (joint_limiter)
    {
      // TODO(henrygerardmoore): what should we do with this?
    }
    previous_state_ = output_state;

    start_segment_itr = --end(axis_index);
    end_segment_itr = end(axis_index);
  }

  return true;
}

bool Trajectory::interpolate_between_points(
  const rclcpp::Time & time_a, const control_msgs::msg::AxisTrajectoryPoint & state_a,
  const rclcpp::Time & time_b, const control_msgs::msg::AxisTrajectoryPoint & state_b,
  const rclcpp::Time & sample_time, const bool do_ruckig_smoothing, const bool skip_splines,
  control_msgs::msg::AxisTrajectoryPoint & output, const rclcpp::Duration & period,
  control_msgs::msg::AxisTrajectoryPoint & splines_state,
  control_msgs::msg::AxisTrajectoryPoint & ruckig_state,
  control_msgs::msg::AxisTrajectoryPoint & ruckig_input_state, std::size_t axis_index)
{
  //   RCLCPP_WARN(rclcpp::get_logger("trajectory"), "New iteration");

  rclcpp::Duration duration_so_far = sample_time - time_a;
  rclcpp::Duration duration_btwn_points = time_b - time_a;

  output.position = 0;
  output.velocity = 0;
  output.acceleration = 0;

  bool has_velocity = !std::isnan(state_a.velocity) && !std::isnan(state_b.velocity);
  bool has_accel = !std::isnan(state_a.acceleration) && !std::isnan(state_b.acceleration);

  splines_state.position = std::numeric_limits<double>::quiet_NaN();
  splines_state.velocity = std::numeric_limits<double>::quiet_NaN();
  splines_state.acceleration = std::numeric_limits<double>::quiet_NaN();
  splines_state.effort = std::numeric_limits<double>::quiet_NaN();
  ruckig_state.position = std::numeric_limits<double>::quiet_NaN();
  ruckig_state.velocity = std::numeric_limits<double>::quiet_NaN();
  ruckig_state.acceleration = std::numeric_limits<double>::quiet_NaN();
  ruckig_state.effort = std::numeric_limits<double>::quiet_NaN();
  ruckig_input_state.position = std::numeric_limits<double>::quiet_NaN();
  ruckig_input_state.velocity = std::numeric_limits<double>::quiet_NaN();
  ruckig_input_state.acceleration = std::numeric_limits<double>::quiet_NaN();
  ruckig_input_state.effort = std::numeric_limits<double>::quiet_NaN();

  if (!skip_splines)
  {
    auto generate_powers = [](int n, double x, double * powers)
    {
      powers[0] = 1.0;
      for (int i = 1; i <= n; ++i)
      {
        powers[i] = powers[i - 1] * x;
      }
    };

    if (duration_so_far.seconds() < 0.0)
    {
      duration_so_far = rclcpp::Duration::from_seconds(0.0);
      has_velocity = has_accel = false;
      RCLCPP_WARN(rclcpp::get_logger("trajectory"), "Duration so far is negative");
    }
    if (duration_so_far.seconds() > duration_btwn_points.seconds())
    {
      RCLCPP_WARN(
        rclcpp::get_logger("trajectory"),
        "Duration so far is smaller then duration between points");
      duration_so_far = duration_btwn_points;
      has_velocity = has_accel = false;
    }

    output.effort = state_b.position;

    double t[6];
    generate_powers(5, duration_so_far.seconds(), t);

    if (!has_velocity && !has_accel)
    {
      // do linear interpolation
      double start_pos = state_a.position;
      double end_pos = state_b.position;

      double coefficients[2] = {0.0, 0.0};
      coefficients[0] = start_pos;
      if (duration_btwn_points.seconds() != 0.0)
      {
        coefficients[1] = (end_pos - start_pos) / duration_btwn_points.seconds();
      }

      output.position = t[0] * coefficients[0] + t[1] * coefficients[1];
      output.velocity = t[0] * coefficients[1];
    }
    else if (has_velocity && !has_accel)
    {
      // do cubic interpolation
      double T[4];
      generate_powers(3, duration_btwn_points.seconds(), T);

      double start_pos = state_a.position;
      double start_vel = state_a.velocity;
      double end_pos = state_b.position;
      double end_vel = state_b.velocity;

      double coefficients[4] = {0.0, 0.0, 0.0, 0.0};
      coefficients[0] = start_pos;
      coefficients[1] = start_vel;
      if (duration_btwn_points.seconds() != 0.0)
      {
        coefficients[2] =
          (-3.0 * start_pos + 3.0 * end_pos - 2.0 * start_vel * T[1] - end_vel * T[1]) / T[2];
        coefficients[3] =
          (2.0 * start_pos - 2.0 * end_pos + start_vel * T[1] + end_vel * T[1]) / T[3];
      }

      output.position = t[0] * coefficients[0] + t[1] * coefficients[1] + t[2] * coefficients[2] +
                        t[3] * coefficients[3];
      output.velocity =
        t[0] * coefficients[1] + t[1] * 2.0 * coefficients[2] + t[2] * 3.0 * coefficients[3];
      output.acceleration = t[0] * 2.0 * coefficients[2] + t[1] * 6.0 * coefficients[3];
    }
    else if (has_velocity && has_accel)
    {
      // do quintic interpolation
      double T[6];
      generate_powers(5, duration_btwn_points.seconds(), T);

      double start_pos = state_a.position;
      double start_vel = state_a.velocity;
      double start_acc = state_a.acceleration;
      double end_pos = state_b.position;
      double end_vel = state_b.velocity;
      double end_acc = state_b.acceleration;

      double coefficients[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
      coefficients[0] = start_pos;
      coefficients[1] = start_vel;
      coefficients[2] = 0.5 * start_acc;
      if (duration_btwn_points.seconds() != 0.0)
      {
        coefficients[3] = (-20.0 * start_pos + 20.0 * end_pos - 3.0 * start_acc * T[2] +
                           end_acc * T[2] - 12.0 * start_vel * T[1] - 8.0 * end_vel * T[1]) /
                          (2.0 * T[3]);
        coefficients[4] = (30.0 * start_pos - 30.0 * end_pos + 3.0 * start_acc * T[2] -
                           2.0 * end_acc * T[2] + 16.0 * start_vel * T[1] + 14.0 * end_vel * T[1]) /
                          (2.0 * T[4]);
        coefficients[5] = (-12.0 * start_pos + 12.0 * end_pos - start_acc * T[2] + end_acc * T[2] -
                           6.0 * start_vel * T[1] - 6.0 * end_vel * T[1]) /
                          (2.0 * T[5]);
      }

      output.position = t[0] * coefficients[0] + t[1] * coefficients[1] + t[2] * coefficients[2] +
                        t[3] * coefficients[3] + t[4] * coefficients[4] + t[5] * coefficients[5];
      output.velocity = t[0] * coefficients[1] + t[1] * 2.0 * coefficients[2] +
                        t[2] * 3.0 * coefficients[3] + t[3] * 4.0 * coefficients[4] +
                        t[4] * 5.0 * coefficients[5];
      output.acceleration = t[0] * 2.0 * coefficients[2] + t[1] * 6.0 * coefficients[3] +
                            t[2] * 12.0 * coefficients[4] + t[3] * 20.0 * coefficients[5];
    }
  }
  else
  {
    output.position = state_b.position;
    output.velocity = state_b.velocity;
    output.acceleration = state_b.acceleration;

    output.position = 0.0;
    output.velocity = 0.0;
    output.acceleration = 0.0;
  }

  splines_state.position = output.position;
  splines_state.velocity = output.velocity;
  splines_state.acceleration = output.acceleration;
  splines_state.effort = output.effort;

  // Optionally apply velocity, acceleration, and jerk limits with Ruckig
  //   if ((duration_so_far.seconds() > 0 || skip_splines) && do_ruckig_smoothing)
  if (do_ruckig_smoothing)
  {
    // If Ruckig has run previously on this trajectory, use the output as input for the next cycle
    if (have_previous_ruckig_output_)
    {
      //       RCLCPP_WARN(rclcpp::get_logger("trajectory"), "Applying previous Ruckig input");
      ruckig_input_state.effort = 1;

      ruckig_input_.current_position = ruckig_output_.new_position;
      ruckig_input_.current_velocity = ruckig_output_.new_velocity;
      ruckig_input_.current_acceleration = ruckig_output_.new_acceleration;
    }
    // else, need to initialize to robot state
    else
    {
      ruckig_input_state.effort = -1;

      ruckig_input_.current_position[axis_index] = state_a.position;
      if (!std::isnan(state_a.velocity))
      {
        ruckig_input_.current_velocity[axis_index] = state_a.velocity;
      }
      else
      {
        ruckig_input_.current_velocity[axis_index] = 0;
      }
      if (!std::isnan(state_a.acceleration))
      {
        ruckig_input_.current_acceleration[axis_index] = state_a.acceleration;
      }
      else
      {
        ruckig_input_.current_acceleration[axis_index] = 0;
      }
    }
    // Target state comes from the polynomial interpolation
    ruckig_input_.target_position[axis_index] = output.position;

    // double max_vel_ratio = 1.0;
    // for (size_t i = 0; i < dim; ++i)
    // {
    //   if (std::fabs(output.velocities[i]) > joint_limits[i].max_velocity)
    //   {
    //     const double ratio = std::fabs(output.velocities[i] / joint_limits[i].max_velocity);
    //     if (ratio > max_vel_ratio)
    //     {
    //       max_vel_ratio = ratio;
    //     }
    //   }
    // }

    // for (size_t i = 0; i < dim; ++i)
    // {
    //   // Set the target velocities to follow the joint limits
    //   ruckig_input_.target_velocity[i] = output.velocities[i] / max_vel_ratio;
    //
    //   // Set the target accelerations to follow the joint limits
    //   ruckig_input_.target_acceleration[i] = std::clamp(
    //     output.accelerations[i], -1.0 * joint_limits[i].max_acceleration,
    //     joint_limits[i].max_acceleration);
    // }

    ruckig_input_state.position = ruckig_input_.current_position[axis_index];
    ruckig_input_state.velocity = ruckig_input_.current_velocity[axis_index];
    ruckig_input_state.acceleration = ruckig_input_.current_acceleration[axis_index];

    ruckig_state.position = ruckig_input_.target_position[axis_index];
    ruckig_state.velocity = ruckig_input_.target_velocity[axis_index];
    ruckig_state.acceleration = ruckig_input_.target_acceleration[axis_index];

    ruckig_state.effort = ruckig_input_.target_acceleration[axis_index] / period.seconds();

    ruckig::Result result = smoother_->update(ruckig_input_, ruckig_output_);

    // If Ruckig was successful, update the output state
    // Else, just pass the output from the polynomial interpolation
    if (result == ruckig::Result::Working || result == ruckig::Result::Finished)
    {
      ruckig_input_state.effort = 1.1;
      have_previous_ruckig_output_ = true;
      output.position = ruckig_output_.new_position[axis_index];
      output.velocity = ruckig_output_.new_velocity[axis_index];
      output.acceleration = ruckig_output_.new_acceleration[axis_index];
    }
    else
    {
      ruckig_input_state.effort = -1.1;
      if (result == ruckig::Result::ErrorInvalidInput)
      {
        RCLCPP_WARN(rclcpp::get_logger("trajectory"), "Ruckig got invalid input");
        RCLCPP_WARN(
          rclcpp::get_logger("trajectory"),
          "Ruckig NOK input CURRENT pos: %.10f; vel: %.10f; acc: %.10f",
          ruckig_input_.current_position[axis_index], ruckig_input_.current_velocity[axis_index],
          ruckig_input_.current_acceleration[axis_index]);
        RCLCPP_WARN(
          rclcpp::get_logger("trajectory"),
          "Ruckig NOK input TARGET pos: %.10f; vel: %.10f; acc: %.10f",
          ruckig_input_.target_position[axis_index], ruckig_input_.target_velocity[axis_index],
          ruckig_input_.target_acceleration[axis_index]);
      }
      RCLCPP_WARN(rclcpp::get_logger("trajectory"), "Ruckig NOK!");
      return false;
    }
  }
  else
  {
    //     RCLCPP_WARN(rclcpp::get_logger("trajectory"), "Skipping Ruckig");
  }

  return true;
}

void Trajectory::deduce_from_derivatives(
  control_msgs::msg::AxisTrajectoryPoint & first_state,
  control_msgs::msg::AxisTrajectoryPoint & second_state, const double delta_t)
{
  if (std::isnan(second_state.position))
  {
    if (std::isnan(first_state.velocity))
    {
      first_state.velocity = 0.0;
    }
    if (std::isnan(second_state.velocity))
    {
      if (std::isnan(first_state.acceleration))
      {
        first_state.acceleration = 0.0;
      }
      second_state.velocity =
        first_state.velocity +
        (first_state.acceleration + second_state.acceleration) * 0.5 * delta_t;
    }
    // second state velocity should be reached on the end of the segment, so use middle
    second_state.position =
      first_state.position + (first_state.velocity + second_state.velocity) * 0.5 * delta_t;
  }
  else
  {
    if (std::isnan(second_state.position))
    {
      double first_state_velocity = std::isnan(first_state.velocity) ? 0.0 : first_state.velocity;
      if (std::isnan(first_state_velocity))
      {
        first_state.velocity = 0.0;
        first_state_velocity = 0.0;
      }
      double second_state_velocity =
        std::isnan(second_state.velocity) ? 0.0 : second_state.velocity;
      if (std::isnan(second_state_velocity))
      {
        second_state.velocity = 0.0;
        second_state_velocity = 0.0;
      }

      second_state.position =
        first_state.position + (first_state_velocity + second_state_velocity) * 0.5 * delta_t;
    }
  }
}

TrajectoryPointConstIter Trajectory::begin(std::size_t axis_index) const
{
  THROW_ON_NULLPTR(trajectory_msg_)

  return trajectory_msg_->axis_trajectories[axis_index].axis_points.begin();
}

TrajectoryPointConstIter Trajectory::end(std::size_t axis_index) const
{
  THROW_ON_NULLPTR(trajectory_msg_)

  return trajectory_msg_->axis_trajectories[axis_index].axis_points.end();
}

rclcpp::Time Trajectory::time_from_start() const { return trajectory_start_time_; }

bool Trajectory::has_trajectory_msg() const { return trajectory_msg_.get() != nullptr; }

bool Trajectory::has_nontrivial_msg(std::size_t axis_index) const
{
  return has_trajectory_msg() &&
         trajectory_msg_->axis_trajectories[axis_index].axis_points.size() > 1;
}

}  // namespace multi_time_trajectory_controller
