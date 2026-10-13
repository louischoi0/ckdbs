#!/usr/bin/env python3
"""BT-S1 ablations: writes step_vm.cpp / heap_page.hpp of the source tree from
the pristine copies, with the named ablations applied. Measurement only."""
import sys

R = '/home/cdkbs/bench-runs/bt-s1'
P = R + '/pristine'
S = R + '/src'
which = set(sys.argv[1].split('+')) if sys.argv[1] != 'A' else set()
if 'all' in which:
    which = {'D1', 'D2', 'D3'}
if 'D4' in which:
    which |= {'D1', 'D2', 'D3'}

vm = open(P + '/step_vm.cpp').read()
hp = open(P + '/heap_page.hpp').read()


def sub(s, old, new):
    assert s.count(old) == 1, old[:80]
    return s.replace(old, new)


if 'D1' in which:
    # #1 moved past the rejected return.
    vm = sub(vm, "        spills_.clear();\n\n        // ---- MVCC, phase 1",
             "\n        // ---- MVCC, phase 1")
    vm = sub(vm, "        if (!StepShapeChecked(step)) {\n            if (Status s = CheckStepShapeOnce",
             "        spills_.clear();\n        if (!StepShapeChecked(step)) {\n            if (Status s = CheckStepShapeOnce")
    # #4 read only where it can be consumed.
    vm = sub(vm, "            observed_epoch = page.RelayoutEpoch();\n", "")
    vm = sub(vm, "                        walk_trx_id = tuple.value().trx_id;\n",
             "                        observed_epoch = page.RelayoutEpoch();\n"
             "                        walk_trx_id = tuple.value().trx_id;\n")
    vm = sub(vm, "                        // **The row's bytes, copied; the row itself, not\n",
             "                        observed_epoch = page.RelayoutEpoch();\n"
             "                        // **The row's bytes, copied; the row itself, not\n")
    # #2 cached once armed (wrong across steps; one-step `bn` only).
    vm = sub(vm, "        const ScanKernel* kernel = ArmedKernel(step, recording_here);",
             "        if (bt_kernel_ == nullptr) bt_kernel_ = ArmedKernel(step, recording_here);\n"
             "        const ScanKernel* kernel = bt_kernel_;")
    vm = sub(vm, "    std::vector<PendingSpill> spills_;\n",
             "    std::vector<PendingSpill> spills_;\n    const ScanKernel* bt_kernel_ = nullptr;\n")

if 'D2' in which:
    hp = sub(hp, "#include <cstdint>\n", "#include <cstdint>\n#include <cstring>\n")
    hp = sub(hp, "    StatusOr<Tuple> ReadTupleIn(std::uint16_t slot, std::uint16_t nr_slots) const;\n",
             "    StatusOr<Tuple> ReadTupleIn(std::uint16_t slot, std::uint16_t nr_slots) const;\n"
             "    bool TryReadTupleIn(std::uint16_t slot, std::uint16_t nr_slots, Tuple& t) const noexcept {\n"
             "        if (slot >= nr_slots) return false;\n"
             "        const std::byte* p = page_.data() + kHeapHeaderOffset + kHeaderSize +\n"
             "                             static_cast<std::size_t>(slot) * kSlotOnDiskSize;\n"
             "        std::uint16_t off, len; std::uint8_t fl;\n"
             "        std::memcpy(&off, p + kSlotOffsetOffset, sizeof(off));\n"
             "        std::memcpy(&len, p + kSlotLengthOffset, sizeof(len));\n"
             "        std::memcpy(&fl, p + kSlotFlagsOffset, sizeof(fl));\n"
             "        if ((fl & kSlotFlagDead) != 0 || len == 0) return false;\n"
             "        if (len < kTupleHeaderOnDiskSize || static_cast<std::size_t>(off) + len > kPageSize) return false;\n"
             "        const std::byte* b = page_.data() + off;\n"
             "        std::memcpy(&t.trx_id, b + kTupleTrxIdOffset, sizeof(t.trx_id));\n"
             "        std::memcpy(&t.undo_ptr, b + kTupleUndoPtrOffset, sizeof(t.undo_ptr));\n"
             "        t.deleted = (fl & kSlotFlagDeleted) != 0;\n"
             "        std::uint16_t dl; std::memcpy(&dl, b + kTupleDataLenOffset, sizeof(dl));\n"
             "        if (static_cast<std::size_t>(dl) + kTupleHeaderOnDiskSize > len) return false;\n"
             "        t.payload = std::span<const std::byte>(b + kTupleHeaderOnDiskSize, dl);\n"
             "        return true;\n"
             "    }\n")
    vm = sub(vm, "            auto tuple = page.ReadTupleIn(slot, nr_slots);\n            if (tuple.ok()) {\n",
             "            heap::PageView::Tuple bt_t{};\n"
             "            struct BtW { heap::PageView::Tuple& t; heap::PageView::Tuple& value() { return t; } } tuple{bt_t};\n"
             "            if (page.TryReadTupleIn(slot, nr_slots, bt_t)) {\n")

if 'D3' in which:
    vm = sub(vm, "            StepStats& rejected_stats = stats_.For(step.step_id);\n"
                 "            ++rejected_stats.rows_examined;\n"
                 "            ++rejected_stats.rows_kernel_rejected;\n"
                 "            return budget_.ChargeRow();\n",
             "            return Status::OK();\n")

if 'D4' in which:
    # The floor: the walk itself rejects (wrong by BT-R1; a price only).
    vm = sub(vm, """                if (!prefixed) {
                    return AcceptTupleAt(steps, index, step, *live_access, page_id, page, at,
                                         nr_slots);""", """                if (!prefixed) {
                    if (bt_kernel_ != nullptr) {
                        heap::PageView::Tuple t{};
                        if (page.TryReadTupleIn(at, nr_slots, t) &&
                            txn::Classify(snapshot_.view, t) == txn::Visibility::kVisible &&
                            t.payload.size() == live_access->layout.row_size &&
                            ScanKernelRejects(*bt_kernel_, live_access->layout, t.payload)) {
                            return Status::OK();
                        }
                    }
                    return AcceptTupleAt(steps, index, step, *live_access, page_id, page, at,
                                         nr_slots);""")

open(S + '/src/exec/step_vm.cpp', 'w').write(vm)
open(S + '/include/kds/storage/heap/heap_page.hpp', 'w').write(hp)
print('variant', sys.argv[1], sorted(which))
