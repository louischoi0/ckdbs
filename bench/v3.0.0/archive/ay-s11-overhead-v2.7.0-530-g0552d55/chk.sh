#!/bin/bash
# behavioural proof that B's fence is live and A has none (not a price)
R=/home/cdkbs/bench-runs/ay-s11
rm -rf $R/chk; mkdir -p $R/chk
for X in B A; do
  if [ $X = B ]; then P=15602; else P=15603; fi
  cat > $R/chk/$X.conf <<EOF
data_file = $R/chk/$X.db
cores = 1
port = $P
durability = relaxed
log_dir = $R/chk
log_file = $X.log
log_level = warn
EOF
  $R/kds_server-$X --config $R/chk/$X.conf > $R/chk/$X.stdout 2>&1 &
  echo $! > $R/chk/$X.pid
done
sleep 2
cd /home/cdkbs/ckdbs/.claude/worktrees/ay-s11-close/tools
python3 - <<'EOF'
import time
from ckdbs_cli import ServerConnection
for name,port in (("B",15602),("A",15603)):
    c=ServerConnection("127.0.0.1",port); d=ServerConnection("127.0.0.1",port)
    c.send_command("CREATE TABLE p (id int64, v int64) BTREE"); c.send_command("CREATE TABLE ch (id int64, pid int64 REFERENCES p, v int64) BTREE")
    for i in range(1,4): c.send_command(f"INSERT INTO p VALUES ({i},0)")
    c.send_command("BEGIN"); print(name,c.send_command("INSERT INTO ch VALUES (100,1,0)"))
    t=time.time(); r=d.send_command("DELETE FROM p WHERE id = 1"); print(name,"parent delete during open child txn:",r,f"{time.time()-t:.2f}s")
    print(name,c.send_command("COMMIT"))
EOF
kill -TERM $(cat $R/chk/B.pid) $(cat $R/chk/A.pid)
