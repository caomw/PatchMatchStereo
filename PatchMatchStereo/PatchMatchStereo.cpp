
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <vector>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "SLIC.h"

#ifdef _DEBUG
#pragma comment(lib, "opencv_core248d.lib")
#pragma comment(lib, "opencv_highgui248d.lib")
#pragma comment(lib, "opencv_imgproc248d.lib")
#pragma comment(lib, "opencv_features2d248d.lib")
#pragma comment(lib, "opencv_calib3d248d.lib")
#pragma comment(lib, "opencv_video248d.lib")
#pragma comment(lib, "opencv_flann248d.lib")
#else
#pragma comment(lib, "opencv_core248.lib")
#pragma comment(lib, "opencv_highgui248.lib")
#pragma comment(lib, "opencv_imgproc248.lib")
#pragma comment(lib, "opencv_features2d248.lib")
#pragma comment(lib, "opencv_calib3d248.lib")
#pragma comment(lib, "opencv_video248.lib")
#pragma comment(lib, "opencv_flann248.lib")
#endif


#define FRONTAL_PARALLEL_ONLY
#define DO_POST_PROCESSING


// Gobal variables
static int nrows, ncols, ndisps, dmax, scale;
static float BAD_PLANE_PENALTY = 120;  // defined as 2 times the max cost of dsi.
static const int	patch_w = 35;
static const int	patch_r = 17;
static const int	maxiters = 2;
static const float gamma = 10;
static const float gamma_proximity = 25;
static int			g_improve_cnt = 0;

cv::Mat g_L, g_OC, g_ALL, g_DISC, g_GT;


struct Plane {
	float a, b, c;
	float nx, ny, nz;
	Plane() {}
	Plane(float a_, float b_, float c_, float nx_, float ny_, float nz_)
		:a(a_), b(b_), c(c_), nx(nx_), ny(ny_), nz(nz_) {}
	Plane(float nx_, float ny_, float nz_, int y, int x, float z)
	{
		const float eps = 0.001;
		nx = nx_;  ny = ny_;  nz = nz_;
		if (std::abs(nz) < eps) {
			if (nz > 0)  nz = +eps;
			else		 nz = -eps;
		}
		a = -nx / nz;
		b = -ny / nz;
		c = (nx * x + ny * y + nz * z) / nz;
	}
	Plane ReparametrizeInOtherView(int y, int x, int sign, int& qy, int &qx)
	{
		float z = a * x + b * y + c;
		qx = x + sign * z;
		qy = y;
		return Plane(nx, ny, nz, qy, qx, z);
	}
	Plane RandomSearch(int y, int x, float radius_z0, float radius_n)
	{
		const int RAND_HALF = RAND_MAX / 2;

		float nx__ = nx + radius_n * (((double)rand() - RAND_HALF) / RAND_HALF);
		float ny__ = ny + radius_n * (((double)rand() - RAND_HALF) / RAND_HALF);
		float nz__ = nz + radius_n * (((double)rand() - RAND_HALF) / RAND_HALF);

		float z = a * x + b * y + c
			+ radius_z0 * (((double)rand() - RAND_HALF) / RAND_HALF);
		z = std::max(0.f, std::min(z, (float)dmax));
		float norm = std::max(0.01f, sqrt(nx__*nx__ + ny__*ny__ + nz__*nz__));
		nx__ /= norm;
		ny__ /= norm;
		nz__ /= norm;
#ifndef FRONTAL_PARALLEL_ONLY
		return Plane(nx__, ny__, nz__, y, x, z);
#else
		z = (int)(z + 0.5);
		return Plane(0.f, 0.f, 1.f, y, x, z);
#endif
	}
	float ToDisparity(int y, int x) { return a * x + b * y + c; }
	void ToAbc(float *v) { v[0] = a; v[1] = b; v[2] = c; }
	void SetAbc(float *abc)
	{
		a = abc[0];
		b = abc[1];
		c = abc[2];
	}
};

template<class T>
class VECBITMAP {
public:
	T *data;
	int w, h, n;
	bool is_shared;
	T *get(int y, int x) { return &data[(y*w + x)*n]; }		/* Get patch (y, x). */
	T *line_n1(int y) { return &data[y*w]; }				/* Get line y assuming n=1. */
	VECBITMAP() { }
	VECBITMAP(const VECBITMAP& obj)
	{
		// This constructor is very necessary for returning an object in a function,
		// in the case that the Name Return Value Optimization (NRVO) is turned off.
		w = obj.w; h = obj.h; n = obj.n; is_shared = obj.is_shared;
		if (is_shared) { data = obj.data; }
		else { data = new T[w*h*n]; memcpy(data, obj.data, w*h*n*sizeof(T)); }

	}
	VECBITMAP(int h_, int w_, int n_ = 1, T* data_ = NULL)
	{
		w = w_; h = h_; n = n_;
		if (!data_) { data = new T[w*h*n]; is_shared = false; }
		else	    { data = data_;        is_shared = true; }
	}
	~VECBITMAP() { if (!is_shared) delete[] data; }
	T *operator[](int y) { return &data[y*w]; }
};




inline bool InBound(float y, float x) { return 0 <= y && y < nrows && 0 <= x && x < ncols; }

