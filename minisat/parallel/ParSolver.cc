/************************************************************************************[ParSolver.cc]
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

#include "parallel/ParSolver.h"
#include "mtl/Sort.h"
#include "parallel/JobQueue.h"
#include "utils/Options.h"
#include "utils/System.h"

using namespace MERGESAT_NSPACE;

//=================================================================================================
// Options:


static const char *_cat = "PAR";

static IntOption opt_cores(_cat, "cores", "Number of solvers to use, 0 means each CPU, -1 every 2nd CPU", 0, IntRange(-1, INT32_MAX));

//=================================================================================================
// Constructor/Destructor:

ParSolver::ParSolver()
  : par_reparsed_options(updateOptions())
  , parsing(false)
  , verbosity(0)
  , use_simplification(true)
  , cores(opt_cores)
  , initialized(false)
  , primary_modified(false)
  , solved_current_call(false)
  , synced_clauses(0)
  , synced_units(0)
  , jobqueue(nullptr)
  , solvingBarrier(nullptr)
  , syncing_solvers(0)
  , simplification_seconds(0)
{
    // Get number of cores, and allocate arrays
    init_solvers();
}

ParSolver::~ParSolver() { tear_down_solvers(); }

int ParSolver::nVars() const
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    return solvers[0]->nVars();
}

int ParSolver::nClauses() const
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    return solvers[0]->nClauses();
}

Var ParSolver::newVar(bool polarity, bool dvar)
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    primary_modified = true;
    return solvers[0]->newVar(polarity, dvar);
}

void ParSolver::reserveVars(Var vars)
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    solvers[0]->reserveVars(vars);
}

bool ParSolver::addClause_(vec<Lit> &ps)
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    primary_modified = true;
    return solvers[0]->addClause_(ps);
}

void ParSolver::addInputClause_(vec<Lit> &ps)
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    primary_modified = true;
    solvers[0]->addInputClause_(ps);
}

// Variable mode:
//
void ParSolver::setFrozen(Var v, bool b)
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    // not relevant: only primary node would run simplification primary_modified = true;
    solvers[0]->setFrozen(v, b);
}

bool ParSolver::isEliminated(Var v) const
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    return solvers[0]->isEliminated(v);
}

bool ParSolver::eliminate(bool turn_off_elim)
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    if (verbosity > 1) std::cout << "c primary elimination" << std::endl;
    primary_modified = true;
    return solvers[0]->eliminate(turn_off_elim);
}

#define SUM_STATS(sum_variable, member)                                                                                \
    do {                                                                                                               \
        sum_variable = 0;                                                                                              \
        for (int i = 0; i < solvers.size(); ++i) sum_variable += solvers[i]->member;                                   \
    } while (false)

void ParSolver::printStats()
{
    printf("c used cores:                    : %d\n", cores);
    printf("c simplification wall time:      : %g s\n", simplification_seconds);

    const double cpu_time = cpuTime();
    printf("c CPU time                       : %g s\n", cpu_time);

    double theoretical_max_wall = (wallClockTime() - simplification_seconds) * cores + simplification_seconds;
    printf("c theor. Max CPU time:           : %g s\n", theoretical_max_wall);

    double total_idle_time = 0;
    for (int i = 0; i < solverData.size(); ++i) total_idle_time += solverData[i]._idle_s;
    printf("c idle wall search time (sum):   : %g s\n", total_idle_time);

    uint64_t total_conflicts = 0, total_decisions = 0, total_restarts = 0;
    SUM_STATS(total_conflicts, conflicts);
    SUM_STATS(total_decisions, decisions);
    SUM_STATS(total_restarts, starts);
    printf("c SUM stats conflicts:           : %ld\n", total_conflicts);
    printf("c SUM stats decisions:           : %ld\n", total_decisions);
    printf("c SUM stats restarts:            : %ld\n", total_restarts);
    printf("c winning threads:");
    for (int i = 0 ; i < solverData.size(); ++ i) {
        if (solverData[i]._winning > 0 ) printf(" %d:%d", i, solverData[i]._winning);
    }
    printf("\n");
    printf("c barrier data:");
    for (int i = 0 ; i < solverData.size(); ++ i) {
        printf(" %d:%u/%u", i, solverData[i]._entered_barrier, solverData[i]._blocked_by_barrier);
    }
    printf("\n");
}

// Solving:
//
bool ParSolver::solve(const vec<Lit> &assumps, bool do_simp, bool turn_off_simp)
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    assert(false && "support parallelism here!"); // TODO: implement simple wrapper properly
    return solvers[0]->solve(assumps, do_simp, turn_off_simp);
}

lbool ParSolver::solveLimited(const vec<Lit> &assumps, bool do_simp, bool turn_off_simp)
{
    assert(solvers[0] != nullptr && "there has to be one working solver");

    /* prepare next search iteration, local and object global state */
    lbool ret = l_Undef;
    solved_current_call = false;
    conflict.clear();
    model.clear();

    /* in case we shall simplify, first simplify sequentially (for now. TODO: might do something smart with the other threads in the future) */
    if (use_simplification) {
        if (verbosity > 1) std::cout << "c run simplification with primary solver" << std::endl;
        simplification_seconds = wallClockTime() - simplification_seconds;
        use_simplification = false;
        ret = lbool(solvers[0]->eliminate(true));
        simplification_seconds = wallClockTime() - simplification_seconds;
        solverData[0]._next_sync_counter_limit = solvers[0]->counter_access.sum();
        if (ret == l_False) {
            if (verbosity > 1) std::cout << "c simplification solved formula as unsat" << std::endl;
            assert(conflict.size() == 0);
            goto done_solving;
        }
        ret = l_Undef;
    }

    if (sequential()) {
        assert(solvers.size() == 1 && "actually implement parallel case");
        ret = solvers[0]->solveLimited(assumps, do_simp, turn_off_simp);
        solvers[0]->conflict.moveTo(conflict);
        solvers[0]->model.moveTo(model);
    } else {
        assert(jobqueue && "jobqueue should be initialized");
        assert(solvingBarrier && "solvingBarrier should be initialized");

        /* allow to use the barrier for ALL parallel solvers before they start solving in parallel */
        solvingBarrier->grow(cores);

        assumps.copyTo(assumptions); // copy to shared assumptions object
        jobqueue->setState(JobQueue::SLEEP);
        for (int t = 1; t < cores; ++t) { // all except master now
            if (primary_modified) {
                sync_solver_from_primary(t);
            }

            // initialize communication for solver
            solvers[t]->initialize_parallel_solver(&solverData[t], ParSolver::portfolio_sync_and_share);

            // assert(t < solverData.size() && "enough solver data needs to be available");
            JobQueue::Job job;
            job.function = &(ParSolver::thread_entrypoint); // function that controls how a solver runs
            job.argument = (void *)&(solverData[t]);
            jobqueue->addJob(job); // TODO: instead of queue, use a slot based data structure, to make use of core pinning
        }
        // initialize communication for primary solver
        solvers[0]->initialize_parallel_solver(&solverData[0], ParSolver::portfolio_sync_and_share);

        // parallel execution will start
        jobqueue->setState(JobQueue::WORKING);

        // we now run search, so we should stop
        primary_modified = false;
        // also run the primary solver
        thread_run_solve(0);
        // prepare to sync from the state of the primary solver for incremental solving
        synced_clauses = solvers[0]->nClauses();
        synced_units = solvers[0]->nUnits();


        // when returning from this, all parallel solvers are 'done' as well, i.e. do not modify relevant state anymore

#warning TODO: consume result, set 'ret' value
        ret = collect_solvers_results();
        // allow new call to solve method
        if (verbosity > 1) std::cout << "c terminate ParSolver::solveLimited()" << std::endl;
        assert(solvingBarrier->empty() && "all job functions should terminate themselves now");
    }
