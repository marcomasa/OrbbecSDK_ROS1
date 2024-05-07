/*******************************************************************************
 * Copyright (c) 2023 Orbbec 3D Technology, Inc
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
 *******************************************************************************/

#include "orbbec_camera/ob_camera_node.h"
#include "orbbec_camera/utils.h"
#include <std_msgs/String.h>

namespace orbbec_camera {

void OBCameraNode::setupConfig() {
  stream_name_[DEPTH] = "depth";
  unit_step_size_[DEPTH] = sizeof(uint16_t);
  format_[DEPTH] = OB_FORMAT_Y16;
  image_format_[DEPTH] = CV_16UC1;
  encoding_[DEPTH] = sensor_msgs::image_encodings::TYPE_16UC1;
  format_str_[DEPTH] = "Y16";

  stream_name_[COLOR] = "color";
  unit_step_size_[COLOR] = 3;
  format_[COLOR] = OB_FORMAT_RGB888;
  image_format_[COLOR] = CV_8UC3;
  encoding_[COLOR] = sensor_msgs::image_encodings::RGB8;
  format_str_[COLOR] = "RGB";

  stream_name_[INFRA0] = "ir";
  unit_step_size_[INFRA0] = sizeof(uint16_t);
  format_[INFRA0] = OB_FORMAT_Y16;
  image_format_[INFRA0] = CV_16UC1;
  encoding_[INFRA0] = sensor_msgs::image_encodings::MONO16;
  format_str_[INFRA0] = "Y16";

  stream_name_[INFRA1] = "left_ir";
  unit_step_size_[INFRA1] = sizeof(uint16_t);
  format_[INFRA1] = OB_FORMAT_Y16;
  image_format_[INFRA1] = CV_16UC1;
  encoding_[INFRA1] = sensor_msgs::image_encodings::MONO16;
  format_str_[INFRA1] = "Y16";

  stream_name_[INFRA2] = "right_ir";
  unit_step_size_[INFRA2] = sizeof(uint16_t);
  format_[INFRA2] = OB_FORMAT_Y16;
  image_format_[INFRA2] = CV_16UC1;
  encoding_[INFRA2] = sensor_msgs::image_encodings::MONO16;
  format_str_[INFRA2] = "Y16";
}

void OBCameraNode::selectBaseStream() {
  if (enable_stream_[DEPTH]) {
    base_stream_ = DEPTH;
  } else if (enable_stream_[INFRA0]) {
    base_stream_ = INFRA0;
  } else if (enable_stream_[INFRA1]) {
    base_stream_ = INFRA1;
  } else if (enable_stream_[INFRA2]) {
    base_stream_ = INFRA2;
  } else if (enable_stream_[COLOR]) {
    base_stream_ = COLOR;
  } else {
    ROS_ERROR_STREAM("No base stream is enabled!");
  }
}

void OBCameraNode::setupDevices() {
  auto sensor_list = device_->getSensorList();
  for (size_t i = 0; i < sensor_list->count(); i++) {
    auto sensor = sensor_list->getSensor(i);
    auto profiles = sensor->getStreamProfileList();
    for (size_t j = 0; j < profiles->count(); j++) {
      auto profile = profiles->getProfile(j);
      stream_index_pair sip{profile->type(), 0};
      if (sensors_.find(sip) == sensors_.end()) {
        sensors_[sip] = std::make_shared<ROSOBSensor>(device_, sensor, stream_name_[sip]);
      }
      if (imu_sensor_.find(sip) == imu_sensor_.end()) {
        imu_sensor_[sip] = sensor;
      }
    }
  }
  for (const auto& item : enable_stream_) {
    auto stream_index = item.first;
    auto enable = item.second;
    if (enable && sensors_.find(stream_index) == sensors_.end()) {
      ROS_INFO_STREAM(stream_name_[stream_index]
                      << "sensor isn't supported by current device! -- Skipping...");
      enable_stream_[stream_index] = false;
    }
  }
  if (enable_d2c_viewer_) {
    d2c_viewer_ = std::make_shared<D2CViewer>(nh_, nh_private_);
  }
  CHECK_NOTNULL(device_info_.get());
  if (enable_pipeline_) {
    pipeline_ = std::make_shared<ob::Pipeline>(device_);
  }
  if (enable_sync_output_accel_gyro_) {
    imuPipeline_ = std::make_shared<ob::Pipeline>(device_);
  }

  try {
    if (enable_hardware_d2d_ &&
        device_->isPropertySupported(OB_PROP_DISPARITY_TO_DEPTH_BOOL, OB_PERMISSION_READ_WRITE)) {
      device_->setBoolProperty(OB_PROP_DISPARITY_TO_DEPTH_BOOL, true);
      bool is_hardware_d2d = device_->getBoolProperty(OB_PROP_DISPARITY_TO_DEPTH_BOOL);
      std::string d2d_mode = is_hardware_d2d ? "HW D2D" : "SW D2D";
      ROS_INFO_STREAM("Depth process is " << d2d_mode);
    }
    if (!device_preset_.empty()) {
      device_->loadPreset(device_preset_.c_str());
    }
    auto depth_sensor = device_->getSensor(OB_SENSOR_DEPTH);
    device_->setBoolProperty(OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL, enable_ir_auto_exposure_);
    device_->setBoolProperty(OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, enable_color_auto_exposure_);
    if (color_exposure_ != -1) {
      device_->setIntProperty(OB_PROP_COLOR_EXPOSURE_INT, color_exposure_);
    }
    if (ir_exposure_ != -1) {
      device_->setIntProperty(OB_PROP_DEPTH_EXPOSURE_INT, ir_exposure_);
    }
    if (device_->isPropertySupported(OB_PROP_LASER_CONTROL_INT, OB_PERMISSION_READ_WRITE)) {
      device_->setIntProperty(OB_PROP_LASER_CONTROL_INT, enable_laser_);
    }
    if (device_->isPropertySupported(OB_PROP_LASER_ON_OFF_MODE_INT, OB_PERMISSION_READ_WRITE)) {
      device_->setIntProperty(OB_PROP_LASER_ON_OFF_MODE_INT, laser_on_off_mode_);
    }

    if (!depth_precision_str_.empty() &&
        device_->isPropertySupported(OB_PROP_DEPTH_PRECISION_LEVEL_INT, OB_PERMISSION_READ_WRITE)) {
      auto default_precision_level = device_->getIntProperty(OB_PROP_DEPTH_PRECISION_LEVEL_INT);
      if (default_precision_level != depth_precision_level_) {
        device_->setIntProperty(OB_PROP_DEPTH_PRECISION_LEVEL_INT, depth_precision_level_);
        ROS_INFO_STREAM("set depth precision to " << depth_precision_str_);
      }
    }
    if (!depth_precision_str_.empty() &&
        device_->isPropertySupported(OB_PROP_DEPTH_UNIT_FLEXIBLE_ADJUSTMENT_FLOAT,
                                     OB_PERMISSION_READ_WRITE)) {
      auto depth_unit_flexible_adjustment = depthPrecisionFromString(depth_precision_str_);
      auto range = device_->getFloatPropertyRange(OB_PROP_DEPTH_UNIT_FLEXIBLE_ADJUSTMENT_FLOAT);
      ROS_ERROR_STREAM("Depth unit flexible adjustment range: " << range.min << " - " << range.max);
      if (depth_unit_flexible_adjustment < range.min ||
          depth_unit_flexible_adjustment > range.max) {
        ROS_ERROR_STREAM(
            "depth unit flexible adjustment value is out of range, please check the value");
      } else {
        ROS_INFO_STREAM("set depth unit to " << depth_unit_flexible_adjustment << "mm");
        device_->setFloatProperty(OB_PROP_DEPTH_UNIT_FLEXIBLE_ADJUSTMENT_FLOAT,
                                  depth_unit_flexible_adjustment);
      }
    }
    // set depth sensor to filter
    auto filter_list = depth_sensor->getRecommendedFilters();
    for (size_t i = 0; i < filter_list->count(); i++) {
      auto filter = filter_list->getFilter(i);
      std::map<std::string, bool> filter_params = {
          {"DecimationFilter", enable_decimation_filter_},
          {"HDRMerge", enable_hdr_merge_},
          {"SequencedFilter", enable_sequenced_filter_},
          {"ThresholdFilter", enable_threshold_filter_},
          {"NoiseRemovalFilter", enable_noise_removal_filter_},
          {"SpatialAdvancedFilter", enable_spatial_filter_},
          {"TemporalFilter", enable_temporal_filter_},
          {"HoleFillingFilter", enable_hole_filling_filter_},

      };
      std::string filter_name = filter->type();
      ROS_INFO_STREAM("Setting " << filter_name << "......");
      if (filter_params.find(filter_name) != filter_params.end()) {
        std::string value = filter_params[filter_name] ? "true" : "false";
        ROS_INFO_STREAM("set " << filter_name << " to " << value);
        filter->enable(filter_params[filter_name]);
        filter_status_[filter_name] = filter_params[filter_name];
      }
      if (filter_name == "DecimationFilter" && enable_decimation_filter_) {
        auto decimation_filter = filter->as<ob::DecimationFilter>();
        decimation_filter->setScaleValue(decimation_filter_scale_range_);
      } else if (filter_name == "ThresholdFilter" && enable_threshold_filter_) {
        auto threshold_filter = filter->as<ob::ThresholdFilter>();
        threshold_filter->setValueRange(threshold_filter_min_, threshold_filter_max_);
      } else if (filter_name == "SpatialAdvancedFilter" && enable_spatial_filter_) {
        auto spatial_filter = filter->as<ob::SpatialAdvancedFilter>();
        OBSpatialAdvancedFilterParams params{};
        params.alpha = spatial_filter_alpha_;
        params.magnitude = spatial_filter_magnitude_;
        params.radius = spatial_filter_radius_;
        params.disp_diff = spatial_filter_diff_threshold_;
        spatial_filter->setFilterParams(params);
      } else if (filter_name == "TemporalFilter" && enable_temporal_filter_) {
        auto temporal_filter = filter->as<ob::TemporalFilter>();
        temporal_filter->setDiffScale(temporal_filter_diff_threshold_);
        temporal_filter->setWeight(temporal_filter_weight_);
      } else if (filter_name == "HoleFillingFilter") {
        auto hole_filling_filter = filter->as<ob::HoleFillingFilter>();
        OBHoleFillingMode hole_filling_mode = holeFillingModeFromString(hole_filling_filter_mode_);
        hole_filling_filter->setFilterMode(hole_filling_mode);
      } else if (filter_name == "SequenceIdFilter" && enable_sequenced_filter_) {
        auto sequenced_filter = filter->as<ob::SequenceIdFilter>();
        sequenced_filter->selectSequenceId(sequence_id_filter_id_);
      } else if (filter_name == "NoiseRemovalFilter" && enable_noise_removal_filter_) {
        auto noise_removal_filter = filter->as<ob::NoiseRemovalFilter>();
        OBNoiseRemovalFilterParams params{};
        params.disp_diff = noise_removal_filter_min_diff_;
        params.max_size = noise_removal_filter_max_size_;
        noise_removal_filter->setFilterParams(params);
      } else if (filter_name == "HDRMerge") {
        // do nothing
      } else {
        ROS_INFO_STREAM("Skip setting " << filter_name);
      }
    }
    if (device_->isPropertySupported(OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, OB_PERMISSION_WRITE)) {
      device_->setBoolProperty(OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, enable_color_auto_exposure_);
    }

    if (device_->isPropertySupported(OB_PROP_IR_AUTO_EXPOSURE_BOOL, OB_PERMISSION_WRITE)) {
      device_->setBoolProperty(OB_PROP_IR_AUTO_EXPOSURE_BOOL, enable_ir_auto_exposure_);
    }

    if (device_->isPropertySupported(OB_PROP_IR_LONG_EXPOSURE_BOOL, OB_PERMISSION_WRITE)) {
      device_->setBoolProperty(OB_PROP_IR_LONG_EXPOSURE_BOOL, enable_ir_long_exposure_);
    }

  } catch (const ob::Error& e) {
    ROS_ERROR_STREAM("Failed to setup devices: " << e.getMessage());
  } catch (const std::exception& e) {
    ROS_ERROR_STREAM("Failed to setup devices: " << e.what());
  }
}

void OBCameraNode::setupFrameCallback() {
  for (const auto& stream_index : IMAGE_STREAMS) {
    if (enable_stream_[stream_index]) {
      auto callback = [this, stream_index](std::shared_ptr<ob::Frame> frame) {
        this->onNewFrameCallback(frame, stream_index);
      };
      frame_callback_[stream_index] = callback;
    }
  }
}

bool OBCameraNode::setupFormatConvertType(OBFormat type) {
  switch (type) {
    case OB_FORMAT_I420:
      format_convert_filter_.setFormatConvertType(FORMAT_I420_TO_RGB888);
      break;
    case OB_FORMAT_MJPG:
      format_convert_filter_.setFormatConvertType(FORMAT_MJPEG_TO_RGB888);
      break;
    case OB_FORMAT_YUYV:
      format_convert_filter_.setFormatConvertType(FORMAT_YUYV_TO_RGB888);
      break;
    case OB_FORMAT_NV21:
      format_convert_filter_.setFormatConvertType(FORMAT_NV21_TO_RGB888);
      break;
    case OB_FORMAT_NV12:
      format_convert_filter_.setFormatConvertType(FORMAT_NV12_TO_RGB888);
      break;
    case OB_FORMAT_UYVY:
      format_convert_filter_.setFormatConvertType(FORMAT_UYVY_TO_RGB888);
      break;
    default:
      return false;
  }
  return true;
}

void OBCameraNode::setupProfiles() {
  for (const auto& stream_index : IMAGE_STREAMS) {
    if (!enable_stream_[stream_index] && stream_index != base_stream_) {
      continue;
    }
    try {
      auto profile_list = sensors_[stream_index]->getStreamProfileList();
      supported_profiles_[stream_index] = profile_list;
      auto selected_profile = profile_list->getVideoStreamProfile(
          width_[stream_index], height_[stream_index], format_[stream_index], fps_[stream_index]);
      auto default_profile = profile_list->getVideoStreamProfile(
          width_[stream_index], height_[stream_index], format_[stream_index]);
      if (!selected_profile) {
        ROS_WARN_STREAM("Given stream configuration is not supported by the device! "
                        << " Stream: " << stream_name_[stream_index]
                        << ", Width: " << width_[stream_index]
                        << ", Height: " << height_[stream_index] << ", FPS: " << fps_[stream_index]
                        << ", Format: " << format_[stream_index]);
        if (default_profile) {
          ROS_WARN_STREAM("Using default profile instead.");
          ROS_WARN_STREAM("default FPS " << default_profile->fps());
          selected_profile = default_profile;
        } else {
          ROS_WARN_STREAM(" NO default_profile found , Stream: " << stream_index.first
                                                                 << " will be disable");
          enable_stream_[stream_index] = false;
          continue;
        }
      }
      CHECK_NOTNULL(selected_profile.get());
      stream_profile_[stream_index] = selected_profile;
      images_[stream_index] = cv::Mat(height_[stream_index], width_[stream_index],
                                      image_format_[stream_index], cv::Scalar(0, 0, 0));
      ROS_INFO_STREAM(" stream " << stream_name_[stream_index] << " is enabled - width: "
                                 << width_[stream_index] << ", height: " << height_[stream_index]
                                 << ", fps: " << fps_[stream_index] << ", "
                                 << "Format: " << OBFormatToString(format_[stream_index]));
    } catch (const ob::Error& e) {
      ROS_ERROR_STREAM("Failed to setup << " << stream_name_[stream_index]
                                             << " profile: " << e.getMessage());
    }
  }
  // IMU
  for (const auto& stream_index : HID_STREAMS) {
    if (!enable_stream_[stream_index]) {
      continue;
    }
    try {
      auto profile_list = sensors_[stream_index]->getStreamProfileList();
      supported_profiles_[stream_index] = profile_list;
      if (stream_index == ACCEL) {
        auto full_scale_range = fullAccelScaleRangeFromString(imu_range_[stream_index]);
        auto sample_rate = sampleRateFromString(imu_rate_[stream_index]);
        auto profile = profile_list->getAccelStreamProfile(full_scale_range, sample_rate);
        stream_profile_[stream_index] = profile;
      } else if (stream_index == GYRO) {
        auto full_scale_range = fullGyroScaleRangeFromString(imu_range_[stream_index]);
        auto sample_rate = sampleRateFromString(imu_rate_[stream_index]);
        auto profile = profile_list->getGyroStreamProfile(full_scale_range, sample_rate);
        stream_profile_[stream_index] = profile;
      }
      ROS_INFO_STREAM("stream " << stream_name_[stream_index] << " full scale range "
                                << imu_range_[stream_index] << " sample rate "
                                << imu_rate_[stream_index]);
    } catch (const ob::Error& e) {
      ROS_ERROR_STREAM("Failed to setup << " << stream_name_[stream_index]
                                             << " profile: " << e.getMessage());
      enable_stream_[stream_index] = false;
      stream_profile_[stream_index] = nullptr;
    }
  }
  if (!enable_pipeline_ && (depth_registration_ || enable_colored_point_cloud_)) {
    int index = getCameraParamIndex();
    try {
      device_->setIntProperty(OB_PROP_DEPTH_ALIGN_HARDWARE_MODE_INT, index);
      device_->setBoolProperty(OB_PROP_DEPTH_ALIGN_HARDWARE_BOOL, depth_registration_);
    } catch (ob::Error& e) {
      ROS_ERROR_STREAM("set d2c error " << e.getMessage());
    }
  }
  if (depth_registration_ || enable_colored_point_cloud_) {
    align_filter_ = std::make_shared<ob::Align>(align_target_stream_);
  }
}

void OBCameraNode::setupTopics() {
  setupPublishers();
  if (publish_tf_) {
    publishStaticTransforms();
  }
}

void OBCameraNode::setupPublishers() {
  image_transport::ImageTransport image_transport(nh_);
  for (const auto& stream_index : IMAGE_STREAMS) {
    if (!enable_stream_[stream_index]) {
      continue;
    }
    std::string name = stream_name_[stream_index];
    std::string topic_name = "/" + camera_name_ + "/" + name + "/image_raw";
    image_transport::SubscriberStatusCallback it_subscribed_cb =
        boost::bind(&OBCameraNode::imageSubscribedCallback, this, stream_index);
    image_transport::SubscriberStatusCallback it_unsubscribed_cb =
        boost::bind(&OBCameraNode::imageUnsubscribedCallback, this, stream_index);
    image_publishers_[stream_index] =
        image_transport.advertise(topic_name, 1, it_subscribed_cb, it_unsubscribed_cb);
    topic_name = "/" + camera_name_ + "/" + name + "/camera_info";
    ros::SubscriberStatusCallback image_subscribed_cb =
        boost::bind(&OBCameraNode::imageSubscribedCallback, this, stream_index);
    ros::SubscriberStatusCallback image_unsubscribed_cb =
        boost::bind(&OBCameraNode::imageUnsubscribedCallback, this, stream_index);
    camera_info_publishers_[stream_index] = nh_.advertise<sensor_msgs::CameraInfo>(
        topic_name, 1, image_subscribed_cb, image_unsubscribed_cb);
    CHECK_NOTNULL(device_info_.get());
    if (isGemini335PID(device_info_->pid())) {
      metadata_publishers_[stream_index] =
          nh_.advertise<orbbec_camera::Metadata>("/" + camera_name_ + "/" + name + "/metadata", 1,
                                                 image_subscribed_cb, image_unsubscribed_cb);
    }
  }
  if (enable_point_cloud_ && enable_stream_[DEPTH]) {
    ros::SubscriberStatusCallback depth_cloud_subscribed_cb =
        boost::bind(&OBCameraNode::pointCloudSubscribedCallback, this);
    ros::SubscriberStatusCallback depth_cloud_unsubscribed_cb =
        boost::bind(&OBCameraNode::pointCloudUnsubscribedCallback, this);
    depth_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>(
        "depth/points", 1, depth_cloud_subscribed_cb, depth_cloud_unsubscribed_cb);
  }
  if (enable_colored_point_cloud_ && enable_stream_[DEPTH] && enable_stream_[COLOR]) {
    ros::SubscriberStatusCallback depth_registered_cloud_subscribed_cb =
        boost::bind(&OBCameraNode::coloredPointCloudSubscribedCallback, this);
    ros::SubscriberStatusCallback depth_registered_cloud_unsubscribed_cb =
        boost::bind(&OBCameraNode::coloredPointCloudUnsubscribedCallback, this);
    depth_registered_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>(
        "depth_registered/points", 1, depth_registered_cloud_subscribed_cb,
        depth_registered_cloud_unsubscribed_cb);
  }

  if (enable_sync_output_accel_gyro_) {
    std::string topic_name = stream_name_[GYRO] + "_" + stream_name_[ACCEL] + "/sample";
    ros::SubscriberStatusCallback imu_subscribed_cb =
        boost::bind(&OBCameraNode::imuSubscribedCallback, this, GYRO);
    ros::SubscriberStatusCallback imu_unsubscribed_cb =
        boost::bind(&OBCameraNode::imuUnsubscribedCallback, this, GYRO);
    imu_gyro_accel_publisher_ =
        nh_.advertise<sensor_msgs::Imu>(topic_name, 1, imu_subscribed_cb, imu_unsubscribed_cb);
    topic_name = stream_name_[GYRO] + "/imu_info";
    imu_info_publishers_[GYRO] = nh_.advertise<orbbec_camera::IMUInfo>(
        topic_name, 1, imu_subscribed_cb, imu_unsubscribed_cb);
    topic_name = stream_name_[ACCEL] + "/imu_info";
    imu_info_publishers_[ACCEL] = nh_.advertise<orbbec_camera::IMUInfo>(
        topic_name, 1, imu_subscribed_cb, imu_unsubscribed_cb);
  } else {
    for (const auto& stream_index : HID_STREAMS) {
      if (!enable_stream_[stream_index]) {
        continue;
      }
      std::string topic_name = stream_name_[stream_index] + "/sample";
      ros::SubscriberStatusCallback imu_subscribed_cb =
          boost::bind(&OBCameraNode::imuSubscribedCallback, this, stream_index);
      ros::SubscriberStatusCallback imu_unsubscribed_cb =
          boost::bind(&OBCameraNode::imuUnsubscribedCallback, this, stream_index);
      imu_publishers_[stream_index] =
          nh_.advertise<sensor_msgs::Imu>(topic_name, 1, imu_subscribed_cb, imu_unsubscribed_cb);
      topic_name = stream_name_[stream_index] + "/imu_info";
      imu_info_publishers_[stream_index] = nh_.advertise<orbbec_camera::IMUInfo>(
          topic_name, 1, imu_subscribed_cb, imu_unsubscribed_cb);
    }
  }
  if (enable_stream_[DEPTH] && enable_stream_[INFRA0]) {
    depth_to_other_extrinsics_publishers_[INFRA0] =
        nh_.advertise<orbbec_camera::Extrinsics>("/" + camera_name_ + "/depth_to_ir", 1, true);
  }
  if (enable_stream_[DEPTH] && enable_stream_[COLOR]) {
    depth_to_other_extrinsics_publishers_[COLOR] =
        nh_.advertise<orbbec_camera::Extrinsics>("/" + camera_name_ + "/depth_to_color", 1, true);
  }
  if (enable_stream_[DEPTH] && enable_stream_[INFRA1]) {
    depth_to_other_extrinsics_publishers_[INFRA1] =
        nh_.advertise<orbbec_camera::Extrinsics>("/" + camera_name_ + "/depth_to_left_ir", 1, true);
  }
  if (enable_stream_[DEPTH] && enable_stream_[INFRA2]) {
    depth_to_other_extrinsics_publishers_[INFRA2] = nh_.advertise<orbbec_camera::Extrinsics>(
        "/" + camera_name_ + "/depth_to_right_ir", 1, true);
  }
  if (enable_stream_[DEPTH] && enable_stream_[ACCEL]) {
    depth_to_other_extrinsics_publishers_[ACCEL] =
        nh_.advertise<orbbec_camera::Extrinsics>("/" + camera_name_ + "/depth_to_accel", 1, true);
  }
  if (enable_stream_[DEPTH] && enable_stream_[GYRO]) {
    depth_to_other_extrinsics_publishers_[GYRO] =
        nh_.advertise<orbbec_camera::Extrinsics>("/" + camera_name_ + "/depth_to_gyro", 1, true);
  }
  filter_status_pub_ =
      nh_.advertise<std_msgs::String>("/" + camera_name_ + "/filter_status", 1, true);
  std_msgs::String msg;
  msg.data = filter_status_.dump(2);
  filter_status_pub_.publish(msg);
}

void OBCameraNode::setupCameraInfo() {
  color_camera_info_manager_ = std::make_shared<camera_info_manager::CameraInfoManager>(
      nh_rgb_, camera_name_ + "_" + stream_name_[COLOR], color_info_uri_);
  ir_camera_info_manager_ = std::make_shared<camera_info_manager::CameraInfoManager>(
      nh_ir_, camera_name_ + "_" + stream_name_[INFRA0], ir_info_uri_);
  auto param = getCameraParam();
  if (param) {
    camera_infos_[DEPTH] = convertToCameraInfo(param->depthIntrinsic, param->depthDistortion,
                                               param->depthIntrinsic.width);
    camera_infos_[INFRA0] = convertToCameraInfo(param->depthIntrinsic, param->depthDistortion,
                                                param->depthIntrinsic.width);
    camera_infos_[COLOR] =
        convertToCameraInfo(param->rgbIntrinsic, param->rgbDistortion, param->rgbIntrinsic.width);
  } else {
    // ROS_WARN_STREAM("Failed to get camera parameters");
  }
}

void OBCameraNode::setupPipelineConfig() {
  if (pipeline_config_) {
    pipeline_config_.reset();
  }
  pipeline_config_ = std::make_shared<ob::Config>();
  auto pid = device_info_->pid();
  if (depth_registration_ && enable_stream_[COLOR] && enable_stream_[DEPTH]) {
    OBAlignMode align_mode = align_mode_ == "HW" ? ALIGN_D2C_HW_MODE : ALIGN_D2C_SW_MODE;
    ROS_INFO_STREAM("set align mode to " << align_mode_);
    pipeline_config_->setAlignMode(align_mode);
    pipeline_config_->setDepthScaleRequire(enable_depth_scale_);
  }
  for (const auto& stream_index : IMAGE_STREAMS) {
    if (enable_stream_[stream_index]) {
      pipeline_config_->enableStream(stream_profile_[stream_index]);
    }
  }
}

void OBCameraNode::diagnosticTemperature(diagnostic_updater::DiagnosticStatusWrapper& stat) {
  try {
    OBDeviceTemperature temperature;
    uint32_t data_size = sizeof(OBDeviceTemperature);
    device_->getStructuredData(OB_STRUCT_DEVICE_TEMPERATURE, &temperature, &data_size);
    stat.add("CPU Temperature", temperature.cpuTemp);
    stat.add("IR Temperature", temperature.irTemp);
    stat.add("LDM Temperature", temperature.ldmTemp);
    stat.add("MainBoard Temperature", temperature.mainBoardTemp);
    stat.add("TEC Temperature", temperature.tecTemp);
    stat.add("IMU Temperature", temperature.imuTemp);
    stat.add("RGB Temperature", temperature.rgbTemp);
    stat.add("Left IR Temperature", temperature.irLeftTemp);
    stat.add("Right IR Temperature", temperature.irRightTemp);
    stat.add("Chip Top Temperature", temperature.chipTopTemp);
    stat.add("Chip Bottom Temperature", temperature.chipBottomTemp);
    stat.summary(diagnostic_msgs::DiagnosticStatus::OK, "Temperature is normal");
  } catch (const ob::Error& e) {
    stat.summary(diagnostic_msgs::DiagnosticStatus::ERROR, e.getMessage());
  }
}
void OBCameraNode::setupDiagnosticUpdater() {
  std::string serial_number = device_info_->serialNumber();
  diagnostic_updater_ =
      std::make_shared<diagnostic_updater::Updater>(nh_, nh_private_, "ob_camera_" + serial_number);
  diagnostic_updater_->setHardwareID(serial_number);
  ros::WallRate rate(diagnostics_frequency_);
  diagnostic_updater_->add("Temperature", this, &OBCameraNode::diagnosticTemperature);
  while (is_running_ && ros::ok()) {
    diagnostic_updater_->force_update();
    rate.sleep();
  }
}

void OBCameraNode::readDefaultGain() {
  for (const auto& stream_index : IMAGE_STREAMS) {
    if (!enable_stream_[stream_index]) {
      continue;
    }
    try {
      auto sensor = sensors_[stream_index];
      CHECK_NOTNULL(sensor.get());
      auto gain = sensor->getGain();
      ROS_INFO_STREAM("stream " << stream_name_[stream_index] << " gain " << gain);
      default_gain_[stream_index] = gain;
    } catch (ob::Error& e) {
      default_gain_[stream_index] = 0;
      ROS_DEBUG_STREAM("get gain error " << e.getMessage());
    }
  }
}

void OBCameraNode::readDefaultExposure() {
  for (const auto& stream_index : IMAGE_STREAMS) {
    if (!enable_stream_[stream_index]) {
      continue;
    }
    try {
      auto sensor = sensors_[stream_index];
      CHECK_NOTNULL(sensor.get());
      auto exposure = sensor->getExposure();
      ROS_INFO_STREAM("stream " << stream_name_[stream_index] << " exposure " << exposure);
      default_exposure_[stream_index] = exposure;
    } catch (ob::Error& e) {
      default_exposure_[stream_index] = 0;
      ROS_DEBUG_STREAM("get " << stream_name_[stream_index] << " exposure error "
                              << e.getMessage());
    }
  }
}

void OBCameraNode::readDefaultWhiteBalance() {
  try {
    auto sensor = sensors_[COLOR];
    if (!sensor) {
      ROS_INFO_STREAM("does not have color sensor");
      return;
    }
    CHECK_NOTNULL(sensor.get());
    auto wb = sensor->getWhiteBalance();
    ROS_INFO_STREAM("stream " << stream_name_[COLOR] << " wb " << wb);
    default_white_balance_ = wb;
  } catch (ob::Error& e) {
    default_white_balance_ = 0;
    ROS_DEBUG_STREAM("get white balance error " << e.getMessage());
  }
}
}  // namespace orbbec_camera