float *PrecomputeWeights(VECBITMAP<unsigned char>& im)
{
	const int nw = nrows * ncols * patch_w * patch_w;
	const int patchsize = patch_w * patch_w;
	float *weights = new float[nw];
	memset(weights, 0, nw * sizeof(float));

	float w_prox[patch_w * patch_w];
	for (int y = 0; y < patch_w; y++) {
		for (int x = 0; x < patch_w; x++) {
			float dist = sqrt((y - patch_r) * (y - patch_r) + (x - patch_r) * (x - patch_r));
			w_prox[y * patch_w + x] = exp(-dist / gamma_proximity);
		}
	}

	for (int yc = 0; yc < nrows; yc++) {
		for (int xc = 0; xc < ncols; xc++) {

			float *w = &weights[(yc*ncols + xc) * patchsize];
			unsigned char *rgb1 = im.get(yc, xc);
			unsigned char *rgb2 = NULL;

			int yb = std::max(0, yc - patch_r), ye = std::min(nrows - 1, yc + patch_r);
			int xb = std::max(0, xc - patch_r), xe = std::min(ncols - 1, xc + patch_r);

			for (int y = yb; y <= ye; y++) {
				for (int x = xb; x <= xe; x++) {
					rgb2 = im.get(y, x);
					float cost = std::abs((float)rgb1[0] - (float)rgb2[0])
						+ std::abs((float)rgb1[1] - (float)rgb2[1])
						+ std::abs((float)rgb1[2] - (float)rgb2[2]);
					w[(y - yc + patch_r) * patch_w + (x - xc + patch_r)] = cost;
				}
			}
		}
	}

	for (int i = 0; i < nw; i++) {
		weights[i] = exp(-weights[i] / gamma);
	}

	return weights;
}

float ComputePlaneCost(int yc, int xc, Plane& coeff_try, VECBITMAP<float>& dsi, VECBITMAP<float>& w)
{
	float cost = 0.0f;
	for (int y = yc - patch_r; y <= yc + patch_r; y++) {
		for (int x = xc - patch_r; x <= xc + patch_r; x++) {
			int d = 0.5 + (coeff_try.a * x + coeff_try.b * y + coeff_try.c);
			if (InBound(y, x)) {
				if (d < 0 || d > dmax) {	// must be a bad plane.
					cost += BAD_PLANE_PENALTY;
				}
				else {
					cost += w[y - yc + patch_r][x - xc + patch_r] * (dsi.get(y, x)[d]);
				}
			}
		}
	}
	return cost;
}

float ComputePlaneCost(int yc, int xc, Plane& coeff_try, VECBITMAP<unsigned char>& imL, VECBITMAP<unsigned char> &imR, VECBITMAP<float>& w, int sign)
{
	float cost = 0.0f;
	for (int y = yc - patch_r; y <= yc + patch_r; y++) {
		for (int x = xc - patch_r; x <= xc + patch_r; x++) {

			float d = (coeff_try.a * x + coeff_try.b * y + coeff_try.c);
			if (InBound(y, x)) {
				if (d < 0 || d > dmax) {	// must be a bad plane.
					cost += BAD_PLANE_PENALTY;
				}
				else {
					float xm = std::max(0.f, std::min((float)ncols - 1, x + sign * d));
					int xmL = (int)xm;
					int xmR = (int)(xm + 0.5);
					float wL = (xmR - xm);
					float wR = 1 - wL;
					float costL = fabs((float)imL.get(y, x)[0] - imR.get(y, xmL)[0])
								+ fabs((float)imL.get(y, x)[1] - imR.get(y, xmL)[1])
								+ fabs((float)imL.get(y, x)[2] - imR.get(y, xmL)[2]);
					float costR = fabs((float)imL.get(y, x)[0] - imR.get(y, xmR)[0])
								+ fabs((float)imL.get(y, x)[1] - imR.get(y, xmR)[1])
								+ fabs((float)imL.get(y, x)[2] - imR.get(y, xmR)[2]);
					
					cost += w[y - yc + patch_r][x - xc + patch_r] * (wL * costL + wR * costR);
				}
			}
		}
	}
	return cost;
}

void RandomInit(VECBITMAP<Plane>& coeffs, VECBITMAP<float>& bestcosts, VECBITMAP<float>& dsi, float* weights)
{
	const int	RAND_HALF = RAND_MAX / 2;
	const float eps = 0.001;

	for (int y = 0; y < nrows; y++) {
		for (int x = 0; x < ncols; x++) {
			// FIXME: Consider using double to avoid numerical issues
			
#ifndef FRONTAL_PARALLEL_ONLY
			float z = dmax * ((double)rand() / RAND_MAX);
			float nx = ((double)rand() - RAND_HALF) / RAND_HALF;
			float ny = ((double)rand() - RAND_HALF) / RAND_HALF;
			float nz = ((double)rand() - RAND_HALF) / RAND_HALF;
			float norm = std::max(0.01f, sqrt(nx*nx + ny*ny + nz*nz));
			nx /= norm;
			ny /= norm;
			nz /= norm;
#else 
			//float z = (int)((4 * ndisps - 1) * ((double)rand() / RAND_MAX) + 0.5);
			//z /= 4.f;
			float z = (int)(dmax * ((double)rand() / RAND_MAX));
			float nx = 0;
			float ny = 0;
			float nz = 1.f;
#endif

			coeffs[y][x] = Plane(nx, ny, nz, y, x, z);
		}
	}

	for (int y = 0; y < nrows; y++) {
		for (int x = 0; x < ncols; x++) {
			bestcosts[y][x] = ComputePlaneCost(y, x, coeffs[y][x], dsi, VECBITMAP<float>(patch_w, patch_w, 1, &weights[(y*ncols + x) * patch_w * patch_w]));
		}
	}
}

