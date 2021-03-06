/*----------------------------------------------------------------------------

  Measure the straightness of a set of points grouped into groups
  that should correspond to aligned points.

  Copyright 2011 rafael grompone von gioi (grompone@gmail.com),
                 Zhongwei Tang (tang@cmla.ens-cachan.fr).

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program. If not, see <http://www.gnu.org/licenses/>.

  ----------------------------------------------------------------------------*/
#include "distCorrection.h"

#include "messager.h"
// libNumerics
#include "matrix.h"
// libImage
#include "correction.h"
#include "spline.h"
// libDistortion
#include "distortionline.h"
// libLineDetection
#include "gaussian_convol_on_curve.h"
#include "straight_edge_points.h"
// libConcurrent
#include "abstractthreadpool.h"
#include "stlCallable.h"
//#include "simplethreadpool.h"
//static concurrent::AbstractThreadPool& DEFAULT_THREAD_POOL = concurrent::SimpleThreadPool::DEFAULT;
#include "qthreadpoolbridge.h"
static concurrent::AbstractThreadPool& DEFAULT_THREAD_POOL = QThreadpoolBridge::DEFAULT;

#include <atomic>
#include <thread>
#include <algorithm>
#include <iostream>
#include <ctime>



/*----------------------------------------------------------------------------*/
/* Static parameters.
 */
static const double sigma = 1.0;
static const double th_low = 1.0;
static const double th_hi = 10.0;
static const double min_length = 100.0;

/* Static parameters for Gaussian convolution along the lines */
static const double unit_sigma = 0.8;
static const double Nsigma = 0.8;
static const bool resampling = true;
static const bool eliminate_border = false;
static const double up_factor = 1.0;

using namespace libNumerics;

/* Measure RMSE of lines on the given image */
template<typename T>
T image_RMSE(int length_thresh, double down_factor, ImageGray<double> &image)
{
    printf("\n Measuring RMSE of lines on the image... \n");
    DistortedLines<T> distLines;
    int w, h;
    ntuple_ll point_set, p;
    double x, y;
    int total_nb_lines, total_threshed_nb_lines, threshed_nb_lines;
    ntuple_list convolved_pts;
    point_set = new_ntuple_ll(1);
    total_nb_lines = 0;
    total_threshed_nb_lines = 0;
    threshed_nb_lines = 0;

    p = straight_edge_points(image, sigma, th_low, th_hi, min_length);
    w = image.xsize();
    h = image.ysize();
    /* copy the points set in the new image to the global set of points */
    for (int j = 0; j < (int)p->size; j++) {
        if ((int)p->list[j]->size > length_thresh) {
            add_ntuple_list(point_set, p->list[j]);
            p->list[j] = 0;
        } else {
            threshed_nb_lines += 1;
        }
    }
    printf("For image there are totally %d lines detected and %d of them are eliminated.\n",
           p->size, threshed_nb_lines);
    free_ntuple_ll(p);

    distLines.pushMemGroup((int)point_set->size);
    int countL = 0;
    for (unsigned int i = 0; i < point_set->size; i++) {
        /* Gaussian convolution and sub-sampling */
        convolved_pts = gaussian_convol_on_curve(unit_sigma, Nsigma, resampling, eliminate_border,
                                                 up_factor, down_factor, point_set->list[i]);
        countL++;
        /* Save points to DistortionLines structure */
        for (unsigned int j = 0; j < convolved_pts->size; j++) {
            x = convolved_pts->values[j*convolved_pts->dim];
            y = convolved_pts->values[j*convolved_pts->dim+1];
            distLines.pushPoint(countL-1, x, y);
        }
    }
    free_ntuple_ll(point_set);
    free_ntuple_list(convolved_pts);

    int order = 3;
    T xp = (T)w/2+0.2, yp = (T)h/2+0.2;
    int sizexy = (order + 1) * (order + 2) / 2;
    vector<T> paramsX = vector<T>::zeros(sizexy);
    paramsX[sizexy-3] = 1;
    vector<T> paramsY = vector<T>::zeros(sizexy);
    paramsY[sizexy-2] = 1;
    T rmse = distLines.RMSE(paramsX, paramsY, order, order, xp, xp);
    T rmse_max = distLines.RMSE_max(paramsX, paramsY, order, order, xp, yp);
    printf("RMSE / maximum RMSE: %12.6g / %12.6g \n", rmse, rmse_max);
    return rmse;
}

