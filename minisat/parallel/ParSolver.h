/*************************************************************************************[ParSolver.h]
MergeSat -- Copyright (c) 2021,      Norbert Manthey

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**************************************************************************************************/

#ifndef MergeSat_ParSolver_h
#define MergeSat_ParSolver_h

#include "mtl/Queue.h"
#include "simp/SimpSolver.h"


namespace MERGESAT_NSPACE
{

//=================================================================================================

class JobQueue;
class Barrier;

class ParSolver : protected SimpSolver
{
    bool par_reparsed_options; // Indicate whether the update parameter method has been used

    /* structure, that holds relevant data for parallel solving */
    struct SolverData {
        ParSolver *_parent;
        int _threadnr;
        lbool _status;
        SolverData(ParSolver *parent, int threadnr) : _parent(parent), _threadnr(threadnr), _status(l_Undef) {}
        SolverData() : _parent(nullptr), _threadnr(0), _status(l_Undef) {}
    };

    public:
    // Constructor/Destructor:
    //
    ParSolver();
    ~ParSolver();

    // Problem specification:
    //
    int nVars() const;    // The current number of variables.
    int nClauses() const; // The current number of original clauses.
    Var newVar(bool polarity = true, bool dvar = true);
    void reserveVars(Var vars);    // Reserve space for given amount of variables
    bool addClause_(vec<Lit> &ps); // Add a clause to the solver without making superflous internal copy. Will
    // change the passed vector 'ps'.
    void addInputClause_(vec<Lit> &ps); // Add a clause to the online proof checker

    // Variable mode:
    //
    void setFrozen(Var v, bool b); // If a variable is frozen it will not be eliminated.
    bool isEliminated(Var v) const;
    bool eliminate(bool turn_off_elim = false); // Perform variable elimination based simplification.

    // Solving:
    //
    bool solve(const vec<Lit> &assumps, bool do_simp = true, bool turn_off_simp = false);
    lbool solveLimited(const vec<Lit> &assumps, bool do_simp = true, bool turn_off_simp = false);
    void interrupt();  // Trigger a (potentially asynchronous) interruption of the solver.
    bool okay() const; // FALSE means solver is in a conflicting state

    // Extra results: (read-only member variable)
    //
    vec<lbool> model;  // If problem is satisfiable, this vector contains the model (if any).
    vec<Lit> conflict; // If problem is unsatisfiable (possibly under assumptions),

    int max_simp_cls(); // Return number of clauses when we do not perform simplification anymore

    // TODO FIXME: to be implemented
    void printStats() {}

    // Mode of operation:
    //
    bool parsing;
    int verbosity;
    bool use_simplification;

    protected:
    // Solver state:
    //
    int cores; /// number of cores available to this parallel solver
    bool initialized;
    vec<SimpSolver *> solvers;
    vec<SolverData> solverData;
    vec<Lit> assumptions;

    bool primary_modified; // indicate whether state of primary solver has been changed (new variables or clauses)
    size_t synced_clauses; // store number of clauses in primary solver after last sync (after solving)
    size_t synced_units;   // store number of unit clauses in primary solver after last sync (after solving)

    JobQueue *jobqueue;      /// hold jobs for parallel items
    Barrier *solvingBarrier; /// sync execution after parallel solveing (prt for simpler integration)
    static void *thread_entrypoint(void *argument);
    void thread_run_solve(size_t threadnr);
    bool sync_solver_from_primary(int destination_solver_id); /// sync from primary to parallel solver
    lbool collect_solvers_results();

    // Iternal helper methods:
    //
    void init_solvers();
    void tear_down_solvers();
    bool sequential() const { return cores == 1; }

    // Extra stats
    double simplification_seconds; // seconds of sequential core spend during simplification
};

//=================================================================================================
} // namespace MERGESAT_NSPACE

#endif
