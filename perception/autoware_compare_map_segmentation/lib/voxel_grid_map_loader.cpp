// Copyright 2023 Autoware Foundation
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

#include "autoware/compare_map_segmentation/voxel_grid_map_loader.hpp"

#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace autoware::compare_map_segmentation
{
VoxelGridMapLoader::VoxelGridMapLoader(
  rclcpp::Node * node, double leaf_size, double downsize_ratio_z_axis,
  std::string * tf_map_input_frame)
: logger_(node->get_logger()),
  voxel_leaf_size_(leaf_size),
  downsize_ratio_z_axis_(downsize_ratio_z_axis)
{
  tf_map_input_frame_ = tf_map_input_frame;

  downsampled_map_pub_ = node->create_publisher<sensor_msgs::msg::PointCloud2>(
    "debug/downsampled_map/pointcloud", rclcpp::QoS{1}.transient_local());
  debug_ = node->declare_parameter<bool>("publish_debug_pcd");

  // initiate diagnostic status
  diagnostics_map_voxel_status_.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
  diagnostics_map_voxel_status_.message = "VoxelGridMapLoader initialized.";
}

// check if the pointcloud is filterable with PCL voxel grid
bool VoxelGridMapLoader::isFeasibleWithPCLVoxelGrid(
  const pcl::PointCloud<pcl::PointXYZ>::ConstPtr & pointcloud,
  const pcl::VoxelGrid<pcl::PointXYZ> & voxel_grid)
{
  // obtain the total voxel number
  pcl::PointXYZ min_pt, max_pt;
  pcl::getMinMax3D(*pointcloud, min_pt, max_pt);
  const double x_range = max_pt.x - min_pt.x;
  const double y_range = max_pt.y - min_pt.y;
  const double z_range = max_pt.z - min_pt.z;

  const auto voxel_leaf_size = voxel_grid.getLeafSize();
  const double inv_voxel_x = 1.0 / voxel_leaf_size[0];
  const double inv_voxel_y = 1.0 / voxel_leaf_size[1];
  const double inv_voxel_z = 1.0 / voxel_leaf_size[2];

  const std::int64_t x_voxel_num = std::ceil(x_range * inv_voxel_x) + 1;
  const std::int64_t y_voxel_num = std::ceil(y_range * inv_voxel_y) + 1;
  const std::int64_t z_voxel_num = std::ceil(z_range * inv_voxel_z) + 1;
  const std::int64_t voxel_num = x_voxel_num * y_voxel_num * z_voxel_num;

  // check if the voxel_num is within the range of int32_t
  if (voxel_num > std::numeric_limits<std::int32_t>::max()) {
    // voxel_num overflows int32_t limit
    diagnostics_map_voxel_status_.level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
    diagnostics_map_voxel_status_.message =
      "Given map voxel grid is not feasible. (Number of voxel overflows int32_t limit) "
      "Check the voxel grid filter parameters and input pointcloud map."
      "  (1) If use_dynamic_map_loading is false, consider to enable use_dynamic_map_loading"
      "  (2) If use_dynamic_map_loading is true, consider to adjust map pointcloud split size "
      "smaller"
      "     and confirm the given pointcloud map is separated sufficiently."
      "  (2) If static map is only the option, consider to enlarge distance_threshold to generate "
      "     more larger leaf size";
    return false;
  }
  // voxel_num is within the range of int32_t
  diagnostics_map_voxel_status_.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
  diagnostics_map_voxel_status_.message = "Given map voxel grid is within the feasible range";
  return true;
}

void VoxelGridMapLoader::publish_downsampled_map(
  const pcl::PointCloud<pcl::PointXYZ> & downsampled_pc)
{
  sensor_msgs::msg::PointCloud2 downsampled_map_msg;
  pcl::toROSMsg(downsampled_pc, downsampled_map_msg);
  downsampled_map_msg.header.frame_id = "map";
  downsampled_map_pub_->publish(downsampled_map_msg);
}

bool VoxelGridMapLoader::is_close_to_neighbor_voxels(
  const pcl::PointXYZ & point, const double distance_threshold, VoxelGridPointXYZ & voxel,
  pcl::search::Search<pcl::PointXYZ>::Ptr tree)
{
  const int index = voxel.getCentroidIndexAt(voxel.getGridCoordinates(point.x, point.y, point.z));
  if (index != -1) {
    return true;
  }
  if (tree == nullptr) {
    return false;
  }
  std::vector<int> nn_indices(1);
  std::vector<float> nn_distances(1);
  if (tree->radiusSearch(point, distance_threshold, nn_indices, nn_distances, 1) == 0) {
    return false;
  }
  return true;
}

bool VoxelGridMapLoader::is_close_to_neighbor_voxels(
  const pcl::PointXYZ & point, const double distance_threshold, const FilteredPointCloudPtr & map,
  VoxelGridPointXYZ & voxel) const
{
  // check map downsampled pc
  double distance_threshold_z = downsize_ratio_z_axis_ * distance_threshold;
  if (map == nullptr) {
    return false;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x, point.y, point.z), point, distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x, point.y - distance_threshold, point.z - distance_threshold_z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x, point.y - distance_threshold, point.z), point, distance_threshold,
        map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x, point.y - distance_threshold, point.z + distance_threshold_z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x, point.y, point.z - distance_threshold_z), point, distance_threshold,
        map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x, point.y, point.z + distance_threshold_z), point, distance_threshold,
        map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x, point.y + distance_threshold, point.z - distance_threshold_z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x, point.y + distance_threshold, point.z), point, distance_threshold,
        map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x, point.y + distance_threshold, point.z + distance_threshold_z), point,
        distance_threshold, map, voxel)) {
    return true;
  }

  if (is_in_voxel(
        pcl::PointXYZ(
          point.x - distance_threshold, point.y - distance_threshold,
          point.z - distance_threshold_z),
        point, distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x - distance_threshold, point.y - distance_threshold, point.z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(
          point.x - distance_threshold, point.y - distance_threshold,
          point.z + distance_threshold_z),
        point, distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x - distance_threshold, point.y, point.z - distance_threshold_z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x - distance_threshold, point.y, point.z), point, distance_threshold,
        map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x - distance_threshold, point.y, point.z + distance_threshold_z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(
          point.x - distance_threshold, point.y + distance_threshold,
          point.z - distance_threshold_z),
        point, distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x - distance_threshold, point.y + distance_threshold, point.z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(
          point.x - distance_threshold, point.y + distance_threshold,
          point.z + distance_threshold_z),
        point, distance_threshold, map, voxel)) {
    return true;
  }

  if (is_in_voxel(
        pcl::PointXYZ(
          point.x + distance_threshold, point.y - distance_threshold,
          point.z - distance_threshold_z),
        point, distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x + distance_threshold, point.y - distance_threshold, point.z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(
          point.x + distance_threshold, point.y - distance_threshold,
          point.z + distance_threshold_z),
        point, distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x + distance_threshold, point.y, point.z - distance_threshold_z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x + distance_threshold, point.y, point.z), point, distance_threshold,
        map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x + distance_threshold, point.y, point.z + distance_threshold_z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(
          point.x + distance_threshold, point.y + distance_threshold,
          point.z - distance_threshold_z),
        point, distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(point.x + distance_threshold, point.y + distance_threshold, point.z), point,
        distance_threshold, map, voxel)) {
    return true;
  }
  if (is_in_voxel(
        pcl::PointXYZ(
          point.x + distance_threshold, point.y + distance_threshold,
          point.z + distance_threshold_z),
        point, distance_threshold, map, voxel)) {
    return true;
  }
  return false;
}

