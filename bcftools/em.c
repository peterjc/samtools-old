#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bcf.h"

static double g_q2p[256];

#define ITER_MAX 50
#define ITER_EPS 1e-5

// get the 3 genotype likelihoods
static double *get_pdg3(const bcf1_t *b)
{
	double *pdg;
	const uint8_t *PL = 0;
	int i, PL_len = 0;
	// initialize g_q2p if necessary
	if (g_q2p[0] == 0.)
		for (i = 0; i < 256; ++i)
			g_q2p[i] = pow(10., -i / 10.);
	// set PL and PL_len
	for (i = 0; i < b->n_gi; ++i) {
		if (b->gi[i].fmt == bcf_str2int("PL", 2)) {
			PL = (const uint8_t*)b->gi[i].data;
			PL_len = b->gi[i].len;
			break;
		}
	}
	if (i == b->n_gi) return 0; // no PL
	// fill pdg
	pdg = malloc(3 * b->n_smpl * sizeof(double));
	for (i = 0; i < b->n_smpl; ++i) {
		const uint8_t *pi = PL + i * PL_len;
		double *p = pdg + i * 3;
		p[0] = g_q2p[pi[2]]; p[1] = g_q2p[pi[1]]; p[2] = g_q2p[pi[0]];
	}
	return pdg;
}

static double g3_iter(double g[3], const double *_pdg, int beg, int end)
{
	double err, gg[3];
	int i;
	gg[0] = gg[1] = gg[2] = 0.;
//	printf("%lg,%lg,%lg\n", g[0], g[1], g[2]);
	for (i = beg; i < end; ++i) {
		double sum, tmp[3];
		const double *pdg = _pdg + i * 3;
		tmp[0] = pdg[0] * g[0]; tmp[1] = pdg[1] * g[1]; tmp[2] = pdg[2] * g[2];
		sum = (tmp[0] + tmp[1] + tmp[2]) * (end - beg);
		gg[0] += tmp[0] / sum; gg[1] += tmp[1] / sum; gg[2] += tmp[2] / sum;
	}
	err = fabs(gg[0] - g[0]) > fabs(gg[1] - g[1])? fabs(gg[0] - g[0]) : fabs(gg[1] - g[1]);
	err = err > fabs(gg[2] - g[2])? err : fabs(gg[2] - g[2]);
	g[0] = gg[0]; g[1] = gg[1]; g[2] = gg[2];
	return err;
}

// x[0..2]: alt-alt, alt-ref, ref-ref frequency
// x[3]: ref frequency
int bcf_em1(const bcf1_t *b, int flag, int n1, double x[10])
{
	double *pdg, *g = x;
	int i, n, n2, gcnt[3], tmp1;
	if (b->n_alleles < 2) return -1; // one allele only
	// initialization
	n = b->n_smpl; n2 = n - n1;
	pdg = get_pdg3(b);
	// get a rough estimate of the genotype frequency
	gcnt[0] = gcnt[1] = gcnt[2] = 0;
	for (i = 0; i < n; ++i) {
		double *p = pdg + i * 3;
		if (p[0] != 1. || p[1] != 1. || p[2] != 1.) {
			int which = p[0] > p[1]? 0 : 1;
			which = p[which] > p[2]? which : 2;
			++gcnt[which];
		}
	}
	tmp1 = gcnt[0] + gcnt[1] + gcnt[2];
	if (tmp1 == 0) return -1; // no data
	for (i = 0; i < 3; ++i) g[i] = (double)gcnt[i] / tmp1;
	{ // estimate the genotype frequency
		for (i = 0; i < ITER_MAX; ++i)
			if (g3_iter(g, pdg, 0, n) < ITER_EPS) break;
	}
	if (flag & 1<<3) {
		double f = g[1] + g[2] * 2;
		
	}
	// free
	free(pdg);
	return 0;
}

#define _G1(h, k) ((h>>1&1) + (k>>1&1))
#define _G2(h, k) ((h&1) + (k&1))

// 0: the previous site; 1: the current site
static int freq_iter(int n, double *pdg[2], double f[4])
{
	double ff[4];
	int i, k, h;
	memset(ff, 0, 4 * sizeof(double));
	for (i = 0; i < n; ++i) {
		double *p[2], sum, tmp;
		p[0] = pdg[0] + i * 3; p[1] = pdg[1] + i * 3;
		for (k = 0, sum = 0.; k < 4; ++k)
			for (h = 0; h < 4; ++h)
				sum += f[k] * f[h] * p[0][_G1(k,h)] * p[1][_G2(k,h)];
		for (k = 0; k < 4; ++k) {
			tmp = f[0] * (p[0][_G1(0,k)] * p[1][_G2(0,k)] + p[0][_G1(k,0)] * p[1][_G2(k,0)])
				+ f[1] * (p[0][_G1(1,k)] * p[1][_G2(1,k)] + p[0][_G1(k,1)] * p[1][_G2(k,1)])
				+ f[2] * (p[0][_G1(2,k)] * p[1][_G2(2,k)] + p[0][_G1(k,2)] * p[1][_G2(k,2)])
				+ f[3] * (p[0][_G1(3,k)] * p[1][_G2(3,k)] + p[0][_G1(k,3)] * p[1][_G2(k,3)]);
			ff[k] += f[k] * tmp / sum;
		}
	}
	for (k = 0; k < 4; ++k) f[k] = ff[k] / (2 * n);
	return 0;
}

double bcf_pair_freq(const bcf1_t *b0, const bcf1_t *b1, double f[4])
{
	const bcf1_t *b[2];
	int i, j, n_smpl;
	double *pdg[2], flast[4], r;
	// initialize others
	if (b0->n_smpl != b1->n_smpl) return -1; // different number of samples
	n_smpl = b0->n_smpl;
	b[0] = b0; b[1] = b1;
	f[0] = f[1] = f[2] = f[3] = -1.;
	if (b[0]->n_alleles < 2 || b[1]->n_alleles < 2) return -1; // one allele only
	pdg[0] = get_pdg3(b0); pdg[1] = get_pdg3(b1);
	if (pdg[0] == 0 || pdg[1] == 0) {
		free(pdg[0]); free(pdg[1]);
		return -1;
	}
	// iteration
	f[0] = f[1] = f[2] = f[3] = 0.25; // this is a really bad guess...
	for (j = 0; j < ITER_MAX; ++j) {
		double eps = 0;
		memcpy(flast, f, 4 * sizeof(double));
		freq_iter(n_smpl, pdg, f);
		for (i = 0; i < 4; ++i) {
			double x = fabs(f[i] - flast[i]);
			if (x > eps) eps = x;
		}
		if (eps < ITER_EPS) break;
	}
	// free
	free(pdg[0]); free(pdg[1]);
	{ // calculate r^2
		double p[2], q[2], D;
		p[0] = f[0] + f[1]; q[0] = 1 - p[0];
		p[1] = f[0] + f[2]; q[1] = 1 - p[1];
		D = f[0] * f[3] - f[1] * f[2];
		r = sqrt(D * D / (p[0] * p[1] * q[0] * q[1]));
		// fprintf(stderr, "R(%lf,%lf,%lf,%lf)=%lf\n", f[0], f[1], f[2], f[3], r2);
		if (isnan(r)) r = -1.;
	}
	return r;
}
