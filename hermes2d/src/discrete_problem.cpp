// This file is part of Hermes2D.
//
// Hermes2D is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// Hermes2D is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Hermes2D.  If not, see <http://www.gnu.org/licenses/>.

#include "h2d_common.h"
#include "integrals/h1.h"
#include "quadrature/limit_order.h"
#include "discrete_problem.h"
#include "mesh/traverse.h"
#include "space/space.h"
#include "shapeset/precalc.h"
#include "matrix.h"
#include "solver/umfpack_solver.h"
#include "mesh/refmap.h"
#include "function/solution.h"
#include "config.h"
#include "neighbor.h"
#include "views/Scalar_view.h"
#include "views/base_view.h"
#include "boundaryconditions/essential_bcs.h"

template<typename Scalar>
DiscreteProblem<Scalar>::DiscreteProblem(WeakForm<Scalar>* wf, Hermes::vector<Space<Scalar>*> spaces) : wf(wf), wf_seq(-1), spaces(spaces)
{
  _F_;
  init();
}

template<typename Scalar>
DiscreteProblem<Scalar>::DiscreteProblem(WeakForm<Scalar>* wf, Space<Scalar>* space)
  : wf(wf), wf_seq(-1)
{
  _F_;
  spaces.push_back(space);
  init();
}

template<typename Scalar>
void DiscreteProblem<Scalar>::init()
{
  _F_;
  // Sanity checks.
  if(wf == NULL)
    error("WeakForm<Scalar>* wf can not be NULL in DiscreteProblem<Scalar>::DiscreteProblem.");

  if (spaces.size() != (unsigned) wf->get_neq()) error("Bad number of spaces in DiscreteProblem.");
  if (spaces.size() > 0) have_spaces = true;
  else error("Zero number of spaces in DiscreteProblem.");

  // Internal variables settings.
  sp_seq = new int[wf->get_neq()];
  memset(sp_seq, -1, sizeof(int) * wf->get_neq());

  // Matrix<Scalar> related settings.
  matrix_buffer = NULL;
  matrix_buffer_dim = 0;
  have_matrix = false;
  values_changed = true;
  struct_changed = true;

  // Initialize precalc shapesets according to spaces provided.
  pss = new PrecalcShapeset*[wf->get_neq()];
  for (unsigned int i = 0; i < wf->get_neq(); i++) pss[i] = NULL;
  num_user_pss = 0;
  for (unsigned int i = 0; i < wf->get_neq(); i++){
    Shapeset *shapeset = spaces[i]->get_shapeset();
    if (shapeset == NULL) error("Internal in DiscreteProblem<Scalar>::init_spaces().");
    PrecalcShapeset *p = new PrecalcShapeset(shapeset);
    if (p == NULL) error("New PrecalcShapeset could not be allocated in DiscreteProblem<Scalar>::init_spaces().");
    pss[i] = p;
    num_user_pss++;
  }

  // Create global enumeration of dof and fill the ndof variable.
  ndof = Space<Scalar>::assign_dofs(spaces);

  // Update the weak formulation with the user-supplied string markers
  // according to the conversion table contained in the mesh.
  element_markers_conversion = &spaces[0]->get_mesh()->element_markers_conversion;
  boundary_markers_conversion = &spaces[0]->get_mesh()->boundary_markers_conversion;
  wf->set_markers_conversion(&spaces[0]->get_mesh()->element_markers_conversion, 
    &spaces[0]->get_mesh()->boundary_markers_conversion);

  // There is a special function that sets a DiscreteProblem to be FVM.
  // Purpose is that this constructor looks cleaner and is simpler.
  this->is_fvm = false;

  vector_valued_forms = false;

  Geom<Ord> *tmp = init_geom_ord();
  geom_ord = *tmp;
  delete tmp;
}

