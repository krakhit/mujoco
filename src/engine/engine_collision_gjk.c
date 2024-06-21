// Copyright 2024 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "engine/engine_collision_gjk.h"

#include <stddef.h>

#include <mujoco/mjtnum.h>
#include "engine/engine_util_blas.h"
#include "engine/engine_util_spatial.h"
#include "engine/engine_collision_convex.h"

// Computes the shortest distance between the origin and an n-simplex (n <= 3) and returns the
// barycentric coordinates of the closest point in the simplex. This is the so called distance
// sub-algorithm of the original 1988 GJK algorithm.
//
//  We have adapted the Signed Volume method for our approach from the paper:
//     Improving the GJK Algorithm for Faster and More Reliable Distance Queries Between Two
//     Convex Objects, Montanari et al, ToG 2017.
static void signedVolume(mjtNum lambda[4], const mjtNum simplex[12], int n);

// these internal functions compute the barycentric coordinates of the closest point
// to the origin in the n-simplex, where n = 3, 2, 1 respectively
static void S3D(mjtNum lambda[4], const mjtNum simplex[12]);
static void S2D(mjtNum lambda[3], const mjtNum simplex[9]);
static void S1D(mjtNum lambda[2], const mjtNum simplex[6]);

// helper function to compute the support point in the Minkowski difference
static void support(mjtNum res[3], mjtCCObj* obj1, mjtCCObj* obj2, const mjtNum dir[3]);

// linear algebra utility functions
static mjtNum det3(const mjtNum v1[3], const mjtNum v2[3], const mjtNum v3[3]);
static void lincomb(mjtNum res[3], const mjtNum* coef, const mjtNum* v, int n);



// returns the distance between the two given objects given an initial guess x0
mjtNum mj_gjk(const mjGjkConfig* config, mjtCCObj* obj1, mjtCCObj* obj2, const mjtNum x0[3]) {
  // our current simplex with max 4 vertices due to only 3 dimensions
  mjtNum simplex[12];
  int n = 0;  // number of vertices in the simplex

  // the kth approximation point with initial value x_0
  mjtNum x_k[3];
  mju_copy3(x_k, x0);

  int N = config->max_iterations;
  for (size_t k = 0; k < N; k++) {
    mjtNum s_k[3];     // the kth support point
    mjtNum lambda[4];  // barycentric coordinates for x_k

    // compute the kth support point in the direction of -(x_k)
    mjtNum x_k_neg[3];
    mju_scl3(x_k_neg, x_k, -1);
    mju_normalize3(x_k_neg);
    support(s_k, obj1, obj2, x_k_neg);

    // the stopping criteria relies on the Frank-Wolfe duality gap given by
    //  f(x_k) - f(x_min) <= < grad f(x_k), (x_k - s_k) >
    mjtNum diff[3];
    mju_sub3(diff, x_k, s_k);
    if (2*mju_dot3(x_k, diff) < config->tolerance) {
      return mju_norm3(x_k);
    }

    // copy new support point into the simplex
    mju_copy3(simplex + 3*n++, s_k);

    // run the distance subalgorithm to compute the barycentric coordinates
    // of the closest point to the origin in the simplex
    signedVolume(lambda, simplex, n);
    lincomb(x_k, lambda, simplex, 4);

    // for lambda[i] == 0, remove the ith vertex from the simplex
    n = 0;
    for (int i = 0; i < 4; i++) {
      if (lambda[i] == 0) continue;
      mju_copy3(simplex + 3*n++, simplex + 3*i);
    }
  }

  return mju_norm3(x_k);
}



// helper function to compute the support point in the Minkowski difference
static void support(mjtNum res[3], mjtCCObj* obj1, mjtCCObj* obj2, const mjtNum dir[3]) {
  mjtNum s1[3], s2[3], dir_neg[3];
  mju_scl3(dir_neg, dir, -1);

  // compute S_{A-B}(dir) = S_A(dir) - S_B(-dir)
  mjc_support(s1, obj1, dir);
  mjc_support(s2, obj2, dir_neg);
  mju_sub3(res, s1, s2);
}



