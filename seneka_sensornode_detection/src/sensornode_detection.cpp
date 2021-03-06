/**********************
* Author: Matthias Nösner
********************** */
#include <ros/ros.h>

#include <opencv/cv.h>
#include <opencv/highgui.h>
#include <cob_object_detection_msgs/DetectObjects.h>

#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>

#include <SerializeIO.h>

#include <boost/thread/mutex.hpp>
#include <boost/timer.hpp>

const double PI = 3.14159265359;

struct pose{
  double x;
  double y;
  double z;
};

struct quaternion{
  double w;
  double x;
  double y;
  double z;
};

struct pose3d{
  pose translation;
  quaternion rotation;
};

struct point4d{
  double x;
  double y;
  double z;
  double e;
  bool detected;
};

struct fiducialmarker{
  unsigned int id;
  cv::Point3d trans;
  cv::Point3d rot;
};

struct handle{
  cv::Point3d trans;
  cv::Point3d rot;
};

struct trigger_points{
  cv::Point3d up;
  cv::Point3d down;
};

using namespace std;

bool loadParameters(std::vector<fiducialmarker>*,std::vector<handle>*, trigger_points*, pose*);

//---------------------------<coordinate transformations>---------------------------------------
cv::Mat eulerToMatrix(double rot1, double rot2, double rot3);
void multiplyFrame7(std::vector<double> q1,std::vector<double> q2,std::vector<double>* q);
std::vector<double> FrameToVec7(const cv::Mat frame);
//---------------------------</coordinate transformations>---------------------------------------

std::vector<fiducialmarker> fiducialmarkers;
std::vector<handle> handles;
trigger_points trigger_offset;
pose grab_entry;
double gripper_length = 0.255; 
double gripper_depth = 0.00;
pose3d sensornode_pose;

boost::mutex tf_lock_;
ros::Timer tf_pub_timer_;
tf::StampedTransform marker_tf_;

//camera in quanjo system as vec7
tf::Quaternion qt = tf::createQuaternionFromRPY(-PI/2,0,-PI/2);
//double tmp[7] = { 0.935,0,0.452,qt.getW(),qt.getX(),qt.getY(),qt.getZ()};
double tmp[7] = { 1.05,0.015,0.452,qt.getW(),qt.getX(),qt.getY(),qt.getZ()};
std::vector<double> camera7(&tmp[0], &tmp[0]+7);

