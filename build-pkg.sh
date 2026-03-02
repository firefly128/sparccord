#!/bin/sh
# build-pkg.sh - Build SPARCcord SVR4 package on Solaris
# Run this on the SPARCstation (or any Solaris 7 SPARC system)
#
# Usage: sh build-pkg.sh
#
# Produces: sparccord-0.1-sparc.pkg

PKGNAME="JWsprccrd"
VERSION="0.1"
BASEDIR="/opt/sparccord"
SRCDIR=`pwd`
BUILDDIR="/tmp/sparccord-build"
STAGEDIR="/tmp/sparccord-stage"
PKGDIR="/tmp/sparccord-pkg"

echo "=== SPARCcord SVR4 Package Builder ==="
echo "Source: ${SRCDIR}"
echo ""

# Clean previous builds
rm -rf ${BUILDDIR} ${STAGEDIR} ${PKGDIR} /tmp/sparccord-spool
mkdir -p ${BUILDDIR}
mkdir -p ${STAGEDIR}${BASEDIR}/bin
mkdir -p ${PKGDIR}
mkdir -p /tmp/sparccord-spool

# Copy source and build
echo "--- Compiling ---"
cp ${SRCDIR}/*.c ${SRCDIR}/*.h ${SRCDIR}/Makefile ${BUILDDIR}/
cd ${BUILDDIR}
make clean 2>/dev/null
make
if [ $? -ne 0 ]; then
    echo "BUILD FAILED"
    exit 1
fi
echo "Build OK"

# Stage files
echo "--- Staging ---"
cp ${BUILDDIR}/sparccord ${STAGEDIR}${BASEDIR}/bin/
cp ${SRCDIR}/README.md ${STAGEDIR}${BASEDIR}/ 2>/dev/null

echo "Staged to ${STAGEDIR}"

# Generate prototype file
echo "--- Generating prototype ---"
PSTAMP=`date '+%Y%m%d%H%M%S'`
cat > ${PKGDIR}/pkginfo <<EOF
PKG=${PKGNAME}
NAME=SPARCcord - Discord Client for Solaris 7
ARCH=sparc
VERSION=${VERSION}
CATEGORY=application
VENDOR=Julian Wolfe
EMAIL=
PSTAMP=${PSTAMP}
BASEDIR=${BASEDIR}
CLASSES=none
EOF

cp ${SRCDIR}/pkg/depend ${PKGDIR}/
cp ${SRCDIR}/pkg/postinstall ${PKGDIR}/
cp ${SRCDIR}/pkg/preremove ${PKGDIR}/

# Build prototype
cat > ${PKGDIR}/prototype <<PROTO
i pkginfo
i depend
i postinstall
i preremove
d none bin 0755 root bin
f none bin/sparccord 0755 root bin
PROTO

# Add README if staged
if [ -f ${STAGEDIR}${BASEDIR}/README.md ]; then
    echo "f none README.md 0644 root bin" >> ${PKGDIR}/prototype
fi

echo "Prototype:"
cat ${PKGDIR}/prototype
echo ""

# Build package
echo "--- Building package ---"
pkgmk -o -r ${STAGEDIR}${BASEDIR} -d /tmp/sparccord-spool -f ${PKGDIR}/prototype

# Convert to datastream
echo "--- Creating datastream package ---"
OUTPKG="${SRCDIR}/sparccord-${VERSION}-sparc.pkg"
pkgtrans -s /tmp/sparccord-spool ${OUTPKG} ${PKGNAME}

echo ""
echo "=== Package built successfully ==="
echo "Output: ${OUTPKG}"
ls -la ${OUTPKG}
echo ""
echo "Install with: pkgadd -d ${OUTPKG}"

# Cleanup
rm -rf "${BUILDDIR}" "${STAGEDIR}" "${PKGDIR}" /tmp/sparccord-spool
