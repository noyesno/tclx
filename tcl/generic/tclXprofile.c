/*
 * tclXprofile.c --
 *
 * Tcl performance profile monitor.
 *-----------------------------------------------------------------------------
 * Copyright 1991-1997 Karl Lehenbauer and Mark Diekhans.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose and without fee is hereby granted, provided
 * that the above copyright notice appear in all copies.  Karl Lehenbauer and
 * Mark Diekhans make no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without express or
 * implied warranty.
 *-----------------------------------------------------------------------------
 * $Id: tclXprofile.c,v 1.7 1997/01/25 05:38:25 markd Exp $
 *-----------------------------------------------------------------------------
 */

/*
 * FIX: What about compiled commands???
 */

#include "tclExtdInt.h"

/*
 * For when the level is not known.
 */
#define UNKNOWN_LEVEL -1

/*
 * Stack entry used to keep track of an profiling information for procedures
 * (and commands in command mode).  This stack mirrors the Tcl procedure stack.
 * A chain of variable scope entries is also kept.  This tracks the uplevel
 * chain kept in the Tcl stack.  Unlike the Tcl stack, an entry is also make
 * for the global context and for the commands when in command mode.  We count
 * the amount of time actually in the procedure, not what it has called.  This
 * is the time it spent on the top of the stack.  This is do for both eval
 * level and variable scope.
 */

typedef struct profEntry_t {
    int                 isProc;           /* Procedure, not command.       */
    int                 procLevel;        /* Procedure level.              */ 
    int                 scopeLevel;       /* Varaible scope level.         */ 
    int                 evalLevel;        /* Tcl_Eval level.               */ 
    clock_t             evalRealTime;     /* Cumulative real and CPU time  */
    clock_t             evalCpuTime;      /* entry was on top of stack.    */
    clock_t             scopeRealTime;    /* Cumulative Real and CPU time  */
    clock_t             scopeCpuTime;     /* entry's scope was active.     */
    struct profEntry_t *prevEntryPtr;     /* Procedure call stack.         */
    struct profEntry_t *prevScopePtr;     /* Procedure var scope chain.    */
    char                cmdName [1];      /* Command name. MUST BE LAST!   */
} profEntry_t;


/*
 * Data keeped on a stack snapshot.
 */
typedef struct profDataEntry_t {
    clock_t count;
    clock_t realTime;
    clock_t cpuTime;
} profDataEntry_t;

/*
 * Client data structure for profile command.  This contains all global
 * profiling information for the interpreter.
 */

typedef struct profInfo_t { 
    Tcl_Interp     *interp;                /* Interpreter this is for.       */
    Tcl_Trace       traceHandle;           /* Handle to current trace.       */
    int             commandMode;           /* Prof all commands?             */
    int             evalMode;              /* Use eval stack.                */
    Command        *currentCmdPtr;         /* Current command table entry.   */
    Tcl_CmdProc    *savedStrCmdProc;       /* Saved string command function  */
    ClientData      savedStrCmdClientData; /* and clientData.                */
    Tcl_ObjCmdProc *savedObjCmdProc;       /* Saved object command function  */
    ClientData      savedObjCmdClientData; /* and clientData.                */
    int             evalLevel;             /* Eval level when invoked.       */
    clock_t         realTime;              /* Current real and CPU time.     */
    clock_t         cpuTime;
    clock_t         prevRealTime;          /* Real and CPU time of previous  */
    clock_t         prevCpuTime;           /* trace.                         */
    int             updatedTimes;          /* Has current times been updated?*/
    profEntry_t    *stackPtr;              /* Proc/command nesting stack.    */
    int             stackSize;             /* Size of the stack.             */
    profEntry_t    *scopeChainPtr;         /* Variable scope chain.          */
    Tcl_HashTable   profDataTable;         /* Cumulative time table, Keyed   */
                                           /* by call stack list.            */
} profInfo_t;

/*
 * Argument to panic on logic errors.  Takes an id number.
 */
static char *PROF_PANIC = "TclX profile bug id = %d\n";

/*
 * Prototypes of internal functions.
 */
static void
PushEntry _ANSI_ARGS_((profInfo_t *infoPtr,
                       char       *cmdName,
                       int         isProc,
                       int         procLevel,
                       int         scopeLevel,
                       int         evalLevel));

static void
RecordData _ANSI_ARGS_((profInfo_t  *infoPtr,
                        profEntry_t *entryPtr));

static void
PopEntry _ANSI_ARGS_((profInfo_t *infoPtr));

static void
UpdateTOSTimes _ANSI_ARGS_((profInfo_t *infoPtr));

static Command *
ProfCommandEvalSetup _ANSI_ARGS_((profInfo_t *infoPtr,
                                  char       *cmdName,
                                  int        *isProcPtr));
    
