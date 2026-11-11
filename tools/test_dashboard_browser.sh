#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
# test_dashboard_browser.sh [CHROME] - headless-Chrome checks of docs/dashboard.html function paths:
#   1. URL state: ?cond=nominal&arms=single_pass,corr_aware_retry is applied (body data-cond / data-arms);
#   2. invalid params (cond=zzz, arms=bogus) fall back to field / all arms;
#   3. no horizontal page scroll at 390 px (the page is loaded in a 390 px iframe: headless Chrome clamps windows
#      to >= 500 px, an iframe does not);
#   4. a 390x844 screenshot can be taken.
set -u
DIR=$(cd "$(dirname "$0")/.." && pwd)
PAGE=$DIR/docs/dashboard.html
CH=${1:-$(command -v google-chrome || command -v chromium || command -v chromium-browser)}
[ -n "$CH" ] || { echo "test_dashboard_browser: SKIP (no chrome)"; exit 0; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "test_dashboard_browser: FAIL: $*" >&2; exit 1; }
run() { timeout 60 "$CH" --headless=new --no-sandbox --disable-gpu "$@" 2>/dev/null; }
body() { run --hide-scrollbars --virtual-time-budget=3000 --allow-file-access-from-files --dump-dom "$1" | grep -o '<body[^>]*>' | head -1; }

b=$(body "file://$PAGE?cond=nominal&arms=single_pass,corr_aware_retry")
echo "$b" | grep -q 'data-cond="nominal"' || fail "cond param not applied: $b"
echo "$b" | grep -q 'data-arms="single_pass,corr_aware_retry"' || fail "arms param not applied: $b"
echo "$b" | grep -q 'data-ready="1"' || fail "page script did not finish: $b"

b=$(body "file://$PAGE?cond=zzz&arms=bogus")
echo "$b" | grep -q 'data-cond="field"' || fail "invalid cond did not fall back to field: $b"
echo "$b" | grep -q 'data-arms="single_pass,naive_retry,corr_aware_retry"' || fail "invalid arms did not fall back: $b"

cat >"$T/frame.html" <<HTML
<!DOCTYPE html><html><body style="margin:0"><iframe id="f" src="file://$PAGE" style="width:390px;height:844px;border:0"></iframe>
<script>
document.getElementById('f').addEventListener('load', function () {
  var d = this.contentDocument.documentElement;
  setTimeout(function () { document.body.setAttribute('data-sw', d.scrollWidth + '/' + d.clientWidth); }, 300);
});
</script></body></html>
HTML
b=$(body "file://$T/frame.html")
sw=$(echo "$b" | sed -n 's/.*data-sw="\([0-9]*\)\/\([0-9]*\)".*/\1 \2/p')
[ -n "$sw" ] || fail "could not measure the 390 px layout: $b"
set -- $sw
[ "$2" -eq 390 ] || fail "iframe viewport is $2 px, expected 390"
[ "$1" -le 390 ] || fail "horizontal scroll at 390 px: scrollWidth=$1"

run --hide-scrollbars --window-size=390,844 --screenshot="$T/m.png" "file://$PAGE" >/dev/null
[ -s "$T/m.png" ] || fail "screenshot at 390x844 failed"
echo "test_dashboard_browser: OK (URL state, fallback, no horizontal scroll at 390 px: $1/$2, screenshot)"
