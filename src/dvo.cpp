// This source code is intended for use in the teaching course "Vision-Based Navigation" at Technical University Munich only.
// Copyright 2015 Robert Maier, Joerg Stueckler, Technical University Munich

#include <sheet3_dvo/dvo.h>

#include <iostream>
#include <fstream>
#include <vector>
#include <sstream>
#include <iomanip>

// #ifndef WIN64
//     #define EIGEN_DONT_ALIGN_STATICALLY
// #endif
#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Cholesky>

#include <opencv2/core/core.hpp>
#include <opencv2/core/eigen.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <sophus/se3.hpp>

#include "ros/ros.h"


#define STR1(x)  #x
#define STR(x)  STR1(x)


#define DEBUG_OUTPUT 0

void convertSE3ToTf(const Eigen::VectorXf &xi, Eigen::Matrix3f &rot, Eigen::Vector3f &t)
{
    // rotation
    Sophus::SE3f se3 = Sophus::SE3f::exp(xi);
    Eigen::Matrix4f mat = se3.matrix();
    rot = mat.topLeftCorner(3, 3);
    t = mat.topRightCorner(3, 1);
}


void convertTfToSE3(const Eigen::Matrix3f &rot, const Eigen::Vector3f &t, Eigen::VectorXf &xi)
{
    Sophus::SE3f se3(rot, t);
    xi = Sophus::SE3f::log(se3);
}



cv::Mat downsampleGray(const cv::Mat &gray)
{
    const float* ptrIn = (const float*)gray.data;
    int w = gray.cols;
    int h = gray.rows;
    int wDown = w/2;
    int hDown = h/2;

    cv::Mat grayDown = cv::Mat::zeros(hDown, wDown, gray.type());
    float* ptrOut = (float*)grayDown.data;
    for (size_t y = 0; y < hDown; ++y)
    {
        for (size_t x = 0; x < wDown; ++x)
        {
            float sum = 0.0f;
            sum += ptrIn[2*y * w + 2*x] * 0.25f;
            sum += ptrIn[2*y * w + 2*x+1] * 0.25f;
            sum += ptrIn[(2*y+1) * w + 2*x] * 0.25f;
            sum += ptrIn[(2*y+1) * w + 2*x+1] * 0.25f;
            ptrOut[y*wDown + x] = sum;
        }
    }

    return grayDown;
}


cv::Mat downsampleDepth(const cv::Mat &depth)
{
    const float* ptrIn = (const float*)depth.data;
    int w = depth.cols;
    int h = depth.rows;
    int wDown = w/2;
    int hDown = h/2;

    // downscaling by averaging the inverse depth
    cv::Mat depthDown = cv::Mat::zeros(hDown, wDown, depth.type());
    float* ptrOut = (float*)depthDown.data;
    for (size_t y = 0; y < hDown; ++y)
    {
        for (size_t x = 0; x < wDown; ++x)
        {
            float d0 = ptrIn[2*y * w + 2*x];
            float d1 = ptrIn[2*y * w + 2*x+1];
            float d2 = ptrIn[(2*y+1) * w + 2*x];
            float d3 = ptrIn[(2*y+1) * w + 2*x+1];

            int cnt = 0;
            float sum = 0.0f;
            if (d0 != 0.0f)
            {
                sum += 1.0f / d0;
                ++cnt;
            }
            if (d1 != 0.0f)
            {
                sum += 1.0f / d1;
                ++cnt;
            }
            if (d2 != 0.0f)
            {
                sum += 1.0f / d2;
                ++cnt;
            }
            if (d3 != 0.0f)
            {
                sum += 1.0f / d3;
                ++cnt;
            }

            if (cnt > 0)
            {
                float dInv = sum / float(cnt);
                if (dInv != 0.0f)
                    ptrOut[y*wDown + x] = 1.0f / dInv;
            }
        }
    }

    return depthDown;
}


bool depthToVertexMap(const Eigen::Matrix3f &K, const cv::Mat &depth, cv::Mat &vertexMap)
{
    int w = depth.cols;
    int h = depth.rows;
    float cx = K(0, 2);
    float cy = K(1, 2);
    float fxInv = 1.0f / K(0, 0);
    float fyInv = 1.0f / K(1, 1);

    vertexMap = cv::Mat::zeros(h, w, CV_32FC3);
    float* ptrVert = (float*)vertexMap.data;
    const float* ptrDepth = (const float*)depth.data;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float depthVal = ptrDepth[y*w + x];
            float x0 = (float(x) - cx) * fxInv;
            float y0 = (float(y) - cy) * fyInv;
            //depthVal = depthVal * std::sqrt(x0*x0 + y0*y0 + 1.0f);

            size_t off = (y*w + x) * 3;
            ptrVert[off] = x0 * depthVal;
            ptrVert[off+1] = y0 * depthVal;
            ptrVert[off+2] = depthVal;
        }
    }

    return true;
}


