﻿#include "plane_segment.h"

#include <tf2/LinearMath/Quaternion.h>

/// Parameters of HOPE 
float th_grid_rsl_ = 0.015; // Resolution in XY direction
float th_z_rsl_ = 0.002; // Resolution in Z direction

/// Calculated parameters
float th_theta_;
float th_norm_;
float th_area_;

// Depth threshold for filtering source cloud, only used for real data
float th_max_depth_ = 8.0;

bool vis_cluster_ = false;

PlaneSegment::PlaneSegment(bool use_real_data, string base_frame, float th_xy, float th_z) :
  use_real_data_(use_real_data),
  fi_(new FetchRGBD),
  pub_it_(nh_),
  src_mono_cloud_(new PointCloudMono),
  src_rgb_cloud_(new PointCloud),
  src_rgbn_cloud_(new PointCloudRGBN),
  cloud_norm_fit_(new PointCloudRGBN),
  cloud_norm_fit_mono_(new PointCloudMono),
  src_sp_cloud_(new PointCloudMono),
  src_normals_(new NormalCloud),
  idx_norm_fit_(new pcl::PointIndices),
  src_z_inliers_(new pcl::PointIndices),
  m_tf_(new Transform),
  utl_(new Utilities),
  base_frame_(base_frame),
  viewer(new pcl::visualization::PCLVisualizer("HOPE Result")),
  hst_("total")
{
  th_grid_rsl_ = th_xy;
  th_z_rsl_ = th_z;
  th_theta_ = th_z_rsl_ / th_grid_rsl_;
  th_area_ = pow(th_grid_rsl_, 2);
  th_norm_ = sqrt(1 / (1 + th_theta_ / 1.414));
  
  // For store max hull id and area
  global_area_temp_ = 0;
  global_size_temp_ = 0;
  
  // Regist the callback if using real point cloud data
  sub_pointcloud_ = nh_.subscribe<sensor_msgs::PointCloud2>("/camera/depth_registered/points", 1, 
                                                            &PlaneSegment::cloudCallback, this);
  
  // Detect table obstacle
  pub_max_plane_ = nh_.advertise<sensor_msgs::PointCloud2>("/vision/max_plane", 1, true);
  pub_cloud_ = nh_.advertise<sensor_msgs::PointCloud2>("/vision/points", 1, true);
  pub_max_mesh_ = nh_.advertise<geometry_msgs::PolygonStamped>("/vision/max_mesh",1, true);
  
  viewer->setBackgroundColor(0.2, 0.22, 0.24);
  viewer->initCameraParameters();
  viewer->addCoordinateSystem(0.5);
}

void PlaneSegment::setParams(int dataset_type, float roll, float pitch, 
                             float tx, float ty, float tz, float qx, float qy, float qz, float qw)
{
  if (dataset_type == 0) {
    roll_ = roll;
    pitch = pitch;
  }
  else if (dataset_type == 1 || dataset_type == 2) {
    tx_ = tx;
    ty_ = ty;
    tz_ = tz;
    qx_ = qx;
    qy_ = qy;
    qz_ = qz;
    qw_ = qw;
  }
  dataset_type_ = dataset_type;
}

void PlaneSegment::getHorizontalPlanes(PointCloud::Ptr cloud)
{
  // Notice that the point cloud is not transformed before this function
  if (use_real_data_) {
    // If using real data, the transform from camera frame to base frame
    // need to be provided
    getSourceCloud();
  }
  else {
    PointCloud::Ptr temp(new PointCloud);
    
    // To remove Nan and unreliable points with z value
    utl_->getCloudByZ(cloud, src_z_inliers_, temp, 0.3, th_max_depth_);
    
    //string name = utl_->getName(0, "so_", 0);
    //pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> src_rgb(temp);
    //viewer->addPointCloud<pcl::PointXYZRGB>(temp, src_rgb, name);
    //viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2.0, name);
    
    if (dataset_type_ >= 1) {
      m_tf_->doTransform(temp, src_rgb_cloud_, tx_, ty_, tz_, qx_, qy_, qz_, qw_);
    }
    else {
      m_tf_->doTransform(temp, src_rgb_cloud_, roll_, pitch_);
    }
    utl_->pointTypeTransfer(src_rgb_cloud_, src_mono_cloud_);
    //pcl::io::savePCDFile("/home/aicrobo/tum.pcd", *src_mono_cloud_);
  }
  
  // Down sampling
  utl_->downSampling(src_mono_cloud_, src_sp_cloud_, th_grid_rsl_, th_z_rsl_);
  
  // Start timer
  hst_.start();
  
  findAllPlanes();
  //  findPlaneWithPCL();
  
  // Stop timer and get total processing time
  hst_.stop();
  hst_.print();
  
  visualizeResult();
}

