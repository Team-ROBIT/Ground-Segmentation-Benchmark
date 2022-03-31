#ifndef PATCHWORK_H
#define PATCHWORK_H

#include "../common.hpp"

#define MARKER_Z_VALUE -2.2
#define SURFACE_ENOUGH 0.2
#define Z_THR_FILTERED 0.0
#define TOO_TILTED 1.0

#define NUM_HEURISTIC_MAX_PTS_IN_PATCH 3000

using Eigen::MatrixXf;
using Eigen::JacobiSVD;
using Eigen::VectorXf;

using PointType = PointXYZILID;

typedef std::vector<pcl::PointCloud<PointType> > Ring;
typedef std::vector<Ring>                        Zone;

/*
    @brief PathWork ROS Node.
*/
bool point_z_cmp(PointType a, PointType b) {
    return a.z < b.z;
}

class PatchWork {
public:
    PatchWork() {};

    PatchWork(ros::NodeHandle *nh) : node_handle_(*nh) {
        // Init ROS related
        ROS_INFO("Inititalizing PatchWork...");

        node_handle_.param("sensor_height", sensor_height_, 1.723);
        node_handle_.param<bool>("/patchwork/verbose", verbose_, false);

        node_handle_.param("/patchwork/num_iter", num_iter_, 3);
        node_handle_.param("/patchwork/num_lpr", num_lpr_, 20);
        node_handle_.param("/patchwork/num_min_pts", num_min_pts_, 10);
        node_handle_.param("/patchwork/th_seeds", th_seeds_, 0.4);
        node_handle_.param("/patchwork/th_dist", th_dist_, 0.3);
        node_handle_.param("/patchwork/max_r", max_range_, 80.0);
        node_handle_.param("/patchwork/min_r", min_range_, 2.7); // It indicates bodysize of the car.
        node_handle_.param("/patchwork/uniform/num_rings", num_rings_, 30);
        node_handle_.param("/patchwork/uniform/num_sectors", num_sectors_, 108);
        node_handle_.param("/patchwork/uprightness_thr", uprightness_thr_, 0.5); // The more larger, the more strict
        node_handle_.param("/patchwork/adaptive_seed_selection_margin", adaptive_seed_selection_margin_,
                           -1.1); // The more larger, the more soft

        ROS_INFO("Sensor Height: %f", sensor_height_);
        ROS_INFO("Num of Iteration: %d", num_iter_);
        ROS_INFO("Num of LPR: %d", num_lpr_);
        ROS_INFO("Num of min. points: %d", num_min_pts_);
        ROS_INFO("Seeds Threshold: %f", th_seeds_);
        ROS_INFO("Distance Threshold: %f", th_dist_);
        ROS_INFO("Max. range:: %f", max_range_);
        ROS_INFO("Min. range:: %f", min_range_);
        ROS_INFO("Num. rings: %d", num_rings_);
        ROS_INFO("Num. sectors: %d", num_sectors_);
        ROS_INFO("Normal vector threshold: %f", uprightness_thr_);
        ROS_INFO("adaptive_seed_selection_margin: %f", adaptive_seed_selection_margin_);

        // CZM denotes 'Concentric Zone Model'. Please refer to our paper
        node_handle_.getParam("/patchwork/czm/num_zones", num_zones_);
        node_handle_.getParam("/patchwork/czm/num_sectors_each_zone", num_sectors_each_zone_);
        node_handle_.getParam("/patchwork/czm/mum_rings_each_zone", num_rings_each_zone_);
        node_handle_.getParam("/patchwork/czm/elevation_thresholds", elevation_thr_);
        node_handle_.getParam("/patchwork/czm/flatness_thresholds", flatness_thr_);

        ROS_INFO("Num. zones: %d", num_zones_);

        if (num_zones_ != 4 || num_sectors_each_zone_.size() != num_rings_each_zone_.size()) {
            throw invalid_argument("Some parameters are wrong! Check the num_zones and num_rings/sectors_each_zone");
        }
        if (elevation_thr_.size() != flatness_thr_.size()) {
            throw invalid_argument("Some parameters are wrong! Check the elevation/flatness_thresholds");
        }

        cout << (boost::format("Num. sectors: %d, %d, %d, %d") % num_sectors_each_zone_[0] % num_sectors_each_zone_[1] %
                 num_sectors_each_zone_[2] %
                 num_sectors_each_zone_[3]).str() << endl;
        cout << (boost::format("Num. rings: %01d, %01d, %01d, %01d") % num_rings_each_zone_[0] % num_rings_each_zone_[1] %
                 num_rings_each_zone_[2] %
                 num_rings_each_zone_[3]).str() << endl;
        cout << (boost::format("elevation_thr_: %0.4f, %0.4f, %0.4f, %0.4f ") % elevation_thr_[0] % elevation_thr_[1] % elevation_thr_[2] %
                 elevation_thr_[3]).str() << endl;
        cout << (boost::format("flatness_thr_: %0.4f, %0.4f, %0.4f, %0.4f ") % flatness_thr_[0] % flatness_thr_[1] % flatness_thr_[2] %
                 flatness_thr_[3]).str() << endl;
        num_rings_of_interest_ = elevation_thr_.size();

        node_handle_.param("/patchwork/visualize", visualize_, true);
        poly_list_.header.frame_id = "/map";
        poly_list_.polygons.reserve(130000);

        revert_pc.reserve(NUM_HEURISTIC_MAX_PTS_IN_PATCH);
        ground_pc_.reserve(NUM_HEURISTIC_MAX_PTS_IN_PATCH);
        non_ground_pc_.reserve(NUM_HEURISTIC_MAX_PTS_IN_PATCH);
        regionwise_ground_.reserve(NUM_HEURISTIC_MAX_PTS_IN_PATCH);
        regionwise_nonground_.reserve(NUM_HEURISTIC_MAX_PTS_IN_PATCH);

        PlaneViz      = node_handle_.advertise<jsk_recognition_msgs::PolygonArray>("/gpf/plane", 100);
        revert_pc_pub = node_handle_.advertise<sensor_msgs::PointCloud2>("/revert_pc", 100);
        reject_pc_pub = node_handle_.advertise<sensor_msgs::PointCloud2>("/reject_pc", 100);

        min_range_z2_ = (7 * min_range_ + max_range_) / 8.0;
        min_range_z3_ = (3 * min_range_ + max_range_) / 4.0;
        min_range_z4_ = (min_range_ + max_range_) / 2.0;

        min_ranges   = {min_range_, min_range_z2_, min_range_z3_, min_range_z4_};
        ring_sizes   = {(min_range_z2_ - min_range_) / num_rings_each_zone_.at(0),
                        (min_range_z3_ - min_range_z2_) / num_rings_each_zone_.at(1),
                        (min_range_z4_ - min_range_z3_) / num_rings_each_zone_.at(2),
                        (max_range_ - min_range_z4_) / num_rings_each_zone_.at(3)};
        sector_sizes = {2 * M_PI / num_sectors_each_zone_.at(0), 2 * M_PI / num_sectors_each_zone_.at(1),
                        2 * M_PI / num_sectors_each_zone_.at(2),
                        2 * M_PI / num_sectors_each_zone_.at(3)};
        cout << "INITIALIZATION COMPLETE" << endl;

        for (int iter = 0; iter < num_zones_; ++iter) {
            Zone z;
            initialize_zone(z, num_sectors_each_zone_.at(iter), num_rings_each_zone_.at(iter));
            ConcentricZoneModel_.push_back(z);
        }
    }