done_solving:;

    return ret;
}

lbool ParSolver::collect_solvers_results()
{
    lbool status = l_Undef;
    size_t smallest_conflict = ~0UL;
    int smallest_conflict_idx = -1, sat_solver = -1;
    for (int t = 0; t < cores; ++t) { // all except master now
        lbool r = solverData[t]._status;
        assert((status == l_Undef || r == l_Undef || status == r) && "solvers have to have same result");
        assert(solverData[t]._blocked_by_barrier == solverData[0]._blocked_by_barrier && "barrier blocks have to match");
        assert(solverData[t]._entered_barrier == solverData[0]._entered_barrier && "entered barriers have to match");

        /* pick 'winning solver' */
        if (r != l_Undef) {
            /* for conflicts, heuristicalls select the smallest conflict */
            if (r == l_False && solvers[t]->conflict.size() < smallest_conflict) {
                smallest_conflict_idx = t;
            } else if (r == l_True) {
                /* select the first solver that won */
                sat_solver = sat_solver >= 0 ? sat_solver : t;
            }

            if (status != l_Undef && r != status) {
                throw "c detected unsound parallel behavior when collecting results, aborting";
            }
            status = r;
        }
    }

    /* update model or conflict */
    if (status == l_True) {
        /* in case we used elimination, we also have to fix the model (unless, the first solver won) */
        if (sat_solver != 0) {
            solvers[sat_solver]->model.moveTo(solvers[0]->model);
            solvers[0]->extendModel();
        }
        /* at this point, solver 0 holds the valid model */
        solvers[0]->model.moveTo(model);
    } else if (status == l_False) {
        assert(smallest_conflict_idx >= 0 && "at least one index has been found");
        solvers[smallest_conflict_idx]->conflict.moveTo(conflict);
    }

    return status;
}