bool PlaneSegment::getSourceCloud()
{
  while (ros::ok()) {
    if (!src_mono_cloud_->points.empty())
      return true;
    
    // Handle callbacks and sleep for a small amount of time
    // before looping again
    ros::spinOnce();
    ros::Duration(0.005).sleep();
  }
}

void PlaneSegment::cloudCallback(const sensor_msgs::PointCloud2ConstPtr &msg)
{
  if (msg->data.empty()) {
    ROS_WARN_THROTTLE(31, "PlaneSegment: PointCloud is empty.");
    return;
  }
  pcl::PCLPointCloud2 pcl_pc2;
  pcl_conversions::toPCL(*msg, pcl_pc2);
  pcl::fromPCLPointCloud2(pcl_pc2, *src_rgb_cloud_);
  
  PointCloudMono::Ptr temp_mono(new PointCloudMono);
  utl_->pointTypeTransfer(src_rgb_cloud_, temp_mono);
  
  PointCloudMono::Ptr temp_filtered(new PointCloudMono);
  // Get cloud within range represented by z value
  utl_->getCloudByZ(temp_mono, src_z_inliers_, temp_filtered, 0.3, th_max_depth_);
  
  m_tf_->getTransform(base_frame_, msg->header.frame_id);
  m_tf_->doTransform(temp_filtered, src_mono_cloud_);
}

void PlaneSegment::findPlaneWithPCL() 
{
  PointCloudMono::Ptr temp(new PointCloudMono);
  // To remove Nan and unreliable points with z value
  pcl::PointIndices::Ptr in(new pcl::PointIndices);
  utl_->getCloudByZ(src_sp_cloud_, in, temp, 0.82, 3);
  // Pose heuristics
  pcl::SACSegmentation<pcl::PointXYZ> planeSegmenter;
  pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients);
  pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
  // set the segmentaion parameters
  planeSegmenter.setOptimizeCoefficients(true);
  planeSegmenter.setModelType(pcl::SACMODEL_PLANE);
  planeSegmenter.setMethodType(pcl::SAC_RANSAC);
  planeSegmenter.setDistanceThreshold(th_grid_rsl_);
  
  // Extract the global (environment) dominant plane
  planeSegmenter.setInputCloud(temp);
  planeSegmenter.segment(*inliers, *coefficients);
  if (!inliers->indices.empty())
  {
    viewer->addPlane(*coefficients);
  }
  
  PointCloudMono::Ptr temp_in(new PointCloudMono);
  utl_->getCloudByInliers(temp, temp_in, inliers, false, false);
  string name = utl_->getName(0, "pe_", 0);
  pcl::visualization::PointCloudColorHandlerRandom<pcl::PointXYZ> rgb(temp_in);
  viewer->addPointCloud<pcl::PointXYZ>(temp_in, rgb, name);
  viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 6.0, name);
}

void PlaneSegment::findAllPlanes()
{
  if (src_mono_cloud_->points.empty()) {
    ROS_WARN("PlaneSegment: Source cloud is empty.");
    return;
  }
  
  // Clear temp
  plane_results_.clear();
  plane_z_value_.clear();
  cloud_fit_parts_.clear();
  seed_clusters_indices_.clear();
  plane_coeff_.clear();
  plane_hull_.clear();
  plane_mesh_.clear();
  global_area_temp_ = 0.0;
  global_size_temp_ = 0;
  hst_.reset();
  
  utl_->estimateNorm(src_sp_cloud_, src_normals_, 1.01 * th_grid_rsl_); 
  utl_->getCloudByNorm(src_normals_, idx_norm_fit_, th_norm_);
  
  if (idx_norm_fit_->indices.empty()) return;
  
  utl_->getCloudByInliers(src_sp_cloud_, cloud_norm_fit_mono_, idx_norm_fit_, false, false);
  
  //calInitClusters(cloud_norm_fit_mono_);
  //getMeanZofEachCluster(cloud_norm_fit_mono_);
  //extractPlaneForEachZ(cloud_norm_fit_mono_);
  //ZRGEachCluster(cloud_norm_fit_mono_);
  
  clusterWithZGrowing(cloud_norm_fit_mono_);
  getMeanZofEachCluster(cloud_norm_fit_mono_);
  extractPlaneForEachZ(cloud_norm_fit_mono_);  
}