    void estimate_ground(
            const pcl::PointCloud<PointType> &cloudIn,
            pcl::PointCloud<PointType> &cloudOut,
            pcl::PointCloud<PointType> &cloudNonground,
            double &time_taken);

    void estimate_ground(
            pcl::PointCloud<PointType> &cloudIn,
            vector<int> &labels,
            double &time_taken);

    geometry_msgs::PolygonStamped set_plane_polygon(const MatrixXf &normal_v, const float &d);

private:
    ros::NodeHandle node_handle_;

    int num_iter_;
    int num_lpr_;
    int num_min_pts_;
    int num_rings_;
    int num_sectors_;
    int num_zones_;
    int num_rings_of_interest_;

    double sensor_height_;
    double th_seeds_;
    double th_dist_;
    double max_range_;
    double min_range_;
    double uprightness_thr_;
    double adaptive_seed_selection_margin_;
    double min_range_z2_; // 12.3625
    double min_range_z3_; // 22.025
    double min_range_z4_; // 41.35

    bool verbose_;

    float           d_;
    MatrixXf        normal_;
    VectorXf        singular_values_;
    float           th_dist_d_;
    Eigen::Matrix3f cov_;
    Eigen::Vector4f pc_mean_;
    double          ring_size;
    double          sector_size;
    // For visualization
    bool            visualize_;

