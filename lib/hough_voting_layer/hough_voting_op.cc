/* Copyright 2015 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// Computing label Op

#include <stdio.h>
#include <cfloat>
#include <math.h>
#include <time.h>

#include "types.h"
#include "sampler2D.h"
#include "ransac.h"
#include "Hypothesis.h"
#include "detection.h"
#include <Eigen/Geometry> 

#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor_shape.h"

using namespace tensorflow;
typedef Eigen::ThreadPoolDevice CPUDevice;

REGISTER_OP("Houghvoting")
    .Attr("T: {float, double}")
    .Attr("preemptive_batch: int")
    .Input("bottom_label: int32")
    .Input("bottom_vertex: T")
    .Input("bottom_extents: T")
    .Input("bottom_meta_data: T")
    .Input("bottom_gt: T")
    .Output("top_box: T")
    .Output("top_pose: T")
    .Output("top_target: T")
    .Output("top_weight: T");

REGISTER_OP("HoughvotingGrad")
    .Attr("T: {float, double}")
    .Input("bottom_label: int32")
    .Input("bottom_vertex: T")
    .Input("grad: T")
    .Output("output_label: T")
    .Output("output_vertex: T");

/**
 * @brief Data used in NLOpt callback loop.
 */
struct DataForOpt
{
  int imageWidth;
  int imageHeight;
  float rx, ry;
  cv::Rect bb2D;
  std::vector<cv::Point3f> bb3D;
  cv::Mat_<float> camMat;
};

void getProbs(const float* probability, std::vector<jp::img_stat_t>& probs, int width, int height, int num_classes);
void getCenters(const float* vertmap, std::vector<jp::img_center_t>& vertexs, int width, int height, int num_classes);
void getLabels(const int* label_map, std::vector<std::vector<int>>& labels, std::vector<int>& object_ids, int width, int height, int num_classes, int minArea);
void getBb3Ds(float* extents, std::vector<std::vector<cv::Point3f>>& bb3Ds, int num_classes);
void createSamplers(std::vector<Sampler2D>& samplers, const std::vector<jp::img_stat_t>& probs, int imageWidth, int imageHeight);
inline bool samplePoint2D(jp::id_t objID, std::vector<cv::Point2f>& eyePts, std::vector<cv::Point2f>& objPts, const cv::Point2f& pt2D, const float* vertmap, int width, int num_classes);
std::vector<TransHyp*> getWorkingQueue(std::map<jp::id_t, std::vector<TransHyp>>& hypMap, int maxIt);
inline float point2line(cv::Point2d x, cv::Point2f n, cv::Point2f p);
inline void countInliers2D(TransHyp& hyp, const float * vertmap, const std::vector<std::vector<int>>& labels, float inlierThreshold, int width, int num_classes, int pixelBatch);
inline void updateHyp2D(TransHyp& hyp, int maxPixels);
inline void filterInliers2D(TransHyp& hyp, int maxInliers);
inline cv::Point2f getMode2D(jp::id_t objID, const cv::Point2f& pt, const float* vertmap, int width, int num_classes);
static double optEnergy(const std::vector<double> &pose, std::vector<double> &grad, void *data);
double poseWithOpt(std::vector<double> & vec, DataForOpt data, int iterations);
void estimateCenter(const int* labelmap, const float* vertmap, const float* extents, int batch, int height, int width, int num_classes, int preemptive_batch,
  float fx, float fy, float px, float py, std::vector<cv::Vec<float, 13> >& outputs);
void compute_target_weight(float* target, float* weight, const float* poses_gt, int num_gt, int num_classes, std::vector<cv::Vec<float, 13> > outputs);

template <typename Device, typename T>
class HoughvotingOp : public OpKernel {
 public:
  explicit HoughvotingOp(OpKernelConstruction* context) : OpKernel(context) {
    // Get the pool height
    OP_REQUIRES_OK(context,
                   context->GetAttr("preemptive_batch", &preemptive_batch_));
    // Check that pooled_height is positive
    OP_REQUIRES(context, preemptive_batch_ >= 0,
                errors::InvalidArgument("Need preemptive_batch >= 0, got ",
                                        preemptive_batch_));
  }

