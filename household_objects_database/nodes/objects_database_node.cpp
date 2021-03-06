/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2009, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

// Author(s): Matei Ciocarlie

//! Wraps around the most common functionality of the objects database and offers it
//! as ROS services

#include <vector>
#include <boost/shared_ptr.hpp>

#include <ros/ros.h>

#include <tf/transform_datatypes.h>
#include <tf/transform_listener.h>

#include <object_manipulation_msgs/GraspPlanning.h>

#include <household_objects_database_msgs/GetModelList.h>
#include <household_objects_database_msgs/GetModelMesh.h>
#include <household_objects_database_msgs/GetModelDescription.h>
#include <household_objects_database_msgs/GetModelScans.h>
#include <household_objects_database_msgs/DatabaseScan.h>
#include <household_objects_database_msgs/SaveScan.h>

#include "household_objects_database/objects_database.h"

const std::string GET_MODELS_SERVICE_NAME = "get_model_list";
const std::string GET_MESH_SERVICE_NAME = "get_model_mesh";
const std::string GET_DESCRIPTION_SERVICE_NAME = "get_model_description";
const std::string GRASP_PLANNING_SERVICE_NAME = "database_grasp_planning";
const std::string GET_SCANS_SERVICE_NAME = "get_model_scans";
const std::string SAVE_SCAN_SERVICE_NAME = "save_model_scan";

using namespace household_objects_database_msgs;
using namespace household_objects_database;
using namespace object_manipulation_msgs;

//! Retrieves hand description info from the parameter server
/*! Duplicated from object_manipulator to avoid an additional dependency */
class HandDescription
{
 private:
  //! Node handle in the root namespace
  ros::NodeHandle root_nh_;

  inline std::string getStringParam(std::string name)
  {
    std::string value;
    if (!root_nh_.getParamCached(name, value))
    {
      ROS_ERROR("Hand description: could not find parameter %s", name.c_str());
    }
    //ROS_INFO_STREAM("Hand description param " << name << " resolved to " << value);
    return value;
  }

  inline std::vector<std::string> getVectorParam(std::string name)
  {
    std::vector<std::string> values;
    XmlRpc::XmlRpcValue list;
    if (!root_nh_.getParamCached(name, list)) 
    {
      ROS_ERROR("Hand description: could not find parameter %s", name.c_str());
    }
    if (list.getType() != XmlRpc::XmlRpcValue::TypeArray)
    {
      ROS_ERROR("Hand description: bad parameter %s", name.c_str());
    }
    //ROS_INFO_STREAM("Hand description vector param " << name << " resolved to:");
    for (int32_t i=0; i<list.size(); i++)
    {
      if (list[i].getType() != XmlRpc::XmlRpcValue::TypeString)
      {
	ROS_ERROR("Hand description: bad parameter %s", name.c_str());
      }
      values.push_back( static_cast<std::string>(list[i]) );
      //ROS_INFO_STREAM("  " << values.back());
    }
    return values;	
  }

 public:
 HandDescription() : root_nh_("~") {}
  
  inline std::string handDatabaseName(std::string arm_name)
  {
    return getStringParam("/hand_description/" + arm_name + "/hand_database_name");
  }
  
  inline std::vector<std::string> handJointNames(std::string arm_name)
  {
    return getVectorParam("/hand_description/" + arm_name + "/hand_joints");
  }
  
};

//! Wraps around database connection to provide database-related services through ROS
/*! Contains very thin wrappers for getting a list of scaled models and for getting the mesh
  of a model, as well as a complete server for the grasp planning service */
class ObjectsDatabaseNode
{
private:
  //! Node handle in the private namespace
  ros::NodeHandle priv_nh_;

  //! Node handle in the root namespace
  ros::NodeHandle root_nh_;

  //! Server for the get models service
  ros::ServiceServer get_models_srv_;

  //! Server for the get mesh service
  ros::ServiceServer get_mesh_srv_;

  //! Server for the get description service
  ros::ServiceServer get_description_srv_;

  //! Server for the get grasps service
  ros::ServiceServer grasp_planning_srv_;

  //! Server for the get scans service
  ros::ServiceServer get_scans_srv_;

  //! Server for the save scan service
  ros::ServiceServer save_scan_srv_;