//------------------------------<Callbacks>----------------------------------------------------------
//The sensorsondeCoordinateManager manages and publishes all coordinate transform for the sensorsonde
//IN:Marker positions from cob_fiducials
//OUT:sensornode position, handles and trigger points for sensornode manipulation 
//--------
//TODO: Compute sensorsonde pose from all markers. (Right now only marker 1 is used)
void sensorsondeCoordinateManager(const cob_object_detection_msgs::DetectionArray::ConstPtr& msg)
{
  ROS_INFO("[sensornode_detection] I heard from  [%u] fiducial detections", (unsigned int)msg->detections.size());

  //dummy quanjo_position
  static tf::TransformBroadcaster br;
  tf::Transform transform;
  transform.setOrigin( tf::Vector3(0,0,0) );
  transform.setRotation( tf::createQuaternionFromRPY(0,0,0) );
  br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "/world_dummy_link", "/quanjo_body"));

  //Camera Position
  transform.setOrigin( tf::Vector3(camera7[0],camera7[1],camera7[2]) );
  transform.setRotation( tf::Quaternion(camera7[4],camera7[5],camera7[6],camera7[3]) );
  br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), "/quanjo_body", "/quanjo_camera"));

  pose origin;
  quaternion rotation;

  //Right now only the first detected marker is used
  //TODO: Make use of all detected markers
  if(msg->detections.size() > 0){

    origin.x = msg->detections[0].pose.pose.position.x;
    origin.y = msg->detections[0].pose.pose.position.y;
    origin.z = msg->detections[0].pose.pose.position.z;
    
    rotation.w = msg->detections[0].pose.pose.orientation.w;
    rotation.x = msg->detections[0].pose.pose.orientation.x;
    rotation.y = msg->detections[0].pose.pose.orientation.y;
    rotation.z = msg->detections[0].pose.pose.orientation.z;

    transform.setOrigin( tf::Vector3(origin.x,origin.y,origin.z) );
    transform.setRotation( tf::Quaternion(rotation.x,rotation.y,rotation.z,rotation.w));
    br.sendTransform(tf::StampedTransform(transform,  ros::Time::now(), "/quanjo_camera", "/seneka_marker"));

    //-> Transform each marker pose to object pose
    for(unsigned int i = 0; i < fiducialmarkers.size(); i++){
      //for loop j to iterate through container
      if(msg->detections[0].id == fiducialmarkers[i].id){

	//transformation matrices are named after the format tm_object_basesystem or q_object_basesystem
        //tm = transformation matrix, q = quaternion7
	//m = marker, sn = sensornode, hl = handle, ca = camera
	cv::Mat tm_m_sn = eulerToMatrix(fiducialmarkers[i].rot.x,fiducialmarkers[i].rot.y,fiducialmarkers[i].rot.z);
        cv::Mat tmp = cv::Mat(tm_m_sn.t());//transposed
	tm_m_sn = cv::Mat(3,4, CV_64FC1);
	//create homogenous coordinates
	for (int k=0; k<3; k++)
	  for (int l=0; l<3; l++)
	    tm_m_sn.at<double>(k,l) = tmp.at<double>(k,l);
	tm_m_sn.at<double>(0,3) = -fiducialmarkers[i].trans.x;
	tm_m_sn.at<double>(1,3) = -fiducialmarkers[i].trans.y;
	tm_m_sn.at<double>(2,3) = -fiducialmarkers[i].trans.z;

	std::vector<double> q_sn_m = FrameToVec7(tm_m_sn);	
	  
	transform.setOrigin( tf::Vector3(q_sn_m[0], q_sn_m[1], q_sn_m[2]));
	transform.setRotation( tf::Quaternion(q_sn_m[4],q_sn_m[5],q_sn_m[6],q_sn_m[3]));
	br.sendTransform(tf::StampedTransform(transform,  ros::Time::now(), "/seneka_marker", "/sensornode"));

	//!!not good to do it this way!!
	sensornode_pose.translation.x = q_sn_m[0];
	sensornode_pose.translation.y = q_sn_m[1];
	sensornode_pose.translation.z = q_sn_m[2];

	sensornode_pose.rotation.w = q_sn_m[3];
	sensornode_pose.rotation.x = q_sn_m[4];
	sensornode_pose.rotation.y = q_sn_m[5];
	sensornode_pose.rotation.z = q_sn_m[6];
      }      
    }

    //Do the coordinate transformations and publish handle position based on sensornode_pose
    for(unsigned int i = 0; i < handles.size(); i++){
            
      cv::Mat tm_hl_sn = eulerToMatrix( handles[i].rot.x, handles[i].rot.y, handles[i].rot.z);//3x3Matrix
      cv::Mat tmp = cv::Mat(tm_hl_sn);
      tm_hl_sn = cv::Mat(3,4, CV_64FC1);  
      
      //create homogenous coordinates
      for (int k=0; k<3; k++)
	for (int l=0; l<3; l++)
	  tm_hl_sn.at<double>(k,l) = tmp.at<double>(k,l);
      tm_hl_sn.at<double>(0,3) = handles[i].trans.x;
      tm_hl_sn.at<double>(1,3) = handles[i].trans.y;
      tm_hl_sn.at<double>(2,3) = handles[i].trans.z;

      std::vector<double> q_hl_sn = FrameToVec7(tm_hl_sn);

      //publish handle positions
      char handle_name[50];
      sprintf(handle_name,"/handle%u",i+1);
      transform.setOrigin( tf::Vector3(q_hl_sn[0], q_hl_sn[1], q_hl_sn[2]));
      transform.setRotation( tf::Quaternion(q_hl_sn[4],q_hl_sn[5],q_hl_sn[6],q_hl_sn[3]));
      br.sendTransform(tf::StampedTransform(transform,  ros::Time::now(), "/sensornode", handle_name));

      //publish trigger points and initial grabbing pose
      char trigger_name[50];      
      sprintf(trigger_name,"trigger_%u_up",i+1);
      transform.setOrigin( tf::Vector3(trigger_offset.up.x, trigger_offset.up.y, trigger_offset.up.z));
      transform.setRotation( tf::Quaternion(0,0,0,1));
      br.sendTransform(tf::StampedTransform(transform,  ros::Time::now(), handle_name, trigger_name));

      sprintf(trigger_name,"trigger_%u_down",i+1);
      transform.setOrigin( tf::Vector3(trigger_offset.down.x, trigger_offset.down.y, trigger_offset.down.z));
      transform.setRotation( tf::Quaternion(0,0,0,1));
      br.sendTransform(tf::StampedTransform(transform,  ros::Time::now(), handle_name, trigger_name));  

      sprintf(trigger_name,"grab_entry%u",i+1);
      transform.setOrigin( tf::Vector3(grab_entry.x, grab_entry.y, grab_entry.z));
      transform.setRotation( tf::Quaternion(0,0,0,1));
      br.sendTransform(tf::StampedTransform(transform,  ros::Time::now(), handle_name, trigger_name));       
    }
  }   
}
//------------------------------</Callbacks>----------------------------------------------------------


