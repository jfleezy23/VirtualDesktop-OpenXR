#pragma once
#include "pair_capture_nr_abi.h"
/* Candidate host-transfer policy, separate from the frozen general packet
   validator (which also accepts appended final FG records). Actual retained
   reference counts remain exact; these words are conservative prepaid ceilings.
   The shared three-reference scratch pool is assigned to B: 89+35+3=127. */
static inline int nr_capture_reference_reservation(const NrPacketV1* packet) {
    return packet && packet->header.operation == NR_FRAME_TRANSFER && packet->header.frameOrdinal < 2 &&
           packet->header.flags <= 1 &&
           packet->header.refCount <= ((packet->header.flags == 1 && packet->header.frameOrdinal == 1) ? 124u : 89u) &&
           packet->budget.senderRefCountA == 89 && packet->budget.senderRefCountB == 89 &&
           packet->budget.receiverRefCountA == 89 && packet->budget.receiverRefCountB == 127;
}
static inline int nr_capture_host_reference_reservation(const NrPacketV1* packet) {
    return nr_capture_reference_reservation(packet) && packet->header.flags == 0;
}
