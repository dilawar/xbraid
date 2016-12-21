// Copyright (c) 2013, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory. Written by
// Jacob Schroder, Rob Falgout, Tzanio Kolev, Ulrike Yang, Veselin
// Dobrev, et al. LLNL-CODE-660355. All rights reserved.
//
// This file is part of XBraid. Email xbraid-support@llnl.gov for support.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License (as published by the Free Software
// Foundation) version 2.1 dated February 1999.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the terms and conditions of the GNU General Public
// License for more details.
//
// You should have received a copy of the GNU Lesser General Public License along
// with this program; if not, write to the Free Software Foundation, Inc., 59
// Temple Place, Suite 330, Boston, MA 02111-1307 USA
//
//TODO Added a C to test the spatail coarsening stuff
//
/* Drive pLaplcian: 2D nonlinear p-laplacian equation. Requires MFEM, Hpyre, Metis and GlVis

   Interface: MFEM
f
   Compile with: make drive-06 -- Modify Makefile to point to metis, mfem and hypre libraries

   Help with:    drive-06 -help

   Sample run:   mpirun -n12 ./drive-06

   Description:  This code solves the 2D p -laplacian problem using BDF1 
  
                                     u_t - div( (|grad(u)|^2+C) grad(u) ) = b,
                                                 u(x,0) = U0(x)
                                 (|grad(u)|^p + C) grad(u) . n = g(x,t) on boundary

                 on a given mesh using MFEM. The coressponding weak form is

                     <u_t,v> + <(C+ |grad(u)|^p) grad(u), grad(v) = <b,v> + <g,v>_b = <f,v>

                 
                The option "-pow" sets the power of the p-Laplacian. The default is "2". Setting 
                pow = 0 solves the linear heat equation, be it with a Newton solver. Setting pow 
                = -1, removes the nonlinearity, replacing it with the exact solution. 
                     
                Implicit methods use newtons method. Consider BDF1 for p=2. Let u_k = u(t_k) and define

                               L(u)(v) = <u,v> + dt*<( C + |grad(u)|^2 ) grad(u),grad(v)>
                                       f(v) = <u_k + dt*b, v> + dt*<g,v>_b
                L'(u)(v)[w] = <w,v> + dt*<(|grad(u)|^2 + 2 grad(u)grad(u)^t ) grad(w), grad(v)>

                where L' is the Frechet derivitive of L. Each implicit time step solves

                                            L(u_{k+1})(v) - f(v) = 0

                Letting superscript denote iteration number, Newtons method is

                            u^{j+1} = u^j -  L'(u^j)(v)[L(u^j)(v) - f(v)] = u^j - m^j

                where the previous time step value is used for u^0 and m^j solves
                the linear equaiton

                              L'(u^j)(v)[m^j] = L(u_j)(v) - f(v)

                Hypre BOOMERamg is used as the linear solver */

#include "braid_mfem.hpp"
#include <fstream>
#include <iostream>

//Extra Hypre Functions