  //! The database connection itself
  ObjectsDatabase *database_;

  //! Transform listener
  tf::TransformListener listener_;

  //! Threshold for pruning grasps based on gripper opening
  double prune_gripper_opening_;

  //! Threshold for pruning grasps based on table clearance
  double prune_table_clearance_;

  //! Callback for the get models service
  bool getModelsCB(GetModelList::Request &request, GetModelList::Response &response)
  {
    if (!database_)
    {
      response.return_code.code = response.return_code.DATABASE_NOT_CONNECTED;
      return true;
    }
    std::vector< boost::shared_ptr<DatabaseScaledModel> > models;
    if (!database_->getScaledModelsBySet(models, request.model_set))
    {
      response.return_code.code = response.return_code.DATABASE_QUERY_ERROR;
      return true;
    }
    for (size_t i=0; i<models.size(); i++)
    {
      response.model_ids.push_back( models[i]->id_.data() );
    }
    response.return_code.code = response.return_code.SUCCESS;
    return true;
  }

  //! Callback for the get mesh service
  bool getMeshCB(GetModelMesh::Request &request, GetModelMesh::Response &response)
  {
    if (!database_)
    {
      response.return_code.code = response.return_code.DATABASE_NOT_CONNECTED;
      return true;
    }
    if ( !database_->getScaledModelMesh(request.model_id, response.mesh) )
    {
      response.return_code.code = response.return_code.DATABASE_QUERY_ERROR;
      return true;
    }
    response.return_code.code = response.return_code.SUCCESS;
    return true;
  }

  //! Callback for the get description service
  bool getDescriptionCB(GetModelDescription::Request &request, GetModelDescription::Response &response)
  {
    if (!database_)
    {
      response.return_code.code = response.return_code.DATABASE_NOT_CONNECTED;
      return true;
    }
    std::vector< boost::shared_ptr<DatabaseScaledModel> > models;
    
    std::stringstream id;
    id << request.model_id;
    std::string where_clause("scaled_model_id=" + id.str());
    if (!database_->getList(models, where_clause) || models.size() != 1)
    {
      response.return_code.code = response.return_code.DATABASE_QUERY_ERROR;
      return true;
    }
    response.tags = models[0]->tags_.data();
    response.name = models[0]->model_.data();
    response.maker = models[0]->maker_.data();
    response.return_code.code = response.return_code.SUCCESS;
    return true;
  }

  bool getScansCB(GetModelScans::Request &request, GetModelScans::Response &response)
  {
    if (!database_)
    {
      ROS_ERROR("GetModelScan: database not connected");
      response.return_code.code = DatabaseReturnCode::DATABASE_NOT_CONNECTED;
      return true;
    }

    database_->getModelScans(request.model_id, request.scan_source,response.matching_scans);
    response.return_code.code = DatabaseReturnCode::SUCCESS;
    return true;
  }

  bool saveScanCB(SaveScan::Request &request, SaveScan::Response &response)
  {
    if (!database_)
    {
      ROS_ERROR("SaveScan: database not connected");
      response.return_code.code = DatabaseReturnCode::DATABASE_NOT_CONNECTED;
      return true;
    }
    household_objects_database::DatabaseScan scan;
    scan.frame_id_.get() = request.ground_truth_pose.header.frame_id;
    scan.cloud_topic_.get() = request.cloud_topic;
    scan.object_pose_.get().pose_ = request.ground_truth_pose.pose;
    scan.scaled_model_id_.get() = request.scaled_model_id;
    scan.scan_bagfile_location_.get() = request.bagfile_location;
    scan.scan_source_.get() = request.scan_source;
    database_->insertIntoDatabase(&scan);
    response.return_code.code = DatabaseReturnCode::SUCCESS;
    return true;
  }