  // bottom_label: (batch_size, height, width)
  // bottom_vertex: (batch_size, height, width, 2 * num_classes)
  // top_box: (num, 6) i.e., batch_index, cls, x1, y1, x2, y2
  void Compute(OpKernelContext* context) override 
  {
    // Grab the input tensor
    const Tensor& bottom_label = context->input(0);
    const Tensor& bottom_vertex = context->input(1);
    const Tensor& bottom_extents = context->input(2);

    // format of the meta_data
    // intrinsic matrix: meta_data[0 ~ 8]
    // inverse intrinsic matrix: meta_data[9 ~ 17]
    // pose_world2live: meta_data[18 ~ 29]
    // pose_live2world: meta_data[30 ~ 41]
    // voxel step size: meta_data[42, 43, 44]
    // voxel min value: meta_data[45, 46, 47]
    const Tensor& bottom_meta_data = context->input(3);
    auto meta_data = bottom_meta_data.flat<T>();

    const Tensor& bottom_gt = context->input(4);
    const float* gt = bottom_gt.flat<float>().data();

    // data should have 5 dimensions.
    OP_REQUIRES(context, bottom_label.dims() == 3,
                errors::InvalidArgument("label must be 3-dimensional"));

    OP_REQUIRES(context, bottom_vertex.dims() == 4,
                errors::InvalidArgument("vertex must be 4-dimensional"));

    // batch size
    int batch_size = bottom_label.dim_size(0);
    // height
    int height = bottom_label.dim_size(1);
    // width
    int width = bottom_label.dim_size(2);
    // num of classes
    int num_classes = bottom_vertex.dim_size(3) / 2;
    int num_meta_data = bottom_meta_data.dim_size(3);
    int num_gt = bottom_gt.dim_size(0);

    // for each image, run hough voting
    std::vector<cv::Vec<float, 13> > outputs;
    const float* extents = bottom_extents.flat<float>().data();

    int index_meta_data = 0;
    for (int n = 0; n < batch_size; n++)
    {
      const int* labelmap = bottom_label.flat<int>().data() + n * height * width;
      const float* vertmap = bottom_vertex.flat<float>().data() + n * height * width * 2 * num_classes;
      float fx = meta_data(index_meta_data + 0);
      float fy = meta_data(index_meta_data + 4);
      float px = meta_data(index_meta_data + 2);
      float py = meta_data(index_meta_data + 5);

      estimateCenter(labelmap, vertmap, extents, n, height, width, num_classes, preemptive_batch_, fx, fy, px, py, outputs);

      index_meta_data += num_meta_data;
    }

    // Create output tensors
    // top_box
    int dims[2];
    dims[0] = outputs.size();
    dims[1] = 6;
    TensorShape output_shape;
    TensorShapeUtils::MakeShape(dims, 2, &output_shape);

    Tensor* top_box_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &top_box_tensor));
    float* top_box = top_box_tensor->template flat<float>().data();

    // top_pose
    dims[1] = 7;
    TensorShape output_shape_pose;
    TensorShapeUtils::MakeShape(dims, 2, &output_shape_pose);

    Tensor* top_pose_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(1, output_shape_pose, &top_pose_tensor));
    float* top_pose = top_pose_tensor->template flat<float>().data();

    // top target
    dims[1] = 4 * num_classes;
    TensorShape output_shape_target;
    TensorShapeUtils::MakeShape(dims, 2, &output_shape_target);

    Tensor* top_target_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(2, output_shape_target, &top_target_tensor));
    float* top_target = top_target_tensor->template flat<float>().data();
    memset(top_target, 0, outputs.size() * 4 * num_classes *sizeof(T));

    // top weight
    Tensor* top_weight_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(3, output_shape_target, &top_weight_tensor));
    float* top_weight = top_weight_tensor->template flat<float>().data();
    memset(top_weight, 0, outputs.size() * 4 * num_classes *sizeof(T));
    
    for(int n = 0; n < outputs.size(); n++)
    {
      cv::Vec<float, 13> roi = outputs[n];

      for (int i = 0; i < 6; i++)
        top_box[n * 6 + i] = roi(i);

      for (int i = 0; i < 7; i++)
        top_pose[n * 7 + i] = roi(6 + i);
    }

    compute_target_weight(top_target, top_weight, gt, num_gt, num_classes, outputs);
  }
 private:
  int preemptive_batch_;
};

