// **************************************************************************
//                                lj_coul.cu
//                             -------------------
//                           W. Michael Brown (ORNL)
//
//  Device code for acceleration of the lj/coul/cut pair style
//
// __________________________________________________________________________
//    This file is part of the LAMMPS Accelerator Library (LAMMPS_AL)
// __________________________________________________________________________
//
//    begin                : 
//    email                : brownw@ornl.gov
// ***************************************************************************/

#ifdef NV_KERNEL
#include "nv_kernel_def.h"
texture<float4> pos_tex;
texture<float> q_tex;
#ifndef _DOUBLE_DOUBLE
__inline float4 fetch_pos(const int& i, const float4 *pos) 
  { return tex1Dfetch(pos_tex, i); }
__inline float fetch_q(const int& i, const float *q) 
  { return tex1Dfetch(q_tex, i); }
#endif
#endif

__kernel void kernel_pair(__global numtyp4 *x_, __global numtyp4 *lj1,
                          __global numtyp4* lj3, const int lj_types, 
                          __global numtyp *sp_lj_in, __global int *dev_nbor, 
                          __global int *dev_packed, __global acctyp4 *ans,
                          __global acctyp *engv, const int eflag,
                          const int vflag, const int inum,
                          const int nbor_pitch, __global numtyp *q_ ,
                          __global numtyp *cutsq, const numtyp qqrd2e,
                          const int t_per_atom) {
  int tid=THREAD_ID_X;
  int ii=mul24((int)BLOCK_ID_X,(int)(BLOCK_SIZE_X)/t_per_atom);
  ii+=tid/t_per_atom;
  int offset=tid%t_per_atom;

  __local numtyp sp_lj[8];
  sp_lj[0]=sp_lj_in[0];
  sp_lj[1]=sp_lj_in[1];
  sp_lj[2]=sp_lj_in[2];
  sp_lj[3]=sp_lj_in[3];
  sp_lj[4]=sp_lj_in[4];
  sp_lj[5]=sp_lj_in[5];
  sp_lj[6]=sp_lj_in[6];
  sp_lj[7]=sp_lj_in[7];

  acctyp energy=(acctyp)0;
  acctyp e_coul=(acctyp)0;
  acctyp4 f;
  f.x=(acctyp)0;
  f.y=(acctyp)0;
  f.z=(acctyp)0;
  acctyp virial[6];
  for (int i=0; i<6; i++)
    virial[i]=(acctyp)0;
  
  if (ii<inum) {
    __global int *nbor=dev_nbor+ii;
    int i=*nbor;
    nbor+=nbor_pitch;
    int numj=*nbor;
    nbor+=nbor_pitch;

    int n_stride;
    __global int *list_end;
    if (dev_nbor==dev_packed) {
      list_end=nbor+mul24(numj,nbor_pitch);
      nbor+=mul24(offset,nbor_pitch);
      n_stride=mul24(t_per_atom,nbor_pitch);
    } else {
      nbor=dev_packed+*nbor;
      list_end=nbor+numj;
      n_stride=t_per_atom;
      nbor+=offset;
    }
  
    numtyp4 ix=fetch_pos(i,x_); //x_[i];
    numtyp qtmp=fetch_q(i,q_);
    int itype=ix.w;

    for ( ; nbor<list_end; nbor+=n_stride) {
      int j=*nbor;

      numtyp factor_lj, factor_coul;
      factor_lj = sp_lj[sbmask(j)];
      factor_coul = sp_lj[sbmask(j)+4];
      j &= NEIGHMASK;

      numtyp4 jx=fetch_pos(j,x_); //x_[j];
      int jtype=jx.w;

      // Compute r12
      numtyp delx = ix.x-jx.x;
      numtyp dely = ix.y-jx.y;
      numtyp delz = ix.z-jx.z;
      numtyp rsq = delx*delx+dely*dely+delz*delz;

      int mtype=itype*lj_types+jtype;
      if (rsq<cutsq[mtype]) {
        numtyp r2inv=(numtyp)1.0/rsq;
        numtyp forcecoul, force_lj, force, r6inv;

        if (rsq < lj1[mtype].z) {
          r6inv = r2inv*r2inv*r2inv;
          force_lj = factor_lj*r6inv*(lj1[mtype].x*r6inv-lj1[mtype].y);
        } else
          force_lj = (numtyp)0.0;

        if (rsq < lj1[mtype].w) 
          forcecoul = qqrd2e*qtmp*fetch_q(j,q_)*sqrt(r2inv)*factor_coul;
        else
          forcecoul = (numtyp)0.0;

        force = (force_lj + forcecoul) * r2inv;

        f.x+=delx*force;
        f.y+=dely*force;
        f.z+=delz*force;

        if (eflag>0) {
          e_coul += forcecoul;
          if (rsq < lj1[mtype].z) {
            numtyp e=r6inv*(lj3[mtype].x*r6inv-lj3[mtype].y);
            energy+=factor_lj*(e-lj3[mtype].z);
          } 
        }
        if (vflag>0) {
          virial[0] += delx*delx*force;
          virial[1] += dely*dely*force;
          virial[2] += delz*delz*force;
          virial[3] += delx*dely*force;
          virial[4] += delx*delz*force;
          virial[5] += dely*delz*force;
        }
      }

    } // for nbor
  } // if ii
  
  // Reduce answers
  if (t_per_atom>1) {
    __local acctyp red_acc[6][BLOCK_PAIR];
    
    red_acc[0][tid]=f.x;
    red_acc[1][tid]=f.y;
    red_acc[2][tid]=f.z;
    red_acc[3][tid]=energy;
    red_acc[4][tid]=e_coul;

    for (unsigned int s=t_per_atom/2; s>0; s>>=1) {
      if (offset < s) {
        for (int r=0; r<5; r++)
          red_acc[r][tid] += red_acc[r][tid+s];
      }
    }
    
    f.x=red_acc[0][tid];
    f.y=red_acc[1][tid];
    f.z=red_acc[2][tid];
    energy=red_acc[3][tid];
    e_coul=red_acc[4][tid];

    if (vflag>0) {
      for (int r=0; r<6; r++)
        red_acc[r][tid]=virial[r];

      for (unsigned int s=t_per_atom/2; s>0; s>>=1) {
        if (offset < s) {
          for (int r=0; r<6; r++)
            red_acc[r][tid] += red_acc[r][tid+s];
        }
      }
    
      for (int r=0; r<6; r++)
        virial[r]=red_acc[r][tid];
    }
  }

  // Store answers
  if (ii<inum && offset==0) {
    __global acctyp *ap1=engv+ii;
    if (eflag>0) {
      *ap1=energy;
      ap1+=inum;
      *ap1=e_coul;
      ap1+=inum;
    }
    if (vflag>0) {
      for (int i=0; i<6; i++) {
        *ap1=virial[i];
        ap1+=inum;
      }
    }
    ans[ii]=f;
  } // if ii
}