void RandomAbc(float *v, int y, int x)
{
	const int	RAND_HALF = RAND_MAX / 2;
	const float eps = 0.001;

#ifndef FRONTAL_PARALLEL_ONLY
	float z = dmax * ((double)rand() / RAND_MAX);
	float nx = ((double)rand() - RAND_HALF) / RAND_HALF;
	float ny = ((double)rand() - RAND_HALF) / RAND_HALF;
	float nz = ((double)rand() - RAND_HALF) / RAND_HALF;
	float norm = std::max(0.01f, sqrt(nx*nx + ny*ny + nz*nz));
	nx /= norm;
	ny /= norm;
	nz /= norm;
#else
	//float z = (int)((4 * ndisps - 1) * ((double)rand() / RAND_MAX) + 0.5);
	//z /= 4.f;
	float z = (int)(dmax * ((double)rand() / RAND_MAX));
	float nx = 0;
	float ny = 0;
	float nz = 1.f;
#endif

	Plane(nx, ny, nz, y, x, z).ToAbc(v);

}

void ImproveGuess(int y, int x, Plane& coeff_old, float& bestcost, Plane& coeff_try, VECBITMAP<float>& dsi, VECBITMAP<float>& w)
{
	float cost = ComputePlaneCost(y, x, coeff_try, dsi, w);
	if (cost < bestcost) {
		g_improve_cnt++;
		bestcost = cost;
		coeff_old = coeff_try;
	}
}

void ImproveGuess(int y, int x, Plane& coeff_old, float& bestcost, Plane& coeff_try, VECBITMAP<unsigned char>& imL, VECBITMAP<unsigned char>& imR, VECBITMAP<float>& w, int sign)
{
	float cost = ComputePlaneCost(y, x, coeff_try, imL, imR, w, sign);
	if (cost < bestcost) {
		g_improve_cnt++;
		bestcost = cost;
		coeff_old = coeff_try;
	}
}

int NelderMeadOptimize(float *, float(*)(float*, int), int);

struct NM_OPT_PARAM {
	int yc, xc;
	Plane coeff;
	VECBITMAP<float> *dsi;
	VECBITMAP<float> *w;
};
NM_OPT_PARAM nm_opt_struct;

float nm_compute_plane_cost(float *abc, int n = 3)
{
	Plane coeff;
	coeff.SetAbc(abc);
	return ComputePlaneCost(nm_opt_struct.yc, nm_opt_struct.xc, coeff, *nm_opt_struct.dsi, *nm_opt_struct.w);
}

void ProcessView(
	VECBITMAP<Plane>& coeffsL,		VECBITMAP<Plane>& coeffsR, 
	VECBITMAP<float>& bestcostsL,	VECBITMAP<float>& bestcostsR, 
	VECBITMAP<float>& dsiL,			VECBITMAP<float>& dsiR, 
	float* weightsL,				float* weightsR,
	int iter, 
	int sign)
{
	int xstart, xend, xchange, ystart, yend, ychange;
	if (iter % 2 == 0) {
		xstart = 0;  xend = ncols;  xchange = +1;
		ystart = 0;  yend = nrows;  ychange = +1;
	}
	else {
		xstart = ncols - 1;  xend = -1;  xchange = -1;
		ystart = nrows - 1;  yend = -1;  ychange = -1;
	}

	const int dx[] = { -1, 0, +1, 0 }; 
	const int dy[] = { 0, -1, 0, +1 };
	float nm_opt_x[3 * 4];

	for (int y = ystart; y != yend; y += ychange) {
		for (int x = xstart; x != xend; x += xchange) {

			VECBITMAP<float> wL(patch_w, patch_w, 1, &weightsL[(y*ncols + x) * patch_w * patch_w]);
#if 1
			// Spatial Propagation
			//int qy = y - ychange, qx = x;
			//if (InBound(qy, qx)) {
			//	Plane coeff_try = coeffsL[qy][qx];
			//	ImproveGuess(y, x, coeffsL[y][x], bestcostsL[y][x], coeff_try, dsiL, wL);
			//}

			//qy = y; qx = x - xchange;
			//if (InBound(qy, qx)) {
			//	Plane coeff_try = coeffsL[qy][qx];
			//	ImproveGuess(y, x, coeffsL[y][x], bestcostsL[y][x], coeff_try, dsiL, wL);
			//}
			int qx, qy;
			for (int dir = 0; dir < 4; dir++) {
				qy = y + dy[dir];
				qx = x + dx[dir];
				if (InBound(qy, qx)) {
					Plane coeff_try = coeffsL[qy][qx];
					ImproveGuess(y, x, coeffsL[y][x], bestcostsL[y][x], coeff_try, dsiL, wL);
				}
			}

			// Random Search
			float radius_z = dmax / 2.0f;
			float radius_n = 1.0f;
			while (radius_z >= 0.1) {
				Plane coeff_try = coeffsL[y][x].RandomSearch(y, x, radius_z, radius_n);
				ImproveGuess(y, x, coeffsL[y][x], bestcostsL[y][x], coeff_try, dsiL, wL);
				radius_z /= 2.0f;
				radius_n /= 2.0f;
			}

			// View Propagation
			Plane coeff_try = coeffsL[y][x].ReparametrizeInOtherView(y, x, sign, qy, qx);
			if (0 <= qx && qx < ncols) {
				VECBITMAP<float> wR(patch_w, patch_w, 1, &weightsR[(qy*ncols + qx) * patch_w * patch_w]);
				ImproveGuess(qy, qx, coeffsR[qy][qx], bestcostsR[qy][qx], coeff_try, dsiR, wR);
			}
#else

			for (int dir = 0; dir < 4; dir++) {
				int qy = y + dy[dir];
				int qx = x + dx[dir];
				if (InBound(qy, qx)) {
					coeffsL[qy][qx].ToAbc(nm_opt_x + 3 * dir);
				}
				else {
					RandomAbc(nm_opt_x + 3 * dir, qy, qx);
				}
			}
			nm_opt_struct.yc = y;
			nm_opt_struct.xc = x;
			nm_opt_struct.w = &wL;
			nm_opt_struct.dsi = &dsiL;
			NelderMeadOptimize(nm_opt_x, nm_compute_plane_cost, 5);
			coeffsL[y][x].SetAbc(nm_opt_x);
#endif
		}
	}
}