REGISTER_KERNEL_BUILDER(Name("Houghvoting").Device(DEVICE_CPU).TypeConstraint<float>("T"), HoughvotingOp<CPUDevice, float>);


// compute gradient
template <class Device, class T>
class HoughvotingGradOp : public OpKernel {
 public:
  explicit HoughvotingGradOp(OpKernelConstruction* context) : OpKernel(context) {
  }

  void Compute(OpKernelContext* context) override 
  {
    // Grab the input tensor
    const Tensor& bottom_label = context->input(0);
    const Tensor& bottom_vertex = context->input(1);

    // data should have 5 dimensions.
    OP_REQUIRES(context, bottom_label.dims() == 3,
                errors::InvalidArgument("label must be 3-dimensional"));

    OP_REQUIRES(context, bottom_vertex.dims() == 4,
                errors::InvalidArgument("vertex must be 4-dimensional"));

    // batch size
    int batch_size = bottom_label.dim_size(0);
    // height
    int height = bottom_label.dim_size(1);
    // width
    int width = bottom_label.dim_size(2);
    // num of classes
    int num_classes = bottom_vertex.dim_size(3) / 2;

    // construct the output shape
    TensorShape output_shape = bottom_label.shape();
    Tensor* top_label_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(0, output_shape, &top_label_tensor));
    T* top_label = top_label_tensor->template flat<T>().data();
    memset(top_label, 0, batch_size * height * width * sizeof(T));

    TensorShape output_shape_1 = bottom_vertex.shape();
    Tensor* top_vertex_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(1, output_shape_1, &top_vertex_tensor));
    T* top_vertex = top_vertex_tensor->template flat<T>().data();
    memset(top_vertex, 0, batch_size * height * width * 2 * num_classes *sizeof(T));
  }
};

REGISTER_KERNEL_BUILDER(Name("HoughvotingGrad").Device(DEVICE_CPU).TypeConstraint<float>("T"), HoughvotingGradOp<CPUDevice, float>);


// get probs
void getProbs(const float* probability, std::vector<jp::img_stat_t>& probs, int width, int height, int num_classes)
{
  // for each object
  for (int i = 1; i < num_classes; i++)
  {
    jp::img_stat_t img(height, width);

    #pragma omp parallel for
    for(int x = 0; x < width; x++)
    for(int y = 0; y < height; y++)
    {
      int offset = i + num_classes * (y * width + x);
      img(y, x) = probability[offset];
    }

    probs.push_back(img);
  }
}


// get centers
void getCenters(const float* vertmap, std::vector<jp::img_center_t>& vertexs, int width, int height, int num_classes)
{
  // for each object
  for (int i = 1; i < num_classes; i++)
  {
    jp::img_center_t img(height, width);

    #pragma omp parallel for
    for(int x = 0; x < width; x++)
    for(int y = 0; y < height; y++)
    {
      int channel = 2 * i;
      int offset = channel + 2 * num_classes * (y * width + x);

      jp::coord2_t obj;
      obj(0) = vertmap[offset];
      obj(1) = vertmap[offset + 1];

      img(y, x) = obj;
    }

    vertexs.push_back(img);
  }
}


// get label lists
void getLabels(const int* label_map, std::vector<std::vector<int>>& labels, std::vector<int>& object_ids, int width, int height, int num_classes, int minArea)
{
  for(int i = 0; i < num_classes; i++)
    labels.push_back( std::vector<int>() );

  // for each pixel
  #pragma omp parallel for
  for(int x = 0; x < width; x++)
  for(int y = 0; y < height; y++)
  {
    int label = label_map[y * width + x];
    labels[label].push_back(y * width + x);
  }

  for(int i = 1; i < num_classes; i++)
  {
    if (labels[i].size() > minArea)
    {
      object_ids.push_back(i);
    }
  }
}


// get 3D bounding boxes
void getBb3Ds(const float* extents, std::vector<std::vector<cv::Point3f>>& bb3Ds, int num_classes)
{
  // for each object
  for (int i = 1; i < num_classes; i++)
  {
    cv::Vec<float, 3> extent;
    extent(0) = extents[i * 3];
    extent(1) = extents[i * 3 + 1];
    extent(2) = extents[i * 3 + 2];

    bb3Ds.push_back(getBB3D(extent));
  }
}