namespace hypre
{
/**------------------------------------------------------------------------
 * Perform the operation A += beta*B, assuming that the sparsity pattern of
 * A contains that of B.
 * -----------------------------------------------------------------------*/
HYPRE_Int
hypre_CSRMatrixSum( hypre_CSRMatrix *A,
                    HYPRE_Complex    beta,
                    hypre_CSRMatrix *B )
{
   HYPRE_Complex    *A_data   = hypre_CSRMatrixData(A);
   HYPRE_Int        *A_i      = hypre_CSRMatrixI(A);
   HYPRE_Int        *A_j      = hypre_CSRMatrixJ(A);
   HYPRE_Int         nrows_A  = hypre_CSRMatrixNumRows(A);
   HYPRE_Int         ncols_A  = hypre_CSRMatrixNumCols(A);
   HYPRE_Complex    *B_data   = hypre_CSRMatrixData(B);
   HYPRE_Int        *B_i      = hypre_CSRMatrixI(B);
   HYPRE_Int        *B_j      = hypre_CSRMatrixJ(B);
   HYPRE_Int         nrows_B  = hypre_CSRMatrixNumRows(B);
   HYPRE_Int         ncols_B  = hypre_CSRMatrixNumCols(B);

   HYPRE_Int         ia, j, pos;
   HYPRE_Int        *marker;

   if (nrows_A != nrows_B || ncols_A != ncols_B)
   {
      hypre_printf("Warning! incompatible matrix dimensions!\n");
      return -1;
   }

   marker = hypre_CTAlloc(HYPRE_Int, ncols_A);
   for (ia = 0; ia < ncols_A; ia++)
   {
      marker[ia] = -1;
   }

   for (ia = 0; ia < nrows_A; ia++)
   {
      for (j = A_i[ia]; j < A_i[ia+1]; j++)
      {
         marker[A_j[j]] = j;
      }

      for (j = B_i[ia]; j < B_i[ia+1]; j++)
      {
         pos = marker[B_j[j]];
         if (pos < A_i[ia])
         {
            hypre_printf("Warning! hypre_CSRMatrixSum: Incorrect input!\n");
            return -2;
         }
         A_data[pos] += beta * B_data[j];
      }
   }

   hypre_TFree(marker);
   return 0;
}

/**------------------------------------------------------------------------
 * Return a new matrix containing the sum of A and B, assuming that both
 * matrices use the same row and column partitions and the same col_map_offd
 * arrays.
 * -----------------------------------------------------------------------*/
hypre_ParCSRMatrix *
hypre_ParCSRMatrixAdd( hypre_ParCSRMatrix *A,
                       hypre_ParCSRMatrix *B )
{
   MPI_Comm            comm   = hypre_ParCSRMatrixComm(A);
   hypre_CSRMatrix    *A_diag = hypre_ParCSRMatrixDiag(A);
   hypre_CSRMatrix    *A_offd = hypre_ParCSRMatrixOffd(A);
   HYPRE_Int          *A_cmap = hypre_ParCSRMatrixColMapOffd(A);
   HYPRE_Int           A_cmap_size = hypre_CSRMatrixNumCols(A_offd);
   hypre_CSRMatrix    *B_diag = hypre_ParCSRMatrixDiag(B);
   hypre_CSRMatrix    *B_offd = hypre_ParCSRMatrixOffd(B);
   HYPRE_Int          *B_cmap = hypre_ParCSRMatrixColMapOffd(B);
   HYPRE_Int           B_cmap_size = hypre_CSRMatrixNumCols(B_offd);
   hypre_ParCSRMatrix *C;
   hypre_CSRMatrix    *C_diag;
   hypre_CSRMatrix    *C_offd;
   HYPRE_Int          *C_cmap;
   HYPRE_Int           im;

   /* Make sure A_cmap and B_cmap are the same. */
   if (A_cmap_size != B_cmap_size)
   {
      hypre_printf("Warning! hypre_ParCSRMatrixAdd: cmap_size!\n");
      return NULL;
   }
   for (im = 0; im < A_cmap_size; im++)
   {
      if (A_cmap[im] != B_cmap[im])
      {
         hypre_printf("Warning! hypre_ParCSRMatrixAdd: cmap!\n");
         return NULL;
      }
   }

   /* Add diagonals, off-diagonals, copy cmap. */
   C_diag = hypre_CSRMatrixAdd(A_diag, B_diag);
   C_offd = hypre_CSRMatrixAdd(A_offd, B_offd);
   C_cmap = hypre_TAlloc(HYPRE_Int, A_cmap_size);
   for (im = 0; im < A_cmap_size; im++)
   {
      C_cmap[im] = A_cmap[im];
   }

   C = hypre_ParCSRMatrixCreate( comm,
                                 hypre_ParCSRMatrixGlobalNumRows(A),
                                 hypre_ParCSRMatrixGlobalNumCols(A),
                                 hypre_ParCSRMatrixRowStarts(A),
                                 hypre_ParCSRMatrixColStarts(A),
                                 hypre_CSRMatrixNumCols(C_offd),
                                 hypre_CSRMatrixNumNonzeros(C_diag),
                                 hypre_CSRMatrixNumNonzeros(C_offd) );

   /* In C, destroy diag/offd (allocated by Create) and replace them with
      C_diag/C_offd. */
   hypre_CSRMatrixDestroy(hypre_ParCSRMatrixDiag(C));
   hypre_CSRMatrixDestroy(hypre_ParCSRMatrixOffd(C));
   hypre_ParCSRMatrixDiag(C) = C_diag;
   hypre_ParCSRMatrixOffd(C) = C_offd;

   hypre_ParCSRMatrixColMapOffd(C) = C_cmap;

   /* hypre_ParCSRMatrixSetNumNonzeros(A); */

   /* Make sure that the first entry in each row is the diagonal one. */
   hypre_CSRMatrixReorder(hypre_ParCSRMatrixDiag(C));

   /* C owns diag, offd, and cmap. */
   hypre_ParCSRMatrixSetDataOwner(C, 1);
   /* C does not own row and column starts. */
   hypre_ParCSRMatrixSetRowStartsOwner(C, 0);
   hypre_ParCSRMatrixSetColStartsOwner(C, 0);

   return C;
}

/**------------------------------------------------------------------------
 * Perform the operation A += beta*B, assuming that both matrices use the
 * same row and column partitions and the same col_map_offd arrays. Also,
 * it is assumed that the sparsity pattern of A contains that of B.
 * -----------------------------------------------------------------------*/
HYPRE_Int
hypre_ParCSRMatrixSum( hypre_ParCSRMatrix *A,
                       HYPRE_Complex       beta,
                       hypre_ParCSRMatrix *B )
{
   hypre_CSRMatrix *A_diag = hypre_ParCSRMatrixDiag(A);
   hypre_CSRMatrix *A_offd = hypre_ParCSRMatrixOffd(A);
   hypre_CSRMatrix *B_diag = hypre_ParCSRMatrixDiag(B);
   hypre_CSRMatrix *B_offd = hypre_ParCSRMatrixOffd(B);
   HYPRE_Int error;

   error = hypre_CSRMatrixSum(A_diag, beta, B_diag);
   if (!error)
   {
      error = hypre_CSRMatrixSum(A_offd, beta, B_offd);
   }

   return error;
}

HYPRE_Int
hypre_CSRMatrixSetConstantValues( hypre_CSRMatrix *A,
                                  HYPRE_Complex    value )
{
   HYPRE_Complex *A_data = hypre_CSRMatrixData(A);
   HYPRE_Int      A_nnz  = hypre_CSRMatrixNumNonzeros(A);
   HYPRE_Int      ia;

   for (ia = 0; ia < A_nnz; ia++)
   {
      A_data[ia] = value;
   }

   return 0;
}

HYPRE_Int
hypre_ParCSRMatrixSetConstantValues( hypre_ParCSRMatrix *A,
                                     HYPRE_Complex       value )
{
   hypre_CSRMatrixSetConstantValues(hypre_ParCSRMatrixDiag(A), value);
   hypre_CSRMatrixSetConstantValues(hypre_ParCSRMatrixOffd(A), value);

   return 0;
}
}

using namespace hypre;

using namespace std;

/**************************************************************************************
Return the coefficient in the diffusion term of the weak form

               <|nl_coeff(u,t)*grad(u),grad(v)>
       nl_coeff(u,t) = -a*grad(u)|^2*I - 2*b*grad(u)*grad(u)^t

where:    1. a = 1, b = 0 is is the coeff for the nolinear diffusion term
          2. a = b = dt is the coeff for the Newton Linearization diffusion term
*/
class Problem; 

class NonlinearCoefficient : public MatrixCoefficient
{
protected:
   Problem *problem;
   int dim;
   ParGridFunction *gfunc;   
   double a, b, time; 

public:
   NonlinearCoefficient(Problem *_problem, ParGridFunction *_gfunc);

   void SetGridFunction( HypreParVector &X, double _a, double _b, double time = -1 );
   void Eval(DenseMatrix &K, ElementTransformation &T, const IntegrationPoint &ip);

   virtual void Read(istream &in) { }

   virtual ~NonlinearCoefficient();
};

/*************************************************************************************
Class that contains all coefficients and variables relating to the manufactured solution */
class Problem
{
protected:
   static double kap;  //Spatial frequency of manufactured solution
   static double tau;  //Temporal frequency of manufactured solution
   static double power; // p in p-laplacian equation
   static double t_start;
   static double C;

public:

   ConstantCoefficient one;          //One
   FunctionCoefficient *source;      //Source Term
   VectorFunctionCoefficient *nbc; //Boundary Conditions
   FunctionCoefficient *exact_sol;    //Exact Solution
   FunctionCoefficient *ic;			 // Initial Condition
   Array<NonlinearCoefficient *> nlc;   // Nonlinear Coefficient


   Problem(double _t_start, double _C, double _kap, double _tau, double _power, int num_space_levels);
   
   // These functions are needed to set the exact solution and return the boundady conditions
   // intitial condition, and the source term to fit that exact solution 
   static void Solution(double x, double y, double t, double &u);
   static void FirstDeriv(double x, double y, double t, double &Ut, double &Ux, double &Uy);
   static void SecondDeriv(double x, double y, double t, double &Uxx, double &Uyy, double &Uxy);
   
   // The problem solves the equation u_t - div( K(u,x,t) grad(u) ) = g. This function
   // gives f(u,x,t) and its derivitives. 
   static void ff(double x, double y, double t, double &f, double &fx, double &fy);
   
   //When forming the diffusion matrix, we allow for a matrix coefficient. K is the 
   //diffusion matrix coefficient, and J is the Newton linearization Matrix coefficient. 
   //That is: K = K(u,x,t), J = is ... TODO 
   static void GetKJ( ParGridFunction *g,
                               double time, 
                               ElementTransformation &T, 
                               const IntegrationPoint &ip,
                               DenseMatrix &K,
                               DenseMatrix &J);

   //Neuman boundary Condition
   static void   NBC(const Vector &p, double t, Vector &v);
   static double ExSol(Vector &p, double t);
   static double ST(Vector &p, double t);
   static double IC(Vector &x);

   virtual ~Problem();
};

/*************************************************************************************
Struct to hold all command line and defualt options */
struct NonlinearOptions : public BraidOptions
{

   int mi_fine, mi_coarse;      //Max nonlinear iterations on f/c grids
   double tol_fine, tol_coarse; //Nonlinear tolerance on f/c grids
   int ode_solver_type;         //What type of ode solver to use
   int cheap_first_iters;       //(bool) Make the first two iterations of Braid use
   //   sqrt(tol) as the fine-grid Newton tolerance
   int scale_coarse_newt_tol;   //(bool) Scale the coarse-grid Newton tolerance
   //   by min( (4**lvl)*tol, 0.001)

   int sc_print_level; //Spatial coarsening print level
   int ndelay  ;        // Spatial coarsening delay level
   int braid_level; //Current temporal level of braid
   int braid_iter;  //Current braid Iteration

   int boomer_mi_fine;   // Max inner-inner BoomerAMG iterations on fine grid
   int boomer_mi_coarse; // Max inner-inner BoomerAMG iterations on all coarse grids
   int boomer_mi;        // Actual value used to set max BoomerAMG iterations
   // (alternates between fine and coarse, set int Step())

   double kap, tau; //Exact Solution parameters
   double power;    //P in p-laplacian
   double dt;       //time step on the finest grid.
   double C;        //constant in p-laplacian
   //Visualization Options
   const char *vishost;
   int         visport;
   int vis_time_steps;   // Visualize every x time steps
   int vis_braid_steps;  // Visualize every x braid iterations
   bool vis_screenshots; // Save Screenshots t
   Problem *prob;       // All of the problem specific info
   HypreParVector *u;  //Stores a pointer to the initial guess for nonlinear solver
   BraidUtil util;     // Braid utility for tolerance scaling.

   NonlinearOptions(int argc, char *argv[]);

   virtual ~NonlinearOptions();
};

/*************************************************************************************
A time dependent Operator that solves the nonlinear system ode using newtons method */
class NonlinearSystemODE : public TimeDependentOperator
{
private:


   ParBilinearForm *a;
   ParLinearForm *b_form;


   mutable HypreParVector *b;
   mutable HypreParMatrix *A;
   HypreParMatrix *B, *M;
   Coefficient *b_coeff;
   VectorCoefficient *b_vcoeff;
   NonlinearOptions *opts;
   NonlinearCoefficient *nl_coeff;
   HypreParVector *X, *Y, *Z;
   HypreParVector *U, *W;
   HyprePCG *B_pcg;
   HypreBoomerAMG *B_amg;
   int braid_iter;

public:
   NonlinearSystemODE(int              l,
                      ParBilinearForm *_a,
                      HypreParMatrix  *_M,
                      ParLinearForm   *_b_form,
                      NonlinearOptions *_opts);


   void AssembleDiffusionMatrix() const;

   // Assemble the rhs
   void AssembleBVector() const;

   //Nonlinear Multiplication -- Not implimented yet -- Not needed??
   virtual void Mult(const Vector &x, Vector &y) const;

   //Nonlinear Solve using Newtons Method
   virtual void ImplicitSolve(const double dt, const Vector &x, Vector &k);

   //Store the number of iterations taken during the last time-step
   int num_iter;

   virtual ~NonlinearSystemODE();
};

/*************************************************************************************
Class defining the NonlinearApp */
class NonlinearApp : public MFEMBraidApp
{
protected:

   NonlinearOptions *opts;
   FiniteElementCollection *fe_coll;
   Problem *prob;
   int braid_iter;


   //Allocate Arrays needed by the NonlinearApp
   virtual void AllocLevels(int num_levels);

   //Construct the Meshes on each level
   virtual ParFiniteElementSpace *ConstructFESpace(ParMesh *pmesh);

   //Init the ode/solver/max_dt for each level
   virtual void InitLevel(int l);

public:
   SpaceTimeMeshInfo MeshInfo;
   Array<double> NewtonItersMax, NewtonItersMin, NewtonItersSum, NewtonItersCount;

   NonlinearApp(NonlinearOptions *opts, MPI_Comm comm_t_, ParMesh *pmesh);

   //Modified MFEMBraidApp::Phi to also record space-time mesh info
   virtual int Step(braid_Vector    u_,
                    braid_Vector    ustop_,
                    braid_Vector    fstop_,
                    BraidStepStatus &pstatus);