void ProcessView(
	VECBITMAP<Plane>& coeffsL, VECBITMAP<Plane>& coeffsR,
	VECBITMAP<float>& bestcostsL, VECBITMAP<float>& bestcostsR,
	VECBITMAP<float>& imL, VECBITMAP<float>& imR,
	float* weightsL, float* weightsR,
	int iter,
	int sign)
{
	int xstart, xend, xchange, ystart, yend, ychange;
	if (iter % 2 == 0) {
		xstart = 0;  xend = ncols;  xchange = +1;
		ystart = 0;  yend = nrows;  ychange = +1;
	}
	else {
		xstart = ncols - 1;  xend = -1;  xchange = -1;
		ystart = nrows - 1;  yend = -1;  ychange = -1;
	}

	const int dx[] = { -1, 0, +1, 0 };
	const int dy[] = { 0, -1, 0, +1 };
	float nm_opt_x[3 * 4];

	for (int y = ystart; y != yend; y += ychange) {
		for (int x = xstart; x != xend; x += xchange) {

			VECBITMAP<float> wL(patch_w, patch_w, 1, &weightsL[(y*ncols + x) * patch_w * patch_w]);
#if 1
			// Spatial Propagation
			//int qy = y - ychange, qx = x;
			//if (InBound(qy, qx)) {
			//	Plane coeff_try = coeffsL[qy][qx];
			//	ImproveGuess(y, x, coeffsL[y][x], bestcostsL[y][x], coeff_try, dsiL, wL);
			//}

			//qy = y; qx = x - xchange;
			//if (InBound(qy, qx)) {
			//	Plane coeff_try = coeffsL[qy][qx];
			//	ImproveGuess(y, x, coeffsL[y][x], bestcostsL[y][x], coeff_try, dsiL, wL);
			//}
			int qx, qy;
			for (int dir = 0; dir < 4; dir++) {
				qy = y + dy[dir];
				qx = x + dx[dir];
				if (InBound(qy, qx)) {
					Plane coeff_try = coeffsL[qy][qx];
					ImproveGuess(y, x, coeffsL[y][x], bestcostsL[y][x], coeff_try, imL, imR, wL, sign);
				}
			}

			// Random Search
			float radius_z = dmax / 2.0f;
			float radius_n = 1.0f;
			while (radius_z >= 0.1) {
				Plane coeff_try = coeffsL[y][x].RandomSearch(y, x, radius_z, radius_n);
				ImproveGuess(y, x, coeffsL[y][x], bestcostsL[y][x], coeff_try, dsiL, wL);
				radius_z /= 2.0f;
				radius_n /= 2.0f;
			}

			// View Propagation
			Plane coeff_try = coeffsL[y][x].ReparametrizeInOtherView(y, x, sign, qy, qx);
			if (0 <= qx && qx < ncols) {
				VECBITMAP<float> wR(patch_w, patch_w, 1, &weightsR[(qy*ncols + qx) * patch_w * patch_w]);
				ImproveGuess(qy, qx, coeffsR[qy][qx], bestcostsR[qy][qx], coeff_try, dsiR, wR);
			}
#else

			for (int dir = 0; dir < 4; dir++) {
				int qy = y + dy[dir];
				int qx = x + dx[dir];
				if (InBound(qy, qx)) {
					coeffsL[qy][qx].ToAbc(nm_opt_x + 3 * dir);
				}
				else {
					RandomAbc(nm_opt_x + 3 * dir, qy, qx);
				}
			}
			nm_opt_struct.yc = y;
			nm_opt_struct.xc = x;
			nm_opt_struct.w = &wL;
			nm_opt_struct.dsi = &dsiL;
			NelderMeadOptimize(nm_opt_x, nm_compute_plane_cost, 5);
			coeffsL[y][x].SetAbc(nm_opt_x);
#endif
		}
	}
}

