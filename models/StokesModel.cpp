/*
 * Multi-relaxation time LBM Model
 */
#include "models/StokesModel.h"
#include "analysis/distance.h"
#include "common/ReadMicroCT.h"

ScaLBL_StokesModel::ScaLBL_StokesModel(int RANK, int NP, MPI_Comm COMM):
rank(RANK), nprocs(NP), Restart(0),timestep(0),timestepMax(0),tau(0),
Fx(0),Fy(0),Fz(0),flux(0),din(0),dout(0),mu(0),h(0),nu_phys(0),time_conv(0),tolerance(0),
Nx(0),Ny(0),Nz(0),N(0),Np(0),nprocx(0),nprocy(0),nprocz(0),BoundaryCondition(0),Lx(0),Ly(0),Lz(0),comm(COMM)
{

}
ScaLBL_StokesModel::~ScaLBL_StokesModel(){

}

void ScaLBL_StokesModel::ReadParams(string filename,int num_iter){
	// read the input database 
	db = std::make_shared<Database>( filename );
	domain_db = db->getDatabase( "Domain" );
	stokes_db = db->getDatabase( "Stokes" );
	
    //------ Load number of iteration from multiphysics controller ------//
	timestepMax = num_iter;
    //-------------------------------------------------------------------//

	//---------------------- Default model parameters --------------------------//		
    nu_phys = 1.004e-6;//by default use water kinematic viscosity at 20C; unit [m^2/sec]    
    h = 1.0;//image resolution;[um]
	tau = 1.0;
    mu = (tau-0.5)/3.0;//LB kinematic viscosity;unit [lu^2/lt]
    time_conv = h*h*mu/nu_phys;//time conversion factor from physical to LB unit; [sec/lt]
	tolerance = 1.0e-8;
	Fx = Fy = 0.0;
	Fz = 1.0e-5;
    //Body electric field [V/lu]
    Ex = Ey = 0.0;
    Ez = 1.0e-3;
    //--------------------------------------------------------------------------//

	// Single-fluid Navier-Stokes Model parameters
	//if (stokes_db->keyExists( "timestepMax" )){
	//	timestepMax = stokes_db->getScalar<int>( "timestepMax" );
	//}
	if (stokes_db->keyExists( "tolerance" )){
		tolerance = stokes_db->getScalar<double>( "tolerance" );
	}
	if (stokes_db->keyExists( "tau" )){
		tau = stokes_db->getScalar<double>( "tau" );
	}
	if (stokes_db->keyExists( "nu_phys" )){
		nu_phys = stokes_db->getScalar<double>( "nu_phys" );
	}
	if (stokes_db->keyExists( "F" )){
		Fx = stokes_db->getVector<double>( "F" )[0];
		Fy = stokes_db->getVector<double>( "F" )[1];
		Fz = stokes_db->getVector<double>( "F" )[2];
	}
	if (stokes_db->keyExists( "ElectricField" )){//NOTE user-input has physical unit [V/m]
		Ex = stokes_db->getVector<double>( "ElectricField" )[0];
		Ey = stokes_db->getVector<double>( "ElectricField" )[1];
		Ez = stokes_db->getVector<double>( "ElectricField" )[2];
	}
	if (stokes_db->keyExists( "Restart" )){
		Restart = stokes_db->getScalar<bool>( "Restart" );
	}
	if (stokes_db->keyExists( "din" )){
		din = stokes_db->getScalar<double>( "din" );
	}
	if (stokes_db->keyExists( "dout" )){
		dout = stokes_db->getScalar<double>( "dout" );
	}
	if (stokes_db->keyExists( "flux" )){
		flux = stokes_db->getScalar<double>( "flux" );
	}	
	
	// Read domain parameters
	if (domain_db->keyExists( "BC" )){
		BoundaryCondition = domain_db->getScalar<int>( "BC" );
	}
	if (domain_db->keyExists( "voxel_length" )){//default unit: um/lu
		h = domain_db->getScalar<double>( "voxel_length" );
	}

    // Re-calculate model parameters due to parameter read
	mu=(tau-0.5)/3.0;
    time_conv = (h*h*1.0e-12)*mu/nu_phys;//time conversion factor from physical to LB unit; [sec/lt]
    // convert user-input electric field ([V/m]) from physical unit to LB unit 
    Ex = Ex*(h*1.0e-6);//LB electric field: V/lu 
    Ey = Ey*(h*1.0e-6);
    Ez = Ez*(h*1.0e-6);

	if (rank==0) printf("*****************************************************\n");
	if (rank==0) printf("LB Single-Fluid Navier-Stokes Solver: \n");
	if (rank==0) printf("      Time conversion factor: %.5g [sec/lt]\n", time_conv);
	if (rank==0) printf("      Internal iteration: %i [lt]\n", timestepMax);
	if (rank==0) printf("*****************************************************\n");
}
void ScaLBL_StokesModel::SetDomain(){
	Dm  = std::shared_ptr<Domain>(new Domain(domain_db,comm));      // full domain for analysis
	Mask  = std::shared_ptr<Domain>(new Domain(domain_db,comm));    // mask domain removes immobile phases

	// domain parameters
	Nx = Dm->Nx;
	Ny = Dm->Ny;
	Nz = Dm->Nz;
	Lx = Dm->Lx;
	Ly = Dm->Ly;
	Lz = Dm->Lz;
	
	N = Nx*Ny*Nz;
	Distance.resize(Nx,Ny,Nz);
	Velocity_x.resize(Nx,Ny,Nz);
	Velocity_y.resize(Nx,Ny,Nz);
	Velocity_z.resize(Nx,Ny,Nz);
	
	for (int i=0; i<Nx*Ny*Nz; i++) Dm->id[i] = 1;               // initialize this way
	//Averages = std::shared_ptr<TwoPhase> ( new TwoPhase(Dm) ); // TwoPhase analysis object
	MPI_Barrier(comm);
	Dm->CommInit();
	MPI_Barrier(comm);
	
	rank = Dm->rank();	
	nprocx = Dm->nprocx();
	nprocy = Dm->nprocy();
	nprocz = Dm->nprocz();
}