template<typename Scalar>
DiscreteProblem<Scalar>::~DiscreteProblem()
{
  _F_;
  free();
  if (sp_seq != NULL) delete [] sp_seq;
  if (pss != NULL) {
    for(int i = 0; i < num_user_pss; i++)
      delete pss[i];
    delete [] pss;
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::free()
{
  _F_;
  struct_changed = values_changed = true;
  if (wf != NULL)
    memset(sp_seq, -1, sizeof(int) * wf->get_neq());
  wf_seq = -1;
}

template<typename Scalar>
int DiscreteProblem<Scalar>::get_num_dofs()
{
  _F_;
  ndof = 0;
  for (unsigned int i = 0; i < wf->get_neq(); i++)
    ndof += spaces[i]->get_num_dofs();
  return ndof;
}

template<typename Scalar>
Scalar** DiscreteProblem<Scalar>::get_matrix_buffer(int n)
{
  _F_;
  if (n <= matrix_buffer_dim) 
    return matrix_buffer;
  if (matrix_buffer != NULL) 
    delete [] matrix_buffer;
  matrix_buffer_dim = n;
  return (matrix_buffer = new_matrix<Scalar>(n, n));
}

template<typename Scalar>
bool DiscreteProblem<Scalar>::is_up_to_date()
{
  _F_;
  // check if we can reuse the matrix structure
  bool up_to_date = true;
  if (!have_matrix) up_to_date = false;

  for (unsigned int i = 0; i < wf->get_neq(); i++) {
    if (spaces[i]->get_seq() != sp_seq[i]) {
      up_to_date = false;
      break;
    }
  }

  if (wf->get_seq() != wf_seq)
    up_to_date = false;

  return up_to_date;
}

template<typename Scalar>
void DiscreteProblem<Scalar>::create_sparse_structure(SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, 
  bool force_diagonal_blocks, Table* block_weights)
{
  _F_;

  if (is_up_to_date())
  {
    if (mat != NULL)
    {
      verbose("Reusing matrix sparse structure.");
      mat->zero();
    }
    if (rhs != NULL) rhs->zero();
    return;
  }

  // For DG, the sparse structure is different as we have to 
  // account for over-edge calculations.
  bool is_DG = false;
  for(unsigned int i = 0; i < this->wf->mfsurf.size(); i++) {
    if(this->wf->mfsurf[i]->areas[0] == H2D_DG_INNER_EDGE) {
      is_DG = true;
      break;
    }
  }
  for(unsigned int i = 0; i < this->wf->vfsurf.size(); i++) {
    if(this->wf->vfsurf[i]->areas[0] == H2D_DG_INNER_EDGE) {
      is_DG = true;
      break;
    }
  }
  for(unsigned int i = 0; i < this->wf->mfsurf_mc.size(); i++) {
    if(this->wf->mfsurf_mc[i]->areas[0] == H2D_DG_INNER_EDGE) {
      is_DG = true;
      break;
    }
  }
  for(unsigned int i = 0; i < this->wf->vfsurf_mc.size(); i++) {
    if(this->wf->vfsurf_mc[i]->areas[0] == H2D_DG_INNER_EDGE) {
      is_DG = true;
      break;
    }
  }

  int ndof = get_num_dofs();

  if (mat != NULL)
  {
    // Spaces have changed: create the matrix from scratch.
    have_matrix = true;
    mat->free();
    mat->prealloc(ndof);

    AsmList<Scalar>* al = new AsmList<Scalar>[wf->get_neq()];
    Mesh** meshes = new Mesh*[wf->get_neq()];
    bool **blocks = wf->get_blocks(force_diagonal_blocks);

    // Init multi-mesh traversal.
    for (unsigned int i = 0; i < wf->get_neq(); i++) meshes[i] = spaces[i]->get_mesh();

    Traverse trav;
    trav.begin(wf->get_neq(), meshes);

    // Loop through all elements.
    Element **e;
    while ((e = trav.get_next_state(NULL, NULL)) != NULL) {
      // Obtain assembly lists for the element at all spaces.
      for (unsigned int i = 0; i < wf->get_neq(); i++) {
        // TODO: do not get the assembly list again if the element was not changed.
        if (e[i] != NULL) spaces[i]->get_element_assembly_list(e[i], &(al[i]));
      }

      if(is_DG) {
        // Number of edges (= number of vertices).
        int num_edges = e[0]->get_num_surf();

        // Allocation an array of arrays of neighboring elements for every mesh x edge.
        Element **** neighbor_elems_arrays = new Element *** [wf->get_neq()];
        for(unsigned int i = 0; i < wf->get_neq(); i++)
          neighbor_elems_arrays[i] = new Element ** [num_edges];

        // The same, only for number of elements
        int ** neighbor_elems_counts = new int * [wf->get_neq()];
        for(unsigned int i = 0; i < wf->get_neq(); i++)
          neighbor_elems_counts[i] = new int [num_edges];

        // Get the neighbors.
        for(unsigned int el = 0; el < wf->get_neq(); el++) {
          NeighborSearch<Scalar> ns(e[el], meshes[el]);

          // Ignoring errors (and doing nothing) in case the edge is a boundary one.
          ns.set_ignore_errors(true);

          for(int ed = 0; ed < num_edges; ed++) {
            ns.set_active_edge(ed);
            std::vector<Element *> *neighbors = ns.get_neighbors();

            neighbor_elems_counts[el][ed] = ns.get_num_neighbors();
            neighbor_elems_arrays[el][ed] = new Element * [neighbor_elems_counts[el][ed]];
            for(int neigh = 0; neigh < neighbor_elems_counts[el][ed]; neigh++)
              neighbor_elems_arrays[el][ed][neigh] = (*neighbors)[neigh];
          }
        }

        // Pre-add into the stiffness matrix.
        for (unsigned int m = 0; m < wf->get_neq(); m++) {
          for(unsigned int el = 0; el < wf->get_neq(); el++) {

            // Do not include blocks with zero weight except if
            // (force_diagonal_blocks == true && this is a diagonal block).
            bool is_diagonal_block = (m == el);
            if (is_diagonal_block == false || force_diagonal_blocks == false) {
              if (block_weights != NULL) {
                if (fabs(block_weights->get_A(m, el)) < 1e-12) continue;
              }
            }

            for(int ed = 0; ed < num_edges; ed++) {
              for(int neigh = 0; neigh < neighbor_elems_counts[el][ed]; neigh++) {
                if ((blocks[m][el] || blocks[el][m]) && e[m] != NULL)  {
                  AsmList<Scalar>*am = &(al[m]);
                  AsmList<Scalar>*an = new AsmList<Scalar>;
                  spaces[el]->get_element_assembly_list(neighbor_elems_arrays[el][ed][neigh], an);

                  // pretend assembling of the element stiffness matrix
                  // register nonzero elements
                  for (unsigned int i = 0; i < am->cnt; i++) {
                    if (am->dof[i] >= 0) {
                      for (unsigned int j = 0; j < an->cnt; j++) {
                        if (an->dof[j] >= 0) {
                          if(blocks[m][el]) mat->pre_add_ij(am->dof[i], an->dof[j]);
                          if(blocks[el][m]) mat->pre_add_ij(an->dof[j], am->dof[i]);
                        }
                      }
                    }
                  }
                  delete an;
                }
              }
            }
          }
        }

        // Deallocation an array of arrays of neighboring elements 
        // for every mesh x edge.
        for(unsigned int el = 0; el < wf->get_neq(); el++) {
          for(int ed = 0; ed < num_edges; ed++)
            delete [] neighbor_elems_arrays[el][ed];
          delete [] neighbor_elems_arrays[el];
        }
        delete [] neighbor_elems_arrays;

        // The same, only for number of elements.
        for(unsigned int el = 0; el < wf->get_neq(); el++)
          delete [] neighbor_elems_counts[el];
        delete [] neighbor_elems_counts;
      }

      // Go through all equation-blocks of the local stiffness matrix.
      for (unsigned int m = 0; m < wf->get_neq(); m++) {
        for (unsigned int n = 0; n < wf->get_neq(); n++) {

          // Do not include blocks with zero weight except if
          // (force_diagonal_blocks == true && this is a diagonal block).
          bool is_diagonal_block = (m == n);
          if (is_diagonal_block == false || force_diagonal_blocks == false) {
            if (block_weights != NULL) {
              if (fabs(block_weights->get_A(m, n)) < 1e-12) continue;
            }
          }

          if (blocks[m][n] && e[m] != NULL && e[n] != NULL) {
            AsmList<Scalar>*am = &(al[m]);
            AsmList<Scalar>*an = &(al[n]);

            // Pretend assembling of the element stiffness matrix.
            for (unsigned int i = 0; i < am->cnt; i++) {
              if (am->dof[i] >= 0) {
                for (unsigned int j = 0; j < an->cnt; j++) {
                  if (an->dof[j] >= 0) {
                    mat->pre_add_ij(am->dof[i], an->dof[j]);
                  }
                }
              }
            }
          }
        }
      }
    }

    trav.finish();
    delete [] al;
    delete [] meshes;
    delete [] blocks;

    mat->alloc();
  }

  // WARNING: unlike Matrix<Scalar>::alloc(), Vector<Scalar>::alloc(ndof) frees the memory occupied
  // by previous vector before allocating
  if (rhs != NULL) rhs->alloc(ndof);

  // save space seq numbers and weakform seq number, so we can detect their changes
  for (unsigned int i = 0; i < wf->get_neq(); i++)
    sp_seq[i] = spaces[i]->get_seq();

  wf_seq = wf->get_seq();

  struct_changed = true;
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble(SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs,
  bool force_diagonal_blocks, Table* block_weights)
{
  _F_;
  Scalar* coeff_vec = NULL;
  assemble(coeff_vec, mat, rhs, force_diagonal_blocks, block_weights);
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_sanity_checks(Table* block_weights) 
{
  _F_;
  // Check that spaces have been set either when constructing or by a call to set_spaces().
  if (!have_spaces) 
    error("You have to call DiscreteProblem<Scalar>::set_spaces() before calling assemble().");
  for (unsigned int i = 0; i < wf->get_neq(); i++)
    if (this->spaces[i] == NULL) error("A space is NULL in assemble().");

  // Check that the block scaling table have proper dimension.
  if (block_weights != NULL)
    if (block_weights->get_size() != wf->get_neq())
      error ("Bad dimension of block scaling table in DiscreteProblem<Scalar>::assemble().");
}

template<typename Scalar>
void DiscreteProblem<Scalar>::convert_coeff_vec(Scalar* coeff_vec, Hermes::vector<Solution<Scalar>*> & u_ext,
  bool add_dir_lift) 
{
  _F_;
  if (coeff_vec != NULL) {
    for (unsigned int i = 0; i < wf->get_neq(); i++) {
      Solution<Scalar>* external_solution_i = new Solution<Scalar>(spaces[i]->get_mesh());
      Solution<Scalar>::vector_to_solution(coeff_vec, spaces[i], external_solution_i, add_dir_lift);
      u_ext.push_back(external_solution_i);
    }
  }
  else 
    for (unsigned int i = 0; i < wf->get_neq(); i++)
      u_ext.push_back(new Solution<Scalar>(spaces[i]->get_mesh(), 0.0));
}

template<typename Scalar>
void DiscreteProblem<Scalar>::initialize_psss(Hermes::vector<PrecalcShapeset *>& spss) 
{
  _F_;
  for (unsigned int i = 0; i < wf->get_neq(); i++) {
    spss.push_back(new PrecalcShapeset(pss[i]));
    spss[i]->set_quad_2d(&g_quad_2d_std);
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::initialize_refmaps(Hermes::vector<RefMap *>& refmap) 
{
  _F_;
  for (unsigned int i = 0; i < wf->get_neq(); i++) {
    refmap.push_back(new RefMap());
    refmap[i]->set_quad_2d(&g_quad_2d_std);
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble(Scalar* coeff_vec, SparseMatrix<Scalar>* mat, 
  Vector<Scalar>* rhs,
  bool force_diagonal_blocks, bool add_dir_lift, 
  Table* block_weights)
{
  _F_;
  // Sanity checks.
  assemble_sanity_checks(block_weights);

  // Creating matrix sparse structure.
  create_sparse_structure(mat, rhs, force_diagonal_blocks, block_weights);

  // Convert the coefficient vector 'coeff_vec' into solutions Hermes::vector 'u_ext'.
  Hermes::vector<Solution<Scalar>*> u_ext = Hermes::vector<Solution<Scalar>*>();
  convert_coeff_vec(coeff_vec, u_ext, add_dir_lift);

  // Reset the warnings about insufficiently high integration order.
  reset_warn_order();

  // Create slave pss's, refmaps.
  Hermes::vector<PrecalcShapeset *> spss;
  Hermes::vector<RefMap *> refmap;

  // Initialize slave pss's, refmaps.
  initialize_psss(spss);
  initialize_refmaps(refmap);

  // Initialize matrix buffer.
  matrix_buffer = NULL;
  matrix_buffer_dim = 0;
  if (mat != NULL) 
    get_matrix_buffer(9);

  // Create assembling stages.
  std::vector<Stage<Scalar> > stages = std::vector<Stage<Scalar> >();
  bool want_matrix = (mat != NULL);
  bool want_vector = (rhs != NULL);
  wf->get_stages(spaces, u_ext, stages, want_matrix, want_vector);

  // Loop through all assembling stages -- the purpose of this is increased performance
  // in multi-mesh calculations, where, e.g., only the right hand side uses two meshes.
  // In such a case, the matrix forms are assembled over one mesh, and only the rhs
  // traverses through the union mesh. On the other hand, if you don't use multi-mesh
  // at all, there will always be only one stage in which all forms are assembled as usual.
  for (unsigned ss = 0; ss < stages.size(); ss++)
    // Assemble one stage. One stage is a collection of functions, 
    // and meshes that can not be further minimized.
    // E.g. if a linear form uses two external solutions, each of 
    // which is defined on a different mesh, and different to the
    // mesh of the current test function, then the stage would have 
    // three meshes. By stage functions, all functions are meant: shape 
    // functions (their precalculated values), and mesh functions.
    assemble_one_stage(stages[ss], mat, rhs, force_diagonal_blocks, 
    block_weights, spss, refmap, u_ext);

  // Deinitialize matrix buffer.
  if(matrix_buffer != NULL)
    delete [] matrix_buffer;
  matrix_buffer = NULL;
  matrix_buffer_dim = 0;

  // Deinitialize slave pss's, refmaps.
  for(std::vector<PrecalcShapeset *>::iterator it = spss.begin(); it != spss.end(); it++)
    delete *it;
  for(std::vector<RefMap *>::iterator it = refmap.begin(); it != refmap.end(); it++)
    delete *it;

  // Delete the vector u_ext.
  for(typename std::vector<Solution<Scalar>*>::iterator it = u_ext.begin(); it != u_ext.end(); it++)
    delete *it;
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_one_stage(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs,
  bool force_diagonal_blocks, Table* block_weights,
  Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, 
  Hermes::vector<Solution<Scalar>*>& u_ext)
{
  _F_;
  // Boundary flags. bnd[i] == true if i-th edge of the current 
  // element is a boundary edge.
  bool bnd[4];

  // Info about the boundary edge.
  SurfPos surf_pos[4];

  // Create the assembling states.
  Traverse trav;
  for (unsigned i = 0; i < stage.idx.size(); i++)
    stage.fns[i] = pss[stage.idx[i]];
  for (unsigned i = 0; i < stage.ext.size(); i++)
    stage.ext[i]->set_quad_2d(&g_quad_2d_std);
  trav.begin(stage.meshes.size(), &(stage.meshes.front()), &(stage.fns.front()));

  // Check that there is a DG form, so that the DG assembling procedure needs to be performed.
  DG_matrix_forms_present = false;
  DG_vector_forms_present = false;
  for(unsigned int i = 0; i < stage.mfsurf.size(); i++) {
    if (stage.mfsurf[i]->areas[0] == H2D_DG_INNER_EDGE) {
      DG_matrix_forms_present = true;
      break;
    }
  }
  for(unsigned int i = 0; i < stage.vfsurf.size(); i++) {
    if (stage.vfsurf[i]->areas[0] == H2D_DG_INNER_EDGE) {
      DG_vector_forms_present = true;
      break;
    }
  }
  for(unsigned int i = 0; i < stage.mfsurf_mc.size(); i++) {
    if (stage.mfsurf_mc[i]->areas[0] == H2D_DG_INNER_EDGE) {
      DG_matrix_forms_present = true;
      break;
    }
  }
  for(unsigned int i = 0; i < stage.vfsurf_mc.size(); i++) {
    if (stage.vfsurf_mc[i]->areas[0] == H2D_DG_INNER_EDGE) {
      DG_vector_forms_present = true;
      break;
    }
  }

  // Loop through all assembling states.
  // Assemble each one.
  Element** e;
  while ((e = trav.get_next_state(bnd, surf_pos)) != NULL)
    // One state is a collection of (virtual) elements sharing 
    // the same physical location on (possibly) different meshes.
    // This is then the same element of the virtual union mesh. 
    // The proper sub-element mappings to all the functions of
    // this stage is supplied by the function Traverse::get_next_state() 
    // called in the while loop.
    assemble_one_state(stage, mat, rhs, force_diagonal_blocks, 
    block_weights, spss, refmap, 
    u_ext, e, bnd, surf_pos, trav.get_base());

  if (mat != NULL) 
    mat->finish();
  if (rhs != NULL) 
    rhs->finish();
  trav.finish();

  if(DG_matrix_forms_present || DG_vector_forms_present) {
    Element* element_to_set_nonvisited;
    for(unsigned int mesh_i = 0; mesh_i < stage.meshes.size(); mesh_i++)
      for_all_elements(element_to_set_nonvisited, stage.meshes[mesh_i])
      element_to_set_nonvisited->visited = false;
  }
}

template<typename Scalar>
Element* DiscreteProblem<Scalar>::init_state(Stage<Scalar>& stage, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, Element** e, Hermes::vector<bool>& isempty, Hermes::vector<AsmList<Scalar>*>& al)
{
  _F_;
  // Find a non-NULL e[i].
  Element* e0 = NULL;
  for (unsigned int i = 0; i < stage.idx.size(); i++)
    if ((e0 = e[i]) != NULL) 
      break;
  if(e0 == NULL) 
    return NULL;

  // Set maximum integration order for use in integrals, see limit_order()
  update_limit_table(e0->get_mode());

  // Obtain assembly lists for the element at all spaces of the stage, set appropriate mode for each pss.
  // NOTE: Active elements and transformations for external functions (including the solutions from previous
  // Newton's iteration) as well as basis functions (master PrecalcShapesets) have already been set in
  // trav.get_next_state(...).
  for (unsigned int i = 0; i < stage.idx.size(); i++) {
    int j = stage.idx[i];
    if (e[i] == NULL) {
      isempty[j] = true;
      continue;
    }

    // TODO: do not obtain again if the element was not changed.
    spaces[j]->get_element_assembly_list(e[i], al[j]);

    // Set active element to all test functions.
    spss[j]->set_active_element(e[i]);
    spss[j]->set_master_transform();

    // Set active element to reference mappings.
    refmap[j]->set_active_element(e[i]);
    refmap[j]->force_transform(pss[j]->get_transform(), pss[j]->get_ctm());

    // Mark the active element on each mesh in order to prevent assembling on its edges from the other side.
    if(DG_matrix_forms_present || DG_vector_forms_present)
      e[i]->visited = true;
  }
  return e0;
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_one_state(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs,
  bool force_diagonal_blocks, Table* block_weights,
  Hermes::vector<PrecalcShapeset *>& spss, Hermes::vector<RefMap *>& refmap, 
  Hermes::vector<Solution<Scalar>*>& u_ext, Element** e, 
  bool* bnd, SurfPos* surf_pos, Element* trav_base)
{
  _F_;
  // Assembly list vector.
  Hermes::vector<AsmList<Scalar>*> al;
  for(unsigned int i = 0; i < wf->get_neq(); i++) 
    al.push_back(new AsmList<Scalar>);

  // Natural boundary condition flag.
  Hermes::vector<bool> nat;
  for(unsigned int i = 0; i < wf->get_neq(); i++) 
    nat.push_back(false);

  // Element usage flag: iempty[i] == true if the current state does not posses an active element in the i-th space.
  Hermes::vector<bool> isempty;
  for(unsigned int i = 0; i < wf->get_neq(); i++) 
    isempty.push_back(false);

  // Initialize the state, return a non-NULL element; if no such element found, return.
  Element* rep_element = init_state(stage, spss, refmap, e, isempty, al);
  if(rep_element == NULL)
    return;

  init_cache();

  /// Assemble volume matrix forms.
  assemble_volume_matrix_forms(stage, mat, rhs, force_diagonal_blocks, 
    block_weights, spss, refmap, u_ext, isempty, 
    rep_element->marker, al);
  if(!stage.mfvol_mc.empty())
    assemble_multicomponent_volume_matrix_forms(stage, mat, rhs, force_diagonal_blocks,
    block_weights, spss, refmap, u_ext, isempty,
    rep_element->marker, al);

  /// Assemble volume vector forms.
  if (rhs != NULL) {
    assemble_volume_vector_forms(stage, mat, rhs, force_diagonal_blocks,
      block_weights, spss, refmap, u_ext, isempty,
      rep_element->marker, al);
    if(!stage.vfvol_mc.empty()) {
      assemble_multicomponent_volume_vector_forms(stage, mat, rhs, force_diagonal_blocks,
        block_weights, spss, refmap, u_ext, isempty,
        rep_element->marker, al);
    }
  }

  // Assemble surface integrals now: loop through surfaces of the element.
  for (int isurf = 0; isurf < e[0]->get_num_surf(); isurf++) {
    assemble_surface_integrals(stage, mat, rhs, force_diagonal_blocks,
      block_weights, spss, refmap, u_ext, isempty,
      surf_pos[isurf].marker, al, bnd[isurf], surf_pos[isurf],
      nat, isurf, e, trav_base, rep_element);
  }

  // Delete assembly lists.
  for(unsigned int i = 0; i < wf->get_neq(); i++) 
    delete al[i];

  delete_cache();
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_volume_matrix_forms(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights,
  Hermes::vector<PrecalcShapeset *>& spss, Hermes::vector<RefMap *>& refmap, 
  Hermes::vector<Solution<Scalar>*>& u_ext, Hermes::vector<bool>& isempty, 
  int marker, Hermes::vector<AsmList<Scalar>*>& al)
{
  _F_;
  for (unsigned ww = 0; ww < stage.mfvol.size(); ww++) {
    MatrixFormVol<Scalar>* mfv = stage.mfvol[ww];
    int m = mfv->i;
    int n = mfv->j;
    if (isempty[m] || isempty[n]) continue;
    if (fabs(mfv->scaling_factor) < 1e-12) continue;

    // Assemble this form only if one of its areas is HERMES_ANY
    // of if the element marker coincides with one of the form's areas.
    bool assemble_this_form = false;
    for (unsigned int ss = 0; ss < mfv->areas.size(); ss++) {
      if ((mfv->areas[ss] == HERMES_ANY) || (marker == element_markers_conversion->get_internal_marker(mfv->areas[ss]))) {
        assemble_this_form = true;
        break;
      }
    }
    if (!assemble_this_form)
      continue;

    // If a block scaling table is provided, and if the scaling coefficient
    // A_mn for this block is zero, then the form does not need to be assembled.
    double block_scaling_coeff = 1.;
    if (block_weights != NULL) {
      block_scaling_coeff = block_weights->get_A(m, n);
      if (fabs(block_scaling_coeff) < 1e-12)
        continue;
    }
    bool tra = (m != n) && (mfv->sym != 0);
    bool sym = (m == n) && (mfv->sym == 1);

    // Assemble the local stiffness matrix for the form mfv.
    Scalar **local_stiffness_matrix = NULL;
    local_stiffness_matrix = get_matrix_buffer(std::max(al[m]->cnt, al[n]->cnt));

    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      if (!tra && al[m]->dof[i] < 0) continue;
      spss[m]->set_active_shape(al[m]->idx[i]);
      // Unsymmetric block.
      if (!sym) {
        for (unsigned int j = 0; j < al[n]->cnt; j++) {
          pss[n]->set_active_shape(al[n]->idx[j]);
          if (al[n]->dof[j] >= 0) {
            if (mat != NULL) {  
              Scalar val = 0;
              // Numerical integration performed only if all 
              // coefficients multiplying the form are nonzero.
              if (std::abs(al[m]->coef[i]) > 1e-12 && std::abs(al[n]->coef[j]) > 1e-12) {
                val = block_scaling_coeff * eval_form(mfv, u_ext, pss[n], spss[m], refmap[n],
                  refmap[m]) * al[n]->coef[j] * al[m]->coef[i];
              }
              local_stiffness_matrix[i][j] = val;
            }
          }
        }
      }
      // Symmetric block.
      else {
        for (unsigned int j = 0; j < al[n]->cnt; j++) {
          if (j < i && al[n]->dof[j] >= 0) continue;
          pss[n]->set_active_shape(al[n]->idx[j]);
          if (al[n]->dof[j] >= 0) { 
            if (mat != NULL) {

              Scalar val = 0;
              // Numerical integration performed only if all coefficients 
              // multiplying the form are nonzero.
              if (std::abs(al[m]->coef[i]) > 1e-12 && std::abs(al[n]->coef[j]) > 1e-12)
                val = block_scaling_coeff * eval_form(mfv, u_ext, pss[n], spss[m], refmap[n],
                refmap[m]) * al[n]->coef[j] * al[m]->coef[i];
              local_stiffness_matrix[i][j] = local_stiffness_matrix[j][i] = val;
            }
          }
        }
      }

      // Insert the local stiffness matrix into the global one.
      if (mat != NULL) {
        mat->add(al[m]->cnt, al[n]->cnt, local_stiffness_matrix, al[m]->dof, al[n]->dof);
      }

      // Insert also the off-diagonal (anti-)symmetric block, if required.
      if (tra) {
        if (mfv->sym < 0)
          chsgn(local_stiffness_matrix, al[m]->cnt, al[n]->cnt);

        transpose(local_stiffness_matrix, al[m]->cnt, al[n]->cnt);

        if (mat != NULL)
          mat->add(al[n]->cnt, al[m]->cnt, local_stiffness_matrix, al[n]->dof, al[m]->dof);
      }
    }
  }
}
template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_multicomponent_volume_matrix_forms(Stage<Scalar> & stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights,
  Hermes::vector<PrecalcShapeset *>& spss, Hermes::vector<RefMap *>& refmap, 
  Hermes::vector<Solution<Scalar>*>& u_ext, Hermes::vector<bool>& isempty, 
  int marker, Hermes::vector<AsmList<Scalar>*>& al)
{
  _F_;
  for (unsigned ww = 0; ww < stage.mfvol_mc.size(); ww++) {
    MultiComponentMatrixFormVol<Scalar>* mfv = stage.mfvol_mc[ww];
    if(fabs(mfv->scaling_factor) < 1e-12) continue;

    // Assemble this form only if one of its areas is HERMES_ANY
    // of if the element marker coincides with one of the form's areas.
    bool assemble_this_form = false;
    for (unsigned int ss = 0; ss < mfv->areas.size(); ss++) {
      if ((mfv->areas[ss] == HERMES_ANY) || 
        (marker == element_markers_conversion->get_internal_marker(mfv->areas[ss]))) {
          assemble_this_form = true;
          break;
      }
    }
    if (assemble_this_form == false) continue;

    // If a block scaling table is provided, and if the scaling coefficient
    // A_mn for this block is zero, then the form does not need to be assembled.
    Hermes::vector<double> block_scaling_coeffs;
    for(unsigned int coordinate_i = 0; coordinate_i < mfv->coordinates.size(); coordinate_i++)
      if (block_weights != NULL)
        block_scaling_coeffs.push_back(block_weights->get_A(mfv->coordinates[coordinate_i].first, 
        mfv->coordinates[coordinate_i].second));
      else
        block_scaling_coeffs.push_back(1);

    // Assemble the local stiffness matrix for the form mfv.
    unsigned int m = mfv->coordinates[0].first;
    unsigned int n = mfv->coordinates[0].second;

    if(mfv->sym)
      for(unsigned int coordinate_i = 0; coordinate_i < mfv->coordinates.size(); coordinate_i++)
        if(mfv->coordinates[coordinate_i].first != mfv->coordinates[coordinate_i].second)
          error("Symmetric multicomponent forms must take both the basis function and the test function from the same space.");

    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      spss[m]->set_active_shape(al[m]->idx[i]);
      if(al[m]->dof[i] < 0 && mfv->sym == HERMES_NONSYM)
        continue;
      if(mfv->sym == HERMES_NONSYM) {
        for (unsigned int j = 0; j < al[n]->cnt; j++) {
          pss[n]->set_active_shape(al[n]->idx[j]);
          if (al[n]->dof[j] >= 0) {
            if (mat != NULL) {
              if (std::abs(al[m]->coef[i]) > 1e-12 && std::abs(al[n]->coef[j]) > 1e-12) {
                Hermes::vector<Scalar> result;
                eval_form(mfv, u_ext, pss[n], spss[m], refmap[n], refmap[m], result);
                for(unsigned int coordinate_i = 0; coordinate_i < mfv->coordinates.size(); coordinate_i++) {
                  mat->add(al[mfv->coordinates[coordinate_i].first]->dof[i], al[mfv->coordinates[coordinate_i].second]->dof[j],
                    result[coordinate_i] * block_scaling_coeffs[coordinate_i] * al[n]->coef[j] * al[m]->coef[i]);
                }
              }
            }
          }
        }
      }
      else {
        for (unsigned int j = 0; j < al[n]->cnt; j++) {
          if(j < i && al[n]->dof[j] >= 0) continue;
          pss[n]->set_active_shape(al[n]->idx[j]);

          if (al[n]->dof[j] >= 0) {
            if (al[m]->dof[i] >= 0) {
              if (mat != NULL) {
                if (std::abs(al[m]->coef[i]) > 1e-12 && std::abs(al[n]->coef[j]) > 1e-12) {
                  Hermes::vector<Scalar> result;
                  eval_form(mfv, u_ext, pss[n], spss[m], refmap[n], refmap[m], result);
                  for(unsigned int coordinate_i = 0; coordinate_i < mfv->coordinates.size(); coordinate_i++) {
                    mat->add(al[mfv->coordinates[coordinate_i].first]->dof[i], 
                      al[mfv->coordinates[coordinate_i].second]->dof[j],
                      result[coordinate_i] * block_scaling_coeffs[coordinate_i] * al[n]->coef[j] * al[m]->coef[i]);
                    if(i != j)
                      mat->add(al[mfv->coordinates[coordinate_i].first]->dof[j], 
                      al[mfv->coordinates[coordinate_i].second]->dof[i],
                      result[coordinate_i] * block_scaling_coeffs[coordinate_i] * al[n]->coef[j] * al[m]->coef[i]);
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_volume_vector_forms(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al)
{
  _F_;

  if(rhs == NULL)
    return;
  for (unsigned int ww = 0; ww < stage.vfvol.size(); ww++) {
    VectorFormVol<Scalar>* vfv = stage.vfvol[ww];
    int m = vfv->i;
    if (isempty[vfv->i]) continue;
    if (fabs(vfv->scaling_factor) < 1e-12) continue;

    // Assemble this form only if one of its areas is HERMES_ANY
    // of if the element marker coincides with one of the form's areas.
    bool assemble_this_form = false;
    for (unsigned int ss = 0; ss < vfv->areas.size(); ss++) {
      if ((vfv->areas[ss] == HERMES_ANY) || 
        (marker == element_markers_conversion->get_internal_marker(vfv->areas[ss]))) {
          assemble_this_form = true;
          break;
      }
    }
    if (assemble_this_form == false) continue;

    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      if (al[m]->dof[i] < 0) continue;

      spss[m]->set_active_shape(al[m]->idx[i]);

      // Numerical integration performed only if the coefficient 
      // multiplying the form is nonzero.
      if (std::abs(al[m]->coef[i]) > 1e-12) {   
        rhs->add(al[m]->dof[i], eval_form(vfv, u_ext, spss[m], refmap[m]) * al[m]->coef[i]);
      }
    }
  }
}
template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_multicomponent_volume_vector_forms(Stage<Scalar> & stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al)
{
  _F_;

  if (rhs == NULL) return;

  for (unsigned int ww = 0; ww < stage.vfvol_mc.size(); ww++) {
    MultiComponentVectorFormVol<Scalar>* vfv = stage.vfvol_mc[ww];
    if (fabs(vfv->scaling_factor) < 1e-12) continue;

    // Assemble this form only if one of its areas is HERMES_ANY
    // of if the element marker coincides with one of the form's areas.
    bool assemble_this_form = false;
    for (unsigned int ss = 0; ss < vfv->areas.size(); ss++) {
      if ((vfv->areas[ss] == HERMES_ANY) || 
        (marker == element_markers_conversion->get_internal_marker(vfv->areas[ss]))) {
          assemble_this_form = true;
          break;
      }
    }
    if (assemble_this_form == false) continue; 

    unsigned int m = vfv->coordinates[0];

    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      if (al[m]->dof[i] < 0) continue;

      spss[m]->set_active_shape(al[m]->idx[i]);

      // Numerical integration performed only if the coefficient 
      // multiplying the form is nonzero.
      if (std::abs(al[m]->coef[i]) > 1e-12) {   
        Hermes::vector<Scalar> result;
        eval_form(vfv, u_ext, spss[m], refmap[m], result);
        for(unsigned int coordinate_i = 0; coordinate_i < vfv->coordinates.size(); coordinate_i++)
          rhs->add(al[vfv->coordinates[coordinate_i]]->dof[i], result[coordinate_i] 
        * al[vfv->coordinates[coordinate_i]]->coef[i]);
      }
    }
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_surface_integrals(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, 
  bool bnd, SurfPos& surf_pos, Hermes::vector<bool>& nat, int isurf, 
  Element** e, Element* trav_base, Element* rep_element)
{
  _F_;
  // Obtain the list of shape functions which are nonzero on this surface.
  for (unsigned int i = 0; i < stage.idx.size(); i++) {
    int j = stage.idx[i];
    if (isempty[j])
      continue;
    if(marker > 0) {
      nat[j] = true;
      if(spaces[j]->get_essential_bcs() != NULL)
        if(spaces[j]->get_essential_bcs()->get_boundary_condition(boundary_markers_conversion->get_user_marker(marker)) != NULL)
          nat[j] = false;
    }
    spaces[j]->get_boundary_assembly_list(e[i], isurf, al[j]);
  }

  // Assemble boundary edges: 
  if(bnd == 1) {
    assemble_surface_matrix_forms(stage, mat, rhs, force_diagonal_blocks, block_weights, 
      spss, refmap, u_ext, isempty, 
      marker, al, bnd, surf_pos, nat, isurf, e, trav_base);
    if(!stage.mfsurf_mc.empty())
      assemble_multicomponent_surface_matrix_forms(stage, mat, rhs, force_diagonal_blocks, block_weights, 
      spss, refmap, u_ext, isempty, 
      marker, al, bnd, surf_pos, nat, isurf, e, trav_base);
    if (rhs != NULL) {
      assemble_surface_vector_forms(stage, mat, rhs, force_diagonal_blocks, block_weights, 
        spss, refmap, u_ext, isempty, 
        marker, al, bnd, surf_pos, nat, isurf, e, trav_base);
      if(!stage.vfsurf_mc.empty())
        assemble_multicomponent_surface_vector_forms(stage, mat, rhs, force_diagonal_blocks, block_weights, 
        spss, refmap, u_ext, isempty, 
        marker, al, bnd, surf_pos, nat, isurf, e, trav_base);
    }
  }
  // Assemble inner edges (in discontinuous Galerkin discretization): 
  else
    if(DG_vector_forms_present || DG_matrix_forms_present)
      assemble_DG_forms(stage, mat, rhs, force_diagonal_blocks, block_weights, 
      spss, refmap, u_ext, isempty, 
      marker, al, bnd, surf_pos, nat, isurf, e, trav_base, rep_element);
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_DG_forms(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, 
  bool bnd, SurfPos& surf_pos, Hermes::vector<bool>& nat, 
  int isurf, Element** e, Element* trav_base, Element* rep_element)
{
  _F_;
  // Determine the minimum mesh seq in this stage.
  min_dg_mesh_seq = 0;
  for(unsigned int i = 0; i < stage.meshes.size(); i++)
    if(stage.meshes[i]->get_seq() < min_dg_mesh_seq || i == 0)
      min_dg_mesh_seq = stage.meshes[i]->get_seq();

  // Initialize the NeighborSearches.
  // 5 is for bits per page in the array.
  LightArray<NeighborSearch<Scalar>*> neighbor_searches(5);
  init_neighbors(neighbor_searches, stage, isurf);

  // Create a multimesh tree;
  NeighborNode* root = new NeighborNode(NULL, 0);
  build_multimesh_tree(root, neighbor_searches);

  // Update all NeighborSearches according to the multimesh tree.
  // After this, all NeighborSearches in neighbor_searches should have the same count 
  // of neighbors and proper set of transformations
  // for the central and the neighbor element(s) alike.
  // Also check that every NeighborSearch has the same number of neighbor elements.
  unsigned int num_neighbors = 0;
  for(unsigned int i = 0; i < neighbor_searches.get_size(); i++)
    if(neighbor_searches.present(i)) {
      NeighborSearch<Scalar>* ns = neighbor_searches.get(i);
      update_neighbor_search(ns, root);
      if(num_neighbors == 0)
        num_neighbors = ns->n_neighbors;
      if(ns->n_neighbors != num_neighbors)
        error("Num_neighbors of different NeighborSearches not matching in DiscreteProblem<Scalar>::assemble_surface_integrals().");
    }

    // Create neighbor psss, refmaps.
    std::map<unsigned int, PrecalcShapeset *> npss;
    std::map<unsigned int, PrecalcShapeset *> nspss;
    std::map<unsigned int, RefMap *> nrefmap;

    // Initialize neighbor precalc shapesets and refmaps.      
    // This is only needed when there are matrix DG forms present.
    if(DG_matrix_forms_present)
      for (unsigned int i = 0; i < stage.idx.size(); i++) {
        PrecalcShapeset* new_ps = new PrecalcShapeset(pss[i]->get_shapeset());
        new_ps->set_quad_2d(&g_quad_2d_std);
        npss.insert(std::pair<unsigned int, PrecalcShapeset*>(stage.idx[i], new_ps));

        PrecalcShapeset* new_pss = new PrecalcShapeset(new_ps);
        new_pss->set_quad_2d(&g_quad_2d_std);
        nspss.insert(std::pair<unsigned int, PrecalcShapeset*>(stage.idx[i], new_pss));

        RefMap* new_rm = new RefMap();
        new_rm->set_quad_2d(&g_quad_2d_std);
        nrefmap.insert(std::pair<unsigned int, RefMap*>(stage.idx[i], new_rm));
      }

      for(unsigned int neighbor_i = 0; neighbor_i < num_neighbors; neighbor_i++) {
        // If the active segment has already been processed (when the neighbor element was assembled), it is skipped.
        // We test all neighbor searches, because in the case of intra-element edge, the neighboring (the same as central) element
        // will be marked as visited, even though the edge was not calculated.
        bool processed = true;
        for(unsigned int i = 0; i < neighbor_searches.get_size(); i++)
          if(neighbor_searches.present(i))
            if(!neighbor_searches.get(i)->neighbors.at(neighbor_i)->visited) {
              processed = false;
              break;
            }

            if(!DG_vector_forms_present && processed)
              continue;

            // For every neighbor we want to delete the geometry caches and create new ones.
            for (int i = 0; i < g_max_quad + 1 + 4 * g_max_quad + 4; i++)
              if (cache_e[i] != NULL) {
                cache_e[i]->free(); 
                delete cache_e[i];
                cache_e[i] = NULL;
                delete [] cache_jwt[i];
              }

              assemble_DG_one_neighbor(processed, neighbor_i, stage, mat, rhs, 
                force_diagonal_blocks, block_weights, spss, refmap, 
                npss, nspss, nrefmap, neighbor_searches, u_ext, isempty, 
                marker, al, bnd, surf_pos, nat, isurf, e, trav_base, rep_element);
      }

      // Delete the multimesh tree;
      delete root;

      // Deinitialize neighbor pss's, refmaps.
      if(DG_matrix_forms_present) {
        for(std::map<unsigned int, PrecalcShapeset *>::iterator it = nspss.begin(); it != nspss.end(); it++)
          delete it->second;
        for(std::map<unsigned int, PrecalcShapeset *>::iterator it = npss.begin(); it != npss.end(); it++)
          delete it->second;
        for(std::map<unsigned int, RefMap *>::iterator it = nrefmap.begin(); it != nrefmap.end(); it++)
          delete it->second;
      }

      // Delete the neighbor_searches array.
      for(unsigned int i = 0; i < neighbor_searches.get_size(); i++) 
        if(neighbor_searches.present(i))
          delete neighbor_searches.get(i);
}


template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_DG_one_neighbor(bool edge_processed, unsigned int neighbor_i, Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, Table* block_weights,
       Hermes::vector<PrecalcShapeset *>& spss, Hermes::vector<RefMap *>& refmap, std::map<unsigned int, PrecalcShapeset *> npss,
       std::map<unsigned int, PrecalcShapeset *> nspss, std::map<unsigned int, RefMap *> nrefmap, LightArray<NeighborSearch<Scalar>*>& neighbor_searches, Hermes::vector<Solution<Scalar>*>& u_ext, 
        Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, bool bnd, SurfPos& surf_pos, Hermes::vector<bool>& nat, 
  int isurf, Element** e, Element* trav_base, Element* rep_element)
{
  _F_;
  // Set the active segment in all NeighborSearches
  for(unsigned int i = 0; i < neighbor_searches.get_size(); i++)
    if(neighbor_searches.present(i)) {
      neighbor_searches.get(i)->active_segment = neighbor_i;
      neighbor_searches.get(i)->neighb_el = neighbor_searches.get(i)->neighbors[neighbor_i];
      neighbor_searches.get(i)->neighbor_edge = neighbor_searches.get(i)->neighbor_edges[neighbor_i];
    }

    // Push all the necessary transformations to all functions of this stage.
    // The important thing is that the transformations to the current subelement are already there.
    for(unsigned int fns_i = 0; fns_i < stage.fns.size(); fns_i++)
      for(unsigned int trf_i = 0; trf_i < neighbor_searches.get(stage.meshes[fns_i]->get_seq() - min_dg_mesh_seq)->central_n_trans[neighbor_i]; trf_i++)
        stage.fns[fns_i]->push_transform(neighbor_searches.get(stage.meshes[fns_i]->get_seq() - min_dg_mesh_seq)->central_transformations[neighbor_i][trf_i]);

    // For neighbor psss.
    if(DG_matrix_forms_present && !edge_processed)
      for(unsigned int idx_i = 0; idx_i < stage.idx.size(); idx_i++) {
        npss[stage.idx[idx_i]]->set_active_element((*neighbor_searches.get(stage.meshes[idx_i]->get_seq() - min_dg_mesh_seq)->get_neighbors())[neighbor_i]);
        for(unsigned int trf_i = 0; trf_i < neighbor_searches.get(stage.meshes[idx_i]->get_seq() - min_dg_mesh_seq)->neighbor_n_trans[neighbor_i]; trf_i++)
          npss[stage.idx[idx_i]]->push_transform(neighbor_searches.get(stage.meshes[idx_i]->get_seq() - min_dg_mesh_seq)->neighbor_transformations[neighbor_i][trf_i]);
      }

      // Also push the transformations to the slave psss and refmaps.
      for (unsigned int i = 0; i < stage.idx.size(); i++) {
        if(isempty[stage.idx[i]])
          continue;
        spss[stage.idx[i]]->set_master_transform();
        refmap[stage.idx[i]]->force_transform(pss[stage.idx[i]]->get_transform(), pss[stage.idx[i]]->get_ctm());

        // Neighbor.
        if(DG_matrix_forms_present && !edge_processed) {
          nspss[stage.idx[i]]->set_active_element(npss[stage.idx[i]]->get_active_element());
          nspss[stage.idx[i]]->set_master_transform();
          nrefmap[stage.idx[i]]->set_active_element(npss[stage.idx[i]]->get_active_element());
          nrefmap[stage.idx[i]]->force_transform(npss[stage.idx[i]]->get_transform(), npss[stage.idx[i]]->get_ctm());
        }
      }

      /***/

      // The computation takes place here.
      if(DG_matrix_forms_present && !edge_processed) {
        assemble_DG_matrix_forms(stage, mat, rhs, force_diagonal_blocks, block_weights, spss, refmap, npss, nspss, nrefmap, neighbor_searches, u_ext, isempty, 
          marker, al, bnd, surf_pos, nat, isurf, e, trav_base, rep_element);
        if(!stage.mfsurf_mc.empty())
          assemble_multicomponent_DG_matrix_forms(stage, mat, rhs, force_diagonal_blocks, block_weights, spss, refmap, npss, nspss, nrefmap, neighbor_searches, u_ext, isempty, 
          marker, al, bnd, surf_pos, nat, isurf, e, trav_base, rep_element);
      }
      if (DG_vector_forms_present && rhs != NULL) {
        assemble_DG_vector_forms(stage, mat, rhs, force_diagonal_blocks, block_weights, spss, refmap, neighbor_searches, u_ext, isempty, 
          marker, al, bnd, surf_pos, nat, isurf, e, trav_base, rep_element);
        if(!stage.vfsurf_mc.empty())
          assemble_multicomponent_DG_vector_forms(stage, mat, rhs, force_diagonal_blocks, block_weights, spss, refmap, neighbor_searches, u_ext, isempty, 
          marker, al, bnd, surf_pos, nat, isurf, e, trav_base, rep_element);
      }
      /***/

      // This is just cleaning after ourselves.
      // Clear the transformations from the RefMaps and all functions.
      for(unsigned int fns_i = 0; fns_i < stage.fns.size(); fns_i++)
        stage.fns[fns_i]->set_transform(neighbor_searches.get(stage.meshes[fns_i]->get_seq() - min_dg_mesh_seq)->original_central_el_transform);

      // Also clear the transformations from the slave psss and refmaps.
      for (unsigned int i = 0; i < stage.idx.size(); i++) {
        if(isempty[stage.idx[i]])
          continue;
        spss[stage.idx[i]]->set_master_transform();
        refmap[stage.idx[i]]->force_transform(pss[stage.idx[i]]->get_transform(), pss[stage.idx[i]]->get_ctm());
      }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::init_neighbors(LightArray<NeighborSearch<Scalar>*>& neighbor_searches, 
  const Stage<Scalar>& stage, const int& isurf)
{
  _F_;
  // Initialize the NeighborSearches.
  for(unsigned int i = 0; i < stage.meshes.size(); i++) {
    if(!neighbor_searches.present(stage.meshes[i]->get_seq() - min_dg_mesh_seq)) {
      NeighborSearch<Scalar>* ns = new NeighborSearch<Scalar>(stage.fns[i]->get_active_element(), stage.meshes[i]);
      ns->original_central_el_transform = stage.fns[i]->get_transform();
      neighbor_searches.add(ns, stage.meshes[i]->get_seq() - min_dg_mesh_seq);
    }
  }

  // Calculate respective neighbors.
  // Also clear the initial_sub_idxs from the central element transformations 
  // of NeighborSearches with multiple neighbors.
  for(unsigned int i = 0; i < neighbor_searches.get_size(); i++) 
    if(neighbor_searches.present(i)) {
      neighbor_searches.get(i)->set_active_edge_multimesh(isurf);
      neighbor_searches.get(i)->clear_initial_sub_idx();
    }
    return;
}

template<typename Scalar>
void DiscreteProblem<Scalar>::build_multimesh_tree(NeighborNode* root, 
  LightArray<NeighborSearch<Scalar>*>& neighbor_searches)
{
  _F_;
  for(unsigned int i = 0; i < neighbor_searches.get_size(); i++) 
    if(neighbor_searches.present(i)) {
      NeighborSearch<Scalar>* ns = neighbor_searches.get(i);
      if(ns->n_neighbors == 1 && ns->central_n_trans[0] == 0)
        continue;
      for(unsigned int j = 0; j < ns->n_neighbors; j++)
        insert_into_multimesh_tree(root, ns->central_transformations[j], ns->central_n_trans[j]);
    }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::insert_into_multimesh_tree(NeighborNode* node, 
  unsigned int* transformations,  
  unsigned int transformation_count)
{
  _F_;
  // If we are already in the leaf.
  if(transformation_count == 0)
    return;
  // Both sons are null. We have to add a new Node. Let us do it for the left sone of node.
  if(node->get_left_son() == NULL && node->get_right_son() == NULL) {
    node->set_left_son(new NeighborNode(node, transformations[0]));
    insert_into_multimesh_tree(node->get_left_son(), transformations + 1, transformation_count - 1);
  }
  // At least the left son is not null (it is impossible only for the right one to be not null, because
  // the left one always gets into the tree first, as seen above).
  else {
    // The existing left son is the right one to continue through.
    if(node->get_left_son()->get_transformation() == transformations[0])
      insert_into_multimesh_tree(node->get_left_son(), transformations + 1, transformation_count - 1);
    // The right one also exists, check that it is the right one, or return an error.
    else if(node->get_right_son() != NULL) {
      if(node->get_right_son()->get_transformation() == transformations[0])
        insert_into_multimesh_tree(node->get_right_son(), transformations + 1, transformation_count - 1);
      else error("More than two possible sons in insert_into_multimesh_tree().");
    }
    // If the right one does not exist and the left one was not correct, create a right son and continue this way.
    else {
      node->set_right_son(new NeighborNode(node, transformations[0]));
      insert_into_multimesh_tree(node->get_right_son(), transformations + 1, transformation_count - 1);
    }
  }
}

template<typename Scalar>
Hermes::vector<Hermes::vector<unsigned int>*> DiscreteProblem<Scalar>::get_multimesh_neighbors_transformations(NeighborNode* multimesh_tree)
{
  _F_;
  // Initialize the vector.
  Hermes::vector<Hermes::vector<unsigned int>*> running_transformations;
  // Prepare the first neighbor's vector.
  running_transformations.push_back(new Hermes::vector<unsigned int>);
  // Fill the vector.
  traverse_multimesh_tree(multimesh_tree, running_transformations);
  return running_transformations;
}

template<typename Scalar>
void DiscreteProblem<Scalar>::traverse_multimesh_tree(NeighborNode* node, 
  Hermes::vector<Hermes::vector<unsigned int>*>& running_transformations)
{
  _F_;
  // If we are in the root.
  if(node->get_transformation() == 0) {
    if(node->get_left_son() != NULL)
      traverse_multimesh_tree(node->get_left_son(), running_transformations);
    if(node->get_right_son() != NULL)
      traverse_multimesh_tree(node->get_right_son(), running_transformations);
    // Delete the vector prepared by the last accessed leaf.
    running_transformations.pop_back();
    return;
  }
  // If we are in a leaf.
  if(node->get_left_son() == NULL && node->get_right_son() == NULL) {
    // Create a vector for the new neighbor.
    Hermes::vector<unsigned int>* new_neighbor_transformations = new Hermes::vector<unsigned int>;
    // Copy there the whole path except for this leaf.
    for(unsigned int i = 0; i < running_transformations.back()->size(); i++)
      new_neighbor_transformations->push_back((*running_transformations.back())[i]);
    // Insert this leaf into the current running transformation, thus complete it.
    running_transformations.back()->push_back(node->get_transformation());
    // Make the new_neighbor_transformations the current running transformation.
    running_transformations.push_back(new_neighbor_transformations);
    return;
  }
  else {
    running_transformations.back()->push_back(node->get_transformation());
    if(node->get_left_son() != NULL)
      traverse_multimesh_tree(node->get_left_son(), running_transformations);
    if(node->get_right_son() != NULL)
      traverse_multimesh_tree(node->get_right_son(), running_transformations);
    running_transformations.back()->pop_back();
    return;
  }
  return;
}

template<typename Scalar>
void DiscreteProblem<Scalar>::update_neighbor_search(NeighborSearch<Scalar>* ns, NeighborNode* multimesh_tree)
{
  _F_;
  // This has to be done, because we pass ns by reference and the number of neighbors is changing.
  unsigned int num_neighbors = ns->get_num_neighbors();

  for(unsigned int i = 0; i < num_neighbors; i++) {
    // Find the node corresponding to this neighbor in the tree.
    NeighborNode* node = find_node(ns->central_transformations[i], ns->central_n_trans[i], multimesh_tree);
    // Update the NeighborSearch.
    unsigned int added = update_ns_subtree(ns, node, i);
    i += added;
    num_neighbors += added;
  }
}

template<typename Scalar>
NeighborNode* DiscreteProblem<Scalar>::find_node(unsigned int* transformations, 
  unsigned int transformation_count, 
  NeighborNode* node)
{
  _F_;
  // If there are no transformations left.
  if(transformation_count == 0)
    return node;
  else {
    if(node->get_left_son() != NULL) {
      if(node->get_left_son()->get_transformation() == transformations[0])
        return find_node(transformations + 1, transformation_count - 1, node->get_left_son());
    }
    if(node->get_right_son() != NULL) {
      if(node->get_right_son()->get_transformation() == transformations[0])
        return find_node(transformations + 1, transformation_count - 1, node->get_right_son());
    }
  }
  // We always should be able to empty the transformations array.
  error("Transformation of a central element not found in the multimesh tree.");
  return NULL;
}

template<typename Scalar>
unsigned int DiscreteProblem<Scalar>::update_ns_subtree(NeighborSearch<Scalar>* ns, 
  NeighborNode* node, unsigned int ith_neighbor)
{
  _F_;
  // No subtree => no work.
  // Also check the assertion that if one son is null, then the other too.
  if(node->get_left_son() == NULL) {
    if(node->get_right_son() != NULL)
      error("Only one son (right) not null in DiscreteProblem<Scalar>::update_ns_subtree.");
    return 0;
  }

  // Key part.
  // Begin with storing the info about the current neighbor.
  Element* neighbor = ns->neighbors[ith_neighbor];
  typename NeighborSearch<Scalar>::NeighborEdgeInfo edge_info = ns->neighbor_edges[ith_neighbor];

  // Initialize the vector for central transformations->
  Hermes::vector<Hermes::vector<unsigned int>*> running_central_transformations;
  // Prepare the first new neighbor's vector. Push back the current transformations (in case of GO_DOWN neighborhood).
  running_central_transformations.push_back(new Hermes::vector<unsigned int>);
  for(unsigned int i = 0; i < ns->central_n_trans[ith_neighbor]; i++)
    running_central_transformations.back()->push_back(ns->central_transformations[ith_neighbor][i]);

  // Initialize the vector for neighbor transformations->
  Hermes::vector<Hermes::vector<unsigned int>*> running_neighbor_transformations;
  // Prepare the first new neighbor's vector. Push back the current transformations (in case of GO_UP/NO_TRF neighborhood).
  running_neighbor_transformations.push_back(new Hermes::vector<unsigned int>);
  for(unsigned int i = 0; i < ns->neighbor_n_trans[ith_neighbor]; i++)
    running_neighbor_transformations.back()->push_back(ns->neighbor_transformations[ith_neighbor][i]);

  // Delete the current neighbor.
  ns->delete_neighbor(ith_neighbor);

  // Move down the subtree.
  if(node->get_left_son() != NULL)
    traverse_multimesh_subtree(node->get_left_son(), running_central_transformations, 
    running_neighbor_transformations, edge_info, ns->active_edge, 
    ns->central_el->get_mode());
  if(node->get_right_son() != NULL)
    traverse_multimesh_subtree(node->get_right_son(), running_central_transformations, 
    running_neighbor_transformations, edge_info, ns->active_edge, 
    ns->central_el->get_mode());

  // Delete the last neighbors' info (this is a dead end, caused by the function traverse_multimesh_subtree.
  running_central_transformations.pop_back();
  running_neighbor_transformations.pop_back();

  // Insert new neighbors.
  for(unsigned int i = 0; i < running_central_transformations.size(); i++) {
    ns->neighbors.push_back(neighbor);
    ns->neighbor_edges.push_back(edge_info);
    ns->central_n_trans[ns->n_neighbors] = running_central_transformations[i]->size();
    ns->neighbor_n_trans[ns->n_neighbors] = running_neighbor_transformations[i]->size();
    for(unsigned int ii = 0; ii < ns->central_n_trans[ns->n_neighbors]; ii++)
      ns->central_transformations[ns->n_neighbors][ii] = (*running_central_transformations[i])[ii];
    for(unsigned int ii = 0; ii < ns->neighbor_n_trans[ns->n_neighbors]; ii++)
      ns->neighbor_transformations[ns->n_neighbors][ii] = (*running_neighbor_transformations[i])[ii];
    ns->n_neighbors++;
  }

  // Return the number of neighbors deleted.
  return - 1;
}

template<typename Scalar>
void DiscreteProblem<Scalar>::traverse_multimesh_subtree(NeighborNode* node, 
  Hermes::vector<Hermes::vector<unsigned int>*>& running_central_transformations,
  Hermes::vector<Hermes::vector<unsigned int>*>& running_neighbor_transformations, 
  const typename NeighborSearch<Scalar>::NeighborEdgeInfo& edge_info, const int& active_edge, const int& mode)
{
  _F_;
  // If we are in a leaf.
  if(node->get_left_son() == NULL && node->get_right_son() == NULL) {
    // Create vectors for the new neighbor.
    Hermes::vector<unsigned int>* new_neighbor_central_transformations = new Hermes::vector<unsigned int>;
    Hermes::vector<unsigned int>* new_neighbor_neighbor_transformations = new Hermes::vector<unsigned int>;

    // Copy there the whole path except for this leaf.
    for(unsigned int i = 0; i < running_central_transformations.back()->size(); i++)
      new_neighbor_central_transformations->push_back((*running_central_transformations.back())[i]);
    for(unsigned int i = 0; i < running_neighbor_transformations.back()->size(); i++)
      new_neighbor_neighbor_transformations->push_back((*running_neighbor_transformations.back())[i]);

    // Insert this leaf into the current running central transformation, thus complete it.
    running_central_transformations.back()->push_back(node->get_transformation());

    // Make the new_neighbor_central_transformations the current running central transformation.
    running_central_transformations.push_back(new_neighbor_central_transformations);

    // Take care of the neighbor transformation.
    // Insert appropriate info from this leaf into the current running neighbor transformation, thus complete it.
    if(mode == HERMES_MODE_TRIANGLE)
      if ((active_edge == 0 && node->get_transformation() == 0) ||
        (active_edge == 1 && node->get_transformation() == 1) ||
        (active_edge == 2 && node->get_transformation() == 2))
        running_neighbor_transformations.back()->push_back((!edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 3));
      else
        running_neighbor_transformations.back()->push_back((edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 3));
    // Quads.
    else
      if ((active_edge == 0 && (node->get_transformation() == 0 || node->get_transformation() == 6)) ||
        (active_edge == 1 && (node->get_transformation() == 1 || node->get_transformation() == 4)) ||
        (active_edge == 2 && (node->get_transformation() == 2 || node->get_transformation() == 7)) ||
        (active_edge == 3 && (node->get_transformation() == 3 || node->get_transformation() == 5)))
        running_neighbor_transformations.back()->push_back((!edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 4));
      else
        running_neighbor_transformations.back()->push_back((edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 4));

    // Make the new_neighbor_neighbor_transformations the current running neighbor transformation.
    running_neighbor_transformations.push_back(new_neighbor_neighbor_transformations);

    return;
  }
  else {
    // Insert this leaf into the current running central transformation, thus complete it.
    running_central_transformations.back()->push_back(node->get_transformation());

    // Insert appropriate info from this leaf into the current running neighbor transformation, thus complete it.
    // Triangles.
    if(mode == HERMES_MODE_TRIANGLE)
      if ((active_edge == 0 && node->get_transformation() == 0) ||
        (active_edge == 1 && node->get_transformation() == 1) ||
        (active_edge == 2 && node->get_transformation() == 2))
        running_neighbor_transformations.back()->push_back((!edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 3));
      else
        running_neighbor_transformations.back()->push_back((edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 3));
    // Quads.
    else
      if ((active_edge == 0 && (node->get_transformation() == 0 || node->get_transformation() == 6)) ||
        (active_edge == 1 && (node->get_transformation() == 1 || node->get_transformation() == 4)) ||
        (active_edge == 2 && (node->get_transformation() == 2 || node->get_transformation() == 7)) ||
        (active_edge == 3 && (node->get_transformation() == 3 || node->get_transformation() == 5)))
        running_neighbor_transformations.back()->push_back((!edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 4));
      else
        running_neighbor_transformations.back()->push_back((edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 4));

    // Move down.
    if(node->get_left_son() != NULL)
      traverse_multimesh_subtree(node->get_left_son(), running_central_transformations, running_neighbor_transformations, 
      edge_info, active_edge, mode);
    if(node->get_right_son() != NULL)
      traverse_multimesh_subtree(node->get_right_son(), running_central_transformations, running_neighbor_transformations, 
      edge_info, active_edge, mode);

    // Take this transformation out.
    running_central_transformations.back()->pop_back();
    running_neighbor_transformations.back()->pop_back();
    return;
  }
  return;
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_surface_matrix_forms(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, 
  bool bnd, SurfPos& surf_pos, Hermes::vector<bool>& nat, 
  int isurf, Element** e, Element* trav_base)
{
  _F_;
  for (unsigned int ww = 0; ww < stage.mfsurf.size(); ww++) {
    MatrixFormSurf<Scalar>* mfs = stage.mfsurf[ww];
    int m = mfs->i;
    int n = mfs->j;
    if (isempty[m] || isempty[n]) continue;
    if (!nat[m] || !nat[n]) continue;
    if (fabs(mfs->scaling_factor) < 1e-12) continue;
    if (mfs->areas[0] == H2D_DG_INNER_EDGE) continue;

    // Assemble this form only if one of its areas is HERMES_ANY or H2D_DG_BOUNDARY_EDGE,
    // or if the element marker coincides with one of the form's areas.
    bool assemble_this_form = false;
    for (unsigned int ss = 0; ss < mfs->areas.size(); ss++) {
      if ((mfs->areas[ss] == HERMES_ANY) || (marker == boundary_markers_conversion->get_internal_marker(mfs->areas[ss]))
        || (mfs->areas[ss] == H2D_DG_BOUNDARY_EDGE)) {
          assemble_this_form = true;
          break;
      }
    }
    if (assemble_this_form == false) continue;

    // If a block scaling table is provided, and if the scaling coefficient
    // A_mn for this block is zero, then the form does not need to be assembled.
    double block_scaling_coeff = 1.;
    if (block_weights != NULL) {
      block_scaling_coeff = block_weights->get_A(m, n);
      if (fabs(block_scaling_coeff) < 1e-12) 
        continue;
    }

    surf_pos.base = trav_base;

    Scalar **local_stiffness_matrix = get_matrix_buffer(std::max(al[m]->cnt, al[n]->cnt));
    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      if (al[m]->dof[i] < 0) continue;
      spss[m]->set_active_shape(al[m]->idx[i]);
      for (unsigned int j = 0; j < al[n]->cnt; j++) {
        pss[n]->set_active_shape(al[n]->idx[j]);
        if (al[n]->dof[j] >= 0) {
          if (mat != NULL) {
            Scalar val = 0;
            // Numerical integration performed only if all coefficients multiplying the form are nonzero.
            if (std::abs(al[m]->coef[i]) > 1e-12 && std::abs(al[n]->coef[j]) > 1e-12) {
              val = block_scaling_coeff * eval_form(mfs, u_ext, pss[n], spss[m], refmap[n],
                refmap[m], &surf_pos) * al[n]->coef[j] * al[m]->coef[i];
            }
            local_stiffness_matrix[i][j] = val;
          }
        }
      }
    }
    if (mat != NULL)
      mat->add(al[m]->cnt, al[n]->cnt, local_stiffness_matrix, al[m]->dof, al[n]->dof);
  }
}
template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_multicomponent_surface_matrix_forms(Stage<Scalar> & stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, 
  bool bnd, SurfPos& surf_pos, Hermes::vector<bool>& nat, 
  int isurf, Element** e, Element* trav_base)
{
  _F_;
  for (unsigned int ww = 0; ww < stage.mfsurf_mc.size(); ww++) {
    MultiComponentMatrixFormSurf<Scalar>* mfs = stage.mfsurf_mc[ww];
    unsigned int m = mfs->coordinates[0].first;
    unsigned int n = mfs->coordinates[0].second;

    if (!nat[m] || !nat[n]) continue;
    if (fabs(mfs->scaling_factor) < 1e-12) continue;
    if (mfs->areas[0] == H2D_DG_INNER_EDGE) continue;

    // Assemble this form only if one of its areas is HERMES_ANY or H2D_DG_BOUNDARY_EDGE,
    // or if the element marker coincides with one of the form's areas.
    bool assemble_this_form = false;
    for (unsigned int ss = 0; ss < mfs->areas.size(); ss++) {
      if ((mfs->areas[ss] == HERMES_ANY) || (marker == boundary_markers_conversion->get_internal_marker(mfs->areas[ss]))
        || (mfs->areas[ss] == H2D_DG_BOUNDARY_EDGE)) {
          assemble_this_form = true;
          break;
      }
    }
    if (assemble_this_form == false) continue; 

    // If a block scaling table is provided, and if the scaling coefficient
    // A_mn for this block is zero, then the form does not need to be assembled.
    // If a block scaling table is provided, and if the scaling coefficient
    // A_mn for this block is zero, then the form does not need to be assembled.
    Hermes::vector<double> block_scaling_coeffs;
    for(unsigned int coordinate_i = 0; coordinate_i < mfs->coordinates.size(); coordinate_i++)
      if (block_weights != NULL)
        block_scaling_coeffs.push_back(block_weights->get_A(mfs->coordinates[coordinate_i].first, 
        mfs->coordinates[coordinate_i].second));
      else
        block_scaling_coeffs.push_back(1);

    surf_pos.base = trav_base;

    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      if (al[m]->dof[i] < 0) continue;
      spss[m]->set_active_shape(al[m]->idx[i]);
      for (unsigned int j = 0; j < al[n]->cnt; j++) {
        pss[n]->set_active_shape(al[n]->idx[j]);
        if (al[n]->dof[j] >= 0) {
          if (mat != NULL) {
            // Numerical integration performed only if all coefficients multiplying the form are nonzero.
            if (std::abs(al[m]->coef[i]) > 1e-12 && std::abs(al[n]->coef[j]) > 1e-12) {
              Hermes::vector<Scalar> result;
              eval_form(mfs, u_ext, pss[n], spss[m], refmap[n], refmap[m], &surf_pos, result);
              for(unsigned int coordinate_i = 0; coordinate_i < mfs->coordinates.size(); coordinate_i++)
                mat->add(al[mfs->coordinates[coordinate_i].first]->dof[i], al[mfs->coordinates[coordinate_i].second]->dof[j],
                result[coordinate_i] * block_scaling_coeffs[coordinate_i] * al[n]->coef[j] * al[m]->coef[i]);
            }
          }
        }
      }
    }
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_surface_vector_forms(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, 
  bool bnd, SurfPos& surf_pos, Hermes::vector<bool>& nat, 
  int isurf, Element** e, Element* trav_base)
{
  _F_;
  if(rhs == NULL)
    return;
  for (unsigned int ww = 0; ww < stage.vfsurf.size(); ww++) {
    VectorFormSurf<Scalar>* vfs = stage.vfsurf[ww];
    int m = vfs->i;
    if (isempty[m]) continue;
    if (fabs(vfs->scaling_factor) < 1e-12) continue;
    if (vfs->areas[0] == H2D_DG_INNER_EDGE) continue;

    // Assemble this form only if one of its areas is HERMES_ANY or H2D_DG_BOUNDARY_EDGE,
    // or if the element marker coincides with one of the form's areas.
    bool assemble_this_form = false;
    for (unsigned int ss = 0; ss < vfs->areas.size(); ss++) {
      if ((vfs->areas[ss] == HERMES_ANY) || (marker == boundary_markers_conversion->get_internal_marker(vfs->areas[ss]))
        || (vfs->areas[ss] == H2D_DG_BOUNDARY_EDGE)) {
          assemble_this_form = true;
          break;
      }
    }
    if (assemble_this_form == false) continue; 

    if (vfs->areas[0] == HERMES_ANY && !nat[m]) continue;

    surf_pos.base = trav_base;

    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      if (al[m]->dof[i] < 0) continue;

      spss[m]->set_active_shape(al[m]->idx[i]);

      // Numerical integration performed only if the coefficient multiplying the form is nonzero.
      if (std::abs(al[m]->coef[i]) > 1e-12) {   
        rhs->add(al[m]->dof[i], eval_form(vfs, u_ext, spss[m], refmap[m], &surf_pos) * al[m]->coef[i]);
      }
    }
  }
}
template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_multicomponent_surface_vector_forms(Stage<Scalar> & stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, 
  bool bnd, SurfPos& surf_pos, Hermes::vector<bool>& nat, 
  int isurf, Element** e, Element* trav_base)
{
  _F_;

  if (rhs == NULL) return;

  for (unsigned int ww = 0; ww < stage.vfsurf_mc.size(); ww++) {
    MultiComponentVectorFormSurf<Scalar>* vfs = stage.vfsurf_mc[ww];
    unsigned int m = vfs->coordinates[0];
    if (fabs(vfs->scaling_factor) < 1e-12) continue;
    if (vfs->areas[0] == H2D_DG_INNER_EDGE) continue;

    // Assemble this form only if one of its areas is HERMES_ANY or H2D_DG_BOUNDARY_EDGE,
    // or if the element marker coincides with one of the form's areas.
    bool assemble_this_form = false;
    for (unsigned int ss = 0; ss < vfs->areas.size(); ss++) {
      if ((vfs->areas[ss] == HERMES_ANY) || (marker == boundary_markers_conversion->get_internal_marker(vfs->areas[ss]))
        || (vfs->areas[ss] == H2D_DG_BOUNDARY_EDGE)) {
          assemble_this_form = true;
          break;
      }
    }
    if (assemble_this_form == false) continue; 

    if (vfs->areas[0] == HERMES_ANY && !nat[m]) continue;

    surf_pos.base = trav_base;

    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      if (al[m]->dof[i] < 0) continue;

      spss[m]->set_active_shape(al[m]->idx[i]);

      // Numerical integration performed only if the coefficient multiplying the form is nonzero.
      if (std::abs(al[m]->coef[i]) > 1e-12) {   
        Hermes::vector<Scalar> result;
        eval_form(vfs, u_ext, spss[m], refmap[m], &surf_pos, result);
        for(unsigned int coordinate_i = 0; coordinate_i < vfs->coordinates.size(); coordinate_i++)
          rhs->add(al[vfs->coordinates[coordinate_i]]->dof[i], result[coordinate_i] * al[vfs->coordinates[coordinate_i]]->coef[i]);
      }
    }
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_DG_matrix_forms(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, std::map<unsigned int, PrecalcShapeset *> npss,
  std::map<unsigned int, PrecalcShapeset *> nspss, std::map<unsigned int, RefMap *> nrefmap, 
  LightArray<NeighborSearch<Scalar>*>& neighbor_searches, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, bool bnd, 
  SurfPos& surf_pos, Hermes::vector<bool>& nat, int isurf, Element** e, 
  Element* trav_base, Element* rep_element)
{ 
  _F_;
  for (unsigned int ww = 0; ww < stage.mfsurf.size(); ww++) {
    MatrixFormSurf<Scalar>* mfs = stage.mfsurf[ww];
    if (mfs->areas[0] != H2D_DG_INNER_EDGE) continue;
    int m = mfs->i;
    int n = mfs->j;

    if (isempty[m] || isempty[n]) continue;
    if (fabs(mfs->scaling_factor) < 1e-12) continue;

    surf_pos.base = trav_base;

    // Create the extended shapeset on the union of the central element and its current neighbor.
    typename NeighborSearch<Scalar>::ExtendedShapeset* ext_asmlist_u = neighbor_searches.get(spaces[n]->get_mesh()->get_seq() - min_dg_mesh_seq)->create_extended_asmlist(spaces[n], al[n]);
    typename NeighborSearch<Scalar>::ExtendedShapeset* ext_asmlist_v = neighbor_searches.get(spaces[m]->get_mesh()->get_seq() - min_dg_mesh_seq)->create_extended_asmlist(spaces[m], al[m]);

    // If a block scaling table is provided, and if the scaling coefficient
    // A_mn for this block is zero, then the form does not need to be assembled.
    double block_scaling_coeff = 1.;
    if (block_weights != NULL) {
      block_scaling_coeff = block_weights->get_A(m, n);
      if (fabs(block_scaling_coeff) < 1e-12) 
        continue;
    }

    // Precalc shapeset and refmaps used for the evaluation.
    PrecalcShapeset* fu;
    PrecalcShapeset* fv;
    RefMap* ru;
    RefMap* rv;
    bool support_neigh_u, support_neigh_v;

    Scalar **local_stiffness_matrix = get_matrix_buffer(std::max(ext_asmlist_u->cnt, ext_asmlist_v->cnt));
    for (int i = 0; i < ext_asmlist_v->cnt; i++) {
      if (ext_asmlist_v->dof[i] < 0) 
        continue;
      // Choose the correct shapeset for the test function.
      if (!ext_asmlist_v->has_support_on_neighbor(i)) {
        spss[m]->set_active_shape(ext_asmlist_v->central_al->idx[i]);
        fv = spss[m];
        rv = refmap[m];
        support_neigh_v = false;
      }
      else {
        nspss[m]->set_active_shape(ext_asmlist_v->neighbor_al->idx[i - ext_asmlist_v->central_al->cnt]);
        fv = nspss[m];
        rv = nrefmap[m];
        support_neigh_v = true;
      }
      for (int j = 0; j < ext_asmlist_u->cnt; j++) {
        // Choose the correct shapeset for the solution function.
        if (!ext_asmlist_u->has_support_on_neighbor(j)) {
          pss[n]->set_active_shape(ext_asmlist_u->central_al->idx[j]);
          fu = pss[n];
          ru = refmap[n];
          support_neigh_u = false;
        }
        else {
          npss[n]->set_active_shape(ext_asmlist_u->neighbor_al->idx[j - ext_asmlist_u->central_al->cnt]);
          fu = npss[n];
          ru = nrefmap[n];
          support_neigh_u = true;
        }

        if (ext_asmlist_u->dof[j] >= 0) {
          if (mat != NULL) {
            Scalar val = block_scaling_coeff * eval_dg_form(mfs, u_ext, fu, fv, refmap[n], ru, rv, support_neigh_u, support_neigh_v, &surf_pos, neighbor_searches, stage.meshes[n]->get_seq() - min_dg_mesh_seq, stage.meshes[m]->get_seq() - min_dg_mesh_seq)
              * (support_neigh_u ? ext_asmlist_u->neighbor_al->coef[j - ext_asmlist_u->central_al->cnt]: ext_asmlist_u->central_al->coef[j])
              * (support_neigh_v ? ext_asmlist_v->neighbor_al->coef[i - ext_asmlist_v->central_al->cnt]: ext_asmlist_v->central_al->coef[i]);
            local_stiffness_matrix[i][j] = val;
          }
        }
      }
    }
    if (mat != NULL)
      mat->add(ext_asmlist_v->cnt, ext_asmlist_u->cnt, local_stiffness_matrix, ext_asmlist_v->dof, ext_asmlist_u->dof);
  }
}
template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_multicomponent_DG_matrix_forms(Stage<Scalar> & stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, 
  Table* block_weights, Hermes::vector<PrecalcShapeset *>& spss, 
  Hermes::vector<RefMap *>& refmap, std::map<unsigned int, PrecalcShapeset *> npss,
  std::map<unsigned int, PrecalcShapeset *> nspss, std::map<unsigned int, RefMap *> nrefmap, 
  LightArray<NeighborSearch<Scalar>*>& neighbor_searches, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, bool bnd, 
  SurfPos& surf_pos, Hermes::vector<bool>& nat, int isurf, Element** e, 
  Element* trav_base, Element* rep_element)
{ 
  _F_;
  for (unsigned int ww = 0; ww < stage.mfsurf_mc.size(); ww++) {
    MultiComponentMatrixFormSurf<Scalar>* mfs = stage.mfsurf_mc[ww];
    if (mfs->areas[0] != H2D_DG_INNER_EDGE) continue;

    if (fabs(mfs->scaling_factor) < 1e-12) continue;

    unsigned int m = mfs->coordinates[0].first;
    unsigned int n = mfs->coordinates[0].second;

    surf_pos.base = trav_base;

    // Create the extended shapeset on the union of the central element and its current neighbor.
    Hermes::vector<NeighborSearch<Scalar>::ExtendedShapeset*> ext_asmlists;
    Hermes::vector<unsigned int> coordinates_processed;
    for(unsigned int coordinate_i = 0; coordinate_i < mfs->coordinates.size(); coordinate_i++) {
      bool new_coordinate = true;
      for(unsigned int i = 0; i < coordinates_processed.size(); i++)
        if(coordinates_processed[i] == mfs->coordinates[coordinate_i].first)
          new_coordinate = false;
      if(new_coordinate) {
        coordinates_processed.push_back(mfs->coordinates[coordinate_i].first);
        ext_asmlists.push_back(neighbor_searches.get(stage.meshes[mfs->coordinates[coordinate_i].first]->get_seq() - min_dg_mesh_seq)->create_extended_asmlist_multicomponent(spaces[mfs->coordinates[coordinate_i].first], al[mfs->coordinates[coordinate_i].first]));
      }

      new_coordinate = true;
      for(unsigned int i = 0; i < coordinates_processed.size(); i++)
        if(coordinates_processed[i] == mfs->coordinates[coordinate_i].second)
          new_coordinate = false;
      if(new_coordinate) {
        coordinates_processed.push_back(mfs->coordinates[coordinate_i].second);
        ext_asmlists.push_back(neighbor_searches.get(stage.meshes[mfs->coordinates[coordinate_i].second]->get_seq() - min_dg_mesh_seq)->create_extended_asmlist_multicomponent(spaces[mfs->coordinates[coordinate_i].second], al[mfs->coordinates[coordinate_i].second]));
      }
    }

    NeighborSearch<Scalar>::ExtendedShapeset* ext_asmlist_u = neighbor_searches.get(spaces[n]->get_mesh()->get_seq() - min_dg_mesh_seq)->create_extended_asmlist(spaces[n], al[n]);
    NeighborSearch<Scalar>::ExtendedShapeset* ext_asmlist_v = neighbor_searches.get(spaces[m]->get_mesh()->get_seq() - min_dg_mesh_seq)->create_extended_asmlist(spaces[m], al[m]);

    // If a block scaling table is provided, and if the scaling coefficient
    // A_mn for this block is zero, then the form does not need to be assembled.
    Hermes::vector<double> block_scaling_coeffs;
    for(unsigned int coordinate_i = 0; coordinate_i < mfs->coordinates.size(); coordinate_i++)
      if (block_weights != NULL)
        block_scaling_coeffs.push_back(block_weights->get_A(mfs->coordinates[coordinate_i].first, mfs->coordinates[coordinate_i].second));
      else
        block_scaling_coeffs.push_back(1);

    // Precalc shapeset and refmaps used for the evaluation.
    PrecalcShapeset* fu;
    PrecalcShapeset* fv;
    RefMap* ru;
    RefMap* rv;
    bool support_neigh_u, support_neigh_v;

    for (int i = 0; i < ext_asmlist_v->cnt; i++) {
      if (ext_asmlist_v->dof[i] < 0) 
        continue;
      // Choose the correct shapeset for the test function.
      if (!ext_asmlist_v->has_support_on_neighbor(i)) {
        spss[m]->set_active_shape(ext_asmlist_v->central_al->idx[i]);
        fv = spss[m];
        rv = refmap[m];
        support_neigh_v = false;
      }
      else {
        nspss[m]->set_active_shape(ext_asmlist_v->neighbor_al->idx[i - ext_asmlist_v->central_al->cnt]);
        fv = nspss[m];
        rv = nrefmap[m];
        support_neigh_v = true;
      }
      for (int j = 0; j < ext_asmlist_u->cnt; j++) {
        // Choose the correct shapeset for the solution function.
        if (!ext_asmlist_u->has_support_on_neighbor(j)) {
          pss[n]->set_active_shape(ext_asmlist_u->central_al->idx[j]);
          fu = pss[n];
          ru = refmap[n];
          support_neigh_u = false;
        }
        else {
          npss[n]->set_active_shape(ext_asmlist_u->neighbor_al->idx[j - ext_asmlist_u->central_al->cnt]);
          fu = npss[n];
          ru = nrefmap[n];
          support_neigh_u = true;
        }

        if (ext_asmlist_u->dof[j] >= 0) {
          if (mat != NULL) {
            Hermes::vector<Scalar> result;
            eval_dg_form(mfs, u_ext, fu, fv, refmap[n], ru, rv, support_neigh_u, support_neigh_v, &surf_pos, neighbor_searches, stage.meshes[n]->get_seq() - min_dg_mesh_seq, stage.meshes[m]->get_seq() - min_dg_mesh_seq, result);
            for(unsigned int coordinate_i = 0; coordinate_i < mfs->coordinates.size(); coordinate_i++)
              mat->add(ext_asmlists[mfs->coordinates[coordinate_i].first]->dof[i], ext_asmlists[mfs->coordinates[coordinate_i].second]->dof[j],
              result[coordinate_i] * block_scaling_coeffs[coordinate_i] 
            * (support_neigh_u ? ext_asmlist_u->neighbor_al->coef[j - ext_asmlist_u->central_al->cnt]: ext_asmlist_u->central_al->coef[j])
              * (support_neigh_v ? ext_asmlist_v->neighbor_al->coef[i - ext_asmlist_v->central_al->cnt]: ext_asmlist_v->central_al->coef[i]));
          }
        }
      }
    }

    for(unsigned int ext_asms_i = 0; ext_asms_i < ext_asmlists.size(); ext_asms_i++) {
      ext_asmlists[ext_asms_i]->free_central_al();
      delete ext_asmlists[ext_asms_i];
    }
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_DG_vector_forms(Stage<Scalar>& stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, Table* block_weights,
  Hermes::vector<PrecalcShapeset *>& spss, Hermes::vector<RefMap *>& refmap, LightArray<NeighborSearch<Scalar>*>& neighbor_searches, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, bool bnd, SurfPos& surf_pos, Hermes::vector<bool>& nat, 
  int isurf, Element** e, Element* trav_base, Element* rep_element)
{
  _F_;
  for (unsigned int ww = 0; ww < stage.vfsurf.size(); ww++) {
    VectorFormSurf<Scalar>* vfs = stage.vfsurf[ww];
    if (vfs->areas[0] != H2D_DG_INNER_EDGE) continue;
    int m = vfs->i;
    if (isempty[m]) continue;
    if (fabs(vfs->scaling_factor) < 1e-12) continue;

    // Here we use the standard pss, possibly just transformed by NeighborSearch.
    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      if (al[m]->dof[i] < 0) continue;
      spss[m]->set_active_shape(al[m]->idx[i]);
      rhs->add(al[m]->dof[i], eval_dg_form(vfs, u_ext, spss[m], refmap[m], &surf_pos, neighbor_searches, stage.meshes[m]->get_seq() - min_dg_mesh_seq) * al[m]->coef[i]);
    }
  }
}
template<typename Scalar>
void DiscreteProblem<Scalar>::assemble_multicomponent_DG_vector_forms(Stage<Scalar> & stage, 
  SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs, bool force_diagonal_blocks, Table* block_weights,
  Hermes::vector<PrecalcShapeset *>& spss, Hermes::vector<RefMap *>& refmap, LightArray<NeighborSearch<Scalar>*>& neighbor_searches, Hermes::vector<Solution<Scalar>*>& u_ext, 
  Hermes::vector<bool>& isempty, int marker, Hermes::vector<AsmList<Scalar>*>& al, bool bnd, SurfPos& surf_pos, Hermes::vector<bool>& nat, 
  int isurf, Element** e, Element* trav_base, Element* rep_element)
{
  _F_;

  if (rhs == NULL) return;

  for (unsigned int ww = 0; ww < stage.vfsurf_mc.size(); ww++) {
    MultiComponentVectorFormSurf<Scalar>* vfs = stage.vfsurf_mc[ww];
    if (vfs->areas[0] != H2D_DG_INNER_EDGE) continue;
    if (fabs(vfs->scaling_factor) < 1e-12) continue;

    // Here we use the standard pss, possibly just transformed by NeighborSearch.
    unsigned int m = vfs->coordinates[0];
    for (unsigned int i = 0; i < al[m]->cnt; i++) {
      if (al[m]->dof[i] < 0) continue;
      Hermes::vector<Scalar> result;
      spss[m]->set_active_shape(al[m]->idx[i]);
      eval_dg_form(vfs, u_ext, spss[m], refmap[m], &surf_pos, neighbor_searches, stage.meshes[m]->get_seq() - min_dg_mesh_seq, result);
      for(unsigned int coordinate_i = 0; coordinate_i < vfs->coordinates.size(); coordinate_i++)
        rhs->add(al[vfs->coordinates[coordinate_i]]->dof[i], result[coordinate_i] * al[vfs->coordinates[coordinate_i]]->coef[i]);
    }
  }
}

template<typename Scalar>
ExtData<Ord>* DiscreteProblem<Scalar>::init_ext_fns_ord(Hermes::vector<MeshFunction<Scalar>*> &ext)
{
  _F_;
  ExtData<Ord>* fake_ext = new ExtData<Ord>;

  // External functions.
  fake_ext->nf = ext.size();
  Func<Ord>** fake_ext_fn = new Func<Ord>*[fake_ext->nf];
  for (int i = 0; i < fake_ext->nf; i++)
    fake_ext_fn[i] = get_fn_ord(ext[i]->get_fn_order());
  fake_ext->fn = fake_ext_fn;

  return fake_ext;
}

template<typename Scalar>
ExtData<Scalar>* DiscreteProblem<Scalar>::init_ext_fns(Hermes::vector<MeshFunction<Scalar>*> &ext, 
  RefMap *rm, const int order)
{
  _F_;
  ExtData<Scalar>* ext_data = new ExtData<Scalar>;

  // Copy external functions.
  Func<Scalar>** ext_fn = new Func<Scalar>*[ext.size()];
  for (unsigned i = 0; i < ext.size(); i++) {
    if (ext[i] != NULL) ext_fn[i] = init_fn(ext[i], order);
    else ext_fn[i] = NULL;
  }
  ext_data->nf = ext.size();
  ext_data->fn = ext_fn;

  return ext_data;
}

template<typename Scalar>
ExtData<Ord>* DiscreteProblem<Scalar>::init_ext_fns_ord(Hermes::vector<MeshFunction<Scalar>*> &ext, 
  int edge)
{
  _F_;
  ExtData<Ord>* fake_ext = new ExtData<Ord>;

  // External functions.
  fake_ext->nf = ext.size();
  Func<Ord>** fake_ext_fn = new Func<Ord>*[fake_ext->nf];
  for (int i = 0; i < fake_ext->nf; i++)
    fake_ext_fn[i] = get_fn_ord(ext[i]->get_edge_fn_order(edge));
  fake_ext->fn = fake_ext_fn;

  return fake_ext;
}

template<typename Scalar>
ExtData<Scalar>* DiscreteProblem<Scalar>::init_ext_fns(Hermes::vector<MeshFunction<Scalar>*> &ext, 
  LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int order)
{
  _F_;
  Func<Scalar>** ext_fns = new Func<Scalar>*[ext.size()];
  for(unsigned int j = 0; j < ext.size(); j++) {
    neighbor_searches.get(ext[j]->get_mesh()->get_seq() - min_dg_mesh_seq)->set_quad_order(order);
    ext_fns[j] = neighbor_searches.get(ext[j]->get_mesh()->get_seq() - min_dg_mesh_seq)->init_ext_fn(ext[j]);
  }

  ExtData<Scalar>* ext_data = new ExtData<Scalar>;
  ext_data->fn = ext_fns;
  ext_data->nf = ext.size();

  return ext_data;
}

template<typename Scalar>
ExtData<Ord>* DiscreteProblem<Scalar>::init_ext_fns_ord(Hermes::vector<MeshFunction<Scalar>*> &ext, 
  LightArray<NeighborSearch<Scalar>*>& neighbor_searches)
{
  _F_;
  Func<Ord>** fake_ext_fns = new Func<Ord>*[ext.size()];
  for (unsigned int j = 0; j < ext.size(); j++)
    fake_ext_fns[j] = init_ext_fn_ord(neighbor_searches.get(ext[j]->get_mesh()->get_seq() - min_dg_mesh_seq), ext[j]);

  ExtData<Ord>* fake_ext = new ExtData<Ord>;
  fake_ext->fn = fake_ext_fns;
  fake_ext->nf = ext.size();

  return fake_ext;
}

template<typename Scalar>
Func<double>* DiscreteProblem<Scalar>::get_fn(PrecalcShapeset *fu, RefMap *rm, const int order)
{
  _F_;
  if(rm->is_jacobian_const()) {
    typename AssemblingCaches::KeyConst key(256 - fu->get_active_shape(), order, fu->get_transform(), fu->get_shapeset()->get_id(), rm->get_const_inv_ref_map());
    if(rm->get_active_element()->get_mode() == HERMES_MODE_TRIANGLE) {
      if(assembling_caches.const_cache_fn_triangles.find(key) == assembling_caches.const_cache_fn_triangles.end())
        assembling_caches.const_cache_fn_triangles[key] = init_fn(fu, rm, order);
      return assembling_caches.const_cache_fn_triangles[key];
    }
    else {
      if(assembling_caches.const_cache_fn_quads.find(key) == assembling_caches.const_cache_fn_quads.end())
        assembling_caches.const_cache_fn_quads[key] = init_fn(fu, rm, order);
      return assembling_caches.const_cache_fn_quads[key];
    }
  }
  else {
    typename AssemblingCaches::KeyNonConst key(256 - fu->get_active_shape(), order, 
      fu->get_transform(), fu->get_shapeset()->get_id());
    if(rm->get_active_element()->get_mode() == HERMES_MODE_TRIANGLE) {
      if(assembling_caches.cache_fn_triangles.find(key) == assembling_caches.cache_fn_triangles.end())
        assembling_caches.cache_fn_triangles[key] = init_fn(fu, rm, order);
      return assembling_caches.cache_fn_triangles[key];
    }
    else {
      if(assembling_caches.cache_fn_quads.find(key) == assembling_caches.cache_fn_quads.end())
        assembling_caches.cache_fn_quads[key] = init_fn(fu, rm, order);
      return assembling_caches.cache_fn_quads[key];
    }
  }
}

template<typename Scalar>
Func<Ord>* DiscreteProblem<Scalar>::get_fn_ord(const int order)
{
  _F_;
  assert(order >= 0);
  unsigned int cached_order = (unsigned int) order;
  if(!assembling_caches.cache_fn_ord.present(cached_order))
    assembling_caches.cache_fn_ord.add(init_fn_ord(cached_order), cached_order);
  return assembling_caches.cache_fn_ord.get(cached_order);
}

template<typename Scalar>
void DiscreteProblem<Scalar>::init_cache()
{
  _F_;
  for (int i = 0; i < g_max_quad + 1 + 4 * g_max_quad + 4; i++)
  {
    cache_e[i] = NULL;
    cache_jwt[i] = NULL;
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::delete_single_geom_cache(int order)
{
  if (cache_e[order] != NULL) {
    cache_e[order]->free();
    delete cache_e[order];
    cache_e[order] = NULL;
    delete [] cache_jwt[order];
  }
}

template<typename Scalar>
void DiscreteProblem<Scalar>::delete_cache()
{
  _F_;
  for (int i = 0; i < g_max_quad + 1 + 4 * g_max_quad + 4; i++)
  {
    if (cache_e[i] != NULL)
    {
      cache_e[i]->free(); delete cache_e[i];
      delete [] cache_jwt[i];
    }
  }

  for (typename std::map<typename AssemblingCaches::KeyNonConst, Func<double>*, typename AssemblingCaches::CompareNonConst>::const_iterator it = assembling_caches.cache_fn_quads.begin();
    it != assembling_caches.cache_fn_quads.end(); it++)
  {
    (it->second)->free_fn(); delete (it->second);
  }
  assembling_caches.cache_fn_quads.clear();

  for (typename std::map<typename AssemblingCaches::KeyNonConst, Func<double>*, typename AssemblingCaches::CompareNonConst>::const_iterator it = assembling_caches.cache_fn_triangles.begin();
    it != assembling_caches.cache_fn_triangles.end(); it++)
  {
    (it->second)->free_fn(); delete (it->second);
  }
  assembling_caches.cache_fn_triangles.clear();
}

template<typename Scalar>
DiscontinuousFunc<Ord>* DiscreteProblem<Scalar>::init_ext_fn_ord(NeighborSearch<Scalar>* ns, MeshFunction<Scalar>* fu)
{
  _F_;
  int inc = (fu->get_num_components() == 2) ? 1 : 0;
  int central_order = fu->get_edge_fn_order(ns->active_edge) + inc;
  int neighbor_order = fu->get_edge_fn_order(ns->neighbor_edge.local_num_of_edge) + inc;
  return new DiscontinuousFunc<Ord>(get_fn_ord(central_order), get_fn_ord(neighbor_order));
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form(MatrixFormVol<Scalar> *mfv, 
  Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, 
  RefMap *ru, RefMap *rv)
{
  _F_;
  Scalar result = 0;

  if (mfv->adapt_eval == false) {
    // Determine the integration order by parsing the form.
    int order = calc_order_matrix_form_vol(mfv, u_ext, fu, fv, ru, rv);
    // Perform non-adaptive numerical quadrature of order "order".
    result = eval_form_subelement(order, mfv, u_ext, fu, fv, ru, rv);
  }
  else {
    // Perform adaptive numerical quadrature starting with order = 2.
    // TODO: The choice of initial order matters a lot for efficiency,
    //       this needs more research.
    Shapeset* fu_shapeset = fu->get_shapeset();
    Shapeset* fv_shapeset = fv->get_shapeset();
    int fu_index = fu->get_active_shape();
    int fv_index = fv->get_active_shape();
    int fu_order = fu_shapeset->get_order(fu_index);
    int fv_order = fv_shapeset->get_order(fv_index);
    int fu_order_h = H2D_GET_H_ORDER(fu_order);
    int fu_order_v = H2D_GET_V_ORDER(fu_order);
    int fv_order_h = H2D_GET_H_ORDER(fv_order);
    int fv_order_v = H2D_GET_V_ORDER(fv_order);

    // FIXME - this needs more research.
    int order_init = (fu_order_h + fu_order_v) / 2 + (fv_order_h + fv_order_v) / 2;

    // Calculate initial value of the form on coarse element.
    Scalar result_init = eval_form_subelement(order_init, mfv, u_ext, fu, fv, ru, rv);

    //printf("Eval_form: initial result = %g\n", result_init);

    // Calculate the value of the form using adaptive quadrature.    
    result = eval_form_adaptive(order_init, result_init,
      mfv, u_ext, fu, fv, ru, rv);
  }

  return result;
}
template<typename Scalar>
void DiscreteProblem<Scalar>::eval_form(MultiComponentMatrixFormVol<Scalar>*mfv, 
  Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, 
  RefMap *ru, RefMap *rv, Hermes::vector<Scalar>& result)
{
  _F_;
  // Determine the integration order by parsing the form.
  int order = calc_order_matrix_form_vol(mfv, u_ext, fu, fv, ru, rv);

  // Evaluate the form using numerical quadrature of order "order".
  Quad2D* quad = fu->get_quad_2d();
  double3* pt = quad->get_points(order);
  int np = quad->get_num_points(order);

  // Init geometry and jacobian*weights.
  if (cache_e[order] == NULL)
  {
    cache_e[order] = init_geom_vol(ru, order);
    double* jac = NULL;
    if(!ru->is_jacobian_const()) 
      jac = ru->get_jacobian(order);
    cache_jwt[order] = new double[np];
    for(int i = 0; i < np; i++) {
      if(ru->is_jacobian_const())
        cache_jwt[order][i] = pt[i][2] * ru->get_const_jacobian();
      else
        cache_jwt[order][i] = pt[i][2] * jac[i];
    }
  }
  Geom<double>* e = cache_e[order];
  double* jwt = cache_jwt[order];

  // Values of the previous Newton iteration, shape functions 
  // and external functions in quadrature points.
  int prev_size = u_ext.size() - mfv->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + mfv->u_ext_offset] != NULL) 
        prev[i] = init_fn(u_ext[i + mfv->u_ext_offset], order);
      else 
        prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* u = get_fn(fu, ru, order);
  Func<double>* v = get_fn(fv, rv, order);

  ExtData<Scalar>* ext = init_ext_fns(mfv->ext, rv, order);

  // The actual calculation takes place here.
  mfv->value(np, jwt, prev, u, v, e, ext, result);

  for(unsigned int i = 0; i < result.size(); i++)
    result[i] *= mfv->scaling_factor;

  // Clean up.
  for(int i = 0; i < prev_size; i++)
    if (prev[i] != NULL) { 
      prev[i]->free_fn(); 
      delete prev[i]; 
    }
    delete [] prev;

    if (ext != NULL) {
      ext->free(); 
      delete ext;
    }
}
template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_matrix_form_vol(MatrixFormVol<Scalar> *mfv, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, RefMap *rv)
{
  _F_;
  // Order that will be returned.
  int order;

  if(is_fvm) 
    order = ru->get_inv_ref_order();
  else {
    int u_ext_length = u_ext.size();      // Number of external solutions.
    int u_ext_offset = mfv->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
    // and there will be only u_ext_length - u_ext_offset of them.

    // Increase for multi-valued shape functions.
    int inc = (fu->get_num_components() == 2) ? 1 : 0;

    // Order of solutions from the previous Newton iteration.
    Func<Ord>** oi = new Func<Ord>*[u_ext_length - u_ext_offset];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        if (u_ext[i + u_ext_offset] != NULL)
          oi[i] = get_fn_ord(u_ext[i + u_ext_offset]->get_fn_order() + inc);
        else
          oi[i] = get_fn_ord(0);
    else
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        oi[i] = get_fn_ord(0);

    // Order of shape functions.
    Func<Ord>* ou = get_fn_ord(fu->get_fn_order() + inc);
    Func<Ord>* ov = get_fn_ord(fv->get_fn_order() + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(mfv->ext);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    double fake_wt = 1.0;

    // Total order of the matrix form.
    Ord o = mfv->ord(1, &fake_wt, oi, ou, ov, &geom_ord, fake_ext);

    // Increase due to reference map.
    order = ru->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Cleanup.
    delete [] oi;

    if (fake_ext != NULL) {
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}
template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_matrix_form_vol(MultiComponentMatrixFormVol<Scalar>*mfv, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, RefMap *rv)
{
  _F_;
  // Order that will be returned.
  int order;

  if(is_fvm) 
    order = ru->get_inv_ref_order();
  else {
    int u_ext_length = u_ext.size();      // Number of external solutions.
    int u_ext_offset = mfv->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
    // and there will be only u_ext_length - u_ext_offset of them.

    // Increase for multi-valued shape functions.
    int inc = (fu->get_num_components() == 2) ? 1 : 0;

    // Order of solutions from the previous Newton iteration.
    Func<Ord>** oi = new Func<Ord>*[u_ext_length - u_ext_offset];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        if (u_ext[i + u_ext_offset] != NULL)
          oi[i] = get_fn_ord(u_ext[i + u_ext_offset]->get_fn_order() + inc);
        else
          oi[i] = get_fn_ord(0);
    else
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        oi[i] = get_fn_ord(0);

    // Order of shape functions.
    Func<Ord>* ou = get_fn_ord(fu->get_fn_order() + inc);
    Func<Ord>* ov = get_fn_ord(fv->get_fn_order() + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(mfv->ext);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    double fake_wt = 1.0;

    // Total order of the matrix form.
    Ord o = mfv->ord(1, &fake_wt, oi, ou, ov, &geom_ord, fake_ext);

    // Increase due to reference map.
    order = ru->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Cleanup.
    delete [] oi;

    if (fake_ext != NULL) {
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}
template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form_subelement(int order, MatrixFormVol<Scalar> *mfv, 
  Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, 
  RefMap *ru, RefMap *rv)
{
  // Evaluate the form using numerical quadrature of order "order".
  Quad2D* quad = fu->get_quad_2d();
  double3* pt = quad->get_points(order);
  int np = quad->get_num_points(order);

  // Init geometry and jacobian*weights.
  if (cache_e[order] == NULL)
  {
    cache_e[order] = init_geom_vol(ru, order);
    double* jac = NULL;
    if(!ru->is_jacobian_const()) 
      jac = ru->get_jacobian(order);
    cache_jwt[order] = new double[np];
    for(int i = 0; i < np; i++) {
      if(ru->is_jacobian_const())
        cache_jwt[order][i] = pt[i][2] * ru->get_const_jacobian();
      else
        cache_jwt[order][i] = pt[i][2] * jac[i];
    }
  }
  Geom<double>* e = cache_e[order];
  double* jwt = cache_jwt[order];

  // Values of the previous Newton iteration, shape functions 
  // and external functions in quadrature points.
  int prev_size = u_ext.size() - mfv->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + mfv->u_ext_offset] != NULL) 
        prev[i] = init_fn(u_ext[i + mfv->u_ext_offset], order);
      else 
        prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* u = get_fn(fu, ru, order);
  Func<double>* v = get_fn(fv, rv, order);

  ExtData<Scalar>* ext = init_ext_fns(mfv->ext, rv, order);

  // The actual calculation takes place here.
  Scalar res = mfv->value(np, jwt, prev, u, v, e, ext) * mfv->scaling_factor;

  // Clean up.
  for(int i = 0; i < prev_size; i++)
    if (prev[i] != NULL) { 
      prev[i]->free_fn(); 
      delete prev[i]; 
    }
    delete [] prev;

    if (ext != NULL) {
      ext->free(); 
      delete ext;
    }

    return res;
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form_adaptive(int order_init, Scalar result_init,
  MatrixFormVol<Scalar> *mfv, 
  Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, 
  RefMap *ru, RefMap *rv)
{
  // Initialize set of all transformable entities.
  std::set<Transformable *> transformable_entities;
  transformable_entities.insert(fu);
  transformable_entities.insert(fv);
  transformable_entities.insert(ru);
  transformable_entities.insert(rv);
  transformable_entities.insert(mfv->ext.begin(), mfv->ext.end());
  transformable_entities.insert(u_ext.begin(), u_ext.end());

  Scalar result = 0;

  int order_increase = mfv->adapt_order_increase;

  Scalar subs_value[4];

  Scalar result_current_subelements = 0;

  // Clear of the geometry cache for the current active subelement. This needs to be done before AND after
  // as the further division to subelements must be only local.
  this->delete_single_geom_cache(order_init + order_increase);
  for(unsigned int sons_i = 0; sons_i < 4; sons_i++) {
    // Push the transformation to the current son to all functions and reference mappings involved.
    Transformable::push_transforms(transformable_entities, sons_i);

    // The actual calculation.
    subs_value[sons_i] = eval_form_subelement(order_init + order_increase, mfv, 
      u_ext, fu, fv, ru, rv);

    // Clear of the geometry cache for the current active subelement.
    this->delete_single_geom_cache(order_init + order_increase);

    //printf("subs_value[%d] = %g\n", sons_i, subs_value[sons_i]);

    result_current_subelements += subs_value[sons_i];

    // Reset the transformation.
    Transformable::pop_transforms(transformable_entities);
  }

  // Tolerance checking.
  // First, if the result is negligible.
  if (std::abs(result_current_subelements) < 1e-6)
    return result_current_subelements;

  // Relative error.
  double rel_error = std::abs(result_current_subelements - result_init) / std::abs(result_current_subelements);

  if (rel_error < mfv->adapt_rel_error_tol)
    // Relative error in tolerance.
    return result_current_subelements;
  else {
    Scalar result_recursion = 0;
    // Relative error exceeds allowed tolerance: Call the function 
    // eval_form_adaptive() in each subelement, with initial values
    // subs_value[sons_i].

    // Call the function eval_form_adaptive() recursively in all sons.
    for(unsigned int sons_i = 0; sons_i < 4; sons_i++) {
      // Push the transformation to the current son to all functions and reference mappings involved.
      Transformable::push_transforms(transformable_entities, sons_i);

      // Recursion.
      subs_value[sons_i] = eval_form_adaptive(order_init + order_increase, subs_value[sons_i],
        mfv, u_ext, fu, fv, ru, rv);

      result_recursion += subs_value[sons_i];

      // Reset the transformation.
      Transformable::pop_transforms(transformable_entities);
    }

    return result_recursion;
  }
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form(VectorFormVol<Scalar> *vfv, 
  Hermes::vector<Solution<Scalar>*> u_ext, 
  PrecalcShapeset *fv, RefMap *rv)
{
  _F_;
  Scalar result = 0;

  if (vfv->adapt_eval == false) {
    // Determine the integration order by parsing the form.
    int order = calc_order_vector_form_vol(vfv, u_ext, fv, rv);
    // Perform non-adaptive numerical quadrature of order "order".
    result = eval_form_subelement(order, vfv, u_ext, fv, rv);
  }
  else {
    // Perform adaptive numerical quadrature starting with order = 2.
    // TODO: The choice of initial order matters a lot for efficiency,
    //       this needs more research.
    Shapeset* fv_shapeset = fv->get_shapeset();
    int fv_index = fv->get_active_shape();
    int fv_order = fv_shapeset->get_order(fv_index);
    int fv_order_h = H2D_GET_H_ORDER(fv_order);
    int fv_order_v = H2D_GET_V_ORDER(fv_order);

    // FIXME - this needs more research.
    int order_init = (fv_order_h + fv_order_v) / 2;

    // Calculate initial value of the form on coarse element.
    Scalar result_init = eval_form_subelement(order_init, vfv, u_ext, fv, rv);

    //printf("Eval_form: initial result = %g\n", result_init);

    // Calculate the value of the form using adaptive quadrature.    
    result = eval_form_adaptive(order_init, result_init,
      vfv, u_ext, fv, rv);
  }

  return result;
}
template<typename Scalar>
void DiscreteProblem<Scalar>::eval_form(MultiComponentVectorFormVol<Scalar>*vfv, 
  Hermes::vector<Solution<Scalar>*> u_ext, 
  PrecalcShapeset *fv, RefMap *rv, Hermes::vector<Scalar>& result)
{
  _F_;
  // Determine the integration order by parsing the form.
  int order = calc_order_vector_form_vol(vfv, u_ext, fv, rv);
  // Evaluate the form using numerical quadrature of order "order".
  Quad2D* quad = fv->get_quad_2d();
  double3* pt = quad->get_points(order);
  int np = quad->get_num_points(order);

  // Init geometry and jacobian*weights.
  if (cache_e[order] == NULL)
  {
    cache_e[order] = init_geom_vol(rv, order);
    double* jac = NULL;
    if(!rv->is_jacobian_const()) 
      jac = rv->get_jacobian(order);
    cache_jwt[order] = new double[np];
    for(int i = 0; i < np; i++) {
      if(rv->is_jacobian_const())
        cache_jwt[order][i] = pt[i][2] * rv->get_const_jacobian();
      else
        cache_jwt[order][i] = pt[i][2] * jac[i];
    }
  }
  Geom<double>* e = cache_e[order];
  double* jwt = cache_jwt[order];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - vfv->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + vfv->u_ext_offset] != NULL) 
        prev[i] = init_fn(u_ext[i + vfv->u_ext_offset], order);
      else 
        prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* v = get_fn(fv, rv, order);
  ExtData<Scalar>* ext = init_ext_fns(vfv->ext, rv, order);

  // The actual calculation takes place here.
  vfv->value(np, jwt, prev, v, e, ext, result);

  for(unsigned int i = 0; i < result.size(); i++)
    result[i] *= vfv->scaling_factor;

  // Clean up.
  for(int i = 0; i < prev_size; i++)
    if (prev[i] != NULL) { 
      prev[i]->free_fn(); 
      delete prev[i]; 
    }
    delete [] prev;

    if (ext != NULL) {
      ext->free(); 
      delete ext;
    }
}

template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_vector_form_vol(VectorFormVol<Scalar> *vfv, 
  Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv)
{
  _F_;
  // Order that will be returned.
  int order;

  if(is_fvm) 
    order = rv->get_inv_ref_order();
  else {
    int u_ext_length = u_ext.size();      // Number of external solutions.
    int u_ext_offset = vfv->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
    // and there will be only u_ext_length - u_ext_offset of them.

    // Increase for multi-valued shape functions.
    int inc = (fv->get_num_components() == 2) ? 1 : 0;

    // Order of solutions from the previous Newton iteration.
    Func<Ord>** oi = new Func<Ord>*[u_ext_length - u_ext_offset];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        if (u_ext[i + u_ext_offset] != NULL)
          oi[i] = get_fn_ord(u_ext[i + u_ext_offset]->get_fn_order() + inc);
        else
          oi[i] = get_fn_ord(0);
    else
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        oi[i] = get_fn_ord(0);

    // Order of the shape function.
    Func<Ord>* ov = get_fn_ord(fv->get_fn_order() + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(vfv->ext);

    // Order of geometric attributes (eg. for multiplication of 
    // a solution with coordinates, normals, etc.).
    double fake_wt = 1.0;

    // Total order of the vector form.
    Ord o = vfv->ord(1, &fake_wt, oi, ov, &geom_ord, fake_ext);

    // Increase due to reference map.
    order = rv->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Cleanup.
    delete [] oi;

    if (fake_ext != NULL) {
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}
template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_vector_form_vol(MultiComponentVectorFormVol<Scalar>*vfv, 
  Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv)
{
  _F_;
  // Order that will be returned.
  int order;

  if(is_fvm) 
    order = rv->get_inv_ref_order();
  else {
    int u_ext_length = u_ext.size();      // Number of external solutions.
    int u_ext_offset = vfv->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
    // and there will be only u_ext_length - u_ext_offset of them.

    // Increase for multi-valued shape functions.
    int inc = (fv->get_num_components() == 2) ? 1 : 0;

    // Order of solutions from the previous Newton iteration.
    Func<Ord>** oi = new Func<Ord>*[u_ext_length - u_ext_offset];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        if (u_ext[i + u_ext_offset] != NULL)
          oi[i] = get_fn_ord(u_ext[i + u_ext_offset]->get_fn_order() + inc);
        else
          oi[i] = get_fn_ord(0);
    else
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        oi[i] = get_fn_ord(0);

    // Order of the shape function.
    Func<Ord>* ov = get_fn_ord(fv->get_fn_order() + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(vfv->ext);

    // Order of geometric attributes (eg. for multiplication of 
    // a solution with coordinates, normals, etc.).
    double fake_wt = 1.0;

    // Total order of the vector form.
    Ord o = vfv->ord(1, &fake_wt, oi, ov, &geom_ord, fake_ext);

    // Increase due to reference map.
    order = rv->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Cleanup.
    delete [] oi;

    if (fake_ext != NULL) {
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form_subelement(int order, VectorFormVol<Scalar> *vfv, 
  Hermes::vector<Solution<Scalar>*> u_ext, 
  PrecalcShapeset *fv, RefMap *rv)
{
  _F_;
  // Evaluate the form using numerical quadrature of order "order".
  Quad2D* quad = fv->get_quad_2d();
  double3* pt = quad->get_points(order);
  int np = quad->get_num_points(order);

  // Init geometry and jacobian*weights.
  if (cache_e[order] == NULL)
  {
    cache_e[order] = init_geom_vol(rv, order);
    double* jac = NULL;
    if(!rv->is_jacobian_const()) 
      jac = rv->get_jacobian(order);
    cache_jwt[order] = new double[np];
    for(int i = 0; i < np; i++) {
      if(rv->is_jacobian_const())
        cache_jwt[order][i] = pt[i][2] * rv->get_const_jacobian();
      else
        cache_jwt[order][i] = pt[i][2] * jac[i];
    }
  }
  Geom<double>* e = cache_e[order];
  double* jwt = cache_jwt[order];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - vfv->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + vfv->u_ext_offset] != NULL) 
        prev[i] = init_fn(u_ext[i + vfv->u_ext_offset], order);
      else 
        prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* v = get_fn(fv, rv, order);
  ExtData<Scalar>* ext = init_ext_fns(vfv->ext, rv, order);

  // The actual calculation takes place here.
  Scalar res = vfv->value(np, jwt, prev, v, e, ext) * vfv->scaling_factor;

  // Clean up.
  for(int i = 0; i < prev_size; i++)
    if (prev[i] != NULL) { 
      prev[i]->free_fn(); 
      delete prev[i]; 
    }
    delete [] prev;

    if (ext != NULL) {
      ext->free(); 
      delete ext;
    }

    return res;
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form_adaptive(int order_init, Scalar result_init,
  VectorFormVol<Scalar> *vfv, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv)
{
  // Initialize set of all transformable entities.
  std::set<Transformable *> transformable_entities;
  transformable_entities.insert(fv);
  transformable_entities.insert(rv);
  transformable_entities.insert(vfv->ext.begin(), vfv->ext.end());
  transformable_entities.insert(u_ext.begin(), u_ext.end());

  Scalar result = 0;

  int order_increase = vfv->adapt_order_increase;

  Scalar subs_value[4];

  Scalar result_current_subelements = 0;

  // Clear of the geometry cache for the current active subelement. This needs to be done before AND after
  // as the further division to subelements must be only local.
  this->delete_single_geom_cache(order_init + order_increase);
  for(unsigned int sons_i = 0; sons_i < 4; sons_i++) {
    // Push the transformation to the current son to all functions and reference mappings involved.
    Transformable::push_transforms(transformable_entities, sons_i);

    // The actual calculation.
    subs_value[sons_i] = eval_form_subelement(order_init + order_increase, vfv, 
      u_ext, fv, rv);

    // Clear of the geometry cache for the current active subelement.
    this->delete_single_geom_cache(order_init + order_increase);

    //printf("subs_value[%d] = %g\n", sons_i, subs_value[sons_i]);

    result_current_subelements += subs_value[sons_i];

    // Reset the transformation.
    Transformable::pop_transforms(transformable_entities);
  }

  // Tolerance checking.
  // First, if the result is negligible.
  if (std::abs(result_current_subelements) < 1e-6)
    return result_current_subelements;

  // Relative error.
  double rel_error = std::abs(result_current_subelements - result_init) / std::abs(result_current_subelements);

  if (rel_error < vfv->adapt_rel_error_tol)
    // Relative error in tolerance.
    return result_current_subelements;
  else {
    Scalar result_recursion = 0;
    // Relative error exceeds allowed tolerance: Call the function 
    // eval_form_adaptive() in each subelement, with initial values
    // subs_value[sons_i].

    // Call the function eval_form_adaptive() recursively in all sons.
    for(unsigned int sons_i = 0; sons_i < 4; sons_i++) {
      // Push the transformation to the current son to all functions and reference mappings involved.
      Transformable::push_transforms(transformable_entities, sons_i);

      // Recursion.
      subs_value[sons_i] = eval_form_adaptive(order_init + order_increase, subs_value[sons_i],
        vfv, u_ext, fv, rv);

      result_recursion += subs_value[sons_i];

      // Reset the transformation.
      Transformable::pop_transforms(transformable_entities);
    }

    return result_recursion;
  }
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form(MatrixFormSurf<Scalar> *mfs, 
  Hermes::vector<Solution<Scalar>*> u_ext, 
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, RefMap *rv, SurfPos* surf_pos)
{
  _F_;
  Scalar result = 0;

  if (mfs->adapt_eval == false) {
    // Determine the integration order by parsing the form.
    int order = calc_order_matrix_form_surf(mfs, u_ext, fu, fv, ru, rv, surf_pos);
    // Perform non-adaptive numerical quadrature of order "order".
    result = eval_form_subelement(order, mfs, u_ext, fu, fv, ru, rv, surf_pos);
  }
  else {
    // Perform adaptive numerical quadrature starting with order = 2.
    // TODO: The choice of initial order matters a lot for efficiency,
    //       this needs more research.
    int fu_order = fu->get_edge_fn_order(surf_pos->surf_num);
    int fv_order = fv->get_edge_fn_order(surf_pos->surf_num);

    // FIXME - this needs more research.
    int order_init = fu_order + fv_order;

    // Calculate initial value of the form on coarse element.
    Scalar result_init = eval_form_subelement(order_init, mfs, u_ext, fu, fv, ru, rv, surf_pos);

    // Calculate the value of the form using adaptive quadrature.    
    result = eval_form_adaptive(order_init, result_init,
      mfs, u_ext, fu, fv, ru, rv, surf_pos);
  }

  return result;
}
template<typename Scalar>
void DiscreteProblem<Scalar>::eval_form(MultiComponentMatrixFormSurf<Scalar>*mfs, 
  Hermes::vector<Solution<Scalar>*> u_ext, 
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, RefMap *rv, SurfPos* surf_pos, Hermes::vector<Scalar>& result)
{
  _F_;
  // Determine the integration order by parsing the form.
  int order = calc_order_matrix_form_surf(mfs, u_ext, fu, fv, ru, rv, surf_pos);
  // Evaluate the form using numerical quadrature of order "order".
  Quad2D* quad = fu->get_quad_2d();

  int eo = quad->get_edge_points(surf_pos->surf_num, order);
  double3* pt = quad->get_points(eo);
  int np = quad->get_num_points(eo);

  // Init geometry and jacobian*weights.
  if (cache_e[eo] == NULL)
  {
    cache_e[eo] = init_geom_surf(ru, surf_pos, eo);
    double3* tan = ru->get_tangent(surf_pos->surf_num, eo);
    cache_jwt[eo] = new double[np];
    for(int i = 0; i < np; i++)
      cache_jwt[eo][i] = pt[i][2] * tan[i][2];
  }
  Geom<double>* e = cache_e[eo];
  double* jwt = cache_jwt[eo];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - mfs->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + mfs->u_ext_offset] != NULL) 
        prev[i] = init_fn(u_ext[i + mfs->u_ext_offset], eo);
      else 
        prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* u = get_fn(fu, ru, eo);
  Func<double>* v = get_fn(fv, rv, eo);
  ExtData<Scalar>* ext = init_ext_fns(mfs->ext, rv, eo);

  // The actual calculation takes place here.
  mfs->value(np, jwt, prev, u, v, e, ext, result);

  for(unsigned int i = 0; i < result.size(); i++)
    result[i] *= mfs->scaling_factor * 0.5;

  // Clean up.
  for(int i = 0; i < prev_size; i++)
    if (prev[i] != NULL) { 
      prev[i]->free_fn(); 
      delete prev[i]; 
    }
    delete [] prev;

    if (ext != NULL) {
      ext->free(); 
      delete ext;
    }
}

template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_matrix_form_surf(MatrixFormSurf<Scalar> *mfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, RefMap *rv, SurfPos* surf_pos)
{
  _F_;
  // Order that will be returned.
  int order;

  if(is_fvm)
    order = ru->get_inv_ref_order();
  else {
    int u_ext_length = u_ext.size();      // Number of external solutions.
    int u_ext_offset = mfs->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
    // and there will be only u_ext_length - u_ext_offset of them.

    // Increase for multi-valued shape functions.
    int inc = (fu->get_num_components() == 2) ? 1 : 0;

    // Order of solutions from the previous Newton iteration.
    Func<Ord>** oi = new Func<Ord>*[u_ext_length - u_ext_offset];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        if (u_ext[i + u_ext_offset] != NULL)
          oi[i] = get_fn_ord(u_ext[i + u_ext_offset]->get_edge_fn_order(surf_pos->surf_num) + inc);
        else
          oi[i] = get_fn_ord(0);
    else
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        oi[i] = get_fn_ord(0);

    // Order of shape functions.
    Func<Ord>* ou = get_fn_ord(fu->get_edge_fn_order(surf_pos->surf_num) + inc);
    Func<Ord>* ov = get_fn_ord(fv->get_edge_fn_order(surf_pos->surf_num) + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(mfs->ext, surf_pos->surf_num);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    double fake_wt = 1.0;

    // Total order of the matrix form.
    Ord o = mfs->ord(1, &fake_wt, oi, ou, ov, &geom_ord, fake_ext);

    // Increase due to reference map.
    order = ru->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Cleanup.
    delete [] oi;

    if (fake_ext != NULL) {
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}
template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_matrix_form_surf(MultiComponentMatrixFormSurf<Scalar>*mfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, RefMap *rv, SurfPos* surf_pos)
{
  _F_;
  // Order that will be returned.
  int order;

  if(is_fvm)
    order = ru->get_inv_ref_order();
  else {
    int u_ext_length = u_ext.size();      // Number of external solutions.
    int u_ext_offset = mfs->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
    // and there will be only u_ext_length - u_ext_offset of them.

    // Increase for multi-valued shape functions.
    int inc = (fu->get_num_components() == 2) ? 1 : 0;

    // Order of solutions from the previous Newton iteration.
    Func<Ord>** oi = new Func<Ord>*[u_ext_length - u_ext_offset];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        if (u_ext[i + u_ext_offset] != NULL)
          oi[i] = get_fn_ord(u_ext[i + u_ext_offset]->get_edge_fn_order(surf_pos->surf_num) + inc);
        else
          oi[i] = get_fn_ord(0);
    else
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        oi[i] = get_fn_ord(0);

    // Order of shape functions.
    Func<Ord>* ou = get_fn_ord(fu->get_edge_fn_order(surf_pos->surf_num) + inc);
    Func<Ord>* ov = get_fn_ord(fv->get_edge_fn_order(surf_pos->surf_num) + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(mfs->ext, surf_pos->surf_num);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    double fake_wt = 1.0;

    // Total order of the matrix form.
    Ord o = mfs->ord(1, &fake_wt, oi, ou, ov, &geom_ord, fake_ext);

    // Increase due to reference map.
    order = ru->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Cleanup.
    delete [] oi;

    if (fake_ext != NULL) {
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form_subelement(int order, MatrixFormSurf<Scalar> *mfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, RefMap *rv, SurfPos* surf_pos)
{
  _F_;
  // Evaluate the form using numerical quadrature of order "order".
  Quad2D* quad = fu->get_quad_2d();

  int eo = quad->get_edge_points(surf_pos->surf_num, order);
  double3* pt = quad->get_points(eo);
  int np = quad->get_num_points(eo);

  // Init geometry and jacobian*weights.
  if (cache_e[eo] == NULL)
  {
    cache_e[eo] = init_geom_surf(ru, surf_pos, eo);
    double3* tan = ru->get_tangent(surf_pos->surf_num, eo);
    cache_jwt[eo] = new double[np];
    for(int i = 0; i < np; i++)
      cache_jwt[eo][i] = pt[i][2] * tan[i][2];
  }
  Geom<double>* e = cache_e[eo];
  double* jwt = cache_jwt[eo];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - mfs->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + mfs->u_ext_offset] != NULL) 
        prev[i] = init_fn(u_ext[i + mfs->u_ext_offset], eo);
      else 
        prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* u = get_fn(fu, ru, eo);
  Func<double>* v = get_fn(fv, rv, eo);
  ExtData<Scalar>* ext = init_ext_fns(mfs->ext, rv, eo);

  // The actual calculation takes place here.
  Scalar res = mfs->value(np, jwt, prev, u, v, e, ext) * mfs->scaling_factor;

  // Clean up.
  for(int i = 0; i < prev_size; i++)
    if (prev[i] != NULL) { 
      prev[i]->free_fn(); 
      delete prev[i]; 
    }
    delete [] prev;

    if (ext != NULL) {
      ext->free(); 
      delete ext;
    }

    return 0.5 * res; // Edges are parameterized from 0 to 1 while integration weights
    // are defined in (-1, 1). Thus multiplying with 0.5 to correct
    // the weights.
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form_adaptive(int order_init, Scalar result_init,
  MatrixFormSurf<Scalar> *mfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, RefMap *rv, SurfPos* surf_pos)
{
  // Initialize set of all transformable entities.
  std::set<Transformable *> transformable_entities;
  transformable_entities.insert(fu);
  transformable_entities.insert(fv);
  transformable_entities.insert(ru);
  transformable_entities.insert(rv);
  transformable_entities.insert(mfs->ext.begin(), mfs->ext.end());
  transformable_entities.insert(u_ext.begin(), u_ext.end());

  Scalar result = 0;

  int order_increase = mfs->adapt_order_increase;

  Scalar subs_value[4];

  Scalar result_current_subelements = 0;

  // Clear of the geometry cache for the current active subelement. This needs to be done before AND after
  // as the further division to subelements must be only local.
  this->delete_single_geom_cache(order_init + order_increase);
  for(unsigned int sons_i = 0; sons_i < 4; sons_i++) {
    // Push the transformation to the current son to all functions and reference mappings involved.
    Transformable::push_transforms(transformable_entities, sons_i);

    // The actual calculation.
    subs_value[sons_i] = eval_form_subelement(order_init + order_increase, mfs, 
      u_ext, fu, fv, ru, rv, surf_pos);

    // Clear of the geometry cache for the current active subelement.
    this->delete_single_geom_cache(order_init + order_increase);

    //printf("subs_value[%d] = %g\n", sons_i, subs_value[sons_i]);

    result_current_subelements += subs_value[sons_i];

    // Reset the transformation.
    Transformable::pop_transforms(transformable_entities);
  }

  // Tolerance checking.
  // First, if the result is negligible.
  if (std::abs(result_current_subelements) < 1e-6)
    return result_current_subelements;

  // Relative error.
  double rel_error = std::abs(result_current_subelements - result_init) / std::abs(result_current_subelements);

  if (rel_error < mfs->adapt_rel_error_tol)
    // Relative error in tolerance.
    return result_current_subelements;
  else {
    Scalar result_recursion = 0;
    // Relative error exceeds allowed tolerance: Call the function 
    // eval_form_adaptive() in each subelement, with initial values
    // subs_value[sons_i].

    // Call the function eval_form_adaptive() recursively in all sons.
    for(unsigned int sons_i = 0; sons_i < 4; sons_i++) {
      // Push the transformation to the current son to all functions and reference mappings involved.
      Transformable::push_transforms(transformable_entities, sons_i);

      // Recursion.
      subs_value[sons_i] = eval_form_adaptive(order_init + order_increase, subs_value[sons_i],
        mfs, u_ext, fu, fv, ru, rv, surf_pos);

      result_recursion += subs_value[sons_i];

      // Reset the transformation.
      Transformable::pop_transforms(transformable_entities);
    }

    return result_recursion;
  }
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form(VectorFormSurf<Scalar> *vfs, 
  Hermes::vector<Solution<Scalar>*> u_ext, 
  PrecalcShapeset *fv, RefMap *rv, SurfPos* surf_pos)
{
  _F_;
  Scalar result = 0;

  if (vfs->adapt_eval == false) {
    // Determine the integration order by parsing the form.
    int order = calc_order_vector_form_surf(vfs, u_ext, fv, rv, surf_pos);
    // Perform non-adaptive numerical quadrature of order "order".
    result = eval_form_subelement(order, vfs, u_ext, fv, rv, surf_pos);
  }
  else {
    // Perform adaptive numerical quadrature starting with order = 2.
    // TODO: The choice of initial order matters a lot for efficiency,
    //       this needs more research.
    int fv_order = fv->get_edge_fn_order(surf_pos->surf_num);

    // FIXME - this needs more research.
    int order_init = fv_order;

    // Calculate initial value of the form on coarse element.
    Scalar result_init = eval_form_subelement(order_init, vfs, u_ext, fv, rv, surf_pos);

    //printf("Eval_form: initial result = %g\n", result_init);

    // Calculate the value of the form using adaptive quadrature.    
    result = eval_form_adaptive(order_init, result_init,
      vfs, u_ext, fv, rv, surf_pos);
  }

  return result;
}
template<typename Scalar>
void DiscreteProblem<Scalar>::eval_form(MultiComponentVectorFormSurf<Scalar>*vfs, 
  Hermes::vector<Solution<Scalar>*> u_ext, 
  PrecalcShapeset *fv, RefMap *rv, SurfPos* surf_pos, Hermes::vector<Scalar>& result)
{
  _F_;
  // Determine the integration order by parsing the form.
  int order = calc_order_vector_form_surf(vfs, u_ext, fv, rv, surf_pos);
  // Evaluate the form using numerical quadrature of order "order".
  Quad2D* quad = fv->get_quad_2d();

  int eo = quad->get_edge_points(surf_pos->surf_num, order);
  double3* pt = quad->get_points(eo);
  int np = quad->get_num_points(eo);

  // Init geometry and jacobian*weights.
  if (cache_e[eo] == NULL) {
    cache_e[eo] = init_geom_surf(rv, surf_pos, eo);
    double3* tan = rv->get_tangent(surf_pos->surf_num, eo);
    cache_jwt[eo] = new double[np];
    for(int i = 0; i < np; i++)
      cache_jwt[eo][i] = pt[i][2] * tan[i][2];
  }
  Geom<double>* e = cache_e[eo];
  double* jwt = cache_jwt[eo];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - vfs->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + vfs->u_ext_offset] != NULL) 
        prev[i] = init_fn(u_ext[i + vfs->u_ext_offset], eo);
      else 
        prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* v = get_fn(fv, rv, eo);
  ExtData<Scalar>* ext = init_ext_fns(vfs->ext, rv, eo);

  // The actual calculation takes place here.
  vfs->value(np, jwt, prev, v, e, ext, result);

  for(unsigned int i = 0; i < result.size(); i++)
    result[i] *= vfs->scaling_factor * 0.5;

  // Clean up.
  for(int i = 0; i < prev_size; i++)
    if (prev[i] != NULL) { 
      prev[i]->free_fn(); 
      delete prev[i]; 
    }
    delete [] prev;

    if (ext != NULL) {
      ext->free(); 
      delete ext;
    }
}

template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_vector_form_surf(VectorFormSurf<Scalar> *vfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv, SurfPos* surf_pos)
{
  _F_;
  // Order that will be returned.
  int order;

  if(is_fvm) 
    order = rv->get_inv_ref_order();
  else {
    int u_ext_length = u_ext.size();      // Number of external solutions.
    int u_ext_offset = vfs->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
    // and there will be only u_ext_length - u_ext_offset of them.

    // Increase for multi-valued shape functions.
    int inc = (fv->get_num_components() == 2) ? 1 : 0;

    // Order of solutions from the previous Newton iteration.
    Func<Ord>** oi = new Func<Ord>*[u_ext_length - u_ext_offset];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        if (u_ext[i + u_ext_offset] != NULL)
          oi[i] = get_fn_ord(u_ext[i]->get_edge_fn_order(surf_pos->surf_num) + inc);
        else
          oi[i] = get_fn_ord(0);
    else
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        oi[i] = get_fn_ord(0);

    // Order of the shape function.
    Func<Ord>* ov = get_fn_ord(fv->get_edge_fn_order(surf_pos->surf_num) + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(vfs->ext);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    double fake_wt = 1.0;

    // Total order of the vector form.
    Ord o = vfs->ord(1, &fake_wt, oi, ov, &geom_ord, fake_ext);

    // Increase due to reference map.
    order = rv->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Cleanup.
    delete [] oi;

    if (fake_ext != NULL) {
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}
template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_vector_form_surf(MultiComponentVectorFormSurf<Scalar>*vfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv, SurfPos* surf_pos)
{
  _F_;
  // Order that will be returned.
  int order;

  if(is_fvm) 
    order = rv->get_inv_ref_order();
  else {
    int u_ext_length = u_ext.size();      // Number of external solutions.
    int u_ext_offset = vfs->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
    // and there will be only u_ext_length - u_ext_offset of them.

    // Increase for multi-valued shape functions.
    int inc = (fv->get_num_components() == 2) ? 1 : 0;

    // Order of solutions from the previous Newton iteration.
    Func<Ord>** oi = new Func<Ord>*[u_ext_length - u_ext_offset];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        if (u_ext[i + u_ext_offset] != NULL)
          oi[i] = get_fn_ord(u_ext[i]->get_edge_fn_order(surf_pos->surf_num) + inc);
        else
          oi[i] = get_fn_ord(0);
    else
      for(int i = 0; i < u_ext_length - u_ext_offset; i++)
        oi[i] = get_fn_ord(0);

    // Order of the shape function.
    Func<Ord>* ov = get_fn_ord(fv->get_edge_fn_order(surf_pos->surf_num) + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(vfs->ext);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    double fake_wt = 1.0;

    // Total order of the vector form.
    Ord o = vfs->ord(1, &fake_wt, oi, ov, &geom_ord, fake_ext);

    // Increase due to reference map.
    order = rv->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Cleanup.
    delete [] oi;

    if (fake_ext != NULL) {
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form_subelement(int order, VectorFormSurf<Scalar> *vfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv, SurfPos* surf_pos)
{
  _F_;
  // Evaluate the form using numerical quadrature of order "order".
  Quad2D* quad = fv->get_quad_2d();

  int eo = quad->get_edge_points(surf_pos->surf_num, order);
  double3* pt = quad->get_points(eo);
  int np = quad->get_num_points(eo);

  // Init geometry and jacobian*weights.
  if (cache_e[eo] == NULL) {
    cache_e[eo] = init_geom_surf(rv, surf_pos, eo);
    double3* tan = rv->get_tangent(surf_pos->surf_num, eo);
    cache_jwt[eo] = new double[np];
    for(int i = 0; i < np; i++)
      cache_jwt[eo][i] = pt[i][2] * tan[i][2];
  }
  Geom<double>* e = cache_e[eo];
  double* jwt = cache_jwt[eo];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - vfs->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + vfs->u_ext_offset] != NULL) 
        prev[i] = init_fn(u_ext[i + vfs->u_ext_offset], eo);
      else 
        prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* v = get_fn(fv, rv, eo);
  ExtData<Scalar>* ext = init_ext_fns(vfs->ext, rv, eo);

  // The actual calculation takes place here.
  Scalar res = vfs->value(np, jwt, prev, v, e, ext) * vfs->scaling_factor;

  // Clean up.
  for(int i = 0; i < prev_size; i++)
    if (prev[i] != NULL) { 
      prev[i]->free_fn(); 
      delete prev[i]; 
    }
    delete [] prev;

    if (ext != NULL) {
      ext->free(); 
      delete ext;
    }

    return 0.5 * res; // Edges are parameterized from 0 to 1 while integration weights
    // are defined in (-1, 1). Thus multiplying with 0.5 to correct
    // the weights.
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_form_adaptive(int order_init, Scalar result_init,
  VectorFormSurf<Scalar> *vfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv, SurfPos* surf_pos)
{
  // Initialize set of all transformable entities.
  std::set<Transformable *> transformable_entities;
  transformable_entities.insert(fv);
  transformable_entities.insert(rv);
  transformable_entities.insert(vfs->ext.begin(), vfs->ext.end());
  transformable_entities.insert(u_ext.begin(), u_ext.end());

  Scalar result = 0;

  int order_increase = vfs->adapt_order_increase;

  Scalar subs_value[4];

  Scalar result_current_subelements = 0;

  // Clear of the geometry cache for the current active subelement. This needs to be done before AND after
  // as the further division to subelements must be only local.
  this->delete_single_geom_cache(order_init + order_increase);
  for(unsigned int sons_i = 0; sons_i < 4; sons_i++) {
    // Push the transformation to the current son to all functions and reference mappings involved.
    Transformable::push_transforms(transformable_entities, sons_i);

    // The actual calculation.
    subs_value[sons_i] = eval_form_subelement(order_init + order_increase, vfs, 
      u_ext, fv, rv, surf_pos);

    // Clear of the geometry cache for the current active subelement.
    this->delete_single_geom_cache(order_init + order_increase);

    //printf("subs_value[%d] = %g\n", sons_i, subs_value[sons_i]);

    result_current_subelements += subs_value[sons_i];

    // Reset the transformation.
    Transformable::pop_transforms(transformable_entities);
  }

  // Tolerance checking.
  // First, if the result is negligible.
  if (std::abs(result_current_subelements) < 1e-6)
    return result_current_subelements;

  // Relative error.
  double rel_error = std::abs(result_current_subelements - result_init) / std::abs(result_current_subelements);

  if (rel_error < vfs->adapt_rel_error_tol)
    // Relative error in tolerance.
    return result_current_subelements;
  else {
    Scalar result_recursion = 0;
    // Relative error exceeds allowed tolerance: Call the function 
    // eval_form_adaptive() in each subelement, with initial values
    // subs_value[sons_i].

    // Call the function eval_form_adaptive() recursively in all sons.
    for(unsigned int sons_i = 0; sons_i < 4; sons_i++) {
      // Push the transformation to the current son to all functions and reference mappings involved.
      Transformable::push_transforms(transformable_entities, sons_i);

      // Recursion.
      subs_value[sons_i] = eval_form_adaptive(order_init + order_increase, subs_value[sons_i],
        vfs, u_ext, fv, rv, surf_pos);

      result_recursion += subs_value[sons_i];

      // Reset the transformation.
      Transformable::pop_transforms(transformable_entities);
    }

    return result_recursion;
  }
}

template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_dg_matrix_form(MatrixFormSurf<Scalar> *mfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, SurfPos* surf_pos,
  bool neighbor_supp_u, bool neighbor_supp_v, LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int neighbor_index_u)
{
  NeighborSearch<Scalar>* nbs_u = neighbor_searches.get(neighbor_index_u);
  // Order that will be returned.
  int order;

  if(this->is_fvm)
    order = ru->get_inv_ref_order();
  else {
    // Order of solutions from the previous Newton iteration.
    int prev_size = u_ext.size() - mfs->u_ext_offset;
    Func<Ord>** oi = new Func<Ord>*[prev_size];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for (int i = 0; i < prev_size; i++)
        if (u_ext[i + mfs->u_ext_offset] != NULL) 
          oi[i] = init_ext_fn_ord(neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq), u_ext[i]);
        else 
          oi[i] = get_fn_ord(0);
    else
      for (int i = 0; i < prev_size; i++) 
        oi[i] = get_fn_ord(0);

    // Order of shape functions.
    int inc = (fv->get_num_components() == 2) ? 1 : 0;
    DiscontinuousFunc<Ord>* ou = new DiscontinuousFunc<Ord>(get_fn_ord(fu->get_edge_fn_order(surf_pos->surf_num) + inc), neighbor_supp_u);
    DiscontinuousFunc<Ord>* ov = new DiscontinuousFunc<Ord>(get_fn_ord(fv->get_edge_fn_order(surf_pos->surf_num) + inc), neighbor_supp_v);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(mfs->ext, neighbor_searches);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    Geom<Ord>* fake_e = new InterfaceGeom<Ord>(&geom_ord, nbs_u->neighb_el->marker, 
      nbs_u->neighb_el->id, nbs_u->neighb_el->get_diameter());
    double fake_wt = 1.0;

    // Total order of the matrix form.
    Ord o = mfs->ord(1, &fake_wt, oi, ou, ov, fake_e, fake_ext);

    // Increase due to reference maps.
    order = ru->get_inv_ref_order();

    order += o.get_order();
    limit_order(order);

    // Clean up.
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for (int i = 0; i < prev_size; i++)
        if (u_ext[i + mfs->u_ext_offset] != NULL) 
          delete oi[i];
    delete [] oi;
    delete fake_e;
    delete ou;
    delete ov;
    if (fake_ext != NULL) {
      for (int i = 0; i < fake_ext->nf; i++) {
        delete fake_ext->fn[i];
      }
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}
template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_dg_matrix_form(MultiComponentMatrixFormSurf<Scalar>*mfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru, SurfPos* surf_pos,
  bool neighbor_supp_u, bool neighbor_supp_v, LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int neighbor_index_u)
{
  NeighborSearch<Scalar>* nbs_u = neighbor_searches.get(neighbor_index_u);
  // Order that will be returned.
  int order;

  if(this->is_fvm)
    order = ru->get_inv_ref_order();
  else {
    // Order of solutions from the previous Newton iteration.
    int prev_size = u_ext.size() - mfs->u_ext_offset;
    Func<Ord>** oi = new Func<Ord>*[prev_size];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for (int i = 0; i < prev_size; i++)
        if (u_ext[i + mfs->u_ext_offset] != NULL) 
          oi[i] = init_ext_fn_ord(neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq), u_ext[i]);
        else 
          oi[i] = get_fn_ord(0);
    else
      for (int i = 0; i < prev_size; i++) 
        oi[i] = get_fn_ord(0);

    // Order of shape functions.
    int inc = (fv->get_num_components() == 2) ? 1 : 0;
    DiscontinuousFunc<Ord>* ou = new DiscontinuousFunc<Ord>(get_fn_ord(fu->get_edge_fn_order(surf_pos->surf_num) + inc), neighbor_supp_u);
    DiscontinuousFunc<Ord>* ov = new DiscontinuousFunc<Ord>(get_fn_ord(fv->get_edge_fn_order(surf_pos->surf_num) + inc), neighbor_supp_v);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(mfs->ext, neighbor_searches);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    Geom<Ord>* fake_e = new InterfaceGeom<Ord>(&geom_ord, nbs_u->neighb_el->marker, 
      nbs_u->neighb_el->id, nbs_u->neighb_el->get_diameter());
    double fake_wt = 1.0;

    // Total order of the matrix form.
    Ord o = mfs->ord(1, &fake_wt, oi, ou, ov, fake_e, fake_ext);

    // Increase due to reference maps.
    order = ru->get_inv_ref_order();

    order += o.get_order();
    limit_order(order);

    // Clean up.
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for (int i = 0; i < prev_size; i++)
        if (u_ext[i + mfs->u_ext_offset] != NULL) 
          delete oi[i];
    delete [] oi;
    delete fake_e;
    delete ou;
    delete ov;
    if (fake_ext != NULL) {
      for (int i = 0; i < fake_ext->nf; i++) {
        delete fake_ext->fn[i];
      }
      fake_ext->free_ord(); 
      delete fake_ext;
    }
  }
  return order;
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_dg_form(MatrixFormSurf<Scalar>* mfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru_central, RefMap *ru_actual, RefMap *rv, 
  bool neighbor_supp_u, bool neighbor_supp_v,
  SurfPos* surf_pos, LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int neighbor_index_u, int neighbor_index_v)
{
  _F_;

  NeighborSearch<Scalar>* nbs_u = neighbor_searches.get(neighbor_index_u);
  NeighborSearch<Scalar>* nbs_v = neighbor_searches.get(neighbor_index_v);

  // Determine the integration order.
  int order = calc_order_dg_matrix_form(mfs, u_ext, fu, fv, ru_actual, surf_pos, neighbor_supp_u, neighbor_supp_v, neighbor_searches, neighbor_index_u);

  // Evaluate the form using just calculated order.
  Quad2D* quad = fu->get_quad_2d();
  int eo = quad->get_edge_points(surf_pos->surf_num, order);
  int np = quad->get_num_points(eo);
  double3* pt = quad->get_points(eo);

  // A (debug) check.
  assert(surf_pos->surf_num == neighbor_searches.get(neighbor_index_u)->active_edge);

  // Init geometry and jacobian*weights.
  if (cache_e[eo] == NULL) {
    cache_e[eo] = init_geom_surf(ru_central, surf_pos, eo);
    double3* tan = ru_central->get_tangent(surf_pos->surf_num, eo);
    cache_jwt[eo] = new double[np];
    for(int i = 0; i < np; i++)
      cache_jwt[eo][i] = pt[i][2] * tan[i][2];
  }

  Geom<double>* e = new InterfaceGeom<double>(cache_e[eo], nbs_u->neighb_el->marker, 
    nbs_u->neighb_el->id, nbs_u->neighb_el->get_diameter());
  double* jwt = cache_jwt[eo];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - mfs->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + mfs->u_ext_offset] != NULL) {
        neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->set_quad_order(order);
        prev[i]  = neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->init_ext_fn(u_ext[i]);
      }
      else prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  nbs_u->set_quad_order(order);
  DiscontinuousFunc<double>* u = new DiscontinuousFunc<double>(get_fn(fu, ru_actual, nbs_u->get_quad_eo(neighbor_supp_u)),
    neighbor_supp_u, nbs_u->neighbor_edge.orientation);
  nbs_v->set_quad_order(order);
  DiscontinuousFunc<double>* v = new DiscontinuousFunc<double>(get_fn(fv, rv, nbs_v->get_quad_eo(neighbor_supp_v)),
    neighbor_supp_v, nbs_v->neighbor_edge.orientation);

  ExtData<Scalar>* ext = init_ext_fns(mfs->ext, neighbor_searches, order);

  Scalar res = mfs->value(np, jwt, prev, u, v, e, ext);

  // Clean up.
  for (int i = 0; i < prev_size; i++) {
    if (prev[i] != NULL) {
      prev[i]->free_fn();
      delete prev[i];
    }
  }

  delete [] prev;


  if (ext != NULL) {
    ext->free(); 
    delete ext;
  }

  delete u;
  delete v;
  delete e;

  // Scaling.
  res *= mfs->scaling_factor;

  return 0.5 * res; // Edges are parametrized from 0 to 1 while integration weights
  // are defined in (-1, 1). Thus multiplying with 0.5 to correct
  // the weights.
}
template<typename Scalar>
void DiscreteProblem<Scalar>::eval_dg_form(MultiComponentMatrixFormSurf<Scalar>* mfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fu, PrecalcShapeset *fv, RefMap *ru_central, RefMap *ru_actual, RefMap *rv, 
  bool neighbor_supp_u, bool neighbor_supp_v,
  SurfPos* surf_pos, LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int neighbor_index_u, int neighbor_index_v, Hermes::vector<Scalar>& result)
{
  _F_;

  NeighborSearch<Scalar>* nbs_u = neighbor_searches.get(neighbor_index_u);
  NeighborSearch<Scalar>* nbs_v = neighbor_searches.get(neighbor_index_v);

  // Determine the integration order.
  int order = calc_order_dg_matrix_form(mfs, u_ext, fu, fv, ru_actual, surf_pos, neighbor_supp_u, neighbor_supp_v, neighbor_searches, neighbor_index_u);

  // Evaluate the form using just calculated order.
  Quad2D* quad = fu->get_quad_2d();
  int eo = quad->get_edge_points(surf_pos->surf_num, order);
  int np = quad->get_num_points(eo);
  double3* pt = quad->get_points(eo);

  // A (debug) check.
  assert(surf_pos->surf_num == neighbor_searches.get(neighbor_index_u)->active_edge);

  // Init geometry and jacobian*weights.
  if (cache_e[eo] == NULL) {
    cache_e[eo] = init_geom_surf(ru_central, surf_pos, eo);
    double3* tan = ru_central->get_tangent(surf_pos->surf_num, eo);
    cache_jwt[eo] = new double[np];
    for(int i = 0; i < np; i++)
      cache_jwt[eo][i] = pt[i][2] * tan[i][2];
  }

  Geom<double>* e = new InterfaceGeom<double>(cache_e[eo], nbs_u->neighb_el->marker, 
    nbs_u->neighb_el->id, nbs_u->neighb_el->get_diameter());
  double* jwt = cache_jwt[eo];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - mfs->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + mfs->u_ext_offset] != NULL) {
        neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->set_quad_order(order);
        prev[i]  = neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->init_ext_fn(u_ext[i]);
      }
      else prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  nbs_u->set_quad_order(order);
  DiscontinuousFunc<double>* u = new DiscontinuousFunc<double>(get_fn(fu, ru_actual, nbs_u->get_quad_eo(neighbor_supp_u)),
    neighbor_supp_u, nbs_u->neighbor_edge.orientation);
  nbs_v->set_quad_order(order);
  DiscontinuousFunc<double>* v = new DiscontinuousFunc<double>(get_fn(fv, rv, nbs_v->get_quad_eo(neighbor_supp_v)),
    neighbor_supp_v, nbs_v->neighbor_edge.orientation);

  ExtData<Scalar>* ext = init_ext_fns(mfs->ext, neighbor_searches, order);

  mfs->value(np, jwt, prev, u, v, e, ext, result);

  for(unsigned int i = 0; i < result.size(); i++)
    result[i] *= mfs->scaling_factor * 0.5;

  // Clean up.
  for (int i = 0; i < prev_size; i++) {
    if (prev[i] != NULL) {
      prev[i]->free_fn();
      delete prev[i];
    }
  }

  delete [] prev;


  if (ext != NULL) {
    ext->free(); 
    delete ext;
  }

  delete u;
  delete v;
  delete e;
}

template<typename Scalar>
int DiscreteProblem<Scalar>::calc_order_dg_vector_form(VectorFormSurf<Scalar> *vfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv, SurfPos* surf_pos,
  LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int neighbor_index_v)
{
  NeighborSearch<Scalar>* nbs_v = neighbor_searches.get(neighbor_index_v);
  // Order that will be returned.
  int order;
  int u_ext_length = u_ext.size();      // Number of external solutions.
  int u_ext_offset = vfs->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
  // and there will be only u_ext_length - u_ext_offset of them.
  if(this->is_fvm)
    order = rv->get_inv_ref_order();
  else {
    // Order of solutions from the previous Newton iteration.
    int prev_size = u_ext.size() - vfs->u_ext_offset;
    Func<Ord>** oi = new Func<Ord>*[prev_size];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for (int i = 0; i < prev_size; i++)
        if (u_ext[i + vfs->u_ext_offset] != NULL) 
          oi[i] = init_ext_fn_ord(neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq), u_ext[i]);
        else 
          oi[i] = get_fn_ord(0);
    else
      for (int i = 0; i < prev_size; i++) 
        oi[i] = get_fn_ord(0);

    // Order of the shape function.
    // Determine the integration order.
    int inc = (fv->get_num_components() == 2) ? 1 : 0;
    Func<Ord>* ov = get_fn_ord(fv->get_edge_fn_order(surf_pos->surf_num) + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(vfs->ext, neighbor_searches);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    Geom<Ord>* fake_e = new InterfaceGeom<Ord>(&geom_ord,
      nbs_v->neighb_el->marker, nbs_v->neighb_el->id, nbs_v->neighb_el->get_diameter());
    double fake_wt = 1.0;

    // Total order of the vector form.
    Ord o = vfs->ord(1, &fake_wt, oi, ov, fake_e, fake_ext);

    // Increase due to reference map.
    order = rv->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Clean up.
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for (int i = 0; i < prev_size; i++)
        if (u_ext[i + vfs->u_ext_offset] != NULL) 
          delete oi[i];
    delete [] oi;
    if (fake_ext != NULL) {
      for (int i = 0; i < fake_ext->nf; i++) {
        delete fake_ext->fn[i];
      }
      fake_ext->free_ord(); 
      delete fake_ext;
    }

    delete fake_e;
  }
  return order;
}template<typename Scalar>
  int DiscreteProblem<Scalar>::calc_order_dg_vector_form(MultiComponentVectorFormSurf<Scalar>*vfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv, SurfPos* surf_pos,
  LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int neighbor_index_v)
{
  NeighborSearch<Scalar>* nbs_v = neighbor_searches.get(neighbor_index_v);
  // Order that will be returned.
  int order;
  int u_ext_length = u_ext.size();      // Number of external solutions.
  int u_ext_offset = vfs->u_ext_offset; // External solutions will start with u_ext[u_ext_offset]
  // and there will be only u_ext_length - u_ext_offset of them.
  if(this->is_fvm)
    order = rv->get_inv_ref_order();
  else {
    // Order of solutions from the previous Newton iteration.
    int prev_size = u_ext.size() - vfs->u_ext_offset;
    Func<Ord>** oi = new Func<Ord>*[prev_size];
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for (int i = 0; i < prev_size; i++)
        if (u_ext[i + vfs->u_ext_offset] != NULL) 
          oi[i] = init_ext_fn_ord(neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq), u_ext[i]);
        else 
          oi[i] = get_fn_ord(0);
    else
      for (int i = 0; i < prev_size; i++) 
        oi[i] = get_fn_ord(0);

    // Order of the shape function.
    // Determine the integration order.
    int inc = (fv->get_num_components() == 2) ? 1 : 0;
    Func<Ord>* ov = get_fn_ord(fv->get_edge_fn_order(surf_pos->surf_num) + inc);

    // Order of additional external functions.
    ExtData<Ord>* fake_ext = init_ext_fns_ord(vfs->ext, neighbor_searches);

    // Order of geometric attributes (eg. for multiplication of a solution with coordinates, normals, etc.).
    Geom<Ord>* fake_e = new InterfaceGeom<Ord>(&geom_ord,
      nbs_v->neighb_el->marker, nbs_v->neighb_el->id, nbs_v->neighb_el->get_diameter());
    double fake_wt = 1.0;

    // Total order of the vector form.
    Ord o = vfs->ord(1, &fake_wt, oi, ov, fake_e, fake_ext);

    // Increase due to reference map.
    order = rv->get_inv_ref_order();
    order += o.get_order();
    limit_order(order);

    // Clean up.
    if (u_ext != Hermes::vector<Solution<Scalar>*>())
      for (int i = 0; i < prev_size; i++)
        if (u_ext[i + vfs->u_ext_offset] != NULL) 
          delete oi[i];
    delete [] oi;
    if (fake_ext != NULL) {
      for (int i = 0; i < fake_ext->nf; i++) {
        delete fake_ext->fn[i];
      }
      fake_ext->free_ord(); 
      delete fake_ext;
    }

    delete fake_e;
  }
  return order;
}

template<typename Scalar>
Scalar DiscreteProblem<Scalar>::eval_dg_form(VectorFormSurf<Scalar>* vfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv, 
  SurfPos* surf_pos, LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int neighbor_index_v)
{
  _F_;
  NeighborSearch<Scalar>* nbs_v = (neighbor_searches.get(neighbor_index_v));
  int order = calc_order_dg_vector_form(vfs, u_ext, fv, rv, surf_pos, neighbor_searches, neighbor_index_v);

  // Evaluate the form using just calculated order.
  Quad2D* quad = fv->get_quad_2d();
  int eo = quad->get_edge_points(surf_pos->surf_num, order);
  int np = quad->get_num_points(eo);
  double3* pt = quad->get_points(eo);

  // A (debug) check.
  assert(surf_pos->surf_num == nbs_v->active_edge);

  // Init geometry and jacobian*weights.
  if (cache_e[eo] == NULL) {
    cache_e[eo] = init_geom_surf(rv, surf_pos, eo);
    double3* tan = rv->get_tangent(surf_pos->surf_num, eo);
    cache_jwt[eo] = new double[np];
    for(int i = 0; i < np; i++)
      cache_jwt[eo][i] = pt[i][2] * tan[i][2];
  }

  Geom<double>* e = new InterfaceGeom<double>(cache_e[eo], nbs_v->neighb_el->marker, 
    nbs_v->neighb_el->id, nbs_v->neighb_el->get_diameter());
  double* jwt = cache_jwt[eo];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - vfs->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + vfs->u_ext_offset] != NULL) {
        neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->set_quad_order(order);
        prev[i]  = neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->init_ext_fn(u_ext[i]);
      }
      else prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* v = get_fn(fv, rv, eo);
  ExtData<Scalar>* ext = init_ext_fns(vfs->ext, neighbor_searches, order);

  Scalar res = vfs->value(np, jwt, prev, v, e, ext);

  // Clean up.
  for (int i = 0; i < prev_size; i++) {
    if (prev[i] != NULL) {
      prev[i]->free_fn(); 
      delete prev[i];
    }
  }

  delete [] prev;

  if (ext != NULL) {
    ext->free(); 
    delete ext;
  }

  delete e;

  // Scaling.
  res *= vfs->scaling_factor;

  return 0.5 * res; // Edges are parametrized from 0 to 1 while integration weights
  // are defined in (-1, 1). Thus multiplying with 0.5 to correct
  // the weights.
}
template<typename Scalar>
void DiscreteProblem<Scalar>::eval_dg_form(MultiComponentVectorFormSurf<Scalar>* vfs, Hermes::vector<Solution<Scalar>*> u_ext,
  PrecalcShapeset *fv, RefMap *rv, 
  SurfPos* surf_pos, LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int neighbor_index_v, Hermes::vector<Scalar>& result)
{
  _F_;
  NeighborSearch<Scalar>* nbs_v = (neighbor_searches.get(neighbor_index_v));
  int order = calc_order_dg_vector_form(vfs, u_ext, fv, rv, surf_pos, neighbor_searches, neighbor_index_v);

  // Evaluate the form using just calculated order.
  Quad2D* quad = fv->get_quad_2d();
  int eo = quad->get_edge_points(surf_pos->surf_num, order);
  int np = quad->get_num_points(eo);
  double3* pt = quad->get_points(eo);

  // A (debug) check.
  assert(surf_pos->surf_num == nbs_v->active_edge);

  // Init geometry and jacobian*weights.
  if (cache_e[eo] == NULL) {
    cache_e[eo] = init_geom_surf(rv, surf_pos, eo);
    double3* tan = rv->get_tangent(surf_pos->surf_num, eo);
    cache_jwt[eo] = new double[np];
    for(int i = 0; i < np; i++)
      cache_jwt[eo][i] = pt[i][2] * tan[i][2];
  }

  Geom<double>* e = new InterfaceGeom<double>(cache_e[eo], nbs_v->neighb_el->marker, 
    nbs_v->neighb_el->id, nbs_v->neighb_el->get_diameter());
  double* jwt = cache_jwt[eo];

  // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
  int prev_size = u_ext.size() - vfs->u_ext_offset;
  Func<Scalar>** prev = new Func<Scalar>*[prev_size];
  if (u_ext != Hermes::vector<Solution<Scalar>*>())
    for (int i = 0; i < prev_size; i++)
      if (u_ext[i + vfs->u_ext_offset] != NULL) {
        neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->set_quad_order(order);
        prev[i]  = neighbor_searches.get(u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->init_ext_fn(u_ext[i]);
      }
      else prev[i] = NULL;
  else
    for (int i = 0; i < prev_size; i++) 
      prev[i] = NULL;

  Func<double>* v = get_fn(fv, rv, eo);
  ExtData<Scalar>* ext = init_ext_fns(vfs->ext, neighbor_searches, order);

  vfs->value(np, jwt, prev, v, e, ext, result);

  for(unsigned int i = 0; i < result.size(); i++)
    result[i] *= vfs->scaling_factor * 0.5;

  // Clean up.
  for (int i = 0; i < prev_size; i++) {
    if (prev[i] != NULL) {
      prev[i]->free_fn(); 
      delete prev[i];
    }
  }

  delete [] prev;

  if (ext != NULL) {
    ext->free(); 
    delete ext;
  }

  delete e;
}

NeighborNode::NeighborNode(NeighborNode* parent, unsigned int transformation) : parent(parent), transformation(transformation)
{
  left_son = right_son = NULL;
}

NeighborNode::~NeighborNode()
{
  if(left_son != NULL) {
    delete left_son;
    left_son = NULL;
  }
  if(right_son != NULL) {
    delete right_son;
    right_son = NULL;
  }
}

void NeighborNode::set_left_son(NeighborNode* left_son)
{
  this->left_son = left_son;
}
void NeighborNode::set_right_son(NeighborNode* right_son)
{
  this->right_son = right_son;
}
void NeighborNode::set_transformation(unsigned int transformation)
{
  this->transformation = transformation;
}
NeighborNode* NeighborNode::get_left_son()
{
  return left_son;
}
NeighborNode* NeighborNode::get_right_son()
{
  return right_son;
}
unsigned int NeighborNode::get_transformation()
{
  return this->transformation;
}

template<typename Scalar>
DiscreteProblem<Scalar>::AssemblingCaches::AssemblingCaches()
{

};

template<typename Scalar>
DiscreteProblem<Scalar>::AssemblingCaches::~AssemblingCaches()
{
  _F_;
  for (typename std::map<KeyConst, Func<double>*, CompareConst>::const_iterator it = const_cache_fn_triangles.begin();
    it != const_cache_fn_triangles.end(); it++)
  {
    (it->second)->free_fn(); delete (it->second);
  }
  const_cache_fn_triangles.clear();

  for (typename std::map<KeyConst, Func<double>*, CompareConst>::const_iterator it = const_cache_fn_quads.begin();
    it != const_cache_fn_quads.end(); it++)
  {
    (it->second)->free_fn(); delete (it->second);
  }
  const_cache_fn_quads.clear();

  for(unsigned int i = 0; i < cache_fn_ord.get_size(); i++)
    if(cache_fn_ord.present(i)) {
      cache_fn_ord.get(i)->free_ord(); 
      delete cache_fn_ord.get(i);
    }
};

template class HERMES_API DiscreteProblem<double>;
template class HERMES_API DiscreteProblem<std::complex<double> >;