void WinnerTakesAll(VECBITMAP<float>& dsi, VECBITMAP<float> &disp)
{
	for (int y = 0; y < nrows; y++) {
		for (int x = 0; x < ncols; x++) {
			int minidx = 0;
			float *cost = dsi.get(y, x);
			for (int k = 1; k < ndisps; k++) {
				if (cost[k] < cost[minidx]) {
					minidx = k;
				}
			}
			disp[y][x] = minidx;
		}
	}
}

void PlaneMapToDisparityMap(VECBITMAP<Plane>& coeffs, VECBITMAP<float>& disp)
{
	for (int y = 0; y < nrows; y++) {
		for (int x = 0; x < ncols; x++) {
			disp[y][x] = coeffs[y][x].ToDisparity(y, x);
		}
	}
}

void CrossCheck(VECBITMAP<float>& dispL, VECBITMAP<float>& dispR, VECBITMAP<bool>& validL, VECBITMAP<bool>& validR)
{
	for (int y = 0; y < nrows; y++) {
		for (int x = 0; x < ncols; x++) {

			int xR = std::max(0.f, std::min((float)ncols, x - dispL[y][x]));
			validL[y][x] = (std::abs(dispL[y][x] - dispR[y][xR]) <= 1);

			int xL = std::max(0.f, std::min((float)ncols, x + dispR[y][x]));
			validR[y][x] = (std::abs(dispR[y][x] - dispL[y][xL]) <= 1);
		}
	}
}

void FillHole(int y, int x, VECBITMAP<bool>& valid, VECBITMAP<Plane>& coeffs)
{
	// This function fills the invalid pixel (y,x) by finding its nearst (left and right) 
    // valid neighbors on the same scanline, and select the one with lower disparity.

	int xL = x - 1, xR = x + 1, bestx = x;
	while (!valid[y][xL] && 0 <= xL) {
		xL--;
	}
	while (!valid[y][xR] && xR < ncols) {
		xR++;
	}
	if (0 <= xL) {
		bestx = xL;
	}
	if (xR < ncols) {
		if (bestx == xL) {
			float dL = coeffs[y][xL].ToDisparity(y, x);
			float dR = coeffs[y][xR].ToDisparity(y, x);
			if (dR < dL) {
				bestx = xR;
			}
		}
		else {
			bestx = xR;
		}
	}
	coeffs[y][x] = coeffs[y][bestx];
}

void WeightedMedianFilter(int yc, int xc, VECBITMAP<float>& disp, VECBITMAP<float>& weights, VECBITMAP<bool>& valid, bool useInvalidPixels)
{
	std::vector<std::pair<float, float>> dw_pairs;

	int yb = std::max(0, yc - patch_r), ye = std::min(nrows - 1, yc + patch_r);
	int xb = std::max(0, xc - patch_r), xe = std::min(ncols - 1, xc + patch_r);

	for (int y = yb; y <= ye; y++) {
		for (int x = xb; x <= xe; x++) {
			if (useInvalidPixels || valid[y][x]) {
				std::pair<float, float> dw(disp[y][x], weights[y - yc + patch_r][x - xc + patch_r]);
				dw_pairs.push_back(dw);
			}
		}
	}

	std::sort(dw_pairs.begin(), dw_pairs.end());

	float w = 0.f, wsum = 0.f;
	for (int i = 0; i < dw_pairs.size(); i++) {
		wsum += dw_pairs[i].second;
	}

	for (int i = 0; i < dw_pairs.size(); i++) {
		w += dw_pairs[i].second;
		if (w >= wsum / 2.f) {
			// Note that this line can always be reached.
			if (i > 0) {
				disp[yc][xc] = (dw_pairs[i - 1].first + dw_pairs[i].first) / 2.f;
			}
			else {
				disp[yc][xc] = dw_pairs[i].first;
			}
			break;
		}
	}
}


