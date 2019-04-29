#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <glob.h>
#include <thread>
#include <mutex>
#include <omp.h>
#include <iomanip>

#include "render.h"


std::vector<std::string> globVector(const std::string& pattern){
    glob_t glob_result;
    glob(pattern.c_str(), GLOB_TILDE, NULL, &glob_result);
    std::vector<std::string> files;
    for(unsigned int i=0; i<glob_result.gl_pathc; ++i){
        files.push_back(std::string(glob_result.gl_pathv[i]));
    }
    globfree(&glob_result);
    return files;
}

std::vector<std::string> readlist(const std::string& data_dir, const std::string& scene_list){
    std::ifstream scene_list_file(scene_list);
    std::vector<std::string> scenes;
    std::string str;
    while (getline(scene_list_file, str)) {
        scenes.push_back(data_dir+"/"+str);
    }
    return scenes;
}


std::mutex vector_mutex;

void batch_render(unsigned int start, unsigned int end, std::vector<render::RenderBuffer>& valid_images, render::RenderParam param){
    if(start < param.poses.size() && end <= param.poses.size()){
        for(unsigned int i=start; i<end; i++){
            // extrinsic
            render::RenderBuffer buf(param.width, param.height);
            Eigen::Matrix4f extrinsic = param.poses[i] * param.cam_extrinsic;
            render::render(param.v, param.f, param.v_color, param.cam_intrinsic, extrinsic, param.height, param.width, buf);
            if(buf.is_valid){
                vector_mutex.lock();
                valid_images.push_back(buf);
                vector_mutex.unlock();
            }
            else{ 
                delete [] buf.depth;
                delete [] buf.depth_vis;
                delete [] buf.color;
                delete [] buf.normal;
                delete [] buf.normal_vis;
                delete [] buf.pixcoord;
                delete [] buf.pixmeta;
                delete [] buf.mesh_vertex;
            }
        }
    }
    //std::cout << "** [INFO] DONE, start: " << start <<", end: "<< end <<", num poses: " << param.poses.size() << std::endl;
}


//bool batch_dump(unsigned int start, unsigned int end, std::vector<render::RenderBuffer>& images, std::string scene_dir){
//    //std::cout << "** [INFO] start: " << start <<", end: "<< end <<", num images: " << images.size() << std::endl;
//    if(start < images.size() && end <= images.size()){
//        for (unsigned int i=start; i<end; i++){
//            std::ostringstream fname;
//            fname << std::setw(6) << std::setfill('0') << i ;
//            //std::cout << fname.str() << std::endl;
//            render::dump_buffer(images[i], scene_dir, fname.str());
//        }
//    }
//    //std::cout << "** [INFO] DONE, start: " << start <<", end: "<< end <<", num images: " << images.size() << std::flush;
//    return true;
//}


