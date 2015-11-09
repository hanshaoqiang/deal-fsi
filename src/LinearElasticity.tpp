
/*
 * Author
 *   David Blom, TU Delft. All rights reserved.
 */

template <int dim>
LinearElasticity<dim>::LinearElasticity (
    double time_step,
    double theta,
    double degree,
    double gravity,
    double distributed_load,
    double rho,
    unsigned int n_global_refines
    ) :
    deg( degree ),
    n_global_refines( n_global_refines ),
    fe( FE_Q<dim>(degree), dim ),
    dof_handler( triangulation ),
    time_step( time_step ),
    theta( theta ),
    gravity( gravity ),
    distributed_load( distributed_load ),
    init( false ),
    initial_time( 0 ),
    rho( rho )
{
    assert( degree >= 1 );
    assert( time_step > 0 );
    assert( theta >= 0 && theta <= 1 );
    assert( rho > 0 );
}

template <int dim>
void LinearElasticity<dim>::setup_system()
{
    // GridGenerator::hyper_cube (triangulation, -1, 1);

    double x1 = 0.24899;
    double y1 = 0.21;
    double x2 = 0.6;
    double y2 = 0.19;

    Point<dim, double> point1( x1, y1 );
    Point<dim, double> point2( x2, y2 );

    std::vector<unsigned int> repetitions( dim );
    repetitions[0] = 35;
    repetitions[1] = 2;

    // GridGenerator::hyper_rectangle( triangulation, point1, point2 );
    GridGenerator::subdivided_hyper_rectangle( triangulation, repetitions, point1, point2, true );

    triangulation.refine_global( n_global_refines );

    std::cout << "Number of active cells: "
              << triangulation.n_active_cells()
              << std::endl;

    dof_handler.distribute_dofs( fe );

    std::cout << "Number of degrees of freedom: "
              << dof_handler.n_dofs()
              << std::endl
              << std::endl;

    DynamicSparsityPattern dsp( dof_handler.n_dofs(), dof_handler.n_dofs() );
    DoFTools::make_sparsity_pattern( dof_handler, dsp );
    sparsity_pattern.copy_from( dsp );

    mass_matrix.reinit( sparsity_pattern );
    laplace_matrix.reinit( sparsity_pattern );
    matrix_u.reinit( sparsity_pattern );
    matrix_v.reinit( sparsity_pattern );

    MatrixCreator::create_mass_matrix( dof_handler, QGauss<dim>( 3 ),
        mass_matrix );

    solution_u.reinit( dof_handler.n_dofs() );
    solution_v.reinit( dof_handler.n_dofs() );
    old_solution_u.reinit( dof_handler.n_dofs() );
    old_solution_v.reinit( dof_handler.n_dofs() );
    system_rhs.reinit( dof_handler.n_dofs() );
    body_force.reinit( dof_handler.n_dofs() );
    old_body_force.reinit( dof_handler.n_dofs() );

    constraints.close();

    assemble_system();
}