    vector<int> num_sectors_each_zone_;
    vector<int> num_rings_each_zone_;

    vector<double> sector_sizes;
    vector<double> ring_sizes;
    vector<double> min_ranges;
    vector<double> elevation_thr_;
    vector<double> flatness_thr_;

    vector<Zone> ConcentricZoneModel_;

    jsk_recognition_msgs::PolygonArray poly_list_;

    ros::Publisher             PlaneViz, revert_pc_pub, reject_pc_pub;
    pcl::PointCloud<PointType> revert_pc, reject_pc;
    pcl::PointCloud<PointType> ground_pc_;
    pcl::PointCloud<PointType> non_ground_pc_;

    pcl::PointCloud<PointType> regionwise_ground_;
    pcl::PointCloud<PointType> regionwise_nonground_;

    void initialize_zone(Zone &z, int num_sectors, int num_rings);

    void flush_patches_in_zone(Zone &patches, int num_sectors, int num_rings);

    double calc_principal_variance(const Eigen::Matrix3f &cov, const Eigen::Vector4f &centroid);

    double xy2theta(const double &x, const double &y);

    double xy2radius(const double &x, const double &y);

    void pc2czm(const pcl::PointCloud<PointType> &src, std::vector<Zone> &czm);

    void estimate_plane_(const pcl::PointCloud<PointType> &ground);

    void extract_piecewiseground(
            const int zone_idx, const pcl::PointCloud<PointType> &src,
            pcl::PointCloud<PointType> &dst,
            pcl::PointCloud<PointType> &non_ground_dst);

    void estimate_plane_(const int zone_idx, const pcl::PointCloud<PointType> &ground);

    void extract_initial_seeds_(
            const int zone_idx, const pcl::PointCloud<PointType> &p_sorted,
            pcl::PointCloud<PointType> &init_seeds);

    /***
     * For visulization of Ground Likelihood Estimation
     */
    geometry_msgs::PolygonStamped set_polygons(int r_idx, int theta_idx, int num_split);

    geometry_msgs::PolygonStamped set_polygons(int zone_idx, int r_idx, int theta_idx, int num_split);

    void set_ground_likelihood_estimation_status(
            const int k, const int ring_idx,
            const double z_vec,
            const double z_elevation,
            const double surface_variable);

};


void PatchWork::initialize_zone(Zone &z, int num_sectors, int num_rings) {
    z.clear();
    pcl::PointCloud<PointType> cloud;
    cloud.reserve(1000);
    Ring     ring;
    for (int i = 0; i < num_sectors; i++) {
        ring.emplace_back(cloud);
    }
    for (int j = 0; j < num_rings; j++) {
        z.emplace_back(ring);
    }
}

void PatchWork::flush_patches_in_zone(Zone &patches, int num_sectors, int num_rings) {
    for (int i = 0; i < num_sectors; i++) {
        for (int j = 0; j < num_rings; j++) {
            if (!patches[j][i].points.empty()) patches[j][i].points.clear();
        }
    }
}


void PatchWork::estimate_plane_(const pcl::PointCloud<PointType> &ground) {
    pcl::computeMeanAndCovarianceMatrix(ground, cov_, pc_mean_);
    // Singular Value Decomposition: SVD
    Eigen::JacobiSVD<Eigen::MatrixXf> svd(cov_, Eigen::DecompositionOptions::ComputeFullU);
    singular_values_ = svd.singularValues();

    // use the least singular vector as normal
    normal_ = (svd.matrixU().col(2));
    // mean ground seeds value
    Eigen::Vector3f seeds_mean = pc_mean_.head<3>();

    // according to normal.T*[x,y,z] = -d
    d_         = -(normal_.transpose() * seeds_mean)(0, 0);
    // set distance threhold to `th_dist - d`
    th_dist_d_ = th_dist_ - d_;
}