using std::atomic_int;
using std::memory_order_relaxed;
using std::memory_order_acquire;


static void correctSegment(const ImageGray<double> *in, ImageGray<double> *out,
                           const Bi<std::vector<double> > *poly_params_inv, const int spline_order,
                           const int startRow, const int rowCount, atomic_int *progress)
{
    libMsg::abortIfAsked();
    const size_t wi = in->xsize(), he = in->ysize();
    const Vector2D origin = { static_cast<double>(wi)/2+0.2, static_cast<double>(he)/2+0.2 };
    for (int y = startRow, i = rowCount; i > 0; y++, i--) {
        for (int x = 0; x < wi; x++) {
            /* do the correction for every pixel */
            Vector2D poisition = undistortPixel(*poly_params_inv, {static_cast<double>(x)-origin.x,
                                                                   static_cast<double>(y)
                                                                   -origin.y});
            double clrR;
            if (!interpolate_spline(*in, spline_order, poisition.x+origin.x, poisition.y+origin.y,
                                    clrR))
                clrR = 0.;
            clrR = std::min(std::max(clrR, 0.), 255.);
            out->pixel(x, y) = clrR;
        }
        progress->fetch_add(1, memory_order_relaxed);
        libMsg::abortIfAsked();
    }
}

const static int TASK_BATCH_SIZE = 100;
/* Given an image and a correction polynomial. Apply it to every pixel and save result to output folder */
static bool correct_image(ImageGray<double> &in, ImageGray<double> &out, int spline_order,
                          const Bi<std::vector<double> > &poly_params_inv,
                          concurrent::AbstractThreadPool& thPool =DEFAULT_THREAD_POOL)
{
    auto startTime = std::chrono::high_resolution_clock::now();

    libMsg::cout<<"\n Undistorted image is being calculated... \n"<<libMsg::endl;
    size_t wi = in.xsize(), he = in.ysize();
    out.resize(wi, he);

    atomic_int progress;

    libMsg::cout<<"Prepare Spline Gray"<<libMsg::endl;
    prepare_spline(in, spline_order);
    // divide image into 100row segments, and correct concurrentlly.
    int row = 0;
    progress.store(0);

    // Lauche MultiTask{
    std::vector<concurrent::Future<void>*> ftrs;//TODO: use move sementics in Future.
    ftrs.clear();
    while (row + TASK_BATCH_SIZE < he) {
        ftrs.push_back(concurrent::asyncInvoke(
                            thPool, &correctSegment, (const ImageGray<double> *)(&in), &out,
                           &poly_params_inv, spline_order, row, TASK_BATCH_SIZE, &progress));

        row += TASK_BATCH_SIZE;
    }
    // correct the rest segment.
    ftrs.push_back(concurrent::asyncInvoke(
                     thPool, &correctSegment, (const ImageGray<double> *)(&in), &out,
                     &poly_params_inv, spline_order, row, (int)(he-row), &progress));
    libMsg::cout<<ftrs.size()<<" Tasks lauched"<<libMsg::endl;
    // }Lauche MultiTask

    //Report progress and wait for all task to finish
    concurrent::ReportProgrsAndWaitFtr(progress,he,ftrs);

    // get futures and handle exceptions in multi-task
    bool allOk;
    concurrent::getFtr_CheckExcpt(allOk,ftrs);
    std::for_each(ftrs.begin(), ftrs.end(), [](concurrent::Future<void>* ftr){ delete ftr; });

    if (allOk) {
        libMsg::cout<<" Done, "<<std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() -startTime).count() *1e-3
                    <<" Seconds spent." << libMsg::endl;
        return true;
    } else {
        return false;
    }
}