  //! Prune grasps that require gripper to be open all the way, or that are marked in db as colliding with table
  /*! Use negative value for table_clearance_threshold if no clearing should be done
    based on table clearance.
  */
  virtual void pruneGraspList(std::vector< boost::shared_ptr<DatabaseGrasp> > &grasps,
			      double gripper_threshold, 
			      double table_clearance_threshold)
  {
    std::vector< boost::shared_ptr<DatabaseGrasp> >::iterator prune_it = grasps.begin();
    int pruned = 0;
    while ( prune_it != grasps.end() )
    {
      /*
      //by mistake, table clearance in database is currently in mm
      if ((*prune_it)->final_grasp_posture_.get().joint_angles_[0] > gripper_threshold ||
	  (table_clearance_threshold >= 0.0 && (*prune_it)->table_clearance_.get() < table_clearance_threshold*1.0e3) ) */
      if ((*prune_it)->quality_.get() >= -40)
      {
	prune_it = grasps.erase(prune_it);
	pruned++;
      } 
      else 
      {
	prune_it++;
      }
    }
    ROS_INFO("Database grasp planner: pruned %d grasps for table collision or gripper angle above threshold", pruned);
  }

  geometry_msgs::Pose multiplyPoses(const geometry_msgs::Pose &p1, 
                                    const geometry_msgs::Pose &p2)
  {
    tf::Transform t1;
    tf::poseMsgToTF(p1, t1);
    tf::Transform t2;
    tf::poseMsgToTF(p2, t2);
    t2 = t1 * t2;        
    geometry_msgs::Pose out_pose;
    tf::poseTFToMsg(t2, out_pose);
    return out_pose;
  }