void PatchWork::extract_initial_seeds_(
        const int zone_idx, const pcl::PointCloud<PointType> &p_sorted,
        pcl::PointCloud<PointType> &init_seeds) {
    init_seeds.points.clear();

    // LPR is the mean of low point representative
    double sum = 0;
    int    cnt = 0;

    int init_idx = 0;
    if (zone_idx == 0) {
        for (int i = 0; i < p_sorted.points.size(); i++) {
            if (p_sorted.points[i].z < adaptive_seed_selection_margin_ * sensor_height_) {
                ++init_idx;
            } else {
                break;
            }
        }
    }

    // Calculate the mean height value.
    for (int i          = init_idx; i < p_sorted.points.size() && cnt < num_lpr_; i++) {
        sum += p_sorted.points[i].z;
        cnt++;
    }
    double   lpr_height = cnt != 0 ? sum / cnt : 0;// in case divide by 0

    // iterate pointcloud, filter those height is less than lpr.height+th_seeds_
    for (int i = 0; i < p_sorted.points.size(); i++) {
        if (p_sorted.points[i].z < lpr_height + th_seeds_) {
            init_seeds.points.push_back(p_sorted.points[i]);
        }
    }
}


/*
    @brief Velodyne pointcloud callback function. The main GPF pipeline is here.
    PointCloud SensorMsg -> Pointcloud -> z-value sorted Pointcloud
    ->error points removal -> extract ground seeds -> ground plane fit mainloop
*/
void PatchWork::estimate_ground(
        const pcl::PointCloud<PointType> &cloudIn,
        pcl::PointCloud<PointType> &cloudOut,
        pcl::PointCloud<PointType> &cloudNonground,
        double &time_taken) {
    poly_list_.header.stamp = ros::Time::now();
    if (!poly_list_.polygons.empty()) poly_list_.polygons.clear();
    if (!poly_list_.likelihood.empty()) poly_list_.likelihood.clear();

    static double start, t0, t1, t2, end;

    double t_total_ground = 0.0;
    double t_total_estimate = 0.0;
    // 1.Msg to pointcloud
    pcl::PointCloud<PointType> laserCloudIn;
    laserCloudIn = cloudIn;

    start = ros::Time::now().toSec();

    // 2.Sort on Z-axis value.
    sort(laserCloudIn.points.begin(), laserCloudIn.end(), point_z_cmp);

    t0 = ros::Time::now().toSec();
    // 3.Error point removal
    // As there are some error mirror reflection under the ground,
    // here regardless point under 1.8* sensor_height
    // Sort point according to height, here uses z-axis in default
    pcl::PointCloud<PointType>::iterator it = laserCloudIn.points.begin();
    for (int                             i  = 0; i < laserCloudIn.points.size(); i++) {
        if (laserCloudIn.points[i].z < -1.8 * sensor_height_) {
            it++;
        } else {
            break;
        }
    }
    laserCloudIn.points.erase(laserCloudIn.points.begin(), it);

    t1 = ros::Time::now().toSec();
    // 4. pointcloud -> regionwise setting
    for (int k = 0; k < num_zones_; ++k) {
        flush_patches_in_zone(ConcentricZoneModel_[k], num_sectors_each_zone_[k], num_rings_each_zone_[k]);
    }
    pc2czm(laserCloudIn, ConcentricZoneModel_);

    t2 = ros::Time::now().toSec();

    cloudOut.clear();
    cloudNonground.clear();
    revert_pc.clear();
    reject_pc.clear();

    for (int k = 0; k < num_zones_; ++k) {
        auto          zone     = ConcentricZoneModel_[k];
        for (uint16_t ring_idx = 0; ring_idx < num_rings_each_zone_[k]; ++ring_idx) {
            for (uint16_t sector_idx = 0; sector_idx < num_sectors_each_zone_[k]; ++sector_idx) {
                if (zone[ring_idx][sector_idx].points.size() > num_min_pts_) {
                    double t_tmp0 = ros::Time::now().toSec();
                    extract_piecewiseground(k, zone[ring_idx][sector_idx], regionwise_ground_, regionwise_nonground_);
                    double t_tmp1 = ros::Time::now().toSec();
                    t_total_ground += t_tmp1 - t_tmp0;

                    // Status of each patch
                    // used in checking uprightness, elevation, and flatness, respectively
                    const double ground_z_vec       = abs(normal_(2, 0));
                    const double ground_z_elevation = pc_mean_(2, 0);
                    const double surface_variable   =
                                         singular_values_.minCoeff() / (singular_values_(0) + singular_values_(1) + singular_values_(2));

                    if (visualize_) {
                        auto polygons = set_polygons(k, ring_idx, sector_idx, 3);
                        polygons.header = poly_list_.header;
                        poly_list_.polygons.push_back(polygons);
                        set_ground_likelihood_estimation_status(k, ring_idx, ground_z_vec, ground_z_elevation, surface_variable);
                    }

                    double t_tmp2 = ros::Time::now().toSec();
                    if (ground_z_vec < uprightness_thr_) {
                        // All points are rejected
                        cloudNonground += regionwise_ground_;
                        cloudNonground += regionwise_nonground_;
                    } else { // satisfy uprightness
                        if ((k < 2) && (ring_idx < 2)) {
                            if (ground_z_elevation > elevation_thr_[ring_idx + 2 * k]) {
                                if (flatness_thr_[ring_idx + 2 * k] > surface_variable) {
                                    if (verbose_) {
                                        std::cout << "\033[1;36m[Flatness] Recovery operated. Check " << ring_idx + 2 * k
                                                  << "th param. flatness_thr_: " << flatness_thr_[ring_idx + 2 * k] << " > "
                                                  << surface_variable << "\033[0m" << std::endl;
                                        revert_pc += regionwise_ground_;
                                    }
                                    cloudOut += regionwise_ground_;
                                    cloudNonground += regionwise_nonground_;
                                } else {
                                    if (verbose_) {
//                                        std::cout << "\033[1;34m[Elevation] Rejection operated. Check " << ring_idx + 2 * k
//                                                  << "th param. of elevation_thr_: " << elevation_thr_[ring_idx + 2 * k] << " < "
//                                                  << ground_z_elevation << "\033[0m" << std::endl;
                                        reject_pc += regionwise_ground_;
                                    }
                                    cloudNonground += regionwise_ground_;
                                    cloudNonground += regionwise_nonground_;
                                }
                            } else {
                                cloudOut += regionwise_ground_;
                                cloudNonground += regionwise_nonground_;
                            }
                        } else {
                            cloudOut += regionwise_ground_;
                            cloudNonground += regionwise_nonground_;
                        }
                    }
                    double t_tmp3 = ros::Time::now().toSec();
                    t_total_estimate += t_tmp3 - t_tmp2;
                }
            }
        }
    }
    end        = ros::Time::now().toSec();
    time_taken = end - start;
    ofstream time_txt("/home/shapelim/patchwork_time_anal.txt", std::ios::app);
    time_txt<<t0 - start<<" "<<t1 - t0 <<" "<<t2-t1<<" "<<t_total_ground<< " "<<t_total_estimate<<"\n";
    time_txt.close();

    if (verbose_) {
        sensor_msgs::PointCloud2 cloud_ROS;
        pcl::toROSMsg(revert_pc, cloud_ROS);
        cloud_ROS.header.stamp    = ros::Time::now();
        cloud_ROS.header.frame_id = "/map";
        revert_pc_pub.publish(cloud_ROS);
        pcl::toROSMsg(reject_pc, cloud_ROS);
        cloud_ROS.header.stamp    = ros::Time::now();
        cloud_ROS.header.frame_id = "/map";
        reject_pc_pub.publish(cloud_ROS);
    }
    PlaneViz.publish(poly_list_);
}

