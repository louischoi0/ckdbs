#!/bin/bash
# The code evidence of section 3 of the BK close document: call structure of the per-row Keystone read in A and in B.
SP=/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad
cd $SP
mkdir -p run/evidence
{
  echo "# nm -S -C and objdump -d on the measured copies (run/bin/kds_server-A, kds_server-B); scripts callees.sh and disasm.sh"
  echo
  echo "## A (5979b5e7): DecodeRowInto, call targets inside the function"
  bash callees.sh run/bin/kds_server-A "kds::exec::DecodeRowInto"
  echo
  echo "## B (efa6adba): DecodeRowInto, call targets inside the function"
  bash callees.sh run/bin/kds_server-B "kds::exec::DecodeRowInto"
} > run/evidence/callees.txt 2>&1
bash disasm.sh run/bin/kds_server-A "kds::exec::RowKeystoneId" > run/evidence/disasm-A-RowKeystoneId.txt
bash disasm.sh run/bin/kds_server-A "kds::KeystoneIdOfPayload" > run/evidence/disasm-A-KeystoneIdOfPayload.txt
bash disasm.sh run/bin/kds_server-B "kds::KeystoneIdOfPayload" > run/evidence/disasm-B-KeystoneIdOfPayload.txt
for f in run/evidence/*; do echo "$f: $(wc -l < $f) lines, $(grep -c 'call' $f) call lines"; done
