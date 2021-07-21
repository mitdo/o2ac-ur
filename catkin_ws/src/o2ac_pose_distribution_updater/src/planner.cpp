#include "o2ac_pose_distribution_updater/planner.hpp"
#include "o2ac_pose_distribution_updater/convex_hull.hpp"

namespace {
double LARGE_EPS = 1e-8, EPS = 1e-9;
double pi = acos(-1);
} // namespace

void Planner::apply_action(const Eigen::Isometry3d &old_mean,
                           const CovarianceMatrix &old_covariance,
                           const UpdateAction &action,
                           Eigen::Isometry3d &new_mean,
                           CovarianceMatrix &new_covariance) {
  if (action.type == place_action_type) {
    place_step_with_Lie_distribution(
        gripped_geometry->vertices, gripped_geometry->triangles,
        action.gripper_pose, 0.0, old_mean, old_covariance, new_mean,
        new_covariance, true);
    new_mean = action.gripper_pose * new_mean;
    new_covariance = transform_covariance(action.gripper_pose, new_covariance);
  } else if (action.type == grasp_action_type) {
    grasp_step_with_Lie_distribution(
        gripped_geometry->vertices, gripped_geometry->triangles,
        action.gripper_pose, action.gripper_pose.inverse() * old_mean,
        transform_covariance(action.gripper_pose.inverse(), old_covariance),
        new_mean, new_covariance, true);
  } else if (action.type == push_action_type) {
    push_step_with_Lie_distribution(
        gripped_geometry->vertices, gripped_geometry->triangles,
        action.gripper_pose, action.gripper_pose.inverse() * old_mean,
        transform_covariance(action.gripper_pose.inverse(), old_covariance),
        new_mean, new_covariance, true);
    new_mean = action.gripper_pose * new_mean;
    new_covariance = transform_covariance(action.gripper_pose, new_covariance);
  } else if (action.type == touch_action_type) {
    touched_step_with_Lie_distribution(
        0, gripped_geometry->vertices, gripped_geometry->triangles,
        eigen_to_fcl_transform(action.gripper_pose), old_mean, old_covariance,
        new_mean, new_covariance);
  } else if (action.type == look_action_type) {
    cv::Mat mean_image;
    boost::array<unsigned int, 4> ROI{0, image_height, 0, image_width};
    generate_image(mean_image, gripped_geometry->vertices,
                   gripped_geometry->triangles, old_mean, ROI);
    look_step_with_Lie_distribution(
        gripped_geometry->vertices, gripped_geometry->triangles,
        action.gripper_pose, mean_image, ROI, old_mean, old_covariance,
        new_mean, new_covariance, true);
  }
}

Eigen::AngleAxisd rotation_to_minus_Z(const Eigen::Vector3d vector) {
  Eigen::Vector3d axis =
      vector[2] != 0.0 ? vector.cross(-Eigen::Vector3d::UnitZ()).normalized()
                       : Eigen::Vector3d::UnitX();
  double angle = atan2(vector.head<2>().norm(), -vector[2]);
  return Eigen::AngleAxisd(angle, axis);
}

void Planner::calculate_action_candidates(
    const Eigen::Isometry3d &current_gripper_pose,
    const Eigen::Isometry3d &current_mean, const CovarianceMatrix &covariance,
    const bool &gripping, std::vector<UpdateAction> &candidates) {
  if (gripping) {
    // add place action candidates
    for (auto &plane : place_candidates) {
      UpdateAction action;
      action.type = place_action_type;

      Eigen::Vector3d normal = current_gripper_pose.rotation() *
                               current_mean.rotation() * plane.normal();

      Eigen::Isometry3d rotated_gripper_pose =
          rotation_to_minus_Z(normal) * current_gripper_pose;
      double sigma_z = 3.0 * sqrt(covariance(2, 2));
      action.gripper_pose =
          Eigen::Translation3d((sigma_z - (rotated_gripper_pose * current_mean *
                                           (-plane.offset() * plane.normal()))
                                              .z()) *
                               Eigen::Vector3d::UnitZ()) *
          rotated_gripper_pose;

      candidates.push_back(action);
    }
    // add touch action candidates
    /*for(auto& vertex : convex_hull_vertices){
      UpdateAction action;
      action.type = place_action_type;

      Eigen::Vector3d direction = current_gripper_pose.rotation() *
      current_mean.rotation() * (vertex - center_of_gravity);
      }*/
  } else {
    for (auto &grasp_point : *grasp_points) {
      Eigen::Isometry3d gripper_pose = current_mean * grasp_point;
      if (abs((gripper_pose.rotation() * Eigen::Vector3d::UnitY())(2)) <=
          LARGE_EPS) {
        UpdateAction action;
        action.type = grasp_action_type;
        action.gripper_pose = gripper_pose;
        candidates.push_back(action);
      }
    }
    std::vector<Eigen::Vector2d> projected_points, hull;
    std::transform(gripped_geometry->vertices.begin(),
                   gripped_geometry->vertices.end(),
                   std::back_inserter(projected_points),
                   [&current_mean](const Eigen::Vector3d &vertex) {
                     return (Eigen::Vector2d)(current_mean * vertex).head<2>();
                   });

    convex_hull_for_Eigen_Vector2d(projected_points, hull);

    Eigen::Vector3d current_center = current_mean * center_of_gravity;
    Eigen::Vector2d projected_center = current_center.head<2>();
    for (int i = 0; i < hull.size() - 1; i++) {
      Eigen::Vector2d edge = (hull[i + 1] - hull[i]).normalized();
      if (edge.dot(projected_center - hull[i]) < -EPS ||
          edge.dot(projected_center - hull[i + 1]) > EPS) {
        continue;
      }
      Eigen::Vector2d edge_normal;
      edge_normal << edge[1], -edge[0];
      Eigen::Vector3d translation;
      double sigma_y =
          4.0 * sqrt(pow(edge_normal(0), 2) * covariance(0, 0) +
                     2.0 * edge_normal(0) * edge_normal(1) * covariance(0, 1) +
                     pow(edge_normal(1), 2) * covariance(1, 1) +
                     pow((hull[i + 1] - hull[i]).norm(), 2) * covariance(5, 5));
      translation << (edge_normal.dot(hull[i]) - sigma_y +
                      gripper_width / 2.0) *
                             edge_normal +
                         edge.dot(projected_center) * edge,
          0.0;
      double angle = -atan2(-edge(0), -edge(1));

      UpdateAction action;
      action.type = push_action_type;
      action.gripper_pose =
          Eigen::Translation3d(translation) *
          Eigen::AngleAxisd(angle, Eigen::Vector3d::UnitZ()) *
          Eigen::AngleAxisd(pi / 2.0, Eigen::Vector3d::UnitY());
      candidates.push_back(action);
    }
  }
}