static void
ProfCommandEvalFinishup _ANSI_ARGS_((profInfo_t *infoPtr,
                                     int         isProc));

static int
ProfStrCommandEval _ANSI_ARGS_((ClientData    clientData,
                                Tcl_Interp   *interp,
                                int           argc,
                                char        **argv));

static int
ProfObjCommandEval _ANSI_ARGS_((ClientData    clientData,
                                Tcl_Interp   *interp,
                                int           objc,
                                Tcl_Obj     **objv));

static void
ProfTraceRoutine _ANSI_ARGS_((ClientData    clientData,
                              Tcl_Interp   *interp,
                              int           evalLevel,
                              char         *command,
                              Tcl_CmdProc  *cmdProc,
                              ClientData    cmdClientData,
                              int           argc,
                              char        **argv));

static void
CleanDataTable _ANSI_ARGS_((profInfo_t *infoPtr));

static void
InitializeProcStack _ANSI_ARGS_((profInfo_t *infoPtr,
                                 CallFrame  *framePtr));

static void
TurnOnProfiling _ANSI_ARGS_((profInfo_t *infoPtr,
                             int         commandMode,
                             int         evalMode));

static void
DeleteProfTrace _ANSI_ARGS_((profInfo_t *infoPtr));

static int
TurnOffProfiling _ANSI_ARGS_((Tcl_Interp *interp,
                              profInfo_t *infoPtr,
                              char       *varName));

static int
TclX_ProfileObjCmd _ANSI_ARGS_((ClientData   clientData,
                                Tcl_Interp  *interp,
                                int          objc,
                                Tcl_Obj    **objv));

static void
ProfMonCleanUp _ANSI_ARGS_((ClientData  clientData,
                            Tcl_Interp *interp));


/*-----------------------------------------------------------------------------
 * PushEntry --
 *   Push a procedure or command entry onto the stack.
 *
 * Parameters:
 *   o infoPtr - The global profiling info.
 *   o cmdName  The procedure or command name.
 *   o isProc - TRUE if its a proc, FALSE if other command.
 *   o procLevel - The procedure call level that the procedure or command will
 *     execute at.
 *   o scopeLevel - The procedure variable scope level that the commands local
 *     variables are at.
 *   o evalLevel - The eval level the command was executed at.  For procedures
 *     this is the level it was called at, since if the procedure's commands
 *     are logged, they will be an the next eval level.  Maybe be
 *     UNKNOWN_LEVEL.
 *-----------------------------------------------------------------------------
 */
static void
PushEntry (infoPtr, cmdName, isProc, procLevel, scopeLevel, evalLevel)
    profInfo_t *infoPtr;
    char       *cmdName;
    int         isProc;
    int         procLevel;
    int         scopeLevel;
    int         evalLevel;
{
    profEntry_t *entryPtr, *scanPtr;

    /*
     * Calculate the size of an entry.  One byte for name is in the entry.
     */
    entryPtr = (profEntry_t *) ckalloc (sizeof (profEntry_t) +
                                        strlen (cmdName));
    
    /*
     * Fill it in and push onto the stack.  Note that the procedures frame has
     * not yet been layed down or the procedure body eval execute, so the value
     * they will be in the procedure is recorded.
     */
    entryPtr->isProc = isProc;
    entryPtr->procLevel = procLevel;
    entryPtr->scopeLevel = scopeLevel;
    entryPtr->evalLevel = evalLevel;
    entryPtr->evalRealTime = 0;
    entryPtr->evalCpuTime = 0;
    entryPtr->scopeRealTime = 0;
    entryPtr->scopeCpuTime = 0;
    strcpy (entryPtr->cmdName, cmdName);

    /*
     * Push onto the stack and set the variable scope chain.  The variable
     * scope entry is chained to the first entry who's scope is less than ours
     * if this is a proc or less than or equal to ours if this is a command.
     */
    entryPtr->prevEntryPtr = infoPtr->stackPtr;
    infoPtr->stackPtr = entryPtr;
    infoPtr->stackSize++;

    scanPtr = infoPtr->scopeChainPtr;
    while ((scanPtr != NULL) && (scanPtr->procLevel > 0) &&
           ((isProc && (scanPtr->scopeLevel >= scopeLevel)) ||
            ((!isProc) && (scanPtr->scopeLevel > scopeLevel)))) {
        scanPtr = scanPtr->prevScopePtr;
        /*
         * Only global level can be NULL.
         */
        if (scanPtr == NULL)
            panic (PROF_PANIC, 3);
    }
    entryPtr->prevScopePtr = scanPtr;
    infoPtr->scopeChainPtr = entryPtr;
}

