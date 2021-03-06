/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2018, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/* \internal \file
 *
 * \brief Implements atom distribution functions.
 *
 * \author Berk Hess <hess@kth.se>
 * \ingroup module_domdec
 */

#include "gmxpre.h"

#include "distribute.h"

#include "config.h"

#include <vector>

#include "gromacs/domdec/domdec_network.h"
#include "gromacs/math/vec.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/mdtypes/df_history.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/smalloc.h"

#include "atomdistribution.h"
#include "cellsizes.h"
#include "domdec_internal.h"
#include "utility.h"

static void dd_distribute_vec_sendrecv(gmx_domdec_t *dd, const t_block *cgs,
                                       const rvec *v, rvec *lv)
{
    AtomDistribution *ma;
    int               nalloc = 0;
    rvec             *buf    = nullptr;

    if (DDMASTER(dd))
    {
        ma  = dd->ma;

        for (int rank = 0; rank < dd->nnodes; rank++)
        {
            if (rank != dd->rank)
            {
                const auto &domainGroups = ma->domainGroups[rank];

                if (domainGroups.numAtoms > nalloc)
                {
                    nalloc = over_alloc_dd(domainGroups.numAtoms);
                    srenew(buf, nalloc);
                }
                /* Use lv as a temporary buffer */
                int localAtom = 0;
                for (const int &cg : domainGroups.atomGroups)
                {
                    for (int globalAtom = cgs->index[cg]; globalAtom < cgs->index[cg + 1]; globalAtom++)
                    {
                        copy_rvec(v[globalAtom], buf[localAtom++]);
                    }
                }
                GMX_RELEASE_ASSERT(localAtom == domainGroups.numAtoms, "The index count and number of indices should match");

#if GMX_MPI
                MPI_Send(buf, domainGroups.numAtoms*sizeof(rvec), MPI_BYTE,
                         rank, rank, dd->mpi_comm_all);
#endif
            }
        }
        sfree(buf);

        const auto &domainGroups = ma->domainGroups[dd->masterrank];
        int         localAtom    = 0;
        for (const int &cg : domainGroups.atomGroups)
        {
            for (int globalAtom = cgs->index[cg]; globalAtom < cgs->index[cg + 1]; globalAtom++)
            {
                copy_rvec(v[globalAtom], lv[localAtom++]);
            }
        }
    }
    else
    {
#if GMX_MPI
        MPI_Recv(lv, dd->nat_home*sizeof(rvec), MPI_BYTE, dd->masterrank,
                 MPI_ANY_TAG, dd->mpi_comm_all, MPI_STATUS_IGNORE);
#endif
    }
}

static void dd_distribute_vec_scatterv(gmx_domdec_t *dd, const t_block *cgs,
                                       const rvec *v, rvec *lv)
{
    int *sendCounts    = nullptr;
    int *displacements = nullptr;

    if (DDMASTER(dd))
    {
        AtomDistribution *ma = dd->ma;

        get_commbuffer_counts(ma, &sendCounts, &displacements);

        gmx::ArrayRef<gmx::RVec> buffer = ma->rvecBuffer;
        int localAtom                   = 0;
        for (int rank = 0; rank < dd->nnodes; rank++)
        {
            const auto &domainGroups = ma->domainGroups[rank];
            for (const int &cg : domainGroups.atomGroups)
            {
                for (int globalAtom = cgs->index[cg]; globalAtom < cgs->index[cg + 1]; globalAtom++)
                {
                    copy_rvec(v[globalAtom], buffer[localAtom++]);
                }
            }
        }
    }

    dd_scatterv(dd, sendCounts, displacements,
                DDMASTER(dd) ? dd->ma->rvecBuffer.data() : nullptr,
                dd->nat_home*sizeof(rvec), lv);
}