void PostProcess(
	float *	weightsL,			float *weightsR, 
	VECBITMAP<Plane>& coeffsL,	VECBITMAP<Plane>& coeffsR, 
	VECBITMAP<float>& dispL,	VECBITMAP<float>& dispR)
{
	// This function perform several times of weighted median filtering at each invalid position.
	// weights of the neighborhood are set by exp(-||cp-cq|| / gamma), except in the last iteration,
	// where the weights of invalid pixels are set to zero.

	VECBITMAP<bool> validL(nrows, ncols), validR(nrows, ncols);
	PlaneMapToDisparityMap(coeffsL, dispL);
	PlaneMapToDisparityMap(coeffsR, dispR);
#ifdef DO_POST_PROCESSING
	// Hole filling
	CrossCheck(dispL, dispR, validL, validR);
	//for (int y = 0; y < nrows; y++) {
	//	for (int x = 0; x < ncols; x++) {
	//		if (!validL[y][x]) {
	//			FillHole(y, x, validL, coeffsL);
	//		}
	//		if (!validR[y][x]) {
	//			FillHole(y, x, validR, coeffsR);
	//		}
	//	}
	//}

	// Weighted median filtering 
	int maxround = 1; 
	bool useInvalidPixels = true;
	for (int round = 0; round < maxround; round++) {

		PlaneMapToDisparityMap(coeffsL, dispL);
		PlaneMapToDisparityMap(coeffsR, dispR);
		CrossCheck(dispL, dispR, validL, validR);

		if (round + 1 == maxround) {
			useInvalidPixels = false;
		}

		for (int y = 0; y < nrows; y++) {
			if (y % 10 == 0) { printf("median filtering row %d\n", y); }
			for (int x = 0; x < ncols; x++) {
				if (!validL[y][x]){
					VECBITMAP<float> wL(patch_w, patch_w, 1, &weightsL[(y * ncols + x) * patch_w * patch_w]);
					WeightedMedianFilter(y, x, dispL, wL, validL, useInvalidPixels);
				}
				if (!validR[y][x]){
					VECBITMAP<float> wR(patch_w, patch_w, 1, &weightsR[(y * ncols + x) * patch_w * patch_w]);
					WeightedMedianFilter(y, x, dispR, wR, validR, useInvalidPixels);
				}
			}
		}
	}
#endif
}

void RunPatchMatchStereo(VECBITMAP<unsigned char>& imL, VECBITMAP<unsigned char>& imR, 
						 VECBITMAP<float>& dsiL,		VECBITMAP<float>& dsiR,  
						 VECBITMAP<float>& dispL,		VECBITMAP<float>& dispR)
{
	VECBITMAP<Plane> coeffsL(nrows, ncols), coeffsR(nrows, ncols);
	VECBITMAP<float> bestcostsL(nrows, ncols), bestcostsR(nrows, ncols);


	int tic, toc;
	// Precompute color weights to speed up
	tic = clock();  printf("precomputing weights ...\n");
	float *weightsL = PrecomputeWeights(imL);
	float *weightsR = PrecomputeWeights(imR);
	toc = clock();  printf("precomputing weights use %.2fs\n\n", (toc - tic) / 1000.f);


	// Random initialization
	tic = clock();  printf("performing random init ...\n");
	RandomInit(coeffsL, bestcostsL, dsiL, weightsL);
	RandomInit(coeffsR, bestcostsR, dsiR, weightsR);
	toc = clock();  printf("random init use %.2fs\n\n", (toc - tic) / 1000.f);


	// Iteration
	for (int iter = 0; iter < maxiters; iter++) {

		tic = clock();  printf("iter %d, scaning left view ...\n", iter);
		ProcessView(coeffsL, coeffsR, bestcostsL, bestcostsR, dsiL, dsiR, weightsL, weightsR, iter, -1);
		toc = clock();  printf("%.2fs\n\n", (toc - tic) / 1000.f);

		tic = clock();  printf("iter %d, scaning right view ...\n", iter);
		ProcessView(coeffsR, coeffsL, bestcostsR, bestcostsL, dsiR, dsiL, weightsR, weightsL, iter, +1);
		toc = clock();  printf("%.2fs\n\n", (toc - tic) / 1000.f);
	}

	printf("g_improve_cnt: %d\n", g_improve_cnt);


	// Post processing
	tic = clock();  printf("POST PROCESSING...\n");
	PostProcess(weightsL, weightsR, coeffsL, coeffsR, dispL, dispR);
	toc = clock();  printf("%.2fs\n\n", (toc - tic) / 1000.f);


	if (!weightsL) delete[] weightsL;
	if (!weightsR) delete[] weightsR;
}


using namespace std;
using namespace cv;



unsigned hamdist(long long x, long long  y)
{
	int dist = 0;
	long long val = x ^ y; 
	while (val) {
		++dist;
		val &= val - 1;
	}
	return dist;
}

VECBITMAP<long long> ComputeCensusImage(VECBITMAP<unsigned char> im)
{
	int vpad = 3, hpad = 4;
	VECBITMAP<long long> census(nrows, ncols);
	memset(census.data, 0, nrows * ncols * sizeof(long long));

	for (int yc = 0; yc < nrows; yc++) {
		for (int xc = 0; xc < ncols; xc++) {

			int uu = std::max(yc - vpad, 0);
			int dd = std::min(yc + vpad, nrows - 1);
			int ll = std::max(xc - hpad, 0);
			int rr = std::min(xc + hpad, ncols - 1);

			int idx = 0;
			long long feature = 0;
			unsigned char center = im[yc][xc];

			for (int y = uu; y <= dd; y++) {
				for (int x = ll; x <= rr; x++) {
					feature |= ((long long)(im[y][x] > center) << idx);
					idx++;
				}
			}

			census[yc][xc] = feature;
		}
	}

	return census;
}

VECBITMAP<float> ComputeCensusTensor(VECBITMAP<unsigned char>& imL, VECBITMAP<unsigned char>& imR, int sign)
{
	VECBITMAP<float> dsi(nrows, ncols, ndisps);
	VECBITMAP<long long> censusL = ComputeCensusImage(imL);
	VECBITMAP<long long> censusR = ComputeCensusImage(imR);

	for (int y = 0; y < nrows; y++) {
		for (int x = 0; x < ncols; x++) {
			for (int d = 0; d < ndisps; d++) {
				int xm = (x + sign*d + ncols) % ncols;
				dsi.get(y, x)[d] = hamdist(censusL[y][x], censusR[y][xm]);
			}
		}
	}
	return dsi;
}