void PatchWork::estimate_ground(
        pcl::PointCloud<PointType> &cloudIn,
        vector<int> &labels,
        double &time_taken) {
    pcl::PointCloud<PointType> cloudOut;
    pcl::PointCloud<PointType> cloudNonground;
    if (!labels.empty()) labels.clear();

    pcl::PointCloud<PointType> laserCloudIn;
    laserCloudIn = cloudIn;

    poly_list_.header.stamp = ros::Time::now();
    if (!poly_list_.polygons.empty()) poly_list_.polygons.clear();
    if (!poly_list_.likelihood.empty()) poly_list_.likelihood.clear();

    static double start = ros::Time::now().toSec();

    sort(laserCloudIn.points.begin(), laserCloudIn.end(), point_z_cmp);

    pcl::PointCloud<PointType>::iterator it = laserCloudIn.points.begin();
    for (int                             i  = 0; i < laserCloudIn.points.size(); i++) {
        if (laserCloudIn.points[i].z < -1.8 * sensor_height_) {
            it++;
        } else {
            break;
        }
    }
    laserCloudIn.points.erase(laserCloudIn.points.begin(), it);

    for (int k = 0; k < num_zones_; ++k) {
        flush_patches_in_zone(ConcentricZoneModel_[k], num_sectors_each_zone_[k], num_rings_each_zone_[k]);
    }
    pc2czm(laserCloudIn, ConcentricZoneModel_);
    for (int k = 0; k < num_zones_; ++k) {
        auto zone = ConcentricZoneModel_[k];
        for (uint16_t ring_idx = 0; ring_idx < num_rings_each_zone_[k]; ++ring_idx) {
            for (uint16_t sector_idx = 0; sector_idx < num_sectors_each_zone_[k]; ++sector_idx) {
                if (zone[ring_idx][sector_idx].points.size() > num_min_pts_) {
                    extract_piecewiseground(k, zone[ring_idx][sector_idx], regionwise_ground_, regionwise_nonground_);
                    const double ground_z_vec = abs(normal_(2, 0));
                    const double ground_z_elevation = pc_mean_(2, 0);
                    const double surface_variable =
                            singular_values_.minCoeff() /
                            (singular_values_(0) + singular_values_(1) + singular_values_(2));

                    if (visualize_) {
                        auto polygons = set_polygons(k, ring_idx, sector_idx, 3);
                        polygons.header = poly_list_.header;
                        poly_list_.polygons.push_back(polygons);
                        set_ground_likelihood_estimation_status(k, ring_idx, ground_z_vec, ground_z_elevation,
                                                                surface_variable);
                    }
                    if (ground_z_vec < uprightness_thr_) {
                        // All points are rejected
                        cloudNonground += regionwise_ground_;
                        cloudNonground += regionwise_nonground_;
                    } else { // satisfy uprightness
                        if ((k < 2) && (ring_idx < 2)) {
                            if (ground_z_elevation > elevation_thr_[ring_idx + 2 * k]) {
                                if (flatness_thr_[ring_idx + 2 * k] > surface_variable) {
                                    if (verbose_) {
                                        revert_pc += regionwise_ground_;
                                    }
                                    cloudOut += regionwise_ground_;
                                    cloudNonground += regionwise_nonground_;
                                } else {
                                    if (verbose_) {
                                        reject_pc += regionwise_ground_;
                                    }
                                    cloudNonground += regionwise_ground_;
                                    cloudNonground += regionwise_nonground_;
                                }
                            } else {
                                cloudOut += regionwise_ground_;
                                cloudNonground += regionwise_nonground_;
                            }
                        } else {
                            cloudOut += regionwise_ground_;
                            cloudNonground += regionwise_nonground_;
                        }
                    }
                }
            }
        }
    }
    /*
    for (int i = 0; i<cloudIn.points.size(); i++){
        PointType pt = cloudIn.points[i];
        ground=0;
        for (int j=0 ; j<cloudOut.size() ; j++){
            PointType g_pt = cloudOut.points[j];
            if (pt.x == g_pt.x &&pt.y == g_pt.y &&pt.z == g_pt.z){
                labels.push_back(1);
                ground=1;
                break;
            }
        }
        if (ground!=1){
            labels.push_back(0);
        }
    }
     */
////try with kdtree
    pcl::KdTreeFLANN<PointType> kdtree;
    std::vector<int> idxes;
    std::vector<float> sqr_dists;
//    int cnt=0;

    auto cloudGround = boost::make_shared<pcl::PointCloud<PointType>>(cloudOut);
    kdtree.setInputCloud(cloudGround);

    for (int i = 0; i<cloudIn.points.size(); i++) {
        PointType query = cloudIn.points[i];
        kdtree.nearestKSearch(query, 1, idxes, sqr_dists);
        if (sqr_dists[0]==0)    {
            labels.push_back(1);
//            cnt++;
        }
        else                    labels.push_back(0);
    }
//    cout<<"number of 1: "<<cnt<<endl;
///////
    static double end        = ros::Time::now().toSec();

    if (verbose_) {
        sensor_msgs::PointCloud2 cloud_ROS;
        pcl::toROSMsg(revert_pc, cloud_ROS);
        cloud_ROS.header.stamp    = ros::Time::now();
        cloud_ROS.header.frame_id = "/map";
        revert_pc_pub.publish(cloud_ROS);
        pcl::toROSMsg(reject_pc, cloud_ROS);
        cloud_ROS.header.stamp    = ros::Time::now();
        cloud_ROS.header.frame_id = "/map";
        reject_pc_pub.publish(cloud_ROS);
    }
    PlaneViz.publish(poly_list_);
}