//Load Parameters positions of markers,handles and triggers in reference to the sensorsonde using my own SerializeIO class + scaling the values.
bool loadParameters(std::vector<fiducialmarker>* afiducialmarkers, std::vector<handle>* ahandles, trigger_points* atriggers, pose* aentry){
  
  SerializeIO *ser = new SerializeIO("/home/matthias/groovy_workspace/catkin_ws/src/seneka_deployment_unit/seneka_sensornode_detection/launch/sensorsonde_coordinates.def",'i');
	
  fiducialmarker fiducial1, fiducial2, fiducial3, fiducial4, fiducial5, fiducial6;
  handle handle1, handle2, handle3, handle4, handle5, handle6;
  trigger_points triggers;
	

  double scale = 0.001;//from mm to m

  //Warning: Fiducial id must be identical with the id('s) from cob_fiducials!!
  //Warning: You are responsible for that!!
  if(!ser->readVariable("fiducial1_ID",&(fiducial1.id))) return false;
  if(!ser->readVariable("fiducial1_POSX",&(fiducial1.trans.x))) return false;
  if(!ser->readVariable("fiducial1_POSY",&(fiducial1.trans.y))) return false;
  if(!ser->readVariable("fiducial1_POSZ",&(fiducial1.trans.z))) return false;
  if(!ser->readVariable("fiducial1_ROT1",&(fiducial1.rot.x))) return false;
  if(!ser->readVariable("fiducial1_ROT2",&(fiducial1.rot.y))) return false;
  if(!ser->readVariable("fiducial1_ROT3",&(fiducial1.rot.z))) return false;

  if(!ser->readVariable("fiducial2_ID",&(fiducial2.id))) return false;
  if(!ser->readVariable("fiducial2_POSX",&(fiducial2.trans.x))) return false;
  if(!ser->readVariable("fiducial2_POSY",&(fiducial2.trans.y))) return false;
  if(!ser->readVariable("fiducial2_POSZ",&(fiducial2.trans.z))) return false;
  if(!ser->readVariable("fiducial2_ROT1",&(fiducial2.rot.x))) return false;
  if(!ser->readVariable("fiducial2_ROT2",&(fiducial2.rot.y))) return false;
  if(!ser->readVariable("fiducial2_ROT3",&(fiducial2.rot.z))) return false;

  if(!ser->readVariable("fiducial3_ID",&(fiducial3.id))) return false;
  if(!ser->readVariable("fiducial3_POSX",&(fiducial3.trans.x))) return false;
  if(!ser->readVariable("fiducial3_POSY",&(fiducial3.trans.y))) return false;
  if(!ser->readVariable("fiducial3_POSZ",&(fiducial3.trans.z))) return false;
  if(!ser->readVariable("fiducial3_ROT1",&(fiducial3.rot.x))) return false;
  if(!ser->readVariable("fiducial3_ROT2",&(fiducial3.rot.y))) return false;
  if(!ser->readVariable("fiducial3_ROT3",&(fiducial3.rot.z))) return false;

  if(!ser->readVariable("fiducial4_ID",&(fiducial4.id))) return false;
  if(!ser->readVariable("fiducial4_POSX",&(fiducial4.trans.x))) return false;
  if(!ser->readVariable("fiducial4_POSY",&(fiducial4.trans.y))) return false;
  if(!ser->readVariable("fiducial4_POSZ",&(fiducial4.trans.z))) return false;
  if(!ser->readVariable("fiducial4_ROT1",&(fiducial4.rot.x))) return false;
  if(!ser->readVariable("fiducial4_ROT2",&(fiducial4.rot.y))) return false;
  if(!ser->readVariable("fiducial4_ROT3",&(fiducial4.rot.z))) return false;

  if(!ser->readVariable("fiducial5_ID",&(fiducial5.id))) return false;
  if(!ser->readVariable("fiducial5_POSX",&(fiducial5.trans.x))) return false;
  if(!ser->readVariable("fiducial5_POSY",&(fiducial5.trans.y))) return false;
  if(!ser->readVariable("fiducial5_POSZ",&(fiducial5.trans.z))) return false;
  if(!ser->readVariable("fiducial5_ROT1",&(fiducial5.rot.x))) return false;
  if(!ser->readVariable("fiducial5_ROT2",&(fiducial5.rot.y))) return false;
  if(!ser->readVariable("fiducial5_ROT3",&(fiducial5.rot.z))) return false;

  if(!ser->readVariable("fiducial6_ID",&(fiducial6.id))) return false;
  if(!ser->readVariable("fiducial6_POSX",&(fiducial6.trans.x))) return false;
  if(!ser->readVariable("fiducial6_POSY",&(fiducial6.trans.y))) return false;
  if(!ser->readVariable("fiducial6_POSZ",&(fiducial6.trans.z))) return false;
  if(!ser->readVariable("fiducial6_ROT1",&(fiducial6.rot.x))) return false;
  if(!ser->readVariable("fiducial6_ROT2",&(fiducial6.rot.y))) return false;
  if(!ser->readVariable("fiducial6_ROT3",&(fiducial6.rot.z))) return false;	

  //--- Read handle pose ---
  if(!ser->readVariable("handle1_x",&(handle1.trans.x))) return false;
  if(!ser->readVariable("handle1_y",&(handle1.trans.y))) return false;
  if(!ser->readVariable("handle1_z",&(handle1.trans.z))) return false;
  if(!ser->readVariable("handle1_roll",&(handle1.rot.x))) return false;
  if(!ser->readVariable("handle1_pitch",&(handle1.rot.y))) return false;
  if(!ser->readVariable("handle1_yaw",&(handle1.rot.z))) return false;

  if(!ser->readVariable("handle2_x",&(handle2.trans.x))) return false;
  if(!ser->readVariable("handle2_y",&(handle2.trans.y))) return false;
  if(!ser->readVariable("handle2_z",&(handle2.trans.z))) return false;
  if(!ser->readVariable("handle2_roll",&(handle2.rot.x))) return false;
  if(!ser->readVariable("handle2_pitch",&(handle2.rot.y))) return false;
  if(!ser->readVariable("handle2_yaw",&(handle2.rot.z))) return false;

  if(!ser->readVariable("handle3_x",&(handle3.trans.x))) return false;
  if(!ser->readVariable("handle3_y",&(handle3.trans.y))) return false;
  if(!ser->readVariable("handle3_z",&(handle3.trans.z))) return false;
  if(!ser->readVariable("handle3_roll",&(handle3.rot.x))) return false;
  if(!ser->readVariable("handle3_pitch",&(handle3.rot.y))) return false;
  if(!ser->readVariable("handle3_yaw",&(handle3.rot.z))) return false;

  if(!ser->readVariable("handle4_x",&(handle4.trans.x))) return false;
  if(!ser->readVariable("handle4_y",&(handle4.trans.y))) return false;
  if(!ser->readVariable("handle4_z",&(handle4.trans.z))) return false;
  if(!ser->readVariable("handle4_roll",&(handle4.rot.x))) return false;
  if(!ser->readVariable("handle4_pitch",&(handle4.rot.y))) return false;
  if(!ser->readVariable("handle4_yaw",&(handle4.rot.z))) return false;

  if(!ser->readVariable("handle5_x",&(handle5.trans.x))) return false;
  if(!ser->readVariable("handle5_y",&(handle5.trans.y))) return false;
  if(!ser->readVariable("handle5_z",&(handle5.trans.z))) return false;
  if(!ser->readVariable("handle5_roll",&(handle5.rot.x))) return false;
  if(!ser->readVariable("handle5_pitch",&(handle5.rot.y))) return false;
  if(!ser->readVariable("handle5_yaw",&(handle5.rot.z))) return false;

  if(!ser->readVariable("handle6_x",&(handle6.trans.x))) return false;
  if(!ser->readVariable("handle6_y",&(handle6.trans.y))) return false;
  if(!ser->readVariable("handle6_z",&(handle6.trans.z))) return false;
  if(!ser->readVariable("handle6_roll",&(handle6.rot.x))) return false;
  if(!ser->readVariable("handle6_pitch",&(handle6.rot.y))) return false;
  if(!ser->readVariable("handle6_yaw",&(handle6.rot.z))) return false;

  //--- Read trigger offset ---
  if(!ser->readVariable("trigger_up_offset_x",&(triggers.up.x))) return false;
  if(!ser->readVariable("trigger_up_offset_y",&(triggers.up.y))) return false;
  if(!ser->readVariable("trigger_up_offset_z",&(triggers.up.z))) return false;

  if(!ser->readVariable("trigger_down_offset_x",&(triggers.down.x))) return false;
  if(!ser->readVariable("trigger_down_offset_y",&(triggers.down.y))) return false;
  if(!ser->readVariable("trigger_down_offset_z",&(triggers.down.z))) return false;

  if(!ser->readVariable("grab_entry_x",&(grab_entry.x))) return false;
  if(!ser->readVariable("grab_entry_y",&(grab_entry.y))) return false;
  if(!ser->readVariable("grab_entry_z",&(grab_entry.z))) return false;


  afiducialmarkers->push_back(fiducial1);
  afiducialmarkers->push_back(fiducial2);
  afiducialmarkers->push_back(fiducial3);
  afiducialmarkers->push_back(fiducial4);
  afiducialmarkers->push_back(fiducial5);
  afiducialmarkers->push_back(fiducial6);

  ahandles->push_back(handle1);
  ahandles->push_back(handle2);
  ahandles->push_back(handle3);
  ahandles->push_back(handle4);
  ahandles->push_back(handle5);
  ahandles->push_back(handle6);

  *atriggers = triggers;

  //Scale to meter
  for(unsigned int i = 0; i < afiducialmarkers->size(); i++){
	  
    (*afiducialmarkers)[i].trans.x =  (*afiducialmarkers)[i].trans.x * scale;
    (*afiducialmarkers)[i].trans.y =  (*afiducialmarkers)[i].trans.y * scale;
    (*afiducialmarkers)[i].trans.z =  (*afiducialmarkers)[i].trans.z * scale;
  }
  for(unsigned int i = 0; i < ahandles->size(); i++){
	  
    (*ahandles)[i].trans.x =  (*ahandles)[i].trans.x * scale;
    (*ahandles)[i].trans.y =  (*ahandles)[i].trans.y * scale;
    (*ahandles)[i].trans.z =  (*ahandles)[i].trans.z * scale;
  }
  atriggers->up.x = atriggers->up.x * scale + gripper_depth;
  atriggers->up.y = atriggers->up.y * scale;
  atriggers->up.z = atriggers->up.z * scale + gripper_length;
  atriggers->down.x = atriggers->down.x * scale + gripper_depth;
  atriggers->down.y = atriggers->down.y * scale;
  atriggers->down.z = (atriggers->down.z * scale) + gripper_length;
  aentry->x = aentry->x * scale + gripper_depth;
  aentry->y = aentry->y * scale;
  aentry->z = aentry->z * scale + gripper_length;

  ser->close();
  delete ser;
  return true;
}


