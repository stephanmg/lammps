/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
   Contributing author: Pierre de Buyl (KU Leuven)
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "limits.h"
#include "ch5md.h"
#include "dump_h5md.h"
#include "domain.h"
#include "atom.h"
#include "update.h"
#include "group.h"
#include "output.h"
#include "error.h"
#include "force.h"
#include "memory.h"

using namespace LAMMPS_NS;

#define MYMIN(a,b) ((a) < (b) ? (a) : (b))
#define MYMAX(a,b) ((a) > (b) ? (a) : (b))

/* ---------------------------------------------------------------------- */

DumpH5MD::DumpH5MD(LAMMPS *lmp, int narg, char **arg) : Dump(lmp, narg, arg)
{
  if (narg<6) error->all(FLERR,"Illegal dump h5md command");
  if (binary || compressed || multifile || multiproc)
    error->all(FLERR,"Invalid dump h5md filename");

  size_one = 6;
  sort_flag = 1;
  sortcol = 0;
  format_default = NULL;
  flush_flag = 0;
  unwrap_flag = 0;

  every_dump = force->inumeric(FLERR,arg[3]);
  every_position = every_image = -1;
  every_velocity = every_force = every_species = -1;

  int iarg=5;
  size_one=0;
  while (iarg<narg) {
    if (strcmp(arg[iarg], "position")==0) {
      if (iarg+1>=narg) {
        error->all(FLERR, "Invalid number of arguments in dump h5md");
      }
      every_position = atoi(arg[iarg+1]);
      iarg+=2;
      size_one+=domain->dimension;
    } else if (strcmp(arg[iarg], "image")==0) {
      if (every_position<=0) error->all(FLERR, "Illegal dump h5md command");
      iarg+=1;
      size_one+=domain->dimension;
      every_image = every_position;
    } else if (strcmp(arg[iarg], "velocity")==0) {
      if (iarg+1>=narg) {
        error->all(FLERR, "Invalid number of arguments in dump h5md");
      }
      every_velocity = atoi(arg[iarg+1]);
      iarg+=2;
      size_one+=domain->dimension;
    } else if (strcmp(arg[iarg], "force")==0) {
      if (iarg+1>=narg) {
        error->all(FLERR, "Invalid number of arguments in dump h5md");
      }
      every_force = atoi(arg[iarg+1]);
      iarg+=2;
      size_one+=domain->dimension;
    } else if (strcmp(arg[iarg], "species")==0) {
      if (iarg+1>=narg) {
        error->all(FLERR, "Invalid number of arguments in dump h5md");
      }
      every_species = atoi(arg[iarg+1]);
      iarg+=2;
      size_one+=1;
    } else {
      error->all(FLERR, "Invalid argument to dump h5md");
    }
  }

  // allocate global array for atom coords

  bigint n = group->count(igroup);
  natoms = static_cast<int> (n);

  if (every_position>0)
    memory->create(dump_position,domain->dimension*natoms,"dump:position");
  if (every_image>0)
    memory->create(dump_image,domain->dimension*natoms,"dump:image");
  if (every_velocity>0)
    memory->create(dump_velocity,domain->dimension*natoms,"dump:velocity");
  if (every_force>0)
    memory->create(dump_force,domain->dimension*natoms,"dump:force");
  if (every_species>0)
    memory->create(dump_species,natoms,"dump:species");

  openfile();
  ntotal = 0;
}

/* ---------------------------------------------------------------------- */

DumpH5MD::~DumpH5MD()
{
  if (every_position>0)
    memory->destroy(dump_position);
  if (every_image>0)
    memory->destroy(dump_image);
  if (every_velocity>0)
    memory->destroy(dump_velocity);
  if (every_force>0)
    memory->destroy(dump_force);
  if (every_species>0)
    memory->destroy(dump_species);
}

/* ---------------------------------------------------------------------- */

void DumpH5MD::init_style()
{
  if (sort_flag == 0 || sortcol != 0)
    error->all(FLERR,"Dump h5md requires sorting by atom ID");
}

/* ---------------------------------------------------------------------- */

void DumpH5MD::openfile()
{
  char *group_name;
  int group_name_length;
  int dims[2];
  char *boundary[3];
  for (int i=0; i<3; i++) {
    boundary[i] = new char[9];
    if (domain->periodicity[i]==1) {
      strcpy(boundary[i], "periodic");
    } else {
      strcpy(boundary[i], "none");
    }
  }

  if (me == 0) {
    datafile = h5md_create_file(filename, "N/A", NULL, "lammps", "N/A");
    group_name_length = strlen(group->names[igroup]);
    group_name = new char[group_name_length];
    strcpy(group_name, group->names[igroup]);
    particles_data = h5md_create_particles_group(datafile, group_name);
    delete [] group_name;
    dims[0] = natoms;
    dims[1] = domain->dimension;
    if (every_position>0) {
      particles_data.position = h5md_create_time_data(particles_data.group, "position", 2, dims, H5T_NATIVE_DOUBLE, NULL);
      h5md_create_box(&particles_data, dims[1], boundary, true, NULL, &particles_data.position);
    } else {
      h5md_create_box(&particles_data, dims[1], boundary, true, NULL, NULL);
    }
    if (every_image>0)
      particles_data.image = h5md_create_time_data(particles_data.group, "image", 2, dims, H5T_NATIVE_INT, &particles_data.position);
    if (every_velocity>0)
      particles_data.velocity = h5md_create_time_data(particles_data.group, "velocity", 2, dims, H5T_NATIVE_DOUBLE, NULL);
    if (every_force>0)
      particles_data.force = h5md_create_time_data(particles_data.group, "force", 2, dims, H5T_NATIVE_DOUBLE, NULL);
    if (every_species>0)
      particles_data.species = h5md_create_time_data(particles_data.group, "species", 1, dims, H5T_NATIVE_INT, NULL);

  }
  for (int i=0; i<3; i++) {
    delete [] boundary[i];
  }

}

