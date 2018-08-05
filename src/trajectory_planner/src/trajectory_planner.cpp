/**
* This file is part of Ewok.
*
* Copyright 2017 Vladyslav Usenko, Technical University of Munich.
* Developed by Vladyslav Usenko <vlad dot usenko at tum dot de>,
* for more information see <http://vision.in.tum.de/research/robotvision/replanning>.
* If you use this code, please cite the respective publications as
* listed on the above website.
*
* Ewok is free software: you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* Ewok is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with Ewok. If not, see <http://www.gnu.org/licenses/>.
*/

#include <ros/ros.h>
#include <Eigen/Core>
#include <geometry_msgs/PoseStamped.h>
#include <ewok/ed_ring_buffer.h>
#include <sensor_msgs/Image.h>
#include <cholmod.h>
#include <chrono>
#include <cv_bridge/cv_bridge.h>
#include <tf/transform_listener.h>
#include <tf_conversions/tf_eigen.h>

#include <ewok/polynomial_3d_optimization.h>
#include <ewok/uniform_bspline_3d_optimization.h>


static const int POW = 6;
//static const int N = (1 << POW);
static const double dt = 0.5;
static const int num_opt_points = 7;
static const double max_velocity = 0.3;
static const double max_acceleration = 0.5;
static const double resolution = 0.1;
static const double distance_threshold = 0.3;

bool initialized = false;

ros::Subscriber local_pos_sub;
ros::Subscriber endpoint_pos_sub;
ros::Subscriber depth_cam_sub;

ros::Publisher setpoint_pos_pub;

geometry_msgs::PoseStamped endpoint_position;
geometry_msgs::PoseStamped local_position;
sensor_msgs::Image depth_cam_img;

ewok::PolynomialTrajectory3D<10>::Ptr traj;
ewok::EuclideanDistanceRingBuffer<POW>::Ptr edrb;
ewok::UniformBSpline3DOptimization<6>::Ptr spline_optimization;

tf::TransformListener * listener;

void endpoint_position_cb(const geometry_msgs::PoseStamped::ConstPtr& msg)
{
  endpoint_position = *msg;

  // Set up desired trajectory
  Eigen::Vector3d start_point(local_position.pose.position.x, local_position.pose.position.y, local_position.pose.position.z),
                  end_point(endpoint_position.pose.position.x, endpoint_position.pose.position.y, endpoint_position.pose.position.z);

  Eigen::Vector4d limits(max_velocity, max_acceleration, 0, 0);

  ewok::Polynomial3DOptimization<10> to(limits);
  typename ewok::Polynomial3DOptimization<10>::Vector3Array path;
  path.push_back(start_point);
  path.push_back(end_point);

  traj = to.computeTrajectory(path);

  spline_optimization.reset(new ewok::UniformBSpline3DOptimization<6>(traj, dt));

  spline_optimization->setNumControlPointsOptimized(num_opt_points);
  spline_optimization->setDistanceBuffer(edrb);
  spline_optimization->setDistanceThreshold(distance_threshold);
  spline_optimization->setLimits(limits);

}

void depth_cam_cb(const sensor_msgs::Image::ConstPtr& msg)
{
  cv_bridge::CvImageConstPtr cv_ptr;
  try
  {
    cv_ptr = cv_bridge::toCvShare(msg);
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }

  const float fx = 457.815979003906;
  const float fy = 457.815979003906;
  const float cx = 249.322647094727;
  const float cy = 179.5;

  tf::StampedTransform transform;

  try
  {
    listener->lookupTransform("map", "drone", msg->header.stamp, transform);
  }
  catch (tf::TransformException &ex)
  {
    ROS_INFO("Couldn't get transform");
    ROS_WARN("%s",ex.what());
    return;
  }

  Eigen::Affine3d dT_w_c;
  tf::transformTFToEigen(transform, dT_w_c);

  Eigen::Affine3f T_w_c = dT_w_c.cast<float>();

  uint16_t * data = (uint16_t *) cv_ptr->image.data;

  auto t1 = std::chrono::high_resolution_clock::now();

  ewok::EuclideanDistanceRingBuffer<POW>::PointCloud cloud1;

  for(int u=0; u < cv_ptr->image.cols; u+=4)
  {
    for(int v=0; v < cv_ptr->image.rows; v+=4)
    {
      uint16_t uval = data[v*cv_ptr->image.cols + u];

      //ROS_INFO_STREAM(val);

      if(uval > 0)
      {
        float val = uval/5000.0;
        Eigen::Vector4f p;
        p[0] = val*(u - cx)/fx;
        p[1] = val*(v - cy)/fy;
        p[2] = val;
        p[3] = 1;

        p = T_w_c * p;

        cloud1.push_back(p);
      }
    }
  }

  Eigen::Vector3f origin = (T_w_c * Eigen::Vector4f(0,0,0,1)).head<3>();

  auto t2 = std::chrono::high_resolution_clock::now();

  if(!initialized)
  {
    Eigen::Vector3i idx;
    edrb->getIdx(origin, idx);

    ROS_INFO_STREAM("Origin: " << origin.transpose() << " idx " << idx.transpose());

    edrb->setOffset(idx);

    initialized = true;
  }
  else
  {
    Eigen::Vector3i origin_idx, offset, diff;
    edrb->getIdx(origin, origin_idx);

    offset = edrb->getVolumeCenter();
    diff = origin_idx - offset;

    while(diff.array().any())
    {
      //ROS_INFO("Moving Volume");
      edrb->moveVolume(diff);

      offset = edrb->getVolumeCenter();
      diff = origin_idx - offset;
    }
  }

  //ROS_INFO_STREAM("cloud1 size: " << cloud1.size());


  auto t3 = std::chrono::high_resolution_clock::now();

  edrb->insertPointCloud(cloud1, origin);

  auto t4 = std::chrono::high_resolution_clock::now();

  //f_time << std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count() << " " <<
  //          std::chrono::duration_cast<std::chrono::nanoseconds>(t3-t2).count() << " " <<
  //          std::chrono::duration_cast<std::chrono::nanoseconds>(t4-t3).count() << std::endl;

  //visualization_msgs::Marker m_occ, m_free;
  //edrb->getMarkerOccupied(m_occ);
  //edrb->getMarkerFree(m_free);


  //occ_marker_pub.publish(m_occ);
  //free_marker_pub.publish(m_free);
}

void local_position_cb(const geometry_msgs::PoseStamped::ConstPtr& msg)
{
  local_position = *msg;
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "collision_avoid");
  ros::NodeHandle nh;

  listener = new tf::TransformListener;

  endpoint_pos_sub = nh.subscribe<geometry_msgs::PoseStamped>("/ewok/endpoint_position", 10, endpoint_position_cb);
  local_pos_sub = nh.subscribe<geometry_msgs::PoseStamped>("/mavros/local_position/pose", 10, local_position_cb);
  depth_cam_sub = nh.subscribe<sensor_msgs::Image>("/camera/depth/image_raw", 10, depth_cam_cb);

  setpoint_pos_pub = nh.advertise<geometry_msgs::PoseStamped>("/mavros/setpoint_position/local", 10);

  edrb.reset(new ewok::EuclideanDistanceRingBuffer<POW>(resolution, 1.0));
  //ewok::EuclideanDistanceRingBuffer<POW>::PointCloud cloud;

  ros::Rate r(1);
  while (ros::ok())
  {
    r.sleep();

    ros::spinOnce();
  }

  return 0;
}