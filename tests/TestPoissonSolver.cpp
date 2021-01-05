#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <iostream>
#include <exception>
#include <stdexcept>
#include <fstream>
#include <math.h>

#include "models/PoissonSolver.h"
#include "common/Utilities.h"

using namespace std;

//********************************************************
// Test lattice-Boltzmann solver of Poisson equation
//********************************************************

int main(int argc, char **argv)
{
    // Initialize MPI
    Utilities::startup( argc, argv );
    Utilities::MPI comm( MPI_COMM_WORLD );
    int rank = comm.getRank();
    int nprocs = comm.getSize();    
    {// Limit scope so variables that contain communicators will free before MPI_Finialize

        if (rank == 0){
            printf("********************************************************\n");
            printf("Running Test for LB-Poisson Solver \n");
            printf("********************************************************\n");
        }
		// Initialize compute device
		int device=ScaLBL_SetDevice(rank);
        NULL_USE( device );
		ScaLBL_DeviceBarrier();
		comm.barrier();

        PROFILE_ENABLE(1);
        //PROFILE_ENABLE_TRACE();
        //PROFILE_ENABLE_MEMORY();
        PROFILE_SYNCHRONIZE();
        PROFILE_START("Main");
        Utilities::setErrorHandlers();

        auto filename = argv[1];
        ScaLBL_Poisson PoissonSolver(rank,nprocs,comm); 

        // Initialize LB-Poisson model
        PoissonSolver.ReadParams(filename);
        PoissonSolver.SetDomain();    
        PoissonSolver.ReadInput();    
        PoissonSolver.Create();       
        PoissonSolver.Initialize();   

        //Initialize dummy charge density for test
        PoissonSolver.DummyChargeDensity();   

        PoissonSolver.Run(PoissonSolver.ChargeDensityDummy);
        PoissonSolver.getElectricPotential_debug(1);
        PoissonSolver.getElectricField_debug(1);

        if (rank==0) printf("Maximum timestep is reached and the simulation is completed\n");
        if (rank==0) printf("*************************************************************\n");

        PROFILE_STOP("Main");
        PROFILE_SAVE("TestPoissonSolver",1);
        // ****************************************************
        
    } // Limit scope so variables that contain communicators will free before MPI_Finialize

    Utilities::shutdown();
}