/* ---------------------------------------------------------------------- */

void DumpH5MD::write_header(bigint nbig)
{
  return;
}

/* ---------------------------------------------------------------------- */

void DumpH5MD::pack(tagint *ids)
{
  int m,n;

  tagint *tag = atom->tag;
  double **x = atom->x;
  double **v = atom->v;
  double **f = atom->f;
  int *species = atom->type;
  imageint *image = atom->image;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;
  int dim=domain->dimension;

  double xprd = domain->xprd;
  double yprd = domain->yprd;
  double zprd = domain->zprd;

  m = n = 0;
  for (int i = 0; i < nlocal; i++)
    if (mask[i] & groupbit) {
      if (every_position>0) {
	int ix = (image[i] & IMGMASK) - IMGMAX;
	int iy = (image[i] >> IMGBITS & IMGMASK) - IMGMAX;
	int iz = (image[i] >> IMG2BITS) - IMGMAX;
	if (unwrap_flag == 1) {
	  buf[m++] = (x[i][0] + ix * xprd);
	  buf[m++] = (x[i][1] + iy * yprd);
	  if (dim>2) buf[m++] = (x[i][2] + iz * zprd);
	} else {
	  buf[m++] = x[i][0];
	  buf[m++] = x[i][1];
	  if (dim>2) buf[m++] = x[i][2];
	}
	if (every_image>0) {
	  buf[m++] = ix;
	  buf[m++] = iy;
	  if (dim>2) buf[m++] = iz;
	}
      }
      if (every_velocity>0) {
	buf[m++] = v[i][0];
	buf[m++] = v[i][1];
	if (dim>2) buf[m++] = v[i][2];
      }
      if (every_force>0) {
	buf[m++] = f[i][0];
	buf[m++] = f[i][1];
	if (dim>2) buf[m++] = f[i][2];
      }
      if (every_species>0)
	buf[m++] = species[i];
      ids[n++] = tag[i];
    }
}

/* ---------------------------------------------------------------------- */

void DumpH5MD::write_data(int n, double *mybuf)
{
  // copy buf atom coords into global array

  int m = 0;
  int dim = domain->dimension;
  int k = dim*ntotal;
  int k_image = dim*ntotal;
  int k_velocity = dim*ntotal;
  int k_force = dim*ntotal;
  int k_species = ntotal;
  for (int i = 0; i < n; i++) {
    if (every_position>0) {
      for (int j=0; j<dim; j++) {
	dump_position[k++] = mybuf[m++];
      }
      if (every_image>0)
	for (int j=0; j<dim; j++) {
	  dump_image[k_image++] = mybuf[m++];
	}
    }
    if (every_velocity>0)
      for (int j=0; j<dim; j++) {
	dump_velocity[k_velocity++] = mybuf[m++];
      }
    if (every_force>0)
      for (int j=0; j<dim; j++) {
	dump_force[k_force++] = mybuf[m++];
      }
    if (every_species>0)
      dump_species[k_species++] = mybuf[m++];
    ntotal++;
  }

  // if last chunk of atoms in this snapshot, write global arrays to file

  if (ntotal == natoms) {
    write_frame();
    ntotal = 0;
  }
}

/* ---------------------------------------------------------------------- */

int DumpH5MD::modify_param(int narg, char **arg)
{
  if (strcmp(arg[0],"unwrap") == 0) {
    if (narg < 2) error->all(FLERR,"Illegal dump_modify command");
    if (strcmp(arg[1],"yes") == 0) unwrap_flag = 1;
    else if (strcmp(arg[1],"no") == 0) unwrap_flag = 0;
    else error->all(FLERR,"Illegal dump_modify command");
    return 2;
  }
  return 0;
}

/* ---------------------------------------------------------------------- */

void DumpH5MD::write_frame()
{
  int local_step;
  double local_time;
  double edges[3];
  local_step = update->ntimestep;
  local_time = local_step * update->dt;
  edges[0] = boxxhi - boxxlo;
  edges[1] = boxyhi - boxylo;
  edges[2] = boxzhi - boxzlo;
  if (every_position>0) {
    if (local_step % (every_position*every_dump) == 0) {
      h5md_append(particles_data.position, dump_position, local_step, local_time);
      h5md_append(particles_data.box_edges, edges, local_step, local_time);
      if (every_image>0)
	h5md_append(particles_data.image, dump_image, local_step, local_time);
    }
  } else {
    h5md_append(particles_data.box_edges, edges, local_step, local_time);
  }
  if (every_velocity>0 && local_step % (every_velocity*every_dump) == 0) {
    h5md_append(particles_data.velocity, dump_velocity, local_step, local_time);
  }
  if (every_force>0 && local_step % (every_force*every_dump) == 0) {
    h5md_append(particles_data.force, dump_force, local_step, local_time);
  }
  if (every_species>0 && local_step % (every_species*every_dump) == 0) {
    h5md_append(particles_data.species, dump_species, local_step, local_time);
  }
}

