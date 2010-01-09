# Copyright (C) 2008 Hiroaki Kawai

pkgplugindir=@MYSQL_PLUGINDIR@
INCLUDES= -I$(top_builddir)/include -I$(top_srcdir)/include -I$(top_srcdir)/libstemmer_c/include @MYSQL_INCLUDES@
SUBDIRS = libstemmer_c

AUTOMAKE_OPTIONS = subdir-objects
AM_CPPFLAGS = -I$(top_srcdir)/libstemmer_c/include

pkgplugin_LTLIBRARIES= libftsnowball.la
libftsnowball_la_SOURCES= mempool.cpp buffer.cpp reader.cpp reader_norm.cpp reader_snowball.cpp plugin_snowball.cpp
libftsnowball_la_LIBADD= libstemmer_c/libstemmer.la
libftsnowball_la_LDFLAGS= -module -rpath $(pkgplugindir)
libftsnowball_la_CFLAGS= -DMYSQL_DYNAMIC_PLUGIN
libftsnowball_la_CXXFLAGS= -DMYSQL_DYNAMIC_PLUGIN