void PlaneSegment::calInitClusters(PointCloudMono::Ptr cloud_in)
{
  utl_->clusterExtract(cloud_in, seed_clusters_indices_, th_z_rsl_, 3, 307200);
}

void PlaneSegment::getMeanZofEachCluster(PointCloudRGBN::Ptr cloud_norm_fit)
{
  if (seed_clusters_indices_.empty())
    ROS_DEBUG("PlaneSegment: Region growing get nothing.");
  
  else {
    size_t k = 0;
    // Traverse each part to determine its mean Z
    for (vector<pcl::PointIndices>::const_iterator it = seed_clusters_indices_.begin(); 
         it != seed_clusters_indices_.end(); ++it) {
      PointCloudRGBN::Ptr cloud_fit_part(new PointCloudRGBN);
      
      pcl::PointIndices::Ptr idx_rg(new pcl::PointIndices);
      idx_rg->indices = it->indices;
      utl_->getCloudByInliers(cloud_norm_fit, cloud_fit_part, idx_rg, false, false);
      
      float part_mean_z = utl_->getCloudMeanZ(cloud_fit_part);
      plane_z_value_.push_back(part_mean_z);
      k++;
    }
    
    ROS_DEBUG("Hypothetic plane number: %d", plane_z_value_.size());
    // Z is ordered from small to large, i.e., low to high
    //sort(planeZVector_.begin(), planeZVector_.end());
  }
}

void PlaneSegment::getMeanZofEachCluster(PointCloudMono::Ptr cloud_norm_fit_mono)
{
  if (seed_clusters_indices_.empty())
    ROS_DEBUG("PlaneSegment: Region growing get nothing.");
  
  else {
    size_t k = 0;
    // Traverse each part to determine its mean Z
    for (vector<pcl::PointIndices>::const_iterator it = seed_clusters_indices_.begin(); 
         it != seed_clusters_indices_.end(); ++it) {
      PointCloudMono::Ptr cloud_fit_part(new PointCloudMono);
      
      pcl::PointIndices::Ptr idx_seed(new pcl::PointIndices);
      idx_seed->indices = it->indices;
      utl_->getCloudByInliers(cloud_norm_fit_mono, cloud_fit_part, idx_seed, false, false);
      
      if (vis_cluster_) {
        string name = utl_->getName(k, "part_", 0);
        pcl::visualization::PointCloudColorHandlerRandom<pcl::PointXYZ> rgb(cloud_fit_part);
        viewer->addPointCloud<pcl::PointXYZ>(cloud_fit_part, rgb, name);
        viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 10.0, name);
      }
      
      float part_mean_z = utl_->getCloudMeanZ(cloud_fit_part);
      //cout << "Cluster has " << idx_seed->indices.size() << " points at z: " << part_mean_z << endl;
      plane_z_value_.push_back(part_mean_z);
      k++;
    }
    
    ROS_DEBUG("Hypothetic plane number: %d", plane_z_value_.size());
    // Z is ordered from small to large, i.e., low to high
    //sort(planeZVector_.begin(), planeZVector_.end());
  }
}

void PlaneSegment::clusterWithZGrowing(PointCloudMono::Ptr cloud_norm_fit_mono)
{
  ZGrowing zg;
  pcl::search::Search<pcl::PointXYZ>::Ptr tree = boost::shared_ptr<pcl::search::Search<pcl::PointXYZ> > 
      (new pcl::search::KdTree<pcl::PointXYZ>);
  
  zg.setMinClusterSize(20);
  zg.setMaxClusterSize(307200);
  zg.setSearchMethod(tree);
  zg.setNumberOfNeighbours(8);
  zg.setInputCloud(cloud_norm_fit_mono);
  zg.setZThreshold(th_z_rsl_);
  
  zg.extract(seed_clusters_indices_);
}