double PatchWork::calc_principal_variance(const Eigen::Matrix3f &cov, const Eigen::Vector4f &centroid) {
    double angle       = atan2(centroid(1, 0), centroid(0, 0)); // y, x
    double c           = cos(angle);
    double s           = sin(angle);
    double var_x_prime = c * c * cov(0, 0) + s * s * cov(1, 1) + 2 * c * s * cov(0, 1);
    double var_y_prime = s * s * cov(0, 0) + c * c * cov(1, 1) - 2 * c * s * cov(0, 1);
    return max(var_x_prime, var_y_prime);
}

double PatchWork::xy2theta(const double &x, const double &y) { // 0 ~ 2 * PI
    if (y >= 0) {
        return atan2(y, x); // 1, 2 quadrant
    } else {
        return 2 * M_PI + atan2(y, x);// 3, 4 quadrant
    }
}

double PatchWork::xy2radius(const double &x, const double &y) {
    return sqrt(pow(x, 2) + pow(y, 2));
}

void PatchWork::pc2czm(const pcl::PointCloud<PointXYZILID> &src, std::vector<Zone> &czm) {

    for (auto const &pt : src.points) {
        int    ring_idx, sector_idx;
        double r = xy2radius(pt.x, pt.y);
        if ((r <= max_range_) && (r > min_range_)) {
            double theta = xy2theta(pt.x, pt.y);

            if (r < min_range_z2_) { // In First rings
                ring_idx   = min(static_cast<int>(((r - min_range_) / ring_sizes[0])), num_rings_each_zone_[0] - 1);
                sector_idx = min(static_cast<int>((theta / sector_sizes[0])), num_sectors_each_zone_[0] - 1);
                czm[0][ring_idx][sector_idx].points.emplace_back(pt);
            } else if (r < min_range_z3_) {
                ring_idx   = min(static_cast<int>(((r - min_range_z2_) / ring_sizes[1])), num_rings_each_zone_[1] - 1);
                sector_idx = min(static_cast<int>((theta / sector_sizes[1])), num_sectors_each_zone_[1] - 1);
                czm[1][ring_idx][sector_idx].points.emplace_back(pt);
            } else if (r < min_range_z4_) {
                ring_idx   = min(static_cast<int>(((r - min_range_z3_) / ring_sizes[2])), num_rings_each_zone_[2] - 1);
                sector_idx = min(static_cast<int>((theta / sector_sizes[2])), num_sectors_each_zone_[2] - 1);
                czm[2][ring_idx][sector_idx].points.emplace_back(pt);
            } else { // Far!
                ring_idx   = min(static_cast<int>(((r - min_range_z4_) / ring_sizes[3])), num_rings_each_zone_[3] - 1);
                sector_idx = min(static_cast<int>((theta / sector_sizes[3])), num_sectors_each_zone_[3] - 1);
                czm[3][ring_idx][sector_idx].points.emplace_back(pt);
            }
        }

    }
}

