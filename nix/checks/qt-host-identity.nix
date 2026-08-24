# qt-host-identity — the logos-qt-host a consumer LINKS must be the one in that
# consumer's own closure.
#
# WHY THIS CHECK EXISTS
#
# logos-qt-sdk exports to its consumers the logos-qt-host that IT was built
# against, rather than the one in the consumer's closure, by three routes at
# once — a propagated build input, a store path baked into
# logos-qt-sdkConfig.cmake as find_package HINTS, and (on older revs) forwarding
# headers that #include an absolute store path. A consumer that takes
# logos-qt-sdk therefore silently compiles and links against a qt-host it never
# chose. When a Q_INVOKABLE is added to the Qt host runtime, the consumer cannot
# see it: QMetaObject::invokeMethod fails at RUNTIME with "No such method", and
# the build stays GREEN. That is how currentCallerJson came to be missing from
# logos_host_qt in every module process, collapsing current_caller() to
# {"kind":"unknown"} fleet-wide.
#
# WHY THESE THREE LAYERS, AND WHY NOT FEWER (each point is measured)
#
#   layer 0 — closure cardinality. Catches DUPLICATION: two logos-qt-host in one
#     build. It CANNOT catch staleness. Measured: the tree on which
#     currentCallerJson is invisible in every image has exactly ONE qt-host in
#     its closure, so a closure-cardinality check alone is GREEN on the very
#     defect this file exists for.
#
#   layer 1 — store-path scan of consumed prefixes. All three export routes are
#     TEXT naming a store path, so one scan catches all three. Measured: 13
#     files in one logos-qt-sdk prefix name a foreign qt-host — 1 propagated
#     input, 1 CMake HINTS, 11 forwarding headers. NOTE the scan follows
#     symlinks and reads the TARGET's content: logos-qt-sdk is a symlinkJoin, so
#     nix-support/propagated-build-inputs is a symlink into the -lib derivation,
#     and both `grep -r` and a link-text-only scanner report that prefix as
#     CLEAN while the stale path sits in the target.
#
#   layer 2 — metaobject identity on the BUILT images. This is the one that
#     catches staleness, and it is the only layer that can see a STATICALLY
#     linked consumer. Measured: logos_host_qt links liblogos_qt_host.a
#     statically and its entire runtime closure contains ZERO "-logos-qt-host-"
#     paths, so no store-path comparison can identify what it linked. Symbol-set
#     containment does not work either: a correctly built logos_host_qt is
#     missing 223 of its qt-host's 609 defined symbols to function-level DCE.
#     A moc metaobject blob, by contrast, is ONE INDIVISIBLE LINKER OBJECT —
#     --gc-sections takes it whole or not at all — and it encodes every
#     Q_INVOKABLE name, signature and parameter of the class. Byte-identity of
#     that blob is therefore DCE-proof, and it is the exact ABI whose mismatch
#     produced the bug, so it catches the NEXT Qt-facing API rather than only
#     currentCallerJson.
#
# Layers 0 and 2 are complements, not redundancy — measured in both directions:
# a tree with two qt-hosts whose metaobjects are byte-identical passes layer 2
# and fails layer 0; the fleet-wide stale tree passes layer 0 and fails layer 2.
#
# ANTI-VACUITY. Every layer's PASS verdict is a "found nothing", which is the
# classic way a check like this goes green over an empty set. So the check FAILS
# WITH EXIT 2 (distinct from a violation's exit 1) if: the declared prefix has no
# qt-host library; it defines zero metaobject blobs; no image was examined; no
# examined image carries any of the declared qt-host's classes; zero blobs were
# compared; or a scanned prefix yielded zero store-path references of ANY kind
# (every real nix prefix names some store path, so zero means the scan read
# nothing).
#
# USAGE
#
#   checks.qt-host-identity = import ./nix/checks/qt-host-identity.nix {
#     inherit pkgs;
#     declaredQtHost = logosQtHost;          # this repo's LOGOS_QT_HOST_ROOT
#     subjects       = [ self.packages.${system}.default ];
#     scanPrefixes   = [ logosQtSdk ];       # prefixes that must not name a qt-host
#   };
{ pkgs
, declaredQtHost
, subjects        # derivations whose output trees are searched for images
, scanPrefixes ? []
, script ? ./qt_host_identity.py
}:

let
  closure = pkgs.closureInfo { rootPaths = subjects; };
  imageArgs = builtins.concatStringsSep " "
    (map (s: "--image-dir ${s}") subjects);
  scanArgs = builtins.concatStringsSep " "
    (map (s: "--scan ${s}") scanPrefixes);
in
pkgs.runCommand "qt-host-identity"
{
  nativeBuildInputs = [ pkgs.python3 pkgs.binutils ];
  # Named so a failure log says which qt-host was expected without having to
  # re-derive it from the flake.
  passthru = { inherit declaredQtHost; };
} ''
  set -euo pipefail

  # `ar` is used to open liblogos_qt_host.a; if binutils ever drops out of the
  # inputs the check would read only the .so and silently narrow its class set,
  # so require it up front rather than discovering it as an empty comparison.
  command -v ar >/dev/null || { echo "FAIL: ar is missing; the static archive "\
    "could not be opened and the comparison would be narrowed silently"; exit 2; }

  python3 ${script} \
    --declared ${declaredQtHost} \
    --closure-file ${closure}/store-paths \
    ${imageArgs} ${scanArgs} | tee report.txt
  rc=''${PIPESTATUS[0]}

  if [ "$rc" -eq 2 ]; then
    echo ""
    echo "qt-host-identity could not make a meaningful assertion (see above)."
    echo "That is a FAILURE, not a skip: a check whose pass verdict is 'found"
    echo "nothing' cannot distinguish 'nothing wrong' from 'looked at nothing'."
    exit 1
  fi
  [ "$rc" -eq 0 ] || exit 1

  mkdir -p $out
  cp report.txt $out/report.txt
''