static void dd_distribute_vec(gmx_domdec_t *dd, const t_block *cgs,
                              const rvec *v, rvec *lv)
{
    if (dd->nnodes <= c_maxNumRanksUseSendRecvForScatterAndGather)
    {
        dd_distribute_vec_sendrecv(dd, cgs, v, lv);
    }
    else
    {
        dd_distribute_vec_scatterv(dd, cgs, v, lv);
    }
}

static void dd_distribute_dfhist(gmx_domdec_t *dd, df_history_t *dfhist)
{
    if (dfhist == nullptr)
    {
        return;
    }

    dd_bcast(dd, sizeof(int), &dfhist->bEquil);
    dd_bcast(dd, sizeof(int), &dfhist->nlambda);
    dd_bcast(dd, sizeof(real), &dfhist->wl_delta);

    if (dfhist->nlambda > 0)
    {
        int nlam = dfhist->nlambda;
        dd_bcast(dd, sizeof(int)*nlam, dfhist->n_at_lam);
        dd_bcast(dd, sizeof(real)*nlam, dfhist->wl_histo);
        dd_bcast(dd, sizeof(real)*nlam, dfhist->sum_weights);
        dd_bcast(dd, sizeof(real)*nlam, dfhist->sum_dg);
        dd_bcast(dd, sizeof(real)*nlam, dfhist->sum_minvar);
        dd_bcast(dd, sizeof(real)*nlam, dfhist->sum_variance);

        for (int i = 0; i < nlam; i++)
        {
            dd_bcast(dd, sizeof(real)*nlam, dfhist->accum_p[i]);
            dd_bcast(dd, sizeof(real)*nlam, dfhist->accum_m[i]);
            dd_bcast(dd, sizeof(real)*nlam, dfhist->accum_p2[i]);
            dd_bcast(dd, sizeof(real)*nlam, dfhist->accum_m2[i]);
            dd_bcast(dd, sizeof(real)*nlam, dfhist->Tij[i]);
            dd_bcast(dd, sizeof(real)*nlam, dfhist->Tij_empirical[i]);
        }
    }
}

