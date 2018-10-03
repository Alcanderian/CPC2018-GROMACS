/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2013,2014, by the GROMACS development team, led by
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

#define UNROLLI    NBNXN_CPU_CLUSTER_I_SIZE
#define UNROLLJ    NBNXN_CPU_CLUSTER_I_SIZE

/* We could use nbat->xstride and nbat->fstride, but macros might be faster */
#define X_STRIDE   3
#define F_STRIDE   3
/* Local i-atom buffer strides */
#define XI_STRIDE  3
#define FI_STRIDE  3


/* All functionality defines are set here, except for:
 * CALC_ENERGIES, ENERGY_GROUPS which are defined before.
 * CHECK_EXCLS, which is set just before including the inner loop contents.
 */

/* We always calculate shift forces, because it's cheap anyhow */
#define CALC_SHIFTFORCES

#ifdef CALC_COUL_RF
#define NBK_FUNC_NAME2(ljt, feg) nbnxn_kernel ## _ElecRF ## ljt ## feg ## _ref
#endif
#ifdef CALC_COUL_TAB
#ifndef VDW_CUTOFF_CHECK
#define NBK_FUNC_NAME2(ljt, feg) nbnxn_kernel ## _ElecQSTab ## ljt ## feg ## _ref
#else
#define NBK_FUNC_NAME2(ljt, feg) nbnxn_kernel ## _ElecQSTabTwinCut ## ljt ## feg ## _ref
#endif
#endif

#if defined LJ_CUT && !defined LJ_EWALD
#define NBK_FUNC_NAME(feg) NBK_FUNC_NAME2(_VdwLJ, feg)
#elif defined LJ_FORCE_SWITCH
#define NBK_FUNC_NAME(feg) NBK_FUNC_NAME2(_VdwLJFsw, feg)
#elif defined LJ_POT_SWITCH
#define NBK_FUNC_NAME(feg) NBK_FUNC_NAME2(_VdwLJPsw, feg)
#elif defined LJ_EWALD
#ifdef LJ_EWALD_COMB_GEOM
#define NBK_FUNC_NAME(feg) NBK_FUNC_NAME2(_VdwLJEwCombGeom, feg)
#else
#define NBK_FUNC_NAME(feg) NBK_FUNC_NAME2(_VdwLJEwCombLB, feg)
#endif
#else
#error "No VdW type defined"
#endif