void transformVertexMap(const Eigen::Matrix3f &R, const Eigen::Vector3f &t, cv::Mat &vertexMap)
{
    int w = vertexMap.cols;
    int h = vertexMap.rows;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            cv::Vec3f pt = vertexMap.at<cv::Vec3f>(y, x);
            if (pt.val[2] == 0.0 || std::isnan(pt.val[2]))
                continue;
            Eigen::Vector3f ptTf(pt.val[0], pt.val[1], pt.val[2]);
            ptTf = R * ptTf + t;
            vertexMap.at<cv::Vec3f>(y, x) = cv::Vec3f(ptTf[0], ptTf[1], ptTf[2]);
        }
    }
}


bool savePlyFile(const std::string &filename, const std::vector<Eigen::Vector3f> &pts, const std::vector<Eigen::Vector3f> &colors)
{
    if (pts.empty())
        return false;

    std::ofstream plyFile;
    plyFile.open(filename.c_str());
    if (!plyFile.is_open())
        return false;

    plyFile << "ply" << std::endl;
    plyFile << "format ascii 1.0" << std::endl;
    plyFile << "element vertex " << pts.size() << std::endl;
    plyFile << "property float x" << std::endl;
    plyFile << "property float y" << std::endl;
    plyFile << "property float z" << std::endl;
    plyFile << "property uchar red" << std::endl;
    plyFile << "property uchar green" << std::endl;
    plyFile << "property uchar blue" << std::endl;
    plyFile << "element face 0" << std::endl;
    plyFile << "property list uchar int vertex_indices" << std::endl;
    plyFile << "end_header" << std::endl;

    for (size_t i = 0; i < pts.size(); i++)
    {
        plyFile << pts[i][0] << " " << pts[i][1] << " " << pts[i][2];
        plyFile << " " << (int)colors[i][0] << " " << (int)colors[i][1] << " " << (int)colors[i][2];
        plyFile << std::endl;
    }
    plyFile.close();

    return true;
}


bool savePlyFile(const std::string &filename, const cv::Mat &color, const cv::Mat &vertexMap)
{
    // convert frame to points vector and colors vector
    std::vector<Eigen::Vector3f> pts;
    std::vector<Eigen::Vector3f> colors;
    for (int y = 0; y < vertexMap.rows; ++y)
    {
        for (int x = 0; x < vertexMap.cols; ++x)
        {
            cv::Vec3f pt = vertexMap.at<cv::Vec3f>(y, x);
            if (pt.val[2] == 0.0 || std::isnan(pt.val[2]))
                continue;
            pts.push_back(Eigen::Vector3f(pt.val[0], pt.val[1], pt.val[2]));

            cv::Vec3b c = color.at<cv::Vec3b>(y, x);
            colors.push_back(Eigen::Vector3f(c.val[2], c.val[1], c.val[0]));
        }
    }

    return savePlyFile(filename, pts, colors);
}


void computeGradient(const cv::Mat &gray, cv::Mat &gradient, int direction)
{
    int dirX = 1;
    int dirY = 0;
    if (direction == 1)
    {
        dirX = 0;
        dirY = 1;
    }

    // compute gradient manually using finite differences
    int w = gray.cols;
    int h = gray.rows;
    const float* ptrIn = (const float*)gray.data;
    gradient = cv::Mat::zeros(h, w, CV_32FC1);
    float* ptrOut = (float*)gradient.data;

    int yStart = dirY;
    int yEnd = h - dirY;
    int xStart = dirX;
    int xEnd = w - dirX;
    for (size_t y = yStart; y < yEnd; ++y)
    {
        for (size_t x = xStart; x < xEnd; ++x)
        {
            float v0;
            float v1;
            if (direction == 1)
            {
                // y-direction
                v0 = ptrIn[(y-1)*w + x];
                v1 = ptrIn[(y+1)*w + x];
            }
            else
            {
                // x-direction
                v0 = ptrIn[y*w + (x-1)];
                v1 = ptrIn[y*w + (x+1)];
            }
            ptrOut[y*w + x] = 0.5f * (v1 - v0);
        }
    }
}