/*-----------------------------------------------------------------------------
 * RecordData --
 *   Record an entries times in the data table.
 *
 * Parameters:
 *   o infoPtr - The global profiling info.
 *   o entryPtr - The entry to record.
 *-----------------------------------------------------------------------------
 */
static void
RecordData (infoPtr, entryPtr)
    profInfo_t  *infoPtr;
    profEntry_t *entryPtr;
{
    int idx, newEntry;
    profEntry_t *scanPtr;
    char **stackArgv, *stackListPtr;
    Tcl_HashEntry *hashEntryPtr;
    profDataEntry_t *dataEntryPtr;

    /*
     * Build up a stack list.  Entry [0] is the top of the stack, either the
     * scope or eval stack is followed, based on the -eval option.  If both
     * scope and command mode are enabled, commands other than the top command
     * are skipped.
     */
    stackArgv = (char **) ckalloc (sizeof (char *) * infoPtr->stackSize);
    if (infoPtr->evalMode) {
        for (idx= 0, scanPtr = entryPtr; scanPtr != NULL;
             scanPtr = scanPtr->prevEntryPtr) {
            stackArgv [idx++] = scanPtr->cmdName;
        }
    } else {
        for (idx= 0, scanPtr = entryPtr; scanPtr != NULL;
             scanPtr = scanPtr->prevScopePtr) {
            stackArgv [idx++] = scanPtr->cmdName;
        }
    }
    stackListPtr = Tcl_Merge (idx, stackArgv);
    ckfree ((char *) stackArgv);

    /*
     * Check the hash table for this entry, either finding an existing or
     * creating a new hash entry.
     */

    hashEntryPtr = Tcl_CreateHashEntry (&infoPtr->profDataTable,
                                        stackListPtr,
                                        &newEntry);
    ckfree (stackListPtr);

    /*
     * Either get the existing entry or create a new one.
     */
    if (newEntry) {
        dataEntryPtr = (profDataEntry_t *) ckalloc (sizeof (profDataEntry_t));
        Tcl_SetHashValue (hashEntryPtr, dataEntryPtr);
        dataEntryPtr->count = 0;
        dataEntryPtr->realTime = 0;
        dataEntryPtr->cpuTime  = 0;
    } else {
        dataEntryPtr = (profDataEntry_t *) Tcl_GetHashValue (hashEntryPtr);
    }

    /*
     * Increment the cumulative data.
     */
    dataEntryPtr->count++;
    if (infoPtr->evalMode) {
        dataEntryPtr->realTime += entryPtr->evalRealTime;
        dataEntryPtr->cpuTime += entryPtr->evalCpuTime;
    } else {
        dataEntryPtr->realTime += entryPtr->scopeRealTime;
        dataEntryPtr->cpuTime += entryPtr->scopeCpuTime;
    }
}

/*-----------------------------------------------------------------------------
 * PopEntry --
 *   Pop the procedure entry from the top of the stack and record its
 * times in the data table.
 *
 * Parameters:
 *   o infoPtr - The global profiling info.
 *-----------------------------------------------------------------------------
 */
static void
PopEntry (infoPtr)
    profInfo_t *infoPtr;
{
    profEntry_t *entryPtr = infoPtr->stackPtr;

    RecordData (infoPtr, entryPtr);

    /*
     * Remove from the stack, reset the scope chain and free.
     */
    infoPtr->stackPtr = entryPtr->prevEntryPtr;
    infoPtr->stackSize--;
    infoPtr->scopeChainPtr = infoPtr->stackPtr;

    ckfree ((char *) entryPtr);
}

/*-----------------------------------------------------------------------------
 * UpdateTOSTimes --
 *   Update the time spent in the entry on the top of the stack before another
 * is pushed on top or its poped off.
 *
 * Parameters:
 *   o infoPtr - The global profiling info.
 *-----------------------------------------------------------------------------
 */
static void
UpdateTOSTimes (infoPtr)
    profInfo_t *infoPtr;
{
    /*
     * Get the current time if we haven't already.
     */
    if (!infoPtr->updatedTimes) {
        infoPtr->prevRealTime = infoPtr->realTime;
        infoPtr->prevCpuTime = infoPtr->cpuTime;
        TclXOSElapsedTime (&infoPtr->realTime, &infoPtr->cpuTime);
        infoPtr->updatedTimes = TRUE;
    }
    if (infoPtr->stackPtr != NULL) {
        infoPtr->stackPtr->evalRealTime +=
            infoPtr->realTime - infoPtr->prevRealTime;
        infoPtr->stackPtr->evalCpuTime +=
            infoPtr->cpuTime - infoPtr->prevCpuTime;
    }
    if (infoPtr->scopeChainPtr != NULL) {
        infoPtr->scopeChainPtr->scopeRealTime +=
            infoPtr->realTime - infoPtr->prevRealTime;
        infoPtr->scopeChainPtr->scopeCpuTime +=
            infoPtr->cpuTime - infoPtr->prevCpuTime;
    }
}