int main(int argc, char *argv[]) {
    std::cout <<"[INFO] Read "<< argc <<" arguments."<<std::endl;
    if(argc<7){
        std::cout << "Usage: ./render dataset_dir scene_list pose_mode output_height output_depth num_thread"<<std::endl;
        std::cout << "\tdataset_dir, e.g. scannet_data/val/"<<std::endl;
        std::cout << "\tscene_list, e.g. scannetv2_val.txt"<<std::endl;
        std::cout << "\tpose_mode, e.g. {1/2/3/4}"<<std::endl;
        std::cout << "\toutput_height, e.g. 480"<<std::endl;
        std::cout << "\toutput_width, e.g. 640"<<std::endl;
        std::cout << "\tnum_thread, e.g. 10"<<std::endl;
        return 0;
    }
    else{
        std::cout << "[INFO]: ./render dataset_dir scene_list pose_mode output_height output_depth num_thread"<<std::endl;
        std::cout << "\tdataset_dir: "<<argv[1]<<std::endl;
        std::cout << "\tscene_list: "<<argv[2]<<std::endl;
        std::cout << "\tpose_mode: "<<argv[3]<<std::endl;
        std::cout << "\toutput_height: "<<argv[4]<<std::endl;
        std::cout << "\toutput_width:"<<argv[5]<<std::endl;
        std::cout << "\tnum_thread:"<<argv[6]<<std::endl;
    }
    std::string dataset_dir = argv[1];
    std::string scene_list = argv[2];
    std::vector<std::string> scene_dirs = readlist(dataset_dir, scene_list);
    ////std::vector<std::string> scene_dirs = globVector(dataset_dir.append("/scene0*"));
    std::cout <<"\n[INFO] Find "<< scene_dirs.size() << " scenes" <<std::endl;
    std::cout << "\n------------------------ Start rendering ----------------------------" << std::endl;
    double total_non_coverage = 0.0;
    unsigned int total_valid_images = 0;
    unsigned int total_coverage_images = 0;
    unsigned int total_scene_images = 0;
    for(unsigned int sid=0; sid<scene_dirs.size(); sid++){
        float ts = omp_get_wtime();
        /** load scene_ply file **/
        std::vector<render::Point3> vertex; 
        std::vector<render::int3> face;
        std::vector<render::uchar4> vertex_color;
        std::string scene_name = scene_dirs[sid].substr(scene_dirs[sid].length()-12, scene_dirs[sid].length());
        std::string scene_ply = scene_dirs[sid]+"/"+scene_name+"_vh_clean_2.ply";

        render::read_ply(scene_ply, vertex, face, vertex_color);

        /** scene_ply boundary **/
        std::vector<int> scene_min(3, INT_MAX);
        std::vector<int> scene_max(3, INT_MIN);
        render::scene_range(vertex, scene_min, scene_max);

        /** read camera intrinsic and extrinsic **/
        std::string depth_intrinsic = scene_dirs[sid]+"/intrinsic_depth.txt";
        std::string depth_extrinsic = scene_dirs[sid]+"/extrinsic_depth.txt";
        Eigen::Matrix4f cam_intrinsic = render::read_camera_parameters(depth_intrinsic);
        Eigen::Matrix4f cam_extrinsic = render::read_camera_parameters(depth_extrinsic);

        /** get all poses **/
        int pose_mode = atoi(argv[3]);
        std::vector<Eigen::Matrix4f> pseudo_poses = render::pseudo_camera_poses(scene_min, scene_max, pose_mode);

        /** output scene info **/
        std::cout<<"[INFO] rendering "<< scene_dirs[sid] << std::endl;
        std::cout <<"\tscene_min: ";
        for(int i=0; i<3; i++)
            std::cout<<scene_min[i]<<" ";
        std::cout<<std::endl;
        std::cout <<"\tscene_max: ";
        for(int i=0; i<3; i++)
            std::cout<<scene_max[i]<<" ";
        std::cout<<std::endl;
        std::cout<<"\tpseudo_poses size:"<<pseudo_poses.size()<<std::endl;

        /** get valid poses from pseudo poses **/
        unsigned int height = atoi(argv[4]);
        unsigned int width = atoi(argv[5]);
        unsigned int num_workers = atoi(argv[6]);
        // parameters
        render::RenderParam param(height, width);
        param.poses = pseudo_poses;
        param.f = face;
        param.v = vertex;
        param.v_color = vertex_color;
        param.cam_intrinsic = cam_intrinsic;
        param.cam_extrinsic = cam_extrinsic;

        std::vector<render::RenderBuffer> valid_images;
        std::thread thread_pool[num_workers];
        int batch_size = (int)ceil(param.poses.size() / (float)num_workers);
        for(unsigned int i=0; i<num_workers; i++){
            // execute thread
            unsigned int start = i * batch_size;
            unsigned int end = start + batch_size;
            if(end > param.poses.size())
                end = param.poses.size();
            //std::cout << "start: " << start <<", end: "<< end <<", num poses: " << param.poses.size() << std::endl;
            thread_pool[i] = std::thread(batch_render, start, end, ref(valid_images), param);
        }
        for(unsigned int i=0; i<num_workers; i++){
            // wait the thread stop
            thread_pool[i].join();
        }

        sort(valid_images.begin(), valid_images.end(), 
            [](const render::RenderBuffer & a, const render::RenderBuffer & b) -> bool{ 
                return a.vertex_set.size() > b.vertex_set.size(); 
            });
        //for(unsigned int i=0; i<valid_images.size(); i++){
        //    std::cout<<i<<"/"<<valid_images.size()<<", "<<valid_images[i].vertex_set.size()<<std::endl;
        //}

        /** gready algo to cover scene vertices **/
        std::vector<render::RenderBuffer> scene_images;
        std::vector<render::RenderBuffer> coverage_images;
        double scene_non_coverage = render::cover_scene_images(vertex, valid_images, coverage_images, scene_images);
        total_non_coverage += scene_non_coverage;
        total_valid_images += valid_images.size();
        total_coverage_images += coverage_images.size();
        total_scene_images += scene_images.size();
        
        std::cout <<"\tvalid image size:"<<valid_images.size()<<std::endl;
        std::cout <<"\tcoverage image size:"<<coverage_images.size()<<std::endl;
        std::cout <<"\tscene image size:"<<scene_images.size()<<std::endl;
        std::cout <<"\tscene non-coverage:"<< scene_non_coverage <<", current total non-coverage:"<<total_non_coverage/(sid+1)<<", "<<(sid+1)<<"/"<<scene_dirs.size()<<std::endl;
        std::cout << "\trendering time: "<< omp_get_wtime() - ts <<" sec"<< std::endl;

        float ots = omp_get_wtime();
        batch_size = (int)ceil(coverage_images.size() / (float)num_workers);
//        for(unsigned int i=0; i<num_workers; i++){
//            // execute thread
//            unsigned int start = i * batch_size;
//            unsigned int end = start + batch_size;
//            if(end > coverage_images.size())
//                end = coverage_images.size();
//            //std::cout << "start: " << start <<", end: "<< end <<", num images: " << coverage_images.size() << std::endl;
//            thread_pool[i] = std::thread(batch_dump, start, end, ref(coverage_images), scene_dirs[sid]);
//        }
//        for(unsigned int i=0; i<num_workers; i++){
//            // wait the thread stop
//            thread_pool[i].join();
//        }
        std::cout << "\toutput time: "<< omp_get_wtime() - ots <<" sec"<< std::endl;
        for(unsigned int i=0; i<valid_images.size(); i++){
            delete [] valid_images[i].depth;
            delete [] valid_images[i].depth_vis;
            delete [] valid_images[i].color;
            delete [] valid_images[i].normal;
            delete [] valid_images[i].normal_vis;
            delete [] valid_images[i].pixcoord;
            delete [] valid_images[i].mesh_vertex;
            delete [] valid_images[i].pixmeta;
        }
        
        std::cout << "\ttotal processing time: "<< omp_get_wtime() - ts <<" sec"<< std::endl;
    }
    std::cout << "\n------------------------ Finish ----------------------------" << std::endl;
    std::cout <<"\n[INFO] Total Non-Coverage:"<<total_non_coverage/scene_dirs.size()<<std::endl;
    std::cout <<"\n[INFO] Total Number of Valid Images:"<<total_valid_images<<std::endl;
    std::cout <<"\n[INFO] Total Number of Coverage Images:"<<total_coverage_images<<std::endl;
    std::cout <<"\n[INFO] Total Number of Scene Images:"<<total_scene_images<<std::endl;

    return 0;
}