float interpolate(const float* ptrImgIntensity, float x, float y, int w, int h)
{
    float valCur = std::numeric_limits<float>::quiet_NaN();

#if 0
    // direct lookup, no interpolation
    int x0 = static_cast<int>(std::floor(x + 0.5));
    int y0 = static_cast<int>(std::floor(y + 0.5));
    if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h)
        valCur = ptrImgIntensity[y0*w + x0];
#else
    //bilinear interpolation
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    float x1_weight = x - static_cast<float>(x0);
    float y1_weight = y - static_cast<float>(y0);
    float x0_weight = 1.0 - x1_weight;
    float y0_weight = 1.0 - y1_weight;

    if (x0 < 0 || x0 >= w)
        x0_weight = 0.0;
    if (x1 < 0 || x1 >= w)
        x1_weight = 0.0;
    if (y0 < 0 || y0 >= h)
        y0_weight = 0.0;
    if (y1 < 0 || y1 >= h)
        y1_weight = 0.0;
    float w00 = x0_weight * y0_weight;
    float w10 = x1_weight * y0_weight;
    float w01 = x0_weight * y1_weight;
    float w11 = x1_weight * y1_weight;

    float sumWeights = w00 + w10 + w01 + w11;
    float sum = 0.0;
    if (w00 > 0.0)
        sum += ptrImgIntensity[y0*w + x0] * w00;
    if (w01 > 0.0)
        sum += ptrImgIntensity[y1*w + x0] * w01;
    if (w10 > 0.0)
        sum += ptrImgIntensity[y0*w + x1] * w10;
    if (w11 > 0.0)
        sum += ptrImgIntensity[y1*w + x1] * w11;

    if (sumWeights > 0.0)
        valCur = sum / sumWeights;
#endif

    return valCur;
}


float calculateError(const Eigen::VectorXf &residuals)
{
    float error = 0.0;
    int n = residuals.size();
    int numValid = 0;
    for (int i = 0; i < n; ++i)
    {
        if (residuals[i] != 0.0)
        {
            error += residuals[i] * residuals[i];
            ++numValid;
        }
    }
    if (numValid > 0)
        error = error / static_cast<float>(numValid);
    return error;
}


void calculateErrorImage(const Eigen::VectorXf &residuals, int w, int h, cv::Mat &errorImage)
{
    cv::Mat imgResiduals = cv::Mat::zeros(h, w, CV_32FC1);
    float* ptrResiduals = (float*)imgResiduals.data;

    // fill residuals image
    for (size_t y = 0; y < h; ++y)
    {
        for (size_t x = 0; x < w; ++x)
        {
            size_t off = y*w + x;
            if (residuals[off] != 0.0)
                ptrResiduals[off] = residuals[off];
        }
    }

    imgResiduals.convertTo(errorImage, CV_8SC1, 127.0);
}


Eigen::VectorXf calculateError(const cv::Mat &grayRef, const cv::Mat &depthRef,
                                  const cv::Mat &grayCur, const cv::Mat &depthCur,
                                  const Eigen::VectorXf xi , const Eigen::Matrix3f &K)
{
    Eigen::VectorXf residualsVec;

    // create residual image
    int w = grayRef.cols;
    int h = grayRef.rows;

    // camera intrinsics
    float fx = K(0, 0);
    float fy = K(1, 1);
    float cx = K(0, 2);
    float cy = K(1, 2);
    float fxInv = 1.0 / fx;
    float fyInv = 1.0 / fy;

    // convert SE3 to rotation matrix and translation vector
    Eigen::Matrix3f rotMat;
    Eigen::Vector3f t;
    convertSE3ToTf(xi, rotMat, t);

    const float* ptrGrayRef = (const float*)grayRef.data;
    const float* ptrDepthRef = (const float*)depthRef.data;
    const float* ptrGrayCur = (const float*)grayCur.data;
    const float* ptrDepthCur = (const float*)depthCur.data;

    residualsVec.resize(w*h);
    for (size_t y = 0; y < h; ++y)
    {
        for (size_t x = 0; x < w; ++x)
        {
            size_t off = y*w + x;
            float residual = 0.0;

            // project 2d point back into 3d using its depth
            float dRef = ptrDepthRef[y*w + x];
            if (dRef > 0.0)
            {
                float x0 = (static_cast<float>(x) - cx) * fxInv;
                float y0 = (static_cast<float>(y) - cy) * fyInv;
                //dRef = dRef * std::sqrt(x0*x0 + y0*y0 + 1.0);
                x0 = x0 * dRef;
                y0 = y0 * dRef;

                // transform reference 3d point into current frame
                // reference 3d point
                Eigen::Vector3f pt3Ref(x0, y0, dRef);
                Eigen::Vector3f pt3Cur = rotMat * pt3Ref + t;
                if (pt3Cur[2] > 0.0)
                {
                    // project 3d point to 2d
                    Eigen::Vector3f pt2CurH = K * pt3Cur;
                    float px = pt2CurH[0] / pt2CurH[2];
                    float py = pt2CurH[1] / pt2CurH[2];

                    // interpolate residual
                    float valCur = interpolate(ptrGrayCur, px, py, w, h);
                    if (!std::isnan(valCur))
                    {
                        float valRef = ptrGrayRef[off];
                        float valDiff = valRef - valCur;
                        residual = valDiff;
                    }
                }
            }
            residualsVec[off] = residual;
        }
    }

    return residualsVec;
}