/*-----------------------------------------------------------------------------
 * ProfCommandEvalSetup --
 *   Do initial work that is common to both the string and object command
 * evaluators.
 *
 * Returns:
 *   A pointer to the current command table entry.
 *-----------------------------------------------------------------------------
 */
static Command *
ProfCommandEvalSetup (infoPtr, cmdName, isProcPtr)
    profInfo_t *infoPtr;
    char       *cmdName;
    int        *isProcPtr;
{
    Interp *iPtr = (Interp *) infoPtr->interp;
    Command *currentCmdPtr;
    CallFrame *framePtr;
    int procLevel, scopeLevel, isProc;

    /*
     * Restore the command table entry.  If the command has modified it, don't
     * mess with it.
     */
    currentCmdPtr = infoPtr->currentCmdPtr;
    if (currentCmdPtr->proc == ProfStrCommandEval)
        currentCmdPtr->proc = infoPtr->savedStrCmdProc;
    if (currentCmdPtr->clientData == infoPtr)
        currentCmdPtr->clientData = infoPtr->savedStrCmdClientData;
    if (currentCmdPtr->objProc == ProfObjCommandEval)
        currentCmdPtr->objProc = infoPtr->savedObjCmdProc;
    if (currentCmdPtr->objClientData == infoPtr)
        currentCmdPtr->objClientData = infoPtr->savedObjCmdClientData;
    infoPtr->currentCmdPtr = NULL;
    infoPtr->savedStrCmdProc = NULL;
    infoPtr->savedStrCmdClientData = NULL;
    infoPtr->savedObjCmdProc = NULL;
    infoPtr->savedObjCmdClientData = NULL;

    /*
     * Determine current proc and var levels.
     */
    procLevel = 0;
    for (framePtr = iPtr->framePtr; framePtr != NULL; framePtr =
             framePtr->callerPtr) {
        procLevel++;
    }
    scopeLevel = (iPtr->varFramePtr == NULL) ? 0 : iPtr->varFramePtr->level;

    /* 
     * If there are entries on the stack that are at a higher proc call level
     * than we are, we have exited into the initial entries that where pushed
     * on the stack before we started.  Pop those entries.
     */
    if (infoPtr->stackPtr->procLevel > procLevel)
        UpdateTOSTimes (infoPtr);
    while (infoPtr->stackPtr->procLevel > procLevel) {
        if (infoPtr->stackPtr->evalLevel != UNKNOWN_LEVEL) 
            panic (PROF_PANIC, 4);  /* Not an initial entry */
        PopEntry (infoPtr);
    }

    /*
     * If this command is a procedure or if all commands are being traced,
     * handle the entry.
     */
     isProc = (TclFindProc (iPtr, cmdName) != NULL);
#ifdef ITCL_NAMESPACES      
    if (infoPtr->commandMode || isProc) {
        char *commandNamesp;
        Tcl_DString commandWithNs;

        /*
         * Append the namespace of the command.
         */
        Tcl_DStringInit (&commandWithNs);
        commandNamesp = Itcl_GetNamespPath (Itcl_GetActiveNamesp (interp));
	if (commandNamesp != NULL) {
            Tcl_DStringAppend (&commandWithNs, commandNamesp, -1);

            /*
             * If the argument does not have a leading ::, then append one.
             */
            if (!STRNEQU (argv[0], "::", 2)) 
                Tcl_DStringAppend (&commandWithNs, "::", -1); 
	}
	Tcl_DStringAppend (&commandWithNs, argv[0], -1); 

        UpdateTOSTimes (infoPtr);
        if (isProc) {
            PushEntry (infoPtr, Tcl_DStringValue (&commandWithNs), TRUE,
                       procLevel + 1, scopeLevel + 1, infoPtr->evalLevel);
        } else {
            PushEntry (infoPtr, Tcl_DStringValue (&commandWithNs), FALSE,
                       procLevel, scopeLevel, infoPtr->evalLevel);
        }
	Tcl_DStringFree (&commandWithNs);
    }
#else
    if (infoPtr->commandMode || isProc) {
        UpdateTOSTimes (infoPtr);
        if (isProc) {
            PushEntry (infoPtr, cmdName, TRUE,
                       procLevel + 1, scopeLevel + 1, infoPtr->evalLevel);
        } else {
            PushEntry (infoPtr, cmdName, FALSE,
                       procLevel, scopeLevel, infoPtr->evalLevel);
        }
    }
#endif
    /*
     * Leaving profiler, must get time again when we reenter.
     */
    infoPtr->updatedTimes = FALSE;

    *isProcPtr = isProc;
    return currentCmdPtr;
}