void ScaLBL_StokesModel::ReadInput(){
    
    sprintf(LocalRankString,"%05d",Dm->rank());
    sprintf(LocalRankFilename,"%s%s","ID.",LocalRankString);
    sprintf(LocalRestartFile,"%s%s","Restart.",LocalRankString);

    
    if (domain_db->keyExists( "Filename" )){
    	auto Filename = domain_db->getScalar<std::string>( "Filename" );
    	Mask->Decomp(Filename);
    }
    else if (domain_db->keyExists( "GridFile" )){
    	// Read the local domain data
    	auto input_id = readMicroCT( *domain_db, comm );
    	// Fill the halo (assuming GCW of 1)
    	array<int,3> size0 = { (int) input_id.size(0), (int) input_id.size(1), (int) input_id.size(2) };
    	ArraySize size1 = { (size_t) Mask->Nx, (size_t) Mask->Ny, (size_t) Mask->Nz };
    	ASSERT( (int) size1[0] == size0[0]+2 && (int) size1[1] == size0[1]+2 && (int) size1[2] == size0[2]+2 );
    	fillHalo<signed char> fill( comm, Mask->rank_info, size0, { 1, 1, 1 }, 0, 1 );
    	Array<signed char> id_view;
    	id_view.viewRaw( size1, Mask->id );
    	fill.copy( input_id, id_view );
    	fill.fill( id_view );
    }
    else{
    	Mask->ReadIDs();
    }

    // Generate the signed distance map
	// Initialize the domain and communication
	Array<char> id_solid(Nx,Ny,Nz);
	// Solve for the position of the solid phase
	for (int k=0;k<Nz;k++){
		for (int j=0;j<Ny;j++){
			for (int i=0;i<Nx;i++){
				int n = k*Nx*Ny+j*Nx+i;
				// Initialize the solid phase
				if (Mask->id[n] > 0)	id_solid(i,j,k) = 1;
				else	     	    id_solid(i,j,k) = 0;
			}
		}
	}
	// Initialize the signed distance function
	for (int k=0;k<Nz;k++){
		for (int j=0;j<Ny;j++){
			for (int i=0;i<Nx;i++){
				// Initialize distance to +/- 1
				Distance(i,j,k) = 2.0*double(id_solid(i,j,k))-1.0;
			}
		}
	}
//	MeanFilter(Averages->SDs);
	if (rank==0) printf("LB Single-Fluid Solver: initialized solid phase & converting to Signed Distance function \n");
	CalcDist(Distance,id_solid,*Dm);
    if (rank == 0) cout << "    Domain set." << endl;
}