static void correctRGBSegment(const ImageRGB<double> *in, ImageRGB<double> *out,
                              const Bi<std::vector<double> > *poly_params_inv,
                              const int spline_order, const int startRow, const int rowCount,
                              atomic_int *progress)
{
    libMsg::abortIfAsked();
    const size_t wi = in->xsize(), he = in->ysize();
    const Vector2D origin = { static_cast<double>(wi)/2+0.2, static_cast<double>(he)/2+0.2 };
    // int lastPercentage = 0;
    for (int y = startRow, i = rowCount; i > 0; y++, i--) {
        for (int x = 0; x < wi; x++) {
            /* do the correction for every pixel */
            Vector2D poisition = undistortPixel(*poly_params_inv, {static_cast<double>(x)-origin.x,
                                                                   static_cast<double>(y)
                                                                   -origin.y});
            double R, G, B;
            if (!interpolate_spline_RGB(*in, spline_order, poisition.x+origin.x,
                                        poisition.y+origin.y, R, G, B))
                R = G = B = 0.;
            R = std::min(std::max(R, 0.), 255.);
            G = std::min(std::max(G, 0.), 255.);
            B = std::min(std::max(B, 0.), 255.);
            out->pixel_R(x, y) = R;
            out->pixel_G(x, y) = G;
            out->pixel_B(x, y) = B;
        }
        progress->fetch_add(1, memory_order_relaxed);
    }
    libMsg::abortIfAsked();
}

static bool correct_image_RGB(ImageRGB<double> &in, ImageRGB<double> &out, int spline_order,
                              const Bi<std::vector<double> > &poly_params_inv,
                              concurrent::AbstractThreadPool& thPool =DEFAULT_THREAD_POOL)
{
    auto startTime = std::chrono::high_resolution_clock::now();

    libMsg::cout<<"\n Undistorted image is being calculated... \n"<<libMsg::endl;
    size_t wi = in.xsize(), he = in.ysize();
    out.resize(wi, he);

    atomic_int progress;


    libMsg::cout<<"Prepare Spline RGB"<<libMsg::endl;
    prepare_spline_RGB(in, spline_order);

    // divide image into 100row segments, and correct concurrentlly.
    int row = 0;
    progress.store(0);

    // Lauche MultiTask{
    std::vector<concurrent::Future<void>*> ftrs;//TODO: use move sementics in Future.
    while (row + TASK_BATCH_SIZE < he) {
        ftrs.push_back(concurrent::asyncInvoke(
                            thPool,&correctRGBSegment,(const ImageRGB<double> *)(&in), &out,
                            &poly_params_inv, spline_order, row, TASK_BATCH_SIZE, &progress));

        row += TASK_BATCH_SIZE;
    }
    // correct the rest segment.
    ftrs.push_back(concurrent::asyncInvoke(
                        thPool, &correctRGBSegment, (const ImageRGB<double> *)(&in), &out,
                        &poly_params_inv, spline_order, row, (int)(he-row), &progress));
    libMsg::cout<<ftrs.size()<<" Tasks lauched"<<libMsg::endl;
    // }Lauche MultiTask

    //Report progress and wait for all task to finish
    concurrent::ReportProgrsAndWaitFtr(progress,he,ftrs);

    // get futures and handle exceptions in multi-task
    bool allOk;
    concurrent::getFtr_CheckExcpt(allOk,ftrs);
//TODO: use move sementics in Future.
    std::for_each(ftrs.begin(), ftrs.end(), [](concurrent::Future<void>* ftr){ delete ftr; });

    if (allOk) {
        libMsg::cout<<" Done, "<<std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() -startTime).count() *1e-3
                    <<" Seconds spent." << libMsg::endl;
        return true;
    } else {
        return false;
    }
}

/* Pop out one character from char array */
char popchar(char *c, int idx, int size)
{
    char res = c[idx];
    for (int i = idx; i < size; i++)
        if (i > idx) c[i-1] = c[i];
    return res;
}