   void PrintNewtonInfo(int num_levels, int max_levels, int num_iter, int skip, int myid_x);

   virtual ~NonlinearApp();

};

/*************************************************************************************
Main */
int main (int argc, char *argv[])
{
   //Init MPI
   MPI_Init(&argc, &argv);

   MPI_Comm comm = MPI_COMM_WORLD;
   int myid, myid_x, num_levels, num_iter;
   double hmin, hmax;
   MPI_Comm_rank(comm, &myid);

   //Parse Command Line
   NonlinearOptions opts(argc, argv);

   if (!opts.Good())
   {
      if (myid == 0)
      {
         opts.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }
   // Print the used options
   if (myid == 0)
   {
      opts.PrintOptions(cout);
   }

   //Load the Mesh and refine in serial
   Mesh *mesh = opts.LoadMeshAndSerialRefine();
   if (!mesh)
   {
      if (myid == 0)
      {
         cerr << "\nError loading mesh file: " << opts.mesh_file
              << '\n' << endl;
      }
      MPI_Finalize();
      return 2;
   }

   // Split comm (MPI_COMM_WORLD) into spatial and temporal communicators
   MPI_Comm comm_x, comm_t;
   BraidUtil util;
   util.SplitCommworld(&comm, opts.num_procs_x, &comm_x, &comm_t);

   // Partition the mesh accross the spatial communicator
   MPI_Comm_rank(comm_x, &myid_x);
   ParMesh *pmesh = new ParMesh(comm_x, *mesh);
   delete mesh;

   //Init braid App
   NonlinearApp app(&opts, comm_t, pmesh);
   //Run braid

   //pmesh->PrintInfo();
   BraidCore core(comm, &app);
   // Not sure why this doesn't work here...so we assume that the hmin and hmax
   // are based on inline-quad.mesh which has a characteristic size of 0.25
   //app.MeshInfo.ComputeMeshSize(pmesh, &hmin, &hmax);
   hmax = hmin = 0.25 / pow(2.0, (opts.par_ref_levels+opts.ser_ref_levels));
   opts.tol = opts.tol / (sqrt(opts.dt)*hmax);
   opts.SetBraidCoreOptions(core);
   core.Drive();

   core.GetNLevels(&num_levels);
   core.GetNumIter(&num_iter);
   app.PrintNewtonInfo(num_levels, opts.max_levels, num_iter, opts.skip, myid_x);
   if (opts.sc_print_level)
   {
      app.MeshInfo.Print(comm);
   }

   MPI_Finalize();
   return 0;
}

//*************************************************************************************//
/* Implimentation::NonlinearCoefficient */
NonlinearCoefficient::NonlinearCoefficient(Problem *_problem, ParGridFunction *_gfunc )
   :MatrixCoefficient(2), problem(_problem), gfunc(_gfunc), a(1), b(0), time(-1)
{
}

void NonlinearCoefficient::SetGridFunction(HypreParVector &X, double _a, double _b, double _time)
{
   *gfunc = X; // distribute (communication)
   a = _a;
   b = _b;
   time = _time;
}

void NonlinearCoefficient::Eval(DenseMatrix &K, ElementTransformation &T, const IntegrationPoint &ip)
{
   
   DenseMatrix Jacobi(2); 
   problem->GetKJ( gfunc, time, T, ip, K, Jacobi );
   K *= -1.0*a;
   Jacobi *= -1.0*b;
   K += Jacobi;  
   return;
}

NonlinearCoefficient::~NonlinearCoefficient()
{
}

/*************************************************************************************
Implimentaion::Problem */

//Initailize static data members
double Problem::kap = 0;
double Problem::power = 0;
double Problem::tau = 0;
double Problem::t_start = 0;
double Problem::C = 0;

Problem::Problem(double _t_start, double _C, double _kap, double _tau, double _power, int num_space_levels)
   :  one(1.0), nlc(num_space_levels)
{
   t_start = _t_start;
   kap = _kap;
   tau = _tau;
   power = _power;
   C = _C;
   source = new FunctionCoefficient(ST);
   exact_sol = new FunctionCoefficient(ExSol);
   ic = new FunctionCoefficient(IC);
   nbc = new VectorFunctionCoefficient(2,NBC);
}

void Problem::Solution(double x, double y, double t, double &u)
{
   u = sin(kap*x)*sin(kap*y)*cos(tau*t);
   return;
}

void Problem::FirstDeriv(double x, double y, double t, double &Ut, double &Ux, double &Uy)
{
   Ut = -tau*sin(kap*x)*sin(kap*y)*sin(tau*t);
   Ux =  kap*cos(kap*x)*sin(kap*y)*cos(tau*t);
   Uy =  kap*sin(kap*x)*cos(kap*y)*cos(tau*t);
   return;                       
}

void Problem::SecondDeriv(double x, double y, double t, double &Uxx, double &Uyy, double &Uxy)
{
   Uxx = -kap*kap*sin(kap*x)*sin(kap*y)*cos(tau*t);
   Uxy =  kap*kap*cos(kap*x)*cos(kap*y)*cos(tau*t);
   Uyy =  Uxx;  
   return;
}

void Problem::ff(double x, double y, double t, double &f, double &fx, double &fy)
{
   double Ut, Ux, Uy, Uxx,Uyy, Uxy;
   FirstDeriv(x, y, t, Ut, Ux, Uy);
   SecondDeriv(x,y,t, Uxx, Uyy, Uxy);
   
   if ( power < 0 )
   {
      f = Ux*Ux + Uy*Uy;
      fx = 2.0*Ux*Uxx + 2.0*Uy*Uxy;
      fy = 2.0*Ux*Uxy + 2.0*Uy*Uyy;
   }
   else if ( power == 0 )
   {
      f = 1;
      fx = 0;
      fy = 0;
   }
   else
   {
       f  = pow(Ux*Ux + Uy*Uy,power/2);
       fx = (power/2.0)*pow( Ux*Ux +Uy*Uy , (power/2.0) - 1 )*(  2.0*Ux*Uxx + 2.0*Uy*Uxy );
       fy = (power/2.0)*pow( Ux*Ux +Uy*Uy , (power/2.0) - 1 )*(  2.0*Ux*Uxy + 2.0*Uy*Uyy );
   }
   return;
}

void Problem::GetKJ( ParGridFunction *g,
                               double time, 
                               ElementTransformation &T, 
                               const IntegrationPoint &ip,
                               DenseMatrix &K,
                               DenseMatrix &J)
{
   

    if ( power > 0 )
   {
        Vector grad;
        g->GetGradient(T, grad);
        K.Diag( C + pow( grad * grad , power/2. ) ,2);
      
        double *data = grad.GetData();  
        double aa[4] = {data[0]*data[0],data[0]*data[1],data[0]*data[1],data[1]*data[1]};
        J = aa;
        J *= power*pow(grad * grad, (power-2.)/2.) ;
   }
   else if ( power == 0 )
   {
      K.Diag(1+C, 2);
      J = 0.0;
   }
   else //Remove the nonlinearaity
   {
     double x[3], f, fx, fy;     
     Vector p(x, 3);
     T.Transform(ip, p);
     ff(p(0),p(1), time, f, fx, fy);
     K.Diag(C+f, 2);
     J = 0.0;
   }
}

void Problem::NBC(const Vector &p, double t, Vector &v)
{

   double f, fx, fy, Ut, Ux, Uy;
   FirstDeriv(p(0), p(1), t, Ut, Ux, Uy);
   ff(p(0),p(1),t, f, fx, fy);
   
   v(0) = ( f + C)*Ux;
   v(1) = ( f + C)*Uy;
   return;
}

double Problem::ExSol(Vector &p, double t)
{
   double u;
   Solution(p(0),p(1),t,u);
   return u;
}

double Problem::ST(Vector &p, double t)
{
   double Ut, Ux, Uy, Uxx, Uyy, Uxy, f, fx, fy;
   FirstDeriv( p(0), p(1), t, Ut, Ux, Uy );
   SecondDeriv( p(0), p(1), t, Uxx, Uyy, Uxy);
   ff( p(0), p(1),t, f, fx, fy );

   return Ut - (C+f)*Uxx - (C+f)*Uyy - Ux*fx - Uy*fy ; 

}
double Problem::IC(Vector &x)
{
   return ExSol(x,Problem::t_start);
}

Problem::~Problem()
{
   delete source;
   delete exact_sol;
   delete ic;
   delete nbc;

   for (int l = 0; l < nlc.Size() ; l++)
   {
      delete nlc[l];
   }
}
/*************************************************************************************
Implementation::NonlinearOptions */
NonlinearOptions::NonlinearOptions(int argc, char *argv[])
   : BraidOptions(argc, argv)
{
   //Set inherited default options
   num_time_steps = 32;
   t_start = 0.0;
   t_final = 4.0;
   //Set defualts for the mesh/refinement inherited options
   //mesh_file = "/g/g90/oneill10/Programs_vulcan/mfem-3.2/data/inline-quad.mesh";
   mesh_file = "/home/ben/xbraid/drivers/inline-quad.mesh";
   ser_ref_levels = 1;
   par_ref_levels = 1;
   AddMeshOptions();

   //Set defualts for the NonlinearOptions specific options
   mi_fine = 100;
   mi_coarse = 100;
   cheap_first_iters = 0;
   scale_coarse_newt_tol = 0;
   boomer_mi_fine = 2000;
   boomer_mi_coarse = 2000;
   boomer_mi = 2000;
   tol_fine = 0.0000001;
   tol_coarse = tol_fine;
   kap = M_PI;
   tau = (2 + 1./6.)*M_PI;
   power = 2;
   C = 0;
   ndelay = 2;

   sc_print_level = 0;
   ode_solver_type = -1;

   vishost         = "localhost";
   visport         = 19916;
   vis_time_steps  = 0;
   vis_braid_steps = 1;
   vis_screenshots = false;

   //Parse Command Line

   AddOption(&ndelay, "-delay", "--delay",
             "delay spatial refinement until braid level ndelay fine grid.");
   AddOption(&mi_fine, "-mif", "--max-iter-fine",
             "Max Newton iterations on the fine grid.");
   AddOption(&mi_coarse, "-mic", "--max-iter-coarse",
             "Max Newton iterations on the coarse grids.");
   AddOption(&boomer_mi_fine, "-boomer_mif", "--boomer-max-iter-fine",
             "Max BoomerAMG iterations per Newton iteration on the fine grid.");
   AddOption(&boomer_mi_coarse, "-boomer_mic", "--boomer-max-iter-coarse",
             "Max BoomerAMG iterations per Newton iterations on the coarse grids.");
   AddOption(&tol_fine, "-rtf", "--resid-tol-fine",
             "Nonlinear Residual tolerance on the fine grid.");
   AddOption(&tol_coarse, "-rtc", "--resid-tol-coarse",
             "Nonlinear Residual tolerance on the coarse grid.");
   AddOption(&cheap_first_iters, "-cfi", "--cheap-first-iter",
             "(0 or 1) Make the first two iterations of Braid use sqrt(tol_fine) as the fine-grid Newton tolerance.");
   AddOption(&scale_coarse_newt_tol, "-scnt", "--scale-coarse-newt-tol",
             "(0 or 1) Scale the coarse-grid Newton tolerance by min( (4**lvl)*tol, 0.001).");
   AddOption(&sc_print_level, "-scprint", "--spatial-coarsening-print_level",
             "Print level for spatial coarseing.");
   AddOption(&ode_solver_type, "-s", "--ode-solver",
             "ODE solver: 1 - Forward Euler, 2 - RK2 SSP, 3 - RK3 SSP,"
             " 4 - RK4, 6 - RK6, -1 - Backward Euler, -2 - SDIRK22 "
             " -3 - SDIRK33 -4 -SDIRK23, -5 - SDIRK34.");
   AddOption(&power, "-pow", "--p-laplacian",
             "Value of p in the p-laplacian.");
   AddOption(&C, "-C", "--constant in p-laplacian",
             "Value of C in the p-laplacian.");
   AddOption(&vishost, "-vh", "--visualization-host",
             "Set the GLVis host.");
   AddOption(&visport, "-vp", "--visualization-port",
             "Set the GLVis port.");
   AddOption(&vis_time_steps, "-vts", "--visualize-time-steps",
             "Visualize every n-th time step (0:final only).");
   AddOption(&vis_braid_steps, "-vbs", "--visualize-braid-steps",
             "Visualize every n-th Braid step (0:final only).");
   AddOption(&vis_screenshots, "-vss", "--vis-screenshots , 1",
             "-no-vss", "--vis-no-screenshots, 0", "Enable/disable the saving of"
             " screenshots of the visualized data.");

   Parse();

   //Init Problem class

   if (spatial_coarsen)
   {
      prob = new Problem(t_start, C, kap, tau, power, par_ref_levels + 1);
   }
   else
   {
      prob = new Problem(t_start, C, kap, tau, power, 1);
   }

   dt = (t_final - t_start) / num_time_steps;

}


NonlinearOptions::~NonlinearOptions()
{
   delete prob;
}

/*************************************************************************************
Implementation::NonlinearSystemODE */
NonlinearSystemODE::NonlinearSystemODE(int             l,
                                       ParBilinearForm *_a,
                                       HypreParMatrix  *_M,
                                       ParLinearForm   *_b_form,
                                       NonlinearOptions *_opts) :
   a(_a), b_form(_b_form), M(_M), b_coeff(_opts->prob->source), b_vcoeff(_opts->prob->nbc),
   opts(_opts), nl_coeff(_opts->prob->nlc[l])
{
   a->Assemble();
   a->Finalize();
   A = a->ParallelAssemble();
   width = A->Width();
   height = A->Height();
   num_iter = -1;

   double tmp; // workaround to avoid memory leak
   X = new HypreParVector(A->GetComm(), A->GetGlobalNumCols(), &tmp,
                          A->GetColStarts());
   Y = new HypreParVector(A->GetComm(), A->GetGlobalNumCols(), &tmp,
                          A->GetColStarts());

   b = NULL;
   B = NULL;
   B_amg = NULL;
   B_pcg = NULL;

   U = new HypreParVector(*A);
   W = new HypreParVector(*A);
   Z = new HypreParVector(*A);
}

void NonlinearSystemODE::AssembleDiffusionMatrix() const
{
   delete A;
   a->Update();
   a->Assemble();
   a->Finalize();
   A = a->ParallelAssemble();
}

void NonlinearSystemODE::AssembleBVector() const
{
   b_coeff->SetTime(GetTime());
   b_vcoeff->SetTime(GetTime());
   b_form->Assemble();
   if (!b)
   {
      b = b_form->ParallelAssemble();
   }
   else
   {
      b_form->ParallelAssemble(*b);
   }
}

void NonlinearSystemODE::Mult(const Vector &x, Vector &y) const
{
   MFEM_ABORT(" Nonlinear System ODE is implimented for Backward Euler only. " )
}

//Iterate over the system M*k^(j+1) = A(x + dt*k(j)) + b(x,t)
void NonlinearSystemODE::ImplicitSolve(const double dt, const Vector &x, Vector &k)
{

   //Set the nonlinear options
   num_iter = 0;
   double resid;
   double tol;
   int max_it;
   int vcycles = 0;
   int temp_vcycles;
   int cheap_first_iters = opts->cheap_first_iters;
   int scale_coarse_newt_tol = opts->scale_coarse_newt_tol;

   if (opts->braid_level == 0)
   {
      tol = opts->tol_fine;
      max_it = opts->mi_fine;
   }
   else
   {
      tol = opts->tol_coarse;
      max_it = opts->mi_coarse;
   }

   // If using cheap first iters, use everywhere.
   // Else, (if enabled) scale only the coarse-grid tolerances
   if(cheap_first_iters && (opts->braid_iter < 2))
   {
      tol = sqrt(tol);
   }
   else if(scale_coarse_newt_tol)
   {
      tol = std::min( pow(4.0, opts->braid_level)*tol, 0.001);
   }

    /*The variables are as follows: 
     * X == u_j the solution at time t = t_j
     * Z == u_j+1 == the current guess at the solution at t = t_j + dt
     * Y == k == the current guess at k = (Z-X)/dt
     * W == Residual = Mk - A(Z)[Z] - b 
     * b == Source vector
     * U == No specific definition 
    **/ 
   
   //Wrap the data (X == x, Y == k)
   X->SetData(x.GetData());
   Y->SetData(k.GetData());

   //Assemble the source term vector
   AssembleBVector();

   //Set Inital guess for the diffusion matrix
   *Z = *(opts->u);
   nl_coeff->SetGridFunction(*Z, 1.0,0.0, GetTime());
   AssembleDiffusionMatrix(); 

   //Set the initial guess for Y 
   add(1.0/dt, *Z, -1.0/dt , *X, *Y);

   //Do a check to see if we even need to complete an iteration.
   A->Mult(*Z,*U);		       	// U = A[x+k(j)*dt](x+k(j)*dt)
   M->Mult(*Y,*W);			      // W = Mk(j)
   *W -= *U;                     // W = Mk - A[]()
   *W -= *b;                     // W = Mk - A[]() - RHS
   resid = sqrt(InnerProduct(W,W));
 
   while ( resid > tol && num_iter < max_it )    
   {
      if (!B)
      {
         B = new HypreParMatrix(hypre_ParCSRMatrixAdd(*M, *A));
      }

      num_iter+=1;
      nl_coeff->SetGridFunction(*Z, 1.0,1.0, GetTime());
      AssembleDiffusionMatrix();  // Newton Linearization Matrix

      hypre_ParCSRMatrixSetConstantValues(*B, 0.0);
      hypre_ParCSRMatrixSum(*B, 1.0, *M);
      hypre_ParCSRMatrixSum(*B, -dt, *A);

      B_amg = new HypreBoomerAMG(*B);
      B_amg->SetPrintLevel(-1);
      B_pcg = new HyprePCG(*B);
      B_pcg->SetTol(tol);
      B_pcg->SetMaxIter(opts->boomer_mi);
      B_pcg->SetPrintLevel(0);
      B_pcg->SetPreconditioner(*B_amg);

      // Calculate the Newton update to Y = Y - DY 
      *U = 0.0;               // Set initial condition
      B_pcg->Mult(*W, *U);    // Solve for the Newton update
      *Y -= *U;               // Update Y
      add(*X, dt, *Y, *Z);    // Upadte Z

      //Update the Residual 
      nl_coeff->SetGridFunction(*Z, 1.0,0.0, GetTime() );
      AssembleDiffusionMatrix();               
      A->Mult(*Z,*U);			                
      M->Mult(*Y,*W);			                 
      *W -= *U;                               
      *W -= *b;                              
      resid = sqrt(InnerProduct(W,W));         

      //Pull the convergence information and clean up. 
      B_pcg->GetNumIterations(temp_vcycles);
      vcycles += temp_vcycles;
      delete B_pcg;
      delete B_amg;
            
   }
   if ( opts->power <= 0 )
   {
      num_iter = vcycles; 
   }
   braid_iter = opts->braid_iter;
}

NonlinearSystemODE::~NonlinearSystemODE()
{
   delete B;
   delete b;
   delete A;
   delete b_form;
   delete a;
   delete M;
   delete X;
   delete Y;
   delete Z;
   delete U;
   delete W;

}



/*************************************************************************************
Implementation::NonlinearApp */
NonlinearApp::NonlinearApp(
   NonlinearOptions *_opts, MPI_Comm comm_t_, ParMesh *pmesh)
   : MFEMBraidApp(comm_t_, _opts->t_start, _opts->t_final, _opts->num_time_steps),
     opts(_opts), MeshInfo(opts->max_levels)

{
   //Init all meshes/odes/solvers/etc for all space levels
   fe_coll = new LinearFECollection;
   InitMultilevelApp(pmesh, opts->par_ref_levels, opts->spatial_coarsen);

   //Set the initial/exact solutions
   x[0]->ProjectCoefficient(*(opts->prob->ic));
   HypreParVector *U = x[0]->GetTrueDofs();
   SetInitialCondition(U);
   SetExactSolution(opts->prob->exact_sol);

   //Init glvis visualization
   SetVisHostAndPort(opts->vishost, opts->visport);
   SetVisSampling(opts->vis_time_steps, opts->vis_braid_steps);
   SetVisScreenshots(opts->vis_screenshots);

   //Set the array size for Newton iteration counters
   NewtonItersMax.SetSize(opts->max_levels*(opts->max_iter+1), 0);
   NewtonItersMin.SetSize(opts->max_levels*(opts->max_iter+1), 100000);
   NewtonItersSum.SetSize(opts->max_levels*(opts->max_iter+1), 0);
   NewtonItersCount.SetSize(opts->max_levels*(opts->max_iter+1), 0);
}

int NonlinearApp::Step(braid_Vector    u_,
                       braid_Vector    ustop_,
                       braid_Vector    fstop_,
                       BraidStepStatus &pstatus)
{
   // This contains one small change over the default Phi, we store the Space-Time mesh info

   // Extract info from braid_vector
   BraidVector *u = (BraidVector*) u_;
   BraidVector *ustop = (BraidVector*) ustop_;

   int level = u->level;
   int braid_level;
   int braid_iter, index;
   double iters_taken;

   pstatus.GetLevel(&braid_level);
   pstatus.GetIter(&braid_iter);

   opts->braid_iter = braid_iter;
   opts->braid_level =  braid_level;

   double tstart, tstop, dt;
   pstatus.GetTstartTstop(&tstart, &tstop);
   dt = tstop - tstart;

   //Store run-time mesh info
   if (opts->sc_print_level)
   {
      MeshInfo.SetRow(braid_level, u->level, x[u->level]->ParFESpace()->GetParMesh(), dt);
   }

   if(braid_level == 0)
   {
      opts->boomer_mi = opts->boomer_mi_fine;
   }
   else
   {
      opts->boomer_mi = opts->boomer_mi_coarse;
   }
   opts->u = ustop;
   MFEMBraidApp::Step(u_,ustop_, fstop_, pstatus);

   //Store Number of Newton Iterations
   iters_taken = ((NonlinearSystemODE *)(ode[level]))->num_iter;
   index = opts->max_levels*braid_iter + braid_level;
   NewtonItersMax[index] =  std::max(iters_taken, NewtonItersMax[index]);
   NewtonItersMin[index] =  std::min(iters_taken, NewtonItersMin[index]);
   NewtonItersSum[index] += iters_taken;
   NewtonItersCount[index] +=  1;

   // no refinement
   pstatus.SetRFactor(1);
   return 0;
}

void NonlinearApp::AllocLevels(int num_levels) {}

ParFiniteElementSpace* NonlinearApp::ConstructFESpace(ParMesh *pmesh)
{
   return new ParFiniteElementSpace(pmesh, fe_coll);
}

void NonlinearApp::InitLevel(int l)
{
   //Create Bilinear and linear forms
   ParBilinearForm *a = new ParBilinearForm(fe_space[l]);
   ParBilinearForm *m = new ParBilinearForm(fe_space[l]);
   ParLinearForm   *b = new ParLinearForm(fe_space[l]);

   //Create a nonlinear coefficient for this level
   opts->prob->nlc[l] = new NonlinearCoefficient(opts->prob, x[l]);

   //Add Boundary and domain integrators
   m->AddDomainIntegrator(new MassIntegrator(opts->prob->one));
   b->AddDomainIntegrator(new DomainLFIntegrator(*opts->prob->source));
   b->AddBoundaryIntegrator(new BoundaryNormalLFIntegrator(*opts->prob->nbc));
   a->AddDomainIntegrator(new DiffusionIntegrator(*opts->prob->nlc[l]));
   // Local assembly. Use precomputed sparsity to ensure that 'a' and 'm' have
   // the same sparsity patterns.
   a->UsePrecomputedSparsity();
   m->UsePrecomputedSparsity();

   //Finalize the constant Mass Matrix
   m->Assemble();
   m->Finalize();
   HypreParMatrix *M = m->ParallelAssemble();
   delete m;

   //Create a nonlinear system to be solved on this level
   ode[l] = new NonlinearSystemODE(l, a, M, b, opts );

   //Set ODESolver and inititialize with ode[l]
   ODESolver *ode_solver = NULL;
   switch (opts->ode_solver_type)
   {
   case -1:
      ode_solver = new BackwardEulerSolver;
      break;
   case -2:
      ode_solver = new SDIRK23Solver(2);
      break;
   case -3:
      ode_solver = new SDIRK33Solver;
      break;
   case -4:
      ode_solver = new SDIRK23Solver;
      break;
   case -5:
      ode_solver = new SDIRK34Solver;
      break;

   default:
      ode_solver = new BackwardEulerSolver;
      break;
   }

   solver[l] = ode_solver;
   solver[l]->Init(*ode[l]);

   // Set max_dt[l] -- the maximum time step allowed on this level
   if ( l == 0 && opts->spatial_coarsen)
   {
      int Nfine(opts->ndelay),myid ;
      MPI_Comm_rank(MPI_COMM_WORLD, &myid);
      double dd = 1.01*( (opts->t_final - opts->t_start)/( (double) opts->num_time_steps ) );
      max_dt[0] = dd*pow( (double) opts->cfactor, (double) Nfine );
      if ( myid == 0 )
      {
         std::cout << " Min time steps on space level 0 " << 4.0/max_dt[0] <<  std::endl;
      }
      for (int i = 1; i <= opts->par_ref_levels; i++)
      {
         max_dt[i] = max_dt[i-1]*( (double) opts->cfactor );
         if ( myid == 0 )
         {
            std::cout << " Min time steps on space level " << i << " " << 4.0/max_dt[i] <<  std::endl;
         }
      }
      max_dt[opts->par_ref_levels] = opts->t_final - opts->t_start ;
   }
}

void NonlinearApp::PrintNewtonInfo(int num_levels, int max_levels, int num_iter, int skip, int myid_x)
{
   int myid_global, index;
   double maxg, ming, sumg, countg;

   MPI_Comm_rank(MPI_COMM_WORLD, &myid_global);

   if(num_levels == 1)
   {
      num_iter = 1;
      skip = 0;
   }

   std::cout.precision(2);
   std::cout << std::scientific;

   // The data is replicated across each spatial communicator, so only have the first process
   // in each comm_x participate is computing global values.  Note, that each comm_t corresponds
   // to processors with the same myid_x value.
   if(myid_x == 0)
   {
      for(int i=0; i < num_iter; i++)
      {
         for(int j=0; j < num_levels; j++)
         {
            index = i*max_levels + j;
            MPI_Allreduce(&(NewtonItersMax[index]), &maxg, 1, MPI_DOUBLE, MPI_MAX, comm_t);
            NewtonItersMax[index] = maxg;
            MPI_Allreduce(&(NewtonItersMin[index]), &ming, 1, MPI_DOUBLE, MPI_MIN, comm_t);
            NewtonItersMin[index] = ming;
            MPI_Allreduce(&(NewtonItersSum[index]), &sumg, 1, MPI_DOUBLE, MPI_SUM, comm_t);
            NewtonItersSum[index] = sumg;
            MPI_Allreduce(&(NewtonItersCount[index]), &countg, 1, MPI_DOUBLE, MPI_SUM, comm_t);
            NewtonItersCount[index] = countg;
         }
      }
   }

   if(skip == 1)
   {
      NewtonItersMax[0] = -1;
      NewtonItersMin[0] = -1;
      NewtonItersSum[0] = -1;
      NewtonItersCount[0] = 1;
   }

   if(myid_global == 0)
   { 
      if ( opts->power <= 0)
          std::cout << "MAX BoomerAMG Iterations Gathered at Run-Time" << std::endl;
      else         
         std::cout << "MAX Newton Iterations Gathered at Run-Time" << std::endl;
      std::cout << " Iteration |";
      for(int i=0; i<num_levels; i++)
      {
         cout << " Level = " << i << " |";
      }
      std::cout << "\n------------";
      for(int i=0; i<num_levels; i++)
      {
         cout << "------------";
      }
      std::cout << "\n";
      for(int i=0; i < num_iter; i++)
      {
         std::cout << std::setw(10) << i << " |";
         for(int j=0; j < num_levels; j++)
         {
            std::cout << std::setw(10) << NewtonItersMax[i*max_levels + j] << " |";
         }
         std::cout << '\n';
      }

      std::cout << "\n";
      if ( opts->power <= 0 )
         std::cout << "MIN BoomerAMG iterations Gathered at Run-Time" << std::endl;
      else
         std::cout << "MIN Newton Iterations Gathered at Run-Time" << std::endl;
      std::cout << " Iteration |";
      for(int i=0; i<num_levels; i++)
      {
         cout << " Level = " << i << " |";
      }
      std::cout << "\n------------";
      for(int i=0; i<num_levels; i++)
      {
         cout << "------------";
      }
      std::cout << "\n";
      for(int i=0; i < num_iter; i++)
      {
         std::cout << std::setw(10) << i << " |";
         for(int j=0; j < num_levels; j++)
         {
            std::cout << std::setw(10) << NewtonItersMin[i*max_levels + j] << " |";
         }
         std::cout << '\n';
      }

      std::cout << "\n";
      
      if ( opts->power <= 0 )
         std::cout << "AVG BoomerAMG iterations Gathered at Run-Time" << std::endl;
      else
         std::cout << "AVG Newton Iterations Gathered at Run-Time" << std::endl;
      
      std::cout << " Iteration |";
      for(int i=0; i<num_levels; i++)
      {
         cout << " Level = " << i << " |";
      }
      std::cout << "\n------------";
      for(int i=0; i<num_levels; i++)
      {
         cout << "------------";
      }
      std::cout << "\n";
      for(int i=0; i < num_iter; i++)
      {
         std::cout << std::setw(10) << i << " |";
         for(int j=0; j < num_levels; j++)
         {
            std::cout << std::setw(10) << NewtonItersSum[i*max_levels + j]/NewtonItersCount[i*max_levels + j] << " |";
         }
         std::cout << '\n';
      }
      std::cout << "\n";

   }


}

NonlinearApp:: ~NonlinearApp()
{
   delete fe_coll;
}