void EvaluateDisparity(VECBITMAP<float>& av_disp, const int grayScale, bool bValidOnly, float *badPixelRate, float *validPixelRate)
{
	typedef unsigned char BYTE;
	const int width = ncols, height = nrows;
	float count[3] = { 0, 0, 0 };
	float *h_disp = av_disp.data;

	*validPixelRate = 0;  badPixelRate[0] = 0; badPixelRate[1] = 0; badPixelRate[2] = 0;
	cv::Mat disp(height, width, CV_8UC3), badOnALL(height, width, CV_8UC3), badOnOC(height, width, CV_8UC3), gray;
	cv::cvtColor(g_L, gray, CV_BGR2GRAY);

	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			count[0] += (g_OC.at<cv::Vec3b>(y, x)[0] == 255);
			count[1] += (g_ALL.at<cv::Vec3b>(y, x)[0] == 255);
			count[2] += (g_DISC.at<cv::Vec3b>(y, x)[0] == 255);

			BYTE g = gray.at<BYTE>(y, x);
			badOnALL.at<cv::Vec3b>(y, x) = cv::Vec3b(g, g, g);
			badOnOC.at<cv::Vec3b>(y, x) = cv::Vec3b(g, g, g);
			if (h_disp[y * width + x] >= 0)
			{
				(*validPixelRate)++;

				disp.at<cv::Vec3b>(y, x) = cv::Vec3b(h_disp[y * width + x], h_disp[y * width + x], h_disp[y * width + x]);
				float diff = abs(h_disp[y * width + x] * grayScale - g_GT.at<cv::Vec3b>(y, x)[0]);
				if (g_OC.at<cv::Vec3b>(y, x)[0] == 255 && diff > grayScale)	//non occlusion
				{
					badPixelRate[0]++;					
					badOnOC.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 0, 255);
				}
				if (g_ALL.at<cv::Vec3b>(y, x)[0] == 255 && diff > grayScale)										//all
				{
					badPixelRate[1]++;
					badOnALL.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 0, 255);
				}
				if (g_DISC.at<cv::Vec3b>(y, x)[0] == 255 && diff > grayScale)	//dis-continuous region
				{
					badPixelRate[2]++;
				}				
				//disp.at<cv::Vec3b>(y, x) = disp.at<cv::Vec3b>(y, x) * grayScale;
				disp.at<cv::Vec3b>(y, x) = cv::Vec3b(h_disp[y * width + x] * grayScale, h_disp[y * width + x] * grayScale, h_disp[y * width + x] * grayScale);
			}
			else
			{
				if (!bValidOnly)
				{
					badPixelRate[0]++; badPixelRate[1]++; badPixelRate[2]++;
				}
				badOnALL.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 255, 0);
				badOnOC.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 255, 0);
				disp.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 255, 0);
			}
			if (g_OC.at<cv::Vec3b>(y, x)[0] == 0)	//draw occlusion region
			{
				badOnOC.at<cv::Vec3b>(y, x) = cv::Vec3b(255, 0, 0);
			}
			if (g_ALL.at<cv::Vec3b>(y, x)[0] == 0)	//draw occlusion region
			{
				badOnALL.at<cv::Vec3b>(y, x) = cv::Vec3b(255, 0, 0);
			}		
		}
	}
	
	//free(h_disp);
	for (int i = 0; i < 3; i++)
	{
		badPixelRate[i] /= count[i];
	}
	*validPixelRate = (*validPixelRate) / (float) (width * height);
	printf("validPixelRate: %f%%, badPixelRate: %f%%, %f%%, %f%%\n",
		*validPixelRate * 100.0f, badPixelRate[0] * 100.0f, badPixelRate[1] * 100.0f, badPixelRate[2] * 100.0f);
	if (1)
	{
		cv::Mat compareImg(height * 2, width * 3, CV_8UC3);
		cv::Mat outImg1 = compareImg(cv::Rect(0, 0, width, height)),
			outImg2 = compareImg(cv::Rect(width, 0, width, height)),
			outImg3 = compareImg(cv::Rect(0, height, width, height)),
			outImg4 = compareImg(cv::Rect(width, height, width, height)),
			outImg5 = compareImg(cv::Rect(width * 2, 0, width, height)),
			outImg6 = compareImg(cv::Rect(width * 2, height, width, height));
		g_GT.copyTo(outImg1);	disp.copyTo(outImg2);
		/*g_segments.copyTo(outImg3);*/	badOnOC.copyTo(outImg4);
		g_L.copyTo(outImg5);	badOnALL.copyTo(outImg6);
		cv::imwrite("d:\\disparity.png", disp);
		cv::imwrite("d:\\badOnALL.png", badOnALL);
		cv::imwrite("d:\\badOnOC.png", badOnOC);
		cv::imshow("disparity", compareImg);

		//g_height = height; g_width = width;
		//g_stereo = compareImg;
		//g_ldisp = disp;
		//g_grayScale = grayScale;
		//cv::setMouseCallback("disparity", on_mouse);

		cv::waitKey(0);
	}
}

