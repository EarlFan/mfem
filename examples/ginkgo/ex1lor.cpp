//                          MFEM Example 1, modified
//
// This code has been modified from `ex1.cpp` provided in the examples of
// MFEM.  The sections not marked as related to Ginkgo are largely unchanged
// from the version provided by MFEM.
//
// This code also contains portions from `miniapps/performance/ex1.cpp`
// pertaining to the LOR preconditioner.  The preconditioner types
// used in this example are applied to the SparseMatrix created from
// the LOR mesh, so they are all LOR preconditioners with different
// subtypes.
//
// The default mesh option is "beam-hex.mesh", provided by MFEM.
// Important non-default options:
//  -m [file] : Mesh file.
//  -d "cuda" : Use the MFEM cuda backend and Ginkgo CudaExecutor.
//  -pc-type "gko:ilu" : Use the Ginkgo ILU preconditioner (default is Block
//                       Jacobi)
//  -pc-type "none" : No LOR preconditioner
//
//  Options only for the Block Jacobi preconditioner (default:)
//  -pc-so "none" : Don't let Ginkgo automatically pick options for precision
//                  reduction in the storage of the Block Jacobi preconditioner
//  -pc-acc [value] : Accuracy parameter.
//
// MFEM's provided information about `ex1.cpp`:
// Description:  This example code demonstrates the use of MFEM to define a
//               simple finite element discretization of the Laplace problem
//               -Delta u = 1 with homogeneous Dirichlet boundary conditions.
//               Specifically, we discretize using a FE space of the specified
//               order, or if order < 1 using an isoparametric/isogeometric
//               space (i.e. quadratic for quadratic curvilinear mesh, NURBS for
//               NURBS mesh, etc.)
//
//               The example highlights the use of mesh refinement, finite
//               element grid functions, as well as linear and bilinear forms
//               corresponding to the left-hand side and right-hand side of the
//               discrete linear system. We also cover the explicit elimination
//               of essential boundary conditions, static condensation, and the
//               optional connection to the GLVis tool for visualization.

#include "mfem.hpp"

#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

// helper functions for cg/pcg solve with timer and iter count return
int cg_solve(const Operator &A, const Vector &b, Vector &x,
        int print_iter, int max_num_iter,
        double RTOLERANCE, double ATOLERANCE, double &it_time)
{

   int total_its = 0;
 
   CGSolver cg;
   cg.SetPrintLevel(print_iter);
   cg.SetMaxIter(max_num_iter);
   cg.SetRelTol(sqrt(RTOLERANCE));
   cg.SetAbsTol(sqrt(ATOLERANCE));
   cg.SetOperator(A);

   tic_toc.Clear();
   tic_toc.Start();
   
   cg.Mult(b, x);
  
   tic_toc.Stop();
   it_time = tic_toc.RealTime();

   return cg.GetNumIterations();
}

int pcg_solve(const Operator &A, Solver &B, const Vector &b, Vector &x,
        int print_iter, int max_num_iter,
        double RTOLERANCE, double ATOLERANCE, double &it_time)
{

   int total_its = 0;
 
   CGSolver pcg;
   pcg.SetPrintLevel(print_iter);
   pcg.SetMaxIter(max_num_iter);
   pcg.SetRelTol(sqrt(RTOLERANCE));
   pcg.SetAbsTol(sqrt(ATOLERANCE));
   pcg.SetOperator(A);
   pcg.SetPreconditioner(B);

   tic_toc.Clear();
   tic_toc.Start();
   
   pcg.Mult(b, x);
  
   tic_toc.Stop();
   it_time = tic_toc.RealTime();

   return pcg.GetNumIterations();
}