/*-----------------------------------------------------------------------------
 * ProfCommandEvalFinishup --
 *   Do final work that is common to both the string and object command
 * evaluators.
 *-----------------------------------------------------------------------------
 */
static void
ProfCommandEvalFinishup (infoPtr, isProc)
    profInfo_t *infoPtr;
    int         isProc;
{
    /*
     * If tracing is still running, pop the entry, recording the information.
     */
    if (infoPtr->traceHandle != NULL) {
        if (infoPtr->commandMode || isProc) {
            UpdateTOSTimes (infoPtr);
            PopEntry (infoPtr);
        }
    }
    /*
     * Leaving profiler, must get time again when we reenter.
     */
    infoPtr->updatedTimes = FALSE;
}

/*-----------------------------------------------------------------------------
 * ProfStrCommandEval --
 *   Function to evaluate a string command.  The procedure trace routine
 * substitutes this function for the command executor function in the Tcl
 * command table.  We restore the command table, record data about the start
 * of the command and then actually execute the command.  When the command
 * returns, we record data about the time it took.
 *
 * FIX:  This all falls apart if another trace is executed between the
 * doctoring of the command entry and this function being called.
 *-----------------------------------------------------------------------------
 */
static int
ProfStrCommandEval (clientData, interp, argc, argv)
    ClientData    clientData;
    Tcl_Interp   *interp;
    int           argc;
    char        **argv;
{
    profInfo_t *infoPtr = (profInfo_t *) clientData;
    Command *currentCmdPtr;
    int isProc, result;

    currentCmdPtr = ProfCommandEvalSetup (infoPtr, argv [0], &isProc);

    result = (*currentCmdPtr->proc) (currentCmdPtr->clientData, interp,
                                     argc, argv);

    ProfCommandEvalFinishup (infoPtr, isProc);
    return result;
}

/*-----------------------------------------------------------------------------
 * ProfObjCommandEval --
 *   Function to evaluate a object command.  The procedure trace routine
 * substitutes this function for the command executor function in the Tcl
 * command table.  We restore the command table, record data about the start
 * of the command and then actually execute the command.  When the command
 * returns, we record data about the time it took.
 *
 * FIX:  This all falls apart if another trace is executed between the
 * doctoring of the command entry and this function being called.
 *-----------------------------------------------------------------------------
 */
static int
ProfObjCommandEval (clientData, interp, objc, objv)
    ClientData    clientData;
    Tcl_Interp   *interp;
    int           objc;
    Tcl_Obj     **objv;
{
    profInfo_t *infoPtr = (profInfo_t *) clientData;
    Command *currentCmdPtr;
    int isProc, result, strLen;

    /* FIX: Need version of ProfCommandEvalSetup that takes an object */
    currentCmdPtr = ProfCommandEvalSetup (infoPtr, 
                                          Tcl_GetStringFromObj (objv [0],
                                                                &strLen),
                                          &isProc);

    result = (*currentCmdPtr->objProc) (currentCmdPtr->objClientData, interp,
                                        objc, objv);

    ProfCommandEvalFinishup (infoPtr, isProc);
    return result;
}

/*-----------------------------------------------------------------------------
  * ProfTraceRoutine --
 *   Routine called by Tcl_Eval to do profiling.  It intercepts the current
 * command being executed by temporarily editing the command table.
 *-----------------------------------------------------------------------------
 */
