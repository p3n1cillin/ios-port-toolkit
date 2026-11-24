#!/bin/bash
# mktest.sh A B KERNEL_MACHO ROOT_NEW "message": gen script B from A (A's gen must exist), build loader/shim/package
set -e
A=$1; B=$2; K=$3; ROOT=$4; MSG=$5
# (Git Bash on Windows, WSL for the Linux build steps.) R = this port as Git Bash sees it, RL = the same from WSL,
# S = where the genNNN.py test generators are (pipeline/gen: one per test, each made from the one before)
R=${R5Q_PORT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
RL=${R5Q_PORT_WSL:-$(echo "$R" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')}
S=${GEN_DIR:-$R/pipeline/gen}
cd $R
P=$((A-1))
python - "$S/gen$A.py" "$S/gen$B.py" "$A" "$B" "$K" "$ROOT" "$MSG" <<'EOF'
import re, sys
src, dst, A, B, K, ROOT, MSG = sys.argv[1:]
s = open(src).read()
a, b = int(A), int(B)
s = re.sub(r'A, B = \d+, \d+', 'A, B = %d, %d' % (a, b), s)
s = re.sub(r"shutil.copyfile\(R / 'port/build/xnu-r5q-t\d+(?:-[\w-]+)?\.macho'", "shutil.copyfile(R / 'port/build/%s'" % K, s)
s = re.sub(r"replace\(b'TEST\d+', b'TEST\d+'\)", "replace(b'TEST%d', b'TEST%d')" % (a, b), s)
s = re.sub(r"if \"'--test\d+'\" not in t:", "if \"'--test%d'\" not in t:" % b, s)
s = re.sub(r"t.replace\(\"\('--test\d+',\): 'build-test\d+.sh', \", \"\('--test\d+',\): 'build-test\d+.sh', \('--test\d+',\): 'build-test\d+.sh', \"\)",
           "t.replace(\"('--test%d',): 'build-test%d.sh', \", \"('--test%d',): 'build-test%d.sh', ('--test%d',): 'build-test%d.sh', \")" % (a, a, a, a, b, b), s)
s = re.sub(r"port/build/test\d+-root(?:-[\w-]+)?\.apfs(?:\.new)?", "port/build/%s" % ROOT, s)
s = re.sub(r"print\('generated test\d+: [^']*'\)", "print('generated test%d: %s')" % (b, MSG.replace("'", "").replace("\\", "/")), s)   # (a quote in MSG broke the generator)
open(dst, 'w').write(s)
EOF
python "$S/gen$B.py"
V=1 wsl -e bash -c "R=$RL; cd /tmp && python3 -I \$R/build_loader_linux.py \$R/port/xnuloader \$R/port/build/r5q-test$B-loader.efi --test$B 2>&1 | grep -iE 'error' | head -5; cd \$R && python3 build_test${B}_shim.py \$R/port 2>&1 | tail -1 && python3 package_install_boot.py . $B 2>&1 | grep -m1 sha256" 2>&1 | grep -v Nmap
