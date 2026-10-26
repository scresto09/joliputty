#!/bin/sh
# make_help.sh: make JoliPuTTY's online manual, in html/<version>/
# beside this script (as it goes on the web site, where JoliPuTTY's Help
# looks for it: see JOLIPUTTY_HELP in winui-window.c, and
# .github/workflows/help.yml, which puts it there for each release):
# PuTTY's own manual (from the PuTTY source's doc directory),
# with a chapter of its own on what JoliPuTTY does differently
# (src/joliputty.but), its own preface (src/blurb.but, in place of
# PuTTY's), and its own style (src/joliputty.css).
#
# It needs Halibut, PuTTY's documentation tool: from $HALIBUT, or the
# PATH, or else it's fetched and built here, in .halibut (which takes
# git, cmake and a C compiler). And Perl, to make the licence sections
# from the PuTTY source's LICENCE.
#
#   ./make_help.sh [path to the PuTTY source]
#
# (by default .., the source this directory is in)
#
# The version is JoliPuTTY's, from the git tags, as the build has it (see
# cmake/joliputty-version.cmake): 0.2 on the joliputty-0.2 tag, or else
# dev, 0.2-5-gabc1234 or the like. It's also written to html/version,
# for whatever publishes it.
#
# Three of PuTTY's chapters (gs.but, using.but and config.but) get notes
# saying what JoliPuTTY does differently, from src/putty-doc.patch,
# applied to copies of them. If the patch no longer applies, after
# PuTTY's manual has changed, remake it: copy those files to a/ and b/,
# put the notes back in b/, and run "diff -ru a b > src/putty-doc.patch".

set -e

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/..}" && pwd)
doc=$src/doc

[ -f "$doc/config.but" ] || { echo "$doc: not PuTTY's doc directory" >&2; exit 1; }

# Halibut
if [ -z "$HALIBUT" ]; then
    HALIBUT=$(command -v halibut || true)
fi
if [ -z "$HALIBUT" ]; then
    HALIBUT=$here/.halibut/build/halibut
    if [ ! -x "$HALIBUT" ]; then
        echo "Building Halibut in $here/.halibut"
        [ -d "$here/.halibut" ] ||
            git clone -q https://git.tartarus.org/simon/halibut.git \
                "$here/.halibut"
        cmake -S "$here/.halibut" -B "$here/.halibut/build" \
            -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$here/.halibut/build" --target halibut >/dev/null
    fi
fi

# JoliPuTTY's version, and the PuTTY one it's based on (as
# cmake/joliputty-version.cmake has them)
version=$(git -C "$src" describe --tags --match 'joliputty-*' --dirty \
              2>/dev/null || true)
version=${version#joliputty-}
[ -n "$version" ] || version=dev
base=$(git -C "$src" describe --tags --match '[0-9]*' --abbrev=0 \
           2>/dev/null || true)
[ -n "$base" ] || base=$(head -n 1 "$src/LATEST.VER")
out=$here/html/$version

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
perl "$src/licence.pl" --copyrightdoc -o "$tmp/copy.but"
perl "$src/licence.pl" --licencedoc -o "$tmp/licence.but"
printf '\\versionid JoliPuTTY %s, based on PuTTY %s\n' \
    "$version" "$base" > "$tmp/version.but"
mkdir "$tmp/doc"
cp "$doc/gs.but" "$doc/using.but" "$doc/config.but" "$tmp/doc/"
patch -s -p1 -d "$tmp/doc" < "$here/src/putty-doc.patch"

rm -rf "$out"
mkdir -p "$out"
cd "$out"
# (in the order of PuTTY's doc/CMakeLists.txt, with JoliPuTTY's chapter
# after the introduction, and its section of the privacy appendix at the
# end of it)
"$HALIBUT" --html \
    "$tmp/copy.but" \
    "$here/src/blurb.but" \
    "$doc/intro.but" \
    "$here/src/joliputty.but" \
    "$tmp/doc/gs.but" \
    "$tmp/doc/using.but" \
    "$tmp/doc/config.but" \
    "$doc/pscp.but" \
    "$doc/psftp.but" \
    "$doc/plink.but" \
    "$doc/pubkey.but" \
    "$doc/pageant.but" \
    "$doc/errors.but" \
    "$doc/faq.but" \
    "$doc/feedback.but" \
    "$doc/pubkeyfmt.but" \
    "$tmp/licence.but" \
    "$doc/udp.but" \
    "$doc/pgpkeys.but" \
    "$doc/sshnames.but" \
    "$doc/authplugin.but" \
    "$doc/privacy.but" \
    "$here/src/privacy.but" \
    "$doc/index.but" \
    "$tmp/version.but"
# (the Welcome tab's logo at the top of the first page, under its title)
perl -0pi -e '
    BEGIN { local $/; open my $f, "<", shift or die; $logo = <$f> }
    s!</h1>\n!</h1>\n$logo!' "$here/src/logo.html" "$out/index.html"
cp "$here/src/joliputty.css" "$out/"
echo "$version" > "$here/html/version"
echo "Made $out/index.html"