static void
ProfTraceRoutine (clientData, interp, evalLevel, command, cmdProc,
                  cmdClientData, argc, argv)
    ClientData    clientData;
    Tcl_Interp   *interp;
    int           evalLevel;
    char         *command;
    Tcl_CmdProc  *cmdProc;
    ClientData    cmdClientData;
    int           argc;
    char        **argv;
{
    Interp *iPtr = (Interp *) interp;
    profInfo_t *infoPtr = (profInfo_t *) clientData;
    Command *cmdPtr;
#ifdef ITCL_NAMESPACES      
    Tcl_Command cmd;
#else
    Tcl_HashEntry *hPtr;
#endif    

    if (infoPtr->currentCmdPtr != NULL)
        panic (PROF_PANIC, 5);

#ifdef ITCL_NAMESPACES      
    if (Itcl_FindCommand((Tcl_Interp*)iPtr, argv [0], 0, &cmd) != TCL_OK)
        panic (PROF_PANIC, 56);
    cmdPtr = (Command*)cmd;
#else
    hPtr = Tcl_FindHashEntry (&iPtr->commandTable, argv [0]);
    cmdPtr = (Command *) Tcl_GetHashValue (hPtr);
#endif
    if ((cmdPtr->proc != cmdProc) || (cmdPtr->clientData != cmdClientData))
        panic (PROF_PANIC, 6);

    /*
     * If command is to be compiled, we can't profile it.
     */
    if (cmdPtr->compileProc != NULL)
        return;

    /*
     * Save current state information.
     */
    infoPtr->currentCmdPtr = cmdPtr;
    infoPtr->savedStrCmdProc = cmdPtr->proc;
    infoPtr->savedStrCmdClientData = cmdPtr->clientData;
    infoPtr->savedObjCmdProc = cmdPtr->objProc;
    infoPtr->savedObjCmdClientData = cmdPtr->objClientData;
    infoPtr->evalLevel = evalLevel;

    /*
     * Force our routines to be called.
     */
    cmdPtr->proc = ProfStrCommandEval;
    cmdPtr->clientData = infoPtr;
    cmdPtr->objProc = ProfObjCommandEval;
    cmdPtr->objClientData = infoPtr;
}

/*-----------------------------------------------------------------------------
 * CleanDataTable --
 *    Clean up the hash data table, releasing all resources and setting it
 * to the empty state.
 *
 * Parameters:
 *   o infoPtr - The global profiling info.
 *-----------------------------------------------------------------------------
 */
static void
CleanDataTable (infoPtr)
    profInfo_t *infoPtr;
{
    Tcl_HashEntry    *hashEntryPtr;
    Tcl_HashSearch   searchCookie;

    hashEntryPtr = Tcl_FirstHashEntry (&infoPtr->profDataTable,
                                       &searchCookie);
    while (hashEntryPtr != NULL) {
        ckfree ((char *) Tcl_GetHashValue (hashEntryPtr));
        Tcl_DeleteHashEntry (hashEntryPtr);
        hashEntryPtr = Tcl_NextHashEntry (&searchCookie);
    }
}

/*-----------------------------------------------------------------------------
 * InitializeProcStack --
 *    Recursive procedure to initialize the procedure call stack so its in the
 * same state as the actual procedure  call stack.  If commandMode is enable,
 * command records are still are not initialized on the stack, as we have no
 * way of knowing what command did a Tcl_Eval.
 *
 * Parameters:
 *   o infoPtr - The global profiling info.
 *   o framePtr - Pointer to the frame to push.  We recurse down to the bottom,
 *     then push on the way out.
 *-----------------------------------------------------------------------------
 */
static void
InitializeProcStack (infoPtr, framePtr)
    profInfo_t *infoPtr;
    CallFrame  *framePtr;
{
    int nameLen;  /* FIX: remove when NULL is allowed in GetString */

    if (framePtr == NULL)
        return;
    InitializeProcStack (infoPtr, framePtr->callerPtr);
    
        
    PushEntry (infoPtr,
               Tcl_GetStringFromObj (framePtr->objv [0], &nameLen),
               TRUE,
               infoPtr->stackPtr->procLevel + 1,
               framePtr->level,
               UNKNOWN_LEVEL);
}

/*-----------------------------------------------------------------------------
 * TurnOnProfiling --
 *    Turn on profiling.
 *
 * Parameters:
 *   o infoPtr - The global profiling info.
 *   o commandMode - TRUE if all commands are going to be logged, FALSE if just
 *     procs.
 *   o evalMode - TRUE if eval stack is to be used to log entries.  FALSE if
 *     the scope stack is to be used.
 *-----------------------------------------------------------------------------
 */
static void
TurnOnProfiling (infoPtr, commandMode, evalMode)
    profInfo_t *infoPtr;
    int         commandMode;
    int         evalMode;
{
    Interp *iPtr = (Interp *) infoPtr->interp;
    int scopeLevel;
    profEntry_t *scanPtr;

    CleanDataTable (infoPtr);

    infoPtr->traceHandle =
        Tcl_CreateTrace (infoPtr->interp, MAXINT,
                         (Tcl_CmdTraceProc *) ProfTraceRoutine,
                         (ClientData) infoPtr);
    infoPtr->commandMode = commandMode;
    infoPtr->evalMode = evalMode;
    infoPtr->realTime = 0;
    infoPtr->cpuTime = 0;
    infoPtr->prevRealTime = 0;
    infoPtr->prevCpuTime = 0;
    infoPtr->updatedTimes = FALSE;
    
    /*
     * Add entry for global context, then add in current procedures.
     */
    PushEntry (infoPtr, "<global>", TRUE, 0, 0, 0);
    InitializeProcStack (infoPtr, ((Interp *) infoPtr->interp)->framePtr);

    /*
     * Find the current top of the scope stack.
     */
    scopeLevel = (iPtr->varFramePtr == NULL) ? 0 : iPtr->varFramePtr->level;
    scanPtr = infoPtr->scopeChainPtr;
    while ((scanPtr != NULL) && (scanPtr->scopeLevel >= scopeLevel) &&
           (scanPtr->procLevel > 0)) {
        scanPtr = scanPtr->prevScopePtr;
        /*
         * Only global level can be NULL.
         */
        if (scanPtr == NULL)
            panic (PROF_PANIC, 4);
    }
    infoPtr->scopeChainPtr = scanPtr;

    /*
     * Get the time we started.
     */
    TclXOSElapsedTime (&infoPtr->realTime, &infoPtr->cpuTime);
}