void ScaLBL_StokesModel::Create(){
	/*
	 *  This function creates the variables needed to run a LBM 
	 */
	int rank=Mask->rank();
	//.........................................................
	// Initialize communication structures in averaging domain
	for (int i=0; i<Nx*Ny*Nz; i++) Dm->id[i] = Mask->id[i];
	Mask->CommInit();
	Np=Mask->PoreCount();
	//...........................................................................
	if (rank==0)    printf ("LB Single-Fluid Solver: Create ScaLBL_Communicator \n");
	// Create a communicator for the device (will use optimized layout)
	// ScaLBL_Communicator ScaLBL_Comm(Mask); // original
	ScaLBL_Comm  = std::shared_ptr<ScaLBL_Communicator>(new ScaLBL_Communicator(Mask));

	int Npad=(Np/16 + 2)*16;
	if (rank==0)    printf ("LB Single-Fluid Solver: Set up memory efficient layout \n");
	Map.resize(Nx,Ny,Nz);       Map.fill(-2);
	auto neighborList= new int[18*Npad];
	Np = ScaLBL_Comm->MemoryOptimizedLayoutAA(Map,neighborList,Mask->id,Np);
	MPI_Barrier(comm);
	//...........................................................................
	//                MAIN  VARIABLES ALLOCATED HERE
	//...........................................................................
	// LBM variables
	if (rank==0)    printf ("LB Single-Fluid Solver: Allocating distributions \n");
	//......................device distributions.................................
	int dist_mem_size = Np*sizeof(double);
	int neighborSize=18*(Np*sizeof(int));
	//...........................................................................
	ScaLBL_AllocateDeviceMemory((void **) &NeighborList, neighborSize);
	ScaLBL_AllocateDeviceMemory((void **) &fq, 19*dist_mem_size);  
	ScaLBL_AllocateDeviceMemory((void **) &Pressure, sizeof(double)*Np);
	ScaLBL_AllocateDeviceMemory((void **) &Velocity, 3*sizeof(double)*Np);
	//...........................................................................
	// Update GPU data structures
	if (rank==0)    printf ("LB Single-Fluid Solver: Setting up device map and neighbor list \n");
	// copy the neighbor list 
	ScaLBL_CopyToDevice(NeighborList, neighborList, neighborSize);
	MPI_Barrier(comm);
	
}        

void ScaLBL_StokesModel::Initialize(){
	/*
	 * This function initializes model
	 */
    if (rank==0) printf("LB Single-Fluid Solver: Initializing distributions \n");
	if (rank==0) printf("****************************************************************\n");
    ScaLBL_D3Q19_Init(fq, Np);
}