/**
 * @brief Creates a list of samplers that return pixel positions according to probability maps.
 * 
 * This method generates numberOfObjects+1 samplers. The first sampler is a sampler 
 * for accumulated object probabilities. It samples pixel positions according to the 
 * probability of the pixel being any object (1-backgroundProbability). The 
 * following samplers correspond to the probabilities for individual objects.
 * 
 * @param samplers Output parameter. List of samplers.
 * @param probs Probability maps according to which should be sampled. One per object. The accumulated probability will be calculated in this method.
 * @param imageWidth Width of input images.
 * @param imageHeight Height of input images.
 * @return void
*/
void createSamplers(std::vector<Sampler2D>& samplers, const std::vector<jp::img_stat_t>& probs, int imageWidth, int imageHeight)
{	
  samplers.clear();
  jp::img_stat_t objProb = jp::img_stat_t::zeros(imageHeight, imageWidth);
	
  // calculate accumulated probability (any object vs background)
  #pragma omp parallel for
  for(unsigned x = 0; x < objProb.cols; x++)
  for(unsigned y = 0; y < objProb.rows; y++)
  for(auto prob : probs)
    objProb(y, x) += prob(y, x);
	
  // create samplers
  samplers.push_back(Sampler2D(objProb));
  for(auto prob : probs)
    samplers.push_back(Sampler2D(prob));
}


inline cv::Point2f getMode2D(jp::id_t objID, const cv::Point2f& pt, const float* vertmap, int width, int num_classes)
{
  int channel = 2 * objID;
  int offset = channel + 2 * num_classes * (pt.y * width + pt.x);

  jp::coord2_t mode;
  mode(0) = vertmap[offset];
  mode(1) = vertmap[offset + 1];

  return cv::Point2f(mode(0), mode(1));
}


inline bool samplePoint2D(jp::id_t objID, std::vector<cv::Point2f>& eyePts, std::vector<cv::Point2f>& objPts, const cv::Point2f& pt2D, const float* vertmap, int width, int num_classes)
{
  cv::Point2f obj = getMode2D(objID, pt2D, vertmap, width, num_classes); // read out object coordinate

  eyePts.push_back(pt2D);
  objPts.push_back(obj);

  return true;
}


/**
 * @brief Creates a list of pose hypothesis (potentially belonging to multiple objects) which still have to be processed (e.g. refined).
 * 
 * The method includes all remaining hypotheses of an object if there is still more than one, or if there is only one remaining but it still needs to be refined.
 * 
 * @param hypMap Map of object ID to a list of hypotheses for that object.
 * @param maxIt Each hypotheses should be at least this often refined.
 * @return std::vector< Ransac3D::TransHyp*, std::allocator< void > > List of hypotheses to be processed further.
*/
std::vector<TransHyp*> getWorkingQueue(std::map<jp::id_t, std::vector<TransHyp>>& hypMap, int maxIt)
{
  std::vector<TransHyp*> workingQueue;
      
  for(auto it = hypMap.begin(); it != hypMap.end(); it++)
  for(int h = 0; h < it->second.size(); h++)
    if(it->second.size() > 1 || it->second[h].refSteps < maxIt) //exclude a hypothesis if it is the only one remaining for an object and it has been refined enough already
      workingQueue.push_back(&(it->second[h]));

  return workingQueue;
}


inline float point2line(cv::Point2d x, cv::Point2f n, cv::Point2f p)
{
  float n1 = -n.y;
  float n2 = n.x;
  float p1 = p.x;
  float p2 = p.y;
  float x1 = x.x;
  float x2 = x.y;

  return fabs(n1 * (x1 - p1) + n2 * (x2 - p2)) / sqrt(n1 * n1 + n2 * n2);
}


