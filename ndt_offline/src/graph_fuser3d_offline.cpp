#include <ndt_fuser/ndt_fuser_hmt.h>
#include <ndt_offline/VelodyneBagReader.h>
#include <ndt_generic/eigen_utils.h>
// PCL specific includes
#include <pcl/conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <ndt_map/ndt_map.h>
#include <ndt_map/ndt_cell.h>
#include <ndt_map/pointcloud_utils.h>
#include <tf_conversions/tf_eigen.h>
#include <cstdio>
#include <Eigen/Eigen>
#include <Eigen/Geometry>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <algorithm>
#include <boost/program_options.hpp>
#include "graph_map_fuser.h"
#include "ndt/ndt_map_param.h"
#include "ndt/ndtd2d_reg_type.h"
#include "ndt/ndt_map_type.h"
#include "ros/ros.h"
#include "nav_msgs/Odometry.h"
#include "ros/publisher.h"
#include "tf/transform_broadcaster.h"
#include "ndt_generic/eigen_utils.h"


using namespace libgraphMap;
namespace po = boost::program_options;
using namespace std;
using namespace lslgeneric;

/*!
 * \brief Parameters for the offline fuser node
 */






std::string dirname;
std::string map_dirname;
std::string base_name;
std::string dataset;
std::ofstream gt_file, odom_file, est_file, sensorpose_est_file; //output files
//map parameters
double size_xy;
double size_z;
double resolution;
double resolution_local_factor;
double hori_min, hori_max;
double min_dist, min_rot_in_deg;
double max_range, min_range;
int itrs;
int nb_neighbours;
int nb_scan_msgs;

bool use_odometry;
bool visualize;
bool do_baseline;
bool guess_zpitch;
bool use_multires;
bool beHMT;
bool fuse_incomplete ;
bool preload;
bool filter_fov;
bool step_control;
bool check_consistency;
bool registration2d;


bool COOP;
bool VCE;
bool VCEnov16;
bool dustcart;
bool alive;
bool save_map;
bool use_gt_as_interp_link;
bool save_clouds;

bool match2d,disable_reg, do_soft_constraints;
lslgeneric::MotionModel2d::Params motion_params;
std::string base_link_id, gt_base_link_id, tf_world_frame;
std::string velodyne_config_file;
std::string velodyne_packets_topic;
std::string velodyne_frame_id;
std::string map_type_name,registration_type_name;
std::string tf_topic;
tf::Transform tf_sensor_pose;
Eigen::Affine3d sensor_offset,fuser_pose;//Mapping from base frame to sensor frame
ros::NodeHandle *n_;
RegParamPtr regParPtr;
MapParamPtr mapParPtr;
GraphParamPtr graphParPtr;
double sensor_time_offset;

double maxRotationNorm_;
double compound_radius_;
double interchange_radius_;
double maxTranslationNorm_;
double rotationRegistrationDelta_;
double sensorRange_;
double translationRegistrationDelta_;
double mapSizeZ_;


ros::Publisher *gt_pub,*fuser_pub,*cloud_pub;
nav_msgs::Odometry gt_pose_msg,fuser_pose_msg;
pcl::PointCloud<pcl::PointXYZ>::Ptr msg_cloud;
/*
inline void normalizeEulerAngles(Eigen::Vector3d &euler) {
  if (fabs(euler[0]) > M_PI/2) {
    euler[0] += M_PI;
    euler[1] += M_PI;
    euler[2] += M_PI;

    euler[0] = angles::normalize_angle(euler[0]);
    euler[1] = angles::normalize_angle(euler[1]);
    euler[2] = angles::normalize_angle(euler[2]);
  }
}
*/
template<class T> std::string toString (const T& x)
{
  std::ostringstream o;

  if (!(o << x))
    throw std::runtime_error ("::toString()");

  return o.str ();
}

void filter_fov_fun(pcl::PointCloud<pcl::PointXYZ> &cloud, pcl::PointCloud<pcl::PointXYZ> &cloud_nofilter, double hori_min, double hori_max) {
  for(int i=0; i<cloud_nofilter.points.size(); ++i) {
    double ang = atan2(cloud_nofilter.points[i].y, cloud_nofilter.points[i].x);
    if(ang < hori_min || ang > hori_max) continue;
    cloud.points.push_back(cloud_nofilter.points[i]);
  }
  cloud.width = cloud.points.size();
  cloud.height = 1;
  std::cout << "nb clouds : " << cloud.points.size() << std::endl;
}