// linear combination of n 3D vectors:
//   res = coef[0]*v[0] + ... + coef[n-1]*v[3*(n-1)]
static void lincomb(mjtNum res[3], const mjtNum* coef, const mjtNum* v, int n) {
  mju_zero3(res);
  for (int i = 0; i < n; i++) {
    if (coef[i] == 0) continue;
    res[0] += coef[i] * v[3*i + 0];
    res[1] += coef[i] * v[3*i + 1];
    res[2] += coef[i] * v[3*i + 2];
  }
}



// returns determinant of the 3x3 matrix with columns v1, v2, v3
static mjtNum det3(const mjtNum v1[3], const mjtNum v2[3], const mjtNum v3[3]) {
  mjtNum temp[3];
  mju_cross(temp, v2, v3);
  return mju_dot3(v1, temp);
}



// returns true only when a and b are both strictly positive or both strictly negative
static int compareSigns(mjtNum a, mjtNum b) {
  if (a > 0 && b > 0) return 1;
  if (a < 0 && b < 0) return 1;
  return 0;
}



// computes the barycentric coordinates of the closest point to the origin in the n-simplex
void signedVolume(mjtNum lambda[4], const mjtNum simplex[12], int n) {
  int r = n - 1;  // spatial dimension of the simplex
  mju_zero4(lambda);

  if (r == 3) {
    S3D(lambda, simplex);
  } else if (r == 2) {
    S2D(lambda, simplex);
  } else if (r == 1) {
    S1D(lambda, simplex);
  } else {
    lambda[0] = 1;
  }
}



static void S3D(mjtNum lambda[4], const mjtNum simplex[12]) {
  // the four vertices of the 3-simplex that correspond to 4 support points
  const mjtNum* s1 = simplex;
  const mjtNum* s2 = simplex + 3;
  const mjtNum* s3 = simplex + 6;
  const mjtNum* s4 = simplex + 9;

  // the matrix M is given by
  //  [[ s1_x, s2_x, s3_x, s4_x ],
  //   [ s1_y, s2_y, s3_y, s4_y ],
  //   [ s1_z, s2_z, s3_z, s4_z ],
  //   [ 1,    1,    1,    1    ]]
  // we want to solve M*lambda = P, where P = [p_x, p_y, p_z, 1] with [p_x, p_y, p_z] is the
  // origin projected onto the simplex

  // compute cofactors to find det(M)
  mjtNum C41 = -det3(s2, s3, s4);
  mjtNum C42 =  det3(s1, s3, s4);
  mjtNum C43 = -det3(s1, s2, s4);
  mjtNum C44 =  det3(s1, s2, s3);

  // note that m_det = 6*SignVol(simplex) with C4i corresponding to the volume of the 3-simplex
  // with vertices {s1, s2, s3, 0} - si
  mjtNum m_det = C41 + C42 + C43 + C44;

  int comp1 = compareSigns(m_det, C41),
      comp2 = compareSigns(m_det, C42),
      comp3 = compareSigns(m_det, C43),
      comp4 = compareSigns(m_det, C44);

  // if all signs are the same then the origin is inside the simplex
  if (comp1 && comp2 && comp3 && comp4) {
    lambda[0] = C41 / m_det;
    lambda[1] = C42 / m_det;
    lambda[2] = C43 / m_det;
    lambda[3] = C44 / m_det;
    return;
  }

  // find the smallest distance, and use the corresponding barycentric coordinates
  mjtNum dist;

  if (!comp1) {
    mjtNum lambda_2d[3], verts[9], x[3];
    mju_copy3(verts, s2);
    mju_copy3(verts + 3, s3);
    mju_copy3(verts + 6, s4);
    S2D(lambda_2d, verts);
    lincomb(x, lambda_2d, verts, 3);
    dist = mju_norm3(x);
    lambda[1] = lambda_2d[0];
    lambda[2] = lambda_2d[1];
    lambda[3] = lambda_2d[2];
  }

  if (!comp2) {
    mjtNum lambda_2d[3], verts[9], x[3];
    mju_copy3(verts, s1);
    mju_copy3(verts + 3, s3);
    mju_copy3(verts + 6, s4);
    S2D(lambda_2d, verts);
    lincomb(x, lambda_2d, verts, 3);
    mjtNum d = mju_norm3(x);
    if (d < dist) {
      lambda[0] = lambda_2d[0];
      lambda[2] = lambda_2d[1];
      lambda[3] = lambda_2d[2];
      dist = d;
    }
  }

  if (!comp3) {
    mjtNum lambda_2d[3], verts[9], x[3];
    mju_copy3(verts, s1);
    mju_copy3(verts + 3, s2);
    mju_copy3(verts + 6, s4);
    S2D(lambda_2d, verts);
    lincomb(x, lambda_2d, verts, 3);
    mjtNum d = mju_norm3(x);
    if (d < dist) {
      lambda[0] = lambda_2d[0];
      lambda[1] = lambda_2d[1];
      lambda[3] = lambda_2d[2];
      dist = d;
    }
  }

  if (!comp4) {
    mjtNum lambda_2d[3], verts[9], x[3];
    mju_copy3(verts, s1);
    mju_copy3(verts + 3, s2);
    mju_copy3(verts + 6, s3);
    S2D(lambda_2d, verts);
    lincomb(x, lambda_2d, verts, 3);
    mjtNum d = mju_norm3(x);
    if (d < dist) {
      lambda[0] = lambda_2d[0];
      lambda[1] = lambda_2d[1];
      lambda[2] = lambda_2d[2];
      dist = d;
    }
  }
}