/*-----------------------------------------------------------------------------
 * DeleteProfTrace --
 *   Delete the profile trace and clean up the stack, logging all procs
 * as if they had exited.  Data table must still be available.
 *
 * Parameters:
 *   o infoPtr - The global profiling info.
 *-----------------------------------------------------------------------------
 */
static void
DeleteProfTrace (infoPtr)
    profInfo_t *infoPtr;
{
    Tcl_DeleteTrace (infoPtr->interp, infoPtr->traceHandle);
    infoPtr->traceHandle = NULL;

    UpdateTOSTimes (infoPtr);
    while (infoPtr->stackPtr != NULL) {
        PopEntry (infoPtr);
    }
}

/*-----------------------------------------------------------------------------
 * TurnOffProfiling --
 *   Turn off profiling.  Dump the table data to an array variable.  Entries
 * will be deleted as they are dumped to limit memory utilization.
 *
 * Parameters:
 *   o interp - Pointer to the interprer.
 *   o infoPtr - The global profiling info.
 *   o varName - The name of the variable to save the data in.
 * Returns:
 *   TCL_OK or TCL_ERROR.
 * FIX: Should take Tcl_Obj for varName.
 *-----------------------------------------------------------------------------
 */
static int
TurnOffProfiling (interp, infoPtr, varName)
    Tcl_Interp *interp;
    profInfo_t *infoPtr;
    char       *varName;
{
    Tcl_HashEntry *hashEntryPtr;
    Tcl_HashSearch searchCookie;
    profDataEntry_t *dataEntryPtr;
    char *dataArgv [3], *dataListPtr;
    char countBuf [32], realTimeBuf [32], cpuTimeBuf [32];

    DeleteProfTrace (infoPtr);

    dataArgv [0] = countBuf;
    dataArgv [1] = realTimeBuf;
    dataArgv [2] = cpuTimeBuf;

    Tcl_UnsetVar (interp, varName, 0);
    hashEntryPtr = Tcl_FirstHashEntry (&infoPtr->profDataTable,
                                       &searchCookie);
    while (hashEntryPtr != NULL) {
        dataEntryPtr = 
            (profDataEntry_t *) Tcl_GetHashValue (hashEntryPtr);

        sprintf (countBuf, "%ld", dataEntryPtr->count);
        sprintf (realTimeBuf,"%ld", dataEntryPtr->realTime);
        sprintf (cpuTimeBuf, "%ld", dataEntryPtr->cpuTime);

        dataListPtr = Tcl_Merge (3, dataArgv);

        if (Tcl_SetVar2 (interp, varName,
                         Tcl_GetHashKey (&infoPtr->profDataTable,
                                         hashEntryPtr),
                         dataListPtr, TCL_LEAVE_ERR_MSG) == NULL) {
            ckfree (dataListPtr);
            return TCL_ERROR;
        }
        ckfree (dataListPtr);
        ckfree ((char *) dataEntryPtr);
        Tcl_DeleteHashEntry (hashEntryPtr);

        hashEntryPtr = Tcl_NextHashEntry (&searchCookie);
    }

    return TCL_OK;
}

/*-----------------------------------------------------------------------------
 * TclX_ProfileObjCmd --
 *   Implements the TCL profile command:
 *     profile ?-commands? ?-eval? on
 *     profile off arrayvar
 *-----------------------------------------------------------------------------
 */
