/*
 *       $Id$
 *
 *       This source code is part of
 *
 *        G   R   O   M   A   C   S
 *
 * GROningen MAchine for Chemical Simulations
 *
 *            VERSION 2.0
 * 
 * Copyright (c) 1991-1997
 * BIOSON Research Institute, Dept. of Biophysical Chemistry
 * University of Groningen, The Netherlands
 * 
 * Please refer to:
 * GROMACS: A message-passing parallel molecular dynamics implementation
 * H.J.C. Berendsen, D. van der Spoel and R. van Drunen
 * Comp. Phys. Comm. 91, 43-56 (1995)
 *
 * Also check out our WWW page:
 * http://rugmd0.chem.rug.nl/~gmx
 * or e-mail to:
 * gromacs@chem.rug.nl
 *
 * And Hey:
 * Green Red Orange Magenta Azure Cyan Skyblue
 */
static char *SRCID_g_rmsf_c = "$Id$";

#include "smalloc.h"
#include "math.h"
#include "macros.h"
#include "typedefs.h"
#include "xvgr.h"
#include "copyrite.h"
#include "statutil.h"
#include "string2.h"
#include "vec.h"
#include "rdgroup.h"
#include "pdbio.h"
#include "tpxio.h"
#include "pbc.h"
#include "fatal.h"
#include "futil.h"
#include "gstat.h"
#include "confio.h"

static int calc_xav(bool bAverX,
		    char *fn,rvec xref[],t_topology *top,matrix box,
		    real w_rls[],int isize,atom_id index[])
{
  rvec *x,*xav;
  rvec xcm;
  real tmas,t,rmsd,xxx,tfac;
  int  i,m,natoms,status,teller;
  
  /* remove pbc */
  rm_pbc(&(top->idef),top->atoms.nr,box,xref,xref);

  /* set center of mass to zero */
  tmas = sub_xcm(xref,isize,index,top->atoms.atom,xcm,FALSE);

  /* read first frame  */
  natoms = read_first_x(&status,fn,&t,&x,box);
  if (natoms != top->atoms.nr) 
    fatal_error(0,"Topology (%d atoms) does not match trajectory (%d atoms)",
		top->atoms.nr,natoms);
		
  if (bAverX) {
    snew(xav,natoms);
    teller = 0;
    do {
      /* remove periodic boundary */
      rm_pbc(&(top->idef),top->atoms.nr,box,x,x);
      
      /* set center of mass to zero */
      tmas = sub_xcm(x,isize,index,top->atoms.atom,xcm,FALSE);
      
      /* fit to reference structure */
      do_fit(top->atoms.nr,w_rls,xref,x);
      
      for(i=0; (i<natoms); i++)
	rvec_inc(xav[i],x[i]);
      
      teller++;
    } while (read_next_x(status,&t,natoms,x,box));

    tfac = 1.0/teller;    
    rmsd = 0;
    for(i=0; (i<natoms); i++) {
      for(m=0; (m<DIM); m++) {
	xxx        = xav[i][m] * tfac;
	rmsd      += sqr(xref[i][m] - xxx);
	xref[i][m] = xxx;
      }
    }
    rmsd = sqrt(rmsd/natoms);    
    sfree(xav);
    fprintf(stderr,"Computed average structure. RMSD with reference is %g nm\n",
	    rmsd);
  }
  sfree(x);

  rewind_trj(status);
  
  return status;
}

static int find_pdb(int npdb,t_pdbatom pdba[],char *resnm,
		    int resnr,char *atomnm)
{
  int i;
  
  resnm[3]='\0';
  for(i=0; (i<npdb); i++) {
    if ((resnr == pdba[i].resnr) &&
	(strcmp(pdba[i].resnm,resnm) == 0) &&
	(strstr(atomnm,pdba[i].atomnm) != NULL))
      break;
  }
  if (i == npdb) {
    fprintf(stderr,"\rCan not find %s%d-%s in pdbfile\n",resnm,resnr,atomnm);
    return -1;
  }
    
  return i;
}