void calculateMeanStdDev(const Eigen::VectorXf &residuals, float &mean, float &stdDev)
{
    mean = residuals.mean();

#if 1
    float variance = 0.0;
    for (int i = 0; i < residuals.size(); ++i)
        variance += (residuals[i] - mean) * (residuals[i] - mean);
    stdDev = std::sqrt(variance);
#else
    stdDev = 4.0;
#endif
}


void weighting(Eigen::VectorXf &residuals, Eigen::VectorXf &weights)
{
    int n = residuals.size();

#if 0
    // no weighting
    weights = Eigen::VectorXf::Ones(n);
#if 0
    // squared residuals
    for (int i = 0; i < n; ++i)
        residuals[i] = residuals[i] * residuals[i];
    return;
#endif
#endif

    // compute mean and standard deviation
    float mean, stdDev;
    calculateMeanStdDev(residuals, mean, stdDev);

    // compute robust Huber weights
    float k = 1.345 * stdDev;
    weights = Eigen::VectorXf(n);
    for (int i = 0; i < n; ++i)
    {
        float w;
        if (std::abs(residuals[i]) <= k)
            w = 1.0;
        else
            w = k / std::abs(residuals[i]);
        weights[i] = w;
    }
    //std::cout << W.block(0, 0, 10, 10) << std::endl;

#if 0
    // adjust residuals
    for (int i = 0; i < n; ++i)
    {
        if (std::abs(residuals[i]) <= k)
            residuals[i] = 0.5 * residuals[i] * residuals[i];
        else
            residuals[i] = k * std::abs(residuals[i]) - 0.5*k*k;
    }
#endif
}

Eigen::Matrix4f generateSE3ToTf(const Eigen::VectorXf &xi)
{
	Eigen::Matrix3f rot;
	Eigen::Vector3f t;
  	convertSE3ToTf(xi, rot, t);

    Eigen::Matrix4f Tf = Eigen::Matrix4f::Zero(4,4);

    Tf.block<3,3>(0,0) = rot;
    Tf.block<3,1>(0,3) = t;
    Tf(3,3) = 1;

	return Tf;
}