static void dd_distribute_state(gmx_domdec_t *dd, const t_block *cgs,
                                const t_state *state, t_state *state_local,
                                PaddedRVecVector *f)
{
    int nh = state_local->nhchainlength;

    if (DDMASTER(dd))
    {
        GMX_RELEASE_ASSERT(state->nhchainlength == nh, "The global and local Nose-Hoover chain lengths should match");

        for (int i = 0; i < efptNR; i++)
        {
            state_local->lambda[i] = state->lambda[i];
        }
        state_local->fep_state = state->fep_state;
        state_local->veta      = state->veta;
        state_local->vol0      = state->vol0;
        copy_mat(state->box, state_local->box);
        copy_mat(state->box_rel, state_local->box_rel);
        copy_mat(state->boxv, state_local->boxv);
        copy_mat(state->svir_prev, state_local->svir_prev);
        copy_mat(state->fvir_prev, state_local->fvir_prev);
        if (state->dfhist != nullptr)
        {
            copy_df_history(state_local->dfhist, state->dfhist);
        }
        for (int i = 0; i < state_local->ngtc; i++)
        {
            for (int j = 0; j < nh; j++)
            {
                state_local->nosehoover_xi[i*nh+j]        = state->nosehoover_xi[i*nh+j];
                state_local->nosehoover_vxi[i*nh+j]       = state->nosehoover_vxi[i*nh+j];
            }
            state_local->therm_integral[i] = state->therm_integral[i];
        }
        for (int i = 0; i < state_local->nnhpres; i++)
        {
            for (int j = 0; j < nh; j++)
            {
                state_local->nhpres_xi[i*nh+j]        = state->nhpres_xi[i*nh+j];
                state_local->nhpres_vxi[i*nh+j]       = state->nhpres_vxi[i*nh+j];
            }
        }
        state_local->baros_integral = state->baros_integral;
    }
    dd_bcast(dd, ((efptNR)*sizeof(real)), state_local->lambda.data());
    dd_bcast(dd, sizeof(int), &state_local->fep_state);
    dd_bcast(dd, sizeof(real), &state_local->veta);
    dd_bcast(dd, sizeof(real), &state_local->vol0);
    dd_bcast(dd, sizeof(state_local->box), state_local->box);
    dd_bcast(dd, sizeof(state_local->box_rel), state_local->box_rel);
    dd_bcast(dd, sizeof(state_local->boxv), state_local->boxv);
    dd_bcast(dd, sizeof(state_local->svir_prev), state_local->svir_prev);
    dd_bcast(dd, sizeof(state_local->fvir_prev), state_local->fvir_prev);
    dd_bcast(dd, ((state_local->ngtc*nh)*sizeof(double)), state_local->nosehoover_xi.data());
    dd_bcast(dd, ((state_local->ngtc*nh)*sizeof(double)), state_local->nosehoover_vxi.data());
    dd_bcast(dd, state_local->ngtc*sizeof(double), state_local->therm_integral.data());
    dd_bcast(dd, ((state_local->nnhpres*nh)*sizeof(double)), state_local->nhpres_xi.data());
    dd_bcast(dd, ((state_local->nnhpres*nh)*sizeof(double)), state_local->nhpres_vxi.data());

    /* communicate df_history -- required for restarting from checkpoint */
    dd_distribute_dfhist(dd, state_local->dfhist);

    dd_resize_state(state_local, f, dd->nat_home);

    if (state_local->flags & (1 << estX))
    {
        const rvec *xGlobal = (DDMASTER(dd) ? as_rvec_array(state->x.data()) : nullptr);
        dd_distribute_vec(dd, cgs, xGlobal, as_rvec_array(state_local->x.data()));
    }
    if (state_local->flags & (1 << estV))
    {
        const rvec *vGlobal = (DDMASTER(dd) ? as_rvec_array(state->v.data()) : nullptr);
        dd_distribute_vec(dd, cgs, vGlobal, as_rvec_array(state_local->v.data()));
    }
    if (state_local->flags & (1 << estCGP))
    {
        const rvec *cgpGlobal = (DDMASTER(dd) ? as_rvec_array(state->cg_p.data()) : nullptr);
        dd_distribute_vec(dd, cgs, cgpGlobal, as_rvec_array(state_local->cg_p.data()));
    }
}

