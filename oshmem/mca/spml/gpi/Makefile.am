#
# Copyright (c) 2026      NVIDIA Corporation.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#

dist_ompidata_DATA =

AM_CPPFLAGS = $(spml_gpi_CPPFLAGS)

gpi_sources  = \
 spml_gpi_component.h \
 spml_gpi_component.c \
 spml_gpi.h \
 spml_gpi.c


if MCA_BUILD_oshmem_spml_gpi_DSO
component_noinst =
component_install = mca_spml_gpi.la
else
component_noinst = libmca_spml_gpi.la
component_install =
endif

mcacomponentdir = $(ompilibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_spml_gpi_la_SOURCES = $(gpi_sources)
mca_spml_gpi_la_LIBADD = $(top_builddir)/oshmem/liboshmem.la \
	$(spml_gpi_LIBS)
mca_spml_gpi_la_LDFLAGS = -module -avoid-version $(spml_gpi_LDFLAGS)

noinst_LTLIBRARIES = $(component_noinst)
libmca_spml_gpi_la_SOURCES = $(gpi_sources)
libmca_spml_gpi_la_LIBADD = $(spml_gpi_LIBS)
libmca_spml_gpi_la_LDFLAGS = -module -avoid-version $(spml_gpi_LDFLAGS)
