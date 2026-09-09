# Exact locally built device artifacts

These files were built from the canonical sources identified by the parent
record's build manifests and source digests. The board application is the
1,275,952-byte image at application offset `0x20000`; its full serial readback
matched. The APK is the final Android bench including subscription/readiness
ordering and the bounded pending-frame queue.

They are retained private experimental artifacts, not a released firmware,
signed product distribution, third-party dependency or release approval.
`../SHA256SUMS.txt` records their exact bytes. The historical image hashes and
failed runs remain in the parent logs rather than being relabelled as results
from these final artifacts.
