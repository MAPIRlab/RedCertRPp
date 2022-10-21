#include "Essential.h"


using namespace std::chrono;
using namespace Optimization;
using namespace Optimization::Convex;

namespace Essential{
    EssentialEstimationResult EssentialClass::getResults(void)
    {

               Matrix9 C;

               // output
               EssentialEstimationResult result;


               Matrix3 E_8pts;
               Matrix3 matrix_precon;


                //////////////////////////////////////////////////
                if (options_.estimation_verbose)
                    std::cout << " Constructing data matrix C.\n";
                // Get starting timepoint
                auto start_time_init = high_resolution_clock::now();
                // create data matrix
                C = constructDataMatrix(points_);

                auto duration_construction_C = duration_cast<microseconds>(high_resolution_clock::now() - start_time_init);

                auto start_time_bounds = high_resolution_clock::now();
                //////////////////////////////////////////////////


                // Compute and cache preconditioning matrices
                // Just compute this if required
                if ((options_.use_preconditioning != Preconditioner::Any))
                        E_8pts = initialize8pts(C, matrix_precon, options_.use_preconditioning);

                auto duration_8pts = duration_cast<microseconds>(high_resolution_clock::now() - start_time_bounds);
                //////////////////////////////////////////////////


               // Update initial guess
               Matrix3 E_initial;

               Matrix3 R_s;
               Vector3 t_s;

             
               E_initial = projectToEssentialManifold(E_initial_);
               computeRtfromE(points_, E_initial, R_s, t_s);

              

        
               if (options_.estimation_verbose)
               {
                std::cout << "Initial guess rotation:\n" << R_s << std::endl;
                std::cout << "Initial guess translation Tgt:\n" << t_s << std::endl;
               }
               



               Matrix34 Rt;
               Rt.block<3, 3>(0, 0) = R_s;
               Rt.block<3, 1>(0, 3) = t_s;

               // Show initial objective
               double f_initial = 0.5 * vec(E_initial).transpose() * C * vec(E_initial);

               if (options_.estimation_verbose)
                   std::cout << "Initial objective value f_{init} = " << f_initial << std::endl;




               /// Define the problem
               EssentialProblem problem(C, options_.use_preconditioning);
               problem.setPointCorrespondences(points_);
               // compute precon if needed
               // set precon matrix
               if (options_.use_preconditioning != Preconditioner::None)
                 problem.setMatrixPrecon(matrix_precon);

                // Cache these three matrices for gradient & hessian
                ProblemCachedMatrices problem_matrices;
 
                /// Function handles required by the TNT optimization algorithm
                // Preconditioning operator (optional)
                  std::experimental::optional<Optimization::Riemannian::LinearOperator<Matrix34, Matrix34, ProblemCachedMatrices>> precon;

                  if (options_.use_preconditioning == Preconditioner::None)
                    {
                        precon = std::experimental::nullopt;
                    }
                  else {
                    Optimization::Riemannian::LinearOperator<Matrix34, Matrix34, ProblemCachedMatrices> precon_op =
                        [&problem](const Matrix34 &Y, const Matrix34 &Ydot,
                                   const ProblemCachedMatrices & problem_matrices) {
                          return problem.precondition(Y, Ydot);
                        };
                    precon = precon_op;
                  }



              // Objective
              Optimization::Objective<Matrix34, double, ProblemCachedMatrices> F =
                  [&problem](const Matrix34 &Y, ProblemCachedMatrices & problem_matrices){
                    return problem.evaluate_objective(Y, problem_matrices);
                  };


             /// Gradient
              Optimization::Riemannian::VectorField<Matrix34, Matrix34, ProblemCachedMatrices> grad_F = [&problem](const Matrix34 &Y,
                                                ProblemCachedMatrices & problem_matrices) {
                // Compute and cache Euclidean gradient at the current iterate
                problem_matrices.NablaF_Y = problem.Euclidean_gradient(Y, problem_matrices);
       
                // Compute Riemannian gradient from Euclidean one
                return problem.Riemannian_gradient(Y, problem_matrices.NablaF_Y);
              };
              

              // Local quadratic model constructor
              Optimization::Riemannian::QuadraticModel<Matrix34, Matrix34, ProblemCachedMatrices> QM =
                  [&problem](
                      const Matrix34 &Y, Matrix34 &grad,
                      Optimization::Riemannian::LinearOperator<Matrix34, Matrix34, ProblemCachedMatrices> &HessOp,
                      ProblemCachedMatrices & problem_matrices) {
                    // Compute and cache Euclidean gradient at the current iterate
                    problem_matrices.NablaF_Y  = problem.Euclidean_gradient(Y, problem_matrices);
                    // Compute Riemannian gradient from Euclidean gradient
                    grad = problem.Riemannian_gradient(Y, problem_matrices.NablaF_Y );


                    // Define linear operator for computing Riemannian Hessian-vector
                    // products
                    HessOp = [&problem](const Matrix34 &Y, const Matrix34 &Ydot,
                                        const ProblemCachedMatrices & problem_matrices) {
                                        Matrix34 Hss = problem.Riemannian_Hessian_vector_product(Y, problem_matrices, Ydot);

                                        return Hss;
                    };
                  };

                  // Riemannian metric

                  // We consider a realization of the product of Stiefel manifolds as an
                  // embedded submanifold of R^{r x dn}; consequently, the induced Riemannian
                  // metric is simply the usual Euclidean inner product
                  Optimization::Riemannian::RiemannianMetric<Matrix34, Matrix34, double, ProblemCachedMatrices>
                      metric = [&problem](const Matrix34 &Y, const Matrix34 &V1, const Matrix34 &V2,
                                          const ProblemCachedMatrices & problem_matrices) {

                        Matrix3 R1, R2;
                        Vector3 t1, t2;
                        R1 = V1.block<3,3>(0,0);
                        R2 = V2.block<3,3>(0,0);

                        t1 = V1.block<3,1>(0,3);
                        t2 = V2.block<3,1>(0,3);

                        return ((R1 * R2.transpose()).trace() + t1.dot(t2));
                      };

              // Retraction operator
              Optimization::Riemannian::Retraction<Matrix34, Matrix34, ProblemCachedMatrices> retraction =
                  [&problem](const Matrix34 &Y, const Matrix34 &Ydot, const ProblemCachedMatrices & problem_matrices) {
                    return problem.retract(Y, Ydot);
                  };





              // Stop timer
              auto stop_init = high_resolution_clock::now();

              auto duration_init = duration_cast<microseconds>( stop_init- start_time_init);


              // set up params for solver
              Optimization::Riemannian::TNTParams<double> params;
              params.gradient_tolerance = options_.tol_grad_norm;
              params.relative_decrease_tolerance = options_.rel_func_decrease_tol;
              params.max_iterations = options_.max_RTR_iterations;
              params.max_TPCG_iterations = options_.max_tCG_iterations;
              params.preconditioned_gradient_tolerance = options_.preconditioned_grad_norm_tol;

              params.stepsize_tolerance = options_.stepsize_tol;


              // params.estimation_verbose = options.estimation_verbose;
              params.verbose = options_.verbose;


              /** An optional user-supplied function that can be used to instrument/monitor
               * the performance of the internal Riemannian truncated-Newton trust-region
               * optimization algorithm as it runs. */
              // std::experimental::optional<EssentialTNTUserFunction> user_fcn;

              /// Run optimization!
              auto start_opt = high_resolution_clock::now();

              Optimization::Riemannian::TNTResult<Matrix34, double> TNTResults = Optimization::Riemannian::TNT<Matrix34, Matrix34, double, ProblemCachedMatrices>(
                        F, QM, metric, retraction, Rt, problem_matrices, precon, params);
                 


               auto stop_opt = high_resolution_clock::now();
                auto duration_opt = duration_cast<microseconds>(stop_opt - start_opt);

               // Extract the results
               if (options_.estimation_verbose)
               {
                std::cout << "Final point SO(3) x S(2): " << TNTResults.x << std::endl;
                std::cout << "Final objective: " << TNTResults.f << std::endl;
               }

	           Matrix34 Rs_opt = TNTResults.x;
	           Matrix3 R_opt = Rs_opt.block<3,3>(0,0);
	           Vector3 t_opt = Rs_opt.block<3,1>(0,3);


	           Matrix3 E_opt = computeEfromRt(R_opt, t_opt);

	           double f_hat = TNTResults.f;
    
                    auto duration_total = duration_cast<microseconds>(high_resolution_clock::now() - start_time_init);



                        
	               //  assign all the results to this struct
	            
	               result.f_hat = f_hat;
	               result.gradnorm = problem.Riemannian_gradient(Rs_opt).norm();
                       result.R_opt = R_opt;
                       result.t_opt = t_opt;
                       result.E_opt = E_opt;
                       // Save time
                       result.elapsed_init_time = duration_init.count();
                       result.elapsed_C_time = duration_construction_C.count();  // in microsecs
                       result.elapsed_8pt_time = duration_8pts.count();  // in microsecs
                       result.elapsed_iterative_time = duration_opt.count();  // in microsecs
                       result.elapsed_estimation_time = duration_total.count();

               return result;

    }; // end of fcn getResult






void EssentialClass::printResult(EssentialEstimationResult & my_result)
{
    // print data

      std::cout << "Data from estimation\n--------------------\n";
      std::cout << "f_hat = " << my_result.f_hat << std::endl;


      std::cout << "######## Times [in microseconds]:\n";
      std::cout << "Data Matric C construction: " << my_result.elapsed_C_time << std::endl;
      std::cout << "-----\n Total init: " << my_result.elapsed_init_time << std::endl;
      std::cout << "Iterative Method: " << my_result.elapsed_iterative_time << std::endl;
      std::cout << "---------------------\n";
      std::cout << "Total time: " << my_result.elapsed_estimation_time << std::endl << std::endl;

      std::cout << "\n Recovered R:\n" << my_result.R_opt << std::endl;
      std::cout << "\n Recovered t:\n" << my_result.t_opt << std::endl;


};  //end of print fcn

}  // end of essential namespace