static std::vector < std::vector < int>>
getAtomGroupDistribution(FILE *fplog,
                         const matrix box, const gmx_ddbox_t &ddbox,
                         const t_block *cgs, rvec pos[],
                         gmx_domdec_t *dd)
{
    AtomDistribution *ma = dd->ma;

    /* Clear the count */
    for (int rank = 0; rank < dd->nnodes; rank++)
    {
        ma->domainGroups[rank].numAtoms = 0;
    }

    matrix tcm;
    make_tric_corr_matrix(dd->npbcdim, box, tcm);

    ivec       npulse;
    const auto cellBoundaries =
        set_dd_cell_sizes_slb(dd, &ddbox, setcellsizeslbMASTER, npulse);

    const int *cgindex = cgs->index;

    std::vector < std::vector < int>> indices(dd->nnodes);

    /* Compute the center of geometry for all charge groups */
    for (int icg = 0; icg < cgs->nr; icg++)
    {
        /* Set the reference location cg_cm for assigning the group */
        rvec cg_cm;
        int  k0      = cgindex[icg];
        int  k1      = cgindex[icg + 1];
        int  nrcg    = k1 - k0;
        if (nrcg == 1)
        {
            copy_rvec(pos[k0], cg_cm);
        }
        else
        {
            real inv_ncg = 1/static_cast<real>(nrcg);

            clear_rvec(cg_cm);
            for (int k = k0; k < k1; k++)
            {
                rvec_inc(cg_cm, pos[k]);
            }
            for (int d = 0; d < DIM; d++)
            {
                cg_cm[d] *= inv_ncg;
            }
        }
        /* Put the charge group in the box and determine the cell index ind */
        ivec ind;
        for (int d = DIM - 1; d >= 0; d--)
        {
            real pos_d = cg_cm[d];
            if (d < dd->npbcdim)
            {
                bool bScrew = (dd->bScrewPBC && d == XX);
                if (ddbox.tric_dir[d] && dd->nc[d] > 1)
                {
                    /* Use triclinic coordinates for this dimension */
                    for (int j = d + 1; j < DIM; j++)
                    {
                        pos_d += cg_cm[j]*tcm[j][d];
                    }
                }
                while (pos_d >= box[d][d])
                {
                    pos_d -= box[d][d];
                    rvec_dec(cg_cm, box[d]);
                    if (bScrew)
                    {
                        cg_cm[YY] = box[YY][YY] - cg_cm[YY];
                        cg_cm[ZZ] = box[ZZ][ZZ] - cg_cm[ZZ];
                    }
                    for (int k = k0; k < k1; k++)
                    {
                        rvec_dec(pos[k], box[d]);
                        if (bScrew)
                        {
                            pos[k][YY] = box[YY][YY] - pos[k][YY];
                            pos[k][ZZ] = box[ZZ][ZZ] - pos[k][ZZ];
                        }
                    }
                }
                while (pos_d < 0)
                {
                    pos_d += box[d][d];
                    rvec_inc(cg_cm, box[d]);
                    if (bScrew)
                    {
                        cg_cm[YY] = box[YY][YY] - cg_cm[YY];
                        cg_cm[ZZ] = box[ZZ][ZZ] - cg_cm[ZZ];
                    }
                    for (int k = k0; k < k1; k++)
                    {
                        rvec_inc(pos[k], box[d]);
                        if (bScrew)
                        {
                            pos[k][YY] = box[YY][YY] - pos[k][YY];
                            pos[k][ZZ] = box[ZZ][ZZ] - pos[k][ZZ];
                        }
                    }
                }
            }
            /* This could be done more efficiently */
            ind[d] = 0;
            while (ind[d]+1 < dd->nc[d] && pos_d >= cellBoundaries[d][ind[d] + 1])
            {
                ind[d]++;
            }
        }
        int domainIndex = dd_index(dd->nc, ind);
        indices[domainIndex].push_back(icg);
        ma->domainGroups[domainIndex].numAtoms += cgindex[icg + 1] - cgindex[icg];
    }

    if (fplog)
    {
        // Use double for the sums to avoid natoms^2 overflowing
        // (65537^2 > 2^32)
        int    nat_sum, nat_min, nat_max;
        double nat2_sum;

        nat_sum  = 0;
        nat2_sum = 0;
        nat_min  = ma->domainGroups[0].numAtoms;
        nat_max  = ma->domainGroups[0].numAtoms;
        for (int rank = 0; rank < dd->nnodes; rank++)
        {
            int numAtoms  = ma->domainGroups[rank].numAtoms;
            nat_sum      += numAtoms;
            // cast to double to avoid integer overflows when squaring
            nat2_sum     += gmx::square(static_cast<double>(numAtoms));
            nat_min       = std::min(nat_min, numAtoms);
            nat_max       = std::max(nat_max, numAtoms);
        }
        nat_sum  /= dd->nnodes;
        nat2_sum /= dd->nnodes;

        fprintf(fplog, "Atom distribution over %d domains: av %d stddev %d min %d max %d\n",
                dd->nnodes,
                nat_sum,
                static_cast<int>(std::sqrt(nat2_sum - gmx::square(static_cast<double>(nat_sum)) + 0.5)),
                nat_min, nat_max);
    }

    return indices;
}

