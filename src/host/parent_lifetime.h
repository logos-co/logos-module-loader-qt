#pragma once

// Moves this host into its own session and makes it exit when the process
// that spawned it dies without stopping it (a crashed daemon). POSIX only:
// on Windows the container's kill-on-close job object does this.
void isolateAndFollowParent();