static void S2D(mjtNum lambda[3], const mjtNum simplex[9]) {
  // the three vertices of the 2-simplex that correspond to 3 support points
  const mjtNum* s1 = simplex;
  const mjtNum* s2 = simplex + 3;
  const mjtNum* s3 = simplex + 6;

  // compute normal
  mjtNum diff1[3], diff2[3], n[3];
  mju_sub3(diff1, s2, s1);
  mju_sub3(diff2, s3, s2);
  mju_cross(n, diff1, diff2);

  // project origin
  mjtNum p_o[3];
  mju_scl3(p_o, n, mju_dot3(n, s1) / mju_dot3(n, n));

  int index;
  mjtNum mu_max = 0;
  int k = 1, l = 2;

  for (int i = 0; i < 3; i++) {
    mjtNum mu = s2[k]*s3[l] + s1[k]*s2[l] + s3[k]*s1[l] - s2[k]*s1[l] - s3[k]*s2[l] - s1[k]*s3[l];
    if (mju_abs(mu) >= mju_abs(mu_max)) {
      mu_max = mu;
      index = i;
    }
    k = l; l = i;
  }

  // exclude index component
  mjtNum s1_2D[2], s2_2D[2], s3_2D[2], p_o_2D[2];

  if (index == 0) {
    s1_2D[0] = s1[1];
    s1_2D[1] = s1[2];

    s2_2D[0] = s2[1];
    s2_2D[1] = s2[2];

    s3_2D[0] = s3[1];
    s3_2D[1] = s3[2];

    p_o_2D[0] = p_o[1];
    p_o_2D[1] = p_o[2];
  } else if (index == 1) {
    s1_2D[0] = s1[0];
    s1_2D[1] = s1[2];

    s2_2D[0] = s2[0];
    s2_2D[1] = s2[2];

    s3_2D[0] = s3[0];
    s3_2D[1] = s3[2];

    p_o_2D[0] = p_o[0];
    p_o_2D[1] = p_o[2];
  } else {
    s1_2D[0] = s1[0];
    s1_2D[1] = s1[1];

    s2_2D[0] = s2[0];
    s2_2D[1] = s2[1];

    s3_2D[0] = s3[0];
    s3_2D[1] = s3[1];

    p_o_2D[0] = p_o[0];
    p_o_2D[1] = p_o[1];
  }

  // substitute p_o as a vertex in simplex
  mjtNum C1 = p_o_2D[0]*s2_2D[1] + p_o_2D[1]*s3_2D[0] + s2_2D[0]*s3_2D[1]
            - p_o_2D[0]*s3_2D[1] - p_o_2D[1]*s2_2D[0] - s3_2D[0]*s2_2D[1];

  mjtNum C2 = p_o_2D[0]*s3_2D[1] + p_o_2D[1]*s1_2D[0] + s3_2D[0]*s1_2D[1]
            - p_o_2D[0]*s1_2D[1] - p_o_2D[1]*s3_2D[0] - s1_2D[0]*s3_2D[1];

  mjtNum C3 = p_o_2D[0]*s1_2D[1] + p_o_2D[1]*s2_2D[0] + s1_2D[0]*s2_2D[1]
            - p_o_2D[0]*s2_2D[1] - p_o_2D[1]*s1_2D[0] - s2_2D[0]*s1_2D[1];

  int comp1 = compareSigns(mu_max, C1),
      comp2 = compareSigns(mu_max, C2),
      comp3 = compareSigns(mu_max, C3);

  // inside the simplex
  if (comp1 && comp2 && comp3) {
    lambda[0] = C1 / mu_max;
    lambda[1] = C2 / mu_max;
    lambda[2] = C3 / mu_max;
    return;
  }

  // find the smallest distance, and use the corresponding barycentric coordinates
  mjtNum dist;

  if (!comp1) {
    mjtNum lambda_1d[4], verts[6], x[3];
    mju_copy3(verts, s2);
    mju_copy3(verts + 3, s3);
    S1D(lambda_1d, verts);
    lincomb(x, lambda_1d, verts, 2);
    dist = mju_norm3(x);
    lambda[1] = lambda_1d[0];
    lambda[2] = lambda_1d[1];
  }

  if (!comp2) {
    mjtNum lambda_1d[4], verts[6], x[3];
    mju_copy3(verts, s1);
    mju_copy3(verts + 3, s3);
    S1D(lambda_1d, verts);
    lincomb(x, lambda_1d, verts, 2);
    mjtNum d = mju_norm3(x);
    if (d < dist) {
      lambda[0] = lambda_1d[0];
      lambda[2] = lambda_1d[1];
      dist = d;
    }
  }

  if (!comp3) {
    mjtNum lambda_1d[4], verts[6], x[3];
    mju_copy3(verts, s1);
    mju_copy3(verts + 3, s2);
    S1D(lambda_1d, verts);
    lincomb(x, lambda_1d, verts, 2);
    mjtNum d = mju_norm3(x);
    if (d < dist) {
      lambda[0] = lambda_1d[0];
      lambda[1] = lambda_1d[1];
      dist = d;
    }
  }
}