void ParSolver::interrupt()
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    for (int i = 0; i < solvers.size(); ++i) solvers[i]->interrupt();
}

bool ParSolver::okay() const
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    for (int i = 0; i < solvers.size(); ++i) {
        if (!solvers[i]->okay()) return false;
    }
    return ok;
}

int ParSolver::max_simp_cls()
{
    assert(solvers[0] != nullptr && "there has to be one working solver");
    return solvers[0]->max_simp_cls();
}


void ParSolver::init_solvers()
{
    assert(solvers.size() == 0 && "do not allocate solvers multiple times");

    // auto detect cores
    if (cores <= 0) {
        if (cores == 0)
            cores = nrCores();
        else
            cores = (nrCores() + 1) / 2;
    }

    /* make sure we have at least one solver */
    cores = cores <= 1 ? 1 : cores;
    if (verbosity > 1) std::cout << "c initialize solver for " << cores << " cores" << std::endl;

    solvers.growTo(cores, nullptr);
    for (int i = 0; i < solvers.size(); ++i) {
        solvers[i] = new SimpSolver();
        solvers[i]->diversify(i, 32);
        /* setup non-primary solvers specially */
        if (i > 0) {
            solvers[i]->eliminate(true); // we simplify only sequentially
        }
    }

    if (cores > 1) {
        assert(!jobqueue && "do not override the jobqueue");
        if (jobqueue) delete jobqueue;
        if (verbosity > 1) std::cout << "c initialize thread pool for " << cores - 1 << " non-primary threads" << std::endl;
        jobqueue = new JobQueue(cores - 1);  // all except the main core
        jobqueue->setState(JobQueue::SLEEP); // set all to sleep

        solvingBarrier = new Barrier(0); // setup a dummy barrier for now

        solverData.growTo(cores);
        for (int i = 0; i < solvers.size(); ++i) {
            solverData[i] = { this, i };
        }
    }

    assert(solvers[0] != nullptr && "there has to be one working solver");

    /* in case outside parameters decided against simplification, disable it */
    if (!use_simplification) solvers[0]->eliminate(true);

    initialized = true;
}

void ParSolver::tear_down_solvers()
{
    if (solvers.size() > 0) {
        for (int i = 0; i < solvers.size(); ++i) {
            if (solvers[i] != nullptr) delete solvers[i];
            solvers[i] = nullptr;
        }
    }
    solvers.clear();
    solverData.clear();
    if (solvingBarrier) delete solvingBarrier;
    solvingBarrier = nullptr;
    if (jobqueue) delete jobqueue;
    jobqueue = nullptr;

    initialized = false;
}