std::string transformToEvalString(const Eigen::Transform<double,3,Eigen::Affine,Eigen::ColMajor> &T) {
  std::ostringstream stream;
  stream << std::setprecision(std::numeric_limits<double>::digits10);
  Eigen::Quaternion<double> tmp(T.rotation());
  stream << T.translation().transpose() << " " << tmp.x() << " " << tmp.y() << " " << tmp.z() << " " << tmp.w() << std::endl;
  return stream.str();
}
bool GetSensorPose(const std::string &dataset,  Eigen::Vector3d & transl,  Eigen::Vector3d &euler,tf::Transform &tf_sensor){

  tf::Quaternion quat;
  bool found_sensor_pose=false;
  if(dataset.compare("oru-basement")){
    transl[0]=0.3;
    transl[1]=0;
    transl[2]=0;
    euler[0]=0;
    euler[1]=0;
    euler[2]=-1.62;
    found_sensor_pose=true;
  }
  else if(dataset.compare("default")){
    transl[0]=0.3;
    transl[1]=0;
    transl[2]=0;
    euler[0]=0;
    euler[1]=0;
    euler[2]=-1.62;
    found_sensor_pose=true;
  }
  quat.setRPY(euler[0], euler[1], euler[2]);
  tf::Vector3 trans(transl[0], transl[1], transl[2]);
  tf_sensor  = tf::Transform(quat,trans);
  tf::poseTFToEigen(tf_sensor,sensor_offset);
  return found_sensor_pose;
}

void saveCloud(int counter, const pcl::PointCloud<pcl::PointXYZ> &cloud) {
  std::string pcd_file = std::string("cloud") + toString(counter) + std::string(".pcd");
  std::cout << "saving : " << pcd_file << std::endl;
  pcl::io::savePCDFileASCII (pcd_file, cloud);
}
//!
//! \brief ReadAllParameters by advertise all program options and read then from command line
//! \param desc is augmented with all options
//! \param argc
//! \param argv
//! \return true if an error
//!
//!