template <int dim>
void LinearElasticity<dim>::assemble_system()
{
    body_force.reinit( dof_handler.n_dofs() );
    laplace_matrix.reinit( sparsity_pattern );

    QGauss<dim>  quadrature_formula( deg + 1 );

    FEValues<dim> fe_values( fe, quadrature_formula,
        update_values | update_gradients |
        update_quadrature_points | update_JxW_values );

    QGauss<dim - 1>  quadrature_formula_face( deg + 1 );
    FEFaceValues<dim> fe_face_values( fe, quadrature_formula_face, update_values | update_quadrature_points | update_JxW_values );

    const unsigned int dofs_per_cell = fe.dofs_per_cell;
    const unsigned int n_q_points = quadrature_formula.size();

    FullMatrix<double>   cell_matrix( dofs_per_cell, dofs_per_cell );
    Vector<double> cell_rhs( dofs_per_cell );

    std::vector<types::global_dof_index> local_dof_indices( dofs_per_cell );

    unsigned int dofs_per_face = fe.n_dofs_per_face();

    std::vector<double>     lambda_values( n_q_points );
    std::vector<double>     mu_values( n_q_points );

    double nu = 0.4;
    double E = 1.4e6;
    double mu_s = E / ( 2.0 * (1.0 + nu) );
    double lambda_s = nu * E / ( (1.0 + nu) * (1.0 - 2.0 * nu) );

    ConstantFunction<dim> lambda( lambda_s ), mu( mu_s );

    // Now we can begin with the loop over all cells:
    typename DoFHandler<dim>::active_cell_iterator cell = dof_handler.begin_active(),
    endc = dof_handler.end();

    for (; cell != endc; ++cell )
    {
        cell_matrix = 0;
        cell_rhs = 0;

        fe_values.reinit( cell );

        lambda.value_list( fe_values.get_quadrature_points(), lambda_values );
        mu.value_list( fe_values.get_quadrature_points(), mu_values );

        for ( unsigned int i = 0; i < dofs_per_cell; ++i )
        {
            const unsigned int
                component_i = fe.system_to_component_index( i ).first;

            for ( unsigned int j = 0; j < dofs_per_cell; ++j )
            {
                const unsigned int
                    component_j = fe.system_to_component_index( j ).first;

                for ( unsigned int q_point = 0; q_point < n_q_points;
                    ++q_point )
                {
                    cell_matrix( i, j )
                        +=

                        (
                        (fe_values.shape_grad( i, q_point )[component_i] *
                            fe_values.shape_grad( j, q_point )[component_j] *
                            lambda_values[q_point])
                        +
                        (fe_values.shape_grad( i, q_point )[component_j] *
                            fe_values.shape_grad( j, q_point )[component_i] *
                            mu_values[q_point])
                        +

                        ( (component_i == component_j) ?
                            (fe_values.shape_grad( i, q_point ) *
                                fe_values.shape_grad( j, q_point ) *
                                mu_values[q_point])  :
                            0 )
                        )
                        *
                        fe_values.JxW( q_point );
                }
            }
        }

        for ( unsigned int i = 0; i < dofs_per_cell; ++i )
        {
            const unsigned int component_i = fe.system_to_component_index( i ).first;

            for ( unsigned int face = 0; face < GeometryInfo<dim>::faces_per_cell; ++face )
            {
                if ( cell->face( face )->at_boundary() == true
                    && cell->face( face )->boundary_id() == 3 )
                {
                    fe_face_values.reinit( cell, face );

                    std::vector<unsigned int> local_face_dof_indices( dofs_per_face );
                    cell->face( face )->get_dof_indices( local_face_dof_indices );

                    assert( dofs_per_face == fe_face_values.n_quadrature_points * dim );

                    for ( unsigned int q = 0; q < fe_face_values.n_quadrature_points; ++q )
                        cell_rhs( i ) += -get_traction( component_i, local_face_dof_indices[q * dim + component_i] ) * fe_face_values.shape_value( i, q ) * fe_face_values.JxW( q );
                }
            }
        }

        cell->get_dof_indices( local_dof_indices );

        for ( unsigned int i = 0; i < dofs_per_cell; ++i )
        {
            for ( unsigned int j = 0; j < dofs_per_cell; ++j )
                laplace_matrix.add( local_dof_indices[i],
                    local_dof_indices[j],
                    cell_matrix( i, j ) );

            body_force( local_dof_indices[i] ) += cell_rhs( i );
        }
    }

    constraints.condense( laplace_matrix );
}

template <int dim>
void LinearElasticity<dim>::initTimeStep()
{
    assert( !init );

    std::cout << "Time step " << timestep_number
              << " at t=" << time
              << std::endl;

    init = true;
}

template <int dim>
void LinearElasticity<dim>::finalizeTimeStep()
{
    assert( init );

    output_results();

    old_solution_u = solution_u;
    old_solution_v = solution_v;
    old_body_force = body_force;

    timestep_number++;
    time = initial_time + timestep_number * time_step;

    init = false;
}

template <int dim>
bool LinearElasticity<dim>::isRunning()
{
    double final_time = 0.05;

    return time <= final_time;
}

template <int dim>
void LinearElasticity<dim>::solve_u()
{
    SolverControl solver_control( 1000, 1e-12 * system_rhs.l2_norm() );
    SolverCG<>              cg( solver_control );

    SparseDirectUMFPACK A_direct;
    A_direct.initialize( matrix_u );

    // cg.solve (matrix_u, solution_u, system_rhs,
    // preconditioner);

    A_direct.vmult( solution_u, system_rhs );

    std::cout << "   u-equation: " << solver_control.last_step()
              << " CG iterations."
              << std::endl;
}

template <int dim>
void LinearElasticity<dim>::solve_v()
{
    SolverControl solver_control( 1000, 1e-12 * system_rhs.l2_norm() );
    SolverCG<>              cg( solver_control );

    SparseDirectUMFPACK A_direct;
    A_direct.initialize( matrix_v );

    // cg.solve (matrix_v, solution_v, system_rhs,
    // preconditioner);

    A_direct.vmult( solution_v, system_rhs );

    std::cout << "   v-equation: " << solver_control.last_step()
              << " CG iterations."
              << std::endl;
}

template <int dim>
void LinearElasticity<dim>::output_results() const
{
    return;
    DataOut<dim> data_out;

    std::vector<DataComponentInterpretation::DataComponentInterpretation>
    data_component_interpretation( dim,
        DataComponentInterpretation::component_is_part_of_vector );
    std::vector<std::string> solution_name_u( dim, "displacement" );
    std::vector<std::string> solution_name_v( dim, "velocity" );

    data_out.attach_dof_handler( dof_handler );

    data_out.add_data_vector( solution_u,
        solution_name_u,
        DataOut<dim>::type_dof_data,
        data_component_interpretation );
    data_out.add_data_vector( solution_v,
        solution_name_v,
        DataOut<dim>::type_dof_data,
        data_component_interpretation );

    data_out.build_patches();

    const std::string filename = "solution-" +
        Utilities::int_to_string( timestep_number, 3 ) +
        ".vtk";
    std::ofstream output( filename.c_str() );
    data_out.write_vtk( output );
}

