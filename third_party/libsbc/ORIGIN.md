Unmodified source files from the Bluetooth SBC library:

https://git.kernel.org/pub/scm/bluetooth/sbc.git

Commit: b3deb8a5dcfb42d8c10ba1f2f1ac9bd7bf7271cc

Library license: LGPL-2.1-or-later (COPYING.LIB). COPYING is also retained
from upstream. AUTHORS and original per-file notices are preserved.

The Switch build uses sbc.c and the portable C primitives. Platform assembly
files are retained as source but not compiled. Full build inputs and scripts
are provided so the executable can be relinked with modified library code.