/* Finds an index of coefTerm[] vector based on given x and y degrees. */
int coefIdx(int degree, int x, int y)
{
    int a1 = degree + 1;
    int n = std::abs(x+y-degree)+1;
    int an = x+y+1;
    int Sn = n * (a1 + an);
    Sn /= 2;
    return Sn-an+y;
}

/*----------------------------------------------------------------------------*/
/*                                    Main                                    */
/*----------------------------------------------------------------------------*/
// int main2(int argc, char **argv)
// {
// printf(
// "INFO parameters use: \nlength_threshold sampling_factor <poly_fname> <input.pgm> <output.pgm> \n");
// printf("=======================\n \n");
// int degX, degY;
// printf(" Reading the correction polynomial from file... \n");
// vector<double> poly_params_inv = read_poly<double>(argv[3], degX, degY);
// printf("done. \n");
// int spline_order = 5;
// image_double img = correct_image<double>(argv[4], argv[5], spline_order, poly_params_inv, degX,
// degY);
// image_RMSE<double>(int(atoi(argv[1])), double(atoi(argv[2])), img);
// free_image_double(img);
// return EXIT_SUCCESS;
// }

/*----------------------------------------------------------------------------*/

bool DistortionModule::distortionCorrect_RGB(ImageRGB<double> &in, ImageRGB<double> &out,
                                             const Bi<std::vector<double> > &polynome)
{
    if (!correct_image_RGB(in, out, 5, polynome))
        return false;
    return true;
}

bool DistortionModule::distortionCorrect(ImageGray<double> &in, ImageGray<double> &out,
                                         const Bi<std::vector<double> > &polynome)
{
    // need double
    if (!correct_image(in, out, 5, polynome))
        return false;
    return true;
}

template<typename T>
static bool read_images(DistortedLines<T> &distLines,
                        const std::vector<ImageGray<BYTE> > &imageList, int length_thresh,
                        int down_factor)
{
    int w_tmp = 0, h_tmp = 0;
    ntuple_ll point_set, p;
    ImageGray<double> image;
    // int length_thresh;
    // double down_factor,
    double x, y;
    int w, h;
    int total_nb_lines, total_threshed_nb_lines, threshed_nb_lines;
    ntuple_list convolved_pts;

    libMsg::cout<<"There are "<<static_cast<unsigned>(imageList.size())
                <<" input images.\n The minimal length of lines is set to "<<length_thresh
                <<libMsg::endl;

    /* initialize memory */
    point_set = new_ntuple_ll(1);
    total_nb_lines = 0;
    total_threshed_nb_lines = 0;

    /* process each of the input images */
    for (int i = 0; i < imageList.size(); ++i) {
        threshed_nb_lines = 0;

        /* open image, compute edge points, close it */
        libMsg::cout<<"start convert image "<<i+1<<"..."<<libMsg::endl;
        imageDoubleFromImageBYTE(imageList[i], image);
        p = straight_edge_points(image, sigma, th_low, th_hi, min_length);
        w = image.xsize();
        h = image.ysize();
        if (i == 0) {
            w_tmp = w;
            h_tmp = h;
        } else {
            assert(w == w_tmp && h == h_tmp);
        }

        /* copy the points set in the new image to the global set of points */
        int count = 0;
        for (int j = 0; j < (int)p->size; j++) {
            if ((int)p->list[j]->size > length_thresh) {
                add_ntuple_list(point_set, p->list[j]);
                p->list[j] = 0;
                count++;
            } else {
                threshed_nb_lines += 1;
            }
        }

        distLines.pushMemGroup(count);

        libMsg::cout<<"For image"<<i<<", there are totally "<<p->size<<" lines detected and "
                    <<threshed_nb_lines<<" of them are eliminated.\n"<<libMsg::endl;
        total_nb_lines += p->size;
        total_threshed_nb_lines += threshed_nb_lines;
        free_ntuple_ll(p);
        libMsg::abortIfAsked();
    }
    if(distLines.nLines<=0){
        libMsg::cout<<"Nothing detected in any image. Please check"<<libMsg::endl;
        return false;
    }
    int countL = 0;
    for (unsigned int i = 0; i < point_set->size; i++) {
        /* Gaussian convolution and sub-sampling */
        convolved_pts = gaussian_convol_on_curve(unit_sigma, Nsigma, resampling, eliminate_border,
                                                 up_factor, down_factor, point_set->list[i]);

        /* Save points to DistortionLines structure */
        for (unsigned int j = 0; j < convolved_pts->size; j++) {
            x = convolved_pts->values[j*convolved_pts->dim];
            y = convolved_pts->values[j*convolved_pts->dim+1];
            distLines.pushPoint(countL, x, y);
        }
        countL++;
    }
    libMsg::cout<<"Totally there are "<<total_nb_lines<<" lines detected and "
                <<total_threshed_nb_lines<< " of them are eliminated.\n"<<libMsg::endl;
    /* free memory */
    free_ntuple_ll(point_set);
    free_ntuple_list(convolved_pts);
    return true;
}