inline void countInliers2D(TransHyp& hyp, const float * vertmap, const std::vector<std::vector<int>>& labels, float inlierThreshold, int width, int num_classes, int pixelBatch)
{
  // reset data of last RANSAC iteration
  hyp.inlierPts2D.clear();
  hyp.inliers = 0;

  hyp.effPixels = 0; // num of pixels drawn
  hyp.maxPixels += pixelBatch; // max num of pixels to be drawn	

  int maxPt = labels[hyp.objID].size(); // num of pixels of this class
  float successRate = hyp.maxPixels / (float) maxPt; // probability to accept a pixel

  std::mt19937 generator;
  std::negative_binomial_distribution<int> distribution(1, successRate); // lets you skip a number of pixels until you encounter the next pixel to accept

  for(unsigned ptIdx = 0; ptIdx < maxPt;)
  {
    int index = labels[hyp.objID][ptIdx];
    cv::Point2d pt2D(index % width, index / width);
  
    hyp.effPixels++;
  
    // read out object coordinate
    cv::Point2d obj = getMode2D(hyp.objID, pt2D, vertmap, width, num_classes);

    // inlier check
    if(point2line(hyp.center, obj, pt2D) < inlierThreshold)
    {
      hyp.inlierPts2D.push_back(std::pair<cv::Point2d, cv::Point2d>(obj, pt2D)); // store object coordinate - camera coordinate correspondence
      hyp.inliers++; // keep track of the number of inliers (correspondences might be thinned out for speed later)
    }

    // advance to the next accepted pixel
    if(successRate < 1)
      ptIdx += std::max(1, distribution(generator));
    else
      ptIdx++;
  }
}


inline void updateHyp2D(TransHyp& hyp, int maxPixels)
{
  if(hyp.inlierPts2D.size() < 4) return;
  filterInliers2D(hyp, maxPixels); // limit the number of correspondences
      
  // data conversion
  cv::Point2d center = hyp.center;
  Hypothesis trans(center);	
	
  // recalculate pose
  trans.calcCenter(hyp.inlierPts2D);
  hyp.center = trans.getCenter();
}


inline void filterInliers2D(TransHyp& hyp, int maxInliers)
{
  if(hyp.inlierPts2D.size() < maxInliers) return; // maximum number not reached, do nothing
      		
  std::vector<std::pair<cv::Point2d, cv::Point2d>> inlierPts; // filtered list of inlier correspondences
	
  // select random correspondences to keep
  for(unsigned i = 0; i < maxInliers; i++)
  {
    int idx = irand(0, hyp.inlierPts2D.size());
	    
    inlierPts.push_back(hyp.inlierPts2D[idx]);
  }
	
  hyp.inlierPts2D = inlierPts;
}


