/************************************************************************ *
 * Goma - Multiphysics finite element software                             *
 * Sandia National Laboratories                                            *
 *                                                                         *
 * Copyright (c) 2014 Sandia Corporation.                                  *
 *                                                                         *
 * Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,  *
 * the U.S. Government retains certain rights in this software.            *
 *                                                                         *
 * This software is distributed under the GNU General Public License.	  *
 \************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "std.h"

#include "exo_struct.h"
#include "rf_fem_const.h"
#include "rf_vars_const.h"
#include "mm_as_const.h"
#include "mm_as_structs.h"
#include "rf_node_const.h"
#include "usr_print.h"
#include "sl_amesos_interface.h"
#include "brk_utils.h"

#include "sl_epetra_interface.h"
#include "sl_epetra_util.h"

#define _RF_SOLVE_SEGREGATED_C
#include "goma.h"
#include "el_quality.h"

/*************************************************************************************
 *  solve_problem_segregated
 *
 *      	 Routine that controls solution of overall FEM problem in segregated fashion.
 *        This routine will control time integration, order of matrices to be solved,
 *        matrices fills and nonlinear solver and results output.
 *
 *        Started as a clone from solve_problem routine
 *
 * Revision History
 * ================
 * 26 November 2014 - Kristianto Tjiptowidjojo - Creation.
 *
 * ******************************************************************************/

void solve_problem_segregated(Exo_DB *exo, /* ptr to the finite element mesh database  */
                              Dpi *dpi, /* distributed processing information       */
                              dbl *te_out) /* te_out - return actual end time */