static void distributeAtomGroups(FILE *fplog, gmx_domdec_t *dd,
                                 const t_block *cgs,
                                 const matrix box, const gmx_ddbox_t *ddbox,
                                 rvec pos[])
{
    AtomDistribution *ma = nullptr;
    int              *ibuf, buf2[2] = { 0, 0 };
    gmx_bool          bMaster = DDMASTER(dd);

    std::vector < std::vector < int>> groupIndices;

    if (bMaster)
    {
        ma = dd->ma;

        if (dd->bScrewPBC)
        {
            check_screw_box(box);
        }

        groupIndices = getAtomGroupDistribution(fplog, box, *ddbox, cgs, pos, dd);

        for (int rank = 0; rank < dd->nnodes; rank++)
        {
            ma->intBuffer[rank*2]     = groupIndices[rank].size();
            ma->intBuffer[rank*2 + 1] = ma->domainGroups[rank].numAtoms;
        }
        ibuf = ma->intBuffer.data();
    }
    else
    {
        ibuf = nullptr;
    }
    dd_scatter(dd, 2*sizeof(int), ibuf, buf2);

    dd->ncg_home = buf2[0];
    dd->nat_home = buf2[1];
    dd->ncg_tot  = dd->ncg_home;
    dd->nat_tot  = dd->nat_home;
    if (dd->ncg_home > dd->cg_nalloc || dd->cg_nalloc == 0)
    {
        dd->cg_nalloc = over_alloc_dd(dd->ncg_home);
        srenew(dd->index_gl, dd->cg_nalloc);
        srenew(dd->cgindex, dd->cg_nalloc+1);
    }

    if (bMaster)
    {
        ma->atomGroups.clear();

        int groupOffset = 0;
        for (int rank = 0; rank < dd->nnodes; rank++)
        {
            ma->intBuffer[rank]                = groupIndices[rank].size()*sizeof(int);
            ma->intBuffer[dd->nnodes + rank]   = groupOffset*sizeof(int);

            ma->atomGroups.insert(ma->atomGroups.end(),
                                  groupIndices[rank].begin(), groupIndices[rank].end());

            ma->domainGroups[rank].atomGroups  = gmx::constArrayRefFromArray(ma->atomGroups.data() + groupOffset, groupIndices[rank].size());

            groupOffset                            += groupIndices[rank].size();
        }
    }

    dd_scatterv(dd,
                bMaster ? ma->intBuffer.data() : nullptr,
                bMaster ? ma->intBuffer.data() + dd->nnodes : nullptr,
                bMaster ? ma->atomGroups.data() : nullptr,
                dd->ncg_home*sizeof(int), dd->index_gl);

    /* Determine the home charge group sizes */
    dd->cgindex[0] = 0;
    for (int i = 0; i < dd->ncg_home; i++)
    {
        int cg_gl        = dd->index_gl[i];
        dd->cgindex[i+1] =
            dd->cgindex[i] + cgs->index[cg_gl+1] - cgs->index[cg_gl];
    }

    if (debug)
    {
        fprintf(debug, "Home charge groups:\n");
        for (int i = 0; i < dd->ncg_home; i++)
        {
            fprintf(debug, " %d", dd->index_gl[i]);
            if (i % 10 == 9)
            {
                fprintf(debug, "\n");
            }
        }
        fprintf(debug, "\n");
    }
}

void distributeState(FILE                *fplog,
                     gmx_domdec_t        *dd,
                     t_state             *state_global,
                     const t_block       &cgs_gl,
                     const gmx_ddbox_t   &ddbox,
                     t_state             *state_local,
                     PaddedRVecVector    *f)
{
    rvec *xGlobal = (DDMASTER(dd) ? as_rvec_array(state_global->x.data()) : nullptr);

    distributeAtomGroups(fplog, dd, &cgs_gl,
                         DDMASTER(dd) ? state_global->box : nullptr,
                         &ddbox, xGlobal);

    dd_distribute_state(dd, &cgs_gl,
                        state_global, state_local, f);
}
