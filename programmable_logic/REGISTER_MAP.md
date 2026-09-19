# NCLP programmable-logic register maps

This document describes the current cleaned contracts for the project-owned
AXI-Lite blocks. All registers are 32-bit and all offsets are byte offsets. The
previous register layouts are not retained. Each block reports its own ABI
version as listed below.
The four BD subsystems are capture, compute, stimulation and platform. Capture,
compute and stimulation expose project-owned AXI-Lite controls. The SFP packet
mailbox is a separate project-owned AXI-Lite peripheral in the compute group.

The Zynq `pl_ps_irq0` input is a three-bit, active-high level vector:

| Bit | Source | Deassertion |
| --- | --- | --- |
| 0 | Intan-to-DDR transfer IRQ, Intan error, or stimulation fault | ACK/clear each pending source |
| 1 | Compute-fabric fault | Clear the fabric diagnostic source or disable its fault IRQ |
| 2 | SFP packet-mailbox interrupt | Pop RX data, clear TX completion, or clear the enabled mailbox error |

The public `NCLP_PL_IRQ_*` constants specify these bit positions. Production
firmware must service the fabric and mailbox independently. The TTL
router does not add an IRQ bit. Use the generated BSP/OS interrupt mapping when
registering handlers. [AMD PG201](https://docs.amd.com/r/en-US/pg201-zynq-ultrascale-plus-processing-system/Programmable-Logic-Clocks-and-Interrupts)

Each owned block begins with the same identity header:

| Offset | Register | Meaning |
|---:|---|---|
| `0x00` | `BLOCK_ID` | Four-byte ASCII block identity |
| `0x04` | `ABI_VERSION` | Major/minor ABI version |
| `0x08` | `CAPABILITIES` | Implemented optional features |
| `0x0C` | `INFO` or block-specific `LIMITS` | Fixed geometry or limits |

Software verifies `BLOCK_ID`, the ABI major, the required capability subset,
and the `0x0C` geometry/limits word where the block defines it. Reserved or unaligned reads return zero
and writes have no effect.

| Block | PS base | Aperture | ABI | `BLOCK_ID` | `INFO` |
|---|---:|---:|---:|---:|---:|
| Intan SPI | `0x80000000` | 64 KiB | v3 | `0x494E544E` (`INTN`) | `0x10100400` |
| DDR circular writer | `0x80010000` | 4 KiB | v2 | `0x44445257` (`DDRW`) | `0x00100802` |
| LED control | `0x80011000` | 4 KiB | v2 | `0x4C454453` (`LEDS`) | `0x00000605` |
| Stimulation/DAC | `0x80012000` | 8 KiB | v3 | `0x5354494D` (`STIM`) | `0x02040400` |
| TTL output router | `0x80014000` | 4 KiB | v2 | `0x54544C52` (`TTLR`) | `0x00040202` |
| Ripple detector | `0x80020000` | 4 KiB | v3 | `0x52505754` (`RPWT`) | `0x001E0100` |
| Compute fabric | `0x80021000` | 4 KiB | v4 | `0x4E434650` (`NCFP`) | reserved (`0`) |
| SFP packet mailbox | `0x80030000` | 4 KiB | v2 | `0x4E504D42` (`NPMB`) | `0x00280040` |

Firmware constants are in `software/common/nclp_pl_registers.h`. The maps group
lifecycle commands/status, configuration, runtime results, diagnostics, and
memory windows by function. Redundant identity/state readbacks are removed;
all live controls and safety diagnostics remain. Retired register addresses
have no compatibility aliases. Unassigned addresses remain
inert. Drivers compare ABI majors with `NCLP_ABI_MAJOR_MASK` and check their
required capability subset. The compute fabric owns Intan destination selection,
local decoding, SFP packet routing, trigger validation, and diagnostics. Aurora
transport has no register bank. The packet mailbox owns fixed PS command/reply
queues without accessing DDR; the capture writer is the only PL DDR master.
STIM remains the only owner of DAC configuration and status.

## Intan SPI v3

ABI v3 exposes two independently enabled copies of one compact raw stream:
`M_AXIS_RECORDING` for the DDR writer and `M_AXIS_COMPUTE` for the compute
router. It removes the dedicated 64-bit sample output. All memory windows and
live diagnostics remain.

The two outputs preserve identical 16-bit payload words. On `M_AXIS_COMPUTE`,
the final TTL word of each complete frame carries `TKEEP=0b11, TLAST=1` so the
compute switch knows the raw frame boundary. The selected SFP packetizer buffers
the complete frame before output. Recording keeps payload `TLAST=0`. Both outputs mark session EOS with
`TKEEP=0b00, TLAST=1`.

The Intan block is one flat PS-visible ABI. There is no command mailbox and no
second internal register-ID map. Register accesses are direct 32-bit MMIO, and
three direct windows expose all AUX command RAM banks.

| Offset | Register | Access | Meaning |
|---:|---|---|---|
| `0x00-0x0C` | Identity header | RO | `INTN`, ABI `0x00030000`, capabilities `0x3FF`, geometry |
| `0x10` | `ACQUISITION_COMMAND` | W1P | bit 0 `START`; reads zero |
| `0x14` | `ACQUISITION_STATUS` | RO | running, START pending, access locked, START rejected |
| `0x20` | `ACQUISITION_CONFIG` | RW | bit 0 continuous, bit 1 DSP settle, bit 2 init dummy |
| `0x24` | `FINITE_FRAME_COUNT` | RW | complete 32-bit finite frame count |
| `0x28` | `LOGICAL_STREAM_ENABLE` | RW | enabled logical streams `[15:0]` |
| `0x2C` | `OUTPUT_CONFIG` | RW | bit 0 recording output, bit 1 compute output |
| `0x30` | `MISO_PHASE_PRIMARY` | RW | eight packed 4-bit primary-lane phases |
| `0x34` | `MISO_PHASE_SECONDARY` | RW | eight packed 4-bit secondary-lane phases |
| `0x40` | `SAMPLE_CLOCK_COMMAND` | W1P | bit 0 `APPLY`; reads zero |
| `0x44` | `SAMPLE_CLOCK_CONFIG` | RW | O `[7:0]`, D `[11:8]`, M `[18:12]` |
| `0x48` | `SAMPLE_CLOCK_STATUS` | RO | locked, ready, busy, error, error code `[6:4]` |
| `0x50/0x54/0x58` | `AUX1_BANK_SELECT/END_INDEX/LOOP_INDEX` | RW | bank fields `[7:0]`, inclusive end `[9:0]`, loop start `[9:0]` |
| `0x60/0x64/0x68` | `AUX2_BANK_SELECT/END_INDEX/LOOP_INDEX` | RW | same fields for AUX2 |
| `0x70/0x74/0x78` | `AUX3_BANK_SELECT/END_INDEX/LOOP_INDEX` | RW | same fields for AUX3 |
| `0x80` | `FAST_SETTLE_CONFIG` | RW | enable bit 0; channel `[7:4]` |
| `0x90` | `SYNC_MODE` | RW | 0 off, 1 periodic, 2 recording gate |
| `0x94` | `SYNC_PERIOD_FRAMES` | RW | periodic-sync period in complete sample frames |
| `0x98` | `SYNC_HIGH_FRAMES` | RW | periodic-sync high time in complete sample frames |
| `0xA0` | `OUTPUT_STATUS` | RO | source loss, recording session end seen, recording active/stall |
| `0xA4` | `DIAGNOSTIC_COMMAND` | W1P | bit 0 clears output diagnostics/counters; reads zero |
| `0xA8/0xAC` | `RECORDING_PAYLOAD_WORD_COUNT_LO/HI` | RO | accepted 16-bit recording payload-word count |
| `0xB0` | `RECORDING_END_OF_SESSION_MARKER_COUNT` | RO | accepted recording end-of-session markers |
| `0xB4` | `UNACCEPTED_SOURCE_EVENT_COUNT` | RO | source events rejected by an enabled output |
| `0xB8` | `RECORDING_STALL_CYCLE_COUNT` | RO | SPI-source clock cycles stalled by recording backpressure |
| `0xC0` | `ERROR_STATUS` | RO/W1C | sticky incident vector |
| `0xC4` | `ERROR_ENABLE` | RW | interrupt-enable vector |
| `0xC8` | `ERROR_INCIDENT_COUNT` | RO | total recorded incidents |
| `0xCC` | `LAST_ERROR_CODE` | RO | most recent incident code |
| `0xE0/0xE4` | `STREAM_SOURCE_MAP_LO/HI` | RO | fixed logical-to-physical source descriptor `0xF7E6D5C4_B3A29180` |

`CAPABILITIES` bit assignments are: flat register map (0), recording output
stream (1), compute output stream (2), periodic sync (3), recording gate (4), error
IRQ (5), coherent stimulation marker (6), direct AUX windows (7), AUX RAM
readback (8), and coherent START snapshot (9).

`START` is accepted only when at least one logical stream is enabled,
configuration is unlocked, both prior output paths and their EOS markers have
drained, either continuous mode is set or
`FINITE_FRAME_COUNT` is nonzero, every AUX `LOOP_INDEX <= END_INDEX`, and the
sync mode/timing tuple is valid, with no active clock-manager error. An accepted
request waits if the sample clock is temporarily not ready. When recording output is
enabled, the DDR writer must be armed before software writes
`ACQUISITION_COMMAND.START`; compute-only sessions leave the writer idle.
DSP settle, init dummy, finite
count, stream mask, output routing, both full phase words, AUX selection and
indices, fast settle, sync configuration, and the stimulation marker mask join
one coherent START snapshot. The continuous bit remains live so clearing it can
request a clean frame-boundary stop.

To change sample rate, write the complete packed O/D/M value to
`SAMPLE_CLOCK_CONFIG`, write `SAMPLE_CLOCK_COMMAND.APPLY`, observe BUSY, and
wait for BUSY to clear with READY and LOCKED set and ERROR clear. APPLY is
accepted while stopped and then held BUSY until both output FIFOs and pending
end markers have drained; only then may the clock manager reset the SPI clock
domain. The supported tuples all use D=4:
`(O,M)=(80,32),(40,32),(30,36),(20,32),(16,32),(15,36)`.

Periodic sync is defined in frames:

```text
frequency = Intan sample rate / SYNC_PERIOD_FRAMES
duty      = SYNC_HIGH_FRAMES / SYNC_PERIOD_FRAMES
```

Period and high time must be nonzero and high must not exceed period. Recording
gate is high only while recording.

The 64-KiB aperture assigns these direct AUX command windows:

| Offset range | Contents |
|---:|---|
| `0x4000-0x7FFC` | AUX1 banks A-D |
| `0x8000-0xBFFC` | AUX2 banks A-D |
| `0xC000-0xFFFC` | AUX3 banks A-D |

Each window contains four banks of 1024 16-bit commands. Command `(bank,index)`
is addressed at `window + (bank * 1024 + index) * 4`. Reads return the stored
16-bit command in bits `[15:0]`. Writes are rejected while configuration is
locked or acquisition is active.

`ERROR_STATUS` bits 0 through 6 are source-event loss, recording stall, rejected AUX
write, rejected START, rejected sample-clock request, rejected control write,
and sample-clock failure. `LAST_ERROR_CODE` uses 0 for none and 1 through 7 in
that same order. The error-enable reset value enables source-event loss and
sample-clock failure. A runtime clock failure aborts the SPI domain without an
EOS marker; the sticky clock-failure IRQ tells software to abort the paired DDR
session. A successful later APPLY is the supported clock-recovery path.
Diagnostic CLEAR requests use a request/acknowledge CDC handshake; a second
CLEAR while the first is still pending is rejected as a control-write error.

## DDR circular writer v2

Software programs shadow geometry, then issues START to accept it atomically.
`CONSUMED_BLOCK_COUNT` is the single live RW absolute consumer count; it is not
START-latched. ABI v2 removes its duplicate readback and the duplicate numeric
state field from STATUS. No old-address aliases are provided.

| Offset | Register group | Access | Meaning |
|---:|---|---|---|
| `00-0C` | Identity header | RO | `DDRW`, ABI v2, capabilities `0x0F`, geometry |
| `10` | `COMMAND` | W1P | START, ABORT, SOFT_RESET, IRQ_ACK, SNAPSHOT |
| `14` | `STATUS` | RO | run/data/IRQ/EOS/fault flags; bits `[31:14]` reserved |
| `18` | `SESSION_STATE` | RO | explicit numeric state |
| `1C` | `SESSION_ID` | RO | increments on accepted START |
| `20/24` | `RING_BASE_ADDR_LO/HI` | RW | DDR ring base |
| `28` | `RING_SIZE_BYTES` | RW | total extent; checked against block size × capacity |
| `2C` | `BLOCK_SIZE_BYTES` | RW | fixed consumer slot size |
| `30` | `RING_CAPACITY_BLOCKS` | RW | number of slots |
| `34` | `IRQ_COMPLETION_INTERVAL_BLOCKS` | RW | produced-block IRQ interval |
| `40/44` | `CURRENT_WRITE_ADDR_LO/HI` | RO | active session write address |
| `48` | `CURRENT_WRITE_OFFSET_BYTES` | RO | offset relative to active ring base |
| `4C/50` | `LAST_PRODUCED_BLOCK_ADDR_LO/HI` | RO | last committed block address |
| `54` | `LAST_PRODUCED_BLOCK_SIZE_BYTES` | RO | bytes in last committed block |
| `60` | `CONSUMED_BLOCK_COUNT` | RW | absolute released-block count; cannot exceed produced count |
| `64` | `PRODUCED_BLOCK_COUNT` | RO | committed blocks |
| `68` | `OUTSTANDING_BLOCK_COUNT` | RO | produced minus consumed |
| `6C` | `FINAL_BLOCK_SIZE_BYTES` | RO | exact terminal block size |
| `70/74` | `FIFO_LEVEL_ENTRIES/FIFO_HIGH_WATER_ENTRIES` | RO | live occupancy and peak |
| `80` | `ERROR_STATUS` | RO/W1C | sticky error vector |
| `84` | `LAST_AXI_BRESP` | RO | most recent AXI write response |
| `88` | `INPUT_PROTOCOL_ERROR_COUNT` | RO | rejected malformed input events |
| `A0-A8` | Transfer diagnostics | RO | backpressure cycles and 64-bit DDR-committed bytes |
| `C0-D0` | Coherent snapshot | RO | sequence, produced, consumed, outstanding, final-block size |

`COMMAND` bits 0 through 4 are START, ABORT, SOFT_RESET, IRQ_ACK, and
SNAPSHOT. `ERROR_STATUS` bits 0 through 7 are data loss, invalid configuration,
AXI write-response error, session abort, input protocol, invalid address, reset
during a session, and rejected START. IRQ acknowledgement and W1C error clearing
are separate operations. SNAPSHOT captures the accounting fields together;
reset, accepted START and SOFT_RESET invalidate the snapshot to zero.

Capabilities identify 16-beat bursts, the internal FIFO, fixed consumer slots,
and end-of-stream handling. `INFO` encodes 16 maximum AXI beats, 8 bytes per
AXI beat, and 2 bytes per input AXIS beat.

## LED control v2

| Offset | Register | Access | Meaning |
|---:|---|---|---|
| `00-0C` | Identity header | RO | `LEDS`, ABI v2, capabilities `0x07`, geometry |
| `10` | `SOFTWARE_ON_MASK` | RW | logical-on bits for A, B, C, D, error LED |
| `14` | `STATUS` | RO | physical active-high outputs `[5:0]` (A, B, C, D, error, running); hardware-error input bit 6 |

One status word exposes both physical levels and the hardware-error source;
the latter distinguishes a fault override from a software-requested error LED.
The duplicated telemetry registers at `40/44` are unmapped. Hardware faults always
light the error LED and cannot be hidden by `SOFTWARE_ON_MASK`. `running`
directly follows the synchronized SPI-running indication.

## Stimulation/DAC controller v3

Stimulation control is independent of Intan acquisition. An external trigger
rising edge can start a timed TTL pulse or MCP4922 playback only while armed;
the safety-gated trigger monitor and raw Intan sync may be routed to physical
TTL pins for observation without ARM. The compute trigger mux chooses either
the local `ripple_detector` request or the validated SFP trigger request as
the external trigger; reset selects the local source. RTL/C fields use
`EXTERNAL_TRIGGER_ENABLE` and `TRIGGER_MONITOR` for this generic input.
An external pulse that arrives while a timed TTL action or DAC playback is
already active is not queued. The controller ignores that pulse as an action
and increments `UNSERVED_TRIGGER_COUNT`, while `ERROR_STATUS.TRIGGER_DROPPED`
(bit 6) becomes sticky. The detector continues processing samples and counts its own emitted requests
independently of STIM acceptance. Continuous DAC playback accepts its first
trigger and drops later detector requests until playback stops.
The active-low, pull-up hardware button locally forces stimulation safe-off without
resetting Intan, the selected raw sync route, configuration, or waveform RAM.
It shares DISARM and retains a completed PRIME, allowing ARM again after release
and zeroing; an unfinished PRIME is cancelled.

| Offset | Register | Access | Meaning |
|---:|---|---|---|
| `00-0C` | Identity header | RO | `STIM`, ABI v3 (`0x00030000`), capabilities `0x7EF`, RAM geometry |
| `10` | `COMMAND` | W1P | ARM, DISARM, software trigger, STOP_ACTIVITY, CLEAR_DIAGNOSTICS, PRIME_DAC |
| `14` | `STATUS` | RO | live arm, activity, clock and fault state; bits 9, 10 and 13 reserved |
| `18` | `ACCEPTED_TRIGGER_COUNT` | RO | accepted stimulation actions |
| `1C` | `UNSERVED_TRIGGER_COUNT` | RO | controller-rejected triggers, DAC-domain UNSERVED acknowledgements, and waveform aborts |
| `20` | `ACTION_CONFIG` | RW | OFF/TTL/DAC action and external-trigger enable |
| `24` | `TTL_PULSE_WIDTH_AXI_CYCLES` | RW | generated timed-TTL or DAC companion-pulse length in 100 MHz AXI clocks; zero disables a DAC companion |
| `28` | `INTAN_STIM_MARKER_MASK` | RW | recorded marker mask for logical TTL `[15:2]` |
| `40-54` | DAC playback preset | RW | channel enable, continuous mode, update period, indices, finite count |
| `60-68` | DAC playback status | RO | prime state, waveform index, completed-update count |
| `70` | `SAFETY_STATUS` | RO | safe-off active and seen bits |
| `74` | `SAFE_OFF_COUNT` | RO | saturating debounced press count |
| `80` | `ERROR_STATUS` | RO/W1C | sticky incident vector |
| `84` | `FAULT_IRQ_ENABLE` | RW | fault IRQ mask |
| `88/8C` | Error history | RO | saturating incident-cycle count and the most recent complete incident vector |
| `A0` | `DAC_CLOCK_CONFIG` | RW | DAC PLL O, D, M tuple |
| `A4/A8` | `DAC_CLOCK_COMMAND/STATUS` | W1P/RO | program; locked/ready/busy |
| `1000+4*n` | `WAVEFORM[n]` | RW | packed B `[27:16]`, A `[11:0]` codes |

The register groups are lifecycle/outcomes (`10-1C`), output action (`20-28`),
DAC playback (`40-68`), safety (`70-74`), diagnostics (`80-8C`), clock (`A0-A8`),
and waveform RAM (`1000+`). Unmapped words, including `30-3C`, read zero and
ignore writes. All live safety and diagnostic registers remain implemented.

The `1024 x 32` dual-clock BRAM stores paired A/B codes. `COMMAND.PRIME_DAC`
validates and snapshots the preset, then writes hard zero to both DAC channels.
An accepted action, waveform-RAM, DAC-playback, or DAC-clock change invalidates
that prime and requires a new `PRIME_DAC` command.
Every accepted trigger starts at START and wraps END back to LOOP. Finite
completion, STOP, DISARM, and hardware safe-off finish an accepted SPI
frame/pair and then write zero to both A and B. Cleanup zero frames are not
counted as waveform updates. Reset and an interrupted DAC-clock epoch retain a
mandatory A/B-zero request and execute it when a valid DAC clock returns; DAC
PRIME/ARM remain inhibited until that recovery finishes. `STATUS.BUSY` remains
set for the full obligation, even while the DAC clock is unavailable.

The 14-bit Intan marker output is an event interface: the mask stays stable for
the event and `intan_marker_active` denotes the accepted TTL/DAC event envelope.
For DAC it includes the short request/prelaunch interval and terminal A/B-zero
cleanup; a hardware safe-off press drops it immediately. The Intan block owns
the marker CDC and frame-boundary application. Priming is not marker activity.
Intan running never locks stimulation configuration and DISARM never waits for
acquisition to stop. Waveform accesses must be word-aligned; zero-`WSTRB`
writes are true no-ops and do not invalidate PRIME.

STIM owns the source levels presented to the router. `stimulus_level` is the
timed pulse gated low during safe-off. `trigger_monitor_level` follows the
selected external trigger, gated low during safe-off and held-trigger recovery.
A trigger held high through safe-off remains suppressed until it returns low
after release; a low source while the button is held cannot clear suppression.
Both levels are combinationally gated, with no added output clock cycle.
`CAPABILITIES[8]` (`STIM_CAP_TRIGGER_MONITOR`) advertises monitor ownership.
`configuration_locked` protects action settings and drives the router's
`route_write_locked` input. Internal safety state is not a router input.

STIM no longer reads Intan running/sync levels or owns physical pin routing.
Its `STATUS[13,10,9]` and `ERROR_STATUS[0]` are reserved zero. The implemented
error mask is `0x1FE` and the reset fault IRQ mask is `0x3E`. Pin routing cannot
veto ARM/PRIME: a timed pulse can be generated with no physical pin selected,
and a TTL route error cannot stop an otherwise valid DAC action.

## TTL output router v2

The router has its own AXI slave at `0x80014000` and retains its configuration
independently of stimulus settings. It accepts `stimulus_level` and
`trigger_monitor_level` already gated by STIM, raw `intan_sync` from capture,
and `route_write_locked` from STIM's configuration lock. It owns pin mapping and
route diagnostics; it has no safe-off input or monitor-recovery state.

| Offset | Register | Access | Meaning |
|---:|---|---|---|
| `00-0C` | Identity header | RO | `TTLR`, ABI v2 (`0x00020000`), capabilities `0xB`, INFO `0x00040202` |
| `10` | `ROUTE` | RW | TTL0 selector `[1:0]`, TTL1 selector `[3:2]`; reset OFF/OFF |
| `14` | `STATUS` | RO | route-write lock, conflict and source/output levels; bits 1 and 3 reserved zero |
| `18` | `ERROR_STATUS` | RO/W1C | bit 0 blocked route write; bit 1 invalid route write |

Capabilities are source mux (bit 0), raw Intan sync (bit 1), and route-write
protection (bit 3). Bit 2 is no longer a safety capability.

Each selector is 0 OFF, 1 stimulation level, 2 raw Intan sync, or 3 trigger-monitor
level. Duplicate non-OFF selections set live conflict and force both pins low;
OFF/OFF is valid. Route validity is independent of the stimulus action mode.
Only byte strobe 0 updates `ROUTE`; reserved bits are masked. While
`route_write_locked` is high, route writes are ignored and record a blocked-write
error. This input never gates the selected levels or changes an existing route.
Invalid route writes store the route and set the invalid-route error. New errors
win simultaneous W1C. Reserved/unaligned accesses and zero-strobe writes have no
effect. The router exposes no interrupt.

`STATUS` bit 0 is `ROUTE_WRITE_LOCKED`, bit 2 `ROUTE_CONFLICT`, `[5:4]` sampled
physical TTL levels, bit 6 sampled Intan sync, bit 7 `STIMULUS_LEVEL`, and bit 8
`TRIGGER_MONITOR_LEVEL`. Bits 1 and 3 are reserved zero. The source level bits
reflect the gated inputs; safety diagnostics remain in STIM. Async telemetry
uses two-flop synchronizers, while physical routing stays combinational. A valid
Intan-sync route continues during safe-off because STIM does not gate that source.

## Ripple detector v3

The selected-channel HLS detector uses fixed-point internal arithmetic.
Configuration and threshold telemetry retain IEEE-754 binary32 register words;
reported power is a raw unsigned 32-bit integer mean square. It uses
fixed 11-tap `/2` and minimum-phase 120-tap `/5` anti-alias filters, followed by
a selectable ripple FIR or fourth-order IIR and PWT.
It requires hardware-verified 30 kS/s acquisition; its working rate is 3 kS/s.
`LIMITS` encodes 30 maximum PWT samples in `[31:16]` and 256 maximum ripple
FIR taps in `[15:0]`.

| Offset | Register | Access | Meaning |
|---:|---|---|---|
| `00-0C` | Identity header | RO | RPWT, ABI `0x00030000`, capabilities `0x1FF`, LIMITS `0x001E0100` |
| `10` | `COMMAND` | WO | `STOP=0`; `START=1`; `VALIDATE=2`; `RESTORE_DEFAULTS=4`; reads zero |
| `14` | `STATUS` | RO | bits 0 enabled, 1 busy, 2 config valid, 3 rate valid, 4 waiting stream, 5 session closed, 6 fault, 7 write rejected, 8 warmup |
| `20` | `INPUT_CHANNEL_ID` | RW | stream `[8:5]`, amplifier channel `[4:0]`; range 0..511 |
| `24` | `FILTER_KIND` | RW | 0 FIR (default); 1 fourth-order IIR, two normalized biquads |
| `28` | `FIR_TAP_COUNT` | RW | configured ripple FIR length, 1..256; default 129 |
| `2C` | `POWER_WINDOW_US` | RW | PWT duration, 1..10000 us; default 4000 us |
| `30` | `REFRACTORY_PERIOD_MS` | RW | interval after an accepted crossing; default 1000 ms |
| `34` | `BASELINE_MEAN_BITS` | RW | baseline RMS mean, binary32 ADC counts |
| `38` | `BASELINE_STDDEV_BITS` | RW | baseline RMS standard deviation, binary32 ADC counts |
| `3C` | `THRESHOLD_K_BITS` | RW | nonnegative finite binary32 threshold multiplier; default 4; supports live update |
| `40` | `COEFFICIENT_INDEX` | RW | FIR index 0..255; IIR coefficients 256..265 |
| `44` | `COEFFICIENT_BITS` | RW | indexed binary32 coefficient upload; commits and reads back the last HLS-accepted word |
| `50` | `TRIGGER_REQUEST_COUNT` | RO | detector trigger requests, independent of downstream acceptance |
| `54` | `POWER_SAMPLE_COUNT` | RO | processed 3 kS/s power outputs |
| `58` | `MEAN_SQUARE` | RO | latest mean square, raw uint32 in ADC counts squared |
| `5C` | `SUM_SQUARE_THRESHOLD_BITS` | RO | validated `N * (mean + K * stddev)^2`, binary32 |
| `60` | `LAST_INPUT_TIMESTAMP` | RO | newest selected-channel input timestamp |

This table is the internal PL ABI. The public firmware profile accepts filter
kinds 0 minimum-phase FIR, 1 linear-phase FIR, and 2 fixed IIR. Firmware maps
both FIR choices to hardware `FILTER_KIND=0` and uploads the designed taps; it
maps the IIR choice to `FILTER_KIND=1`. Public control uses
`RIPPLE_APPLY_PROFILE`; there is no public coefficient-upload or VALIDATE
command. Firmware admits the complete profile only after acquisition is stopped
and drained in `IDLE` or `READY`, and preflights all scalar values plus the
designed filter before issuing any PL write. `RIPPLE_SET_K` remains the only
public live detector-configuration update. Baseline START instead requires the
active 30 kS/s local/UDP detector stream; the raw SFP route bypasses this block.

Except for live `THRESHOLD_K_BITS`, parameters and coefficients are writable only while disabled and HLS-idle;
changes invalidate `CONFIG_VALID`. VALIDATE recomputes threshold/configuration
without starting acquisition. START clears detector state and enables only after
successful HLS validation. A coefficient write is staged until its HLS update
completes. Nonfinite or fixed-point-unrepresentable coefficients set
`WRITE_REJECT` and leave the previous accepted `COEFFICIENT_BITS` readback unchanged.
Invalid baseline parameters or geometry remain disabled and report a fault.
STOP mutes triggers immediately.

An enabled detector accepts a full-word `THRESHOLD_K_BITS` write while a sample is processing.
One update can be queued, and `BUSY` includes that queue. HLS applies the new
threshold between transactions without resetting histories, power-window fill,
edge state, counters or refractory timing. `THRESHOLD_K_BITS` and
`SUM_SQUARE_THRESHOLD_BITS` commit after
successful validation. A rejected update sets `WRITE_REJECT` and preserves the
previous threshold and enabled state. A concurrent STOP, EOS or rate loss
cancels an uncommitted update. Lowering K can cause a crossing on the next power
sample. Firmware exposes this operation as `RIPPLE_SET_K`.

IIR indices 256..260 and 261..265 each contain `b0,b1,b2,a1,a2`, with `a0=1`.
For each section, `y=b0*x+b1*x1+b2*x2-a1*y1-a2*y2`. Validation rejects nonfinite
coefficients or poles outside the strict stable region. The default is a
four-pole Butterworth bandpass at 150..250 Hz, sampled at 3 kS/s. Custom
coefficients determine the band and response. Capability bits 0 through 8
respectively denote FIR, IIR, programmable coefficients, input-channel
selection, configurable power window, refractory control, live K updates,
uint32 mean-square reporting, and fixed-point internal DSP.

The PWT evaluates RMS against `baseline_mean + K * baseline_stddev` using a
sum-of-squares comparison.
Host software supplies baseline statistics in that RMS domain. The default
4 ms window contains 12 samples at 3 kS/s. Configuration, selected-channel
history, window warmup, and refractory timing belong to the detector. STIM owns
all later playback, acceptance, ARM, busy rejection, and safe-off behavior.

Internally, `S=sum(x^2)` and the trigger compares the exact 64-bit sum against
`N*(baseline_mean+K*baseline_stddev)^2`. `MEAN_SQUARE` reports the rounded
integer mean `S/C`, where
`C` is the current valid-sample count during warmup and `N` afterward. The
reported value is bounded by `32768^2` and does not scale with window length.

See [algorithm-module.md](../docs/algorithm-module.md) for the DSP chain and HLS
build contract. A stopped detector or ended acquisition does not cancel an
already accepted stimulation action.

## Compute fabric v4

The compute fabric accepts the compact 16-bit Intan compute stream and sends it
exclusively to the local decoder or the raw SFP packetizer. It also arbitrates
Intan and PS mailbox packets onto Aurora, strips target-2 RX headers, and
validates the one target-3 trigger command. Aurora/CDC and the AXI-Lite mailbox
are separate components.

The block is at `0x80021000`, identifies as `NCFP`, and uses ABI v4. Its
capabilities value `0x3F` represents local decoding, raw Intan SFP transport, PS
packet routing, SFP stimulation trigger, AUX events, and TTL events.

| Group | Offset | Register | Access | Meaning |
| --- | ---: | --- | --- | --- |
| Identity | `00` | `BLOCK_ID` | RO | `0x4E434650` (`NCFP`) |
| Identity | `04` | `ABI_VERSION` | RO | `0x00040000` |
| Identity | `08` | `CAPABILITIES` | RO | `0x0000003F` |
| Configuration | `10` | `CONTROL` | RW | bit 0 PHY enable, bit 1 SFP application-mode select, bit 8 fault IRQ enable |
| Configuration | `14` | `SOURCE_STREAM_MASK` | RW | exact nonzero logical-stream mask `[15:0]` for local decode |
| Command | `18` | `COMMAND` | W1P | bit 0 clear diagnostics; bit 1 clear the Intan EOS latch |
| Status | `20` | `STATUS` | RO | mode, drain, queue, fault, and EOS state below |
| Status | `24` | `LINK_STATUS` | RO | bit 0 link up, bit 1 link fault, bit 2 RX parser idle |
| Diagnostics | `30` | `CONTROL_REJECT_COUNT` | RO | rejected reserved/unsafe configuration or command writes |
| Diagnostics | `34` | `LOCAL_DECODE_FAULT_COUNT` | RO | malformed or truncated local frames |
| Diagnostics | `38` | `SFP_PACKETIZER_FAULT_COUNT` | RO | malformed, truncated, or overlength raw frames dropped by the SFP packetizer |
| Diagnostics | `3C` | `SFP_TX_INTERRUPTED_PACKET_COUNT` | RO | SFP TX packets interrupted after payload transmission began |
| Diagnostics | `40` | `RX_MALFORMED_PACKET_COUNT` | RO | invalid header or payload framing |
| Diagnostics | `44` | `RX_FIFO_OVERFLOW_COUNT` | RO | target-2 packet storage overflow |
| Diagnostics | `48` | `RX_DROPPED_PACKET_COUNT` | RO | packets for unknown/unsupported targets |
| Diagnostics | `4C` | `PS_RX_PACKET_COUNT` | RO | complete target-2 packets committed to PS |
| Diagnostics | `50` | `RX_STIM_TRIGGER_COUNT` | RO | accepted target-3 stimulation-trigger commands |
| Diagnostics | `54` | `RX_STIM_TRIGGER_REJECT_COUNT` | RO | rejected target-3 stimulation-trigger commands |
| Diagnostics | `58` | `INTAN_TX_PACKET_ENQUEUED_COUNT` | RO | complete raw Intan packets enqueued into the TX CDC |
| Diagnostics | `5C` | `PS_TX_ATTEMPT_ENQUEUED_COUNT` | RO | complete PS TX attempts enqueued into the TX CDC |

`CONTROL` resets to `0x00000101`: PHY and fault IRQ enabled, local compute
selected. Only bits 0, 1, and 8 are writable. A mode change is rejected unless
`CONFIG_IDLE` is set. PHY and IRQ state may be updated without changing the
mode. `SOURCE_STREAM_MASK` resets to `1`; zero, upper-bit writes, and a changed
mask while not configuration-idle are rejected.

PS routing is fixed in hardware. Every reply uses target 2, EOM set, and protocol
version 1, producing header `0x0000000000000182`.

`COMMAND.CLEAR_DIAGNOSTICS` clears sticky fault state and all fabric/RX counters.
An incident in the clear cycle is still counted. `CLEAR_INTAN_EOS` clears
`STATUS.INTAN_EOS_SEEN` and is accepted only while configuration-idle. A new
compute-stream active edge also clears that EOS bit; an accepted zero-keep Intan
TLAST sets it.

| STATUS bit | Meaning |
| ---: | --- |
| 0 | qualified physical link up |
| 1 | PHY enabled |
| 2 | SFP application mode selected |
| 3 | Intan compute-output session active |
| 4 | configuration idle |
| 5 | local decoder inside a frame |
| 6 | raw SFP packetizer inside a frame |
| 7 | SFP TX arbiter idle |
| 8 | Intan TX packet active |
| 9 | PS TX packet pending or active |
| 10 | complete PS RX data pending |
| 11 | local decode fault sticky |
| 12 | raw SFP packetizer fault sticky |
| 13 | interrupted SFP TX packet sticky |
| 14 | RX route/framing/overflow/trigger fault sticky |
| 15 | rejected control write sticky |
| 16 | Intan session EOS accepted |

The compute IRQ is active when fault IRQ is enabled and either physical
`link_fault` is live or any sticky bit 11 through 15 is set. PS packet
availability and mailbox completion/error state use PL IRQ bit 2.

### Local algorithm event format

The local decoder snapshots the full stream mask at frame start and emits:

```text
[63:32] timestamp
[31:30] kind: 0 AMP, 1 AUX reply, 2 TTL
[29:25] flags = 0
[24:21] sparse logical stream ID
[20:16] channel 0..31 or AUX slot 0..2
[15:0]  value
```

AMP and AUX events use full keep without TLAST. The TTL event uses full keep and
TLAST. Session EOS is zero-keep TLAST. The local stream begins before the full
raw frame has arrived.

### SFP packet header and targets

Every physical AXIS64 packet starts with one full-keep, non-TLAST header:

```text
[63:16] reserved = 0
[15:8]  protocol version = 1
[7]     end of logical message (EOM)
[6:0]   target ID
```

Targets are 1 for a raw Intan frame, 2 for PS data, and 3 for the stimulation trigger
request. Target-1 packets always set EOM and contain the exact compact Intan
frame, grouped four 16-bit words per payload beat. The magic, timestamp, all 35
rows, and TTL are unchanged. The source mask is not inserted into the packet.
The final payload beat uses the contiguous low-byte TKEEP appropriate for one to
four remaining halfwords. The packetizer buffers the complete frame, up to 567
halfwords, before output; malformed, truncated, and overlength frames emit no
partial packet.

PS-to-card payload comes from the fixed packet mailbox; the fabric adds header
`0x0182`. Card-to-PS target-2 packets require EOM and version 1. The RX router
strips the header before delivering payload to the mailbox. Intan and PS packets
are arbitrated round-robin and never interleaved.

Target 3 accepts only one full-keep payload word equal to `1`, with EOM set and
SFP mode selected. It emits one trigger pulse. DAC waveform, preset, PRIME,
ARM, STOP, DISARM, and status remain exclusively in the STIM AXI-Lite block.

## SFP packet mailbox v2

The project-owned `NPMB` block is mapped at `0x80030000` with a 4 KiB aperture.
It has two packet slots in each direction and no DDR master. RX accepts exactly
eight full AXIS64 beats with `TLAST` on beat eight, stores one 64-byte canonical
command atomically, and exposes the front command as sixteen 32-bit AXI-Lite
words. TX snapshots ten 32-bit staging words into one 40-byte reply and emits
five full AXIS64 beats. Software uses `RX_POP` after copying a command and
`TX_COMMIT` after filling the reply window.

| Group | Offset | Register | Access | Meaning |
| --- | ---: | --- | --- | --- |
| Identity | `00` | `BLOCK_ID` | RO | `0x4E504D42` (`NPMB`) |
| Identity | `04` | `ABI_VERSION` | RO | `0x00020000` |
| Identity | `08` | `CAPABILITIES` | RO | `0x0000003F` |
| Identity | `0C` | `INFO` | RO | `0x00280040`: TX 40 bytes, RX 64 bytes |
| Status | `10` | `STATUS` | RO | queue, link, Aurora acceptance, replay, and error state below |
| Interrupt | `14` | `IRQ_ENABLE` | RW | bit 0 RX available, bit 1 TX accepted by Aurora, bit 2 error |
| Command | `18` | `COMMAND` | W1P | bit 0 RX pop, bit 1 TX commit, bit 2 clear Aurora-accepted status, bit 3 clear diagnostics |
| Error | `1C` | `ERROR_STATUS` | RW1C | malformed RX, RX overflow, empty pop, full TX commit, TX feedback errors |
| Counter | `20` | `RX_PACKET_COUNT` | RO | commands committed into RX slots |
| Counter | `24` | `RX_OVERFLOW_COUNT` | RO | complete commands dropped because both RX slots were full |
| Counter | `28` | `RX_MALFORMED_COUNT` | RO | wrong RX payload length, TLAST position, or TKEEP |
| Counter | `2C` | `TX_AURORA_ACCEPTED_COUNT` | RO | replies whose final PS-owned beat was accepted by the local Aurora TX interface |
| Counter | `30` | `TX_REJECT_COUNT` | RO | TX commits rejected because both slots were full |
| Counter | `34` | `TX_REPLAY_COUNT` | RO | interrupted replies retained for replay |
| RX window | `40..7C` | `RX_DATA[0..15]` | RO | front 64-byte command; stable until RX pop, zero when empty |
| TX window | `80..A4` | `TX_DATA[0..9]` | RW | 40-byte staging reply snapshotted by TX commit |

`STATUS` uses bit 0 RX available, bit 1 RX full, bit 2 TX space available,
bit 3 TX pending, bit 4 TX output valid, bit 5 link up, bit 6 TX accepted-by-Aurora sticky,
bit 7 TX replay pending, bits `[9:8]` RX count, bits `[11:10]` TX count, and
bit 12 error pending. `ERROR_STATUS` uses bit 0 RX malformed, bit 1 RX overflow,
bit 2 RX pop while empty, bit 3 TX commit while full, and bit 4 unexpected TX
acceptance/interruption feedback. `ERROR_STATUS` is write-one-to-clear.
`CLEAR_DIAGNOSTICS` clears all six counters while preserving those W1C errors.

The mailbox IRQ is level-high for any enabled RX-available, TX-Aurora-accepted, or
error condition. A reply remains queued until the transport reports that the
local Aurora TX interface accepted its final PS-owned beat. This is a local
completion event, not an acknowledgment from the remote card. Link interruption
rewinds the reply for a complete replay rather than publishing its tail as a new
packet.

Firmware automatically selects compute `CONTROL.SFP_MODE_SELECT` and enables SFP
command ingress only when the full usable-link predicate is true: both ABIs are
valid, compute reports `LINK_STATUS.UP` without `LINK_STATUS.FAULT`, and the
mailbox reports synchronized `STATUS.LINK_UP`. That state gives SFP ownership of
commands, replies, and the raw stream while muting RJ45 command execution and
UDP output. A raw `link_up` bit alone is insufficient. lwIP maintenance and the
RJ45 ownership-banner listener continue while Ethernet application traffic is
muted.

See [`docs/aurora-link-v1.md`](../docs/aurora-link-v1.md) for the wire contract.