void estimateCenter(const int* labelmap, const float* vertmap, const float* extents, int batch, int height, int width, int num_classes, int preemptive_batch,
  float fx, float fy, float px, float py, std::vector<cv::Vec<float, 13> >& outputs)
{
  // probs
  // std::vector<jp::img_stat_t> probs;
  // getProbs(probability, probs, width, height, num_classes);

  // vertexs
  // std::vector<jp::img_center_t> vertexs;
  // getCenters(vertmap, vertexs, width, height, num_classes);
      
  //set parameters, see documentation of GlobalProperties
  int maxIterations = 10000000;
  float minArea = 400; // a hypothesis covering less projected area (2D bounding box) can be discarded (too small to estimate anything reasonable)
  float inlierThreshold3D = 0.5;
  int ransacIterations = 256;  // 256
  int poseIterations = 100;
  int preemptiveBatch = preemptive_batch;  // 1000
  int maxPixels = 1000;  // 1000
  int refIt = 8;  // 8

  // labels
  std::vector<std::vector<int>> labels;
  std::vector<int> object_ids;
  getLabels(labelmap, labels, object_ids, width, height, num_classes, minArea);

  // bb3Ds
  std::vector<std::vector<cv::Point3f>> bb3Ds;
  getBb3Ds(extents, bb3Ds, num_classes);

  // camera matrix
  cv::Mat_<float> camMat = cv::Mat_<float>::zeros(3, 3);
  camMat(0, 0) = fx;
  camMat(1, 1) = fy;
  camMat(2, 2) = 1.f;
  camMat(0, 2) = px;
  camMat(1, 2) = py;

  if (object_ids.size() == 0)
    return;
	
  int imageWidth = width;
  int imageHeight = height;

  // create samplers for choosing pixel positions according to probability maps
  // std::vector<Sampler2D> samplers;
  // createSamplers(samplers, probs, imageWidth, imageHeight);
		
  // hold for each object a list of pose hypothesis, these are optimized until only one remains per object
  std::map<jp::id_t, std::vector<TransHyp>> hypMap;
	
  // sample initial pose hypotheses
  #pragma omp parallel for
  for(unsigned h = 0; h < ransacIterations; h++)
  for(unsigned i = 0; i < maxIterations; i++)
  {
    // camera coordinate - object coordinate correspondences
    std::vector<cv::Point2f> eyePts;
    std::vector<cv::Point2f> objPts;
	    
    // sample first point and choose object ID
    jp::id_t objID = object_ids[irand(0, object_ids.size())];

    if(objID == 0) continue;

    int pindex = irand(0, labels[objID].size());
    int index = labels[objID][pindex];
    cv::Point2f pt1(index % width, index / width);
    
    // sample first correspondence
    if(!samplePoint2D(objID, eyePts, objPts, pt1, vertmap, width, num_classes))
      continue;

    // sample other points in search radius, discard hypothesis if minimum distance constrains are violated
    pindex = irand(0, labels[objID].size());
    index = labels[objID][pindex];
    cv::Point2f pt2(index % width, index / width);

    if(!samplePoint2D(objID, eyePts, objPts, pt2, vertmap, width, num_classes))
      continue;

    // reconstruct camera
    std::vector<std::pair<cv::Point2d, cv::Point2d>> pts2D;
    for(unsigned j = 0; j < eyePts.size(); j++)
    {
      pts2D.push_back(std::pair<cv::Point2d, cv::Point2d>(
      cv::Point2d(objPts[j].x, objPts[j].y),
      cv::Point2d(eyePts[j].x, eyePts[j].y)
      ));
    }

    Hypothesis trans(pts2D);

    // center
    cv::Point2d center = trans.getCenter();
    
    // create a hypothesis object to store meta data
    TransHyp hyp(objID, center);
    
    #pragma omp critical
    {
      hypMap[objID].push_back(hyp);
    }

    break;
  }

  // create a list of all objects where hypptheses have been found
  std::vector<jp::id_t> objList;
  for(std::pair<jp::id_t, std::vector<TransHyp>> hypPair : hypMap)
  {
    objList.push_back(hypPair.first);
  }

  // create a working queue of all hypotheses to process
  std::vector<TransHyp*> workingQueue = getWorkingQueue(hypMap, refIt);
	
  // main preemptive RANSAC loop, it will stop if there is max one hypothesis per object remaining which has been refined a minimal number of times
  while(!workingQueue.empty())
  {
    // draw a batch of pixels and check for inliers, the number of pixels looked at is increased in each iteration
    #pragma omp parallel for
    for(int h = 0; h < workingQueue.size(); h++)
      countInliers2D(*(workingQueue[h]), vertmap, labels, inlierThreshold3D, width, num_classes, preemptiveBatch);
	    	    
    // sort hypothesis according to inlier count and discard bad half
    #pragma omp parallel for 
    for(unsigned o = 0; o < objList.size(); o++)
    {
      jp::id_t objID = objList[o];
      if(hypMap[objID].size() > 1)
      {
	std::sort(hypMap[objID].begin(), hypMap[objID].end());
	hypMap[objID].erase(hypMap[objID].begin() + hypMap[objID].size() / 2, hypMap[objID].end());
      }
    }
    workingQueue = getWorkingQueue(hypMap, refIt);
	    
    // refine
    #pragma omp parallel for
    for(int h = 0; h < workingQueue.size(); h++)
    {
      updateHyp2D(*(workingQueue[h]), maxPixels);
      workingQueue[h]->refSteps++;
    }
    
    workingQueue = getWorkingQueue(hypMap, refIt);
  }

  #pragma omp parallel for
  for(auto it = hypMap.begin(); it != hypMap.end(); it++)
  for(int h = 0; h < it->second.size(); h++)
  {
    // std::cout << "Estimated Hypothesis for Object " << (int) it->second[h].objID << ":" << std::endl;

    cv::Point2d center = it->second[h].center;
    it->second[h].compute_width_height();

    cv::Vec<float, 13> roi;
    roi(0) = batch;
    roi(1) = it->second[h].objID;
    roi(2) = std::max(center.x - it->second[h].width_ / 2, 0.0);
    roi(3) = std::max(center.y - it->second[h].height_ / 2, 0.0);
    roi(4) = std::min(center.x + it->second[h].width_ / 2, double(width));
    roi(5) = std::min(center.y + it->second[h].height_ / 2, double(height));

    // initial pose estimation
    // 2D bounding box
    cv::Rect bb2D(roi(2), roi(3), roi(4) - roi(2), roi(5) - roi(3));

    // 2D center
    float cx = (roi(2) + roi(4)) / 2;
    float cy = (roi(3) + roi(5)) / 2;

    // backproject the center
    float rx = (cx - px) / fx;
    float ry = (cy - py) / fy;

    // 3D bounding box
    int objID = int(roi(1));
    std::vector<cv::Point3f> bb3D = bb3Ds[objID-1];

    // construct the data
    DataForOpt data;
    data.imageHeight = height;
    data.imageWidth = width;
    data.bb2D = bb2D;
    data.bb3D = bb3D;
    data.camMat = camMat;
    data.rx = rx;
    data.ry = ry;

    // initialize pose
    std::vector<double> vec(6);
    vec[0] = 0.0;
    vec[1] = 0.0;
    vec[2] = 0.0;
    vec[3] = data.rx;
    vec[4] = data.ry;
    vec[5] = 1.0;

    // optimization
    poseWithOpt(vec, data, poseIterations);

    // convert pose to our format
    cv::Mat tvec(3, 1, CV_64F);
    cv::Mat rvec(3, 1, CV_64F);
      
    for(int i = 0; i < 6; i++)
    {
      if(i > 2) 
        tvec.at<double>(i-3, 0) = vec[i];
      else 
        rvec.at<double>(i, 0) = vec[i];
    }
	
    jp::cv_trans_t trans(rvec, tvec);
    jp::jp_trans_t pose = jp::cv2our(trans);

    // use the projected 3D box
    cv::Rect bb2D_proj = getBB2D(width, height, bb3D, camMat, trans);

    roi(2) = bb2D_proj.x - 0.1 * bb2D_proj.width;
    roi(3) = bb2D_proj.y - 0.1 * bb2D_proj.height;
    roi(4) = bb2D_proj.x + 1.1 * bb2D_proj.width;
    roi(5) = bb2D_proj.y + 1.1 * bb2D_proj.height;

    // convert to quarternion
    cv::Mat pose_t;
    cv::transpose(pose.first, pose_t);
    Eigen::Map<Eigen::Matrix3d> eigenT( (double*)pose_t.data );
    Eigen::Quaterniond quaternion(eigenT);

    // assign result
    roi(6) = quaternion.w();
    roi(7) = quaternion.x();
    roi(8) = quaternion.y();
    roi(9) = quaternion.z();
    roi(10) = pose.second.x;
    roi(11) = pose.second.y;
    roi(12) = pose.second.z;

    /*
    std::cout << pose.first << std::endl;
    std::cout << eigenT << std::endl;
    std::cout << quaternion.w() << " " << quaternion.x() << " " << quaternion.y() << " " << quaternion.z() << std::endl;
    std::cout << pose.second << std::endl;
    
    std::cout << "Inliers: " << it->second[h].inliers;
    std::printf(" (Rate: %.1f\%)\n", it->second[h].getInlierRate() * 100);
    std::cout << "Refined " << it->second[h].refSteps << " times. " << std::endl;
    std::cout << "Center " << center << std::endl;
    std::cout << "Width: " << it->second[h].width_ << " Height: " << it->second[h].height_ << std::endl;
    std::cout << "---------------------------------------------------" << std::endl;
    std::cout << roi << std::endl;
    */

    outputs.push_back(roi);

    // add jittering rois
    float x1 = roi(2);
    float y1 = roi(3);
    float x2 = roi(4);
    float y2 = roi(5);
    float w = x2 - x1;
    float h = y2 - y1;

    roi(2) = x1 - 0.05 * w;
    roi(3) = y1 - 0.05 * h;
    roi(4) = roi(2) + w;
    roi(4) = roi(3) + h;
    outputs.push_back(roi);

    roi(2) = x1 + 0.05 * w;
    roi(3) = y1 - 0.05 * h;
    roi(4) = roi(2) + w;
    roi(4) = roi(3) + h;
    outputs.push_back(roi);

    roi(2) = x1 - 0.05 * w;
    roi(3) = y1 + 0.05 * h;
    roi(4) = roi(2) + w;
    roi(4) = roi(3) + h;
    outputs.push_back(roi);

    roi(2) = x1 + 0.05 * w;
    roi(3) = y1 + 0.05 * h;
    roi(4) = roi(2) + w;
    roi(4) = roi(3) + h;
    outputs.push_back(roi);

  }
}


