
### m1b-c1 (cores 1, archive None, target None)
backup 18355600794583972943: path B, start 4966062064, stop 5174875200, window 199 MiB of log, segments 74..77, backup size 1080 MiB; verify 1.432 s
| rep | backup files | total | layout (copy + verify) | recovery (mount, replay, exit) |
|---|---|---|---|---|
| 0 | cold (fadvise DONTNEED) | 18.928 s | 10.474 s | 8.414 s |
| 1 | warm | 17.111 s | 8.711 s | 8.362 s |
| 2 | warm | 17.159 s | 8.757 s | 8.363 s |
serve run's recovery block: {'recovery_records': 540798.0, 'recovery_committed': 121678.0, 'recovery_redo_applied': 99841.0, 'recovery_analysis_us': 159879.0, 'recovery_redo_us': 368863.0, 'recovery_undo_us': 0.0, 'recovery_checkpoint_us': 12731.0, 'recovery_high_water_us': 0.0}
phases sum 0.54 s of the recovery wall time 8.36 s
layout lines: ['backup 18355600794583972943 laid out: lsn 4966062064 .. 5174875200, segments to 77, new lineage 10849046783438917940; recovering', 'restore of backup 18355600794583972943 recovered past its stop lsn 5174875200']

### m1b-c4 (cores 4, archive None, target None)
backup 14296509397782360924: path B, start 4907477288, stop 5252946384, window 329 MiB of log, segments 73..78, backup size 1099 MiB; verify 1.601 s
| rep | backup files | total | layout (copy + verify) | recovery (mount, replay, exit) |
|---|---|---|---|---|
| 0 | cold (fadvise DONTNEED) | 21.971 s | 11.275 s | 10.65 s |
| 1 | warm | 20.405 s | 9.796 s | 10.567 s |
| 2 | warm | 20.282 s | 9.748 s | 10.494 s |
serve run's recovery block: {'recovery_records': 849326.0, 'recovery_committed': 191095.0, 'recovery_redo_applied': 41606.0, 'recovery_analysis_us': 236763.0, 'recovery_redo_us': 576520.0, 'recovery_undo_us': 0.0, 'recovery_checkpoint_us': 7755.0, 'recovery_high_water_us': 0.0}
phases sum 0.82 s of the recovery wall time 10.57 s
layout lines: ['backup 14296509397782360924 laid out: lsn 4907477288 .. 5252946384, segments to 78, new lineage 15708156826655722160; recovering', 'restore of backup 14296509397782360924 recovered past its stop lsn 5252946384']

### m2-pitr-latest (cores 4, archive /home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/work/m2/archive, target latest)
backup 15350963357412918531: path B, start 618004624, stop 4256699640, window 3470 MiB of log, segments 9..63, backup size 1075 MiB; verify 5.668 s
| rep | backup files | total | layout (copy + verify) | recovery (mount, replay, exit) |
|---|---|---|---|---|
| 0 | cold (fadvise DONTNEED) | 138.532 s | 93.718 s | 44.659 s |
| 1 | warm | 138.711 s | 93.365 s | 45.171 s |
serve run's recovery block: {'recovery_records': 2617179.0, 'recovery_committed': 15742.0, 'recovery_redo_applied': 1647436.0, 'recovery_analysis_us': 12062536.0, 'recovery_redo_us': 17671322.0, 'recovery_undo_us': 1583633.0, 'recovery_checkpoint_us': 13462047.0, 'recovery_high_water_us': 0.0}
phases sum 44.78 s of the recovery wall time 44.91 s
layout lines: ['backup 15350963357412918531 laid out: lsn 618004624 .. 4256699640, segments to 156, new lineage 13287913274277792165; recovering', 'restore of backup 15350963357412918531 recovered past its stop lsn 4256699640']
