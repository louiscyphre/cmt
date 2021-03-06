#include "CMT.h"

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <iostream>
namespace cmt {

void CMT::initialize(const Mat &im_gray, const Rect &rect, const
std::vector<cv::KeyPoint> &points)
{
    FILE_LOG(logDEBUG) << "CMT::initialize() call";

    //Remember initial image
    context->im_prev = im_gray;

    //Compute center of rect
    center = Point2f(rect.x + rect.width/2.0, rect.y + rect.height/2.0);

    //Initialize rotated bounding box
    context->bb_rot = RotatedRect(center, rect.size(), 0.0);
    context->initialMark = context->bb_rot;

    vector<cv::KeyPoint> keypoints;

    if (points.size() == 0) {
        vector<Point2f> coords2f;
        cv::goodFeaturesToTrack(im_gray, coords2f, MAX_POINTS,
                                POINTS_QUALITY_LEVEL_INIT, MIN_DISTANCE);
        cv::KeyPoint::convert(coords2f, keypoints);
    } else {
        keypoints = points;
    }

    //Divide keypoints into foreground and background keypoints according to selection
    vector<KeyPoint> keypoints_fg;
    vector<KeyPoint> keypoints_bg;

    for (size_t i = 0; i < keypoints.size(); i++)
    {
        KeyPoint k = keypoints[i];
        Point2f pt = k.pt;

        if (pt.x > rect.x && pt.y > rect.y && pt.x < rect.br().x && pt.y < rect.br().y)
        {
            keypoints_fg.push_back(k);
        }

        else
        {
            keypoints_bg.push_back(k);
        }

    }

    //Create foreground classes
    vector<int> classes_fg;
    classes_fg.reserve(keypoints_fg.size());
    for (size_t i = 0; i < keypoints_fg.size(); i++)
    {
        classes_fg.push_back(i);
    }

    //Compute foreground/background features
    Mat descs_fg;
    Mat descs_bg;

    descriptor = DESCRIPTORS_T::create();
    descriptor->compute(im_gray, keypoints_fg, descs_fg);
    descriptor->compute(im_gray, keypoints_bg, descs_bg);

    //Only now is the right time to convert keypoints to points, as compute() might remove some keypoints
    vector<Point2f> points_fg;
    vector<Point2f> points_bg;

    for (size_t i = 0; i < keypoints_fg.size(); i++)
    {
        points_fg.push_back(keypoints_fg[i].pt);
    }

    FILE_LOG(logDEBUG) << points_fg.size() << " foreground points.";

    for (size_t i = 0; i < keypoints_bg.size(); i++)
    {
        points_bg.push_back(keypoints_bg[i].pt);
    }

    //Create normalized points
    vector<Point2f> points_normalized;
    for (size_t i = 0; i < points_fg.size(); i++)
    {
        points_normalized.push_back(points_fg[i] - center);
    }

    //Initialize matcher
    context->matcher.initialize(points_normalized, descs_fg, classes_fg,
                                descs_bg);

    //Initialize consensus
    context->consensus.initialize(points_normalized);

    //Create initial set of active keypoints
    for (size_t i = 0; i < keypoints_fg.size(); i++)
    {
        context->points_active.push_back(keypoints_fg[i].pt);
        context->classes_active = classes_fg;
    }

    FILE_LOG(logDEBUG) << "CMT::initialize() return";
}

void CMT::processFrame(Mat im_gray) {

    FILE_LOG(logDEBUG) << "CMT::processFrame() call";

    //Track keypoints
    vector<Point2f> points_tracked;
    vector<unsigned char> status;
    tracker.track(context->im_prev, im_gray, context->points_active,
                  points_tracked, status);

    FILE_LOG(logDEBUG) << points_tracked.size() << " tracked points.";

    //keep only successful classes
    vector<int> classes_tracked;
    for (size_t i = 0; i < context->classes_active.size(); i++)
    {
        if (status[i])
        {
            classes_tracked.push_back(context->classes_active[i]);
        }

    }
    //Detect keypoints, compute descriptors
    vector<Point2f> coords2f;
    //The more points is better, later we will remove unnecessary points
    cv::goodFeaturesToTrack(im_gray, coords2f, MAX_POINTS,
        POINTS_QUALITY_LEVEL_DETECT, MIN_DISTANCE);
    vector<KeyPoint> keypoints;
    cv::KeyPoint::convert(coords2f, keypoints);

    FILE_LOG(logDEBUG) << keypoints.size() << " keypoints found.";

    Mat descriptors;
    descriptor->compute(im_gray, keypoints, descriptors);

    //Match keypoints globally
    vector<Point2f> points_matched_global;
    vector<int> classes_matched_global;
    context->matcher.matchGlobal(keypoints, descriptors, points_matched_global,
                         classes_matched_global);

    FILE_LOG(logDEBUG) << points_matched_global.size() << " points matched globally.";

    //Fuse tracked and globally matched points
    vector<Point2f> points_fused;
    vector<int> classes_fused;
    fusion.preferFirst(points_tracked, classes_tracked, points_matched_global, classes_matched_global,
            points_fused, classes_fused);

    FILE_LOG(logDEBUG) << points_fused.size() << " points fused.";

    //Estimate scale and rotation from the fused points
    context->consensus.estimateScaleRotation(points_fused, classes_fused, scale, rotation);

    FILE_LOG(logDEBUG) << "scale " << scale << ", " << "rotation " << rotation;

    //Find inliers and the center of their votes
    vector<Point2f> points_inlier;
    vector<int> classes_inlier;
    context->consensus.findConsensus(points_fused, classes_fused, scale, rotation,
            center, points_inlier, classes_inlier);

    FILE_LOG(logDEBUG) << points_inlier.size() << " inlier points.";
    FILE_LOG(logDEBUG) << "center " << center;

    //Match keypoints locally
    vector<Point2f> points_matched_local;
    vector<int> classes_matched_local;
    context->matcher.matchLocal(keypoints, descriptors, center, scale, rotation,
                        points_matched_local, classes_matched_local);

    FILE_LOG(logDEBUG) << points_matched_local.size() << " points matched locally.";

    //Clear active points
    //context->points_active.clear();
    //context->classes_active.clear();
    //Fuse locally matched points and inliers
    //fusion.preferFirst(points_matched_local, classes_matched_local,
    //                  points_inlier, classes_inlier, context->points_active,
    //                   context->classes_active);
    //FIXME//TODO to check, this seems work better
    context->points_active = points_inlier;
    context->classes_active = classes_inlier;

    FILE_LOG(logDEBUG) << context->points_active.size() << " final fused points.";

    //TODO: Use theta to suppress result
    context->bb_rot = RotatedRect(center,  context->initialMark.size * scale,
            rotation/CV_PI * 180);

    //Remember current image
    context->im_prev = im_gray;

    FILE_LOG(logDEBUG) << "CMT::processFrame() return";
}

} /* namespace CMT */