void deriveNumeric(const cv::Mat &grayRef, const cv::Mat &depthRef,
                                  const cv::Mat &grayCur, const cv::Mat &depthCur,
                                  const Eigen::VectorXf &xi, const Eigen::Matrix3f &K,
                                  Eigen::VectorXf &residuals, Eigen::MatrixXf &J)
{
    residuals = calculateError(grayRef, depthRef, grayCur, depthCur, xi, K);
    J = Eigen::MatrixXf::Zero(grayRef.rows*grayRef.cols, 6);

    float eps = std::numeric_limits<float>::epsilon(); //1e-6;



    for (int i = 0; i < 6; ++i) {
        Eigen::Matrix<float, 6, 1> epsVec = Eigen::Matrix<float, 6, 1>::Zero(6);
        epsVec[i] = eps;


        /*Eigen::Matrix4f tf = generateSE3ToTf(xi)*generateSE3ToTf(epsVec);
        Eigen::Matrix3f rot = tf.block<3, 3>(0, 0);
        Eigen::Vector3f t = tf.block<3, 1>(0, 3);

        Eigen::VectorXf xiPerm;

        convertTfToSE3(rot, t, xiPerm);*/

	    Eigen::VectorXf xiPerm = Sophus::SE3f::log( Sophus::SE3f::exp(xi) * Sophus::SE3f::exp(epsVec) );

        Eigen::VectorXf newResidual = calculateError(grayRef, depthRef, grayCur, depthCur, xiPerm, K);


        J.block(0, i, grayRef.rows*grayRef.cols, 1) =  (newResidual - residuals)/ eps;
    }

    for(int i = 0; i < grayRef.rows*grayRef.cols; ++i) {
        bool toZero = false;

        for (int j = 0; j < 6; ++j) {
            if(std::isnan(J(i,j))){
                toZero = true;
                break;
            }
        }
        if (toZero)
        {
            J.block(i, 0, 1, 6) =  Eigen::MatrixXf::Zero(1, 6);
            residuals[i] =  0.0;
        }
    }

}


void deriveAnalytic(const cv::Mat &grayRef, const cv::Mat &depthRef,
                   const cv::Mat &grayCur, const cv::Mat &depthCur,
                   const cv::Mat &gradX, const cv::Mat &gradY,
                   const Eigen::VectorXf &xi, const Eigen::Matrix3f &K,
                   Eigen::VectorXf &residuals, Eigen::MatrixXf &J)
{
    residuals = calculateError(grayRef, depthRef, grayCur, depthCur, xi, K);

    cv::Mat vertexMap;
  	Eigen::Matrix3f rot;
	Eigen::Vector3f t;
  	convertSE3ToTf(xi, rot, t);

	depthToVertexMap(K, depthRef, vertexMap);
	transformVertexMap(rot, t, vertexMap);

	const int w = vertexMap.cols;
    const int h = vertexMap.rows;

	cv::Mat Ixfx = cv::Mat::zeros(h, w, CV_32FC1);
	cv::Mat Iyfy = cv::Mat::zeros(h, w, CV_32FC1);
	const float* ptrDxIntensity = (const float*)gradX.data;
	const float* ptrDyIntensity = (const float*)gradY.data;

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            cv::Vec3f pt = vertexMap.at<cv::Vec3f>(y, x);
            Eigen::Vector3f pointTemp(pt.val[0], pt.val[1], pt.val[2]);

			if (!(pt.val[2] == 0.0 || std::isnan(pt.val[2])))
			{
                pointTemp = K*pointTemp;
				if ( pt.val[2] > 0.0)
				{
		        	const float px = pointTemp[0]/pointTemp[2];
					const float py = pointTemp[1]/pointTemp[2];

					float valCur = interpolate(ptrDxIntensity, px, py, w, h);
					if (!std::isnan(valCur))
	                {
						Ixfx.at<float>(y, x) = K(0,0)*valCur;
					}
					valCur = interpolate(ptrDyIntensity, px, py, w, h);
					if (!std::isnan(valCur))
	                {
						Iyfy.at<float>(y, x) = K(1,1)*valCur;
					}
				}
			}
		}
	}

	J = Eigen::MatrixXf::Zero(grayRef.rows*grayRef.cols, 6);

    float A, B, C, D, E, F;
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			/*
			A = xp ./ zp
			B = yp ./ zp
			C = Ixfx ./zp
			D = Iyfy ./zp
			E = Iyfy .* A
			F = Ixfx .* B
			*/

            float depth = vertexMap.at<cv::Vec3f>(y, x).val[2];

            if (!(depth == 0.0 || std::isnan(depth)))
            {
                A = vertexMap.at<cv::Vec3f>(y, x).val[0]/depth;
                B = vertexMap.at<cv::Vec3f>(y, x).val[1]/depth;
                C = Ixfx.at<float>(y, x)/depth;
                D = Iyfy.at<float>(y, x)/depth;
                E = Iyfy.at<float>(y, x)*A;
                F = Ixfx.at<float>(y, x)*B;

                J(y*w+x, 0) = C;
                J(y*w+x, 1) = D;
                J(y*w+x, 2) = - (C * A + D * B);
                J(y*w+x, 3) = - (F * A ) -  Iyfy.at<float>(y, x) * (1 + B*B);
                J(y*w+x, 4) =  Ixfx.at<float>(y, x) * (1 + A*A) + (E* B);
                J(y*w+x, 5) = - F + E;
            }
            /*else
            {
                J(y*w+x, 0) = 0;
                J(y*w+x, 1) = 0;
                J(y*w+x, 2) = 0;
                J(y*w+x, 3) = 0;
                J(y*w+x, 4) = 0;
                J(y*w+x, 5) = 0;
            }*/




		}
	}

    for(int i = 0; i < grayRef.rows*grayRef.cols; ++i) {
        bool toZero = false;

        for (int j = 0; j < 6; ++j) {
            if(std::isnan(J(i,j))){
                toZero = true;
                break;
            }
        }
        if (toZero)
        {
            J.block(i, 0, 1, 6) =  Eigen::MatrixXf::Zero(1, 6);
            residuals[i] =  0.0;
        }
    }

	J = -1*J;
}


