/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#include "modules/control/control_component.h"

#include <iomanip>
#include <string>

#include "modules/common/adapters/adapter_gflags.h"
#include "modules/common/log.h"
#include "modules/common/time/time.h"
#include "modules/common/vehicle_state/vehicle_state_provider.h"
#include "modules/control/common/control_gflags.h"

DECLARE_string(flagfile);

namespace apollo {
namespace control {

using apollo::canbus::Chassis;
using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::VehicleStateProvider;
using apollo::common::monitor::MonitorMessageItem;
using apollo::common::time::Clock;
using apollo::control::ControlCommand;
using apollo::control::PadMessage;
using apollo::localization::LocalizationEstimate;
using apollo::planning::ADCTrajectory;

bool ControlComponent::Init() {
  // init_time_ = Clock::NowInSeconds();

  AINFO << "Control init, starting ...";

  AINFO << "Loading gflag from file: " << ConfigFilePath();
  google::SetCommandLineOption("flagfile", ConfigFilePath().c_str());

  CHECK(apollo::cybertron::common::GetProtoFromFile(FLAGS_control_conf_file,
                                                    &control_conf_))
      << "Unable to load control conf file: " + FLAGS_control_conf_file;

  AINFO << "Conf file: " << FLAGS_control_conf_file << " is loaded.";

  AINFO << "Conf file: " << ConfigFilePath() << " is loaded.";

  // common::monitor::MonitorLogBuffer buffer(&monitor_logger_);

  // set controller
  if (!controller_agent_.Init(&control_conf_).ok()) {
    std::string error_msg = "Control init controller failed! Stopping...";
    //  buffer.ERROR(error_msg);
    return false;
  }

  chassis_reader_ = node_->CreateReader<Chassis>(
      control_conf_.chassis_channel(),
      [this](const std::shared_ptr<Chassis> &chassis) {
        ADEBUG << "Received chassis data: run chassis callback.";
        std::lock_guard<std::mutex> lock(mutex_);
        chassis_.CopyFrom(*chassis);
      });

  trajectory_reader_ = node_->CreateReader<ADCTrajectory>(
      control_conf_.chassis_channel(),
      [this](const std::shared_ptr<ADCTrajectory> &trajectory) {
        ADEBUG << "Received chassis data: run trajectory callback.";
        std::lock_guard<std::mutex> lock(mutex_);
        trajectory_.CopyFrom(*trajectory);
      });

  localization_reader_ = node_->CreateReader<LocalizationEstimate>(
      control_conf_.localization_channel(),
      [this](const std::shared_ptr<LocalizationEstimate> &localization) {
        ADEBUG << "Received control data: run localization message callback.";
        std::lock_guard<std::mutex> lock(mutex_);
        localization_.CopyFrom(*localization);
      });

  pad_msg_reader_ = node_->CreateReader<PadMessage>(
      control_conf_.pad_msg_channel(),
      [this](const std::shared_ptr<PadMessage> &pad_msg) {
        ADEBUG << "Received control data: run pad message callback.";
        OnPad(*pad_msg);
      });

  control_cmd_writer_ = node_->CreateWriter<ControlCommand>(
      control_conf_.control_command_channel());

  return true;
}

void ControlComponent::OnPad(const PadMessage &pad) {
  pad_msg_ = pad;
  ADEBUG << "Received Pad Msg:" << pad.DebugString();
  AERROR_IF(!pad_msg_.has_action()) << "pad message check failed!";

  // do something according to pad message
  if (pad_msg_.action() == DrivingAction::RESET) {
    AINFO << "Control received RESET action!";
    estop_ = false;
    estop_reason_.clear();
  }
  pad_received_ = true;
}

void ControlComponent::OnMonitor(
    const common::monitor::MonitorMessage &monitor_message) {
  for (const auto &item : monitor_message.item()) {
    if (item.log_level() == MonitorMessageItem::FATAL) {
      estop_ = true;
      return;
    }
  }
}

Status ControlComponent::ProduceControlCommand(
    ControlCommand *control_command) {
  Status status = CheckInput();
  // check data

  if (!status.ok()) {
    AERROR_EVERY(100) << "Control input data failed: "
                      << status.error_message();
    control_command->mutable_engage_advice()->set_advice(
        apollo::common::EngageAdvice::DISALLOW_ENGAGE);
    control_command->mutable_engage_advice()->set_reason(
        status.error_message());
    estop_ = true;
    estop_reason_ = status.error_message();
  } else {
    Status status_ts = CheckTimestamp();
    if (!status_ts.ok()) {
      AERROR << "Input messages timeout";
      // estop_ = true;
      status = status_ts;
      if (chassis_.driving_mode() !=
          apollo::canbus::Chassis::COMPLETE_AUTO_DRIVE) {
        control_command->mutable_engage_advice()->set_advice(
            apollo::common::EngageAdvice::DISALLOW_ENGAGE);
        control_command->mutable_engage_advice()->set_reason(
            status.error_message());
      }
    } else {
      control_command->mutable_engage_advice()->set_advice(
          apollo::common::EngageAdvice::READY_TO_ENGAGE);
    }
  }

  // check estop
  estop_ = control_conf_.enable_persistent_estop()
               ? estop_ || trajectory_.estop().is_estop()
               : trajectory_.estop().is_estop();

  if (trajectory_.estop().is_estop()) {
    estop_reason_ = "estop from planning";
  }

  // if planning set estop, then no control process triggered
  if (!estop_) {
    if (chassis_.driving_mode() == Chassis::COMPLETE_MANUAL) {
      controller_agent_.Reset();
      AINFO_EVERY(100) << "Reset Controllers in Manual Mode";
    }

    auto debug = control_command->mutable_debug()->mutable_input_debug();
    debug->mutable_localization_header()->CopyFrom(localization_.header());
    debug->mutable_canbus_header()->CopyFrom(chassis_.header());
    debug->mutable_trajectory_header()->CopyFrom(trajectory_.header());

    Status status_compute = controller_agent_.ComputeControlCommand(
        &localization_, &chassis_, &trajectory_, control_command);

    if (!status_compute.ok()) {
      AERROR << "Control main function failed"
             << " with localization: " << localization_.ShortDebugString()
             << " with chassis: " << chassis_.ShortDebugString()
             << " with trajectory: " << trajectory_.ShortDebugString()
             << " with cmd: " << control_command->ShortDebugString()
             << " status:" << status_compute.error_message();
      estop_ = true;
      estop_reason_ = status_compute.error_message();
      status = status_compute;
    }
  }

  if (estop_) {
    AWARN_EVERY(100) << "Estop triggered! No control core method executed!";
    // set Estop command
    control_command->set_speed(0);
    control_command->set_throttle(0);
    control_command->set_brake(control_conf_.soft_estop_brake());
    control_command->set_gear_location(Chassis::GEAR_DRIVE);
  }
  // check signal
  if (trajectory_.decision().has_vehicle_signal()) {
    control_command->mutable_signal()->CopyFrom(
        trajectory_.decision().vehicle_signal());
  }
  return status;
}

bool ControlComponent::Proc() {
  // set initial vehicle state by cmd
  // need to sleep, because advertised channel is not ready immediately
  // simple test shows a short delay of 80 ms or so
  AINFO << "Control resetting vehicle state, sleeping for 1000 ms ...";
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  // should init_vehicle first, let car enter work status, then use status msg
  // trigger control

  AINFO << "Control default driving action is "
        << DrivingAction_Name(control_conf_.action());
  pad_msg_.set_action(control_conf_.action());

  double start_timestamp = Clock::NowInSeconds();

  if (control_conf_.is_control_test_mode() &&
      control_conf_.control_test_duration() > 0 &&
      (start_timestamp - init_time_) > control_conf_.control_test_duration()) {
    AERROR << "Control finished testing. exit";
    return false;
  }

  ControlCommand control_command;

  Status status = ProduceControlCommand(&control_command);
  AERROR_IF(!status.ok()) << "Failed to produce control command:"
                          << status.error_message();

  double end_timestamp = Clock::NowInSeconds();

  if (pad_received_) {
    control_command.mutable_pad_msg()->CopyFrom(pad_msg_);
    pad_received_ = false;
  }

  const double time_diff_ms = (end_timestamp - start_timestamp) * 1000;
  control_command.mutable_latency_stats()->set_total_time_ms(time_diff_ms);
  control_command.mutable_latency_stats()->set_total_time_exceeded(
      time_diff_ms < control_conf_.control_period());
  ADEBUG << "control cycle time is: " << time_diff_ms << " ms.";
  status.Save(control_command.mutable_header()->mutable_status());

  // forward estop reason among following control frames.
  if (estop_) {
    control_command.mutable_header()->mutable_status()->set_msg(estop_reason_);
  }

  SendCmd(control_command);

  return true;
}

Status ControlComponent::CheckInput() {
  if (localization_reader_ == nullptr) {
    AWARN_EVERY(100) << "No Localization msg yet. ";
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "No localization msg");
  }
  ADEBUG << "Received localization:" << localization_.ShortDebugString();

