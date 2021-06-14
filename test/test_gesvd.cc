// Copyright (c) 2017-2020, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/slate.hh"
#include "test.hh"
#include "blas/flops.hh"
#include "lapack/flops.hh"
#include "print_matrix.hh"
#include "grid_utils.hh"

#include "scalapack_wrappers.hh"
#include "scalapack_support_routines.hh"
#include "scalapack_copy.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#define SLATE_HAVE_SCALAPACK
//------------------------------------------------------------------------------
template <typename scalar_t>
void test_gesvd_work(Params& params, bool run)
{
    using real_t = blas::real_type<scalar_t>;
    using blas::real;
    using llong = long long;

    // get & mark input values
    lapack::Job jobu = params.jobu();
    lapack::Job jobvt = params.jobvt();
    int64_t m = params.dim.m();
    int64_t n = params.dim.n();

    int64_t p = params.grid.m();
    int64_t q = params.grid.n();
    int64_t nb = params.nb();
    int64_t ib = params.ib();
    int64_t panel_threads = params.panel_threads();
    int64_t lookahead = params.lookahead();
    bool ref_only = params.ref() == 'o';
    bool ref = params.ref() == 'y' || ref_only;
    bool check = params.check() == 'y' && ! ref_only;
    bool trace = params.trace() == 'y';
    int verbose = params.verbose();
    slate::Origin origin = params.origin();
    slate::Target target = params.target();
    params.matrix.mark();

    params.time();
    params.ref_time();
    // params.gflops();
    // params.ref_gflops();

    if (! run)
        return;

    slate::Options const opts =  {
        {slate::Option::Lookahead, lookahead},
        {slate::Option::Target, target},
        {slate::Option::MaxPanelThreads, panel_threads},
        {slate::Option::InnerBlocking, ib}
    };

    // Constants
    const int izero = 0, ione = 1;

    // Local values
    int64_t minmn = std::min(m, n);
    int myrow, mycol;
    int mpi_rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
    gridinfo(mpi_rank, p, q, &myrow, &mycol);

    // skip unsupported
    if (jobu != lapack::Job::NoVec) {
        if (mpi_rank == 0)
            printf("\nskipping: Only singular values supported (vectors not yet supported)\n");
        return;
    }

    // figure out local size, allocate, create descriptor, initialize
    // matrix A (local input), m-by-n
    int64_t mlocA = num_local_rows_cols(m, nb, myrow, p);
    int64_t nlocA = num_local_rows_cols(n, nb, mycol, q);
    int64_t lldA  = blas::max(1, mlocA); // local leading dimension of A
    std::vector<scalar_t> A_data(lldA*nlocA);

    // matrix U (local output), U(m, minmn), singular values of A
    int64_t mlocU = num_local_rows_cols(m, nb, myrow, p);
    int64_t nlocU = num_local_rows_cols(minmn, nb, mycol, q);
    int64_t lldU  = blas::max(1, mlocU); // local leading dimension of U
    std::vector<scalar_t> U_data(lldU * nlocU, 0);

    // matrix VT (local output), VT(minmn, n)
    int64_t mlocVT = num_local_rows_cols(minmn, nb, myrow, p);
    int64_t nlocVT = num_local_rows_cols(n, nb, mycol, q);
    int64_t lldVT  = blas::max(1, mlocVT); // local leading dimension of VT
    std::vector<scalar_t> VT_data(lldVT * nlocVT, 0);

    // array S (global output), S(size), singular values of A
    std::vector<real_t> S_data(minmn);

    slate::Matrix<scalar_t> A; // (m, n);
    slate::Matrix<scalar_t> U; // (m, minmn);
    slate::Matrix<scalar_t> VT; // (minmn, n);

    if (origin != slate::Origin::ScaLAPACK) {
        // Copy local ScaLAPACK data to GPU or CPU tiles.
        slate::Target origin_target = origin2target(origin);
        A = slate::Matrix<scalar_t>(m, n, nb, p, q, MPI_COMM_WORLD);
        A.insertLocalTiles(origin_target);

        U = slate::Matrix<scalar_t>(m, minmn, nb, p, q, MPI_COMM_WORLD);
        U.insertLocalTiles(origin_target);

        VT = slate::Matrix<scalar_t>(minmn, n, nb, p, q, MPI_COMM_WORLD);
        VT.insertLocalTiles(origin_target);
    }
    else {
        // create SLATE matrices from the ScaLAPACK layouts
        A = slate::Matrix<scalar_t>::fromScaLAPACK(m, n, &A_data[0],  lldA,  nb, p, q, MPI_COMM_WORLD);
        U = slate::Matrix<scalar_t>::fromScaLAPACK(m, minmn, &U_data[0], lldU, nb, p, q, MPI_COMM_WORLD);
        VT = slate::Matrix<scalar_t>::fromScaLAPACK(minmn, n, &VT_data[0], lldVT, nb, p, q, MPI_COMM_WORLD);
    }

    if (verbose >= 1) {
        printf( "%% A   %6lld-by-%6lld\n", llong(   A.m() ), llong(   A.n() ) );
        printf( "%% U   %6lld-by-%6lld\n", llong(   U.m() ), llong(   U.n() ) );
        printf( "%% VT  %6lld-by-%6lld\n", llong(  VT.m() ), llong(  VT.n() ) );
    }

    if (verbose > 1) {
        print_matrix( "A",  A  );
        print_matrix( "U",  U  );
        print_matrix( "VT", VT );
    }

    //params.matrix.kind.set_default("svd");
    //params.matrix.cond.set_default(1.e16);

    slate::generate_matrix( params.matrix, A);
    if (verbose > 1) {
        print_matrix( "A0",  A  );
    }

    std::vector<real_t> Sref_data;
    slate::Matrix<scalar_t> Aref;
    if (check || ref) {
        Sref_data.resize(S_data.size());
        Aref = slate::Matrix<scalar_t>(m, n, nb, p, q, MPI_COMM_WORLD);
        slate::Target origin_target = origin2target(origin);
        Aref.insertLocalTiles(origin_target);
        slate::copy(A, Aref);
    }

    if (! ref_only) {
        if (trace) slate::trace::Trace::on();
        else slate::trace::Trace::off();

        double time = barrier_get_wtime(MPI_COMM_WORLD);

        //==================================================
        // Run SLATE test.
        //==================================================
        slate::svd_vals(A, S_data, opts);
        // Using traditional BLAS/LAPACK name
        // slate::gesvd(A, S_data, opts);

        time = barrier_get_wtime(MPI_COMM_WORLD) - time;

        if (trace) slate::trace::Trace::finish();

        // compute and save timing/performance
        params.time() = time;

        if (verbose > 1) {
            print_matrix( "A",  A  );
            print_matrix( "U",  U  );
            print_matrix( "VT", VT );
        }
    }

    if (check || ref) {
        #ifdef SLATE_HAVE_SCALAPACK
            // Run reference routine from ScaLAPACK

            // BLACS/MPI variables
            int ictxt, p_, q_, myrow_, mycol_, info;
            int mpi_rank_ = 0, nprocs = 1;

            // initialize BLACS and ScaLAPACK
            Cblacs_pinfo(&mpi_rank_, &nprocs);
            slate_assert( mpi_rank == mpi_rank_ );
            slate_assert(p*q <= nprocs);
            Cblacs_get(-1, 0, &ictxt);
            Cblacs_gridinit(&ictxt, "Col", p, q);
            Cblacs_gridinfo(ictxt, &p_, &q_, &myrow_, &mycol_);
            slate_assert( p == p_ );
            slate_assert( q == q_ );
            slate_assert( myrow == myrow_ );
            slate_assert( mycol == mycol_ );

            int A_desc[9];
            scalapack_descinit(A_desc, m, n, nb, nb, izero, izero, ictxt, mlocA, &info);
            slate_assert(info == 0);
            std::vector<scalar_t> Aref_data(lldA*nlocA);
            copy(Aref, &Aref_data[0], A_desc);

            int U_desc[9];
            scalapack_descinit(U_desc, m, minmn, nb, nb, izero, izero, ictxt, mlocU, &info);
            slate_assert(info == 0);

            int VT_desc[9];
            scalapack_descinit(VT_desc, minmn, n, nb, nb, izero, izero, ictxt, mlocVT, &info);
            slate_assert(info == 0);

            // set MKL num threads appropriately for parallel BLAS
            int omp_num_threads = 1;
            #pragma omp parallel
            { omp_num_threads = omp_get_num_threads(); }
            int saved_num_threads = slate_set_num_blas_threads(omp_num_threads);

            // query for workspace size
            int64_t info_ref = 0;
            scalar_t dummy_work;
            real_t dummy_rwork;
            scalapack_pgesvd(job2str(jobu), job2str(jobvt), m, n,
                             &Aref_data[0],  ione, ione, A_desc, &Sref_data[0],
                             &U_data[0],  ione, ione, U_desc,
                             &VT_data[0], ione, ione, VT_desc,
                             &dummy_work, -1, &dummy_rwork, &info_ref);
            slate_assert(info_ref == 0);
            int64_t lwork  = int64_t( real( dummy_work ) );
            int64_t lrwork = int64_t( dummy_rwork );
            std::vector<scalar_t> work(lwork);
            std::vector<real_t> rwork(lrwork);

            //==================================================
            // Run ScaLAPACK reference routine.
            //==================================================
            double time = barrier_get_wtime(MPI_COMM_WORLD);
            scalapack_pgesvd(job2str(jobu), job2str(jobvt), m, n,
                             &Aref_data[0],  ione, ione, A_desc, &Sref_data[0],
                             &U_data[0],  ione, ione, U_desc,
                             &VT_data[0], ione, ione, VT_desc,
                             &work[0], lwork, &rwork[0], &info_ref);
            slate_assert(info_ref == 0);
            time = barrier_get_wtime(MPI_COMM_WORLD) - time;

            params.ref_time() = time;

            slate_set_num_blas_threads(saved_num_threads);

            // Reference Scalapack was run, check reference against test
            // Perform a local operation to get differences S_data = S_data - Sref_data
            blas::axpy(Sref_data.size(), -1.0, &Sref_data[0], 1, &S_data[0], 1);

            // Relative forward error: || Sref_data - S_data || / || Sref_data ||.
            params.error() = blas::asum(S_data.size(), &S_data[0], 1)
                           / blas::asum(Sref_data.size(), &Sref_data[0], 1);

            real_t tol = params.tol() * 0.5 * std::numeric_limits<real_t>::epsilon();
            params.okay() = (params.error() <= tol);
            Cblacs_gridexit(ictxt);
            //Cblacs_exit(1) does not handle re-entering
        #else
            if (mpi_rank == 0)
                printf( "ScaLAPACK not available\n" );
        #endif
    }
}

// -----------------------------------------------------------------------------
void test_gesvd(Params& params, bool run)
{
    switch (params.datatype()) {
        case testsweeper::DataType::Integer:
            throw std::exception();
            break;

        case testsweeper::DataType::Single:
            test_gesvd_work<float> (params, run);
            break;

        case testsweeper::DataType::Double:
            test_gesvd_work<double> (params, run);
            break;

        case testsweeper::DataType::SingleComplex:
            test_gesvd_work<std::complex<float>> (params, run);
            break;

        case testsweeper::DataType::DoubleComplex:
            test_gesvd_work<std::complex<double>> (params, run);
            break;
    }
}