static double optEnergy(const std::vector<double> &pose, std::vector<double> &grad, void *data)
{
  DataForOpt* dataForOpt = (DataForOpt*) data;

  cv::Mat tvec(3, 1, CV_64F);
  cv::Mat rvec(3, 1, CV_64F);
      
  for(int i = 0; i < 6; i++)
  {
    if(i > 2) 
      tvec.at<double>(i-3, 0) = pose[i];
    else 
      rvec.at<double>(i, 0) = pose[i];
  }
	
  jp::cv_trans_t trans(rvec, tvec);

  // project the 3D bounding box according to the current pose
  cv::Rect bb2D = getBB2D(dataForOpt->imageWidth, dataForOpt->imageHeight, dataForOpt->bb3D, dataForOpt->camMat, trans);

  // compute IoU between boxes
  float energy = -1 * getIoU(bb2D, dataForOpt->bb2D);

  return energy;
}


double poseWithOpt(std::vector<double> & vec, DataForOpt data, int iterations) 
{
  // set up optimization algorithm (gradient free)
  nlopt::opt opt(nlopt::LN_NELDERMEAD, 6); 

  // set optimization bounds 
  double rotRange = 180;
  rotRange *= PI / 180;
  double tRangeXY = 0.1;
  double tRangeZ = 0.5; // pose uncertainty is larger in Z direction
	
  std::vector<double> lb(6);
  lb[0] = vec[0]-rotRange; lb[1] = vec[1]-rotRange; lb[2] = vec[2]-rotRange;
  lb[3] = vec[3]-tRangeXY; lb[4] = vec[4]-tRangeXY; lb[5] = vec[5]-tRangeZ;
  opt.set_lower_bounds(lb);
      
  std::vector<double> ub(6);
  ub[0] = vec[0]+rotRange; ub[1] = vec[1]+rotRange; ub[2] = vec[2]+rotRange;
  ub[3] = vec[3]+tRangeXY; ub[4] = vec[4]+tRangeXY; ub[5] = vec[5]+tRangeZ;
  opt.set_upper_bounds(ub);
      
  // configure NLopt
  opt.set_min_objective(optEnergy, &data);
  opt.set_maxeval(iterations);

  // run optimization
  double energy;
  nlopt::result result = opt.optimize(vec, energy);

  // std::cout << "IoU after optimization: " << -energy << std::endl;
   
  return energy;
}