  if (chassis_reader_ == nullptr) {
    AWARN_EVERY(100) << "No Chassis msg yet. ";
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "No chassis msg");
  }
  ADEBUG << "Received chassis:" << chassis_.ShortDebugString();

  if (trajectory_reader_ == nullptr) {
    AWARN_EVERY(100) << "No planning msg yet. ";
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "No planning msg");
  }
  if (!trajectory_.estop().is_estop() &&
      trajectory_.trajectory_point_size() == 0) {
    AWARN_EVERY(100) << "planning has no trajectory point. ";
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR,
                  "planning has no trajectory point.");
  }

  for (auto &trajectory_point : *trajectory_.mutable_trajectory_point()) {
    if (trajectory_point.v() < control_conf_.minimum_speed_resolution()) {
      trajectory_point.set_v(0.0);
      trajectory_point.set_a(0.0);
    }
  }

  VehicleStateProvider::Instance()->Update(localization_, chassis_);

  return Status::OK();
}

Status ControlComponent::CheckTimestamp() {
  if (!control_conf_.enable_input_timestamp_check() ||
      control_conf_.is_control_test_mode()) {
    ADEBUG << "Skip input timestamp check by gflags.";
    return Status::OK();
  }
  double current_timestamp = Clock::NowInSeconds();
  double localization_diff =
      current_timestamp - localization_.header().timestamp_sec();
  if (localization_diff > (control_conf_.max_localization_miss_num() *
                           control_conf_.localization_period())) {
    AERROR << "Localization msg lost for " << std::setprecision(6)
           << localization_diff << "s";
    // common::monitor::MonitorLogBuffer buffer(&monitor_logger_);
    // buffer.ERROR("Localization msg lost");
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "Localization msg timeout");
  }

  double chassis_diff = current_timestamp - chassis_.header().timestamp_sec();
  if (chassis_diff >
      (control_conf_.max_chassis_miss_num() * control_conf_.chassis_period())) {
    AERROR << "Chassis msg lost for " << std::setprecision(6) << chassis_diff
           << "s";
    //    common::monitor::MonitorLogBuffer buffer(&monitor_logger_);
    //    buffer.ERROR("Chassis msg lost");
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "Chassis msg timeout");
  }

  double trajectory_diff =
      current_timestamp - trajectory_.header().timestamp_sec();
  if (trajectory_diff > (control_conf_.max_planning_miss_num() *
                         control_conf_.trajectory_period())) {
    AERROR << "Trajectory msg lost for " << std::setprecision(6)
           << trajectory_diff << "s";
    //    common::monitor::MonitorLogBuffer buffer(&monitor_logger_);
    //  buffer.ERROR("Trajectory msg lost");
    return Status(ErrorCode::CONTROL_COMPUTE_ERROR, "Trajectory msg timeout");
  }
  return Status::OK();
}

void ControlComponent::SendCmd(ControlCommand &control_command) {
  // set header
  control_command.mutable_header()->set_lidar_timestamp(
      trajectory_.header().lidar_timestamp());
  control_command.mutable_header()->set_camera_timestamp(
      trajectory_.header().camera_timestamp());
  control_command.mutable_header()->set_radar_timestamp(
      trajectory_.header().radar_timestamp());

  common::util::FillHeader(node_->Name(), &control_command);

  ADEBUG << control_command.ShortDebugString();
  if (control_conf_.is_control_test_mode()) {
    ADEBUG << "Skip publish control command in test mode";
    return;
  }

  control_cmd_writer_->Write(std::make_shared<ControlCommand>(control_command));
}

}  // namespace control
}  // namespace apollo