void TestSuperPixels()
{
	cv::Mat imL = cv::imread("./teddy/im2.png");
	//cv::imshow("imL", imL);
	//cv::waitKey(0);
	nrows = imL.rows;
	ncols = imL.cols;
	cv::Mat argb(nrows, ncols, CV_8UC4);
	assert(argb.isContinuous());
	

	int from_to[] = { -1, 0, 0, 3, 1, 2, 2, 1 };
	cv::mixChannels(&imL, 1, &argb, 1, from_to, 4);

	int width(ncols), height(nrows);
	unsigned int* pbuff = (unsigned int*)argb.data;


	int k = 200;//Desired number of superpixels.
	double m = 20;//Compactness factor. use a value ranging from 10 to 40 depending on your needs. Default is 10
	cv::Mat labelmap(nrows, ncols, CV_32SC1);
	int* klabels = (int*)labelmap.data;
	int numlabels(0);

	SLIC segment;
	segment.DoSuperpixelSegmentation_ForGivenNumberOfSuperpixels(pbuff, width, height, klabels, numlabels, k, m);
	segment.DrawContoursAroundSegments(pbuff, klabels, width, height, 0xff0000);

	int from_to2[] = { 1, 2, 2, 1, 3, 0 };
	cv::mixChannels(&argb, 1, &imL, 1, from_to2, 3);
	imshow("contour", imL);
	cv::waitKey(0);

	printf("number of labels: %d\n", numlabels);

	//if (pbuff) delete[] pbuff;
	if (klabels) delete[] klabels;
}

int main()
{
	int id = 0;

	std::string folders[] = { "tsukuba/", "venus/", "teddy/", "cones/" };
	const int scales[] = { 16, 8, 4, 4 };
	const int drange[] = { 16, 20, 60, 60 };
	scale  = scales[id];
    ndisps = drange[id];
	dmax   = ndisps - 1;

	//TestSuperPixels();
	//return 0;

	Mat cvimL = imread(folders[id] + "im2.png");
	Mat cvimR = imread(folders[id] + "im6.png");
	Mat cvgrayL, cvgrayR;
	cvtColor(cvimL, cvgrayL, CV_BGR2GRAY);
	cvtColor(cvimR, cvgrayR, CV_BGR2GRAY);

	nrows = cvimL.rows;
	ncols = cvimL.cols;

	VECBITMAP<unsigned char> imL(nrows, ncols, 3, cvimL.data);
	VECBITMAP<unsigned char> imR(nrows, ncols, 3, cvimR.data);
	VECBITMAP<unsigned char> grayL(nrows, ncols, 1, cvgrayL.data);
	VECBITMAP<unsigned char> grayR(nrows, ncols, 1, cvgrayR.data);

	VECBITMAP<float> dsiL = ComputeCensusTensor(grayL, grayR, -1);
	VECBITMAP<float> dsiR = ComputeCensusTensor(grayR, grayL, +1);
	VECBITMAP<float> dispL(nrows, ncols);
	VECBITMAP<float> dispR(nrows, ncols);


#if 1
	RunPatchMatchStereo(imL, imR, dsiL, dsiR, dispL, dispR);
#else
	WinnerTakesAll(dsiL, dispL);
	WinnerTakesAll(dsiR, dispR);
#endif
	
	g_L = cv::imread(folders[id] + "im2.png");
	g_GT = cv::imread(folders[id] + "disp2.png");
	g_OC = cv::imread(folders[id] + "nonocc.png");
	g_ALL = cv::imread(folders[id] + "all.png");
	g_DISC = cv::imread(folders[id] + "disc.png");

	float badPixelRate[3], validPixelRate[1];
	EvaluateDisparity(dispL, scale, false, badPixelRate, validPixelRate);

	return 0;
}





void RansacPlanefit(cv::Mat& imL, cv::Mat& imR, VECBITMAP<float>& dsiL, VECBITMAP<float>& dsiR, VECBITMAP<float>& dispL, VECBITMAP<float>& dispR)
{
	nrows = imL.rows;
	ncols = imL.cols;
	cv::Mat argb(nrows, ncols, CV_8UC4);
	cv::Mat labelmap(nrows, ncols, CV_32SC1);
	assert(argb.isContinuous());
	assert(labelmap.isContinuous());

	int from_to[] = { -1, 0, 0, 3, 1, 2, 2, 1 };
	cv::mixChannels(&imL, 1, &argb, 1, from_to, 4);

	int width(ncols), height(nrows), numlabels(0);;
	unsigned int* pbuff = (unsigned int*)argb.data;
	int* klabels = (int*)labelmap.data;

	int		k = 200;	// Desired number of superpixels.
	double	m = 20;		// Compactness factor. use a value ranging from 10 to 40 depending on your needs. Default is 10
	SLIC segment;
	segment.DoSuperpixelSegmentation_ForGivenNumberOfSuperpixels(pbuff, width, height, klabels, numlabels, k, m);
	segment.DrawContoursAroundSegments(pbuff, klabels, width, height, 0xff0000);


	// FIXME: unfinished.
}