//!
//! \brief CreateOutputFiles opens filestream to where the result is stored
//! \return true if creation was succesfull otherwise return false
//!
bool CreateOutputFiles(){

  std::string filename;
  {
    filename = base_name + std::string("_gt.txt");
    gt_file.open(filename.c_str());
  }
  {
    filename = base_name + std::string("_est.txt");
    est_file.open(filename.c_str());
  }
  {
    filename = base_name + std::string("_sensorpose_est.txt");
    sensorpose_est_file.open(filename.c_str());
  }
  {
    filename = base_name + std::string("_odom.txt");
    odom_file.open(filename.c_str());
  }
  if (!gt_file.is_open() || !est_file.is_open() || !odom_file.is_open())
  {
    return false;
    ROS_ERROR_STREAM("Failed to open : ");
  }
  else
    return true;
}
//!
//! \brief LocateRosBagFilePaths
//! \param scanfiles Get the path of all rosbags in folder recursively
//! \return true if the function is succesfull, return false if no bag was found or an error occured
//!
bool LocateRosBagFilePaths(const std::string &folder_name,std::vector<std::string> &scanfiles){
  DIR *dir;
  struct dirent *ent;
  if ((dir = opendir (folder_name.c_str())) != NULL) {
    while ((ent = readdir (dir)) != NULL) {
      if(ent->d_name[0] == '.') continue;
      char tmpcname[400];
      snprintf(tmpcname,399,"%s/%s",folder_name.c_str(),ent->d_name);
      std::string tmpfname = tmpcname;
      scanfiles.push_back(tmpfname);
    }
    closedir (dir);
  } else {
    std::cerr<<"Could not parse dir name\n";
    return false;
  }
  sort(scanfiles.begin(),scanfiles.end());
  {
    std::cout << "files to be loaded : " << std::endl;
    for (size_t i = 0; i < scanfiles.size(); i++) {
      std::cout << " " << scanfiles[i] << std::flush;
    }
    std::cout << std::endl;
  }
  return true;
}
bool ReadAllParameters(po::options_description &desc,int &argc, char ***argv){
  Eigen::Vector3d transl;
  Eigen::Vector3d euler;
  // First of all, make sure to advertise all program options
  desc.add_options()
      ("help", "produce help message")
      ("map_type_name", po::value<string>(&map_type_name)->default_value(std::string("default")), "type of map to use e.g. ndt_map or ndt_dl_map (default it default)")
      ("registration_type_name", po::value<string>(&registration_type_name)->default_value(std::string("default")), "type of map to use e.g. ndt_d2d_reg or ndt_dl_reg (default it default)")
      ("visualize", "visualize the output")
      ("use-odometry", "use initial guess from odometry")
      ("disable-mapping", "build maps from cloud data")
      ("no-step-control", "use step control in the optimization (default=false)")
      ("pre-load", "loads maps from the map directory if available")
      ("base-name", po::value<string>(&base_name), "prefix for all generated files")
      ("data-set", po::value<string>(&dataset)->default_value(std::string("default")), "choose which dataset that is currently used, this option will assist with assigning the sensor pose")
      ("dir-name", po::value<string>(&dirname), "where to look for ros bags")
      ("map-dir-name", po::value<string>(&map_dirname), "where to save the pieces of the map (default it ./map)")
      ("size-xy", po::value<double>(&size_xy)->default_value(150.), "size of the central map xy")
      ("itrs", po::value<int>(&itrs)->default_value(30), "resolution of the map")
      ("baseline", "run also the baseline registration algorithms")
      ("guess-zpitch", "guess also z and pitch from odometry")
      ("use-multires", "run the multi-resolution guess")
      ("fuse-incomplete", "fuse in registration estimate even if iterations ran out. may be useful in combination with low itr numbers")
      ("filter-fov", "cutoff part of the field of view")
      ("hori-max", po::value<double>(&hori_max)->default_value(2*M_PI), "the maximum field of view angle horizontal")
      ("hori-min", po::value<double>(&hori_min)->default_value(-hori_max), "the minimum field of view angle horizontal")
      ("COOP", "if parameters from the COOP data set should be used (sensorpose)")
      ("VCE", "if sensorpose parameters from VCE should be used")
      ("VCEnov16", "if sensorpose parameters from VCE 16 nov data collection should be used")
      ("dustcart", "if the sensorpose parameters from dustcart should be used")
      ("do-soft-constraints", "if soft constraints from odometry should be used")
      ("Dd", po::value<double>(&motion_params.Dd)->default_value(1.), "forward uncertainty on distance traveled")
      ("Dt", po::value<double>(&motion_params.Dt)->default_value(1.), "forward uncertainty on rotation")
      ("Cd", po::value<double>(&motion_params.Cd)->default_value(1.), "side uncertainty on distance traveled")
      ("Ct", po::value<double>(&motion_params.Ct)->default_value(1.), "side uncertainty on rotation")
      ("Td", po::value<double>(&motion_params.Td)->default_value(1.), "rotation uncertainty on distance traveled")
      ("Tt", po::value<double>(&motion_params.Tt)->default_value(1.), "rotation uncertainty on rotation")
      ("min_dist", po::value<double>(&min_dist)->default_value(0.2), "minimum distance traveled before adding cloud")
      ("min_rot_in_deg", po::value<double>(&min_rot_in_deg)->default_value(5), "minimum rotation before adding cloud")
      ("tf_base_link", po::value<std::string>(&base_link_id)->default_value(std::string("/odom_base_link")), "tf_base_link")
      ("tf_gt_link", po::value<std::string>(&gt_base_link_id)->default_value(std::string("/state_base_link")), "tf ground truth link")
      ("velodyne_config_file", po::value<std::string>(&velodyne_config_file)->default_value(std::string("velo32.yaml")), "configuration file for the scanner")
      ("tf_world_frame", po::value<std::string>(&tf_world_frame)->default_value(std::string("/world")), "tf world frame")
      ("velodyne_packets_topic", po::value<std::string>(&velodyne_packets_topic)->default_value(std::string("/velodyne_packets")), "velodyne packets topic used")
      ("velodyne_frame_id", po::value<std::string>(&velodyne_frame_id)->default_value(std::string("/velodyne")), "frame_id of the velodyne")
      ("alive", "keep the mapper/visualization running even though it is completed (e.g. to take screen shots etc.")
      ("nb_neighbours", po::value<int>(&nb_neighbours)->default_value(2), "number of neighbours used in the registration")
      ("min_range", po::value<double>(&min_range)->default_value(0.6), "minimum range used from scanner")
      ("max_range", po::value<double>(&max_range)->default_value(30), "minimum range used from scanner")
      ("save_map", "saves the map at the end of execution")
      ("nb_scan_msgs", po::value<int>(&nb_scan_msgs)->default_value(1), "number of scan messages that should be loaded at once from the bag")
      ("use_gt_as_interp_link", "use gt when performing point interplation while unwrapping the velodyne scans")
      ("save_clouds", "save all clouds that are added to the map")
      ("tf_topic", po::value<std::string>(&tf_topic)->default_value(std::string("/tf")), "tf topic to listen to")
      ("x", po::value<double>(&transl[0])->default_value(0.), "sensor pose - translation vector x")
      ("y", po::value<double>(&transl[1])->default_value(0.), "sensor pose - translation vector y")
      ("z", po::value<double>(&transl[2])->default_value(0.), "sensor pose - translation vector z")
      ("ex", po::value<double>(&euler[0])->default_value(0.), "sensor pose - euler angle vector x")
      ("ey", po::value<double>(&euler[1])->default_value(0.), "sensor pose - euler angle vector y")
      ("ez", po::value<double>(&euler[2])->default_value(0.), "sensor pose - euler angle vector z")
      ("sensor_time_offset", po::value<double>(&sensor_time_offset)->default_value(0.), "timeoffset of the scanner data")
      ("registration2d","registration2d")
      ("mapSizeZ_", po::value<double>(&mapSizeZ_)->default_value(6.0), "sensor pose - translation vector x")
      ("check-consistency", "if consistency should be checked after registration")
      ("do-soft-constraints", "do_soft_constraints_")
      ("disable-registration", "Disable Registration")
      ("maxRotationNorm",po::value<double>(&maxRotationNorm_)->default_value(0.78539816339),"maxRotationNorm")
      ("maxTranslationNorm",po::value<double>(&maxTranslationNorm_)->default_value(0.4),"maxTranslationNorm")
      ("rotationRegistrationDelta",po::value<double>(&rotationRegistrationDelta_)->default_value(M_PI/6),"rotationRegistrationDelta")
      ("translationRegistrationDelta",po::value<double>(&translationRegistrationDelta_)->default_value(1.5),"sensorRange")
      ("resolution", po::value<double>(&resolution)->default_value(0.6), "resolution of the map")
      ("resolution_local_factor", po::value<double>(&resolution_local_factor)->default_value(1.), "resolution factor of the local map used in the match and fusing step")
      ("use-submap", "Adopt the sub-mapping technique which represent the global map as a set of local submaps")
      ("compound-radius", po::value<double>(&compound_radius_)->default_value(10.0), "Requires sub-mapping enabled, When creating new sub-lamps, information from previous map is transfered to the new map. The following radius is used to select the map objects to transfer")
      ("interchange-radius", po::value<double>(&interchange_radius_)->default_value(10.0), "This radius is used to trigger creation or selection of which submap to use");

  cout<<"working 0"<<endl;
  //Boolean parameres are read through notifiers
  po::variables_map vm;
  po::store(po::parse_command_line(argc, *argv, desc), vm);
  po::notify(vm);
  cout<<"sensor pose"<<endl;
  if(!GetSensorPose(dataset,transl,euler,tf_sensor_pose))
    exit(0);




  mapParPtr= GraphFactory::CreateMapParam(map_type_name); //map_type_name
  regParPtr=GraphFactory::CreateRegParam(registration_type_name);
  graphParPtr=GraphFactory::CreateGraphParam();
  if(mapParPtr==NULL || regParPtr==NULL || graphParPtr==NULL)
    return false;

  use_odometry = vm.count("use-odometry");
  visualize = vm.count("visualize");
  do_baseline = vm.count("baseline");
  guess_zpitch = vm.count("guess-zpitch");
  use_multires = vm.count("use-multires");
  fuse_incomplete = vm.count("fuse-incomplete");
  preload = vm.count("pre-load");
  filter_fov = vm.count("filter-fov");
  step_control = (vm.count("no-step-control") == 0);
  bool use_gt_as_interp_link = vm.count("use_gt_as_interp_link");
  bool save_clouds = vm.count("save_clouds");
  check_consistency=vm.count("check-consistency");
  COOP = vm.count("COOP");
  VCE = vm.count("VCE");
  VCEnov16 = vm.count("VCEnov16");
  dustcart = vm.count("dustcart");
  alive = vm.count("alive");
  save_map = vm.count("save_map");
  registration2d=vm.count("registration2d");
  do_soft_constraints=vm.count("do-soft-constraints");
  regParPtr->do_soft_constraints_ = vm.count("do-soft-constraints");
  regParPtr->enableRegistration_ = !vm.count("disable-registration");
  regParPtr->registration2d_=registration2d;
  regParPtr->maxRotationNorm_=maxRotationNorm_;
  regParPtr->maxTranslationNorm_=maxTranslationNorm_;
  regParPtr->rotationRegistrationDelta_=rotationRegistrationDelta_;
  regParPtr->translationRegistrationDelta_=translationRegistrationDelta_;
  regParPtr->sensorRange_=max_range;
  regParPtr->mapSizeZ_=mapSizeZ_;
  regParPtr->checkConsistency_=check_consistency;
  mapParPtr->sizez_=mapSizeZ_;
  mapParPtr->max_range_=max_range;
  mapParPtr->min_range_=min_range;
  graphParPtr->compound_radius_=compound_radius_;
  graphParPtr->interchange_radius_=interchange_radius_;

  mapParPtr->enable_mapping_=!vm.count("disable-mapping");
  mapParPtr->sizey_=size_xy;
  mapParPtr->sizex_=size_xy;
  graphParPtr->use_submap_=vm.count("use-submap");

  if(  NDTD2DRegParamPtr ndt_reg_ptr=boost::dynamic_pointer_cast<NDTD2DRegParam>(regParPtr)){
    ndt_reg_ptr->resolution_=resolution;
    ndt_reg_ptr->resolutionLocalFactor_=resolution_local_factor;
  }
  if(  NDTMapParamPtr ndt_map_ptr=boost::dynamic_pointer_cast<NDTMapParam>(mapParPtr)){
    ndt_map_ptr->resolution_=resolution;
  }


  //Check if all iputs are assigned
  if (!vm.count("base-name") || !vm.count("dir-name")){
    cout << "Missing base or dir names.\n";
    cout << desc << "\n";
    return false;
  }
  if (vm.count("help")){
    cout << desc << "\n";
    return false;
  }
  if (!vm.count("map-dir-name")){
    map_dirname="map";
  }
  cout<<"base-name:"<<base_name<<endl;
  cout<<"dir-name:"<<dirname<<endl;
  return true;


}