bool VoxelGridMapLoader::is_in_voxel(
  const pcl::PointXYZ & src_point, const pcl::PointXYZ & target_point,
  const double distance_threshold, const FilteredPointCloudPtr & map,
  VoxelGridPointXYZ & voxel) const
{
  int voxel_index =
    voxel.getCentroidIndexAt(voxel.getGridCoordinates(src_point.x, src_point.y, src_point.z));
  if (voxel_index != -1) {  // not empty voxel
    const double dist_x = map->points.at(voxel_index).x - target_point.x;
    const double dist_y = map->points.at(voxel_index).y - target_point.y;
    const double dist_z = map->points.at(voxel_index).z - target_point.z;
    // check if the point is inside the distance threshold voxel
    if (
      std::abs(dist_x) < distance_threshold && std::abs(dist_y) < distance_threshold &&
      std::abs(dist_z) < distance_threshold * downsize_ratio_z_axis_) {
      return true;
    }
  }
  return false;
}

VoxelGridStaticMapLoader::VoxelGridStaticMapLoader(
  rclcpp::Node * node, double leaf_size, double downsize_ratio_z_axis,
  std::string * tf_map_input_frame)
: VoxelGridMapLoader(node, leaf_size, downsize_ratio_z_axis, tf_map_input_frame)
{
  voxel_leaf_size_z_ = voxel_leaf_size_ * downsize_ratio_z_axis_;
  sub_map_ = node->create_subscription<sensor_msgs::msg::PointCloud2>(
    "map", rclcpp::QoS{1}.transient_local(),
    std::bind(&VoxelGridStaticMapLoader::onMapCallback, this, std::placeholders::_1));
  RCLCPP_INFO(logger_, "VoxelGridStaticMapLoader initialized.\n");
}

