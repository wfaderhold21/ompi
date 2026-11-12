# -*- shell-script -*-
#
# Copyright (c) 2026      NVIDIA Corporation.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#

# OMPI_CHECK_LIBGPI(prefix, [action-if-found], [action-if-not-found])
# ------------------------------------------------
# Locate a libgpi install tree. --with-libgpi=DIR takes a source/install
# root (headers under DIR/include, fallback DIR); --with-libgpi-libdir=DIR
# overrides the library search dir (default DIR/build, then DIR/lib — the
# in-tree build lands its archive in build/).
AC_DEFUN([OMPI_CHECK_LIBGPI],[
    AC_ARG_WITH([libgpi],
        [AS_HELP_STRING([--with-libgpi(=DIR)],
             [Build spml/gpi with libgpi, adding DIR/include to CPPFLAGS])],
        [], [with_libgpi=no])
    AC_ARG_WITH([libgpi-libdir],
        [AS_HELP_STRING([--with-libgpi-libdir=DIR],
             [Directory with the libgpi library (default DIR/build, then DIR/lib)])],
        [], [with_libgpi_libdir=""])

    AS_IF([test "$with_libgpi" = "no" || test -z "$with_libgpi"],
          [$2],
          [
            AS_IF([test -d "$with_libgpi/include"],
                  [$1_check_incdir="$with_libgpi/include"],
                  [$1_check_incdir="$with_libgpi"])
            AS_IF([test -n "$with_libgpi_libdir"],
                  [$1_check_libdir="$with_libgpi_libdir"],
                  [AS_IF([test -d "$with_libgpi/build"],
                         [$1_check_libdir="$with_libgpi/build"],
                         [$1_check_libdir="$with_libgpi/lib"])])

            AS_IF([test ! -d "$$1_check_libdir"],
                  [$1_happy="no"
                   AC_MSG_WARN([libgpi library directory $$1_check_libdir not found])],
                  [
                    opmi_check_libgpi_save_CPPFLAGS=$CPPFLAGS
                    opmi_check_libgpi_save_LIBS=$LIBS
                    CPPFLAGS="-I$$1_check_incdir $CPPFLAGS"
                    LIBS="-L$$1_check_libdir -lgpi $LIBS"

                    # shm_open lives in librt on glibc, libSystem on macOS.
                    # AC_SEARCH_LIBS caches the resolution; splice it into the
                    # component LIBS below (spml/gpi calls shm_open/shm_unlink
                    # directly for channel and region cleanup).
                    AC_SEARCH_LIBS([shm_open], [rt])
                    AS_IF([test "x$ac_cv_search_shm_open" != "xnone required"],
                          [spml_gpi_shm_libs="$ac_cv_search_shm_open"],
                          [spml_gpi_shm_libs=""])

                    $1_happy="yes"
                    AC_CHECK_HEADER([gpi_shm.h],
                        [], [$1_happy="no"])
                    AC_CHECK_HEADER([gpi_emu.h],
                        [], [$1_happy="no"])
                    AS_IF([test "$$1_happy" = "yes"],
                        [AC_CHECK_LIB([gpi], [gpi_shm_chan_create],
                            [], [$1_happy="no"])])

                    CPPFLAGS=$opmi_check_libgpi_save_CPPFLAGS
                    LIBS=$opmi_check_libgpi_save_LIBS

                    AS_IF([test "$$1_happy" = "yes"],
                          [$1_CPPFLAGS="-I$$1_check_incdir"
                           $1_LDFLAGS="-L$$1_check_libdir"
                           $1_LIBS="-L$$1_check_libdir -lgpi -lpthread $spml_gpi_shm_libs"],
                          [AC_MSG_WARN([libgpi check failed; spml/gpi component will not be built])])
                  ])

            AS_IF([test "$$1_happy" = "yes"], [$1], [$2])
          ])
])dnl

# MCA_oshmem_spml_gpi_CONFIG([action-if-can-compile],
#                            [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_oshmem_spml_gpi_CONFIG],[
    AC_CONFIG_FILES([oshmem/mca/spml/gpi/Makefile])

    OMPI_CHECK_LIBGPI([spml_gpi],
                      [spml_gpi_happy="yes"],
                      [spml_gpi_happy="no"])

    AS_IF([test "$spml_gpi_happy" = "yes"],
          [$1],
          [$2])

    # substitute in the things needed to build gpi
    AC_SUBST([spml_gpi_CPPFLAGS])
    AC_SUBST([spml_gpi_LDFLAGS])
    AC_SUBST([spml_gpi_LIBS])
])dnl