double Planner::calculate_cost(const Eigen::Isometry3d &current_gripper_pose,
                               const Eigen::Isometry3d &next_gripper_pose) {
  Eigen::Isometry3d move = next_gripper_pose * current_gripper_pose.inverse();
  return action_cost + translation_cost * move.translation().norm() +
         rotation_cost * Eigen::AngleAxisd(move.rotation()).angle();
}

std::vector<UpdateAction> Planner::calculate_plan(
    const std::shared_ptr<mesh_object> &gripped_geometry,
    const std::shared_ptr<std::vector<Eigen::Isometry3d>> &grasp_points,
    const Eigen::Isometry3d &current_gripper_pose, const bool &current_gripping,
    const Eigen::Isometry3d &current_mean,
    const CovarianceMatrix &current_covariance,
    const CovarianceMatrix &objective_coefficients,
    const double &objective_value) {
  this->gripped_geometry = gripped_geometry;
  this->grasp_points = grasp_points;
  center_of_gravity = calculate_center_of_gravity(gripped_geometry->vertices,
                                                  gripped_geometry->triangles);
  calculate_place_candidates(gripped_geometry->vertices, center_of_gravity,
                             place_candidates);

  struct node {
    Eigen::Isometry3d mean, gripper_pose;
    CovarianceMatrix covariance;

    int previous_node_id;
    UpdateAction previous_action;
    double cost;
    bool gripping;
  };

  std::vector<node> nodes(1);
  nodes[0].mean = current_mean;
  nodes[0].covariance = current_covariance;
  nodes[0].previous_node_id = -1;
  nodes[0].gripper_pose = current_gripper_pose;
  nodes[0].cost = 0.0;
  nodes[0].gripping = current_gripping;

  std::priority_queue<std::pair<double, int>> open_nodes;

  open_nodes.push(std::make_pair(0.0, 0));
  int goal_node_id = -1;
  while (!open_nodes.empty()) {
    int id = -open_nodes.top().second;
    fprintf(stderr, "%d %d %d %lf %lf\n", id, nodes[id].previous_node_id,
            id > 0 ? nodes[id].previous_action.type : -1, nodes[id].cost,
            objective_coefficients.cwiseProduct(nodes[id].covariance).sum());
    if (objective_coefficients.cwiseProduct(nodes[id].covariance).sum() <
        objective_value) {
      goal_node_id = id;
      break;
    }
    open_nodes.pop();

    std::vector<UpdateAction> candidates;
    calculate_action_candidates(nodes[id].gripper_pose, nodes[id].mean,
                                nodes[id].covariance, nodes[id].gripping,
                                candidates);
    for (auto &candidate : candidates) {
      node new_node;
      try {
        apply_action(nodes[id].mean, nodes[id].covariance, candidate,
                     new_node.mean, new_node.covariance);
      } catch (std::runtime_error &e) {
        std::cerr << candidate.type << std::endl;
        std::cerr << e.what() << std::endl;
        continue;
      }
      new_node.gripper_pose = candidate.gripper_pose;
      new_node.gripping = (candidate.type == grasp_action_type ? true : false);
      new_node.previous_node_id = id;
      new_node.previous_action = candidate;
      new_node.cost = nodes[id].cost + calculate_cost(nodes[id].gripper_pose,
                                                      candidate.gripper_pose);
      int new_id = nodes.size();
      nodes.push_back(new_node);
      open_nodes.push(std::make_pair(-new_node.cost, -new_id));
    }
  }
  if (goal_node_id == -1) {
    throw std::runtime_error("planning failed");
  }
  std::vector<UpdateAction> actions;
  while (goal_node_id != 0) {
    std::cerr << nodes[goal_node_id].mean.matrix() << std::endl;
    std::cerr << nodes[goal_node_id].covariance << std::endl;
    std::cerr << nodes[goal_node_id].previous_action.type << std::endl;
    std::cerr << nodes[goal_node_id].previous_action.gripper_pose.matrix()
              << std::endl;
    actions.push_back(nodes[goal_node_id].previous_action);
    goal_node_id = nodes[goal_node_id].previous_node_id;
  }
  std::reverse(actions.begin(), actions.end());
  return actions;
}
