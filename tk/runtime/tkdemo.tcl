#
# tkdemo --
#
# Run the Tk demo at anytime after Extended Tcl is installed.
# 
#------------------------------------------------------------------------------
# Copyright 1992 Karl Lehenbauer and Mark Diekhans.
#
# Permission to use, copy, modify, and distribute this software and its
# documentation for any purpose and without fee is hereby granted, provided
# that the above copyright notice appear in all copies.  Karl Lehenbauer and
# Mark Diekhans make no representations about the suitability of this
# software for any purpose.  It is provided "as is" without express or
# implied warranty.
#------------------------------------------------------------------------------
# $Id: tkdemo.tcl,v 1.1 1992/12/19 07:50:39 markd Exp markd $
#------------------------------------------------------------------------------
#

#@package: Tk-demo tkdemo

proc tkdemo {} {
    global TCLPATH
    if {[info commands tkwait] == ""} {
        error "tkdemo may only be used from wishx"
    }
    set demos [searchpath $TCLPATH demos]
    if {$demos == "" || ![file isdirectory $demos]} {
        error "can't find Tk `demos' directory on the TCLPATH ($TCLPATH)
Extended Tcl must be installed with the Tk demos option to use this procedure"
    }
    uplevel #0 source $demos/widget
}