//------------------------Coordinate Transformations----------------------------------------------------------------------------------
//XYZ (Roll,Pitch,Yaw) Euler System to Matrix representation
cv::Mat eulerToMatrix(double rot1, double rot2, double rot3) {
  
  cv::Mat m(3,3,CV_64FC1);
	
  cv::Mat X_rot(3,3,CV_64F);
  cv::Mat Y_rot(3,3,CV_64F);
  cv::Mat Z_rot(3,3,CV_64F);
	
  double alpha =  rot1;
  double beta  =  rot2;
  double gamma =  rot3;
	
  X_rot.at<double>(0,0) =  1.0;
  X_rot.at<double>(0,1) =  0.0;
  X_rot.at<double>(0,2) =  0.0;
  X_rot.at<double>(1,0) =  0.0;
  X_rot.at<double>(1,1) =  cos(alpha);
  X_rot.at<double>(1,2) =  -sin(alpha);
  X_rot.at<double>(2,0) =  0.0;
  X_rot.at<double>(2,1) =  sin(alpha);
  X_rot.at<double>(2,2) =  cos(alpha);
	
  Y_rot.at<double>(0,0) =  cos(beta);
  Y_rot.at<double>(0,1) =  0.0;
  Y_rot.at<double>(0,2) =  sin(beta);
  Y_rot.at<double>(1,0) =  0.0;
  Y_rot.at<double>(1,1) =  1.0;
  Y_rot.at<double>(1,2) =  0.0;
  Y_rot.at<double>(2,0) =  -sin(beta);
  Y_rot.at<double>(2,1) =  0.0;
  Y_rot.at<double>(2,2) =  cos(beta);
	
  Z_rot.at<double>(0,0) =  cos(gamma);
  Z_rot.at<double>(0,1) =  -sin(gamma);
  Z_rot.at<double>(0,2) =  0.0;
  Z_rot.at<double>(1,0) =  sin(gamma);
  Z_rot.at<double>(1,1) =  cos(gamma);
  Z_rot.at<double>(1,2) =  0.0;
  Z_rot.at<double>(2,0) =  0.0;
  Z_rot.at<double>(2,1) =  0.0;
  Z_rot.at<double>(2,2) =  1.0;
	
  m = Z_rot*Y_rot*X_rot;
	
  return m;
}