static void S1D(mjtNum lambda[2], const mjtNum simplex[6]) {
  // the two vertices of the 1-simplex correspond to 2 support points
  const mjtNum* s1 = simplex;
  const mjtNum* s2 = simplex + 3;

  // find projection of origin onto the 1-simplex:
  //   p_o = s2 - <s2, s2 - s1> / <s2 - s1, s2 - s1> * (s2 - s1)
  mjtNum p_o[3];
  mjtNum diff[3];
  mju_sub3(diff, s2, s1);
  mjtNum temp1 = mju_dot3(s2, diff);
  mjtNum temp2 = mju_dot3(diff, diff);
  mju_addScl3(p_o, s2, diff, - temp1 / temp2);

  // find the axis with the largest projection "shadow" of the simplex
  mjtNum mu_max = 0;
  int index;
  for (int i = 0; i < 3; i++) {
    mjtNum mu = s1[i] - s2[i];
    if (mju_abs(mu) >= mju_abs(mu_max)) {
      mu_max = mu;
      index = i;
    }
  }

  mjtNum C1 = p_o[index] - s2[index];
  mjtNum C2 = s1[index] - p_o[index];

  // inside the simplex
  if (compareSigns(mu_max, C1) && compareSigns(mu_max, C2)) {
    lambda[0] = C1 / mu_max;
    lambda[1] = C2 / mu_max;
  } else {
    lambda[0] = 1;
  }
}