{

  /*
   * Sparse matrix storage vectors
   * (MSR format.  See "SPARSKIT: a basic tool kit for sparse matrix
   * computations" by Youcef Saad)
   */
  int **ija = NULL; /* column pointer array                     */
  double **a = NULL; /* nonzero array                            */
  double **a_old = NULL; /* nonzero array                            */
  int **ija_attic = NULL; /* storage for external dofs                */

  /*
   * Variables
   */

  static double **x = NULL; /* solution vector                   */

  static double **x_old = NULL; /* old solution vector               */
  static double **x_older = NULL; /* older solution vector             */
  static double **x_oldest = NULL; /* oldest solution vector saved      */
  static double **xdot = NULL; /* current time derivative of soln   */
  static double **xdot_old = NULL; /* old time derivative of soln       */
  static double **xdot_older = NULL; /* old time derivative of soln       */

  double **x_update = NULL; /* update at last iteration          */
  double **resid_vector = NULL; /* residual                          */
  double **resid_vector_sens = NULL; /* residual sensitivity              */

  double **scale = NULL; /* scale vector for modified newton  */

  int *node_to_fill = NULL;

  static struct Aztec_Linear_Solver_System **ams;

  /*
   * Variables for time integration
   */

  double theta = 0.0;
  static double time1 = 0.0; /* Current time that the simulation is trying  to find the solution for */
  double delta_t;
  int time_step_reform; /* counter for jacobian reformation stride  */
  int converged = TRUE; /* success or failure of Newton iteration   */
  static int nprint = 0;

  /*
   * Local variables
   */

  int error, err, is_steady_state;
  static double **gvec = NULL;
  static double ****gvec_elem = NULL;
  static struct Results_Description **rd;

  double *gv; /* Global variable values for ExoII database */
  int *tnv; /* total number of nodal variables and kinds */
  int *tev; /* total number of elem variables and kinds  */
  int *tnv_post; /* total number of nodal variables and kinds
   for post processing                       */
  int *tev_post; /* total number of elem variables and kinds
   for post processing                       */

  unsigned int matrix_systems_mask;

  int i;
  int *numProcUnknowns;

  static int callnum = 1; /* solve_problem_segregated call counter */

  static const char yo[] = "solve_problem_segregated"; /* So my name is in a string.        */

  tnv = malloc(sizeof(int) * upd->Total_Num_Matrices);
  tev = malloc(sizeof(int) * upd->Total_Num_Matrices);
  tnv_post = malloc(sizeof(int) * upd->Total_Num_Matrices);
  tev_post = malloc(sizeof(int) * upd->Total_Num_Matrices);

  /*
   *            BEGIN EXECUTION
   */

  is_steady_state = (TimeIntegration == STEADY) ? TRUE : FALSE;



  /*
   *  Malloc the space for the results description structure and set all
   *  of that space to naught initially.
   *  Do this only once if in library mode.
   */
  if (callnum == 1) {
    rd = malloc(sizeof(struct Results_Description *) * upd->Total_Num_Matrices);
    for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
      rd[pg->imtrx] = alloc_struct_1(struct Results_Description, 1);
    }
  }

  for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
    /*
     * Some preliminaries to help setup EXODUS II database output.
     */

    tnv[pg->imtrx] = cnt_nodal_vars(); /*  tnv_post is calculated in load_nodal_tkn*/
    tev[pg->imtrx] = cnt_elem_vars(); /*  tev_post is calculated in load_elem_tkn*/

    if (tnv[pg->imtrx] < 0) {
      DPRINTF(stderr, "%s:\tbad tnv.\n", yo);
      EH(-1, "\t");
    }

    if (tev[pg->imtrx] < 0) {
      DPRINTF(stderr, "%s:\tMaybe bad tev? See goma design committee ;) \n", yo);
      EH(-1, "\t");
    }

    rd[pg->imtrx]->nev = 0; /* number element variables in results */
    rd[pg->imtrx]->ngv = 0; /* number global variables in results  */
    rd[pg->imtrx]->nhv = 0; /* number history variables in results */

    rd[pg->imtrx]->ngv = 5; /* number global variables in results
     * see load_global_var_info for names
     */

    error = load_global_var_info(rd[pg->imtrx], 0, "CONV");
    error = load_global_var_info(rd[pg->imtrx], 1, "NEWT_IT");
    error = load_global_var_info(rd[pg->imtrx], 2, "MAX_IT");
    error = load_global_var_info(rd[pg->imtrx], 3, "CONVRATE");
    error = load_global_var_info(rd[pg->imtrx], 4, "MESH_VOLUME");

    gv = alloc_dbl_1(rd[pg->imtrx]->ngv, 0.0);

    /*
     *  Load output nodal types, kinds and names into the structure
     *  which will be used to define what's in the output file.
     */
    error = load_nodal_tkn(rd[pg->imtrx], &tnv[pg->imtrx], &tnv_post[pg->imtrx]);
    if (error != 0) {
      DPRINTF(stderr, "%s:  problem with load_nodal_tkn()\n", yo);
      EH(-1, "\t");
    }

    /*
     *  Load output element var types, kinds and names into the structure
     *  which will be used to define what's in the output file.
     */
    error = load_elem_tkn(rd[pg->imtrx], exo, tev[pg->imtrx], &tev_post[pg->imtrx]);
    if (error != 0) {
      DPRINTF(stderr, "%s:  problem with load_elem_tkn()\n", yo);
      EH(-1, "\t");
    }

    /*
     * Write out the names of the nodal variables that we will be sending to
     * the EXODUS II output file later - do only once if in library mode.
     */

    if (callnum == 1) {
      gvec_elem = (double ***) alloc_ptr_1(exo->num_elem_blocks);
      if ((tev[pg->imtrx] + tev_post[pg->imtrx]) > 0) {
        for (i = 0; i < exo->num_elem_blocks; i++) {
          gvec_elem[i] = (double **) alloc_ptr_1(tev[pg->imtrx] + tev_post[pg->imtrx]);
        }
      }
    }
  }

  if (callnum == 1) {
    wr_result_prelim_exo_segregated(rd, exo, ExoFileOut, gvec_elem);
  }

  /*
   * This gvec workhorse transports output variables as nodal based vectors
   * that are gather from the solution vector. Note: it is NOT a global
   * vector at all and only carries this processor's nodal variables to
   * the exodus database.
   */

  gvec = malloc(sizeof(double *) * upd->Total_Num_Matrices);
  for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
    asdv(&gvec[pg->imtrx], Num_Node);
  }

  /*
   * Allocate space and manipulate for all the nodes that this processor
   * is aware of...
   */

  numProcUnknowns = malloc(upd->Total_Num_Matrices * sizeof(int));
  for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
    numProcUnknowns[pg->imtrx] = NumUnknowns[pg->imtrx] + NumExtUnknowns[pg->imtrx];
  }

  resid_vector = malloc(upd->Total_Num_Matrices * sizeof(double *));
  resid_vector_sens = malloc(upd->Total_Num_Matrices * sizeof(double *));
  scale = malloc(upd->Total_Num_Matrices * sizeof(double *));

  for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
    asdv(&(resid_vector[pg->imtrx]), numProcUnknowns[pg->imtrx]);
    asdv(&(resid_vector_sens[pg->imtrx]), numProcUnknowns[pg->imtrx]);
    asdv(&(scale[pg->imtrx]), numProcUnknowns[pg->imtrx]);
  }

  /*
   * Allocate Aztec structures and initialize all elements to zero
   */
  if (callnum == 1) {
    ams = (struct Aztec_Linear_Solver_System **) alloc_ptr_1(
        upd->Total_Num_Matrices);
    for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
      ams[pg->imtrx] = alloc_struct_1(struct Aztec_Linear_Solver_System, 1);
    }
  }

  for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
    AZ_set_proc_config(ams[pg->imtrx]->proc_config, MPI_COMM_WORLD);
  }

  /* Allocate solution arrays on first call only */
  if (callnum == 1) {
    x = malloc(upd->Total_Num_Matrices * sizeof(double *));
    x_old = malloc(upd->Total_Num_Matrices * sizeof(double *));
    x_older = malloc(upd->Total_Num_Matrices * sizeof(double *));
    x_oldest = malloc(upd->Total_Num_Matrices * sizeof(double *));
    xdot = malloc(upd->Total_Num_Matrices * sizeof(double *));
    xdot_old = malloc(upd->Total_Num_Matrices * sizeof(double *));
    xdot_older = malloc(upd->Total_Num_Matrices * sizeof(double *));

    for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
      x[pg->imtrx] = alloc_dbl_1(numProcUnknowns[pg->imtrx], 0.0);
      x_old[pg->imtrx] = alloc_dbl_1(numProcUnknowns[pg->imtrx], 0.0);
      x_older[pg->imtrx] = alloc_dbl_1(numProcUnknowns[pg->imtrx], 0.0);
      x_oldest[pg->imtrx] = alloc_dbl_1(numProcUnknowns[pg->imtrx], 0.0);
      xdot[pg->imtrx] = alloc_dbl_1(numProcUnknowns[pg->imtrx], 0.0);
      xdot_old[pg->imtrx] = alloc_dbl_1(numProcUnknowns[pg->imtrx], 0.0);
      xdot_older[pg->imtrx] = alloc_dbl_1(numProcUnknowns[pg->imtrx], 0.0);
    }
  }

  x_update = malloc(upd->Total_Num_Matrices * sizeof(double *));
  for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
    x_update[pg->imtrx] = alloc_dbl_1(
        numProcUnknowns[pg->imtrx] + numProcUnknowns[pg->imtrx], 0.0);
  }

  /* Allocate sparse matrix */

  ija = malloc(upd->Total_Num_Matrices * sizeof(int *));
  ija_attic = malloc(upd->Total_Num_Matrices * sizeof(int *));
  a = malloc(upd->Total_Num_Matrices * sizeof(double *));
  a_old = malloc(upd->Total_Num_Matrices * sizeof(double *));

  if (strcmp(Matrix_Format, "epetra") == 0) {
    err = check_compatible_solver();
    EH(err,
        "Incompatible matrix solver for epetra, epetra supports amesos and aztecoo solvers.");
    check_parallel_error("Matrix format / Solver incompatibility");
    for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
      ams[pg->imtrx]->RowMatrix = EpetraCreateRowMatrix(
          num_internal_dofs[pg->imtrx] + num_boundary_dofs[pg->imtrx]);
      EpetraCreateGomaProblemGraph(ams[pg->imtrx], exo, dpi);
    }
  } else if (strcmp(Matrix_Format, "msr") == 0) {

    log_msg("alloc_MSR_sparse_arrays...");
    for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {
      alloc_MSR_sparse_arrays(&(ija[pg->imtrx]), &(a[pg->imtrx]), &(a_old[pg->imtrx]), 0,
          node_to_fill, exo, dpi);

      /*
       * An attic to store external dofs column names is needed when
       * running in parallel.
       */

      alloc_extern_ija_buffer(num_universe_dofs[pg->imtrx],
          num_internal_dofs[pg->imtrx] + num_boundary_dofs[pg->imtrx], ija[pg->imtrx],
          &(ija_attic[pg->imtrx]));

      /*
       * Any necessary one time initialization of the linear
       * solver package (Aztec).
       */
      ams[pg->imtrx]->bindx = ija[pg->imtrx];
      ams[pg->imtrx]->val = a[pg->imtrx];
      ams[pg->imtrx]->belfry = ija_attic[pg->imtrx];
      ams[pg->imtrx]->val_old = a_old[pg->imtrx];

      /*
       * These point to nowhere since we're using MSR instead of VBR
       * format.
       */

      ams[pg->imtrx]->indx = NULL;
      ams[pg->imtrx]->bpntr = NULL;
      ams[pg->imtrx]->rpntr = NULL;
      ams[pg->imtrx]->cpntr = NULL;

      ams[pg->imtrx]->npn = dpi->num_internal_nodes + dpi->num_boundary_nodes;
      ams[pg->imtrx]->npn_plus = dpi->num_internal_nodes + dpi->num_boundary_nodes
          + dpi->num_external_nodes;

      ams[pg->imtrx]->npu = num_internal_dofs[pg->imtrx] + num_boundary_dofs[pg->imtrx];
      ams[pg->imtrx]->npu_plus = num_universe_dofs[pg->imtrx];

      ams[pg->imtrx]->nnz = ija[pg->imtrx][num_internal_dofs[pg->imtrx]
          + num_boundary_dofs[pg->imtrx]] - 1;
      ams[pg->imtrx]->nnz_plus = ija[pg->imtrx][num_universe_dofs[pg->imtrx]];

    }
  } else {
    EH(-1, "Attempted to allocate unknown sparse matrix format");
  }

  /***************************************************************************
   *            STEADY STATE SOLUTION PROCEDURE
   ***************************************************************************/
  if (TimeIntegration == STEADY) {

    theta = 0.0; /* for steady problems. theta def in rf_fem.h */
    delta_t = 0.0;

    /* Right now, we are solving segregated problems in numerical order fashion
     * Matrix 0, then 1, then so on. In the future we could change that.
     */

    for (pg->imtrx = 0; pg->imtrx < upd->Total_Num_Matrices; pg->imtrx++) {

      find_and_set_Dirichlet(x[pg->imtrx], xdot[pg->imtrx], exo, dpi);

      matrix_systems_mask = 1;

      log_msg("sl_init()...");
      sl_init(matrix_systems_mask, ams, exo, dpi, cx);

#ifdef PARALLEL
      /*
       * Make sure the solver was properly initialized on all processors.
       */
      check_parallel_error("Solver initialization problems");
#endif /* PARALLEL */

      err = solve_nonlinear_problem(ams[pg->imtrx], x[pg->imtrx], delta_t,
          theta, x_old[pg->imtrx], x_older[pg->imtrx], xdot[pg->imtrx],
          xdot_old[pg->imtrx], resid_vector[pg->imtrx], x_update[pg->imtrx],
          scale[pg->imtrx], &converged, &nprint, tev[pg->imtrx],
          tev_post[pg->imtrx], gv, rd[pg->imtrx], NULL, NULL, gvec[pg->imtrx],
          gvec_elem, time1, exo, dpi, cx, 0, &time_step_reform, is_steady_state,
          NULL, NULL, time1, NULL,
          NULL, NULL, NULL);

    }

    write_solution_segregated(ExoFileOut, resid_vector, x,
                         x_old, xdot, xdot_old, tev, tev_post, gv,
                         rd, gvec, gvec_elem,
                         &nprint, delta_t, theta, time1, NULL,
                         exo, dpi);
  }
}
