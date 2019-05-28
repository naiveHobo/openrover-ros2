#include "rover.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include "tf2/convert.h"
#include <math.h>
#include "data.hpp"
#include "tf2_geometry_msgs/tf2_geometry_msgs.h"
#include "rclcpp/node_options.hpp"
#include "eigen3/Eigen/Dense"

using std::placeholders::_1;
using namespace openrover;
using namespace std::literals::chrono_literals;
using geometry_msgs::msg::Twist;
using geometry_msgs::msg::Vector3;
using namespace openrover_core_msgs;

using Cls = Rover;

Rover::Rover() : Node("rover", rclcpp::NodeOptions().use_intra_process_comms(true))
{
  RCLCPP_INFO(this->get_logger(), "Starting rover driver node");

  sub_raw_data = create_subscription<msg::RawData>("raw_data", rclcpp::QoS(16), std::bind(&Cls::on_raw_data, this, _1));
  double diagnostics_frequency = declare_parameter("diagnostics_frequency", 1.0);
  tmr_diagnostics = create_wall_timer(1s / diagnostics_frequency, std::bind(&Cls::update_diagnostics, this));
  double odometry_frequency = declare_parameter("odometry_frequency", 10.0);
  tmr_odometry = create_wall_timer(1s / odometry_frequency, std::bind(&Cls::update_odom, this));

  sub_cmd_vel =
      create_subscription<geometry_msgs::msg::Twist>("cmd_vel", rclcpp::QoS(1), std::bind(&Cls::on_cmd_vel, this, _1));
  pub_rover_command = create_publisher<openrover_core_msgs::msg::RawCommand>("openrover_command", rclcpp::QoS(16));
  pub_motor_efforts = create_publisher<openrover_core_msgs::msg::RawMotorCommand>("motor_efforts", rclcpp::QoS(1));
  pub_diagnostics = create_publisher<diagnostic_msgs::msg::DiagnosticArray>("diagnostics", rclcpp::QoS(1));
  pub_odom = create_publisher<nav_msgs::msg::Odometry>("odom_raw", rclcpp::QoS(4));

  // based on the physical capabilities of the rover. Depends on the wheel configuration (2wd/4wd/treads) and terrain
  top_speed_linear = declare_parameter("top_speed_linear", 3.05);
  top_speed_angular = declare_parameter("top_speed_angular", 16.2);
  // this value determined by driving straight and dividing distance by average encoder reading
  meters_per_encoder_sum = declare_parameter("meters_per_encoder_sum", 0.0006875);
  // this value determined by driving in a circle and dividing rotation by difference in encoder readings
  radians_per_encoder_difference = declare_parameter("radians_per_encoder_difference", 0.00371);

  odom_frame_id = declare_parameter("odom_frame_id", "odom");
  odom_child_frame_id = declare_parameter("odom_child_frame_id", "base_footprint");
}

/// Takes a number between -1.0 and +1.0 and converts it to the nearest motor command value.
/// values out of range will be clamped to the nearest value in range
uint8_t to_motor_command(double d)
{
  if (std::isnan(d))
  {
    return 125;
  }
  d = trunc(d * 125);
  d = std::min(d, +125.0);
  d = std::max(d, -125.0);
  d += 125;
  return static_cast<uint8_t>(d);
}

void Rover::on_cmd_vel(geometry_msgs::msg::Twist::SharedPtr msg)
{
  // expecting values in the range of +/- linear_top_speed and +/- angular top speed
  auto linear_rate = msg->linear.x;
  if (top_speed_linear < std::abs(linear_rate))
  {
    RCLCPP_WARN(get_logger(), "Requested linear velocity %f higher than maximum %f", linear_rate, top_speed_linear);
  }

  auto turn_rate = msg->angular.z;
  if (top_speed_angular < std::abs(turn_rate))
  {
    RCLCPP_WARN(get_logger(), "Requested angular velocity %f higher than maximum %f", turn_rate, top_speed_angular);
  }

  // convert the requested speeds to per-motor speeds of [-1.0,+1.0]
  double l_motor = (linear_rate / top_speed_linear) - (turn_rate / top_speed_angular);
  double r_motor = (linear_rate / top_speed_linear) + (turn_rate / top_speed_angular);

  // save off wheel direction for odometry purposes
  left_wheel_fwd = (l_motor >= 0);
  right_wheel_fwd = (r_motor >= 0);

  // and translate speeds to the hardware values [0, 250]
  openrover_core_msgs::msg::RawMotorCommand e;
  e.left = to_motor_command(l_motor);
  e.right = to_motor_command(r_motor);
  e.flipper = to_motor_command(0);  // todo

  pub_motor_efforts->publish(e);
}

void openrover::Rover::update_diagnostics()
{
  auto diagnostics = std::make_unique<diagnostic_msgs::msg::DiagnosticArray>();
  diagnostics->header.stamp = get_clock()->now();
  {
    diagnostic_msgs::msg::DiagnosticStatus rover_status;
    rover_status.hardware_id = "openrover";
    if (auto v = get_recent<data::RoverVersion>())
    {
      diagnostic_msgs::msg::KeyValue kv;
      kv.key = "firmware revision";
      kv.value = v->state.to_string();
      rover_status.values.push_back(kv);
    }
    diagnostics->status.push_back(rover_status);
  }
  {
    diagnostic_msgs::msg::DiagnosticStatus encoders_status;
    encoders_status.hardware_id = "openrover/motor encoders";
    if (auto data = get_recent<data::LeftMotorEncoderState>())
    {
      diagnostic_msgs::msg::KeyValue kv;
      kv.key = "left position";
      kv.value = std::to_string(data->state);
      encoders_status.values.push_back(kv);
    }
    if (auto data = get_recent<data::RightMotorEncoderState>())
    {
      diagnostic_msgs::msg::KeyValue kv;
      kv.key = "right position";
      kv.value = std::to_string(data->state);
      encoders_status.values.push_back(kv);
    }
    if (auto data = get_recent<data::LeftMotorEncoderPeriod>())
    {
      diagnostic_msgs::msg::KeyValue kv;
      kv.key = "left period";
      kv.value = std::to_string(data->state);
      encoders_status.values.push_back(kv);
    }
    if (auto data = get_recent<data::RightMotorEncoderPeriod>())
    {
      diagnostic_msgs::msg::KeyValue kv;
      kv.key = "right period";
      kv.value = std::to_string(data->state);
      encoders_status.values.push_back(kv);
    }
    diagnostics->status.push_back(encoders_status);
  }
  pub_diagnostics->publish(std::move(diagnostics));
}