static int
TclX_ProfileObjCmd (clientData, interp, objc, objv)
    ClientData   clientData;
    Tcl_Interp  *interp;
    int          objc;
    Tcl_Obj    **objv;
{
    profInfo_t *infoPtr = (profInfo_t *) clientData;
    int argIdx, strLen;
    int commandMode = FALSE, evalMode = FALSE;
    char *argStr;
        
    /*
     * Parse option arguments.
     */
    for (argIdx = 1; argIdx < objc; argIdx++) {
        argStr = Tcl_GetStringFromObj (objv [argIdx], &strLen);
        if (argStr[0] != '-')
            break;
        if (STREQU (argStr, "-commands")) {
            commandMode = TRUE;
        } else if (STREQU (argStr, "-eval")) {
            evalMode = TRUE;
        } else {
            TclX_StringAppendObjResult (interp,
                                        "expected one of \"-commands\", or ",
                                        "\"-eval\", got \"", argStr,
                                        (char *) NULL);
            return TCL_ERROR;
        }
    }
    if (argIdx >= objc)
        goto wrongArgs;

    /*
     * Get argument string for remainder of strings.
     */
    argStr = Tcl_GetStringFromObj (objv [argIdx], &strLen);
    
    /*
     * Handle the on command.
     */
    if (STREQU (argStr, "on")) {
        if (argIdx != objc - 1)
            goto wrongArgs;

        if (infoPtr->traceHandle != NULL) {
            TclX_StringAppendObjResult (interp,
                                        "profiling is already enabled",
                                        (char *) NULL);
            return TCL_ERROR; 
        }

        TurnOnProfiling (infoPtr, commandMode, evalMode);
        return TCL_OK;
    }

    /*
     * Handle the off command.  Dump the hash table to a variable.
     */
    if (STREQU (argStr, "off")) {

        if (argIdx != objc - 2)
            goto wrongArgs;

        if (commandMode || evalMode) {
            TclX_StringAppendObjResult (interp, "option \"",
                                        commandMode ? "-command" : "-eval",
                                        "\" not valid when turning off ",
                                        "profiling", (char *) NULL);
            return TCL_ERROR;
        }

        if (infoPtr->traceHandle == NULL) {
            TclX_StringAppendObjResult (interp,
                                        "profiling is not currently enabled",
                                        (char *) NULL);
            return TCL_ERROR;
        }
            
        if (TurnOffProfiling (interp, infoPtr, 
                              Tcl_GetStringFromObj (objv [argIdx + 1],
                                                    &strLen)) != TCL_OK)
            return TCL_ERROR;
        return TCL_OK;
    }

    /*
     * Not a valid subcommand.
     */
    TclX_StringAppendObjResult (interp,
                                "expected one of \"on\" or \"off\", got \"",
                                argStr, "\"", (char *) NULL);
    return TCL_ERROR;

  wrongArgs:
    return TclX_WrongArgs (interp, objv [0],
                           "?-commands? ?-eval? on|off arrayVar");
}

/*-----------------------------------------------------------------------------
 * ProfMonCleanUp --
 *   Release the client data area when the interpreter is deleted.
 *-----------------------------------------------------------------------------
 */
static void
ProfMonCleanUp (clientData, interp)
    ClientData  clientData;
    Tcl_Interp *interp;
{
    profInfo_t *infoPtr = (profInfo_t *) clientData;

    if (infoPtr->traceHandle != NULL)
        DeleteProfTrace (infoPtr);
    CleanDataTable (infoPtr);
    Tcl_DeleteHashTable (&infoPtr->profDataTable);
    ckfree ((char *) infoPtr);
}

/*-----------------------------------------------------------------------------
 * Tcl_InitProfile --
 *   Initialize the Tcl profiling command.
 *-----------------------------------------------------------------------------
 */
void
TclX_ProfileInit (interp)
    Tcl_Interp *interp;
{
    profInfo_t *infoPtr;

    infoPtr = (profInfo_t *) ckalloc (sizeof (profInfo_t));

    infoPtr->interp = interp;
    infoPtr->traceHandle = NULL;
    infoPtr->commandMode = FALSE;
    infoPtr->evalMode = FALSE;
    infoPtr->currentCmdPtr = NULL;
    infoPtr->savedStrCmdProc = NULL;
    infoPtr->savedStrCmdClientData = NULL;
    infoPtr->savedObjCmdProc = NULL;
    infoPtr->savedObjCmdClientData = NULL;
    infoPtr->evalLevel = UNKNOWN_LEVEL;
    infoPtr->realTime = 0;
    infoPtr->cpuTime = 0;
    infoPtr->prevRealTime = 0;
    infoPtr->prevCpuTime = 0;
    infoPtr->updatedTimes = FALSE;
    infoPtr->stackPtr = NULL;
    infoPtr->stackSize = 0;
    infoPtr->scopeChainPtr = NULL;
    Tcl_InitHashTable (&infoPtr->profDataTable, TCL_STRING_KEYS);

    Tcl_CallWhenDeleted (interp, ProfMonCleanUp, (ClientData) infoPtr);

    Tcl_CreateObjCommand (interp, "profile", -1, TclX_ProfileObjCmd,
                          (ClientData) infoPtr, (Tcl_CmdDeleteProc*) NULL);
}