static void
#ifndef CALC_ENERGIES
NBK_FUNC_NAME(_F)
#else
#ifndef ENERGY_GROUPS
NBK_FUNC_NAME(_VF)
#else
NBK_FUNC_NAME(_VgrpF)
#endif
#endif
#undef NBK_FUNC_NAME
#undef NBK_FUNC_NAME2
(const nbnxn_pairlist_t     *nbl,
 const nbnxn_atomdata_t     *nbat,
 const interaction_const_t  *ic,
 rvec                       *shift_vec,
 real                       *f,
 real                       *fshift,
 real                       *Vvdw,
 real                       *Vc
)
{
    const nbnxn_ci_t   *nbln;
    const nbnxn_cj_t   *l_cj;
    const int          *type;
    const real         *q;
    const real         *shiftvec;
    const real         *x;
    const real         *nbfp;
    real                rcut2;
    real                rvdw2;
    int                 ntype2;
    real                facel;
    real               *nbfp_i;
    int                 n, ci, ci_sh;
    int                 ish, ishf;
    gmx_bool            do_LJ, half_LJ, do_coul, do_self;
    int                 cjind0, cjind1, cjind;
    int                 ip, jp;

    real                xi[UNROLLI*XI_STRIDE];
    real                fi[UNROLLI*FI_STRIDE];
    real                qi[UNROLLI];

    real       Vvdw_ci, Vc_ci;

    int        egp_mask;
    int        egp_sh_i[UNROLLI];

    real       swV3, swV4, swV5;
    real       swF2, swF3, swF4;

    real        lje_coeff2, lje_coeff6_6, lje_vc;
    const real *ljc;

    real       k_rf2;

    real       k_rf, c_rf;

    real       tabscale;

    real       halfsp;

#ifndef GMX_DOUBLE
    const real *tab_coul_FDV0;
#else
    const real *tab_coul_F;
    const real *tab_coul_V;
#endif

    int ninner;

#ifdef COUNT_PAIRS
    int npair = 0;
#endif

#ifdef LJ_POT_SWITCH
    swV3 = ic->vdw_switch.c3;
    swV4 = ic->vdw_switch.c4;
    swV5 = ic->vdw_switch.c5;
    swF2 = 3*ic->vdw_switch.c3;
    swF3 = 4*ic->vdw_switch.c4;
    swF4 = 5*ic->vdw_switch.c5;
#endif

#ifdef LJ_EWALD
    lje_coeff2   = ic->ewaldcoeff_lj*ic->ewaldcoeff_lj;
    lje_coeff6_6 = lje_coeff2*lje_coeff2*lje_coeff2/6.0;
    lje_vc       = ic->sh_lj_ewald;

    ljc          = nbat->nbfp_comb;
#endif

#ifdef CALC_COUL_RF
    k_rf2 = 2*ic->k_rf;
#ifdef CALC_ENERGIES
    k_rf = ic->k_rf;
    c_rf = ic->c_rf;
#endif
#endif
#ifdef CALC_COUL_TAB
    tabscale = ic->tabq_scale;
#ifdef CALC_ENERGIES
    halfsp = 0.5/ic->tabq_scale;
#endif

#ifndef GMX_DOUBLE
    tab_coul_FDV0 = ic->tabq_coul_FDV0;
#else
    tab_coul_F    = ic->tabq_coul_F;
    tab_coul_V    = ic->tabq_coul_V;
#endif
#endif

#ifdef ENERGY_GROUPS
    egp_mask = (1<<nbat->neg_2log) - 1;
#endif


    rcut2               = ic->rcoulomb*ic->rcoulomb;
#ifdef VDW_CUTOFF_CHECK
    rvdw2               = ic->rvdw*ic->rvdw;
#endif

    ntype2              = nbat->ntype*2;
    nbfp                = nbat->nbfp;
    q                   = nbat->q;
    type                = nbat->type;
    facel               = ic->epsfac;
    shiftvec            = shift_vec[0];
    x                   = nbat->x;

    l_cj = nbl->cj;

    ninner = 0;

    int macro_para = 0;
    enum {para_CALC_COUL_RF, para_CALC_COUL_TAB, para_CALC_ENERGIES, para_ENERGY_GROUPS, 
        para_LJ_CUT, para_LJ_EWALD, para_LJ_EWALD_COMB_GEOM, para_LJ_EWALD_COMB_LB, 
        para_LJ_FORCE_SWITCH, para_LJ_POT_SWITCH, para_VDW_CUTOFF_CHECK, 
        para_EXCL_FORCES, para_count
    };

    #ifdef CALC_COUL_RF
        macro_para |= 1 << para_CALC_COUL_RF;
    #endif
    #ifdef CALC_COUL_TAB
        macro_para |= 1 << para_CALC_COUL_TAB;
    #endif
    #ifdef CALC_ENERGIES
        macro_para |= 1 << para_CALC_ENERGIES;
    #endif
    #ifdef ENERGY_GROUPS
        macro_para |= 1 << para_ENERGY_GROUPS;
    #endif
    #ifdef LJ_CUT
        macro_para |= 1 << para_LJ_CUT;
    #endif
    #ifdef LJ_EWALD
        macro_para |= 1 << para_LJ_EWALD;
    #endif
    #ifdef LJ_EWALD_COMB_GEOM
        macro_para |= 1 << para_LJ_EWALD_COMB_GEOM;
    #endif
    #ifdef LJ_EWALD_COMB_LB
        macro_para |= 1 << para_LJ_EWALD_COMB_LB;
    #endif
    #ifdef LJ_FORCE_SWITCH
        macro_para |= 1 << para_LJ_FORCE_SWITCH;
    #endif
    #ifdef LJ_POT_SWITCH
        macro_para |= 1 << para_LJ_POT_SWITCH;
    #endif
    #ifdef VDW_CUTOFF_CHECK
        macro_para |= 1 << para_VDW_CUTOFF_CHECK;
    #endif

    #define macro_has(para_name) ((macro_para >> para_name) & 1)

    // host_param.host_to_device[PARAM_DEVICE_ACTION] = DEVICE_ACTION_RUN;
    // notice_device()；
    subcore_func(
    	macro_para,
    	nbl,
		nbat,
		ic,
		shift_vec,
		f,
		fshift,
		Vvdw,
		Vc,
		nbln,
		l_cj,
		type,
		q,
		shiftvec,
		x,
		nbfp,
		rcut2,
		rvdw2,
		ntype2,
		facel,
		nbfp_i,
		n,
		ci,
		ci_sh,
		ish,
		ishf,
		do_LJ,
		half_LJ,
		do_coul,
		do_self,
		cjind0,
		cjind1,
		cjind,
		ip,
		jp,

		xi,
		fi,
		qi,

		Vvdw_ci,
		Vc_ci,

		egp_mask,
		egp_sh_i,

		swV3,
		swV4,
		swV5,
		swF2,
		swF3,
		swF4,

		lje_coeff2,
		lje_coeff6_6,
		lje_vc,
		ljc,

		k_rf2,

		k_rf, 
		c_rf,

		tabscale,

		halfsp,

		#ifndef GMX_DOUBLE
		tab_coul_FDV0,
		#else
		tab_coul_F,
		tab_coul_V,
		#endif
		ninner
    );
    

#ifdef COUNT_PAIRS
    printf("atom pairs %d\n", npair);
#endif
}

#undef CALC_SHIFTFORCES

#undef X_STRIDE
#undef F_STRIDE
#undef XI_STRIDE
#undef FI_STRIDE

#undef UNROLLI
#undef UNROLLJ