void ParSolver::solver_start_measure_idling(size_t threadnr)
{
    assert(threadnr < solverData.size() && "only existing solvers can idle");
    solverData[threadnr]._idle_s = wallClockTime() - solverData[threadnr]._idle_s;
}

void ParSolver::solver_stop_measure_idling(size_t threadnr)
{
    assert(threadnr < solverData.size() && "only existing solvers can idle");
    solverData[threadnr]._idle_s = wallClockTime() - solverData[threadnr]._idle_s;
    ;
    assert(solverData[threadnr]._idle_s >= 0 && "idling cannot become negative");
}

void ParSolver::thread_run_solve(size_t threadnr)
{
    if (verbosity > 1) std::cout << "c started thread " << threadnr << std::endl;

    assert(solvers.size() == solverData.size() && "number of solvers and data should match");
    assert(threadnr < solverData.size() && "cannot run threads beyond initialized cores");
    if (threadnr >= solverData.size()) {
        return; // do not interrupt too aggressively, just ignore the ask
    }

    // stop early, in case solver is in a bad state initially already
    if (!solvers[threadnr]->okay()) {
        solverData[threadnr]._status = l_False;
        return;
    }
    solverData[threadnr]._status = l_Undef;
    solverData[threadnr]._status = solvers[threadnr]->solveLimited(assumptions);
    if (verbosity > 1) std::cout << "c thread " << threadnr << " finished solving with " << solverData[threadnr]._status << " and accesses:" << solvers[threadnr]->counter_access.sum() << std::endl;
    /* wait until all solvers enter here */
    solver_start_measure_idling(threadnr);
    /* final sync, enforce it (e.g. do not respect limits) */
    sync_thread_portfolio(threadnr, true);
    solver_stop_measure_idling(threadnr);
}

void *ParSolver::thread_entrypoint(void *argument)
{
    SolverData *s = (SolverData *)argument;

    (s->_parent)->thread_run_solve(s->_threadnr);
    return 0;
}

bool ParSolver::sync_solver_from_primary(int destination_solver_id)
{
    if (!primary_modified) return false;
    if (verbosity > 1) std::cout << "c sync solver " << destination_solver_id << " from primary solver object" << std::endl;
    SimpSolver *const dest_solver = solvers[destination_solver_id];
    SimpSolver *const source_solver = solvers[0];

    // sync variables
    if (dest_solver->nVars() < source_solver->nVars()) {
        if (verbosity > 1) std::cout << "c resolve variable diff: " << source_solver->nVars() - dest_solver->nVars() << std::endl;
        dest_solver->reserveVars(source_solver->nVars());
        while (dest_solver->nVars() < source_solver->nVars()) {
            // ignore eliminated variables for decisions
            Var next = dest_solver->nVars();
            dest_solver->newVar(true, !source_solver->isEliminated(next));
        }
    }
    // sync unit clauses
    bool succeed_adding_clauses = true;
    if (verbosity > 1) std::cout << "c resolve unit diff: " << source_solver->nUnits() - synced_units << std::endl;
    for (size_t unit_idx = synced_units; unit_idx < source_solver->nUnits(); ++unit_idx) {
        const Lit l = source_solver->getUnit(unit_idx);
        succeed_adding_clauses = succeed_adding_clauses && dest_solver->addClause(l);
    }

    // sync clauses (after simplification, this will only sync the simplified clauses)
    if (verbosity > 1) std::cout << "c resolve clause diff: " << source_solver->nClauses() - synced_clauses << std::endl;
    for (size_t cls_idx = synced_clauses; cls_idx < source_solver->nClauses(); ++cls_idx) {
        const Clause &c = source_solver->getClause(cls_idx);
        if (c.mark() == 1) continue; // skip satisfied clauses
        succeed_adding_clauses = succeed_adding_clauses && dest_solver->importClause(c);
    }

    // sub solver object is not unsat
    return succeed_adding_clauses && dest_solver->okay();
}

