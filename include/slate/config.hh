// Copyright (c) 2017-2022, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#ifndef SLATE_CONFIG_HH
#define SLATE_CONFIG_HH

#include <string.h>
#include <stdlib.h>

//------------------------------------------------------------------------------
/// Query whether MPI is GPU-aware.
/// Currently checks if environment variable $SLATE_GPU_AWARE_MPI is set
/// and either empty or 1. In the future, could check
/// `MPIX_GPU_query_support` (MPICH) or
/// `MPIX_Query_cuda_support` (Open MPI).
///
class GPU_Aware_MPI
{
public:
    /// @see bool gpu_aware_mpi()
    static bool value()
    {
        return get().gpu_aware_mpi_;
    }

    /// @see void gpu_aware_mpi( bool )
    static void value( bool val )
    {
        get().gpu_aware_mpi_ = val;
    }

private:
    /// @return GPU_Aware_MPI singleton.
    /// Uses thread-safe Scott Meyers' singleton to query on first call only.
    static GPU_Aware_MPI& get()
    {
        static GPU_Aware_MPI singleton;
        return singleton;
    }

    /// Constructor checks $SLATE_GPU_AWARE_MPI.
    GPU_Aware_MPI()
    {
        const char* env = getenv( "SLATE_GPU_AWARE_MPI" );
        gpu_aware_mpi_ = env != nullptr
                         && (strcmp( env, "" ) == 0
                             || strcmp( env, "1" ) == 0);
    }

    //----------------------------------------
    // Data

    /// Cached value whether MPI is GPU-aware.
    bool gpu_aware_mpi_;
};

//------------------------------------------------------------------------------
/// @return true if MPI is GPU-aware and not $SLATE_GPU_AWARE_MPI=0.
inline bool gpu_aware_mpi()
{
    return GPU_Aware_MPI::value();
}

//------------------------------------------------------------------------------
/// Set whether MPI is GPU-aware. Overrides $SLATE_GPU_AWARE_MPI.
/// @param[in] val: true if MPI is GPU-aware.
inline void gpu_aware_mpi( bool value )
{
    return GPU_Aware_MPI::value( value );
}

#endif // SLATE_CONFIG_HH