  //! Callback for the get grasps service
  bool graspPlanningCB(GraspPlanning::Request &request, GraspPlanning::Response &response)
  {
    if (!database_)
    {
      ROS_ERROR("Database grasp planning: database not connected");
      response.error_code.value = response.error_code.OTHER_ERROR;
      return true;
    }

    if (request.target.potential_models.empty())
    {
      ROS_ERROR("Database grasp planning: no potential model information in grasp planning target");
      response.error_code.value = response.error_code.OTHER_ERROR;
      return true;      
    }

    if (request.target.potential_models.size() > 1)
    {
      ROS_WARN("Database grasp planning: target has more than one potential models. "
               "Returning grasps for first model only");
    }

    HandDescription hd;
    int model_id = request.target.potential_models[0].model_id;
    std::string hand_id = hd.handDatabaseName(request.arm_name);
    
    //retrieve the raw grasps from the database
    std::vector< boost::shared_ptr<DatabaseGrasp> > grasps;
    if (!database_->getClusterRepGrasps(model_id, hand_id, grasps))
    {
      ROS_ERROR("Database grasp planning: database query error");
      response.error_code.value = response.error_code.OTHER_ERROR;
      return true;
    }
    ROS_INFO("Database object node: retrieved %u grasps from database", (unsigned int)grasps.size());
    
    //prune the retrieved grasps
    pruneGraspList(grasps, prune_gripper_opening_, prune_table_clearance_);

    //convert to the Grasp data type
    std::vector< boost::shared_ptr<DatabaseGrasp> >::iterator it;
    for (it = grasps.begin(); it != grasps.end(); it++)
    {
      ROS_ASSERT( (*it)->final_grasp_posture_.get().joint_angles_.size() == 
		  (*it)->pre_grasp_posture_.get().joint_angles_.size() );
      Grasp grasp;
      std::vector<std::string> joint_names = hd.handJointNames(request.arm_name);

      if (hand_id == "Schunk")
      {
        if (joint_names.size() != 7 )
        {
           ROS_ERROR("Hardcoded Schunk hand expects to have 7 joints");
           continue;
        }
        if ((*it)->final_grasp_posture_.get().joint_angles_.size() != 8)
        {
		ROS_ERROR("Hardcoded database model of Schunk hand expected to have 8 joints");
		continue;
	}
	
	grasp.pre_grasp_posture.name = joint_names;
        grasp.pre_grasp_posture.position.resize(7);
	grasp.pre_grasp_posture.position[0] = std::min(1.5707, std::max( (*it)->pre_grasp_posture_.get().joint_angles_[0], 0.0) );
	grasp.pre_grasp_posture.position[1] = std::min(1.5707, std::max( (*it)->pre_grasp_posture_.get().joint_angles_[6], -1.5707) );
	grasp.pre_grasp_posture.position[2] = std::min(1.5707, std::max( (*it)->pre_grasp_posture_.get().joint_angles_[7], -1.5707) );
	grasp.pre_grasp_posture.position[3] = std::min(1.5707, std::max( (*it)->pre_grasp_posture_.get().joint_angles_[1], -1.5707) );
	grasp.pre_grasp_posture.position[4] = std::min(1.5707, std::max( (*it)->pre_grasp_posture_.get().joint_angles_[2], -1.5707) );
	grasp.pre_grasp_posture.position[5] = std::min(1.5707, std::max( (*it)->pre_grasp_posture_.get().joint_angles_[3], -1.5707) );
	grasp.pre_grasp_posture.position[6] = std::min(1.5707, std::max( (*it)->pre_grasp_posture_.get().joint_angles_[4], -1.5707) );

	grasp.grasp_posture.name = joint_names;
        grasp.grasp_posture.position.resize(7);
	grasp.grasp_posture.position[0] = std::min(1.5707, std::max( (*it)->final_grasp_posture_.get().joint_angles_[0], 0.0) );
	grasp.grasp_posture.position[1] = std::min(1.5707, std::max( (*it)->final_grasp_posture_.get().joint_angles_[6], -1.5707) );
	grasp.grasp_posture.position[2] = std::min(1.5707, std::max( (*it)->final_grasp_posture_.get().joint_angles_[7], -1.5707) );
	grasp.grasp_posture.position[3] = std::min(1.5707, std::max( (*it)->final_grasp_posture_.get().joint_angles_[1], -1.5707) );
	grasp.grasp_posture.position[4] = std::min(1.5707, std::max( (*it)->final_grasp_posture_.get().joint_angles_[2], -1.5707) );
	grasp.grasp_posture.position[5] = std::min(1.5707, std::max( (*it)->final_grasp_posture_.get().joint_angles_[3], -1.5707) );
	grasp.grasp_posture.position[6] = std::min(1.5707, std::max( (*it)->final_grasp_posture_.get().joint_angles_[4], -1.5707) );
      }
      else if (hand_id != "WILLOW_GRIPPER_2010")
      {
	//check that the number of joints in the ROS description of this hand
	//matches the number of values we have in the database
	if (joint_names.size() != (*it)->final_grasp_posture_.get().joint_angles_.size())
	{
	  ROS_ERROR("Database grasp specification does not match ROS description of hand. "
		    "Hand is expected to have %d joints, but database grasp specifies %d values", 
		    (int)joint_names.size(), (int)(*it)->final_grasp_posture_.get().joint_angles_.size());
	  continue;
	}
	//for now we silently assume that the order of the joints in the ROS description of
	//the hand is the same as in the database description
	grasp.pre_grasp_posture.name = joint_names;
	grasp.grasp_posture.name = joint_names;
	grasp.pre_grasp_posture.position = (*it)->pre_grasp_posture_.get().joint_angles_;
	grasp.grasp_posture.position = (*it)->final_grasp_posture_.get().joint_angles_;	
      }
      else
      {
	//unfortunately we have to hack this, as the grasp is really defined by a single
	//DOF, but the urdf for the PR2 gripper is not well set up to do that
	if ( joint_names.size() != 4 || (*it)->final_grasp_posture_.get().joint_angles_.size() != 1)
	{
	  ROS_ERROR("PR2 gripper specs and database grasp specs do not match expected values");
	  continue;
	}
	grasp.pre_grasp_posture.name = joint_names;
	grasp.grasp_posture.name = joint_names;
	//replicate the single value from the database 4 times
	grasp.pre_grasp_posture.position.resize( joint_names.size(), 
						 (*it)->pre_grasp_posture_.get().joint_angles_.at(0));
	grasp.grasp_posture.position.resize( joint_names.size(), 
					     (*it)->final_grasp_posture_.get().joint_angles_.at(0));
      }
      //for now the effort is not in the database so we hard-code it in here
      //this will change at some point
      grasp.grasp_posture.effort.resize(joint_names.size(), 50);
      grasp.pre_grasp_posture.effort.resize(joint_names.size(), 100);
      //min and desired approach distances are the same for all grasps
      grasp.desired_approach_distance = 0.15;
      grasp.min_approach_distance = 0.07;
      //the pose of the grasp
      grasp.grasp_pose = (*it)->final_grasp_pose_.get().pose_;
      //convert it to the frame of the detection
      grasp.grasp_pose = multiplyPoses(request.target.potential_models[0].pose.pose, grasp.grasp_pose);
      //and then finally to the reference frame of the object
      if (request.target.potential_models[0].pose.header.frame_id !=
          request.target.reference_frame_id)
      {
        tf::StampedTransform ref_trans;
        try
        {
          listener_.lookupTransform(request.target.reference_frame_id,
                                    request.target.potential_models[0].pose.header.frame_id,                    
                                    ros::Time(0), ref_trans);
        }
        catch (tf::TransformException ex)
        {
          ROS_ERROR("Grasp planner: failed to get transform from %s to %s; exception: %s", 
                    request.target.reference_frame_id.c_str(), 
                    request.target.potential_models[0].pose.header.frame_id.c_str(), ex.what());
          response.error_code.value = response.error_code.OTHER_ERROR;
          return true;      
        }        
        geometry_msgs::Pose ref_pose;
        tf::poseTFToMsg(ref_trans, ref_pose);
        grasp.grasp_pose = multiplyPoses(ref_pose, grasp.grasp_pose);
      }
	  //stick the scaled quality into the success_probability field
	  grasp.success_probability = (*it)->scaled_quality_.get();

      //insert the new grasp in the list
      response.grasps.push_back(grasp);
    }

    ROS_INFO("Database grasp planner: returning %u grasps", (unsigned int)response.grasps.size());
    response.error_code.value = response.error_code.SUCCESS;
    return true;
  }

public:
  ObjectsDatabaseNode() : priv_nh_("~"), root_nh_("")
  {
    //initialize database connection
    std::string database_host, database_port, database_user, database_pass, database_name;
    root_nh_.param<std::string>("/household_objects_database/database_host", database_host, "");
    int port_int;
    root_nh_.param<int>("/household_objects_database/database_port", port_int, -1);
    std::stringstream ss; ss << port_int; database_port = ss.str();
    root_nh_.param<std::string>("/household_objects_database/database_user", database_user, "");
    root_nh_.param<std::string>("/household_objects_database/database_pass", database_pass, "");
    root_nh_.param<std::string>("/household_objects_database/database_name", database_name, "");
    database_ = new ObjectsDatabase(database_host, database_port, database_user, database_pass, database_name);
    if (!database_->isConnected())
    {
      ROS_ERROR("ObjectsDatabaseNode: failed to open model database on host "
		"%s, port %s, user %s with password %s, database %s. Unable to do grasp "
		"planning on database recognized objects. Exiting.",
		database_host.c_str(), database_port.c_str(), 
		database_user.c_str(), database_pass.c_str(), database_name.c_str());
      delete database_; database_ = NULL;
    }

    //advertise services
    get_models_srv_ = priv_nh_.advertiseService(GET_MODELS_SERVICE_NAME, &ObjectsDatabaseNode::getModelsCB, this);    
    get_mesh_srv_ = priv_nh_.advertiseService(GET_MESH_SERVICE_NAME, &ObjectsDatabaseNode::getMeshCB, this);    
    get_description_srv_ = priv_nh_.advertiseService(GET_DESCRIPTION_SERVICE_NAME, 
						     &ObjectsDatabaseNode::getDescriptionCB, this);    
    grasp_planning_srv_ = priv_nh_.advertiseService(GRASP_PLANNING_SERVICE_NAME, 
						    &ObjectsDatabaseNode::graspPlanningCB, this);

    get_scans_srv_ = priv_nh_.advertiseService(GET_SCANS_SERVICE_NAME,
                                               &ObjectsDatabaseNode::getScansCB, this);
    save_scan_srv_ = priv_nh_.advertiseService(SAVE_SCAN_SERVICE_NAME,
                                               &ObjectsDatabaseNode::saveScanCB, this);

    priv_nh_.param<double>("prune_gripper_opening", prune_gripper_opening_, 0.5);
    priv_nh_.param<double>("prune_table_clearance", prune_table_clearance_, 0.0);
  }

  ~ObjectsDatabaseNode()
  {
    delete database_;
  }
};

int main(int argc, char **argv)
{
  ros::init(argc, argv, "objects_database_node");
  ObjectsDatabaseNode node;
  ros::spin();
  return 0;  
}
