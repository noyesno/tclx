/*
 * tclXwinId.c --
 *
 * Win32 version of the id command.
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
 * $Id: tclXwinId.c,v 8.3 1997/07/01 02:58:17 markd Exp $
 *-----------------------------------------------------------------------------
 */

#include "tclExtdInt.h"

/*
 * Prototypes of internal functions.
 */
static int
IdProcess  _ANSI_ARGS_((Tcl_Interp *interp,
                        int         argc,
                        char      **argv));

static int
IdHost _ANSI_ARGS_((Tcl_Interp *interp,
                    int         argc,
                    char      **argv));

static int 
TclX_IdObjCmd _ANSI_ARGS_((ClientData clientData,
                           Tcl_Interp *interp,
                           int objc,
                           Tcl_Obj *CONST objv[]));

/*-----------------------------------------------------------------------------
 * Tcl_IdCmd --
 *     Implements the TclX id command on Win32.
 *
 *        id host
 *        id process
 *
 * Results:
 *  Standard TCL results, may return the Posix system error message.
 *
 *-----------------------------------------------------------------------------
 */

/*
 * id process
 */
static int
IdProcess (interp, argc, argv)
    Tcl_Interp *interp;
    int         argc;
    char      **argv;
{
    char numBuf [32];

    if (argc != 2) {
        TclX_AppendObjResult (interp, tclXWrongArgs, argv [0], 
                              " process", (char *) NULL);
        return TCL_OK;
    }
    sprintf (numBuf, "%d", getpid ());
    Tcl_SetResult (interp, numBuf, TCL_VOLATILE);
    return TCL_OK;
}

/*
 * id host
 */
static int
IdHost (interp, argc, argv)
    Tcl_Interp *interp;
    int         argc;
    char      **argv;
{
    char hostName [TCL_RESULT_SIZE];

    if (argc != 2) {
        TclX_AppendObjResult (interp, tclXWrongArgs, argv [0], 
                              " host", (char *) NULL);
        return TCL_ERROR;
    }
    if (gethostname (hostName, sizeof (hostName)) < 0) {
        TclX_AppendObjResult (interp, "failed to get host name: ",
                              Tcl_PosixError (interp), (char *) NULL);
        return TCL_ERROR;
    }
    TclX_AppendObjResult (interp, hostName, (char *) NULL);
    return TCL_OK;
}

static int
Tcl_IdCmd (clientData, interp, argc, argv)
    ClientData  clientData;
    Tcl_Interp *interp;
    int         argc;
    char      **argv;
{
    if (argc < 2) {
        TclX_AppendObjResult (interp, tclXWrongArgs, argv [0], " arg ?arg...?",
                              (char *) NULL);
        return TCL_ERROR;
    }

    /*
     * If the first argument is "process", return the process ID, parent's
     * process ID, process group or set the process group depending on args.
     */
    if (STREQU (argv[1], "process")) {
        return IdProcess (interp, argc, argv);
    }

    /*
     * Handle returning the host name if its available.
     */
    if (STREQU (argv[1], "host")) {
        return IdHost (interp, argc, argv);
    }

    TclX_AppendObjResult (interp, "second arg must be one of \"process\", ",
                          "or \"host\"", (char *) NULL);
    return TCL_ERROR;
}


/*-----------------------------------------------------------------------------
 * TclX_IdInit --
 *     Initialize the id command.
 *-----------------------------------------------------------------------------
 */
void
TclX_IdInit (interp)
    Tcl_Interp *interp;
{
    Tcl_CreateObjCommand (interp,
			  "id",
			  TclX_IdObjCmd,
                          (ClientData) NULL,
			  (Tcl_CmdDeleteProc*) NULL);
}