__kernel void kernel_pair_fast(__global numtyp4 *x_, __global numtyp4 *lj1_in,
                               __global numtyp4* lj3_in, 
                               __global numtyp* sp_lj_in,
                               __global int *dev_nbor, __global int *dev_packed,
                               __global acctyp4 *ans, __global acctyp *engv, 
                               const int eflag, const int vflag, const int inum, 
                               const int nbor_pitch, __global numtyp *q_,
                               __global numtyp *_cutsq, const numtyp qqrd2e,
                               const int t_per_atom) {
  int tid=THREAD_ID_X;
  int ii=mul24((int)BLOCK_ID_X,(int)(BLOCK_SIZE_X)/t_per_atom);
  ii+=tid/t_per_atom;
  int offset=tid%t_per_atom;

  __local numtyp4 lj1[MAX_SHARED_TYPES*MAX_SHARED_TYPES];
  __local numtyp4 lj3[MAX_SHARED_TYPES*MAX_SHARED_TYPES];
  __local numtyp cutsq[MAX_SHARED_TYPES*MAX_SHARED_TYPES];
  __local numtyp sp_lj[8];
  if (tid<8)
    sp_lj[tid]=sp_lj_in[tid];
  if (tid<MAX_SHARED_TYPES*MAX_SHARED_TYPES) {
    lj1[tid]=lj1_in[tid];
    cutsq[tid]=_cutsq[tid];
    if (eflag>0)
      lj3[tid]=lj3_in[tid];
  }
  
  acctyp energy=(acctyp)0;
  acctyp e_coul=(acctyp)0;
  acctyp4 f;
  f.x=(acctyp)0;
  f.y=(acctyp)0;
  f.z=(acctyp)0;
  acctyp virial[6];
  for (int i=0; i<6; i++)
    virial[i]=(acctyp)0;
  
  __syncthreads();
  
  if (ii<inum) {
    __global int *nbor=dev_nbor+ii;
    int i=*nbor;
    nbor+=nbor_pitch;
    int numj=*nbor;
    nbor+=nbor_pitch;

    int n_stride;
    __global int *list_end;
    if (dev_nbor==dev_packed) {
      list_end=nbor+mul24(numj,nbor_pitch);
      nbor+=mul24(offset,nbor_pitch);
      n_stride=mul24(t_per_atom,nbor_pitch);
    } else {
      nbor=dev_packed+*nbor;
      list_end=nbor+numj;
      n_stride=t_per_atom;
      nbor+=offset;
    }
  
    numtyp4 ix=fetch_pos(i,x_); //x_[i];
    numtyp qtmp=fetch_q(i,q_);
    int iw=ix.w;
    int itype=mul24((int)MAX_SHARED_TYPES,iw);

    for ( ; nbor<list_end; nbor+=n_stride) {
      int j=*nbor;

      numtyp factor_lj, factor_coul;
      factor_lj = sp_lj[sbmask(j)];
      factor_coul = sp_lj[sbmask(j)+4];
      j &= NEIGHMASK;

      numtyp4 jx=fetch_pos(j,x_); //x_[j];
      int mtype=itype+jx.w;

      // Compute r12
      numtyp delx = ix.x-jx.x;
      numtyp dely = ix.y-jx.y;
      numtyp delz = ix.z-jx.z;
      numtyp rsq = delx*delx+dely*dely+delz*delz;

      if (rsq<cutsq[mtype]) {
        numtyp r2inv=(numtyp)1.0/rsq;
        numtyp forcecoul, force_lj, force, r6inv;

        if (rsq < lj1[mtype].z) {
          r6inv = r2inv*r2inv*r2inv;
          force_lj = factor_lj*r6inv*(lj1[mtype].x*r6inv-lj1[mtype].y);
        } else
          force_lj = (numtyp)0.0;

        if (rsq < lj1[mtype].w)
          forcecoul = qqrd2e*qtmp*fetch_q(j,q_)*sqrt(r2inv)*factor_coul;
        else
          forcecoul = (numtyp)0.0;

        force = (force_lj + forcecoul) * r2inv;

        f.x+=delx*force;
        f.y+=dely*force;
        f.z+=delz*force;

        if (eflag>0) {
          e_coul += forcecoul;
          if (rsq < lj1[mtype].z) {
            numtyp e=r6inv*(lj3[mtype].x*r6inv-lj3[mtype].y);
            energy+=factor_lj*(e-lj3[mtype].z);
          }
        }
        if (vflag>0) {
          virial[0] += delx*delx*force;
          virial[1] += dely*dely*force;
          virial[2] += delz*delz*force;
          virial[3] += delx*dely*force;
          virial[4] += delx*delz*force;
          virial[5] += dely*delz*force;
        }
      }

    } // for nbor
  } // if ii

  // Reduce answers
  if (t_per_atom>1) {
    __local acctyp red_acc[6][BLOCK_PAIR];
    
    red_acc[0][tid]=f.x;
    red_acc[1][tid]=f.y;
    red_acc[2][tid]=f.z;
    red_acc[3][tid]=energy;
    red_acc[4][tid]=e_coul;

    for (unsigned int s=t_per_atom/2; s>0; s>>=1) {
      if (offset < s) {
        for (int r=0; r<5; r++)
          red_acc[r][tid] += red_acc[r][tid+s];
      }
    }
    
    f.x=red_acc[0][tid];
    f.y=red_acc[1][tid];
    f.z=red_acc[2][tid];
    energy=red_acc[3][tid];
    e_coul=red_acc[4][tid];

    if (vflag>0) {
      for (int r=0; r<6; r++)
        red_acc[r][tid]=virial[r];

      for (unsigned int s=t_per_atom/2; s>0; s>>=1) {
        if (offset < s) {
          for (int r=0; r<6; r++)
            red_acc[r][tid] += red_acc[r][tid+s];
        }
      }
    
      for (int r=0; r<6; r++)
        virial[r]=red_acc[r][tid];
    }
  }

  // Store answers
  if (ii<inum && offset==0) {
    __global acctyp *ap1=engv+ii;
    if (eflag>0) {
      *ap1=energy;
      ap1+=inum;
      *ap1=e_coul;
      ap1+=inum;
    }
    if (vflag>0) {
      for (int i=0; i<6; i++) {
        *ap1=virial[i];
        ap1+=inum;
      }
    }
    ans[ii]=f;
  } // if ii*/
}
