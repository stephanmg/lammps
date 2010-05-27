/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "pair_coul_debye_omp.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "memory.h"
#include "error.h"

using namespace LAMMPS_NS;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

/* ---------------------------------------------------------------------- */

PairCoulDebyeOMP::PairCoulDebyeOMP(LAMMPS *lmp) : PairCoulCutOMP(lmp) {}

/* ---------------------------------------------------------------------- */

void PairCoulDebyeOMP::compute(int eflag, int vflag)
{
  if (eflag || vflag) {
    ev_setup(eflag,vflag);
    ev_setup_thr(eflag,vflag);
  } else evflag = vflag_fdotr = 0;

  if (evflag) {
    if (eflag) {
      if (force->newton_pair) return eval<1,1,1>();
      else return eval<1,1,0>();
    } else {
      if (force->newton_pair) return eval<1,0,1>();
      else return eval<1,0,0>();
    }
  } else {
    if (force->newton_pair) return eval<0,0,1>();
    else return eval<0,0,0>();
  }
}

template <int EVFLAG, int EFLAG, int NEWTON_PAIR>
void PairCoulDebyeOMP::eval()
{

#if defined(_OPENMP)
#pragma omp parallel default(shared)
#endif
  {

    int i,j,ii,jj,inum,jnum,itype,jtype, tid;
    double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,ecoul,fpair;
    double rsq,r2inv,r,rinv,forcecoul,factor_coul,screening;
    int *ilist,*jlist,*numneigh,**firstneigh;

    ecoul = 0.0;

    const int nlocal = atom->nlocal;
    const int nall = nlocal + atom->nghost;
    const int nthreads = comm->nthreads;

    double **x = atom->x;
    double **f = atom->f;
    double *q = atom->q;
    int *type = atom->type;
    double *special_coul = force->special_coul;
    double *special_lj = force->special_lj;
    double qqrd2e = force->qqrd2e;

    inum = list->inum;
    ilist = list->ilist;
    numneigh = list->numneigh;
    firstneigh = list->firstneigh;

    // loop over neighbors of my atoms

    int iifrom, iito;
    f = loop_setup_thr(f, iifrom, iito, tid, inum, nall, nthreads);
    for (ii = iifrom; ii < iito; ++ii) {
      i = ilist[ii];
      qtmp = q[i];
      xtmp = x[i][0];
      ytmp = x[i][1];
      ztmp = x[i][2];
      itype = type[i];
      jlist = firstneigh[i];
      jnum = numneigh[i];

      for (jj = 0; jj < jnum; jj++) {
	j = jlist[jj];

	if (j < nall) factor_coul = 1.0;
	else {
	  factor_coul = special_coul[j/nall];
	  j %= nall;
	}

	delx = xtmp - x[j][0];
	dely = ytmp - x[j][1];
	delz = ztmp - x[j][2];
	rsq = delx*delx + dely*dely + delz*delz;
        jtype = type[j];

        if (rsq < cutsq[itype][jtype]) {
            r2inv = 1.0/rsq;
            r = sqrt(rsq);
            rinv = 1.0/r;
            screening = exp(-kappa*r);
            forcecoul = qqrd2e * qtmp*q[j] * screening * (kappa + rinv);
            fpair = factor_coul*forcecoul * r2inv;

              f[i][0] += delx*fpair;
              f[i][1] += dely*fpair;
              f[i][2] += delz*fpair;
              if (NEWTON_PAIR || j < nlocal) {
                f[j][0] -= delx*fpair;
                f[j][1] -= dely*fpair;
                f[j][2] -= delz*fpair;
	  }

	  if (EFLAG) 
                ecoul = factor_coul * qqrd2e * qtmp*q[j] * rinv * screening;

	  if (EVFLAG) ev_tally_thr(i,j,nlocal,NEWTON_PAIR,
			     0.0,ecoul,fpair,delx,dely,delz, tid);
	}
      }
    }

    // reduce per thread forces into global force array.
    force_reduce_thr(atom->f, nall, nthreads, tid);
  }
  ev_reduce_thr();
  if (vflag_fdotr) virial_compute();
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void PairCoulDebyeOMP::settings(int narg, char **arg)
{
  if (narg != 2) error->all("Illegal pair_style command");

  kappa = force->numeric(arg[0]);
  cut_global = force->numeric(arg[1]);

  // reset cutoffs that have been explicitly set

  if (allocated) {
    int i,j;
    for (i = 1; i <= atom->ntypes; i++)
      for (j = i+1; j <= atom->ntypes; j++)
	if (setflag[i][j]) cut[i][j] = cut_global;
  }
}

/* ----------------------------------------------------------------------
  proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairCoulDebyeOMP::write_restart_settings(FILE *fp)
{
  fwrite(&cut_global,sizeof(double),1,fp);
  fwrite(&kappa,sizeof(double),1,fp);
  fwrite(&offset_flag,sizeof(int),1,fp);
  fwrite(&mix_flag,sizeof(int),1,fp);
}

/* ----------------------------------------------------------------------
  proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairCoulDebyeOMP::read_restart_settings(FILE *fp)
{
  if (comm->me == 0) {
    fread(&cut_global,sizeof(double),1,fp);
    fread(&kappa,sizeof(double),1,fp);
    fread(&offset_flag,sizeof(int),1,fp);
    fread(&mix_flag,sizeof(int),1,fp);
  }
  MPI_Bcast(&cut_global,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&kappa,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&offset_flag,1,MPI_INT,0,world);
  MPI_Bcast(&mix_flag,1,MPI_INT,0,world);
}

/* ---------------------------------------------------------------------- */

double PairCoulDebyeOMP::single(int i, int j, int itype, int jtype,
			   double rsq, double factor_coul, double factor_lj,
			   double &fforce)
{
  double r2inv,r,rinv,forcecoul,phicoul,screening;

  r2inv = 1.0/rsq;
  r = sqrt(rsq);
  rinv = 1.0/r;
  screening = exp(-kappa*r);
  forcecoul = force->qqrd2e * atom->q[i]*atom->q[j] *
    screening * (kappa + rinv);
  fforce = factor_coul*forcecoul * r2inv;

  phicoul = force->qqrd2e * atom->q[i]*atom->q[j] * rinv * screening;
  return factor_coul*phicoul;
}

/* ---------------------------------------------------------------------- */

double PairCoulDebyeOMP::memory_usage()
{
  const int n=atom->ntypes;

  double bytes = PairOMP::memory_usage();

  bytes += 9*((n+1)*(n+1) * sizeof(double) + (n+1)*sizeof(double *));
  bytes += 1*((n+1)*(n+1) * sizeof(int) + (n+1)*sizeof(int *));

  return bytes;
}