void VoxelGridStaticMapLoader::onMapCallback(
  const sensor_msgs::msg::PointCloud2::ConstSharedPtr map)
{
  pcl::PointCloud<pcl::PointXYZ> map_pcl;
  pcl::fromROSMsg<pcl::PointXYZ>(*map, map_pcl);
  const auto map_pcl_ptr = pcl::make_shared<pcl::PointCloud<pcl::PointXYZ>>(map_pcl);
  *tf_map_input_frame_ = map_pcl_ptr->header.frame_id;
  voxel_map_ptr_.reset(new pcl::PointCloud<pcl::PointXYZ>);
  voxel_grid_.setLeafSize(voxel_leaf_size_, voxel_leaf_size_, voxel_leaf_size_z_);

  // check if the pointcloud is filterable with PCL voxel grid
  isFeasibleWithPCLVoxelGrid(map_pcl_ptr, voxel_grid_);

  voxel_grid_.setInputCloud(map_pcl_ptr);
  voxel_grid_.setSaveLeafLayout(true);
  voxel_grid_.filter(*voxel_map_ptr_);
  is_initialized_.store(true, std::memory_order_release);

  if (debug_) {
    publish_downsampled_map(*voxel_map_ptr_);
  }
}
bool VoxelGridStaticMapLoader::is_close_to_map(
  const pcl::PointXYZ & point, const double distance_threshold)
{
  if (!is_initialized_.load(std::memory_order_acquire)) {
    return false;
  }
  if (is_close_to_neighbor_voxels(point, distance_threshold, voxel_map_ptr_, voxel_grid_)) {
    return true;
  }
  return false;
}

VoxelGridDynamicMapLoader::VoxelGridDynamicMapLoader(
  rclcpp::Node * node, double leaf_size, double downsize_ratio_z_axis,
  std::string * tf_map_input_frame, rclcpp::CallbackGroup::SharedPtr main_callback_group)