bool ParSolver::portfolio_sync_and_share(void *issuer, lbool *status)
{
    // no communication set, just return
    if (issuer == nullptr) return false;

    SolverData *solverData = (SolverData *)issuer;

    /* actuall sync all solvers with the portfolio sharing strategy */
    bool stop_search = solverData->_parent->sync_thread_portfolio(solverData->_threadnr);

    // forward status of solver after sharing
    if (status != nullptr && *status == l_Undef) {
        *status = solverData->_status;
    }

    return stop_search;
}

bool ParSolver::sync_thread_portfolio(size_t threadnr, bool caller_has_solution)
{
    /* TODO: make use of the solvingBarrier here to make solving deterministic */
    assert(solvingBarrier && "in case of parallel solving, there needs to be a barrier");
    assert( (!caller_has_solution || solved_current_call || solverData[threadnr]._status != l_Undef)
      && "we have to know the solution with this flag set, either ourselves, or another solver in an earlier sync");

    /* ignore this call, in case we did not reach the solver internal step limit */
    if (!caller_has_solution) {
        if (solverData[threadnr]._next_sync_counter_limit >= solvers[threadnr]->counter_access.sum()) return false;
    }

    /* keep evaluation and early aborts below limitation check, to maintain deterministic behavior */

    /* do not block, in case some solver already found the solution */
    if (solved_current_call) return true;

    solverData[threadnr]._entered_barrier ++;

    /* use this variable to determine how to treat difficiencies in synchronizing */
    int64_t sync_diff = 10000;               // as a start, allow 10k more clause accesses
    int entering_sync = (syncing_solvers++); // remember when we reached syncing, to better adjust sync_diff

    /* block on the barrier */
    if (verbosity > 1) std::cout << "c sync_thread_portfolio barrier wait 1 by thread " << threadnr << std::endl;
    /* in case we do not have the solution already, count waiting time as idle time */
    if (!caller_has_solution) solver_start_measure_idling(threadnr);
    solvingBarrier->wait();
    if (!caller_has_solution) solver_stop_measure_idling(threadnr);
    syncing_solvers = 0; // set back to 0 for next syncing

    /* check for new status only after the barrier! */
    lbool status = l_Undef;
    for (int t = 0; t < cores; ++t) { // all except master now
        lbool r = solverData[t]._status;
        assert((status == l_Undef || r == l_Undef || status == r) && "solvers have to have same result");

        /* pick 'winning solver' */
        if (r != l_Undef) {
            if (status != l_Undef && r != status) {
                throw "c detected unsound parallel behavior when collecting results, aborting";
                exit(1); // unsound
            }
            status = r;
        }
    }
    assert(sync_diff > 0 && "do not decrease sync diff value");
    solverData[threadnr]._next_sync_counter_limit += sync_diff;
    if (solverData[threadnr]._status != l_Undef) solverData[threadnr]._winning ++;
    /* abort early, in case we already found a solution */
    if (status != l_Undef) {
        /* some solver solved the current call */
        solved_current_call = true;
        if (verbosity > 1) std::cout << "c sync clauses with thread " << threadnr << " exits early" << std::endl;
        return true;
    }

    /* prepare clauses to share */

    /* block on the barrier */
    if (verbosity > 1) std::cout << "c sync_thread_portfolio barrier wait 2 by thread " << threadnr << std::endl;
    solvingBarrier->wait();
    if (verbosity > 1) std::cout << "c sync clauses with thread " << threadnr << " and access limit: " << solvers[threadnr]->counter_access.sum() << std::endl;

    /* consume clauses shared by others */

    /* block on the barrier */
    if (verbosity > 1) std::cout << "c sync_thread_portfolio barrier wait 3 by thread " << threadnr << std::endl;
    solvingBarrier->wait();

    assert(sync_diff > 0 && "do not decrease sync diff value");
    solverData[threadnr]._next_sync_counter_limit += sync_diff;
    if (solverData[threadnr]._status != l_Undef) solverData[threadnr]._winning ++;

    /* stop search in case we found a solution */
    if (status == l_Undef) solverData[threadnr]._blocked_by_barrier ++;
    return status != l_Undef;
}