/////////////////////////////////////////////////////////////////////////////////7
/////////////////////////////////////////////////////////////////////////////////7
/// *!!MAIN!!*
/////////////////////////////////////////////////////////////////////////////////7
/////////////////////////////////////////////////////////////////////////////////7
///

int main(int argc, char **argv){
  cout<<"start"<<endl;
  ros::init(argc, argv, "graph_fuser3d_offline");
  cout<<"po options"<<endl;
  po::options_description desc("Allowed options");
  cout<<"node handle"<<endl;
  n_=new ros::NodeHandle("~");
  cout<<"test"<<endl;

  GraphMapFuser *fuser_;
  cout<<"read params"<<endl;
  bool succesfull=ReadAllParameters(desc,argc,&argv);
  if(!succesfull){
    cout<<"Problem reading parameters"<<endl;
    exit(0);
  }

  gt_pub=new ros::Publisher();
  fuser_pub=new ros::Publisher();
  cloud_pub=new ros::Publisher();
  *gt_pub    =n_->advertise<nav_msgs::Odometry>("/GT", 50);
  *fuser_pub =n_->advertise<nav_msgs::Odometry>("/fuser", 50);
  *cloud_pub = n_->advertise<pcl::PointCloud<pcl::PointXYZ>>("/points2", 1);
  tf::TransformBroadcaster br;
  cout<<"done reading params"<<endl;
  gt_pose_msg.header.frame_id="/world";
  fuser_pose_msg.header.frame_id="/world";



  std::string tf_interp_link = base_link_id;
  if (use_gt_as_interp_link) {
    tf_interp_link = gt_base_link_id;
  }

  if(filter_fov) {
    cout << "filtering FOV of sensor to min/max "<<hori_min<<" "<<hori_max<<endl;
  }

  base_name += motion_params.getDescString() + std::string("_res") + toString(resolution) + std::string("_SC") + toString(do_soft_constraints) + std::string("_mindist") + toString(min_dist) + std::string("_sensorcutoff") + toString(max_range) + std::string("_stepcontrol") + toString(step_control) + std::string("_neighbours") + toString(nb_neighbours) + std::string("_rlf") + toString(resolution_local_factor);


  ros::Time::init();
  srand(time(NULL));

  //ndtslammer.disableRegistration = disable_reg;

  /// Set up the sensor link
  tf::StampedTransform sensor_link; ///Link from /odom_base_link -> velodyne
  sensor_link.child_frame_id_ = velodyne_frame_id;
  sensor_link.frame_id_ = tf_interp_link;//tf_base_link; //"/odom_base_link";
  sensor_link.setData(tf_sensor_pose);

  std::vector<std::string> ros_bag_paths;
  if(!LocateRosBagFilePaths(dirname,ros_bag_paths)){
    cout<<"couldnt locate ros bags"<<endl;
    exit(0);
  }
  Eigen::Affine3d Todom_base_prev,Tgt_base_prev;
  int counter = 0;
  if(!CreateOutputFiles()){
    cout<<"couldnt create output files"<<endl;
    exit(0);
  }

  cout<<"opening bag files"<<endl;
  for(int i=0; i<ros_bag_paths.size(); i++) {
    std::string bagfilename = ros_bag_paths[i];
    fprintf(stderr,"Opening %s\n",bagfilename.c_str());
    cout<<velodyne_config_file<<","<<bagfilename<<","<<velodyne_packets_topic<<","<<velodyne_frame_id<<","<<tf_world_frame<<","<<tf_topic<<endl;
    VelodyneBagReader<pcl::PointXYZ> vreader(velodyne_config_file,
                                             bagfilename,
                                             velodyne_packets_topic,  //"/velodyne_packets"
                                             velodyne_frame_id,
                                             tf_world_frame,
                                             tf_topic,
                                             ros::Duration(3600),
                                             &sensor_link, max_range, min_range,
                                             sensor_time_offset);

    pcl::PointCloud<pcl::PointXYZ> cloud, cloud_nofilter;
    tf::Transform tf_scan_source;
    tf::Transform tf_gt_base;


    //cout<<sensor_pose.getOrigin()<<endl;
    while(vreader.readMultipleMeasurements(nb_scan_msgs,cloud_nofilter,tf_scan_source,tf_gt_base,tf_interp_link)){
      if(!n_->ok())
        exit(0);

      if(cloud_nofilter.size()==0) continue;

      if(filter_fov) {
        filter_fov_fun(cloud,cloud_nofilter,hori_min,hori_max);
      } else {
        cloud = cloud_nofilter;
      }

      if (cloud.size() == 0) continue; // Check that we have something to work with depending on the FOV filter here...

      tf::Transform tf_odom_base;
      vreader.getPoseFor(tf_odom_base, base_link_id);
      vreader.getPoseFor(tf_gt_base, gt_base_link_id);

      Eigen::Affine3d Todom_base,Tgt_base;
      tf::transformTFToEigen(tf_gt_base,Tgt_base);
      tf::transformTFToEigen(tf_odom_base,Todom_base);

      /*     cout<<"velodyne pose=\n"<<Ttot.translation()<<endl;
      cout<<"sensor offset=\n"<<Ts.translation()<<endl;
      cout<<"GT translation=\n"<<Tgt.translation()<<endl;
      cout<<"odom translation=\n"<<Tbase.translation()<<endl;*/
      if(counter == 0){
        counter ++;
        cloud.clear();
        cloud_nofilter.clear();
        continue;
      }
      if(counter == 1){
        // Make the initialization of the pose to be at the sensory level...
        // Tgt.translation()[2] = 4.;//trans[2];
        fuser_pose=Tgt_base;
        Tgt_base_prev = Tgt_base;
        Todom_base_prev = Todom_base;
        fuser_=new GraphMapFuser(regParPtr,mapParPtr,graphParPtr,Tgt_base,sensor_offset);
        fuser_->Visualize(visualize);

        if (save_clouds)
          saveCloud(counter-1, cloud);

        counter ++;
        cloud.clear();
        cloud_nofilter.clear();
        continue;
      }


      Eigen::Affine3d Tmotion = Todom_base_prev.inverse()*Todom_base;
      Eigen::Vector3d Tmotion_euler = Tmotion.rotation().eulerAngles(0,1,2);
      ndt_generic::normalizeEulerAngles(Tmotion_euler);
      if(!use_odometry) {
        Tmotion.setIdentity();
      } else {

        if(Tmotion.translation().norm()<min_dist && Tmotion_euler.norm()<(min_rot_in_deg*M_PI/180.0)) {
          cloud.clear();
          cloud_nofilter.clear();
          continue;
        }
      }
      if (save_clouds) {
        saveCloud(counter-1, cloud);
      }

      counter++;
      fuser_pose=fuser_pose*Tmotion;

      if(visualize){
        br.sendTransform(tf::StampedTransform(tf_gt_base,ros::Time::now(), "/world", "/state_base_link"));
        cloud.header.frame_id="/velodyne";
        pcl_conversions::toPCL(ros::Time::now(), cloud.header.stamp);
        cloud_pub->publish(cloud);
        gt_pose_msg.header.stamp=ros::Time::now();
        tf::poseEigenToMsg(Tgt_base, gt_pose_msg.pose.pose);
        gt_pub->publish(gt_pose_msg);
        fuser_pose_msg.header.stamp=ros::Time::now();
        tf::poseEigenToMsg(fuser_pose, fuser_pose_msg.pose.pose);
        fuser_pub->publish(fuser_pose_msg);
      }
      fuser_->ProcessFrame(cloud,fuser_pose,Eigen::Affine3d::Identity());


      double diff = (fuser_pose.inverse() * Tgt_base).translation().norm();

      Tgt_base_prev = Tgt_base;
      Todom_base_prev = Todom_base;

      cloud.clear();
      cloud_nofilter.clear();

      // Evaluation
      // ROS_INFO_STREAM("Tgt : " << transformToEvalString(Tgt));
      // ROS_INFO_STREAM("Tbase : " << transformToEvalString(Tbase));
      // ROS_INFO_STREAM("Todo : " << transformToEvalString(Todo));
      //ROS_INFO_STREAM("diff : " << ndt_generic::affine3dToStringRPY(diff));

      //cout<<"diff norm="<<diff<<endl;

      ros::Time frame_time = vreader.getTimeStampOfLastSensorMsg();
      gt_file << frame_time << " " << transformToEvalString(Tgt_base);
      odom_file << frame_time << " " << transformToEvalString(Todom_base);
      est_file << frame_time << " " << transformToEvalString(fuser_pose);
      sensorpose_est_file << frame_time << " " << transformToEvalString(fuser_pose * sensor_offset);
    }
  }

  gt_file.close();
  odom_file.close();
  est_file.close();
  sensorpose_est_file.close();

  /* if (save_map) {
    std::cout << "Saving map" << std::endl;
    if (ndtslammer.wasInit() && ndtslammer.map != NULL) {
      ndtslammer.map->writeToJFF("map.jff");
      std::cout << "Done." << std::endl;
    }
    else {
      std::cout << "Failed to save map, ndtslammer was not initiated(!)" << std::endl;
    }
  }*/

  if (alive) {
    while (1) {
      usleep(1000);
    }
  }

  usleep(1000*1000);
  std::cout << "Done." << std::endl;
  // fclose(gtf);
  // fclose(fuserf);
  // fclose(frame2);
  // fclose(fgicp);
  // char c;
  // std::cin >> c;
}