int main (int argc,char *argv[])
{
  static char *desc[] = {
    "g_rmsf computes the root mean square fluctuation (RMSF, i.e. standard ",
    "deviation) of atomic positions ",
    "after first fitting to a reference frame.[PAR]",
    "When the (optional) pdb file is given, the RMSF values are converted",
    "to B-factor values and plotted with the experimental data.",
    "With option -aver the average coordinates will be calculated and used",
    "as reference for fitting. They are also saved to a gro file."
  };
  static bool bAverX=FALSE;
  t_pargs pargs[] = { 
    { "-aver", FALSE, etBOOL, &bAverX,
      "Calculate average coordinates first. Requires reading the coordinates twice" }
  };
  int          step,nre,natom,natoms,i,g,m,teller=0;
  real         t,lambda,*w_rls,*w_rms,tmas;
  
  t_tpxheader header;
  t_inputrec   ir;
  t_topology   top;
  t_pdbatom    *pdba;
  int          npdba;
  bool         bCont;

  matrix       box;
  rvec         *x,*v,*xref;
  int          status;
  char         buf[256];
  char         title[STRLEN];
  
  FILE         *fp;               /* the graphics file */
  int          resnr,pdb_i;
  
  atom_id      *index;
  int          isize;
  char         *grpnames;

  real         bfac;
  atom_id      aid;
  rvec         *rmsf_xx,*rmsf_x;
  real         *rmsf;
  int          d;
  real         count=0;
  rvec         xcm;

  t_filenm fnm[] = {
    { efTPX, NULL, NULL, ffREAD  },
    { efTRX, "-f", NULL, ffREAD  },
    { efPDB, "-q", NULL, ffOPTRD },
    { efNDX, NULL, NULL, ffOPTRD },
    { efXVG, NULL, NULL, ffWRITE },
    { efGRO, "-ox","xaver", ffOPTWR }
  };
#define NFILE asize(fnm)

  CopyRight(stderr,argv[0]);
  parse_common_args(&argc,argv,PCA_CAN_TIME | PCA_CAN_VIEW,TRUE,
		    NFILE,fnm,asize(pargs),pargs,asize(desc),desc,0,NULL);

  read_tpxheader(ftp2fn(efTPX,NFILE,fnm),&header);
  snew(x,header.natoms);
  snew(v,header.natoms);
  snew(xref,header.natoms);
  snew(w_rls,header.natoms);
  read_tpx(ftp2fn(efTPX,NFILE,fnm),&step,&t,&lambda,&ir,
	   box,&natom,xref,NULL,NULL,&top);

  /* Set box type*/
  init_pbc(box,FALSE);
  
  fprintf(stderr,"Select group(s) for root mean square calculation\n");
  get_index(&top.atoms,ftp2fn_null(efNDX,NFILE,fnm),1,&isize,&index,&grpnames);

  /* Set the weight */
  for(i=0;(i<isize);i++) 
    w_rls[index[i]]=top.atoms.atom[index[i]].m;

  /* Malloc the rmsf arrays */
  snew(rmsf_xx,isize);
  snew(rmsf_x, isize);
  snew(rmsf,isize);

  /* This routine computes average coordinates. Input in xref is the
   * reference structure, output the average.
   * Also input is the file name, returns the file number.
   */
  status = calc_xav(bAverX,
		    ftp2fn(efTRX,NFILE,fnm),xref,&top,box,w_rls,isize,index);
    
  if (bAverX) {
    FILE *fp=ftp2FILE(efGRO,NFILE,fnm,"w");
    write_hconf_indexed(fp,"Average coords generated by g_rmsf",
			&top.atoms,isize,index,xref,NULL,box); 
    fclose(fp);
  }
  
  /* Now read the trj again to compute fluctuations */
  teller = 0;
  while (read_next_x(status,&t,natom,x,box)) {
    /* Remove periodic boundary */
    rm_pbc(&(top.idef),top.atoms.nr,box,x,x);

    /* Set center of mass to zero */
    tmas = sub_xcm(x,isize,index,top.atoms.atom,xcm,FALSE);
    
    /* Fit to reference structure */
    do_fit(top.atoms.nr,w_rls,xref,x);
 
    /* Print time of frame*/
    if ((teller % 10) == 0)
      fprintf(stderr,"\r %5.2f",t);
      
    /* Calculate root_least_squares*/
    for(i=0;(i<isize);i++) {
      aid = index[i];
      for(d=0;(d<DIM);d++) {
	rmsf_xx[i][d]+=x[aid][d]*x[aid][d];
	rmsf_x[i][d] +=x[aid][d];
      }
    }
    count += 1.0;
    teller++;
  } 
  close_trj(status);

  for(i=0;(i<isize);i++) {
    rmsf[i] = (rmsf_xx[i][XX]/count - sqr(rmsf_x[i][XX]/count)+ 
	       rmsf_xx[i][YY]/count - sqr(rmsf_x[i][YY]/count)+ 
	       rmsf_xx[i][ZZ]/count - sqr(rmsf_x[i][ZZ]/count)); 
  }
  
  if (ftp2bSet(efPDB,NFILE,fnm)) {
    fp    = ftp2FILE(efPDB,NFILE,fnm,"r");
    npdba = read_pdbatoms(fp,title,&pdba,box,FALSE);
    renumber_pdb(npdba,pdba);
    fclose(fp);
    pdba_trimnames(npdba,pdba);
  }
  else
    npdba=0;

  /* write output */
  if (npdba == 0) {
    fp=xvgropen(ftp2fn(efXVG,NFILE,fnm),"RMS fluctuation","Atom","nm");
    for(i=0;(i<isize);i++) 
      fprintf(fp,"%5d %8.4f\n",i,sqrt(rmsf[i]));
  }
  else {
    bfac=8.0*M_PI*M_PI/3.0*100;
    fp=xvgropen(ftp2fn(efXVG,NFILE,fnm),"B-Factors",
		"Atom","A\\b\\S\\So\\N\\S 2");
    for(i=0;(i<isize);i++) {
      resnr=top.atoms.atom[index[i]].resnr;
      pdb_i=find_pdb(npdba,pdba,*(top.atoms.resname[resnr]),resnr,
		     *(top.atoms.atomname[index[i]]));
      
      fprintf(fp,"%5d  %10.5f  %10.5f\n",i,rmsf[i]*bfac,
	      (pdb_i == -1) ? 0.0 : pdba[pdb_i].bfac);
    }
  }
  
  fclose(fp);

  xvgr_file(ftp2fn(efXVG,NFILE,fnm),"-nxy");
    
  thanx(stdout);
  
  return 0;
}
