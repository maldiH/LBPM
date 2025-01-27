/*
 * Dilute Ion Transport LBM Model
 */
#include <algorithm>
#include "models/IonModel.h"
#include "analysis/distance.h"
#include "common/ReadMicroCT.h"

ScaLBL_IonModel::ScaLBL_IonModel(int RANK, int NP, const Utilities::MPI& COMM):
rank(RANK),nprocs(NP),timestep(0),timestepMax(0),time_conv(0),kb(0),electron_charge(0),T(0),Vt(0),k2_inv(0),h(0),
tolerance(0),number_ion_species(0),Nx(0),Ny(0),Nz(0),N(0),Np(0),nprocx(0),nprocy(0),nprocz(0),
fluidVelx_dummy(0),fluidVely_dummy(0),fluidVelz_dummy(0),
BoundaryConditionInlet(0),BoundaryConditionOutlet(0),BoundaryConditionSolid(0),Lx(0),Ly(0),Lz(0),comm(COMM)
{

}
ScaLBL_IonModel::~ScaLBL_IonModel(){

}

void ScaLBL_IonModel::ReadParams(string filename,vector<int> &num_iter){
    
    // read the input database 
	db = std::make_shared<Database>( filename );
	domain_db = db->getDatabase( "Domain" );
	ion_db = db->getDatabase( "Ions" );

    // Universal constant
    kb = 1.38e-23;//Boltzmann constant;unit [J/K]
    electron_charge = 1.6e-19;//electron charge;unit [C]

	//---------------------- Default model parameters --------------------------//		
    T = 300.0;//temperature; unit [K]
    Vt = kb*T/electron_charge;//thermal voltage; unit [Vy]
    k2_inv = 4.0;//speed of sound for D3Q7 lattice
    h = 1.0;//resolution; unit: um/lu
	tolerance = 1.0e-8;
	number_ion_species = 1;
    tau.push_back(1.0);
    IonDiffusivity.push_back(1.0e-9);//user-input diffusivity has physical unit [m^2/sec]
    IonValence.push_back(1);//algebraic valence charge
    IonConcentration.push_back(1.0e-3);//user-input ion concentration has physical unit [mol/m^3]
    //tau.push_back(0.5+k2_inv*time_conv/(h*1.0e-6)/(h*1.0e-6)*IonDiffusivity[0]);
    time_conv.push_back((tau[0]-0.5)/k2_inv*(h*h*1.0e-12)/IonDiffusivity[0]);
    fluidVelx_dummy = 0.0;//for debugging, unit [m/sec]
    fluidVely_dummy = 0.0;//for debugging, unit [m/sec]
    fluidVelz_dummy = 0.0;//for debugging, unit [m/sec]
    Ex_dummy = 0.0;//for debugging, unit [V/m]
    Ey_dummy = 0.0;//for debugging, unit [V/m]
    Ez_dummy = 0.0;//for debugging, unit [V/m]
    //--------------------------------------------------------------------------//

	// Read domain parameters
	if (domain_db->keyExists( "voxel_length" )){//default unit: um/lu
		h = domain_db->getScalar<double>( "voxel_length" );
	}

	// LB-Ion Model parameters		
	//if (ion_db->keyExists( "timestepMax" )){
	//	timestepMax = ion_db->getScalar<int>( "timestepMax" );
	//}
	if (ion_db->keyExists( "tolerance" )){
		tolerance = ion_db->getScalar<double>( "tolerance" );
	}
	if (ion_db->keyExists( "temperature" )){
		T = ion_db->getScalar<int>( "temperature" );
        //re-calculate thermal voltage 
        Vt = kb*T/electron_charge;//thermal voltage; unit [Vy]
	}
	if (ion_db->keyExists( "FluidVelDummy" )){
		fluidVelx_dummy = ion_db->getVector<double>( "FluidVelDummy" )[0];
		fluidVely_dummy = ion_db->getVector<double>( "FluidVelDummy" )[1];
		fluidVelz_dummy = ion_db->getVector<double>( "FluidVelDummy" )[2];
	}
	if (ion_db->keyExists( "ElectricFieldDummy" )){
		Ex_dummy = ion_db->getVector<double>( "ElectricFieldDummy" )[0];
		Ey_dummy = ion_db->getVector<double>( "ElectricFieldDummy" )[1];
		Ez_dummy = ion_db->getVector<double>( "ElectricFieldDummy" )[2];
	}
	if (ion_db->keyExists( "number_ion_species" )){
		number_ion_species = ion_db->getScalar<int>( "number_ion_species" );
	}
    //------ Load number of iteration from multiphysics controller ------//
    if (num_iter.size()!=number_ion_species){
		ERROR("Error: number_ion_species and num_iter_Ion_List (from Multiphysics) must be of the same length! \n");
    }
    else{
        timestepMax.assign(num_iter.begin(),num_iter.end());
    }
    //-------------------------------------------------------------------//
	if (ion_db->keyExists("tauList")){
        tau.clear();
	    tau = ion_db->getVector<double>( "tauList" );
        vector<double>Di = ion_db->getVector<double>( "IonDiffusivityList" );//temp storing ion diffusivity in physical unit
        if (tau.size()!=number_ion_species || Di.size()!=number_ion_species){
		    ERROR("Error: number_ion_species, tauList and IonDiffusivityList must be of the same length! \n");
        }
        else{
            time_conv.clear();
            for (size_t i=0; i<tau.size();i++){
                time_conv.push_back((tau[i]-0.5)/k2_inv*(h*h*1.0e-12)/Di[i]);
            }
        }
    }
    //read ion related list
    //NOTE: ion diffusivity has INPUT unit: [m^2/sec]
    //      it must be converted to LB unit: [lu^2/lt]
	if (ion_db->keyExists("IonDiffusivityList")){
        IonDiffusivity.clear();
	    IonDiffusivity = ion_db->getVector<double>( "IonDiffusivityList" );
        if (IonDiffusivity.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and IonDiffusivityList must be the same length! \n");
        }
        else{
            for (size_t i=0; i<IonDiffusivity.size();i++){
                IonDiffusivity[i] = IonDiffusivity[i]*time_conv[i]/(h*h*1.0e-12);//LB diffusivity has unit [lu^2/lt]
            }
        }
    }
    else {
        for (size_t i=0; i<IonDiffusivity.size();i++){
            //convert ion diffusivity in physical unit to LB unit
            IonDiffusivity[i] = IonDiffusivity[i]*time_conv[i]/(h*h*1.0e-12);//LB diffusivity has unit [lu^2/lt]
        }
    }
    // read time relaxation time list
    //read ion algebric valence list
	if (ion_db->keyExists("IonValenceList")){
        IonValence.clear();
	    IonValence = ion_db->getVector<int>( "IonValenceList" );
        if (IonValence.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and IonValenceList must be the same length! \n");
        }
    }
    //read initial ion concentration list; INPUT unit [mol/m^3]
    //it must be converted to LB unit [mol/lu^3]
    if (ion_db->keyExists("IonConcentrationList")){
        IonConcentration.clear();
	    IonConcentration = ion_db->getVector<double>( "IonConcentrationList" );
        if (IonConcentration.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and IonConcentrationList must be the same length! \n");
        }
        else{
            for (size_t i=0; i<IonConcentration.size();i++){
                IonConcentration[i] = IonConcentration[i]*(h*h*h*1.0e-18);//LB ion concentration has unit [mol/lu^3]
            }
        }
    }
    else {
        for (size_t i=0; i<IonConcentration.size();i++){
            IonConcentration[i] = IonConcentration[i]*(h*h*h*1.0e-18);//LB ion concentration has unit [mol/lu^3]
        }
    
    }

    //Read solid boundary condition specific to Ion model
    BoundaryConditionSolid = 0;
	if (ion_db->keyExists( "BC_Solid" )){
		BoundaryConditionSolid = ion_db->getScalar<int>( "BC_Solid" );
	}
    // Read boundary condition for ion transport
    // BC = 0: normal periodic BC
    // BC = 1: fixed ion concentration; unit=[mol/m^3]
    // BC = 2: fixed ion flux (inward flux); unit=[mol/m^2/sec]
    BoundaryConditionInlet.push_back(0);
    BoundaryConditionOutlet.push_back(0);
    //Inlet
	if (ion_db->keyExists( "BC_InletList" )){
		BoundaryConditionInlet = ion_db->getVector<int>( "BC_InletList" );
        if (BoundaryConditionInlet.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and BC_InletList must be of the same length! \n");
        }
        unsigned short int BC_inlet_min = *min_element(BoundaryConditionInlet.begin(),BoundaryConditionInlet.end());
        unsigned short int BC_inlet_max = *max_element(BoundaryConditionInlet.begin(),BoundaryConditionInlet.end());
        if (BC_inlet_min == 0 && BC_inlet_max>0){
		    ERROR("Error: BC_InletList: mix of periodic, ion concentration and flux BC is not supported! \n");
        }
        if (BC_inlet_min>0){
            //read in inlet values Cin
            if (ion_db->keyExists("InletValueList")){
                Cin = ion_db->getVector<double>( "InletValueList" );
                if (Cin.size()!=number_ion_species){
                    ERROR("Error: number_ion_species and InletValueList must be the same length! \n");
                }
            }
            else {
                ERROR("Error: Non-periodic BCs are specified but InletValueList cannot be found! \n");
            }
            for (size_t i=0;i<BoundaryConditionInlet.size();i++){
                switch (BoundaryConditionInlet[i]){
                    case 1://fixed boundary ion concentration [mol/m^3]
                       Cin[i] = Cin[i]*(h*h*h*1.0e-18);//LB ion concentration has unit [mol/lu^3]
                       break;
                    case 21://fixed boundary ion flux [mol/m^2/sec]
                       Cin[i] = Cin[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                    case 22://fixed boundary ion flux [mol/m^2/sec]
                       Cin[i] = Cin[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                    case 23://fixed boundary ion flux [mol/m^2/sec]
                       Cin[i] = Cin[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                }
            }
        }
	}
    //Outlet
	if (ion_db->keyExists( "BC_OutletList" )){
		BoundaryConditionOutlet = ion_db->getVector<int>( "BC_OutletList" );
        if (BoundaryConditionOutlet.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and BC_OutletList must be of the same length! \n");
        }
        unsigned short int BC_outlet_min = *min_element(BoundaryConditionOutlet.begin(),BoundaryConditionOutlet.end());
        unsigned short int BC_outlet_max = *max_element(BoundaryConditionOutlet.begin(),BoundaryConditionOutlet.end());
        if (BC_outlet_min == 0 && BC_outlet_max>0){
		    ERROR("Error: BC_OutletList: mix of periodic, ion concentration and flux BC is not supported! \n");
        }
        if (BC_outlet_min>0){
            //read in outlet values Cout
            if (ion_db->keyExists("OutletValueList")){
                Cout = ion_db->getVector<double>( "OutletValueList" );
                if (Cout.size()!=number_ion_species){
                    ERROR("Error: number_ion_species and OutletValueList must be the same length! \n");
                }
            }
            else {
                ERROR("Error: Non-periodic BCs are specified but OutletValueList cannot be found! \n");
            }
            for (size_t i=0;i<BoundaryConditionOutlet.size();i++){
                switch (BoundaryConditionOutlet[i]){
                    case 1://fixed boundary ion concentration [mol/m^3]
                       Cout[i] = Cout[i]*(h*h*h*1.0e-18);//LB ion concentration has unit [mol/lu^3]
                       break;
                    case 21://fixed boundary ion flux [mol/m^2/sec]
                       Cout[i] = Cout[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                    case 22://fixed boundary ion flux [mol/m^2/sec]
                       Cout[i] = Cout[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                    case 23://fixed boundary ion flux [mol/m^2/sec]
                       Cout[i] = Cout[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                }
            }
        }
	}
}

void ScaLBL_IonModel::ReadParams(string filename){
    //NOTE: the maximum iteration timesteps for ions are left unspecified
    //      it relies on the multiphys controller to compute the max timestep
    
    // read the input database 
	db = std::make_shared<Database>( filename );
	domain_db = db->getDatabase( "Domain" );
	ion_db = db->getDatabase( "Ions" );

    // Universal constant
    kb = 1.38e-23;//Boltzmann constant;unit [J/K]
    electron_charge = 1.6e-19;//electron charge;unit [C]

	//---------------------- Default model parameters --------------------------//		
    T = 300.0;//temperature; unit [K]
    Vt = kb*T/electron_charge;//thermal voltage; unit [Vy]
    k2_inv = 4.0;//speed of sound for D3Q7 lattice
    h = 1.0;//resolution; unit: um/lu
	tolerance = 1.0e-8;
	number_ion_species = 1;
    tau.push_back(1.0);
    IonDiffusivity.push_back(1.0e-9);//user-input diffusivity has physical unit [m^2/sec]
    IonValence.push_back(1);//algebraic valence charge
    IonConcentration.push_back(1.0e-3);//user-input ion concentration has physical unit [mol/m^3]
    //tau.push_back(0.5+k2_inv*time_conv/(h*1.0e-6)/(h*1.0e-6)*IonDiffusivity[0]);
    time_conv.push_back((tau[0]-0.5)/k2_inv*(h*h*1.0e-12)/IonDiffusivity[0]);
    fluidVelx_dummy = 0.0;//for debugging, unit [m/sec]
    fluidVely_dummy = 0.0;//for debugging, unit [m/sec]
    fluidVelz_dummy = 0.0;//for debugging, unit [m/sec]
    Ex_dummy = 0.0;//for debugging, unit [V/m]
    Ey_dummy = 0.0;//for debugging, unit [V/m]
    Ez_dummy = 0.0;//for debugging, unit [V/m]
    //--------------------------------------------------------------------------//

	// Read domain parameters
	if (domain_db->keyExists( "voxel_length" )){//default unit: um/lu
		h = domain_db->getScalar<double>( "voxel_length" );
	}

	// LB-Ion Model parameters		
	//if (ion_db->keyExists( "timestepMax" )){
	//	timestepMax = ion_db->getScalar<int>( "timestepMax" );
	//}
	if (ion_db->keyExists( "tolerance" )){
		tolerance = ion_db->getScalar<double>( "tolerance" );
	}
	if (ion_db->keyExists( "temperature" )){
		T = ion_db->getScalar<int>( "temperature" );
        //re-calculate thermal voltage 
        Vt = kb*T/electron_charge;//thermal voltage; unit [Vy]
	}
	if (ion_db->keyExists( "FluidVelDummy" )){
		fluidVelx_dummy = ion_db->getVector<double>( "FluidVelDummy" )[0];
		fluidVely_dummy = ion_db->getVector<double>( "FluidVelDummy" )[1];
		fluidVelz_dummy = ion_db->getVector<double>( "FluidVelDummy" )[2];
	}
	if (ion_db->keyExists( "ElectricFieldDummy" )){
		Ex_dummy = ion_db->getVector<double>( "ElectricFieldDummy" )[0];
		Ey_dummy = ion_db->getVector<double>( "ElectricFieldDummy" )[1];
		Ez_dummy = ion_db->getVector<double>( "ElectricFieldDummy" )[2];
	}
	if (ion_db->keyExists( "number_ion_species" )){
		number_ion_species = ion_db->getScalar<int>( "number_ion_species" );
	}
	if (ion_db->keyExists("tauList")){
        tau.clear();
	    tau = ion_db->getVector<double>( "tauList" );
        vector<double>Di = ion_db->getVector<double>( "IonDiffusivityList" );//temp storing ion diffusivity in physical unit
        if (tau.size()!=number_ion_species || Di.size()!=number_ion_species){
		    ERROR("Error: number_ion_species, tauList and IonDiffusivityList must be of the same length! \n");
        }
        else{
            time_conv.clear();
            for (size_t i=0; i<tau.size();i++){
                time_conv.push_back((tau[i]-0.5)/k2_inv*(h*h*1.0e-12)/Di[i]);
            }
        }
    }
    //read ion related list
    //NOTE: ion diffusivity has INPUT unit: [m^2/sec]
    //      it must be converted to LB unit: [lu^2/lt]
	if (ion_db->keyExists("IonDiffusivityList")){
        IonDiffusivity.clear();
	    IonDiffusivity = ion_db->getVector<double>( "IonDiffusivityList" );
        if (IonDiffusivity.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and IonDiffusivityList must be the same length! \n");
        }
        else{
            for (size_t i=0; i<IonDiffusivity.size();i++){
                IonDiffusivity[i] = IonDiffusivity[i]*time_conv[i]/(h*h*1.0e-12);//LB diffusivity has unit [lu^2/lt]
            }
        }
    }
    else {
        for (size_t i=0; i<IonDiffusivity.size();i++){
            //convert ion diffusivity in physical unit to LB unit
            IonDiffusivity[i] = IonDiffusivity[i]*time_conv[i]/(h*h*1.0e-12);//LB diffusivity has unit [lu^2/lt]
        }
    }
    // read time relaxation time list
    //read ion algebric valence list
	if (ion_db->keyExists("IonValenceList")){
        IonValence.clear();
	    IonValence = ion_db->getVector<int>( "IonValenceList" );
        if (IonValence.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and IonValenceList must be the same length! \n");
        }
    }
    //read initial ion concentration list; INPUT unit [mol/m^3]
    //it must be converted to LB unit [mol/lu^3]
	if (ion_db->keyExists("IonConcentrationList")){
        IonConcentration.clear();
	    IonConcentration = ion_db->getVector<double>( "IonConcentrationList" );
        if (IonConcentration.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and IonConcentrationList must be the same length! \n");
        }
        else{
            for (size_t i=0; i<IonConcentration.size();i++){
                IonConcentration[i] = IonConcentration[i]*(h*h*h*1.0e-18);//LB ion concentration has unit [mol/lu^3]
            }
        }
    }
    else {
        for (size_t i=0; i<IonConcentration.size();i++){
            IonConcentration[i] = IonConcentration[i]*(h*h*h*1.0e-18);//LB ion concentration has unit [mol/lu^3]
        }
    
    }

    //Read solid boundary condition specific to Ion model
    BoundaryConditionSolid = 0;
	if (ion_db->keyExists( "BC_Solid" )){
		BoundaryConditionSolid = ion_db->getScalar<int>( "BC_Solid" );
	}
    // Read boundary condition for ion transport
    // BC = 0: normal periodic BC
    // BC = 1: fixed ion concentration; unit=[mol/m^3]
    // BC = 2: fixed ion flux (inward flux); unit=[mol/m^2/sec]
    BoundaryConditionInlet.push_back(0);
    BoundaryConditionOutlet.push_back(0);
    //Inlet
	if (ion_db->keyExists( "BC_InletList" )){
		BoundaryConditionInlet = ion_db->getVector<int>( "BC_InletList" );
        if (BoundaryConditionInlet.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and BC_InletList must be of the same length! \n");
        }
        unsigned short int BC_inlet_min = *min_element(BoundaryConditionInlet.begin(),BoundaryConditionInlet.end());
        unsigned short int BC_inlet_max = *max_element(BoundaryConditionInlet.begin(),BoundaryConditionInlet.end());
        if (BC_inlet_min == 0 && BC_inlet_max>0){
		    ERROR("Error: BC_InletList: mix of periodic, ion concentration and flux BC is not supported! \n");
        }
        if (BC_inlet_min>0){
            //read in inlet values Cin
            if (ion_db->keyExists("InletValueList")){
                Cin = ion_db->getVector<double>( "InletValueList" );
                if (Cin.size()!=number_ion_species){
                    ERROR("Error: number_ion_species and InletValueList must be the same length! \n");
                }
            }
            else {
                ERROR("Error: Non-periodic BCs are specified but InletValueList cannot be found! \n");
            }
            for (size_t i=0;i<BoundaryConditionInlet.size();i++){
                switch (BoundaryConditionInlet[i]){
                    case 1://fixed boundary ion concentration [mol/m^3]
                       Cin[i] = Cin[i]*(h*h*h*1.0e-18);//LB ion concentration has unit [mol/lu^3]
                       break;
                    case 21://fixed boundary ion flux [mol/m^2/sec]
                       Cin[i] = Cin[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                    case 22://fixed boundary ion flux [mol/m^2/sec]
                       Cin[i] = Cin[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                    case 23://fixed boundary ion flux [mol/m^2/sec]
                       Cin[i] = Cin[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                }
            }
        }
	}
    //Outlet
	if (ion_db->keyExists( "BC_OutletList" )){
		BoundaryConditionOutlet = ion_db->getVector<int>( "BC_OutletList" );
        if (BoundaryConditionOutlet.size()!=number_ion_species){
		    ERROR("Error: number_ion_species and BC_OutletList must be of the same length! \n");
        }
        unsigned short int BC_outlet_min = *min_element(BoundaryConditionOutlet.begin(),BoundaryConditionOutlet.end());
        unsigned short int BC_outlet_max = *max_element(BoundaryConditionOutlet.begin(),BoundaryConditionOutlet.end());
        if (BC_outlet_min == 0 && BC_outlet_max>0){
		    ERROR("Error: BC_OutletList: mix of periodic, ion concentration and flux BC is not supported! \n");
        }
        if (BC_outlet_min>0){
            //read in outlet values Cout
            if (ion_db->keyExists("OutletValueList")){
                Cout = ion_db->getVector<double>( "OutletValueList" );
                if (Cout.size()!=number_ion_species){
                    ERROR("Error: number_ion_species and OutletValueList must be the same length! \n");
                }
            }
            else {
                ERROR("Error: Non-periodic BCs are specified but OutletValueList cannot be found! \n");
            }
            for (size_t i=0;i<BoundaryConditionOutlet.size();i++){
                switch (BoundaryConditionOutlet[i]){
                    case 1://fixed boundary ion concentration [mol/m^3]
                       Cout[i] = Cout[i]*(h*h*h*1.0e-18);//LB ion concentration has unit [mol/lu^3]
                       break;
                    case 21://fixed boundary ion flux [mol/m^2/sec]
                       Cout[i] = Cout[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                    case 22://fixed boundary ion flux [mol/m^2/sec]
                       Cout[i] = Cout[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                    case 23://fixed boundary ion flux [mol/m^2/sec]
                       Cout[i] = Cout[i]*(h*h*1.0e-12)*time_conv[i];//LB ion flux has unit [mol/lu^2/lt]
                       break; 
                }
            }
        }
	}
}

void ScaLBL_IonModel::SetDomain(){
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
	
	for (int i=0; i<Nx*Ny*Nz; i++) Dm->id[i] = 1;               // initialize this way
	//Averages = std::shared_ptr<TwoPhase> ( new TwoPhase(Dm) ); // TwoPhase analysis object
	comm.barrier();
    
    unsigned short int BC_inlet_min  = *min_element(BoundaryConditionInlet.begin(),BoundaryConditionInlet.end());
    unsigned short int BC_outlet_min = *min_element(BoundaryConditionOutlet.begin(),BoundaryConditionOutlet.end());
    if (BC_inlet_min==0 && BC_outlet_min==0){
        Dm->BoundaryCondition   = 0;
        Mask->BoundaryCondition = 0;
    }
    else if (BC_inlet_min>0 && BC_outlet_min>0){
        Dm->BoundaryCondition   = 1;
        Mask->BoundaryCondition = 1;
    }
    else { //i.e. periodic and non-periodic BCs are mixed
        ERROR("Error: check the type of inlet and outlet boundary condition! Mixed periodic and non-periodic BCs are found. \n");
    }
    
	Dm->CommInit();
	comm.barrier();
	
	rank = Dm->rank();	
	nprocx = Dm->nprocx();
	nprocy = Dm->nprocy();
	nprocz = Dm->nprocz();
}

void ScaLBL_IonModel::ReadInput(){
    
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
    	id_view.viewRaw( size1, Mask->id.data() );
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
	if (rank==0) printf("LB Ion Solver: Initialized solid phase & converting to Signed Distance function \n");
	CalcDist(Distance,id_solid,*Dm);
    if (rank == 0) cout << "    Domain set." << endl;
}


void ScaLBL_IonModel::AssignSolidBoundary(double *ion_solid)
{
	size_t NLABELS=0;
	signed char VALUE=0;
	double AFFINITY=0.f;

	auto LabelList = ion_db->getVector<int>( "SolidLabels" );
	auto AffinityList = ion_db->getVector<double>( "SolidValues" );

	NLABELS=LabelList.size();
	if (NLABELS != AffinityList.size()){
		ERROR("Error: LB Ion Solver: SolidLabels and SolidValues must be the same length! \n");
	}


	// Assign the labels
	double *label_count;
	double *label_count_global;
	label_count = new double [NLABELS];
	label_count_global = new double [NLABELS];
	for (size_t idx=0; idx<NLABELS; idx++) label_count[idx]=0;

	for (int k=0;k<Nz;k++){
		for (int j=0;j<Ny;j++){
			for (int i=0;i<Nx;i++){
				int n = k*Nx*Ny+j*Nx+i;
				VALUE=Mask->id[n];
                AFFINITY=0.f;
				// Assign the affinity from the paired list
				for (size_t idx=0; idx < NLABELS; idx++){
				      //printf("idx=%i, value=%i, %i, \n",idx, VALUE,LabelList[idx]);
					if (VALUE == LabelList[idx]){
						AFFINITY=AffinityList[idx];
                        //NOTE need to convert the user input phys unit to LB unit
                        AFFINITY = AFFINITY*(h*h*h*1.0e-18); 
						label_count[idx] += 1.0;
						idx = NLABELS;
						//Mask->id[n] = 0; // set mask to zero since this is an immobile component
					}
				}
				ion_solid[n] = AFFINITY;
			}
		}
	}

	for (size_t idx=0; idx<NLABELS; idx++)
		label_count_global[idx]=Dm->Comm.sumReduce(  label_count[idx]);

	if (rank==0){
		printf("LB Ion Solver: number of ion solid labels: %lu \n",NLABELS);
		for (unsigned int idx=0; idx<NLABELS; idx++){
			VALUE=LabelList[idx];
			AFFINITY=AffinityList[idx];
			double volume_fraction  = double(label_count_global[idx])/double((Nx-2)*(Ny-2)*(Nz-2)*nprocs);
			printf("   label=%d, surface ion concentration=%.3g [mol/m^2], volume fraction=%.2g\n",VALUE,AFFINITY,volume_fraction); 
		}
	}
}

void ScaLBL_IonModel::AssignIonConcentration_FromFile(double *Ci,const vector<std::string> &File_ion)
{
    double *Ci_host;
    Ci_host = new double[N];
	double VALUE=0.f;

    Mask->ReadFromFile(File_ion[0],File_ion[1],Ci_host);

	for (int k=0;k<Nz;k++){
		for (int j=0;j<Ny;j++){
			for (int i=0;i<Nx;i++){
				int idx = Map(i,j,k);
				if (!(idx < 0)){
				    int n = k*Nx*Ny+j*Nx+i;
                    //NOTE: default user-input unit: mol/m^3
                    VALUE = Ci_host[n]*(h*h*h*1.0e-18);
                    if (VALUE<0.0){
                        ERROR("Error: Ion concentration value must be a positive number! \n");
                    }
                    else{
                        Ci[idx] = VALUE;
                    }
                }
			}
		}
	}
    delete [] Ci_host;
}

void ScaLBL_IonModel::Create(){
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
	if (rank==0)    printf ("LB Ion Solver: Create ScaLBL_Communicator \n");
	// Create a communicator for the device (will use optimized layout)
	// ScaLBL_Communicator ScaLBL_Comm(Mask); // original
	ScaLBL_Comm  = std::shared_ptr<ScaLBL_Communicator>(new ScaLBL_Communicator(Mask));

	int Npad=(Np/16 + 2)*16;
	if (rank==0)    printf ("LB Ion Solver: Set up memory efficient layout \n");
	Map.resize(Nx,Ny,Nz);       Map.fill(-2);
	auto neighborList= new int[18*Npad];
	Np = ScaLBL_Comm->MemoryOptimizedLayoutAA(Map,neighborList,Mask->id.data(),Np,1);
	comm.barrier();

	//...........................................................................
	//                MAIN  VARIABLES ALLOCATED HERE
	//...........................................................................
	// LBM variables
	if (rank==0)    printf ("LB Ion Solver: Allocating distributions \n");
	//......................device distributions.................................
	int dist_mem_size = Np*sizeof(double);
	int neighborSize=18*(Np*sizeof(int));
	//...........................................................................
	ScaLBL_AllocateDeviceMemory((void **) &NeighborList, neighborSize);
	ScaLBL_AllocateDeviceMemory((void **) &fq, number_ion_species*7*dist_mem_size);  
	ScaLBL_AllocateDeviceMemory((void **) &Ci, number_ion_species*sizeof(double)*Np);
	ScaLBL_AllocateDeviceMemory((void **) &ChargeDensity, sizeof(double)*Np);
	//...........................................................................
	// Update GPU data structures
	if (rank==0)    printf ("LB Ion Solver: Setting up device map and neighbor list \n");
	// copy the neighbor list 
	ScaLBL_CopyToDevice(NeighborList, neighborList, neighborSize);
	comm.barrier();
	
    //Initialize solid boundary for electrical potential
    //if ion concentration at solid surface is specified
    if (BoundaryConditionSolid==1){

	ScaLBL_AllocateDeviceMemory((void **) &IonSolid, sizeof(double)*Nx*Ny*Nz);
        ScaLBL_Comm->SetupBounceBackList(Map, Mask->id.data(), Np);
        comm.barrier();

        double *IonSolid_host;
        IonSolid_host = new double[Nx*Ny*Nz];
        AssignSolidBoundary(IonSolid_host);
        ScaLBL_CopyToDevice(IonSolid, IonSolid_host, Nx*Ny*Nz*sizeof(double));
        ScaLBL_Comm->Barrier();
        delete [] IonSolid_host;
    }
}        

void ScaLBL_IonModel::Initialize(){
	/*
	 * This function initializes model
	 */
    if (rank==0)    printf ("LB Ion Solver: initializing D3Q7 distributions\n");
	if (ion_db->keyExists("IonConcentrationFile")){
        //TODO: Need to figure out how to deal with multi-species concentration initialization
        //NOTE: "IonConcentrationFile" is a vector, including "file_name, datatype"
		auto File_ion = ion_db->getVector<std::string>( "IonConcentrationFile" );
        double *Ci_host;
        Ci_host = new double[number_ion_species*Np];
        for (size_t ic=0; ic<number_ion_species; ic++){
            AssignIonConcentration_FromFile(&Ci_host[ic*Np],File_ion);
        }
	    ScaLBL_CopyToDevice(Ci, Ci_host, number_ion_species*sizeof(double)*Np);
	    comm.barrier();
        for (size_t ic=0; ic<number_ion_species; ic++){
            ScaLBL_D3Q7_Ion_Init_FromFile(&fq[ic*Np*7],&Ci[ic*Np],Np); 
        }
        delete [] Ci_host;
    }
    else{
        for (size_t ic=0; ic<number_ion_species; ic++){
            ScaLBL_D3Q7_Ion_Init(&fq[ic*Np*7],&Ci[ic*Np],IonConcentration[ic],Np); 
        }
    }
    if (rank==0)    printf ("LB Ion Solver: initializing charge density\n");
	for (size_t ic=0; ic<number_ion_species; ic++){
        ScaLBL_D3Q7_Ion_ChargeDensity(Ci, ChargeDensity, IonValence[ic], ic, ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np);
        ScaLBL_D3Q7_Ion_ChargeDensity(Ci, ChargeDensity, IonValence[ic], ic, 0, ScaLBL_Comm->LastExterior(), Np);
    }

    switch (BoundaryConditionSolid){
        case 0:
          if (rank==0) printf("LB Ion Solver: solid boundary: non-flux boundary is assigned\n");  
          break;
        case 1:
          if (rank==0) printf("LB Ion Solver: solid boundary: Dirichlet-type surfacen ion concentration is assigned\n");  
          break;
        default:
          if (rank==0) printf("LB Ion Solver: solid boundary: non-flux boundary is assigned\n");  
          break;
    }

    for (size_t i=0; i<number_ion_species;i++){
        switch (BoundaryConditionInlet[i]){
            case 0:
                if (rank==0) printf("LB Ion Solver: inlet boundary for Ion %zu is periodic \n",i+1);
                break;
            case 1:
                if (rank==0) printf("LB Ion Solver: inlet boundary for Ion %zu is concentration = %.5g [mol/m^3] \n",i+1,Cin[i]/(h*h*h*1.0e-18));
                break;
            case 21:
                if (rank==0) printf("LB Ion Solver: inlet boundary for Ion %zu is (inward) flux = %.5g [mol/m^2/sec]; Diffusive flux only. \n",i+1,Cin[i]/(h*h*1.0e-12)/time_conv[i]);
                break;
            case 22:
                if (rank==0) printf("LB Ion Solver: inlet boundary for Ion %zu is (inward) flux = %.5g [mol/m^2/sec]; Diffusive + advective flux. \n",i+1,Cin[i]/(h*h*1.0e-12)/time_conv[i]);
                break;
            case 23:
                if (rank==0) printf("LB Ion Solver: inlet boundary for Ion %zu is (inward) flux = %.5g [mol/m^2/sec]; Diffusive + advective + electric flux. \n",i+1,Cin[i]/(h*h*1.0e-12)/time_conv[i]);
                break;
        }
        switch (BoundaryConditionOutlet[i]){
            case 0:
                if (rank==0) printf("LB Ion Solver: outlet boundary for Ion %zu is periodic \n",i+1);
                break;
            case 1:
                if (rank==0) printf("LB Ion Solver: outlet boundary for Ion %zu is concentration = %.5g [mol/m^3] \n",i+1,Cout[i]/(h*h*h*1.0e-18));
                break;
            case 21:
                if (rank==0) printf("LB Ion Solver: outlet boundary for Ion %zu is (inward) flux = %.5g [mol/m^2/sec]; Diffusive flux only. \n",i+1,Cout[i]/(h*h*1.0e-12)/time_conv[i]);
                break;
            case 22:
                if (rank==0) printf("LB Ion Solver: outlet boundary for Ion %zu is (inward) flux = %.5g [mol/m^2/sec]; Diffusive + advective flux. \n",i+1,Cout[i]/(h*h*1.0e-12)/time_conv[i]);
                break;
            case 23:
                if (rank==0) printf("LB Ion Solver: outlet boundary for Ion %zu is (inward) flux = %.5g [mol/m^2/sec]; Diffusive + advective + electric flux. \n",i+1,Cout[i]/(h*h*1.0e-12)/time_conv[i]);
                break;
        }
    }

	if (rank==0) printf("*****************************************************\n");
	if (rank==0) printf("LB Ion Transport Solver: \n");
    for (size_t i=0; i<number_ion_species;i++){
	    if (rank==0) printf("      Ion %zu: LB relaxation tau = %.5g\n", i+1,tau[i]);
	    if (rank==0) printf("              Time conversion factor: %.5g [sec/lt]\n", time_conv[i]);
	    if (rank==0) printf("              Internal iteration: %i [lt]\n", timestepMax[i]);
    }
	if (rank==0) printf("*****************************************************\n");
}

void ScaLBL_IonModel::Run(double *Velocity, double *ElectricField){

    //Input parameter:
    //1. Velocity is from StokesModel
    //2. ElectricField is from Poisson model

    //LB-related parameter
    vector<double> rlx;
    for (size_t ic=0;ic<tau.size();ic++){
        rlx.push_back(1.0/tau[ic]); 
    }
    
	//.......create and start timer............
	//double starttime,stoptime,cputime;
	//ScaLBL_Comm->Barrier(); comm.barrier();
    //auto t1 = std::chrono::system_clock::now();

	for (size_t ic=0; ic<number_ion_species; ic++){
        timestep=0;
        while (timestep < timestepMax[ic]) {
            //************************************************************************/
            // *************ODD TIMESTEP*************//
            timestep++;
            //Update ion concentration and charge density
            ScaLBL_Comm->SendD3Q7AA(fq, ic); //READ FROM NORMAL
            ScaLBL_D3Q7_AAodd_IonConcentration(NeighborList, &fq[ic*Np*7],&Ci[ic*Np],ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np);
            ScaLBL_Comm->RecvD3Q7AA(fq, ic); //WRITE INTO OPPOSITE
            ScaLBL_Comm->Barrier();
            //--------------------------------------- Set boundary conditions -------------------------------------//
            if (BoundaryConditionInlet[ic]>0){
                switch (BoundaryConditionInlet[ic]){
                    case 1: 
                        ScaLBL_Comm->D3Q7_Ion_Concentration_BC_z(NeighborList, &fq[ic*Np*7],  Cin[ic], timestep);
                        break;
                    case 21: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_Diff_BC_z(NeighborList, &fq[ic*Np*7],  Cin[ic], tau[ic], &Velocity[2*Np], timestep);
                        break;
                    case 22: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_DiffAdvc_BC_z(NeighborList, &fq[ic*Np*7],  Cin[ic], tau[ic], &Velocity[2*Np], timestep);
                        break;
                    case 23: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_DiffAdvcElec_BC_z(NeighborList,&fq[ic*Np*7],Cin[ic],tau[ic],&Velocity[2*Np],&ElectricField[2*Np],IonDiffusivity[ic],IonValence[ic],Vt,timestep);
                        break;
                }
            }
            if (BoundaryConditionOutlet[ic]>0){
                switch (BoundaryConditionOutlet[ic]){
                    case 1: 
                        ScaLBL_Comm->D3Q7_Ion_Concentration_BC_Z(NeighborList, &fq[ic*Np*7],  Cout[ic], timestep);
                        break;
                    case 21: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_Diff_BC_Z(NeighborList, &fq[ic*Np*7],  Cout[ic], tau[ic], &Velocity[2*Np], timestep);
                        break;
                    case 22: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_DiffAdvc_BC_Z(NeighborList, &fq[ic*Np*7],  Cout[ic], tau[ic], &Velocity[2*Np], timestep);
                        break;
                    case 23: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_DiffAdvcElec_BC_Z(NeighborList,&fq[ic*Np*7],Cout[ic],tau[ic],&Velocity[2*Np],&ElectricField[2*Np],IonDiffusivity[ic],IonValence[ic],Vt,timestep);
                        break;
                }
            }
            //----------------------------------------------------------------------------------------------------//
            ScaLBL_D3Q7_AAodd_IonConcentration(NeighborList, &fq[ic*Np*7],&Ci[ic*Np], 0, ScaLBL_Comm->LastExterior(), Np);
            

            //LB-Ion collison
            ScaLBL_D3Q7_AAodd_Ion(NeighborList, &fq[ic*Np*7],&Ci[ic*Np],Velocity,ElectricField,IonDiffusivity[ic],IonValence[ic],
                                  rlx[ic],Vt,ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np);
            ScaLBL_D3Q7_AAodd_Ion(NeighborList, &fq[ic*Np*7],&Ci[ic*Np],Velocity,ElectricField,IonDiffusivity[ic],IonValence[ic],
                                  rlx[ic],Vt,0, ScaLBL_Comm->LastExterior(), Np);
            
            if (BoundaryConditionSolid==1){
                //TODO IonSolid may also be species-dependent
                ScaLBL_Comm->SolidDirichletD3Q7(&fq[ic*Np*7], IonSolid);
            }
            ScaLBL_Comm->Barrier(); comm.barrier();

            // *************EVEN TIMESTEP*************//
            timestep++;
            //Update ion concentration and charge density
            ScaLBL_Comm->SendD3Q7AA(fq, ic); //READ FORM NORMAL
            ScaLBL_D3Q7_AAeven_IonConcentration(&fq[ic*Np*7],&Ci[ic*Np],ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np);
            ScaLBL_Comm->RecvD3Q7AA(fq, ic); //WRITE INTO OPPOSITE
            ScaLBL_Comm->Barrier();
            //--------------------------------------- Set boundary conditions -------------------------------------//
            if (BoundaryConditionInlet[ic]>0){
                switch (BoundaryConditionInlet[ic]){
                    case 1: 
                        ScaLBL_Comm->D3Q7_Ion_Concentration_BC_z(NeighborList, &fq[ic*Np*7],  Cin[ic], timestep);
                        break;
                    case 21: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_Diff_BC_z(NeighborList, &fq[ic*Np*7],  Cin[ic], tau[ic], &Velocity[2*Np], timestep);
                        break;
                    case 22: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_DiffAdvc_BC_z(NeighborList, &fq[ic*Np*7],  Cin[ic], tau[ic], &Velocity[2*Np], timestep);
                        break;
                    case 23: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_DiffAdvcElec_BC_z(NeighborList,&fq[ic*Np*7],Cin[ic],tau[ic],&Velocity[2*Np],&ElectricField[2*Np],IonDiffusivity[ic],IonValence[ic],Vt,timestep);
                        break;
                }
            }
            if (BoundaryConditionOutlet[ic]>0){
                switch (BoundaryConditionOutlet[ic]){
                    case 1: 
                        ScaLBL_Comm->D3Q7_Ion_Concentration_BC_Z(NeighborList, &fq[ic*Np*7],  Cout[ic], timestep);
                        break;
                    case 21: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_Diff_BC_Z(NeighborList, &fq[ic*Np*7],  Cout[ic], tau[ic], &Velocity[2*Np], timestep);
                        break;
                    case 22: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_DiffAdvc_BC_Z(NeighborList, &fq[ic*Np*7],  Cout[ic], tau[ic], &Velocity[2*Np], timestep);
                        break;
                    case 23: 
                        ScaLBL_Comm->D3Q7_Ion_Flux_DiffAdvcElec_BC_Z(NeighborList,&fq[ic*Np*7],Cout[ic],tau[ic],&Velocity[2*Np],&ElectricField[2*Np],IonDiffusivity[ic],IonValence[ic],Vt,timestep);
                        break;
                }
            }
            //----------------------------------------------------------------------------------------------------//
            ScaLBL_D3Q7_AAeven_IonConcentration(&fq[ic*Np*7],&Ci[ic*Np], 0, ScaLBL_Comm->LastExterior(), Np);
            

            //LB-Ion collison
            ScaLBL_D3Q7_AAeven_Ion(&fq[ic*Np*7],&Ci[ic*Np],Velocity,ElectricField,IonDiffusivity[ic],IonValence[ic],
                                  rlx[ic],Vt,ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np);
            ScaLBL_D3Q7_AAeven_Ion(&fq[ic*Np*7],&Ci[ic*Np],Velocity,ElectricField,IonDiffusivity[ic],IonValence[ic],
                                  rlx[ic],Vt,0, ScaLBL_Comm->LastExterior(), Np);
            
            if (BoundaryConditionSolid==1){
                //TODO IonSolid may also be species-dependent
                ScaLBL_Comm->SolidDirichletD3Q7(&fq[ic*Np*7], IonSolid);
            }
            ScaLBL_Comm->Barrier(); comm.barrier();
        }
    }

    //Compute charge density for Poisson equation
	for (size_t ic=0; ic<number_ion_species; ic++){
        ScaLBL_D3Q7_Ion_ChargeDensity(Ci, ChargeDensity, IonValence[ic], ic, ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np);
        ScaLBL_D3Q7_Ion_ChargeDensity(Ci, ChargeDensity, IonValence[ic], ic, 0, ScaLBL_Comm->LastExterior(), Np);
    }
	//************************************************************************/
	//if (rank==0) printf("-------------------------------------------------------------------\n");
	//// Compute the walltime per timestep
    //auto t2 = std::chrono::system_clock::now();
	//double cputime = std::chrono::duration<double>( t2 - t1 ).count() / timestep;
	//// Performance obtained from each node
	//double MLUPS = double(Np)/cputime/1000000;

	//if (rank==0) printf("********************************************************\n");
	//if (rank==0) printf("CPU time = %f \n", cputime);
	//if (rank==0) printf("Lattice update rate (per core)= %f MLUPS \n", MLUPS);
	//MLUPS *= nprocs;
	//if (rank==0) printf("Lattice update rate (total)= %f MLUPS \n", MLUPS);
	//if (rank==0) printf("********************************************************\n");
}

void ScaLBL_IonModel::getIonConcentration(DoubleArray &IonConcentration, const size_t ic){
	//This function wirte out the data in a normal layout (by aggregating all decomposed domains)

	ScaLBL_Comm->RegularLayout(Map,&Ci[ic*Np],IonConcentration);
	ScaLBL_Comm->Barrier(); comm.barrier();
	IonConcentration_LB_to_Phys(IonConcentration);

}

void ScaLBL_IonModel::getIonConcentration_debug(int timestep){
    //This function write out decomposed data
    DoubleArray PhaseField(Nx,Ny,Nz);
	for (size_t ic=0; ic<number_ion_species; ic++){
	    ScaLBL_Comm->RegularLayout(Map,&Ci[ic*Np],PhaseField);
        ScaLBL_Comm->Barrier(); comm.barrier();
        IonConcentration_LB_to_Phys(PhaseField);

        FILE *OUTFILE;
        sprintf(LocalRankFilename,"Ion%02zu_Time_%i.%05i.raw",ic+1,timestep,rank);
        OUTFILE = fopen(LocalRankFilename,"wb");
        fwrite(PhaseField.data(),8,N,OUTFILE);
        fclose(OUTFILE);
    }
}

void ScaLBL_IonModel::IonConcentration_LB_to_Phys(DoubleArray &Den_reg){
	for (int k=0;k<Nz;k++){
		for (int j=0;j<Ny;j++){
			for (int i=0;i<Nx;i++){
                int idx=Map(i,j,k);
				if (!(idx < 0)){
                    Den_reg(i,j,k) = Den_reg(i,j,k)/(h*h*h*1.0e-18); 
                }
            }
        }
    }
}

void ScaLBL_IonModel::DummyFluidVelocity(){
    double *FluidVelocity_host;
    FluidVelocity_host = new double[3*Np];

	for (int k=0; k<Nz; k++){
		for (int j=0; j<Ny; j++){
			for (int i=0; i<Nx; i++){
				int idx=Map(i,j,k);
				if (!(idx < 0))
                    FluidVelocity_host[idx+0*Np] = fluidVelx_dummy/(h*1.0e-6)*time_conv[0];
                    FluidVelocity_host[idx+1*Np] = fluidVely_dummy/(h*1.0e-6)*time_conv[0];
                    FluidVelocity_host[idx+2*Np] = fluidVelz_dummy/(h*1.0e-6)*time_conv[0];
            }
        }
    }
	ScaLBL_AllocateDeviceMemory((void **) &FluidVelocityDummy, sizeof(double)*3*Np);
	ScaLBL_CopyToDevice(FluidVelocityDummy, FluidVelocity_host, sizeof(double)*3*Np);
	ScaLBL_Comm->Barrier();
	delete [] FluidVelocity_host;
}

void ScaLBL_IonModel::DummyElectricField(){
    double *ElectricField_host;
    ElectricField_host = new double[3*Np];

	for (int k=0; k<Nz; k++){
		for (int j=0; j<Ny; j++){
			for (int i=0; i<Nx; i++){
				int idx=Map(i,j,k);
				if (!(idx < 0))
                    ElectricField_host[idx+0*Np] = Ex_dummy*(h*1.0e-6);
                    ElectricField_host[idx+1*Np] = Ey_dummy*(h*1.0e-6);
                    ElectricField_host[idx+2*Np] = Ez_dummy*(h*1.0e-6);
            }
        }
    }
	ScaLBL_AllocateDeviceMemory((void **) &ElectricFieldDummy, sizeof(double)*3*Np);
	ScaLBL_CopyToDevice(ElectricFieldDummy, ElectricField_host, sizeof(double)*3*Np);
	ScaLBL_Comm->Barrier();
	delete [] ElectricField_host;
}

double ScaLBL_IonModel::CalIonDenConvergence(vector<double> &ci_avg_previous){
    double *Ci_host;
    Ci_host = new double[Np];
    vector<double> error(number_ion_species,0.0);

	for (size_t ic=0; ic<number_ion_species; ic++){

	    ScaLBL_CopyToHost(Ci_host,&Ci[ic*Np],Np*sizeof(double));
		double count_loc=0;
		double count;
        double ci_avg;
        double ci_loc=0.f;

        for (int idx=0; idx<ScaLBL_Comm->LastExterior(); idx++){
            ci_loc +=Ci_host[idx];
            count_loc+=1.0;
        }
        for (int idx=ScaLBL_Comm->FirstInterior(); idx<ScaLBL_Comm->LastInterior(); idx++){
            ci_loc +=Ci_host[idx];
            count_loc+=1.0;
        }
		ci_avg = Mask->Comm.sumReduce( ci_loc);
		count = Mask->Comm.sumReduce(  count_loc);
		ci_avg /= count;
        double ci_avg_mag=ci_avg;
		if (ci_avg==0.0) ci_avg_mag=1.0;
        error[ic] = fabs(ci_avg-ci_avg_previous[ic])/fabs(ci_avg_mag);
		ci_avg_previous[ic] = ci_avg;
    }
    double error_max;
    error_max = *max_element(error.begin(),error.end());
    if (rank==0){
        printf("IonModel: error max: %.5g\n",error_max);
    }
    return error_max;
}

//void ScaLBL_IonModel::getIonConcentration(){
//	for (int ic=0; ic<number_ion_species; ic++){
//        ScaLBL_IonConcentration_Phys(Ci, h, ic, ScaLBL_Comm->FirstInterior(), ScaLBL_Comm->LastInterior(), Np);
//        ScaLBL_IonConcentration_Phys(Ci, h, ic, 0, ScaLBL_Comm->LastExterior(), Np);
//    }
//
//    DoubleArray PhaseField(Nx,Ny,Nz);
//	for (int ic=0; ic<number_ion_species; ic++){
//	    ScaLBL_Comm->RegularLayout(Map,&Ci[ic*Np],PhaseField);
//        ScaLBL_Comm->Barrier(); comm.barrier();
//
//        FILE *OUTFILE;
//        sprintf(LocalRankFilename,"Ion%02i.%05i.raw",ic+1,rank);
//        OUTFILE = fopen(LocalRankFilename,"wb");
//        fwrite(PhaseField.data(),8,N,OUTFILE);
//        fclose(OUTFILE);
//    }
//
//}

