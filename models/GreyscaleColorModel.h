/*
Implementation of two-fluid greyscale color lattice boltzmann model
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <iostream>
#include <exception>
#include <stdexcept>
#include <fstream>

#include "common/Communication.h"
#include "analysis/GreyPhase.h"
#include "common/MPI.h"
#include "ProfilerApp.h"
#include "threadpool/thread_pool.h"

class ScaLBL_GreyscaleColorModel{
public:
	ScaLBL_GreyscaleColorModel(int RANK, int NP, const Utilities::MPI& COMM);
	~ScaLBL_GreyscaleColorModel();	
	
	// functions in they should be run
	void ReadParams(string filename);
	void ReadParams(std::shared_ptr<Database> db0);
	void SetDomain();
	void ReadInput();
	void Create();
	void Initialize();
	void Run();
	void WriteDebug();
	
	bool Restart,pBC;
	bool REVERSE_FLOW_DIRECTION;
	int timestep,timestepMax;
	int BoundaryCondition;
	double tauA,tauB,rhoA,rhoB,alpha,beta;
    double tauA_eff,tauB_eff;
	double Fx,Fy,Fz,flux;
	double din,dout,inletA,inletB,outletA,outletB;
    double GreyPorosity;
    bool RecoloringOff;//recoloring can be turn off for grey nodes if this is true
    //double W;//wetting strength paramter for capillary pressure penalty for grey nodes
	
	int Nx,Ny,Nz,N,Np;
	int rank,nprocx,nprocy,nprocz,nprocs;
	double Lx,Ly,Lz;

	std::shared_ptr<Domain> Dm;   // this domain is for analysis
	std::shared_ptr<Domain> Mask; // this domain is for lbm
	std::shared_ptr<ScaLBL_Communicator> ScaLBL_Comm;
	std::shared_ptr<ScaLBL_Communicator> ScaLBL_Comm_Regular;
    std::shared_ptr<GreyPhaseAnalysis> Averages;
    
    // input database
    std::shared_ptr<Database> db;
    std::shared_ptr<Database> domain_db;
    std::shared_ptr<Database> greyscaleColor_db;
    std::shared_ptr<Database> analysis_db;
    std::shared_ptr<Database> vis_db;

    IntArray Map;
    signed char *id;    
	int *NeighborList;
	int *dvcMap;
	double *fq, *Aq, *Bq;
	double *Den, *Phi;
    //double *GreySolidPhi; //Model 2 & 3
    //double *GreySolidGrad;//Model 1 & 4
    double *GreySolidW;
    double *GreySn;
    double *GreySw;
    double *GreyKn;
    double *GreyKw;
	//double *ColorGrad;
	double *Velocity;
	double *Pressure;
    double *Porosity_dvc;
    double *Permeability_dvc;
    //double *Psi;
		
private:
	Utilities::MPI comm;
    
	int dist_mem_size;
	int neighborSize;
	// filenames
    char LocalRankString[8];
    char LocalRankFilename[40];
    char LocalRestartFile[40];
   
    //int rank,nprocs;
    void LoadParams(std::shared_ptr<Database> db0);
    void AssignComponentLabels();
    void AssignGreySolidLabels();
    void AssignGreyPoroPermLabels();
    //void AssignGreyscalePotential();
    void ImageInit(std::string filename);
    double MorphInit(const double beta, const double morph_delta);
    double SeedPhaseField(const double seed_water_in_oil);
    double MorphOpenConnected(double target_volume_change);
    void WriteVisFiles();
};

