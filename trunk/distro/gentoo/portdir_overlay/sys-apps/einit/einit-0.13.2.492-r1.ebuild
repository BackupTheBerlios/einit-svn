# Copyright 1999-2006 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: $

inherit subversion versionator

ESVN_REPO_URI="http://einit.svn.sourceforge.net/svnroot/einit/trunk/${PN}"
SRC_URI=""

DESCRIPTION="eINIT - an alternate /sbin/init"
HOMEPAGE="http://einit.sourceforge.net/"
 
ESVN_REVISION=$(get_version_component_range 4 ${PV})
ESVN_FETCH_CMD="svn co -r ${ESVN_REVISION}" 
ESVN_UPDATE_CMD="svn up -r ${ESVN_REVISION}"

LICENSE="BSD" 
SLOT="0" 
KEYWORDS="~amd64 ~x86" 
IUSE="doc efl" 

RDEPEND="dev-libs/expat 
        doc? ( app-text/docbook-sgml app-doc/doxygen ) 
        efl? ( media-libs/edje
               x11-libs/evas
               x11-libs/ecore )" 
DEPEND="${RDEPEND}" 

S=${WORKDIR}/einit 

src_unpack() {
        subversion_src_unpack
        cd "${S}"
}

src_compile() {
        local myconf

        myconf="--svn --ebuild"

        if use efl ; then
                myconf="${myconf} --enable-linux --use-posix-regex --prefix=${ROOT} --enable-efl"
        else
                myconf="${myconf} --enable-linux --use-posix-regex --prefix=${ROOT}"
        fi

        econf ${myconf} || die
        emake || die

        if use doc ; then
                make documentation || die
        fi
}

src_install() {
        emake -j1 install DESTDIR="${D}" || die
        dodoc AUTHORS ChangeLog COPYING
        if use doc ; then
                dohtml build/documentation/html/*
        fi
}

