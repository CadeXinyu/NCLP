# Third-Party Notices

The top-level MIT License applies only to original NCLP contributions. The
following components retain their existing copyright and license terms:

- `open-ephys/NCLPIntanSource/` is based in part on the
  [GLANCE Open Ephys plugin](https://github.com/glanceneuro/glance-neuro-plugin)
  and Open Ephys plugin code. It is distributed under GPL-3.0-or-later; its
  full license is included in that directory and under `LICENSES/`.
- `software/main/src/headstage/impedance_headstage.c` adapts impedance
  processing from the Open Ephys RHD Recording Controller and is distributed
  under GPL-3.0-or-later. Firmware binaries that incorporate this file are
  subject to the GPL; the full license is included under `LICENSES/`.
- `programmable_logic/src/spi/fifo/axis_async_fifo.v` is Copyright (c)
  2014–2023 Alex Forencich and is licensed under the MIT License. Its complete
  notice is retained in the file.
- The fixed decimation coefficients under
  `programmable_logic/hls/ripple_detector/` derive from
  [GLANCE Neuro](https://github.com/glanceneuro/glance-neuro), Copyright (c)
  2025–2026 Caleb Kemere and Rice University, under the MIT License. The
  source notices are retained in the generated files.
- AMD and Xilinx generated or vendor files retain their per-file copyright
  and license notices.
- `PCB/12pin/5054331271.SchLib` contains an Ultra Librarian component model
  with its own copyright notice and is excluded from the top-level MIT grant.

Nothing in the top-level license overrides these terms or any copyright and
license notice contained in an individual file.