void PlaneSegment::extractPlaneForEachZ(PointCloudMono::Ptr cloud_norm_fit)
{
  size_t id = 0;
  for (vector<float>::iterator cit = plane_z_value_.begin(); 
       cit != plane_z_value_.end(); cit++) {
    getPlane(id, *cit, cloud_norm_fit);
    id++;
  }
}

void PlaneSegment::getPlane(size_t id, float z_in, PointCloudMono::Ptr &cloud_norm_fit_mono)
{
  pcl::ModelCoefficients::Ptr coeff(new pcl::ModelCoefficients);
  // Plane function: ax + by + cz + d = 0, here coeff[3] = d = -cz
  coeff->values.push_back(0.0);
  coeff->values.push_back(0.0);
  coeff->values.push_back(1.0);
  coeff->values.push_back(-z_in);
  
  pcl::PointIndices::Ptr idx_seed (new pcl::PointIndices);
  idx_seed->indices = seed_clusters_indices_[id].indices;
  
  // Extract one plane from clusters
  PointCloudMono::Ptr cluster_near_z(new PointCloudMono);
  utl_->getCloudByInliers(cloud_norm_fit_mono, cluster_near_z, idx_seed, false, false);
  
  PointCloud::Ptr cloud_2d_rgb(new PointCloud);
  if (!errorAnalyse(z_in, cluster_near_z, cloud_2d_rgb, true)) {
    return;
  }
  plane_results_.push_back(cloud_2d_rgb);
  
  if (cloud_2d_rgb->points.size() > global_size_temp_) {
    plane_max_result_ = cloud_2d_rgb;
    global_size_temp_ = cloud_2d_rgb->points.size();
  }
  
  //getFakeColorCloud(z_in, cluster_near_z, cloud_proj_fc, true);
  
  // Use convex hull to represent the plane patch
  //  PointCloudMono::Ptr cloud_hull(new PointCloudMono);
  //  PointCloud::Ptr cloud_hull_c(new PointCloud);
  //  pcl::ConvexHull<pcl::PointXYZRGB> hull;
  //  pcl::PolygonMesh mesh;
  //  hull.setInputCloud(cloud_proj_fc);
  //  hull.setComputeAreaVolume(true);
  //  hull.reconstruct(*cloud_hull_c);
  //  hull.reconstruct(mesh);
  //  utl_->pointTypeTransfer(cloud_hull_c, cloud_hull);
  
  //  float area_hull = hull.getTotalArea();
  
  // Small plane is filtered here with threshold th_area_
  //  if (cloud_hull->points.size() > 2 && area_hull > th_area_) {
  //    /* Select the plane which has similar th_height and
  //       * largest plane to be the output, notice that z_in is in base_link frame
  //       * th_height_ is the height of table, base_link is 0.4m above the ground */
  //    if (area_hull > global_area_temp_) {
  //      plane_max_hull_ = cloud_hull;
  //      plane_max_mesh_ = mesh;
  //      plane_max_coeff_ = coeff;
  //      // Update temp
  //      global_area_temp_ = area_hull;
  //    }
  
  //    //    PointCloud::Ptr cluster_fake(new PointCloud);
  //    //    getFakeColorCloud(z_in, cluster_near_z, cluster_fake);
  //    //    string name;
  //    //    /// Point size must be set after adding point cloud
  //    //    // Add source colored cloud for reference
  //    //    utl_->getName(id, "fake_", 0, name);
  //    //    pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> src_rgb(cluster_fake);
  //    //    viewer->addPointCloud<pcl::PointXYZRGB>(cluster_fake, src_rgb, name);
  //    //    viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 16.0, name);
  
  //    ROS_DEBUG("Found plane with area %f.", area_hull);
  //    plane_coeff_.push_back(coeff);
  //    plane_points_.push_back(cluster_near_z);
  //    plane_hull_.push_back(cloud_hull);
  //    plane_mesh_.push_back(mesh);
  //  }
}