void ScaLBL_StokesModel::Run_Lite(double *ChargeDensity, double *ElectricField){
	double rlx_setA=1.0/tau;
	double rlx_setB = 8.f*(2.f-rlx_setA)/(8.f-rlx_setA);
    timestep = 0;
	while (timestep < timestepMax) {
        //************************************************************************/
        ScaLBL_Comm->SendD3Q19AA(fq); //READ FROM NORMAL
        ScaLBL_D3Q19_AAodd_StokesMRT(NeighborList, fq, Velocity, ChargeDensity, ElectricField, rlx_setA, rlx_setB, Fx, Fy, Fz, Ex, Ey, Ez, 
                                     ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np);
        ScaLBL_Comm->RecvD3Q19AA(fq); //WRITE INTO OPPOSITE
        // Set boundary conditions
        if (BoundaryCondition == 3){
            ScaLBL_Comm->D3Q19_Pressure_BC_z(NeighborList, fq, din, timestep);
            ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
        }
        else if (BoundaryCondition == 4){
            din = ScaLBL_Comm->D3Q19_Flux_BC_z(NeighborList, fq, flux, timestep);
            ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
        }
        else if (BoundaryCondition == 5){
            ScaLBL_Comm->D3Q19_Reflection_BC_z(fq);
            ScaLBL_Comm->D3Q19_Reflection_BC_Z(fq);
        }
        ScaLBL_D3Q19_AAodd_StokesMRT(NeighborList, fq, Velocity, ChargeDensity, ElectricField, rlx_setA, rlx_setB, Fx, Fy, Fz, Ex, Ey, Ez, 0, ScaLBL_Comm->LastExterior(), Np);
        ScaLBL_DeviceBarrier(); MPI_Barrier(comm);

        timestep++;
        ScaLBL_Comm->SendD3Q19AA(fq); //READ FORM NORMAL
        ScaLBL_D3Q19_AAeven_StokesMRT(fq, Velocity, ChargeDensity, ElectricField, rlx_setA, rlx_setB, Fx, Fy, Fz, Ex, Ey, Ez, ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np);
        ScaLBL_Comm->RecvD3Q19AA(fq); //WRITE INTO OPPOSITE
        // Set boundary conditions
        if (BoundaryCondition == 3){
            ScaLBL_Comm->D3Q19_Pressure_BC_z(NeighborList, fq, din, timestep);
            ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
        }
        else if (BoundaryCondition == 4){
            din = ScaLBL_Comm->D3Q19_Flux_BC_z(NeighborList, fq, flux, timestep);
            ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
        }
        else if (BoundaryCondition == 5){
            ScaLBL_Comm->D3Q19_Reflection_BC_z(fq);
            ScaLBL_Comm->D3Q19_Reflection_BC_Z(fq);
        }
        ScaLBL_D3Q19_AAeven_StokesMRT(fq, Velocity, ChargeDensity, ElectricField, rlx_setA, rlx_setB, Fx, Fy, Fz, Ex, Ey, Ez, 0, ScaLBL_Comm->LastExterior(), Np);
        ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
        //************************************************************************/
    }
}

void ScaLBL_StokesModel::getVelocity(){
    //get velocity in physical unit [m/sec]
	ScaLBL_D3Q19_Momentum_Phys(fq, Velocity, h, time_conv, Np);
	ScaLBL_DeviceBarrier(); MPI_Barrier(comm);

    DoubleArray PhaseField(Nx,Ny,Nz);
	ScaLBL_Comm->RegularLayout(Map,&Velocity[0],PhaseField);
	FILE *VELX_FILE;
	sprintf(LocalRankFilename,"Velocity_X.%05i.raw",rank);
	VELX_FILE = fopen(LocalRankFilename,"wb");
	fwrite(PhaseField.data(),8,N,VELX_FILE);
	fclose(VELX_FILE);

	ScaLBL_Comm->RegularLayout(Map,&Velocity[Np],PhaseField);
	FILE *VELY_FILE;
	sprintf(LocalRankFilename,"Velocity_Y.%05i.raw",rank);
	VELY_FILE = fopen(LocalRankFilename,"wb");
	fwrite(PhaseField.data(),8,N,VELY_FILE);
	fclose(VELY_FILE);

	ScaLBL_Comm->RegularLayout(Map,&Velocity[2*Np],PhaseField);
	FILE *VELZ_FILE;
	sprintf(LocalRankFilename,"Velocity_Z.%05i.raw",rank);
	VELZ_FILE = fopen(LocalRankFilename,"wb");
	fwrite(PhaseField.data(),8,N,VELZ_FILE);
	fclose(VELZ_FILE);

}