template <int dim>
void LinearElasticity<dim>::run()
{
    setup_system();

    initial_time = 0;

    timestep_number = 0;

    output_results();

    timestep_number = 1;
    time = initial_time + time_step;

    while ( isRunning() )
    {
        initTimeStep();

        solve();

        finalizeTimeStep();
    }

    timestep_number--;
    time = initial_time + timestep_number * time_step;
}

template <int dim>
void LinearElasticity<dim>::solve()
{
    Vector<double> tmp( solution_u.size() );
    Vector<double> forcing_terms( solution_u.size() );

    assemble_system();

    // mass_matrix.vmult (system_rhs, old_solution_u);
    system_rhs = mass_matrix * old_solution_u;

    // mass_matrix.vmult (tmp, old_solution_v);
    tmp = mass_matrix * old_solution_v;
    system_rhs.add( time_step, tmp );

    // laplace_matrix.vmult (tmp, old_solution_u);
    tmp = laplace_matrix * old_solution_u;
    system_rhs.add( -theta * (1 - theta) * time_step * time_step / rho, tmp );

    RightHandSide<dim> rhs_function( gravity );
    rhs_function.set_time( time );
    VectorTools::create_right_hand_side( dof_handler, QGauss<dim>( 2 ),
        rhs_function, tmp );
    tmp += body_force;
    forcing_terms = tmp;
    forcing_terms *= theta * time_step;

    rhs_function.set_time( time - time_step );
    VectorTools::create_right_hand_side( dof_handler, QGauss<dim>( 2 ),
        rhs_function, tmp );

    tmp += old_body_force;
    forcing_terms.add( (1 - theta) * time_step, tmp );
    forcing_terms *= 1.0 / rho;

    system_rhs.add( theta * time_step, forcing_terms );

    {
        std::map<types::global_dof_index, double> boundary_values;
        VectorTools::interpolate_boundary_values( dof_handler,
            0,
            ZeroFunction<dim>( dim ),
            boundary_values );

        matrix_u.copy_from( mass_matrix );
        matrix_u.add( theta * theta * time_step * time_step / rho, laplace_matrix );
        MatrixTools::apply_boundary_values( boundary_values,
            matrix_u,
            solution_u,
            system_rhs );
    }
    solve_u();

    // laplace_matrix.vmult (system_rhs, solution_u);
    system_rhs = laplace_matrix * solution_u;
    system_rhs *= -theta * time_step / rho;

    // mass_matrix.vmult (tmp, old_solution_v);
    tmp = mass_matrix * old_solution_v;
    system_rhs += tmp;

    // laplace_matrix.vmult (tmp, old_solution_u);
    tmp = laplace_matrix * old_solution_u;
    system_rhs.add( -time_step * (1 - theta) / rho, tmp );

    system_rhs += forcing_terms;

    {
        std::map<types::global_dof_index, double> boundary_values;
        VectorTools::interpolate_boundary_values( dof_handler,
            0,
            ZeroFunction<dim>( dim ),
            boundary_values );
        matrix_v.copy_from( mass_matrix );
        MatrixTools::apply_boundary_values( boundary_values,
            matrix_v,
            solution_v,
            system_rhs );
    }
    solve_v();
}

template <int dim>
unsigned int LinearElasticity<dim>::n_dofs() const
{
    return dof_handler.n_dofs();
}

template <int dim>
double LinearElasticity<dim>::point_value() const
{
    Point<dim> point( 0.6, 0.2 );

    Vector<double> vector_value( dim );

    VectorTools::point_value( dof_handler,
        solution_u,
        point,
        vector_value
        );

    std::cout << "   Point value = " << vector_value[1] << std::endl;

    return vector_value[1];
}

template <int dim>
double LinearElasticity<dim>::get_traction(
    const unsigned int component_i,
    const unsigned int
    )
{
    // return 0;
    double t = time;
    double T = 0.01;
    double offset = 0.01;
    double value = distributed_load;

    if ( t - offset < T )
        value *= 0.5 - 0.5 * std::cos( M_PI * (t - offset) / T );

    if ( t < offset )
        value = 0.0;

    if ( component_i == 1 )
        return value;

    return 0.0;
}

template <class Scalar>
Vector<Scalar> operator *(
    SparseMatrix<Scalar> & A,
    Vector<Scalar> & b
    )
{
    Vector<Scalar> tmp( b.size() );
    A.vmult( tmp, b );
    return tmp;
}