int main(int argc, char *argv[])
{
   // 1. Parse command-line options.
   const char *mesh_file = "../../data/beam-hex.mesh";
   int order = 2;
   const char *basis_type = "G"; // Gauss-Lobatto
   bool static_cond = false;
   bool pa = true;
   const char *device_config = "cpu";
   bool visualization = true;
   const char *pc_type = "gko:bj";
   const char *pc_storage_opt = "auto";
   double pc_acc = 1.e-1;
   int pc_max_bs = 32;
   bool output_mesh = false;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh", "Mesh file to use.");
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree) or -1 for"
                  " isoparametric space.");
   args.AddOption(&static_cond, "-sc", "--static-condensation", "-no-sc",
                  "--no-static-condensation", "Enable static condensation.");
   args.AddOption(&pa, "-pa", "--partial-assembly", "-no-pa",
                  "--no-partial-assembly", "Enable Partial Assembly.");
   args.AddOption(&device_config, "-d", "--device",
                  "Device configuration string, see Device::Configure().");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&pc_type, "-pc-type", "--preconditioner-type",
                  "Type of preconditioner used on LOR matrix.");
   args.AddOption(&pc_storage_opt, "-pc-so",
                  "--preconditioner-storage-optimization",
                  "Type of precision storage optimization to use for Ginkgo BlockJacobi.");
   args.AddOption(&pc_acc, "-pc-acc", "--preconditioner-accuracy",
                  "Accuracy parameter for Ginkgo BlockJacobi.");
   args.AddOption(&pc_max_bs, "-pc-mbs", "--preconditioner-max-block-size",
                  "Maximum block size for Ginkgo BlockJacobi.");
   args.Parse();
   if (!args.Good())
   {
      args.PrintUsage(cout);
      return 1;
   }
   args.PrintOptions(cout);

   enum PCType { NONE, GKO_BLOCK_JACOBI, GKO_ILU, MFEM_GS };
   PCType pc_choice;
   bool pc = true;
   if (!strcmp(pc_type, "gko:bj")) { pc_choice = GKO_BLOCK_JACOBI; }
   else if (!strcmp(pc_type, "gko:ilu")) { pc_choice = GKO_ILU; }
   else if (!strcmp(pc_type, "mfem:gs")) { pc_choice = MFEM_GS; }
   else if (!strcmp(pc_type, "none"))
   {
      pc_choice = NONE;
      pc = false;
   }
   else
   {
      mfem_error("Invalid Preconditioner specified");
      return 3;
   }

   // ---------------------------------------------------------------
   // -------------------- Start Ginkgo set-up ----------------------

   // Create Ginkgo executor.

   // This will point to the selected executor default executor
   std::shared_ptr<gko::Executor> executor;

   // We will always need an OpenMP executor.
   auto omp_executor = gko::OmpExecutor::create();

   // If the user has requested to use CUDA, then build a
   // CudaExecutor and set `executor` to it; otherwise,
   // use the OmpExecutor
   if (!strcmp(device_config, "cuda"))
   {
      auto cuda_executor =
         gko::CudaExecutor::create(0, gko::OmpExecutor::create());
      executor = cuda_executor;
   }
   else
   {
      executor = omp_executor;
   }

   // --------------------- End Ginkgo set-up -----------------------
   // ---------------------------------------------------------------

   // See class BasisType in fem/fe_coll.hpp for available basis types
   int basis = BasisType::GetType(basis_type[0]);
   cout << "Using " << BasisType::Name(basis) << " basis ..." << endl;

   // 2. Enable hardware devices such as GPUs, and programming models such as
   //    CUDA, OCCA, RAJA and OpenMP based on command line options.
   Device device(device_config);
   device.Print();

   // 3. Read the mesh from the given mesh file. We can handle triangular,
   //    quadrilateral, tetrahedral, hexahedral, surface and volume meshes with
   //    the same code.
   Mesh *mesh = new Mesh(mesh_file, 1, 1);
   int dim = mesh->Dimension();

   // 4. Refine the mesh to increase the resolution. In this example we do
   //    'ref_levels' of uniform refinement. We choose 'ref_levels' to be the
   //    largest number that gives a final mesh with no more than 20,000
   //    elements.
   {
      int ref_levels =
         (int)floor(log(20000. / mesh->GetNE()) / log(2.) / dim);
      for (int l = 0; l < ref_levels; l++)
      {
         mesh->UniformRefinement();
      }
   }

   // 5. Define a finite element space on the mesh. Here we use continuous
   //    Lagrange finite elements of the specified order. If order < 1, we
   //    instead use an isoparametric/isogeometric space.
   FiniteElementCollection *fec;
   if (order > 0)
   {
      fec = new H1_FECollection(order, dim, basis);
   }
   else if (mesh->GetNodes())
   {
      fec = mesh->GetNodes()->OwnFEC();
      cout << "Using isoparametric FEs: " << fec->Name() << endl;
   }
   else
   {
      fec = new H1_FECollection(order = 1, dim, basis);
   }
   FiniteElementSpace *fespace = new FiniteElementSpace(mesh, fec);
   cout << "Number of finite element unknowns: " << fespace->GetTrueVSize()
        << endl;

   // Create the LOR mesh and finite element space. In the settings of this
   // example, we can transfer between HO and LOR with the identity operator.
   Mesh *mesh_lor = NULL;
   FiniteElementCollection *fec_lor = NULL;
   FiniteElementSpace *fespace_lor = NULL;
   if (pc)
   {
      int basis_lor = basis;
      if (basis == BasisType::Positive) { basis_lor=BasisType::ClosedUniform; }
      mesh_lor = new Mesh(mesh, order, basis_lor);
      fec_lor = new H1_FECollection(1, dim);
      fespace_lor = new FiniteElementSpace(mesh_lor, fec_lor);
   }

   // 6. Determine the list of true (i.e. conforming) essential boundary dofs.
   //    In this example, the boundary conditions are defined by marking all
   //    the boundary attributes from the mesh as essential (Dirichlet) and
   //    converting them to a list of true dofs.
   Array<int> ess_tdof_list;
   if (mesh->bdr_attributes.Size())
   {
      Array<int> ess_bdr(mesh->bdr_attributes.Max());
      ess_bdr = 1;
      fespace->GetEssentialTrueDofs(ess_bdr, ess_tdof_list);
   }

   // 7. Set up the linear form b(.) which corresponds to the right-hand side
   // of
   //    the FEM linear system, which in this case is (1,phi_i) where phi_i are
   //    the basis functions in the finite element fespace.
   LinearForm *b = new LinearForm(fespace);

   ConstantCoefficient one(1.0);
   b->AddDomainIntegrator(new DomainLFIntegrator(one));
   b->Assemble();

   // 8. Define the solution vector x as a finite element grid function
   //    corresponding to fespace. Initialize x with initial guess of zero,
   //    which satisfies the boundary conditions.
   GridFunction x(fespace);
   x = 0.0;

   // 9. Set up the bilinear form a(.,.) on the finite element space
   //    corresponding to the Laplacian operator -Delta, by adding the
   //    Diffusion domain integrator.
   BilinearForm *a = new BilinearForm(fespace);
   BilinearForm *a_pc = NULL;
   if (pc) { a_pc = new BilinearForm(fespace_lor); }
   if (pa)
   {
      a->SetAssemblyLevel(AssemblyLevel::PARTIAL);
   }
   a->AddDomainIntegrator(new DiffusionIntegrator(one));

   // 10. Assemble the bilinear form and the corresponding linear system,
   //     applying any necessary transformations such as: eliminating boundary
   //     conditions, applying conforming constraints for non-conforming AMR,
   //     static condensation, etc.
   if (static_cond)
   {
      a->EnableStaticCondensation();
   }
   a->Assemble();

   OperatorPtr A;
   Vector B, X;

   a->FormLinearSystem(ess_tdof_list, x, *b, A, X, B);

   // 11. Solve the linear system A X = B.
   double it_time = 0.;
   int total_its = 0;

   SparseMatrix A_pc;
   if (pc)
   {

      tic_toc.Clear();
      tic_toc.Start();

      a_pc->AddDomainIntegrator(new DiffusionIntegrator(one));
      a_pc->UsePrecomputedSparsity();
      a_pc->Assemble();
      a_pc->FormSystemMatrix(ess_tdof_list, A_pc);

      tic_toc.Stop();
      std::cout << "Real time creating A_pc SparseMatrix: " <<
                 tic_toc.RealTime() << std::endl;

      if (pc_choice == GKO_BLOCK_JACOBI)
      {

         // Create Ginkgo Jacobi preconditioner

         tic_toc.Clear();
         tic_toc.Start();

         GinkgoWrappers::GinkgoJacobiPreconditioner M(executor, A_pc, pc_storage_opt,
                                                      pc_acc, pc_max_bs);

         tic_toc.Stop();
         std::cout << "Real time creating Ginkgo BlockJacobi preconditioner: " <<
                   tic_toc.RealTime() << std::endl;

         // Use preconditioned CG
         total_its = pcg_solve(*A, M, B, X, 0, X.Size(), 1e-12, 0.0, it_time);

         std::cout << "Real time in PCG: " << it_time << std::endl;
      }
      else if (pc_choice == GKO_ILU)
      {

         // Create Ginkgo ILU preconditioner
         tic_toc.Clear();
         tic_toc.Start();

         GinkgoWrappers::GinkgoIluPreconditioner M(executor, A_pc);

         tic_toc.Stop();
         std::cout << "Real time creating Ginkgo Ilu preconditioner: " <<
                   tic_toc.RealTime() << std::endl;

         // Use preconditioned CG
         total_its = pcg_solve(*A, M, B, X, 0, X.Size(), 1e-12, 0.0, it_time);

         std::cout << "Real time in PCG: " << it_time << std::endl;

      }
      else if (pc_choice == MFEM_GS)
      {

         // Create MFEM preconditioner
         tic_toc.Clear();
         tic_toc.Start();

         GSSmoother M(A_pc);

         tic_toc.Stop();
         std::cout << "Real time creating MFEM GS preconditioner: " <<
                   tic_toc.RealTime() << std::endl;

         // Use preconditioned CG
         total_its = pcg_solve(*A, M, B, X, 0, X.Size(), 1e-12, 0.0, it_time);

         std::cout << "Real time in PCG: " << it_time << std::endl;

      }
   }
   else
   {

      total_its = cg_solve(*A, B, X, 0, X.Size(), 1e-12, 0.0, it_time);

      std::cout << "Real time in CG: " << it_time << std::endl;
   }

   std::cout << "Total iterations: " << total_its << std::endl;
   std::cout << "Avg time per iteration: " << it_time/double(total_its) << std::endl;

   // 12. Recover the solution as a finite element grid function.
   a->RecoverFEMSolution(X, *b, x);

   // 13. Save the refined mesh and the solution. This output can be viewed
   // later
   //     using GLVis: "glvis -m refined.mesh -g sol.gf".

   if (output_mesh) {
     ofstream mesh_ofs("refined.mesh");
     mesh_ofs.precision(8);
     mesh->Print(mesh_ofs);
     ofstream sol_ofs("sol.gf");
     sol_ofs.precision(8);
     x.Save(sol_ofs);

     if (pc)
     {
        ofstream mesh_lor_ofs("lor-refined.mesh");
        mesh_lor_ofs.precision(8);
        mesh_lor->Print(mesh_lor_ofs);


        ofstream apc_lor_ofs("lor-mat.dat");
        mesh_lor_ofs.precision(8);
        A_pc.PrintCSR(apc_lor_ofs);
     }
   }

   // 14. Send the solution by socket to a GLVis server.
   if (visualization)
   {
      char vishost[] = "localhost";
      int visport = 19916;
      socketstream sol_sock(vishost, visport);
      sol_sock.precision(8);
      sol_sock << "solution\n" << *mesh << x << flush;
   }

   // 15. Free the used memory.
   delete a;
   delete b;
   delete fespace;
   delete fespace_lor;
   delete fec_lor;
   delete mesh_lor;
   if (order > 0)
   {
      delete fec;
   }
   delete mesh;
}