bool PlaneSegment::errorAnalyse(float z, PointCloudMono::Ptr cloud_in, 
                                PointCloud::Ptr &cloud_out, bool fix_z)
{
  pcl::PointXYZ minPt, maxPt;
  pcl::getMinMax3D(*cloud_in, minPt, maxPt);
  float dz = maxPt.z - minPt.z;
  
  cloud_out->width = cloud_in->width;
  cloud_out->height = cloud_in->height;
  cloud_out->resize(cloud_out->width *cloud_out->height);
  
  size_t k = 0;
  for (PointCloudMono::const_iterator pit = cloud_in->begin(); 
       pit != cloud_in->end(); ++pit) {
    float dis_z = pit->z - z;
    //avr_dz += fabs(dis_z);
    // The color represents the distance from the point in cloud_in to z
    float rgb = utl_->shortRainbowColorMap(dis_z, minPt.z - z, maxPt.z - z);
    
    cloud_out->points[k].x = pit->x;
    cloud_out->points[k].y = pit->y;
    if (fix_z)
      cloud_out->points[k].z = z;
    else
      cloud_out->points[k].z = pit->z;
    cloud_out->points[k].rgb = rgb;
    k++;
  }
  
  float dxy_max = 0;
  if (utl_->pcaAnalyse(cloud_out, dxy_max)) {
    float zmax = dxy_max * th_theta_;
    if (zmax > dz)
      return true;
    else {
      cout << "Error exceeded the threshold for current point cloud" << endl;
      return false;
    }
  }
  else
    return false;
}

void PlaneSegment::visualizeResult()
{
  // For visualizing in RViz
  //publishCloud(src_rgb_cloud_, pub_cloud_);
  //publishCloud(plane_max_hull_, pub_max_plane_);
  
  // Clear temps
  viewer->removeAllPointClouds();
  viewer->removeAllShapes();
  
  /// Point size must be set after adding point cloud
  // Add source colored cloud for reference
  string name = utl_->getName(0, "source_", 0);
  pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> src_rgb(src_rgb_cloud_);
  if (!viewer->updatePointCloud(src_rgb_cloud_, src_rgb, name)){
    viewer->addPointCloud<pcl::PointXYZRGB>(src_rgb_cloud_, src_rgb, name);
    viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 2.0, name);
  }
  
  //viewer->addPointCloudNormals<pcl::PointXYZRGBNormal, pcl::Normal>(src_rgbn_cloud_, src_normals_, 10, 0.05, "src_normals");
  //viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY, 0.9, name);
  
  //utl_->getName(0, "max_", 0, name);
  //pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> occ_rgb(plane_max_result_);
  //viewer->addPointCloud<pcl::PointXYZRGB>(plane_max_result_, occ_rgb, name);
  //viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 6.0, name);
  
  // Add normal filtered cloud as reference
  //  utl_->getName(0, "norm_", "", name);
  //  pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGBNormal> src_rgbn(cloud_norm_fit_);
  //  viewer->addPointCloud<pcl::PointXYZRGBNormal>(cloud_norm_fit_, src_rgbn, name);
  //  viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 4.0, name);
  
  for (size_t i = 0; i < plane_results_.size(); i++) {
    // Add hull points
    //utl_->getName(i, "hull_", "", name);
    //pcl::visualization::PointCloudColorHandlerRandom<pcl::PointXYZ> rgb(plane_hull_[i]);
    //viewer->addPointCloud<pcl::PointXYZ>(plane_hull_[i], rgb, name);
    //viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 10.0, name);
    
    // Add plane points
    //utl_->getName(i, "plane_", "", name);
    //pcl::visualization::PointCloudColorHandlerRandom<pcl::PointXYZ> rgb(plane_points_[i]);
    //viewer->addPointCloud<pcl::PointXYZ>(plane_points_[i], rgb, name);
    //viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 6.0, name);
    name = utl_->getName(i, "occ_", 0);
    pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> occ_rgb(plane_results_[i]);
    if (!viewer->updatePointCloud(plane_results_[i], occ_rgb, name)){
      viewer->addPointCloud<pcl::PointXYZRGB>(plane_results_[i], occ_rgb, name);
      viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 6.0, name);
    }
    
    // Add hull polygons
    //utl_->getName(i, "poly_", 0, name);        
    
    // Use Random color
    //viewer->addPolygonMesh(plane_mesh_[i], name);
    //viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY, 0.9, name);
    //double red = 0;
    //double green = 0;
    //double blue = 0;;
    //pcl::visualization::getRandomColors(red, green, blue);
    //viewer->setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_COLOR, red, green, blue, name);
    
    // Use point cloud color
    //PointCloud::Ptr cc(new PointCloud);
    //pcl::fromPCLPointCloud2(plane_mesh_[i].cloud, *cc);
    //viewer->addPolygonMesh<pcl::PointXYZRGB>(cc, plane_mesh_[i].polygons, name);
    
    // Add model
    //NormalCloud::Ptr normals(new NormalCloud);
    //normals->height = plane_points_[i]->height;
    //normals->width  = plane_points_[i]->width;
    //normals->is_dense = true;
    //normals->resize(normals->height * normals->width);
    //for (size_t j = 0; j < normals->size(); ++j) {
    // normals->points[j].normal_x = 0;
    //  normals->points[j].normal_y = 0;
    //  normals->points[j].normal_z = 1;
    //}
    //pcl::PolygonMesh m = utl_->generateMesh(plane_points_[i], normals);
    //utl_->generateName(i, "model_", "", name);
    //const double kTableThickness = 0.02;
    //viewer->addCube(input.x_min, input.x_max, input.y_min, input.y_max,
    //                input.table_height - kTableThickness, input.table_height, 1.0, 0.0, 0.0,
    //                "support_surface");
    //viewer->addPolygonMesh(m, name);
    //viewer->setShapeRenderingProperties(pcl::visualization::PCL_VISUALIZER_OPACITY,
    //                                    0.6, name);
    //viewer->setShapeRenderingProperties(
    //      pcl::visualization::PCL_VISUALIZER_REPRESENTATION,
    //      pcl::visualization::PCL_VISUALIZER_REPRESENTATION_WIREFRAME,
    //      name);
  }
  
  while (!viewer->wasStopped()) {
    viewer->spinOnce(1); // ms
    if (dataset_type_ == 2)
      break;
  }
}