// Function copied from cob_vision_ipa_utils/MathUtils.h to avoid dependency
inline float SIGN(float x)
{
  return (x >= 0.0f) ? +1.0f : -1.0f;
}

// Function copied from cob_vision_ipa_utils/MathUtils.h to avoid dependency
std::vector<double> FrameToVec7(const cv::Mat frame){

  // [0]-[2]: translation xyz
  // [3]-[6]: quaternion wxyz
  std::vector<double> pose(7, 0.0);

  double r11 = frame.at<double>(0,0);
  double r12 = frame.at<double>(0,1);
  double r13 = frame.at<double>(0,2);
  double r21 = frame.at<double>(1,0);
  double r22 = frame.at<double>(1,1);
  double r23 = frame.at<double>(1,2);
  double r31 = frame.at<double>(2,0);
  double r32 = frame.at<double>(2,1);
  double r33 = frame.at<double>(2,2);

  double qw = ( r11 + r22 + r33 + 1.0) / 4.0;
  double qx = ( r11 - r22 - r33 + 1.0) / 4.0;
  double qy = (-r11 + r22 - r33 + 1.0) / 4.0;
  double qz = (-r11 - r22 + r33 + 1.0) / 4.0;
  if(qw < 0.0f) qw = 0.0;
  if(qx < 0.0f) qx = 0.0;
  if(qy < 0.0f) qy = 0.0;
  if(qz < 0.0f) qz = 0.0;
  qw = std::sqrt(qw);
  qx = std::sqrt(qx);
  qy = std::sqrt(qy);
  qz = std::sqrt(qz);
  if(qw >= qx && qw >= qy && qw >= qz)
    {
      qw *= +1.0;
      qx *= SIGN(r32 - r23);
      qy *= SIGN(r13 - r31);
      qz *= SIGN(r21 - r12);
    }
  else if(qx >= qw && qx >= qy && qx >= qz)
    {
      qw *= SIGN(r32 - r23);
      qx *= +1.0;
      qy *= SIGN(r21 + r12);
      qz *= SIGN(r13 + r31);
    }
  else if(qy >= qw && qy >= qx && qy >= qz)
    {
      qw *= SIGN(r13 - r31);
      qx *= SIGN(r21 + r12);
      qy *= +1.0;
      qz *= SIGN(r32 + r23);
    }
  else if(qz >= qw && qz >= qx && qz >= qy)
    {
      qw *= SIGN(r21 - r12);
      qx *= SIGN(r31 + r13);
      qy *= SIGN(r32 + r23);
      qz *= +1.0;
    }
  else
    {
      printf("coding error\n");
    }
  double r = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz);
  qw /= r;
  qx /= r;
  qy /= r;
  qz /= r;

  pose[3] = qw;
  pose[4] = qx;
  pose[5] = qy;
  pose[6] = qz;

  // Translation
  pose[0] = frame.at<double>(0,3);
  pose[1] = frame.at<double>(1,3);
  pose[2] = frame.at<double>(2,3);
  return pose;
}