// For adaptive
void PatchWork::extract_piecewiseground(
        const int zone_idx, const pcl::PointCloud<PointXYZILID> &src,
        pcl::PointCloud<PointXYZILID> &dst,
        pcl::PointCloud<PointXYZILID> &non_ground_dst) {
    // 0. Initialization
    if (!ground_pc_.empty()) ground_pc_.clear();
    if (!dst.empty()) dst.clear();
    if (!non_ground_dst.empty()) non_ground_dst.clear();
    // 1. set seeds!

    extract_initial_seeds_(zone_idx, src, ground_pc_);
    // 2. Extract ground
    for (int i = 0; i < num_iter_; i++) {
        estimate_plane_(ground_pc_);
        ground_pc_.clear();

        //pointcloud to matrix
        Eigen::MatrixXf points(src.points.size(), 3);
        int             j      = 0;
        for (auto       &p:src.points) {
            points.row(j++) << p.x, p.y, p.z;
        }
        // ground plane model
        Eigen::VectorXf result = points * normal_;
        // threshold filter
        for (int        r      = 0; r < result.rows(); r++) {
            if (i < num_iter_ - 1) {
                if (result[r] < th_dist_d_) {
                    ground_pc_.points.push_back(src[r]);
                }
            } else { // Final stage
                if (result[r] < th_dist_d_) {
                    dst.points.push_back(src[r]);
                } else {
                    if (i == num_iter_ - 1) {
                        non_ground_dst.push_back(src[r]);
                    }
                }
            }
        }
    }
}