// expects float images (CV_32FC1), grayscale scaled to [0,1], metrical depth
void alignImages( Eigen::Matrix4f& transform, const cv::Mat& imgGrayRef, const cv::Mat& imgDepthRef, const cv::Mat& imgGrayCur, const cv::Mat& imgDepthCur, const Eigen::Matrix3f& cameraMatrix )
{

    cv::Mat grayRef = imgGrayRef;
    cv::Mat grayCur = imgGrayCur;
    cv::Mat depthRef = imgDepthRef;
    cv::Mat depthCur = imgDepthCur;

    // downsampling
    int numPyramidLevels = 9;
    std::vector<Eigen::Matrix3f> kPyramid;
    kPyramid.push_back(cameraMatrix);
    std::vector<cv::Mat> grayRefPyramid;
    grayRefPyramid.push_back(grayRef);
    std::vector<cv::Mat> depthRefPyramid;
    depthRefPyramid.push_back(depthRef);
    std::vector<cv::Mat> grayCurPyramid;
    grayCurPyramid.push_back(grayCur);
    std::vector<cv::Mat> depthCurPyramid;
    depthCurPyramid.push_back(depthCur);
    for (int i = 1; i < numPyramidLevels; ++i)
    {
        // downsample camera matrix
        Eigen::Matrix3f kDown = kPyramid[i-1];
        kDown(0, 2) += 0.5; kDown(1, 2) += 0.5;
        kDown.topLeftCorner(2, 3) = kDown.topLeftCorner(2, 3) * 0.5;
        kDown(0, 2) -= 0.5; kDown(1, 2) -= 0.5;
        kPyramid.push_back(kDown);
        //std::cout << "Camera matrix (level " << i << "): " << kDown << std::endl;

        // downsample grayscale images
        cv::Mat grayRefDown = downsampleGray(grayRefPyramid[i-1]);
        grayRefPyramid.push_back(grayRefDown);
        cv::Mat grayCurDown = downsampleGray(grayCurPyramid[i-1]);
        grayCurPyramid.push_back(grayCurDown);

        // downsample depth images
        cv::Mat depthRefDown = downsampleDepth(depthRefPyramid[i-1]);
        depthRefPyramid.push_back(depthRefDown);
        cv::Mat depthCurDown = downsampleDepth(depthCurPyramid[i-1]);
        depthCurPyramid.push_back(depthCurDown);
    }

    Eigen::Matrix3f rot;
    Eigen::Vector3f t;
    typedef Eigen::Matrix<float, 6, 6> Mat6f;
    typedef Eigen::Matrix<float, 6, 1> Vec6f;
    Eigen::VectorXf xi = Eigen::VectorXf::Zero( 6 );
    Eigen::VectorXf lastXi = Eigen::VectorXf::Zero( 6 );

//     convertSE3ToTf(xi, rot, t);
    rot = transform.block<3,3>(0,0);
    t = transform.block<3,1>(0,3);
    convertTfToSE3( rot, t, xi );

    std::cout << "Initial pose: " << std::endl;
    std::cout << "t = " << t.transpose() << std::endl;
    std::cout << "R = " << rot << std::endl;


    bool useNumericDerivative = false;

    bool useGN = false;
    bool useGD = false;
    bool useLM = true;
    bool useWeights = true;
    int numIterations = 90;
    int maxLevel = numPyramidLevels-1;
    int minLevel = 1;

    Mat6f A;                      // 6 x 6
    Mat6f diagMatA = Mat6f::Identity();
    Vec6f delta;

    float errorDebug = 0;

    float tmr = (float)cv::getTickCount();
    for (int level = maxLevel; level >= minLevel; level =level-2)
    {
        float lambda = 0.1;

        grayRef = grayRefPyramid[level];
        depthRef = depthRefPyramid[level];
        grayCur = grayCurPyramid[level];
        depthCur = depthCurPyramid[level];
        Eigen::Matrix3f kLevel = kPyramid[level];
        //std::cout << "level " << level << " (size " << depthRef.cols << "x" << depthRef.rows << ")" << std::endl;

        // compute gradient images
        cv::Mat gradX;
        computeGradient(grayCur, gradX, 0);
        cv::Mat gradY;
        computeGradient(grayCur, gradY, 1);

        float errorLast = std::numeric_limits<float>::max();
        for (int itr = 0; itr < numIterations; ++itr)
        {
            // compute residuals and Jacobian
            Eigen::VectorXf residuals;
            Eigen::MatrixXf J;

            if( useNumericDerivative )
              deriveNumeric(grayRef, depthRef, grayCur, depthCur, xi, kLevel, residuals, J);
            else
              deriveAnalytic(grayRef, depthRef, grayCur, depthCur, gradX, gradY, xi, kLevel, residuals, J);

#if DEBUG_OUTPUT
            // compute and show error image
            cv::Mat errorImage;
            calculateErrorImage(residuals, grayRef.cols, grayRef.rows, errorImage);
            cv::imshow("error", errorImage);
            cv::waitKey(30);
#endif

            // calculate error
            float error = calculateError(residuals);

            Eigen::MatrixXf Jt = J.transpose();     // 6 x n
            Eigen::VectorXf weights;                // n x 1
            if (useWeights)
            {
                // compute robust weights
                weighting(residuals, weights);
                if (weights.size() != residuals.size())
                {
                    std::cout << "weight vector has wrong size!" << std::endl;
                    continue;
                }
                residuals = residuals.cwiseProduct(weights);

                // compute weighted Jacobian
                for (int i = 0; i < residuals.size(); ++i)
                    for (int j = 0; j < J.cols(); ++j)
                        J(i, j) = J(i, j) * weights[i];
            }

            // compute update
            Eigen::VectorXf b = Jt * residuals;
            if (useGD)
            {
                // TODO: Implement Gradient Descent (step size 0.001)
				delta = -000.1 * b;
            }

            if (useGN)
            {
                // Gauss-Newton algorithm
                A = Jt * J;
                // solve using Cholesky LDLT decomposition
                delta = -(A.ldlt().solve(b));
            }

            if (useLM)
            {
                // TODO: Implement Levenberg-Marquardt algorithm
				A = Jt * J;
                Vec6f A_diag_vec = A.diagonal();
                Mat6f A_diag = A_diag_vec.asDiagonal();
				A = A + lambda*A_diag;
				delta = -(A.ldlt().solve(b));
            }

            // apply update
            // right-multiplicative increment on SE3
            lastXi = xi;
            xi = Sophus::SE3f::log( Sophus::SE3f::exp(xi) * Sophus::SE3f::exp(delta) );
/*#if DEBUG_OUTPUT
            ROS_ERROR_STREAM( "delta = " << delta.transpose() << " size = " << delta.rows() << " x " << delta.cols() << std::endl;
            std::cout << "xi = " << xi.transpose() << std::endl);
#endif*/

            // compute error again
            error = (residuals.cwiseProduct(residuals)).mean();

            if (useLM)
            {
                if (error >= errorLast)
                {
                    lambda = lambda * 5.0;
                    xi = lastXi;

                    if (lambda > 5.0)
                        break;
                }
                else
                {
                    lambda = lambda / 1.5;
                }
            }

            if (useGN || useGD)
            {
                // break if no improvement (0.99 or 0.995)
                if (error / errorLast > 0.995)
                    break;
            }

            errorLast = error;
            errorDebug = error;
        }
    }
    tmr = ((float)cv::getTickCount() - tmr)/cv::getTickFrequency();
    ROS_ERROR_STREAM( "runtime: " << tmr );
    ROS_ERROR_STREAM( "error: " << errorDebug );

    convertSE3ToTf(xi, rot, t);

    transform.block<3,3>(0,0) = rot;
    transform.block<3,1>(0,3) = t;



#if DEBUG_OUTPUT
    cv::waitKey(30);
//    cv::waitKey(0);
#endif

}
