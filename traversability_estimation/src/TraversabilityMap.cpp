/*
 * TraversabilityMap.cpp
 *
 *  Created on: Jun 09, 2014
 *      Author: Martin Wermelinger
 *   Institute: ETH Zurich, Autonomous Systems Lab
 */

#include "traversability_estimation/TraversabilityMap.hpp"
#include "traversability_estimation/common.h"

// System
#include <algorithm>

// Grid Map
#include <grid_map_msgs/GetGridMap.h>
#include <grid_map_core/GridMap.hpp>
#include <grid_map_core/Polygon.hpp>

// ROS
#include <geometry_msgs/PolygonStamped.h>
#include <geometry_msgs/Pose.h>
#include <ros/package.h>
#include <xmlrpcpp/XmlRpcValue.h>

// kindr
#include <kindr/Core>

// Eigen
#include <Eigen/Core>
#include <Eigen/Geometry>

// Param IO
#include <param_io/get_param.hpp>
#include <traversability_estimation/TraversabilityMap.hpp>

using namespace std;

namespace traversability_estimation {

TraversabilityMap::TraversabilityMap(ros::NodeHandle& nodeHandle)
    : nodeHandle_(nodeHandle),
      traversabilityType_("traversability"),
      slopeType_("traversability_slope"),
      stepType_("traversability_step"),
      roughnessType_("traversability_roughness"),
      robotSlopeType_("robot_slope"),
      filter_chain_("grid_map::GridMap"),
      zPosition_(0),
      elevationMapInitialized_(false),
      traversabilityMapInitialized_(false),
      checkForRoughness_(false),
      checkRobotInclination_(false) {
  ROS_INFO("Traversability Map started.");

  readParameters();
  traversabilityMapPublisher_ = nodeHandle_.advertise<grid_map_msgs::GridMap>("traversability_map", 1, true);
  terrainMapPublisher_ = nodeHandle_.advertise<grid_map_msgs::GridMap>("terrain_map", 1, true);
  footprintPublisher_ = nodeHandle_.advertise<geometry_msgs::PolygonStamped>("footprint_polygon", 1, true);
  untraversablePolygonPublisher_ = nodeHandle_.advertise<geometry_msgs::PolygonStamped>("untraversable_polygon", 1, true);
}

TraversabilityMap::~TraversabilityMap() { nodeHandle_.shutdown(); }

bool TraversabilityMap::createLayers(bool useRawMap) {
  boost::recursive_mutex::scoped_lock scopedLockForElevationMap(elevationMapMutex_);
  elevationMapLayers_.push_back("elevation");
  if (!useRawMap) {
    elevationMapLayers_.push_back("upper_bound");
    elevationMapLayers_.push_back("lower_bound");
  } else {
    elevationMapLayers_.push_back("variance");
    elevationMapLayers_.push_back("horizontal_variance_x");
    elevationMapLayers_.push_back("horizontal_variance_y");
    elevationMapLayers_.push_back("horizontal_variance_xy");
    elevationMapLayers_.push_back("time");
  }
  scopedLockForElevationMap.unlock();
  // TODO: Adapt map layers to traversability filters.
  // TODO: Add a layer(terrain_type_)
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  traversabilityMapLayers_.push_back(traversabilityType_);
  traversabilityMapLayers_.push_back(slopeType_);
  traversabilityMapLayers_.push_back(stepType_);
  traversabilityMapLayers_.push_back(roughnessType_);
  scopedLockForTraversabilityMap.unlock();
  return true;
}

bool TraversabilityMap::readParameters() {
  // Read footprint polygon.
  XmlRpc::XmlRpcValue footprint;
  if (nodeHandle_.getParam("footprint/footprint_polygon", footprint)) {
    if (footprint.size() < 3) {
      ROS_WARN("Footprint polygon must consist of at least 3 points. Only %i points found.", footprint.size());
      footprintPoints_.clear();
    } else {
      geometry_msgs::Point32 pt;
      pt.z = 0.0;
      for (int i = 0; i < footprint.size(); i++) {
        pt.x = (double)footprint[i][0];
        pt.y = (double)footprint[i][1];
        footprintPoints_.push_back(pt);
      }
    }
  } else {
    ROS_WARN("Traversability Map: No footprint polygon defined.");
  }

  mapFrameId_ = param_io::param<std::string>(nodeHandle_, "map_frame_id", "map");
  traversabilityDefaultReadAtInit_ = param_io::param(nodeHandle_, "footprint/traversability_default", 0.5);
  // Safety check
  traversabilityDefaultReadAtInit_ = boundTraversabilityValue(traversabilityDefaultReadAtInit_);
  setDefaultTraversabilityUnknownRegions(traversabilityDefaultReadAtInit_);
  checkForRoughness_ = param_io::param(nodeHandle_, "footprint/verify_roughness_footprint", false);
  checkRobotInclination_ = param_io::param(nodeHandle_, "footprint/check_robot_inclination", false);
  maxGapWidth_ = param_io::param(nodeHandle_, "max_gap_width", 0.3);

  XmlRpc::XmlRpcValue filterParameter;
  bool filterParamsAvailable = param_io::getParam(nodeHandle_, "traversability_map_filters", filterParameter);
  if (filterParamsAvailable) {
    ROS_ASSERT(filterParameter.getType() == XmlRpc::XmlRpcValue::TypeArray);
    for (int index = 0; index < filterParameter.size(); index++) {
      if (filterParameter[index]["name"] == "stepFilter") {
        criticalStepHeight_ = (double)filterParameter[index]["params"]["critical_value"];
      }
    }
  }

  // Configure filter chain
  if (!filter_chain_.configure("traversability_map_filters", nodeHandle_)) {
    ROS_ERROR("Could not configure the filter chain!");
  }
  return true;
}

bool TraversabilityMap::setElevationMap(const grid_map_msgs::GridMap& msg) {
  if (getMapFrameId() != msg.info.header.frame_id) {
    ROS_ERROR("Received elevation map has frame_id = '%s', but an elevation map with frame_id = '%s' is expected.",
              msg.info.header.frame_id.c_str(), getMapFrameId().c_str());
    return false;
  }
  grid_map::GridMap elevationMap;
  grid_map::GridMapRosConverter::fromMessage(msg, elevationMap);
  boost::recursive_mutex::scoped_lock scopedLockForElevationMap(elevationMapMutex_);
  zPosition_ = msg.info.pose.position.z;
  for (auto& layer : elevationMapLayers_) {
    if (!elevationMap.exists(layer)) {
      ROS_WARN("Traversability Map: Can't set elevation map because there is no layer %s.", layer.c_str());
      return false;
    }
  }
  elevationMap_ = elevationMap;
  elevationMapInitialized_ = true;
  return true;
}

bool TraversabilityMap::setTraversabilityMap(const grid_map_msgs::GridMap& msg) {
  grid_map::GridMap traversabilityMap;
  grid_map::GridMapRosConverter::fromMessage(msg, traversabilityMap);
  zPosition_ = msg.info.pose.position.z;
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  for (auto& layer : traversabilityMapLayers_) {
    if (!traversabilityMap.exists(layer)) {
      ROS_WARN("Traversability Map: Can't set traversability map because there exists no layer %s.", layer.c_str());
      return false;
    }
  }
  traversabilityMap_ = traversabilityMap;
  traversabilityMapInitialized_ = true;
  return true;
}

void TraversabilityMap::publishTraversabilityMap() {
  if (!traversabilityMapPublisher_.getNumSubscribers() < 1) {
    grid_map_msgs::GridMap mapMessage;
    boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
    grid_map::GridMap traversabilityMapCopy = traversabilityMap_;
    scopedLockForTraversabilityMap.unlock();
    if (traversabilityMapCopy.exists("upper_bound") && traversabilityMapCopy.exists("lower_bound")) {
      traversabilityMapCopy.add("uncertainty_range", traversabilityMapCopy.get("upper_bound") - traversabilityMapCopy.get("lower_bound"));
    }

    grid_map::GridMapRosConverter::toMessage(traversabilityMapCopy, mapMessage);
    mapMessage.info.pose.position.z = zPosition_;
    traversabilityMapPublisher_.publish(mapMessage);
    ROS_DEBUG_STREAM("[TraversabilityMap::publishTraversabilityMap] Publishing the traversability map !!!");
  }
}

void TraversabilityMap::publishTerrainMap() {
  if (!terrainMapPublisher_.getNumSubscribers() < 1) {
    grid_map_msgs::GridMap mapMessage;
    boost::recursive_mutex::scoped_lock scopedLockForTerrainMap(terrainMapMutex_);
    grid_map::GridMap terrainMapCopy = terrainMap_;
    scopedLockForTerrainMap.unlock();
    if (terrainMapCopy.exists("upper_bound") && terrainMapCopy.exists("lower_bound")) {
      terrainMapCopy.add("uncertainty_range", terrainMapCopy.get("upper_bound") - terrainMapCopy.get("lower_bound"));
    }

    grid_map::GridMapRosConverter::toMessage(terrainMapCopy, mapMessage);
    mapMessage.info.pose.position.z = zPosition_;
    terrainMapPublisher_.publish(mapMessage);
    ROS_DEBUG_STREAM("[TraversabilityMap::publishTerrainMap] Publishing the terrain map !!!");
  }
}

grid_map::GridMap TraversabilityMap::getTraversabilityMap() {
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  return traversabilityMap_;
}

grid_map::GridMap TraversabilityMap::downsamplingMap(const grid_map::GridMap& traversabilityMap){
  
  grid_map::Position SubmapPosition(robotPos_relative_to_odom_.point.x, robotPos_relative_to_odom_.point.y);
  grid_map::Length SubmapLength(2.5, 1.5);
  grid_map::GridMap subMap;
  bool isSuccess;
  subMap = traversabilityMap.getSubmap(SubmapPosition, SubmapLength, isSuccess);
  ROS_INFO("[Downsampling map]Submap Created with size %f X %f m (%i X %i cells).",
    subMap.getLength().x(), subMap.getLength().y(), subMap.getSize()(0), subMap.getSize()(1));

  return subMap;
}

bool TraversabilityMap::traversabilityMapInitialized() { return traversabilityMapInitialized_; }

void TraversabilityMap::resetTraversabilityFootprintLayers() {
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  if (traversabilityMap_.exists("step_footprint")) traversabilityMap_.clear("step_footprint");
  if (traversabilityMap_.exists("slope_footprint")) traversabilityMap_.clear("slope_footprint");
  if (traversabilityMap_.exists("traversability_footprint")) traversabilityMap_.clear("traversability_footprint");
}

bool TraversabilityMap::computeTraversability() {
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  grid_map::GridMap traversabilityMapCopy = traversabilityMap_;
  scopedLockForTraversabilityMap.unlock();
  boost::recursive_mutex::scoped_lock scopedLockForElevationMap(elevationMapMutex_);
  grid_map::GridMap elevationMapCopy = elevationMap_;
  scopedLockForElevationMap.unlock();
  // Terrain Map
  boost::recursive_mutex::scoped_lock scopedLockForTerrainMap(terrainMapMutex_);
  grid_map::GridMap terrainMapCopy = terrainMap_;
  scopedLockForTerrainMap.unlock();

  // Initialize timer.
  ros::WallTime start = ros::WallTime::now();

  if (elevationMapInitialized_) {
    if (!filter_chain_.update(elevationMapCopy, traversabilityMapCopy)) {
      ROS_ERROR("Traversability Estimation: Could not update the filter chain! No traversability computed!");
      traversabilityMapInitialized_ = false;
      return false;
    }
  } else {
    ROS_ERROR("Traversability Estimation: Elevation map is not initialized!");
    traversabilityMapInitialized_ = false;
    return false;
  }
  traversabilityMapInitialized_ = true;
  traversabilityMapCopy.add("step_footprint");
  traversabilityMapCopy.add("slope_footprint");
  if (checkForRoughness_) traversabilityMapCopy.add("roughness_footprint");
  traversabilityMapCopy.add("traversability_footprint");

  scopedLockForTraversabilityMap.lock();
  traversabilityMap_ = traversabilityMapCopy;
  scopedLockForTraversabilityMap.unlock();

  // Terrain Map Downsampling
  terrainMapCopy = downsamplingMap(traversabilityMapCopy);
  scopedLockForTerrainMap.lock();
  // Assign Cost according to terrain types
  //terrainMap_ = terrainMapCopy;
  terrainMap_ = assignTerrainCost(terrainMapCopy);
  scopedLockForTerrainMap.unlock();

  publishTraversabilityMap();
  publishTerrainMap();

  ROS_DEBUG("Traversability map has been updated in %f s.", (ros::WallTime::now() - start).toSec());
  return true;
}

bool TraversabilityMap::traversabilityFootprint(double footprintYaw) {
  if (!traversabilityMapInitialized_) return false;

  // Initialize timer.
  ros::WallTime start = ros::WallTime::now();

  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  traversabilityMap_.add("traversability_x");
  traversabilityMap_.add("traversability_rot");

  grid_map::Position position;
  grid_map::Polygon polygonX, polygonRot;

  ROS_DEBUG_STREAM("footprint yaw: " << footprintYaw);
  // Compute Orientation
  kindr::RotationQuaternionD xquat, rquat;
  kindr::AngleAxisD rotationAxis(footprintYaw, 0.0, 0.0, 1.0);
  rquat = rotationAxis * xquat;
  Eigen::Quaterniond orientationX = xquat.toImplementation();
  Eigen::Quaterniond orientationRot = rquat.toImplementation();

  for (grid_map::GridMapIterator iterator(traversabilityMap_); !iterator.isPastEnd(); ++iterator) {
    polygonX.removeVertices();
    polygonRot.removeVertices();
    traversabilityMap_.getPosition(*iterator, position);

    grid_map::Position3 positionToVertex, positionToVertexTransformedX, positionToVertexTransformedRot;
    Eigen::Translation<double, 3> toPosition;
    Eigen::Quaterniond orientation;

    toPosition.x() = position.x();
    toPosition.y() = position.y();
    toPosition.z() = 0.0;

    for (const auto& point : footprintPoints_) {
      positionToVertex.x() = point.x;
      positionToVertex.y() = point.y;
      positionToVertex.z() = point.z;
      positionToVertexTransformedX = toPosition * orientationX * positionToVertex;
      positionToVertexTransformedRot = toPosition * orientationRot * positionToVertex;

      grid_map::Position vertexX, vertexRot;
      vertexX.x() = positionToVertexTransformedX.x();
      vertexRot.x() = positionToVertexTransformedRot.x();
      vertexX.y() = positionToVertexTransformedX.y();
      vertexRot.y() = positionToVertexTransformedRot.y();
      polygonX.addVertex(vertexX);
      polygonRot.addVertex(vertexRot);
    }

    double traversability;
    if (isTraversable(polygonX, traversability))
      traversabilityMap_.at("traversability_x", *iterator) = traversability;
    else
      traversabilityMap_.at("traversability_x", *iterator) = 0.0;
    if (isTraversable(polygonRot, traversability))
      traversabilityMap_.at("traversability_rot", *iterator) = traversability;
    else
      traversabilityMap_.at("traversability_rot", *iterator) = 0.0;
  }
  scopedLockForTraversabilityMap.unlock();

  publishTraversabilityMap();

  ROS_INFO("Traversability of footprint has been computed in %f s.", (ros::WallTime::now() - start).toSec());
  return true;
}

bool TraversabilityMap::traversabilityFootprint(const double& radius, const double& offset) {
  double traversability;
  grid_map::Position center;
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  for (grid_map::GridMapIterator iterator(traversabilityMap_); !iterator.isPastEnd(); ++iterator) {
    traversabilityMap_.getPosition(*iterator, center);
    isTraversable(center, radius + offset, traversability, radius);
  }
  scopedLockForTraversabilityMap.unlock();
  publishTraversabilityMap();
  return true;
}

bool TraversabilityMap::checkFootprintPath(const traversability_msgs::FootprintPath& path,
                                           traversability_msgs::TraversabilityResult& result, const bool publishPolygons) {
  bool successfullyCheckedFootprint;
  if (!traversabilityMapInitialized_) {
    ROS_WARN_THROTTLE(periodThrottledConsoleMessages, "Traversability Estimation: check Footprint path: Traversability map not yet initialized.");
    result.is_safe = static_cast<unsigned char>(false);
    return true;
  }

  const auto arraySize = path.poses.poses.size();
  if (arraySize == 0) {
    ROS_WARN("Traversability Estimation: This path has no poses to check!");
    result.is_safe = static_cast<unsigned char>(false);
    return false;
  }

  if (path.footprint.polygon.points.size() == 0) {
    successfullyCheckedFootprint = checkCircularFootprintPath(path, publishPolygons, result);
  } else {
    successfullyCheckedFootprint = checkPolygonalFootprintPath(path, publishPolygons, result);
  }

  return successfullyCheckedFootprint;
}

bool TraversabilityMap::checkCircularFootprintPath(const traversability_msgs::FootprintPath& path, const bool publishPolygons,
                                                   traversability_msgs::TraversabilityResult& result) {
  double radius = path.radius;
  double offset = 0.15;
  grid_map::Position start, end;
  const auto arraySize = path.poses.poses.size();
  const bool computeUntraversablePolygon = path.compute_untraversable_polygon;
  result.is_safe = static_cast<unsigned char>(false);
  result.traversability = 0.0;
  result.area = 0.0;
  double traversability = 0.0;
  double area = 0.0;
  grid_map::Polygon untraversablePolygon;
  auto robotHeight = computeMeanHeightFromPoses(path.poses.poses);

  for (int i = 0; i < arraySize; i++) {
    start = end;
    end.x() = path.poses.poses[i].position.x;
    end.y() = path.poses.poses[i].position.y;

    if (arraySize == 1) {
      if (checkRobotInclination_) {
        if (!checkInclination(end, end)) {
          return true;
        }
      }
      bool pathIsTraversable =
          isTraversable(end, radius + offset, computeUntraversablePolygon, traversability, untraversablePolygon, radius);
      if (publishPolygons) {
        grid_map::Polygon polygon = grid_map::Polygon::fromCircle(end, radius + offset);
        polygon.setFrameId(getMapFrameId());
        polygon.setTimestamp(ros::Time::now().toNSec());
        publishFootprintPolygon(polygon);
        if (computeUntraversablePolygon) {
          publishUntraversablePolygon(untraversablePolygon, robotHeight);
        }
      }
      if (!pathIsTraversable) {
        // return such that default values in result - i.e. non traversable - are used.
        return true;
      }
      result.traversability = traversability;
    }

    if (arraySize > 1 && i > 0) {
      if (checkRobotInclination_) {
        if (!checkInclination(start, end)) {
          return true;
        }
      }
      double traversabilityTemp, traversabilitySum = 0.0;
      int nLine = 0;
      grid_map::Index startIndex, endIndex;
      boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
      traversabilityMap_.getIndex(start, startIndex);
      traversabilityMap_.getIndex(end, endIndex);
      int nSkip = 3;  // TODO: Remove magic number.
      grid_map::Polygon auxiliaryUntraversablePolygon;
      bool pathIsTraversable = true;
      for (grid_map::LineIterator lineIterator(traversabilityMap_, endIndex, startIndex); !lineIterator.isPastEnd(); ++lineIterator) {
        grid_map::Position center;
        traversabilityMap_.getPosition(*lineIterator, center);
        pathIsTraversable = pathIsTraversable && isTraversable(center, radius + offset, computeUntraversablePolygon, traversabilityTemp,
                                                               auxiliaryUntraversablePolygon, radius);

        if (publishPolygons && computeUntraversablePolygon && auxiliaryUntraversablePolygon.nVertices() > 0) {
          untraversablePolygon = grid_map::Polygon::convexHull(untraversablePolygon, auxiliaryUntraversablePolygon);
        }

        if (!pathIsTraversable && !computeUntraversablePolygon && !publishPolygons) {
          // return such that default values in result - i.e. non traversable - are used.
          return true;
        }

        traversabilitySum += traversabilityTemp;
        nLine++;
        for (int j = 0; j < nSkip; j++) {
          if (!lineIterator.isPastEnd()) {
            ++lineIterator;
          }
        }
      }
      scopedLockForTraversabilityMap.unlock();

      if (publishPolygons) {
        grid_map::Polygon polygon = grid_map::Polygon::fromCircle(end, radius + offset);
        polygon.setFrameId(getMapFrameId());
        polygon.setTimestamp(ros::Time::now().toNSec());
        publishFootprintPolygon(polygon);
        if (computeUntraversablePolygon) {
          untraversablePolygon.setFrameId(auxiliaryUntraversablePolygon.getFrameId());
          untraversablePolygon.setTimestamp(auxiliaryUntraversablePolygon.getTimestamp());
          publishUntraversablePolygon(untraversablePolygon, robotHeight);
        }
      }

      if (pathIsTraversable) {
        traversability = traversabilitySum / (double)nLine;
        double lengthSegment, lengthPreviousPath, lengthPath;
        lengthSegment = (end - start).norm();
        if (i > 1) {
          lengthPreviousPath = lengthPath;
          lengthPath += lengthSegment;
          result.traversability = (lengthSegment * traversability + lengthPreviousPath * result.traversability) / lengthPath;
        } else {
          lengthPath = lengthSegment;
          result.traversability = traversability;
        }
      } else {
        // return such that default values in result - i.e. non traversable - are used.
        return true;
      }
    }
  }

  result.is_safe = static_cast<unsigned char>(true);
  return true;
}

bool TraversabilityMap::checkPolygonalFootprintPath(const traversability_msgs::FootprintPath& path, const bool publishPolygons,
                                                    traversability_msgs::TraversabilityResult& result) {
  grid_map::Position start, end;
  const auto arraySize = path.poses.poses.size();
  const bool computeUntraversablePolygon = path.compute_untraversable_polygon;
  result.is_safe = static_cast<unsigned char>(false);
  result.traversability = 0.0;
  result.area = 0.0;
  double traversability = 0.0;
  grid_map::Polygon untraversablePolygon;
  auto robotHeight = computeMeanHeightFromPoses(path.poses.poses);

  grid_map::Polygon polygon, polygon1, polygon2;
  polygon1.setFrameId(getMapFrameId());
  polygon1.setTimestamp(ros::Time::now().toNSec());
  polygon2 = polygon1;
  for (int i = 0; i < arraySize; i++) {
    polygon1 = polygon2;
    start = end;
    polygon2.removeVertices();
    grid_map::Position3 positionToVertex, positionToVertexTransformed;
    Eigen::Translation<double, 3> toPosition;
    Eigen::Quaterniond orientation;

    toPosition.x() = path.poses.poses[i].position.x;
    toPosition.y() = path.poses.poses[i].position.y;
    toPosition.z() = path.poses.poses[i].position.z;
    orientation.x() = path.poses.poses[i].orientation.x;
    orientation.y() = path.poses.poses[i].orientation.y;
    orientation.z() = path.poses.poses[i].orientation.z;
    orientation.w() = path.poses.poses[i].orientation.w;
    end.x() = toPosition.x();
    end.y() = toPosition.y();

    for (const auto& point : path.footprint.polygon.points) {
      positionToVertex.x() = point.x;
      positionToVertex.y() = point.y;
      positionToVertex.z() = point.z;
      positionToVertexTransformed = toPosition * orientation * positionToVertex;

      grid_map::Position vertex;
      vertex.x() = positionToVertexTransformed.x();
      vertex.y() = positionToVertexTransformed.y();
      polygon2.addVertex(vertex);
    }

    if (path.conservative && i > 0) {
      grid_map::Vector startToEnd = end - start;
      vector<grid_map::Position> vertices1 = polygon1.getVertices();
      vector<grid_map::Position> vertices2 = polygon2.getVertices();
      for (const auto& vertex : vertices1) {
        polygon2.addVertex(vertex + startToEnd);
      }
      for (const auto& vertex : vertices2) {
        polygon1.addVertex(vertex - startToEnd);
      }
    }

    if (arraySize == 1) {
      polygon = polygon2;
      if (checkRobotInclination_) {
        if (!checkInclination(end, end)) return true;
      }
      bool pathIsTraversable = isTraversable(polygon, computeUntraversablePolygon, traversability, untraversablePolygon);

      if (publishPolygons) {
        publishFootprintPolygon(polygon);
        if (computeUntraversablePolygon) {
          publishUntraversablePolygon(untraversablePolygon, robotHeight);
        }
      }

      if (!pathIsTraversable) {
        // return such that default values in result - i.e. non traversable - are used.
        return true;
      }

      result.traversability = traversability;
      result.area = polygon.getArea();
    }

    if (arraySize > 1 && i > 0) {
      polygon = grid_map::Polygon::convexHull(polygon1, polygon2);
      polygon.setFrameId(getMapFrameId());
      polygon.setTimestamp(ros::Time::now().toNSec());

      if (checkRobotInclination_) {
        if (!checkInclination(start, end)) {
          return true;
        }
      }
      bool pathIsTraversable = isTraversable(polygon, computeUntraversablePolygon, traversability, untraversablePolygon);

      if (publishPolygons) {
        publishFootprintPolygon(polygon, robotHeight);
        if (computeUntraversablePolygon) {
          publishUntraversablePolygon(untraversablePolygon, robotHeight);
        }
      }

      if (!pathIsTraversable) {
        // return such that default values in result - i.e. non traversable - are used.
        return true;
      }

      double areaPolygon, areaPrevious;
      if (i > 1) {
        areaPrevious = result.area;
        areaPolygon = polygon.getArea() - polygon1.getArea();
        result.area += areaPolygon;
        result.traversability = (areaPolygon * traversability + areaPrevious * result.traversability) / result.area;
      } else {
        result.area = polygon.getArea();
        result.traversability = traversability;
      }
    }
  }

  result.is_safe = static_cast<unsigned char>(true);
  return true;
}

bool TraversabilityMap::isTraversable(const grid_map::Polygon& polygon, double& traversability) {
  const bool computeUntraversablePolygon = false;
  grid_map::Polygon untraversablePolygon;
  return isTraversable(polygon, computeUntraversablePolygon, traversability, untraversablePolygon);
}

bool TraversabilityMap::isTraversable(const grid_map::Polygon& polygon, const bool& computeUntraversablePolygon, double& traversability,
                                      grid_map::Polygon& untraversablePolygon) {
  unsigned int nCells = 0;
  traversability = 0.0;
  bool pathIsTraversable = true;
  std::vector<grid_map::Position> untraversablePositions;
  // Iterate through polygon and check for traversability.
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  for (grid_map::PolygonIterator polygonIterator(traversabilityMap_, polygon); !polygonIterator.isPastEnd(); ++polygonIterator) {
    bool currentPositionIsTraversale = isTraversableForFilters(*polygonIterator);

    if (!currentPositionIsTraversale) {
      pathIsTraversable = false;
      if (computeUntraversablePolygon) {
        grid_map::Position positionUntraversableCell;
        traversabilityMap_.getPosition(*polygonIterator, positionUntraversableCell);
        untraversablePositions.push_back(positionUntraversableCell);
      } else {
        return false;
      }
    } else {
      nCells++;
      if (!traversabilityMap_.isValid(*polygonIterator, traversabilityType_)) {
        traversability += traversabilityDefault_;
      } else {
        traversability += traversabilityMap_.at(traversabilityType_, *polygonIterator);
      }
    }
  }
  scopedLockForTraversabilityMap.unlock();

  if (pathIsTraversable) {
    // Handle cases of footprints outside of map.
    if (nCells == 0) {
      ROS_DEBUG("TraversabilityMap: isTraversable: No cells within polygon.");
      traversability = traversabilityDefault_;
      pathIsTraversable = traversabilityDefault_ != 0.0;
    } else {
      traversability /= nCells;
    }
  }

  if (computeUntraversablePolygon) {
    if (pathIsTraversable) {
      untraversablePolygon = grid_map::Polygon();  // empty untraversable polygon
    } else {
      untraversablePolygon = grid_map::Polygon::monotoneChainConvexHullOfPoints(untraversablePositions);
    }
    untraversablePolygon.setFrameId(getMapFrameId());
    untraversablePolygon.setTimestamp(ros::Time::now().toNSec());
  }

  return pathIsTraversable;
}

bool TraversabilityMap::isTraversable(const grid_map::Position& center, const double& radiusMax, double& traversability,
                                      const double& radiusMin) {
  const bool computeUntraversablePolygon = false;
  grid_map::Polygon untraversablePolygon;
  return isTraversable(center, radiusMax, computeUntraversablePolygon, traversability, untraversablePolygon, radiusMin);
}

bool TraversabilityMap::isTraversable(const grid_map::Position& center, const double& radiusMax, const bool& computeUntraversablePolygon,
                                      double& traversability, grid_map::Polygon& untraversablePolygon, const double& radiusMin) {
  bool circleIsTraversable = true;
  std::vector<grid_map::Position> untraversablePositions;
  grid_map::Position positionUntraversableCell;
  untraversablePolygon = grid_map::Polygon();  // empty untraversable polygon
  // Handle cases of footprints outside of map.
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  if (!traversabilityMap_.isInside(center)) {
    traversability = traversabilityDefault_;
    circleIsTraversable = traversabilityDefault_ != 0.0;
    if (computeUntraversablePolygon && !circleIsTraversable) {
      untraversablePolygon = grid_map::Polygon::fromCircle(center, radiusMax);
    }
  } else {
    // Footprints inside map.
    // Get index of center position.
    grid_map::Index indexCenter;
    traversabilityMap_.getIndex(center, indexCenter);
    if (traversabilityMap_.isValid(indexCenter, "traversability_footprint")) {
      traversability = traversabilityMap_.at("traversability_footprint", indexCenter);
      circleIsTraversable = traversability != 0.0;
      if (computeUntraversablePolygon && !circleIsTraversable) {
        untraversablePolygon = grid_map::Polygon::fromCircle(center, radiusMax);
      }
    } else {
      // Non valid (non finite traversability)
      int nCells = 0;
      traversability = 0.0;

      // Iterate through polygon and check for traversability.
      double maxUntraversableRadius = 0.0;
      bool traversableRadiusBiggerMinRadius = false;
      for (grid_map::SpiralIterator iterator(traversabilityMap_, center, radiusMax);
           !iterator.isPastEnd() && !traversableRadiusBiggerMinRadius; ++iterator) {
        const bool currentPositionIsTraversale = isTraversableForFilters(*iterator);
        if (!currentPositionIsTraversale) {
          const auto untraversableRadius = iterator.getCurrentRadius();
          maxUntraversableRadius = std::max(maxUntraversableRadius, untraversableRadius);

          if (radiusMin == 0.0) {
            traversabilityMap_.at("traversability_footprint", indexCenter) = 0.0;
            circleIsTraversable = false;
            traversabilityMap_.getPosition(*iterator, positionUntraversableCell);
            untraversablePositions.push_back(positionUntraversableCell);
          } else {
            if (untraversableRadius <= radiusMin) {
              traversabilityMap_.at("traversability_footprint", indexCenter) = 0.0;
              circleIsTraversable = false;
              traversabilityMap_.getPosition(*iterator, positionUntraversableCell);
              untraversablePositions.push_back(positionUntraversableCell);
            } else if (circleIsTraversable) {  // if circleIsTraversable is not changed by any previous loop
              auto factor = ((untraversableRadius - radiusMin) / (radiusMax - radiusMin) + 1.0) / 2.0;
              traversability *= factor / nCells;
              traversabilityMap_.at("traversability_footprint", indexCenter) = static_cast<float>(traversability);
              circleIsTraversable = true;
              traversableRadiusBiggerMinRadius = true;
            }
          }

          if (!computeUntraversablePolygon) {
            // Do not keep on checking, one cell is already non-traversable.
            return false;
          }
        } else {
          nCells++;
          if (!traversabilityMap_.isValid(*iterator, traversabilityType_)) {
            traversability += traversabilityDefault_;
          } else {
            traversability += traversabilityMap_.at(traversabilityType_, *iterator);
          }
        }
      }

      if (computeUntraversablePolygon && !circleIsTraversable) {
        untraversablePolygon = grid_map::Polygon::monotoneChainConvexHullOfPoints(untraversablePositions);
      }

      if (circleIsTraversable) {
        traversability /= nCells;
        traversabilityMap_.at("traversability_footprint", indexCenter) = static_cast<float>(traversability);
      }
    }
  }
  scopedLockForTraversabilityMap.unlock();

  if (computeUntraversablePolygon) {
    untraversablePolygon.setFrameId(getMapFrameId());
    untraversablePolygon.setTimestamp(ros::Time::now().toNSec());
  }

  return circleIsTraversable;
}

bool TraversabilityMap::checkInclination(const grid_map::Position& start, const grid_map::Position& end) {
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  if (end == start) {
    if (traversabilityMap_.atPosition(robotSlopeType_, start) == 0.0) return false;
  } else {
    grid_map::Index startIndex, endIndex;
    traversabilityMap_.getIndex(start, startIndex);
    traversabilityMap_.getIndex(end, endIndex);
    for (grid_map::LineIterator lineIterator(traversabilityMap_, startIndex, endIndex); !lineIterator.isPastEnd(); ++lineIterator) {
      if (!traversabilityMap_.isValid(*lineIterator, robotSlopeType_)) continue;
      if (traversabilityMap_.at(robotSlopeType_, *lineIterator) == 0.0) return false;
    }
  }
  return true;
}

bool TraversabilityMap::updateFilter() {
  // Reconfigure filter chain.
  filter_chain_.clear();
  if (!filter_chain_.configure("traversability_map_filters", nodeHandle_)) {
    ROS_ERROR("Could not configure the filter chain!");
    return false;
  }
  return true;
}

bool TraversabilityMap::isTraversableForFilters(const grid_map::Index& indexStep) {
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  bool currentPositionIsTraversale = true;
  if (checkForSlope(indexStep)) {
    if (checkForStep(indexStep)) {
      if (checkForRoughness_) {
        if (!checkForRoughness(indexStep)) {
          currentPositionIsTraversale = false;
        }
      }
    } else {
      currentPositionIsTraversale = false;
    }
  } else {
    currentPositionIsTraversale = false;
  }

  return currentPositionIsTraversale;
}

bool TraversabilityMap::checkForStep(const grid_map::Index& indexStep) {
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  if (traversabilityMap_.at(stepType_, indexStep) == 0.0) {
    if (!traversabilityMap_.isValid(indexStep, "step_footprint")) {
      double windowRadiusStep = 2.5 * traversabilityMap_.getResolution();  // 0.075;

      vector<grid_map::Index> indices;
      grid_map::Position center;
      traversabilityMap_.getPosition(indexStep, center);
      double height = traversabilityMap_.at("elevation", indexStep);
      for (grid_map::CircleIterator circleIterator(traversabilityMap_, center, windowRadiusStep); !circleIterator.isPastEnd();
           ++circleIterator) {
        if (traversabilityMap_.at("elevation", *circleIterator) > criticalStepHeight_ + height &&
            traversabilityMap_.at(stepType_, *circleIterator) == 0.0)
          indices.push_back(*circleIterator);
      }
      if (indices.empty()) indices.push_back(indexStep);
      for (auto& index : indices) {
        grid_map::Length subMapLength(2.5 * traversabilityMap_.getResolution(), 2.5 * traversabilityMap_.getResolution());
        grid_map::Position subMapPos;
        bool isSuccess;
        traversabilityMap_.getPosition(index, subMapPos);
        grid_map::Vector toCenter = center - subMapPos;
        grid_map::GridMap subMap = traversabilityMap_.getSubmap(subMapPos, subMapLength, isSuccess);
        if (!isSuccess) {
          ROS_WARN("Traversability map: Check for step window could not retrieve submap.");
          traversabilityMap_.at("step_footprint", indexStep) = 0.0;
          return false;
        }
        height = traversabilityMap_.at("elevation", index);
        for (grid_map::GridMapIterator subMapIterator(subMap); !subMapIterator.isPastEnd(); ++subMapIterator) {
          if (subMap.at(stepType_, *subMapIterator) == 0.0 && subMap.at("elevation", *subMapIterator) < height - criticalStepHeight_) {
            grid_map::Position pos;
            subMap.getPosition(*subMapIterator, pos);
            grid_map::Vector vec = pos - subMapPos;
            if (vec.norm() < 0.025) continue;
            if (toCenter.norm() > 0.025) {
              if (toCenter.dot(vec) < 0.0) continue;
            }
            pos = subMapPos + vec;
            while ((pos - subMapPos + vec).norm() < maxGapWidth_ && traversabilityMap_.isInside(pos + vec)) pos += vec;
            grid_map::Index endIndex;
            traversabilityMap_.getIndex(pos, endIndex);
            bool gapStart = false;
            bool gapEnd = false;
            for (grid_map::LineIterator lineIterator(traversabilityMap_, index, endIndex); !lineIterator.isPastEnd(); ++lineIterator) {
              if (traversabilityMap_.at("elevation", *lineIterator) > height + criticalStepHeight_) {
                traversabilityMap_.at("step_footprint", indexStep) = 0.0;
                return false;
              }
              if (traversabilityMap_.at("elevation", *lineIterator) < height - criticalStepHeight_ ||
                  !traversabilityMap_.isValid(*lineIterator, "elevation")) {
                gapStart = true;
              } else if (gapStart) {
                gapEnd = true;
                break;
              }
            }
            if (gapStart && !gapEnd) {
              traversabilityMap_.at("step_footprint", indexStep) = 0.0;
              return false;
            }
          }
        }
      }
      traversabilityMap_.at("step_footprint", indexStep) = 1.0;
    } else if (traversabilityMap_.at("step_footprint", indexStep) == 0.0) {
      return false;
    }
  }
  return true;
}

bool TraversabilityMap::checkForSlope(const grid_map::Index& index) {
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  if (traversabilityMap_.at(slopeType_, index) == 0.0) {
    if (!traversabilityMap_.isValid(index, "slope_footprint")) {
      double windowRadius = 3.0 * traversabilityMap_.getResolution();  // TODO: read this as a parameter?
      double criticalLength = maxGapWidth_ / 3.0;
      int nSlopesCritical = floor(2 * windowRadius * criticalLength / pow(traversabilityMap_.getResolution(), 2));

      // Requested position (center) of circle in map.
      grid_map::Position center;
      traversabilityMap_.getPosition(index, center);
      int nSlopes = 0;
      for (grid_map::CircleIterator circleIterator(traversabilityMap_, center, windowRadius); !circleIterator.isPastEnd();
           ++circleIterator) {
        if (traversabilityMap_.at(slopeType_, *circleIterator) == 0.0) nSlopes++;
        if (nSlopes > nSlopesCritical) {
          traversabilityMap_.at("slope_footprint", index) = 0.0;
          return false;
        }
      }
      traversabilityMap_.at("slope_footprint", index) = 1.0;
    } else if (traversabilityMap_.at("slope_footprint", index) == 0.0) {
      return false;
    }
  }
  return true;
}

bool TraversabilityMap::checkForRoughness(const grid_map::Index& index) {
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  if (traversabilityMap_.at(roughnessType_, index) == 0.0) {
    if (!traversabilityMap_.isValid(index, "roughness_footprint")) {
      double windowRadius = 3.0 * traversabilityMap_.getResolution();  // TODO: read this as a parameter?
      double criticalLength = maxGapWidth_ / 3.0;
      int nRoughnessCritical = floor(1.5 * windowRadius * criticalLength / pow(traversabilityMap_.getResolution(), 2));

      // Requested position (center) of circle in map.
      grid_map::Position center;
      traversabilityMap_.getPosition(index, center);
      int nRoughness = 0;
      for (grid_map::CircleIterator circleIterator(traversabilityMap_, center, windowRadius); !circleIterator.isPastEnd();
           ++circleIterator) {
        if (traversabilityMap_.at(roughnessType_, *circleIterator) == 0.0) nRoughness++;
        if (nRoughness > nRoughnessCritical) {
          traversabilityMap_.at("roughness_footprint", index) = 0.0;
          return false;
        }
      }
      traversabilityMap_.at("roughness_footprint", index) = 1.0;
    } else if (traversabilityMap_.at("roughness_footprint", index) == 0.0) {
      return false;
    }
  }
  return true;
}

void TraversabilityMap::publishFootprintPolygon(const grid_map::Polygon& polygon, double zPosition) {
  if (footprintPublisher_.getNumSubscribers() < 1) return;
  geometry_msgs::PolygonStamped polygonMsg;
  grid_map::PolygonRosConverter::toMessage(polygon, polygonMsg);
  for (int i = 0; i < polygonMsg.polygon.points.size(); i++) {
    polygonMsg.polygon.points.at(i).z = zPosition;
  }
  footprintPublisher_.publish(polygonMsg);
}

void TraversabilityMap::publishUntraversablePolygon(const grid_map::Polygon& untraversablePolygon, double zPosition) {
  if (untraversablePolygonPublisher_.getNumSubscribers() < 1 || untraversablePolygon.nVertices() == 0) {
    return;
  }
  geometry_msgs::PolygonStamped polygonMsg;
  grid_map::PolygonRosConverter::toMessage(untraversablePolygon, polygonMsg);
  for (auto& polygon_point : polygonMsg.polygon.points) {
    polygon_point.z = static_cast<float>(zPosition);
  }
  untraversablePolygonPublisher_.publish(polygonMsg);
}

std::string TraversabilityMap::getMapFrameId() const { return mapFrameId_; }

double TraversabilityMap::getDefaultTraversabilityUnknownRegions() const { return traversabilityDefault_; }

void TraversabilityMap::setDefaultTraversabilityUnknownRegions(const double& defaultTraversability) {
  traversabilityDefault_ = boundTraversabilityValue(defaultTraversability);
}

void TraversabilityMap::restoreDefaultTraversabilityUnknownRegionsReadAtInit() {
  setDefaultTraversabilityUnknownRegions(traversabilityDefaultReadAtInit_);
}

double TraversabilityMap::boundTraversabilityValue(const double& traversabilityValue) const {
  if (traversabilityValue > traversabilityMaxValue) {
    ROS_WARN("Passed traversability value (%f) is higher than max allowed value (%f). It is set equal to the max.", traversabilityValue,
             traversabilityMaxValue);
    return traversabilityMaxValue;
  }
  if (traversabilityValue < traversabilityMinValue) {
    ROS_WARN("Passed traversability value (%f) is lower than min allowed value (%f). It is set equal to the min.", traversabilityValue,
             traversabilityMinValue);
    return traversabilityMinValue;
  }
  return traversabilityValue;
}

bool TraversabilityMap::mapHasValidTraversabilityAt(double x, double y) const {
  grid_map::Position positionToCheck(x, y);
  grid_map::Index indexToCheck;
  boost::recursive_mutex::scoped_lock scopedLockForTraversabilityMap(traversabilityMapMutex_);
  auto indexObtained = traversabilityMap_.getIndex(positionToCheck, indexToCheck);
  if (!indexObtained) {
    ROS_ERROR("It was not possible to get index of the position (%f, %f) in the current grid_map representation of the traversability map.",
              x, y);
    return false;
  }

  return traversabilityMap_.isValid(indexToCheck, traversabilityType_);
}

void TraversabilityMap::setRobotPose(geometry_msgs::PointStamped position){
  robotPos_relative_to_odom_ = position;
}

void TraversabilityMap::setCameraModel(const sensor_msgs::CameraInfoConstPtr& info_msg){
  cam_model_.fromCameraInfo(info_msg);
  /*
  ROS_DEBUG_STREAM("Tx = " << cam_model_.Tx());
  ROS_DEBUG_STREAM("Ty = " << cam_model_.Ty());
  ROS_DEBUG_STREAM("Cx = " << cam_model_.cx());
  ROS_DEBUG_STREAM("Cy = " << cam_model_.cy());
  ROS_DEBUG_STREAM("fx = " << cam_model_.fx());
  ROS_DEBUG_STREAM("fy = " << cam_model_.fy());
  ROS_DEBUG_STREAM("Full projection matrix" << cam_model_.fullProjectionMatrix());*/
}

void TraversabilityMap::getSemanticMask(const sensor_msgs::ImageConstPtr& image_msg){
  cv_bridge::CvImagePtr input_bridge;
  try
  {
    input_bridge = cv_bridge::toCvCopy(image_msg, sensor_msgs::image_encodings::BGR8);
    semantic_mask_ = input_bridge->image;
    //cv::imwrite("/home/tiga/Documents/IRP/dev/testing.test.jpg", semantic_mask_);
  }
  catch(cv_bridge::Exception& e)
  {
    ROS_ERROR("[getSemanticMask] Falied to convert image");
    return;
  }
}


grid_map::GridMap TraversabilityMap::assignTerrainCost(const grid_map::GridMap& MapIn){
  std::string type_ = "traversability_terrain";
  grid_map::GridMap MapOut;
  MapOut = MapIn;
  MapOut.add("terrain_traversability");
  MapOut.add("color");
  double nonTraversableMax = 0.0;
  
  // 1: Extract Grid Position &&
  //    Convert from Odom Frame to Camera Frame
  std::vector<cv::Point3d> GridPosCameraFrame_vector;
  GridPosCameraFrame_vector= extractAllGridPosition(MapOut);
  //ROS_DEBUG_STREAM("GridPosCameraFrame_vector.size() = " << GridPosCameraFrame_vector.size());

  // 2: Convert Grid Position into Pixel Coordinates
  std::vector<cv::Point2d> GridPosPixel_vector;
  GridPosPixel_vector = projectAllGridPosition(GridPosCameraFrame_vector);
  
  /*if(GridPosPixel_vector.size() != 0){
    drawPoints(GridPosPixel_vector);
  }*/

  // 3: Assign Terrain Cost according to projected point's RGB value
  //for(cv::Point2d pixel_uv : GridPosPixel_vector){
  for(unsigned int i=0; i < GridPosPixel_vector.size(); ++i){
    cv::Vec3b cvColor = semantic_mask_.at<cv::Vec3b>(int(GridPosPixel_vector[i].y), int(GridPosPixel_vector[i].x));
    std::array<int,3> bgr_color_vector {int(cvColor.val[0]), int(cvColor.val[1]), int(cvColor.val[2])};
    
    std::array<int,3> floor {155,155,155};
    std::array<int,3> shit {255,0,0};

    Eigen::Vector3i colorVector;
    grid_map::Position terrain_map_grid_pos;
    grid_map::Index terrain_map_grid_index;
    
    terrain_map_grid_pos.x() = filtered_GridPosOdomFrame_vector_[i].x();
    terrain_map_grid_pos.y() = filtered_GridPosOdomFrame_vector_[i].y();
    MapOut.getIndex(terrain_map_grid_pos, terrain_map_grid_index);

    if(bgr_color_vector == floor){
      MapOut.at("terrain_traversability", terrain_map_grid_index) = 1.0;
      colorVector(0) = floor[0];
      colorVector(1) = floor[1];
      colorVector(2) = floor[2];
    }
    else if (bgr_color_vector == shit){
      MapOut.at("terrain_traversability", terrain_map_grid_index) = 0.5;
      colorVector(0) = floor[0];
      colorVector(1) = floor[1];
      colorVector(2) = floor[2];
    }
    else{
      MapOut.at("terrain_traversability", terrain_map_grid_index) = 0.35;
      colorVector(0) = shit[0];
      colorVector(1) = shit[1];
      colorVector(2) = shit[2];
    }
    grid_map::colorVectorToValue(colorVector, MapOut.at("color", terrain_map_grid_index));
  }
  
  

  
  return MapOut;

}

grid_map::Position3 TraversabilityMap::extractSingleGridPosition(const grid_map::Position3& position){
  geometry_msgs::PointStamped GridPositionOdom;
  geometry_msgs::PointStamped GridPositionCamera;
  try{
    GridPositionOdom.header.frame_id = "odom";
    GridPositionOdom.header.stamp = ros::Time(0);
    GridPositionOdom.point.x = position.x();
    GridPositionOdom.point.y = position.y();
    GridPositionOdom.point.z = position.z();
    GridPositionCamera.header.frame_id = "camera_rgb_optical_frame";
    
    transformListener_.waitForTransform(GridPositionCamera.header.frame_id,GridPositionOdom.header.frame_id, ros::Time(0), ros::Duration(1.0));
    transformListener_.transformPoint(GridPositionCamera.header.frame_id, GridPositionOdom, GridPositionCamera);
  }
  catch (tf::TransformException& ex) {
    ROS_ERROR("[TraversabilityMap::assignTerrainCost] Caught an TF Error!");
    ROS_ERROR("%s", ex.what());
  }
  grid_map::Position3 GridPosCameraFrame;
  GridPosCameraFrame.x() = GridPositionCamera.point.x;
  GridPosCameraFrame.y() = GridPositionCamera.point.y;
  GridPosCameraFrame.z() = GridPositionCamera.point.z;
  return GridPosCameraFrame;
}

std::vector<cv::Point3d> TraversabilityMap::extractAllGridPosition(const grid_map::GridMap& MapOut){
  std::vector<cv::Point3d> GridPosCameraFrame_vector;
  if (GridPosOdomFrame_vector_.size() != 0){
    GridPosOdomFrame_vector_.clear();
  }
  for (grid_map::GridMapIterator iterator(MapOut); !iterator.isPastEnd(); ++iterator){
    grid_map::Position3 position;
    MapOut.getPosition3("elevation", *iterator, position);
    //ROS_DEBUG_STREAM("position3 : [" << position.x() << " , " << position.y() << " , " << position.z() << "]");
    grid_map::Position3 GridPosCameraFrame;
    GridPosCameraFrame = extractSingleGridPosition(position);
    
    if(GridPosCameraFrame.z() >= 0.0){
      GridPosCameraFrame_vector.push_back(cv::Point3d(GridPosCameraFrame.x(),GridPosCameraFrame.y(), GridPosCameraFrame.z()));
      GridPosOdomFrame_vector_.push_back(position);
    }
  }
  return GridPosCameraFrame_vector;
}

std::vector<cv::Point2d> TraversabilityMap::projectAllGridPosition(const std::vector<cv::Point3d>& GridPosCameraFrame_vector){
  ROS_DEBUG_STREAM("Start Projection ~!");
  if (filtered_GridPosOdomFrame_vector_.size() != 0){
    filtered_GridPosOdomFrame_vector_.clear();
  }
  std::vector<cv::Point2d> GridPosPixel_vector;
    //for(cv::Point3d point : GridPosCameraFrame_vector){
    for(unsigned int i=0; i < GridPosCameraFrame_vector.size(); ++i){
      cv::Point2d pixel_uv;
      pixel_uv = cam_model_.project3dToPixel(GridPosCameraFrame_vector[i]);
      //pixel_uv = cam_model_.project3dToPixel(point);

      int width, height;
      width = cam_model_.fullResolution().width;
      height = cam_model_.fullResolution().height;
      if((pixel_uv.x < width) && (pixel_uv.y < height) && (pixel_uv.x >=0) && (pixel_uv.y >=0)){
        GridPosPixel_vector.push_back(pixel_uv);
        filtered_GridPosOdomFrame_vector_.push_back(GridPosOdomFrame_vector_[i]);
        //ROS_DEBUG_STREAM("3D point: [" << point.x << " , " << point.y << " , " << point.z <<"]");
        //ROS_DEBUG_STREAM("Project to Pixel: (" << pixel_uv.x << " , " << pixel_uv.y << "]");
      }
    }
    ROS_DEBUG_STREAM("GridPosPixel_vector.size = " << GridPosPixel_vector.size());
    ROS_DEBUG_STREAM("GridPosCameraFrame_vector.size = " << GridPosCameraFrame_vector.size());
    ROS_DEBUG_STREAM("filtered_GridPosOdomFrame_vector_.size = " << filtered_GridPosOdomFrame_vector_.size());
    
    
  

  return GridPosPixel_vector;
}

void TraversabilityMap::drawPoints(const std::vector<cv::Point2d> GridPosPixel_vector){
  cv_bridge::CvImagePtr input_bridge;
  cv::Mat image;
  image = semantic_mask_;
  for(cv::Point2d uv : GridPosPixel_vector){
    cv::circle(image, uv, 3, CV_RGB(255,0,0), -1);
  }
  cv::imwrite("/home/tiga/Documents/IRP/dev/testing/test.jpg", image);
}

}  // namespace traversability_estimation
