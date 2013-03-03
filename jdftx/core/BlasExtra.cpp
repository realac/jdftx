/*-------------------------------------------------------------------
Copyright 2011 Ravishankar Sundararaman

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <core/BlasExtra.h>
#include <cstring>

void eblas_lincomb_sub(int iMin, int iMax,
	const complex& sX, const complex* X, const int incX,
	const complex& sY, const complex* Y, const int incY,
	complex* Z, const int incZ)
{	for(int i=iMin; i<iMax; i++) Z[i*incZ] = sX*X[i*incX] + sY*Y[i*incY];
}
void eblas_lincomb(const int N,
	const complex& sX, const complex* X, const int incX,
	const complex& sY, const complex* Y, const int incY,
	complex* Z, const int incZ)
{	if(incZ==0) die("incZ cannot be = 0")
	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0, //force single threaded for small problem sizes
		eblas_lincomb_sub, N, sX, X, incX, sY, Y, incY, Z, incZ);
}


void eblas_zgemm_sub(size_t iMin, size_t iMax,
	const CBLAS_TRANSPOSE TransA, const CBLAS_TRANSPOSE TransB, const int M, const int N, const int K,
	const complex* alpha, const complex *A, const int lda, const complex *B, const int ldb,
	const complex* beta, complex *C, const int ldc)
{
	int Msub, Nsub; const complex *Asub, *Bsub; complex *Csub;
	if(M>N)
	{	Msub = iMax-iMin;
		Nsub = N;
		Asub = A+iMin*(TransA==CblasNoTrans ? 1 : lda);
		Bsub = B;
		Csub = C+iMin;
	}
	else
	{	Msub = M;
		Nsub = iMax-iMin;
		Asub = A;
		Bsub = B+iMin*(TransB==CblasNoTrans ? ldb : 1);
		Csub = C+iMin*ldc;
	}
	cblas_zgemm(CblasColMajor, TransA, TransB, Msub, Nsub, K, alpha, Asub, lda, Bsub, ldb, beta, Csub, ldc);
}
void eblas_zgemm(
	const CBLAS_TRANSPOSE TransA, const CBLAS_TRANSPOSE TransB, const int M, const int N, const int K,
	const complex& alpha, const complex *A, const int lda, const complex *B, const int ldb,
	const complex& beta, complex *C, const int ldc)
{
	threadLaunch(threadOperators ? 0 : 1, //disable threads if called from threaded top-level code
		eblas_zgemm_sub, std::max(M,N), //parallelize along larger dimension of output
 		TransA, TransB, M, N, K, &alpha, A, lda, B, ldb, &beta, C, ldc);
}


template<typename scalar> void eblas_scatter_axpy_sub(size_t iStart, size_t iStop, double a, const int* index, const scalar* x, scalar* y)
{	for(size_t i=iStart; i<iStop; i++) y[index[i]] += a * x[i];
}
template<typename scalar> void eblas_scatter_axpy(const int Nindex, double a, const int* index, const scalar* x, scalar* y)
{	threadLaunch((Nindex<100000 || (!threadOperators)) ? 1 : 0,  //force single threaded for small problem sizes
		eblas_scatter_axpy_sub<scalar>, Nindex, a, index, x, y);
}
void eblas_scatter_zdaxpy(const int Nindex, double a, const int* index, const complex* x, complex* y) { eblas_scatter_axpy<complex>(Nindex, a, index, x, y); }
void eblas_scatter_daxpy(const int Nindex, double a, const int* index, const double* x, double* y) { eblas_scatter_axpy<double>(Nindex, a, index, x, y); }


template<typename scalar> void eblas_gather_axpy_sub(size_t iStart, size_t iStop, double a, const int* index, const scalar* x, scalar* y)
{	for(size_t i=iStart; i<iStop; i++) y[i] += a * x[index[i]];
}
template<typename scalar> void eblas_gather_axpy(const int Nindex, double a, const int* index, const scalar* x, scalar* y)
{	threadLaunch((Nindex<100000 || (!threadOperators)) ? 1 : 0,  //force single threaded for small problem sizes
		eblas_gather_axpy_sub<scalar>, Nindex, a, index, x, y);
}
void eblas_gather_zdaxpy(const int Nindex, double a, const int* index, const complex* x, complex* y) { eblas_gather_axpy<complex>(Nindex, a, index, x, y); }
void eblas_gather_daxpy(const int Nindex, double a, const int* index, const double* x, double* y) { eblas_gather_axpy<double>(Nindex, a, index, x, y); }


void eblas_accumNorm_sub(size_t iStart, size_t iStop, const double& a, const complex* x, double* y)
{	for(size_t i=iStart; i<iStop; i++) y[i] += a * x[i].norm();
}
void eblas_accumNorm(int N, const double& a, const complex* x, double* y)
{	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0, //force single threaded for small problem sizes
		eblas_accumNorm_sub, N, a, x, y);
}


template<typename scalar> void eblas_symmetrize_sub(size_t iStart, size_t iStop, int n, const int* symmIndex, scalar* x)
{	double nInv = 1./n;
	for(size_t i=iStart; i<iStop; i++)
	{	scalar xSum=0.0;
		for(int j=0; j<n; j++) xSum += x[symmIndex[n*i+j]];
		xSum *= nInv; //average n in the equivalence class
		for(int j=0; j<n; j++) x[symmIndex[n*i+j]] = xSum;
	}
}
template<typename scalar> void eblas_symmetrize(int N, int n, const int* symmIndex, scalar* x)
{	threadLaunch((N*n<10000 || (!threadOperators)) ? 1 : 0, //force single threaded for small problem sizes
		eblas_symmetrize_sub<scalar>, N, n, symmIndex, x);
}
void eblas_symmetrize(int N, int n, const int* symmIndex, double* x) { eblas_symmetrize<double>(N, n, symmIndex, x); }
void eblas_symmetrize(int N, int n, const int* symmIndex, complex* x) { eblas_symmetrize<complex>(N, n, symmIndex, x); }


//BLAS-1 threaded wrappers
template<typename T>
void eblas_zero_sub(size_t iStart, size_t iStop, T* x)
{	memset(x+iStart, 0, (iStop-iStart)*sizeof(T));
}
void eblas_zero(int N, complex* x)
{	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0,
		eblas_zero_sub<complex>, N, x);
}
void eblas_zero(int N, double* x)
{	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0,
		eblas_zero_sub<double>, N, x);
}

void eblas_zscal_sub(size_t iStart, size_t iStop, const complex* a, complex* x, int incx)
{	cblas_zscal(iStop-iStart, a, x+incx*iStart, incx);
}
void eblas_zscal(int N, const complex& a, complex* x, int incx)
{	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0, 
		eblas_zscal_sub, N, &a, x, incx);
}
void eblas_zdscal_sub(size_t iStart, size_t iStop, double a, complex* x, int incx)
{	cblas_zdscal(iStop-iStart, a, x+incx*iStart, incx);
}
void eblas_zdscal(int N, double a, complex* x, int incx)
{	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0, 
		eblas_zdscal_sub, N, a, x, incx);
}
void eblas_dscal_sub(size_t iStart, size_t iStop, double a, double* x, int incx)
{	cblas_dscal(iStop-iStart, a, x+incx*iStart, incx);
}
void eblas_dscal(int N, double a, double* x, int incx)
{	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0,
		eblas_dscal_sub, N, a, x, incx);
}


void eblas_zaxpy_sub(size_t iStart, size_t iStop, const complex* a, const complex* x, int incx, complex* y, int incy)
{	cblas_zaxpy(iStop-iStart, a, x+incx*iStart, incx, y+incy*iStart, incy);
}
void eblas_zaxpy(int N, const complex& a, const complex* x, int incx, complex* y, int incy)
{	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0,
		eblas_zaxpy_sub, N, &a, x, incx, y, incy);
}
void eblas_daxpy_sub(size_t iStart, size_t iStop, double a, const double* x, int incx, double* y, int incy)
{	cblas_daxpy(iStop-iStart, a, x+incx*iStart, incx, y+incy*iStart, incy);
}
void eblas_daxpy(int N, double a, const double* x, int incx, double* y, int incy)
{	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0,
		eblas_daxpy_sub, N, a, x, incx, y, incy);
}


void eblas_zdotc_sub(size_t iStart, size_t iStop, const complex* x, int incx, const complex* y, int incy,
	complex* ret, std::mutex* lock)
{	//Compute this thread's contribution:
	complex retSub;
	cblas_zdotc_sub(iStop-iStart, x+incx*iStart, incx, y+incy*iStart, incy, &retSub);
	//Accumulate over threads (need sync):
	lock->lock();
	*ret += retSub;
	lock->unlock();
}
complex eblas_zdotc(int N, const complex* x, int incx, const complex* y, int incy)
{	complex ret = 0.;
	std::mutex lock;
	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0,
		eblas_zdotc_sub, N, x, incx, y, incy, &ret, &lock);
	return ret;
}

void eblas_ddot_sub(size_t iStart, size_t iStop, const double* x, int incx, const double* y, int incy,
	double* ret, std::mutex* lock)
{	//Compute this thread's contribution:
	double retSub = cblas_ddot(iStop-iStart, x+incx*iStart, incx, y+incy*iStart, incy);
	//Accumulate over threads (need sync):
	lock->lock();
	*ret += retSub;
	lock->unlock();
}
double eblas_ddot(int N, const double* x, int incx, const double* y, int incy)
{	double ret = 0.;
	std::mutex lock;
	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0,
		eblas_ddot_sub, N, x, incx, y, incy, &ret, &lock);
	return ret;
}

void eblas_dznrm2_sub(size_t iStart, size_t iStop, const complex* x, int incx, double* ret, std::mutex* lock)
{	//Compute this thread's contribution:
	double retSub = cblas_dznrm2(iStop-iStart, x+incx*iStart, incx);
	//Accumulate over threads (need sync):
	lock->lock();
	*ret += retSub*retSub;
	lock->unlock();
}
double eblas_dznrm2(int N, const complex* x, int incx)
{	double ret = 0.;
	std::mutex lock;
	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0, eblas_dznrm2_sub, N, x, incx, &ret, &lock);
	return sqrt(ret);
}

void eblas_dnrm2_sub(size_t iStart, size_t iStop, const double* x, int incx, double* ret, std::mutex* lock)
{	//Compute this thread's contribution:
	double retSub = cblas_dnrm2(iStop-iStart, x+incx*iStart, incx);
	//Accumulate over threads (need sync):
	lock->lock();
	*ret += retSub*retSub;
	lock->unlock();
}
double eblas_dnrm2(int N, const double* x, int incx)
{	double ret = 0.;
	std::mutex lock;
	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0, eblas_dnrm2_sub, N, x, incx, &ret, &lock);
	return sqrt(ret);
}



//Min-max:
void eblas_capMinMax_sub(size_t iStart, size_t iStop,
	double* x, double* xMin, double* xMax, double capLo, double capHi, std::mutex* lock)
{	double xMinLoc = +DBL_MAX;
	double xMaxLoc = -DBL_MAX;
	for(size_t i=iStart; i<iStop; i++)
	{	if(x[i]<xMinLoc) xMinLoc=x[i];
		if(x[i]>xMaxLoc) xMaxLoc=x[i];
		if(x[i]<capLo) x[i]=capLo;
		if(x[i]>capHi) x[i]=capHi;
	}
	lock->lock();
	if(xMinLoc<*xMin) *xMin=xMinLoc;
	if(xMaxLoc>*xMax) *xMax=xMaxLoc;
	lock->unlock();
}
void eblas_capMinMax(const int N, double* x, double& xMin, double& xMax, double capLo, double capHi)
{	xMin = +DBL_MAX;
	xMax = -DBL_MAX;
	std::mutex lock;
	threadLaunch((N<100000 || (!threadOperators)) ? 1 : 0, //force single threaded for small problem sizes
		eblas_capMinMax_sub, N, x, &xMin, &xMax, capLo, capHi, &lock);
}