template <typename PointTPtr>
void PlaneSegment::publishCloud(PointTPtr cloud, ros::Publisher pub)
{
  sensor_msgs::PointCloud2 ros_cloud;
  pcl::toROSMsg(*cloud, ros_cloud);
  ros_cloud.header.frame_id = base_frame_;
  ros_cloud.header.stamp = ros::Time(0);
  pub.publish(ros_cloud);
}

void PlaneSegment::publishMesh(pcl::PolygonMesh mesh, ros::Publisher pub)
{
  //  geometry_msgs::PolygonStamped ros_mesh;
  //  pcl_conversions::fromPCL(mesh, ros_mesh);
  //  ros_mesh.header.frame_id = base_frame_;
  //  ros_mesh.header.stamp = ros::Time(0);
  //  pub.publish(ros_mesh);
}

void PlaneSegment::poisson_reconstruction(NormalPointCloud::Ptr point_cloud, 
                                          pcl::PolygonMesh& mesh)
{
  // Initialize poisson reconstruction
  pcl::Poisson<pcl::PointNormal> poisson;
  
  /*
     * Set the maximum depth of the tree used in Poisson surface reconstruction.
     * A higher value means more iterations which could lead to better results but
     * it is also more computationally heavy.
     */
  poisson.setDepth(10);
  poisson.setInputCloud(point_cloud);
  
  // Perform the Poisson surface reconstruction algorithm
  poisson.reconstruct(mesh);
}

/**
 * Reconstruct a point cloud to a mesh by estimate the normals of the point cloud
 *
 * @param point_cloud The input point cloud that will be reconstructed
 * @return Returns a reconstructed mesh
 */
pcl::PolygonMesh PlaneSegment::mesh(const PointCloudMono::Ptr point_cloud, 
                                    NormalCloud::Ptr normals)
{
  // Add the normals to the point cloud
  NormalPointCloud::Ptr cloud_with_normals(new NormalPointCloud);
  pcl::concatenateFields(*point_cloud, *normals, *cloud_with_normals);
  
  // Point cloud to mesh reconstruction
  pcl::PolygonMesh mesh;
  poisson_reconstruction(cloud_with_normals, mesh);
  
  return mesh;
}