void ScaLBL_StokesModel::Run(){
	double rlx_setA=1.0/tau;
	double rlx_setB = 8.f*(2.f-rlx_setA)/(8.f-rlx_setA);
	
	Minkowski Morphology(Mask);

	if (rank==0){
		bool WriteHeader=false;
		FILE *log_file = fopen("Permeability.csv","r");
		if (log_file != NULL)
			fclose(log_file);
		else
			WriteHeader=true;

		if (WriteHeader){
			log_file = fopen("Permeability.csv","a+");
			fprintf(log_file,"time Fx Fy Fz mu Vs As Js Xs vx vy vz k\n");
			fclose(log_file);
		}
	}

	//.......create and start timer............
	double starttime,stoptime,cputime;
	ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
	starttime = MPI_Wtime();
	if (rank==0) printf("****************************************************************\n");
	if (rank==0) printf("LB Single-Fluid Navier-Stokes Solver: timestepMax = %i\n", timestepMax);
	if (rank==0) printf("****************************************************************\n");
	timestep=0;
	double error = 1.0;
	double flow_rate_previous = 0.0;
	while (timestep < timestepMax && error > tolerance) {
		//************************************************************************/
		timestep++;
		ScaLBL_Comm->SendD3Q19AA(fq); //READ FROM NORMAL
		ScaLBL_D3Q19_AAodd_MRT(NeighborList, fq,  ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np, rlx_setA, rlx_setB, Fx, Fy, Fz);
		ScaLBL_Comm->RecvD3Q19AA(fq); //WRITE INTO OPPOSITE
		// Set boundary conditions
		if (BoundaryCondition == 3){
			ScaLBL_Comm->D3Q19_Pressure_BC_z(NeighborList, fq, din, timestep);
			ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
		}
		else if (BoundaryCondition == 4){
			din = ScaLBL_Comm->D3Q19_Flux_BC_z(NeighborList, fq, flux, timestep);
			ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
		}
		else if (BoundaryCondition == 5){
			ScaLBL_Comm->D3Q19_Reflection_BC_z(fq);
			ScaLBL_Comm->D3Q19_Reflection_BC_Z(fq);
		}
		ScaLBL_D3Q19_AAodd_MRT(NeighborList, fq, 0, ScaLBL_Comm->LastExterior(), Np, rlx_setA, rlx_setB, Fx, Fy, Fz);
		ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
		timestep++;
		ScaLBL_Comm->SendD3Q19AA(fq); //READ FORM NORMAL
		ScaLBL_D3Q19_AAeven_MRT(fq, ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np, rlx_setA, rlx_setB, Fx, Fy, Fz);
		ScaLBL_Comm->RecvD3Q19AA(fq); //WRITE INTO OPPOSITE
		// Set boundary conditions
		if (BoundaryCondition == 3){
			ScaLBL_Comm->D3Q19_Pressure_BC_z(NeighborList, fq, din, timestep);
			ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
		}
		else if (BoundaryCondition == 4){
			din = ScaLBL_Comm->D3Q19_Flux_BC_z(NeighborList, fq, flux, timestep);
			ScaLBL_Comm->D3Q19_Pressure_BC_Z(NeighborList, fq, dout, timestep);
		}
		else if (BoundaryCondition == 5){
			ScaLBL_Comm->D3Q19_Reflection_BC_z(fq);
			ScaLBL_Comm->D3Q19_Reflection_BC_Z(fq);
		}
		ScaLBL_D3Q19_AAeven_MRT(fq, 0, ScaLBL_Comm->LastExterior(), Np, rlx_setA, rlx_setB, Fx, Fy, Fz);
		ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
		//************************************************************************/
		
		if (timestep%1000==0){
			ScaLBL_D3Q19_Momentum(fq,Velocity, Np);
			ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
			ScaLBL_Comm->RegularLayout(Map,&Velocity[0],Velocity_x);
			ScaLBL_Comm->RegularLayout(Map,&Velocity[Np],Velocity_y);
			ScaLBL_Comm->RegularLayout(Map,&Velocity[2*Np],Velocity_z);
			
			double count_loc=0;
			double count;
			double vax,vay,vaz;
			double vax_loc,vay_loc,vaz_loc;
			vax_loc = vay_loc = vaz_loc = 0.f;
			for (int k=1; k<Nz-1; k++){
				for (int j=1; j<Ny-1; j++){
					for (int i=1; i<Nx-1; i++){
						if (Distance(i,j,k) > 0){
							vax_loc += Velocity_x(i,j,k);
							vay_loc += Velocity_y(i,j,k);
							vaz_loc += Velocity_z(i,j,k);
							count_loc+=1.0;
						}
					}
				}
			}
			MPI_Allreduce(&vax_loc,&vax,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			MPI_Allreduce(&vay_loc,&vay,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			MPI_Allreduce(&vaz_loc,&vaz,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			MPI_Allreduce(&count_loc,&count,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
			
			vax /= count;
			vay /= count;
			vaz /= count;
			
			double force_mag = sqrt(Fx*Fx+Fy*Fy+Fz*Fz);
			double dir_x = Fx/force_mag;
			double dir_y = Fy/force_mag;
			double dir_z = Fz/force_mag;
			if (force_mag == 0.0){
				// default to z direction
				dir_x = 0.0;
				dir_y = 0.0;
				dir_z = 1.0;
				force_mag = 1.0;
			}
			double flow_rate = (vax*dir_x + vay*dir_y + vaz*dir_z);
			
			error = fabs(flow_rate - flow_rate_previous) / fabs(flow_rate);
			flow_rate_previous = flow_rate;
			
			//if (rank==0) printf("Computing Minkowski functionals \n");
			Morphology.ComputeScalar(Distance,0.f);
			//Morphology.PrintAll();
			double mu = (tau-0.5)/3.f;
			double Vs = Morphology.V();
			double As = Morphology.A();
			double Hs = Morphology.H();
			double Xs = Morphology.X();
			Vs=sumReduce( Dm->Comm, Vs);
			As=sumReduce( Dm->Comm, As);
			Hs=sumReduce( Dm->Comm, Hs);
			Xs=sumReduce( Dm->Comm, Xs);
			double h = Dm->voxel_length;
			double absperm = h*h*mu*Mask->Porosity()*flow_rate / force_mag;
			if (rank==0) {
				printf("     %f\n",absperm);
				FILE * log_file = fopen("Permeability.csv","a");
				fprintf(log_file,"%i %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g\n",timestep, Fx, Fy, Fz, mu, 
						h*h*h*Vs,h*h*As,h*Hs,Xs,vax,vay,vaz, absperm);
				fclose(log_file);
			}
		}
	}
	//************************************************************************/
	stoptime = MPI_Wtime();
	if (rank==0) printf("-------------------------------------------------------------------\n");
	// Compute the walltime per timestep
	cputime = (stoptime - starttime)/timestep;
	// Performance obtained from each node
	double MLUPS = double(Np)/cputime/1000000;

	if (rank==0) printf("********************************************************\n");
	if (rank==0) printf("CPU time = %f \n", cputime);
	if (rank==0) printf("Lattice update rate (per core)= %f MLUPS \n", MLUPS);
	MLUPS *= nprocs;
	if (rank==0) printf("Lattice update rate (total)= %f MLUPS \n", MLUPS);
	if (rank==0) printf("********************************************************\n");

}

void ScaLBL_StokesModel::VelocityField(){

/*	Minkowski Morphology(Mask);
	int SIZE=Np*sizeof(double);
	ScaLBL_D3Q19_Momentum(fq,Velocity, Np);
	ScaLBL_DeviceBarrier(); MPI_Barrier(comm);
	ScaLBL_CopyToHost(&VELOCITY[0],&Velocity[0],3*SIZE);

	memcpy(Morphology.SDn.data(), Distance.data(), Nx*Ny*Nz*sizeof(double));
	Morphology.Initialize();
	Morphology.UpdateMeshValues();
	Morphology.ComputeLocal();
	Morphology.Reduce();
	
	double count_loc=0;
	double count;
	double vax,vay,vaz;
	double vax_loc,vay_loc,vaz_loc;
	vax_loc = vay_loc = vaz_loc = 0.f;
	for (int n=0; n<ScaLBL_Comm->LastExterior(); n++){
		vax_loc += VELOCITY[n];
		vay_loc += VELOCITY[Np+n];
		vaz_loc += VELOCITY[2*Np+n];
		count_loc+=1.0;
	}
	
	for (int n=ScaLBL_Comm->FirstInterior(); n<ScaLBL_Comm->LastInterior(); n++){
		vax_loc += VELOCITY[n];
		vay_loc += VELOCITY[Np+n];
		vaz_loc += VELOCITY[2*Np+n];
		count_loc+=1.0;
	}
	MPI_Allreduce(&vax_loc,&vax,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
	MPI_Allreduce(&vay_loc,&vay,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
	MPI_Allreduce(&vaz_loc,&vaz,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
	MPI_Allreduce(&count_loc,&count,1,MPI_DOUBLE,MPI_SUM,Mask->Comm);
	
	vax /= count;
	vay /= count;
	vaz /= count;
	
	double mu = (tau-0.5)/3.f;
	if (rank==0) printf("Fx Fy Fz mu Vs As Js Xs vx vy vz\n");
	if (rank==0) printf("%.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g %.8g\n",Fx, Fy, Fz, mu, 
						Morphology.V(),Morphology.A(),Morphology.J(),Morphology.X(),vax,vay,vaz);
						*/
	
	std::vector<IO::MeshDataStruct> visData;
	fillHalo<double> fillData(Dm->Comm,Dm->rank_info,{Dm->Nx-2,Dm->Ny-2,Dm->Nz-2},{1,1,1},0,1);

	auto VxVar = std::make_shared<IO::Variable>();
	auto VyVar = std::make_shared<IO::Variable>();
	auto VzVar = std::make_shared<IO::Variable>();
	auto SignDistVar = std::make_shared<IO::Variable>();

	IO::initialize("","silo","false");
	// Create the MeshDataStruct	
	visData.resize(1);
	visData[0].meshName = "domain";
	visData[0].mesh = std::make_shared<IO::DomainMesh>( Dm->rank_info,Dm->Nx-2,Dm->Ny-2,Dm->Nz-2,Dm->Lx,Dm->Ly,Dm->Lz );
	SignDistVar->name = "SignDist";
	SignDistVar->type = IO::VariableType::VolumeVariable;
	SignDistVar->dim = 1;
	SignDistVar->data.resize(Dm->Nx-2,Dm->Ny-2,Dm->Nz-2);
	visData[0].vars.push_back(SignDistVar);
	
	VxVar->name = "Velocity_x";
	VxVar->type = IO::VariableType::VolumeVariable;
	VxVar->dim = 1;
	VxVar->data.resize(Dm->Nx-2,Dm->Ny-2,Dm->Nz-2);
	visData[0].vars.push_back(VxVar);
	VyVar->name = "Velocity_y";
	VyVar->type = IO::VariableType::VolumeVariable;
	VyVar->dim = 1;
	VyVar->data.resize(Dm->Nx-2,Dm->Ny-2,Dm->Nz-2);
	visData[0].vars.push_back(VyVar);
	VzVar->name = "Velocity_z";
	VzVar->type = IO::VariableType::VolumeVariable;
	VzVar->dim = 1;
	VzVar->data.resize(Dm->Nx-2,Dm->Ny-2,Dm->Nz-2);
	visData[0].vars.push_back(VzVar);
	
	Array<double>& SignData  = visData[0].vars[0]->data;
	Array<double>& VelxData = visData[0].vars[1]->data;
	Array<double>& VelyData = visData[0].vars[2]->data;
	Array<double>& VelzData = visData[0].vars[3]->data;
	
    ASSERT(visData[0].vars[0]->name=="SignDist");
    ASSERT(visData[0].vars[1]->name=="Velocity_x");
    ASSERT(visData[0].vars[2]->name=="Velocity_y");
    ASSERT(visData[0].vars[3]->name=="Velocity_z");
	
    fillData.copy(Distance,SignData);
    fillData.copy(Velocity_x,VelxData);
    fillData.copy(Velocity_y,VelyData);
    fillData.copy(Velocity_z,VelzData);
	
    IO::writeData( timestep, visData, Dm->Comm );

}