void openrover::Rover::update_odom()
{
  for (auto arg : { data::LeftMotorEncoderState::Which, data::RightMotorEncoderState::Which,
                    data::LeftMotorEncoderPeriod::Which, data::RightMotorEncoderPeriod::Which })
  {
    openrover_core_msgs::msg::RawCommand cmd;
    cmd.verb = 10;
    cmd.arg = arg;
    pub_rover_command->publish(cmd);
  }

  auto now = get_clock()->now();
  auto left_encoder_position = get_recent<data::LeftMotorEncoderState>();
  auto right_encoder_position = get_recent<data::RightMotorEncoderState>();
  auto left_period = get_recent<data::LeftMotorEncoderPeriod>();
  auto right_period = get_recent<data::RightMotorEncoderPeriod>();

  if (!left_encoder_position || !right_encoder_position || !left_period || !right_period)
  {
    RCLCPP_INFO(get_logger(), "Odometry not ready yet");
    return;
  }

  if (odom_last_time.get_clock_type() != RCL_ROS_TIME)
  {
    RCLCPP_INFO(get_logger(), "Initializing odometry");
    odom_last_encoder_position_left = left_encoder_position->state;
    odom_last_encoder_position_right = right_encoder_position->state;
    odom_last_time = now;

    // we have no idea where we are, so this is a reasonable starting value
    odom_last_pos_x = 0;
    odom_last_pos_y = 0;
    odom_last_yaw = 0;
    return;
  }

  if (left_encoder_position->time < odom_last_time || right_encoder_position->time < odom_last_time ||
      left_period->time < odom_last_time || right_period->time < odom_last_time)
  {
    RCLCPP_WARN(get_logger(), "Trying to compute odometry based on stale data");
  }

  Eigen::Vector2d encoder_frequency_lr;
  Eigen::Vector2d encoder_frequency_lr_variance;

  // encoder displacement since last time we did odometry.
  auto dt = (now - odom_last_time).seconds();

  // earlier versions of the firmware don't return the encoder position so we have to do it all based on the encoder
  // frequency
  if (left_encoder_position->state == 0 && right_encoder_position->state == 0)
  {
    encoder_frequency_lr = { ((left_period->state == 0) ? 0 : 1.0 / (left_period->state)),
                             ((right_period->state == 0) ? 0 : 1.0 / (right_period->state)) };
    if (!left_wheel_fwd)
      encoder_frequency_lr[0] *= -1;
    if (!right_wheel_fwd)
      encoder_frequency_lr[1] *= -1;
    // ^ the encoder doesn't actually have the wheel direction. So we fake it by assuming the same direction as the last
    // commanded direction we gave to the wheel
  }
  else
  {
    encoder_frequency_lr = { (left_encoder_position->state - odom_last_encoder_position_left) / dt,
                             (right_encoder_position->state - odom_last_encoder_position_right) / dt };
    // ^ remember these values are signed. But taking the difference a-b as signed ints will give either a-b or 1<<16 -
    // a-b, whichever has the lower absolute value. This is exactly what we want.
  }
  encoder_frequency_lr_variance = (0.1 * encoder_frequency_lr).array().square();

  Eigen::Matrix2d encoder_frequency_lr_to_twist_fl;

  encoder_frequency_lr_to_twist_fl << meters_per_encoder_sum,
      meters_per_encoder_sum,  //
      -radians_per_encoder_difference, +radians_per_encoder_difference;
  auto twist = encoder_frequency_lr_to_twist_fl * encoder_frequency_lr;
  auto twist_covariance = encoder_frequency_lr_to_twist_fl * encoder_frequency_lr_variance.asDiagonal() *
                          encoder_frequency_lr_to_twist_fl.adjoint();

  {
    auto odom = std::make_unique<nav_msgs::msg::Odometry>();
    odom->header.frame_id = odom_frame_id;
    odom->child_frame_id = odom_child_frame_id;
    odom->header.stamp = now;

    // In the odom_frame_id
    odom->pose.covariance.fill(-1.0);

    // In the odom_child_frame_id
    odom->twist.twist.linear.x = twist[0];
    odom->twist.twist.angular.z = twist[1];

    odom->twist.covariance.fill(0.0);
    odom->twist.covariance[0 + 0 * 6] = twist_covariance(0, 0);
    odom->twist.covariance[0 + 5 * 6] = odom->twist.covariance[5 + 0 * 6] = twist_covariance(0, 1);
    odom->twist.covariance[5 + 5 * 6] = twist_covariance(1, 1);

    pub_odom->publish(std::move(odom));
  }

  odom_last_encoder_position_left = left_encoder_position->state;
  odom_last_encoder_position_right = right_encoder_position->state;
  odom_last_time = now;
}

void openrover::Rover::on_raw_data(openrover_core_msgs::msg::RawData::SharedPtr data)
{
  most_recent_data[data->which] = std::make_unique<Timestamped<data::RawValue>>(get_clock()->now(), data->value);
}