: VoxelGridMapLoader(node, leaf_size, downsize_ratio_z_axis, tf_map_input_frame)
{
  voxel_leaf_size_z_ = voxel_leaf_size_ * downsize_ratio_z_axis_;
  auto timer_interval_ms = node->declare_parameter<int>("timer_interval_ms");
  map_update_distance_threshold_ = node->declare_parameter<double>("map_update_distance_threshold");
  map_loader_radius_ = node->declare_parameter<double>("map_loader_radius");
  max_map_grid_size_ = node->declare_parameter<double>("max_map_grid_size");
  auto main_sub_opt = rclcpp::SubscriptionOptions();
  main_sub_opt.callback_group = main_callback_group;
  sub_kinematic_state_ = node->create_subscription<nav_msgs::msg::Odometry>(
    "kinematic_state", rclcpp::QoS{1},
    std::bind(&VoxelGridDynamicMapLoader::onEstimatedPoseCallback, this, std::placeholders::_1),
    main_sub_opt);
  RCLCPP_INFO(logger_, "VoxelGridDynamicMapLoader initialized.\n");

  client_callback_group_ =
    node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  map_update_client_ = node->create_client<autoware_map_msgs::srv::GetDifferentialPointCloudMap>(
    "map_loader_service", rmw_qos_profile_services_default, client_callback_group_);

  while (!map_update_client_->wait_for_service(std::chrono::seconds(1)) && rclcpp::ok()) {
    RCLCPP_INFO(logger_, "service not available, waiting again ...");
  }

  const auto period_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::milliseconds(timer_interval_ms));
  timer_callback_group_ = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  map_update_timer_ = rclcpp::create_timer(
    node, node->get_clock(), period_ns, std::bind(&VoxelGridDynamicMapLoader::timer_callback, this),
    timer_callback_group_);
}
void VoxelGridDynamicMapLoader::onEstimatedPoseCallback(nav_msgs::msg::Odometry::ConstSharedPtr msg)
{
  std::lock_guard<std::mutex> lock(dynamic_map_loader_mutex_);
  current_position_ = msg->pose.pose.position;
}
bool VoxelGridDynamicMapLoader::is_close_to_next_map_grid(
  const pcl::PointXYZ & point, const int current_map_grid_index, const double distance_threshold,
  const double origin_x, const double origin_y, const double map_grid_size_x,
  const double map_grid_size_y, const int map_grids_x)
{
  int neighbor_map_grid_index = static_cast<int>(
    std::floor((point.x - origin_x) / map_grid_size_x) +
    map_grids_x * std::floor((point.y - origin_y) / map_grid_size_y));
  std::lock_guard<std::mutex> lock(dynamic_map_loader_mutex_);
  if (
    static_cast<size_t>(neighbor_map_grid_index) >= current_voxel_grid_array_.size() ||
    neighbor_map_grid_index == current_map_grid_index ||
    current_voxel_grid_array_.at(neighbor_map_grid_index) != nullptr) {
    return false;
  }
  if (is_close_to_neighbor_voxels(
        point, distance_threshold,
        current_voxel_grid_array_.at(neighbor_map_grid_index)->map_cell_pc_ptr,
        current_voxel_grid_array_.at(neighbor_map_grid_index)->map_cell_voxel_grid)) {
    return true;
  }
  return false;
}