geometry_msgs::PolygonStamped PatchWork::set_polygons(int r_idx, int theta_idx, int num_split) {
    geometry_msgs::PolygonStamped polygons;
    // Set point of polygon. Start from RL and ccw
    geometry_msgs::Point32        point;

    // RL
    double r_len = r_idx * ring_size + min_range_;
    double angle = theta_idx * sector_size;

    point.x = r_len * cos(angle);
    point.y = r_len * sin(angle);
    point.z = MARKER_Z_VALUE;
    polygons.polygon.points.push_back(point);
    // RU
    r_len = r_len + ring_size;
    point.x = r_len * cos(angle);
    point.y = r_len * sin(angle);
    point.z = MARKER_Z_VALUE;
    polygons.polygon.points.push_back(point);

    // RU -> LU
    for (int idx = 1; idx <= num_split; ++idx) {
        angle = angle + sector_size / num_split;
        point.x = r_len * cos(angle);
        point.y = r_len * sin(angle);
        point.z = MARKER_Z_VALUE;
        polygons.polygon.points.push_back(point);
    }

    r_len = r_len - ring_size;
    point.x = r_len * cos(angle);
    point.y = r_len * sin(angle);
    point.z = MARKER_Z_VALUE;
    polygons.polygon.points.push_back(point);

    for (int idx = 1; idx < num_split; ++idx) {
        angle = angle - sector_size / num_split;
        point.x = r_len * cos(angle);
        point.y = r_len * sin(angle);
        point.z = MARKER_Z_VALUE;
        polygons.polygon.points.push_back(point);
    }

    return polygons;
}

geometry_msgs::PolygonStamped PatchWork::set_polygons(int zone_idx, int r_idx, int theta_idx, int num_split) {
    geometry_msgs::PolygonStamped polygons;
    // Set point of polygon. Start from RL and ccw
    geometry_msgs::Point32        point;

    // RL
    double zone_min_range = min_ranges[zone_idx];
    double r_len          = r_idx * ring_sizes[zone_idx] + zone_min_range;
    double angle          = theta_idx * sector_sizes[zone_idx];

    point.x = r_len * cos(angle);
    point.y = r_len * sin(angle);
    point.z = MARKER_Z_VALUE;
    polygons.polygon.points.push_back(point);
    // RU
    r_len = r_len + ring_sizes[zone_idx];
    point.x = r_len * cos(angle);
    point.y = r_len * sin(angle);
    point.z = MARKER_Z_VALUE;
    polygons.polygon.points.push_back(point);

    // RU -> LU
    for (int idx = 1; idx <= num_split; ++idx) {
        angle = angle + sector_sizes[zone_idx] / num_split;
        point.x = r_len * cos(angle);
        point.y = r_len * sin(angle);
        point.z = MARKER_Z_VALUE;
        polygons.polygon.points.push_back(point);
    }

    r_len = r_len - ring_sizes[zone_idx];
    point.x = r_len * cos(angle);
    point.y = r_len * sin(angle);
    point.z = MARKER_Z_VALUE;
    polygons.polygon.points.push_back(point);

    for (int idx = 1; idx < num_split; ++idx) {
        angle = angle - sector_sizes[zone_idx] / num_split;
        point.x = r_len * cos(angle);
        point.y = r_len * sin(angle);
        point.z = MARKER_Z_VALUE;
        polygons.polygon.points.push_back(point);
    }

    return polygons;
}

void PatchWork::set_ground_likelihood_estimation_status(
        const int k, const int ring_idx,
        const double z_vec,
        const double z_elevation,
        const double surface_variable) {
    if (z_vec > uprightness_thr_) { //orthogonal
        if ((k < 2) && (ring_idx < 2)) {
            if (z_elevation > elevation_thr_[ring_idx + 2 * k]) {
                if (flatness_thr_[ring_idx + 2 * k] > surface_variable) {
                    poly_list_.likelihood.push_back(SURFACE_ENOUGH);
                } else {
                    poly_list_.likelihood.push_back(Z_THR_FILTERED);
                }
            } else {
                poly_list_.likelihood.push_back(NORMAL_ENOUGH);
            }
        } else {
            poly_list_.likelihood.push_back(NORMAL_ENOUGH);
        }
    } else { // tilted
        poly_list_.likelihood.push_back(TOO_TILTED);
    }
}

#endif