template<typename T>
static libNumerics::vector<T> incLMA(DistortedLines<T> &distLines, const int order,
                                     const int inc_order, T xp, T yp)
{
    int sizexy = (order + 1) * (order + 2) / 2;
    vector<T> paramsX = vector<T>::zeros(sizexy);
    paramsX[sizexy-3] = 1;
    vector<T> paramsY = vector<T>::zeros(sizexy);
    paramsY[sizexy-2] = 1;
    T rmse = distLines.RMSE(paramsX, paramsY, order, order, xp, xp);
    T rmse_max = distLines.RMSE_max(paramsX, paramsY, order, order, xp, yp);
    libMsg::cout<<"Point to straight line distance: RMSE / max error distance"<<libMsg::endl;
    libMsg::cout<<"Original photo:"<<libMsg::endl;
    libMsg::cout<<"initial RMSE / maximum: "<<rmse<<" / "<<rmse_max<<" \n"<<libMsg::endl;
    const int beginOrder = 3;
    vector<T> midParams(1);
    for (int i = beginOrder; i <= order; i = i+inc_order) {
        int sizebc = (i+1) * (i+2) / 2;
        vector<T> b = vector<T>::zeros(sizebc);
        vector<T> c = vector<T>::zeros(sizebc);
        vector<int> flagX = vector<int>::ones(sizebc);
        vector<int> flagY = vector<int>::ones(sizebc);
        flagX[sizebc-3] = flagX[sizebc-2] = flagX[sizebc-1] = 0;
        flagY[sizebc-2] = flagY[sizebc-3] = flagY[sizebc-1] = 0;
        if (i == beginOrder) {
            b[sizebc-3] = 1;
            c[sizebc-2] = 1;
        } else {
            int sizebcOld = (i-inc_order+1) * (i-inc_order+2) / 2;
            for (int k = 0; k < sizebcOld; k++) {
                b[sizebc-sizebcOld+k] = midParams[k];
                c[sizebc-sizebcOld+k] = midParams[sizebcOld+k];
            }
        }
        midParams = distLines.correctionLMA(b, c, flagX, flagY, i, i, xp, yp);
        rmse = distLines.RMSE(midParams.copy(0, sizebc-1), midParams.copyRef(sizebc,
                                                                             sizebc+sizebc-1), i, i, xp,
                              yp);
        rmse_max
            = distLines.RMSE_max(midParams.copyRef(0, sizebc-1),
                                 midParams.copyRef(sizebc, sizebc+sizebc-1), i, i, xp, yp);
        libMsg::cout<<"When order = "<<i<<" :"<<libMsg::endl;
        libMsg::cout<<"RMSE / maximum: "<<rmse<<" / "<<rmse_max<<" \n"<<libMsg::endl;
    }
    libMsg::cout<<"\n Iterative linear minimization step: \n"<<libMsg::endl;
    T diff = 100;
    T prevRmse = 1e+16; /* infinity */
    vector<int> flagX = vector<int>::ones(sizexy);
    vector<int> flagY = vector<int>::ones(sizexy);
    flagX[sizexy-3] = flagX[sizexy-1] = flagX[sizexy-2] = 0;
    flagY[sizexy-2] = flagY[sizexy-1] = flagY[sizexy-3] = 0;
    vector<T> estParams(sizexy+sizexy);
    int iter = 0;
    while (diff > 0.001) {
        estParams
            = distLines.verification(midParams.copy(0, sizexy-1), midParams.copy(sizexy,
                                                                                 sizexy+sizexy-1), flagX, flagY, order, order, xp,
                                     yp);
        midParams = estParams;
        rmse = distLines.RMSE(estParams.copy(0, sizexy-1), estParams.copyRef(sizexy,
                                                                             sizexy+sizexy-1), order, order, xp,
                              yp);
        rmse_max
            = distLines.RMSE_max(estParams.copyRef(0, sizexy-1),
                                 estParams.copyRef(sizexy, sizexy+sizexy-1), order, order, xp, yp);
        libMsg::cout<<"initial RMSE / maximum RMSE: "<<rmse<<" / "<<rmse_max<<" \n"<<libMsg::endl;

        diff = fabs(prevRmse - rmse);
        prevRmse = rmse;
        iter++;
    }
    return estParams;
}