bool VoxelGridDynamicMapLoader::is_close_to_map(
  const pcl::PointXYZ & point, const double distance_threshold)
{
  double origin_x, origin_y, map_grid_size_x, map_grid_size_y;
  int map_grids_x, map_grid_index;
  {
    std::lock_guard<std::mutex> lock(dynamic_map_loader_mutex_);
    if (current_voxel_grid_dict_.size() == 0) {
      return false;
    }
    origin_x = origin_x_;
    origin_y = origin_y_;
    map_grid_size_x = map_grid_size_x_;
    map_grid_size_y = map_grid_size_y_;
    map_grids_x = map_grids_x_;
    // Compare point with map grid that point belong to

    map_grid_index = static_cast<int>(
      std::floor((point.x - origin_x) / map_grid_size_x) +
      map_grids_x * std::floor((point.y - origin_y) / map_grid_size_y));

    if (static_cast<size_t>(map_grid_index) >= current_voxel_grid_array_.size()) {
      return false;
    }
    if (
      current_voxel_grid_array_.at(map_grid_index) != nullptr &&
      is_close_to_neighbor_voxels(
        point, distance_threshold, current_voxel_grid_array_.at(map_grid_index)->map_cell_pc_ptr,
        current_voxel_grid_array_.at(map_grid_index)->map_cell_voxel_grid)) {
      return true;
    }
  }

  // Compare point with the neighbor map cells if point close to map cell boundary

  if (is_close_to_next_map_grid(
        pcl::PointXYZ(point.x - distance_threshold, point.y, point.z), map_grid_index,
        distance_threshold, origin_x, origin_y, map_grid_size_x, map_grid_size_y, map_grids_x)) {
    return true;
  }

  if (is_close_to_next_map_grid(
        pcl::PointXYZ(point.x + distance_threshold, point.y, point.z), map_grid_index,
        distance_threshold, origin_x, origin_y, map_grid_size_x, map_grid_size_y, map_grids_x)) {
    return true;
  }

  if (is_close_to_next_map_grid(
        pcl::PointXYZ(point.x, point.y - distance_threshold, point.z), map_grid_index,
        distance_threshold, origin_x, origin_y, map_grid_size_x, map_grid_size_y, map_grids_x)) {
    return true;
  }
  if (is_close_to_next_map_grid(
        pcl::PointXYZ(point.x, point.y + distance_threshold, point.z), map_grid_index,
        distance_threshold, origin_x, origin_y, map_grid_size_x, map_grid_size_y, map_grids_x)) {
    return true;
  }

  return false;
}
void VoxelGridDynamicMapLoader::timer_callback()
{
  std::optional<geometry_msgs::msg::Point> current_position;
  {
    std::lock_guard<std::mutex> lock(dynamic_map_loader_mutex_);
    current_position = current_position_;
  }

  if (current_position == std::nullopt) {
    return;
  }
  if (
    last_updated_position_ == std::nullopt ||
    should_update_map(
      current_position.value(), last_updated_position_.value(), map_update_distance_threshold_)) {
    request_update_map(current_position.value());
    last_updated_position_ = current_position;
  }
}

bool VoxelGridDynamicMapLoader::should_update_map(
  const geometry_msgs::msg::Point & current_point, const geometry_msgs::msg::Point & last_point,
  const double map_update_distance_threshold)
{
  if (distance2D(current_point, last_point) > map_update_distance_threshold) {
    return true;
  }
  return false;
}

void VoxelGridDynamicMapLoader::request_update_map(const geometry_msgs::msg::Point & position)
{
  auto request = std::make_shared<autoware_map_msgs::srv::GetDifferentialPointCloudMap::Request>();
  request->area.center_x = position.x;
  request->area.center_y = position.y;
  request->area.radius = map_loader_radius_;
  request->cached_ids = getCurrentMapIDs();

  auto result{map_update_client_->async_send_request(
    request,
    [](rclcpp::Client<autoware_map_msgs::srv::GetDifferentialPointCloudMap>::SharedFuture) {})};

  std::future_status status = result.wait_for(std::chrono::seconds(0));
  while (status != std::future_status::ready) {
    RCLCPP_INFO(logger_, "Waiting for response...\n");
    if (!rclcpp::ok()) {
      return;
    }
    status = result.wait_for(std::chrono::seconds(1));
  }
  //
  if (status == std::future_status::ready) {
    if (
      result.get()->new_pointcloud_with_ids.size() == 0 &&
      result.get()->ids_to_remove.size() == 0) {
      return;
    }
    updateDifferentialMapCells(result.get()->new_pointcloud_with_ids, result.get()->ids_to_remove);
    if (debug_) {
      publish_downsampled_map(getCurrentDownsampledMapPc());
    }
  }
}

}  // namespace autoware::compare_map_segmentation