// compute the pose target and weight
void compute_target_weight(float* target, float* weight, const float* poses_gt, int num_gt, int num_classes, std::vector<cv::Vec<float, 13> > outputs)
{
  int num = outputs.size();

  for (int i = 0; i < num; i++)
  {
    cv::Vec<float, 13> roi = outputs[i];
    int batch_id = int(roi(0));
    int class_id = int(roi(1));

    // find the gt index
    int gt_ind = -1;
    for (int j = 0; j < num_gt; j++)
    {
      int gt_batch = int(poses_gt[j * 13 + 0]);
      int gt_id = int(poses_gt[j * 13 + 1]);
      if(class_id == gt_id && batch_id == gt_batch)
      {
        gt_ind = j;
        break;
      }
    }

    if (gt_ind == -1)
      continue;

    target[i * 4 * num_classes + 4 * class_id + 0] = poses_gt[gt_ind * 13 + 6];
    target[i * 4 * num_classes + 4 * class_id + 1] = poses_gt[gt_ind * 13 + 7];
    target[i * 4 * num_classes + 4 * class_id + 2] = poses_gt[gt_ind * 13 + 8];
    target[i * 4 * num_classes + 4 * class_id + 3] = poses_gt[gt_ind * 13 + 9];

    weight[i * 4 * num_classes + 4 * class_id + 0] = 1;
    weight[i * 4 * num_classes + 4 * class_id + 1] = 1;
    weight[i * 4 * num_classes + 4 * class_id + 2] = 1;
    weight[i * 4 * num_classes + 4 * class_id + 3] = 1;

  }
}