template<typename T>
static vector<T> polyInv(const vector<T> &poly_params, const int degX, const int degY, int wi,
                         int he, T xp, T yp)
{
    int sizex = (degX + 1) * (degX + 2) / 2;
    int sizey = (degY + 1) * (degY + 2) / 2;
    return getParamsInv(poly_params.copyRef(0, sizex-1), poly_params.copyRef(sizex,
                                                                             sizex+sizey-1), degX, degY, wi, he, xp,
                        yp);
}

bool DistortionModule::polyEstime(const std::vector<ImageGray<BYTE> > &list,
                                  std::vector<double> &polynome, int order,
                                  std::vector<std::vector<std::vector<std::pair<double,
                                                                                double> > > > &detectedLines)
{
    unsigned int w, h;
    // check: same size for all image
    for (int i = 0; i < list.size(); ++i) {
        if (i == 0) {
            w = list[0].xsize();
            h = list[0].ysize();
        } else {
            if (w != list[i].xsize() || h != list[i].ysize()) {
                libMsg::cout<<"All images harp must have the same size ! But the Image_"<<i+1
                            <<" has the wrong size"<<libMsg::endl;
                return false;
            }
        }
    }
    int min_length = std::min(w, h)*0.3;
    DistortedLines<double> distLines;
    if(!read_images<double>(distLines, list, min_length, 60))
        return false;

    // store lines detected in detectedLines
    int count = 0;
    int nImage = list.size();
    detectedLines.clear();
    detectedLines.resize(nImage);
    for (int i = 0; i < nImage; ++i) {
        std::vector<std::vector<std::pair<double, double> > > &linesInImage = detectedLines[i];
        linesInImage.resize(distLines.nlines4Group[i]);
        for (int j = 0; j < distLines.nlines4Group[i]; ++j) {
            std::vector<std::pair<double, double> > &oneLine = linesInImage[j];
            for (int k = 0; k < distLines._line.at(count+j).sizeLine(); ++k) {
                double x = distLines._line.at(count+j).x(k);
                double y = distLines._line.at(count+j).y(k);
                oneLine.push_back(std::make_pair(x, y));
            }
        }
        count += distLines.nlines4Group[i];
    }

    double xp = (double)w/2+0.2, yp = (double)h/2+0.2; /* +0.2 - to avoid integers */
    const int inc = 2; /* increment; only odd orders will be taken */
    libMsg::cout<<"Calculate Polynomial"<<libMsg::endl;
    vector<double> poly_params = incLMA <double>(distLines, order, inc, xp, yp);

    /* Get an inverse polynomial */
    libMsg::cout<<"Inverse Polynomial ..."<<libMsg::endl;
    vector<double> poly_params_inv = polyInv<double>(poly_params, order, order, w, h, xp, yp);
    polynome.clear();
    for (int i = 0; i < poly_params_inv.size(); ++i)
        polynome.push_back(poly_params_inv[i]);

    return true;
}
