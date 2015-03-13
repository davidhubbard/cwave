# Copyright 1999-2009 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: /var/cvsroot/gentoo-x86/dev-libs/protobuf/protobuf-2.1.0.ebuild,v 1.3 2009/05/24 22:32:50 nelchael Exp $

EAPI="2"

inherit eutils distutils elisp-common

DESCRIPTION="Google's Protocol Buffers -- C compiler"
HOMEPAGE="http://code.google.com/p/protobuf-c/"
SRC_URI="http://protobuf-c.googlecode.com/files/${PF}.tar.gz"

LICENSE="Apache-2.0"
SLOT="0"
KEYWORDS="x86 amd64"
IUSE=""

# protobuf-2.2.0 will die "file not found: google/protobuf/wire_format_inl.h"
# protobuf-c hasn't been updated to protobuf-2.2.0...
DEPEND="=dev-libs/protobuf-2.1.0"
RDEPEND="${DEPEND}"

src_prepare() {
	epatch "${FILESDIR}/${PN}-01-add-lpthread.patch"
}

src_compile() {
	emake || die
}

src_install() {
	emake DESTDIR="${D}" install
	dodoc ChangeLog TODO
}

src_test() {
	emake check
}