/** Quaternion multiplication
 *  It's not used right now and it's not tested!
 */
void multiplyFrame7(std::vector<double> q1,std::vector<double> q2, std::vector<double>* q)
{
  // First quaternion q1 (x1 y1 z1 r1)
  double x1=q1[4];
  double y1=q1[5];
  double z1=q1[6];
  double r1=q1[3];

  // Second quaternion q2 (x2 y2 z2 r2)
  double x2=q2[4];
  double y2=q2[5];
  double z2=q2[6];
  double r2=q2[3];

  //translation
  q->push_back(q1[0]+q2[0]); 
  q->push_back(q1[1]+q2[1]);
  q->push_back(q1[2]+q2[2]);

  //quaternion
  q->push_back(r1*r2 - x1*x2 - y1*y2 - z1*z2);   // r component
  q->push_back(x1*r2 + r1*x2 + y1*z2 - z1*y2);   // x component
  q->push_back(r1*y2 - x1*z2 + y1*r2 + z1*x2);   // y component
  q->push_back(r1*z2 + x1*y2 - y1*x2 + z1*r2);   // z component
}

//-----------------------------------main----------------------------------------------------------------------------
int main( int argc, char** argv )
{  
  ros::init(argc, argv, "sensornode_detection");
  ros::NodeHandle node;
 
  ros::Subscriber sub = node.subscribe("/fiducials/fiducial_detection_array", 1, sensorsondeCoordinateManager);
 
  if(!loadParameters(&fiducialmarkers,&handles,&trigger_offset,&grab_entry)){
    ROS_ERROR("Failed to load parameters. Check the path...");
    return 0;
  } else {
    ROS_INFO("Congratulations! All parameters succesfully loaded :)!!.");
  }
 
  ros::spin();
  return 0;
}

